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

#include "configuration.h"
#include "decoder.h"
#include "encoder.h"
#include "fixtures.h"
#include "integrity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t INPUT_SIZE = 4096;
constexpr std::size_t SMALL_INPUT_SIZE = SYMBOL_SIZE_BYTES * 4;
constexpr std::size_t UNALIGNED_INPUT_SIZE = SYMBOL_SIZE_BYTES * 16 + 100;

constexpr std::byte INVERTED_BITS{0xFF};

uint32_t read_uint32_little_endian(const std::byte* source) {
  uint32_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return value;
}

struct encode_result {
  std::vector<packet> packets;
  chunk_manifest_entry manifest;
};

encode_result encode_test_data(const encoder& encoder, const std::vector<std::byte>& data,
                               const uint32_t chunk_index = 0,
                               const chunk_position position = chunk_position::BEFORE_LAST,
                               const payload_encryption encryption = payload_encryption::PLAIN) {
  auto [packets, manifest] = encoder.encode_chunk(chunk_index, data, position, encryption);
  return {std::move(packets), manifest};
}

bool feed_all_packets_to_decoder(decoder& decoder, const std::vector<packet>& packets) {
  bool any_decoded = false;
  for (const packet& packet : packets) {
    if (std::optional<chunk_decode_result> result = decoder.add_packet(fixtures::packet_span(packet));
        result.has_value() && result->success) {
      any_decoded = true;
    }
  }

  return any_decoded;
}

bool carries_a_partial_symbol(const packet& packet) {
  const std::optional<parsed_packet> parsed = parse_packet(fixtures::packet_span(packet));
  return parsed && parsed->header.payload_length < parsed->header.symbol_size;
}
}

TEST(codec_test, should_encode_a_chunk_into_its_packet_count) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);
  const std::size_t source_symbol_count = (input_data.size() + SYMBOL_SIZE_BYTES - 1) / SYMBOL_SIZE_BYTES;
  const std::size_t repair =
      static_cast<std::size_t>(std::ceil(static_cast<double>(source_symbol_count) * REPAIR_OVERHEAD));
  const std::size_t packet_count = packets.size();
  EXPECT_EQ(packet_count, source_symbol_count + repair);
}

TEST(codec_test, should_start_every_packet_with_the_magic) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);
  for (const auto& [bytes] : packets) {
    const uint32_t magic = read_uint32_little_endian(bytes.data());
    EXPECT_EQ(magic, MAGIC_ID);
  }
}

TEST(codec_test, should_write_version_two_packets) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);
  for (const auto& [bytes] : packets) {
    const uint8_t version = static_cast<uint8_t>(bytes[VERSION_OFFSET]);
    EXPECT_EQ(version, VERSION_ID_V2);
  }
}

TEST(codec_test, should_describe_the_input_in_the_manifest) {
  constexpr uint32_t chunk_index = 7;
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data, chunk_index);
  const uint32_t input_size = static_cast<uint32_t>(input_data.size());
  EXPECT_EQ(manifest.chunk_index, chunk_index);
  EXPECT_EQ(manifest.original_size, input_size);
}

TEST(codec_test, should_flag_the_last_chunk) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, chunk_position::LAST);
  for (const auto& [bytes] : packets) {
    const uint8_t flags = static_cast<uint8_t>(bytes[FLAGS_OFFSET]);
    EXPECT_NE(flags & FLAG_LAST_CHUNK, 0);
  }
}

TEST(codec_test, should_not_flag_a_middle_chunk_as_the_last) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, chunk_position::BEFORE_LAST);
  for (const auto& [bytes] : packets) {
    const uint8_t flags = static_cast<uint8_t>(bytes[FLAGS_OFFSET]);
    EXPECT_EQ(flags & FLAG_LAST_CHUNK, 0);
  }
}

TEST(codec_test, should_pad_small_data_to_a_whole_symbol) {
  const std::vector<std::byte> small_data = fixtures::make_counting_data(100);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, small_data);

  constexpr uint32_t minimum_padded_size = static_cast<uint32_t>(SYMBOL_SIZE_BYTES * 2);
  EXPECT_GE(manifest.chunk_size, minimum_padded_size);
}

TEST(codec_test, should_parse_a_valid_packet) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  const std::optional<parsed_packet> parsed = parse_packet(fixtures::packet_span(packets[0]));
  const bool parsed_present = parsed.has_value();
  ASSERT_TRUE(parsed_present);
  EXPECT_EQ(parsed->header.magic, MAGIC_ID);
  EXPECT_EQ(parsed->header.version, VERSION_ID_V2);
}

TEST(codec_test, should_reject_a_packet_with_the_wrong_magic) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  std::vector corrupted_packet(packets[0].bytes.begin(), packets[0].bytes.end());
  std::fill_n(corrupted_packet.begin() + MAGIC_OFFSET, MAGIC_SIZE, std::byte{0});

  const std::optional<parsed_packet> parsed = parse_packet(corrupted_packet);
  const bool parsed_present = parsed.has_value();
  EXPECT_FALSE(parsed_present);
}

TEST(codec_test, should_reject_a_truncated_packet) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  const std::array<std::byte, PACKET_SIZE>& source_bytes = packets[0].bytes;
  const std::vector truncated_packet(source_bytes.begin(), source_bytes.begin() + 100);

  const std::optional<parsed_packet> parsed = parse_packet(truncated_packet);
  const bool parsed_present = parsed.has_value();
  EXPECT_FALSE(parsed_present);
}

TEST(codec_test, should_check_the_checksum_of_a_parsed_packet) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  const std::optional<parsed_packet> parsed = parse_packet(fixtures::packet_span(packets[0]));
  const bool parsed_present = parsed.has_value();
  ASSERT_TRUE(parsed_present);

  const bool checksum_valid = verify_packet_checksum(*parsed);
  EXPECT_TRUE(checksum_valid);
}

TEST(codec_test, should_check_the_checksum_of_raw_packet_bytes) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  const bool crc_valid = parse_and_verify_packet(fixtures::packet_span(packets[0])).has_value();
  EXPECT_TRUE(crc_valid);
}

TEST(codec_test, should_detect_a_corrupted_payload) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  std::vector corrupted_packet(packets[0].bytes.begin(), packets[0].bytes.end());
  corrupted_packet[HEADER_SIZE_V2] ^= INVERTED_BITS;

  const bool crc_valid = parse_and_verify_packet(corrupted_packet).has_value();
  EXPECT_FALSE(crc_valid);
}

TEST(codec_test, should_detect_a_corrupted_payload_of_a_partial_symbol) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(UNALIGNED_INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  auto [packets, manifest] = encode_test_data(encoder, input_data);

  const auto partial = std::ranges::find_if(packets, carries_a_partial_symbol);
  const auto packets_end = packets.end();
  ASSERT_NE(partial, packets_end);

  partial->bytes[HEADER_SIZE_V2] ^= INVERTED_BITS;

  const bool crc_valid = parse_and_verify_packet(fixtures::packet_span(*partial)).has_value();
  EXPECT_FALSE(crc_valid);
}

TEST(codec_test, should_decode_the_chunk_it_encoded) {
  const std::vector<std::byte> original_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, original_data);

  decoder decoder;
  const bool decoded = feed_all_packets_to_decoder(decoder, packets);

  const bool chunk_complete = decoder.is_chunk_complete(0);
  EXPECT_TRUE(decoded);
  EXPECT_TRUE(chunk_complete);

  const std::optional<std::vector<std::byte>> recovered_chunk = decoder.get_chunk_data(0);
  const bool recovered_chunk_present = recovered_chunk.has_value();
  ASSERT_TRUE(recovered_chunk_present);
  EXPECT_EQ(*recovered_chunk, original_data);
}

TEST(codec_test, should_decode_a_chunk_matching_its_sha256) {
  const std::vector<std::byte> original_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, original_data);

  decoder decoder;
  std::optional<chunk_decode_result> decode_result;
  for (const packet& packet : packets) {
    if (std::optional<chunk_decode_result> result =
            decoder.add_packet(fixtures::packet_span(packet), sha256_computation::COMPUTE);
        result.has_value() && result->success) {
      decode_result = std::move(*result);
    }
  }

  const bool decode_result_present = decode_result.has_value();
  ASSERT_TRUE(decode_result_present);

  const std::span original_span(original_data.data(), original_data.size());
  const sha256_digest expected_digest = sha256(original_span);
  EXPECT_EQ(decode_result->sha256, expected_digest);
}

TEST(codec_test, should_decode_a_chunk_with_xxhash_checksums) {
  const std::vector<std::byte> original_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id(), hash_algorithm_type::XXHASH32);
  const auto [packets, manifest] = encode_test_data(encoder, original_data);

  decoder decoder;
  const bool decoded = feed_all_packets_to_decoder(decoder, packets);

  EXPECT_TRUE(decoded);

  const std::optional<std::vector<std::byte>> recovered_chunk = decoder.get_chunk_data(0);
  const bool recovered_chunk_present = recovered_chunk.has_value();
  ASSERT_TRUE(recovered_chunk_present);
  EXPECT_EQ(*recovered_chunk, original_data);
}

TEST(codec_test, should_flag_encrypted_packets) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] =
      encode_test_data(encoder, input_data, 0, chunk_position::BEFORE_LAST, payload_encryption::ENCRYPTED);
  for (const auto& [bytes] : packets) {
    const uint8_t flags = static_cast<uint8_t>(bytes[FLAGS_OFFSET]);
    EXPECT_NE(flags & FLAG_ENCRYPTED, 0);
  }
}

TEST(codec_test, should_ignore_packets_for_a_decoded_chunk) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  decoder decoder;
  feed_all_packets_to_decoder(decoder, packets);

  const bool chunk_complete = decoder.is_chunk_complete(0);
  EXPECT_TRUE(chunk_complete);

  for (const packet& packet : packets) {
    std::optional<chunk_decode_result> result = decoder.add_packet(fixtures::packet_span(packet));
    const bool result_present = result.has_value();
    EXPECT_FALSE(result_present);
  }
}

TEST(codec_test, should_decode_several_chunks) {
  const std::vector<std::byte> chunk0_data = fixtures::make_counting_data(2048);
  const std::vector<std::byte> chunk1_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());

  auto [packets0, manifest0] = encoder.encode_chunk(0, chunk0_data, chunk_position::BEFORE_LAST);
  auto [packets1, manifest1] = encoder.encode_chunk(1, chunk1_data, chunk_position::LAST);

  decoder decoder;
  feed_all_packets_to_decoder(decoder, packets0);
  feed_all_packets_to_decoder(decoder, packets1);

  const std::size_t chunks_completed = decoder.chunks_completed();
  const bool first_complete = decoder.is_chunk_complete(0);
  const bool second_complete = decoder.is_chunk_complete(1);
  EXPECT_EQ(chunks_completed, 2u);
  EXPECT_TRUE(first_complete);
  EXPECT_TRUE(second_complete);

  const std::optional<std::vector<std::byte>> data0 = decoder.get_chunk_data(0);
  const std::optional<std::vector<std::byte>> data1 = decoder.get_chunk_data(1);
  const bool data0_present = data0.has_value();
  const bool data1_present = data1.has_value();
  ASSERT_TRUE(data0_present);
  ASSERT_TRUE(data1_present);
  EXPECT_EQ(*data0, chunk0_data);
  EXPECT_EQ(*data1, chunk1_data);
}

TEST(codec_test, should_track_which_chunks_are_decoded) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  decoder decoder;
  const std::size_t initial_packets_received = decoder.total_packets_received();
  const std::size_t initial_chunks_completed = decoder.chunks_completed();
  const bool initially_complete = decoder.is_chunk_complete(0);
  EXPECT_EQ(initial_packets_received, 0u);
  EXPECT_EQ(initial_chunks_completed, 0u);
  EXPECT_FALSE(initially_complete);

  feed_all_packets_to_decoder(decoder, packets);

  const std::size_t packets_received = decoder.total_packets_received();
  const std::size_t packet_count = packets.size();
  const std::size_t chunks_completed = decoder.chunks_completed();
  const bool chunk_complete = decoder.is_chunk_complete(0);
  EXPECT_EQ(packets_received, packet_count);
  EXPECT_EQ(chunks_completed, 1u);
  EXPECT_TRUE(chunk_complete);

  std::vector<uint32_t> indices = decoder.completed_chunk_indices();
  const std::size_t index_count = indices.size();
  ASSERT_EQ(index_count, 1u);
  EXPECT_EQ(indices[0], 0u);
}

TEST(codec_test, should_track_chunks_decoded_out_of_order) {
  const std::vector<std::byte> first_data = fixtures::make_counting_data(INPUT_SIZE);
  const std::vector<std::byte> second_data = fixtures::make_counting_data(SMALL_INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [first_packets, first_manifest] = encode_test_data(encoder, first_data);
  const auto [second_packets, second_manifest] = encode_test_data(encoder, second_data, 1, chunk_position::LAST);
  constexpr uint32_t two_chunks = 2;
  decoder decoder;

  feed_all_packets_to_decoder(decoder, second_packets);

  const bool first_complete = decoder.is_chunk_complete(0);
  const std::vector<uint32_t> indices = decoder.completed_chunk_indices();
  const std::vector<uint32_t> expected_indices = {1};
  EXPECT_FALSE(first_complete);
  EXPECT_EQ(indices, expected_indices);

  feed_all_packets_to_decoder(decoder, first_packets);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(two_chunks);
  std::vector<std::byte> expected = first_data;
  expected.insert(expected.end(), second_data.begin(), second_data.end());
  EXPECT_EQ(assembled, expected);
}

TEST(codec_test, should_not_assemble_a_file_with_a_chunk_beyond_the_expected_count) {
  const std::vector<std::byte> data = fixtures::make_counting_data(SMALL_INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  constexpr uint32_t third_chunk = 2;
  constexpr uint32_t two_chunks = 2;
  const auto [first_packets, first_manifest] = encode_test_data(encoder, data);
  const auto [third_packets, third_manifest] = encode_test_data(encoder, data, third_chunk, chunk_position::LAST);
  decoder decoder;
  feed_all_packets_to_decoder(decoder, first_packets);
  feed_all_packets_to_decoder(decoder, third_packets);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(two_chunks);

  const std::size_t chunks_completed = decoder.chunks_completed();
  const bool assembled_present = assembled.has_value();
  EXPECT_EQ(chunks_completed, two_chunks);
  EXPECT_FALSE(assembled_present);
}

TEST(codec_test, should_return_a_copy_of_the_chunk_data) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  decoder decoder;
  feed_all_packets_to_decoder(decoder, packets);

  const bool first_copy_present = decoder.get_chunk_data(0).has_value();
  const bool second_copy_present = decoder.get_chunk_data(0).has_value();
  EXPECT_TRUE(first_copy_present);
  EXPECT_TRUE(second_copy_present);
}

TEST(codec_test, should_decode_a_chunk_despite_lost_packets) {
  const std::vector<std::byte> original_data = fixtures::make_counting_data(SYMBOL_SIZE_BYTES * 64);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, original_data);

  const std::size_t repair = packets.size() - manifest.source_symbol_count;
  const std::size_t kept = packets.size() - repair / 2;

  decoder decoder;
  bool decoded = false;
  for (std::size_t i = 0; i < kept; ++i) {
    if (std::optional<chunk_decode_result> result = decoder.add_packet(fixtures::packet_span(packets[i]));
        result.has_value() && result->success) {
      decoded = true;
    }
  }

  EXPECT_TRUE(decoded);

  const std::optional<std::vector<std::byte>> recovered = decoder.get_chunk_data(0);
  const bool recovered_present = recovered.has_value();
  ASSERT_TRUE(recovered_present);
  EXPECT_EQ(*recovered, original_data);
}

TEST(codec_test, should_decode_packets_delivered_out_of_order) {
  const std::vector<std::byte> original_data = fixtures::make_counting_data(SMALL_INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  auto [packets, manifest] = encode_test_data(encoder, original_data);

  std::ranges::reverse(packets);

  decoder decoder;
  const bool decoded = feed_all_packets_to_decoder(decoder, packets);

  EXPECT_TRUE(decoded);

  const std::optional<std::vector<std::byte>> recovered = decoder.get_chunk_data(0);
  const bool recovered_present = recovered.has_value();
  ASSERT_TRUE(recovered_present);
  EXPECT_EQ(*recovered, original_data);
}

TEST(codec_test, should_not_return_an_incomplete_chunk) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  decoder decoder;
  (void)decoder.add_packet(fixtures::packet_span(packets[0]));

  const bool chunk_complete = decoder.is_chunk_complete(0);
  const bool chunk_data_present = decoder.get_chunk_data(0).has_value();
  EXPECT_FALSE(chunk_complete);
  EXPECT_FALSE(chunk_data_present);
}

TEST(codec_test, should_checksum_every_packet_of_an_unaligned_chunk) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(UNALIGNED_INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);

  bool saw_partial_symbol = false;
  for (const packet& packet : packets) {
    const std::optional<parsed_packet> parsed = parse_and_verify_packet(fixtures::packet_span(packet));
    const bool parsed_present = parsed.has_value();
    ASSERT_TRUE(parsed_present);
    saw_partial_symbol |= parsed->header.payload_length < parsed->header.symbol_size;
  }

  EXPECT_TRUE(saw_partial_symbol);
}

TEST(codec_test, should_accept_the_legacy_checksum_of_a_partial_symbol) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(UNALIGNED_INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  auto [packets, manifest] = encode_test_data(encoder, input_data);

  const auto partial = std::ranges::find_if(packets, carries_a_partial_symbol);
  const auto packets_end = packets.end();
  ASSERT_NE(partial, packets_end);

  const packet_header header = parse_packet(fixtures::packet_span(*partial))->header;
  const std::span<std::byte> bytes(partial->bytes);
  write_packet_header(bytes.first(HEADER_SIZE_V2), header, bytes.subspan(HEADER_SIZE_V2, header.payload_length),
                      header.hash_algorithm());

  const bool partial_packet_verified = parse_and_verify_packet(fixtures::packet_span(*partial)).has_value();
  EXPECT_TRUE(partial_packet_verified);

  decoder decoder;
  const bool decoded = feed_all_packets_to_decoder(decoder, packets);
  EXPECT_TRUE(decoded);

  const std::optional<std::vector<std::byte>> recovered = decoder.get_chunk_data(0);
  const bool recovered_present = recovered.has_value();
  ASSERT_TRUE(recovered_present);
  EXPECT_EQ(*recovered, input_data);
}

namespace {
std::vector<std::byte> to_version_one(const packet& packet) {
  const parsed_packet parsed = *parse_packet(fixtures::packet_span(packet));
  std::vector<std::byte> converted(packet.bytes.begin(), packet.bytes.begin() + HEADER_SIZE);
  converted[VERSION_OFFSET] = std::byte{VERSION_ID};
  const uint32_t checksum = packet_checksum(converted, parsed.payload, CRC_OFFSET, parsed.header.hash_algorithm());
  std::memcpy(converted.data() + CRC_OFFSET, &checksum, sizeof(checksum));
  converted.insert(converted.end(), parsed.payload.begin(), parsed.payload.end());
  return converted;
}
}

TEST(codec_test, should_reject_a_packet_shorter_than_the_version_one_header) {
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, fixtures::make_counting_data(SYMBOL_SIZE_BYTES));
  const std::span<const std::byte> packet = fixtures::packet_span(packets[0]);

  const std::optional<parsed_packet> parsed = parse_packet(packet.first(HEADER_SIZE - 1));

  const bool parsed_present = parsed.has_value();
  EXPECT_FALSE(parsed_present);
}

TEST(codec_test, should_reject_a_version_two_packet_shorter_than_its_header) {
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, fixtures::make_counting_data(SYMBOL_SIZE_BYTES));
  const std::span<const std::byte> packet = fixtures::packet_span(packets[0]);

  const std::optional<parsed_packet> parsed = parse_packet(packet.first(HEADER_SIZE_V2 - 1));

  const bool parsed_present = parsed.has_value();
  EXPECT_FALSE(parsed_present);
}

TEST(codec_test, should_parse_a_version_one_packet_with_the_chunk_size_as_original_size) {
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, fixtures::make_counting_data(SMALL_INPUT_SIZE));
  const std::vector<std::byte> converted = to_version_one(packets[0]);

  const std::optional<parsed_packet> parsed = parse_and_verify_packet(converted);

  const bool parsed_present = parsed.has_value();
  ASSERT_TRUE(parsed_present);

  const std::size_t header_size = packet_header_size(VERSION_ID);
  const std::size_t raw_header_size = parsed->raw_header.size();
  const std::byte* const payload_start = parsed->payload.data();
  const std::byte* const expected_payload_start = converted.data() + HEADER_SIZE;
  const uint32_t expected_crc = read_uint32_little_endian(converted.data() + CRC_OFFSET);
  EXPECT_EQ(header_size, HEADER_SIZE);
  EXPECT_EQ(parsed->header.version, VERSION_ID);
  EXPECT_EQ(parsed->header.original_size, parsed->header.chunk_size);
  EXPECT_EQ(raw_header_size, HEADER_SIZE);
  EXPECT_EQ(payload_start, expected_payload_start);
  EXPECT_EQ(parsed->header.crc, expected_crc);
}

TEST(codec_test, should_decode_a_chunk_from_version_one_packets) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(SYMBOL_SIZE_BYTES * 16);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);
  decoder decoder;

  bool decoded = false;
  for (const packet& packet : packets) {
    const std::vector<std::byte> converted = to_version_one(packet);
    if (const std::optional<chunk_decode_result> result = decoder.add_packet(converted); result && result->success) {
      decoded = true;
    }
  }

  const std::optional<std::vector<std::byte>> chunk_data = decoder.get_chunk_data(0);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(chunk_data, input_data);
}

namespace {
constexpr const char* MISSING_DIRECTORY = "missing-directory";
constexpr const char* OUTPUT_NAME = "out.bin";
constexpr const char* BINARY_EXTENSION = ".bin";
constexpr const char* NO_EXTENSION = "";
constexpr std::size_t LARGE_CHUNK_BYTES = 64 * 1024;
constexpr std::byte KEY_FILL{0x5A};
constexpr std::byte WRONG_KEY_FILL{0xA5};

crypto_key make_key(const std::byte fill) {
  crypto_key key{};
  key.fill(fill);
  return key;
}

chunk_decoder make_chunk_decoder(const parsed_packet& packet) {
  const packet_header& header = packet.header;
  return chunk_decoder(header.chunk_index, header.chunk_size, header.source_symbol_count, header.symbol_size);
}

std::vector<parsed_packet> parse_all(const std::vector<packet>& packets) {
  std::vector<parsed_packet> parsed;
  for (const packet& packet : packets) {
    parsed.push_back(*parse_packet(fixtures::packet_span(packet)));
  }

  return parsed;
}

decoder decode_chunk(const std::vector<std::byte>& data, const payload_encryption encryption) {
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, data, 0, chunk_position::LAST, encryption);
  decoder decoder;
  feed_all_packets_to_decoder(decoder, packets);
  return decoder;
}

decoder decode_encrypted_chunk(const std::vector<std::byte>& plain) {
  const std::vector<std::byte> cipher = encrypt_chunk(plain, make_key(KEY_FILL), fixtures::make_test_file_id(), 0);
  return decode_chunk(cipher, payload_encryption::ENCRYPTED);
}
}

TEST(codec_test, should_refuse_a_chunk_decoder_for_fewer_than_two_symbols) {
  constexpr uint32_t one_byte = 1;
  constexpr uint32_t one_symbol = 1;

  const std::string message = fixtures::find_error_message<std::runtime_error>(
      [] { const chunk_decoder decoder(0, one_byte, one_symbol, SYMBOL_SIZE_BYTES); });

  EXPECT_EQ(message, "Wirehair decoder could not be created!");
}

TEST(codec_test, should_return_decoded_data_and_ignore_packets_after_decoding) {
  const std::vector<std::byte> input_data = fixtures::make_counting_data(SMALL_INPUT_SIZE);
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, input_data);
  const std::vector<parsed_packet> parsed = parse_all(packets);
  chunk_decoder decoder = make_chunk_decoder(parsed[0]);
  bool decoded = false;
  for (std::size_t i = 0; i < parsed.size() && !decoded; ++i) {
    decoded = decoder.add_packet(parsed[i].header.esi, parsed[i].payload);
  }

  const uint32_t received = decoder.packets_received();

  const bool added_after_decoding = decoder.add_packet(parsed.back().header.esi, parsed.back().payload);

  const uint32_t packets_received = decoder.packets_received();
  const std::vector<std::byte> decoded_data = decoder.get_decoded_data();
  EXPECT_TRUE(decoded);
  EXPECT_TRUE(added_after_decoding);
  EXPECT_EQ(packets_received, received);
  EXPECT_EQ(decoded_data, input_data);
}

TEST(codec_test, should_refuse_decoded_data_before_the_chunk_is_decoded) {
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, fixtures::make_counting_data(SMALL_INPUT_SIZE));
  chunk_decoder decoder = make_chunk_decoder(parse_all(packets)[0]);

  const std::string copy_message =
      fixtures::find_error_message<std::runtime_error>([&] { static_cast<void>(decoder.get_decoded_data()); });
  const std::string move_message =
      fixtures::find_error_message<std::runtime_error>([&] { static_cast<void>(decoder.consume_decoded_data()); });

  EXPECT_EQ(copy_message, "Chunk data is not decoded yet!");
  EXPECT_EQ(move_message, "Chunk data is not decoded yet!");
}

TEST(codec_test, should_reject_packets_on_a_moved_from_chunk_decoder) {
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, fixtures::make_counting_data(SMALL_INPUT_SIZE));
  const std::vector<parsed_packet> parsed = parse_all(packets);
  chunk_decoder original = make_chunk_decoder(parsed[0]);
  const chunk_decoder moved = std::move(original);

  const std::string message = fixtures::find_error_message<std::runtime_error>(
      [&] { static_cast<void>(original.add_packet(parsed[0].header.esi, parsed[0].payload)); });

  const uint32_t source_symbol_count = moved.source_symbol_count();
  EXPECT_EQ(message, "Codec must not be null!");
  EXPECT_EQ(source_symbol_count, parsed[0].header.source_symbol_count);
}

TEST(codec_test, should_reject_a_payload_of_the_wrong_size) {
  const encoder encoder(fixtures::make_test_file_id());
  const auto [packets, manifest] = encode_test_data(encoder, fixtures::make_counting_data(SMALL_INPUT_SIZE));
  const std::vector<parsed_packet> parsed = parse_all(packets);
  chunk_decoder decoder = make_chunk_decoder(parsed[0]);
  const std::span<const std::byte> short_payload = parsed[0].payload.first(SYMBOL_SIZE_BYTES - 1);

  const std::string message = fixtures::find_error_message<std::runtime_error>(
      [&] { static_cast<void>(decoder.add_packet(parsed[0].header.esi, short_payload)); });

  EXPECT_EQ(message, "Wirehair could not decode a packet!");
}

TEST(codec_test, should_not_assemble_before_any_packet_arrives) {
  const decoder decoder;
  constexpr uint32_t no_chunks = 0;

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(no_chunks);

  const bool assembled_present = assembled.has_value();
  EXPECT_FALSE(assembled_present);
}

TEST(codec_test, should_not_assemble_an_encrypted_file_without_a_key) {
  const decoder decoder = decode_encrypted_chunk(fixtures::make_counting_data(SMALL_INPUT_SIZE));

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);

  const bool encrypted = decoder.is_encrypted();
  const bool assembled_present = assembled.has_value();
  EXPECT_TRUE(encrypted);
  EXPECT_FALSE(assembled_present);
}

TEST(codec_test, should_assemble_an_encrypted_file_with_its_key) {
  const std::vector<std::byte> plain = fixtures::make_counting_data(SMALL_INPUT_SIZE);
  decoder decoder = decode_encrypted_chunk(plain);
  decoder.set_decrypt_key(make_key(KEY_FILL));

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);

  EXPECT_EQ(assembled, plain);
}

TEST(codec_test, should_not_assemble_an_encrypted_file_with_the_wrong_key) {
  decoder decoder = decode_encrypted_chunk(fixtures::make_counting_data(SMALL_INPUT_SIZE));
  decoder.set_decrypt_key(make_key(WRONG_KEY_FILL));

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);

  const bool assembled_present = assembled.has_value();
  EXPECT_FALSE(assembled_present);
}

TEST(codec_test, should_not_assemble_an_encrypted_chunk_without_a_size_header) {
  decoder decoder =
      decode_chunk(fixtures::make_counting_data(CRYPTO_PLAIN_SIZE_HEADER - 1), payload_encryption::ENCRYPTED);
  decoder.set_decrypt_key(make_key(KEY_FILL));

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);

  const bool assembled_present = assembled.has_value();
  EXPECT_FALSE(assembled_present);
}

TEST(codec_test, should_not_assemble_an_encrypted_chunk_claiming_more_than_a_chunk) {
  std::vector<std::byte> cipher = encrypt_chunk(fixtures::make_counting_data(SYMBOL_SIZE_BYTES), make_key(KEY_FILL),
                                                fixtures::make_test_file_id(), 0);
  const uint32_t oversized = static_cast<uint32_t>(CHUNK_SIZE_BYTES + 1);
  std::memcpy(cipher.data(), &oversized, sizeof(oversized));
  decoder decoder = decode_chunk(cipher, payload_encryption::ENCRYPTED);
  decoder.set_decrypt_key(make_key(KEY_FILL));

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);

  const bool assembled_present = assembled.has_value();
  EXPECT_FALSE(assembled_present);
}

TEST(codec_test, should_not_write_an_incomplete_file) {
  const decoder decoder = decode_chunk(fixtures::make_counting_data(SMALL_INPUT_SIZE), payload_encryption::PLAIN);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  constexpr uint32_t two_chunks = 2;

  const bool written = decoder.write_assembled_file(output.string(), two_chunks);

  const bool output_exists = std::filesystem::exists(output.path());
  EXPECT_FALSE(written);
  EXPECT_FALSE(output_exists);
}

TEST(codec_test, should_not_write_into_a_missing_directory) {
  const decoder decoder = decode_chunk(fixtures::make_counting_data(SMALL_INPUT_SIZE), payload_encryption::PLAIN);
  const fixtures::temporary_file directory(NO_EXTENSION);
  const std::filesystem::path output = directory.path() / MISSING_DIRECTORY / OUTPUT_NAME;

  const bool written = decoder.write_assembled_file(output.string(), 1);

  EXPECT_FALSE(written);
}

TEST(codec_test, should_report_a_failed_write) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  const decoder decoder = decode_chunk(fixtures::make_counting_data(LARGE_CHUNK_BYTES), payload_encryption::PLAIN);

  const bool written = decoder.write_assembled_file(fixtures::FULL_DEVICE, 1);

  EXPECT_FALSE(written);
}

TEST(codec_test, should_write_an_encrypted_file_with_its_key) {
  const std::vector<std::byte> plain = fixtures::make_counting_data(SMALL_INPUT_SIZE);
  decoder decoder = decode_encrypted_chunk(plain);
  decoder.set_decrypt_key(make_key(KEY_FILL));
  const fixtures::temporary_file output(BINARY_EXTENSION);

  const bool written = decoder.write_assembled_file(output.string(), 1);

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output.path());
  EXPECT_TRUE(written);
  EXPECT_EQ(output_bytes, plain);
}

TEST(codec_test, should_reject_a_chunk_larger_than_the_chunk_size) {
  const encoder encoder(fixtures::make_test_file_id());
  const std::vector<std::byte> oversized = fixtures::make_counting_data(CHUNK_SIZE_BYTES + 1);

  EXPECT_THROW(static_cast<void>(encoder.encode_chunk(0, oversized, chunk_position::LAST)), std::runtime_error);
}
