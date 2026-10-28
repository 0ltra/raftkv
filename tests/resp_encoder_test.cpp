#include <gtest/gtest.h>

#include <string>

#include "resp/encoder.hpp"

using namespace raftkv::resp;

TEST(RespEncoder, SimpleString) {
    std::string out;
    appendSimpleString(out, "OK");
    EXPECT_EQ(out, "+OK\r\n");
}

TEST(RespEncoder, Error) {
    std::string out;
    appendError(out, "ERR unknown command");
    EXPECT_EQ(out, "-ERR unknown command\r\n");
}

TEST(RespEncoder, Integer) {
    std::string out;
    appendInteger(out, 42);
    appendInteger(out, -1);
    EXPECT_EQ(out, ":42\r\n:-1\r\n");
}

TEST(RespEncoder, BulkString) {
    std::string out;
    appendBulkString(out, "foo");
    EXPECT_EQ(out, "$3\r\nfoo\r\n");
}

TEST(RespEncoder, EmptyBulkStringIsNotNull) {
    std::string out;
    appendBulkString(out, "");
    EXPECT_EQ(out, "$0\r\n\r\n");
}

TEST(RespEncoder, BulkStringIsBinarySafe) {
    std::string out;
    appendBulkString(out, std::string_view("a\r\nb", 4));
    EXPECT_EQ(out, std::string("$4\r\na\r\nb\r\n"));
}

TEST(RespEncoder, NullBulk) {
    std::string out;
    appendNullBulk(out);
    EXPECT_EQ(out, "$-1\r\n");
}

TEST(RespEncoder, RepliesAppendSoPipelinesCanShareOneBuffer) {
    std::string out;
    appendSimpleString(out, "PONG");
    appendInteger(out, 1);
    EXPECT_EQ(out, "+PONG\r\n:1\r\n");
}
