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

#include "integrity.h"
#include "configuration.h"

#include "libs/picosha2.h"
#include "libs/CRC.h"
#define XXH_INLINE_ALL
#include "libs/xxhash.h"

#include <array>
#include <cstring>

static std::string bytes_to_hex(const std::span<const std::byte> inputBytes) {
    std::string hexString(inputBytes.size() * 2, 0);
    auto outputPosition = hexString.data();
    for (const auto &currentByte: inputBytes) {
        const auto byteValue = std::to_integer<unsigned char>(currentByte);
        *outputPosition++ = SHA_CHARACTERS[byteValue >> 4];
        *outputPosition++ = SHA_CHARACTERS[byteValue & 0x0F];
    }
    return hexString;
}

std::string Sha256Digest::hexValue() const {
    return bytes_to_hex(std::span(bytes.data(), bytes.size()));
}

Sha256Digest sha256(const std::span<const std::byte> data) {
    Sha256Digest digest;
    const auto dataStart = reinterpret_cast<const uint8_t *>(data.data());
    const auto dataEnd = dataStart + data.size();
    std::array<unsigned char, SHA256_HASH_SIZE> hashBuffer{};
    picosha2::hash256(dataStart, dataEnd, hashBuffer.begin(), hashBuffer.end());
    for (size_t byteIndex = 0; byteIndex < SHA256_HASH_SIZE; ++byteIndex) {
        digest.bytes[byteIndex] = std::byte{hashBuffer[byteIndex]};
    }
    return digest;
}

uint32_t crc32c(const std::span<const std::byte> data, const uint32_t seed) {
    if (seed != 0) {
        const uint8_t seedBytes[4] = {
            static_cast<uint8_t>(seed & 0xFFu),
            static_cast<uint8_t>((seed >> 8) & 0xFFu),
            static_cast<uint8_t>((seed >> 16) & 0xFFu),
            static_cast<uint8_t>((seed >> 24) & 0xFFu)
        };
        const auto &table = CRC::CRC_32_MPEG2();
        uint32_t crc = CRC::Calculate(seedBytes, 4, table);
        crc = CRC::Calculate(data.data(),
                             data.size(), table, crc);
        return crc;
    }
    const auto dataPointer = reinterpret_cast<const uint8_t *>(data.data());
    return CRC::Calculate(dataPointer, data.size(), CRC::CRC_32_MPEG2());
}

uint32_t crc32c_concat(const std::span<const std::byte> first,
                       const std::span<const std::byte> second,
                       uint32_t /*unused_seed*/) {
    const auto &table = CRC::CRC_32_MPEG2();
    uint32_t crc = CRC::Calculate(first.data(),
                                  first.size(), table);
    crc = CRC::Calculate(second.data(),
                         second.size(), table, crc);
    return crc;
}

uint32_t packet_crc32c(const std::span<const std::byte> header,
                       const std::span<const std::byte> payload,
                       const std::size_t crc_offset,
                       const std::size_t crc_size) {
    const auto &table = CRC::CRC_32_MPEG2();
    const auto *hdr = reinterpret_cast<const uint8_t *>(header.data());
    uint32_t crc = CRC::Calculate(hdr, crc_offset, table);
    if (crc_size == 4) {
        constexpr uint8_t zeros[4] = {0, 0, 0, 0};
        crc = CRC::Calculate(zeros, 4, table, crc);
    }

    if (const std::size_t after_crc = crc_offset + crc_size; after_crc < header.size()) {
        crc = CRC::Calculate(hdr + after_crc, header.size() - after_crc, table, crc);
    }

    crc = CRC::Calculate(payload.data(),
                         payload.size(), table, crc);

    return crc;
}

uint32_t read_u32_le(const std::span<const std::byte> buffer, const std::size_t byteOffset) {
    uint32_t value = 0;
    if (byteOffset + 4 <= buffer.size()) {
        std::memcpy(&value, buffer.data() + byteOffset, 4);
    }
    return value;
}

bool verify_packet_crc32c(const std::span<const std::byte> header,
                          const std::span<const std::byte> payload,
                          const std::size_t crc_offset,
                          const std::size_t crc_size) {
    if (crc_size != 4) {
        return false;
    }
    if (crc_offset + 4 > header.size()) {
        return false;
    }

    const uint32_t storedChecksum = read_u32_le(header, crc_offset);
    const uint32_t computedChecksum = packet_crc32c(header, payload, crc_offset, crc_size);
    return storedChecksum == computedChecksum;
}

uint32_t xxhash32_packet(const std::span<const std::byte> header,
                         const std::span<const std::byte> payload,
                         const std::size_t crc_offset,
                         const std::size_t crc_size) {
    XXH32_state_t state;
    XXH32_reset(&state, 0);

    XXH32_update(&state, header.data(), crc_offset);

    if (crc_size == 4) {
        constexpr uint8_t zeros[4] = {0, 0, 0, 0};
        XXH32_update(&state, zeros, 4);
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
        return xxhash32_packet(header, payload, crc_offset, crc_size);
    }
    return packet_crc32c(header, payload, crc_offset, crc_size);
}
