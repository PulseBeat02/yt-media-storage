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
#include "configuration.h"
#include "dct_common.h"
#include "video_encoder.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

StreamEncoder::StreamEncoder(const std::string &rtmp_url, const int bitrate_kbps,
                             const int width, const int height, const int fps)
    : width_(width), height_(height), fps_(fps) {
    init_stream(rtmp_url, bitrate_kbps);
}

StreamEncoder::~StreamEncoder() {
    if (!finalized_) {
        try { finalize(); } catch (...) {
        }
    }
    if (av_packet_) av_packet_free(&av_packet_);
    if (frame_) av_frame_free(&frame_);
    if (audio_frame_) av_frame_free(&audio_frame_);
    if (video_codec_ctx_) avcodec_free_context(&video_codec_ctx_);
    if (audio_codec_ctx_) avcodec_free_context(&audio_codec_ctx_);
    if (format_ctx_) {
        if (format_ctx_->pb) avio_closep(&format_ctx_->pb);
        avformat_free_context(format_ctx_);
    }
}

void StreamEncoder::init_audio_encoder() {
    const AVCodec *acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!acodec) {
        throw std::runtime_error("Failed to find AAC encoder");
    }

    audio_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!audio_stream_) {
        throw std::runtime_error("Failed to create audio stream");
    }

    audio_codec_ctx_ = avcodec_alloc_context3(acodec);
    if (!audio_codec_ctx_) {
        throw std::runtime_error("Failed to allocate audio codec context");
    }

#if LIBAVCODEC_VERSION_MAJOR >= 62
    const enum AVSampleFormat *sample_fmts = nullptr;
    int num_sample_fmts = 0;
    avcodec_get_supported_config(nullptr, acodec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                 reinterpret_cast<const void **>(&sample_fmts),
                                 &num_sample_fmts);
    audio_codec_ctx_->sample_fmt = (sample_fmts && num_sample_fmts > 0)
                                       ? sample_fmts[0]
                                       : AV_SAMPLE_FMT_FLTP;
#else
    audio_codec_ctx_->sample_fmt = acodec->sample_fmts
                                       ? acodec->sample_fmts[0]
                                       : AV_SAMPLE_FMT_FLTP;
#endif
    audio_codec_ctx_->sample_rate = 44100;
    audio_codec_ctx_->bit_rate = 128000;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&audio_codec_ctx_->ch_layout, 2);
#else
    audio_codec_ctx_->channels = 2;
    audio_codec_ctx_->channel_layout = AV_CH_LAYOUT_STEREO;
#endif

    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        audio_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(audio_codec_ctx_, acodec, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open AAC codec");
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
#if LIBAVUTIL_VERSION_MAJOR >= 57
    audio_frame_->ch_layout = audio_codec_ctx_->ch_layout;
#else
    audio_frame_->channels = audio_codec_ctx_->channels;
    audio_frame_->channel_layout = audio_codec_ctx_->channel_layout;
#endif
    audio_frame_->sample_rate = audio_codec_ctx_->sample_rate;
    audio_frame_->nb_samples = audio_codec_ctx_->frame_size;

    ret = av_frame_get_buffer(audio_frame_, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to allocate audio frame buffer");
    }

    av_frame_make_writable(audio_frame_);
#if LIBAVUTIL_VERSION_MAJOR >= 57
    const int num_channels = audio_frame_->ch_layout.nb_channels;
#else
    const int num_channels = audio_frame_->channels;
#endif
    for (int ch = 0; ch < num_channels; ++ch) {
        if (audio_frame_->data[ch]) {
            std::memset(audio_frame_->data[ch], 0,
                        static_cast<size_t>(audio_frame_->nb_samples) *
                        av_get_bytes_per_sample(audio_codec_ctx_->sample_fmt));
        }
    }
}

void StreamEncoder::write_audio_up_to(const int64_t video_pts) {
    const double video_time = static_cast<double>(video_pts) / FRAME_FPS;

    while (true) {
        if (const double audio_time = static_cast<double>(audio_pts_) / audio_codec_ctx_->sample_rate;
            audio_time > video_time && audio_pts_ > 0)
            break;

        av_frame_make_writable(audio_frame_);
        audio_frame_->pts = audio_pts_;
        audio_pts_ += audio_frame_->nb_samples;

        int ret = avcodec_send_frame(audio_codec_ctx_, audio_frame_);
        if (ret < 0) break;

        while (true) {
            ret = avcodec_receive_packet(audio_codec_ctx_, av_packet_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            av_packet_rescale_ts(av_packet_, audio_codec_ctx_->time_base,
                                 audio_stream_->time_base);
            av_packet_->stream_index = audio_stream_->index;
            av_interleaved_write_frame(format_ctx_, av_packet_);
            av_packet_unref(av_packet_);
        }
    }
}

void StreamEncoder::init_stream(const std::string &rtmp_url, const int bitrate_kbps) {
    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, "flv", rtmp_url.c_str());
    if (ret < 0 || !format_ctx_) {
        throw std::runtime_error("Failed to create output context for RTMP stream");
    }

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
    video_codec_ctx_->gop_size = 30;
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

    ret = avcodec_open2(video_codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char error_buffer[256];
        av_strerror(ret, error_buffer, sizeof(error_buffer));
        throw std::runtime_error(std::string("Failed to open libx264 codec: ") + error_buffer);
    }

    ret = avcodec_parameters_from_context(video_stream_->codecpar, video_codec_ctx_);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }

    video_stream_->time_base = video_codec_ctx_->time_base;

    init_audio_encoder();

    frame_ = av_frame_alloc();
    if (!frame_) {
        throw std::runtime_error("Failed to allocate frame");
    }

    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = width_;
    frame_->height = height_;

    ret = av_frame_get_buffer(frame_, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to allocate frame buffer");
    }

    av_packet_ = av_packet_alloc();
    if (!av_packet_) {
        throw std::runtime_error("Failed to allocate packet");
    }

    layout_ = compute_frame_layout(width_, height_);
    frame_data_buffer_.reserve(layout_.bytes_per_frame);

    ret = avio_open(&format_ctx_->pb, rtmp_url.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char error_buffer[256];
        av_strerror(ret, error_buffer, sizeof(error_buffer));
        throw std::runtime_error(std::string("Failed to open RTMP stream: ") + error_buffer);
    }

    ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        char error_buffer[256];
        av_strerror(ret, error_buffer, sizeof(error_buffer));
        throw std::runtime_error(std::string("Failed to write stream header: ") + error_buffer);
    }
}

int StreamEncoder::packets_per_frame() {
    const auto layout = compute_frame_layout();
    constexpr std::size_t packet_size = HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES;
    return static_cast<int>(layout.bytes_per_frame / packet_size);
}

void StreamEncoder::embed_data_in_frame(const std::vector<std::byte> &data) const {
#if defined(__APPLE__) && defined(_OPENMP)
    const auto &blocks = get_precomputed_blocks();
    const auto &patterns = blocks.patterns;
#else
    const auto &patterns = get_precomputed_blocks().patterns;
#endif

    ensure_fresh_writable_frame(frame_);

    const std::size_t total_bits = data.size() * 8;
    const int total_blocks = layout_.blocks_per_row * layout_.blocks_per_col;
    const int active_blocks = static_cast<int>(
        std::min(static_cast<std::size_t>(total_blocks),
                 (total_bits + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK));
    const auto *src = reinterpret_cast<const uint8_t *>(data.data());
    const int blocks_per_row = layout_.blocks_per_row;

    uint8_t *y_base = frame_->data[0];
    const int y_stride = frame_->linesize[0];
    const int first_uncovered_row = (active_blocks / blocks_per_row) * 8;
    for (int y = first_uncovered_row; y < height_; ++y)
        std::memset(y_base + y * y_stride, 128, width_);

    const int chroma_h = height_ / 2;
    const int chroma_w = width_ / 2;
    for (int y = 0; y < chroma_h; ++y) {
        std::memset(frame_->data[1] + y * frame_->linesize[1], 128, chroma_w);
        std::memset(frame_->data[2] + y * frame_->linesize[2], 128, chroma_w);
    }

#pragma omp parallel for schedule(static)
    for (int block_idx = 0; block_idx < active_blocks; ++block_idx) {
        const int block_row = block_idx / blocks_per_row;
        const int block_col = block_idx % blocks_per_row;
        const int base_x = block_col * 8;
        const int base_y = block_row * 8;

        const std::size_t bit_start = static_cast<std::size_t>(block_idx) * BITS_PER_BLOCK;
        const std::size_t bit_end = std::min(bit_start + BITS_PER_BLOCK, total_bits);

        int pattern = 0;
        for (std::size_t bit_index = bit_start; bit_index < bit_end; ++bit_index) {
            const std::size_t byte_idx = bit_index / 8;
            const int bit_pos = 7 - static_cast<int>(bit_index % 8);
            const int bit = (src[byte_idx] >> bit_pos) & 1;
            pattern = (pattern << 1) | bit;
        }

        const int bits_extracted = static_cast<int>(bit_end - bit_start);
        pattern <<= (BITS_PER_BLOCK - bits_extracted);

        const auto &block = patterns[pattern];
        for (int y = 0; y < 8; ++y) {
            std::memcpy(y_base + (base_y + y) * y_stride + base_x,
                        block[y], 8);
        }
    }
}

void StreamEncoder::encode_frame() {
    frame_->pts = frame_index_++;

    int ret = avcodec_send_frame(video_codec_ctx_, frame_);
    if (ret < 0) {
        throw std::runtime_error("Error sending frame");
    }

    while (true) {
        ret = avcodec_receive_packet(video_codec_ctx_, av_packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving packet");
        }

        av_packet_rescale_ts(av_packet_, video_codec_ctx_->time_base, video_stream_->time_base);
        av_packet_->stream_index = video_stream_->index;

        ret = av_interleaved_write_frame(format_ctx_, av_packet_);
        if (ret < 0) {
            throw std::runtime_error("Error writing frame to stream");
        }

        av_packet_unref(av_packet_);
    }

    write_audio_up_to(frame_->pts);
}

void StreamEncoder::add_packet(const Packet &packet) {
    if (finalized_) {
        throw std::runtime_error("Stream encoder already finalized");
    }

    if (const auto max_bytes = static_cast<std::size_t>(layout_.bytes_per_frame);
        frame_data_buffer_.size() + packet.bytes.size() > max_bytes) {
        flush_frame_buffer();
    }

    frame_data_buffer_.insert(frame_data_buffer_.end(),
                              packet.bytes.begin(),
                              packet.bytes.end());
}

void StreamEncoder::encode_packets(const std::vector<Packet> &packets) {
    for (const auto &pkt: packets) {
        add_packet(pkt);
    }
}

void StreamEncoder::flush_frame_buffer() {
    if (frame_data_buffer_.empty()) return;

    embed_data_in_frame(frame_data_buffer_);
    encode_frame();
    frame_data_buffer_.clear();
}

void StreamEncoder::flush_encoder() const {
    avcodec_send_frame(video_codec_ctx_, nullptr);
    while (true) {
        const int ret = avcodec_receive_packet(video_codec_ctx_, av_packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) throw std::runtime_error("Error flushing video encoder");

        av_packet_rescale_ts(av_packet_, video_codec_ctx_->time_base, video_stream_->time_base);
        av_packet_->stream_index = video_stream_->index;
        av_interleaved_write_frame(format_ctx_, av_packet_);
        av_packet_unref(av_packet_);
    }

    avcodec_send_frame(audio_codec_ctx_, nullptr);
    while (true) {
        const int ret = avcodec_receive_packet(audio_codec_ctx_, av_packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        av_packet_rescale_ts(av_packet_, audio_codec_ctx_->time_base, audio_stream_->time_base);
        av_packet_->stream_index = audio_stream_->index;
        av_interleaved_write_frame(format_ctx_, av_packet_);
        av_packet_unref(av_packet_);
    }
}

void StreamEncoder::finalize() {
    if (finalized_) return;
    finalized_ = true;
    flush_frame_buffer();
    flush_encoder();
    av_write_trailer(format_ctx_);
}
