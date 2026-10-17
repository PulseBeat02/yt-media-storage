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
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/macros.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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

constexpr std::size_t SLICE_SIZE_BYTES = 3;
constexpr std::size_t SLICE_STATUS_AND_CRC_BYTES = 5;
constexpr std::byte BROKEN_SLICE_SIZE{0xFF};

constexpr const char* H264_EXTENSION = ".h264";
constexpr const char* H264_ENCODER = "libx264";

constexpr int CALIBRATED_FRAME_SIZE = 128;
constexpr int RESIZED_FRAME_SIZE = 192;
constexpr int FRAMES_PER_SIZE = 3;
constexpr int FRAME_SIZES = 3;
constexpr int MID_GRAY = 128;

using codec_pointer =
    std::unique_ptr<AVCodecContext, decltype([](AVCodecContext* context) { avcodec_free_context(&context); })>;
using packet_pointer = std::unique_ptr<AVPacket, decltype([](AVPacket* packet) { av_packet_free(&packet); })>;

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
  const int context_status = avformat_alloc_output_context2(&context, nullptr, nullptr, path.c_str());
  ASSERT_GE(context_status, 0);

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

std::vector<std::byte> read_last_frame_data(const std::string& path) {
  std::vector<std::byte> last_frame;
  AVFormatContext* context = nullptr;
  if (avformat_open_input(&context, path.c_str(), nullptr, nullptr) < 0) {
    return last_frame;
  }

  AVPacket* packet = av_packet_alloc();
  while (av_read_frame(context, packet) >= 0) {
    const auto* const data = reinterpret_cast<const std::byte*>(packet->data);
    last_frame.assign(data, data + packet->size);
    av_packet_unref(packet);
  }

  av_packet_free(&packet);
  avformat_close_input(&context);
  return last_frame;
}

void break_the_slice_chain_of_the_last_frame(const std::filesystem::path& path) {
  const std::vector<std::byte> last_frame = read_last_frame_data(path.string());
  const std::size_t last_frame_size = last_frame.size();
  ASSERT_GT(last_frame_size, SLICE_SIZE_BYTES + SLICE_STATUS_AND_CRC_BYTES);

  std::vector<std::byte> video = fixtures::read_bytes(path);
  const auto found = std::search(video.begin(), video.end(), last_frame.begin(), last_frame.end());
  const auto video_end = video.end();
  ASSERT_NE(found, video_end);

  const std::size_t slice_size_offset = last_frame.size() - SLICE_STATUS_AND_CRC_BYTES - SLICE_SIZE_BYTES;
  const auto slice_size = found + static_cast<std::ptrdiff_t>(slice_size_offset);
  std::fill_n(slice_size, SLICE_SIZE_BYTES, BROKEN_SLICE_SIZE);
  fixtures::write_bytes(path, video);
}

int stream_packets_per_frame() {
  const FrameLayout layout = compute_frame_layout(FRAME_WIDTH_STREAM, FRAME_HEIGHT_STREAM);
  return packets_per_frame(layout);
}

codec_pointer open_h264_encoder(const int width, const int height) {
  const AVCodec* const codec = avcodec_find_encoder_by_name(H264_ENCODER);
  codec_pointer encoder(avcodec_alloc_context3(codec));
  encoder->width = width;
  encoder->height = height;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder->time_base = AVRational{1, FRAME_FPS};
  encoder->max_b_frames = 0;

  const int open_status = avcodec_open2(encoder.get(), codec, nullptr);
  EXPECT_EQ(open_status, 0);

  return encoder;
}

void fill_with_mid_gray(AVFrame& frame) {
  for (const AVBufferRef* const buffer : frame.buf) {
    if (buffer != nullptr) {
      std::fill_n(buffer->data, buffer->size, MID_GRAY);
    }
  }
}

void write_encoded_packets(AVCodecContext* encoder, AVPacket* packet, std::ofstream& output) {
  while (avcodec_receive_packet(encoder, packet) == 0) {
    output.write(reinterpret_cast<const char*>(packet->data), packet->size);
    av_packet_unref(packet);
  }
}

void write_frames_of_size(std::ofstream& output, const int width, const int height) {
  const codec_pointer encoder = open_h264_encoder(width, height);
  const packet_pointer packet(av_packet_alloc());
  const owned_frame frame;
  frame.get()->format = AV_PIX_FMT_YUV420P;
  frame.get()->width = width;
  frame.get()->height = height;

  const int buffer_status = av_frame_get_buffer(frame.get(), 0);
  ASSERT_EQ(buffer_status, 0);

  fill_with_mid_gray(*frame.get());

  for (int i = 0; i < FRAMES_PER_SIZE; ++i) {
    frame.get()->pts = i;
    const int send_status = avcodec_send_frame(encoder.get(), frame.get());
    ASSERT_EQ(send_status, 0);
    write_encoded_packets(encoder.get(), packet.get(), output);
  }

  const int flush_status = avcodec_send_frame(encoder.get(), nullptr);
  ASSERT_EQ(flush_status, 0);

  write_encoded_packets(encoder.get(), packet.get(), output);
}

void write_resizing_stream(const std::string& path) {
  std::ofstream output(path, std::ios::binary);
  write_frames_of_size(output, CALIBRATED_FRAME_SIZE, CALIBRATED_FRAME_SIZE);
  write_frames_of_size(output, RESIZED_FRAME_SIZE, CALIBRATED_FRAME_SIZE);
  write_frames_of_size(output, CALIBRATED_FRAME_SIZE, RESIZED_FRAME_SIZE);
}

}

TEST(av_utils_test, should_give_a_shared_frame_its_own_buffer_of_the_same_shape) {
  const owned_frame frame;
  const owned_frame reference;
  frame.get()->format = AV_PIX_FMT_GRAY8;
  frame.get()->width = SMALL_FRAME_WIDTH;
  frame.get()->height = SMALL_FRAME_HEIGHT;

  const int buffer_status = av_frame_get_buffer(frame.get(), 0);
  ASSERT_EQ(buffer_status, 0);

  const int reference_status = av_frame_ref(reference.get(), frame.get());
  ASSERT_EQ(reference_status, 0);

  const uint8_t* const shared_plane = frame.get()->data[0];

  ensure_fresh_writable_frame(frame.get());

  const int writable = av_frame_is_writable(frame.get());
  uint8_t* const fresh_plane = frame.get()->data[0];
  uint8_t* const reference_plane = reference.get()->data[0];
  const int frame_format = frame.get()->format;
  const int frame_width = frame.get()->width;
  const int frame_height = frame.get()->height;
  EXPECT_TRUE(writable);
  EXPECT_NE(fresh_plane, shared_plane);
  EXPECT_EQ(reference_plane, shared_plane);
  EXPECT_EQ(frame_format, AV_PIX_FMT_GRAY8);
  EXPECT_EQ(frame_width, SMALL_FRAME_WIDTH);
  EXPECT_EQ(frame_height, SMALL_FRAME_HEIGHT);
}

TEST(av_utils_test, should_keep_the_buffer_of_a_frame_that_is_already_writable) {
  const owned_frame frame;
  frame.get()->format = AV_PIX_FMT_GRAY8;
  frame.get()->width = SMALL_FRAME_WIDTH;
  frame.get()->height = SMALL_FRAME_HEIGHT;

  const int buffer_status = av_frame_get_buffer(frame.get(), 0);
  ASSERT_EQ(buffer_status, 0);

  const uint8_t* const plane = frame.get()->data[0];

  ensure_fresh_writable_frame(frame.get());

  uint8_t* const kept_plane = frame.get()->data[0];
  EXPECT_EQ(kept_plane, plane);
}

TEST(av_utils_test, should_name_the_error_and_the_action_in_an_ffmpeg_failure_message) {
  const std::string message = ffmpeg_failure_message(AVERROR(EINVAL), "open the codec");
  EXPECT_EQ(message, "FFmpeg error -22 (Invalid argument) occurred while trying to open the codec!");
}

TEST(video_encoder_test, should_reject_an_output_path_without_a_known_container) {
  const fixtures::temporary_file output(UNKNOWN_EXTENSION);

  const std::string message = fixtures::find_error_message([&output] { VideoEncoder encoder(output.string()); });

  const bool output_exists = std::filesystem::exists(output.path());
  EXPECT_EQ(message, "Output context could not be created!");
  EXPECT_FALSE(output_exists);
}

TEST(video_encoder_test, should_fail_to_open_an_output_file_in_a_missing_directory) {
  const fixtures::temporary_file missing_directory("");
  const std::filesystem::path output = missing_directory.path() / OUTPUT_NAME;

  const std::string message = fixtures::find_error_message([&output] { VideoEncoder encoder(output.string()); });

  const bool reports_open_failure = message.ends_with("occurred while trying to open the output file!");
  EXPECT_TRUE(reports_open_failure) << message;
}

TEST(video_encoder_test, should_fail_to_write_the_header_of_a_container_that_cannot_hold_ffv1) {
  const fixtures::temporary_file output(FLV_EXTENSION);

  const std::string message = fixtures::find_error_message([&output] { VideoEncoder encoder(output.string()); });

  const bool reports_header_failure = message.ends_with("occurred while trying to write the header!");
  EXPECT_TRUE(reports_header_failure) << message;
}

TEST(video_encoder_test, should_reject_a_packet_after_finalizing) {
  const fixtures::temporary_file output(MATROSKA_EXTENSION);
  VideoEncoder encoder(output.string());
  encoder.finalize();

  const std::string message = fixtures::find_error_message([&encoder] { encoder.add_packet(make_framed_packet(0)); });

  const int64_t frames_written = encoder.frames_written();
  EXPECT_EQ(message, "Encoder is already finalized!");
  EXPECT_EQ(frames_written, 0);
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

  const bool reports_frame_failure = message.ends_with("occurred while trying to write a frame!");
  EXPECT_TRUE(reports_frame_failure) << message;
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

  const bool reports_trailer_failure = message.ends_with("occurred while trying to write the trailer!");
  EXPECT_TRUE(reports_trailer_failure) << message;
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

  EXPECT_EQ(message, "Input file could not be opened!");
}

TEST(video_decoder_test, should_reject_an_input_without_a_video_stream) {
  const fixtures::temporary_file input(SUBTITLE_EXTENSION);
  std::ofstream(input.path()) << SUBTITLE_CONTENTS;

  const std::string message = fixtures::find_error_message([&input] { VideoDecoder decoder(input.string()); });

  EXPECT_EQ(message, "Input has no video stream!");
}

TEST(video_decoder_test, should_reject_a_video_whose_codec_has_no_decoder) {
  const fixtures::temporary_file input(AVI_EXTENSION);
  write_undecodable_video(input.string());

  const std::string message = fixtures::find_error_message([&input] { VideoDecoder decoder(input.string()); });

  EXPECT_EQ(message, "Decoder could not be found!");
}

TEST(video_decoder_test, should_count_frames_from_the_frame_count_of_the_container) {
  const fixtures::temporary_file video(AVI_EXTENSION);
  write_video(video.string(), FRAMES_TO_WRITE);
  VideoDecoder decoder(video.string());

  const int64_t total_frames = decoder.total_frames();
  const VideoDecoder::PacketList packets = decoder.decode_all_frames();

  const std::size_t expected_packets = static_cast<std::size_t>(packets_spanning_frames(FRAMES_TO_WRITE));
  const std::size_t packet_count = packets.size();
  const int64_t frames_read = decoder.frames_read();
  EXPECT_EQ(total_frames, FRAMES_TO_WRITE);
  EXPECT_EQ(packet_count, expected_packets);
  EXPECT_EQ(frames_read, FRAMES_TO_WRITE);
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

  const bool packets_empty = packets.empty();
  const int64_t frames_read = decoder.frames_read();
  EXPECT_TRUE(packets_empty);
  EXPECT_EQ(frames_read, 1);
}

TEST(video_decoder_test, should_size_a_packet_of_an_unknown_version_like_version_one) {
  const fixtures::temporary_file video(FLV_EXTENSION);
  Packet packet{};
  std::memcpy(packet.bytes.data(), &MAGIC_ID, MAGIC_SIZE);
  packet.bytes[VERSION_OFF] = std::byte{UNKNOWN_VERSION};
  write_one_packet_stream(video.string(), packet);
  VideoDecoder decoder(video.string());

  const VideoDecoder::PacketList packets = decoder.decode_all_frames();

  const std::size_t packet_count = packets.size();
  const std::size_t packet_size = packets[0].size();
  ASSERT_EQ(packet_count, 1U);
  EXPECT_EQ(packet_size, HEADER_SIZE + SYMBOL_SIZE_BYTES);
  EXPECT_EQ(packets[0][VERSION_OFF], std::byte{UNKNOWN_VERSION});
}

TEST(video_decoder_test, should_skip_a_frame_whose_slices_cannot_be_found) {
  const fixtures::temporary_file video(MATROSKA_EXTENSION);
  write_video(video.string(), FRAMES_TO_WRITE);
  break_the_slice_chain_of_the_last_frame(video.path());
  VideoDecoder decoder(video.string());

  const VideoDecoder::PacketList packets = decoder.decode_all_frames();

  const std::size_t packet_count = packets.size();
  const std::size_t expected_packet_count = static_cast<std::size_t>(VideoEncoder::packets_per_frame());
  const int64_t frames_read = decoder.frames_read();
  const bool at_end = decoder.is_eof();
  EXPECT_EQ(packet_count, expected_packet_count);
  EXPECT_EQ(frames_read, FRAMES_TO_WRITE - 1);
  EXPECT_TRUE(at_end);
}

TEST(video_decoder_test, should_read_every_frame_of_a_stream_whose_resolution_changes) {
  const fixtures::temporary_file stream(H264_EXTENSION);
  ASSERT_NO_FATAL_FAILURE(write_resizing_stream(stream.string()));

  VideoDecoder decoder(stream.string());

  const VideoDecoder::PacketList packets = decoder.decode_all_frames();

  const bool packets_empty = packets.empty();
  const int64_t frames_read = decoder.frames_read();
  EXPECT_TRUE(packets_empty);
  EXPECT_EQ(frames_read, FRAME_SIZES * FRAMES_PER_SIZE);
}

TEST(stream_encoder_test, should_reject_a_packet_after_finalizing) {
  const fixtures::temporary_file output(FLV_EXTENSION);
  StreamEncoder encoder(output.string(), STREAM_BITRATE_KBPS, ONE_PACKET_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT,
                        FRAME_FPS);
  encoder.finalize();

  const std::string message = fixtures::find_error_message([&encoder] { encoder.add_packet(make_framed_packet(0)); });

  EXPECT_EQ(message, "Stream encoder is already finalized!");
}

TEST(stream_encoder_test, should_report_a_frame_width_that_libx264_cannot_encode) {
  const fixtures::temporary_file output(FLV_EXTENSION);

  const std::string message = fixtures::find_error_message([&output] {
    StreamEncoder encoder(output.string(), STREAM_BITRATE_KBPS, ODD_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT, FRAME_FPS);
  });

  const bool reports_codec_failure = message.ends_with("occurred while trying to open the libx264 codec!");
  EXPECT_TRUE(reports_codec_failure) << message;
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

  const bool reports_frame_failure = message.ends_with("occurred while trying to write a frame to the stream!");
  EXPECT_TRUE(reports_frame_failure) << message;
}

TEST(stream_encoder_test, should_report_a_failed_trailer_when_the_disk_is_full) {
  if (!std::filesystem::exists(fixtures::FULL_DEVICE)) {
    GTEST_SKIP() << fixtures::NO_FULL_DEVICE_MESSAGE;
  }

  StreamEncoder encoder(fixtures::FULL_DEVICE, STREAM_BITRATE_KBPS, ONE_PACKET_FRAME_WIDTH, ONE_PACKET_FRAME_HEIGHT,
                        FRAME_FPS);
  encoder.add_packet(make_framed_packet(0));

  const std::string message = fixtures::find_error_message([&encoder] { encoder.finalize(); });

  const bool reports_trailer_failure = message.ends_with("occurred while trying to write the stream trailer!");
  EXPECT_TRUE(reports_trailer_failure) << message;
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
