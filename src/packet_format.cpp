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

#include "packet_format.h"

#include <cstring>

namespace {
uint8_t read_uint8(const std::span<const std::byte> buffer, const std::size_t offset) {
  return static_cast<uint8_t>(buffer[offset]);
}

uint16_t read_uint16_little_endian(const std::span<const std::byte> buffer, const std::size_t offset) {
  uint16_t value;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return value;
}

uint32_t read_uint32_little_endian(const std::span<const std::byte> buffer, const std::size_t offset) {
  uint32_t value;
  std::memcpy(&value, buffer.data() + offset, sizeof(value));
  return value;
}

void write_uint8(const std::span<std::byte> buffer, const std::size_t offset, const uint8_t value) {
  buffer[offset] = std::byte{value};
}

void write_uint16_little_endian(const std::span<std::byte> buffer, const std::size_t offset, const uint16_t value) {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void write_uint32_little_endian(const std::span<std::byte> buffer, const std::size_t offset, const uint32_t value) {
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

std::size_t packet_crc_offset(const uint8_t version) {
  return version == VERSION_ID_V2 ? CRC_OFFSET_V2 : CRC_OFFSET;
}
}

std::size_t packet_header_size(const uint8_t version) {
  switch (version) {
  case VERSION_ID:
    return HEADER_SIZE;
  case VERSION_ID_V2:
    return HEADER_SIZE_V2;
  default:
    return 0;
  }
}

void write_packet_header(const std::span<std::byte> destination, const packet_header& header,
                         const std::span<const std::byte> payload, const hash_algorithm_type algorithm) {
  write_uint32_little_endian(destination, MAGIC_OFFSET, MAGIC_ID);
  write_uint8(destination, VERSION_OFFSET, VERSION_ID_V2);
  write_uint8(destination, FLAGS_OFFSET, header.flags);
  std::memcpy(destination.data() + FILE_ID_OFFSET, header.file_id.data(), FILE_ID_SIZE);
  write_uint32_little_endian(destination, CHUNK_INDEX_OFFSET, header.chunk_index);
  write_uint32_little_endian(destination, CHUNK_SIZE_OFFSET, header.chunk_size);
  write_uint32_little_endian(destination, ORIGINAL_SIZE_OFFSET, header.original_size);
  write_uint16_little_endian(destination, SYMBOL_SIZE_OFFSET, header.symbol_size);
  write_uint32_little_endian(destination, SOURCE_SYMBOL_COUNT_OFFSET, header.source_symbol_count);
  write_uint32_little_endian(destination, ESI_OFFSET, header.esi);
  write_uint16_little_endian(destination, PAYLOAD_LENGTH_OFFSET, header.payload_length);
  write_uint32_little_endian(destination, CRC_OFFSET_V2, 0);

  const std::span<const std::byte> header_bytes(destination.data(), HEADER_SIZE_V2);
  const uint32_t crc = packet_checksum(header_bytes, payload, CRC_OFFSET_V2, algorithm, CRC_SIZE);
  write_uint32_little_endian(destination, CRC_OFFSET_V2, crc);
}

std::optional<parsed_packet> parse_packet(const std::span<const std::byte> packet) {
  if (packet.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  const uint8_t version = read_uint8(packet, VERSION_OFFSET);
  const std::size_t header_size = packet_header_size(version);
  if (header_size == 0 || packet.size() < header_size) {
    return std::nullopt;
  }

  parsed_packet result;
  packet_header& header = result.header;
  header.magic = read_uint32_little_endian(packet, MAGIC_OFFSET);
  if (header.magic != MAGIC_ID) {
    return std::nullopt;
  }

  header.version = version;
  header.flags = read_uint8(packet, FLAGS_OFFSET);
  std::memcpy(header.file_id.data(), packet.data() + FILE_ID_OFFSET, FILE_ID_SIZE);
  header.chunk_index = read_uint32_little_endian(packet, CHUNK_INDEX_OFFSET);
  header.chunk_size = read_uint32_little_endian(packet, CHUNK_SIZE_OFFSET);
  header.original_size =
      version == VERSION_ID_V2 ? read_uint32_little_endian(packet, ORIGINAL_SIZE_OFFSET) : header.chunk_size;
  header.symbol_size = read_uint16_little_endian(packet, SYMBOL_SIZE_OFFSET);
  header.source_symbol_count = read_uint32_little_endian(packet, SOURCE_SYMBOL_COUNT_OFFSET);
  header.esi = read_uint32_little_endian(packet, ESI_OFFSET);
  header.payload_length = read_uint16_little_endian(packet, PAYLOAD_LENGTH_OFFSET);
  header.crc = read_uint32_little_endian(packet, packet_crc_offset(version));

  if (packet.size() < header_size + header.symbol_size) {
    return std::nullopt;
  }

  result.raw_header = packet.subspan(0, header_size);
  result.payload = packet.subspan(header_size, header.symbol_size);
  return result;
}

bool verify_packet_checksum(const parsed_packet& packet) {
  const std::size_t crc_offset = packet_crc_offset(packet.header.version);
  const hash_algorithm_type algorithm = packet.header.hash_algorithm();
  if (packet_checksum(packet.raw_header, packet.payload, crc_offset, algorithm, CRC_SIZE) == packet.header.crc) {
    return true;
  }

  const std::size_t payload_length = packet.header.payload_length;
  return payload_length < packet.payload.size() &&
         packet_checksum(packet.raw_header, packet.payload.first(payload_length), crc_offset, algorithm, CRC_SIZE) ==
             packet.header.crc;
}

std::optional<parsed_packet> parse_and_verify_packet(const std::span<const std::byte> packet) {
  std::optional<parsed_packet> parsed = parse_packet(packet);
  if (!parsed || !verify_packet_checksum(*parsed)) {
    return std::nullopt;
  }

  return parsed;
}
