#pragma once

// Helpers shared by the socket-level tests.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <poll.h>
#include <stdlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <string>
#include <string_view>

namespace raftkv_test {

// Encodes a command the way redis-cli does: an array of bulk strings.
inline std::string encodeCommand(std::initializer_list<std::string_view> args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const std::string_view arg : args) {
        out += "$" + std::to_string(arg.size()) + "\r\n";
        out.append(arg);
        out += "\r\n";
    }
    return out;
}

// A blocking TCP client for tests.
class TestClient {
public:
    explicit TestClient(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        const timeval timeout{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        const int yes = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        connected_ = ::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }

    ~TestClient() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    bool connected() const { return connected_; }
    bool sawEof() const { return sawEof_; }

    bool send(std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }

    // Reads until `count` bytes arrived, or the connection ends or times out.
    std::string readExactly(std::size_t count) {
        std::string out;
        out.reserve(count);
        std::array<char, 65536> buffer{};
        while (out.size() < count) {
            const std::size_t want = std::min(buffer.size(), count - out.size());
            const ssize_t n = ::recv(fd_, buffer.data(), want, 0);
            if (n > 0) {
                out.append(buffer.data(), static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                sawEof_ = (n == 0);
                break;
            }
        }
        return out;
    }

    // Reads until the server closes the connection (or the 5 second timeout hits).
    std::string readUntilEof() {
        std::string out;
        std::array<char, 4096> buffer{};
        for (;;) {
            const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (n > 0) {
                out.append(buffer.data(), static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                sawEof_ = (n == 0);
                return out;
            }
        }
    }

    void shutdownWrite() { ::shutdown(fd_, SHUT_WR); }

    // True if bytes (or EOF) can be read within `timeoutMs`.
    bool waitReadable(int timeoutMs) {
        pollfd p{};
        p.fd = fd_;
        p.events = POLLIN;
        return ::poll(&p, 1, timeoutMs) > 0;
    }

private:
    int fd_ = -1;
    bool connected_ = false;
    bool sawEof_ = false;
};

// A fresh temporary directory, deleted with everything in it when the object goes away.
class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "raftkv-test-XXXXXX").string();
        const char* made = ::mkdtemp(pattern.data());
        if (made != nullptr) {
            path_ = made;
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

inline std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

}  // namespace raftkv_test
