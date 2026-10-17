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

#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "configuration.h"
#include "media_storage.h"

namespace {
constexpr int STREAM_DECODE_TIMEOUT_SECONDS = 30;

constexpr int COMMAND_INDEX = 1;
constexpr int FIRST_OPTION_INDEX = COMMAND_INDEX + 1;
constexpr int MINIMUM_ARGUMENT_COUNT = COMMAND_INDEX + 1;

constexpr std::string_view ENCODE_COMMAND = "encode";
constexpr std::string_view DECODE_COMMAND = "decode";
constexpr std::string_view STREAM_ENCODE_COMMAND = "stream-encode";
constexpr std::string_view STREAM_DECODE_COMMAND = "stream-decode";

constexpr const char* INPUT_AND_OUTPUT_REQUIRED_MESSAGE = "both --input and --output must be specified";
constexpr const char* PASSWORD_REQUIRED_MESSAGE = "--encrypt requires --password";

constexpr const char* INPUT_LABEL = "Input: ";
constexpr const char* OUTPUT_LABEL = "Output: ";
constexpr const char* STREAM_URL_LABEL = "Stream URL: ";
constexpr const char* WRITTEN_TO_LABEL = "Written to: ";
constexpr const char* ERROR_LABEL = "Error: ";

constexpr const char* SIZE_UNITS[] = {"B", "KB", "MB", "GB", "TB"};
constexpr int SIZE_UNIT_RATIO = 1024;
constexpr int SIZE_DECIMAL_PLACES = 1;

constexpr int CONTINUE_OPERATION = 0;

struct cli_options {
  std::string command;
  std::string input_path;
  std::string output_path;
  std::string stream_url;
  bool encrypt = false;
  std::string password;
  ms_hash_algorithm_t hash_algorithm = MS_HASH_CRC32;
  int bitrate_kilobits_per_second = FRAME_BITRATE;
  int width = FRAME_WIDTH_STREAM;
  int height = FRAME_HEIGHT_STREAM;
  int frames_per_second = FRAMES_PER_SECOND;
};

std::string format_size(const uint64_t bytes) {
  constexpr int last_unit = static_cast<int>(std::size(SIZE_UNITS)) - 1;
  int unit = 0;
  double size = static_cast<double>(bytes);
  while (size >= SIZE_UNIT_RATIO && unit < last_unit) {
    size /= SIZE_UNIT_RATIO;
    ++unit;
  }
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(SIZE_DECIMAL_PLACES) << size << " " << SIZE_UNITS[unit];
  return stream.str();
}

int report_progress(const char* label, const uint64_t current, const uint64_t total) {
  std::cout << "\r" << label << " " << current;
  if (total > 0) {
    std::cout << "/" << total;
  }
  std::cout << "..." << std::flush;
  return CONTINUE_OPERATION;
}

int encode_progress(const uint64_t current, const uint64_t total, void*) {
  return report_progress("Encoding chunk", current + 1, total);
}

int decode_progress(const uint64_t current, const uint64_t total, void*) {
  return report_progress("Decoding frame", current, total);
}

int stream_encode_progress(const uint64_t current, const uint64_t total, void*) {
  return report_progress("Streaming chunk", current + 1, total);
}

int stream_decode_progress(const uint64_t current, const uint64_t total, void*) {
  return report_progress("Receiving frame", current, total);
}

void print_usage(const char* program) {
  std::cerr << "Usage:\n"
            << "  " << program
            << " encode --input <file> --output <video> [--encrypt --password <pwd>] [--hash <crc32|xxhash>]\n"
            << "  " << program << " decode --input <video> --output <file> [--password <pwd>]\n"
            << "  " << program
            << " stream-encode --input <file> --url <rtmp://...> [--bitrate <kbps>] [--width <w> --height <h>]"
               " [--fps <n>] [--encrypt --password <pwd>] [--hash <crc32|xxhash>]\n"
            << "  " << program << " stream-decode --url <stream_url> --output <file> [--password <pwd>]\n";
}

int report_failure(const ms_status_t status) {
  std::cout << "\n";
  std::cerr << ERROR_LABEL << ms_status_string(status);
  if (const std::string_view detail = ms_last_error(); !detail.empty()) {
    std::cerr << " (" << detail << ")";
  }
  std::cerr << "\n";
  return EXIT_FAILURE;
}

void print_statistics(const ms_result_t& result) {
  std::cout << "Chunks: " << result.total_chunks << "  Packets: " << result.total_packets
            << "  Frames: " << result.total_frames << "\n";
}

int do_encode(const cli_options& cli) {
  std::cout << INPUT_LABEL << cli.input_path << "\n";
  std::cout << OUTPUT_LABEL << cli.output_path << "\n";

  ms_encode_options_t options{};
  options.input_path = cli.input_path.c_str();
  options.output_path = cli.output_path.c_str();
  options.encrypt = cli.encrypt ? 1 : 0;
  options.password = cli.password.c_str();
  options.password_len = cli.password.size();
  options.hash_algorithm = cli.hash_algorithm;
  options.progress = encode_progress;

  ms_result_t result{};
  if (const ms_status_t status = ms_encode(&options, &result); status != MS_OK) {
    return report_failure(status);
  }

  std::cout << "\n\nEncode complete: " << format_size(result.input_size) << " -> " << format_size(result.output_size)
            << "\n";
  print_statistics(result);
  std::cout << WRITTEN_TO_LABEL << cli.output_path << "\n";
  return EXIT_SUCCESS;
}

int do_decode(const cli_options& cli) {
  std::cout << INPUT_LABEL << cli.input_path << "\n";
  std::cout << OUTPUT_LABEL << cli.output_path << "\n";

  ms_decode_options_t options{};
  options.input_path = cli.input_path.c_str();
  options.output_path = cli.output_path.c_str();
  options.password = cli.password.c_str();
  options.password_len = cli.password.size();
  options.progress = decode_progress;

  ms_result_t result{};
  if (const ms_status_t status = ms_decode(&options, &result); status != MS_OK) {
    return report_failure(status);
  }

  std::cout << "\n\nDecode complete: " << format_size(result.input_size) << " -> " << format_size(result.output_size)
            << "\n";
  print_statistics(result);
  std::cout << WRITTEN_TO_LABEL << cli.output_path << "\n";
  return EXIT_SUCCESS;
}

int do_stream_encode(const cli_options& cli) {
  std::cout << INPUT_LABEL << cli.input_path << "\n";
  std::cout << STREAM_URL_LABEL << cli.stream_url << "\n";
  std::cout << "Resolution: " << cli.width << "x" << cli.height << " (" << cli.frames_per_second << " fps)\n";
  std::cout << "Bitrate: " << cli.bitrate_kilobits_per_second << " kbps\n";

  ms_stream_encode_options_t options{};
  options.input_path = cli.input_path.c_str();
  options.stream_url = cli.stream_url.c_str();
  options.encrypt = cli.encrypt ? 1 : 0;
  options.password = cli.password.c_str();
  options.password_len = cli.password.size();
  options.hash_algorithm = cli.hash_algorithm;
  options.bitrate_kbps = cli.bitrate_kilobits_per_second;
  options.width = cli.width;
  options.height = cli.height;
  options.fps = cli.frames_per_second;
  options.progress = stream_encode_progress;

  ms_result_t result{};
  if (const ms_status_t status = ms_stream_encode(&options, &result); status != MS_OK) {
    return report_failure(status);
  }

  std::cout << "\n\nStream encode complete: " << format_size(result.input_size) << "\n";
  print_statistics(result);
  return EXIT_SUCCESS;
}

int do_stream_decode(const cli_options& cli) {
  std::cout << STREAM_URL_LABEL << cli.stream_url << "\n";
  std::cout << OUTPUT_LABEL << cli.output_path << "\n";
  std::cout << "Waiting for stream...\n";

  ms_stream_decode_options_t options{};
  options.stream_url = cli.stream_url.c_str();
  options.output_path = cli.output_path.c_str();
  options.password = cli.password.c_str();
  options.password_len = cli.password.size();
  options.timeout_sec = STREAM_DECODE_TIMEOUT_SECONDS;
  options.progress = stream_decode_progress;

  ms_result_t result{};
  if (const ms_status_t status = ms_stream_decode(&options, &result); status != MS_OK) {
    return report_failure(status);
  }

  std::cout << "\n\nStream decode complete: -> " << format_size(result.output_size) << "\n";
  print_statistics(result);
  std::cout << WRITTEN_TO_LABEL << cli.output_path << "\n";
  return EXIT_SUCCESS;
}

std::optional<int> parse_positive_integer(const std::string_view text) {
  int value = 0;
  const auto [parsed_end, error_code] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error_code != std::errc{} || parsed_end != text.data() + text.size() || value <= 0) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string_view> take_value(const int argc, char* argv[], int& i) {
  if (i + 1 >= argc) {
    return std::nullopt;
  }
  return std::string_view(argv[++i]);
}

std::optional<int> take_positive_integer(const int argc, char* argv[], int& i, const std::string_view flag) {
  const std::optional<std::string_view> text = take_value(argc, argv, i);
  if (!text) {
    return std::nullopt;
  }
  const std::optional<int> value = parse_positive_integer(*text);
  if (!value) {
    std::cerr << ERROR_LABEL << flag << " expects a positive integer, got '" << *text << "'\n";
  }
  return value;
}

std::optional<cli_options> parse_arguments(const int argc, char* argv[]) {
  cli_options cli;
  cli.command = argv[COMMAND_INDEX];

  for (int i = FIRST_OPTION_INDEX; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::optional<std::string_view> value;
    std::optional<int> number;

    if ((argument == "--input" || argument == "-i") && (value = take_value(argc, argv, i))) {
      cli.input_path = *value;
    } else if ((argument == "--output" || argument == "-o") && (value = take_value(argc, argv, i))) {
      cli.output_path = *value;
    } else if ((argument == "--url" || argument == "-u") && (value = take_value(argc, argv, i))) {
      cli.stream_url = *value;
    } else if ((argument == "--bitrate" || argument == "-b") &&
               (number = take_positive_integer(argc, argv, i, argument))) {
      cli.bitrate_kilobits_per_second = *number;
    } else if (argument == "--width" && (number = take_positive_integer(argc, argv, i, argument))) {
      cli.width = *number;
    } else if (argument == "--height" && (number = take_positive_integer(argc, argv, i, argument))) {
      cli.height = *number;
    } else if (argument == "--fps" && (number = take_positive_integer(argc, argv, i, argument))) {
      cli.frames_per_second = *number;
    } else if (argument == "--encrypt" || argument == "-e") {
      cli.encrypt = true;
    } else if ((argument == "--password" || argument == "-p") && (value = take_value(argc, argv, i))) {
      cli.password = *value;
    } else if ((argument == "--hash" || argument == "-H") && (value = take_value(argc, argv, i))) {
      if (*value == "xxhash") {
        cli.hash_algorithm = MS_HASH_XXHASH32;
      } else if (*value == "crc32") {
        cli.hash_algorithm = MS_HASH_CRC32;
      } else {
        std::cerr << ERROR_LABEL << "unknown hash algorithm '" << *value << "' (use crc32 or xxhash)\n";
        return std::nullopt;
      }
    } else {
      std::cerr << ERROR_LABEL << "unknown or incomplete argument '" << argument << "'\n";
      print_usage(argv[0]);
      return std::nullopt;
    }
  }
  return cli;
}

bool require(const bool condition, const char* message, const char* program) {
  if (!condition) {
    std::cerr << ERROR_LABEL << message << "\n";
    print_usage(program);
  }
  return condition;
}
}

int main(const int argc, char* argv[]) {
  if (argc < MINIMUM_ARGUMENT_COUNT) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  const std::string_view command = argv[COMMAND_INDEX];
  if (command != ENCODE_COMMAND && command != DECODE_COMMAND && command != STREAM_ENCODE_COMMAND &&
      command != STREAM_DECODE_COMMAND) {
    std::cerr << ERROR_LABEL << "unknown command '" << command << "'\n";
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  const std::optional<cli_options> cli = parse_arguments(argc, argv);
  if (!cli) {
    return EXIT_FAILURE;
  }

  const bool encrypt_ok = !cli->encrypt || !cli->password.empty();

  if (command == ENCODE_COMMAND) {
    if (!require(!cli->input_path.empty() && !cli->output_path.empty(), INPUT_AND_OUTPUT_REQUIRED_MESSAGE, argv[0]) ||
        !require(encrypt_ok, PASSWORD_REQUIRED_MESSAGE, argv[0])) {
      return EXIT_FAILURE;
    }
    return do_encode(*cli);
  }
  if (command == DECODE_COMMAND) {
    if (!require(!cli->input_path.empty() && !cli->output_path.empty(), INPUT_AND_OUTPUT_REQUIRED_MESSAGE, argv[0])) {
      return EXIT_FAILURE;
    }
    return do_decode(*cli);
  }
  if (command == STREAM_ENCODE_COMMAND) {
    if (!require(!cli->input_path.empty() && !cli->stream_url.empty(),
                 "--input and --url must be specified for stream-encode", argv[0]) ||
        !require(encrypt_ok, PASSWORD_REQUIRED_MESSAGE, argv[0])) {
      return EXIT_FAILURE;
    }
    return do_stream_encode(*cli);
  }
  if (!require(!cli->stream_url.empty() && !cli->output_path.empty(),
               "--url and --output must be specified for stream-decode", argv[0])) {
    return EXIT_FAILURE;
  }
  return do_stream_decode(*cli);
}
