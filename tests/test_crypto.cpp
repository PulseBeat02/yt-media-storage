#include <gtest/gtest.h>

#include "crypto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

std::array<std::byte, 16> make_test_file_id() {
    std::array<std::byte, 16> file_id{};
    for (std::size_t i = 0; i < 16; ++i) {
        file_id[i] = static_cast<std::byte>(i);
    }
    return file_id;
}

std::array<std::byte, 16> make_salt(const uint8_t base) {
    std::array<std::byte, 16> salt{};
    for (std::size_t i = 0; i < 16; ++i) {
        salt[i] = static_cast<std::byte>(base + i);
    }
    return salt;
}

std::array<std::byte, CRYPTO_KEY_BYTES> make_test_key() {
    static constexpr std::byte password[] = {
        std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'},
        std::byte{'p'}, std::byte{'a'}, std::byte{'s'}, std::byte{'s'},
    };
    const std::array<std::byte, 16> salt = make_salt(0x10);
    return derive_key(std::span(password), salt);
}

std::vector<std::byte> make_patterned_data(const std::size_t byte_count, const uint8_t multiplier = 1) {
    std::vector<std::byte> data(byte_count);
    for (std::size_t i = 0; i < byte_count; ++i) {
        data[i] = static_cast<std::byte>(static_cast<uint8_t>((i * multiplier) & 0xFF));
    }
    return data;
}

} // namespace

TEST(Crypto, DeriveKey_SameInputProducesSameKey) {
    static constexpr std::byte password[] = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const std::array<std::byte, 16> salt = make_salt(0x00);

    const auto first_key = derive_key(std::span(password), salt);
    const auto second_key = derive_key(std::span(password), salt);

    EXPECT_EQ(first_key, second_key);
}

TEST(Crypto, DeriveKey_DifferentPasswordsProduceDifferentKeys) {
    static constexpr std::byte password_a[] = {std::byte{'a'}};
    static constexpr std::byte password_b[] = {std::byte{'b'}};
    const std::array<std::byte, 16> salt = make_salt(0x20);

    const auto key_a = derive_key(std::span(password_a), salt);
    const auto key_b = derive_key(std::span(password_b), salt);

    EXPECT_NE(key_a, key_b);
}

TEST(Crypto, DeriveKey_DifferentSaltsProduceDifferentKeys) {
    static constexpr std::byte password[] = {std::byte{'x'}, std::byte{'y'}};
    const std::array<std::byte, 16> first_salt = make_salt(0x00);
    const std::array<std::byte, 16> second_salt = make_salt(0x80);

    const auto first_key = derive_key(std::span(password), first_salt);
    const auto second_key = derive_key(std::span(password), second_salt);

    EXPECT_NE(first_key, second_key);
}

TEST(Crypto, EncryptDecrypt_BasicRoundtrip) {
    const auto key = make_test_key();
    const auto file_id = make_test_file_id();
    const std::vector<std::byte> plaintext = make_patterned_data(64, 3);

    const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);
    const std::vector<std::byte> decrypted = decrypt_chunk(ciphertext, key, file_id, 0u);

    EXPECT_EQ(decrypted, plaintext);
}

TEST(Crypto, EncryptDecrypt_SingleByte) {
    const auto key = make_test_key();
    const auto file_id = make_test_file_id();
    const std::vector plaintext = {std::byte{0x42}};

    const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);
    const std::vector<std::byte> decrypted = decrypt_chunk(ciphertext, key, file_id, 0u);

    EXPECT_EQ(decrypted, plaintext);
}

TEST(Crypto, EncryptDecrypt_LargeData) {
    const auto key = make_test_key();
    const auto file_id = make_test_file_id();
    constexpr std::size_t large_size = 100u * 1024u;
    const std::vector<std::byte> plaintext = make_patterned_data(large_size);
    constexpr uint32_t chunk_index = 7u;

    const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, chunk_index);
    const std::vector<std::byte> decrypted = decrypt_chunk(ciphertext, key, file_id, chunk_index);

    EXPECT_EQ(decrypted, plaintext);
}

TEST(Crypto, DecryptChunkInto_MatchesDecryptChunk) {
    const auto key = make_test_key();
    const auto file_id = make_test_file_id();
    const std::vector<std::byte> plaintext = make_patterned_data(128, 0xA0);
    constexpr uint32_t chunk_index = 3u;

    const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, chunk_index);

    std::vector<std::byte> output_buffer(plaintext.size());
    decrypt_chunk_into(output_buffer, ciphertext, key, file_id, chunk_index);

    EXPECT_EQ(output_buffer, plaintext);
}

TEST(Crypto, EncryptedSize_IncludesOverhead) {
    const auto key = make_test_key();
    const auto file_id = make_test_file_id();
    const std::vector<std::byte> plaintext = make_patterned_data(500);

    const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);

    constexpr std::size_t expected_overhead = CRYPTO_PLAIN_SIZE_HEADER + 16u;
    EXPECT_EQ(ciphertext.size(), plaintext.size() + expected_overhead);
}

TEST(Crypto, Decrypt_WrongKeyThrows) {
    const auto key = make_test_key();
    const auto file_id = make_test_file_id();
    const std::vector plaintext = {std::byte{1}, std::byte{2}, std::byte{3}};

    const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);

    std::array<std::byte, CRYPTO_KEY_BYTES> wrong_key = key;
    wrong_key[0] ^= std::byte{1};

    EXPECT_THROW(decrypt_chunk(ciphertext, wrong_key, file_id, 0u), std::runtime_error);
}

TEST(Crypto, Decrypt_WrongChunkIndexThrows) {
    const auto key = make_test_key();
    const auto file_id = make_test_file_id();
    const std::vector<std::byte> plaintext = make_patterned_data(32);
    constexpr uint32_t correct_index = 5u;
    constexpr uint32_t wrong_index = 6u;

    const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, correct_index);

    EXPECT_THROW(decrypt_chunk(ciphertext, key, file_id, wrong_index), std::runtime_error);
}

TEST(Crypto, ReadPlainSizeFromHeader_ReadsCorrectValue) {
    std::array<std::byte, 4> header_bytes{};
    constexpr uint32_t expected_size = 0xDEADBEEFu;
    header_bytes[0] = static_cast<std::byte>(expected_size & 0xFFu);
    header_bytes[1] = static_cast<std::byte>((expected_size >> 8) & 0xFFu);
    header_bytes[2] = static_cast<std::byte>((expected_size >> 16) & 0xFFu);
    header_bytes[3] = static_cast<std::byte>((expected_size >> 24) & 0xFFu);

    const uint32_t actual_size = read_plain_size_from_header(header_bytes);

    EXPECT_EQ(actual_size, expected_size);
}

TEST(Crypto, SecureZero_ClearsEntireBuffer) {
    std::vector<std::byte> buffer(64);
    for (auto &byte_val : buffer) {
        byte_val = std::byte{0xFF};
    }

    secure_zero(buffer);

    for (const auto byte_val : buffer) {
        EXPECT_EQ(byte_val, std::byte{0});
    }
}
