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

#include "encoder.h"

#include "configuration.h"
#include "libs/wirehair/wirehair.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

static std::once_flag ensure_init;

static void ensureWirehairInit() {
    std::call_once(ensure_init, [] {
        if (const WirehairResult result = wirehair_init(); result != Wirehair_Success) {
            throw std::runtime_error("wirehair_init failed");
        }
    });
}

static void writeByte(std::span<std::byte> buffer, const std::size_t offset, const uint8_t value) {
    buffer[offset] = std::byte{value};
}

static void writeU16LE(std::span<std::byte> buffer, const std::size_t offset, const uint16_t value) {
    std::byte *dest = buffer.data() + offset;
    std::memcpy(dest, &value, sizeof(value));
}

static void writeU32LE(std::span<std::byte> buffer, const std::size_t offset, const uint32_t value) {
    std::byte *dest = buffer.data() + offset;
    std::memcpy(dest, &value, sizeof(value));
}

static uint32_t computeNumSourceSymbols(const std::size_t dataSize, const std::size_t symbolSize) {
    const std::size_t count = (dataSize + symbolSize - 1) / symbolSize;
    return static_cast<uint32_t>(count);
}

static uint32_t computeRepairCount(const uint32_t numSource, const double overhead) {
    const double repairDouble = static_cast<double>(numSource) * overhead;
    return static_cast<uint32_t>(std::ceil(repairDouble));
}

static uint8_t buildFlags(const uint32_t blockId, const uint32_t numSource, const bool isLastChunk,
                          const bool encrypted) {
    uint8_t flags = None;
    if (blockId > numSource) {
        flags |= IsRepairSymbol;
    }
    if (isLastChunk) {
        flags |= LastChunk;
    }
    if (encrypted) {
        flags |= Encrypted;
    }
    return flags;
}


Encoder::Encoder(const FileId file_id, const HashAlgorithm hash_algo)
    : id(file_id), algo_(hash_algo) {
}

void Encoder::write_packet_header(
    const std::span<std::byte> dest,
    const uint32_t chunk_index,
    const uint32_t chunk_size,
    const uint32_t original_size,
    const uint16_t symbol_size,
    const uint32_t num_source,
    const uint32_t block_id,
    const uint16_t payload_length,
    const uint8_t flags,
    const std::span<const std::byte> payload) const {
    writeU32LE(dest, MAGIC_OFF, MAGIC_ID);
    writeByte(dest, VERSION_OFF, VERSION_ID_V2);
    writeByte(dest, FLAGS_OFF, flags);

    std::memcpy(dest.data() + FILE_ID_OFF, id.data(), id.size());
    writeU32LE(dest, CHUNK_INDEX_OFF, chunk_index);
    writeU32LE(dest, CHUNK_SIZE_OFF, chunk_size);
    writeU32LE(dest, ORIGINAL_SIZE_OFF, original_size);
    writeU16LE(dest, SYMBOL_SIZE_OFF, symbol_size);
    writeU32LE(dest, K_OFF, num_source);
    writeU32LE(dest, ESI_OFF, block_id);
    writeU16LE(dest, PAYLOAD_LEN_OFF, payload_length);
    writeU32LE(dest, CRC_OFF_V2, 0);

    const std::span<const std::byte> headerSpan(dest.data(), HEADER_SIZE_V2);
    const uint32_t crc = packet_checksum(headerSpan, payload, CRC_OFF_V2, algo_, CRC_SIZE);
    writeU32LE(dest, CRC_OFF_V2, crc);
}

std::pair<std::vector<Packet>, ChunkManifestEntry>
Encoder::encode_chunk(
    const uint32_t chunk_index,
    const std::span<const std::byte> chunk_data,
    const bool is_last_chunk,
    const bool encrypted) const {
    ensureWirehairInit();

    if (chunk_data.size() > CHUNK_SIZE_BYTES) {
        throw std::runtime_error("chunkData larger than CHUNK_SIZE_BYTES");
    }

    constexpr std::size_t min_size = SYMBOL_SIZE_BYTES * 2;
    std::vector<std::byte> padded_data;
    std::span<const std::byte> data_to_encode = chunk_data;

    if (chunk_data.size() < min_size) {
        padded_data.assign(chunk_data.begin(), chunk_data.end());
        padded_data.resize(min_size, std::byte{0});
        data_to_encode = std::span<const std::byte>(padded_data);
    }

    const auto chunkSize = static_cast<uint32_t>(data_to_encode.size());
    constexpr auto symbolSize = static_cast<uint16_t>(SYMBOL_SIZE_BYTES);
    const uint32_t numSource = computeNumSourceSymbols(data_to_encode.size(), SYMBOL_SIZE_BYTES);

    ChunkManifestEntry manifest;
    manifest.chunk_index = chunk_index;
    manifest.chunk_size = chunkSize;
    manifest.original_size = static_cast<uint32_t>(chunk_data.size());
    manifest.T = symbolSize;
    manifest.N = numSource;
    manifest.sha256 = sha256(chunk_data);

    const auto* msgData = reinterpret_cast<const uint8_t*>(data_to_encode.data());
    const auto msgSize = static_cast<uint32_t>(data_to_encode.size());
    constexpr auto symbolSizeU32 = static_cast<uint32_t>(SYMBOL_SIZE_BYTES);
    const WirehairCodec codec = wirehair_encoder_create(nullptr, msgData, msgSize, symbolSizeU32);
    if (!codec) {
        throw std::runtime_error("wirehair_encoder_create() failed");
    }

    const uint32_t repairCount = computeRepairCount(numSource, REPAIR_OVERHEAD);
    constexpr uint32_t firstBlockId = INCLUDE_SOURCE ? 1u : (numSource + 1u);
    const uint32_t lastBlockId = numSource + repairCount;

    const uint32_t sourceCount = INCLUDE_SOURCE ? numSource : 0u;
    const uint32_t packetCount = sourceCount + repairCount;

    std::vector<Packet> packets;
    packets.reserve(packetCount);

    for (uint32_t blockId = firstBlockId; blockId <= lastBlockId; ++blockId) {
        Packet packet;
        packet.bytes.resize(HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES);

        auto *payload_dest = reinterpret_cast<uint8_t *>(packet.bytes.data() + HEADER_SIZE_V2);
        uint32_t writeLen = 0;
        if (const WirehairResult result = wirehair_encode(codec, blockId, payload_dest, SYMBOL_SIZE_BYTES, &writeLen); result != Wirehair_Success) {
            wirehair_free(codec);
            throw std::runtime_error("wirehair_encode() failed");
        }

        uint8_t flags = buildFlags(blockId, numSource, is_last_chunk, encrypted);
        if (algo_ == HashAlgorithm::XXHash32) {
            flags |= UseXXHash;
        }
        const auto payloadLen = static_cast<uint16_t>(writeLen);
        const std::span<const std::byte> payload_span(packet.bytes.data() + HEADER_SIZE_V2, writeLen);

        write_packet_header(
            std::span(packet.bytes.data(), HEADER_SIZE_V2),
            chunk_index, chunkSize, manifest.original_size, symbolSize, numSource, blockId, payloadLen, flags, payload_span);

        packets.push_back(std::move(packet));
    }

    wirehair_free(codec);

    return {std::move(packets), manifest};
}
