// raftkv_server: a single-node Redis-compatible server.
//
//   raftkv_server [--bind ADDRESS] [--port PORT] [--aof FILE] [--fsync always|everysec|no]
//
// Try it with:  redis-cli -p 6380 SET greeting hello

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "commands/dispatcher.hpp"
#include "net/server.hpp"
#include "persistence/aof.hpp"
#include "persistence/durable_handler.hpp"
#include "store/clock.hpp"
#include "store/store.hpp"

namespace {

// Set while the server runs so the signal handler can reach it.
// A lock-free atomic pointer is safe to read inside a signal handler.
std::atomic<raftkv::Server*> g_server{nullptr};

void onSignal(int /*signal*/) {
    raftkv::Server* server = g_server.load();
    if (server != nullptr) {
        server->stop();  // only touches an atomic flag and an eventfd: signal-safe
    }
}

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--bind ADDRESS] [--port PORT] [--aof FILE] [--fsync POLICY]\n"
                 "  --bind ADDRESS   IPv4 address to listen on (default 127.0.0.1)\n"
                 "  --port PORT      TCP port, 0 picks a free one (default 6380)\n"
                 "  --aof FILE       log writes to FILE and recover from it on startup (default: off)\n"
                 "  --fsync POLICY   always | everysec | no (default everysec; needs --aof)\n",
                 program);
}

bool parsePort(std::string_view text, std::uint16_t& port) {
    unsigned int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size() || value > 65535U) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    raftkv::ServerConfig config;
    std::optional<std::string> aofPath;
    raftkv::FsyncPolicy fsyncPolicy = raftkv::FsyncPolicy::kEverySec;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        if ((arg == "--port" || arg == "--bind" || arg == "--aof" || arg == "--fsync") && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (arg == "--bind") {
                config.bindAddress = std::string(value);
            } else if (arg == "--aof") {
                aofPath = std::string(value);
            } else if (arg == "--fsync") {
                if (!raftkv::parseFsyncPolicy(value, fsyncPolicy)) {
                    std::fprintf(stderr, "error: --fsync must be always, everysec or no\n");
                    return 2;
                }
            } else if (!parsePort(value, config.port)) {
                std::fprintf(stderr, "error: invalid port '%.*s'\n", static_cast<int>(value.size()), value.data());
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "error: unknown or incomplete option '%s'\n", argv[i]);
        printUsage(argv[0]);
        return 2;
    }

    raftkv::SystemClock systemClock;
    raftkv::PinnableClock clock(systemClock);
    raftkv::Store store(clock);
    raftkv::CommandDispatcher dispatcher(store, clock);

    // With --aof: recover from the log, then log every write before acknowledging it.
    raftkv::AppendOnlyFile aofFile(systemClock, fsyncPolicy);
    std::optional<raftkv::DurableHandler> durable;
    if (aofPath.has_value()) {
        raftkv::ReplayStats stats;
        std::string aofError;
        if (!raftkv::openAndRecover(aofFile, *aofPath, dispatcher, clock, stats, aofError)) {
            std::fprintf(stderr, "error: %s\n", aofError.c_str());
            return 1;
        }
        std::printf("recovered %zu commands from %s\n", stats.records, aofPath->c_str());
        if (stats.discardedBytes > 0) {
            std::fprintf(stderr,
                         "warning: discarded %llu bytes after the last intact record in %s "
                         "(torn write after a crash, or corruption)\n",
                         static_cast<unsigned long long>(stats.discardedBytes), aofPath->c_str());
        }
        durable.emplace(dispatcher, clock, aofFile, stats.lastTimestampMs);
    }
    raftkv::CommandHandler& handler = durable.has_value()
                                          ? static_cast<raftkv::CommandHandler&>(*durable)
                                          : static_cast<raftkv::CommandHandler&>(dispatcher);

    // Once a second: delete expired keys nobody has asked for, and flush the log.
    config.onTick = [&store, &durable] {
        store.purgeExpired();
        if (durable.has_value()) {
            durable->tick();
        }
    };
    config.tickIntervalMs = 1000;

    raftkv::Server server(config, handler);
    std::string error;
    if (!server.start(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    g_server.store(&server);
    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    std::printf("raftkv listening on %s:%u\n", config.bindAddress.c_str(), static_cast<unsigned>(server.port()));
    std::fflush(stdout);

    server.run();

    g_server.store(nullptr);
    if (durable.has_value() && !aofFile.close()) {
        std::fprintf(stderr, "error: could not flush the append-only file: %s\n",
                     aofFile.failureReason().c_str());
        return 1;
    }
    std::printf("raftkv stopped\n");
    return 0;
}
