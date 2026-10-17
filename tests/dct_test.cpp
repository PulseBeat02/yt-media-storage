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

#include <gtest/gtest.h>

#include "configuration.h"
#include "dct_common.h"
#include "frame_layout.h"
#include "video_encoder.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

void expect_block_in_pixel_range(const uint8_t (&block)[BLOCK_SIZE][BLOCK_SIZE]) {
  for (int y = 0; y < BLOCK_SIZE; ++y) {
    for (int x = 0; x < BLOCK_SIZE; ++x) {
      EXPECT_LE(block[y][x], 255u);
    }
  }
}

void flatten_block(const uint8_t (&block)[BLOCK_SIZE][BLOCK_SIZE], float (&block_flat)[BLOCK_AREA]) {
  for (int y = 0; y < BLOCK_SIZE; ++y) {
    for (int x = 0; x < BLOCK_SIZE; ++x) {
      block_flat[y * BLOCK_SIZE + x] = static_cast<float>(block[y][x]);
    }
  }
}

}

TEST(dct_test, should_precompute_one_block_per_bit_pattern) {
  constexpr int expected_patterns = 1 << BITS_PER_BLOCK;
  EXPECT_EQ(precomputed_blocks::PATTERN_COUNT, expected_patterns);
}

TEST(dct_test, should_keep_precomputed_blocks_in_the_pixel_range) {
  const auto& [patterns] = get_precomputed_blocks();
  for (int pattern = 0; pattern < precomputed_blocks::PATTERN_COUNT; ++pattern) {
    expect_block_in_pixel_range(patterns[pattern]);
  }
}

TEST(dct_test, should_give_every_decoder_projection_energy) {
  const auto& [vectors] = get_decoder_projections();
  for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
    float sum_of_squares = 0.0f;
    for (int i = 0; i < BLOCK_AREA; ++i) {
      sum_of_squares += vectors[bit_index][i] * vectors[bit_index][i];
    }
    EXPECT_GT(sum_of_squares, 0.0f);
  }
}

TEST(dct_test, should_extract_every_embedded_bit_pattern) {
  const auto& [patterns] = get_precomputed_blocks();
  const auto& [vectors] = get_decoder_projections();

  for (int pattern = 0; pattern < precomputed_blocks::PATTERN_COUNT; ++pattern) {
    alignas(VECTOR_ALIGNMENT) float block_flat[BLOCK_AREA];
    flatten_block(patterns[pattern], block_flat);

    int recovered = 0;
    for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
      const float sum = dot_product_64(block_flat, vectors[bit_index]);
      recovered = (recovered << 1) | (sum > 0.0f ? 1 : 0);
    }

    EXPECT_EQ(recovered, pattern) << "Pattern " << pattern << " failed the round trip!";
  }
}

TEST(dct_test, should_sum_a_dot_product_of_ones) {
  alignas(VECTOR_ALIGNMENT) float left[BLOCK_AREA];
  alignas(VECTOR_ALIGNMENT) float right[BLOCK_AREA];

  for (int i = 0; i < BLOCK_AREA; ++i) {
    left[i] = 1.0f;
    right[i] = 1.0f;
  }

  const float product = dot_product_64(left, right);
  EXPECT_FLOAT_EQ(product, static_cast<float>(BLOCK_AREA));
}

TEST(dct_test, should_compute_the_dot_product_of_a_linear_sequence) {
  alignas(VECTOR_ALIGNMENT) float left[BLOCK_AREA];
  alignas(VECTOR_ALIGNMENT) float right[BLOCK_AREA];

  for (int i = 0; i < BLOCK_AREA; ++i) {
    left[i] = static_cast<float>(i);
    right[i] = 1.0f;
  }

  const float product = dot_product_64(left, right);
  EXPECT_FLOAT_EQ(product, 2016.0f);
}

TEST(dct_test, should_compute_a_zero_dot_product_for_orthogonal_vectors) {
  alignas(VECTOR_ALIGNMENT) float left[BLOCK_AREA] = {};
  alignas(VECTOR_ALIGNMENT) float right[BLOCK_AREA] = {};

  constexpr int half_area = BLOCK_AREA / 2;
  for (int i = 0; i < half_area; ++i) {
    left[i] = 1.0f;
  }
  for (int i = half_area; i < BLOCK_AREA; ++i) {
    right[i] = 1.0f;
  }

  const float product = dot_product_64(left, right);
  EXPECT_FLOAT_EQ(product, 0.0f);
}

TEST(dct_test, should_build_the_same_cosine_table_every_time) {
  const cosine_table& table1 = get_cosine_table();
  const cosine_table& table2 = get_cosine_table();
  EXPECT_EQ(&table1, &table2);
}

TEST(dct_test, should_set_the_dc_cosine_to_one) {
  const auto& [data] = get_cosine_table();
  for (int i = 0; i < BLOCK_SIZE; ++i) {
    EXPECT_FLOAT_EQ(data[i][0], 1.0f);
  }
}

TEST(dct_test, should_store_one_bit_per_block_in_the_frame_layout) {
  const frame_layout layout = compute_frame_layout(1920, 1080);
  EXPECT_EQ(layout.bits_per_frame, layout.total_blocks * BITS_PER_BLOCK);
  EXPECT_EQ(layout.bytes_per_frame, layout.bits_per_frame / CHAR_BIT);
}

TEST(dct_test, should_produce_a_different_block_for_each_pattern) {
  if constexpr (precomputed_blocks::PATTERN_COUNT < 2) {
    GTEST_SKIP() << "At least 2 patterns are needed!";
  }

  const auto& [patterns] = get_precomputed_blocks();
  const bool any_differ = std::memcmp(patterns[0], patterns[1], sizeof(patterns[0])) != 0;
  EXPECT_TRUE(any_differ);
}

TEST(dct_test, should_fill_the_margins_a_frame_width_leaves_beside_whole_blocks) {
  constexpr int frame_width = 20;
  constexpr int frame_height = 40;
  constexpr int block_width = 16;
  constexpr int data_rows = 32;
  constexpr uint8_t untouched = 0xFF;
  constexpr uint8_t neutral = 128;
  const frame_layout layout = compute_frame_layout(frame_width, frame_height);
  std::vector<uint8_t> plane(static_cast<std::size_t>(frame_width) * frame_height, untouched);
  const std::vector<std::byte> data = {std::byte{0xA5}};

  embed_data_into_plane(plane.data(), frame_width, layout, data);

  for (int y = 0; y < frame_height; ++y) {
    const int first_neutral = y < data_rows ? block_width : 0;
    for (int x = first_neutral; x < frame_width; ++x) {
      const uint8_t sample = plane[static_cast<std::size_t>(y) * frame_width + x];
      EXPECT_EQ(sample, neutral);
    }
  }
}
