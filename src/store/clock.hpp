#pragma once

// Time source for the key-value store.
//
// The store never calls the system clock directly. It asks a Clock, so tests
// (and later the deterministic Raft simulator) can supply fake time they control.

#include <cstdint>
#include <optional>

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

// A clock that can be frozen ("pinned") at one instant, otherwise it follows a base clock.
//
// Every command runs while pinned, so the whole command sees a single moment in time.
// That moment is written to the append-only file next to the command. On recovery the
// clock is pinned to the recorded moment again, so replaying a command gives exactly the
// result it gave the first time, no matter how much later the restart happens.
class PinnableClock final : public Clock {
public:
    // The base clock must outlive this one.
    explicit PinnableClock(const Clock& base) : base_(base) {}

    TimestampMs nowMs() const override { return pinned_.has_value() ? *pinned_ : base_.nowMs(); }

    // The base clock's time, ignoring any pin.
    TimestampMs baseNowMs() const { return base_.nowMs(); }

    void pin(TimestampMs t) { pinned_ = t; }
    void unpin() { pinned_.reset(); }

private:
    const Clock& base_;
    std::optional<TimestampMs> pinned_;
};

// Pins a clock for the duration of a scope.
class ScopedPin {
public:
    ScopedPin(PinnableClock& clock, TimestampMs t) : clock_(clock) { clock_.pin(t); }
    ~ScopedPin() { clock_.unpin(); }

    ScopedPin(const ScopedPin&) = delete;
    ScopedPin& operator=(const ScopedPin&) = delete;

private:
    PinnableClock& clock_;
};

}  // namespace raftkv
