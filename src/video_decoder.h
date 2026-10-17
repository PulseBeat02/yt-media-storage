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
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "frame_layout.h"
#include "gray_lookup.h"

class VideoDecoder {
public:
  using PacketList = std::vector<std::vector<std::byte>>;

  explicit VideoDecoder(const std::string& input_path);

  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;

  VideoDecoder& operator=(const VideoDecoder&) = delete;

  VideoDecoder(VideoDecoder&&) = delete;

  VideoDecoder& operator=(VideoDecoder&&) = delete;

  [[nodiscard]] PacketList decode_next_frame();

  [[nodiscard]] PacketList decode_all_frames();

  [[nodiscard]] int64_t frames_read() const {
    return frame_index_;
  }

  [[nodiscard]] int64_t total_frames() const;

  [[nodiscard]] bool is_eof() const {
    return eof_;
  }

private:
  AVFormatContext* format_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* gray_frame_ = nullptr;
  AVPacket* av_packet_ = nullptr;
  SwsContext* sws_ctx_ = nullptr;

  int video_stream_index_ = -1;
  int64_t frame_index_ = 0;
  bool eof_ = false;
  bool draining_ = false;
  bool pending_packet_ = false;
  bool is_gray8_ = false;
  FrameLayout layout_{};
  std::optional<gray_lookup_table> gray_lookup_;
  std::vector<std::byte> extract_buffer_;

  void init_decoder(const std::string& input_path);

  void release();

  [[nodiscard]] const AVFrame* gray_plane_frame() const;

  static void extract_packets_from_buffer(std::vector<std::byte>& accumulated, PacketList& out_packets);

  [[nodiscard]] PacketList consume_decoded_frame();

  bool feed_next_packet();
};
