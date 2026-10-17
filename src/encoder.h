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

#ifndef MEDIA_STORAGE_ENCODER_H
#define MEDIA_STORAGE_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "configuration.h"
#include "integrity.h"
#include "packet_format.h"

struct ChunkManifestEntry {
  uint32_t chunk_index = 0;
  uint32_t chunk_size = 0;
  uint32_t original_size = 0;
  Sha256Digest sha256{};
  uint32_t num_source_symbols = 0;
  uint16_t symbol_size = 0;
};

class Encoder {
public:
  using FileId = ::FileId;

  explicit Encoder(FileId file_id, HashAlgorithm hash_algo = HashAlgorithm::CRC32);

  [[nodiscard]] std::pair<std::vector<Packet>, ChunkManifestEntry> encode_chunk(uint32_t chunk_index,
                                                                                std::span<const std::byte> chunk_data,
                                                                                bool is_last_chunk,
                                                                                bool encrypted = false) const;

  [[nodiscard]] const FileId& file_id() const {
    return file_id_;
  }

  [[nodiscard]] HashAlgorithm hash_algorithm() const {
    return hash_algo_;
  }

private:
  FileId file_id_;
  HashAlgorithm hash_algo_;
};

#endif
