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

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoder.h"
#include "integrity.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace {
Encoder::FileId make_test_file_id() {
  Encoder::FileId file_id{};
  for (std::size_t i = 0; i < file_id.size(); ++i) {
    file_id[i] = std::byte{static_cast<uint8_t>(i)};
  }
  return file_id;
}

std::vector<std::byte> make_test_data(const std::size_t byte_count) {
  std::vector<std::byte> data(byte_count);
  for (std::size_t i = 0; i < byte_count; ++i) {
    data[i] = std::byte{static_cast<uint8_t>(i % 251)};
  }
  return data;
}

std::span<const std::byte> packet_span(const Packet& packet) {
  return {packet.bytes.data(), packet.bytes.size()};
}

void encode_and_feed(const Encoder& encoder, Decoder& decoder, const std::span<const std::byte> chunk_data,
                     const uint32_t chunk_index, const bool is_last_chunk) {
  for (auto [packets, manifest] = encoder.encode_chunk(chunk_index, chunk_data, is_last_chunk);
       const Packet& packet : packets) {
    (void)decoder.process_packet(packet_span(packet), false);
  }
}
} // namespace

TEST(roundtrip_test, should_restore_a_single_chunk) {
  const std::vector<std::byte> original_data = make_test_data(4096);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  encode_and_feed(encoder, decoder, original_data, 0, true);

  const std::size_t chunks_completed = decoder.chunks_completed();
  EXPECT_EQ(chunks_completed, 1u);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_several_chunks) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES * 2 + 5000;
  const std::vector<std::byte> original_data = make_test_data(total_size);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  const ChunkedStorageData chunked = chunk_byte_data(original_data);
  const auto chunk_count = static_cast<uint32_t>(chunked.chunks.size());

  for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
    const std::span<const std::byte> chunk = chunk_span(chunked, chunk_index);
    const bool is_last = (chunk_index == chunk_count - 1);
    encode_and_feed(encoder, decoder, chunk, chunk_index, is_last);
  }

  const std::size_t chunks_completed = decoder.chunks_completed();
  EXPECT_EQ(chunks_completed, chunk_count);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(chunk_count);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_small_data) {
  const std::vector<std::byte> original_data = make_test_data(10);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  encode_and_feed(encoder, decoder, original_data, 0, true);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_encrypted_data) {
  const std::vector<std::byte> original_data = make_test_data(8192);
  const Encoder::FileId file_id = make_test_file_id();
  const Encoder encoder(file_id);
  Decoder decoder;

  static constexpr std::byte password[] = {std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
  auto encryption_key = derive_key(std::span(password), file_id);

  const std::vector<std::byte> encrypted_chunk = encrypt_chunk(original_data, encryption_key, file_id, 0);
  for (auto [packets, manifest] = encoder.encode_chunk(0, encrypted_chunk, true, true);
       const Packet& packet : packets) {
    (void)decoder.process_packet(packet_span(packet), false);
  }

  const bool encrypted = decoder.is_encrypted();
  EXPECT_TRUE(encrypted);

  decoder.set_decrypt_key(encryption_key);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);

  decoder.clear_decrypt_key();
  secure_zero(std::span<std::byte>(encryption_key));
}

TEST(roundtrip_test, should_restore_data_with_xxhash_checksums) {
  const std::vector<std::byte> original_data = make_test_data(4096);
  const Encoder encoder(make_test_file_id(), HashAlgorithm::XXHash32);
  Decoder decoder;

  encode_and_feed(encoder, decoder, original_data, 0, true);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_not_assemble_a_file_with_incomplete_chunks) {
  const std::vector<std::byte> original_data = make_test_data(4096);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  encode_and_feed(encoder, decoder, original_data, 0, false);

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(2);
  const bool assembled_present = assembled.has_value();
  EXPECT_FALSE(assembled_present);
}

TEST(roundtrip_test, should_restore_data_matching_the_sha256_of_its_manifest) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 1000;
  const std::vector<std::byte> original_data = make_test_data(total_size);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  const ChunkedStorageData chunked = chunk_byte_data(original_data);
  const auto chunk_count = static_cast<uint32_t>(chunked.chunks.size());

  for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
    const std::span<const std::byte> chunk = chunk_span(chunked, chunk_index);
    const bool is_last = (chunk_index == chunk_count - 1);

    auto [packets, manifest] = encoder.encode_chunk(chunk_index, chunk, is_last);

    const Sha256Digest expected_digest = sha256(chunk);
    EXPECT_EQ(manifest.sha256, expected_digest);

    for (const Packet& packet : packets) {
      (void)decoder.process_packet(packet_span(packet), false);
    }
  }

  const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(chunk_count);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_encrypted_data_with_xxhash_checksums) {
  const std::vector<std::byte> original_data = make_test_data(8192);
  const Encoder::FileId file_id = make_test_file_id();
  const Encoder encoder(file_id, HashAlgorithm::XXHash32);
  Decoder decoder;

  static constexpr std::byte password[] = {std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
  auto key = derive_key(std::span(password), file_id);

  const std::vector<std::byte> encrypted = encrypt_chunk(original_data, key, file_id, 0);
  for (auto [packets, manifest] = encoder.encode_chunk(0, encrypted, true, true); const Packet& packet : packets) {
    (void)decoder.process_packet(packet_span(packet), false);
  }

  const bool decoder_encrypted = decoder.is_encrypted();
  EXPECT_TRUE(decoder_encrypted);

  decoder.set_decrypt_key(key);

  const auto assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);

  decoder.clear_decrypt_key();
  secure_zero(std::span<std::byte>(key));
}

TEST(roundtrip_test, should_restore_empty_data) {
  const std::vector<std::byte> original_data;
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  encode_and_feed(encoder, decoder, original_data, 0, true);

  const auto assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  const bool assembled_empty = assembled->empty();
  ASSERT_TRUE(assembled_present);
  EXPECT_TRUE(assembled_empty);
}

TEST(roundtrip_test, should_restore_data_of_one_chunk_size) {
  const std::vector<std::byte> original_data = make_test_data(CHUNK_SIZE_BYTES);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  encode_and_feed(encoder, decoder, original_data, 0, true);

  const auto assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_data_of_two_chunk_sizes) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES * 2;
  const std::vector<std::byte> original_data = make_test_data(total_size);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  const ChunkedStorageData chunked = chunk_byte_data(original_data);
  const auto chunk_count = static_cast<uint32_t>(chunked.chunks.size());
  ASSERT_EQ(chunk_count, 2u);

  for (uint32_t i = 0; i < chunk_count; ++i) {
    encode_and_feed(encoder, decoder, chunk_span(chunked, i), i, i == chunk_count - 1);
  }

  const auto assembled = decoder.assemble_file(chunk_count);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_many_chunks) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES * 5 + 12345;
  const std::vector<std::byte> original_data = make_test_data(total_size);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  const ChunkedStorageData chunked = chunk_byte_data(original_data);
  const auto chunk_count = static_cast<uint32_t>(chunked.chunks.size());
  EXPECT_EQ(chunk_count, 6u);

  for (uint32_t i = 0; i < chunk_count; ++i) {
    encode_and_feed(encoder, decoder, chunk_span(chunked, i), i, i == chunk_count - 1);
  }

  const auto assembled = decoder.assemble_file(chunk_count);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_data_from_packets_out_of_order) {
  const std::vector<std::byte> original_data = make_test_data(SYMBOL_SIZE_BYTES * 4);
  const Encoder encoder(make_test_file_id());
  auto [packets, manifest] = encoder.encode_chunk(0, original_data, true);

  std::ranges::reverse(packets);

  Decoder decoder;
  for (const Packet& packet : packets) {
    (void)decoder.process_packet(packet_span(packet), false);
  }

  const auto assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_restore_data_despite_lost_packets) {
  const std::vector<std::byte> original_data = make_test_data(SYMBOL_SIZE_BYTES * 64);
  const Encoder encoder(make_test_file_id());
  auto [packets, manifest] = encoder.encode_chunk(0, original_data, true);

  const std::size_t repair = packets.size() - manifest.source_symbol_count;
  const std::size_t drop_every = packets.size() / (repair / 2);

  Decoder decoder;
  for (std::size_t i = 0; i < packets.size(); ++i) {
    if (i % drop_every == 0)
      continue;
    (void)decoder.process_packet(packet_span(packets[i]), false);
  }

  const bool chunk_complete = decoder.is_chunk_complete(0);
  EXPECT_TRUE(chunk_complete);

  const auto assembled = decoder.assemble_file(1);
  const bool assembled_present = assembled.has_value();
  ASSERT_TRUE(assembled_present);
  EXPECT_EQ(*assembled, original_data);
}

TEST(roundtrip_test, should_write_the_assembled_file) {
  const std::vector<std::byte> original_data = make_test_data(4096);
  const Encoder encoder(make_test_file_id());
  Decoder decoder;

  encode_and_feed(encoder, decoder, original_data, 0, true);

  const auto output_path = std::filesystem::temp_directory_path() / "test_write_assembled.bin";
  const bool success = decoder.write_assembled_file(output_path.string(), 1);
  EXPECT_TRUE(success);

  std::ifstream input(output_path, std::ios::binary | std::ios::ate);
  const bool file_open = input.is_open();
  ASSERT_TRUE(file_open);

  const auto file_size = input.tellg();
  input.seekg(0);
  std::vector<std::byte> file_data(file_size);
  input.read(reinterpret_cast<char*>(file_data.data()), file_size);

  EXPECT_EQ(file_data, original_data);

  input.close();
  std::filesystem::remove(output_path);
}
