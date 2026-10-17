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

#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

inline std::string ffmpeg_error_string(const int error_code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(error_code, buffer, sizeof(buffer));
  return buffer;
}

inline void ensure_fresh_writable_frame(AVFrame* frame) {
  if (av_frame_is_writable(frame)) {
    return;
  }
  const int format = frame->format;
  const int width = frame->width;
  const int height = frame->height;
  av_frame_unref(frame);
  frame->format = format;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame, 0) < 0) {
    throw std::runtime_error("Failed to allocate frame buffer");
  }
}
