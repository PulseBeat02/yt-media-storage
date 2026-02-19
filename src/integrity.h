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
#include <string>

constexpr size_t SHA256_HASH_SIZE = 32;

enum class HashAlgorithm {
    CRC32,
    XXHash32,
};

struct Sha256Digest {
    std::array<std::byte, 32> bytes{};

    [[nodiscard]] std::string hexValue() const;

    bool operator==(const Sha256Digest &sha256) const = default;
};

uint32_t crc32c(std::span<const std::byte> data, uint32_t seed = 0);

uint32_t crc32c_concat(std::span<const std::byte> first,
                       std::span<const std::byte> second,
                       uint32_t seed = 0);

uint32_t packet_crc32c(std::span<const std::byte> header,
                       std::span<const std::byte> payload,
                       std::size_t crc_offset,
                       std::size_t crc_size = 4);

bool verify_packet_crc32c(std::span<const std::byte> header,
                          std::span<const std::byte> payload,
                          std::size_t crc_offset,
                          std::size_t crc_size = 4);

uint32_t xxhash32_packet(std::span<const std::byte> header,
                         std::span<const std::byte> payload,
                         std::size_t crc_offset,
                         std::size_t crc_size = 4);

uint32_t packet_checksum(std::span<const std::byte> header,
                         std::span<const std::byte> payload,
                         std::size_t crc_offset,
                         HashAlgorithm algo,
                         std::size_t crc_size = 4);

Sha256Digest sha256(std::span<const std::byte> data);
