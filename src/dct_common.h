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

// simd dot product
inline float dot_product_64(const float* left, const float* right) {
#if defined(DCT_USE_AVX)
  __m256 sum0 = _mm256_setzero_ps();
  __m256 sum1 = _mm256_setzero_ps();
  for (int i = 0; i < 64; i += 16) {
    sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(_mm256_loadu_ps(left + i), _mm256_loadu_ps(right + i)));
    sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(_mm256_loadu_ps(left + i + 8), _mm256_loadu_ps(right + i + 8)));
  }
  sum0 = _mm256_add_ps(sum0, sum1);
  const __m128 high = _mm256_extractf128_ps(sum0, 1);
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
  for (int i = 0; i < 64; i += 16) {
    sum0 = _mm_add_ps(sum0, _mm_mul_ps(_mm_loadu_ps(left + i), _mm_loadu_ps(right + i)));
    sum1 = _mm_add_ps(sum1, _mm_mul_ps(_mm_loadu_ps(left + i + 4), _mm_loadu_ps(right + i + 4)));
    sum2 = _mm_add_ps(sum2, _mm_mul_ps(_mm_loadu_ps(left + i + 8), _mm_loadu_ps(right + i + 8)));
    sum3 = _mm_add_ps(sum3, _mm_mul_ps(_mm_loadu_ps(left + i + 12), _mm_loadu_ps(right + i + 12)));
  }
  sum0 = _mm_add_ps(_mm_add_ps(sum0, sum1), _mm_add_ps(sum2, sum3));
  __m128 shuffled = _mm_shuffle_ps(sum0, sum0, _MM_SHUFFLE(2, 3, 0, 1));
  sum0 = _mm_add_ps(sum0, shuffled);
  shuffled = _mm_movehl_ps(shuffled, sum0);
  sum0 = _mm_add_ss(sum0, shuffled);
  return _mm_cvtss_f32(sum0);

#elif defined(DCT_USE_NEON)
  float32x4_t sum0 = vdupq_n_f32(0.0f);
  float32x4_t sum1 = vdupq_n_f32(0.0f);
  float32x4_t sum2 = vdupq_n_f32(0.0f);
  float32x4_t sum3 = vdupq_n_f32(0.0f);
  for (int i = 0; i < 64; i += 16) {
    sum0 = vmlaq_f32(sum0, vld1q_f32(left + i), vld1q_f32(right + i));
    sum1 = vmlaq_f32(sum1, vld1q_f32(left + i + 4), vld1q_f32(right + i + 4));
    sum2 = vmlaq_f32(sum2, vld1q_f32(left + i + 8), vld1q_f32(right + i + 8));
    sum3 = vmlaq_f32(sum3, vld1q_f32(left + i + 12), vld1q_f32(right + i + 12));
  }
  sum0 = vaddq_f32(vaddq_f32(sum0, sum1), vaddq_f32(sum2, sum3));
  return vaddvq_f32(sum0);

#else
  // Scalar fallback
  float sum = 0.0f;
  for (int i = 0; i < 64; ++i)
    sum += left[i] * right[i];
  return sum;
#endif
}

inline constexpr float PI = 3.14159265358979323846f;

inline constexpr std::pair<int, int> EMBED_POSITIONS[] = {
    {0, 1},
    {1, 0},
    {1, 1},
    {0, 2},
};

static_assert(BITS_PER_BLOCK >= 1 && BITS_PER_BLOCK <= static_cast<int>(std::size(EMBED_POSITIONS)),
              "BITS_PER_BLOCK must not exceed the number of embed positions");
static_assert(8 % BITS_PER_BLOCK == 0, "BITS_PER_BLOCK must divide 8");

struct CosineTable {
  float data[8][8];
};

inline const CosineTable& get_cosine_table() {
  static const CosineTable table = [] {
    CosineTable cosine_table{};
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 8; ++j) {
        cosine_table.data[i][j] = std::cos((2.0f * static_cast<float>(i) + 1.0f) * static_cast<float>(j) * PI / 16.0f);
      }
    }
    return cosine_table;
  }();
  return table;
}

constexpr float alpha(const int u) {
  return u == 0 ? 0.70710678118654752f : 1.0f;
}

struct PrecomputedBlocks {
  static constexpr int PATTERN_COUNT = 1 << BITS_PER_BLOCK;
  uint8_t patterns[PATTERN_COUNT][8][8];
};

inline const PrecomputedBlocks& get_precomputed_blocks() {
  static const PrecomputedBlocks blocks = [] {
    PrecomputedBlocks result{};
    const auto& [data] = get_cosine_table();

    constexpr float dc_value = 0.25f * alpha(0) * alpha(0) * 64.0f * 128.0f;

    float dc_image[8][8];
    for (int x = 0; x < 8; ++x) {
      for (int y = 0; y < 8; ++y) {
        dc_image[x][y] = 0.25f * alpha(0) * alpha(0) * dc_value * data[x][0] * data[y][0];
      }
    }

    float embed_basis[4][8][8]{};
    for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
      const auto [u, v] = EMBED_POSITIONS[bit_index];
      const float scale = 0.25f * alpha(u) * alpha(v) * static_cast<float>(COEFFICIENT_STRENGTH);
      for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
          embed_basis[bit_index][x][y] = scale * data[x][u] * data[y][v];
        }
      }
    }

    for (int pattern = 0; pattern < PrecomputedBlocks::PATTERN_COUNT; ++pattern) {
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          float value = dc_image[y][x];
          for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
            const int bit = (pattern >> (BITS_PER_BLOCK - 1 - bit_index)) & 1;
            value += (bit ? 1.0f : -1.0f) * embed_basis[bit_index][y][x];
          }
          value = std::clamp(value, 0.0f, 255.0f);
          result.patterns[pattern][y][x] = static_cast<uint8_t>(value);
        }
      }
    }

    return result;
  }();
  return blocks;
}

struct DecoderProjections {
  alignas(32) float vectors[4][64];
};

inline const DecoderProjections& get_decoder_projections() {
  static const DecoderProjections projections = [] {
    DecoderProjections decoder_projections{};
    const auto& [data] = get_cosine_table();
    for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
      const auto [u, v] = EMBED_POSITIONS[bit_index];
      for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
          decoder_projections.vectors[bit_index][x * 8 + y] = data[x][u] * data[y][v];
        }
      }
    }
    return decoder_projections;
  }();
  return projections;
}

#endif
