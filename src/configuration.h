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

#ifndef MEDIA_STORAGE_CONFIGURATION_H
#define MEDIA_STORAGE_CONFIGURATION_H

#include <array>
#include <cstddef>
#include <cstdint>

// settings you may change
constexpr int FRAME_WIDTH = 3840;
constexpr int FRAME_HEIGHT = 2160;
constexpr int FRAMES_PER_SECOND = 30;
constexpr int FRAME_BITRATE = 35000;
constexpr int FRAME_WIDTH_STREAM = FRAME_WIDTH / 2;
constexpr int FRAME_HEIGHT_STREAM = FRAME_HEIGHT / 2;
constexpr std::size_t CHUNK_SIZE_BYTES = 1024ull * 1024ull;
constexpr std::size_t SYMBOL_SIZE_BYTES = 256;
constexpr double REPAIR_OVERHEAD = 0.25;
constexpr bool INCLUDE_SOURCE = true;
constexpr int BITS_PER_BLOCK = 1;
constexpr double COEFFICIENT_STRENGTH = 500.0;

// fixed by the video format

// the decoder assumes a codec that preserves Y-plane bytes exactly
inline constexpr const char* VIDEO_CODEC = "ffv1";

constexpr std::size_t CRYPTO_PLAIN_SIZE_HEADER = 4;
constexpr std::size_t CRYPTO_AEAD_TAG_BYTES = 16;
constexpr std::size_t CHUNK_SIZE_PLAIN_MAXIMUM_ENCRYPTED =
    CHUNK_SIZE_BYTES - CRYPTO_PLAIN_SIZE_HEADER - CRYPTO_AEAD_TAG_BYTES;

enum packet_flag : uint8_t {
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
constexpr std::size_t SOURCE_SYMBOL_COUNT_SIZE = 4;
constexpr std::size_t ESI_SIZE = 4;
constexpr std::size_t PAYLOAD_LENGTH_SIZE = 2;
constexpr std::size_t ORIGINAL_SIZE_SIZE = 4;
constexpr std::size_t CRC_SIZE = 4;

constexpr std::size_t MAGIC_OFFSET = 0;
constexpr std::size_t VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
constexpr std::size_t FLAGS_OFFSET = VERSION_OFFSET + VERSION_SIZE;
constexpr std::size_t FILE_ID_OFFSET = FLAGS_OFFSET + FLAGS_SIZE;
constexpr std::size_t CHUNK_INDEX_OFFSET = FILE_ID_OFFSET + FILE_ID_SIZE;
constexpr std::size_t CHUNK_SIZE_OFFSET = CHUNK_INDEX_OFFSET + CHUNK_INDEX_SIZE;
constexpr std::size_t SYMBOL_SIZE_OFFSET = CHUNK_SIZE_OFFSET + CHUNK_SIZE_SIZE;
constexpr std::size_t SOURCE_SYMBOL_COUNT_OFFSET = SYMBOL_SIZE_OFFSET + SYMBOL_SIZE_SIZE;
constexpr std::size_t ESI_OFFSET = SOURCE_SYMBOL_COUNT_OFFSET + SOURCE_SYMBOL_COUNT_SIZE;
constexpr std::size_t PAYLOAD_LENGTH_OFFSET = ESI_OFFSET + ESI_SIZE;
constexpr std::size_t CRC_OFFSET = PAYLOAD_LENGTH_OFFSET + PAYLOAD_LENGTH_SIZE;
constexpr std::size_t HEADER_SIZE = CRC_OFFSET + CRC_SIZE;

// version 2 stores the original size before the crc, so decoding drops the padding of small files
constexpr std::size_t ORIGINAL_SIZE_OFFSET = PAYLOAD_LENGTH_OFFSET + PAYLOAD_LENGTH_SIZE;
constexpr std::size_t CRC_OFFSET_V2 = ORIGINAL_SIZE_OFFSET + ORIGINAL_SIZE_SIZE;
constexpr std::size_t HEADER_SIZE_V2 = CRC_OFFSET_V2 + CRC_SIZE;
constexpr std::size_t PACKET_SIZE = HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES;

using file_id_type = std::array<std::byte, FILE_ID_SIZE>;

#endif
