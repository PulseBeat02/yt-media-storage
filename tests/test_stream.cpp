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

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoder.h"
#include "stream.h"
#include "video_decoder.h"
#include "video_encoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
    Encoder::FileId make_test_file_id() {
        Encoder::FileId file_id{};
        for (std::size_t i = 0; i < file_id.size(); ++i) {
            file_id[i] = std::byte{static_cast<uint8_t>(i)};
        }
        return file_id;
    }

    std::vector<std::byte> make_random_data(const std::size_t byte_count) {
        std::vector<std::byte> data(byte_count);
        for (std::size_t i = 0; i < byte_count; ++i) {
            data[i] = std::byte{static_cast<uint8_t>((i * 131 + 17) % 256)};
        }
        return data;
    }

    struct TempFile {
        const std::string path;

        explicit TempFile(const std::string &name)
            : path((std::filesystem::temp_directory_path() / name).string()) {
        }

        ~TempFile() { std::filesystem::remove(path); }
    };

    void write_file(const std::string &path, const std::vector<std::byte> &data) {
        std::ofstream ofs(path, std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        ofs.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    std::vector<std::byte> read_file(const std::string &path) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) return {};
        const auto size = ifs.tellg();
        ifs.seekg(0);
        std::vector<std::byte> data(size);
        ifs.read(reinterpret_cast<char *>(data.data()),
                 size);
        return data;
    }

    void stream_encode_file(const std::string &input_path, const std::string &output_flv,
                            const int bitrate_kbps, const int width, const int height,
                            const bool encrypt = false,
                            const std::string &password = {}) {
        const FileChunkReader reader(input_path.c_str(), encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0);
        const std::size_t num_chunks = reader.num_chunks();
        const auto file_id = make_test_file_id();
        const Encoder encoder(file_id);

        std::array<std::byte, CRYPTO_KEY_BYTES> key{};
        if (encrypt) {
            const std::span pw(reinterpret_cast<const std::byte *>(password.data()), password.size());
            key = derive_key(pw, file_id);
        }

        StreamEncoder stream_encoder(output_flv, bitrate_kbps, width, height);

        for (std::size_t i = 0; i < num_chunks; ++i) {
            std::vector<std::byte> chunk_data = reader.read_chunk(i);
            std::span<const std::byte> data_to_encode(chunk_data);
            std::vector<std::byte> encrypted_buf;
            if (encrypt) {
                encrypted_buf = encrypt_chunk(data_to_encode, key, file_id, static_cast<uint32_t>(i));
                data_to_encode = encrypted_buf;
            }
            const bool is_last = (i == num_chunks - 1);
            auto [packets, manifest] = encoder.encode_chunk(static_cast<uint32_t>(i), data_to_encode, is_last, encrypt);
            stream_encoder.encode_packets(packets);
        }

        stream_encoder.finalize();
        if (encrypt) secure_zero(std::span<std::byte>(key));
    }

    std::vector<std::byte> stream_decode_file(const std::string &flv_path,
                                              const std::string &password = {}) {
        VideoDecoder video_decoder(flv_path);
        Decoder decoder;
        std::size_t total_extracted = 0;
        std::size_t decoded_chunks = 0;
        uint32_t max_chunk_index = 0;
        bool found_last_chunk = false;
        uint32_t last_chunk_index = 0;

        while (!video_decoder.is_eof()) {
            for (auto frame_packets = video_decoder.decode_next_frame(); auto &pkt_data: frame_packets) {
                ++total_extracted;
                if (pkt_data.size() >= HEADER_SIZE) {
                    const auto flags = static_cast<uint8_t>(pkt_data[FLAGS_OFF]);
                    uint32_t chunk_idx = 0;
                    std::memcpy(&chunk_idx, pkt_data.data() + CHUNK_INDEX_OFF, sizeof(chunk_idx));
                    if (chunk_idx > max_chunk_index) max_chunk_index = chunk_idx;
                    if (flags & LastChunk) {
                        found_last_chunk = true;
                        last_chunk_index = chunk_idx;
                    }
                }
                const std::span<const std::byte> data(pkt_data.data(), pkt_data.size());
                if (auto res = decoder.process_packet(data, false); res && res->success) {
                    ++decoded_chunks;
                }
            }
        }

        if (total_extracted == 0) return {};

        const uint32_t expected_chunks = found_last_chunk
                                             ? last_chunk_index + 1
                                             : max_chunk_index + 1;

        if (decoded_chunks < expected_chunks) return {};

        if (decoder.is_encrypted()) {
            if (password.empty()) return {};
            const std::span<const std::byte> pw(
                reinterpret_cast<const std::byte *>(password.data()), password.size());
            auto key = derive_key(pw, *decoder.file_id());
            decoder.set_decrypt_key(key);
            secure_zero(std::span<std::byte>(key));
        }

        auto assembled = decoder.assemble_file(expected_chunks);
        if (decoder.is_encrypted()) decoder.clear_decrypt_key();
        return assembled.value_or(std::vector<std::byte>{});
    }
} // namespace

TEST(Stream, Roundtrip_1080p_SmallFile) {
    const auto original = make_random_data(65536);
    const TempFile input("test_stream_input.bin");
    const TempFile flv("test_stream_1080.flv");
    write_file(input.path, original);

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 8000, 1920, 1080));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, Roundtrip_4K_SmallFile) {
    const auto original = make_random_data(65536);
    const TempFile input("test_stream_input_4k.bin");
    const TempFile flv("test_stream_4k.flv");
    write_file(input.path, original);

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 35000, 3840, 2160));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, Roundtrip_1080p_LargerFile) {
    const auto original = make_random_data(256 * 1024);
    const TempFile input("test_stream_input_256k.bin");
    const TempFile flv("test_stream_256k.flv");
    write_file(input.path, original);

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 8000, 1920, 1080));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, Roundtrip_WithEncryption) {
    const auto original = make_random_data(65536);
    const TempFile input("test_stream_enc_input.bin");
    const TempFile flv("test_stream_enc.flv");
    write_file(input.path, original);

    const std::string password = "test_password_123";

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 8000, 1920, 1080, true, password));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path, password);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, Roundtrip_SmallData) {
    const auto original = make_random_data(100);
    const TempFile input("test_stream_small.bin");
    const TempFile flv("test_stream_small.flv");
    write_file(input.path, original);

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 8000, 1920, 1080));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, FrameLayout_1080p) {
    const FrameLayout layout = compute_frame_layout(1920, 1080);
    EXPECT_EQ(layout.frame_width, 1920);
    EXPECT_EQ(layout.frame_height, 1080);
    EXPECT_EQ(layout.blocks_per_row, 240);
    EXPECT_EQ(layout.blocks_per_col, 135);
    EXPECT_EQ(layout.total_blocks, 32400);
}

TEST(Stream, FrameLayout_4K) {
    const FrameLayout layout = compute_frame_layout(3840, 2160);
    EXPECT_EQ(layout.frame_width, 3840);
    EXPECT_EQ(layout.frame_height, 2160);
    EXPECT_EQ(layout.blocks_per_row, 480);
    EXPECT_EQ(layout.blocks_per_col, 270);
    EXPECT_EQ(layout.total_blocks, 129600);
}

TEST(Stream, FrameLayout_DefaultMatchesConstants) {
    const FrameLayout default_layout = compute_frame_layout();
    const FrameLayout explicit_layout = compute_frame_layout(FRAME_WIDTH, FRAME_HEIGHT);
    EXPECT_EQ(default_layout.frame_width, explicit_layout.frame_width);
    EXPECT_EQ(default_layout.frame_height, explicit_layout.frame_height);
    EXPECT_EQ(default_layout.total_blocks, explicit_layout.total_blocks);
    EXPECT_EQ(default_layout.bytes_per_frame, explicit_layout.bytes_per_frame);
}

TEST(Stream, MKV_Roundtrip) {
    const auto original = make_random_data(65536);
    const TempFile input("test_mkv_input.bin");
    const TempFile mkv("test_mkv_roundtrip.mkv");
    write_file(input.path, original);

    {
        const FileChunkReader reader(input.path.c_str());
        const auto file_id = make_test_file_id();
        const Encoder encoder(file_id);
        VideoEncoder video_encoder(mkv.path);

        for (std::size_t i = 0; i < reader.num_chunks(); ++i) {
            auto chunk_data = reader.read_chunk(i);
            const bool is_last = (i == reader.num_chunks() - 1);
            auto [packets, manifest] = encoder.encode_chunk(
                static_cast<uint32_t>(i), chunk_data, is_last);
            video_encoder.encode_packets(packets);
        }
        video_encoder.finalize();
    }

    ASSERT_TRUE(std::filesystem::exists(mkv.path));

    {
        VideoDecoder video_decoder(mkv.path);
        Decoder decoder;
        std::size_t decoded_chunks = 0;
        uint32_t last_chunk_index = 0;
        bool found_last = false;

        while (!video_decoder.is_eof()) {
            for (auto frame_packets = video_decoder.decode_next_frame();
                 auto &pkt_data: frame_packets) {
                if (pkt_data.size() >= HEADER_SIZE) {
                    const auto flags = static_cast<uint8_t>(pkt_data[FLAGS_OFF]);
                    uint32_t idx = 0;
                    std::memcpy(&idx, pkt_data.data() + CHUNK_INDEX_OFF, sizeof(idx));
                    if (flags & LastChunk) {
                        found_last = true;
                        last_chunk_index = idx;
                    }
                }
                if (auto res = decoder.process_packet(
                        std::span(pkt_data.data(), pkt_data.size()), false);
                    res && res->success) {
                    ++decoded_chunks;
                }
            }
        }

        ASSERT_TRUE(found_last);
        const uint32_t expected = last_chunk_index + 1;
        ASSERT_GE(decoded_chunks, expected);

        const auto assembled = decoder.assemble_file(expected);
        ASSERT_TRUE(assembled.has_value());
        EXPECT_EQ(*assembled, original);
    }
}

TEST(Stream, Roundtrip_WithXXHash) {
    const auto original = make_random_data(65536);
    const TempFile input("test_stream_xxhash_input.bin");
    const TempFile flv("test_stream_xxhash.flv");
    write_file(input.path, original);

    {
        const FileChunkReader reader(input.path.c_str());
        const auto file_id = make_test_file_id();
        const Encoder encoder(file_id, HashAlgorithm::XXHash32);
        StreamEncoder stream_enc(flv.path, 8000, 1920, 1080);

        for (std::size_t i = 0; i < reader.num_chunks(); ++i) {
            auto chunk_data = reader.read_chunk(i);
            const bool is_last = (i == reader.num_chunks() - 1);
            auto [packets, manifest] = encoder.encode_chunk(
                static_cast<uint32_t>(i), chunk_data, is_last);
            stream_enc.encode_packets(packets);
        }
        stream_enc.finalize();
    }

    ASSERT_TRUE(std::filesystem::exists(flv.path));
    const auto decoded = stream_decode_file(flv.path);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, Roundtrip_720p) {
    const auto original = make_random_data(32768);
    const TempFile input("test_stream_720p_input.bin");
    const TempFile flv("test_stream_720p.flv");
    write_file(input.path, original);

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 4000, 1280, 720));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, PacketsPerFrame_ReturnsPositive) {
    const int ppf = VideoEncoder::packets_per_frame();
    EXPECT_GT(ppf, 0);

    const int sppf = StreamEncoder::packets_per_frame();
    EXPECT_GT(sppf, 0);
    EXPECT_EQ(ppf, sppf);
}

TEST(Stream, MaxPacketBytesPerFrame_MatchesLayout) {
    const std::size_t max_bytes = max_packet_bytes_per_frame();
    const FrameLayout layout = compute_frame_layout();
    EXPECT_EQ(max_bytes, static_cast<std::size_t>(layout.bytes_per_frame));
}

TEST(Stream, Roundtrip_MultiChunk) {
    const auto original = make_random_data(CHUNK_SIZE_BYTES + 1024);
    const TempFile input("test_stream_multichunk_input.bin");
    const TempFile flv("test_stream_multichunk.flv");
    write_file(input.path, original);

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 8000, 1920, 1080));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path);
    EXPECT_EQ(decoded, original);
}

TEST(Stream, DecodeAllFrames_Path) {
    const auto original = make_random_data(32768);
    const TempFile input("test_stream_decodeall_input.bin");
    const TempFile flv("test_stream_decodeall.flv");
    write_file(input.path, original);

    stream_encode_file(input.path, flv.path, 8000, 1920, 1080);

    VideoDecoder video_decoder(flv.path);
    const auto all_packets = video_decoder.decode_all_frames();
    EXPECT_GT(all_packets.size(), 0u);

    Decoder decoder;
    for (const auto &pkt_data: all_packets) {
        (void) decoder.process_packet(std::span(pkt_data.data(), pkt_data.size()), false);
    }
    EXPECT_GE(decoder.chunks_completed(), 1u);
}

TEST(Stream, EmptyFile_Roundtrip) {
    const TempFile input("test_stream_empty_input.bin");
    const TempFile flv("test_stream_empty.flv");
    write_file(input.path, {});

    ASSERT_NO_THROW(stream_encode_file(input.path, flv.path, 8000, 1920, 1080));
    ASSERT_TRUE(std::filesystem::exists(flv.path));

    const auto decoded = stream_decode_file(flv.path);
    EXPECT_TRUE(decoded.empty());
}
