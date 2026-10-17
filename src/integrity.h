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

#ifndef MEDIA_STORAGE_INTEGRITY_H
#define MEDIA_STORAGE_INTEGRITY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

constexpr std::size_t SHA256_HASH_SIZE = 32;

enum class hash_algorithm_type {
  CRC32,
  XXHASH32,
};

struct sha256_digest {
  std::array<std::byte, SHA256_HASH_SIZE> bytes{};

  [[nodiscard]] std::string hexadecimal_value() const;

  bool operator==(const sha256_digest& other) const = default;
};

[[nodiscard]] sha256_digest sha256(std::span<const std::byte> data);

[[nodiscard]] uint32_t crc32_mpeg2(std::span<const std::byte> data, uint32_t seed = 0);

[[nodiscard]] uint32_t crc32_mpeg2_concat(std::span<const std::byte> first, std::span<const std::byte> second);

[[nodiscard]] uint32_t packet_crc32(std::span<const std::byte> header, std::span<const std::byte> payload,
                                    std::size_t crc_offset, std::size_t crc_size = 4);

[[nodiscard]] uint32_t packet_xxhash32(std::span<const std::byte> header, std::span<const std::byte> payload,
                                       std::size_t crc_offset, std::size_t crc_size = 4);

[[nodiscard]] uint32_t packet_checksum(std::span<const std::byte> header, std::span<const std::byte> payload,
                                       std::size_t crc_offset, hash_algorithm_type algorithm, std::size_t crc_size = 4);

[[nodiscard]] bool verify_packet_crc32(std::span<const std::byte> header, std::span<const std::byte> payload,
                                       std::size_t crc_offset, std::size_t crc_size = 4);

#endif
