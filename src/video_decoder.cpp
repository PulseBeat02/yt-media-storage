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

#include "video_decoder.h"
#include "video_encoder.h"
#include "configuration.h"
#include "dct_common.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>

VideoDecoder::VideoDecoder(const std::string &input_path) {
    init_decoder(input_path);
}

VideoDecoder::~VideoDecoder() {
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (av_packet_) av_packet_free(&av_packet_);
    if (gray_frame_) av_frame_free(&gray_frame_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (format_ctx_) avformat_close_input(&format_ctx_);
}

void VideoDecoder::init_decoder(const std::string &input_path) {
    int ret = avformat_open_input(&format_ctx_, input_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open input file");
    }

    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to find stream info");
    }

    for (unsigned i = 0; i < format_ctx_->nb_streams; ++i) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_index_ < 0) {
        throw std::runtime_error("No video stream found");
    }

    const AVStream *stream = format_ctx_->streams[video_stream_index_];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        throw std::runtime_error("Failed to find decoder");
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        throw std::runtime_error("Failed to allocate codec context");
    }

    ret = avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open codec");
    }

    frame_ = av_frame_alloc();
    av_packet_ = av_packet_alloc();

    if (!frame_ || !av_packet_) {
        throw std::runtime_error("Failed to allocate frame/packet");
    }
    is_gray8_ = (codec_ctx_->pix_fmt == AV_PIX_FMT_GRAY8);

    if (!is_gray8_) {
        gray_frame_ = av_frame_alloc();
        if (!gray_frame_) {
            throw std::runtime_error("Failed to allocate gray frame");
        }

        gray_frame_->format = AV_PIX_FMT_GRAY8;
        gray_frame_->width = codec_ctx_->width;
        gray_frame_->height = codec_ctx_->height;

        ret = av_frame_get_buffer(gray_frame_, 0);
        if (ret < 0) {
            throw std::runtime_error("Failed to allocate gray frame buffer");
        }

        sws_ctx_ = sws_getContext(
            codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
            codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_GRAY8,
            SWS_POINT, nullptr, nullptr, nullptr
        );

        if (!sws_ctx_) {
            throw std::runtime_error("Failed to create swscale context");
        }
    }

    layout_ = compute_frame_layout();
}

int64_t VideoDecoder::total_frames() const {
    if (video_stream_index_ >= 0) {
        const AVStream *stream = format_ctx_->streams[video_stream_index_];
        if (stream->nb_frames > 0) {
            return stream->nb_frames;
        }
        if (stream->duration > 0 && stream->time_base.den > 0) {
            const double duration_sec = static_cast<double>(stream->duration) *
                                        av_q2d(stream->time_base);
            return static_cast<int64_t>(duration_sec * FRAME_FPS);
        }
    }
    return -1;
}

std::vector<std::byte> VideoDecoder::extract_data_from_frame() const {
    const auto &[vectors] = get_decoder_projections();

    const int blocks_per_row = layout_.blocks_per_row;
    const int total_blocks = layout_.total_blocks;
    constexpr int blocks_per_byte = 8 / BITS_PER_BLOCK;

    const int total_bytes = total_blocks / blocks_per_byte;
    const uint8_t *src_base;
    int src_stride;
    if (is_gray8_) {
        src_base = frame_->data[0];
        src_stride = frame_->linesize[0];
    } else {
        src_base = gray_frame_->data[0];
        src_stride = gray_frame_->linesize[0];
    }

    std::vector data(total_bytes, std::byte{0});
    auto *out = reinterpret_cast<uint8_t *>(data.data());

#pragma omp parallel for schedule(static)
    for (int byte_idx = 0; byte_idx < total_bytes; ++byte_idx) {
        uint8_t current_byte = 0;

        for (int sub = 0; sub < blocks_per_byte; ++sub) {
            const int block_idx = byte_idx * blocks_per_byte + sub;
            const int block_row = block_idx / blocks_per_row;
            const int block_col = block_idx % blocks_per_row;
            const int base_x = block_col * 8;
            const int base_y = block_row * 8;

            alignas(32) float block_flat[64];
            for (int y = 0; y < 8; ++y) {
                const uint8_t *row = src_base + (base_y + y) * src_stride + base_x;
                for (int x = 0; x < 8; ++x)
                    block_flat[y * 8 + x] = static_cast<float>(row[x]);
            }

            for (int b = 0; b < BITS_PER_BLOCK; ++b) {
                const float sum = dot_product_64(block_flat, vectors[b]);
                current_byte = (current_byte << 1) | (sum > 0.0f ? 1 : 0);
            }
        }

        out[byte_idx] = current_byte;
    }

    return data;
}

std::size_t get_packet_size(const std::span<const std::byte> data) {
    if (data.size() < 5) {
        return HEADER_SIZE + SYMBOL_SIZE_BYTES;
    }
    const uint8_t version = static_cast<uint8_t>(data[4]);
    return (version == VERSION_ID_V2)
               ? (HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES)
               : (HEADER_SIZE + SYMBOL_SIZE_BYTES);
}

namespace {
    constexpr std::array<std::byte, 4> MAGIC_BYTES{
        static_cast<std::byte>(MAGIC_ID),
        static_cast<std::byte>(MAGIC_ID >> 8),
        static_cast<std::byte>(MAGIC_ID >> 16),
        static_cast<std::byte>(MAGIC_ID >> 24),
    };
}

void VideoDecoder::extract_packets_from_buffer(std::vector<std::byte> &accumulated,
                                               std::vector<std::vector<std::byte> > &out_packets) {
    std::size_t offset = 0;
    while (offset + 4 <= accumulated.size()) {
        auto it = std::search(accumulated.begin() + static_cast<std::ptrdiff_t>(offset),
                              accumulated.end(),
                              MAGIC_BYTES.begin(), MAGIC_BYTES.end());
        if (it == accumulated.end()) {
            break;
        }
        offset = static_cast<std::size_t>(std::distance(accumulated.begin(), it));
        const std::size_t packet_size = get_packet_size(
            std::span<const std::byte>(accumulated.data() + offset,
                                       accumulated.size() - offset));
        if (offset + packet_size > accumulated.size()) {
            break;
        }
        std::vector<std::byte> packet(
            accumulated.begin() + static_cast<std::ptrdiff_t>(offset),
            accumulated.begin() + static_cast<std::ptrdiff_t>(offset + packet_size));
        out_packets.push_back(std::move(packet));
        offset += packet_size;
    }
    accumulated.erase(accumulated.begin(),
                      accumulated.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::vector<std::vector<std::byte> > VideoDecoder::extract_packets_from_frame() const {
    const auto raw_data = extract_data_from_frame();
    std::vector<std::vector<std::byte> > packets;
    const std::size_t packet_size = get_packet_size(std::span(raw_data));
    packets.reserve(raw_data.size() / packet_size);
    std::size_t offset = 0;
    while (offset + packet_size <= raw_data.size()) {
        if (offset + 4 <= raw_data.size()) {
            uint32_t magic = 0;
            std::memcpy(&magic, raw_data.data() + offset, sizeof(magic));
            if (magic != MAGIC_ID) {
                break;
            }
        }
        std::vector<std::byte> packet(
            raw_data.begin() + static_cast<std::ptrdiff_t>(offset),
            raw_data.begin() + static_cast<std::ptrdiff_t>(offset + packet_size));
        packets.push_back(std::move(packet));
        offset += packet_size;
    }
    return packets;
}

void VideoDecoder::prepare_frame_for_extraction() {
    if (!is_gray8_) {
        sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height,
                  gray_frame_->data, gray_frame_->linesize);
    }
    ++frame_index_;
}

std::vector<std::vector<std::byte> > VideoDecoder::accumulate_frame_and_extract_packets() {
    const auto raw_data = extract_data_from_frame();
    extract_buffer_.insert(extract_buffer_.end(), raw_data.begin(), raw_data.end());
    std::vector<std::vector<std::byte> > packets;
    packets.reserve(extract_buffer_.size() / (HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES));
    extract_packets_from_buffer(extract_buffer_, packets);
    return packets;
}

std::vector<std::vector<std::byte> > VideoDecoder::flush_decoder_and_collect_packets() {
    avcodec_send_packet(codec_ctx_, nullptr);
    std::vector<std::vector<std::byte> > collected;
    while (true) {
        const int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving frame");
        }
        prepare_frame_for_extraction();
        for (auto packets = accumulate_frame_and_extract_packets(); auto &p: packets) {
            collected.push_back(std::move(p));
        }
    }
    return collected;
}

std::vector<std::vector<std::byte> > VideoDecoder::decode_next_frame() {
    if (eof_) {
        return {};
    }

    while (av_read_frame(format_ctx_, av_packet_) >= 0) {
        if (av_packet_->stream_index != video_stream_index_) {
            av_packet_unref(av_packet_);
            continue;
        }

        const int send_ret = avcodec_send_packet(codec_ctx_, av_packet_);
        av_packet_unref(av_packet_);
        if (send_ret < 0) {
            continue;
        }

        const int recv_ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (recv_ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (recv_ret == AVERROR_EOF) {
            eof_ = true;
            return {};
        }
        if (recv_ret < 0) {
            throw std::runtime_error("Error receiving frame");
        }

        prepare_frame_for_extraction();
        return accumulate_frame_and_extract_packets();
    }

    eof_ = true;
    if (auto flushed = flush_decoder_and_collect_packets(); !flushed.empty()) {
        return flushed;
    }
    if (!extract_buffer_.empty()) {
        std::vector<std::vector<std::byte> > packets;
        extract_packets_from_buffer(extract_buffer_, packets);
        return packets;
    }
    return {};
}

std::vector<std::vector<std::byte> > VideoDecoder::decode_all_frames() {
    std::vector<std::vector<std::byte> > results;
    while (!eof_) {
        for (auto packets = decode_next_frame(); auto &pkt: packets) {
            results.push_back(std::move(pkt));
        }
    }
    return results;
}
