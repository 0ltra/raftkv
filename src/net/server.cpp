#include "net/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <utility>

#include "resp/encoder.hpp"
#include "resp/parser.hpp"

namespace raftkv {
namespace {

constexpr int kListenBacklog = 511;
constexpr std::size_t kMaxEventsPerWait = 128;
constexpr std::size_t kReadChunkBytes = 64 * 1024;

// An incomplete command may legitimately hold one maximum-size bulk string.
constexpr std::size_t kMaxPendingInputBytes = resp::kMaxBulkLen + 1024 * 1024;
// A client that pipelines requests but never reads replies must not grow our memory forever.
constexpr std::size_t kMaxPendingOutputBytes = 128 * 1024 * 1024;

constexpr std::uint32_t kEpollIn = EPOLLIN;
constexpr std::uint32_t kEpollOut = EPOLLOUT;
constexpr std::uint32_t kEpollErr = EPOLLERR;
constexpr std::uint32_t kEpollHup = EPOLLHUP;

std::string errnoMessage(int err) {
    return std::system_category().message(err);
}

void logError(const std::string& message) {
    std::fprintf(stderr, "[raftkv] %s\n", message.c_str());
}

bool addToEpoll(int epollFd, int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return ::epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

std::size_t pendingOutput(const std::string& out, std::size_t offset) {
    return out.size() - offset;
}

}  // namespace

Server::Server(ServerConfig config, CommandHandler& handler)
    : config_(std::move(config)), handler_(handler), readBuffer_(kReadChunkBytes) {}

Server::~Server() {
    for (const auto& entry : conns_) {
        ::close(entry.first);
    }
    conns_.clear();
    closeListenerFds();
}

void Server::closeListenerFds() {
    for (int* fd : {&listenFd_, &epollFd_, &wakeFd_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
}

bool Server::start(std::string& error) {
    // Reports the failing step plus the OS reason, and releases anything half-created.
    // errno is read first thing, before anything can overwrite it.
    const auto fail = [&](const std::string& what) {
        const int err = errno;
        error = what + ": " + errnoMessage(err);
        closeListenerFds();
        return false;
    };

    in_addr address{};
    if (::inet_pton(AF_INET, config_.bindAddress.c_str(), &address) != 1) {
        error = "invalid IPv4 bind address '" + config_.bindAddress + "'";
        return false;
    }

    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        return fail("socket");
    }

    // Lets us restart right away instead of waiting out TIME_WAIT sockets.
    const int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(config_.port);
    local.sin_addr = address;
    if (::bind(listenFd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        return fail("bind " + config_.bindAddress + ":" + std::to_string(config_.port));
    }
    if (::listen(listenFd_, kListenBacklog) != 0) {
        return fail("listen");
    }

    socklen_t length = sizeof(local);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
        return fail("getsockname");
    }
    port_ = ntohs(local.sin_port);

    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        return fail("epoll_create1");
    }
    wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) {
        return fail("eventfd");
    }
    if (!addToEpoll(epollFd_, listenFd_, kEpollIn) || !addToEpoll(epollFd_, wakeFd_, kEpollIn)) {
        return fail("epoll_ctl");
    }
    return true;
}

void Server::stop() noexcept {
    stopRequested_.store(true);
    if (wakeFd_ >= 0) {
        // Makes epoll_wait return even if the loop is asleep. write() is async-signal-safe.
        const std::uint64_t one = 1;
        const ssize_t written = ::write(wakeFd_, &one, sizeof(one));
        (void)written;
    }
}

void Server::run() {
    if (epollFd_ < 0) {
        return;  // start() was not called or failed
    }

    using SteadyClock = std::chrono::steady_clock;
    const auto tickInterval = std::chrono::milliseconds(std::max(1, config_.tickIntervalMs));
    auto nextTick = SteadyClock::now() + tickInterval;
    std::array<epoll_event, kMaxEventsPerWait> events{};

    while (!stopRequested_.load()) {
        // Sleep until something happens, but never past the next tick.
        int timeoutMs = -1;
        if (config_.onTick) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(nextTick - SteadyClock::now());
            timeoutMs = static_cast<int>(std::max<std::int64_t>(0, remaining.count()));
        }

        const int ready = ::epoll_wait(epollFd_, events.data(), static_cast<int>(events.size()), timeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            logError("epoll_wait failed: " + errnoMessage(errno));
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const epoll_event& event = events[static_cast<std::size_t>(i)];
            handleEvent(event.events, event.data.fd);
        }

        if (config_.onTick && SteadyClock::now() >= nextTick) {
            config_.onTick();
            nextTick = SteadyClock::now() + tickInterval;
        }
    }

    for (const auto& entry : conns_) {
        ::close(entry.first);
    }
    conns_.clear();
}

void Server::handleEvent(std::uint32_t flags, int fd) {
    if (fd == listenFd_) {
        acceptClients();
        return;
    }
    if (fd == wakeFd_) {
        std::uint64_t counter = 0;
        const ssize_t got = ::read(wakeFd_, &counter, sizeof(counter));  // reset the eventfd
        (void)got;
        return;  // run() checks stopRequested_ at the top of its loop
    }

    const auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    Connection& conn = it->second;

    bool alive = (flags & kEpollErr) == 0;
    if (alive && (flags & kEpollIn) != 0 && !conn.closeAfterWrite) {
        alive = onReadable(conn);
    }
    if (alive && (flags & kEpollOut) != 0) {
        alive = flush(conn);
    }
    if (alive && (flags & kEpollHup) != 0 && (flags & kEpollIn) == 0) {
        alive = false;  // peer is fully gone and there is nothing left to read
    }
    if (!alive) {
        closeConnection(fd);
    }
}

void Server::acceptClients() {
    for (;;) {
        const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // For example EMFILE (out of file descriptors). Known limitation: while
                // that lasts, the loop keeps retrying; there is no connection limit yet.
                logError("accept failed: " + errnoMessage(errno));
            }
            return;
        }

        // Small replies should go out immediately, not wait for Nagle's algorithm.
        const int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        if (!addToEpoll(epollFd_, fd, kEpollIn)) {
            logError("epoll_ctl add failed: " + errnoMessage(errno));
            ::close(fd);
            continue;
        }
        Connection conn;
        conn.fd = fd;
        conn.events = kEpollIn;
        conns_.emplace(fd, std::move(conn));
    }
}

bool Server::onReadable(Connection& conn) {
    const ssize_t got = ::recv(conn.fd, readBuffer_.data(), readBuffer_.size(), 0);
    if (got < 0) {
        // EAGAIN/EINTR: nothing to read after all. Anything else (e.g. reset): drop the client.
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    if (got == 0) {
        // The client closed its sending side. It may still be waiting for replies, so
        // finish writing them first and close afterwards.
        conn.closeAfterWrite = true;
        return flush(conn);
    }

    conn.in.append(readBuffer_.data(), static_cast<std::size_t>(got));
    processInput(conn);

    // Group commit: everything this read executed is made durable in one step, and only
    // then do the replies leave. A reply is a promise; it must not outrun the disk.
    if (!handler_.commit()) {
        logError("commit failed; closing client without acknowledging its commands");
        conn.failed = true;
    }
    return flush(conn);
}

void Server::processInput(Connection& conn) {
    const std::string_view input(conn.in);
    std::size_t pos = 0;

    while (pos < input.size()) {
        const resp::ParseResult parsed = resp::parseCommand(input.substr(pos));
        if (parsed.status == resp::ParseStatus::kNeedMore) {
            break;  // wait for more bytes; the partial command stays in conn.in
        }
        if (parsed.status == resp::ParseStatus::kError) {
            // We can no longer tell where the next command starts, so reply and hang up.
            resp::appendError(conn.out, "ERR " + parsed.error);
            conn.closeAfterWrite = true;
            pos = input.size();
            break;
        }

        handler_.execute(parsed.command, conn.out);
        pos += parsed.consumed;

        if (pendingOutput(conn.out, conn.outOffset) > kMaxPendingOutputBytes) {
            logError("closing client: too many unread replies");
            conn.failed = true;
            return;
        }
    }

    conn.in.erase(0, pos);  // drop what was consumed (one erase per read, not per command)

    if (conn.in.size() > kMaxPendingInputBytes) {
        resp::appendError(conn.out, "ERR Protocol error: command too large");
        conn.closeAfterWrite = true;
        conn.in.clear();
    }
}

bool Server::flush(Connection& conn) {
    if (conn.failed) {
        return false;
    }

    while (conn.outOffset < conn.out.size()) {
        const ssize_t sent = ::send(conn.fd, conn.out.data() + conn.outOffset,
                                    conn.out.size() - conn.outOffset, MSG_NOSIGNAL);
        if (sent > 0) {
            conn.outOffset += static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // socket buffer is full; epoll will tell us when it drains
        } else {
            return false;  // connection broken
        }
    }

    if (conn.outOffset == conn.out.size()) {
        conn.out.clear();
        conn.outOffset = 0;
        if (conn.closeAfterWrite) {
            return false;  // everything has been delivered; now close
        }
    }
    return updateInterest(conn);
}

bool Server::updateInterest(Connection& conn) {
    // Read while the connection is open; ask about writability only while data is waiting.
    std::uint32_t wanted = conn.closeAfterWrite ? 0U : kEpollIn;
    if (pendingOutput(conn.out, conn.outOffset) > 0) {
        wanted |= kEpollOut;
    }
    if (wanted == conn.events) {
        return true;
    }

    epoll_event ev{};
    ev.events = wanted;
    ev.data.fd = conn.fd;
    if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, conn.fd, &ev) != 0) {
        logError("epoll_ctl mod failed: " + errnoMessage(errno));
        return false;
    }
    conn.events = wanted;
    return true;
}

void Server::closeConnection(int fd) {
    ::close(fd);  // closing the descriptor also removes it from the epoll set
    conns_.erase(fd);
}

}  // namespace raftkv
