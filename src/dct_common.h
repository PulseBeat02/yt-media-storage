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

#ifndef MEDIA_STORAGE_DCT_COMMON_H
#define MEDIA_STORAGE_DCT_COMMON_H

#include "configuration.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>

#if defined(__AVX2__) || defined(__AVX__)
#include <immintrin.h>
#define DCT_USE_AVX 1
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64)))
#include <emmintrin.h>
#define DCT_USE_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define DCT_USE_NEON 1
#endif

inline constexpr int BLOCK_SIZE = 8;
inline constexpr int BLOCK_AREA = BLOCK_SIZE * BLOCK_SIZE;

inline constexpr int MIDDLE_GRAY = 128;
inline constexpr float MAXIMUM_GRAY = 255.0f;

inline constexpr std::size_t VECTOR_ALIGNMENT = 32;

#if defined(DCT_USE_AVX)
inline constexpr int AVX_FLOATS_PER_VECTOR = 8;
inline constexpr int AVX_FLOATS_PER_STEP = 2 * AVX_FLOATS_PER_VECTOR;

inline constexpr int AVX_HIGH_HALF = 1;
#elif defined(DCT_USE_SSE2) || defined(DCT_USE_NEON)
inline constexpr int FLOATS_PER_VECTOR = 4;
inline constexpr int SECOND_VECTOR_OFFSET = FLOATS_PER_VECTOR;
inline constexpr int THIRD_VECTOR_OFFSET = 2 * FLOATS_PER_VECTOR;
inline constexpr int FOURTH_VECTOR_OFFSET = 3 * FLOATS_PER_VECTOR;
inline constexpr int FLOATS_PER_STEP = 4 * FLOATS_PER_VECTOR;
#endif

#if defined(DCT_USE_SSE2)
inline constexpr int SWAP_ADJACENT_LANES = _MM_SHUFFLE(2, 3, 0, 1);
#endif

// simd dot product
inline float dot_product_64(const float* left, const float* right) {
#if defined(DCT_USE_AVX)
  __m256 sum0 = _mm256_setzero_ps();
  __m256 sum1 = _mm256_setzero_ps();
  for (int i = 0; i < BLOCK_AREA; i += AVX_FLOATS_PER_STEP) {
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(_mm256_loadu_ps(left + i), _mm256_loadu_ps(right + i)));
    sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(_mm256_loadu_ps(left + i + AVX_FLOATS_PER_VECTOR),
                                             _mm256_loadu_ps(right + i + AVX_FLOATS_PER_VECTOR)));
  }
  sum0 = _mm256_add_ps(sum0, sum1);
  const __m128 high = _mm256_extractf128_ps(sum0, AVX_HIGH_HALF);
  const __m128 low = _mm256_castps256_ps128(sum0);
  __m128 total = _mm_add_ps(low, high);
  total = _mm_add_ps(total, _mm_movehl_ps(total, total));
  total = _mm_add_ss(total, _mm_movehdup_ps(total));
  return _mm_cvtss_f32(total);

#elif defined(DCT_USE_SSE2)
  __m128 sum0 = _mm_setzero_ps();
  __m128 sum1 = _mm_setzero_ps();
  __m128 sum2 = _mm_setzero_ps();
  __m128 sum3 = _mm_setzero_ps();
  for (int i = 0; i < BLOCK_AREA; i += FLOATS_PER_STEP) {
    sum0 = _mm_add_ps(sum0, _mm_mul_ps(_mm_loadu_ps(left + i), _mm_loadu_ps(right + i)));
    sum1 = _mm_add_ps(sum1, _mm_mul_ps(_mm_loadu_ps(left + i + SECOND_VECTOR_OFFSET),
                                       _mm_loadu_ps(right + i + SECOND_VECTOR_OFFSET)));
    sum2 = _mm_add_ps(
        sum2, _mm_mul_ps(_mm_loadu_ps(left + i + THIRD_VECTOR_OFFSET), _mm_loadu_ps(right + i + THIRD_VECTOR_OFFSET)));
    sum3 = _mm_add_ps(sum3, _mm_mul_ps(_mm_loadu_ps(left + i + FOURTH_VECTOR_OFFSET),
                                       _mm_loadu_ps(right + i + FOURTH_VECTOR_OFFSET)));
  }
  sum0 = _mm_add_ps(_mm_add_ps(sum0, sum1), _mm_add_ps(sum2, sum3));
  __m128 shuffled = _mm_shuffle_ps(sum0, sum0, SWAP_ADJACENT_LANES);
  sum0 = _mm_add_ps(sum0, shuffled);
  shuffled = _mm_movehl_ps(shuffled, sum0);
  sum0 = _mm_add_ss(sum0, shuffled);
  return _mm_cvtss_f32(sum0);

#elif defined(DCT_USE_NEON)
  float32x4_t sum0 = vdupq_n_f32(0.0f);
  float32x4_t sum1 = vdupq_n_f32(0.0f);
  float32x4_t sum2 = vdupq_n_f32(0.0f);
  float32x4_t sum3 = vdupq_n_f32(0.0f);
  for (int i = 0; i < BLOCK_AREA; i += FLOATS_PER_STEP) {
    sum0 = vmlaq_f32(sum0, vld1q_f32(left + i), vld1q_f32(right + i));
    sum1 = vmlaq_f32(sum1, vld1q_f32(left + i + SECOND_VECTOR_OFFSET), vld1q_f32(right + i + SECOND_VECTOR_OFFSET));
    sum2 = vmlaq_f32(sum2, vld1q_f32(left + i + THIRD_VECTOR_OFFSET), vld1q_f32(right + i + THIRD_VECTOR_OFFSET));
    sum3 = vmlaq_f32(sum3, vld1q_f32(left + i + FOURTH_VECTOR_OFFSET), vld1q_f32(right + i + FOURTH_VECTOR_OFFSET));
  }
  sum0 = vaddq_f32(vaddq_f32(sum0, sum1), vaddq_f32(sum2, sum3));
  return vaddvq_f32(sum0);

#else
  // Scalar fallback
  float sum = 0.0f;
  for (int i = 0; i < BLOCK_AREA; ++i)
    sum += left[i] * right[i];
  return sum;
#endif
}

inline constexpr float PI = 3.14159265358979323846f;
inline constexpr float INVERSE_SQUARE_ROOT_OF_TWO = 0.70710678118654752f;

inline constexpr float DCT_SCALE = 0.25f;
inline constexpr float HALF_STEPS_PER_SAMPLE = 2.0f;
inline constexpr float HALF_STEPS_PER_BLOCK = HALF_STEPS_PER_SAMPLE * BLOCK_SIZE;

inline constexpr std::pair<int, int> EMBED_POSITIONS[] = {
    {0, 1},
    {1, 0},
    {1, 1},
    {0, 2},
};

static_assert(BITS_PER_BLOCK >= 1 && BITS_PER_BLOCK <= static_cast<int>(std::size(EMBED_POSITIONS)),
              "BITS_PER_BLOCK must not exceed the number of embed positions");
static_assert(CHAR_BIT % BITS_PER_BLOCK == 0, "BITS_PER_BLOCK must divide 8");

struct cosine_table {
  float data[BLOCK_SIZE][BLOCK_SIZE];
};

inline const cosine_table& get_cosine_table() {
  static const cosine_table table = [] {
    cosine_table cosine_table{};
    for (int i = 0; i < BLOCK_SIZE; ++i) {
      for (int j = 0; j < BLOCK_SIZE; ++j) {
        cosine_table.data[i][j] = std::cos((HALF_STEPS_PER_SAMPLE * static_cast<float>(i) + 1.0f) *
                                           static_cast<float>(j) * PI / HALF_STEPS_PER_BLOCK);
      }
    }
    return cosine_table;
  }();
  return table;
}

constexpr float alpha(const int u) {
  return u == 0 ? INVERSE_SQUARE_ROOT_OF_TWO : 1.0f;
}

struct precomputed_blocks {
  static constexpr int PATTERN_COUNT = 1 << BITS_PER_BLOCK;
  uint8_t patterns[PATTERN_COUNT][BLOCK_SIZE][BLOCK_SIZE];
};

inline const precomputed_blocks& get_precomputed_blocks() {
  static const precomputed_blocks blocks = [] {
    precomputed_blocks result{};
    const auto& [data] = get_cosine_table();

    constexpr float dc_value = DCT_SCALE * alpha(0) * alpha(0) * BLOCK_AREA * MIDDLE_GRAY;

    float dc_image[BLOCK_SIZE][BLOCK_SIZE];
    for (int x = 0; x < BLOCK_SIZE; ++x) {
      for (int y = 0; y < BLOCK_SIZE; ++y) {
        dc_image[x][y] = DCT_SCALE * alpha(0) * alpha(0) * dc_value * data[x][0] * data[y][0];
      }
    }

    float embed_basis[std::size(EMBED_POSITIONS)][BLOCK_SIZE][BLOCK_SIZE]{};
    for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
      const auto [u, v] = EMBED_POSITIONS[bit_index];
      const float scale = DCT_SCALE * alpha(u) * alpha(v) * static_cast<float>(COEFFICIENT_STRENGTH);
      for (int x = 0; x < BLOCK_SIZE; ++x) {
        for (int y = 0; y < BLOCK_SIZE; ++y) {
          embed_basis[bit_index][x][y] = scale * data[x][u] * data[y][v];
        }
      }
    }

    for (int pattern = 0; pattern < precomputed_blocks::PATTERN_COUNT; ++pattern) {
      for (int y = 0; y < BLOCK_SIZE; ++y) {
        for (int x = 0; x < BLOCK_SIZE; ++x) {
          float value = dc_image[y][x];
          for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
            const int bit = (pattern >> (BITS_PER_BLOCK - 1 - bit_index)) & 1;
            value += (bit ? 1.0f : -1.0f) * embed_basis[bit_index][y][x];
          }
          value = std::clamp(value, 0.0f, MAXIMUM_GRAY);
          result.patterns[pattern][y][x] = static_cast<uint8_t>(value);
        }
      }
    }

    return result;
  }();
  return blocks;
}

struct decoder_projections {
  alignas(VECTOR_ALIGNMENT) float vectors[std::size(EMBED_POSITIONS)][BLOCK_AREA];
};

inline const decoder_projections& get_decoder_projections() {
  static const decoder_projections projections = [] {
    decoder_projections decoder_projections{};
    const auto& [data] = get_cosine_table();
    for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
      const auto [u, v] = EMBED_POSITIONS[bit_index];
      for (int x = 0; x < BLOCK_SIZE; ++x) {
        for (int y = 0; y < BLOCK_SIZE; ++y) {
          decoder_projections.vectors[bit_index][x * BLOCK_SIZE + y] = data[x][u] * data[y][v];
        }
      }
    }
    return decoder_projections;
  }();
  return projections;
}

#endif
