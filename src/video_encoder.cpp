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

VideoEncoder::VideoEncoder(const std::string &output_path) {
    try {
        init_encoder(output_path);
    } catch (...) {
        release();
        throw;
    }
}

VideoEncoder::~VideoEncoder() {
    if (!finalized_) {
        try {
            finalize();
        } catch (...) {
        }
    }
    release();
}

void VideoEncoder::release() {
    if (av_packet_) av_packet_free(&av_packet_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (format_ctx_) {
        if (format_ctx_->pb) avio_closep(&format_ctx_->pb);
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }
}

void VideoEncoder::init_encoder(const std::string &output_path) {
    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, nullptr, output_path.c_str());
    if (ret < 0 || !format_ctx_) {
        throw std::runtime_error("Failed to create output context");
    }

    const AVCodec *codec = avcodec_find_encoder_by_name(VIDEO_CODEC);
    if (!codec) {
        throw std::runtime_error(std::string("Failed to find encoder: ") + VIDEO_CODEC);
    }

    stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!stream_) {
        throw std::runtime_error("Failed to create stream");
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        throw std::runtime_error("Failed to allocate codec context");
    }

    codec_ctx_->width = FRAME_WIDTH;
    codec_ctx_->height = FRAME_HEIGHT;
    codec_ctx_->time_base = {1, FRAME_FPS};
    codec_ctx_->framerate = {FRAME_FPS, 1};
    codec_ctx_->gop_size = FRAME_FPS;
    codec_ctx_->max_b_frames = 0;
    codec_ctx_->pix_fmt = AV_PIX_FMT_GRAY8;
    codec_ctx_->thread_count = 0;
    codec_ctx_->thread_type = FF_THREAD_SLICE;

    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open codec: " + ffmpeg_error_string(ret));
    }

    ret = avcodec_parameters_from_context(stream_->codecpar, codec_ctx_);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }
    stream_->time_base = codec_ctx_->time_base;
    stream_->avg_frame_rate = codec_ctx_->framerate;

    frame_ = av_frame_alloc();
    if (!frame_) {
        throw std::runtime_error("Failed to allocate frame");
    }
    frame_->format = codec_ctx_->pix_fmt;
    frame_->width = codec_ctx_->width;
    frame_->height = codec_ctx_->height;
    if (av_frame_get_buffer(frame_, 0) < 0) {
        throw std::runtime_error("Failed to allocate frame buffer");
    }

    av_packet_ = av_packet_alloc();
    if (!av_packet_) {
        throw std::runtime_error("Failed to allocate packet");
    }

    layout_ = compute_frame_layout();
    frame_data_buffer_.reserve(layout_.bytes_per_frame);

    ret = avio_open(&format_ctx_->pb, output_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        throw std::runtime_error("Failed to open output file: " + ffmpeg_error_string(ret));
    }

    ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to write header: " + ffmpeg_error_string(ret));
    }
}

int VideoEncoder::packets_per_frame() {
    return ::packets_per_frame(compute_frame_layout());
}

void VideoEncoder::write_pending_packets() {
    while (true) {
        const int ret = avcodec_receive_packet(codec_ctx_, av_packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving packet: " + ffmpeg_error_string(ret));
        }

        av_packet_rescale_ts(av_packet_, codec_ctx_->time_base, stream_->time_base);
        av_packet_->stream_index = stream_->index;

        const int write_ret = av_interleaved_write_frame(format_ctx_, av_packet_);
        av_packet_unref(av_packet_);
        if (write_ret < 0) {
            throw std::runtime_error("Error writing frame: " + ffmpeg_error_string(write_ret));
        }
    }
}

void VideoEncoder::encode_frame() {
    ensure_fresh_writable_frame(frame_);
    embed_data_into_plane(frame_->data[0], frame_->linesize[0], layout_, frame_data_buffer_);
    frame_->pts = frame_index_++;

    if (const int ret = avcodec_send_frame(codec_ctx_, frame_); ret < 0) {
        throw std::runtime_error("Error sending frame: " + ffmpeg_error_string(ret));
    }
    write_pending_packets();
}

void VideoEncoder::add_packet(const Packet &packet) {
    if (finalized_) {
        throw std::runtime_error("Encoder already finalized");
    }

    if (frame_data_buffer_.size() + packet.bytes.size() > static_cast<std::size_t>(layout_.bytes_per_frame)) {
        flush_frame_buffer();
    }
    frame_data_buffer_.insert(frame_data_buffer_.end(), packet.bytes.begin(), packet.bytes.end());
}

void VideoEncoder::encode_packets(const std::vector<Packet> &packets) {
    for (const Packet &packet: packets) {
        add_packet(packet);
    }
}

void VideoEncoder::flush_frame_buffer() {
    if (frame_data_buffer_.empty()) return;
    encode_frame();
    frame_data_buffer_.clear();
}

void VideoEncoder::flush_encoder() {
    if (avcodec_send_frame(codec_ctx_, nullptr) < 0) {
        return;
    }
    write_pending_packets();
}

void VideoEncoder::finalize() {
    if (finalized_) return;
    finalized_ = true;
    flush_frame_buffer();
    flush_encoder();
    if (const int ret = av_write_trailer(format_ctx_); ret < 0) {
        throw std::runtime_error("Error writing trailer: " + ffmpeg_error_string(ret));
    }
}
