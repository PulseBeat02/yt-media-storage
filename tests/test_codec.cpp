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

#include "configuration.h"
#include "decoder.h"
#include "encoder.h"
#include "integrity.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace {
    Encoder::FileId make_test_file_id() {
        Encoder::FileId file_id{};
        for (std::size_t i = 0; i < file_id.size(); ++i) {
            file_id[i] = std::byte{static_cast<uint8_t>(i)};
        }
        return file_id;
    }

    std::vector<std::byte> make_test_data(const std::size_t byte_count) {
        std::vector<std::byte> data(byte_count);
        for (std::size_t i = 0; i < byte_count; ++i) {
            data[i] = std::byte{static_cast<uint8_t>(i % 251)};
        }
        return data;
    }

    uint32_t read_u32_le(const std::byte *source) {
        uint32_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }

    std::span<const std::byte> packet_span(const Packet &packet) {
        return {packet.bytes.data(), packet.bytes.size()};
    }

    struct EncodeResult {
        std::vector<Packet> packets;
        ChunkManifestEntry manifest;
    };

    EncodeResult encode_test_data(const Encoder &encoder,
                                  const std::vector<std::byte> &data,
                                  const uint32_t chunk_index = 0,
                                  const bool is_last_chunk = false,
                                  const bool encrypted = false) {
        auto [packets, manifest] = encoder.encode_chunk(chunk_index, data, is_last_chunk, encrypted);
        return {std::move(packets), manifest};
    }

    bool feed_all_packets_to_decoder(Decoder &decoder, const std::vector<Packet> &packets) {
        bool any_decoded = false;
        for (const Packet &packet: packets) {
            if (std::optional<ChunkDecodeResult> result = decoder.process_packet(packet_span(packet));
                result.has_value() && result->success) {
                any_decoded = true;
            }
        }
        return any_decoded;
    }
} // namespace

TEST(Codec, Encoder_ProducesCorrectPacketCount) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);
    EXPECT_GT(packets.size(), 0u);
    EXPECT_GE(packets.size(), 32u);
    EXPECT_LE(packets.size(), 64u);
}

TEST(Codec, Encoder_PacketHasCorrectMagic) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);
    for (const auto &[bytes]: packets) {
        const uint32_t magic = read_u32_le(bytes.data());
        EXPECT_EQ(magic, MAGIC_ID);
    }
}

TEST(Codec, Encoder_PacketHasV2Version) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);
    for (const auto &[bytes]: packets) {
        const auto version = static_cast<uint8_t>(bytes[VERSION_OFF]);
        EXPECT_EQ(version, VERSION_ID_V2);
    }
}

TEST(Codec, Encoder_ManifestMatchesInput) {
    constexpr uint32_t chunk_index = 7;
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, chunk_index);
    EXPECT_EQ(manifest.chunk_index, chunk_index);
    EXPECT_EQ(manifest.original_size, static_cast<uint32_t>(input_data.size()));
}

TEST(Codec, Encoder_LastChunkFlagIsSet) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, true);
    for (const auto &[bytes]: packets) {
        const auto flags = static_cast<uint8_t>(bytes[FLAGS_OFF]);
        EXPECT_NE(flags & LastChunk, 0);
    }
}

TEST(Codec, Encoder_LastChunkFlagNotSetForMiddleChunk) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, false);
    for (const auto &[bytes]: packets) {
        const auto flags = static_cast<uint8_t>(bytes[FLAGS_OFF]);
        EXPECT_EQ(flags & LastChunk, 0);
    }
}

TEST(Codec, Encoder_SmallDataIsPadded) {
    const std::vector<std::byte> small_data = make_test_data(100);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, small_data);

    constexpr auto min_padded_size = static_cast<uint32_t>(SYMBOL_SIZE_BYTES * 2);
    EXPECT_GE(manifest.chunk_size, min_padded_size);
}

TEST(Codec, Decoder_ParsesValidPacket) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(packet_span(packets[0]));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->header.magic, MAGIC_ID);
    EXPECT_EQ(parsed->header.version, VERSION_ID_V2);
}

TEST(Codec, Decoder_RejectsInvalidMagic) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    std::vector corrupted_packet(packets[0].bytes.begin(),
                                 packets[0].bytes.end());
    corrupted_packet[0] = std::byte{0};
    corrupted_packet[1] = std::byte{0};
    corrupted_packet[2] = std::byte{0};
    corrupted_packet[3] = std::byte{0};

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(corrupted_packet);
    EXPECT_FALSE(parsed.has_value());
}

TEST(Codec, Decoder_RejectsTruncatedPacket) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const auto &source_bytes = packets[0].bytes;
    const std::vector truncated_packet(source_bytes.begin(), source_bytes.begin() + 100);

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(truncated_packet);
    EXPECT_FALSE(parsed.has_value());
}

TEST(Codec, Decoder_ValidatesCrcOnParsedPacket) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const std::optional<DecodedPacket> parsed = Decoder::parse_packet(packet_span(packets[0]));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(Decoder::validate_packet_crc(*parsed));
}

TEST(Codec, Decoder_ValidatesCrcOnRawBytes) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    const bool crc_valid = Decoder::validate_raw_packet_crc(packet_span(packets[0]));
    EXPECT_TRUE(crc_valid);
}

TEST(Codec, Decoder_DetectsCorruptedPayload) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    std::vector corrupted_packet(packets[0].bytes.begin(),
                                 packets[0].bytes.end());
    corrupted_packet[HEADER_SIZE_V2] ^= std::byte{0xFF};

    const bool crc_valid = Decoder::validate_raw_packet_crc(corrupted_packet);
    EXPECT_FALSE(crc_valid);
}

TEST(Codec, Decoder_ChunkRoundtrip) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    Decoder decoder;
    const bool decoded = feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoded);
    EXPECT_TRUE(decoder.is_chunk_complete(0));

    const std::optional<std::vector<std::byte> > recovered_chunk = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered_chunk.has_value());
    EXPECT_EQ(*recovered_chunk, original_data);
}

TEST(Codec, Decoder_ChunkRoundtripVerifiesSha256) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    Decoder decoder;
    std::optional<ChunkDecodeResult> decode_result;
    for (const Packet &packet: packets) {
        if (std::optional<ChunkDecodeResult> result = decoder.process_packet(packet_span(packet), true);
            result.has_value() && result->success) {
            decode_result = std::move(*result);
        }
    }

    ASSERT_TRUE(decode_result.has_value());

    const std::span original_span(original_data.data(), original_data.size());
    const Sha256Digest expected_digest = sha256(original_span);
    EXPECT_EQ(decode_result->sha256, expected_digest);
}

TEST(Codec, Decoder_XXHashAlgorithmRoundtrip) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id(), HashAlgorithm::XXHash32);
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    Decoder decoder;
    const bool decoded = feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoded);

    const std::optional<std::vector<std::byte> > recovered_chunk = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered_chunk.has_value());
    EXPECT_EQ(*recovered_chunk, original_data);
}

TEST(Codec, Encoder_EncryptedFlagIsSet) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data, 0, false, true);
    for (const auto &[bytes]: packets) {
        const auto flags = static_cast<uint8_t>(bytes[FLAGS_OFF]);
        EXPECT_NE(flags & Encrypted, 0);
    }
}

TEST(Codec, Decoder_CompletedChunkReturnsNullopt) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets);
    EXPECT_TRUE(decoder.is_chunk_complete(0));

    for (const Packet &packet: packets) {
        auto result = decoder.process_packet(packet_span(packet));
        EXPECT_FALSE(result.has_value());
    }
}

TEST(Codec, Decoder_MultipleChunks) {
    const auto chunk0_data = make_test_data(2048);
    const auto chunk1_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());

    auto [packets0, m0] = encoder.encode_chunk(0, chunk0_data, false);
    auto [packets1, m1] = encoder.encode_chunk(1, chunk1_data, true);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets0);
    feed_all_packets_to_decoder(decoder, packets1);

    EXPECT_EQ(decoder.chunks_completed(), 2u);
    EXPECT_TRUE(decoder.is_chunk_complete(0));
    EXPECT_TRUE(decoder.is_chunk_complete(1));

    const auto data0 = decoder.get_chunk_data(0);
    const auto data1 = decoder.get_chunk_data(1);
    ASSERT_TRUE(data0.has_value());
    ASSERT_TRUE(data1.has_value());
    EXPECT_EQ(*data0, chunk0_data);
    EXPECT_EQ(*data1, chunk1_data);
}

TEST(Codec, Decoder_StateTracking) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    EXPECT_EQ(decoder.total_packets_received(), 0u);
    EXPECT_EQ(decoder.chunks_completed(), 0u);
    EXPECT_FALSE(decoder.is_chunk_complete(0));

    feed_all_packets_to_decoder(decoder, packets);

    EXPECT_EQ(decoder.total_packets_received(), packets.size());
    EXPECT_EQ(decoder.chunks_completed(), 1u);
    EXPECT_TRUE(decoder.is_chunk_complete(0));

    auto indices = decoder.completed_chunk_indices();
    ASSERT_EQ(indices.size(), 1u);
    EXPECT_EQ(indices[0], 0u);
}

TEST(Codec, Decoder_GetChunkDataIsCopy) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoder.get_chunk_data(0).has_value());
    EXPECT_TRUE(decoder.get_chunk_data(0).has_value());
}

TEST(Codec, Decoder_PacketLossResilience) {
    const std::vector<std::byte> original_data = make_test_data(SYMBOL_SIZE_BYTES * 4);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, original_data);

    const std::size_t half = packets.size() / 2;

    Decoder decoder;
    bool decoded = false;
    for (std::size_t i = 0; i < half; ++i) {
        if (auto result = decoder.process_packet(packet_span(packets[i]));
            result.has_value() && result->success) {
            decoded = true;
        }
    }

    EXPECT_TRUE(decoded);
    const auto recovered = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, original_data);
}

TEST(Codec, Decoder_OutOfOrderDelivery) {
    const std::vector<std::byte> original_data = make_test_data(SYMBOL_SIZE_BYTES * 4);
    const Encoder encoder(make_test_file_id());
    auto [packets, manifest] = encode_test_data(encoder, original_data);

    std::ranges::reverse(packets);

    Decoder decoder;
    const bool decoded = feed_all_packets_to_decoder(decoder, packets);

    EXPECT_TRUE(decoded);
    const auto recovered = decoder.get_chunk_data(0);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, original_data);
}

TEST(Codec, Decoder_IncompleteChunkNotAccessible) {
    const std::vector<std::byte> input_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    const auto [packets, manifest] = encode_test_data(encoder, input_data);

    Decoder decoder;
    (void) decoder.process_packet(packet_span(packets[0]));

    EXPECT_FALSE(decoder.is_chunk_complete(0));
    EXPECT_FALSE(decoder.get_chunk_data(0).has_value());
}
