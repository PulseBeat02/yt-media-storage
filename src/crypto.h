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
#include <span>
#include <vector>

#include "configuration.h"

constexpr std::size_t CRYPTO_KEY_BYTES = 32u;

using CryptoKey = std::array<std::byte, CRYPTO_KEY_BYTES>;

[[nodiscard]] FileId generate_file_id();

[[nodiscard]] uint32_t read_plain_size_from_header(std::span<const std::byte> chunk);

[[nodiscard]] CryptoKey derive_key(std::span<const std::byte> password, std::span<const std::byte, FILE_ID_SIZE> salt);

[[nodiscard]] std::vector<std::byte> encrypt_chunk(std::span<const std::byte> plain,
                                                   std::span<const std::byte, CRYPTO_KEY_BYTES> key,
                                                   std::span<const std::byte, FILE_ID_SIZE> file_id,
                                                   uint32_t chunk_index);

[[nodiscard]] std::vector<std::byte> decrypt_chunk(std::span<const std::byte> chunk_from_decoder,
                                                   std::span<const std::byte, CRYPTO_KEY_BYTES> key,
                                                   std::span<const std::byte, FILE_ID_SIZE> file_id,
                                                   uint32_t chunk_index);

void decrypt_chunk_into(std::span<std::byte> out, std::span<const std::byte> chunk_from_decoder,
                        std::span<const std::byte, CRYPTO_KEY_BYTES> key,
                        std::span<const std::byte, FILE_ID_SIZE> file_id, uint32_t chunk_index);

void secure_zero(std::span<std::byte> data);
