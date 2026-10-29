#pragma once

// Command dispatcher: turns a parsed command into a RESP reply.
//
//   parseCommand()  ->  CommandDispatcher::execute()  ->  reply bytes
//
// Supported commands (strings only): PING, GET, SET, DEL, EXPIRE, TTL, INCR.
// There are no sockets or threads here, so it is fully testable on its own.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "commands/handler.hpp"
#include "resp/parser.hpp"
#include "store/clock.hpp"
#include "store/store.hpp"

namespace raftkv {

class CommandDispatcher final : public CommandHandler {
public:
    // The store and clock must outlive the dispatcher.
    CommandDispatcher(Store& store, const Clock& clock) : store_(store), clock_(clock) {}

    // Executes one command and appends exactly one complete RESP reply to `out`.
    // Unknown commands and bad arguments produce an error reply, never a crash.
    void execute(const resp::Command& command, std::string& out) override;

    // Nothing to make durable here: memory only.
    bool commit() override { return true; }

    // True for commands that change data, which therefore must be logged for recovery.
    // The name is matched case-insensitively. Unknown names are not write commands.
    static bool isWriteCommand(std::string_view name);

private:
    using Handler = void (CommandDispatcher::*)(const resp::Command&, std::string&);

    struct Spec {
        std::string_view name;  // lowercase
        std::size_t minArgc;    // counts the command name itself
        std::size_t maxArgc;
        bool writes;            // changes stored data
        Handler handler;
    };

    static const std::array<Spec, 7>& commandTable();

    void handlePing(const resp::Command& command, std::string& out);
    void handleGet(const resp::Command& command, std::string& out);
    void handleSet(const resp::Command& command, std::string& out);
    void handleDel(const resp::Command& command, std::string& out);
    void handleExpire(const resp::Command& command, std::string& out);
    void handleTtl(const resp::Command& command, std::string& out);
    void handleIncr(const resp::Command& command, std::string& out);

    Store& store_;
    const Clock& clock_;
};

}  // namespace raftkv
