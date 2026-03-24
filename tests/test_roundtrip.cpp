#include <gtest/gtest.h>

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoder.h"
#include "integrity.h"

#include <array>
#include <cstddef>
#include <cstdint>
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

    std::span<const std::byte> packet_span(const Packet &packet) {
        return {packet.bytes.data(), packet.bytes.size()};
    }

    void encode_and_feed(const Encoder &encoder,
                         Decoder &decoder,
                         const std::span<const std::byte> chunk_data,
                         const uint32_t chunk_index,
                         const bool is_last_chunk) {
        for (auto [packets, manifest] = encoder.encode_chunk(chunk_index, chunk_data, is_last_chunk); const Packet &
             packet: packets) {
            (void) decoder.process_packet(packet_span(packet), false);
        }
    }
} // namespace

TEST(Roundtrip, SingleChunk) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    Decoder decoder;

    encode_and_feed(encoder, decoder, original_data, 0, true);

    EXPECT_EQ(decoder.chunks_completed(), 1u);

    const std::optional<std::vector<std::byte> > assembled = decoder.assemble_file(1);
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(*assembled, original_data);
}

TEST(Roundtrip, MultipleChunks) {
    constexpr std::size_t total_size = CHUNK_SIZE_BYTES * 2 + 5000;
    const std::vector<std::byte> original_data = make_test_data(total_size);
    const Encoder encoder(make_test_file_id());
    Decoder decoder;

    const ChunkedStorageData chunked = chunkByteData(original_data);
    const auto num_chunks = static_cast<uint32_t>(chunked.chunks.size());

    for (uint32_t chunk_index = 0; chunk_index < num_chunks; ++chunk_index) {
        const std::span<const std::byte> chunk = chunkSpan(chunked, chunk_index);
        const bool is_last = (chunk_index == num_chunks - 1);
        encode_and_feed(encoder, decoder, chunk, chunk_index, is_last);
    }

    EXPECT_EQ(decoder.chunks_completed(), num_chunks);

    const std::optional<std::vector<std::byte> > assembled = decoder.assemble_file(num_chunks);
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(*assembled, original_data);
}

TEST(Roundtrip, SmallData) {
    const std::vector<std::byte> original_data = make_test_data(10);
    const Encoder encoder(make_test_file_id());
    Decoder decoder;

    encode_and_feed(encoder, decoder, original_data, 0, true);

    const std::optional<std::vector<std::byte> > assembled = decoder.assemble_file(1);
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(*assembled, original_data);
}

TEST(Roundtrip, WithEncryption) {
    const std::vector<std::byte> original_data = make_test_data(8192);
    const Encoder::FileId file_id = make_test_file_id();
    const Encoder encoder(file_id);
    Decoder decoder;

    static constexpr std::byte password[] = {
        std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}
    };
    auto encryption_key = derive_key(std::span(password), file_id);

    const std::vector<std::byte> encrypted_chunk = encrypt_chunk(original_data, encryption_key, file_id, 0);
    for (auto [packets, manifest] = encoder.encode_chunk(0, encrypted_chunk, true, true); const Packet &packet:
         packets) {
        (void) decoder.process_packet(packet_span(packet), false);
    }

    EXPECT_TRUE(decoder.is_encrypted());
    decoder.set_decrypt_key(encryption_key);

    const std::optional<std::vector<std::byte> > assembled = decoder.assemble_file(1);
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(*assembled, original_data);

    decoder.clear_decrypt_key();
    secure_zero(std::span<std::byte>(encryption_key));
}

TEST(Roundtrip, WithXXHash) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id(), HashAlgorithm::XXHash32);
    Decoder decoder;

    encode_and_feed(encoder, decoder, original_data, 0, true);

    const std::optional<std::vector<std::byte> > assembled = decoder.assemble_file(1);
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(*assembled, original_data);
}

TEST(Roundtrip, AssembleFile_FailsWithIncompleteChunks) {
    const std::vector<std::byte> original_data = make_test_data(4096);
    const Encoder encoder(make_test_file_id());
    Decoder decoder;

    encode_and_feed(encoder, decoder, original_data, 0, false);

    const std::optional<std::vector<std::byte> > assembled = decoder.assemble_file(2);
    EXPECT_FALSE(assembled.has_value());
}

TEST(Roundtrip, MultipleChunks_Sha256ManifestVerification) {
    constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 1000;
    const std::vector<std::byte> original_data = make_test_data(total_size);
    const Encoder encoder(make_test_file_id());
    Decoder decoder;

    const ChunkedStorageData chunked = chunkByteData(original_data);
    const auto num_chunks = static_cast<uint32_t>(chunked.chunks.size());

    for (uint32_t chunk_index = 0; chunk_index < num_chunks; ++chunk_index) {
        const std::span<const std::byte> chunk = chunkSpan(chunked, chunk_index);
        const bool is_last = (chunk_index == num_chunks - 1);

        auto [packets, manifest] = encoder.encode_chunk(chunk_index, chunk, is_last);

        const Sha256Digest expected_digest = sha256(chunk);
        EXPECT_EQ(manifest.sha256, expected_digest);

        for (const Packet &packet: packets) {
            (void) decoder.process_packet(packet_span(packet), false);
        }
    }

    const std::optional<std::vector<std::byte> > assembled = decoder.assemble_file(num_chunks);
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(*assembled, original_data);
}
