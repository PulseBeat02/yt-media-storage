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

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define MS_HAS_CH_LAYOUT 1
#else
#define MS_HAS_CH_LAYOUT 0
#endif

namespace {
    constexpr int AUDIO_SAMPLE_RATE = 44100;
    constexpr int64_t AUDIO_BIT_RATE = 128000;
    constexpr int AUDIO_CHANNELS = 2;
    constexpr int KEYFRAME_INTERVAL_SECONDS = 2;

    AVSampleFormat preferred_sample_format(const AVCodec *codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
        const AVSampleFormat *formats = nullptr;
        int count = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                         reinterpret_cast<const void **>(&formats), &count) == 0 &&
            formats && count > 0) {
            return formats[0];
        }
        return AV_SAMPLE_FMT_FLTP;
#else
        return codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
    }

    int frame_channel_count(const AVFrame *frame) {
#if MS_HAS_CH_LAYOUT
        return frame->ch_layout.nb_channels;
#else
        return frame->channels;
#endif
    }
}

StreamEncoder::StreamEncoder(const std::string &rtmp_url, const int bitrate_kbps,
                             const int width, const int height, const int fps)
    : width_(width), height_(height), fps_(fps) {
    if (width <= 0 || height <= 0 || fps <= 0 || bitrate_kbps <= 0) {
        throw std::invalid_argument("Stream resolution, fps and bitrate must be positive");
    }
    layout_ = compute_frame_layout(width, height);
    if (::packets_per_frame(layout_) < 1) {
        throw std::invalid_argument("Stream resolution is too small to hold a single packet");
    }
    try {
        init_stream(rtmp_url, bitrate_kbps);
    } catch (...) {
        release();
        throw;
    }
}

StreamEncoder::~StreamEncoder() {
    if (!finalized_) {
        try {
            finalize();
        } catch (...) {
        }
    }
    release();
}

void StreamEncoder::release() {
    if (av_packet_) av_packet_free(&av_packet_);
    if (frame_) av_frame_free(&frame_);
    if (audio_frame_) av_frame_free(&audio_frame_);
    if (video_codec_ctx_) avcodec_free_context(&video_codec_ctx_);
    if (audio_codec_ctx_) avcodec_free_context(&audio_codec_ctx_);
    if (format_ctx_) {
        if (format_ctx_->pb) avio_closep(&format_ctx_->pb);
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }
}

void StreamEncoder::init_stream(const std::string &rtmp_url, const int bitrate_kbps) {
    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, "flv", rtmp_url.c_str());
    if (ret < 0 || !format_ctx_) {
        throw std::runtime_error("Failed to create output context for RTMP stream");
    }

    init_video_encoder(bitrate_kbps);
    init_audio_encoder();

    av_packet_ = av_packet_alloc();
    if (!av_packet_) {
        throw std::runtime_error("Failed to allocate packet");
    }

    frame_data_buffer_.reserve(layout_.bytes_per_frame);

    ret = avio_open(&format_ctx_->pb, rtmp_url.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        throw std::runtime_error("Failed to open RTMP stream: " + ffmpeg_error_string(ret));
    }

    ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to write stream header: " + ffmpeg_error_string(ret));
    }
}

void StreamEncoder::init_video_encoder(const int bitrate_kbps) {
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        throw std::runtime_error("Failed to find libx264 encoder (is FFmpeg built with x264?)");
    }

    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        throw std::runtime_error("Failed to create video stream");
    }

    video_codec_ctx_ = avcodec_alloc_context3(codec);
    if (!video_codec_ctx_) {
        throw std::runtime_error("Failed to allocate codec context");
    }

    video_codec_ctx_->width = width_;
    video_codec_ctx_->height = height_;
    video_codec_ctx_->time_base = {1, fps_};
    video_codec_ctx_->framerate = {fps_, 1};
    video_codec_ctx_->gop_size = fps_ * KEYFRAME_INTERVAL_SECONDS;
    video_codec_ctx_->max_b_frames = 0;
    video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    video_codec_ctx_->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
    video_codec_ctx_->rc_max_rate = video_codec_ctx_->bit_rate;
    video_codec_ctx_->rc_buffer_size = static_cast<int>(video_codec_ctx_->bit_rate * 2);
    video_codec_ctx_->thread_count = 0;
    video_codec_ctx_->thread_type = FF_THREAD_SLICE;

    av_opt_set(video_codec_ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(video_codec_ctx_->priv_data, "tune", "zerolatency", 0);

    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        video_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(video_codec_ctx_, codec, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open libx264 codec: " + ffmpeg_error_string(ret));
    }

    ret = avcodec_parameters_from_context(video_stream_->codecpar, video_codec_ctx_);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }
    video_stream_->time_base = video_codec_ctx_->time_base;

    frame_ = av_frame_alloc();
    if (!frame_) {
        throw std::runtime_error("Failed to allocate frame");
    }
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = width_;
    frame_->height = height_;
    if (av_frame_get_buffer(frame_, 0) < 0) {
        throw std::runtime_error("Failed to allocate frame buffer");
    }
}

void StreamEncoder::init_audio_encoder() {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        throw std::runtime_error("Failed to find AAC encoder");
    }

    audio_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!audio_stream_) {
        throw std::runtime_error("Failed to create audio stream");
    }

    audio_codec_ctx_ = avcodec_alloc_context3(codec);
    if (!audio_codec_ctx_) {
        throw std::runtime_error("Failed to allocate audio codec context");
    }

    audio_codec_ctx_->sample_fmt = preferred_sample_format(codec);
    audio_codec_ctx_->sample_rate = AUDIO_SAMPLE_RATE;
    audio_codec_ctx_->bit_rate = AUDIO_BIT_RATE;
#if MS_HAS_CH_LAYOUT
    av_channel_layout_default(&audio_codec_ctx_->ch_layout, AUDIO_CHANNELS);
#else
    audio_codec_ctx_->channels = AUDIO_CHANNELS;
    audio_codec_ctx_->channel_layout = AV_CH_LAYOUT_STEREO;
#endif

    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        audio_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(audio_codec_ctx_, codec, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open AAC codec: " + ffmpeg_error_string(ret));
    }

    ret = avcodec_parameters_from_context(audio_stream_->codecpar, audio_codec_ctx_);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy audio codec parameters");
    }
    audio_stream_->time_base = {1, audio_codec_ctx_->sample_rate};

    audio_frame_ = av_frame_alloc();
    if (!audio_frame_) {
        throw std::runtime_error("Failed to allocate audio frame");
    }
    audio_frame_->format = audio_codec_ctx_->sample_fmt;
#if MS_HAS_CH_LAYOUT
    audio_frame_->ch_layout = audio_codec_ctx_->ch_layout;
#else
    audio_frame_->channels = audio_codec_ctx_->channels;
    audio_frame_->channel_layout = audio_codec_ctx_->channel_layout;
#endif
    audio_frame_->sample_rate = audio_codec_ctx_->sample_rate;
    audio_frame_->nb_samples = audio_codec_ctx_->frame_size;

    if (av_frame_get_buffer(audio_frame_, 0) < 0) {
        throw std::runtime_error("Failed to allocate audio frame buffer");
    }
    if (av_frame_make_writable(audio_frame_) < 0) {
        throw std::runtime_error("Audio frame not writable");
    }

    const std::size_t plane_bytes = static_cast<std::size_t>(audio_frame_->nb_samples) *
                                    av_get_bytes_per_sample(audio_codec_ctx_->sample_fmt);
    for (int ch = 0; ch < frame_channel_count(audio_frame_); ++ch) {
        if (audio_frame_->data[ch]) {
            std::memset(audio_frame_->data[ch], 0, plane_bytes);
        }
    }
}

void StreamEncoder::write_pending_packets(AVCodecContext *codec_ctx, const AVStream *stream,
                                          const bool ignore_errors) {
    while (true) {
        const int ret = avcodec_receive_packet(codec_ctx, av_packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        if (ret < 0) {
            if (ignore_errors) return;
            throw std::runtime_error("Error receiving packet: " + ffmpeg_error_string(ret));
        }

        av_packet_rescale_ts(av_packet_, codec_ctx->time_base, stream->time_base);
        av_packet_->stream_index = stream->index;

        const int write_ret = av_interleaved_write_frame(format_ctx_, av_packet_);
        av_packet_unref(av_packet_);
        if (write_ret < 0 && !ignore_errors) {
            throw std::runtime_error("Error writing frame to stream: " + ffmpeg_error_string(write_ret));
        }
    }
}

void StreamEncoder::write_audio_up_to(const int64_t video_pts) {
    const double video_time = static_cast<double>(video_pts) / fps_;

    while (true) {
        const double audio_time = static_cast<double>(audio_pts_) / audio_codec_ctx_->sample_rate;
        if (audio_time > video_time && audio_pts_ > 0) {
            break;
        }

        if (av_frame_make_writable(audio_frame_) < 0) {
            break;
        }
        audio_frame_->pts = audio_pts_;
        audio_pts_ += audio_frame_->nb_samples;

        if (avcodec_send_frame(audio_codec_ctx_, audio_frame_) < 0) {
            break;
        }
        write_pending_packets(audio_codec_ctx_, audio_stream_, true);
    }
}

void StreamEncoder::encode_frame() {
    ensure_fresh_writable_frame(frame_);

    embed_data_into_plane(frame_->data[0], frame_->linesize[0], layout_, frame_data_buffer_);
    const int chroma_height = height_ / 2;
    const int chroma_width = width_ / 2;
    for (int y = 0; y < chroma_height; ++y) {
        std::memset(frame_->data[1] + y * frame_->linesize[1], 128, chroma_width);
        std::memset(frame_->data[2] + y * frame_->linesize[2], 128, chroma_width);
    }

    frame_->pts = frame_index_++;

    if (const int ret = avcodec_send_frame(video_codec_ctx_, frame_); ret < 0) {
        throw std::runtime_error("Error sending frame: " + ffmpeg_error_string(ret));
    }
    write_pending_packets(video_codec_ctx_, video_stream_, false);
    write_audio_up_to(frame_->pts);
}

void StreamEncoder::add_packet(const Packet &packet) {
    if (finalized_) {
        throw std::runtime_error("Stream encoder already finalized");
    }

    if (frame_data_buffer_.size() + packet.bytes.size() > static_cast<std::size_t>(layout_.bytes_per_frame)) {
        flush_frame_buffer();
    }
    frame_data_buffer_.insert(frame_data_buffer_.end(), packet.bytes.begin(), packet.bytes.end());
}

void StreamEncoder::encode_packets(const std::vector<Packet> &packets) {
    for (const Packet &packet: packets) {
        add_packet(packet);
    }
}

void StreamEncoder::flush_frame_buffer() {
    if (frame_data_buffer_.empty()) return;
    encode_frame();
    frame_data_buffer_.clear();
}

void StreamEncoder::flush_encoders() {
    avcodec_send_frame(video_codec_ctx_, nullptr);
    write_pending_packets(video_codec_ctx_, video_stream_, false);

    avcodec_send_frame(audio_codec_ctx_, nullptr);
    write_pending_packets(audio_codec_ctx_, audio_stream_, true);
}

void StreamEncoder::finalize() {
    if (finalized_) return;
    finalized_ = true;
    flush_frame_buffer();
    flush_encoders();
    if (const int ret = av_write_trailer(format_ctx_); ret < 0) {
        throw std::runtime_error("Error writing stream trailer: " + ffmpeg_error_string(ret));
    }
}
