#include <gtest/gtest.h>

#include <optional>
#include <random>
#include <string>
#include <vector>

#include "commands/dispatcher.hpp"
#include "persistence/aof.hpp"
#include "persistence/durable_handler.hpp"
#include "resp/parser.hpp"
#include "store/clock.hpp"
#include "store/store.hpp"
#include "test_support.hpp"

namespace {

using raftkv::AppendOnlyFile;
using raftkv::FsyncPolicy;
using raftkv::TimestampMs;
using raftkv_test::TempDir;

// One "server": a store with its dispatcher, a log, and the handler that ties them together.
struct Instance {
    explicit Instance(raftkv::ManualClock& base, FsyncPolicy policy = FsyncPolicy::kAlways)
        : clock(base), store(clock), dispatcher(store, clock), file(base, policy) {}

    bool open(const std::string& path) {
        std::string error;
        const bool ok = raftkv::openAndRecover(file, path, dispatcher, clock, stats, error);
        EXPECT_TRUE(ok) << error;
        handler.emplace(dispatcher, clock, file, stats.lastTimestampMs);
        return ok;
    }

    // Runs one command like the server would: execute, then commit, then return the reply.
    std::string run(const raftkv::resp::Command& command) {
        std::string out;
        handler->execute(command, out);
        EXPECT_TRUE(handler->commit());
        return out;
    }

    raftkv::PinnableClock clock;
    raftkv::Store store;
    raftkv::CommandDispatcher dispatcher;
    AppendOnlyFile file;
    raftkv::ReplayStats stats;
    std::optional<raftkv::DurableHandler> handler;
};

std::vector<std::pair<TimestampMs, raftkv::resp::Command>> logged(const std::string& path) {
    const std::string bytes = raftkv_test::readFile(path);
    std::vector<std::pair<TimestampMs, raftkv::resp::Command>> records;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        auto record = raftkv::aof::decodeRecord(std::string_view(bytes).substr(pos));
        if (record.status != raftkv::aof::DecodeStatus::kOk) {
            break;
        }
        pos += record.consumed;
        records.emplace_back(record.timestampMs, std::move(record.command));
    }
    return records;
}

}  // namespace

TEST(DurableHandler, SuccessfulWritesAreLoggedWithTheirTimestamp) {
    TempDir dir;
    raftkv::ManualClock base(5000);
    Instance a(base);
    ASSERT_TRUE(a.open(dir.file("a.aof")));

    EXPECT_EQ(a.run({"SET", "k", "v"}), "+OK\r\n");
    base.advanceMs(250);
    EXPECT_EQ(a.run({"INCR", "n"}), ":1\r\n");

    const auto records = logged(dir.file("a.aof"));
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].first, 5000);
    EXPECT_EQ(records[1].first, 5250);
    EXPECT_EQ(records[1].second, (raftkv::resp::Command{"INCR", "n"}));
}

TEST(DurableHandler, ReadsAndFailedCommandsAreNotLogged) {
    TempDir dir;
    raftkv::ManualClock base(1);
    Instance a(base);
    ASSERT_TRUE(a.open(dir.file("a.aof")));

    a.run({"SET", "text", "abc"});
    a.run({"GET", "text"});
    a.run({"PING"});
    a.run({"TTL", "text"});
    a.run({"INCR", "text"});               // error: not an integer
    a.run({"SET", "only-one-arg"});        // error: wrong arity
    a.run({"EXPIRE", "text", "soon"});     // error: bad number
    a.run({"NOSUCHCOMMAND", "x"});         // error: unknown
    a.run({});                             // error: empty

    const auto records = logged(dir.file("a.aof"));
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].second[0], "SET");
}

TEST(DurableHandler, RecoveryRestoresValuesAndCounters) {
    TempDir dir;
    raftkv::ManualClock base(1000);
    {
        Instance a(base);
        ASSERT_TRUE(a.open(dir.file("a.aof")));
        a.run({"SET", "name", "ada"});
        a.run({"SET", "gone", "x"});
        a.run({"DEL", "gone"});
        a.run({"INCR", "n"});
        a.run({"INCR", "n"});
    }
    Instance b(base);
    ASSERT_TRUE(b.open(dir.file("a.aof")));
    EXPECT_EQ(b.stats.records, 5U);
    EXPECT_EQ(b.run({"GET", "name"}), "$3\r\nada\r\n");
    EXPECT_EQ(b.run({"GET", "gone"}), "$-1\r\n");
    EXPECT_EQ(b.run({"GET", "n"}), "$1\r\n2\r\n");
}

TEST(DurableHandler, ExpiryDeadlinesSurviveARestartUnchanged) {
    // EXPIRE is "10 seconds from when it ran", not from when the log is replayed.
    TempDir dir;
    raftkv::ManualClock base(1000);
    {
        Instance a(base);
        ASSERT_TRUE(a.open(dir.file("a.aof")));
        a.run({"SET", "k", "v"});
        EXPECT_EQ(a.run({"EXPIRE", "k", "10"}), ":1\r\n");  // deadline = 11000
    }

    // Restart at t=5000. If replay used the restart time, the deadline would become 15000.
    base.setMs(5000);
    Instance b(base);
    ASSERT_TRUE(b.open(dir.file("a.aof")));
    EXPECT_EQ(b.store.ttlMs("k"), 6000);

    // Restart long after the deadline: the key must be gone.
    base.setMs(1'000'000);
    Instance c(base);
    ASSERT_TRUE(c.open(dir.file("a.aof")));
    EXPECT_EQ(c.run({"GET", "k"}), "$-1\r\n");
    EXPECT_EQ(c.run({"TTL", "k"}), ":-2\r\n");
}

TEST(DurableHandler, AKeyThatExpiredAndWasRewrittenIsReplayedCorrectly) {
    TempDir dir;
    raftkv::ManualClock base(0);
    {
        Instance a(base);
        ASSERT_TRUE(a.open(dir.file("a.aof")));
        a.run({"SET", "k", "old"});
        a.run({"EXPIRE", "k", "1"});
        base.setMs(5000);
        EXPECT_EQ(a.run({"GET", "k"}), "$-1\r\n");  // expired
        a.run({"INCR", "k"});                        // starts over from nothing
    }
    base.setMs(9000);
    Instance b(base);
    ASSERT_TRUE(b.open(dir.file("a.aof")));
    EXPECT_EQ(b.run({"GET", "k"}), "$1\r\n1\r\n");
    EXPECT_EQ(b.run({"TTL", "k"}), ":-1\r\n");
}

TEST(DurableHandler, RandomWorkloadRecoversToTheSameState) {
    // Run a random mix of commands with time moving forward, restart, and compare every key.
    const std::vector<std::string> keys = {"k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7"};
    for (unsigned seed = 1; seed <= 25; ++seed) {
        TempDir dir;
        std::mt19937 rng(seed);
        raftkv::ManualClock base(1'000'000);

        Instance original(base);
        ASSERT_TRUE(original.open(dir.file("a.aof")));
        for (int step = 0; step < 400; ++step) {
            base.advanceMs(static_cast<TimestampMs>(rng() % 700));
            const std::string& key = keys[rng() % keys.size()];
            switch (rng() % 6) {
                case 0: original.run({"SET", key, std::to_string(rng() % 100)}); break;
                case 1: original.run({"SET", key, "text"}); break;
                case 2: original.run({"INCR", key}); break;
                case 3: original.run({"DEL", key}); break;
                case 4: original.run({"EXPIRE", key, std::to_string(rng() % 4)}); break;
                default: original.run({"GET", key}); break;
            }
        }

        // "Restart" at the same instant the original stopped, and compare.
        Instance recovered(base);
        ASSERT_TRUE(recovered.open(dir.file("a.aof")));
        for (const std::string& key : keys) {
            EXPECT_EQ(recovered.store.get(key), original.store.get(key)) << "seed " << seed << " key " << key;
            EXPECT_EQ(recovered.store.ttlMs(key), original.store.ttlMs(key)) << "seed " << seed << " key " << key;
        }
    }
}

TEST(DurableHandler, TimestampsNeverGoBackwardsEvenIfTheClockDoes) {
    TempDir dir;
    raftkv::ManualClock base(10'000);
    Instance a(base);
    ASSERT_TRUE(a.open(dir.file("a.aof")));
    a.run({"SET", "a", "1"});
    base.setMs(4000);  // the system clock jumped back
    a.run({"SET", "b", "2"});

    const auto records = logged(dir.file("a.aof"));
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].first, 10'000);
    EXPECT_EQ(records[1].first, 10'000);
}

TEST(DurableHandler, TimestampsContinueFromTheRecoveredLog) {
    TempDir dir;
    raftkv::ManualClock base(10'000);
    {
        Instance a(base);
        ASSERT_TRUE(a.open(dir.file("a.aof")));
        a.run({"SET", "a", "1"});
    }
    base.setMs(2000);  // restarted on a machine whose clock is behind the log
    Instance b(base);
    ASSERT_TRUE(b.open(dir.file("a.aof")));
    b.run({"SET", "b", "2"});

    const auto records = logged(dir.file("a.aof"));
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[1].first, 10'000);
}

TEST(DurableHandler, ABrokenLogRefusesWritesAndFailsTheCommit) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "/dev/full not available";
    }
    raftkv::ManualClock base(1);
    Instance a(base);
    std::string error;
    ASSERT_TRUE(raftkv::openAndRecover(a.file, "/dev/full", a.dispatcher, a.clock, a.stats, error)) << error;
    a.handler.emplace(a.dispatcher, a.clock, a.file, a.stats.lastTimestampMs);

    std::string out;
    a.handler->execute({"SET", "k", "v"}, out);
    EXPECT_EQ(out, "+OK\r\n");            // executed in memory, but not yet acknowledged to anyone
    EXPECT_FALSE(a.handler->commit());   // the server will drop the client instead of sending "OK"

    // From now on writes are refused outright, so memory cannot drift further from the disk.
    out.clear();
    a.handler->execute({"SET", "other", "v"}, out);
    EXPECT_EQ(out.substr(0, 8), "-MISCONF");
    EXPECT_FALSE(a.store.get("other").has_value());

    // Reads still work, but their commit fails too: the server stops serving this client.
    out.clear();
    a.handler->execute({"GET", "k"}, out);
    EXPECT_EQ(out, "$1\r\nv\r\n");
    EXPECT_FALSE(a.handler->commit());
}
