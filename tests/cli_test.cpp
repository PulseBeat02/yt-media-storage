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

cli_result run_cli(const std::string& arguments) {
  const std::string command = quote(MEDIA_STORAGE_CLI_PATH) + " " + arguments + " 2>&1";
#ifdef _WIN32
  FILE* const pipe = _popen(command.c_str(), "r");
#else
  FILE* const pipe = popen(command.c_str(), "r");
#endif
  std::string output;
  std::array<char, READ_BUFFER_SIZE> buffer{};
  std::size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
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
  cli_test() : input_(".bin"), video_(".mkv"), output_(".out"), input_data_(fixtures::make_pattern(SMALL_INPUT_SIZE)) {
    fixtures::write_bytes(input_.path(), input_data_);
  }

  fixtures::temporary_file input_;
  fixtures::temporary_file video_;
  fixtures::temporary_file output_;
  std::vector<std::byte> input_data_;
};

}

TEST_F(cli_test, should_print_usage_without_a_command) {
  const cli_result result = run_cli("");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Usage:"), std::string::npos);
}

TEST_F(cli_test, should_reject_an_unknown_command) {
  const cli_result result = run_cli("bogus");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: unknown command 'bogus'"), std::string::npos);
  EXPECT_NE(result.output.find("Usage:"), std::string::npos);
}

TEST_F(cli_test, should_reject_an_unknown_argument) {
  const cli_result result = run_cli("encode --bogus");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: unknown or incomplete argument '--bogus'"), std::string::npos);
}

TEST_F(cli_test, should_reject_a_flag_without_a_value) {
  const cli_result result = run_cli("encode --input");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: unknown or incomplete argument '--input'"), std::string::npos);
}

TEST_F(cli_test, should_reject_a_number_flag_without_a_value) {
  const cli_result result = run_cli("stream-encode --fps");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: unknown or incomplete argument '--fps'"), std::string::npos);
}

TEST_F(cli_test, should_reject_a_bitrate_that_is_not_positive) {
  const cli_result result = run_cli("stream-encode --bitrate 0");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: --bitrate expects a positive integer, got '0'"), std::string::npos);
}

TEST_F(cli_test, should_reject_a_width_that_is_not_a_number) {
  const cli_result result = run_cli("stream-encode --width wide");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: --width expects a positive integer, got 'wide'"), std::string::npos);
}

TEST_F(cli_test, should_reject_a_height_with_trailing_characters) {
  const cli_result result = run_cli("stream-encode --height 720p");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: --height expects a positive integer, got '720p'"), std::string::npos);
}

TEST_F(cli_test, should_reject_an_unknown_hash_algorithm) {
  const cli_result result = run_cli("encode --hash md5");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: unknown hash algorithm 'md5' (use crc32 or xxhash)"), std::string::npos);
}

TEST_F(cli_test, should_require_input_and_output_for_encode) {
  const cli_result result = run_cli("encode --input " + quote(input_.string()));

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: both --input and --output must be specified"), std::string::npos);
}

TEST_F(cli_test, should_require_output_for_encode) {
  const cli_result result = run_cli("encode --output " + quote(video_.string()));

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: both --input and --output must be specified"), std::string::npos);
}

TEST_F(cli_test, should_require_a_password_when_encrypting) {
  const cli_result result = run_cli("encode -i " + quote(input_.string()) + " -o " + quote(video_.string()) + " -e");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: --encrypt requires --password"), std::string::npos);
}

TEST_F(cli_test, should_require_input_and_output_for_decode) {
  const cli_result missing_output = run_cli("decode --input " + quote(video_.string()));

  EXPECT_EQ(missing_output.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(missing_output.output.find("Error: both --input and --output must be specified"), std::string::npos);

  const cli_result missing_input = run_cli("decode --output " + quote(output_.string()));

  EXPECT_EQ(missing_input.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(missing_input.output.find("Error: both --input and --output must be specified"), std::string::npos);
}

TEST_F(cli_test, should_require_input_and_url_for_stream_encode) {
  const cli_result missing_url = run_cli("stream-encode --input " + quote(input_.string()));

  EXPECT_EQ(missing_url.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(missing_url.output.find("Error: --input and --url must be specified for stream-encode"), std::string::npos);

  const cli_result missing_input = run_cli("stream-encode --url " + quote(video_.string()));

  EXPECT_EQ(missing_input.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(missing_input.output.find("Error: --input and --url must be specified for stream-encode"),
            std::string::npos);
}

TEST_F(cli_test, should_require_a_password_when_stream_encoding_encrypted) {
  const cli_result result =
      run_cli("stream-encode -i " + quote(input_.string()) + " -u " + quote(video_.string()) + " --encrypt");

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: --encrypt requires --password"), std::string::npos);
}

TEST_F(cli_test, should_require_url_and_output_for_stream_decode) {
  const cli_result missing_output = run_cli("stream-decode --url " + quote(video_.string()));

  EXPECT_EQ(missing_output.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(missing_output.output.find("Error: --url and --output must be specified for stream-decode"),
            std::string::npos);

  const cli_result missing_url = run_cli("stream-decode --output " + quote(output_.string()));

  EXPECT_EQ(missing_url.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(missing_url.output.find("Error: --url and --output must be specified for stream-decode"),
            std::string::npos);
}

TEST_F(cli_test, should_report_a_missing_input_file_on_encode) {
  const std::string missing = input_.string() + ".missing";

  const cli_result result = run_cli("encode --input " + quote(missing) + " --output " + quote(video_.string()));

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: file not found (input file not found: " + missing + ")"), std::string::npos);
}

TEST_F(cli_test, should_report_a_missing_input_file_on_decode) {
  const std::string missing = video_.string() + ".missing";

  const cli_result result = run_cli("decode --input " + quote(missing) + " --output " + quote(output_.string()));

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: file not found (input file not found: " + missing + ")"), std::string::npos);
}

TEST_F(cli_test, should_report_a_missing_input_file_on_stream_encode) {
  const std::string missing = input_.string() + ".missing";

  const cli_result result = run_cli("stream-encode --input " + quote(missing) + " --url " + quote(video_.string()));

  EXPECT_EQ(result.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(result.output.find("Error: file not found"), std::string::npos);
}

TEST_F(cli_test, should_round_trip_a_file_through_encode_and_decode) {
  const cli_result encoded =
      run_cli("encode --input " + quote(input_.string()) + " --output " + quote(video_.string()) + " --hash crc32");

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_NE(encoded.output.find("Encoding chunk 1/1..."), std::string::npos);
  EXPECT_NE(encoded.output.find("Encode complete: 2.9 KB -> "), std::string::npos);
  EXPECT_NE(encoded.output.find("Chunks: 1  Packets: "), std::string::npos);
  EXPECT_NE(encoded.output.find("Written to: " + video_.string()), std::string::npos);

  const cli_result decoded = run_cli("decode -i " + quote(video_.string()) + " -o " + quote(output_.string()));

  EXPECT_EQ(decoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_NE(decoded.output.find("Decoding frame "), std::string::npos);
  EXPECT_NE(decoded.output.find("Decode complete: "), std::string::npos);
  EXPECT_NE(decoded.output.find("-> 2.9 KB"), std::string::npos);
  EXPECT_EQ(fixtures::read_bytes(output_.path()), input_data_);
}

TEST_F(cli_test, should_round_trip_an_encrypted_file_with_xxhash) {
  const cli_result encoded = run_cli("encode -i " + quote(input_.string()) + " -o " + quote(video_.string()) +
                                     " -e -p " + PASSWORD + " -H xxhash");

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);

  const cli_result decoded = run_cli("decode --input " + quote(video_.string()) + " --output " +
                                     quote(output_.string()) + " --password " + PASSWORD);

  EXPECT_EQ(decoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_EQ(fixtures::read_bytes(output_.path()), input_data_);
}

TEST_F(cli_test, should_report_a_decode_without_the_password) {
  const cli_result encoded = run_cli("encode -i " + quote(input_.string()) + " -o " + quote(video_.string()) +
                                     " --encrypt --password " + PASSWORD);

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);

  const cli_result decoded = run_cli("decode -i " + quote(video_.string()) + " -o " + quote(output_.string()));

  EXPECT_EQ(decoded.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(decoded.output.find("Error: encryption/decryption error (input is encrypted but no password was given)"),
            std::string::npos);
}

TEST_F(cli_test, should_round_trip_a_file_through_a_stream) {
  const fixtures::temporary_file stream(".flv");

  const cli_result encoded =
      run_cli("stream-encode -i " + quote(input_.string()) + " -u " + quote(stream.string()) +
              " -b 8000 --width 1280 --height 720 --fps 24 --encrypt --password " + PASSWORD + " --hash xxhash");

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_NE(encoded.output.find("Resolution: 1280x720 (24 fps)"), std::string::npos);
  EXPECT_NE(encoded.output.find("Bitrate: 8000 kbps"), std::string::npos);
  EXPECT_NE(encoded.output.find("Streaming chunk 1/1..."), std::string::npos);
  EXPECT_NE(encoded.output.find("Stream encode complete: 2.9 KB"), std::string::npos);

  const cli_result decoded = run_cli("stream-decode --url " + quote(stream.string()) + " --output " +
                                     quote(output_.string()) + " -p " + PASSWORD);

  EXPECT_EQ(decoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_NE(decoded.output.find("Waiting for stream..."), std::string::npos);
  EXPECT_NE(decoded.output.find("Receiving frame "), std::string::npos);
  EXPECT_NE(decoded.output.find("Stream decode complete: -> 2.9 KB"), std::string::npos);
  EXPECT_EQ(fixtures::read_bytes(output_.path()), input_data_);
}

TEST_F(cli_test, should_report_a_stream_decode_without_the_password) {
  const fixtures::temporary_file stream(".flv");

  const cli_result encoded = run_cli("stream-encode --input " + quote(input_.string()) + " --url " +
                                     quote(stream.string()) + " -e -p " + PASSWORD);

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);

  const cli_result decoded = run_cli("stream-decode -u " + quote(stream.string()) + " -o " + quote(output_.string()));

  EXPECT_EQ(decoded.exit_code, FAILURE_EXIT_CODE);
  EXPECT_NE(decoded.output.find("Error: encryption/decryption error"), std::string::npos);
}

TEST_F(cli_test, should_format_sizes_in_bytes_and_kilobytes) {
  const std::vector<std::byte> tiny = fixtures::make_pattern(1);
  fixtures::write_bytes(input_.path(), tiny);

  const cli_result encoded = run_cli("encode -i " + quote(input_.string()) + " -o " + quote(video_.string()));

  EXPECT_EQ(encoded.exit_code, SUCCESS_EXIT_CODE);
  EXPECT_NE(encoded.output.find("Encode complete: 1.0 B -> "), std::string::npos);
  EXPECT_NE(encoded.output.find(" KB\n"), std::string::npos);
}
