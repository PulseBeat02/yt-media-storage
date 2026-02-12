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

#include "chunker.h"
#include "configuration.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

ChunkedStorageData chunkByteData(std::span<const std::byte> data) {
    ChunkedStorageData result;
    result.storage.assign(data.begin(), data.end());

    if (const std::size_t size = result.storage.size(); size == 0) {
        result.chunks.push_back(ChunkSlice{0, 0});
    } else {
        result.chunks.reserve((size + CHUNK_SIZE_BYTES - 1) / CHUNK_SIZE_BYTES);
        for (std::size_t off = 0; off < size; off += CHUNK_SIZE_BYTES) {
            const std::size_t len = (std::min<std::size_t>)(CHUNK_SIZE_BYTES, size - off);
            result.chunks.push_back(ChunkSlice{off, len});
        }
    }

    return result;
}

ChunkedStorageData chunkFile(const char *path, std::size_t chunk_size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("open failed");
    }

    const std::size_t size = file.tellg();
    file.seekg(0);

    ChunkedStorageData result;
    result.storage.resize(size);
    if (!file.read(reinterpret_cast<char *>(result.storage.data()), size)) {
        throw std::runtime_error("read failed");
    }

    const std::size_t cs = (chunk_size > 0) ? chunk_size : CHUNK_SIZE_BYTES;

    if (size == 0) {
        result.chunks.push_back(ChunkSlice{0, 0});
    } else {
        result.chunks.reserve((size + cs - 1) / cs);
        for (std::size_t off = 0; off < size; off += cs) {
            const std::size_t len = (std::min<std::size_t>)(cs, size - off);
            result.chunks.push_back(ChunkSlice{off, len});
        }
    }

    return result;
}
