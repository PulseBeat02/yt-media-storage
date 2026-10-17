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

#include "video_encoder.h"

#include "av_utils.h"
#include "configuration.h"

#include <stdexcept>
#include <string>

video_encoder::video_encoder(const std::string& output_path) {
  try {
    initialize_encoder(output_path);
  } catch (...) {
    release();
    throw;
  }
}

video_encoder::~video_encoder() {
  if (!finalized_) {
    try {
      finalize();
    } catch (...) {
    }
  }
  release();
}

void video_encoder::release() {
  if (av_packet_)
    av_packet_free(&av_packet_);
  if (frame_)
    av_frame_free(&frame_);
  if (codec_context_)
    avcodec_free_context(&codec_context_);
  if (format_context_) {
    if (format_context_->pb)
      avio_closep(&format_context_->pb);
    avformat_free_context(format_context_);
    format_context_ = nullptr;
  }
}

void video_encoder::initialize_encoder(const std::string& output_path) {
  int status = avformat_alloc_output_context2(&format_context_, nullptr, nullptr, output_path.c_str());
  if (status < 0 || !format_context_) {
    const std::string message = "Output context could not be created!";
    throw std::runtime_error(message);
  }

  const AVCodec* codec = avcodec_find_encoder_by_name(VIDEO_CODEC);
  if (!codec) {
    const std::string message = "Encoder " + std::string(VIDEO_CODEC) + " could not be found!";
    throw std::runtime_error(message);
  }

  stream_ = avformat_new_stream(format_context_, nullptr);
  if (!stream_) {
    throw std::runtime_error(VIDEO_STREAM_CREATION_MESSAGE);
  }

  codec_context_ = avcodec_alloc_context3(codec);
  if (!codec_context_) {
    throw std::runtime_error(CODEC_CONTEXT_ALLOCATION_MESSAGE);
  }

  codec_context_->width = FRAME_WIDTH;
  codec_context_->height = FRAME_HEIGHT;
  codec_context_->time_base = {1, FRAMES_PER_SECOND};
  codec_context_->framerate = {FRAMES_PER_SECOND, 1};
  codec_context_->gop_size = FRAMES_PER_SECOND;
  codec_context_->max_b_frames = NO_B_FRAMES;
  codec_context_->pix_fmt = AV_PIX_FMT_GRAY8;
  codec_context_->thread_count = AUTOMATIC_THREAD_COUNT;
  codec_context_->thread_type = FF_THREAD_SLICE;

  if (format_context_->oformat->flags & AVFMT_GLOBALHEADER) {
    codec_context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  status = avcodec_open2(codec_context_, codec, nullptr);
  if (status < 0) {
    const std::string message = ffmpeg_failure_message(status, "open the codec");
    throw std::runtime_error(message);
  }

  status = avcodec_parameters_from_context(stream_->codecpar, codec_context_);
  if (status < 0) {
    throw std::runtime_error(CODEC_PARAMETERS_COPY_MESSAGE);
  }
  stream_->time_base = codec_context_->time_base;
  stream_->avg_frame_rate = codec_context_->framerate;

  frame_ = av_frame_alloc();
  if (!frame_) {
    throw std::runtime_error(FRAME_ALLOCATION_MESSAGE);
  }
  frame_->format = codec_context_->pix_fmt;
  frame_->width = codec_context_->width;
  frame_->height = codec_context_->height;
  if (av_frame_get_buffer(frame_, DEFAULT_ALIGNMENT) < 0) {
    throw std::runtime_error(FRAME_BUFFER_ALLOCATION_MESSAGE);
  }

  av_packet_ = av_packet_alloc();
  if (!av_packet_) {
    throw std::runtime_error(PACKET_ALLOCATION_MESSAGE);
  }

  layout_ = compute_frame_layout();
  frame_data_buffer_.reserve(layout_.bytes_per_frame);

  status = avio_open(&format_context_->pb, output_path.c_str(), AVIO_FLAG_WRITE);
  if (status < 0) {
    const std::string message = ffmpeg_failure_message(status, "open the output file");
    throw std::runtime_error(message);
  }

  status = avformat_write_header(format_context_, nullptr);
  if (status < 0) {
    const std::string message = ffmpeg_failure_message(status, "write the header");
    throw std::runtime_error(message);
  }
}

int video_encoder::packets_per_frame() {
  return ::packets_per_frame(compute_frame_layout());
}

void video_encoder::write_pending_packets() {
  while (true) {
    const int status = avcodec_receive_packet(codec_context_, av_packet_);
    if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
      return;
    }
    if (status < 0) {
      const std::string message = ffmpeg_failure_message(status, "receive a packet");
      throw std::runtime_error(message);
    }

    av_packet_rescale_ts(av_packet_, codec_context_->time_base, stream_->time_base);
    av_packet_->stream_index = stream_->index;

    const int write_status = av_interleaved_write_frame(format_context_, av_packet_);
    av_packet_unref(av_packet_);
    if (write_status < 0) {
      const std::string message = ffmpeg_failure_message(write_status, "write a frame");
      throw std::runtime_error(message);
    }
  }
}

void video_encoder::encode_frame() {
  ensure_fresh_writable_frame(frame_);
  embed_data_into_plane(frame_->data[0], frame_->linesize[0], layout_, frame_data_buffer_);
  frame_->pts = frame_index_++;

  if (const int status = avcodec_send_frame(codec_context_, frame_); status < 0) {
    const std::string message = ffmpeg_failure_message(status, "send a frame");
    throw std::runtime_error(message);
  }
  write_pending_packets();
}

void video_encoder::add_packet(const packet& packet) {
  if (finalized_) {
    const std::string message = "Encoder is already finalized!";
    throw std::runtime_error(message);
  }

  if (frame_data_buffer_.size() + packet.bytes.size() > static_cast<std::size_t>(layout_.bytes_per_frame)) {
    flush_frame_buffer();
  }
  frame_data_buffer_.insert(frame_data_buffer_.end(), packet.bytes.begin(), packet.bytes.end());
}

void video_encoder::encode_packets(const std::vector<packet>& packets) {
  for (const packet& packet : packets) {
    add_packet(packet);
  }
}

void video_encoder::flush_frame_buffer() {
  if (frame_data_buffer_.empty())
    return;
  encode_frame();
  frame_data_buffer_.clear();
}

void video_encoder::flush_encoder() {
  if (avcodec_send_frame(codec_context_, nullptr) < 0) {
    return;
  }
  write_pending_packets();
}

void video_encoder::finalize() {
  if (finalized_)
    return;
  finalized_ = true;
  flush_frame_buffer();
  flush_encoder();
  if (const int status = av_write_trailer(format_context_); status < 0) {
    const std::string message = ffmpeg_failure_message(status, "write the trailer");
    throw std::runtime_error(message);
  }
}
