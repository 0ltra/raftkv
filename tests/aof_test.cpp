#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "persistence/aof.hpp"
#include "store/clock.hpp"
#include "test_support.hpp"

namespace {

using raftkv::AppendOnlyFile;
using raftkv::FsyncPolicy;
using raftkv::ReplayStats;
using raftkv::TimestampMs;
using raftkv::aof::DecodeStatus;
using raftkv::aof::decodeRecord;
using raftkv::aof::encodeRecord;
using raftkv_test::readFile;
using raftkv_test::TempDir;
using raftkv_test::writeFile;

using Logged = std::pair<TimestampMs, raftkv::resp::Command>;

std::string encoded(TimestampMs ts, const raftkv::resp::Command& command) {
    std::string out;
    EXPECT_TRUE(encodeRecord(out, ts, command));
    return out;
}

// Opens a file and returns everything that was replayed.
struct Opened {
    bool ok = false;
    std::string error;
    ReplayStats stats;
    std::vector<Logged> replayed;
};

Opened openFile(AppendOnlyFile& file, const std::string& path) {
    Opened result;
    result.ok = file.open(
        path,
        [&](TimestampMs ts, const raftkv::resp::Command& c) { result.replayed.emplace_back(ts, c); },
        result.stats, result.error);
    return result;
}

Opened replayOnly(const std::string& path) {
    raftkv::ManualClock clock;
    AppendOnlyFile file(clock, FsyncPolicy::kNo);
    Opened result = openFile(file, path);
    file.close();
    return result;
}

}  // namespace

// ---- record encoding -------------------------------------------------------------------

TEST(AofRecord, RoundTripsBinarySafeArguments) {
    const raftkv::resp::Command command{"SET", std::string("a\0b\r\n", 5), "", std::string(1000, '\xff')};
    const std::string bytes = encoded(123456789012345, command);
    const auto result = decodeRecord(bytes);
    ASSERT_EQ(result.status, DecodeStatus::kOk);
    EXPECT_EQ(result.consumed, bytes.size());
    EXPECT_EQ(result.timestampMs, 123456789012345);
    EXPECT_EQ(result.command, command);
}

TEST(AofRecord, RoundTripsNegativeTimestampAndEmptyCommand) {
    const std::string bytes = encoded(-5, {});
    const auto result = decodeRecord(bytes);
    ASSERT_EQ(result.status, DecodeStatus::kOk);
    EXPECT_EQ(result.timestampMs, -5);
    EXPECT_TRUE(result.command.empty());
}

TEST(AofRecord, DecodesOnlyTheFirstOfTwoRecords) {
    const std::string first = encoded(1, {"PING"});
    const std::string both = first + encoded(2, {"GET", "k"});
    const auto result = decodeRecord(both);
    ASSERT_EQ(result.status, DecodeStatus::kOk);
    EXPECT_EQ(result.consumed, first.size());
    EXPECT_EQ(result.timestampMs, 1);
}

TEST(AofRecord, EveryStrictPrefixNeedsMoreBytes) {
    const std::string bytes = encoded(7, {"SET", "key", "value"});
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        EXPECT_EQ(decodeRecord(std::string_view(bytes).substr(0, n)).status, DecodeStatus::kNeedMore)
            << "prefix length " << n;
    }
}

TEST(AofRecord, NoSingleBitFlipIsAccepted) {
    std::string bytes = encoded(7, {"SET", "key", "value"});
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            bytes[i] = static_cast<char>(bytes[i] ^ (1 << bit));
            EXPECT_NE(decodeRecord(bytes).status, DecodeStatus::kOk) << "byte " << i << " bit " << bit;
            bytes[i] = static_cast<char>(bytes[i] ^ (1 << bit));
        }
    }
}

TEST(AofRecord, ImplausibleLengthsAreCorrupt) {
    std::string zero(16, '\0');
    EXPECT_EQ(decodeRecord(zero).status, DecodeStatus::kCorrupt);

    std::string huge(16, '\0');
    huge[3] = '\x40';  // payload length 0x40000000, far above the cap
    EXPECT_EQ(decodeRecord(huge).status, DecodeStatus::kCorrupt);
}

TEST(AofRecord, RejectsCommandsTooLargeToLog) {
    std::string out;
    const raftkv::resp::Command command{std::string(raftkv::aof::kMaxPayloadBytes, 'x')};
    EXPECT_FALSE(encodeRecord(out, 1, command));
}

TEST(FsyncPolicyParsing, AcceptsOnlyTheThreeNames) {
    FsyncPolicy policy = FsyncPolicy::kNo;
    EXPECT_TRUE(raftkv::parseFsyncPolicy("always", policy));
    EXPECT_EQ(policy, FsyncPolicy::kAlways);
    EXPECT_TRUE(raftkv::parseFsyncPolicy("everysec", policy));
    EXPECT_EQ(policy, FsyncPolicy::kEverySec);
    EXPECT_TRUE(raftkv::parseFsyncPolicy("no", policy));
    EXPECT_EQ(policy, FsyncPolicy::kNo);
    EXPECT_FALSE(raftkv::parseFsyncPolicy("", policy));
    EXPECT_FALSE(raftkv::parseFsyncPolicy("Always", policy));
    EXPECT_FALSE(raftkv::parseFsyncPolicy("sometimes", policy));
}

// ---- the file --------------------------------------------------------------------------

TEST(Aof, CreatesAnEmptyFileAndReplaysNothing) {
    TempDir dir;
    const std::string path = dir.file("a.aof");
    const Opened opened = replayOnly(path);
    ASSERT_TRUE(opened.ok) << opened.error;
    EXPECT_EQ(opened.stats.records, 0U);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(std::filesystem::file_size(path), 0U);
}

TEST(Aof, WhatIsCommittedIsReplayedInOrderAfterReopening) {
    TempDir dir;
    const std::string path = dir.file("a.aof");
    raftkv::ManualClock clock;
    {
        AppendOnlyFile file(clock, FsyncPolicy::kAlways);
        ASSERT_TRUE(openFile(file, path).ok);
        file.append(10, {"SET", "a", "1"});
        file.append(20, {"INCR", "a"});
        ASSERT_TRUE(file.commit());
        file.append(30, {"DEL", "a"});
        ASSERT_TRUE(file.commit());
    }
    const Opened opened = replayOnly(path);
    ASSERT_TRUE(opened.ok) << opened.error;
    ASSERT_EQ(opened.replayed.size(), 3U);
    EXPECT_EQ(opened.replayed[0], (Logged{10, {"SET", "a", "1"}}));
    EXPECT_EQ(opened.replayed[1], (Logged{20, {"INCR", "a"}}));
    EXPECT_EQ(opened.replayed[2], (Logged{30, {"DEL", "a"}}));
    EXPECT_EQ(opened.stats.lastTimestampMs, 30);
    EXPECT_EQ(opened.stats.discardedBytes, 0U);
}

TEST(Aof, NothingReachesTheFileBeforeCommit) {
    TempDir dir;
    const std::string path = dir.file("a.aof");
    raftkv::ManualClock clock;
    AppendOnlyFile file(clock, FsyncPolicy::kNo);
    ASSERT_TRUE(openFile(file, path).ok);
    file.append(1, {"SET", "a", "1"});
    EXPECT_EQ(std::filesystem::file_size(path), 0U);
    ASSERT_TRUE(file.commit());
    EXPECT_GT(std::filesystem::file_size(path), 0U);
}

TEST(Aof, CloseWritesWhatWasStillQueued) {
    TempDir dir;
    const std::string path = dir.file("a.aof");
    raftkv::ManualClock clock;
    {
        AppendOnlyFile file(clock, FsyncPolicy::kEverySec);
        ASSERT_TRUE(openFile(file, path).ok);
        file.append(1, {"SET", "a", "1"});
        EXPECT_TRUE(file.close());
        EXPECT_TRUE(file.close());  // closing twice is harmless
    }
    EXPECT_EQ(replayOnly(path).replayed.size(), 1U);
}

TEST(Aof, ReopeningAppendsAfterTheExistingRecords) {
    TempDir dir;
    const std::string path = dir.file("a.aof");
    raftkv::ManualClock clock;
    for (int round = 0; round < 3; ++round) {
        AppendOnlyFile file(clock, FsyncPolicy::kNo);
        const Opened opened = openFile(file, path);
        ASSERT_TRUE(opened.ok) << opened.error;
        EXPECT_EQ(opened.replayed.size(), static_cast<std::size_t>(round));
        file.append(round, {"INCR", "n"});
        ASSERT_TRUE(file.commit());
    }
    EXPECT_EQ(replayOnly(path).replayed.size(), 3U);
}

TEST(Aof, ATornTailIsCutOffAtEveryPossibleLength) {
    TempDir dir;
    const std::string good = encoded(1, {"SET", "a", "1"}) + encoded(2, {"SET", "b", "2"});
    const std::string last = encoded(3, {"SET", "c", "3"});

    // Simulate a crash after any number of bytes of the last record reached the disk.
    for (std::size_t kept = 0; kept < last.size(); ++kept) {
        const std::string path = dir.file("torn-" + std::to_string(kept) + ".aof");
        writeFile(path, good + last.substr(0, kept));

        raftkv::ManualClock clock;
        AppendOnlyFile file(clock, FsyncPolicy::kNo);
        const Opened opened = openFile(file, path);
        ASSERT_TRUE(opened.ok) << opened.error;
        EXPECT_EQ(opened.replayed.size(), 2U) << "kept=" << kept;
        EXPECT_EQ(opened.stats.discardedBytes, kept) << "kept=" << kept;
        EXPECT_EQ(std::filesystem::file_size(path), good.size()) << "kept=" << kept;

        // The log keeps working afterwards, and the new record lands right after the good ones.
        file.append(4, {"SET", "d", "4"});
        ASSERT_TRUE(file.commit());
        file.close();
        const Opened again = replayOnly(path);
        ASSERT_EQ(again.replayed.size(), 3U) << "kept=" << kept;
        EXPECT_EQ(again.replayed[2].second[1], "d");
    }
}

TEST(Aof, ACorruptRecordEndsRecoveryThereEvenIfLaterRecordsAreIntact) {
    TempDir dir;
    const std::string first = encoded(1, {"SET", "a", "1"});
    std::string second = encoded(2, {"SET", "b", "2"});
    const std::string third = encoded(3, {"SET", "c", "3"});
    second[second.size() - 1] = static_cast<char>(second[second.size() - 1] ^ 0x01);  // flip a payload bit

    const std::string path = dir.file("a.aof");
    writeFile(path, first + second + third);
    const Opened opened = replayOnly(path);
    ASSERT_TRUE(opened.ok) << opened.error;
    EXPECT_EQ(opened.replayed.size(), 1U);
    EXPECT_EQ(opened.stats.validBytes, first.size());
    EXPECT_EQ(opened.stats.discardedBytes, second.size() + third.size());
    EXPECT_EQ(readFile(path), first);
}

TEST(Aof, TrailingGarbageIsDiscarded) {
    TempDir dir;
    const std::string good = encoded(1, {"SET", "a", "1"});
    const std::string path = dir.file("a.aof");
    writeFile(path, good + std::string(100, '\x7f'));
    const Opened opened = replayOnly(path);
    ASSERT_TRUE(opened.ok) << opened.error;
    EXPECT_EQ(opened.replayed.size(), 1U);
    EXPECT_EQ(opened.stats.discardedBytes, 100U);
}

TEST(Aof, RecordsSpanningReplayChunkBoundariesAreReadCorrectly) {
    // The reader works in 1 MiB chunks; mix small records with ones bigger than a chunk.
    TempDir dir;
    std::mt19937 rng(12345);
    std::vector<Logged> written;
    std::string contents;
    for (int i = 0; i < 400; ++i) {
        const std::size_t size = (i == 100) ? 3 * 1024 * 1024 + 17 : (rng() % 20000);
        std::string value(size, '\0');
        for (char& c : value) {
            c = static_cast<char>(rng());
        }
        raftkv::resp::Command command{"SET", "key" + std::to_string(i), value};
        contents += encoded(i, command);
        written.emplace_back(i, std::move(command));
    }
    const std::string path = dir.file("big.aof");
    writeFile(path, contents);

    const Opened opened = replayOnly(path);
    ASSERT_TRUE(opened.ok) << opened.error;
    ASSERT_EQ(opened.replayed.size(), written.size());
    for (std::size_t i = 0; i < written.size(); ++i) {
        ASSERT_EQ(opened.replayed[i], written[i]) << "record " << i;
    }
    EXPECT_EQ(opened.stats.discardedBytes, 0U);
}

TEST(Aof, OpeningAnImpossiblePathFailsWithAMessage) {
    TempDir dir;
    raftkv::ManualClock clock;
    AppendOnlyFile file(clock, FsyncPolicy::kNo);
    const Opened opened = openFile(file, dir.file("no-such-dir/a.aof"));
    EXPECT_FALSE(opened.ok);
    EXPECT_NE(opened.error.find("no-such-dir"), std::string::npos);
}

TEST(Aof, OpeningADirectoryFails) {
    TempDir dir;
    raftkv::ManualClock clock;
    AppendOnlyFile file(clock, FsyncPolicy::kNo);
    EXPECT_FALSE(openFile(file, dir.file("")).ok);
}

TEST(Aof, ADiskThatRejectsWritesMakesCommitFailAndStayFailed) {
    // /dev/full accepts open() but every write() fails with ENOSPC.
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "/dev/full not available";
    }
    raftkv::ManualClock clock;
    AppendOnlyFile file(clock, FsyncPolicy::kAlways);
    ASSERT_TRUE(openFile(file, "/dev/full").ok);
    EXPECT_FALSE(file.failed());

    file.append(1, {"SET", "a", "1"});
    EXPECT_FALSE(file.commit());
    EXPECT_TRUE(file.failed());
    EXPECT_NE(file.failureReason().find("write failed"), std::string::npos) << file.failureReason();

    // Fail-stop: it never recovers, and an empty commit does not pretend everything is fine.
    EXPECT_FALSE(file.commit());
    file.append(2, {"SET", "b", "2"});
    EXPECT_FALSE(file.commit());
    EXPECT_FALSE(file.close());
}

TEST(Aof, CommitWithNothingQueuedSucceeds) {
    TempDir dir;
    raftkv::ManualClock clock;
    AppendOnlyFile file(clock, FsyncPolicy::kAlways);
    ASSERT_TRUE(openFile(file, dir.file("a.aof")).ok);
    EXPECT_TRUE(file.commit());
    file.tick();
    EXPECT_FALSE(file.failed());
}

TEST(Aof, AFailedFsyncIsNeverRetriedIntoSuccess) {
    // fdatasync on /dev/null fails with EINVAL while write() succeeds, which gives us an
    // fsync failure without needing a broken disk. (If a system accepts it, skip.)
    raftkv::ManualClock clock;
    AppendOnlyFile file(clock, FsyncPolicy::kAlways);
    ASSERT_TRUE(openFile(file, "/dev/null").ok);

    file.append(1, {"SET", "a", "1"});
    if (file.commit()) {
        GTEST_SKIP() << "fdatasync on /dev/null succeeded here";
    }
    EXPECT_TRUE(file.failed());
    EXPECT_NE(file.failureReason().find("fsync failed"), std::string::npos) << file.failureReason();

    // The write itself went through, so nothing is queued any more. A retry of the fsync could
    // "succeed" after the kernel dropped the data; the file must keep reporting failure instead.
    EXPECT_FALSE(file.commit());
    file.tick();
    EXPECT_FALSE(file.commit());
}
