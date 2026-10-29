#include <gtest/gtest.h>

#include "store/clock.hpp"

using raftkv::ManualClock;
using raftkv::PinnableClock;
using raftkv::ScopedPin;

TEST(PinnableClock, FollowsTheBaseClockWhenNotPinned) {
    ManualClock base(1000);
    PinnableClock clock(base);
    EXPECT_EQ(clock.nowMs(), 1000);
    base.advanceMs(250);
    EXPECT_EQ(clock.nowMs(), 1250);
}

TEST(PinnableClock, StaysFrozenWhilePinnedEvenAsTheBaseMoves) {
    ManualClock base(1000);
    PinnableClock clock(base);
    clock.pin(500);
    base.advanceMs(10'000);
    EXPECT_EQ(clock.nowMs(), 500);
    EXPECT_EQ(clock.baseNowMs(), 11'000);
}

TEST(PinnableClock, UnpinResumesFollowingTheBase) {
    ManualClock base(1000);
    PinnableClock clock(base);
    clock.pin(5);
    clock.unpin();
    EXPECT_EQ(clock.nowMs(), 1000);
}

TEST(ScopedPin, PinsForTheScopeAndReleasesAfterwards) {
    ManualClock base(1000);
    PinnableClock clock(base);
    {
        ScopedPin pin(clock, 42);
        base.advanceMs(1);
        EXPECT_EQ(clock.nowMs(), 42);
    }
    EXPECT_EQ(clock.nowMs(), 1001);
}

TEST(ScopedPin, ReleasesWhenAnExceptionLeavesTheScope) {
    ManualClock base(1000);
    PinnableClock clock(base);
    try {
        ScopedPin pin(clock, 42);
        throw 1;
    } catch (int) {
    }
    EXPECT_EQ(clock.nowMs(), 1000);
}
