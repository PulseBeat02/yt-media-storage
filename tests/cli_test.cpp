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

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "fixtures.h"

namespace {

constexpr std::size_t SMALL_INPUT_SIZE = 3000;
constexpr std::size_t READ_BUFFER_SIZE = 4096;

constexpr int FAILURE_EXIT_CODE = 1;
constexpr int SUCCESS_EXIT_CODE = 0;

constexpr const char* PASSWORD = "cli-password";

constexpr const char* BINARY_EXTENSION = ".bin";
constexpr const char* MATROSKA_EXTENSION = ".mkv";
constexpr const char* FLV_EXTENSION = ".flv";
constexpr const char* OUTPUT_EXTENSION = ".out";

constexpr const char* READ_MODE = "r";
constexpr const char* ARGUMENT_SEPARATOR = " ";
constexpr const char* MERGE_ERRORS_INTO_OUTPUT = "2>&1";

constexpr const char* ENCODE_COMMAND = "encode";
constexpr const char* DECODE_COMMAND = "decode";
constexpr const char* STREAM_ENCODE_COMMAND = "stream-encode";
constexpr const char* STREAM_DECODE_COMMAND = "stream-decode";

constexpr const char* INPUT_FLAG = "--input";
constexpr const char* SHORT_INPUT_FLAG = "-i";
constexpr const char* OUTPUT_FLAG = "--output";
constexpr const char* SHORT_OUTPUT_FLAG = "-o";
constexpr const char* URL_FLAG = "--url";
constexpr const char* SHORT_URL_FLAG = "-u";
constexpr const char* PASSWORD_FLAG = "--password";
constexpr const char* SHORT_PASSWORD_FLAG = "-p";
constexpr const char* ENCRYPT_FLAG = "--encrypt";
constexpr const char* SHORT_ENCRYPT_FLAG = "-e";
constexpr const char* HASH_FLAG = "--hash";
constexpr const char* SHORT_HASH_FLAG = "-H";
constexpr const char* BITRATE_FLAG = "--bitrate";
constexpr const char* SHORT_BITRATE_FLAG = "-b";
constexpr const char* WIDTH_FLAG = "--width";
constexpr const char* HEIGHT_FLAG = "--height";
constexpr const char* FRAMES_PER_SECOND_FLAG = "--fps";

constexpr const char* CRC32_ALGORITHM = "crc32";
constexpr const char* XXHASH_ALGORITHM = "xxhash";

constexpr const char* USAGE_HEADER = "Usage:";
constexpr const char* MISSING_INPUT_OR_OUTPUT_MESSAGE = "Error: both --input and --output must be specified";
constexpr const char* MISSING_PASSWORD_MESSAGE = "Error: --encrypt requires --password";
constexpr const char* MISSING_INPUT_OR_URL_MESSAGE = "Error: --input and --url must be specified for stream-encode";
constexpr const char* MISSING_URL_OR_OUTPUT_MESSAGE = "Error: --url and --output must be specified for stream-decode";
constexpr const char* INPUT_FILE_NOT_FOUND_PREFIX = "Error: file not found (input file not found: ";

struct cli_result {
  int exit_code;
  std::string output;
};

std::string quote(const std::string& text) {
  return "\"" + text + "\"";
}

int exit_code_of(const int status) {
#ifdef _WIN32
  return status;
#else
  return WEXITSTATUS(status);
#endif
}

std::string build_command(const std::vector<std::string>& arguments) {
  std::string command = quote(MEDIA_STORAGE_CLI_PATH);
  for (const std::string& argument : arguments) {
    command += ARGUMENT_SEPARATOR + argument;
  }

  return command + ARGUMENT_SEPARATOR + MERGE_ERRORS_INTO_OUTPUT;
}

cli_result run_cli(const std::vector<std::string>& arguments) {
  const std::string command = build_command(arguments);
#ifdef _WIN32
  FILE* const pipe = _popen(command.c_str(), READ_MODE);
#else
  FILE* const pipe = popen(command.c_str(), READ_MODE);
#endif
  std::string output;
  std::array<char, READ_BUFFER_SIZE> buffer{};
  std::size_t read = 0;
  while ((read = std::fread(buffer.data(), sizeof(char), buffer.size(), pipe)) > 0) {
    output.append(buffer.data(), read);
  }

#ifdef _WIN32
  const int status = _pclose(pipe);
#else
  const int status = pclose(pipe);
#endif
  return {exit_code_of(status), output};
}

class cli_test : public testing::Test {
protected:
  cli_test()
      : input_(BINARY_EXTENSION), video_(MATROSKA_EXTENSION), output_(OUTPUT_EXTENSION),
        input_data_(fixtures::make_pattern(SMALL_INPUT_SIZE)) {
    fixtures::write_bytes(input_.path(), input_data_);
  }

  fixtures::temporary_file input_;
  fixtures::temporary_file video_;
  fixtures::temporary_file output_;
  std::vector<std::byte> input_data_;
};

}

TEST_F(cli_test, should_print_usage_without_a_command) {
  const cli_result result = run_cli({});

  const bool mentions_usage = result.output.contains(USAGE_HEADER);
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_usage) << result.output;
}

TEST_F(cli_test, should_reject_an_unknown_command) {
  const cli_result result = run_cli({"bogus"});

  const bool mentions_unknown_command_bogus = result.output.contains("Error: unknown command 'bogus'");
  const bool mentions_usage = result.output.contains(USAGE_HEADER);
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_command_bogus) << result.output;
  EXPECT_TRUE(mentions_usage) << result.output;
}

TEST_F(cli_test, should_reject_an_unknown_argument) {
  const cli_result result = run_cli({ENCODE_COMMAND, "--bogus"});

  const bool mentions_unknown_or_incomplete_argument =
      result.output.contains("Error: unknown or incomplete argument '--bogus'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_or_incomplete_argument) << result.output;
}

TEST_F(cli_test, should_reject_a_flag_without_a_value) {
  const cli_result result = run_cli({ENCODE_COMMAND, INPUT_FLAG});

  const bool mentions_unknown_or_incomplete_argument =
      result.output.contains("Error: unknown or incomplete argument '--input'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_or_incomplete_argument) << result.output;
}

TEST_F(cli_test, should_reject_an_output_flag_without_a_value) {
  const cli_result result = run_cli({ENCODE_COMMAND, OUTPUT_FLAG});

  const bool mentions_unknown_or_incomplete_argument =
      result.output.contains("Error: unknown or incomplete argument '--output'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_or_incomplete_argument) << result.output;
}

TEST_F(cli_test, should_reject_a_url_flag_without_a_value) {
  const cli_result result = run_cli({STREAM_ENCODE_COMMAND, URL_FLAG});

  const bool mentions_unknown_or_incomplete_argument =
      result.output.contains("Error: unknown or incomplete argument '--url'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_or_incomplete_argument) << result.output;
}

TEST_F(cli_test, should_reject_a_password_flag_without_a_value) {
  const cli_result result = run_cli({DECODE_COMMAND, PASSWORD_FLAG});

  const bool mentions_unknown_or_incomplete_argument =
      result.output.contains("Error: unknown or incomplete argument '--password'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_or_incomplete_argument) << result.output;
}

TEST_F(cli_test, should_reject_a_hash_flag_without_a_value) {
  const cli_result result = run_cli({ENCODE_COMMAND, HASH_FLAG});

  const bool mentions_unknown_or_incomplete_argument =
      result.output.contains("Error: unknown or incomplete argument '--hash'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_or_incomplete_argument) << result.output;
}

TEST_F(cli_test, should_reject_a_number_flag_without_a_value) {
  const cli_result result = run_cli({STREAM_ENCODE_COMMAND, FRAMES_PER_SECOND_FLAG});

  const bool mentions_unknown_or_incomplete_argument =
      result.output.contains("Error: unknown or incomplete argument '--fps'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_or_incomplete_argument) << result.output;
}

TEST_F(cli_test, should_reject_a_bitrate_that_is_not_positive) {
  const cli_result result = run_cli({STREAM_ENCODE_COMMAND, BITRATE_FLAG, "0"});

  const bool mentions_bitrate_expects_a_positive =
      result.output.contains("Error: --bitrate expects a positive integer, got '0'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_bitrate_expects_a_positive) << result.output;
}

TEST_F(cli_test, should_reject_a_width_that_is_not_a_number) {
  const cli_result result = run_cli({STREAM_ENCODE_COMMAND, WIDTH_FLAG, "wide"});

  const bool mentions_width_expects_a_positive =
      result.output.contains("Error: --width expects a positive integer, got 'wide'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_width_expects_a_positive) << result.output;
}

TEST_F(cli_test, should_reject_a_height_with_trailing_characters) {
  const cli_result result = run_cli({STREAM_ENCODE_COMMAND, HEIGHT_FLAG, "720p"});

  const bool mentions_height_expects_a_positive =
      result.output.contains("Error: --height expects a positive integer, got '720p'");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_height_expects_a_positive) << result.output;
}

TEST_F(cli_test, should_reject_an_unknown_hash_algorithm) {
  const cli_result result = run_cli({ENCODE_COMMAND, HASH_FLAG, "md5"});

  const bool mentions_unknown_hash_algorithm_md5 =
      result.output.contains("Error: unknown hash algorithm 'md5' (use crc32 or xxhash)");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_unknown_hash_algorithm_md5) << result.output;
}

TEST_F(cli_test, should_require_input_and_output_for_encode) {
  const cli_result result = run_cli({ENCODE_COMMAND, INPUT_FLAG, quote(input_.string())});

  const bool mentions_both_input_and_output = result.output.contains(MISSING_INPUT_OR_OUTPUT_MESSAGE);
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_both_input_and_output) << result.output;
}

TEST_F(cli_test, should_require_output_for_encode) {
  const cli_result result = run_cli({ENCODE_COMMAND, OUTPUT_FLAG, quote(video_.string())});

  const bool mentions_both_input_and_output = result.output.contains(MISSING_INPUT_OR_OUTPUT_MESSAGE);
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_both_input_and_output) << result.output;
}

TEST_F(cli_test, should_require_a_password_when_encrypting) {
  const cli_result result = run_cli({ENCODE_COMMAND, SHORT_INPUT_FLAG, quote(input_.string()), SHORT_OUTPUT_FLAG,
                                     quote(video_.string()), SHORT_ENCRYPT_FLAG});

  const bool mentions_encrypt_requires_password = result.output.contains(MISSING_PASSWORD_MESSAGE);
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_encrypt_requires_password) << result.output;
}

TEST_F(cli_test, should_require_input_and_output_for_decode) {
  const cli_result missing_output = run_cli({DECODE_COMMAND, INPUT_FLAG, quote(video_.string())});

  const bool missing_output_explained = missing_output.output.contains(MISSING_INPUT_OR_OUTPUT_MESSAGE);
  EXPECT_EQ(missing_output.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(missing_output_explained) << missing_output.output;

  const cli_result missing_input = run_cli({DECODE_COMMAND, OUTPUT_FLAG, quote(output_.string())});

  const bool missing_input_explained = missing_input.output.contains(MISSING_INPUT_OR_OUTPUT_MESSAGE);
  EXPECT_EQ(missing_input.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(missing_input_explained) << missing_input.output;
}

TEST_F(cli_test, should_require_input_and_url_for_stream_encode) {
  const cli_result missing_url = run_cli({STREAM_ENCODE_COMMAND, INPUT_FLAG, quote(input_.string())});

  const bool missing_url_explained = missing_url.output.contains(MISSING_INPUT_OR_URL_MESSAGE);
  EXPECT_EQ(missing_url.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(missing_url_explained) << missing_url.output;

  const cli_result missing_input = run_cli({STREAM_ENCODE_COMMAND, URL_FLAG, quote(video_.string())});

  const bool missing_input_explained = missing_input.output.contains(MISSING_INPUT_OR_URL_MESSAGE);
  EXPECT_EQ(missing_input.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(missing_input_explained) << missing_input.output;
}

TEST_F(cli_test, should_require_a_password_when_stream_encoding_encrypted) {
  const cli_result result = run_cli({STREAM_ENCODE_COMMAND, SHORT_INPUT_FLAG, quote(input_.string()), SHORT_URL_FLAG,
                                     quote(video_.string()), ENCRYPT_FLAG});

  const bool mentions_encrypt_requires_password = result.output.contains(MISSING_PASSWORD_MESSAGE);
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_encrypt_requires_password) << result.output;
}

TEST_F(cli_test, should_require_url_and_output_for_stream_decode) {
  const cli_result missing_output = run_cli({STREAM_DECODE_COMMAND, URL_FLAG, quote(video_.string())});

  const bool missing_output_explained = missing_output.output.contains(MISSING_URL_OR_OUTPUT_MESSAGE);
  EXPECT_EQ(missing_output.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(missing_output_explained) << missing_output.output;

  const cli_result missing_url = run_cli({STREAM_DECODE_COMMAND, OUTPUT_FLAG, quote(output_.string())});

  const bool missing_url_explained = missing_url.output.contains(MISSING_URL_OR_OUTPUT_MESSAGE);
  EXPECT_EQ(missing_url.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(missing_url_explained) << missing_url.output;
}

TEST_F(cli_test, should_report_a_missing_input_file_on_encode) {
  const fixtures::temporary_file missing_input(BINARY_EXTENSION);
  const std::string missing_input_path = missing_input.string();

  const cli_result result =
      run_cli({ENCODE_COMMAND, INPUT_FLAG, quote(missing_input_path), OUTPUT_FLAG, quote(video_.string())});

  const bool mentions_file_not_found_input =
      result.output.contains(INPUT_FILE_NOT_FOUND_PREFIX + missing_input_path + ")");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_file_not_found_input) << result.output;
}

TEST_F(cli_test, should_report_a_missing_input_file_on_decode) {
  const fixtures::temporary_file missing_video(MATROSKA_EXTENSION);
  const std::string missing_video_path = missing_video.string();

  const cli_result result =
      run_cli({DECODE_COMMAND, INPUT_FLAG, quote(missing_video_path), OUTPUT_FLAG, quote(output_.string())});

  const bool mentions_file_not_found_input =
      result.output.contains(INPUT_FILE_NOT_FOUND_PREFIX + missing_video_path + ")");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_file_not_found_input) << result.output;
}

TEST_F(cli_test, should_report_a_missing_input_file_on_stream_encode) {
  const fixtures::temporary_file missing_input(BINARY_EXTENSION);
  const std::string missing_input_path = missing_input.string();

  const cli_result result =
      run_cli({STREAM_ENCODE_COMMAND, INPUT_FLAG, quote(missing_input_path), URL_FLAG, quote(video_.string())});

  const bool mentions_file_not_found = result.output.contains("Error: file not found");
  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_file_not_found) << result.output;
}

TEST_F(cli_test, should_round_trip_a_file_through_encode_and_decode) {
  const cli_result encoded = run_cli({ENCODE_COMMAND, INPUT_FLAG, quote(input_.string()), OUTPUT_FLAG,
                                      quote(video_.string()), HASH_FLAG, CRC32_ALGORITHM});

  const bool mentions_encoding_chunk = encoded.output.contains("Encoding chunk 1/1...");
  const bool mentions_encode_complete = encoded.output.contains("Encode complete: 2.9 KB -> ");
  const bool mentions_packet_count = encoded.output.contains("Chunks: 1  Packets: ");
  const bool mentions_written_to = encoded.output.contains("Written to: " + video_.string());
  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_TRUE(mentions_encoding_chunk) << encoded.output;
  EXPECT_TRUE(mentions_encode_complete) << encoded.output;
  EXPECT_TRUE(mentions_packet_count) << encoded.output;
  EXPECT_TRUE(mentions_written_to) << encoded.output;

  const cli_result decoded =
      run_cli({DECODE_COMMAND, SHORT_INPUT_FLAG, quote(video_.string()), SHORT_OUTPUT_FLAG, quote(output_.string())});

  const bool mentions_decoding_frame = decoded.output.contains("Decoding frame ");
  const bool mentions_decode_complete = decoded.output.contains("Decode complete: ");
  const bool mentions_decoded_size = decoded.output.contains("-> 2.9 KB");
  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output_.path());
  EXPECT_EQ(decoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_TRUE(mentions_decoding_frame) << decoded.output;
  EXPECT_TRUE(mentions_decode_complete) << decoded.output;
  EXPECT_TRUE(mentions_decoded_size) << decoded.output;
  EXPECT_EQ(output_bytes, input_data_);
}

TEST_F(cli_test, should_round_trip_an_encrypted_file_with_xxhash) {
  const cli_result encoded =
      run_cli({ENCODE_COMMAND, SHORT_INPUT_FLAG, quote(input_.string()), SHORT_OUTPUT_FLAG, quote(video_.string()),
               SHORT_ENCRYPT_FLAG, SHORT_PASSWORD_FLAG, PASSWORD, SHORT_HASH_FLAG, XXHASH_ALGORITHM});

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);

  const cli_result decoded = run_cli({DECODE_COMMAND, INPUT_FLAG, quote(video_.string()), OUTPUT_FLAG,
                                      quote(output_.string()), PASSWORD_FLAG, PASSWORD});

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output_.path());
  EXPECT_EQ(decoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_EQ(output_bytes, input_data_);
}

TEST_F(cli_test, should_report_a_decode_without_the_password) {
  const cli_result encoded = run_cli({ENCODE_COMMAND, SHORT_INPUT_FLAG, quote(input_.string()), SHORT_OUTPUT_FLAG,
                                      quote(video_.string()), ENCRYPT_FLAG, PASSWORD_FLAG, PASSWORD});

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);

  const cli_result decoded =
      run_cli({DECODE_COMMAND, SHORT_INPUT_FLAG, quote(video_.string()), SHORT_OUTPUT_FLAG, quote(output_.string())});

  const bool mentions_encryption_decryption_input_is =
      decoded.output.contains("Error: encryption/decryption error (input is encrypted but no password was given)");
  EXPECT_EQ(decoded.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_encryption_decryption_input_is) << decoded.output;
}

TEST_F(cli_test, should_round_trip_a_file_through_a_stream) {
  const fixtures::temporary_file stream(FLV_EXTENSION);

  const cli_result encoded =
      run_cli({STREAM_ENCODE_COMMAND, SHORT_INPUT_FLAG, quote(input_.string()), SHORT_URL_FLAG, quote(stream.string()),
               SHORT_BITRATE_FLAG, "8000", WIDTH_FLAG, "1280", HEIGHT_FLAG, "720", FRAMES_PER_SECOND_FLAG, "24",
               ENCRYPT_FLAG, PASSWORD_FLAG, PASSWORD, HASH_FLAG, XXHASH_ALGORITHM});

  const bool mentions_resolution = encoded.output.contains("Resolution: 1280x720 (24 fps)");
  const bool mentions_bitrate = encoded.output.contains("Bitrate: 8000 kbps");
  const bool mentions_streaming_chunk = encoded.output.contains("Streaming chunk 1/1...");
  const bool mentions_stream_encode_complete = encoded.output.contains("Stream encode complete: 2.9 KB");
  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_TRUE(mentions_resolution) << encoded.output;
  EXPECT_TRUE(mentions_bitrate) << encoded.output;
  EXPECT_TRUE(mentions_streaming_chunk) << encoded.output;
  EXPECT_TRUE(mentions_stream_encode_complete) << encoded.output;

  const cli_result decoded = run_cli({STREAM_DECODE_COMMAND, URL_FLAG, quote(stream.string()), OUTPUT_FLAG,
                                      quote(output_.string()), SHORT_PASSWORD_FLAG, PASSWORD});

  const bool mentions_waiting_for_stream = decoded.output.contains("Waiting for stream...");
  const bool mentions_receiving_frame = decoded.output.contains("Receiving frame ");
  const bool mentions_stream_decode_complete = decoded.output.contains("Stream decode complete: -> 2.9 KB");
  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output_.path());
  EXPECT_EQ(decoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_TRUE(mentions_waiting_for_stream) << decoded.output;
  EXPECT_TRUE(mentions_receiving_frame) << decoded.output;
  EXPECT_TRUE(mentions_stream_decode_complete) << decoded.output;
  EXPECT_EQ(output_bytes, input_data_);
}

TEST_F(cli_test, should_report_a_stream_decode_without_the_password) {
  const fixtures::temporary_file stream(FLV_EXTENSION);

  const cli_result encoded = run_cli({STREAM_ENCODE_COMMAND, INPUT_FLAG, quote(input_.string()), URL_FLAG,
                                      quote(stream.string()), SHORT_ENCRYPT_FLAG, SHORT_PASSWORD_FLAG, PASSWORD});

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);

  const cli_result decoded = run_cli(
      {STREAM_DECODE_COMMAND, SHORT_URL_FLAG, quote(stream.string()), SHORT_OUTPUT_FLAG, quote(output_.string())});

  const bool mentions_encryption_decryption = decoded.output.contains("Error: encryption/decryption error");
  EXPECT_EQ(decoded.exit_code, FAILURE_EXIT_CODE);
  EXPECT_TRUE(mentions_encryption_decryption) << decoded.output;
}

TEST_F(cli_test, should_format_sizes_in_bytes_and_kilobytes) {
  const std::vector<std::byte> tiny = fixtures::make_pattern(1);
  fixtures::write_bytes(input_.path(), tiny);

  const cli_result encoded =
      run_cli({ENCODE_COMMAND, SHORT_INPUT_FLAG, quote(input_.string()), SHORT_OUTPUT_FLAG, quote(video_.string())});

  const bool mentions_bytes = encoded.output.contains("Encode complete: 1.0 B -> ");
  const bool mentions_kilobytes = encoded.output.contains(" KB\n");
  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_TRUE(mentions_bytes) << encoded.output;
  EXPECT_TRUE(mentions_kilobytes) << encoded.output;
}
