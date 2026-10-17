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

#ifndef MEDIA_STORAGE_CHUNKER_H
#define MEDIA_STORAGE_CHUNKER_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

inline constexpr std::size_t AUTOMATIC_CHUNK_SIZE = 0;

struct chunk_slice_type {
  std::size_t offset = 0;
  std::size_t length = 0;
};

struct chunked_storage_data {
  std::vector<std::byte> storage;
  std::vector<chunk_slice_type> chunks;
};

[[nodiscard]] chunked_storage_data chunk_byte_data(std::span<const std::byte> data);

[[nodiscard]] chunked_storage_data chunk_file(const char* path, std::size_t chunk_size = AUTOMATIC_CHUNK_SIZE);

[[nodiscard]] inline std::span<const std::byte> chunk_span(const chunked_storage_data& chunked_data,
                                                           const std::size_t index) {
  const auto& [offset, length] = chunked_data.chunks[index];
  return {chunked_data.storage.data() + offset, length};
}

class file_chunk_reader {
public:
  explicit file_chunk_reader(const char* path, std::size_t chunk_size = AUTOMATIC_CHUNK_SIZE);

  ~file_chunk_reader();

  file_chunk_reader(const file_chunk_reader&) = delete;

  file_chunk_reader& operator=(const file_chunk_reader&) = delete;

  file_chunk_reader(file_chunk_reader&&) = delete;

  file_chunk_reader& operator=(file_chunk_reader&&) = delete;

  [[nodiscard]] std::size_t chunk_count() const {
    return chunk_count_;
  }

  [[nodiscard]] std::size_t file_size() const {
    return file_size_;
  }

  [[nodiscard]] std::size_t chunk_size() const {
    return chunk_size_;
  }

  [[nodiscard]] bool is_mapped() const {
    return mapped_ != nullptr;
  }

  [[nodiscard]] std::vector<std::byte> read_chunk(std::size_t index) const;

  [[nodiscard]] std::span<const std::byte> chunk_view(std::size_t index) const;

private:
  std::size_t file_size_ = 0;
  std::size_t chunk_size_;
  std::size_t chunk_count_ = 0;
  const std::byte* mapped_ = nullptr;
#if defined(_WIN32)
  void* file_handle_ = nullptr;
  void* mapping_handle_ = nullptr;
#else
  int file_descriptor_ = -1;
#endif
  mutable std::ifstream file_;
  mutable std::size_t file_position_ = 0;

  void map_file(const char* path);

  [[nodiscard]] chunk_slice_type chunk_slice(std::size_t index) const;
};

#endif
