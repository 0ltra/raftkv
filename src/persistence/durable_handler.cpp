#include "persistence/durable_handler.hpp"

#include <algorithm>

#include "resp/encoder.hpp"

namespace raftkv {

void DurableHandler::execute(const resp::Command& command, std::string& out) {
    const bool writes = !command.empty() && CommandDispatcher::isWriteCommand(command[0]);

    if (writes && file_.failed()) {
        // The log is broken. Applying more writes would put memory ahead of the disk.
        resp::appendError(out, "MISCONF the append-only file failed, writes are refused");
        return;
    }

    // One instant for the whole command. Never earlier than the previous command, even if the
    // system clock steps backwards, so replay sees timestamps in non-decreasing order.
    const TimestampMs now = std::max(clock_.baseNowMs(), lastTimestamp_);
    lastTimestamp_ = now;

    const std::size_t replyStart = out.size();
    {
        ScopedPin pin(clock_, now);
        dispatcher_.execute(command, out);
    }

    // Error replies never change data (bad arguments, INCR on text, ...), so don't log them.
    const bool isError = out.size() > replyStart && out[replyStart] == '-';
    if (writes && !isError) {
        file_.append(now, command);
    }
}

bool openAndRecover(AppendOnlyFile& file, const std::string& path, CommandDispatcher& dispatcher,
                    PinnableClock& clock, ReplayStats& stats, std::string& error) {
    std::string scratch;
    const auto apply = [&](TimestampMs timestampMs, const resp::Command& command) {
        scratch.clear();
        ScopedPin pin(clock, timestampMs);
        dispatcher.execute(command, scratch);
    };
    return file.open(path, apply, stats, error);
}

}  // namespace raftkv
