#include "resp/parser.hpp"

#include <charconv>
#include <optional>
#include <utility>

namespace raftkv::resp {
namespace {

enum class LineStatus { kOk, kNeedMore, kError };

struct Line {
    LineStatus status = LineStatus::kNeedMore;
    std::string_view text;   // the line without its trailing \r\n
    std::size_t next = 0;    // index of the first byte after the \r\n
};

ParseResult makeNeedMore() {
    return ParseResult{ParseStatus::kNeedMore, 0, {}, {}};
}

ParseResult makeError(std::string message) {
    return ParseResult{ParseStatus::kError, 0, {}, std::move(message)};
}

// Reads one \r\n-terminated line starting at `pos`.
Line readLine(std::string_view buffer, std::size_t pos) {
    const std::size_t end = buffer.find("\r\n", pos);
    if (end == std::string_view::npos) {
        // No terminator yet. A legitimate header line is short, so a long
        // unterminated run of bytes is garbage, not a partial read.
        if (buffer.size() - pos > kMaxLineLen) {
            return Line{LineStatus::kError, {}, 0};
        }
        return Line{LineStatus::kNeedMore, {}, 0};
    }
    if (end - pos > kMaxLineLen) {
        return Line{LineStatus::kError, {}, 0};
    }
    return Line{LineStatus::kOk, buffer.substr(pos, end - pos), end + 2};
}

// Parses a non-negative decimal integer. Rejects empty text, signs, and spaces.
std::optional<std::size_t> parseLength(std::string_view text) {
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    unsigned long long value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

}  // namespace

ParseResult parseCommand(std::string_view buffer) {
    if (buffer.empty()) {
        return makeNeedMore();
    }
    if (buffer[0] != '*') {
        return makeError("Protocol error: expected '*'");
    }

    // --- Array header: *<count>\r\n ---
    const Line header = readLine(buffer, 1);
    if (header.status == LineStatus::kNeedMore) {
        return makeNeedMore();
    }
    if (header.status == LineStatus::kError) {
        return makeError("Protocol error: invalid multibulk length");
    }
    const std::optional<std::size_t> count = parseLength(header.text);
    if (!count || *count == 0 || *count > kMaxArgs) {
        return makeError("Protocol error: invalid multibulk length");
    }

    // --- Each element: $<len>\r\n<len bytes>\r\n ---
    Command command;
    command.reserve(*count < 16 ? *count : 16);
    std::size_t pos = header.next;

    for (std::size_t i = 0; i < *count; ++i) {
        if (pos >= buffer.size()) {
            return makeNeedMore();
        }
        if (buffer[pos] != '$') {
            return makeError("Protocol error: expected '$'");
        }

        const Line lenLine = readLine(buffer, pos + 1);
        if (lenLine.status == LineStatus::kNeedMore) {
            return makeNeedMore();
        }
        if (lenLine.status == LineStatus::kError) {
            return makeError("Protocol error: invalid bulk length");
        }
        const std::optional<std::size_t> len = parseLength(lenLine.text);
        if (!len || *len > kMaxBulkLen) {
            return makeError("Protocol error: invalid bulk length");
        }

        const std::size_t bodyStart = lenLine.next;
        if (buffer.size() < bodyStart + *len + 2) {
            return makeNeedMore();
        }
        if (buffer.substr(bodyStart + *len, 2) != "\r\n") {
            return makeError("Protocol error: bulk string not terminated by CRLF");
        }

        command.emplace_back(buffer.substr(bodyStart, *len));
        pos = bodyStart + *len + 2;
    }

    return ParseResult{ParseStatus::kComplete, pos, std::move(command), {}};
}

}  // namespace raftkv::resp
