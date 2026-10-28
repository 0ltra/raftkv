#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "resp/parser.hpp"

using raftkv::resp::Command;
using raftkv::resp::ParseResult;
using raftkv::resp::ParseStatus;
using raftkv::resp::parseCommand;

namespace {
const std::string kGetFoo = "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";
}

TEST(RespParser, ParsesSimpleCommand) {
    const ParseResult r = parseCommand(kGetFoo);
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    EXPECT_EQ(r.consumed, kGetFoo.size());
    EXPECT_EQ(r.command, (Command{"GET", "foo"}));
}

TEST(RespParser, EmptyBufferNeedsMore) {
    EXPECT_EQ(parseCommand("").status, ParseStatus::kNeedMore);
}

TEST(RespParser, EveryStrictPrefixNeedsMore) {
    // Simulates the network delivering the command one byte at a time.
    for (std::size_t n = 0; n < kGetFoo.size(); ++n) {
        const ParseResult r = parseCommand(std::string_view(kGetFoo).substr(0, n));
        EXPECT_EQ(r.status, ParseStatus::kNeedMore) << "prefix length " << n;
    }
}

TEST(RespParser, PipelinedCommandsParseOneAtATime) {
    const std::string two = kGetFoo + "*1\r\n$4\r\nPING\r\n";

    const ParseResult first = parseCommand(two);
    ASSERT_EQ(first.status, ParseStatus::kComplete);
    EXPECT_EQ(first.command, (Command{"GET", "foo"}));
    EXPECT_EQ(first.consumed, kGetFoo.size());

    const ParseResult second = parseCommand(std::string_view(two).substr(first.consumed));
    ASSERT_EQ(second.status, ParseStatus::kComplete);
    EXPECT_EQ(second.command, (Command{"PING"}));
    EXPECT_EQ(first.consumed + second.consumed, two.size());
}

TEST(RespParser, TrailingBytesAreNotConsumed) {
    const std::string input = kGetFoo + "*1\r\n$4\r\nPI";  // second command incomplete
    const ParseResult r = parseCommand(input);
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    EXPECT_EQ(r.consumed, kGetFoo.size());
}

TEST(RespParser, EmptyBulkStringIsAllowed) {
    const ParseResult r = parseCommand("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n");
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    EXPECT_EQ(r.command, (Command{"SET", "k", ""}));
}

TEST(RespParser, BulkStringIsBinarySafe) {
    // The value contains \r\n and a NUL byte; only the declared length matters.
    const std::string value("a\r\nb\0c", 6);
    const std::string input = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$6\r\n" + value + "\r\n";
    const ParseResult r = parseCommand(input);
    ASSERT_EQ(r.status, ParseStatus::kComplete);
    ASSERT_EQ(r.command.size(), 3U);
    EXPECT_EQ(r.command[2], value);
}

TEST(RespParser, RejectsInputNotStartingWithStar) {
    EXPECT_EQ(parseCommand("PING\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsZeroAndNegativeArrayLengths) {
    EXPECT_EQ(parseCommand("*0\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parseCommand("*-1\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsNonNumericLengths) {
    EXPECT_EQ(parseCommand("*abc\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parseCommand("*1\r\n$x\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parseCommand("*1\r\n$+3\r\nabc\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parseCommand("*1\r\n$ 3\r\nabc\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsNegativeBulkLength) {
    EXPECT_EQ(parseCommand("*1\r\n$-1\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsElementThatIsNotABulkString) {
    EXPECT_EQ(parseCommand("*1\r\n:5\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsBulkNotTerminatedByCrlf) {
    EXPECT_EQ(parseCommand("*1\r\n$3\r\nabcXY").status, ParseStatus::kError);
}

TEST(RespParser, RejectsOversizedBulkLengthWithoutWaitingForBody) {
    // 64 MiB + 1: must fail immediately instead of buffering 64 MiB first.
    EXPECT_EQ(parseCommand("*1\r\n$67108865\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsHugeNumbersThatOverflow) {
    EXPECT_EQ(parseCommand("*99999999999999999999999\r\n").status, ParseStatus::kError);
}

TEST(RespParser, RejectsEndlessHeaderLine) {
    const std::string garbage = "*" + std::string(200, '1');  // no \r\n ever arrives
    EXPECT_EQ(parseCommand(garbage).status, ParseStatus::kError);
}
