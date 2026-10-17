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

#include <omp.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "configuration.h"
#include "fixtures.h"
#include "media_storage.h"

namespace {

constexpr const char* BINARY_EXTENSION = ".bin";
constexpr const char* MATROSKA_EXTENSION = ".mkv";
constexpr const char* FLV_EXTENSION = ".flv";

constexpr std::size_t SMALL_INPUT_SIZE = 1024;

constexpr int SINGLE_THREAD = 1;

constexpr int CANCEL_OPERATION = 1;

constexpr uint64_t CHUNKS_IN_ONE_CHUNK_AND_A_BYTE = 2;
constexpr uint64_t CHUNKS_IN_AN_EMPTY_FILE = 1;

int request_cancellation(uint64_t, uint64_t, void*) {
  return CANCEL_OPERATION;
}

}

TEST(api_encode_test, should_round_trip_more_chunks_than_threads_through_pipelined_batches) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const std::vector<std::byte> data = fixtures::make_pattern(CHUNK_SIZE_BYTES + 1);
  fixtures::write_bytes(input.path(), data);
  const std::string input_path = input.string();
  const std::string video_path = video.string();
  const std::string output_path = output.string();
  ms_encode_options_t encode_options{};
  encode_options.input_path = input_path.c_str();
  encode_options.output_path = video_path.c_str();
  ms_result_t result{};
  const int thread_count = omp_get_max_threads();

  omp_set_num_threads(SINGLE_THREAD);
  const ms_status_t encode_status = ms_encode(&encode_options, &result);
  omp_set_num_threads(thread_count);

  ASSERT_EQ(encode_status, MS_OK);
  EXPECT_EQ(result.total_chunks, CHUNKS_IN_ONE_CHUNK_AND_A_BYTE);

  ms_decode_options_t decode_options{};
  decode_options.input_path = video_path.c_str();
  decode_options.output_path = output_path.c_str();

  const ms_status_t decode_status = ms_decode(&decode_options, nullptr);
  ASSERT_EQ(decode_status, MS_OK);

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output.path());
  EXPECT_EQ(output_bytes, data);
}

TEST(api_encode_test, should_round_trip_more_chunks_than_threads_through_a_stream) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file stream(FLV_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const std::vector<std::byte> data = fixtures::make_pattern(CHUNK_SIZE_BYTES + 1);
  fixtures::write_bytes(input.path(), data);
  const std::string input_path = input.string();
  const std::string stream_path = stream.string();
  const std::string output_path = output.string();
  ms_stream_encode_options_t encode_options{};
  encode_options.input_path = input_path.c_str();
  encode_options.stream_url = stream_path.c_str();
  ms_result_t result{};
  const int thread_count = omp_get_max_threads();

  omp_set_num_threads(SINGLE_THREAD);
  const ms_status_t encode_status = ms_stream_encode(&encode_options, &result);
  omp_set_num_threads(thread_count);

  ASSERT_EQ(encode_status, MS_OK);
  EXPECT_EQ(result.total_chunks, CHUNKS_IN_ONE_CHUNK_AND_A_BYTE);

  ms_stream_decode_options_t decode_options{};
  decode_options.stream_url = stream_path.c_str();
  decode_options.output_path = output_path.c_str();

  const ms_status_t decode_status = ms_stream_decode(&decode_options, nullptr);
  ASSERT_EQ(decode_status, MS_OK);

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output.path());
  EXPECT_EQ(output_bytes, data);
}

TEST(api_encode_test, should_round_trip_an_empty_file) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const std::vector<std::byte> data;
  fixtures::write_bytes(input.path(), data);
  const std::string input_path = input.string();
  const std::string video_path = video.string();
  const std::string output_path = output.string();
  ms_encode_options_t encode_options{};
  encode_options.input_path = input_path.c_str();
  encode_options.output_path = video_path.c_str();
  ms_result_t result{};

  const ms_status_t encode_status = ms_encode(&encode_options, &result);

  ASSERT_EQ(encode_status, MS_OK);
  EXPECT_EQ(result.total_chunks, CHUNKS_IN_AN_EMPTY_FILE);

  ms_decode_options_t decode_options{};
  decode_options.input_path = video_path.c_str();
  decode_options.output_path = output_path.c_str();

  const ms_status_t decode_status = ms_decode(&decode_options, nullptr);
  ASSERT_EQ(decode_status, MS_OK);

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output.path());
  EXPECT_EQ(output_bytes, data);
}

TEST(api_encode_test, should_round_trip_an_empty_file_through_a_stream) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file stream(FLV_EXTENSION);
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const std::vector<std::byte> data;
  fixtures::write_bytes(input.path(), data);
  const std::string input_path = input.string();
  const std::string stream_path = stream.string();
  const std::string output_path = output.string();
  ms_stream_encode_options_t encode_options{};
  encode_options.input_path = input_path.c_str();
  encode_options.stream_url = stream_path.c_str();
  ms_result_t result{};

  const ms_status_t encode_status = ms_stream_encode(&encode_options, &result);

  ASSERT_EQ(encode_status, MS_OK);
  EXPECT_EQ(result.total_chunks, CHUNKS_IN_AN_EMPTY_FILE);

  ms_stream_decode_options_t decode_options{};
  decode_options.stream_url = stream_path.c_str();
  decode_options.output_path = output_path.c_str();

  const ms_status_t decode_status = ms_stream_decode(&decode_options, nullptr);
  ASSERT_EQ(decode_status, MS_OK);

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output.path());
  EXPECT_EQ(output_bytes, data);
}

TEST(api_encode_test, should_cancel_stream_encoding_from_the_progress_callback) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  const fixtures::temporary_file stream(FLV_EXTENSION);
  fixtures::write_bytes(input.path(), fixtures::make_pattern(SMALL_INPUT_SIZE));
  const std::string input_path = input.string();
  const std::string stream_path = stream.string();
  ms_stream_encode_options_t options{};
  options.input_path = input_path.c_str();
  options.stream_url = stream_path.c_str();
  options.progress = request_cancellation;

  const ms_status_t status = ms_stream_encode(&options, nullptr);

  const char* const last_error = ms_last_error();
  EXPECT_EQ(status, MS_ERR_ENCODE_FAILED);
  EXPECT_STREQ(last_error, "cancelled by progress callback");
}

TEST(api_encode_test, should_report_a_directory_as_stream_input) {
  const std::string directory = std::filesystem::temp_directory_path().string();
  const fixtures::temporary_file stream(FLV_EXTENSION);
  const std::string stream_path = stream.string();
  ms_stream_encode_options_t options{};
  options.input_path = directory.c_str();
  options.stream_url = stream_path.c_str();

  const ms_status_t status = ms_stream_encode(&options, nullptr);

  const bool stream_exists = std::filesystem::exists(stream.path());
  EXPECT_EQ(status, MS_ERR_INVALID_ARGS);
  EXPECT_FALSE(stream_exists);
}

TEST(api_encode_test, should_report_an_input_that_cannot_be_memory_mapped) {
  if (!std::filesystem::exists(fixtures::UNMAPPABLE_FILE)) {
    GTEST_SKIP() << fixtures::NO_UNMAPPABLE_FILE_MESSAGE;
  }

  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  const std::string video_path = video.string();
  ms_encode_options_t options{};
  options.input_path = fixtures::UNMAPPABLE_FILE;
  options.output_path = video_path.c_str();

  const ms_status_t status = ms_encode(&options, nullptr);

  const bool video_exists = std::filesystem::exists(video.path());
  EXPECT_EQ(status, MS_ERR_IO);
  EXPECT_FALSE(video_exists);
}

TEST(api_encode_test, should_report_a_stream_input_that_cannot_be_memory_mapped) {
  if (!std::filesystem::exists(fixtures::UNMAPPABLE_FILE)) {
    GTEST_SKIP() << fixtures::NO_UNMAPPABLE_FILE_MESSAGE;
  }

  const fixtures::temporary_file stream(FLV_EXTENSION);
  const std::string stream_path = stream.string();
  ms_stream_encode_options_t options{};
  options.input_path = fixtures::UNMAPPABLE_FILE;
  options.stream_url = stream_path.c_str();

  const ms_status_t status = ms_stream_encode(&options, nullptr);

  const bool stream_exists = std::filesystem::exists(stream.path());
  EXPECT_EQ(status, MS_ERR_IO);
  EXPECT_FALSE(stream_exists);
}
