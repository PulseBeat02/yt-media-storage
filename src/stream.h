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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "configuration.h"
#include "frame_layout.h"
#include "packet_format.h"

class StreamEncoder {
public:
  explicit StreamEncoder(const std::string& rtmp_url, int bitrate_kbps = FRAME_BITRATE, int width = FRAME_WIDTH_STREAM,
                         int height = FRAME_HEIGHT_STREAM, int fps = FRAME_FPS);

  ~StreamEncoder();

  StreamEncoder(const StreamEncoder&) = delete;

  StreamEncoder& operator=(const StreamEncoder&) = delete;

  StreamEncoder(StreamEncoder&&) = delete;

  StreamEncoder& operator=(StreamEncoder&&) = delete;

  void add_packet(const Packet& packet);

  void encode_packets(const std::vector<Packet>& packets);

  void finalize();

  [[nodiscard]] int64_t frames_written() const {
    return frame_index_;
  }

  [[nodiscard]] int packets_per_frame() const {
    return ::packets_per_frame(layout_);
  }

  [[nodiscard]] const FrameLayout& layout() const {
    return layout_;
  }

private:
  AVFormatContext* format_ctx_ = nullptr;

  AVCodecContext* video_codec_ctx_ = nullptr;
  AVStream* video_stream_ = nullptr;
  AVFrame* frame_ = nullptr;

  AVCodecContext* audio_codec_ctx_ = nullptr;
  AVStream* audio_stream_ = nullptr;
  AVFrame* audio_frame_ = nullptr;
  int64_t audio_pts_ = 0;

  AVPacket* av_packet_ = nullptr;

  int width_;
  int height_;
  int fps_;

  std::vector<std::byte> frame_data_buffer_;
  FrameLayout layout_{};
  int64_t frame_index_ = 0;
  bool finalized_ = false;

  void init_stream(const std::string& rtmp_url, int bitrate_kbps);

  void init_video_encoder(int bitrate_kbps);

  void init_audio_encoder();

  void release();

  void write_pending_packets(AVCodecContext* codec_ctx, const AVStream* stream, bool ignore_errors);

  void write_audio_up_to(int64_t video_pts);

  void encode_frame();

  void flush_frame_buffer();

  void flush_encoders();
};
