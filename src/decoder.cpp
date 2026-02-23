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

#include "decoder.h"

#include "configuration.h"
#include "crypto.h"
#include "libs/wirehair/wirehair.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <ranges>
#include <stdexcept>

static std::once_flag ensure_init;

static void ensureWirehairInit() {
    std::call_once(ensure_init, [] {
        if (const WirehairResult result = wirehair_init(); result != Wirehair_Success) {
            throw std::runtime_error("wirehair_init failed");
        }
    });
}

static uint8_t readByte(std::span<const std::byte> buffer, const std::size_t offset) {
    return static_cast<uint8_t>(buffer[offset]);
}

static uint16_t readU16LE(const std::span<const std::byte> buffer, const std::size_t offset) {
    uint16_t value;
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
}

static uint32_t readU32LE(const std::span<const std::byte> buffer, const std::size_t offset) {
    uint32_t value;
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
}

ChunkDecoder::ChunkDecoder(const uint32_t chunk_index, const uint32_t chunk_size, const uint32_t k,
                           const uint16_t symbol_size)
    : chunk_index_(chunk_index)
      , chunk_size_(chunk_size)
      , k_(k)
      , symbol_size_(symbol_size) {
    ensureWirehairInit();
    codec_ = wirehair_decoder_create(nullptr, chunk_size_, symbol_size_);
    if (!codec_) {
        throw std::runtime_error("wirehair_decoder_create failed");
    }
}

ChunkDecoder::~ChunkDecoder() {
    if (codec_) {
        wirehair_free(static_cast<WirehairCodec>(codec_));
        codec_ = nullptr;
    }
}

ChunkDecoder::ChunkDecoder(ChunkDecoder &&other) noexcept
    : chunk_index_(other.chunk_index_)
      , chunk_size_(other.chunk_size_)
      , k_(other.k_)
      , symbol_size_(other.symbol_size_)
      , codec_(other.codec_)
      , decoded_(other.decoded_)
      , packets_received_(other.packets_received_)
      , decoded_data_(std::move(other.decoded_data_)) {
    other.codec_ = nullptr;
}

ChunkDecoder &ChunkDecoder::operator=(ChunkDecoder &&other) noexcept {
    if (this != &other) {
        if (codec_) {
            wirehair_free(static_cast<WirehairCodec>(codec_));
        }
        chunk_index_ = other.chunk_index_;
        chunk_size_ = other.chunk_size_;
        k_ = other.k_;
        symbol_size_ = other.symbol_size_;
        codec_ = other.codec_;
        decoded_ = other.decoded_;
        packets_received_ = other.packets_received_;
        decoded_data_ = std::move(other.decoded_data_);
        other.codec_ = nullptr;
    }
    return *this;
}

bool ChunkDecoder::add_packet(const uint32_t esi, const std::span<const std::byte> payload) {
    if (decoded_) {
        return true;
    }

    if (!codec_) {
        throw std::runtime_error("codec is null");
    }

    ++packets_received_;

    const auto *payloadData = reinterpret_cast<const uint8_t *>(payload.data());
    const auto payloadSize = static_cast<uint32_t>(payload.size());

    WirehairResult result = wirehair_decode(
        static_cast<WirehairCodec>(codec_),
        esi,
        payloadData,
        payloadSize
    );

    if (result == Wirehair_Success) {
        decoded_data_.resize(chunk_size_);
        result = wirehair_recover(
            static_cast<WirehairCodec>(codec_),
            decoded_data_.data(),
            chunk_size_
        );

        if (result != Wirehair_Success) {
            throw std::runtime_error("wirehair_recover failed");
        }

        decoded_ = true;
        return true;
    }
    if (result == Wirehair_NeedMore) {
        return false;
    }
    throw std::runtime_error("wirehair_decode failed with error");
}

std::vector<std::byte> ChunkDecoder::get_decoded_data() const {
    if (!decoded_) {
        throw std::runtime_error("data not yet decoded");
    }
    return decoded_data_;
}

std::vector<std::byte> ChunkDecoder::consume_decoded_data() {
    if (!decoded_) {
        throw std::runtime_error("data not yet decoded");
    }
    return std::move(decoded_data_);
}

Decoder::Decoder() = default;

std::optional<DecodedPacket> Decoder::parse_packet(const std::span<const std::byte> packet_data) {
    if (packet_data.size() < HEADER_SIZE) {
        return std::nullopt;
    }

    const uint8_t version = readByte(packet_data, VERSION_OFF);
    const size_t header_size = (version == VERSION_ID_V2) ? HEADER_SIZE_V2 : HEADER_SIZE;
    if (version != VERSION_ID && version != VERSION_ID_V2) {
        return std::nullopt;
    }
    if (packet_data.size() < header_size) {
        return std::nullopt;
    }

    DecodedPacket result;
    auto &[magic, v, flags, file_id, chunk_index, chunk_size, original_size, symbol_size, k, esi, payload_len, crc] =
            result.header;

    magic = readU32LE(packet_data, MAGIC_OFF);
    if (magic != MAGIC_ID) {
        return std::nullopt;
    }

    v = version;
    flags = readByte(packet_data, FLAGS_OFF);

    std::memcpy(file_id.data(), packet_data.data() + FILE_ID_OFF, FILE_ID_SIZE);
    chunk_index = readU32LE(packet_data, CHUNK_INDEX_OFF);
    chunk_size = readU32LE(packet_data, CHUNK_SIZE_OFF);
    if (version == VERSION_ID_V2) {
        original_size = readU32LE(packet_data, ORIGINAL_SIZE_OFF);
    } else {
        original_size = chunk_size;
    }
    symbol_size = readU16LE(packet_data, SYMBOL_SIZE_OFF);
    k = readU32LE(packet_data, K_OFF);
    esi = readU32LE(packet_data, ESI_OFF);
    payload_len = readU16LE(packet_data, PAYLOAD_LEN_OFF);
    crc = readU32LE(packet_data, (version == VERSION_ID_V2) ? CRC_OFF_V2 : CRC_OFF);

    if (const size_t expected_total = header_size + symbol_size; packet_data.size() < expected_total) {
        return std::nullopt;
    }
    result.payload.resize(symbol_size);
    std::memcpy(result.payload.data(), packet_data.data() + header_size, symbol_size);

    return result;
}

bool Decoder::validate_raw_packet_crc(const std::span<const std::byte> packet_data) {
    if (packet_data.size() < HEADER_SIZE) {
        return false;
    }

    const uint8_t version = readByte(packet_data, VERSION_OFF);
    const size_t header_size = (version == VERSION_ID_V2) ? HEADER_SIZE_V2 : HEADER_SIZE;
    const size_t crc_offset = (version == VERSION_ID_V2) ? CRC_OFF_V2 : CRC_OFF;
    if (version != VERSION_ID && version != VERSION_ID_V2) {
        return false;
    }
    if (packet_data.size() < header_size) {
        return false;
    }

    const uint16_t symbol_size = readU16LE(packet_data, SYMBOL_SIZE_OFF);
    if (packet_data.size() < header_size + symbol_size) {
        return false;
    }

    const uint32_t stored_crc = readU32LE(packet_data, crc_offset);
    const uint8_t flags = readByte(packet_data, FLAGS_OFF);
    const HashAlgorithm algo = (flags & UseXXHash) ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32;

    const auto header_span = packet_data.subspan(0, header_size);
    const auto payload_span = packet_data.subspan(header_size, symbol_size);
    const uint32_t computed_crc = packet_checksum(header_span, payload_span, crc_offset, algo, CRC_SIZE);

    return stored_crc == computed_crc;
}

bool Decoder::validate_packet_crc(const DecodedPacket &packet) {
    const bool is_v2 = (packet.header.version == VERSION_ID_V2);
    const size_t header_size = is_v2 ? HEADER_SIZE_V2 : HEADER_SIZE;
    const size_t crc_offset = is_v2 ? CRC_OFF_V2 : CRC_OFF;

    std::array<std::byte, HEADER_SIZE_V2> header{};
    const std::span buf(header.data(), header_size);

    std::memcpy(buf.data() + MAGIC_OFF, &packet.header.magic, sizeof(packet.header.magic));
    buf[VERSION_OFF] = std::byte{packet.header.version};
    buf[FLAGS_OFF] = std::byte{packet.header.flags};
    std::memcpy(buf.data() + FILE_ID_OFF, packet.header.file_id.data(), FILE_ID_SIZE);

    const uint32_t chunk_index = packet.header.chunk_index;
    std::memcpy(buf.data() + CHUNK_INDEX_OFF, &chunk_index, sizeof(chunk_index));

    const uint32_t chunk_size = packet.header.chunk_size;
    std::memcpy(buf.data() + CHUNK_SIZE_OFF, &chunk_size, sizeof(chunk_size));

    if (is_v2) {
        const uint32_t original_size = packet.header.original_size;
        std::memcpy(buf.data() + ORIGINAL_SIZE_OFF, &original_size, sizeof(original_size));
    }

    const uint16_t symbol_size = packet.header.symbol_size;
    std::memcpy(buf.data() + SYMBOL_SIZE_OFF, &symbol_size, sizeof(symbol_size));

    const uint32_t k = packet.header.k;
    std::memcpy(buf.data() + K_OFF, &k, sizeof(k));

    const uint32_t esi = packet.header.esi;
    std::memcpy(buf.data() + ESI_OFF, &esi, sizeof(esi));

    const uint16_t payload_len = packet.header.payload_len;
    std::memcpy(buf.data() + PAYLOAD_LEN_OFF, &payload_len, sizeof(payload_len));

    constexpr uint32_t zero_crc = 0;
    std::memcpy(buf.data() + crc_offset, &zero_crc, sizeof(zero_crc));
    const std::span<const std::byte> headerSpan(header.data(), header_size);
    const std::span payloadSpan(packet.payload.data(), packet.payload.size());
    const HashAlgorithm algo = (packet.header.flags & UseXXHash) ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32;
    const uint32_t computed_crc = packet_checksum(headerSpan, payloadSpan, crc_offset, algo, CRC_SIZE);

    return computed_crc == packet.header.crc;
}

static std::optional<DecodedPacket> parse_and_validate_packet(const std::span<const std::byte> packet_data) {
    if (packet_data.size() < HEADER_SIZE) {
        return std::nullopt;
    }

    DecodedPacket result;
    auto &[magic, v, flags, file_id, chunk_index, chunk_size, original_size, symbol_size, k, esi, payload_len, crc] =
            result.header;

    v = readByte(packet_data, VERSION_OFF);
    if (v != VERSION_ID && v != VERSION_ID_V2) {
        return std::nullopt;
    }
    const size_t header_size = (v == VERSION_ID_V2) ? HEADER_SIZE_V2 : HEADER_SIZE;
    const size_t crc_offset = (v == VERSION_ID_V2) ? CRC_OFF_V2 : CRC_OFF;
    if (packet_data.size() < header_size) {
        return std::nullopt;
    }

    magic = readU32LE(packet_data, MAGIC_OFF);
    if (magic != MAGIC_ID) {
        return std::nullopt;
    }

    flags = readByte(packet_data, FLAGS_OFF);
    std::memcpy(file_id.data(), packet_data.data() + FILE_ID_OFF, FILE_ID_SIZE);
    chunk_index = readU32LE(packet_data, CHUNK_INDEX_OFF);
    chunk_size = readU32LE(packet_data, CHUNK_SIZE_OFF);
    original_size = (v == VERSION_ID_V2) ? readU32LE(packet_data, ORIGINAL_SIZE_OFF) : chunk_size;
    symbol_size = readU16LE(packet_data, SYMBOL_SIZE_OFF);
    k = readU32LE(packet_data, K_OFF);
    esi = readU32LE(packet_data, ESI_OFF);
    payload_len = readU16LE(packet_data, PAYLOAD_LEN_OFF);

    if (packet_data.size() < header_size + symbol_size) {
        return std::nullopt;
    }

    crc = readU32LE(packet_data, crc_offset);
    const auto header_span = packet_data.subspan(0, header_size);
    const HashAlgorithm algo = (flags & UseXXHash) ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32;
    if (const auto payload_span = packet_data.subspan(header_size, symbol_size); packet_checksum(header_span,
            payload_span, crc_offset, algo, CRC_SIZE) != crc) {
        return std::nullopt;
    }

    result.payload.resize(symbol_size);
    std::memcpy(result.payload.data(), packet_data.data() + header_size, symbol_size);

    return result;
}

std::optional<ChunkDecodeResult> Decoder::process_packet(const std::span<const std::byte> packet_data) {
    ++total_packets_;

    const auto parsed = parse_and_validate_packet(packet_data);
    if (!parsed) {
        return std::nullopt;
    }

    const PacketHeader &hdr = parsed->header;
    if (!id) {
        id = hdr.file_id;
        encrypted_ = (hdr.flags & Encrypted) != 0;
    }

    if (completed_chunks.contains(hdr.chunk_index)) {
        return std::nullopt;
    }

    auto it = active_decoders.find(hdr.chunk_index);
    if (it == active_decoders.end()) {
        auto [inserted_it, success] = active_decoders.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(hdr.chunk_index),
            std::forward_as_tuple(hdr.chunk_index, hdr.chunk_size, hdr.k, hdr.symbol_size)
        );
        it = inserted_it;
    }

    ChunkDecoder &decoder = it->second;
    if (const std::span payloadSpan(parsed->payload.data(), parsed->payload.size()); decoder.add_packet(
        hdr.esi, payloadSpan)) {
        ChunkDecodeResult result;
        result.chunk_index = hdr.chunk_index;
        result.data = decoder.consume_decoded_data();
        const uint32_t copy_len = std::min(static_cast<uint32_t>(result.data.size()), hdr.original_size);
        result.data.resize(copy_len);
        result.sha256 = sha256(std::span<const std::byte>(result.data.data(), result.data.size()));
        result.success = true;
        completed_chunks[hdr.chunk_index] = std::move(result.data);
        active_decoders.erase(it);

        return result;
    }

    return std::nullopt;
}

std::optional<ChunkDecodeResult> Decoder::process_packet(const DecodedPacket &packet) {
    ++total_packets_;
    if (!validate_packet_crc(packet)) {
        return std::nullopt;
    }

    const PacketHeader &hdr = packet.header;
    if (!id) {
        id = hdr.file_id;
        encrypted_ = (hdr.flags & Encrypted) != 0;
    }

    if (completed_chunks.contains(hdr.chunk_index)) {
        return std::nullopt;
    }

    auto it = active_decoders.find(hdr.chunk_index);
    if (it == active_decoders.end()) {
        auto [inserted_it, success] = active_decoders.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(hdr.chunk_index),
            std::forward_as_tuple(hdr.chunk_index, hdr.chunk_size, hdr.k, hdr.symbol_size)
        );
        it = inserted_it;
    }

    ChunkDecoder &decoder = it->second;
    if (const std::span payloadSpan(packet.payload.data(), packet.payload.size()); decoder.add_packet(
        hdr.esi, payloadSpan)) {
        ChunkDecodeResult result;
        result.chunk_index = hdr.chunk_index;
        result.data = decoder.consume_decoded_data();
        const uint32_t copy_len = std::min(static_cast<uint32_t>(result.data.size()), hdr.original_size);
        result.data.resize(copy_len);
        result.sha256 = sha256(std::span<const std::byte>(result.data.data(), result.data.size()));
        result.success = true;
        completed_chunks[hdr.chunk_index] = std::move(result.data);
        active_decoders.erase(it);

        return result;
    }

    return std::nullopt;
}

bool Decoder::is_chunk_complete(const uint32_t chunk_index) const {
    return completed_chunks.contains(chunk_index);
}

std::optional<std::vector<std::byte> > Decoder::get_chunk_data(const uint32_t chunk_index) const {
    if (const auto it = completed_chunks.find(chunk_index); it != completed_chunks.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<uint32_t> Decoder::completed_chunk_indices() const {
    std::vector<uint32_t> indices;
    indices.reserve(completed_chunks.size());
    for (const auto &index: completed_chunks | std::views::keys) {
        indices.push_back(index);
    }
    return indices;
}

void Decoder::set_decrypt_key(const std::span<const std::byte, 32> key) {
    std::memcpy(decrypt_key_.data(), key.data(), 32);
    decrypt_key_set_ = true;
}

void Decoder::clear_decrypt_key() {
    if (decrypt_key_set_) {
        secure_zero(decrypt_key_);
        decrypt_key_set_ = false;
    }
}

namespace {
    std::optional<std::vector<std::size_t> > compute_chunk_sizes(
        const std::unordered_map<uint32_t, std::vector<std::byte> > &chunks,
        const uint32_t expected_chunks,
        const bool encrypted,
        const bool decrypt_key_set) {
        std::vector<std::size_t> sizes(expected_chunks);
        for (const auto &[idx, chunk]: chunks) {
            if (idx >= expected_chunks) {
                return std::nullopt;
            }
            if (encrypted && decrypt_key_set) {
                if (chunk.size() < CRYPTO_PLAIN_SIZE_HEADER) {
                    return std::nullopt;
                }
                sizes[idx] = read_plain_size_from_header(chunk);
                if (sizes[idx] > CHUNK_SIZE_BYTES) {
                    return std::nullopt;
                }
            } else {
                sizes[idx] = chunk.size();
            }
        }
        return sizes;
    }

    std::vector<std::size_t> compute_prefix_offsets(const std::vector<std::size_t> &sizes) {
        std::vector<std::size_t> offsets(sizes.size() + 1);
        offsets[0] = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            offsets[i + 1] = offsets[i] + sizes[i];
        }
        return offsets;
    }

    void decrypt_and_copy_into(
        std::vector<std::byte> &result,
        const std::unordered_map<uint32_t, std::vector<std::byte> > &chunks,
        const uint32_t expected_chunks,
        const std::vector<std::size_t> &offsets,
        const std::vector<std::size_t> &sizes,
        const bool encrypted,
        const bool decrypt_key_set,
        const std::array<std::byte, 32> &decrypt_key,
        const std::array<std::byte, 16> &file_id) {
        std::vector<const std::vector<std::byte> *> chunk_ptrs(expected_chunks);
        for (uint32_t i = 0; i < expected_chunks; ++i) {
            chunk_ptrs[i] = &chunks.at(i);
        }

#pragma omp parallel for schedule(static)
        for (int i = 0; i < static_cast<int>(expected_chunks); ++i) {
            const auto &chunk = *chunk_ptrs[i];
            const std::size_t copy_size = sizes[i];
            if (encrypted && decrypt_key_set) {
                decrypt_chunk_into(
                    std::span<std::byte>(result.data() + offsets[i], copy_size),
                    chunk, decrypt_key, file_id, static_cast<uint32_t>(i));
            } else {
                std::memcpy(result.data() + offsets[i], chunk.data(), copy_size);
            }
        }
    }
}

std::optional<std::vector<std::byte> > Decoder::assemble_file(const uint32_t expected_chunks) const {
    if (completed_chunks.size() != expected_chunks) {
        return std::nullopt;
    }
    for (const auto &idx: completed_chunks | std::views::keys) {
        if (idx >= expected_chunks) {
            return std::nullopt;
        }
    }
    if (encrypted_ && !decrypt_key_set_) {
        return std::nullopt;
    }
    if (!id) {
        return std::nullopt;
    }

    const auto chunk_sizes = compute_chunk_sizes(
        completed_chunks, expected_chunks, encrypted_, decrypt_key_set_);
    if (!chunk_sizes) {
        return std::nullopt;
    }
    const auto offsets = compute_prefix_offsets(*chunk_sizes);
    std::vector<std::byte> result(offsets[expected_chunks]);
    decrypt_and_copy_into(result, completed_chunks, expected_chunks, offsets,
                          *chunk_sizes, encrypted_, decrypt_key_set_, decrypt_key_, *id);
    return result;
}

bool Decoder::write_assembled_file(const std::string &output_path, const uint32_t expected_chunks) const {
    if (completed_chunks.size() != expected_chunks) {
        return false;
    }
    for (const auto &idx : completed_chunks | std::views::keys) {
        if (idx >= expected_chunks) {
            return false;
        }
    }
    if (encrypted_ && !decrypt_key_set_) {
        return false;
    }
    if (!id) {
        return false;
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        return false;
    }

    for (uint32_t i = 0; i < expected_chunks; ++i) {
        const auto &chunk = completed_chunks.at(i);
        if (encrypted_ && decrypt_key_set_) {
            if (chunk.size() < CRYPTO_PLAIN_SIZE_HEADER) {
                return false;
            }
            const std::size_t plain_size = read_plain_size_from_header(chunk);
            if (plain_size > CHUNK_SIZE_BYTES) {
                return false;
            }
            std::vector<std::byte> decrypted(plain_size);
            decrypt_chunk_into(
                std::span<std::byte>(decrypted.data(), plain_size),
                chunk, decrypt_key_, *id, i);
            out.write(reinterpret_cast<const char *>(decrypted.data()),
                      static_cast<std::streamsize>(plain_size));
        } else {
            out.write(reinterpret_cast<const char *>(chunk.data()),
                      static_cast<std::streamsize>(chunk.size()));
        }
        if (!out.good()) {
            return false;
        }
    }

    return true;
}
