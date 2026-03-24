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
#include "video_encoder.h"

#include <cstdint>

TEST(DCT, PrecomputedBlocks_PatternCountMatchesBitsPerBlock) {
    constexpr int expected_patterns = 1 << BITS_PER_BLOCK;
    EXPECT_EQ(PrecomputedBlocks::NUM_PATTERNS, expected_patterns);
}

TEST(DCT, PrecomputedBlocks_ValuesInValidRange) {
    const auto &[patterns] = get_precomputed_blocks();
    for (int p = 0; p < PrecomputedBlocks::NUM_PATTERNS; ++p) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                EXPECT_LE(patterns[p][y][x], 255u);
            }
        }
    }
}

TEST(DCT, DecoderProjections_NonZeroEnergy) {
    const auto &[vectors] = get_decoder_projections();
    for (int b = 0; b < BITS_PER_BLOCK; ++b) {
        float sum_sq = 0.0f;
        for (int i = 0; i < 64; ++i) {
            sum_sq += vectors[b][i] * vectors[b][i];
        }
        EXPECT_GT(sum_sq, 0.0f);
    }
}

TEST(DCT, EmbedExtract_AllPatternsRoundtrip) {
    const auto &[patterns] = get_precomputed_blocks();
    const auto &[vectors] = get_decoder_projections();

    for (int pattern = 0; pattern < PrecomputedBlocks::NUM_PATTERNS; ++pattern) {
        alignas(32) float block_flat[64];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                block_flat[y * 8 + x] = static_cast<float>(patterns[pattern][y][x]);
            }
        }

        int recovered = 0;
        for (int b = 0; b < BITS_PER_BLOCK; ++b) {
            const float sum = dot_product_64(block_flat, vectors[b]);
            recovered = (recovered << 1) | (sum > 0.0f ? 1 : 0);
        }

        EXPECT_EQ(recovered, pattern) << "Pattern " << pattern << " failed roundtrip";
    }
}

TEST(DCT, DotProduct64_AllOnes) {
    alignas(32) float a[64];
    alignas(32) float b[64];

    for (int i = 0; i < 64; ++i) {
        a[i] = 1.0f;
        b[i] = 1.0f;
    }
    EXPECT_FLOAT_EQ(dot_product_64(a, b), 64.0f);
}

TEST(DCT, DotProduct64_LinearSequence) {
    alignas(32) float a[64];
    alignas(32) float b[64];

    for (int i = 0; i < 64; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = 1.0f;
    }
    EXPECT_FLOAT_EQ(dot_product_64(a, b), 2016.0f);
}

TEST(DCT, DotProduct64_Orthogonal) {
    alignas(32) float a[64] = {};
    alignas(32) float b[64] = {};

    for (int i = 0; i < 32; ++i) a[i] = 1.0f;
    for (int i = 32; i < 64; ++i) b[i] = 1.0f;

    EXPECT_FLOAT_EQ(dot_product_64(a, b), 0.0f);
}

TEST(DCT, CosineTable_Deterministic) {
    const auto &table1 = get_cosine_table();
    const auto &table2 = get_cosine_table();
    EXPECT_EQ(&table1, &table2);
}

TEST(DCT, CosineTable_DC_IsOne) {
    const auto &[data] = get_cosine_table();
    for (int i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(data[i][0], 1.0f);
    }
}

TEST(DCT, FrameLayout_BytesMatchBlocks) {
    const FrameLayout layout = compute_frame_layout(1920, 1080);
    EXPECT_EQ(layout.bits_per_frame, layout.total_blocks * BITS_PER_BLOCK);
    EXPECT_EQ(layout.bytes_per_frame, layout.bits_per_frame / 8);
}

TEST(DCT, DifferentPatternsProduceDifferentBlocks) {
    if constexpr (PrecomputedBlocks::NUM_PATTERNS < 2) {
        GTEST_SKIP() << "Need at least 2 patterns";
    }

    const auto &[patterns] = get_precomputed_blocks();
    bool any_differ = false;
    for (int y = 0; y < 8 && !any_differ; ++y) {
        for (int x = 0; x < 8 && !any_differ; ++x) {
            if (patterns[0][y][x] != patterns[1][y][x]) {
                any_differ = true;
            }
        }
    }
    EXPECT_TRUE(any_differ);
}
