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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <span>
#include <vector>

#include "integrity.h"

struct PacketHeader {
    uint32_t magic = 0;
    uint8_t version = 0;
    uint8_t flags = 0;
    std::array<std::byte, 16> file_id{};
    uint32_t chunk_index = 0;
    uint32_t chunk_size = 0;
    uint32_t original_size = 0; // v2 only; equals chunk_size for v1
    uint16_t symbol_size = 0;
    uint32_t k = 0; // num source symbols
    uint32_t esi = 0; // encoding symbol id (block id)
    uint16_t payload_len = 0;
    uint32_t crc = 0;
};

struct DecodedPacket {
    PacketHeader header;
    std::vector<std::byte> payload;
};

struct ChunkDecodeResult {
    uint32_t chunk_index = 0;
    std::vector<std::byte> data;
    Sha256Digest sha256{};
    bool success = false;
};

class ChunkDecoder {
public:
    explicit ChunkDecoder(uint32_t chunk_index, uint32_t chunk_size, uint32_t k, uint16_t symbol_size);

    ~ChunkDecoder();

    ChunkDecoder(const ChunkDecoder &) = delete;

    ChunkDecoder &operator=(const ChunkDecoder &) = delete;

    ChunkDecoder(ChunkDecoder &&other) noexcept;

    ChunkDecoder &operator=(ChunkDecoder &&other) noexcept;

    [[nodiscard]] bool add_packet(uint32_t esi, std::span<const std::byte> payload);

    [[nodiscard]] bool is_complete() const { return decoded_; }

    [[nodiscard]] std::vector<std::byte> get_decoded_data() const;

    [[nodiscard]] std::vector<std::byte> consume_decoded_data();

    [[nodiscard]] uint32_t chunk_index() const { return chunk_index_; }

    [[nodiscard]] uint32_t packets_received() const { return packets_received_; }

private:
    uint32_t chunk_index_;
    uint32_t chunk_size_;
    uint32_t k_;
    uint16_t symbol_size_;
    void *codec_ = nullptr;
    bool decoded_ = false;
    uint32_t packets_received_ = 0;
    std::vector<std::byte> decoded_data_;
};

class Decoder {
public:
    using FileId = std::array<std::byte, 16>;

    Decoder();

    [[nodiscard]] static std::optional<DecodedPacket> parse_packet(std::span<const std::byte> packet_data);

    [[nodiscard]] static bool validate_packet_crc(const DecodedPacket &packet);

    [[nodiscard]] static bool validate_raw_packet_crc(std::span<const std::byte> packet_data);

    [[nodiscard]] std::optional<ChunkDecodeResult> process_packet(std::span<const std::byte> packet_data);

    [[nodiscard]] std::optional<ChunkDecodeResult> process_packet(const DecodedPacket &packet);

    [[nodiscard]] bool is_chunk_complete(uint32_t chunk_index) const;

    [[nodiscard]] std::optional<std::vector<std::byte> > get_chunk_data(uint32_t chunk_index) const;

    [[nodiscard]] std::optional<FileId> file_id() const { return id; }

    [[nodiscard]] size_t total_packets_received() const { return total_packets_; }

    [[nodiscard]] size_t chunks_completed() const { return completed_chunks.size(); }

    [[nodiscard]] std::vector<uint32_t> completed_chunk_indices() const;

    [[nodiscard]] std::optional<std::vector<std::byte> > assemble_file(uint32_t expected_chunks) const;

    [[nodiscard]] bool write_assembled_file(const std::string &output_path, uint32_t expected_chunks) const;

    void set_decrypt_key(std::span<const std::byte, 32> key);

    void clear_decrypt_key();

    [[nodiscard]] bool is_encrypted() const { return encrypted_; }

private:
    std::optional<FileId> id;
    bool encrypted_ = false;
    std::array<std::byte, 32> decrypt_key_{};
    bool decrypt_key_set_ = false;
    std::unordered_map<uint32_t, ChunkDecoder> active_decoders;
    std::unordered_map<uint32_t, std::vector<std::byte> > completed_chunks;
    size_t total_packets_ = 0;
};
