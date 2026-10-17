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

#include "av_utils.h"
#include "configuration.h"
#include "dct_common.h"
#include "packet_format.h"

#include <climits>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>

namespace {
constexpr int ANY_STREAM = -1;
constexpr int NO_RELATED_STREAM = -1;

constexpr std::size_t BUFFERED_FRAMES = 2;

constexpr int64_t UNKNOWN_FRAME_COUNT = -1;

constexpr int16_t PROJECTION_WEIGHTS[] = {8035, 6811, 4551, 1598};

#if defined(DCT_USE_NEON)
constexpr int SEEDED_ROWS = 2;
#endif

std::size_t packet_size_at(const std::span<const std::byte> data) {
  if (data.size() <= VERSION_OFFSET) {
    return HEADER_SIZE + SYMBOL_SIZE_BYTES;
  }
  const std::size_t header_size = packet_header_size(static_cast<uint8_t>(data[VERSION_OFFSET]));
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

int32_t project_columns(const int (&columns)[BLOCK_SIZE]) {
  int32_t projection = 0;
  for (std::size_t i = 0; i < std::size(PROJECTION_WEIGHTS); ++i) {
    projection += PROJECTION_WEIGHTS[i] * (columns[i] - columns[BLOCK_SIZE - 1 - i]);
  }
  return projection;
}

template <typename GrayOf> int32_t project_block(const uint8_t* block, const int stride, const GrayOf& gray_of) {
  int columns[BLOCK_SIZE] = {};
  for (int y = 0; y < BLOCK_SIZE; ++y) {
    const uint8_t* const row = block + y * stride;
    for (int x = 0; x < BLOCK_SIZE; ++x) {
      columns[x] += gray_of(row[x]);
    }
  }
  return project_columns(columns);
}

#if defined(DCT_USE_NEON)
int32_t project_block(const uint8_t* block, const int stride, const std::identity&) {
  uint16x8_t accumulator = vaddl_u8(vld1_u8(block), vld1_u8(block + stride));
  for (int y = SEEDED_ROWS; y < BLOCK_SIZE; ++y) {
    accumulator = vaddw_u8(accumulator, vld1_u8(block + y * stride));
  }
  const int16x4_t low = vreinterpret_s16_u16(vget_low_u16(accumulator));
  const int16x4_t high = vrev64_s16(vreinterpret_s16_u16(vget_high_u16(accumulator)));
  const int32x4_t products = vmull_s16(vsub_s16(low, high), vld1_s16(PROJECTION_WEIGHTS));
  return vaddvq_s32(products);
}
#endif

template <typename GrayOf>
void extract_data_into(std::vector<std::byte>& destination, const AVFrame& source, const frame_layout& layout,
                       const GrayOf& gray_of) {
  const int blocks_per_row = layout.blocks_per_row;
  constexpr int blocks_per_byte = CHAR_BIT / BITS_PER_BLOCK;
  const int total_bytes = layout.total_blocks / blocks_per_byte;

  const uint8_t* const source_base = source.data[0];
  const int source_stride = source.linesize[0];

  const std::size_t base = destination.size();
  destination.resize(base + total_bytes);
  auto* out = reinterpret_cast<uint8_t*>(destination.data() + base);

  if constexpr (BITS_PER_BLOCK == 1) {
    for (int byte_index = 0; byte_index < total_bytes; ++byte_index) {
      uint8_t current_byte = 0;
      for (int block_in_byte = 0; block_in_byte < blocks_per_byte; ++block_in_byte) {
        const int block_index = byte_index * blocks_per_byte + block_in_byte;
        const int block_row = block_index / blocks_per_row;
        const int block_column = block_index % blocks_per_row;
        const uint8_t* const block = source_base + block_row * BLOCK_SIZE * source_stride + block_column * BLOCK_SIZE;
        const int32_t projection = project_block(block, source_stride, gray_of);
        current_byte = static_cast<uint8_t>((current_byte << 1) | (projection > 0 ? 1 : 0));
      }
      out[byte_index] = current_byte;
    }
  } else {
    const auto& projections = get_decoder_projections();
    const auto& vectors = projections.vectors;

    for (int byte_index = 0; byte_index < total_bytes; ++byte_index) {
      uint8_t current_byte = 0;
      for (int block_in_byte = 0; block_in_byte < blocks_per_byte; ++block_in_byte) {
        const int block_index = byte_index * blocks_per_byte + block_in_byte;
        const int base_x = (block_index % blocks_per_row) * BLOCK_SIZE;
        const int base_y = (block_index / blocks_per_row) * BLOCK_SIZE;

        alignas(VECTOR_ALIGNMENT) float block_flat[BLOCK_AREA];
        for (int y = 0; y < BLOCK_SIZE; ++y) {
          const uint8_t* const row = source_base + (base_y + y) * source_stride + base_x;
          for (int x = 0; x < BLOCK_SIZE; ++x) {
            block_flat[y * BLOCK_SIZE + x] = static_cast<float>(gray_of(row[x]));
          }
        }

        for (int bit_index = 0; bit_index < BITS_PER_BLOCK; ++bit_index) {
          const float sum = dot_product_64(block_flat, vectors[bit_index]);
          current_byte = static_cast<uint8_t>((current_byte << 1) | (sum > 0.0f ? 1 : 0));
        }
      }
      out[byte_index] = current_byte;
    }
  }
}
}

video_decoder::video_decoder(const std::string& input_path) {
  try {
    initialize_decoder(input_path);
  } catch (...) {
    release();
    throw;
  }
}

video_decoder::~video_decoder() {
  release();
}

void video_decoder::release() {
  if (sws_context_) {
    sws_freeContext(sws_context_);
    sws_context_ = nullptr;
  }
  if (av_packet_)
    av_packet_free(&av_packet_);
  if (gray_frame_)
    av_frame_free(&gray_frame_);
  if (frame_)
    av_frame_free(&frame_);
  if (codec_context_)
    avcodec_free_context(&codec_context_);
  if (format_context_)
    avformat_close_input(&format_context_);
}

void video_decoder::initialize_decoder(const std::string& input_path) {
  int status = avformat_open_input(&format_context_, input_path.c_str(), nullptr, nullptr);
  if (status < 0) {
    const std::string message = "Input file could not be opened!";
    throw std::runtime_error(message);
  }

  status = avformat_find_stream_info(format_context_, nullptr);
  if (status < 0) {
    const std::string message = "Stream information could not be found!";
    throw std::runtime_error(message);
  }

  video_stream_index_ =
      av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, ANY_STREAM, NO_RELATED_STREAM, nullptr, NO_FLAGS);
  if (video_stream_index_ < 0) {
    const std::string message = "Input has no video stream!";
    throw std::runtime_error(message);
  }

  const AVStream* const stream = format_context_->streams[video_stream_index_];
  const AVCodec* const codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    const std::string message = "Decoder could not be found!";
    throw std::runtime_error(message);
  }

  codec_context_ = avcodec_alloc_context3(codec);
  if (!codec_context_) {
    throw std::runtime_error(CODEC_CONTEXT_ALLOCATION_MESSAGE);
  }

  status = avcodec_parameters_to_context(codec_context_, stream->codecpar);
  if (status < 0) {
    throw std::runtime_error(CODEC_PARAMETERS_COPY_MESSAGE);
  }

  codec_context_->thread_count = AUTOMATIC_THREAD_COUNT;
  codec_context_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

  status = avcodec_open2(codec_context_, codec, nullptr);
  if (status < 0) {
    const std::string message = "Codec could not be opened!";
    throw std::runtime_error(message);
  }

  frame_ = av_frame_alloc();
  av_packet_ = av_packet_alloc();
  if (!frame_ || !av_packet_) {
    const std::string message = "Frame or packet could not be allocated!";
    throw std::runtime_error(message);
  }

  is_gray8_ = (codec_context_->pix_fmt == AV_PIX_FMT_GRAY8);
  if (!is_gray8_) {
    gray_frame_ = av_frame_alloc();
    if (!gray_frame_) {
      const std::string message = "Gray frame could not be allocated!";
      throw std::runtime_error(message);
    }
    gray_frame_->format = AV_PIX_FMT_GRAY8;
    gray_frame_->width = codec_context_->width;
    gray_frame_->height = codec_context_->height;
    if (av_frame_get_buffer(gray_frame_, DEFAULT_ALIGNMENT) < 0) {
      const std::string message = "Gray frame buffer could not be allocated!";
      throw std::runtime_error(message);
    }

    sws_context_ =
        sws_getContext(codec_context_->width, codec_context_->height, codec_context_->pix_fmt, codec_context_->width,
                       codec_context_->height, AV_PIX_FMT_GRAY8, SWS_POINT, nullptr, nullptr, nullptr);
    if (!sws_context_) {
      const std::string message = "Swscale context could not be created!";
      throw std::runtime_error(message);
    }
#if !defined(DCT_USE_NEON)
    // arm keeps sws_scale and the neon projection: the lookup is only measured on x86
    gray_lookup_ = calibrate_gray_lookup_table(sws_context_, codec_context_->pix_fmt, codec_context_->width,
                                               codec_context_->height);
#endif
  }

  layout_ = compute_frame_layout(codec_context_->width, codec_context_->height);
  extract_buffer_.reserve(static_cast<std::size_t>(layout_.bytes_per_frame) * BUFFERED_FRAMES);
}

int64_t video_decoder::total_frames() const {
  const AVStream* const stream = format_context_->streams[video_stream_index_];
  if (stream->nb_frames > 0) {
    return stream->nb_frames;
  }
  if (stream->duration > 0 && stream->time_base.den > 0) {
    const double duration_seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    const double frames_per_second = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
                                         ? av_q2d(stream->avg_frame_rate)
                                         : FRAMES_PER_SECOND;
    return static_cast<int64_t>(duration_seconds * frames_per_second);
  }
  return UNKNOWN_FRAME_COUNT;
}

const AVFrame* video_decoder::gray_plane_frame() const {
  return is_gray8_ ? frame_ : gray_frame_;
}

void video_decoder::extract_packets_from_buffer(std::vector<std::byte>& accumulated, packet_list& out_packets) {
  std::size_t offset = 0;
  while (offset + MAGIC_SIZE <= accumulated.size()) {
    const std::span<const std::byte> remaining(accumulated.data() + offset, accumulated.size() - offset);
    if (!starts_with_magic(remaining)) {
      ++offset;
      continue;
    }
    const std::size_t packet_size = packet_size_at(remaining);
    if (packet_size > remaining.size()) {
      break;
    }
    out_packets.emplace_back(remaining.begin(), remaining.begin() + static_cast<std::ptrdiff_t>(packet_size));
    offset += packet_size;
  }
  accumulated.erase(accumulated.begin(), accumulated.begin() + static_cast<std::ptrdiff_t>(offset));
}

video_decoder::packet_list video_decoder::consume_decoded_frame() {
  const bool fits_gray_lookup =
      gray_lookup_ && frame_->width == layout_.frame_width && frame_->height == layout_.frame_height;
  if (fits_gray_lookup) {
    const gray_lookup_table& table = *gray_lookup_;
    extract_data_into(extract_buffer_, *frame_, layout_, [&table](const uint8_t value) { return table[value]; });
  } else {
    if (!is_gray8_) {
      sws_scale(sws_context_, frame_->data, frame_->linesize, FIRST_ROW, frame_->height, gray_frame_->data,
                gray_frame_->linesize);
    }
    extract_data_into(extract_buffer_, *gray_plane_frame(), layout_, std::identity{});
  }
  ++frame_index_;

  packet_list packets;
  packets.reserve(extract_buffer_.size() / PACKET_SIZE);
  extract_packets_from_buffer(extract_buffer_, packets);
  return packets;
}

bool video_decoder::feed_next_packet() {
  while (true) {
    if (!pending_packet_) {
      if (av_read_frame(format_context_, av_packet_) < 0) {
        avcodec_send_packet(codec_context_, nullptr);
        draining_ = true;
        return false;
      }
      if (av_packet_->stream_index != video_stream_index_) {
        av_packet_unref(av_packet_);
        continue;
      }
    }
    const int send_status = avcodec_send_packet(codec_context_, av_packet_);
    pending_packet_ = send_status == AVERROR(EAGAIN);
    if (pending_packet_) {
      return true;
    }
    av_packet_unref(av_packet_);
    if (send_status == 0) {
      return true;
    }
  }
}

video_decoder::packet_list video_decoder::decode_next_frame() {
  while (!eof_) {
    const int receive_status = avcodec_receive_frame(codec_context_, frame_);
    if (receive_status == 0) {
      return consume_decoded_frame();
    }
    if (receive_status == AVERROR_EOF) {
      eof_ = true;
      break;
    }
    if (receive_status != AVERROR(EAGAIN)) {
      continue; // a damaged frame is skipped; frame threads report it here instead of on send
    }
    if (draining_) {
      eof_ = true;
      break;
    }
    feed_next_packet();
  }

  packet_list packets;
  extract_packets_from_buffer(extract_buffer_, packets);
  return packets;
}

video_decoder::packet_list video_decoder::decode_all_frames() {
  packet_list results;
  while (!eof_) {
    for (packet_list packets = decode_next_frame(); auto& packet : packets) {
      results.push_back(std::move(packet));
    }
  }
  return results;
}
