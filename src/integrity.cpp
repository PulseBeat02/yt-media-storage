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

#include "integrity.h"

#include "libs/picosha2.h"
#include "libs/CRC.h"
#define XXH_INLINE_ALL
#include "libs/xxhash.h"

#include <array>
#include <cstring>

namespace {
    constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

    std::string bytes_to_hex(const std::span<const std::byte> input) {
        std::string hex(input.size() * 2, '\0');
        char *out = hex.data();
        for (const std::byte b: input) {
            const auto value = std::to_integer<unsigned char>(b);
            *out++ = HEX_DIGITS[value >> 4];
            *out++ = HEX_DIGITS[value & 0x0F];
        }
        return hex;
    }

    const auto &crc_table() {
        return CRC::CRC_32_MPEG2();
    }

    uint32_t read_u32_le(const std::span<const std::byte> buffer, const std::size_t offset) {
        uint32_t value = 0;
        if (offset + sizeof(value) <= buffer.size()) {
            std::memcpy(&value, buffer.data() + offset, sizeof(value));
        }
        return value;
    }
}

std::string Sha256Digest::hex_value() const {
    return bytes_to_hex(bytes);
}

Sha256Digest sha256(const std::span<const std::byte> data) {
    Sha256Digest digest;
    const auto *begin = reinterpret_cast<const uint8_t *>(data.data());
    const auto *end = begin + data.size();
    std::array<unsigned char, SHA256_HASH_SIZE> hash{};
    picosha2::hash256(begin, end, hash.begin(), hash.end());
    for (std::size_t i = 0; i < SHA256_HASH_SIZE; ++i) {
        digest.bytes[i] = std::byte{hash[i]};
    }
    return digest;
}

uint32_t crc32_mpeg2(const std::span<const std::byte> data, const uint32_t seed) {
    if (seed == 0) {
        return CRC::Calculate(data.data(), data.size(), crc_table());
    }
    const uint8_t seed_bytes[4] = {
        static_cast<uint8_t>(seed & 0xFFu),
        static_cast<uint8_t>((seed >> 8) & 0xFFu),
        static_cast<uint8_t>((seed >> 16) & 0xFFu),
        static_cast<uint8_t>((seed >> 24) & 0xFFu)
    };
    const uint32_t crc = CRC::Calculate(seed_bytes, sizeof(seed_bytes), crc_table());
    return CRC::Calculate(data.data(), data.size(), crc_table(), crc);
}

uint32_t crc32_mpeg2_concat(const std::span<const std::byte> first,
                            const std::span<const std::byte> second) {
    const uint32_t crc = CRC::Calculate(first.data(), first.size(), crc_table());
    return CRC::Calculate(second.data(), second.size(), crc_table(), crc);
}

uint32_t packet_crc32(const std::span<const std::byte> header,
                      const std::span<const std::byte> payload,
                      const std::size_t crc_offset,
                      const std::size_t crc_size) {
    const auto &table = crc_table();
    uint32_t crc = CRC::Calculate(header.data(), crc_offset, table);
    if (crc_size == 4) {
        constexpr uint8_t zeros[4] = {0, 0, 0, 0};
        crc = CRC::Calculate(zeros, sizeof(zeros), table, crc);
    }
    if (const std::size_t after_crc = crc_offset + crc_size; after_crc < header.size()) {
        crc = CRC::Calculate(header.data() + after_crc, header.size() - after_crc, table, crc);
    }
    return CRC::Calculate(payload.data(), payload.size(), table, crc);
}

uint32_t packet_xxhash32(const std::span<const std::byte> header,
                         const std::span<const std::byte> payload,
                         const std::size_t crc_offset,
                         const std::size_t crc_size) {
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, header.data(), crc_offset);
    if (crc_size == 4) {
        constexpr uint8_t zeros[4] = {0, 0, 0, 0};
        XXH32_update(&state, zeros, sizeof(zeros));
    }
    if (const std::size_t after_crc = crc_offset + crc_size; after_crc < header.size()) {
        XXH32_update(&state, header.data() + after_crc, header.size() - after_crc);
    }
    XXH32_update(&state, payload.data(), payload.size());
    return XXH32_digest(&state);
}

uint32_t packet_checksum(const std::span<const std::byte> header,
                         const std::span<const std::byte> payload,
                         const std::size_t crc_offset,
                         const HashAlgorithm algo,
                         const std::size_t crc_size) {
    if (algo == HashAlgorithm::XXHash32) {
        return packet_xxhash32(header, payload, crc_offset, crc_size);
    }
    return packet_crc32(header, payload, crc_offset, crc_size);
}

bool verify_packet_crc32(const std::span<const std::byte> header,
                         const std::span<const std::byte> payload,
                         const std::size_t crc_offset,
                         const std::size_t crc_size) {
    if (crc_size != 4 || crc_offset + 4 > header.size()) {
        return false;
    }
    const uint32_t stored = read_u32_le(header, crc_offset);
    return stored == packet_crc32(header, payload, crc_offset, crc_size);
}
