#pragma once

// Command dispatcher: turns a parsed command into a RESP reply.
//
//   parseCommand()  ->  CommandDispatcher::execute()  ->  reply bytes
//
// Supported commands (strings only): PING, GET, SET, DEL, EXPIRE, TTL, INCR.
// There are no sockets or threads here, so it is fully testable on its own.

#include <string>

#include "resp/parser.hpp"
#include "store/clock.hpp"
#include "store/store.hpp"

namespace raftkv {

class CommandDispatcher {
public:
    // The store and clock must outlive the dispatcher.
    CommandDispatcher(Store& store, const Clock& clock) : store_(store), clock_(clock) {}

    // Executes one command and appends exactly one complete RESP reply to `out`.
    // Unknown commands and bad arguments produce an error reply, never a crash.
    void execute(const resp::Command& command, std::string& out);

private:
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
