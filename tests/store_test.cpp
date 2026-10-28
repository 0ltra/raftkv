#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "store/clock.hpp"
#include "store/store.hpp"

using raftkv::IncrStatus;
using raftkv::ManualClock;
using raftkv::Store;

namespace {
// Fixed starting time so tests never depend on the real clock.
constexpr raftkv::TimestampMs kStart = 1'000'000;
}

TEST(Store, GetMissingKeyReturnsNothing) {
    ManualClock clock(kStart);
    Store store(clock);
    EXPECT_FALSE(store.get("nope").has_value());
}

TEST(Store, SetThenGet) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "v");
    EXPECT_EQ(store.get("k"), "v");
}

TEST(Store, SetOverwritesExistingValue) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "one");
    store.set("k", "two");
    EXPECT_EQ(store.get("k"), "two");
    EXPECT_EQ(store.size(), 1U);
}

TEST(Store, EmptyValueIsStoredAndDistinctFromMissing) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "");
    ASSERT_TRUE(store.get("k").has_value());
    EXPECT_EQ(*store.get("k"), "");
}

TEST(Store, DelReportsWhetherKeyExisted) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "v");
    EXPECT_TRUE(store.del("k"));
    EXPECT_FALSE(store.del("k"));
    EXPECT_FALSE(store.get("k").has_value());
}

TEST(StoreExpiry, KeyIsLiveBeforeDeadlineAndGoneAtDeadline) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "v");
    ASSERT_TRUE(store.expireAt("k", kStart + 1000));

    clock.setMs(kStart + 999);
    EXPECT_EQ(store.get("k"), "v");

    clock.setMs(kStart + 1000);  // the deadline itself counts as expired
    EXPECT_FALSE(store.get("k").has_value());
}

TEST(StoreExpiry, ExpireAtOnMissingKeyReturnsFalse) {
    ManualClock clock(kStart);
    Store store(clock);
    EXPECT_FALSE(store.expireAt("nope", kStart + 1000));
}

TEST(StoreExpiry, DeadlineInThePastDeletesTheKey) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "v");
    EXPECT_TRUE(store.expireAt("k", kStart));  // not in the future
    EXPECT_FALSE(store.get("k").has_value());
    EXPECT_EQ(store.size(), 0U);
}

TEST(StoreExpiry, SetRemovesExistingExpiry) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "v");
    store.expireAt("k", kStart + 1000);
    store.set("k", "new");
    clock.advanceMs(5000);
    EXPECT_EQ(store.get("k"), "new");
    EXPECT_EQ(store.ttlMs("k"), -1);
}

TEST(StoreExpiry, TtlReportsMissingNoExpiryAndRemaining) {
    ManualClock clock(kStart);
    Store store(clock);
    EXPECT_EQ(store.ttlMs("nope"), -2);

    store.set("k", "v");
    EXPECT_EQ(store.ttlMs("k"), -1);

    store.expireAt("k", kStart + 5000);
    EXPECT_EQ(store.ttlMs("k"), 5000);
    clock.advanceMs(2000);
    EXPECT_EQ(store.ttlMs("k"), 3000);
    clock.advanceMs(3000);
    EXPECT_EQ(store.ttlMs("k"), -2);
}

TEST(StoreExpiry, DelOnExpiredKeyReturnsFalse) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("k", "v");
    store.expireAt("k", kStart + 10);
    clock.advanceMs(10);
    EXPECT_FALSE(store.del("k"));
}

TEST(StoreExpiry, PurgeRemovesOnlyExpiredKeys) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("a", "1");
    store.set("b", "2");
    store.set("c", "3");
    store.expireAt("a", kStart + 10);
    store.expireAt("b", kStart + 10);
    store.expireAt("c", kStart + 1000);

    clock.advanceMs(10);
    EXPECT_EQ(store.size(), 3U);  // lazy: nothing removed yet
    EXPECT_EQ(store.purgeExpired(), 2U);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.get("c"), "3");
}

TEST(StoreIncr, MissingKeyStartsAtOne) {
    ManualClock clock(kStart);
    Store store(clock);
    const auto r = store.incr("n");
    EXPECT_EQ(r.status, IncrStatus::kOk);
    EXPECT_EQ(r.value, 1);
    EXPECT_EQ(store.get("n"), "1");
}

TEST(StoreIncr, IncrementsExistingInteger) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("n", "41");
    EXPECT_EQ(store.incr("n").value, 42);
    EXPECT_EQ(store.get("n"), "42");
}

TEST(StoreIncr, WorksOnNegativeNumbers) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("n", "-1");
    EXPECT_EQ(store.incr("n").value, 0);
}

TEST(StoreIncr, RejectsNonIntegerValues) {
    ManualClock clock(kStart);
    Store store(clock);
    for (const char* bad : {"abc", "", "1.5", " 1", "1 ", "+1", "12abc"}) {
        store.set("n", bad);
        EXPECT_EQ(store.incr("n").status, IncrStatus::kNotInteger) << "value: '" << bad << "'";
        EXPECT_EQ(store.get("n"), bad);  // a failed INCR must not change the value
    }
}

TEST(StoreIncr, OverflowIsDetectedAndLeavesValueUnchanged) {
    ManualClock clock(kStart);
    Store store(clock);
    const std::string max = std::to_string(std::numeric_limits<std::int64_t>::max());
    store.set("n", max);
    EXPECT_EQ(store.incr("n").status, IncrStatus::kOverflow);
    EXPECT_EQ(store.get("n"), max);
}

TEST(StoreIncr, ValueTooBigForInt64IsNotAnInteger) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("n", "99999999999999999999999");
    EXPECT_EQ(store.incr("n").status, IncrStatus::kNotInteger);
}

TEST(StoreIncr, KeepsExpiry) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("n", "1");
    store.expireAt("n", kStart + 5000);
    store.incr("n");
    EXPECT_EQ(store.ttlMs("n"), 5000);
}

TEST(StoreIncr, ExpiredKeyStartsOverFromZero) {
    ManualClock clock(kStart);
    Store store(clock);
    store.set("n", "100");
    store.expireAt("n", kStart + 10);
    clock.advanceMs(10);
    EXPECT_EQ(store.incr("n").value, 1);
    EXPECT_EQ(store.ttlMs("n"), -1);
}
