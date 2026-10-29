#include <gtest/gtest.h>

#include <string>

#include "persistence/crc32.hpp"

using raftkv::crc32;

TEST(Crc32, MatchesTheStandardCheckValue) {
    const std::string text = "123456789";
    EXPECT_EQ(crc32(text.data(), text.size()), 0xCBF43926U);
}

TEST(Crc32, EmptyInputIsZero) {
    EXPECT_EQ(crc32("", 0), 0U);
}

TEST(Crc32, CheckingInPiecesGivesTheSameResultAsAllAtOnce) {
    const std::string text = "The quick brown fox jumps over the lazy dog";
    const std::uint32_t whole = crc32(text.data(), text.size());
    for (std::size_t split = 0; split <= text.size(); ++split) {
        const std::uint32_t first = crc32(text.data(), split);
        EXPECT_EQ(crc32(text.data() + split, text.size() - split, first), whole) << "split=" << split;
    }
}

TEST(Crc32, AnySingleBitFlipChangesTheChecksum) {
    std::string text = "append-only file record";
    const std::uint32_t original = crc32(text.data(), text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            text[i] = static_cast<char>(text[i] ^ (1 << bit));
            EXPECT_NE(crc32(text.data(), text.size()), original) << "byte " << i << " bit " << bit;
            text[i] = static_cast<char>(text[i] ^ (1 << bit));
        }
    }
}
