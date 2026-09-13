/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <omp.h>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoder.h"
#include "packet_format.h"
#include "stream.h"
#include "video_decoder.h"
#include "video_encoder.h"

namespace {
    constexpr int DEFAULT_STREAM_CONNECT_TIMEOUT_SEC = 30;

    thread_local std::string last_error;

    void set_last_error(std::string message) {
        last_error = std::move(message);
    }

    ms_status_t fail(const ms_status_t status, std::string message) {
        set_last_error(std::move(message));
        return status;
    }

    std::string describe_current_exception() {
        try {
            throw;
        } catch (const std::exception &e) {
            return e.what();
        } catch (...) {
            return "unknown exception";
        }
    }

    HashAlgorithm to_internal_hash(const ms_hash_algorithm_t algo) {
        return algo == MS_HASH_XXHASH32 ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32;
    }

    std::span<const std::byte> password_span(const char *password, const std::size_t length) {
        return {reinterpret_cast<const std::byte *>(password), length};
    }

    class ScopedKey {
    public:
        ScopedKey() = default;

        ScopedKey(const ScopedKey &) = delete;

        ScopedKey &operator=(const ScopedKey &) = delete;

        ~ScopedKey() { secure_zero(key_); }

        void derive(const std::span<const std::byte> password, const FileId &file_id) {
            key_ = derive_key(password, file_id);
        }

        [[nodiscard]] const CryptoKey &get() const { return key_; }

    private:
        CryptoKey key_{};
    };

    struct EncodeStats {
        std::size_t total_chunks = 0;
        std::size_t total_packets = 0;
        int64_t total_frames = 0;
    };

    struct EncodeJob {
        const FileChunkReader &reader;
        const Encoder &encoder;
        bool encrypt;
        const CryptoKey &key;
        ms_progress_fn progress;
        void *progress_user;

        using BatchResults = std::vector<std::vector<Packet> >;

        [[nodiscard]] BatchResults encode_batch(const std::size_t batch_start, const int batch_count) const {
            BatchResults results(batch_count);
            std::atomic<bool> batch_error{false};
            const std::size_t num_chunks = reader.num_chunks();
            const FileId &file_id = encoder.file_id();

#pragma omp parallel for schedule(dynamic)
            for (int j = 0; j < batch_count; ++j) {
                if (batch_error) continue;
                try {
                    const std::size_t i = batch_start + j;
                    std::span<const std::byte> data = reader.chunk_view(i);
                    std::vector<std::byte> encrypted;
                    if (encrypt) {
                        encrypted = encrypt_chunk(data, key, file_id, static_cast<uint32_t>(i));
                        data = encrypted;
                    }
                    results[j] = encoder.encode_chunk(static_cast<uint32_t>(i), data, i == num_chunks - 1, encrypt).first;
                } catch (...) {
                    batch_error = true;
                }
            }

            if (batch_error) {
                throw std::runtime_error("batch FEC encoding failed");
            }
            return results;
        }

        template<typename FrameSink>
        [[nodiscard]] ms_status_t run(FrameSink &sink, EncodeStats &stats) const {
            const std::size_t num_chunks = reader.num_chunks();
            const auto batch_size = static_cast<std::size_t>(std::max(1, omp_get_max_threads()));
            const auto batch_count_at = [&](const std::size_t start) {
                return static_cast<int>(std::min(start + batch_size, num_chunks) - start);
            };

            std::future<BatchResults> pending = std::async(
                std::launch::async, [this, batch_count_at] { return encode_batch(0, batch_count_at(0)); });

            for (std::size_t batch_start = 0; batch_start < num_chunks; batch_start += batch_size) {
                if (progress && progress(batch_start, num_chunks, progress_user) != 0) {
                    pending.wait();
                    return fail(MS_ERR_ENCODE_FAILED, "cancelled by progress callback");
                }

                BatchResults results = pending.get();

                if (const std::size_t next_start = batch_start + batch_size; next_start < num_chunks) {
                    pending = std::async(std::launch::async, [this, next_start, batch_count_at] {
                        return encode_batch(next_start, batch_count_at(next_start));
                    });
                }

                for (const auto &packets: results) {
                    stats.total_packets += packets.size();
                    sink.encode_packets(packets);
                }
            }

            sink.finalize();
            stats.total_chunks = num_chunks;
            stats.total_frames = sink.frames_written();
            return MS_OK;
        }
    };

    struct DecodeStats {
        std::size_t total_packets = 0;
        std::size_t decoded_chunks = 0;
        uint32_t expected_chunks = 0;
        int64_t total_frames = 0;
    };

    ms_status_t consume_video(VideoDecoder &video, Decoder &decoder, const ms_progress_fn progress,
                              void *progress_user, DecodeStats &stats) {
        const int64_t total = video.total_frames();
        const uint64_t total_for_progress = total > 0 ? static_cast<uint64_t>(total) : 0;

        while (!video.is_eof()) {
            if (const auto last = decoder.last_chunk_index();
                last && stats.decoded_chunks >= static_cast<std::size_t>(*last) + 1) {
                break;
            }
            if (progress && progress(static_cast<uint64_t>(video.frames_read()), total_for_progress,
                                     progress_user) != 0) {
                return fail(MS_ERR_DECODE_FAILED, "cancelled by progress callback");
            }

            for (const auto &packet_bytes: video.decode_next_frame()) {
                ++stats.total_packets;
                if (const auto res = decoder.process_packet(packet_bytes, false); res && res->success) {
                    ++stats.decoded_chunks;
                }
            }
        }

        stats.total_frames = video.frames_read();
        stats.expected_chunks = decoder.last_chunk_index().value_or(decoder.max_chunk_index()) + 1;
        return MS_OK;
    }

    ms_status_t write_decoded_file(Decoder &decoder, const std::string &output_path, const DecodeStats &stats,
                                   const char *password, const std::size_t password_len) {
        if (stats.total_packets == 0) {
            return fail(MS_ERR_DECODE_FAILED, "no packets found in input");
        }
        if (stats.decoded_chunks < stats.expected_chunks) {
            return fail(MS_ERR_INCOMPLETE, "decoded " + std::to_string(stats.decoded_chunks) + " of " +
                                           std::to_string(stats.expected_chunks) + " chunks");
        }

        if (decoder.is_encrypted()) {
            if (!password || password_len == 0) {
                return fail(MS_ERR_CRYPTO, "input is encrypted but no password was given");
            }
            ScopedKey key;
            key.derive(password_span(password, password_len), *decoder.file_id());
            decoder.set_decrypt_key(key.get());
        }

        const bool written = decoder.write_assembled_file(output_path, stats.expected_chunks);
        decoder.clear_decrypt_key();
        if (!written) {
            return fail(decoder.is_encrypted() ? MS_ERR_CRYPTO : MS_ERR_IO,
                        decoder.is_encrypted()
                            ? "failed to decrypt or write output (wrong password?)"
                            : "failed to write output file");
        }
        return MS_OK;
    }

    void fill_result(ms_result_t *result, const uint64_t input_size, const uint64_t output_size,
                     const uint64_t total_chunks, const uint64_t total_packets, const uint64_t total_frames) {
        if (!result) return;
        result->input_size = input_size;
        result->output_size = output_size;
        result->total_chunks = total_chunks;
        result->total_packets = total_packets;
        result->total_frames = total_frames;
    }

    uint64_t file_size_or_zero(const std::string &path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        return ec ? 0 : static_cast<uint64_t>(size);
    }

    template<typename MakeSink>
    ms_status_t encode_file(const char *input_path_c, const bool encrypt, const char *password,
                            const std::size_t password_len, const ms_hash_algorithm_t hash_algorithm,
                            const ms_progress_fn progress, void *progress_user,
                            MakeSink &&make_sink, EncodeStats &stats, uint64_t &input_size) {
        if (encrypt && (!password || password_len == 0)) {
            return fail(MS_ERR_INVALID_ARGS, "encryption requires a password");
        }

        const std::string input_path(input_path_c);
        if (!std::filesystem::exists(input_path)) {
            return fail(MS_ERR_FILE_NOT_FOUND, "input file not found: " + input_path);
        }
        if (std::error_code ec; !std::filesystem::is_regular_file(input_path, ec)) {
            return fail(MS_ERR_INVALID_ARGS, "input is not a regular file: " + input_path);
        }
        input_size = file_size_or_zero(input_path);

        try {
            const FileChunkReader reader(input_path.c_str(), encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0);
            if (reader.file_size() > 0 && !reader.is_mapped()) {
                return fail(MS_ERR_IO, "failed to memory-map input file: " + input_path);
            }

            const Encoder encoder(generate_file_id(), to_internal_hash(hash_algorithm));
            ScopedKey key;
            if (encrypt) {
                key.derive(password_span(password, password_len), encoder.file_id());
            }

            const EncodeJob job{reader, encoder, encrypt, key.get(), progress, progress_user};
            auto sink = make_sink();
            return job.run(*sink, stats);
        } catch (...) {
            return fail(MS_ERR_ENCODE_FAILED, describe_current_exception());
        }
    }
}

ms_status_t ms_encode(const ms_encode_options_t *options, ms_result_t *result) {
    set_last_error({});
    if (!options || !options->input_path || !options->output_path) {
        return fail(MS_ERR_INVALID_ARGS, "input_path and output_path are required");
    }

    const std::string output_path(options->output_path);
    EncodeStats stats;
    uint64_t input_size = 0;
    const ms_status_t status = encode_file(
        options->input_path, options->encrypt != 0, options->password, options->password_len,
        options->hash_algorithm, options->progress, options->progress_user,
        [&] { return std::make_unique<VideoEncoder>(output_path); },
        stats, input_size);
    if (status != MS_OK) {
        return status;
    }

    fill_result(result, input_size, file_size_or_zero(output_path), stats.total_chunks, stats.total_packets,
                static_cast<uint64_t>(stats.total_frames));
    return MS_OK;
}

ms_status_t ms_stream_encode(const ms_stream_encode_options_t *options, ms_result_t *result) {
    set_last_error({});
    if (!options || !options->input_path || !options->stream_url) {
        return fail(MS_ERR_INVALID_ARGS, "input_path and stream_url are required");
    }

    const std::string stream_url(options->stream_url);
    const int bitrate = options->bitrate_kbps > 0 ? options->bitrate_kbps : FRAME_BITRATE;
    const int width = options->width > 0 ? options->width : FRAME_WIDTH_STREAM;
    const int height = options->height > 0 ? options->height : FRAME_HEIGHT_STREAM;
    const int fps = options->fps > 0 ? options->fps : FRAME_FPS;

    EncodeStats stats;
    uint64_t input_size = 0;
    const ms_status_t status = encode_file(
        options->input_path, options->encrypt != 0, options->password, options->password_len,
        options->hash_algorithm, options->progress, options->progress_user,
        [&] { return std::make_unique<StreamEncoder>(stream_url, bitrate, width, height, fps); },
        stats, input_size);
    if (status != MS_OK) {
        return status;
    }

    fill_result(result, input_size, 0, stats.total_chunks, stats.total_packets,
                static_cast<uint64_t>(stats.total_frames));
    return MS_OK;
}

ms_status_t ms_decode(const ms_decode_options_t *options, ms_result_t *result) {
    set_last_error({});
    if (!options || !options->input_path || !options->output_path) {
        return fail(MS_ERR_INVALID_ARGS, "input_path and output_path are required");
    }

    const std::string input_path(options->input_path);
    const std::string output_path(options->output_path);
    if (!std::filesystem::exists(input_path)) {
        return fail(MS_ERR_FILE_NOT_FOUND, "input file not found: " + input_path);
    }

    Decoder decoder;
    DecodeStats stats;
    try {
        VideoDecoder video(input_path);
        if (const ms_status_t status = consume_video(video, decoder, options->progress, options->progress_user,
                                                     stats); status != MS_OK) {
            return status;
        }
    } catch (...) {
        return fail(MS_ERR_DECODE_FAILED, describe_current_exception());
    }

    if (const ms_status_t status = write_decoded_file(decoder, output_path, stats, options->password,
                                                      options->password_len); status != MS_OK) {
        return status;
    }

    fill_result(result, file_size_or_zero(input_path), file_size_or_zero(output_path), stats.expected_chunks,
                stats.total_packets, static_cast<uint64_t>(stats.total_frames));
    return MS_OK;
}

ms_status_t ms_stream_decode(const ms_stream_decode_options_t *options, ms_result_t *result) {
    set_last_error({});
    if (!options || !options->stream_url || !options->output_path) {
        return fail(MS_ERR_INVALID_ARGS, "stream_url and output_path are required");
    }

    const std::string stream_url(options->stream_url);
    const std::string output_path(options->output_path);
    const int max_attempts = options->timeout_sec > 0 ? options->timeout_sec : DEFAULT_STREAM_CONNECT_TIMEOUT_SEC;

    Decoder decoder;
    DecodeStats stats;
    try {
        std::unique_ptr<VideoDecoder> video;
        for (int attempt = 1; !video; ++attempt) {
            try {
                video = std::make_unique<VideoDecoder>(stream_url);
            } catch (...) {
                if (attempt >= max_attempts) throw;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        if (const ms_status_t status = consume_video(*video, decoder, options->progress, options->progress_user,
                                                     stats); status != MS_OK) {
            return status;
        }
    } catch (...) {
        return fail(MS_ERR_DECODE_FAILED, describe_current_exception());
    }

    if (const ms_status_t status = write_decoded_file(decoder, output_path, stats, options->password,
                                                      options->password_len); status != MS_OK) {
        return status;
    }

    fill_result(result, 0, file_size_or_zero(output_path), stats.expected_chunks, stats.total_packets,
                static_cast<uint64_t>(stats.total_frames));
    return MS_OK;
}

const char *ms_status_string(const ms_status_t status) {
    switch (status) {
        case MS_OK: return "success";
        case MS_ERR_INVALID_ARGS: return "invalid arguments";
        case MS_ERR_FILE_NOT_FOUND: return "file not found";
        case MS_ERR_IO: return "I/O error";
        case MS_ERR_ENCODE_FAILED: return "encoding failed";
        case MS_ERR_DECODE_FAILED: return "decoding failed";
        case MS_ERR_CRYPTO: return "encryption/decryption error";
        case MS_ERR_INCOMPLETE: return "incomplete data";
        default: return "unknown error";
    }
}

const char *ms_last_error(void) {
    return last_error.c_str();
}

const char *ms_version(void) {
    return MS_VERSION_STRING;
}
