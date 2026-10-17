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

#include "frame_layout.h"

#include "configuration.h"
#include "dct_common.h"

#include <algorithm>
#include <cstring>

FrameLayout compute_frame_layout() {
  return compute_frame_layout(FRAME_WIDTH, FRAME_HEIGHT);
}

FrameLayout compute_frame_layout(const int width, const int height) {
  FrameLayout layout{};
  layout.frame_width = width;
  layout.frame_height = height;
  layout.blocks_per_row = width / 8;
  layout.blocks_per_col = height / 8;
  layout.total_blocks = layout.blocks_per_row * layout.blocks_per_col;
  layout.bits_per_frame = layout.total_blocks * BITS_PER_BLOCK;
  layout.bytes_per_frame = layout.bits_per_frame / 8;
  return layout;
}

int packets_per_frame(const FrameLayout& layout) {
  return static_cast<int>(static_cast<std::size_t>(layout.bytes_per_frame) / PACKET_SIZE);
}

std::size_t max_packet_bytes_per_frame() {
  return static_cast<std::size_t>(compute_frame_layout().bytes_per_frame);
}

void embed_data_into_plane(uint8_t* plane, const int stride, const FrameLayout& layout,
                           const std::span<const std::byte> data) {
  const auto& blocks = get_precomputed_blocks();
  const auto& patterns = blocks.patterns;

  const std::size_t total_bits = data.size() * 8;
  const int active_blocks = static_cast<int>(
      std::min(static_cast<std::size_t>(layout.total_blocks), (total_bits + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK));
  const auto* src = reinterpret_cast<const uint8_t*>(data.data());
  const int blocks_per_row = layout.blocks_per_row;

  const int covered_rows = blocks_per_row > 0 ? (active_blocks / blocks_per_row) * 8 : 0;
  const int covered_width = blocks_per_row * 8;
  if (covered_width < layout.frame_width) {
    for (int y = 0; y < covered_rows; ++y) {
      std::memset(plane + y * stride + covered_width, 128, layout.frame_width - covered_width);
    }
  }
  for (int y = covered_rows; y < layout.frame_height; ++y) {
    std::memset(plane + y * stride, 128, layout.frame_width);
  }

  for (int block_idx = 0; block_idx < active_blocks; ++block_idx) {
    const int base_x = (block_idx % blocks_per_row) * 8;
    const int base_y = (block_idx / blocks_per_row) * 8;

    const std::size_t bit_start = static_cast<std::size_t>(block_idx) * BITS_PER_BLOCK;
    const std::size_t bit_end = std::min(bit_start + BITS_PER_BLOCK, total_bits);

    int pattern = 0;
    for (std::size_t bit_index = bit_start; bit_index < bit_end; ++bit_index) {
      const int bit_pos = 7 - static_cast<int>(bit_index % 8);
      pattern = (pattern << 1) | ((src[bit_index / 8] >> bit_pos) & 1);
    }
    pattern <<= BITS_PER_BLOCK - static_cast<int>(bit_end - bit_start);

    const auto& block = patterns[pattern];
    for (int y = 0; y < 8; ++y) {
      std::memcpy(plane + (base_y + y) * stride + base_x, block[y], 8);
    }
  }
}
