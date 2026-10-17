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

#ifndef MEDIA_STORAGE_TESTS_FIXTURES_H
#define MEDIA_STORAGE_TESTS_FIXTURES_H

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fixtures {

constexpr std::size_t PATTERN_MULTIPLIER = 131;
constexpr std::size_t PATTERN_OFFSET = 17;
constexpr std::size_t BYTE_MASK = 0xFF;

class temporary_file {
public:
  explicit temporary_file(const std::string& extension) {
    const testing::TestInfo* const test = testing::UnitTest::GetInstance()->current_test_info();
    const std::string name = std::string("ms_") + test->test_suite_name() + "_" + test->name() + "_" +
                             std::to_string(next_index()) + extension;
    path_ = std::filesystem::temp_directory_path() / name;
  }

  temporary_file(const temporary_file&) = delete;

  temporary_file& operator=(const temporary_file&) = delete;

  ~temporary_file() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

  [[nodiscard]] const std::filesystem::path& path() const {
    return path_;
  }

private:
  static int next_index() {
    static int index = 0;
    return ++index;
  }

  std::filesystem::path path_;
};

inline std::vector<std::byte> make_pattern(const std::size_t size) {
  std::vector<std::byte> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<std::byte>((i * PATTERN_MULTIPLIER + PATTERN_OFFSET) & BYTE_MASK);
  }
  return data;
}

inline void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& data) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

inline std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<char> characters{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  std::vector<std::byte> data(characters.size());
  for (std::size_t i = 0; i < characters.size(); ++i) {
    data[i] = static_cast<std::byte>(characters[i]);
  }
  return data;
}

}

#endif
