#pragma once

// Append-only file (AOF): a log of every write command, replayed on startup to rebuild the data.
//
// Record format (all integers little-endian):
//
//   [u32 payloadLen] [u32 crc32(payload)] [payload]
//   payload = [i64 timestampMs] [u32 argc] ( [u32 argLen] [argLen bytes] ) * argc
//
// The timestamp is the instant the command originally ran. Replay pins the clock to it, so
// "EXPIRE k 10" recomputes the same absolute deadline it did the first time.
//
// Recovery is point-in-time: replay stops at the first record that is incomplete or fails its
// checksum, and the file is cut off there. A crash in the middle of a write leaves such a
// torn record at the end; losing it loses only a command that was never acknowledged.
//
// Failure policy is fail-stop: after any write or fsync error the file is marked failed and
// stays failed. We never retry an fsync that reported an error, because the kernel may have
// already dropped the dirty pages, so a later "successful" fsync would prove nothing.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "resp/parser.hpp"
#include "store/clock.hpp"

namespace raftkv {

enum class FsyncPolicy {
    kAlways,    // fdatasync before acknowledging every batch: nothing acknowledged is ever lost
    kEverySec,  // fdatasync about once per second: a crash can lose roughly the last second
    kNo,        // never fsync: the OS decides; a power loss can lose much more
};

// "always", "everysec" or "no". Returns false for anything else.
bool parseFsyncPolicy(std::string_view text, FsyncPolicy& policy);

namespace aof {

// Largest payload accepted. Anything bigger is treated as corruption (a damaged length field).
inline constexpr std::size_t kMaxPayloadBytes = 128UL * 1024 * 1024;
inline constexpr std::size_t kHeaderBytes = 8;

// Appends one complete record (header + payload) to `out`. Returns false if too large.
bool encodeRecord(std::string& out, TimestampMs timestampMs, const resp::Command& command);

enum class DecodeStatus { kOk, kNeedMore, kCorrupt };

struct DecodeResult {
    DecodeStatus status = DecodeStatus::kNeedMore;
    std::size_t consumed = 0;  // bytes used by the record, only for kOk
    TimestampMs timestampMs = 0;
    resp::Command command;
};

// Decodes the record at the start of `buffer`. kNeedMore means the buffer ends mid-record.
DecodeResult decodeRecord(std::string_view buffer);

}  // namespace aof

struct ReplayStats {
    std::size_t records = 0;           // records applied
    std::uint64_t validBytes = 0;      // length of the intact prefix
    std::uint64_t discardedBytes = 0;  // bytes cut off the end (torn or corrupt tail)
    TimestampMs lastTimestampMs = 0;   // timestamp of the last applied record (0 if none)
};

class AppendOnlyFile {
public:
    using ApplyFn = std::function<void(TimestampMs, const resp::Command&)>;

    // `clock` is used only to decide when an everysec fsync is due; it must outlive this object.
    AppendOnlyFile(const Clock& clock, FsyncPolicy policy) : clock_(clock), policy_(policy) {}
    ~AppendOnlyFile();

    AppendOnlyFile(const AppendOnlyFile&) = delete;
    AppendOnlyFile& operator=(const AppendOnlyFile&) = delete;

    // Opens (creating if needed) the file, calls `apply` for every intact record in order,
    // cuts off any torn or corrupt tail, and leaves the file ready for append().
    // On a newly created file the parent directory is fsynced too, so the file itself survives a crash.
    bool open(const std::string& path, const ApplyFn& apply, ReplayStats& stats, std::string& error);

    // Queues one record in memory. Nothing reaches the OS until commit().
    void append(TimestampMs timestampMs, const resp::Command& command);

    // Writes everything queued and fsyncs according to the policy.
    // Returns false once the file has failed (now or earlier).
    bool commit();

    // Periodic housekeeping: with kEverySec/kAlways, fsyncs anything written but not yet synced.
    void tick();

    // Writes, fsyncs and closes. Safe to call twice.
    bool close();

    bool failed() const { return failed_; }
    const std::string& failureReason() const { return failureReason_; }

private:
    bool writeQueued();
    bool syncNow();
    void fail(const std::string& reason);

    const Clock& clock_;
    FsyncPolicy policy_;
    int fd_ = -1;
    std::string queued_;       // encoded records waiting for commit()
    bool unsynced_ = false;    // bytes written to the OS but not yet fsynced
    TimestampMs lastSyncMs_ = 0;
    bool failed_ = false;
    std::string failureReason_;
};

}  // namespace raftkv
