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
#define XXH_INLINE_ALL
#include "libs/xxhash.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> bytes_from_string(const std::string& text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const unsigned char ch : text) {
    result.push_back(std::byte{ch});
  }
  return result;
}

void write_u32_le(std::vector<std::byte>& buffer, const std::size_t offset, const uint32_t value) {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

bool is_hex_char(const char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
}

} // namespace

TEST(integrity_test, should_hash_empty_input_to_the_known_sha256_digest) {
  const std::vector<std::byte> empty_input;
  const Sha256Digest digest = sha256(empty_input);
  const std::string hex_output = digest.hex_value();
  EXPECT_EQ(hex_output, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
}

TEST(integrity_test, should_hash_known_input_to_its_sha256_digest) {
  const std::vector<std::byte> hello_bytes = bytes_from_string("hello");
  const Sha256Digest digest = sha256(hello_bytes);
  const std::string hex_output = digest.hex_value();
  EXPECT_EQ(hex_output, "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824");
}

TEST(integrity_test, should_write_the_sha256_digest_in_hex) {
  const std::vector<std::byte> abc_bytes = bytes_from_string("abc");
  const Sha256Digest digest = sha256(abc_bytes);
  for (const std::string hex_output = digest.hex_value(); const char ch : hex_output) {
    const bool hex_digit = is_hex_char(ch);
    EXPECT_TRUE(hex_digit);
  }
}

TEST(integrity_test, should_hash_equal_inputs_to_equal_digests) {
  const std::vector<std::byte> same_data = bytes_from_string("same");
  const std::vector<std::byte> different_data = bytes_from_string("diff");
  const Sha256Digest digest_a = sha256(same_data);
  const Sha256Digest digest_b = sha256(same_data);
  const Sha256Digest digest_c = sha256(different_data);
  EXPECT_EQ(digest_a, digest_b);
  EXPECT_NE(digest_a, digest_c);
}

TEST(integrity_test, should_compute_the_same_crc32_for_the_same_input) {
  const std::vector<std::byte> input = bytes_from_string("deterministic");
  const uint32_t first_crc = crc32_mpeg2(input);
  const uint32_t second_crc = crc32_mpeg2(input);
  EXPECT_EQ(first_crc, second_crc);
}

TEST(integrity_test, should_compute_different_crc32_values_for_different_inputs) {
  const std::vector<std::byte> alpha = bytes_from_string("alpha");
  const std::vector<std::byte> beta = bytes_from_string("beta");
  const uint32_t alpha_crc = crc32_mpeg2(alpha);
  const uint32_t beta_crc = crc32_mpeg2(beta);
  EXPECT_NE(alpha_crc, beta_crc);
}

TEST(integrity_test, should_continue_a_crc32_across_concatenated_data) {
  const std::vector<std::byte> first_part = bytes_from_string("first");
  const std::vector<std::byte> second_part = bytes_from_string("second");

  std::vector<std::byte> combined = first_part;
  combined.insert(combined.end(), second_part.begin(), second_part.end());

  const uint32_t concat_crc = crc32_mpeg2_concat(first_part, second_part);
  const uint32_t combined_crc = crc32_mpeg2(combined);
  EXPECT_EQ(concat_crc, combined_crc);
}

TEST(integrity_test, should_compute_crc32_packet_checksums_with_crc32) {
  const std::vector<std::byte> header = bytes_from_string("hdr");
  const std::vector<std::byte> payload = bytes_from_string("pld");
  constexpr std::size_t crc_offset = 0;

  const uint32_t dispatched = packet_checksum(header, payload, crc_offset, HashAlgorithm::CRC32);
  const uint32_t direct = packet_crc32(header, payload, crc_offset);
  EXPECT_EQ(dispatched, direct);
}

TEST(integrity_test, should_compute_xxhash_packet_checksums_with_xxhash) {
  const std::vector<std::byte> header = bytes_from_string("hdr");
  const std::vector<std::byte> payload = bytes_from_string("pld");
  constexpr std::size_t crc_offset = 0;

  const uint32_t dispatched = packet_checksum(header, payload, crc_offset, HashAlgorithm::XXHash32);
  const uint32_t direct = packet_xxhash32(header, payload, crc_offset);
  EXPECT_EQ(dispatched, direct);
}

TEST(integrity_test, should_accept_a_packet_with_a_valid_checksum) {
  std::vector<std::byte> header = bytes_from_string("\x01\x02\x03\x04\x05\x06");
  const std::vector<std::byte> payload = bytes_from_string("payload-bytes");
  constexpr std::size_t crc_offset = 2;

  const uint32_t computed_crc = packet_crc32(header, payload, crc_offset);
  write_u32_le(header, crc_offset, computed_crc);

  const bool checksum_valid = verify_packet_crc32(header, payload, crc_offset);
  EXPECT_TRUE(checksum_valid);
}

TEST(integrity_test, should_reject_a_packet_with_a_corrupted_payload) {
  std::vector<std::byte> header = bytes_from_string("\x01\x02\x03\x04\x05\x06");
  std::vector<std::byte> payload = bytes_from_string("payload-bytes");
  constexpr std::size_t crc_offset = 2;

  const uint32_t computed_crc = packet_crc32(header, payload, crc_offset);
  write_u32_le(header, crc_offset, computed_crc);

  const auto original_byte = static_cast<unsigned char>(payload[0]);
  payload[0] = static_cast<std::byte>(original_byte ^ 0xFFu);

  const bool checksum_valid = verify_packet_crc32(header, payload, crc_offset);
  EXPECT_FALSE(checksum_valid);
}

TEST(integrity_test, should_compute_the_crc32_of_empty_input) {
  const std::vector<std::byte> empty;
  const uint32_t crc = crc32_mpeg2(empty);
  const uint32_t crc_again = crc32_mpeg2(empty);
  EXPECT_EQ(crc, crc_again);
}

TEST(integrity_test, should_start_a_crc32_from_a_custom_seed) {
  const std::vector<std::byte> data = bytes_from_string("seed-test");
  const uint32_t crc_default = crc32_mpeg2(data, 0);
  const uint32_t crc_seeded = crc32_mpeg2(data, 12345);
  EXPECT_NE(crc_default, crc_seeded);
}

TEST(integrity_test, should_hash_large_input) {
  std::vector<std::byte> large_data(1024 * 1024);
  for (std::size_t i = 0; i < large_data.size(); ++i) {
    large_data[i] = static_cast<std::byte>(i & 0xFF);
  }
  const Sha256Digest digest = sha256(large_data);
  const Sha256Digest digest_again = sha256(large_data);
  const std::size_t hex_length = digest.hex_value().size();
  EXPECT_EQ(digest, digest_again);
  EXPECT_EQ(hex_length, 64u);
}

TEST(integrity_test, should_hash_an_empty_payload_with_xxhash) {
  const std::vector<std::byte> header = bytes_from_string("hdr");
  const std::vector<std::byte> empty_payload;
  const uint32_t hash = packet_xxhash32(header, empty_payload, 0);
  const uint32_t hash_again = packet_xxhash32(header, empty_payload, 0);
  EXPECT_EQ(hash, hash_again);
}

TEST(integrity_test, should_checksum_the_header_bytes_after_the_crc_field) {
  const std::vector<std::byte> header = bytes_from_string("ab\x11\x22\x33\x44trail");
  const std::vector<std::byte> payload = bytes_from_string("payload-bytes");
  constexpr std::size_t crc_offset = 2;
  constexpr std::size_t crc_size = 4;
  std::vector<std::byte> zeroed_header = header;
  std::memset(zeroed_header.data() + crc_offset, 0, crc_size);

  const uint32_t crc = packet_crc32(header, payload, crc_offset);

  const uint32_t expected_crc = crc32_mpeg2_concat(zeroed_header, payload);
  EXPECT_EQ(crc, expected_crc);
}

TEST(integrity_test, should_hash_the_header_bytes_after_the_hash_field) {
  const std::vector<std::byte> header = bytes_from_string("ab\x11\x22\x33\x44trail");
  const std::vector<std::byte> payload = bytes_from_string("payload-bytes");
  constexpr std::size_t hash_offset = 2;
  constexpr std::size_t hash_size = 4;
  constexpr XXH32_hash_t seed = 0;
  std::vector<std::byte> zeroed_packet = header;
  std::memset(zeroed_packet.data() + hash_offset, 0, hash_size);
  zeroed_packet.insert(zeroed_packet.end(), payload.begin(), payload.end());

  const uint32_t hash = packet_xxhash32(header, payload, hash_offset);

  const XXH32_hash_t expected_hash = XXH32(zeroed_packet.data(), zeroed_packet.size(), seed);
  EXPECT_EQ(hash, expected_hash);
}

TEST(integrity_test, should_reject_a_crc_field_that_is_not_four_bytes) {
  std::vector<std::byte> header = bytes_from_string("\x01\x02\x03\x04\x05\x06");
  const std::vector<std::byte> payload = bytes_from_string("payload-bytes");
  constexpr std::size_t crc_offset = 2;
  constexpr std::size_t short_crc_size = 2;
  write_u32_le(header, crc_offset, packet_crc32(header, payload, crc_offset));

  const bool verified = verify_packet_crc32(header, payload, crc_offset, short_crc_size);

  EXPECT_FALSE(verified);
}

TEST(integrity_test, should_reject_a_crc_field_that_ends_past_the_header) {
  const std::vector<std::byte> header = bytes_from_string("\x01\x02\x03\x04\x05\x06");
  const std::vector<std::byte> payload = bytes_from_string("payload-bytes");
  constexpr std::size_t crc_offset = 3;

  const bool verified = verify_packet_crc32(header, payload, crc_offset);

  EXPECT_FALSE(verified);
}
