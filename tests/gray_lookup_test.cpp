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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "configuration.h"
#include "fixtures.h"
#include "gray_lookup.h"

namespace {

constexpr int DEFAULT_ALIGNMENT = 0;
constexpr int FIRST_ROW = 0;

constexpr int SQUARE_FRAME_SIZE = 256;
constexpr int FRAME_HEIGHT_BEYOND_THE_SCALER = SQUARE_FRAME_SIZE * 2;
constexpr int FRAME_SIZE_WITHOUT_EVERY_LUMA_VALUE = 16;

constexpr uint8_t LIMITED_RANGE_BLACK = 16;
constexpr uint8_t LIMITED_RANGE_WHITE = 235;
constexpr uint8_t FULL_RANGE_BLACK = 0;
constexpr uint8_t FULL_RANGE_WHITE = 255;

using scaler_pointer = std::unique_ptr<SwsContext, decltype([](SwsContext* context) { sws_freeContext(context); })>;
using frame_pointer = std::unique_ptr<AVFrame, decltype([](AVFrame* frame) { av_frame_free(&frame); })>;

scaler_pointer make_gray_scaler(const AVPixelFormat format, const int width, const int height) {
  SwsContext* const context =
      sws_getContext(width, height, format, width, height, AV_PIX_FMT_GRAY8, SWS_POINT, nullptr, nullptr, nullptr);
  return scaler_pointer(context);
}

std::optional<gray_lookup_table> calibrate_stream_table(SwsContext* scaler) {
  return calibrate_gray_lookup_table(scaler, AV_PIX_FMT_YUV420P, FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM);
}

frame_pointer make_frame(const AVPixelFormat format, const int width, const int height) {
  frame_pointer frame(av_frame_alloc());
  frame->format = format;
  frame->width = width;
  frame->height = height;

  const int buffer_status = av_frame_get_buffer(frame.get(), DEFAULT_ALIGNMENT);
  EXPECT_EQ(buffer_status, 0);

  return frame;
}

void fill_with_pattern(uint8_t* data, const std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((i * fixtures::PATTERN_MULTIPLIER + fixtures::PATTERN_OFFSET) & fixtures::BYTE_MASK);
  }
}

void fill_with_pattern(AVFrame& frame) {
  for (const AVBufferRef* const buffer : frame.buf) {
    if (buffer != nullptr) {
      fill_with_pattern(buffer->data, buffer->size);
    }
  }
}

bool matches_table_in_row(const uint8_t* luma_row, const uint8_t* gray_row, const int width,
                          const gray_lookup_table& table) {
  return std::equal(luma_row, luma_row + width, gray_row,
                    [&table](const uint8_t luma, const uint8_t gray) { return table[luma] == gray; });
}

bool matches_table(const AVFrame& source, const AVFrame& gray, const gray_lookup_table& table) {
  for (int y = 0; y < source.height; ++y) {
    const uint8_t* const luma_row = source.data[0] + static_cast<std::ptrdiff_t>(y) * source.linesize[0];
    const uint8_t* const gray_row = gray.data[0] + static_cast<std::ptrdiff_t>(y) * gray.linesize[0];
    if (!matches_table_in_row(luma_row, gray_row, source.width, table)) {
      return false;
    }
  }

  return true;
}

}

TEST(gray_lookup_test, should_convert_unseen_frames_like_the_scaler_it_was_calibrated_against) {
  const scaler_pointer scaler = make_gray_scaler(AV_PIX_FMT_YUV420P, FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM);
  const std::optional<gray_lookup_table> table = calibrate_stream_table(scaler.get());
  const bool table_present = table.has_value();
  ASSERT_TRUE(table_present);

  const frame_pointer source = make_frame(AV_PIX_FMT_YUV420P, FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM);
  const frame_pointer gray = make_frame(AV_PIX_FMT_GRAY8, FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM);
  fill_with_pattern(*source);
  const int converted_rows =
      sws_scale(scaler.get(), source->data, source->linesize, FIRST_ROW, source->height, gray->data, gray->linesize);
  ASSERT_EQ(converted_rows, FRAME_HEIGHT_STREAM);

  const bool matches = matches_table(*source, *gray, *table);

  EXPECT_TRUE(matches);
}

TEST(gray_lookup_test, should_stretch_limited_range_luma_to_full_range_gray) {
  const scaler_pointer scaler = make_gray_scaler(AV_PIX_FMT_YUV420P, FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM);

  const std::optional<gray_lookup_table> table = calibrate_stream_table(scaler.get());

  const bool table_present = table.has_value();
  ASSERT_TRUE(table_present);

  const uint8_t mapped_black = table->at(LIMITED_RANGE_BLACK);
  const uint8_t mapped_white = table->at(LIMITED_RANGE_WHITE);
  EXPECT_EQ(mapped_black, FULL_RANGE_BLACK);
  EXPECT_EQ(mapped_white, FULL_RANGE_WHITE);
}

TEST(gray_lookup_test, should_find_no_table_when_gray_depends_on_more_than_one_byte) {
  const scaler_pointer scaler = make_gray_scaler(AV_PIX_FMT_RGB24, SQUARE_FRAME_SIZE, SQUARE_FRAME_SIZE);

  const std::optional<gray_lookup_table> table =
      calibrate_gray_lookup_table(scaler.get(), AV_PIX_FMT_RGB24, SQUARE_FRAME_SIZE, SQUARE_FRAME_SIZE);

  const bool table_present = table.has_value();
  EXPECT_FALSE(table_present);
}

TEST(gray_lookup_test, should_find_no_table_when_the_frame_is_too_small_to_hold_every_luma_value) {
  const scaler_pointer scaler =
      make_gray_scaler(AV_PIX_FMT_YUV420P, FRAME_SIZE_WITHOUT_EVERY_LUMA_VALUE, FRAME_SIZE_WITHOUT_EVERY_LUMA_VALUE);

  const std::optional<gray_lookup_table> table = calibrate_gray_lookup_table(
      scaler.get(), AV_PIX_FMT_YUV420P, FRAME_SIZE_WITHOUT_EVERY_LUMA_VALUE, FRAME_SIZE_WITHOUT_EVERY_LUMA_VALUE);

  const bool table_present = table.has_value();
  EXPECT_FALSE(table_present);
}

TEST(gray_lookup_test, should_find_no_table_when_the_frame_cannot_be_allocated) {
  const scaler_pointer scaler = make_gray_scaler(AV_PIX_FMT_YUV420P, SQUARE_FRAME_SIZE, SQUARE_FRAME_SIZE);

  const std::optional<gray_lookup_table> table =
      calibrate_gray_lookup_table(scaler.get(), AV_PIX_FMT_NONE, SQUARE_FRAME_SIZE, SQUARE_FRAME_SIZE);

  const bool table_present = table.has_value();
  EXPECT_FALSE(table_present);
}

TEST(gray_lookup_test, should_find_no_table_when_the_scaler_rejects_the_frame) {
  const scaler_pointer scaler = make_gray_scaler(AV_PIX_FMT_YUV420P, SQUARE_FRAME_SIZE, SQUARE_FRAME_SIZE);

  const std::optional<gray_lookup_table> table =
      calibrate_gray_lookup_table(scaler.get(), AV_PIX_FMT_YUV420P, SQUARE_FRAME_SIZE, FRAME_HEIGHT_BEYOND_THE_SCALER);

  const bool table_present = table.has_value();
  EXPECT_FALSE(table_present);
}
