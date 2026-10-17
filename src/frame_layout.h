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

#ifndef MEDIA_STORAGE_FRAME_LAYOUT_H
#define MEDIA_STORAGE_FRAME_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <span>

struct FrameLayout {
  int frame_width;
  int frame_height;
  int blocks_per_row;
  int blocks_per_column;
  int total_blocks;
  int bits_per_frame;
  int bytes_per_frame;
};

[[nodiscard]] FrameLayout compute_frame_layout();

[[nodiscard]] FrameLayout compute_frame_layout(int width, int height);

[[nodiscard]] int packets_per_frame(const FrameLayout& layout);

[[nodiscard]] std::size_t maximum_packet_bytes_per_frame();

void embed_data_into_plane(uint8_t* plane, int stride, const FrameLayout& layout, std::span<const std::byte> data);

#endif
