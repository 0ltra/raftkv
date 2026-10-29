#include "persistence/crc32.hpp"

#include <array>

namespace raftkv {
namespace {

// One table entry per byte value, built at compile time (reflected polynomial 0xEDB88320).
constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = makeTable();

}  // namespace

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t previous) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = ~previous;
    for (std::size_t i = 0; i < size; ++i) {
        crc = kTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

}  // namespace raftkv
