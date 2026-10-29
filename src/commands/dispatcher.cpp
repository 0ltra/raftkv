#include "commands/dispatcher.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "resp/encoder.hpp"

namespace raftkv {
namespace {

constexpr std::size_t kUnlimited = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaxEchoLen = 64;
constexpr std::string_view kNotIntegerError = "ERR value is not an integer or out of range";

std::string toLower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

// Client-supplied text is echoed in some error replies. A "\r\n" inside it would
// break RESP framing (the client would see an extra reply), so control characters
// are replaced and the length is capped.
std::string sanitizeForError(std::string_view text) {
    std::string result;
    const std::size_t n = text.size() < kMaxEchoLen ? text.size() : kMaxEchoLen;
    result.reserve(n + 3);
    for (std::size_t i = 0; i < n; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        result.push_back((byte < 0x20 || byte == 0x7f) ? ' ' : text[i]);
    }
    if (text.size() > kMaxEchoLen) {
        result.append("...");
    }
    return result;
}

// Strict base-10 signed 64-bit parse: no spaces, no '+', no trailing junk.
std::optional<std::int64_t> parseInt64(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

const std::array<CommandDispatcher::Spec, 7>& CommandDispatcher::commandTable() {
    static const std::array<Spec, 7> kCommands{{
        {"ping", 1, 2, false, &CommandDispatcher::handlePing},
        {"get", 2, 2, false, &CommandDispatcher::handleGet},
        {"set", 3, 3, true, &CommandDispatcher::handleSet},
        {"del", 2, kUnlimited, true, &CommandDispatcher::handleDel},
        {"expire", 3, 3, true, &CommandDispatcher::handleExpire},
        {"ttl", 2, 2, false, &CommandDispatcher::handleTtl},
        {"incr", 2, 2, true, &CommandDispatcher::handleIncr},
    }};
    return kCommands;
}

bool CommandDispatcher::isWriteCommand(std::string_view name) {
    const std::string lowered = toLower(name);
    for (const Spec& spec : commandTable()) {
        if (spec.name == lowered) {
            return spec.writes;
        }
    }
    return false;
}

void CommandDispatcher::execute(const resp::Command& command, std::string& out) {
    if (command.empty()) {
        resp::appendError(out, "ERR empty command");
        return;
    }

    const std::string name = toLower(command[0]);
    for (const Spec& spec : commandTable()) {
        if (spec.name != name) {
            continue;
        }
        if (command.size() < spec.minArgc || command.size() > spec.maxArgc) {
            std::string message = "ERR wrong number of arguments for '";
            message.append(spec.name);
            message.append("' command");
            resp::appendError(out, message);
            return;
        }
        (this->*spec.handler)(command, out);
        return;
    }

    resp::appendError(out, "ERR unknown command '" + sanitizeForError(command[0]) + "'");
}

void CommandDispatcher::handlePing(const resp::Command& command, std::string& out) {
    if (command.size() == 1) {
        resp::appendSimpleString(out, "PONG");
    } else {
        resp::appendBulkString(out, command[1]);
    }
}

void CommandDispatcher::handleGet(const resp::Command& command, std::string& out) {
    const std::optional<std::string> value = store_.get(command[1]);
    if (value.has_value()) {
        resp::appendBulkString(out, *value);
    } else {
        resp::appendNullBulk(out);
    }
}

void CommandDispatcher::handleSet(const resp::Command& command, std::string& out) {
    store_.set(command[1], command[2]);
    resp::appendSimpleString(out, "OK");
}

void CommandDispatcher::handleDel(const resp::Command& command, std::string& out) {
    long long removed = 0;
    for (std::size_t i = 1; i < command.size(); ++i) {
        if (store_.del(command[i])) {
            ++removed;
        }
    }
    resp::appendInteger(out, removed);
}

void CommandDispatcher::handleExpire(const resp::Command& command, std::string& out) {
    const std::optional<std::int64_t> seconds = parseInt64(command[2]);
    if (!seconds.has_value()) {
        resp::appendError(out, kNotIntegerError);
        return;
    }

    // Convert "N seconds from now" into an absolute deadline once, here. The store
    // only ever sees absolute times (this is what keeps replicas in agreement later).
    const TimestampMs now = clock_.nowMs();
    if (*seconds > (std::numeric_limits<TimestampMs>::max() - now) / 1000) {
        resp::appendError(out, "ERR invalid expire time in 'expire' command");
        return;
    }
    const TimestampMs deadline = (*seconds <= 0) ? now : now + *seconds * 1000;
    resp::appendInteger(out, store_.expireAt(command[1], deadline) ? 1 : 0);
}

void CommandDispatcher::handleTtl(const resp::Command& command, std::string& out) {
    const std::int64_t ms = store_.ttlMs(command[1]);
    if (ms < 0) {
        resp::appendInteger(out, ms);  // -2 missing, -1 no expiry
    } else {
        resp::appendInteger(out, (ms + 500) / 1000);  // seconds, rounded like Redis
    }
}

void CommandDispatcher::handleIncr(const resp::Command& command, std::string& out) {
    const IncrResult result = store_.incr(command[1]);
    switch (result.status) {
        case IncrStatus::kOk:
            resp::appendInteger(out, result.value);
            return;
        case IncrStatus::kNotInteger:
            resp::appendError(out, kNotIntegerError);
            return;
        case IncrStatus::kOverflow:
            resp::appendError(out, "ERR increment or decrement would overflow");
            return;
    }
}

}  // namespace raftkv
