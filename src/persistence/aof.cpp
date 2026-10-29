#include "persistence/aof.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include "persistence/crc32.hpp"

namespace raftkv {
namespace {

constexpr std::size_t kReplayChunkBytes = 1024 * 1024;
constexpr TimestampMs kEverySecIntervalMs = 1000;
constexpr std::size_t kMinPayloadBytes = 8 + 4;  // timestamp + argc

std::string errnoMessage(int err) {
    return std::system_category().message(err);
}

void putU32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

void putU64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

std::uint32_t getU32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}

std::uint64_t getU64(const char* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}

std::string parentDirectory(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? "/" : path.substr(0, slash);
}

// fdatasync, retrying only when interrupted by a signal (never after a real error).
bool fdatasyncRetry(int fd) {
    for (;;) {
        if (::fdatasync(fd) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

bool writeAll(int fd, const char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, data + done, size - done);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            if (n == 0) {
                errno = EIO;
            }
            return false;
        }
    }
    return true;
}

}  // namespace

bool parseFsyncPolicy(std::string_view text, FsyncPolicy& policy) {
    if (text == "always") {
        policy = FsyncPolicy::kAlways;
    } else if (text == "everysec") {
        policy = FsyncPolicy::kEverySec;
    } else if (text == "no") {
        policy = FsyncPolicy::kNo;
    } else {
        return false;
    }
    return true;
}

namespace aof {

bool encodeRecord(std::string& out, TimestampMs timestampMs, const resp::Command& command) {
    std::size_t payloadSize = kMinPayloadBytes;
    for (const std::string& arg : command) {
        payloadSize += 4 + arg.size();
        if (payloadSize > kMaxPayloadBytes) {
            return false;
        }
    }
    if (command.size() > resp::kMaxArgs) {
        return false;
    }

    std::string payload;
    payload.reserve(payloadSize);
    putU64(payload, static_cast<std::uint64_t>(timestampMs));
    putU32(payload, static_cast<std::uint32_t>(command.size()));
    for (const std::string& arg : command) {
        putU32(payload, static_cast<std::uint32_t>(arg.size()));
        payload.append(arg);
    }

    putU32(out, static_cast<std::uint32_t>(payload.size()));
    putU32(out, crc32(payload.data(), payload.size()));
    out.append(payload);
    return true;
}

DecodeResult decodeRecord(std::string_view buffer) {
    DecodeResult result;
    if (buffer.size() < kHeaderBytes) {
        result.status = DecodeStatus::kNeedMore;
        return result;
    }

    const std::uint32_t payloadLen = getU32(buffer.data());
    const std::uint32_t expectedCrc = getU32(buffer.data() + 4);
    if (payloadLen < kMinPayloadBytes || payloadLen > kMaxPayloadBytes) {
        result.status = DecodeStatus::kCorrupt;
        return result;
    }
    if (buffer.size() - kHeaderBytes < payloadLen) {
        result.status = DecodeStatus::kNeedMore;
        return result;
    }

    const char* payload = buffer.data() + kHeaderBytes;
    if (crc32(payload, payloadLen) != expectedCrc) {
        result.status = DecodeStatus::kCorrupt;
        return result;
    }

    // The checksum matched, but still validate the structure so we never read out of bounds.
    const std::uint64_t timestamp = getU64(payload);
    const std::uint32_t argc = getU32(payload + 8);
    std::size_t pos = kMinPayloadBytes;
    if (argc > resp::kMaxArgs || argc > (payloadLen - pos) / 4) {
        result.status = DecodeStatus::kCorrupt;
        return result;
    }

    resp::Command command;
    command.reserve(argc);
    for (std::uint32_t i = 0; i < argc; ++i) {
        if (payloadLen - pos < 4) {
            result.status = DecodeStatus::kCorrupt;
            return result;
        }
        const std::uint32_t argLen = getU32(payload + pos);
        pos += 4;
        if (argLen > payloadLen - pos) {
            result.status = DecodeStatus::kCorrupt;
            return result;
        }
        command.emplace_back(payload + pos, argLen);
        pos += argLen;
    }
    if (pos != payloadLen) {
        result.status = DecodeStatus::kCorrupt;
        return result;
    }

    result.status = DecodeStatus::kOk;
    result.consumed = kHeaderBytes + payloadLen;
    result.timestampMs = static_cast<TimestampMs>(timestamp);
    result.command = std::move(command);
    return result;
}

}  // namespace aof

AppendOnlyFile::~AppendOnlyFile() {
    close();
}

void AppendOnlyFile::fail(const std::string& reason) {
    if (!failed_) {
        failed_ = true;
        failureReason_ = reason;
    }
}

bool AppendOnlyFile::open(const std::string& path, const ApplyFn& apply, ReplayStats& stats,
                          std::string& error) {
    stats = ReplayStats{};

    // Try to create it exclusively first, so we know whether the directory entry is new.
    bool created = false;
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ >= 0) {
        created = true;
    } else if (errno == EEXIST) {
        fd_ = ::open(path.c_str(), O_RDWR | O_APPEND | O_CLOEXEC);
    }
    if (fd_ < 0) {
        error = "cannot open '" + path + "': " + errnoMessage(errno);
        return false;
    }

    const auto giveUp = [&](const std::string& message) {
        error = message + ": " + errnoMessage(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    };

    if (created) {
        // Without this, a crash right after creation can lose the directory entry, and the
        // file (with everything fsynced into it) would vanish.
        const std::string dir = parentDirectory(path);
        const int dirFd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0) {
            return giveUp("cannot open directory '" + dir + "'");
        }
        const bool synced = ::fsync(dirFd) == 0;
        const int savedErrno = errno;
        ::close(dirFd);
        if (!synced) {
            errno = savedErrno;
            return giveUp("cannot fsync directory '" + dir + "'");
        }
    }

    // Replay: stream the file in chunks, decoding records as they become complete.
    std::string buffer;
    std::size_t bufferPos = 0;
    std::uint64_t fileOffset = 0;
    bool eof = false;
    bool corrupt = false;
    std::string chunk(kReplayChunkBytes, '\0');

    while (!corrupt) {
        for (;;) {
            aof::DecodeResult record = aof::decodeRecord(std::string_view(buffer).substr(bufferPos));
            if (record.status == aof::DecodeStatus::kOk) {
                apply(record.timestampMs, record.command);
                bufferPos += record.consumed;
                stats.validBytes += record.consumed;
                stats.lastTimestampMs = record.timestampMs;
                ++stats.records;
                continue;
            }
            corrupt = record.status == aof::DecodeStatus::kCorrupt;
            break;
        }
        if (corrupt || eof) {
            break;
        }

        buffer.erase(0, bufferPos);
        bufferPos = 0;
        const ssize_t got = ::pread(fd_, chunk.data(), chunk.size(), static_cast<off_t>(fileOffset));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return giveUp("cannot read '" + path + "'");
        }
        if (got == 0) {
            eof = true;
        } else {
            buffer.append(chunk.data(), static_cast<std::size_t>(got));
            fileOffset += static_cast<std::uint64_t>(got);
        }
    }

    // Cut off whatever follows the last intact record.
    struct stat info {};
    if (::fstat(fd_, &info) != 0) {
        return giveUp("cannot stat '" + path + "'");
    }
    const auto fileSize = static_cast<std::uint64_t>(info.st_size);
    if (S_ISREG(info.st_mode) && fileSize > stats.validBytes) {
        stats.discardedBytes = fileSize - stats.validBytes;
        if (::ftruncate(fd_, static_cast<off_t>(stats.validBytes)) != 0) {
            return giveUp("cannot truncate '" + path + "'");
        }
        if (!fdatasyncRetry(fd_)) {
            return giveUp("cannot fsync '" + path + "'");
        }
    }

    lastSyncMs_ = clock_.nowMs();
    return true;
}

void AppendOnlyFile::append(TimestampMs timestampMs, const resp::Command& command) {
    if (failed_) {
        return;
    }
    if (!aof::encodeRecord(queued_, timestampMs, command)) {
        fail("command too large to log");
    }
}

bool AppendOnlyFile::writeQueued() {
    if (failed_) {
        return false;
    }
    if (queued_.empty()) {
        return true;
    }
    if (fd_ < 0) {
        fail("append-only file is not open");
        return false;
    }
    if (!writeAll(fd_, queued_.data(), queued_.size())) {
        fail("write failed: " + errnoMessage(errno));
        return false;
    }
    queued_.clear();
    unsynced_ = true;
    return true;
}

bool AppendOnlyFile::syncNow() {
    if (failed_) {
        return false;  // never retry an fsync that already reported an error
    }
    if (!unsynced_) {
        return true;
    }
    if (!fdatasyncRetry(fd_)) {
        fail("fsync failed: " + errnoMessage(errno));
        return false;
    }
    unsynced_ = false;
    lastSyncMs_ = clock_.nowMs();
    return true;
}

bool AppendOnlyFile::commit() {
    if (failed_) {
        return false;
    }
    if (!writeQueued()) {
        return false;
    }
    switch (policy_) {
        case FsyncPolicy::kAlways:
            return syncNow();
        case FsyncPolicy::kEverySec:
            if (unsynced_ && clock_.nowMs() - lastSyncMs_ >= kEverySecIntervalMs) {
                return syncNow();
            }
            return true;
        case FsyncPolicy::kNo:
            return true;
    }
    return true;
}

void AppendOnlyFile::tick() {
    if (failed_ || policy_ == FsyncPolicy::kNo) {
        return;
    }
    // Whatever was written since the last sync is flushed now, so with everysec the
    // unsynced window stays about one tick (one second) long.
    syncNow();
}

bool AppendOnlyFile::close() {
    if (fd_ < 0) {
        return !failed_;
    }
    bool ok = !failed_;
    if (ok) {
        ok = writeQueued() && (policy_ == FsyncPolicy::kNo ? true : syncNow());
    }
    ::close(fd_);
    fd_ = -1;
    return ok;
}

}  // namespace raftkv
