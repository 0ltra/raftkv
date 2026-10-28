#pragma once

// RESP reply encoders. Each function appends one complete reply to `out`,
// so a connection can build a whole pipelined response in one string.

#include <string>
#include <string_view>

namespace raftkv::resp {

// +OK\r\n
void appendSimpleString(std::string& out, std::string_view text);

// -ERR message\r\n   (pass the full text, e.g. "ERR unknown command")
void appendError(std::string& out, std::string_view message);

// :42\r\n
void appendInteger(std::string& out, long long value);

// $3\r\nfoo\r\n   (binary safe; the body may contain \r\n)
void appendBulkString(std::string& out, std::string_view text);

// $-1\r\n   (the "key does not exist" reply)
void appendNullBulk(std::string& out);

}  // namespace raftkv::resp
