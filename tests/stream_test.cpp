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
#include "fixtures.h"
#include "stream.h"
#include "video_decoder.h"
#include "video_encoder.h"

#include <array>
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

constexpr const char* BINARY_EXTENSION = ".bin";
constexpr const char* MATROSKA_EXTENSION = ".mkv";
constexpr const char* FLV_EXTENSION = ".flv";

constexpr const char* UNUSED_OUTPUT_PATH = "unused.flv";

constexpr std::size_t INPUT_SIZE = 65536;
constexpr std::size_t HALF_INPUT_SIZE = INPUT_SIZE / 2;

constexpr int BITRATE_KILOBITS_PER_SECOND = 8000;
constexpr int LOW_BITRATE_KILOBITS_PER_SECOND = 4000;

constexpr int WIDTH_1080P = 1920;
constexpr int HEIGHT_1080P = 1080;
constexpr int WIDTH_720P = 1280;
constexpr int HEIGHT_720P = 720;
constexpr int WIDTH_4K = 3840;
constexpr int HEIGHT_4K = 2160;

void stream_encode_file(const std::string& input_path, const std::string& output_flv,
                        const int bitrate_kilobits_per_second, const int width, const int height,
                        const payload_encryption encryption = payload_encryption::PLAIN,
                        const std::string& password = {}, const int frames_per_second = FRAMES_PER_SECOND) {
  const bool encrypted = encryption == payload_encryption::ENCRYPTED;
  const file_chunk_reader reader(input_path.c_str(),
                                 encrypted ? CHUNK_SIZE_PLAIN_MAXIMUM_ENCRYPTED : AUTOMATIC_CHUNK_SIZE);
  const std::size_t chunk_count = reader.chunk_count();
  const file_id_type file_id = fixtures::make_test_file_id();
  const encoder encoder(file_id);

  std::array<std::byte, CRYPTO_KEY_BYTES> key{};
  if (encrypted) {
    const std::span password_bytes(reinterpret_cast<const std::byte*>(password.data()), password.size());
    key = derive_key(password_bytes, file_id);
  }

  stream_encoder stream_encoder(output_flv, bitrate_kilobits_per_second, width, height, frames_per_second);

  for (std::size_t i = 0; i < chunk_count; ++i) {
    std::vector<std::byte> chunk_data = reader.read_chunk(i);
    std::span<const std::byte> data_to_encode(chunk_data);
    std::vector<std::byte> encrypted_buffer;
    if (encrypted) {
      encrypted_buffer = encrypt_chunk(data_to_encode, key, file_id, static_cast<uint32_t>(i));
      data_to_encode = encrypted_buffer;
    }
    const chunk_position position = position_of_chunk(i, chunk_count);
    auto [packets, manifest] = encoder.encode_chunk(static_cast<uint32_t>(i), data_to_encode, position, encryption);
    stream_encoder.encode_packets(packets);
  }

  stream_encoder.finalize();
  if (encrypted) {
    secure_zero(std::span<std::byte>(key));
  }
}

std::vector<std::byte> stream_decode_file(const std::string& flv_path, const std::string& password = {}) {
  video_decoder video_decoder(flv_path);
  decoder decoder;
  std::size_t total_extracted = 0;
  std::size_t decoded_chunks = 0;
  uint32_t maximum_chunk_index = 0;
  bool found_last_chunk = false;
  uint32_t last_chunk_index = 0;

  while (!video_decoder.is_eof()) {
    std::vector<std::vector<std::byte>> frame_packets = video_decoder.decode_next_frame();
    for (std::vector<std::byte>& packet_data : frame_packets) {
      ++total_extracted;
      if (packet_data.size() >= HEADER_SIZE) {
        const uint8_t flags = static_cast<uint8_t>(packet_data[FLAGS_OFFSET]);
        uint32_t chunk_index = 0;
        std::memcpy(&chunk_index, packet_data.data() + CHUNK_INDEX_OFFSET, sizeof(chunk_index));
        if (chunk_index > maximum_chunk_index) {
          maximum_chunk_index = chunk_index;
        }
        if (flags & FLAG_LAST_CHUNK) {
          found_last_chunk = true;
          last_chunk_index = chunk_index;
        }
      }
      const std::span<const std::byte> data(packet_data.data(), packet_data.size());
      if (std::optional<chunk_decode_result> chunk_result = decoder.process_packet(data, sha256_computation::SKIP);
          chunk_result && chunk_result->success) {
        ++decoded_chunks;
      }
    }
  }

  if (total_extracted == 0) {
    return {};
  }

  const uint32_t expected_chunks = found_last_chunk ? last_chunk_index + 1 : maximum_chunk_index + 1;

  if (decoded_chunks < expected_chunks) {
    return {};
  }

  if (decoder.is_encrypted()) {
    if (password.empty()) {
      return {};
    }
    const std::span<const std::byte> password_bytes(reinterpret_cast<const std::byte*>(password.data()),
                                                    password.size());
    crypto_key key = derive_key(password_bytes, *decoder.file_id());
    decoder.set_decrypt_key(key);
    secure_zero(std::span<std::byte>(key));
  }

  std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(expected_chunks);
  if (decoder.is_encrypted()) {
    decoder.clear_decrypt_key();
  }
  return assembled.value_or(std::vector<std::byte>{});
}
}

TEST(stream_test, should_restore_a_small_file_from_1080p_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(
      stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P, HEIGHT_1080P));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_a_small_file_from_4k_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(stream_encode_file(input.string(), flv.string(), 35000, WIDTH_4K, HEIGHT_4K));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_a_larger_file_from_1080p_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(256 * 1024);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(
      stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P, HEIGHT_1080P));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_an_encrypted_file_from_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  const std::string password = "test_password_123";

  ASSERT_NO_THROW(stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P,
                                     HEIGHT_1080P, payload_encryption::ENCRYPTED, password));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string(), password);
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_small_data_from_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(100);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(
      stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P, HEIGHT_1080P));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_lay_out_1080p_frames) {
  const frame_layout layout = compute_frame_layout(WIDTH_1080P, HEIGHT_1080P);
  EXPECT_EQ(layout.frame_width, WIDTH_1080P);
  EXPECT_EQ(layout.frame_height, HEIGHT_1080P);
  EXPECT_EQ(layout.blocks_per_row, 240);
  EXPECT_EQ(layout.blocks_per_column, 135);
  EXPECT_EQ(layout.total_blocks, 32400);
}

TEST(stream_test, should_lay_out_4k_frames) {
  const frame_layout layout = compute_frame_layout(WIDTH_4K, HEIGHT_4K);
  EXPECT_EQ(layout.frame_width, WIDTH_4K);
  EXPECT_EQ(layout.frame_height, HEIGHT_4K);
  EXPECT_EQ(layout.blocks_per_row, 480);
  EXPECT_EQ(layout.blocks_per_column, 270);
  EXPECT_EQ(layout.total_blocks, 129600);
}

TEST(stream_test, should_lay_out_default_frames_from_the_configuration) {
  const frame_layout default_layout = compute_frame_layout();
  const frame_layout explicit_layout = compute_frame_layout(FRAME_WIDTH, FRAME_HEIGHT);
  EXPECT_EQ(default_layout.frame_width, explicit_layout.frame_width);
  EXPECT_EQ(default_layout.frame_height, explicit_layout.frame_height);
  EXPECT_EQ(default_layout.total_blocks, explicit_layout.total_blocks);
  EXPECT_EQ(default_layout.bytes_per_frame, explicit_layout.bytes_per_frame);
}

TEST(stream_test, should_restore_a_file_from_a_matroska_video) {
  const std::vector<std::byte> original = fixtures::make_pattern(INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file mkv(MATROSKA_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  {
    const std::string input_path = input.string();
    const file_chunk_reader reader(input_path.c_str());
    const file_id_type file_id = fixtures::make_test_file_id();
    const encoder encoder(file_id);
    video_encoder video_encoder(mkv.string());

    for (std::size_t i = 0; i < reader.chunk_count(); ++i) {
      std::vector<std::byte> chunk_data = reader.read_chunk(i);
      const chunk_position position = position_of_chunk(i, reader.chunk_count());
      auto [packets, manifest] = encoder.encode_chunk(static_cast<uint32_t>(i), chunk_data, position);
      video_encoder.encode_packets(packets);
    }
    video_encoder.finalize();
  }

  const bool mkv_exists = std::filesystem::exists(mkv.path());
  ASSERT_TRUE(mkv_exists);

  {
    video_decoder video_decoder(mkv.string());
    decoder decoder;
    std::size_t decoded_chunks = 0;
    uint32_t last_chunk_index = 0;
    bool found_last = false;

    while (!video_decoder.is_eof()) {
      std::vector<std::vector<std::byte>> frame_packets = video_decoder.decode_next_frame();
      for (std::vector<std::byte>& packet_data : frame_packets) {
        if (packet_data.size() >= HEADER_SIZE) {
          const uint8_t flags = static_cast<uint8_t>(packet_data[FLAGS_OFFSET]);
          uint32_t chunk_index = 0;
          std::memcpy(&chunk_index, packet_data.data() + CHUNK_INDEX_OFFSET, sizeof(chunk_index));
          if (flags & FLAG_LAST_CHUNK) {
            found_last = true;
            last_chunk_index = chunk_index;
          }
        }
        if (std::optional<chunk_decode_result> chunk_result =
                decoder.process_packet(std::span(packet_data.data(), packet_data.size()), sha256_computation::SKIP);
            chunk_result && chunk_result->success) {
          ++decoded_chunks;
        }
      }
    }

    ASSERT_TRUE(found_last);
    const uint32_t expected = last_chunk_index + 1;
    ASSERT_GE(decoded_chunks, expected);

    const std::optional<std::vector<std::byte>> assembled = decoder.assemble_file(expected);
    const bool assembled_present = assembled.has_value();
    ASSERT_TRUE(assembled_present);
    EXPECT_EQ(*assembled, original);
  }
}

TEST(stream_test, should_restore_a_file_with_xxhash_checksums_from_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  {
    const std::string input_path = input.string();
    const file_chunk_reader reader(input_path.c_str());
    const file_id_type file_id = fixtures::make_test_file_id();
    const encoder encoder(file_id, hash_algorithm_type::XXHASH32);
    stream_encoder stream_encoder(flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P, HEIGHT_1080P);

    for (std::size_t i = 0; i < reader.chunk_count(); ++i) {
      std::vector<std::byte> chunk_data = reader.read_chunk(i);
      const chunk_position position = position_of_chunk(i, reader.chunk_count());
      auto [packets, manifest] = encoder.encode_chunk(static_cast<uint32_t>(i), chunk_data, position);
      stream_encoder.encode_packets(packets);
    }
    stream_encoder.finalize();
  }

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_a_file_from_720p_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(HALF_INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(
      stream_encode_file(input.string(), flv.string(), LOW_BITRATE_KILOBITS_PER_SECOND, WIDTH_720P, HEIGHT_720P));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_a_file_from_sixty_frames_per_second) {
  const std::vector<std::byte> original = fixtures::make_pattern(INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_720P, HEIGHT_720P,
                                     payload_encryption::PLAIN, {}, 60));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_a_file_from_one_frame_per_second) {
  const std::vector<std::byte> original = fixtures::make_pattern(HALF_INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(stream_encode_file(input.string(), flv.string(), LOW_BITRATE_KILOBITS_PER_SECOND, WIDTH_720P,
                                     HEIGHT_720P, payload_encryption::PLAIN, {}, 1));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_reject_invalid_stream_parameters) {
  EXPECT_THROW(stream_encoder(UNUSED_OUTPUT_PATH, BITRATE_KILOBITS_PER_SECOND, WIDTH_720P, HEIGHT_720P, 0),
               std::invalid_argument);
  EXPECT_THROW(stream_encoder(UNUSED_OUTPUT_PATH, 0, WIDTH_720P, HEIGHT_720P, 30), std::invalid_argument);
  EXPECT_THROW(stream_encoder(UNUSED_OUTPUT_PATH, BITRATE_KILOBITS_PER_SECOND, 16, 16, 30), std::invalid_argument);
}

TEST(stream_test, should_fit_at_least_one_packet_in_a_frame) {
  const int encoder_packets_per_frame = video_encoder::packets_per_frame();
  const int expected_packet_count = packets_per_frame(compute_frame_layout());
  EXPECT_GT(encoder_packets_per_frame, 0);
  EXPECT_EQ(encoder_packets_per_frame, expected_packet_count);

  const int stream_packets_per_frame = packets_per_frame(compute_frame_layout(FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM));
  EXPECT_GT(stream_packets_per_frame, 0);
  EXPECT_LE(stream_packets_per_frame, encoder_packets_per_frame);
}

TEST(stream_test, should_fit_the_packet_bytes_the_layout_holds) {
  const std::size_t maximum_bytes = maximum_packet_bytes_per_frame();
  const frame_layout layout = compute_frame_layout();
  const std::size_t layout_bytes = static_cast<std::size_t>(layout.bytes_per_frame);
  EXPECT_EQ(maximum_bytes, layout_bytes);
}

TEST(stream_test, should_restore_a_file_of_several_chunks_from_frames) {
  const std::vector<std::byte> original = fixtures::make_pattern(CHUNK_SIZE_BYTES + 1024);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  ASSERT_NO_THROW(
      stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P, HEIGHT_1080P));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  EXPECT_EQ(decoded, original);
}

TEST(stream_test, should_restore_a_file_from_every_frame_of_a_video) {
  const std::vector<std::byte> original = fixtures::make_pattern(HALF_INPUT_SIZE);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), original);

  stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P, HEIGHT_1080P);

  video_decoder video_decoder(flv.string());
  const std::vector<std::vector<std::byte>> all_packets = video_decoder.decode_all_frames();
  const std::size_t all_packet_count = all_packets.size();
  EXPECT_GT(all_packet_count, 0u);

  decoder decoder;
  for (const std::vector<std::byte>& packet_data : all_packets) {
    (void)decoder.process_packet(std::span(packet_data.data(), packet_data.size()), sha256_computation::SKIP);
  }

  const std::size_t chunks_completed = decoder.chunks_completed();
  EXPECT_GE(chunks_completed, 1u);
}

TEST(stream_test, should_restore_an_empty_file_from_frames) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file flv(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), {});

  ASSERT_NO_THROW(
      stream_encode_file(input.string(), flv.string(), BITRATE_KILOBITS_PER_SECOND, WIDTH_1080P, HEIGHT_1080P));

  const bool flv_exists = std::filesystem::exists(flv.path());
  ASSERT_TRUE(flv_exists);

  const std::vector<std::byte> decoded = stream_decode_file(flv.string());
  const bool decoded_empty = decoded.empty();
  EXPECT_TRUE(decoded_empty);
}
