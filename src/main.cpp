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

//
// Created by brand on 2/5/2026.
//

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoder.h"
#include "integrity.h"
#include "video_encoder.h"
#include "video_decoder.h"

static std::string format_size(const std::uintmax_t bytes) {
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

static std::array<std::byte, 16> make_file_id() {
    std::array<std::byte, 16> id{};
    for (int i = 0; i < 16; ++i) {
        id[i] = static_cast<std::byte>(i);
    }
    return id;
}

static void print_usage(const char *program) {
    std::cerr << "Usage:\n"
            << "  " << program << " encode --input <file> --output <video> [--encrypt --password <pwd>] [--hash <crc32|xxhash>]\n"
            << "  " << program << " decode --input <video> --output <file> [--password <pwd>]\n";
}

static int do_encode(const std::string &input_path, const std::string &output_path,
                     bool encrypt, const std::string &password, HashAlgorithm hash_algo) {
    if (!std::filesystem::exists(input_path)) {
        std::cerr << "Error: input file not found: " << input_path << "\n";
        return 1;
    }

    const auto input_size = std::filesystem::file_size(input_path);
    std::cout << "Input: " << input_path << " (" << format_size(input_size) << ")\n";

    const std::size_t chunk_size = encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
    const auto chunked = chunkFile(input_path.c_str(), chunk_size);
    const std::size_t num_chunks = chunked.chunks.size();
    std::cout << "Chunks: " << num_chunks << "\n";

    const auto file_id = make_file_id();
    const Encoder encoder(file_id, hash_algo);
    std::vector<std::vector<Packet> > all_chunk_packets(num_chunks);

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypt) {
        const std::span pw(reinterpret_cast<const std::byte *>(password.data()),
                           password.size());
        key = derive_key(pw, file_id);
    }

#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < static_cast<int>(num_chunks); ++i) {
        auto chunk_data = chunkSpan(chunked, static_cast<std::size_t>(i));
        std::span<const std::byte> data_to_encode = chunk_data;
        std::vector<std::byte> encrypted_buf;
        if (encrypt) {
            encrypted_buf = encrypt_chunk(chunk_data, key, file_id, static_cast<uint32_t>(i));
            data_to_encode = encrypted_buf;
        }
        const bool is_last = (i == static_cast<int>(num_chunks) - 1);
        auto [chunk_packets, manifest] =
                encoder.encode_chunk(static_cast<uint32_t>(i), data_to_encode, is_last, encrypt);
        all_chunk_packets[i] = std::move(chunk_packets);
    }

    std::size_t total_packets = 0;
    for (const auto &packets: all_chunk_packets)
        total_packets += packets.size();
    std::cout << "Packets: " << total_packets << "\n";

    try {
        VideoEncoder video_encoder(output_path);
        for (auto &packets: all_chunk_packets) {
            video_encoder.encode_packets(packets);
            packets.clear();
            packets.shrink_to_fit();
        }
        video_encoder.finalize();
    } catch (const std::exception &e) {
        if (encrypt) {
            secure_zero(std::span<std::byte>(key));
        }
        std::cerr << "Error writing video: " << e.what() << "\n";
        return 1;
    }

    if (encrypt) {
        secure_zero(std::span<std::byte>(key));
    }

    const auto video_size = std::filesystem::file_size(output_path);
    std::cout << "\nEncode complete: " << format_size(input_size) << " -> "
            << format_size(video_size) << "\n";
    std::cout << "Written to: " << output_path << "\n";

    return 0;
}

static int do_decode(const std::string &input_path, const std::string &output_path,
                     const std::string &password) {
    if (!std::filesystem::exists(input_path)) {
        std::cerr << "Error: input video not found: " << input_path << "\n";
        return 1;
    }

    const auto video_size = std::filesystem::file_size(input_path);
    std::cout << "Input: " << input_path << " (" << format_size(video_size) << ")\n";

    Decoder decoder;
    std::size_t total_extracted = 0;
    std::size_t decoded_chunks = 0;
    uint32_t max_chunk_index = 0;
    bool found_last_chunk = false;
    uint32_t last_chunk_index = 0;

    try {
        VideoDecoder video_decoder(input_path);
        const int64_t total = video_decoder.total_frames();
        std::cout << "Total frames: "
                << (total >= 0 ? std::to_string(total) : "unknown") << "\n";

        std::size_t valid_frames = 0;

        while (!video_decoder.is_eof()) {
            if (auto frame_packets = video_decoder.decode_next_frame(); !frame_packets.empty()) {
                ++valid_frames;
                for (auto &pkt_data: frame_packets) {
                    ++total_extracted;

                    if (pkt_data.size() >= HEADER_SIZE) {
                        const auto flags =
                                static_cast<uint8_t>(pkt_data[FLAGS_OFF]);
                        uint32_t chunk_idx = 0;
                        std::memcpy(&chunk_idx,
                                    pkt_data.data() + CHUNK_INDEX_OFF,
                                    sizeof(chunk_idx));
                        if (chunk_idx > max_chunk_index)
                            max_chunk_index = chunk_idx;
                        if (flags & LastChunk) {
                            found_last_chunk = true;
                            last_chunk_index = chunk_idx;
                        }
                    }

                    const std::span<const std::byte> data(pkt_data.data(), pkt_data.size());
                    if (auto result = decoder.process_packet(data);
                        result && result->success) {
                        ++decoded_chunks;
                    }
                }
            }
        }

        std::cout << "Valid frames: " << valid_frames << "\n";
        std::cout << "Packets extracted: " << total_extracted << "\n";
    } catch (const std::exception &e) {
        std::cerr << "Error reading video: " << e.what() << "\n";
        return 1;
    }

    if (total_extracted == 0) {
        std::cerr << "No packets could be extracted from the video\n";
        return 1;
    }

    uint32_t expected_chunks;
    if (found_last_chunk) {
        expected_chunks = last_chunk_index + 1;
    } else {
        expected_chunks = max_chunk_index + 1;
    }

    std::cout << "Chunks decoded: " << decoded_chunks << "/" << expected_chunks << "\n";

    if (decoded_chunks < expected_chunks) {
        std::cerr << "Error: only decoded " << decoded_chunks << " of "
                << expected_chunks << " chunks\n";
        return 1;
    }

    if (decoder.is_encrypted()) {
        if (password.empty()) {
            std::cerr << "Error: content is encrypted, password required (use --password)\n";
            return 1;
        }
        const std::span<const std::byte> pw(reinterpret_cast<const std::byte *>(password.data()),
                                            password.size());
        auto key = derive_key(pw, *decoder.file_id());
        decoder.set_decrypt_key(key);
        secure_zero(std::span<std::byte>(key));
    }

    auto assembled = decoder.assemble_file(expected_chunks);
    if (!assembled) {
        if (decoder.is_encrypted()) {
            decoder.clear_decrypt_key();
        }
        std::cerr << "Error: failed to assemble file from decoded chunks "
                << "(wrong password or corrupted data)\n";
        return 1;
    }

    if (decoder.is_encrypted()) {
        decoder.clear_decrypt_key();
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: could not open " << output_path << " for writing\n";
        return 1;
    }

    out.write(reinterpret_cast<const char *>(assembled->data()),
              static_cast<std::streamsize>(assembled->size()));
    out.close();

    std::cout << "\nDecode complete: " << format_size(video_size) << " -> "
            << format_size(assembled->size()) << "\n";
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
    auto hash_algo = HashAlgorithm::CRC32;

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
                hash_algo = HashAlgorithm::XXHash32;
            } else if (algo_str == "crc32") {
                hash_algo = HashAlgorithm::CRC32;
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
