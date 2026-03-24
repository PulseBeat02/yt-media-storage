#include <gtest/gtest.h>

#include "chunker.h"
#include "configuration.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path make_unique_temp_path() {
    static std::atomic<std::uint64_t> counter{0};
    const std::string filename = "chunker_test_" + std::to_string(counter.fetch_add(1)) + ".bin";
    return std::filesystem::temp_directory_path() / filename;
}

struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const std::vector<std::byte> &contents) : path(make_unique_temp_path()) {
        std::ofstream stream(path, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("open failed");
        }
        const auto byte_count = static_cast<std::streamsize>(contents.size());
        stream.write(reinterpret_cast<const char *>(contents.data()), byte_count);
    }

    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    [[nodiscard]] const char *path_cstr() const {
        static thread_local std::string path_str;
        path_str = path.string();
        return path_str.c_str();
    }

    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;
    TempFile(TempFile &&) = delete;
    TempFile &operator=(TempFile &&) = delete;
};

std::vector<std::byte> make_patterned_data(const std::size_t size, const unsigned char multiplier = 1) {
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>(static_cast<unsigned char>((i * multiplier) & 0xFF));
    }
    return data;
}

void expect_chunks_cover_data(const ChunkedStorageData &chunked,
                              const std::vector<std::byte> &original) {
    std::size_t total_length = 0;
    for (const auto &[offset, length] : chunked.chunks) {
        total_length += length;
    }
    EXPECT_EQ(total_length, original.size());
    EXPECT_EQ(chunked.storage, original);
}

} // namespace

TEST(Chunker, ChunkByteData_EmptyInput) {
    constexpr std::vector<std::byte> empty_data;
    const std::span empty_span(empty_data.data(), empty_data.size());
    const auto [storage, chunks] = chunkByteData(empty_span);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].offset, 0u);
    EXPECT_EQ(chunks[0].length, 0u);
    EXPECT_TRUE(storage.empty());
}

TEST(Chunker, ChunkByteData_SmallInput) {
    const std::vector<std::byte> small_data = make_patterned_data(100);
    const std::span data_span(small_data.data(), small_data.size());
    const auto [storage, chunks] = chunkByteData(data_span);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].offset, 0u);
    EXPECT_EQ(chunks[0].length, 100u);
}

TEST(Chunker, ChunkByteData_ExactChunkSize) {
    const std::vector<std::byte> exact_data = make_patterned_data(CHUNK_SIZE_BYTES);
    const std::span data_span(exact_data.data(), exact_data.size());
    const auto [storage, chunks] = chunkByteData(data_span);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].offset, 0u);
    EXPECT_EQ(chunks[0].length, CHUNK_SIZE_BYTES);
}

TEST(Chunker, ChunkByteData_MultipleChunks) {
    constexpr std::size_t total_size = CHUNK_SIZE_BYTES * 2 + CHUNK_SIZE_BYTES / 2;
    const std::vector<std::byte> large_data = make_patterned_data(total_size);
    const std::span data_span(large_data.data(), large_data.size());
    const auto [storage, chunks] = chunkByteData(data_span);
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].length, CHUNK_SIZE_BYTES);
    EXPECT_EQ(chunks[1].length, CHUNK_SIZE_BYTES);
    EXPECT_EQ(chunks[2].length, CHUNK_SIZE_BYTES / 2);
}

TEST(Chunker, ChunkByteData_StoragePreservesData) {
    const std::vector<std::byte> original_data = make_patterned_data(5000, 17);
    const std::span data_span(original_data.data(), original_data.size());
    const ChunkedStorageData chunked = chunkByteData(data_span);
    expect_chunks_cover_data(chunked, original_data);
}

TEST(Chunker, ChunkSpan_ReturnsCorrectSlice) {
    constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 42;
    const std::vector<std::byte> input_data = make_patterned_data(total_size);
    const std::span data_span(input_data.data(), input_data.size());
    const ChunkedStorageData chunked = chunkByteData(data_span);

    ASSERT_EQ(chunked.chunks.size(), 2u);

    for (std::size_t chunk_index = 0; chunk_index < chunked.chunks.size(); ++chunk_index) {
        const std::span<const std::byte> chunk_span = chunkSpan(chunked, chunk_index);
        const auto &[offset, length] = chunked.chunks[chunk_index];
        EXPECT_EQ(chunk_span.size(), length);
        EXPECT_EQ(chunk_span.data(), chunked.storage.data() + offset);
        for (std::size_t byte_index = 0; byte_index < chunk_span.size(); ++byte_index) {
            EXPECT_EQ(chunk_span[byte_index], chunked.storage[offset + byte_index]);
        }
    }
}

TEST(Chunker, ChunkFile_MatchesByteData) {
    constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 99;
    const std::vector<std::byte> original_data = make_patterned_data(total_size, 3);
    const TempFile temp_file(original_data);

    const std::span data_span(original_data.data(), original_data.size());
    const auto [storage, chunks] = chunkByteData(data_span);
    const auto [from_storage, from_chunks] = chunkFile(temp_file.path_cstr());

    EXPECT_EQ(storage, from_storage);
    ASSERT_EQ(chunks.size(), from_chunks.size());

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(chunks[i].offset, from_chunks[i].offset);
        EXPECT_EQ(chunks[i].length, from_chunks[i].length);
    }
}

TEST(Chunker, FileChunkReader_EmptyFile) {
    const TempFile temp_file({});
    const FileChunkReader reader(temp_file.path_cstr());

    EXPECT_EQ(reader.num_chunks(), 1u);
    EXPECT_EQ(reader.file_size(), 0u);
    EXPECT_EQ(reader.chunk_size(), CHUNK_SIZE_BYTES);
}

TEST(Chunker, FileChunkReader_SingleByteFile) {
    const std::vector<std::byte> one_byte = {std::byte{0xAB}};
    const TempFile temp_file(one_byte);
    const FileChunkReader reader(temp_file.path_cstr());

    EXPECT_EQ(reader.num_chunks(), 1u);
    EXPECT_EQ(reader.file_size(), 1u);
}

TEST(Chunker, FileChunkReader_ExactChunkSizeFile) {
    const std::vector<std::byte> exact_data(CHUNK_SIZE_BYTES);
    const TempFile temp_file(exact_data);
    const FileChunkReader reader(temp_file.path_cstr());

    EXPECT_EQ(reader.num_chunks(), 1u);
    EXPECT_EQ(reader.file_size(), CHUNK_SIZE_BYTES);
}

TEST(Chunker, FileChunkReader_TwoChunkFile) {
    const std::vector<std::byte> over_data(CHUNK_SIZE_BYTES + 1);
    const TempFile temp_file(over_data);
    const FileChunkReader reader(temp_file.path_cstr());

    EXPECT_EQ(reader.num_chunks(), 2u);
    EXPECT_EQ(reader.file_size(), CHUNK_SIZE_BYTES + 1);
}

TEST(Chunker, FileChunkReader_ReadChunkContents) {
    constexpr std::size_t total_size = CHUNK_SIZE_BYTES + 50;
    const std::vector<std::byte> original_data = make_patterned_data(total_size, 11);
    const TempFile temp_file(original_data);
    const FileChunkReader reader(temp_file.path_cstr());

    ASSERT_EQ(reader.num_chunks(), 2u);

    const std::vector<std::byte> first_chunk = reader.read_chunk(0);
    const std::vector<std::byte> second_chunk = reader.read_chunk(1);

    ASSERT_EQ(first_chunk.size(), CHUNK_SIZE_BYTES);
    ASSERT_EQ(second_chunk.size(), 50u);

    for (std::size_t i = 0; i < CHUNK_SIZE_BYTES; ++i) {
        EXPECT_EQ(first_chunk[i], original_data[i]);
    }
    for (std::size_t i = 0; i < 50u; ++i) {
        EXPECT_EQ(second_chunk[i], original_data[CHUNK_SIZE_BYTES + i]);
    }
}

TEST(Chunker, FileChunkReader_CustomChunkSize) {
    constexpr std::size_t custom_chunk_size = 100;
    constexpr std::size_t total_size = 250;
    const std::vector<std::byte> original_data = make_patterned_data(total_size);
    const TempFile temp_file(original_data);
    const FileChunkReader reader(temp_file.path_cstr(), custom_chunk_size);

    EXPECT_EQ(reader.chunk_size(), custom_chunk_size);
    EXPECT_EQ(reader.num_chunks(), 3u);

    const std::vector<std::byte> first_chunk = reader.read_chunk(0);
    const std::vector<std::byte> second_chunk = reader.read_chunk(1);
    const std::vector<std::byte> third_chunk = reader.read_chunk(2);

    ASSERT_EQ(first_chunk.size(), 100u);
    ASSERT_EQ(second_chunk.size(), 100u);
    ASSERT_EQ(third_chunk.size(), 50u);

    for (std::size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(first_chunk[i], original_data[i]);
        EXPECT_EQ(second_chunk[i], original_data[100 + i]);
    }
    for (std::size_t i = 0; i < 50; ++i) {
        EXPECT_EQ(third_chunk[i], original_data[200 + i]);
    }
}

TEST(Chunker, FileChunkReader_OutOfRangeThrows) {
    const std::vector<std::byte> small_data(10);
    const TempFile temp_file(small_data);
    const FileChunkReader reader(temp_file.path_cstr());

    const std::size_t invalid_index = reader.num_chunks();
    EXPECT_THROW(static_cast<void>(reader.read_chunk(invalid_index)), std::runtime_error);
}
