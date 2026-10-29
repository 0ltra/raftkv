#pragma once

// TCP server: a single-threaded epoll event loop that speaks RESP.
//
//   bytes in -> parseCommand() -> CommandDispatcher::execute() -> bytes out
//
// One thread owns every socket, every connection buffer and the store, so no
// locks are needed. Sockets are non-blocking and the epoll mode is
// level-triggered (simplest to get right: epoll keeps reporting a socket until
// it has been drained).
//
// Linux only (epoll, eventfd, accept4). IPv4 only.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "commands/dispatcher.hpp"

namespace raftkv {

struct ServerConfig {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 6380;  // 0 = let the OS pick a free port (read it back with port())

    // Optional hook, called on the event-loop thread roughly every tickIntervalMs.
    // Used for housekeeping (sweeping expired keys now; Raft timers later).
    std::function<void()> onTick;
    int tickIntervalMs = 1000;
};

class Server {
public:
    // The dispatcher must outlive the server.
    Server(ServerConfig config, CommandDispatcher& dispatcher);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts listening. On failure returns false and fills `error`.
    // Call once, before run().
    bool start(std::string& error);

    // Runs the event loop on the calling thread until stop() is called.
    void run();

    // Asks run() to return. Safe to call from another thread or a signal handler.
    void stop() noexcept;

    // The port actually bound (useful when the config asked for port 0).
    std::uint16_t port() const { return port_; }

private:
    struct Connection {
        int fd = -1;
        std::string in;                // bytes received but not yet parsed into a full command
        std::string out;               // reply bytes not yet fully sent
        std::size_t outOffset = 0;     // how much of `out` has already been sent
        std::uint32_t events = 0;      // the epoll events we are currently registered for
        bool closeAfterWrite = false;  // flush remaining replies, then close
        bool failed = false;           // close immediately, dropping unsent replies
    };

    void acceptClients();
    void handleEvent(std::uint32_t flags, int fd);
    bool onReadable(Connection& conn);
    void processInput(Connection& conn);
    bool flush(Connection& conn);
    bool updateInterest(Connection& conn);
    void closeConnection(int fd);
    void closeListenerFds();

    ServerConfig config_;
    CommandDispatcher& dispatcher_;

    int listenFd_ = -1;
    int epollFd_ = -1;
    int wakeFd_ = -1;  // eventfd that stop() writes to, to wake epoll_wait
    std::uint16_t port_ = 0;
    std::atomic<bool> stopRequested_{false};

    std::unordered_map<int, Connection> conns_;
    std::vector<char> readBuffer_;
};

}  // namespace raftkv
