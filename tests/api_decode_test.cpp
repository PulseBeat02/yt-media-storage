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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "configuration.h"
#include "encoder.h"
#include "fixtures.h"
#include "media_storage.h"
#include "packet_format.h"
#include "video_encoder.h"

namespace {

constexpr const char* BINARY_EXTENSION = ".bin";
constexpr const char* MATROSKA_EXTENSION = ".mkv";
constexpr const char* FLV_EXTENSION = ".flv";

constexpr std::size_t INPUT_SIZE = 1024;

constexpr int CANCEL_REQUEST = 1;

constexpr int STREAM_CONNECT_ATTEMPTS = 2;
constexpr std::chrono::seconds STREAM_RETRY_DELAY{1};

constexpr uint32_t LAST_CHUNK_INDEX = 1;

int request_cancellation(uint64_t, uint64_t, void*) {
  return CANCEL_REQUEST;
}

void write_encoded_pattern(const fixtures::temporary_file& input, const fixtures::temporary_file& video) {
  fixtures::write_bytes(input.path(), fixtures::make_pattern(INPUT_SIZE));
  const std::string input_path = input.string();
  const std::string video_path = video.string();
  ms_encode_options_t options{};
  options.input_path = input_path.c_str();
  options.output_path = video_path.c_str();

  const ms_status_t encode_status = ms_encode(&options, nullptr);
  ASSERT_EQ(encode_status, MS_OK);
}

void write_video(const std::string& path, const std::vector<Packet>& packets) {
  VideoEncoder encoder(path);
  encoder.encode_packets(packets);
  encoder.finalize();
}

ms_status_t decode_file(const fixtures::temporary_file& video, const fixtures::temporary_file& output) {
  const std::string video_path = video.string();
  const std::string output_path = output.string();
  ms_decode_options_t options{};
  options.input_path = video_path.c_str();
  options.output_path = output_path.c_str();
  return ms_decode(&options, nullptr);
}

}

TEST(api_decode_test, should_stop_decoding_when_the_progress_callback_cancels) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  write_encoded_pattern(input, video);
  const std::string video_path = video.string();
  const std::string output_path = output.string();
  ms_decode_options_t options{};
  options.input_path = video_path.c_str();
  options.output_path = output_path.c_str();
  options.progress = request_cancellation;

  const ms_status_t status = ms_decode(&options, nullptr);

  const bool output_exists = std::filesystem::exists(output.path());
  EXPECT_EQ(status, MS_ERR_DECODE_FAILED);
  EXPECT_FALSE(output_exists);
}

TEST(api_decode_test, should_stop_decoding_a_stream_when_the_progress_callback_cancels) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  write_encoded_pattern(input, video);
  const std::string video_path = video.string();
  const std::string output_path = output.string();
  ms_stream_decode_options_t options{};
  options.stream_url = video_path.c_str();
  options.output_path = output_path.c_str();
  options.progress = request_cancellation;

  const ms_status_t status = ms_stream_decode(&options, nullptr);

  const bool output_exists = std::filesystem::exists(output.path());
  EXPECT_EQ(status, MS_ERR_DECODE_FAILED);
  EXPECT_FALSE(output_exists);
}

TEST(api_decode_test, should_fail_to_decode_an_input_that_is_not_a_video) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), fixtures::make_pattern(INPUT_SIZE));

  const ms_status_t status = decode_file(input, output);

  const bool output_exists = std::filesystem::exists(output.path());
  EXPECT_EQ(status, MS_ERR_DECODE_FAILED);
  EXPECT_FALSE(output_exists);
}

TEST(api_decode_test, should_retry_opening_a_stream_once_a_second_until_the_timeout) {
  const fixtures::temporary_file missing_stream(FLV_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const std::string stream_url = missing_stream.string();
  const std::string output_path = output.string();
  ms_stream_decode_options_t options{};
  options.stream_url = stream_url.c_str();
  options.output_path = output_path.c_str();
  options.timeout_sec = STREAM_CONNECT_ATTEMPTS;
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

  const ms_status_t status = ms_stream_decode(&options, nullptr);

  const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(status, MS_ERR_DECODE_FAILED);
  EXPECT_GE(elapsed, STREAM_RETRY_DELAY);
}

TEST(api_decode_test, should_report_a_video_without_packets) {
  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  write_video(video.string(), {Packet{}});

  const ms_status_t status = decode_file(video, output);

  const char* const last_error = ms_last_error();
  EXPECT_EQ(status, MS_ERR_DECODE_FAILED);
  EXPECT_STREQ(last_error, "no packets found in input");
}

TEST(api_decode_test, should_report_how_many_chunks_an_incomplete_video_holds) {
  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const Encoder encoder(FileId{});
  const std::vector<std::byte> chunk = fixtures::make_pattern(INPUT_SIZE);
  const std::vector<Packet> last_chunk_packets = encoder.encode_chunk(LAST_CHUNK_INDEX, chunk, true).first;
  write_video(video.string(), last_chunk_packets);

  const ms_status_t status = decode_file(video, output);

  const char* const last_error = ms_last_error();
  const bool output_exists = std::filesystem::exists(output.path());
  EXPECT_EQ(status, MS_ERR_INCOMPLETE);
  EXPECT_STREQ(last_error, "decoded 1 of 2 chunks");
  EXPECT_FALSE(output_exists);
}
