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

#include "chunker.h"
#include "configuration.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
constexpr const char* FILE_OPEN_FAILURE_MESSAGE = "File could not be opened!";
constexpr const char* FILE_SIZE_FAILURE_MESSAGE = "Size of the file could not be determined!";
constexpr const char* FILE_READ_FAILURE_MESSAGE = "File could not be read!";

#if defined(_WIN32)
constexpr DWORD MAPPING_SIZE_OF_FILE = 0;
constexpr DWORD FILE_START_OFFSET = 0;
constexpr SIZE_T WHOLE_MAPPING = 0;
#else
constexpr off_t FILE_START_OFFSET = 0;
#endif

std::vector<chunk_slice_type> slice_into_chunks(const std::size_t size, const std::size_t chunk_size) {
  std::vector<chunk_slice_type> chunks;
  if (size == 0) {
    chunks.push_back(chunk_slice_type{0, 0});
    return chunks;
  }
  chunks.reserve((size + chunk_size - 1) / chunk_size);
  for (std::size_t offset = 0; offset < size; offset += chunk_size) {
    chunks.push_back(chunk_slice_type{offset, (std::min)(chunk_size, size - offset)});
  }
  return chunks;
}

std::size_t effective_chunk_size(const std::size_t chunk_size) {
  return chunk_size != AUTOMATIC_CHUNK_SIZE ? chunk_size : CHUNK_SIZE_BYTES;
}
}

chunked_storage_data chunk_byte_data(const std::span<const std::byte> data) {
  chunked_storage_data result;
  result.storage.assign(data.begin(), data.end());
  result.chunks = slice_into_chunks(result.storage.size(), CHUNK_SIZE_BYTES);
  return result;
}

chunked_storage_data chunk_file(const char* path, const std::size_t chunk_size) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error(FILE_OPEN_FAILURE_MESSAGE);
  }

  const std::streamoff end = file.tellg();
  if (end < 0) {
    throw std::runtime_error(FILE_SIZE_FAILURE_MESSAGE);
  }
  const auto size = static_cast<std::size_t>(end);
  file.seekg(0);

  chunked_storage_data result;
  result.storage.resize(size);
  if (size > 0 && !file.read(reinterpret_cast<char*>(result.storage.data()), static_cast<std::streamsize>(size))) {
    throw std::runtime_error(FILE_READ_FAILURE_MESSAGE);
  }
  result.chunks = slice_into_chunks(size, effective_chunk_size(chunk_size));
  return result;
}

file_chunk_reader::file_chunk_reader(const char* path, const std::size_t chunk_size)
    : chunk_size_(effective_chunk_size(chunk_size)), file_(path, std::ios::binary) {
  if (!file_) {
    throw std::runtime_error(FILE_OPEN_FAILURE_MESSAGE);
  }
  file_.seekg(0, std::ios::end);
  const std::streamoff end = file_.tellg();
  if (end < 0) {
    throw std::runtime_error(FILE_SIZE_FAILURE_MESSAGE);
  }
  file_size_ = static_cast<std::size_t>(end);
  file_.seekg(0);
  chunk_count_ = file_size_ == 0 ? 1 : (file_size_ + chunk_size_ - 1) / chunk_size_;

  if (file_size_ > 0) {
    map_file(path);
  }
}

void file_chunk_reader::map_file(const char* path) {
#if defined(_WIN32)
  HANDLE file =
      CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  HANDLE mapping =
      CreateFileMappingA(file, nullptr, PAGE_READONLY, MAPPING_SIZE_OF_FILE, MAPPING_SIZE_OF_FILE, nullptr);
  if (!mapping) {
    CloseHandle(file);
    return;
  }
  const void* view = MapViewOfFile(mapping, FILE_MAP_READ, FILE_START_OFFSET, FILE_START_OFFSET, WHOLE_MAPPING);
  if (!view) {
    CloseHandle(mapping);
    CloseHandle(file);
    return;
  }
  file_handle_ = file;
  mapping_handle_ = mapping;
  mapped_ = static_cast<const std::byte*>(view);
#else
  const int file_descriptor = ::open(path, O_RDONLY);
  if (file_descriptor < 0)
    return;
  const void* const view = ::mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, file_descriptor, FILE_START_OFFSET);
  if (view == MAP_FAILED) {
    ::close(file_descriptor);
    return;
  }
  file_descriptor_ = file_descriptor;
  mapped_ = static_cast<const std::byte*>(view);
#endif
}

file_chunk_reader::~file_chunk_reader() {
#if defined(_WIN32)
  if (mapped_)
    UnmapViewOfFile(mapped_);
  if (mapping_handle_)
    CloseHandle(mapping_handle_);
  if (file_handle_)
    CloseHandle(file_handle_);
#else
  if (mapped_)
    ::munmap(const_cast<std::byte*>(mapped_), file_size_);
  if (file_descriptor_ >= 0)
    ::close(file_descriptor_);
#endif
}

chunk_slice_type file_chunk_reader::chunk_slice(const std::size_t index) const {
  if (index >= chunk_count_) {
    const std::string message =
        "Chunk index " + std::to_string(index) + " must be below the chunk count " + std::to_string(chunk_count_) + "!";
    throw std::runtime_error(message);
  }
  const std::size_t offset = index * chunk_size_;
  if (offset >= file_size_) {
    return chunk_slice_type{offset, 0};
  }
  return chunk_slice_type{offset, (std::min)(chunk_size_, file_size_ - offset)};
}

std::span<const std::byte> file_chunk_reader::chunk_view(const std::size_t index) const {
  const auto [offset, length] = chunk_slice(index);
  if (length == 0) {
    return {};
  }
  if (!mapped_) {
    const std::string message = "Chunk views need a memory-mapped file!";
    throw std::runtime_error(message);
  }
  return {mapped_ + offset, length};
}

std::vector<std::byte> file_chunk_reader::read_chunk(const std::size_t index) const {
  const auto [offset, length] = chunk_slice(index);
  if (length == 0) {
    return {};
  }
  if (mapped_) {
    return {mapped_ + offset, mapped_ + offset + length};
  }

  if (offset != file_position_) {
    file_.seekg(static_cast<std::streamoff>(offset));
  }
  std::vector<std::byte> data(length);
  if (!file_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(length))) {
    throw std::runtime_error(FILE_READ_FAILURE_MESSAGE);
  }
  file_position_ = offset + length;
  return data;
}
