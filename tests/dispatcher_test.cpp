#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "commands/dispatcher.hpp"
#include "resp/parser.hpp"
#include "store/clock.hpp"
#include "store/store.hpp"

namespace {

constexpr raftkv::TimestampMs kStart = 1'000'000;

class DispatcherTest : public ::testing::Test {
protected:
    // Runs one command and returns the exact reply bytes.
    std::string run(const raftkv::resp::Command& command) {
        std::string out;
        dispatcher.execute(command, out);
        return out;
    }

    raftkv::ManualClock clock{kStart};
    raftkv::Store store{clock};
    raftkv::CommandDispatcher dispatcher{store, clock};
};

}  // namespace

TEST_F(DispatcherTest, PingReplyPong) {
    EXPECT_EQ(run({"PING"}), "+PONG\r\n");
}

TEST_F(DispatcherTest, PingWithMessageEchoesItAsBulkString) {
    EXPECT_EQ(run({"PING", "hello"}), "$5\r\nhello\r\n");
}

TEST_F(DispatcherTest, CommandNamesAreCaseInsensitive) {
    EXPECT_EQ(run({"ping"}), "+PONG\r\n");
    EXPECT_EQ(run({"SeT", "k", "v"}), "+OK\r\n");
    EXPECT_EQ(run({"gEt", "k"}), "$1\r\nv\r\n");
}

TEST_F(DispatcherTest, SetThenGet) {
    EXPECT_EQ(run({"SET", "k", "v"}), "+OK\r\n");
    EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
}

TEST_F(DispatcherTest, GetMissingKeyIsNullBulk) {
    EXPECT_EQ(run({"GET", "nope"}), "$-1\r\n");
}

TEST_F(DispatcherTest, EmptyValueIsNotNull) {
    run({"SET", "k", ""});
    EXPECT_EQ(run({"GET", "k"}), "$0\r\n\r\n");
}

TEST_F(DispatcherTest, DelCountsRemovedKeys) {
    run({"SET", "a", "1"});
    run({"SET", "b", "2"});
    EXPECT_EQ(run({"DEL", "a", "b", "missing"}), ":2\r\n");
    EXPECT_EQ(run({"DEL", "a"}), ":0\r\n");
    EXPECT_EQ(run({"GET", "a"}), "$-1\r\n");
}

TEST_F(DispatcherTest, ExpireThenTtlInSeconds) {
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"EXPIRE", "k", "10"}), ":1\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":10\r\n");
    clock.advanceMs(4000);
    EXPECT_EQ(run({"TTL", "k"}), ":6\r\n");
}

TEST_F(DispatcherTest, KeyDisappearsAfterExpiry) {
    run({"SET", "k", "v"});
    run({"EXPIRE", "k", "10"});
    clock.advanceMs(9999);
    EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
    clock.advanceMs(1);
    EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":-2\r\n");
}

TEST_F(DispatcherTest, TtlRoundsToNearestSecond) {
    run({"SET", "k", "v"});
    store.expireAt("k", kStart + 4600);
    EXPECT_EQ(run({"TTL", "k"}), ":5\r\n");
    clock.advanceMs(4200);  // 400 ms left
    EXPECT_EQ(run({"TTL", "k"}), ":0\r\n");
}

TEST_F(DispatcherTest, TtlMissingAndNoExpiry) {
    EXPECT_EQ(run({"TTL", "nope"}), ":-2\r\n");
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");
}

TEST_F(DispatcherTest, ExpireOnMissingKeyReturnsZero) {
    EXPECT_EQ(run({"EXPIRE", "nope", "10"}), ":0\r\n");
}

TEST_F(DispatcherTest, ExpireWithZeroOrNegativeDeletesTheKey) {
    run({"SET", "a", "1"});
    run({"SET", "b", "2"});
    EXPECT_EQ(run({"EXPIRE", "a", "0"}), ":1\r\n");
    EXPECT_EQ(run({"EXPIRE", "b", "-5"}), ":1\r\n");
    EXPECT_EQ(run({"GET", "a"}), "$-1\r\n");
    EXPECT_EQ(run({"GET", "b"}), "$-1\r\n");
}

TEST_F(DispatcherTest, ExpireRejectsNonIntegerSeconds) {
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"EXPIRE", "k", "abc"}), "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(run({"EXPIRE", "k", "1.5"}), "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(run({"EXPIRE", "k", ""}), "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");  // unchanged
}

TEST_F(DispatcherTest, ExpireRejectsHugeSecondsInsteadOfOverflowing) {
    run({"SET", "k", "v"});
    const std::string huge = std::to_string(std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(run({"EXPIRE", "k", huge}), "-ERR invalid expire time in 'expire' command\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");  // unchanged
}

TEST_F(DispatcherTest, IncrCreatesAndCounts) {
    EXPECT_EQ(run({"INCR", "n"}), ":1\r\n");
    EXPECT_EQ(run({"INCR", "n"}), ":2\r\n");
    EXPECT_EQ(run({"GET", "n"}), "$1\r\n2\r\n");
}

TEST_F(DispatcherTest, IncrOnNonIntegerIsAnError) {
    run({"SET", "s", "hello"});
    EXPECT_EQ(run({"INCR", "s"}), "-ERR value is not an integer or out of range\r\n");
    EXPECT_EQ(run({"GET", "s"}), "$5\r\nhello\r\n");
}

TEST_F(DispatcherTest, IncrOverflowIsAnError) {
    run({"SET", "n", std::to_string(std::numeric_limits<std::int64_t>::max())});
    EXPECT_EQ(run({"INCR", "n"}), "-ERR increment or decrement would overflow\r\n");
}

TEST_F(DispatcherTest, WrongArgumentCountNamesTheCommand) {
    EXPECT_EQ(run({"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
    EXPECT_EQ(run({"GET", "a", "b"}), "-ERR wrong number of arguments for 'get' command\r\n");
    EXPECT_EQ(run({"SET", "a"}), "-ERR wrong number of arguments for 'set' command\r\n");
    EXPECT_EQ(run({"DEL"}), "-ERR wrong number of arguments for 'del' command\r\n");
    EXPECT_EQ(run({"EXPIRE", "a"}), "-ERR wrong number of arguments for 'expire' command\r\n");
    EXPECT_EQ(run({"PING", "a", "b"}), "-ERR wrong number of arguments for 'ping' command\r\n");
    EXPECT_EQ(run({"INCR"}), "-ERR wrong number of arguments for 'incr' command\r\n");
}

TEST_F(DispatcherTest, UnknownCommandIsAnError) {
    EXPECT_EQ(run({"FLUSHALL"}), "-ERR unknown command 'FLUSHALL'\r\n");
}

TEST_F(DispatcherTest, EchoedCommandNameCannotInjectExtraReplies) {
    // A name containing \r\n must not break RESP framing in the error reply.
    const std::string reply = run({"BAD\r\n+OK"});
    EXPECT_EQ(reply, "-ERR unknown command 'BAD  +OK'\r\n");
    EXPECT_EQ(reply.find("\r\n"), reply.size() - 2);  // exactly one terminator, at the end
}

TEST_F(DispatcherTest, LongUnknownCommandNameIsTruncated) {
    const std::string reply = run({std::string(500, 'x')});
    EXPECT_LT(reply.size(), 120U);
    EXPECT_NE(reply.find("..."), std::string::npos);
}

TEST_F(DispatcherTest, EmptyCommandIsAnError) {
    EXPECT_EQ(run({}), "-ERR empty command\r\n");
}

TEST_F(DispatcherTest, WorksEndToEndFromRawBytes) {
    // Wire bytes in, wire bytes out: parser + dispatcher together.
    const std::string request =
        "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";

    std::string_view remaining = request;
    std::string reply;
    while (!remaining.empty()) {
        const raftkv::resp::ParseResult parsed = raftkv::resp::parseCommand(remaining);
        ASSERT_EQ(parsed.status, raftkv::resp::ParseStatus::kComplete);
        dispatcher.execute(parsed.command, reply);
        remaining.remove_prefix(parsed.consumed);
    }
    EXPECT_EQ(reply, "+OK\r\n$3\r\nbar\r\n");
}
