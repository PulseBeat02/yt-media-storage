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

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/macros.h>
#include <libavutil/pixfmt.h>
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "av_utils.h"
#include "configuration.h"
#include "fixtures.h"
#include "frame_layout.h"
#include "packet_format.h"
#include "stream.h"
#include "video_decoder.h"
#include "video_encoder.h"

namespace {

constexpr const char* OUTPUT_NAME = "output.mkv";
constexpr const char* MATROSKA_EXTENSION = ".mkv";
constexpr const char* AVI_EXTENSION = ".avi";
constexpr const char* MXF_EXTENSION = ".mxf";
constexpr const char* FLV_EXTENSION = ".flv";
constexpr const char* SUBTITLE_EXTENSION = ".srt";
constexpr const char* UNKNOWN_EXTENSION = ".unknown";

constexpr const char* SUBTITLE_CONTENTS = "1\n00:00:00,000 --> 00:00:01,000\ncaption\n";

constexpr int SMALL_FRAME_WIDTH = 64;
constexpr int SMALL_FRAME_HEIGHT = 32;

constexpr int ONE_PACKET_FRAME_WIDTH = 384;
constexpr int ONE_PACKET_FRAME_HEIGHT = 408;
constexpr int ODD_FRAME_WIDTH = ONE_PACKET_FRAME_WIDTH + 1;

constexpr int STREAM_BITRATE_KBPS = 4000;
constexpr int HIGH_STREAM_BITRATE_KBPS = 50000;

constexpr int FRAMES_TO_WRITE = 2;
constexpr int FRAMES_TO_OVERFLOW_THE_OUTPUT_BUFFER = 30;

constexpr uint8_t UNKNOWN_VERSION = 0xEE;

constexpr uint32_t UNKNOWN_CODEC_TAG = MKTAG('Z', 'Z', 'Z', 'Z');

class owned_frame {
public:
  owned_frame() : frame_(av_frame_alloc()) {
  }

  owned_frame(const owned_frame&) = delete;

  owned_frame& operator=(const owned_frame&) = delete;

  ~owned_frame() {
    av_frame_free(&frame_);
  }

  [[nodiscard]] AVFrame* get() const {
    return frame_;
  }

private:
  AVFrame* frame_;
};

Packet make_framed_packet(const std::size_t seed) {
  Packet packet{};
  for (std::size_t i = 0; i < packet.bytes.size(); ++i) {
    packet.bytes[i] = static_cast<std::byte>((i * fixtures::PATTERN_MULTIPLIER + seed) & fixtures::BYTE_MASK);
  }
  std::memcpy(packet.bytes.data(), &MAGIC_ID, MAGIC_SIZE);
  packet.bytes[VERSION_OFF] = std::byte{VERSION_ID_V2};
  return packet;
}

template <typename EncoderType> void add_framed_packets(EncoderType& encoder, const int count) {
  for (int i = 0; i < count; ++i) {
    encoder.add_packet(make_framed_packet(static_cast<std::size_t>(i)));
  }
}

int packets_spanning_frames(const int frame_count) {
  return VideoEncoder::packets_per_frame() * (frame_count - 1) + 1;
}

void write_video(const std::string& path, const int frame_count) {
  VideoEncoder encoder(path);
  add_framed_packets(encoder, packets_spanning_frames(frame_count));
  encoder.finalize();
}

void write_one_packet_stream(const std::string& path, const Packet& packet) {
  StreamEncoder encoder(path, STREAM_BITRATE_KBPS, ONE_PACKET_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT, FRAME_FPS);
  encoder.add_packet(packet);
  encoder.finalize();
}

void write_undecodable_video(const std::string& path) {
  AVFormatContext* context = nullptr;
  ASSERT_GE(avformat_alloc_output_context2(&context, nullptr, nullptr, path.c_str()), 0);
  AVStream* const stream = avformat_new_stream(context, nullptr);
  stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  stream->codecpar->codec_tag = UNKNOWN_CODEC_TAG;
  stream->codecpar->width = SMALL_FRAME_WIDTH;
  stream->codecpar->height = SMALL_FRAME_HEIGHT;
  stream->time_base = AVRational{1, FRAME_FPS};

  const bool written = avio_open(&context->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0 &&
                       avformat_write_header(context, nullptr) >= 0 && av_write_trailer(context) >= 0;
  avio_closep(&context->pb);
  avformat_free_context(context);

  ASSERT_TRUE(written);
}

int stream_packets_per_frame() {
  const FrameLayout layout = compute_frame_layout(FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM);
  return packets_per_frame(layout);
}

}

TEST(av_utils_test, should_give_a_shared_frame_its_own_buffer_of_the_same_shape) {
  const owned_frame frame;
  const owned_frame reference;
  frame.get()->format = AV_PIX_FMT_GRAY8;
  frame.get()->width = SMALL_FRAME_WIDTH;
  frame.get()->height = SMALL_FRAME_HEIGHT;
  ASSERT_EQ(av_frame_get_buffer(frame.get(), 0), 0);
  ASSERT_EQ(av_frame_ref(reference.get(), frame.get()), 0);
  const uint8_t* const shared_plane = frame.get()->data[0];

  ensure_fresh_writable_frame(frame.get());

  EXPECT_TRUE(av_frame_is_writable(frame.get()));
  EXPECT_NE(frame.get()->data[0], shared_plane);
  EXPECT_EQ(reference.get()->data[0], shared_plane);
  EXPECT_EQ(frame.get()->format, AV_PIX_FMT_GRAY8);
  EXPECT_EQ(frame.get()->width, SMALL_FRAME_WIDTH);
  EXPECT_EQ(frame.get()->height, SMALL_FRAME_HEIGHT);
}

TEST(av_utils_test, should_keep_the_buffer_of_a_frame_that_is_already_writable) {
  const owned_frame frame;
  frame.get()->format = AV_PIX_FMT_GRAY8;
  frame.get()->width = SMALL_FRAME_WIDTH;
  frame.get()->height = SMALL_FRAME_HEIGHT;
  ASSERT_EQ(av_frame_get_buffer(frame.get(), 0), 0);
  const uint8_t* const plane = frame.get()->data[0];

  ensure_fresh_writable_frame(frame.get());

  EXPECT_EQ(frame.get()->data[0], plane);
}

TEST(video_encoder_test, should_reject_an_output_path_without_a_known_container) {
  const fixtures::temporary_file output(UNKNOWN_EXTENSION);

  const std::string message = fixtures::find_error_message([&output] { VideoEncoder encoder(output.string()); });

  EXPECT_EQ(message, "Failed to create output context");
  EXPECT_FALSE(std::filesystem::exists(output.path()));
}

TEST(video_encoder_test, should_fail_to_open_an_output_file_in_a_missing_directory) {
  const fixtures::temporary_file missing_directory("");
  const std::filesystem::path output = missing_directory.path() / OUTPUT_NAME;

  const std::string message = fixtures::find_error_message([&output] { VideoEncoder encoder(output.string()); });

  EXPECT_TRUE(message.starts_with("Failed to open output file: ")) << message;
}

TEST(video_encoder_test, should_fail_to_write_the_header_of_a_container_that_cannot_hold_ffv1) {
  const fixtures::temporary_file output(FLV_EXTENSION);

  const std::string message = fixtures::find_error_message([&output] { VideoEncoder encoder(output.string()); });

  EXPECT_TRUE(message.starts_with("Failed to write header: ")) << message;
}

TEST(video_encoder_test, should_reject_a_packet_after_finalizing) {
  const fixtures::temporary_file output(MATROSKA_EXTENSION);
  VideoEncoder encoder(output.string());
  encoder.finalize();

  const std::string message = fixtures::find_error_message([&encoder] { encoder.add_packet(make_framed_packet(0)); });

  EXPECT_EQ(message, "Encoder already finalized");
  EXPECT_EQ(encoder.frames_written(), 0);
}

TEST(video_encoder_test, should_report_a_failed_frame_write_when_the_disk_is_full) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  const fixtures::temporary_file output(AVI_EXTENSION);
  std::filesystem::create_symlink(fixtures::FULL_DEVICE, output.path());

  const std::string message = fixtures::find_error_message([&output] {
    VideoEncoder encoder(output.string());
    add_framed_packets(encoder, VideoEncoder::packets_per_frame() * FRAMES_TO_OVERFLOW_THE_OUTPUT_BUFFER);
    encoder.finalize();
  });

  EXPECT_TRUE(message.starts_with("Error writing frame: ")) << message;
}

TEST(video_encoder_test, should_report_a_failed_trailer_when_the_disk_is_full) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  const fixtures::temporary_file output(MATROSKA_EXTENSION);
  std::filesystem::create_symlink(fixtures::FULL_DEVICE, output.path());
  VideoEncoder encoder(output.string());
  encoder.add_packet(make_framed_packet(0));

  const std::string message = fixtures::find_error_message([&encoder] { encoder.finalize(); });

  EXPECT_TRUE(message.starts_with("Error writing trailer: ")) << message;
}

TEST(video_encoder_test, should_not_throw_from_its_destructor_when_finalizing_fails) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  const fixtures::temporary_file output(MATROSKA_EXTENSION);
  std::filesystem::create_symlink(fixtures::FULL_DEVICE, output.path());

  EXPECT_NO_THROW({
    VideoEncoder encoder(output.string());
    encoder.add_packet(make_framed_packet(0));
  });
}

TEST(video_decoder_test, should_fail_to_open_a_missing_input) {
  const fixtures::temporary_file input(MATROSKA_EXTENSION);

  const std::string message = fixtures::find_error_message([&input] { VideoDecoder decoder(input.string()); });

  EXPECT_EQ(message, "Failed to open input file");
}

TEST(video_decoder_test, should_reject_an_input_without_a_video_stream) {
  const fixtures::temporary_file input(SUBTITLE_EXTENSION);
  std::ofstream(input.path()) << SUBTITLE_CONTENTS;

  const std::string message = fixtures::find_error_message([&input] { VideoDecoder decoder(input.string()); });

  EXPECT_EQ(message, "No video stream found");
}

TEST(video_decoder_test, should_reject_a_video_whose_codec_has_no_decoder) {
  const fixtures::temporary_file input(AVI_EXTENSION);
  write_undecodable_video(input.string());

  const std::string message = fixtures::find_error_message([&input] { VideoDecoder decoder(input.string()); });

  EXPECT_EQ(message, "Failed to find decoder");
}

TEST(video_decoder_test, should_count_frames_from_the_frame_count_of_the_container) {
  const fixtures::temporary_file video(AVI_EXTENSION);
  write_video(video.string(), FRAMES_TO_WRITE);
  VideoDecoder decoder(video.string());

  const int64_t total_frames = decoder.total_frames();
  const VideoDecoder::PacketList packets = decoder.decode_all_frames();

  const std::size_t expected_packets = static_cast<std::size_t>(packets_spanning_frames(FRAMES_TO_WRITE));
  EXPECT_EQ(total_frames, FRAMES_TO_WRITE);
  EXPECT_EQ(packets.size(), expected_packets);
  EXPECT_EQ(decoder.frames_read(), FRAMES_TO_WRITE);
}

TEST(video_decoder_test, should_estimate_frames_from_the_duration_of_the_stream) {
  const fixtures::temporary_file video(MXF_EXTENSION);
  write_video(video.string(), FRAMES_TO_WRITE);
  const VideoDecoder decoder(video.string());

  const int64_t total_frames = decoder.total_frames();

  EXPECT_EQ(total_frames, FRAMES_TO_WRITE);
}

TEST(video_decoder_test, should_hold_back_a_magic_number_that_ends_the_last_frame) {
  const fixtures::temporary_file video(FLV_EXTENSION);
  Packet packet{};
  std::memcpy(packet.bytes.data() + packet.bytes.size() - MAGIC_SIZE, &MAGIC_ID, MAGIC_SIZE);
  write_one_packet_stream(video.string(), packet);
  VideoDecoder decoder(video.string());

  const VideoDecoder::PacketList packets = decoder.decode_all_frames();

  EXPECT_TRUE(packets.empty());
  EXPECT_EQ(decoder.frames_read(), 1);
}

TEST(video_decoder_test, should_size_a_packet_of_an_unknown_version_like_version_one) {
  const fixtures::temporary_file video(FLV_EXTENSION);
  Packet packet{};
  std::memcpy(packet.bytes.data(), &MAGIC_ID, MAGIC_SIZE);
  packet.bytes[VERSION_OFF] = std::byte{UNKNOWN_VERSION};
  write_one_packet_stream(video.string(), packet);
  VideoDecoder decoder(video.string());

  const VideoDecoder::PacketList packets = decoder.decode_all_frames();

  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets[0].size(), HEADER_SIZE + SYMBOL_SIZE_BYTES);
  EXPECT_EQ(packets[0][VERSION_OFF], std::byte{UNKNOWN_VERSION});
}

TEST(stream_encoder_test, should_reject_a_packet_after_finalizing) {
  const fixtures::temporary_file output(FLV_EXTENSION);
  StreamEncoder encoder(output.string(), STREAM_BITRATE_KBPS, ONE_PACKET_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT,
                        FRAME_FPS);
  encoder.finalize();

  const std::string message = fixtures::find_error_message([&encoder] { encoder.add_packet(make_framed_packet(0)); });

  EXPECT_EQ(message, "Stream encoder already finalized");
}

TEST(stream_encoder_test, should_report_a_frame_width_that_libx264_cannot_encode) {
  const fixtures::temporary_file output(FLV_EXTENSION);

  const std::string message = fixtures::find_error_message([&output] {
    StreamEncoder encoder(output.string(), STREAM_BITRATE_KBPS, ODD_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT, FRAME_FPS);
  });

  EXPECT_TRUE(message.starts_with("Failed to open libx264 codec: ")) << message;
}

TEST(stream_encoder_test, should_report_a_failed_frame_write_when_the_disk_is_full) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  const std::string message = fixtures::find_error_message([] {
    StreamEncoder encoder(fixtures::FULL_DEVICE, HIGH_STREAM_BITRATE_KBPS, FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM,
                          FRAME_FPS);
    add_framed_packets(encoder, stream_packets_per_frame() * FRAMES_TO_OVERFLOW_THE_OUTPUT_BUFFER);
    encoder.finalize();
  });

  EXPECT_TRUE(message.starts_with("Error writing frame to stream: ")) << message;
}

TEST(stream_encoder_test, should_report_a_failed_trailer_when_the_disk_is_full) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  StreamEncoder encoder(fixtures::FULL_DEVICE, STREAM_BITRATE_KBPS, ONE_PACKET_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT,
                        FRAME_FPS);
  encoder.add_packet(make_framed_packet(0));

  const std::string message = fixtures::find_error_message([&encoder] { encoder.finalize(); });

  EXPECT_TRUE(message.starts_with("Error writing stream trailer: ")) << message;
}

TEST(stream_encoder_test, should_not_throw_from_its_destructor_when_finalizing_fails) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  EXPECT_NO_THROW({
    StreamEncoder encoder(fixtures::FULL_DEVICE, STREAM_BITRATE_KBPS, ONE_PACKET_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT,
                          FRAME_FPS);
    encoder.add_packet(make_framed_packet(0));
  });
}
