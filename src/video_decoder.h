/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) Brandon Li <https://brandonli.me/>
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
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "video_encoder.h"

class VideoDecoder {
public:
    explicit VideoDecoder(const std::string &input_path);

    ~VideoDecoder();

    VideoDecoder(const VideoDecoder &) = delete;

    VideoDecoder &operator=(const VideoDecoder &) = delete;

    VideoDecoder(VideoDecoder &&) = delete;

    VideoDecoder &operator=(VideoDecoder &&) = delete;

    std::vector<std::vector<std::byte> > decode_next_frame();

    std::vector<std::vector<std::byte> > decode_all_frames();

    [[nodiscard]] int64_t frames_read() const { return frame_index_; }

    [[nodiscard]] int64_t total_frames() const;

    [[nodiscard]] bool is_eof() const { return eof_; }

private:
    AVFormatContext *format_ctx_ = nullptr;
    AVCodecContext *codec_ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVFrame *gray_frame_ = nullptr;
    AVPacket *av_packet_ = nullptr;
    SwsContext *sws_ctx_ = nullptr;

    int video_stream_index_ = -1;
    int64_t frame_index_ = 0;
    bool eof_ = false;
    bool is_gray8_ = false;
    FrameLayout layout_{};
    std::vector<std::byte> extract_buffer_{};

    void init_decoder(const std::string &input_path);

    [[nodiscard]] std::vector<std::byte> extract_data_from_frame() const;

    [[nodiscard]] std::vector<std::vector<std::byte> > extract_packets_from_frame() const;

    static void extract_packets_from_buffer(std::vector<std::byte> &accumulated,
                                            std::vector<std::vector<std::byte> > &out_packets);

    void prepare_frame_for_extraction();

    [[nodiscard]] std::vector<std::vector<std::byte> > accumulate_frame_and_extract_packets();

    [[nodiscard]] std::vector<std::vector<std::byte> > flush_decoder_and_collect_packets();
};
