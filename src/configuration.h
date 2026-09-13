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

// =============================================================================
// ADJUSTABLE OPTIONS (YOU CAN CHANGE)
// =============================================================================
constexpr int FRAME_WIDTH = 3840;
constexpr int FRAME_HEIGHT = 2160;
constexpr int FRAME_FPS = 30;
constexpr int FRAME_BITRATE = 35000;
constexpr int FRAME_WIDTH_STREAM = FRAME_WIDTH / 2;
constexpr int FRAME_HEIGHT_STREAM = FRAME_HEIGHT / 2;
constexpr std::size_t CHUNK_SIZE_BYTES = 1024ull * 1024ull; // 1 MiB
constexpr std::size_t SYMBOL_SIZE_BYTES = 256;
constexpr double REPAIR_OVERHEAD = 5.00;
constexpr bool INCLUDE_SOURCE = true;
constexpr int BITS_PER_BLOCK = 1;
constexpr double COEFFICIENT_STRENGTH = 500.0;

// =============================================================================
// FIXED ARGUMENTS (DO NOT CHANGE)
// =============================================================================

// the decoder assumes a codec that preserves Y-plane bytes exactly.
inline constexpr const char *VIDEO_CODEC = "ffv1";

constexpr std::size_t CRYPTO_PLAIN_SIZE_HEADER = 4;
constexpr std::size_t CRYPTO_AEAD_TAG_BYTES = 16;
constexpr std::size_t CHUNK_SIZE_PLAIN_MAX_ENCRYPTED =
        CHUNK_SIZE_BYTES - CRYPTO_PLAIN_SIZE_HEADER - CRYPTO_AEAD_TAG_BYTES;

enum PacketFlag : uint8_t {
    FLAG_REPAIR_SYMBOL = 1 << 0,
    FLAG_LAST_CHUNK = 1 << 1,
    FLAG_ENCRYPTED = 1 << 2,
    FLAG_USE_XXHASH = 1 << 3,
};

constexpr uint32_t MAGIC_ID = 0x59544653;
constexpr uint8_t VERSION_ID = 1;
constexpr uint8_t VERSION_ID_V2 = 2;

constexpr std::size_t MAGIC_SIZE = 4;
constexpr std::size_t VERSION_SIZE = 1;
constexpr std::size_t FLAGS_SIZE = 1;
constexpr std::size_t FILE_ID_SIZE = 16;
constexpr std::size_t CHUNK_INDEX_SIZE = 4;
constexpr std::size_t CHUNK_SIZE_SIZE = 4;
constexpr std::size_t SYMBOL_SIZE_SIZE = 2;
constexpr std::size_t K_SIZE = 4;
constexpr std::size_t ESI_SIZE = 4;
constexpr std::size_t PAYLOAD_LEN_SIZE = 2;
constexpr std::size_t ORIGINAL_SIZE_SIZE = 4;
constexpr std::size_t CRC_SIZE = 4;

constexpr std::size_t MAGIC_OFF = 0;
constexpr std::size_t VERSION_OFF = MAGIC_OFF + MAGIC_SIZE;
constexpr std::size_t FLAGS_OFF = VERSION_OFF + VERSION_SIZE;
constexpr std::size_t FILE_ID_OFF = FLAGS_OFF + FLAGS_SIZE;
constexpr std::size_t CHUNK_INDEX_OFF = FILE_ID_OFF + FILE_ID_SIZE;
constexpr std::size_t CHUNK_SIZE_OFF = CHUNK_INDEX_OFF + CHUNK_INDEX_SIZE;
constexpr std::size_t SYMBOL_SIZE_OFF = CHUNK_SIZE_OFF + CHUNK_SIZE_SIZE;
constexpr std::size_t K_OFF = SYMBOL_SIZE_OFF + SYMBOL_SIZE_SIZE;
constexpr std::size_t ESI_OFF = K_OFF + K_SIZE;
constexpr std::size_t PAYLOAD_LEN_OFF = ESI_OFF + ESI_SIZE;
constexpr std::size_t CRC_OFF = PAYLOAD_LEN_OFF + PAYLOAD_LEN_SIZE;
constexpr std::size_t HEADER_SIZE = CRC_OFF + CRC_SIZE;

// V2 header: original_size inserted between PAYLOAD_LEN and CRC (fixes small-file padding)
constexpr std::size_t ORIGINAL_SIZE_OFF = PAYLOAD_LEN_OFF + PAYLOAD_LEN_SIZE;
constexpr std::size_t CRC_OFF_V2 = ORIGINAL_SIZE_OFF + ORIGINAL_SIZE_SIZE;
constexpr std::size_t HEADER_SIZE_V2 = CRC_OFF_V2 + CRC_SIZE;
constexpr std::size_t PACKET_SIZE = HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES;

using FileId = std::array<std::byte, FILE_ID_SIZE>;
