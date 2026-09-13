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
    constexpr int STREAM_DECODE_TIMEOUT_SEC = 30;

    struct CliOptions {
        std::string command;
        std::string input_path;
        std::string output_path;
        std::string stream_url;
        bool encrypt = false;
        std::string password;
        ms_hash_algorithm_t hash_algorithm = MS_HASH_CRC32;
        int bitrate_kbps = FRAME_BITRATE;
        int width = FRAME_WIDTH_STREAM;
        int height = FRAME_HEIGHT_STREAM;
        int fps = FRAME_FPS;
    };

    std::string format_size(const uint64_t bytes) {
        constexpr const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        constexpr int last_unit = static_cast<int>(std::size(units)) - 1;
        int unit = 0;
        auto size = static_cast<double>(bytes);
        while (size >= 1024 && unit < last_unit) {
            size /= 1024;
            ++unit;
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
        return oss.str();
    }

    int report_progress(const char *label, const uint64_t current, const uint64_t total) {
        std::cout << "\r" << label << " " << current;
        if (total > 0) {
            std::cout << "/" << total;
        }
        std::cout << "..." << std::flush;
        return 0;
    }

    int encode_progress(const uint64_t current, const uint64_t total, void *) {
        return report_progress("Encoding chunk", current + 1, total);
    }

    int decode_progress(const uint64_t current, const uint64_t total, void *) {
        return report_progress("Decoding frame", current, total);
    }

    int stream_encode_progress(const uint64_t current, const uint64_t total, void *) {
        return report_progress("Streaming chunk", current + 1, total);
    }

    int stream_decode_progress(const uint64_t current, const uint64_t total, void *) {
        return report_progress("Receiving frame", current, total);
    }

    void print_usage(const char *program) {
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
        std::cerr << "Error: " << ms_status_string(status);
        if (const std::string_view detail = ms_last_error(); !detail.empty()) {
            std::cerr << " (" << detail << ")";
        }
        std::cerr << "\n";
        return 1;
    }

    void print_stats(const ms_result_t &result) {
        std::cout << "Chunks: " << result.total_chunks
                << "  Packets: " << result.total_packets
                << "  Frames: " << result.total_frames << "\n";
    }

    int do_encode(const CliOptions &cli) {
        std::cout << "Input: " << cli.input_path << "\n";
        std::cout << "Output: " << cli.output_path << "\n";

        ms_encode_options_t opts{};
        opts.input_path = cli.input_path.c_str();
        opts.output_path = cli.output_path.c_str();
        opts.encrypt = cli.encrypt ? 1 : 0;
        opts.password = cli.password.c_str();
        opts.password_len = cli.password.size();
        opts.hash_algorithm = cli.hash_algorithm;
        opts.progress = encode_progress;

        ms_result_t result{};
        if (const ms_status_t status = ms_encode(&opts, &result); status != MS_OK) {
            return report_failure(status);
        }

        std::cout << "\n\nEncode complete: " << format_size(result.input_size) << " -> "
                << format_size(result.output_size) << "\n";
        print_stats(result);
        std::cout << "Written to: " << cli.output_path << "\n";
        return 0;
    }

    int do_decode(const CliOptions &cli) {
        std::cout << "Input: " << cli.input_path << "\n";
        std::cout << "Output: " << cli.output_path << "\n";

        ms_decode_options_t opts{};
        opts.input_path = cli.input_path.c_str();
        opts.output_path = cli.output_path.c_str();
        opts.password = cli.password.c_str();
        opts.password_len = cli.password.size();
        opts.progress = decode_progress;

        ms_result_t result{};
        if (const ms_status_t status = ms_decode(&opts, &result); status != MS_OK) {
            return report_failure(status);
        }

        std::cout << "\n\nDecode complete: " << format_size(result.input_size) << " -> "
                << format_size(result.output_size) << "\n";
        print_stats(result);
        std::cout << "Written to: " << cli.output_path << "\n";
        return 0;
    }

    int do_stream_encode(const CliOptions &cli) {
        std::cout << "Input: " << cli.input_path << "\n";
        std::cout << "Stream URL: " << cli.stream_url << "\n";
        std::cout << "Resolution: " << cli.width << "x" << cli.height << " (" << cli.fps << " fps)\n";
        std::cout << "Bitrate: " << cli.bitrate_kbps << " kbps\n";

        ms_stream_encode_options_t opts{};
        opts.input_path = cli.input_path.c_str();
        opts.stream_url = cli.stream_url.c_str();
        opts.encrypt = cli.encrypt ? 1 : 0;
        opts.password = cli.password.c_str();
        opts.password_len = cli.password.size();
        opts.hash_algorithm = cli.hash_algorithm;
        opts.bitrate_kbps = cli.bitrate_kbps;
        opts.width = cli.width;
        opts.height = cli.height;
        opts.fps = cli.fps;
        opts.progress = stream_encode_progress;

        ms_result_t result{};
        if (const ms_status_t status = ms_stream_encode(&opts, &result); status != MS_OK) {
            return report_failure(status);
        }

        std::cout << "\n\nStream encode complete: " << format_size(result.input_size) << "\n";
        print_stats(result);
        return 0;
    }

    int do_stream_decode(const CliOptions &cli) {
        std::cout << "Stream URL: " << cli.stream_url << "\n";
        std::cout << "Output: " << cli.output_path << "\n";
        std::cout << "Waiting for stream...\n";

        ms_stream_decode_options_t opts{};
        opts.stream_url = cli.stream_url.c_str();
        opts.output_path = cli.output_path.c_str();
        opts.password = cli.password.c_str();
        opts.password_len = cli.password.size();
        opts.timeout_sec = STREAM_DECODE_TIMEOUT_SEC;
        opts.progress = stream_decode_progress;

        ms_result_t result{};
        if (const ms_status_t status = ms_stream_decode(&opts, &result); status != MS_OK) {
            return report_failure(status);
        }

        std::cout << "\n\nStream decode complete: -> " << format_size(result.output_size) << "\n";
        print_stats(result);
        std::cout << "Written to: " << cli.output_path << "\n";
        return 0;
    }

    std::optional<int> parse_positive_int(const std::string_view text) {
        int value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size() || value <= 0) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<CliOptions> parse_arguments(const int argc, char *argv[]) {
        CliOptions cli;
        cli.command = argv[1];

        const auto take_value = [&](int &i) -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string_view(argv[++i]);
        };

        const auto take_positive_int = [&](int &i, const std::string_view flag) -> std::optional<int> {
            const auto text = take_value(i);
            if (!text) return std::nullopt;
            const auto value = parse_positive_int(*text);
            if (!value) {
                std::cerr << "Error: " << flag << " expects a positive integer, got '" << *text << "'\n";
            }
            return value;
        };

        for (int i = 2; i < argc; ++i) {
            const std::string_view arg = argv[i];
            std::optional<std::string_view> value;
            std::optional<int> number;

            if ((arg == "--input" || arg == "-i") && (value = take_value(i))) {
                cli.input_path = *value;
            } else if ((arg == "--output" || arg == "-o") && (value = take_value(i))) {
                cli.output_path = *value;
            } else if ((arg == "--url" || arg == "-u") && (value = take_value(i))) {
                cli.stream_url = *value;
            } else if ((arg == "--bitrate" || arg == "-b") && (number = take_positive_int(i, arg))) {
                cli.bitrate_kbps = *number;
            } else if (arg == "--width" && (number = take_positive_int(i, arg))) {
                cli.width = *number;
            } else if (arg == "--height" && (number = take_positive_int(i, arg))) {
                cli.height = *number;
            } else if (arg == "--fps" && (number = take_positive_int(i, arg))) {
                cli.fps = *number;
            } else if (arg == "--encrypt" || arg == "-e") {
                cli.encrypt = true;
            } else if ((arg == "--password" || arg == "-p") && (value = take_value(i))) {
                cli.password = *value;
            } else if ((arg == "--hash" || arg == "-H") && (value = take_value(i))) {
                if (*value == "xxhash") {
                    cli.hash_algorithm = MS_HASH_XXHASH32;
                } else if (*value == "crc32") {
                    cli.hash_algorithm = MS_HASH_CRC32;
                } else {
                    std::cerr << "Error: unknown hash algorithm '" << *value << "' (use crc32 or xxhash)\n";
                    return std::nullopt;
                }
            } else {
                std::cerr << "Error: unknown or incomplete argument '" << arg << "'\n";
                print_usage(argv[0]);
                return std::nullopt;
            }
        }
        return cli;
    }

    bool require(const bool condition, const char *message, const char *program) {
        if (!condition) {
            std::cerr << "Error: " << message << "\n";
            print_usage(program);
        }
        return condition;
    }
}

int main(const int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string_view command = argv[1];
    if (command != "encode" && command != "decode" && command != "stream-encode" && command != "stream-decode") {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage(argv[0]);
        return 1;
    }

    const auto cli = parse_arguments(argc, argv);
    if (!cli) {
        return 1;
    }

    const bool encrypt_ok = !cli->encrypt || !cli->password.empty();

    if (command == "encode") {
        if (!require(!cli->input_path.empty() && !cli->output_path.empty(),
                     "both --input and --output must be specified", argv[0]) ||
            !require(encrypt_ok, "--encrypt requires --password", argv[0])) {
            return 1;
        }
        return do_encode(*cli);
    }
    if (command == "decode") {
        if (!require(!cli->input_path.empty() && !cli->output_path.empty(),
                     "both --input and --output must be specified", argv[0])) {
            return 1;
        }
        return do_decode(*cli);
    }
    if (command == "stream-encode") {
        if (!require(!cli->input_path.empty() && !cli->stream_url.empty(),
                     "--input and --url must be specified for stream-encode", argv[0]) ||
            !require(encrypt_ok, "--encrypt requires --password", argv[0])) {
            return 1;
        }
        return do_stream_encode(*cli);
    }
    if (!require(!cli->stream_url.empty() && !cli->output_path.empty(),
                 "--url and --output must be specified for stream-decode", argv[0])) {
        return 1;
    }
    return do_stream_decode(*cli);
}
