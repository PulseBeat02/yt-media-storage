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

#include "../include/media_storage.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct TempFile {
  std::string path_str;

  explicit TempFile(const std::string& name) : path_str((std::filesystem::temp_directory_path() / name).string()) {
  }

  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_str, ec);
  }

  [[nodiscard]] const char* c_str() const {
    return path_str.c_str();
  }

  TempFile(const TempFile&) = delete;

  TempFile& operator=(const TempFile&) = delete;
};

void write_test_file(const std::string& path, const std::size_t size) {
  std::ofstream ofs(path, std::ios::binary);
  std::vector<char> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>((i * 131 + 17) & 0xFF);
  }
  ofs.write(data.data(), static_cast<std::streamsize>(size));
}

std::vector<char> read_test_file(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs.is_open())
    return {};
  const auto size = ifs.tellg();
  ifs.seekg(0);
  std::vector<char> data(size);
  ifs.read(data.data(), size);
  return data;
}
} // namespace

TEST(api_test, should_report_a_version) {
  const char* version = ms_version();
  ASSERT_NE(version, nullptr);
  EXPECT_GT(std::strlen(version), 0u);
}

TEST(api_test, should_describe_every_status) {
  EXPECT_STREQ(ms_status_string(MS_OK), "success");
  EXPECT_STREQ(ms_status_string(MS_ERR_INVALID_ARGS), "invalid arguments");
  EXPECT_STREQ(ms_status_string(MS_ERR_FILE_NOT_FOUND), "file not found");
  EXPECT_STREQ(ms_status_string(MS_ERR_IO), "I/O error");
  EXPECT_STREQ(ms_status_string(MS_ERR_ENCODE_FAILED), "encoding failed");
  EXPECT_STREQ(ms_status_string(MS_ERR_DECODE_FAILED), "decoding failed");
  EXPECT_STREQ(ms_status_string(MS_ERR_CRYPTO), "encryption/decryption error");
  EXPECT_STREQ(ms_status_string(MS_ERR_INCOMPLETE), "incomplete data");
}

TEST(api_test, should_describe_an_unknown_status) {
  EXPECT_STREQ(ms_status_string(static_cast<ms_status_t>(9999)), "unknown error");
}

TEST(api_test, should_reject_encoding_without_options) {
  EXPECT_EQ(ms_encode(nullptr, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_encoding_without_paths) {
  ms_encode_options_t opts{};
  EXPECT_EQ(ms_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);

  opts.input_path = "foo";
  opts.output_path = nullptr;
  EXPECT_EQ(ms_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_encoding_without_an_input_path) {
  const TempFile output("api_enc_missing_input.mkv");
  ms_encode_options_t opts{};
  opts.input_path = nullptr;
  opts.output_path = output.c_str();
  EXPECT_EQ(ms_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_report_a_missing_input_file_when_encoding) {
  const std::string missing_input =
      (std::filesystem::temp_directory_path() / "nonexistent_ms_api_test_12345.bin").string();
  const TempFile output("api_encode_fnf_out.mkv");
  ms_encode_options_t opts{};
  opts.input_path = missing_input.c_str();
  opts.output_path = output.c_str();
  EXPECT_EQ(ms_encode(&opts, nullptr), MS_ERR_FILE_NOT_FOUND);
}

TEST(api_test, should_reject_encrypting_without_a_password) {
  const TempFile input("api_test_enc_nopw.bin");
  write_test_file(input.path_str, 1024);

  const TempFile output("api_test_enc_nopw.mkv");
  ms_encode_options_t opts{};
  opts.input_path = input.c_str();
  opts.output_path = output.c_str();
  opts.encrypt = 1;
  opts.password = nullptr;
  opts.password_len = 0;

  EXPECT_EQ(ms_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_decoding_without_options) {
  EXPECT_EQ(ms_decode(nullptr, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_decoding_without_paths) {
  const ms_decode_options_t opts{};
  EXPECT_EQ(ms_decode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_decoding_without_an_input_path) {
  const TempFile output("api_dec_missing_in.bin");
  ms_decode_options_t opts{};
  opts.input_path = nullptr;
  opts.output_path = output.c_str();
  EXPECT_EQ(ms_decode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_decoding_without_an_output_path) {
  ms_decode_options_t opts{};
  opts.input_path = "any.mkv";
  opts.output_path = nullptr;
  EXPECT_EQ(ms_decode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_report_a_missing_input_file_when_decoding) {
  const std::string missing_input =
      (std::filesystem::temp_directory_path() / "nonexistent_ms_api_test_12345.mkv").string();
  const TempFile output("api_decode_fnf_out.bin");
  ms_decode_options_t opts{};
  opts.input_path = missing_input.c_str();
  opts.output_path = output.c_str();
  EXPECT_EQ(ms_decode(&opts, nullptr), MS_ERR_FILE_NOT_FOUND);
}

TEST(api_test, should_reject_stream_encoding_without_options) {
  EXPECT_EQ(ms_stream_encode(nullptr, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_stream_encoding_without_fields) {
  ms_stream_encode_options_t opts{};
  EXPECT_EQ(ms_stream_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);

  opts.input_path = "foo";
  opts.stream_url = nullptr;
  EXPECT_EQ(ms_stream_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_stream_encoding_without_an_input_path) {
  ms_stream_encode_options_t opts{};
  opts.input_path = nullptr;
  opts.stream_url = "rtmp://example/live";
  EXPECT_EQ(ms_stream_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_encrypted_stream_encoding_without_a_password) {
  const TempFile input("api_stream_enc_nopw.bin");
  write_test_file(input.path_str, 1024);

  ms_stream_encode_options_t opts{};
  opts.input_path = input.c_str();
  opts.stream_url = "rtmp://example/live";
  opts.encrypt = 1;
  opts.password = nullptr;
  opts.password_len = 0;

  EXPECT_EQ(ms_stream_encode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_stream_decoding_without_options) {
  EXPECT_EQ(ms_stream_decode(nullptr, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_stream_decoding_without_fields) {
  const ms_stream_decode_options_t opts{};
  EXPECT_EQ(ms_stream_decode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_stream_decoding_without_a_stream_url) {
  const TempFile output("api_stream_dec_out.bin");
  ms_stream_decode_options_t opts{};
  opts.stream_url = nullptr;
  opts.output_path = output.c_str();
  EXPECT_EQ(ms_stream_decode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_reject_stream_decoding_without_an_output_path) {
  ms_stream_decode_options_t opts{};
  opts.stream_url = "rtmp://example/live";
  opts.output_path = nullptr;
  EXPECT_EQ(ms_stream_decode(&opts, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(api_test, should_decode_what_it_encoded) {
  const TempFile input("api_rt_input.bin");
  const TempFile encoded("api_rt.mkv");
  const TempFile decoded("api_rt_output.bin");

  write_test_file(input.path_str, 65536);

  ms_encode_options_t enc_opts{};
  enc_opts.input_path = input.c_str();
  enc_opts.output_path = encoded.c_str();

  ms_result_t enc_result{};
  ASSERT_EQ(ms_encode(&enc_opts, &enc_result), MS_OK);
  EXPECT_GT(enc_result.input_size, 0u);
  EXPECT_GT(enc_result.output_size, 0u);
  EXPECT_GT(enc_result.total_chunks, 0u);
  EXPECT_GT(enc_result.total_packets, 0u);
  EXPECT_GT(enc_result.total_frames, 0u);

  ms_decode_options_t dec_opts{};
  dec_opts.input_path = encoded.c_str();
  dec_opts.output_path = decoded.c_str();

  ms_result_t dec_result{};
  ASSERT_EQ(ms_decode(&dec_opts, &dec_result), MS_OK);

  EXPECT_EQ(read_test_file(input.path_str), read_test_file(decoded.path_str));
}

TEST(api_test, should_decode_what_it_encrypted) {
  const TempFile input("api_enc_input.bin");
  const TempFile encoded("api_enc.mkv");
  const TempFile decoded("api_enc_output.bin");

  write_test_file(input.path_str, 32768);
  const std::string password = "test_api_password";

  ms_encode_options_t enc_opts{};
  enc_opts.input_path = input.c_str();
  enc_opts.output_path = encoded.c_str();
  enc_opts.encrypt = 1;
  enc_opts.password = password.c_str();
  enc_opts.password_len = password.size();

  ASSERT_EQ(ms_encode(&enc_opts, nullptr), MS_OK);

  ms_decode_options_t dec_opts{};
  dec_opts.input_path = encoded.c_str();
  dec_opts.output_path = decoded.c_str();
  dec_opts.password = password.c_str();
  dec_opts.password_len = password.size();

  ASSERT_EQ(ms_decode(&dec_opts, nullptr), MS_OK);
  EXPECT_EQ(read_test_file(input.path_str), read_test_file(decoded.path_str));
}

TEST(api_test, should_decode_what_it_encoded_with_xxhash_checksums) {
  const TempFile input("api_xxh_input.bin");
  const TempFile encoded("api_xxh.mkv");
  const TempFile decoded("api_xxh_output.bin");

  write_test_file(input.path_str, 32768);

  ms_encode_options_t enc_opts{};
  enc_opts.input_path = input.c_str();
  enc_opts.output_path = encoded.c_str();
  enc_opts.hash_algorithm = MS_HASH_XXHASH32;

  ASSERT_EQ(ms_encode(&enc_opts, nullptr), MS_OK);

  ms_decode_options_t dec_opts{};
  dec_opts.input_path = encoded.c_str();
  dec_opts.output_path = decoded.c_str();

  ASSERT_EQ(ms_decode(&dec_opts, nullptr), MS_OK);
  EXPECT_EQ(read_test_file(input.path_str), read_test_file(decoded.path_str));
}

TEST(api_test, should_report_encoding_progress) {
  const TempFile input("api_prog_input.bin");
  const TempFile encoded("api_prog.mkv");

  write_test_file(input.path_str, 65536);

  std::atomic<int> call_count{0};

  ms_encode_options_t enc_opts{};
  enc_opts.input_path = input.c_str();
  enc_opts.output_path = encoded.c_str();
  enc_opts.progress = [](uint64_t, uint64_t, void* user) -> int {
    static_cast<std::atomic<int>*>(user)->fetch_add(1);
    return 0;
  };
  enc_opts.progress_user = &call_count;

  ASSERT_EQ(ms_encode(&enc_opts, nullptr), MS_OK);
  EXPECT_GT(call_count.load(), 0);
}

TEST(api_test, should_cancel_encoding_from_the_progress_callback) {
  const TempFile input("api_cancel_input.bin");
  const TempFile encoded("api_cancel.mkv");

  write_test_file(input.path_str, 65536);

  ms_encode_options_t enc_opts{};
  enc_opts.input_path = input.c_str();
  enc_opts.output_path = encoded.c_str();
  enc_opts.progress = [](uint64_t, uint64_t, void*) -> int { return 1; };

  EXPECT_EQ(ms_encode(&enc_opts, nullptr), MS_ERR_ENCODE_FAILED);
}

TEST(api_test, should_describe_the_last_failure_and_clear_it_on_success) {
  EXPECT_EQ(ms_encode(nullptr, nullptr), MS_ERR_INVALID_ARGS);
  EXPECT_GT(std::strlen(ms_last_error()), 0u);

  const TempFile input("api_last_error_input.bin");
  const TempFile encoded("api_last_error.mkv");
  write_test_file(input.path_str, 1024);

  ms_encode_options_t opts{};
  opts.input_path = input.c_str();
  opts.output_path = encoded.c_str();
  ASSERT_EQ(ms_encode(&opts, nullptr), MS_OK);
  EXPECT_STREQ(ms_last_error(), "");
}

TEST(api_test, should_report_a_crypto_error_for_the_wrong_password) {
  const TempFile input("api_wrongpw_input.bin");
  const TempFile encoded("api_wrongpw.mkv");
  const TempFile decoded("api_wrongpw_output.bin");

  write_test_file(input.path_str, 16384);
  const std::string password = "correct horse";
  const std::string wrong_password = "battery staple";

  ms_encode_options_t enc_opts{};
  enc_opts.input_path = input.c_str();
  enc_opts.output_path = encoded.c_str();
  enc_opts.encrypt = 1;
  enc_opts.password = password.c_str();
  enc_opts.password_len = password.size();
  ASSERT_EQ(ms_encode(&enc_opts, nullptr), MS_OK);

  ms_decode_options_t dec_opts{};
  dec_opts.input_path = encoded.c_str();
  dec_opts.output_path = decoded.c_str();
  dec_opts.password = wrong_password.c_str();
  dec_opts.password_len = wrong_password.size();
  EXPECT_EQ(ms_decode(&dec_opts, nullptr), MS_ERR_CRYPTO);
  EXPECT_FALSE(std::filesystem::exists(decoded.path_str));
}

TEST(api_test, should_report_a_crypto_error_without_a_password) {
  const TempFile input("api_nopw_dec_input.bin");
  const TempFile encoded("api_nopw_dec.mkv");
  const TempFile decoded("api_nopw_dec_output.bin");

  write_test_file(input.path_str, 4096);
  const std::string password = "secret";

  ms_encode_options_t enc_opts{};
  enc_opts.input_path = input.c_str();
  enc_opts.output_path = encoded.c_str();
  enc_opts.encrypt = 1;
  enc_opts.password = password.c_str();
  enc_opts.password_len = password.size();
  ASSERT_EQ(ms_encode(&enc_opts, nullptr), MS_OK);

  ms_decode_options_t dec_opts{};
  dec_opts.input_path = encoded.c_str();
  dec_opts.output_path = decoded.c_str();
  EXPECT_EQ(ms_decode(&dec_opts, nullptr), MS_ERR_CRYPTO);
}

TEST(api_test, should_report_a_directory_as_input_without_throwing) {
  const std::string directory = std::filesystem::temp_directory_path().string();
  const TempFile output("api_dir_input.mkv");

  ms_encode_options_t opts{};
  opts.input_path = directory.c_str();
  opts.output_path = output.c_str();
  EXPECT_NE(ms_encode(&opts, nullptr), MS_OK);
}
