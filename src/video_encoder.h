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

#include "frame_layout.h"
#include "packet_format.h"

class VideoEncoder {
public:
  explicit VideoEncoder(const std::string& output_path);

  ~VideoEncoder();

  VideoEncoder(const VideoEncoder&) = delete;

  VideoEncoder& operator=(const VideoEncoder&) = delete;

  VideoEncoder(VideoEncoder&&) = delete;

  VideoEncoder& operator=(VideoEncoder&&) = delete;

  void add_packet(const Packet& packet);

  void encode_packets(const std::vector<Packet>& packets);

  void finalize();

  [[nodiscard]] int64_t frames_written() const {
    return frame_index_;
  }

  [[nodiscard]] static int packets_per_frame();

private:
  AVFormatContext* format_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVStream* stream_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* av_packet_ = nullptr;

  std::vector<std::byte> frame_data_buffer_;
  FrameLayout layout_{};
  int64_t frame_index_ = 0;
  bool finalized_ = false;

  void init_encoder(const std::string& output_path);

  void release();

  void write_pending_packets();

  void encode_frame();

  void flush_frame_buffer();

  void flush_encoder();
};
