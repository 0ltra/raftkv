#include "resp/encoder.hpp"

namespace raftkv::resp {

void appendSimpleString(std::string& out, std::string_view text) {
    out.push_back('+');
    out.append(text);
    out.append("\r\n");
}

void appendError(std::string& out, std::string_view message) {
    out.push_back('-');
    out.append(message);
    out.append("\r\n");
}

void appendInteger(std::string& out, long long value) {
    out.push_back(':');
    out.append(std::to_string(value));
    out.append("\r\n");
}

void appendBulkString(std::string& out, std::string_view text) {
    out.push_back('$');
    out.append(std::to_string(text.size()));
    out.append("\r\n");
    out.append(text);
    out.append("\r\n");
}

void appendNullBulk(std::string& out) {
    out.append("$-1\r\n");
}

}  // namespace raftkv::resp
