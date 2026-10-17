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

#include "encoder.h"

#include "libs/wirehair/wirehair.h"
#include "wirehair_init.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace {
struct WirehairCodecDeleter {
  void operator()(const WirehairCodec codec) const {
    wirehair_free(codec);
  }
};

using WirehairCodecPtr = std::unique_ptr<std::remove_pointer_t<WirehairCodec>, WirehairCodecDeleter>;

uint32_t compute_num_source_symbols(const std::size_t data_size, const std::size_t symbol_size) {
  return static_cast<uint32_t>((data_size + symbol_size - 1) / symbol_size);
}

uint32_t compute_repair_count(const uint32_t num_source, const double overhead) {
  return static_cast<uint32_t>(std::ceil(static_cast<double>(num_source) * overhead));
}

uint8_t build_flags(const uint32_t block_id, const uint32_t num_source, const bool is_last_chunk, const bool encrypted,
                    const HashAlgorithm algo) {
  uint8_t flags = 0;
  if (block_id > num_source) {
    flags |= FLAG_REPAIR_SYMBOL;
  }
  if (is_last_chunk) {
    flags |= FLAG_LAST_CHUNK;
  }
  if (encrypted) {
    flags |= FLAG_ENCRYPTED;
  }
  if (algo == HashAlgorithm::XXHash32) {
    flags |= FLAG_USE_XXHASH;
  }
  return flags;
}
}

Encoder::Encoder(const FileId file_id, const HashAlgorithm hash_algo) : file_id_(file_id), hash_algo_(hash_algo) {
}

std::pair<std::vector<Packet>, ChunkManifestEntry> Encoder::encode_chunk(const uint32_t chunk_index,
                                                                         const std::span<const std::byte> chunk_data,
                                                                         const bool is_last_chunk,
                                                                         const bool encrypted) const {
  ensure_wirehair_initialized();

  if (chunk_data.size() > CHUNK_SIZE_BYTES) {
    throw std::runtime_error("chunk_data larger than CHUNK_SIZE_BYTES");
  }

  constexpr std::size_t min_size = SYMBOL_SIZE_BYTES * 2;
  std::vector<std::byte> padded_data;
  std::span<const std::byte> data_to_encode = chunk_data;
  if (chunk_data.size() < min_size) {
    padded_data.assign(chunk_data.begin(), chunk_data.end());
    padded_data.resize(min_size, std::byte{0});
    data_to_encode = padded_data;
  }

  const auto chunk_size = static_cast<uint32_t>(data_to_encode.size());
  constexpr auto symbol_size = static_cast<uint16_t>(SYMBOL_SIZE_BYTES);
  const uint32_t num_source = compute_num_source_symbols(data_to_encode.size(), SYMBOL_SIZE_BYTES);

  ChunkManifestEntry manifest;
  manifest.chunk_index = chunk_index;
  manifest.chunk_size = chunk_size;
  manifest.original_size = static_cast<uint32_t>(chunk_data.size());
  manifest.symbol_size = symbol_size;
  manifest.num_source_symbols = num_source;
  manifest.sha256 = sha256(chunk_data);

  static thread_local WirehairCodecPtr reusable_codec;
  const WirehairCodec codec = wirehair_encoder_create(
      reusable_codec.release(), reinterpret_cast<const uint8_t*>(data_to_encode.data()), chunk_size, symbol_size);
  if (!codec) {
    throw std::runtime_error("wirehair_encoder_create() failed");
  }
  reusable_codec.reset(codec);

  const uint32_t repair_count = compute_repair_count(num_source, REPAIR_OVERHEAD);
  const uint32_t first_block_id = INCLUDE_SOURCE ? 1u : num_source + 1u;
  const uint32_t last_block_id = num_source + repair_count;

  std::vector<Packet> packets;
  packets.reserve(last_block_id - first_block_id + 1);

  PacketHeader header;
  header.file_id = file_id_;
  header.chunk_index = chunk_index;
  header.chunk_size = chunk_size;
  header.original_size = manifest.original_size;
  header.symbol_size = symbol_size;
  header.k = num_source;

  for (uint32_t block_id = first_block_id; block_id <= last_block_id; ++block_id) {
    Packet& packet = packets.emplace_back();
    auto* payload_dest = reinterpret_cast<uint8_t*>(packet.bytes.data() + HEADER_SIZE_V2);
    uint32_t write_len = 0;
    if (wirehair_encode(codec, block_id, payload_dest, SYMBOL_SIZE_BYTES, &write_len) != Wirehair_Success) {
      throw std::runtime_error("wirehair_encode() failed");
    }

    header.flags = build_flags(block_id, num_source, is_last_chunk, encrypted, hash_algo_);
    header.esi = block_id;
    header.payload_len = static_cast<uint16_t>(write_len);

    const std::span<const std::byte> payload(packet.bytes.data() + HEADER_SIZE_V2, SYMBOL_SIZE_BYTES);
    write_packet_header(std::span(packet.bytes.data(), HEADER_SIZE_V2), header, payload, hash_algo_);
  }

  return {std::move(packets), manifest};
}
