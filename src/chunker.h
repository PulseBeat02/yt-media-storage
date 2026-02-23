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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <span>

struct ChunkSlice {
    std::size_t offset = 0;
    std::size_t length = 0;
};

struct ChunkedStorageData {
    std::vector<std::byte> storage;
    std::vector<ChunkSlice> chunks;
};

ChunkedStorageData chunkByteData(std::span<const std::byte> data);

ChunkedStorageData chunkFile(const char *path, std::size_t chunk_size = 0);

inline std::span<const std::byte> chunkSpan(const ChunkedStorageData &cs, std::size_t i) {
    const auto &[offset, length] = cs.chunks[i];
    return {cs.storage.data() + offset, length};
}

class FileChunkReader {
public:
    explicit FileChunkReader(const char *path, std::size_t chunk_size = 0);

    [[nodiscard]] std::size_t num_chunks() const { return num_chunks_; }
    [[nodiscard]] std::size_t file_size() const { return file_size_; }
    [[nodiscard]] std::size_t chunk_size() const { return chunk_size_; }

    [[nodiscard]] std::vector<std::byte> read_chunk(std::size_t index) const;

private:
    std::string path_;
    std::size_t file_size_;
    std::size_t chunk_size_;
    std::size_t num_chunks_;
};
