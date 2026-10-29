#pragma once

// CRC-32 (the IEEE 802.3 variant used by zip, gzip and PNG).
//
// The append-only file stores a checksum with every record, so a half-written or
// corrupted record is detected on recovery instead of being replayed as garbage.

#include <cstddef>
#include <cstdint>

namespace raftkv {

// Checksum of `size` bytes. To checksum data in pieces, pass the previous result as `previous`.
// crc32("123456789") == 0xCBF43926.
std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t previous = 0);

}  // namespace raftkv
