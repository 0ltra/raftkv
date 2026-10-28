#pragma once

// In-memory string key-value store with optional per-key expiry.
//
// Single-threaded by design: the event loop (and later the Raft state machine)
// is the only caller. Expiry is lazy: an expired key is removed when it is next
// looked up. purgeExpired() sweeps keys nobody asks for.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "store/clock.hpp"

namespace raftkv {

enum class IncrStatus {
    kOk,
    kNotInteger,  // the stored value is not a base-10 64-bit integer
    kOverflow,    // incrementing would exceed the largest 64-bit integer
};

struct IncrResult {
    IncrStatus status = IncrStatus::kOk;
    std::int64_t value = 0;  // the new value (only meaningful for kOk)
};

class Store {
public:
    // The clock must outlive the store.
    explicit Store(const Clock& clock) : clock_(clock) {}

    std::optional<std::string> get(const std::string& key);

    // Creates or overwrites the key and removes any expiry (same as Redis SET).
    void set(std::string key, std::string value);

    // Returns true if the key existed (and was live) and is now gone.
    bool del(const std::string& key);

    // Sets an absolute expiry time. Returns false if the key does not exist.
    // A deadline that is not in the future deletes the key immediately.
    bool expireAt(const std::string& key, TimestampMs deadlineMs);

    // Time to live in milliseconds: -2 if the key does not exist,
    // -1 if it exists with no expiry, otherwise the remaining time.
    std::int64_t ttlMs(const std::string& key);

    // Treats a missing key as 0. Keeps the key's expiry, like Redis.
    IncrResult incr(const std::string& key);

    // Removes every expired key. Returns how many were removed.
    std::size_t purgeExpired();

    // Number of stored keys, including expired ones not yet purged.
    std::size_t size() const { return map_.size(); }

private:
    struct Entry {
        std::string value;
        std::optional<TimestampMs> expireAtMs;
    };
    using Map = std::unordered_map<std::string, Entry>;

    bool isExpired(const Entry& entry) const;

    // Finds a live key, deleting it first if it has expired. Returns end() if absent.
    Map::iterator findLive(const std::string& key);

    const Clock& clock_;
    Map map_;
};

}  // namespace raftkv
