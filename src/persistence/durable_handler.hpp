#pragma once

// DurableHandler: runs commands through the dispatcher and logs the successful writes.
//
//   execute():  run the command with the clock pinned to one instant; if it changed data, queue it
//   commit():   write + fsync the queued records (the server calls this before sending replies)
//
// Because the replies leave only after commit(), "OK" means the write is as safe as the
// fsync policy promises.

#include <limits>
#include <string>

#include "commands/dispatcher.hpp"
#include "commands/handler.hpp"
#include "persistence/aof.hpp"
#include "store/clock.hpp"

namespace raftkv {

class DurableHandler final : public CommandHandler {
public:
    // Everything passed in must outlive the handler. `clock` must be the clock the store and
    // dispatcher use. Timestamps never go below `timestampFloor` (pass the last replayed one).
    DurableHandler(CommandDispatcher& dispatcher, PinnableClock& clock, AppendOnlyFile& file,
                   TimestampMs timestampFloor = std::numeric_limits<TimestampMs>::min())
        : dispatcher_(dispatcher), clock_(clock), file_(file), lastTimestamp_(timestampFloor) {}

    void execute(const resp::Command& command, std::string& out) override;
    bool commit() override { return file_.commit(); }

    // Call periodically (the server's tick).
    void tick() { file_.tick(); }

private:
    CommandDispatcher& dispatcher_;
    PinnableClock& clock_;
    AppendOnlyFile& file_;
    TimestampMs lastTimestamp_;
};

// Opens `path`, replays it into the dispatcher's store, and prepares it for appending.
// Replayed commands run with the clock pinned to their recorded timestamps.
bool openAndRecover(AppendOnlyFile& file, const std::string& path, CommandDispatcher& dispatcher,
                    PinnableClock& clock, ReplayStats& stats, std::string& error);

}  // namespace raftkv
