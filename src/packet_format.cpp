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
    uint8_t read_u8(const std::span<const std::byte> buffer, const std::size_t offset) {
        return static_cast<uint8_t>(buffer[offset]);
    }

    uint16_t read_u16_le(const std::span<const std::byte> buffer, const std::size_t offset) {
        uint16_t value;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        return value;
    }

    uint32_t read_u32_le(const std::span<const std::byte> buffer, const std::size_t offset) {
        uint32_t value;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        return value;
    }

    void write_u8(const std::span<std::byte> buffer, const std::size_t offset, const uint8_t value) {
        buffer[offset] = std::byte{value};
    }

    void write_u16_le(const std::span<std::byte> buffer, const std::size_t offset, const uint16_t value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    }

    void write_u32_le(const std::span<std::byte> buffer, const std::size_t offset, const uint32_t value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    }

    std::size_t packet_crc_offset(const uint8_t version) {
        return version == VERSION_ID_V2 ? CRC_OFF_V2 : CRC_OFF;
    }
}

std::size_t packet_header_size(const uint8_t version) {
    switch (version) {
        case VERSION_ID: return HEADER_SIZE;
        case VERSION_ID_V2: return HEADER_SIZE_V2;
        default: return 0;
    }
}

void write_packet_header(const std::span<std::byte> dest, const PacketHeader &header,
                         const std::span<const std::byte> payload, const HashAlgorithm algo) {
    write_u32_le(dest, MAGIC_OFF, MAGIC_ID);
    write_u8(dest, VERSION_OFF, VERSION_ID_V2);
    write_u8(dest, FLAGS_OFF, header.flags);
    std::memcpy(dest.data() + FILE_ID_OFF, header.file_id.data(), FILE_ID_SIZE);
    write_u32_le(dest, CHUNK_INDEX_OFF, header.chunk_index);
    write_u32_le(dest, CHUNK_SIZE_OFF, header.chunk_size);
    write_u32_le(dest, ORIGINAL_SIZE_OFF, header.original_size);
    write_u16_le(dest, SYMBOL_SIZE_OFF, header.symbol_size);
    write_u32_le(dest, K_OFF, header.k);
    write_u32_le(dest, ESI_OFF, header.esi);
    write_u16_le(dest, PAYLOAD_LEN_OFF, header.payload_len);
    write_u32_le(dest, CRC_OFF_V2, 0);

    const std::span<const std::byte> header_bytes(dest.data(), HEADER_SIZE_V2);
    const uint32_t crc = packet_checksum(header_bytes, payload, CRC_OFF_V2, algo, CRC_SIZE);
    write_u32_le(dest, CRC_OFF_V2, crc);
}

std::optional<ParsedPacket> parse_packet(const std::span<const std::byte> packet) {
    if (packet.size() < HEADER_SIZE) {
        return std::nullopt;
    }

    const uint8_t version = read_u8(packet, VERSION_OFF);
    const std::size_t header_size = packet_header_size(version);
    if (header_size == 0 || packet.size() < header_size) {
        return std::nullopt;
    }

    ParsedPacket result;
    PacketHeader &hdr = result.header;
    hdr.magic = read_u32_le(packet, MAGIC_OFF);
    if (hdr.magic != MAGIC_ID) {
        return std::nullopt;
    }

    hdr.version = version;
    hdr.flags = read_u8(packet, FLAGS_OFF);
    std::memcpy(hdr.file_id.data(), packet.data() + FILE_ID_OFF, FILE_ID_SIZE);
    hdr.chunk_index = read_u32_le(packet, CHUNK_INDEX_OFF);
    hdr.chunk_size = read_u32_le(packet, CHUNK_SIZE_OFF);
    hdr.original_size = version == VERSION_ID_V2 ? read_u32_le(packet, ORIGINAL_SIZE_OFF) : hdr.chunk_size;
    hdr.symbol_size = read_u16_le(packet, SYMBOL_SIZE_OFF);
    hdr.k = read_u32_le(packet, K_OFF);
    hdr.esi = read_u32_le(packet, ESI_OFF);
    hdr.payload_len = read_u16_le(packet, PAYLOAD_LEN_OFF);
    hdr.crc = read_u32_le(packet, packet_crc_offset(version));

    if (packet.size() < header_size + hdr.symbol_size) {
        return std::nullopt;
    }

    result.raw_header = packet.subspan(0, header_size);
    result.payload = packet.subspan(header_size, hdr.symbol_size);
    return result;
}

bool verify_packet_checksum(const ParsedPacket &packet) {
    const std::size_t crc_offset = packet_crc_offset(packet.header.version);
    const HashAlgorithm algo = packet.header.hash_algorithm();
    if (packet_checksum(packet.raw_header, packet.payload, crc_offset, algo, CRC_SIZE) == packet.header.crc) {
        return true;
    }

    const std::size_t payload_len = packet.header.payload_len;
    return payload_len < packet.payload.size() &&
           packet_checksum(packet.raw_header, packet.payload.first(payload_len), crc_offset, algo, CRC_SIZE) ==
           packet.header.crc;
}

std::optional<ParsedPacket> parse_and_verify_packet(const std::span<const std::byte> packet) {
    auto parsed = parse_packet(packet);
    if (!parsed || !verify_packet_checksum(*parsed)) {
        return std::nullopt;
    }
    return parsed;
}
