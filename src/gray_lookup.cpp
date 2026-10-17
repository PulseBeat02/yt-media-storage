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

#include "gray_lookup.h"

#include "av_utils.h"

#include <algorithm>
#include <memory>
#include <random>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

namespace {

constexpr std::minstd_rand::result_type CALIBRATION_SEED = 0x6D73;
constexpr int RANDOM_BYTE_SHIFT = 16;

constexpr int UNSEEN_LEVEL = -1;

using frame_pointer = std::unique_ptr<AVFrame, decltype([](AVFrame* frame) { av_frame_free(&frame); })>;
using observed_levels = std::array<int, GRAY_LEVELS>;

frame_pointer allocate_frame(const AVPixelFormat format, const int width, const int height) {
  frame_pointer frame(av_frame_alloc());
  if (frame == nullptr) {
    return nullptr;
  }

  frame->format = format;
  frame->width = width;
  frame->height = height;

  if (av_frame_get_buffer(frame.get(), DEFAULT_ALIGNMENT) < 0) {
    return nullptr;
  }

  return frame;
}

void fill_with_random_bytes(AVFrame& frame) {
  std::minstd_rand generator(CALIBRATION_SEED);
  for (const AVBufferRef* const buffer : frame.buf) {
    if (buffer != nullptr) {
      std::generate_n(buffer->data, buffer->size,
                      [&generator] { return static_cast<uint8_t>(generator() >> RANDOM_BYTE_SHIFT); });
    }
  }
}

const uint8_t* row_of(const AVFrame& frame, const int y) {
  return frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
}

void record_levels(const AVFrame& source, const AVFrame& gray, observed_levels& levels) {
  for (int y = 0; y < source.height; ++y) {
    const uint8_t* const luma_row = row_of(source, y);
    const uint8_t* const gray_row = row_of(gray, y);
    for (int x = 0; x < source.width; ++x) {
      levels[luma_row[x]] = gray_row[x];
    }
  }
}

bool matches_levels_in_row(const uint8_t* luma_row, const uint8_t* gray_row, const int width,
                           const observed_levels& levels) {
  return std::equal(luma_row, luma_row + width, gray_row,
                    [&levels](const uint8_t luma, const uint8_t gray) { return levels[luma] == gray; });
}

bool matches_levels(const AVFrame& source, const AVFrame& gray, const observed_levels& levels) {
  for (int y = 0; y < source.height; ++y) {
    const uint8_t* const luma_row = row_of(source, y);
    const uint8_t* const gray_row = row_of(gray, y);
    if (!matches_levels_in_row(luma_row, gray_row, source.width, levels)) {
      return false;
    }
  }

  return true;
}

bool has_unseen_level(const observed_levels& levels) {
  return std::ranges::find(levels, UNSEEN_LEVEL) != levels.end();
}

gray_lookup_table to_table(const observed_levels& levels) {
  gray_lookup_table table{};
  std::ranges::transform(levels, table.begin(), [](const int level) { return static_cast<uint8_t>(level); });
  return table;
}

}

std::optional<gray_lookup_table> calibrate_gray_lookup_table(SwsContext* context, const AVPixelFormat source_format,
                                                             const int width, const int height) {
  const frame_pointer source = allocate_frame(source_format, width, height);
  const frame_pointer gray = allocate_frame(AV_PIX_FMT_GRAY8, width, height);
  if (source == nullptr || gray == nullptr) {
    return std::nullopt;
  }

  fill_with_random_bytes(*source);
  const int converted_rows =
      sws_scale(context, source->data, source->linesize, FIRST_ROW, height, gray->data, gray->linesize);
  if (converted_rows != height) {
    return std::nullopt;
  }

  observed_levels levels{};
  levels.fill(UNSEEN_LEVEL);
  record_levels(*source, *gray, levels);

  if (has_unseen_level(levels) || !matches_levels(*source, *gray, levels)) {
    return std::nullopt;
  }

  return to_table(levels);
}
