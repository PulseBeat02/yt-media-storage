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

#pragma once

#include "configuration.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#if defined(__AVX2__) || defined(__AVX__)
#include <immintrin.h>
#define DCT_USE_AVX 1
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64)))
#include <emmintrin.h>
#define DCT_USE_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define DCT_USE_NEON 1
#endif

// simd (used for dot products)
inline float dot_product_64(const float *a, const float *b) {
#if defined(DCT_USE_AVX)
    __m256 sum0 = _mm256_setzero_ps();
    __m256 sum1 = _mm256_setzero_ps();
    for (int i = 0; i < 64; i += 16) {
        sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
        sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8)));
    }
    sum0 = _mm256_add_ps(sum0, sum1);
    const __m128 hi = _mm256_extractf128_ps(sum0, 1);
    const __m128 lo = _mm256_castps256_ps128(sum0);
    __m128 r = _mm_add_ps(lo, hi);
    r = _mm_add_ps(r, _mm_movehl_ps(r, r));
    r = _mm_add_ss(r, _mm_movehdup_ps(r));
    return _mm_cvtss_f32(r);

#elif defined(DCT_USE_SSE2)
    // SSE2: 4 floats/register, unroll 4× → 16 floats/iteration
    __m128 sum0 = _mm_setzero_ps();
    __m128 sum1 = _mm_setzero_ps();
    __m128 sum2 = _mm_setzero_ps();
    __m128 sum3 = _mm_setzero_ps();
    for (int i = 0; i < 64; i += 16) {
        sum0 = _mm_add_ps(sum0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        sum1 = _mm_add_ps(sum1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
        sum2 = _mm_add_ps(sum2, _mm_mul_ps(_mm_loadu_ps(a + i + 8), _mm_loadu_ps(b + i + 8)));
        sum3 = _mm_add_ps(sum3, _mm_mul_ps(_mm_loadu_ps(a + i + 12), _mm_loadu_ps(b + i + 12)));
    }
    sum0 = _mm_add_ps(_mm_add_ps(sum0, sum1), _mm_add_ps(sum2, sum3));
    // Horizontal sum (SSE2-only, no hadd)
    __m128 shuf = _mm_shuffle_ps(sum0, sum0, _MM_SHUFFLE(2, 3, 0, 1));
    sum0 = _mm_add_ps(sum0, shuf);
    shuf = _mm_movehl_ps(shuf, sum0);
    sum0 = _mm_add_ss(sum0, shuf);
    return _mm_cvtss_f32(sum0);

#elif defined(DCT_USE_NEON)
    // ARM NEON: 4 floats/register, unroll 4×
    float32x4_t sum0 = vdupq_n_f32(0.0f);
    float32x4_t sum1 = vdupq_n_f32(0.0f);
    float32x4_t sum2 = vdupq_n_f32(0.0f);
    float32x4_t sum3 = vdupq_n_f32(0.0f);
    for (int i = 0; i < 64; i += 16) {
        sum0 = vmlaq_f32(sum0, vld1q_f32(a + i), vld1q_f32(b + i));
        sum1 = vmlaq_f32(sum1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        sum2 = vmlaq_f32(sum2, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        sum3 = vmlaq_f32(sum3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    sum0 = vaddq_f32(vaddq_f32(sum0, sum1), vaddq_f32(sum2, sum3));
    return vaddvq_f32(sum0);

#else
    // Scalar fallback
    float sum = 0.0f;
    for (int i = 0; i < 64; ++i)
        sum += a[i] * b[i];
    return sum;
#endif
}

inline constexpr float PI_F = 3.14159265358979323846f;

inline constexpr std::pair<int, int> EMBED_POSITIONS[] = {
    {0, 1},
    {1, 0},
    {1, 1},
    {0, 2},
};

struct CosineTable {
    float data[8][8];
};

inline const CosineTable &get_cosine_table() {
    static const CosineTable table = [] {
        CosineTable cosine_table{};
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                cosine_table.data[i][j] = std::cos(
                    (2.0f * static_cast<float>(i) + 1.0f) * static_cast<float>(j) * PI_F / 16.0f);
            }
        }
        return cosine_table;
    }();
    return table;
}

constexpr float alpha_f(const int u) {
    return u == 0 ? 0.70710678118654752f : 1.0f;
}

struct PrecomputedBlocks {
    static constexpr int NUM_PATTERNS = 1 << BITS_PER_BLOCK;
    uint8_t patterns[NUM_PATTERNS][8][8];
};

inline const PrecomputedBlocks &get_precomputed_blocks() {
    static const PrecomputedBlocks blocks = [] {
        PrecomputedBlocks result{};
        const auto &[data] = get_cosine_table();

        constexpr float dc_value = 0.25f * alpha_f(0) * alpha_f(0) * 64.0f * 128.0f;

        float dc_image[8][8];
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                dc_image[x][y] = 0.25f * alpha_f(0) * alpha_f(0) * dc_value
                                 * data[x][0] * data[y][0];
            }
        }

        float embed_basis[4][8][8]{};
        for (int b = 0; b < BITS_PER_BLOCK; ++b) {
            const auto [u, v] = EMBED_POSITIONS[b];
            const float scale = 0.25f * alpha_f(u) * alpha_f(v)
                                * static_cast<float>(COEFFICIENT_STRENGTH);
            for (int x = 0; x < 8; ++x) {
                for (int y = 0; y < 8; ++y) {
                    embed_basis[b][x][y] = scale * data[x][u] * data[y][v];
                }
            }
        }

        for (int pattern = 0; pattern < PrecomputedBlocks::NUM_PATTERNS; ++pattern) {
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    float val = dc_image[y][x];
                    for (int b = 0; b < BITS_PER_BLOCK; ++b) {
                        const int bit = (pattern >> (BITS_PER_BLOCK - 1 - b)) & 1;
                        val += (bit ? 1.0f : -1.0f) * embed_basis[b][y][x];
                    }
                    val = std::clamp(val, 0.0f, 255.0f);
                    result.patterns[pattern][y][x] = static_cast<uint8_t>(val);
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

inline const DecoderProjections &get_decoder_projections() {
    static const DecoderProjections proj = [] {
        DecoderProjections decoder_projections{};
        const auto &[data] = get_cosine_table();
        for (int b = 0; b < BITS_PER_BLOCK; ++b) {
            const auto [u, v] = EMBED_POSITIONS[b];
            for (int x = 0; x < 8; ++x) {
                for (int y = 0; y < 8; ++y) {
                    decoder_projections.vectors[b][x * 8 + y] = data[x][u] * data[y][v];
                }
            }
        }
        return decoder_projections;
    }();
    return proj;
}
