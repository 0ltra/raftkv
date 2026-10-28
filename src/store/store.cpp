#include "store/store.hpp"

#include <charconv>
#include <limits>
#include <utility>

namespace raftkv {

bool Store::isExpired(const Entry& entry) const {
    return entry.expireAtMs.has_value() && *entry.expireAtMs <= clock_.nowMs();
}

Store::Map::iterator Store::findLive(const std::string& key) {
    const auto it = map_.find(key);
    if (it != map_.end() && isExpired(it->second)) {
        map_.erase(it);
        return map_.end();
    }
    return it;
}

std::optional<std::string> Store::get(const std::string& key) {
    const auto it = findLive(key);
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second.value;
}

void Store::set(std::string key, std::string value) {
    map_.insert_or_assign(std::move(key), Entry{std::move(value), std::nullopt});
}

bool Store::del(const std::string& key) {
    const auto it = findLive(key);
    if (it == map_.end()) {
        return false;
    }
    map_.erase(it);
    return true;
}

bool Store::expireAt(const std::string& key, TimestampMs deadlineMs) {
    const auto it = findLive(key);
    if (it == map_.end()) {
        return false;
    }
    if (deadlineMs <= clock_.nowMs()) {
        map_.erase(it);
        return true;
    }
    it->second.expireAtMs = deadlineMs;
    return true;
}

std::int64_t Store::ttlMs(const std::string& key) {
    const auto it = findLive(key);
    if (it == map_.end()) {
        return -2;
    }
    if (!it->second.expireAtMs.has_value()) {
        return -1;
    }
    return *it->second.expireAtMs - clock_.nowMs();
}

IncrResult Store::incr(const std::string& key) {
    const auto it = findLive(key);
    if (it == map_.end()) {
        map_.insert_or_assign(key, Entry{"1", std::nullopt});
        return IncrResult{IncrStatus::kOk, 1};
    }

    const std::string& text = it->second.value;
    std::int64_t current = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), current);
    if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size()) {
        return IncrResult{IncrStatus::kNotInteger, 0};
    }
    if (current == std::numeric_limits<std::int64_t>::max()) {
        return IncrResult{IncrStatus::kOverflow, 0};
    }

    const std::int64_t next = current + 1;
    it->second.value = std::to_string(next);
    return IncrResult{IncrStatus::kOk, next};
}

std::size_t Store::purgeExpired() {
    return std::erase_if(map_, [this](const auto& item) { return isExpired(item.second); });
}

}  // namespace raftkv
