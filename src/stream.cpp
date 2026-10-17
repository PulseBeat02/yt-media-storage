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

#include "stream.h"

#include "av_utils.h"

extern "C" {
#include <libavutil/opt.h>
}

#include <cstring>
#include <stdexcept>
#include <string>

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define MS_HAS_CHANNEL_LAYOUT 1
#else
#define MS_HAS_CHANNEL_LAYOUT 0
#endif

namespace {
constexpr int AUDIO_SAMPLE_RATE = 44100;
constexpr int64_t AUDIO_BIT_RATE = 128000;
constexpr int AUDIO_CHANNELS = 2;
constexpr int KEYFRAME_INTERVAL_SECONDS = 2;

constexpr const char* RTMP_CONTAINER_FORMAT = "flv";
constexpr const char* H264_ENCODER = "libx264";
constexpr const char* X264_PRESET = "ultrafast";
constexpr const char* X264_TUNE = "zerolatency";

constexpr int64_t BITS_PER_KILOBIT = 1000;
constexpr int64_t RATE_CONTROL_BUFFER_SECONDS = 2;

constexpr int Y_PLANE = 0;
constexpr int U_PLANE = 1;
constexpr int V_PLANE = 2;

constexpr int CHROMA_SUBSAMPLING = 2;
constexpr int NEUTRAL_CHROMA = 128;

AVSampleFormat preferred_sample_format(const AVCodec* codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const AVSampleFormat* formats = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, NO_FLAGS,
                                   reinterpret_cast<const void**>(&formats), &count) == 0 &&
      formats && count > 0) {
    return formats[0];
  }
  return AV_SAMPLE_FMT_FLTP;
#else
  return codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
}

int frame_channel_count(const AVFrame* frame) {
#if MS_HAS_CHANNEL_LAYOUT
  return frame->ch_layout.nb_channels;
#else
  return frame->channels;
#endif
}
}

stream_encoder::stream_encoder(const std::string& rtmp_url, const int bitrate_kilobits_per_second, const int width,
                               const int height, const int frames_per_second)
    : width_(width), height_(height), frames_per_second_(frames_per_second) {
  if (width <= 0 || height <= 0 || frames_per_second <= 0 || bitrate_kilobits_per_second <= 0) {
    const std::string message = "Stream resolution, frame rate and bitrate must be positive!";
    throw std::invalid_argument(message);
  }
  layout_ = compute_frame_layout(width, height);
  if (::packets_per_frame(layout_) < 1) {
    const std::string message = "Stream resolution is too small to hold a single packet!";
    throw std::invalid_argument(message);
  }
  try {
    initialize_stream(rtmp_url, bitrate_kilobits_per_second);
  } catch (...) {
    release();
    throw;
  }
}

stream_encoder::~stream_encoder() {
  if (!finalized_) {
    try {
      finalize();
    } catch (...) {
    }
  }
  release();
}

void stream_encoder::release() {
  if (av_packet_) {
    av_packet_free(&av_packet_);
  }
  if (frame_) {
    av_frame_free(&frame_);
  }
  if (audio_frame_) {
    av_frame_free(&audio_frame_);
  }
  if (video_codec_context_) {
    avcodec_free_context(&video_codec_context_);
  }
  if (audio_codec_context_) {
    avcodec_free_context(&audio_codec_context_);
  }
  if (format_context_) {
    if (format_context_->pb) {
      avio_closep(&format_context_->pb);
    }
    avformat_free_context(format_context_);
    format_context_ = nullptr;
  }
}

void stream_encoder::initialize_stream(const std::string& rtmp_url, const int bitrate_kilobits_per_second) {
  int status = avformat_alloc_output_context2(&format_context_, nullptr, RTMP_CONTAINER_FORMAT, rtmp_url.c_str());
  if (status < 0 || !format_context_) {
    const std::string message = "Output context of the RTMP stream could not be created!";
    throw std::runtime_error(message);
  }

  initialize_video_encoder(bitrate_kilobits_per_second);
  initialize_audio_encoder();

  av_packet_ = av_packet_alloc();
  if (!av_packet_) {
    throw std::runtime_error(PACKET_ALLOCATION_MESSAGE);
  }

  frame_data_buffer_.reserve(layout_.bytes_per_frame);

  status = avio_open(&format_context_->pb, rtmp_url.c_str(), AVIO_FLAG_WRITE);
  if (status < 0) {
    const std::string message = ffmpeg_failure_message(status, "open the RTMP stream");
    throw std::runtime_error(message);
  }

  status = avformat_write_header(format_context_, nullptr);
  if (status < 0) {
    const std::string message = ffmpeg_failure_message(status, "write the stream header");
    throw std::runtime_error(message);
  }
}

void stream_encoder::initialize_video_encoder(const int bitrate_kilobits_per_second) {
  const AVCodec* const codec = avcodec_find_encoder_by_name(H264_ENCODER);
  if (!codec) {
    const std::string message = "Encoder libx264 could not be found, so FFmpeg may lack x264 support!";
    throw std::runtime_error(message);
  }

  video_stream_ = avformat_new_stream(format_context_, nullptr);
  if (!video_stream_) {
    throw std::runtime_error(VIDEO_STREAM_CREATION_MESSAGE);
  }

  video_codec_context_ = avcodec_alloc_context3(codec);
  if (!video_codec_context_) {
    throw std::runtime_error(CODEC_CONTEXT_ALLOCATION_MESSAGE);
  }

  video_codec_context_->width = width_;
  video_codec_context_->height = height_;
  video_codec_context_->time_base = {1, frames_per_second_};
  video_codec_context_->framerate = {frames_per_second_, 1};
  video_codec_context_->gop_size = frames_per_second_ * KEYFRAME_INTERVAL_SECONDS;
  video_codec_context_->max_b_frames = NO_B_FRAMES;
  video_codec_context_->pix_fmt = AV_PIX_FMT_YUV420P;
  video_codec_context_->bit_rate = static_cast<int64_t>(bitrate_kilobits_per_second) * BITS_PER_KILOBIT;
  video_codec_context_->rc_max_rate = video_codec_context_->bit_rate;
  video_codec_context_->rc_buffer_size = static_cast<int>(video_codec_context_->bit_rate * RATE_CONTROL_BUFFER_SECONDS);
  video_codec_context_->thread_count = AUTOMATIC_THREAD_COUNT;
  video_codec_context_->thread_type = FF_THREAD_SLICE;

  av_opt_set(video_codec_context_->priv_data, "preset", X264_PRESET, NO_FLAGS);
  av_opt_set(video_codec_context_->priv_data, "tune", X264_TUNE, NO_FLAGS);

  if (format_context_->oformat->flags & AVFMT_GLOBALHEADER) {
    video_codec_context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  int status = avcodec_open2(video_codec_context_, codec, nullptr);
  if (status < 0) {
    const std::string message = ffmpeg_failure_message(status, "open the libx264 codec");
    throw std::runtime_error(message);
  }

  status = avcodec_parameters_from_context(video_stream_->codecpar, video_codec_context_);
  if (status < 0) {
    throw std::runtime_error(CODEC_PARAMETERS_COPY_MESSAGE);
  }
  video_stream_->time_base = video_codec_context_->time_base;
  video_stream_->avg_frame_rate = video_codec_context_->framerate;

  frame_ = av_frame_alloc();
  if (!frame_) {
    throw std::runtime_error(FRAME_ALLOCATION_MESSAGE);
  }
  frame_->format = AV_PIX_FMT_YUV420P;
  frame_->width = width_;
  frame_->height = height_;
  if (av_frame_get_buffer(frame_, DEFAULT_ALIGNMENT) < 0) {
    throw std::runtime_error(FRAME_BUFFER_ALLOCATION_MESSAGE);
  }
}

void stream_encoder::initialize_audio_encoder() {
  const AVCodec* const codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) {
    const std::string message = "AAC encoder could not be found!";
    throw std::runtime_error(message);
  }

  audio_stream_ = avformat_new_stream(format_context_, nullptr);
  if (!audio_stream_) {
    const std::string message = "Audio stream could not be created!";
    throw std::runtime_error(message);
  }

  audio_codec_context_ = avcodec_alloc_context3(codec);
  if (!audio_codec_context_) {
    const std::string message = "Audio codec context could not be allocated!";
    throw std::runtime_error(message);
  }

  audio_codec_context_->sample_fmt = preferred_sample_format(codec);
  audio_codec_context_->sample_rate = AUDIO_SAMPLE_RATE;
  audio_codec_context_->bit_rate = AUDIO_BIT_RATE;
#if MS_HAS_CHANNEL_LAYOUT
  av_channel_layout_default(&audio_codec_context_->ch_layout, AUDIO_CHANNELS);
#else
  audio_codec_context_->channels = AUDIO_CHANNELS;
  audio_codec_context_->channel_layout = AV_CH_LAYOUT_STEREO;
#endif

  if (format_context_->oformat->flags & AVFMT_GLOBALHEADER) {
    audio_codec_context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  int status = avcodec_open2(audio_codec_context_, codec, nullptr);
  if (status < 0) {
    const std::string message = ffmpeg_failure_message(status, "open the AAC codec");
    throw std::runtime_error(message);
  }

  status = avcodec_parameters_from_context(audio_stream_->codecpar, audio_codec_context_);
  if (status < 0) {
    const std::string message = "Audio codec parameters could not be copied!";
    throw std::runtime_error(message);
  }
  audio_stream_->time_base = {1, audio_codec_context_->sample_rate};

  audio_frame_ = av_frame_alloc();
  if (!audio_frame_) {
    const std::string message = "Audio frame could not be allocated!";
    throw std::runtime_error(message);
  }
  audio_frame_->format = audio_codec_context_->sample_fmt;
#if MS_HAS_CHANNEL_LAYOUT
  audio_frame_->ch_layout = audio_codec_context_->ch_layout;
#else
  audio_frame_->channels = audio_codec_context_->channels;
  audio_frame_->channel_layout = audio_codec_context_->channel_layout;
#endif
  audio_frame_->sample_rate = audio_codec_context_->sample_rate;
  audio_frame_->nb_samples = audio_codec_context_->frame_size;

  if (av_frame_get_buffer(audio_frame_, DEFAULT_ALIGNMENT) < 0) {
    const std::string message = "Audio frame buffer could not be allocated!";
    throw std::runtime_error(message);
  }
  if (av_frame_make_writable(audio_frame_) < 0) {
    const std::string message = "Audio frame is not writable!";
    throw std::runtime_error(message);
  }

  const std::size_t plane_bytes =
      static_cast<std::size_t>(audio_frame_->nb_samples) * av_get_bytes_per_sample(audio_codec_context_->sample_fmt);
  for (int channel = 0; channel < frame_channel_count(audio_frame_); ++channel) {
    if (audio_frame_->data[channel]) {
      std::memset(audio_frame_->data[channel], 0, plane_bytes);
    }
  }
}

void stream_encoder::write_pending_packets(AVCodecContext* codec_context, const AVStream* stream,
                                           const bool ignore_errors) {
  while (true) {
    const int status = avcodec_receive_packet(codec_context, av_packet_);
    if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
      return;
    }
    if (status < 0) {
      if (ignore_errors) {
        return;
      }
      const std::string message = ffmpeg_failure_message(status, "receive a packet");
      throw std::runtime_error(message);
    }

    av_packet_rescale_ts(av_packet_, codec_context->time_base, stream->time_base);
    av_packet_->stream_index = stream->index;

    const int write_status = av_interleaved_write_frame(format_context_, av_packet_);
    av_packet_unref(av_packet_);
    if (write_status < 0 && !ignore_errors) {
      const std::string message = ffmpeg_failure_message(write_status, "write a frame to the stream");
      throw std::runtime_error(message);
    }
  }
}

void stream_encoder::write_audio_up_to(const int64_t video_pts) {
  const double video_time = static_cast<double>(video_pts) / frames_per_second_;

  while (true) {
    const double audio_time = static_cast<double>(audio_pts_) / audio_codec_context_->sample_rate;
    if (audio_time > video_time && audio_pts_ > 0) {
      break;
    }

    if (av_frame_make_writable(audio_frame_) < 0) {
      break;
    }
    audio_frame_->pts = audio_pts_;
    audio_pts_ += audio_frame_->nb_samples;

    if (avcodec_send_frame(audio_codec_context_, audio_frame_) < 0) {
      break;
    }
    write_pending_packets(audio_codec_context_, audio_stream_, true);
  }
}

void stream_encoder::encode_frame() {
  ensure_fresh_writable_frame(frame_);

  embed_data_into_plane(frame_->data[Y_PLANE], frame_->linesize[Y_PLANE], layout_, frame_data_buffer_);
  const int chroma_height = height_ / CHROMA_SUBSAMPLING;
  const int chroma_width = width_ / CHROMA_SUBSAMPLING;
  for (int y = 0; y < chroma_height; ++y) {
    std::memset(frame_->data[U_PLANE] + y * frame_->linesize[U_PLANE], NEUTRAL_CHROMA, chroma_width);
    std::memset(frame_->data[V_PLANE] + y * frame_->linesize[V_PLANE], NEUTRAL_CHROMA, chroma_width);
  }

  frame_->pts = frame_index_++;

  if (const int status = avcodec_send_frame(video_codec_context_, frame_); status < 0) {
    const std::string message = ffmpeg_failure_message(status, "send a frame");
    throw std::runtime_error(message);
  }
  write_pending_packets(video_codec_context_, video_stream_, false);
  write_audio_up_to(frame_->pts);
}

void stream_encoder::add_packet(const packet& packet) {
  if (finalized_) {
    const std::string message = "Stream encoder is already finalized!";
    throw std::runtime_error(message);
  }

  if (frame_data_buffer_.size() + packet.bytes.size() > static_cast<std::size_t>(layout_.bytes_per_frame)) {
    flush_frame_buffer();
  }
  frame_data_buffer_.insert(frame_data_buffer_.end(), packet.bytes.begin(), packet.bytes.end());
}

void stream_encoder::encode_packets(const std::vector<packet>& packets) {
  for (const packet& packet : packets) {
    add_packet(packet);
  }
}

void stream_encoder::flush_frame_buffer() {
  if (frame_data_buffer_.empty()) {
    return;
  }
  encode_frame();
  frame_data_buffer_.clear();
}

void stream_encoder::flush_encoders() {
  avcodec_send_frame(video_codec_context_, nullptr);
  write_pending_packets(video_codec_context_, video_stream_, false);

  avcodec_send_frame(audio_codec_context_, nullptr);
  write_pending_packets(audio_codec_context_, audio_stream_, true);
}

void stream_encoder::finalize() {
  if (finalized_) {
    return;
  }
  finalized_ = true;
  flush_frame_buffer();
  flush_encoders();
  if (const int status = av_write_trailer(format_context_); status < 0) {
    const std::string message = ffmpeg_failure_message(status, "write the stream trailer");
    throw std::runtime_error(message);
  }
}
