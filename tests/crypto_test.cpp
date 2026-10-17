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

#include "crypto.h"
#include "fixtures.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t PLAINTEXT_SIZE = 64;

file_id_type make_test_file_id() {
  file_id_type file_id{};
  for (std::size_t i = 0; i < file_id.size(); ++i) {
    file_id[i] = static_cast<std::byte>(i);
  }
  return file_id;
}

file_id_type make_salt(const uint8_t base) {
  file_id_type salt{};
  for (std::size_t i = 0; i < salt.size(); ++i) {
    salt[i] = static_cast<std::byte>(base + i);
  }
  return salt;
}

std::array<std::byte, CRYPTO_KEY_BYTES> make_test_key() {
  static constexpr std::byte password[] = {
      std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'},
      std::byte{'p'}, std::byte{'a'}, std::byte{'s'}, std::byte{'s'},
  };
  const file_id_type salt = make_salt(0x10);
  return derive_key(std::span(password), salt);
}

std::vector<std::byte> make_patterned_data(const std::size_t byte_count, const uint8_t multiplier = 1) {
  std::vector<std::byte> data(byte_count);
  for (std::size_t i = 0; i < byte_count; ++i) {
    data[i] = static_cast<std::byte>(static_cast<uint8_t>((i * multiplier) & fixtures::BYTE_MASK));
  }
  return data;
}

}

TEST(crypto_test, should_generate_distinct_file_ids) {
  const file_id_type first = generate_file_id();
  const file_id_type second = generate_file_id();
  EXPECT_NE(first, second);
  EXPECT_NE(first, file_id_type{});
}

TEST(crypto_test, should_derive_the_same_key_from_the_same_password_and_salt) {
  static constexpr std::byte password[] = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  const file_id_type salt = make_salt(0x00);

  const auto first_key = derive_key(std::span(password), salt);
  const auto second_key = derive_key(std::span(password), salt);

  EXPECT_EQ(first_key, second_key);
}

TEST(crypto_test, should_derive_different_keys_from_different_passwords) {
  static constexpr std::byte password_a[] = {std::byte{'a'}};
  static constexpr std::byte password_b[] = {std::byte{'b'}};
  const file_id_type salt = make_salt(0x20);

  const auto key_a = derive_key(std::span(password_a), salt);
  const auto key_b = derive_key(std::span(password_b), salt);

  EXPECT_NE(key_a, key_b);
}

TEST(crypto_test, should_derive_different_keys_from_different_salts) {
  static constexpr std::byte password[] = {std::byte{'x'}, std::byte{'y'}};
  const file_id_type first_salt = make_salt(0x00);
  const file_id_type second_salt = make_salt(0x80);

  const auto first_key = derive_key(std::span(password), first_salt);
  const auto second_key = derive_key(std::span(password), second_salt);

  EXPECT_NE(first_key, second_key);
}

TEST(crypto_test, should_decrypt_what_it_encrypted) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> plaintext = make_patterned_data(PLAINTEXT_SIZE, 3);

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);
  const std::vector<std::byte> decrypted = decrypt_chunk(ciphertext, key, file_id, 0u);

  EXPECT_EQ(decrypted, plaintext);
}

TEST(crypto_test, should_decrypt_a_single_encrypted_byte) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector plaintext = {std::byte{0x42}};

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);
  const std::vector<std::byte> decrypted = decrypt_chunk(ciphertext, key, file_id, 0u);

  EXPECT_EQ(decrypted, plaintext);
}

TEST(crypto_test, should_decrypt_a_large_encrypted_chunk) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  constexpr std::size_t large_size = 100u * 1024u;
  const std::vector<std::byte> plaintext = make_patterned_data(large_size);
  constexpr uint32_t chunk_index = 7u;

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, chunk_index);
  const std::vector<std::byte> decrypted = decrypt_chunk(ciphertext, key, file_id, chunk_index);

  EXPECT_EQ(decrypted, plaintext);
}

TEST(crypto_test, should_decrypt_in_place_like_a_copying_decryption) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> plaintext = make_patterned_data(128, 0xA0);
  constexpr uint32_t chunk_index = 3u;

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, chunk_index);

  std::vector<std::byte> output_buffer(plaintext.size());
  decrypt_chunk_into(output_buffer, ciphertext, key, file_id, chunk_index);

  EXPECT_EQ(output_buffer, plaintext);
}

TEST(crypto_test, should_add_the_size_header_and_the_tag_to_the_ciphertext) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> plaintext = make_patterned_data(500);

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);

  constexpr std::size_t expected_overhead = CRYPTO_PLAIN_SIZE_HEADER + 16u;
  const std::size_t ciphertext_size = ciphertext.size();
  const std::size_t expected_ciphertext_size = plaintext.size() + expected_overhead;
  EXPECT_EQ(ciphertext_size, expected_ciphertext_size);
}

TEST(crypto_test, should_reject_the_wrong_key) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector plaintext = {std::byte{1}, std::byte{2}, std::byte{3}};

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);

  std::array<std::byte, CRYPTO_KEY_BYTES> wrong_key = key;
  wrong_key[0] ^= std::byte{1};

  EXPECT_THROW(static_cast<void>(decrypt_chunk(ciphertext, wrong_key, file_id, 0u)), std::runtime_error);
}

TEST(crypto_test, should_reject_the_wrong_chunk_index) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> plaintext = make_patterned_data(32);
  constexpr uint32_t correct_index = 5u;
  constexpr uint32_t wrong_index = 6u;

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, correct_index);

  EXPECT_THROW(static_cast<void>(decrypt_chunk(ciphertext, key, file_id, wrong_index)), std::runtime_error);
}

TEST(crypto_test, should_read_the_plain_size_from_the_header) {
  std::array<std::byte, CRYPTO_PLAIN_SIZE_HEADER> header_bytes{};
  constexpr uint32_t expected_size = 0xDEADBEEFu;
  for (std::size_t i = 0; i < header_bytes.size(); ++i) {
    header_bytes[i] = static_cast<std::byte>((expected_size >> (i * CHAR_BIT)) & fixtures::BYTE_MASK);
  }

  const uint32_t actual_size = read_plain_size_from_header(header_bytes);

  EXPECT_EQ(actual_size, expected_size);
}

TEST(crypto_test, should_zero_the_whole_buffer) {
  std::vector<std::byte> buffer(64);
  for (auto& byte_value : buffer) {
    byte_value = std::byte{0xFF};
  }

  secure_zero(buffer);

  for (const auto byte_value : buffer) {
    EXPECT_EQ(byte_value, std::byte{0});
  }
}

TEST(crypto_test, should_decrypt_an_empty_plaintext) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> empty;

  const std::vector<std::byte> ciphertext = encrypt_chunk(empty, key, file_id, 0u);
  const std::vector<std::byte> decrypted = decrypt_chunk(ciphertext, key, file_id, 0u);

  EXPECT_EQ(decrypted, empty);
}

TEST(crypto_test, should_reject_a_chunk_too_small_for_a_size_header) {
  const std::vector<std::byte> tiny = {std::byte{0}, std::byte{1}};
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();

  EXPECT_THROW(static_cast<void>(decrypt_chunk(tiny, key, file_id, 0u)), std::runtime_error);
}

TEST(crypto_test, should_reject_an_output_too_small_for_the_plaintext) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> plaintext = make_patterned_data(PLAINTEXT_SIZE);

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);

  std::vector<std::byte> too_small(1);
  EXPECT_THROW(decrypt_chunk_into(too_small, ciphertext, key, file_id, 0u), std::runtime_error);
}

TEST(crypto_test, should_hide_the_plaintext_in_the_ciphertext) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> plaintext = make_patterned_data(256);

  const std::vector<std::byte> ciphertext = encrypt_chunk(plaintext, key, file_id, 0u);

  const std::vector<std::byte> cipher_body(ciphertext.begin() + CRYPTO_PLAIN_SIZE_HEADER, ciphertext.end());
  const std::size_t prefix_size = std::min(plaintext.size(), cipher_body.size());
  const std::vector<std::byte> plaintext_prefix(plaintext.begin(),
                                                plaintext.begin() + static_cast<std::ptrdiff_t>(prefix_size));
  EXPECT_NE(cipher_body, plaintext_prefix);
}

TEST(crypto_test, should_encrypt_each_chunk_index_differently) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> plaintext = make_patterned_data(PLAINTEXT_SIZE);

  const std::vector<std::byte> ciphertext0 = encrypt_chunk(plaintext, key, file_id, 0u);
  const std::vector<std::byte> ciphertext1 = encrypt_chunk(plaintext, key, file_id, 1u);

  EXPECT_NE(ciphertext0, ciphertext1);
}

namespace {

template <typename Action> std::string find_runtime_error_message(Action action) {
  try {
    action();
  } catch (const std::runtime_error& exception) {
    return exception.what();
  }

  return {};
}

}

TEST(crypto_test, should_reject_a_chunk_without_a_size_header_when_decrypting_in_place) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  const std::vector<std::byte> chunk(CRYPTO_PLAIN_SIZE_HEADER - 1);
  std::vector<std::byte> out(CHUNK_SIZE_BYTES);

  const std::string message = find_runtime_error_message([&] { decrypt_chunk_into(out, chunk, key, file_id, 0u); });

  EXPECT_EQ(message, "Chunk is too small to decrypt!");
}

TEST(crypto_test, should_reject_a_size_header_larger_than_a_chunk) {
  const auto key = make_test_key();
  const auto file_id = make_test_file_id();
  std::vector<std::byte> ciphertext = encrypt_chunk(make_patterned_data(PLAINTEXT_SIZE), key, file_id, 0u);
  const auto oversized = static_cast<uint32_t>(CHUNK_SIZE_BYTES + 1);
  std::memcpy(ciphertext.data(), &oversized, sizeof(oversized));

  const std::string message =
      find_runtime_error_message([&] { static_cast<void>(decrypt_chunk(ciphertext, key, file_id, 0u)); });

  EXPECT_EQ(message, "Chunk could not be decrypted, because the password is wrong or the data is corrupted!");
}
