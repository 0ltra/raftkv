#pragma once

// Time source for the key-value store.
//
// The store never calls the system clock directly. It asks a Clock, so tests
// (and later the deterministic Raft simulator) can supply fake time they control.

#include <cstdint>

namespace raftkv {

// Milliseconds since the Unix epoch. Absolute timestamps (not "10 seconds from
// now") are what get stored, because they stay correct after a restart and
// every Raft replica applying the same log entry computes the same result.
using TimestampMs = std::int64_t;

class Clock {
public:
    virtual ~Clock() = default;
    virtual TimestampMs nowMs() const = 0;
};

// Real wall-clock time.
class SystemClock final : public Clock {
public:
    TimestampMs nowMs() const override;
};

// Fake clock that only moves when told to. Used by tests and the simulator.
class ManualClock final : public Clock {
public:
    explicit ManualClock(TimestampMs start = 0) : now_(start) {}

    TimestampMs nowMs() const override { return now_; }
    void setMs(TimestampMs t) { now_ = t; }
    void advanceMs(TimestampMs delta) { now_ += delta; }

private:
    TimestampMs now_;
};

}  // namespace raftkv
