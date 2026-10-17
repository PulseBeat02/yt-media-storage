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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

#include "fixtures.h"
#include "media_storage.h"
#include "packet_format.h"
#include "video_decoder.h"
#include "video_encoder.h"

namespace {

constexpr const char* GOLDEN_DIRECTORY = MEDIA_STORAGE_GOLDEN_DIRECTORY;
constexpr const char* GOLDEN_INPUT = "input.bin";
constexpr std::string_view GOLDEN_PASSWORD = "golden-password";
constexpr std::string_view NO_PASSWORD;

constexpr const char* PLAIN_CRC32_VIDEO = "plain-crc32.mkv";
constexpr const char* PLAIN_XXHASH_VIDEO = "plain-xxhash.mkv";
constexpr const char* ENCRYPTED_CRC32_VIDEO = "encrypted-crc32.mkv";

constexpr const char* PLAIN_CRC32_STREAM = "plain-crc32.flv";
constexpr const char* ENCRYPTED_XXHASH_STREAM = "encrypted-xxhash.flv";

constexpr const char* BINARY_EXTENSION = ".bin";
constexpr const char* MATROSKA_EXTENSION = ".mkv";

using luma_planes = std::vector<std::vector<uint8_t>>;

using format_pointer =
    std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* context) { avformat_close_input(&context); })>;
using codec_pointer =
    std::unique_ptr<AVCodecContext, decltype([](AVCodecContext* context) { avcodec_free_context(&context); })>;
using frame_pointer = std::unique_ptr<AVFrame, decltype([](AVFrame* frame) { av_frame_free(&frame); })>;
using packet_pointer = std::unique_ptr<AVPacket, decltype([](AVPacket* packet) { av_packet_free(&packet); })>;

std::filesystem::path golden_path(const char* name) {
  return std::filesystem::path(GOLDEN_DIRECTORY) / name;
}

std::vector<std::byte> read_golden_input() {
  return fixtures::read_bytes(golden_path(GOLDEN_INPUT));
}

std::vector<std::byte> decode_golden_video(const char* name, const std::string_view password) {
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const std::string input_path = golden_path(name).string();
  const std::string output_path = output.string();
  ms_decode_options_t options{};
  options.input_path = input_path.c_str();
  options.output_path = output_path.c_str();
  options.password = password.data();
  options.password_len = password.size();

  const ms_status_t status = ms_decode(&options, nullptr);

  EXPECT_EQ(status, MS_OK) << ms_last_error();

  return fixtures::read_bytes(output.path());
}

std::vector<std::byte> decode_golden_stream(const char* name, const std::string_view password) {
  const fixtures::temporary_file output(BINARY_EXTENSION);
  const std::string stream_path = golden_path(name).string();
  const std::string output_path = output.string();
  ms_stream_decode_options_t options{};
  options.stream_url = stream_path.c_str();
  options.output_path = output_path.c_str();
  options.password = password.data();
  options.password_len = password.size();

  const ms_status_t status = ms_stream_decode(&options, nullptr);

  EXPECT_EQ(status, MS_OK) << ms_last_error();

  return fixtures::read_bytes(output.path());
}

std::vector<Packet> read_packets(const std::string& path) {
  VideoDecoder decoder(path);
  const VideoDecoder::PacketList extracted = decoder.decode_all_frames();
  std::vector<Packet> packets(extracted.size());
  for (std::size_t i = 0; i < extracted.size(); ++i) {
    const std::size_t size = std::min(extracted[i].size(), packets[i].bytes.size());
    std::copy_n(extracted[i].begin(), size, packets[i].bytes.begin());
  }
  return packets;
}

void write_video(const std::string& path, const std::vector<Packet>& packets) {
  VideoEncoder encoder(path);
  encoder.encode_packets(packets);
  encoder.finalize();
}

std::vector<uint8_t> copy_luma_plane(const AVFrame& frame) {
  std::vector<uint8_t> plane;
  plane.reserve(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
  for (int row = 0; row < frame.height; ++row) {
    const uint8_t* const start = frame.data[0] + static_cast<std::ptrdiff_t>(row) * frame.linesize[0];
    plane.insert(plane.end(), start, start + frame.width);
  }
  return plane;
}

void receive_luma_planes(AVCodecContext* context, AVFrame* frame, luma_planes& planes) {
  while (avcodec_receive_frame(context, frame) >= 0) {
    planes.push_back(copy_luma_plane(*frame));
    av_frame_unref(frame);
  }
}

format_pointer open_video(const std::string& path) {
  AVFormatContext* context = nullptr;
  if (avformat_open_input(&context, path.c_str(), nullptr, nullptr) < 0) {
    const std::string message = "Video could not be opened!";
    throw std::runtime_error(message);
  }

  format_pointer format(context);
  if (avformat_find_stream_info(format.get(), nullptr) < 0) {
    const std::string message = "Video stream information could not be read!";
    throw std::runtime_error(message);
  }
  return format;
}

codec_pointer open_decoder(const AVCodecParameters& parameters) {
  const AVCodec* const codec = avcodec_find_decoder(parameters.codec_id);
  codec_pointer context(avcodec_alloc_context3(codec));
  if (avcodec_parameters_to_context(context.get(), &parameters) < 0 ||
      avcodec_open2(context.get(), codec, nullptr) < 0) {
    const std::string message = "Video decoder could not be opened!";
    throw std::runtime_error(message);
  }
  return context;
}

luma_planes read_luma_planes(const std::string& path) {
  const format_pointer format = open_video(path);
  const int stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const codec_pointer decoder = open_decoder(*format->streams[stream_index]->codecpar);
  const packet_pointer packet(av_packet_alloc());
  const frame_pointer frame(av_frame_alloc());

  luma_planes planes;
  while (av_read_frame(format.get(), packet.get()) >= 0) {
    if (packet->stream_index == stream_index) {
      avcodec_send_packet(decoder.get(), packet.get());
      receive_luma_planes(decoder.get(), frame.get(), planes);
    }

    av_packet_unref(packet.get());
  }

  avcodec_send_packet(decoder.get(), nullptr);
  receive_luma_planes(decoder.get(), frame.get(), planes);
  return planes;
}

void expect_identical_frames_from_reference_packets(const char* name) {
  const std::string reference_path = golden_path(name).string();
  const fixtures::temporary_file rendered(MATROSKA_EXTENSION);
  const std::vector<Packet> packets = read_packets(reference_path);
  write_video(rendered.string(), packets);

  const luma_planes expected = read_luma_planes(reference_path);
  const luma_planes actual = read_luma_planes(rendered.string());

  const bool expected_empty = expected.empty();
  const std::size_t actual_plane_count = actual.size();
  const std::size_t expected_plane_count = expected.size();
  ASSERT_FALSE(expected_empty);
  EXPECT_EQ(actual_plane_count, expected_plane_count);
  EXPECT_TRUE(actual == expected);
}

}

TEST(golden_test, should_decode_the_reference_video_with_crc32_packets) {
  const std::vector<std::byte> decoded = decode_golden_video(PLAIN_CRC32_VIDEO, NO_PASSWORD);

  const std::vector<std::byte> golden_input = read_golden_input();
  EXPECT_EQ(decoded, golden_input);
}

TEST(golden_test, should_decode_the_reference_video_with_xxhash_packets) {
  const std::vector<std::byte> decoded = decode_golden_video(PLAIN_XXHASH_VIDEO, NO_PASSWORD);

  const std::vector<std::byte> golden_input = read_golden_input();
  EXPECT_EQ(decoded, golden_input);
}

TEST(golden_test, should_decode_the_encrypted_reference_video) {
  const std::vector<std::byte> decoded = decode_golden_video(ENCRYPTED_CRC32_VIDEO, GOLDEN_PASSWORD);

  const std::vector<std::byte> golden_input = read_golden_input();
  EXPECT_EQ(decoded, golden_input);
}

TEST(golden_test, should_decode_the_reference_stream) {
  const std::vector<std::byte> decoded = decode_golden_stream(PLAIN_CRC32_STREAM, NO_PASSWORD);

  const std::vector<std::byte> golden_input = read_golden_input();
  EXPECT_EQ(decoded, golden_input);
}

TEST(golden_test, should_decode_the_encrypted_reference_stream_with_xxhash_packets) {
  const std::vector<std::byte> decoded = decode_golden_stream(ENCRYPTED_XXHASH_STREAM, GOLDEN_PASSWORD);

  const std::vector<std::byte> golden_input = read_golden_input();
  EXPECT_EQ(decoded, golden_input);
}

TEST(golden_test, should_render_the_frames_of_the_reference_video_with_crc32_packets) {
  expect_identical_frames_from_reference_packets(PLAIN_CRC32_VIDEO);
}

TEST(golden_test, should_render_the_frames_of_the_reference_video_with_xxhash_packets) {
  expect_identical_frames_from_reference_packets(PLAIN_XXHASH_VIDEO);
}

TEST(golden_test, should_render_the_frames_of_the_encrypted_reference_video) {
  expect_identical_frames_from_reference_packets(ENCRYPTED_CRC32_VIDEO);
}
