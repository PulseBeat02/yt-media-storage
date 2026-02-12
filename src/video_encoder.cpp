// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) Brandon Li <https://brandonli.me/>
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
#include "configuration.h"
#include "dct_common.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

FrameLayout compute_frame_layout() {
    FrameLayout layout{};
    layout.frame_width = FRAME_WIDTH;
    layout.frame_height = FRAME_HEIGHT;
    layout.blocks_per_row = FRAME_WIDTH / 8;
    layout.blocks_per_col = FRAME_HEIGHT / 8;
    layout.total_blocks = layout.blocks_per_row * layout.blocks_per_col;
    layout.bits_per_frame = layout.total_blocks * BITS_PER_BLOCK;
    layout.bytes_per_frame = layout.bits_per_frame / 8;
    return layout;
}

std::size_t max_packet_bytes_per_frame() {
    return static_cast<std::size_t>(compute_frame_layout().bytes_per_frame);
}

VideoEncoder::VideoEncoder(const std::string &output_path) {
    init_encoder(output_path);
}

VideoEncoder::~VideoEncoder() {
    if (!finalized) {
        try { finalize(); } catch (...) {
        }
    }
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (av_packet) av_packet_free(&av_packet);
    if (frame) av_frame_free(&frame);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (format_ctx) {
        if (format_ctx->pb) avio_closep(&format_ctx->pb);
        avformat_free_context(format_ctx);
    }
}

void VideoEncoder::init_encoder(const std::string &output_path) {
    int ret = avformat_alloc_output_context2(&format_ctx, nullptr, nullptr, output_path.c_str());
    if (ret < 0 || !format_ctx) {
        throw std::runtime_error("Failed to create output context");
    }

    const AVCodec *codec = avcodec_find_encoder_by_name(VIDEO_CODEC.c_str());
    if (!codec) {
        throw std::runtime_error("Failed to find encoder: " + VIDEO_CODEC);
    }

    stream = avformat_new_stream(format_ctx, nullptr);
    if (!stream) {
        throw std::runtime_error("Failed to create stream");
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        throw std::runtime_error("Failed to allocate codec context");
    }

    codec_ctx->width = FRAME_WIDTH;
    codec_ctx->height = FRAME_HEIGHT;
    codec_ctx->time_base = {1, FRAME_FPS};
    codec_ctx->framerate = {FRAME_FPS, 1};
    codec_ctx->gop_size = 30;
    codec_ctx->max_b_frames = 0;
    codec_ctx->pix_fmt = AV_PIX_FMT_GRAY8;

    if (format_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret < 0) {
        char error_buffer[256];
        av_strerror(ret, error_buffer, sizeof(error_buffer));
        throw std::runtime_error(std::string("Failed to open codec: ") + error_buffer);
    }

    ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }

    stream->time_base = codec_ctx->time_base;

    frame = av_frame_alloc();
    if (!frame) {
        throw std::runtime_error("Failed to allocate frame");
    }

    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to allocate frame buffer");
    }

    av_packet = av_packet_alloc();
    if (!av_packet) {
        throw std::runtime_error("Failed to allocate packet");
    }

    if (codec_ctx->pix_fmt != AV_PIX_FMT_GRAY8) {
        gray_buffer.resize(static_cast<std::size_t>(FRAME_WIDTH) * FRAME_HEIGHT);
        sws_ctx = sws_getContext(
            FRAME_WIDTH, FRAME_HEIGHT, AV_PIX_FMT_GRAY8,
            FRAME_WIDTH, FRAME_HEIGHT, codec_ctx->pix_fmt,
            SWS_POINT, nullptr, nullptr, nullptr
        );
        if (!sws_ctx) {
            throw std::runtime_error("Failed to create swscale context");
        }
    }

    layout_ = compute_frame_layout();
    frame_data_buffer.reserve(layout_.bytes_per_frame);

    ret = avio_open(&format_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        throw std::runtime_error("Failed to open output file");
    }

    ret = avformat_write_header(format_ctx, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to write header");
    }
}

int VideoEncoder::packets_per_frame() {
    const auto layout = compute_frame_layout();
    constexpr std::size_t packet_size = HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES;
    return static_cast<int>(layout.bytes_per_frame / packet_size);
}

void VideoEncoder::embed_data_in_frame(const std::vector<std::byte> &data) {
    const auto &blocks = get_precomputed_blocks();
    const auto &patterns = blocks.patterns;

    const std::size_t total_bits = data.size() * 8;
    const int total_blocks = layout_.blocks_per_row * layout_.blocks_per_col;
    const int active_blocks = static_cast<int>(
        std::min(static_cast<std::size_t>(total_blocks),
                 (total_bits + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK));
    const auto *src = reinterpret_cast<const uint8_t *>(data.data());
    const int blocks_per_row = layout_.blocks_per_row;

    uint8_t *dst_base;
    int dst_stride;
    if (sws_ctx) {
        dst_base = gray_buffer.data();
        dst_stride = FRAME_WIDTH;
        std::memset(dst_base, 128, gray_buffer.size());
    } else {
        av_frame_make_writable(frame);
        dst_base = frame->data[0];
        dst_stride = frame->linesize[0];
        for (int y = 0; y < FRAME_HEIGHT; ++y)
            std::memset(dst_base + y * dst_stride, 128, FRAME_WIDTH);
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
            std::memcpy(dst_base + (base_y + y) * dst_stride + base_x,
                        block[y], 8);
        }
    }

    if (sws_ctx) {
        const uint8_t *src_data[1] = {gray_buffer.data()};
        constexpr int src_linesize[1] = {FRAME_WIDTH};
        sws_scale(sws_ctx, src_data, src_linesize, 0, FRAME_HEIGHT,
                  frame->data, frame->linesize);
    }
}

void VideoEncoder::encode_frame() {
    int ret = av_frame_make_writable(frame);
    if (ret < 0) {
        throw std::runtime_error("Frame not writable");
    }

    frame->pts = frame_index++;

    ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0) {
        throw std::runtime_error("Error sending frame");
    }

    while (true) {
        ret = avcodec_receive_packet(codec_ctx, av_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving packet");
        }

        av_packet_rescale_ts(av_packet, codec_ctx->time_base, stream->time_base);
        av_packet->stream_index = stream->index;

        ret = av_interleaved_write_frame(format_ctx, av_packet);
        if (ret < 0) {
            throw std::runtime_error("Error writing frame");
        }

        av_packet_unref(av_packet);
    }
}

void VideoEncoder::add_packet(const Packet &packet) {
    if (finalized) {
        throw std::runtime_error("Encoder already finalized");
    }

    if (const auto max_bytes = static_cast<std::size_t>(layout_.bytes_per_frame);
        frame_data_buffer.size() + packet.bytes.size() > max_bytes) {
        flush_frame_buffer();
    }

    frame_data_buffer.insert(frame_data_buffer.end(),
                             packet.bytes.begin(),
                             packet.bytes.end());
}

void VideoEncoder::encode_packets(const std::vector<Packet> &packets) {
    for (const auto &pkt: packets) {
        add_packet(pkt);
    }
}

void VideoEncoder::flush_frame_buffer() {
    if (frame_data_buffer.empty()) return;

    embed_data_in_frame(frame_data_buffer);
    encode_frame();
    frame_data_buffer.clear();
}

void VideoEncoder::flush_encoder() const {
    int ret = avcodec_send_frame(codec_ctx, nullptr);

    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx, av_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error flushing encoder");
        }

        av_packet_rescale_ts(av_packet, codec_ctx->time_base, stream->time_base);
        av_packet->stream_index = stream->index;

        av_interleaved_write_frame(format_ctx, av_packet);
        av_packet_unref(av_packet);
    }
}

void VideoEncoder::finalize() {
    if (finalized) return;
    finalized = true;
    flush_frame_buffer();
    flush_encoder();
    av_write_trailer(format_ctx);
}
