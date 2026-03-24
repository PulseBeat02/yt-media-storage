// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) 2026 Brandon Li <https://brandonli.me/>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <gtest/gtest.h>

#include "integrity.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> bytes_from_string(const std::string &text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const unsigned char ch : text) {
        result.push_back(std::byte{ch});
    }
    return result;
}

void write_u32_le(std::vector<std::byte> &buffer, const std::size_t offset, const uint32_t value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

bool is_hex_char(const char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
}

} // namespace

TEST(Integrity, Sha256_EmptyInput) {
    const std::vector<std::byte> empty_input;
    const Sha256Digest digest = sha256(empty_input);
    const std::string hex_output = digest.hexValue();
    EXPECT_EQ(hex_output, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
}

TEST(Integrity, Sha256_KnownInput) {
    const std::vector<std::byte> hello_bytes = bytes_from_string("hello");
    const Sha256Digest digest = sha256(hello_bytes);
    const std::string hex_output = digest.hexValue();
    EXPECT_EQ(hex_output, "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824");
}

TEST(Integrity, Sha256_HexOutputContainsOnlyHexChars) {
    const std::vector<std::byte> abc_bytes = bytes_from_string("abc");
    const Sha256Digest digest = sha256(abc_bytes);
    for (const std::string hex_output = digest.hexValue(); const char ch : hex_output) {
        EXPECT_TRUE(is_hex_char(ch));
    }
}

TEST(Integrity, Sha256_EqualInputsProduceEqualDigests) {
    const std::vector<std::byte> same_data = bytes_from_string("same");
    const std::vector<std::byte> different_data = bytes_from_string("diff");
    const Sha256Digest digest_a = sha256(same_data);
    const Sha256Digest digest_b = sha256(same_data);
    const Sha256Digest digest_c = sha256(different_data);
    EXPECT_EQ(digest_a, digest_b);
    EXPECT_NE(digest_a, digest_c);
}

TEST(Integrity, Crc32_SameInputIsDeterministic) {
    const std::vector<std::byte> input = bytes_from_string("deterministic");
    const uint32_t first_crc = crc32c(input);
    const uint32_t second_crc = crc32c(input);
    EXPECT_EQ(first_crc, second_crc);
}

TEST(Integrity, Crc32_DifferentInputsProduceDifferentValues) {
    const std::vector<std::byte> alpha = bytes_from_string("alpha");
    const std::vector<std::byte> beta = bytes_from_string("beta");
    const uint32_t alpha_crc = crc32c(alpha);
    const uint32_t beta_crc = crc32c(beta);
    EXPECT_NE(alpha_crc, beta_crc);
}

TEST(Integrity, Crc32Concat_MatchesSingleCallOnCombinedData) {
    const std::vector<std::byte> first_part = bytes_from_string("first");
    const std::vector<std::byte> second_part = bytes_from_string("second");

    std::vector<std::byte> combined = first_part;
    combined.insert(combined.end(), second_part.begin(), second_part.end());

    const uint32_t concat_crc = crc32c_concat(first_part, second_part);
    const uint32_t combined_crc = crc32c(combined);
    EXPECT_EQ(concat_crc, combined_crc);
}

TEST(Integrity, PacketChecksum_CRC32DispatchMatchesDirect) {
    const std::vector<std::byte> header = bytes_from_string("hdr");
    const std::vector<std::byte> payload = bytes_from_string("pld");
    constexpr std::size_t crc_offset = 0;

    const uint32_t dispatched = packet_checksum(header, payload, crc_offset, HashAlgorithm::CRC32);
    const uint32_t direct = packet_crc32c(header, payload, crc_offset);
    EXPECT_EQ(dispatched, direct);
}

TEST(Integrity, PacketChecksum_XXHashDispatchMatchesDirect) {
    const std::vector<std::byte> header = bytes_from_string("hdr");
    const std::vector<std::byte> payload = bytes_from_string("pld");
    constexpr std::size_t crc_offset = 0;

    const uint32_t dispatched = packet_checksum(header, payload, crc_offset, HashAlgorithm::XXHash32);
    const uint32_t direct = xxhash32_packet(header, payload, crc_offset);
    EXPECT_EQ(dispatched, direct);
}

TEST(Integrity, VerifyPacketCrc_ValidPacketPasses) {
    std::vector<std::byte> header = bytes_from_string("\x01\x02\x03\x04\x05\x06");
    const std::vector<std::byte> payload = bytes_from_string("payload-bytes");
    constexpr std::size_t crc_offset = 2;

    const uint32_t computed_crc = packet_crc32c(header, payload, crc_offset);
    write_u32_le(header, crc_offset, computed_crc);

    EXPECT_TRUE(verify_packet_crc32c(header, payload, crc_offset));
}

TEST(Integrity, VerifyPacketCrc_CorruptedPayloadFails) {
    std::vector<std::byte> header = bytes_from_string("\x01\x02\x03\x04\x05\x06");
    std::vector<std::byte> payload = bytes_from_string("payload-bytes");
    constexpr std::size_t crc_offset = 2;

    const uint32_t computed_crc = packet_crc32c(header, payload, crc_offset);
    write_u32_le(header, crc_offset, computed_crc);

    const auto original_byte = static_cast<unsigned char>(payload[0]);
    payload[0] = static_cast<std::byte>(original_byte ^ 0xFFu);

    EXPECT_FALSE(verify_packet_crc32c(header, payload, crc_offset));
}

TEST(Integrity, Crc32_EmptyInput) {
    const std::vector<std::byte> empty;
    const uint32_t crc = crc32c(empty);
    const uint32_t crc_again = crc32c(empty);
    EXPECT_EQ(crc, crc_again);
}

TEST(Integrity, Crc32_WithCustomSeed) {
    const std::vector<std::byte> data = bytes_from_string("seed-test");
    const uint32_t crc_default = crc32c(data, 0);
    const uint32_t crc_seeded = crc32c(data, 12345);
    EXPECT_NE(crc_default, crc_seeded);
}

TEST(Integrity, Sha256_LargeInput) {
    std::vector<std::byte> large_data(1024 * 1024);
    for (std::size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<std::byte>(i & 0xFF);
    }
    const Sha256Digest digest = sha256(large_data);
    const Sha256Digest digest_again = sha256(large_data);
    EXPECT_EQ(digest, digest_again);
    EXPECT_EQ(digest.hexValue().size(), 64u);
}

TEST(Integrity, XXHash_EmptyPayload) {
    const std::vector<std::byte> header = bytes_from_string("hdr");
    const std::vector<std::byte> empty_payload;
    const uint32_t hash = xxhash32_packet(header, empty_payload, 0);
    const uint32_t hash_again = xxhash32_packet(header, empty_payload, 0);
    EXPECT_EQ(hash, hash_again);
}
