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
#include <cstdint>
#include <cstdlib>
#include <string>

// Video Parameters
constexpr int FRAME_WIDTH = 3840;
constexpr int FRAME_HEIGHT = 2160;
constexpr int FRAME_FPS = 30;

const std::string VIDEO_CODEC = "ffv1";
const std::string VIDEO_CONTAINER = "mkv";

// Encoding Parameters
constexpr size_t CHUNK_SIZE_BYTES = 1024ull * 1024ull; // 1 MiB
constexpr size_t CRYPTO_AEAD_TAG_BYTES = 16;
inline constexpr size_t CHUNK_SIZE_PLAIN_MAX_ENCRYPTED = CHUNK_SIZE_BYTES - 4 - CRYPTO_AEAD_TAG_BYTES;
constexpr size_t SYMBOL_SIZE_BYTES = 256;
constexpr double REPAIR_OVERHEAD = 1.00;
constexpr bool INCLUDE_SOURCE = true;
constexpr int BITS_PER_BLOCK = 1;
constexpr double COEFFICIENT_STRENGTH = 150.0;

enum Flags : uint8_t {
    None = 0,
    IsRepairSymbol = 1 << 0,
    LastChunk = 1 << 1,
    Encrypted = 1 << 2,
    UseXXHash = 1 << 3,
};

// Header Scheme
constexpr char SHA_CHARACTERS[] = "0123456789ABCDEF";

constexpr size_t CHUNK_SIZE = 1024;

constexpr uint32_t MAGIC_ID = 0x59544653;
constexpr uint8_t VERSION_ID = 1;
constexpr uint8_t VERSION_ID_V2 = 2;

constexpr size_t MAGIC_SIZE = 4;
constexpr size_t VERSION_SIZE = 1;
constexpr size_t FLAGS_SIZE = 1;
constexpr size_t FILE_ID_SIZE = 16;
constexpr size_t CHUNK_INDEX_SIZE = 4;
constexpr size_t CHUNK_SIZE_SIZE = 4;
constexpr size_t SYMBOL_SIZE_SIZE = 2;
constexpr size_t K_SIZE = 4;
constexpr size_t ESI_SIZE = 4;
constexpr size_t PAYLOAD_LEN_SIZE = 2;
constexpr size_t ORIGINAL_SIZE_SIZE = 4;
constexpr size_t CRC_SIZE = 4;

constexpr size_t MAGIC_OFF = 0;
constexpr size_t VERSION_OFF = MAGIC_OFF + MAGIC_SIZE;
constexpr size_t FLAGS_OFF = VERSION_OFF + VERSION_SIZE;
constexpr size_t FILE_ID_OFF = FLAGS_OFF + FLAGS_SIZE;
constexpr size_t CHUNK_INDEX_OFF = FILE_ID_OFF + FILE_ID_SIZE;
constexpr size_t CHUNK_SIZE_OFF = CHUNK_INDEX_OFF + CHUNK_INDEX_SIZE;
constexpr size_t SYMBOL_SIZE_OFF = CHUNK_SIZE_OFF + CHUNK_SIZE_SIZE;
constexpr size_t K_OFF = SYMBOL_SIZE_OFF + SYMBOL_SIZE_SIZE;
constexpr size_t ESI_OFF = K_OFF + K_SIZE;
constexpr size_t PAYLOAD_LEN_OFF = ESI_OFF + ESI_SIZE;
constexpr size_t CRC_OFF = PAYLOAD_LEN_OFF + PAYLOAD_LEN_SIZE;
constexpr size_t HEADER_SIZE = CRC_OFF + CRC_SIZE;

// V2 header: original_size inserted between PAYLOAD_LEN and CRC (fixes small-file padding)
constexpr size_t ORIGINAL_SIZE_OFF = PAYLOAD_LEN_OFF + PAYLOAD_LEN_SIZE;
constexpr size_t CRC_OFF_V2 = ORIGINAL_SIZE_OFF + ORIGINAL_SIZE_SIZE;
constexpr size_t HEADER_SIZE_V2 = CRC_OFF_V2 + CRC_SIZE;

// Frame Layout
struct FrameLayout {
    int frame_width;
    int frame_height;
    int blocks_per_row;
    int blocks_per_col;
    int total_blocks;
    int bits_per_frame;
    int bytes_per_frame;
};
