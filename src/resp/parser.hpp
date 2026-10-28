#pragma once

// RESP request parser.
//
// Real clients (redis-cli, redis-benchmark) send every command as an array of
// bulk strings:   *2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
// This parser handles exactly that form. It is stateless: the caller keeps the
// receive buffer and calls parseCommand() on it again whenever more bytes arrive.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace raftkv::resp {

// One parsed command: the command name followed by its arguments.
using Command = std::vector<std::string>;

enum class ParseStatus {
    kComplete,  // one full command was parsed
    kNeedMore,  // the buffer holds only a prefix of a command; wait for more bytes
    kError,     // malformed input; the connection should be closed after replying
};

struct ParseResult {
    ParseStatus status = ParseStatus::kNeedMore;
    std::size_t consumed = 0;  // bytes used by the command (only meaningful for kComplete)
    Command command;           // only meaningful for kComplete
    std::string error;         // only meaningful for kError
};

// Safety limits so a client cannot make the server allocate unbounded memory.
inline constexpr std::size_t kMaxArgs = 1024 * 1024;
inline constexpr std::size_t kMaxBulkLen = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxLineLen = 64;  // header lines ("*3", "$12") are tiny

// Parses at most one command from the start of `buffer`.
// For pipelining, call again on the remainder: buffer.substr(result.consumed).
ParseResult parseCommand(std::string_view buffer);

}  // namespace raftkv::resp
