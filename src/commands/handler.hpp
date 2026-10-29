#pragma once

// The seam between the network layer and everything behind it.
//
// The server hands every parsed command to a CommandHandler and sends the reply bytes
// it gets back. Different handlers add different behaviour (plain in-memory execution,
// logging to disk, later Raft replication) without the server knowing.

#include <string>

#include "resp/parser.hpp"

namespace raftkv {

class CommandHandler {
public:
    virtual ~CommandHandler() = default;

    // Executes one command and appends exactly one complete RESP reply to `out`.
    virtual void execute(const resp::Command& command, std::string& out) = 0;

    // Called once after a batch of commands has been executed, and BEFORE any of their
    // replies are sent. Returns false if the batch could not be made durable; the server
    // then drops the connection instead of acknowledging work that might be lost.
    virtual bool commit() = 0;
};

}  // namespace raftkv
