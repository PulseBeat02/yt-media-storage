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

struct chunk_manifest_entry {
  uint32_t chunk_index = 0;
  uint32_t chunk_size = 0;
  uint32_t original_size = 0;
  sha256_digest sha256{};
  uint32_t source_symbol_count = 0;
  uint16_t symbol_size = 0;
};

enum class chunk_position {
  BEFORE_LAST,
  LAST,
};

enum class payload_encryption {
  PLAIN,
  ENCRYPTED,
};

[[nodiscard]] chunk_position position_of_chunk(std::size_t chunk_index, std::size_t chunk_count);

class encoder {
public:
  using file_id_type = ::file_id_type;

  explicit encoder(file_id_type file_id, hash_algorithm_type hash_algorithm = hash_algorithm_type::CRC32);

  [[nodiscard]] std::pair<std::vector<packet>, chunk_manifest_entry>
  encode_chunk(uint32_t chunk_index, std::span<const std::byte> chunk_data, chunk_position position,
               payload_encryption encryption = payload_encryption::PLAIN) const;

  [[nodiscard]] const file_id_type& file_id() const {
    return file_id_;
  }

  [[nodiscard]] hash_algorithm_type hash_algorithm() const {
    return hash_algorithm_;
  }

private:
  file_id_type file_id_;
  hash_algorithm_type hash_algorithm_;
};

#endif
