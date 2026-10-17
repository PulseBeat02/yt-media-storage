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

#include "crypto.h"

#include <climits>
#include <cstring>
#include <mutex>
#include <sodium.h>
#include <stdexcept>
#include <string>

static_assert(FILE_ID_SIZE == crypto_pwhash_SALTBYTES);
static_assert(CRYPTO_KEY_BYTES == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
static_assert(CRYPTO_AEAD_TAG_BYTES == crypto_aead_xchacha20poly1305_ietf_ABYTES);
static_assert(FILE_ID_SIZE + sizeof(uint32_t) <= crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(CRYPTO_PLAIN_SIZE_HEADER == sizeof(uint32_t));

namespace {
constexpr const char* CHUNK_TOO_SMALL_MESSAGE = "Chunk is too small to decrypt!";
constexpr const char* DECRYPTION_FAILURE_MESSAGE =
    "Chunk could not be decrypted, because the password is wrong or the data is corrupted!";

using nonce = std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES>;

void ensure_sodium_initialized() {
  static std::once_flag initialization_flag;
  std::call_once(initialization_flag, [] {
    if (sodium_init() < 0) {
      const std::string message = "Libsodium could not be initialized!";
      throw std::runtime_error(message);
    }
  });
}

nonce build_nonce(const std::span<const std::byte, FILE_ID_SIZE> file_id, const uint32_t chunk_index) {
  nonce nonce{};
  std::memcpy(nonce.data(), file_id.data(), FILE_ID_SIZE);
  for (std::size_t i = 0; i < sizeof(chunk_index); ++i) {
    nonce[FILE_ID_SIZE + i] = static_cast<unsigned char>(chunk_index >> (i * CHAR_BIT));
  }
  return nonce;
}

void write_plain_size_header(const std::span<std::byte> out, const uint32_t plain_size) {
  for (std::size_t i = 0; i < sizeof(plain_size); ++i) {
    out[i] = static_cast<std::byte>(plain_size >> (i * CHAR_BIT));
  }
}
}

file_id_type generate_file_id() {
  ensure_sodium_initialized();
  file_id_type id{};
  randombytes_buf(id.data(), id.size());
  return id;
}

uint32_t read_plain_size_from_header(const std::span<const std::byte> chunk) {
  uint32_t plain_size = 0;
  for (std::size_t i = 0; i < sizeof(plain_size); ++i) {
    plain_size |= static_cast<uint32_t>(static_cast<uint8_t>(chunk[i])) << (i * CHAR_BIT);
  }
  return plain_size;
}

crypto_key derive_key(const std::span<const std::byte> password, const std::span<const std::byte, FILE_ID_SIZE> salt) {
  ensure_sodium_initialized();

  crypto_key key{};
  if (crypto_pwhash(reinterpret_cast<unsigned char*>(key.data()), key.size(),
                    reinterpret_cast<const char*>(password.data()), password.size(),
                    reinterpret_cast<const unsigned char*>(salt.data()), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                    crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) != 0) {
    const std::string message = "Key could not be derived!";
    throw std::runtime_error(message);
  }
  return key;
}

std::vector<std::byte> encrypt_chunk(const std::span<const std::byte> plain,
                                     const std::span<const std::byte, CRYPTO_KEY_BYTES> key,
                                     const std::span<const std::byte, FILE_ID_SIZE> file_id,
                                     const uint32_t chunk_index) {
  ensure_sodium_initialized();

  const nonce nonce = build_nonce(file_id, chunk_index);
  const std::size_t cipher_length = plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES;
  std::vector<std::byte> result(CRYPTO_PLAIN_SIZE_HEADER + cipher_length);
  write_plain_size_header(result, static_cast<uint32_t>(plain.size()));

  unsigned long long written = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          reinterpret_cast<unsigned char*>(result.data() + CRYPTO_PLAIN_SIZE_HEADER), &written,
          reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nullptr, 0, nullptr, nonce.data(),
          reinterpret_cast<const unsigned char*>(key.data())) != 0) {
    const std::string message = "Chunk could not be encrypted!";
    throw std::runtime_error(message);
  }

  result.resize(CRYPTO_PLAIN_SIZE_HEADER + static_cast<std::size_t>(written));
  return result;
}

void decrypt_chunk_into(const std::span<std::byte> out, const std::span<const std::byte> chunk_from_decoder,
                        const std::span<const std::byte, CRYPTO_KEY_BYTES> key,
                        const std::span<const std::byte, FILE_ID_SIZE> file_id, const uint32_t chunk_index) {
  ensure_sodium_initialized();

  if (chunk_from_decoder.size() < CRYPTO_PLAIN_SIZE_HEADER) {
    throw std::runtime_error(CHUNK_TOO_SMALL_MESSAGE);
  }

  const uint32_t plain_size = read_plain_size_from_header(chunk_from_decoder);
  const std::size_t cipher_length = plain_size + crypto_aead_xchacha20poly1305_ietf_ABYTES;
  if (plain_size > CHUNK_SIZE_BYTES || chunk_from_decoder.size() < CRYPTO_PLAIN_SIZE_HEADER + cipher_length ||
      out.size() < plain_size) {
    throw std::runtime_error(DECRYPTION_FAILURE_MESSAGE);
  }

  const nonce nonce = build_nonce(file_id, chunk_index);
  const auto cipher = chunk_from_decoder.subspan(CRYPTO_PLAIN_SIZE_HEADER, cipher_length);

  unsigned long long written = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(out.data()), &written, nullptr,
                                                 reinterpret_cast<const unsigned char*>(cipher.data()), cipher.size(),
                                                 nullptr, 0, nonce.data(),
                                                 reinterpret_cast<const unsigned char*>(key.data())) != 0) {
    throw std::runtime_error(DECRYPTION_FAILURE_MESSAGE);
  }

  if (written != plain_size) {
    const std::string message = "Decrypted size does not match the chunk header!";
    throw std::runtime_error(message);
  }
}

std::vector<std::byte> decrypt_chunk(const std::span<const std::byte> chunk_from_decoder,
                                     const std::span<const std::byte, CRYPTO_KEY_BYTES> key,
                                     const std::span<const std::byte, FILE_ID_SIZE> file_id,
                                     const uint32_t chunk_index) {
  if (chunk_from_decoder.size() < CRYPTO_PLAIN_SIZE_HEADER) {
    throw std::runtime_error(CHUNK_TOO_SMALL_MESSAGE);
  }

  const uint32_t plain_size = read_plain_size_from_header(chunk_from_decoder);
  if (plain_size > CHUNK_SIZE_BYTES) {
    throw std::runtime_error(DECRYPTION_FAILURE_MESSAGE);
  }

  std::vector<std::byte> plain(plain_size);
  decrypt_chunk_into(plain, chunk_from_decoder, key, file_id, chunk_index);
  return plain;
}

void secure_zero(const std::span<std::byte> data) {
  if (!data.empty()) {
    sodium_memzero(data.data(), data.size());
  }
}
