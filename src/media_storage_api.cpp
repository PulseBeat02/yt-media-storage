/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "media_storage.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoder.h"
#include "video_decoder.h"
#include "video_encoder.h"

static std::array<std::byte, 16> make_file_id() {
    std::array<std::byte, 16> id{};
    for (int i = 0; i < 16; ++i) {
        id[i] = static_cast<std::byte>(i);
    }
    return id;
}

static HashAlgorithm to_internal_hash(const ms_hash_algorithm_t algo) {
    switch (algo) {
        case MS_HASH_XXHASH32: return HashAlgorithm::XXHash32;
        default: return HashAlgorithm::CRC32;
    }
}

ms_status_t ms_encode(const ms_encode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }
    if (options->encrypt && (!options->password || options->password_len == 0)) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string input_path(options->input_path);
    const std::string output_path(options->output_path);

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    const auto input_size = std::filesystem::file_size(input_path);
    const bool encrypt = options->encrypt != 0;
    const std::size_t chunk_size = encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
    const FileChunkReader reader(input_path.c_str(), chunk_size);
    const std::size_t num_chunks = reader.num_chunks();

    const auto file_id = make_file_id();
    const Encoder encoder(file_id, to_internal_hash(options->hash_algorithm));

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypt) {
        const std::span pw(reinterpret_cast<const std::byte *>(options->password),
                           options->password_len);
        key = derive_key(pw, file_id);
    }

    std::size_t total_packets = 0;
    int64_t total_frames = 0;

    try {
        VideoEncoder video_encoder(output_path);

        for (std::size_t i = 0; i < num_chunks; ++i) {
            if (options->progress) {
                if (options->progress(static_cast<uint64_t>(i),
                                      static_cast<uint64_t>(num_chunks),
                                      options->progress_user) != 0) {
                    if (encrypt) secure_zero(std::span<std::byte>(key));
                    return MS_ERR_ENCODE_FAILED;
                }
            }

            auto chunk_data = reader.read_chunk(i);
            std::span<const std::byte> data_to_encode(chunk_data);
            std::vector<std::byte> encrypted_buf;
            if (encrypt) {
                encrypted_buf = encrypt_chunk(data_to_encode, key, file_id,
                                              static_cast<uint32_t>(i));
                data_to_encode = encrypted_buf;
            }
            const bool is_last = (i == num_chunks - 1);
            auto [chunk_packets, manifest] =
                encoder.encode_chunk(static_cast<uint32_t>(i), data_to_encode,
                                     is_last, encrypt);
            total_packets += chunk_packets.size();
            video_encoder.encode_packets(chunk_packets);
        }

        video_encoder.finalize();
        total_frames = video_encoder.frames_written();
    } catch (...) {
        if (encrypt) secure_zero(std::span<std::byte>(key));
        return MS_ERR_ENCODE_FAILED;
    }

    if (encrypt) secure_zero(std::span<std::byte>(key));

    if (result) {
        result->input_size = input_size;
        result->output_size = std::filesystem::file_size(output_path);
        result->total_chunks = num_chunks;
        result->total_packets = total_packets;
        result->total_frames = static_cast<uint64_t>(total_frames);
    }

    return MS_OK;
}

ms_status_t ms_decode(const ms_decode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string input_path(options->input_path);
    const std::string output_path(options->output_path);

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    const auto video_size = std::filesystem::file_size(input_path);

    Decoder decoder;
    std::size_t total_extracted = 0;
    std::size_t decoded_chunks = 0;
    uint32_t max_chunk_index = 0;
    bool found_last_chunk = false;
    uint32_t last_chunk_index = 0;
    int64_t total_frames_read = 0;

    try {
        VideoDecoder video_decoder(input_path);
        const int64_t total = video_decoder.total_frames();

        while (!video_decoder.is_eof()) {
            if (options->progress) {
                const uint64_t cur = static_cast<uint64_t>(video_decoder.frames_read());
                const uint64_t tot = total >= 0 ? static_cast<uint64_t>(total) : 0;
                if (options->progress(cur, tot, options->progress_user) != 0) {
                    return MS_ERR_DECODE_FAILED;
                }
            }

            auto frame_packets = video_decoder.decode_next_frame();
            if (frame_packets.empty()) continue;

            for (auto &pkt_data : frame_packets) {
                ++total_extracted;

                if (pkt_data.size() >= HEADER_SIZE) {
                    const auto flags = static_cast<uint8_t>(pkt_data[FLAGS_OFF]);
                    uint32_t chunk_idx = 0;
                    std::memcpy(&chunk_idx, pkt_data.data() + CHUNK_INDEX_OFF,
                                sizeof(chunk_idx));
                    if (chunk_idx > max_chunk_index) max_chunk_index = chunk_idx;
                    if (flags & LastChunk) {
                        found_last_chunk = true;
                        last_chunk_index = chunk_idx;
                    }
                }

                const std::span<const std::byte> data(pkt_data.data(), pkt_data.size());
                if (auto res = decoder.process_packet(data);
                    res && res->success) {
                    ++decoded_chunks;
                }
            }
        }

        total_frames_read = video_decoder.frames_read();
    } catch (...) {
        return MS_ERR_DECODE_FAILED;
    }

    if (total_extracted == 0) {
        return MS_ERR_DECODE_FAILED;
    }

    const uint32_t expected_chunks = found_last_chunk
        ? last_chunk_index + 1
        : max_chunk_index + 1;

    if (decoded_chunks < expected_chunks) {
        return MS_ERR_INCOMPLETE;
    }

    if (decoder.is_encrypted()) {
        if (!options->password || options->password_len == 0) {
            return MS_ERR_CRYPTO;
        }
        const std::span<const std::byte> pw(
            reinterpret_cast<const std::byte *>(options->password),
            options->password_len);
        auto key = derive_key(pw, *decoder.file_id());
        decoder.set_decrypt_key(key);
        secure_zero(std::span<std::byte>(key));
    }

    if (!decoder.write_assembled_file(output_path, expected_chunks)) {
        if (decoder.is_encrypted()) decoder.clear_decrypt_key();
        return MS_ERR_DECODE_FAILED;
    }

    if (decoder.is_encrypted()) decoder.clear_decrypt_key();

    if (result) {
        result->input_size = video_size;
        result->output_size = std::filesystem::file_size(output_path);
        result->total_chunks = expected_chunks;
        result->total_packets = total_extracted;
        result->total_frames = static_cast<uint64_t>(total_frames_read);
    }

    return MS_OK;
}

const char *ms_status_string(const ms_status_t status) {
    switch (status) {
        case MS_OK:              return "success";
        case MS_ERR_INVALID_ARGS: return "invalid arguments";
        case MS_ERR_FILE_NOT_FOUND: return "file not found";
        case MS_ERR_IO:          return "I/O error";
        case MS_ERR_ENCODE_FAILED: return "encoding failed";
        case MS_ERR_DECODE_FAILED: return "decoding failed";
        case MS_ERR_CRYPTO:      return "encryption/decryption error";
        case MS_ERR_INCOMPLETE:  return "incomplete data";
        default:                 return "unknown error";
    }
}

const char *ms_version(void) {
    return "1.0.0";
}
