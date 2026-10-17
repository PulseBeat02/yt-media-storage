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

#ifndef MEDIA_STORAGE_AV_UTILS_H
#define MEDIA_STORAGE_AV_UTILS_H

#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

inline constexpr int DEFAULT_ALIGNMENT = 0;
inline constexpr int AUTOMATIC_THREAD_COUNT = 0;
inline constexpr int FIRST_ROW = 0;
inline constexpr int NO_B_FRAMES = 0;
inline constexpr int NO_FLAGS = 0;

inline constexpr const char* FRAME_ALLOCATION_MESSAGE = "Frame could not be allocated!";
inline constexpr const char* FRAME_BUFFER_ALLOCATION_MESSAGE = "Frame buffer could not be allocated!";
inline constexpr const char* PACKET_ALLOCATION_MESSAGE = "Packet could not be allocated!";
inline constexpr const char* CODEC_CONTEXT_ALLOCATION_MESSAGE = "Codec context could not be allocated!";
inline constexpr const char* CODEC_PARAMETERS_COPY_MESSAGE = "Codec parameters could not be copied!";
inline constexpr const char* VIDEO_STREAM_CREATION_MESSAGE = "Video stream could not be created!";

inline std::string ffmpeg_error_string(const int error_code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(error_code, buffer, sizeof(buffer));
  return buffer;
}

inline std::string ffmpeg_failure_message(const int error_code, const std::string_view action) {
  const std::string error_text = ffmpeg_error_string(error_code);
  return "FFmpeg error " + std::to_string(error_code) + " (" + error_text + ") occurred while trying to " +
         std::string(action) + "!";
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
  if (av_frame_get_buffer(frame, DEFAULT_ALIGNMENT) < 0) {
    throw std::runtime_error(FRAME_BUFFER_ALLOCATION_MESSAGE);
  }
}

#endif
