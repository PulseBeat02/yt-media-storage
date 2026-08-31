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
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "configuration.h"
#include "encoder.h"

inline void ensure_fresh_writable_frame(AVFrame *frame) {
    if (av_frame_is_writable(frame)) {
        return;
    }
    const int format = frame->format;
    const int width = frame->width;
    const int height = frame->height;
    av_frame_unref(frame);
    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        throw std::runtime_error("Failed to allocate frame buffer");
    }
}

FrameLayout compute_frame_layout();

FrameLayout compute_frame_layout(int width, int height);

std::size_t max_packet_bytes_per_frame();

class VideoEncoder {
public:
    explicit VideoEncoder(const std::string &output_path);

    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;

    VideoEncoder &operator=(const VideoEncoder &) = delete;

    VideoEncoder(VideoEncoder &&) = delete;

    VideoEncoder &operator=(VideoEncoder &&) = delete;

    void add_packet(const Packet &packet);

    void encode_packets(const std::vector<Packet> &packets);

    void finalize();

    [[nodiscard]] int64_t frames_written() const { return frame_index; }

    [[nodiscard]] static int packets_per_frame();

private:
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    AVStream *stream = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *av_packet = nullptr;

    std::vector<std::byte> frame_data_buffer;
    FrameLayout layout_{};
    int64_t frame_index = 0;
    bool finalized = false;

    void init_encoder(const std::string &output_path);

    void embed_data_in_frame(const std::vector<std::byte> &data) const;

    void encode_frame();

    void flush_encoder() const;

    void flush_frame_buffer();
};
