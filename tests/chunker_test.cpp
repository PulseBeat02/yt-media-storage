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

#include "chunker.h"
#include "configuration.h"
#include "fixtures.h"

#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* UNSEEKABLE_FILE = "/proc/self/status";
constexpr const char* NO_UNSEEKABLE_FILE_MESSAGE = "This platform has no file without a size!";

constexpr std::size_t SINGLE_BYTE_CHUNK_SIZE = 1;
constexpr std::size_t CUSTOM_CHUNK_SIZE = 100;

constexpr std::size_t THIRD_CHUNK_INDEX = 2;
constexpr std::size_t THIRD_CHUNK_OFFSET = THIRD_CHUNK_INDEX * CUSTOM_CHUNK_SIZE;

constexpr const char* BINARY_EXTENSION = ".bin";

void expect_chunks_cover_data(const chunked_storage_data& chunked, const std::vector<std::byte>& original) {
  std::size_t total_length = 0;
  for (const auto& [offset, length] : chunked.chunks) {
    total_length += length;
  }

  const std::size_t original_size = original.size();
  EXPECT_EQ(total_length, original_size);
  EXPECT_EQ(chunked.storage, original);
}

}

TEST(chunker_test, should_chunk_empty_data_into_no_chunks) {
  const std::vector<std::byte> empty_data;
  const std::span empty_span(empty_data.data(), empty_data.size());
  const auto [storage, chunks] = chunk_byte_data(empty_span);
  const std::size_t chunk_count = chunks.size();
  const bool storage_empty = storage.empty();
  ASSERT_EQ(chunk_count, 1u);
  EXPECT_EQ(chunks[0].offset, 0u);
  EXPECT_EQ(chunks[0].length, 0u);
  EXPECT_TRUE(storage_empty);
}

TEST(chunker_test, should_chunk_small_data_into_one_chunk) {
  const std::vector<std::byte> small_data = fixtures::make_stepped_data(100);
  const std::span data_span(small_data.data(), small_data.size());
  const auto [storage, chunks] = chunk_byte_data(data_span);
  const std::size_t chunk_count = chunks.size();
  ASSERT_EQ(chunk_count, 1u);
  EXPECT_EQ(chunks[0].offset, 0u);
  EXPECT_EQ(chunks[0].length, 100u);
}

TEST(chunker_test, should_chunk_data_of_one_chunk_size_into_one_chunk) {
  const std::vector<std::byte> exact_data = fixtures::make_stepped_data(CHUNK_SIZE_BYTES);
  const std::span data_span(exact_data.data(), exact_data.size());
  const auto [storage, chunks] = chunk_byte_data(data_span);
  const std::size_t chunk_count = chunks.size();
  ASSERT_EQ(chunk_count, 1u);
  EXPECT_EQ(chunks[0].offset, 0u);
  EXPECT_EQ(chunks[0].length, CHUNK_SIZE_BYTES);
}

TEST(chunker_test, should_chunk_large_data_into_several_chunks) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES * 2 + CHUNK_SIZE_BYTES / 2;
  const std::vector<std::byte> large_data = fixtures::make_stepped_data(total_size);
  const std::span data_span(large_data.data(), large_data.size());
  const auto [storage, chunks] = chunk_byte_data(data_span);
  const std::size_t chunk_count = chunks.size();
  ASSERT_EQ(chunk_count, 3u);
  EXPECT_EQ(chunks[0].length, CHUNK_SIZE_BYTES);
  EXPECT_EQ(chunks[1].length, CHUNK_SIZE_BYTES);
  EXPECT_EQ(chunks[THIRD_CHUNK_INDEX].length, CHUNK_SIZE_BYTES / 2);
}

TEST(chunker_test, should_keep_the_data_in_its_chunks) {
  const std::vector<std::byte> original_data = fixtures::make_stepped_data(5000, 17);
  const std::span data_span(original_data.data(), original_data.size());
  const chunked_storage_data chunked = chunk_byte_data(data_span);
  expect_chunks_cover_data(chunked, original_data);
}

TEST(chunker_test, should_return_the_slice_of_a_chunk) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 42;
  const std::vector<std::byte> input_data = fixtures::make_stepped_data(total_size);
  const std::span data_span(input_data.data(), input_data.size());
  const chunked_storage_data chunked = chunk_byte_data(data_span);

  const std::size_t chunk_count = chunked.chunks.size();
  ASSERT_EQ(chunk_count, 2u);

  for (std::size_t chunk_index = 0; chunk_index < chunked.chunks.size(); ++chunk_index) {
    const std::span<const std::byte> slice = chunk_span(chunked, chunk_index);
    const auto& [offset, length] = chunked.chunks[chunk_index];
    const std::size_t slice_size = slice.size();
    const std::byte* const slice_start = slice.data();
    const std::byte* const expected_slice_start = chunked.storage.data() + offset;
    EXPECT_EQ(slice_size, length);
    EXPECT_EQ(slice_start, expected_slice_start);
    for (std::size_t byte_index = 0; byte_index < slice.size(); ++byte_index) {
      EXPECT_EQ(slice[byte_index], chunked.storage[offset + byte_index]);
    }
  }
}

TEST(chunker_test, should_chunk_a_file_like_its_bytes) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 99;
  const std::vector<std::byte> original_data = fixtures::make_stepped_data(total_size, 3);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), original_data);

  const std::span data_span(original_data.data(), original_data.size());
  const auto [storage, chunks] = chunk_byte_data(data_span);
  const std::string input_path = input.string();
  const auto [from_storage, from_chunks] = chunk_file(input_path.c_str());

  const std::size_t chunk_count = chunks.size();
  const std::size_t expected_chunk_count = from_chunks.size();
  EXPECT_EQ(storage, from_storage);
  ASSERT_EQ(chunk_count, expected_chunk_count);

  for (std::size_t i = 0; i < chunks.size(); ++i) {
    EXPECT_EQ(chunks[i].offset, from_chunks[i].offset);
    EXPECT_EQ(chunks[i].length, from_chunks[i].length);
  }
}

TEST(chunker_test, should_read_no_chunks_from_an_empty_file) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), {});

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str());

  const std::size_t chunk_count = reader.chunk_count();
  const std::size_t file_size = reader.file_size();
  const std::size_t chunk_size = reader.chunk_size();
  EXPECT_EQ(chunk_count, 1u);
  EXPECT_EQ(file_size, 0u);
  EXPECT_EQ(chunk_size, CHUNK_SIZE_BYTES);
}

TEST(chunker_test, should_read_one_chunk_from_a_single_byte_file) {
  const std::vector<std::byte> one_byte = {std::byte{0xAB}};
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), one_byte);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str());

  const std::size_t chunk_count = reader.chunk_count();
  const std::size_t file_size = reader.file_size();
  EXPECT_EQ(chunk_count, 1u);
  EXPECT_EQ(file_size, 1u);
}

TEST(chunker_test, should_read_one_chunk_from_a_file_of_one_chunk_size) {
  const std::vector<std::byte> exact_data(CHUNK_SIZE_BYTES);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), exact_data);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str());

  const std::size_t chunk_count = reader.chunk_count();
  const std::size_t file_size = reader.file_size();
  EXPECT_EQ(chunk_count, 1u);
  EXPECT_EQ(file_size, CHUNK_SIZE_BYTES);
}

TEST(chunker_test, should_read_two_chunks_from_a_file_just_over_one_chunk) {
  const std::vector<std::byte> over_data(CHUNK_SIZE_BYTES + 1);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), over_data);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str());

  const std::size_t chunk_count = reader.chunk_count();
  const std::size_t file_size = reader.file_size();
  EXPECT_EQ(chunk_count, 2u);
  EXPECT_EQ(file_size, CHUNK_SIZE_BYTES + 1);
}

TEST(chunker_test, should_read_the_contents_of_each_chunk) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 50;
  const std::vector<std::byte> original_data = fixtures::make_stepped_data(total_size, 11);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), original_data);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str());

  const std::size_t chunk_count = reader.chunk_count();
  ASSERT_EQ(chunk_count, 2u);

  const std::vector<std::byte> first_chunk = reader.read_chunk(0);
  const std::vector<std::byte> second_chunk = reader.read_chunk(1);

  const std::size_t first_chunk_size = first_chunk.size();
  const std::size_t second_chunk_size = second_chunk.size();
  ASSERT_EQ(first_chunk_size, CHUNK_SIZE_BYTES);
  ASSERT_EQ(second_chunk_size, 50u);

  for (std::size_t i = 0; i < CHUNK_SIZE_BYTES; ++i) {
    EXPECT_EQ(first_chunk[i], original_data[i]);
  }
  for (std::size_t i = 0; i < 50u; ++i) {
    EXPECT_EQ(second_chunk[i], original_data[CHUNK_SIZE_BYTES + i]);
  }
}

TEST(chunker_test, should_read_chunks_of_a_custom_size) {
  constexpr std::size_t total_size = 250;
  const std::vector<std::byte> original_data = fixtures::make_stepped_data(total_size);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), original_data);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str(), CUSTOM_CHUNK_SIZE);

  const std::size_t chunk_size = reader.chunk_size();
  const std::size_t chunk_count = reader.chunk_count();
  EXPECT_EQ(chunk_size, CUSTOM_CHUNK_SIZE);
  EXPECT_EQ(chunk_count, 3u);

  const std::vector<std::byte> first_chunk = reader.read_chunk(0);
  const std::vector<std::byte> second_chunk = reader.read_chunk(1);
  const std::vector<std::byte> third_chunk = reader.read_chunk(THIRD_CHUNK_INDEX);

  const std::size_t first_chunk_size = first_chunk.size();
  const std::size_t second_chunk_size = second_chunk.size();
  const std::size_t third_chunk_size = third_chunk.size();
  ASSERT_EQ(first_chunk_size, CUSTOM_CHUNK_SIZE);
  ASSERT_EQ(second_chunk_size, CUSTOM_CHUNK_SIZE);
  ASSERT_EQ(third_chunk_size, 50u);

  for (std::size_t i = 0; i < CUSTOM_CHUNK_SIZE; ++i) {
    EXPECT_EQ(first_chunk[i], original_data[i]);
    EXPECT_EQ(second_chunk[i], original_data[CUSTOM_CHUNK_SIZE + i]);
  }
  for (std::size_t i = 0; i < 50; ++i) {
    EXPECT_EQ(third_chunk[i], original_data[THIRD_CHUNK_OFFSET + i]);
  }
}

TEST(chunker_test, should_fail_to_read_a_chunk_past_the_end) {
  const std::vector<std::byte> small_data(10);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), small_data);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str());

  const std::size_t invalid_index = reader.chunk_count();
  EXPECT_THROW(static_cast<void>(reader.read_chunk(invalid_index)), std::runtime_error);
}

TEST(chunker_test, should_read_chunks_in_any_order) {
  constexpr std::size_t total_size = 300;
  const std::vector<std::byte> original_data = fixtures::make_stepped_data(total_size, 7);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), original_data);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str(), CUSTOM_CHUNK_SIZE);

  const std::size_t chunk_count = reader.chunk_count();
  ASSERT_EQ(chunk_count, 3u);

  const std::vector<std::byte> chunk2 = reader.read_chunk(THIRD_CHUNK_INDEX);
  const std::vector<std::byte> chunk0 = reader.read_chunk(0);
  const std::vector<std::byte> chunk1 = reader.read_chunk(1);

  for (std::size_t i = 0; i < CUSTOM_CHUNK_SIZE; ++i) {
    EXPECT_EQ(chunk0[i], original_data[i]);
    EXPECT_EQ(chunk1[i], original_data[CUSTOM_CHUNK_SIZE + i]);
    EXPECT_EQ(chunk2[i], original_data[THIRD_CHUNK_OFFSET + i]);
  }
}

TEST(chunker_test, should_read_the_same_chunk_twice) {
  const std::vector<std::byte> original_data = fixtures::make_stepped_data(200, 13);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), original_data);

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str(), CUSTOM_CHUNK_SIZE);

  const std::vector<std::byte> first_read = reader.read_chunk(0);
  const std::vector<std::byte> second_read = reader.read_chunk(0);

  EXPECT_EQ(first_read, second_read);
}

TEST(chunker_test, should_fail_to_read_a_missing_file) {
  const fixtures::temporary_file missing_input(BINARY_EXTENSION);
  const std::string missing_input_path = missing_input.string();
  EXPECT_THROW(file_chunk_reader(missing_input_path.c_str()), std::runtime_error);
}

TEST(chunker_test, should_fail_to_chunk_a_missing_file) {
  const fixtures::temporary_file missing_input(BINARY_EXTENSION);
  const std::string missing_input_path = missing_input.string();
  EXPECT_THROW(static_cast<void>(chunk_file(missing_input_path.c_str())), std::runtime_error);
}

TEST(chunker_test, should_chunk_a_file_into_chunks_of_a_custom_size) {
  constexpr std::size_t custom_chunk = 50;
  const std::vector<std::byte> original_data = fixtures::make_stepped_data(120, 5);
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), original_data);

  const std::string input_path = input.string();
  const auto [storage, chunks] = chunk_file(input_path.c_str(), custom_chunk);

  const std::size_t chunk_count = chunks.size();
  ASSERT_EQ(chunk_count, 3u);
  EXPECT_EQ(chunks[0].length, 50u);
  EXPECT_EQ(chunks[1].length, 50u);
  EXPECT_EQ(chunks[THIRD_CHUNK_INDEX].length, 20u);
  EXPECT_EQ(storage, original_data);
}

TEST(chunker_test, should_chunk_data_of_two_chunk_sizes_into_two_chunks) {
  constexpr std::size_t total_size = CHUNK_SIZE_BYTES * 2;
  const std::vector<std::byte> data = fixtures::make_stepped_data(total_size);
  const std::span data_span(data.data(), data.size());
  const auto [storage, chunks] = chunk_byte_data(data_span);
  const std::size_t chunk_count = chunks.size();
  ASSERT_EQ(chunk_count, 2u);
  EXPECT_EQ(chunks[0].length, CHUNK_SIZE_BYTES);
  EXPECT_EQ(chunks[1].length, CHUNK_SIZE_BYTES);
}

TEST(chunker_test, should_chunk_an_empty_file_into_no_chunks) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), {});

  const std::string input_path = input.string();
  const auto [storage, chunks] = chunk_file(input_path.c_str());
  const std::size_t chunk_count = chunks.size();
  const bool storage_empty = storage.empty();
  ASSERT_EQ(chunk_count, 1u);
  EXPECT_EQ(chunks[0].length, 0u);
  EXPECT_TRUE(storage_empty);
}

TEST(chunker_test, should_return_an_empty_view_of_an_empty_file) {
  const fixtures::temporary_file input(BINARY_EXTENSION);
  fixtures::write_bytes(input.path(), {});

  const std::string input_path = input.string();
  const file_chunk_reader reader(input_path.c_str());

  const std::span<const std::byte> view = reader.chunk_view(0);
  const bool view_empty = view.empty();
  EXPECT_TRUE(view_empty);
}

TEST(chunker_test, should_stream_a_file_that_cannot_be_memory_mapped) {
  if (!std::filesystem::exists(fixtures::UNMAPPABLE_FILE)) {
    GTEST_SKIP() << fixtures::NO_UNMAPPABLE_FILE_MESSAGE;
  }

  const file_chunk_reader reader(fixtures::UNMAPPABLE_FILE, SINGLE_BYTE_CHUNK_SIZE);

  const std::vector<std::byte> first_chunk = reader.read_chunk(0);
  const bool mapped = reader.is_mapped();
  const std::size_t first_chunk_size = first_chunk.size();
  EXPECT_FALSE(mapped);
  EXPECT_EQ(first_chunk_size, SINGLE_BYTE_CHUNK_SIZE);
}

TEST(chunker_test, should_fail_to_read_past_the_content_of_an_unmapped_file) {
  if (!std::filesystem::exists(fixtures::UNMAPPABLE_FILE)) {
    GTEST_SKIP() << fixtures::NO_UNMAPPABLE_FILE_MESSAGE;
  }

  const file_chunk_reader reader(fixtures::UNMAPPABLE_FILE, SINGLE_BYTE_CHUNK_SIZE);
  const std::size_t last_chunk = reader.chunk_count() - 1;

  EXPECT_THROW(static_cast<void>(reader.read_chunk(last_chunk)), std::runtime_error);
}

TEST(chunker_test, should_refuse_a_view_of_an_unmapped_file) {
  if (!std::filesystem::exists(fixtures::UNMAPPABLE_FILE)) {
    GTEST_SKIP() << fixtures::NO_UNMAPPABLE_FILE_MESSAGE;
  }

  const file_chunk_reader reader(fixtures::UNMAPPABLE_FILE, SINGLE_BYTE_CHUNK_SIZE);

  EXPECT_THROW(static_cast<void>(reader.chunk_view(0)), std::runtime_error);
}

TEST(chunker_test, should_fail_to_chunk_a_file_shorter_than_its_reported_size) {
  if (!std::filesystem::exists(fixtures::UNMAPPABLE_FILE)) {
    GTEST_SKIP() << fixtures::NO_UNMAPPABLE_FILE_MESSAGE;
  }

  EXPECT_THROW(static_cast<void>(chunk_file(fixtures::UNMAPPABLE_FILE)), std::runtime_error);
}

TEST(chunker_test, should_fail_to_read_a_file_without_a_size) {
  if (!std::filesystem::exists(UNSEEKABLE_FILE)) {
    GTEST_SKIP() << NO_UNSEEKABLE_FILE_MESSAGE;
  }

  EXPECT_THROW(const file_chunk_reader reader(UNSEEKABLE_FILE), std::runtime_error);
}
