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

#include "video_decoder.h"

#include "configuration.h"
#include "dct_common.h"
#include "packet_format.h"

#include <cstring>
#include <span>
#include <stdexcept>

namespace {
    std::size_t packet_size_at(const std::span<const std::byte> data) {
        if (data.size() <= VERSION_OFF) {
            return HEADER_SIZE + SYMBOL_SIZE_BYTES;
        }
        const std::size_t header_size = packet_header_size(static_cast<uint8_t>(data[VERSION_OFF]));
        return (header_size > 0 ? header_size : HEADER_SIZE) + SYMBOL_SIZE_BYTES;
    }

    bool starts_with_magic(const std::span<const std::byte> data) {
        if (data.size() < MAGIC_SIZE) {
            return false;
        }
        uint32_t magic = 0;
        std::memcpy(&magic, data.data(), sizeof(magic));
        return magic == MAGIC_ID;
    }
}

VideoDecoder::VideoDecoder(const std::string &input_path) {
    try {
        init_decoder(input_path);
    } catch (...) {
        release();
        throw;
    }
}

VideoDecoder::~VideoDecoder() {
    release();
}

void VideoDecoder::release() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
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

    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
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

    codec_ctx_->thread_count = 0;
    codec_ctx_->thread_type = FF_THREAD_SLICE;

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
        if (av_frame_get_buffer(gray_frame_, 0) < 0) {
            throw std::runtime_error("Failed to allocate gray frame buffer");
        }

        sws_ctx_ = sws_getContext(
            codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
            codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_GRAY8,
            SWS_POINT, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            throw std::runtime_error("Failed to create swscale context");
        }
    }

    layout_ = compute_frame_layout(codec_ctx_->width, codec_ctx_->height);
    extract_buffer_.reserve(static_cast<std::size_t>(layout_.bytes_per_frame) * 2);
}

int64_t VideoDecoder::total_frames() const {
    const AVStream *stream = format_ctx_->streams[video_stream_index_];
    if (stream->nb_frames > 0) {
        return stream->nb_frames;
    }
    if (stream->duration > 0 && stream->time_base.den > 0) {
        const double duration_sec = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        const double fps = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
                               ? av_q2d(stream->avg_frame_rate)
                               : FRAME_FPS;
        return static_cast<int64_t>(duration_sec * fps);
    }
    return -1;
}

const AVFrame *VideoDecoder::gray_plane_frame() const {
    return is_gray8_ ? frame_ : gray_frame_;
}

void VideoDecoder::extract_data_into(std::vector<std::byte> &dest) const {
    const int blocks_per_row = layout_.blocks_per_row;
    constexpr int blocks_per_byte = 8 / BITS_PER_BLOCK;
    const int total_bytes = layout_.total_blocks / blocks_per_byte;

    const AVFrame *source = gray_plane_frame();
    const uint8_t *src_base = source->data[0];
    const int src_stride = source->linesize[0];

    const std::size_t base = dest.size();
    dest.resize(base + total_bytes);
    auto *out = reinterpret_cast<uint8_t *>(dest.data() + base);

    if constexpr (BITS_PER_BLOCK == 1) {
        static constexpr int16_t PROJ[4] = {8035, 6811, 4551, 1598};

#pragma omp parallel for schedule(static)
        for (int byte_idx = 0; byte_idx < total_bytes; ++byte_idx) {
            uint8_t current_byte = 0;
            for (int sub = 0; sub < 8; ++sub) {
                const int block_idx = byte_idx * 8 + sub;
                const int block_row = block_idx / blocks_per_row;
                const int block_col = block_idx % blocks_per_row;
                const uint8_t *block = src_base + block_row * 8 * src_stride + block_col * 8;

#if defined(DCT_USE_NEON)
                uint16x8_t acc = vaddl_u8(vld1_u8(block), vld1_u8(block + src_stride));
                for (int y = 2; y < 8; ++y) {
                    acc = vaddw_u8(acc, vld1_u8(block + y * src_stride));
                }
                const int16x4_t lo = vreinterpret_s16_u16(vget_low_u16(acc));
                const int16x4_t hi = vrev64_s16(vreinterpret_s16_u16(vget_high_u16(acc)));
                const int32x4_t prod = vmull_s16(vsub_s16(lo, hi), vld1_s16(PROJ));
                const int32_t s = vaddvq_s32(prod);
#else
                int col[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (int y = 0; y < 8; ++y) {
                    const uint8_t *row = block + y * src_stride;
                    for (int x = 0; x < 8; ++x) {
                        col[x] += row[x];
                    }
                }
                const int32_t s =
                        PROJ[0] * (col[0] - col[7]) +
                        PROJ[1] * (col[1] - col[6]) +
                        PROJ[2] * (col[2] - col[5]) +
                        PROJ[3] * (col[3] - col[4]);
#endif
                current_byte = static_cast<uint8_t>((current_byte << 1) | (s > 0 ? 1 : 0));
            }
            out[byte_idx] = current_byte;
        }
    } else {
        const auto &projections = get_decoder_projections();
        const auto &vectors = projections.vectors;

#pragma omp parallel for schedule(static)
        for (int byte_idx = 0; byte_idx < total_bytes; ++byte_idx) {
            uint8_t current_byte = 0;
            for (int sub = 0; sub < blocks_per_byte; ++sub) {
                const int block_idx = byte_idx * blocks_per_byte + sub;
                const int base_x = (block_idx % blocks_per_row) * 8;
                const int base_y = (block_idx / blocks_per_row) * 8;

                alignas(32) float block_flat[64];
                for (int y = 0; y < 8; ++y) {
                    const uint8_t *row = src_base + (base_y + y) * src_stride + base_x;
                    for (int x = 0; x < 8; ++x) {
                        block_flat[y * 8 + x] = static_cast<float>(row[x]);
                    }
                }

                for (int b = 0; b < BITS_PER_BLOCK; ++b) {
                    const float sum = dot_product_64(block_flat, vectors[b]);
                    current_byte = static_cast<uint8_t>((current_byte << 1) | (sum > 0.0f ? 1 : 0));
                }
            }
            out[byte_idx] = current_byte;
        }
    }
}

void VideoDecoder::extract_packets_from_buffer(std::vector<std::byte> &accumulated, PacketList &out_packets) {
    std::size_t offset = 0;
    while (offset + MAGIC_SIZE <= accumulated.size()) {
        const std::span<const std::byte> remaining(accumulated.data() + offset, accumulated.size() - offset);
        if (!starts_with_magic(remaining)) {
            ++offset;
            continue;
        }
        const std::size_t pkt_size = packet_size_at(remaining);
        if (pkt_size > remaining.size()) {
            break;
        }
        out_packets.emplace_back(remaining.begin(), remaining.begin() + static_cast<std::ptrdiff_t>(pkt_size));
        offset += pkt_size;
    }
    accumulated.erase(accumulated.begin(), accumulated.begin() + static_cast<std::ptrdiff_t>(offset));
}

VideoDecoder::PacketList VideoDecoder::consume_decoded_frame() {
    if (!is_gray8_) {
        sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height,
                  gray_frame_->data, gray_frame_->linesize);
    }
    ++frame_index_;

    extract_data_into(extract_buffer_);
    PacketList packets;
    packets.reserve(extract_buffer_.size() / PACKET_SIZE);
    extract_packets_from_buffer(extract_buffer_, packets);
    return packets;
}

VideoDecoder::PacketList VideoDecoder::flush_decoder_and_collect_packets() {
    avcodec_send_packet(codec_ctx_, nullptr);
    PacketList collected;
    while (true) {
        const int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving frame");
        }
        for (PacketList packets = consume_decoded_frame(); auto &packet: packets) {
            collected.push_back(std::move(packet));
        }
    }
    return collected;
}

VideoDecoder::PacketList VideoDecoder::decode_next_frame() {
    if (eof_) {
        return {};
    }

    while (true) {
        if (av_read_frame(format_ctx_, av_packet_) < 0) {
            eof_ = true;
            PacketList packets = flush_decoder_and_collect_packets();
            if (packets.empty() && !extract_buffer_.empty()) {
                extract_packets_from_buffer(extract_buffer_, packets);
            }
            return packets;
        }

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
        return consume_decoded_frame();
    }
}

VideoDecoder::PacketList VideoDecoder::decode_all_frames() {
    PacketList results;
    while (!eof_) {
        for (PacketList packets = decode_next_frame(); auto &packet: packets) {
            results.push_back(std::move(packet));
        }
    }
    return results;
}
