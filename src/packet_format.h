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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "configuration.h"
#include "integrity.h"

struct Packet {
  std::array<std::byte, PACKET_SIZE> bytes{};
};

struct PacketHeader {
  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t flags = 0;
  FileId file_id{};
  uint32_t chunk_index = 0;
  uint32_t chunk_size = 0;
  uint32_t original_size = 0; // v2 only; equals chunk_size for v1
  uint16_t symbol_size = 0;
  uint32_t k = 0;   // num source symbols
  uint32_t esi = 0; // encoding symbol id (block id)
  uint16_t payload_len = 0;
  uint32_t crc = 0;

  [[nodiscard]] HashAlgorithm hash_algorithm() const {
    return (flags & FLAG_USE_XXHASH) ? HashAlgorithm::XXHash32 : HashAlgorithm::CRC32;
  }
};

struct ParsedPacket {
  PacketHeader header;
  std::span<const std::byte> raw_header;
  std::span<const std::byte> payload;
};

[[nodiscard]] std::size_t packet_header_size(uint8_t version);

void write_packet_header(std::span<std::byte> dest, const PacketHeader& header, std::span<const std::byte> payload,
                         HashAlgorithm algo);

[[nodiscard]] std::optional<ParsedPacket> parse_packet(std::span<const std::byte> packet);

[[nodiscard]] bool verify_packet_checksum(const ParsedPacket& packet);

[[nodiscard]] std::optional<ParsedPacket> parse_and_verify_packet(std::span<const std::byte> packet);
