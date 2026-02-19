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
#include <span>
#include <utility>
#include <vector>

#include "integrity.h"

struct Packet {
    std::vector<std::byte> bytes;
};

struct ChunkManifestEntry {
    uint32_t chunk_index = 0;
    uint32_t chunk_size = 0;
    uint32_t original_size = 0;
    Sha256Digest sha256{};
    uint32_t N = 0;
    uint16_t T = 0;
};

class Encoder {
public:
    using FileId = std::array<std::byte, 16>;

    explicit Encoder(FileId file_id, HashAlgorithm hash_algo = HashAlgorithm::CRC32);

    [[nodiscard]] std::pair<std::vector<Packet>, ChunkManifestEntry>
    encode_chunk(uint32_t chunk_index, std::span<const std::byte> chunk_data, bool is_last_chunk,
                bool encrypted = false) const;

    [[nodiscard]] const FileId &file_id() const { return id; }

private:
    FileId id;
    HashAlgorithm algo_;

    void write_packet_header(
        std::span<std::byte> dest,
        uint32_t chunk_index,
        uint32_t chunk_size,
        uint32_t original_size,
        uint16_t symbol_size,
        uint32_t num_source,
        uint32_t block_id,
        uint16_t payload_length,
        uint8_t flags,
        std::span<const std::byte> payload
    ) const;
};
