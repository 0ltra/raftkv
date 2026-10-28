#include "store/clock.hpp"

#include <chrono>

namespace raftkv {

TimestampMs SystemClock::nowMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace raftkv
