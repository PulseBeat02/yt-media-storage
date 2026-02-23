// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) Brandon Li <https://brandonli.me/>
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

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "media_storage.h"

static std::string format_size(const uint64_t bytes) {
    const char *units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    auto size = static_cast<double>(bytes);
    while (size >= 1024 && unit < 3) {
        size /= 1024;
        ++unit;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
    return oss.str();
}

static int encode_progress(const uint64_t current, const uint64_t total, void *) {
    if (total > 0) {
        std::cout << "\rEncoding chunk " << (current + 1) << "/" << total << "..." << std::flush;
    }
    return 0;
}

static int decode_progress(const uint64_t current, const uint64_t total, void *) {
    if (total > 0) {
        std::cout << "\rDecoding frame " << current << "/" << total << "..." << std::flush;
    }
    return 0;
}

static void print_usage(const char *program) {
    std::cerr << "Usage:\n"
            << "  " << program <<
            " encode --input <file> --output <video> [--encrypt --password <pwd>] [--hash <crc32|xxhash>]\n"
            << "  " << program << " decode --input <video> --output <file> [--password <pwd>]\n";
}

static int do_encode(const std::string &input_path, const std::string &output_path,
                     const bool encrypt, const std::string &password,
                     const ms_hash_algorithm_t hash_algo) {
    std::cout << "Input: " << input_path << "\n";
    std::cout << "Output: " << output_path << "\n";

    ms_encode_options_t opts{};
    opts.input_path = input_path.c_str();
    opts.output_path = output_path.c_str();
    opts.encrypt = encrypt ? 1 : 0;
    opts.password = password.c_str();
    opts.password_len = password.size();
    opts.hash_algorithm = hash_algo;
    opts.progress = encode_progress;
    opts.progress_user = nullptr;

    ms_result_t result{};
    if (const ms_status_t status = ms_encode(&opts, &result); status != MS_OK) {
        std::cout << "\n";
        std::cerr << "Error: " << ms_status_string(status) << "\n";
        return 1;
    }

    std::cout << "\n\nEncode complete: " << format_size(result.input_size) << " -> "
            << format_size(result.output_size) << "\n";
    std::cout << "Chunks: " << result.total_chunks
            << "  Packets: " << result.total_packets
            << "  Frames: " << result.total_frames << "\n";
    std::cout << "Written to: " << output_path << "\n";

    return 0;
}

static int do_decode(const std::string &input_path, const std::string &output_path,
                     const std::string &password) {
    std::cout << "Input: " << input_path << "\n";
    std::cout << "Output: " << output_path << "\n";

    ms_decode_options_t opts{};
    opts.input_path = input_path.c_str();
    opts.output_path = output_path.c_str();
    opts.password = password.c_str();
    opts.password_len = password.size();
    opts.progress = decode_progress;
    opts.progress_user = nullptr;

    ms_result_t result{};
    if (const ms_status_t status = ms_decode(&opts, &result); status != MS_OK) {
        std::cout << "\n";
        std::cerr << "Error: " << ms_status_string(status) << "\n";
        return 1;
    }

    std::cout << "\n\nDecode complete: " << format_size(result.input_size) << " -> "
            << format_size(result.output_size) << "\n";
    std::cout << "Chunks: " << result.total_chunks
            << "  Packets: " << result.total_packets
            << "  Frames: " << result.total_frames << "\n";
    std::cout << "Written to: " << output_path << "\n";

    return 0;
}

int main(const int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string command = argv[1];

    if (command != "encode" && command != "decode") {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path;
    std::string output_path;
    bool encrypt = false;
    std::string password;
    auto hash_algo = MS_HASH_CRC32;

    for (int i = 2; i < argc; ++i) {
        if (const std::string arg = argv[i]; (arg == "--input" || arg == "-i") && i + 1 < argc) {
            input_path = argv[++i];
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            output_path = argv[++i];
        } else if ((arg == "--encrypt" || arg == "-e")) {
            encrypt = true;
        } else if ((arg == "--password" || arg == "-p") && i + 1 < argc) {
            password = argv[++i];
        } else if ((arg == "--hash" || arg == "-H") && i + 1 < argc) {
            if (const std::string algo_str = argv[++i]; algo_str == "xxhash") {
                hash_algo = MS_HASH_XXHASH32;
            } else if (algo_str == "crc32") {
                hash_algo = MS_HASH_CRC32;
            } else {
                std::cerr << "Error: unknown hash algorithm '" << algo_str << "' (use crc32 or xxhash)\n";
                return 1;
            }
        } else {
            std::cerr << "Error: unknown or incomplete argument '" << arg << "'\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty() || output_path.empty()) {
        std::cerr << "Error: both --input and --output must be specified\n";
        print_usage(argv[0]);
        return 1;
    }

    if (encrypt && password.empty()) {
        std::cerr << "Error: --encrypt requires --password\n";
        return 1;
    }

    if (command == "encode") {
        return do_encode(input_path, output_path, encrypt, password, hash_algo);
    } else {
        return do_decode(input_path, output_path, password);
    }
}
