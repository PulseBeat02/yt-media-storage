// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) Brandon Li <https://brandonli.me/>
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
#include "configuration.h"

#include <sodium.h>
#include <mutex>
#include <stdexcept>

static std::once_flag sodium_init_flag;

static void ensure_sodium_init() {
    std::call_once(sodium_init_flag, [] {
        if (sodium_init() < 0) {
            throw std::runtime_error("sodium_init failed");
        }
    });
}

uint32_t read_plain_size_from_header(const std::span<const std::byte> chunk) {
    uint32_t plain_size = 0;
    plain_size |= static_cast<uint32_t>(static_cast<uint8_t>(chunk[0]));
    plain_size |= static_cast<uint32_t>(static_cast<uint8_t>(chunk[1])) << 8;
    plain_size |= static_cast<uint32_t>(static_cast<uint8_t>(chunk[2])) << 16;
    plain_size |= static_cast<uint32_t>(static_cast<uint8_t>(chunk[3])) << 24;
    return plain_size;
}

std::array<std::byte, CRYPTO_KEY_BYTES> derive_key(
    const std::span<const std::byte> password,
    const std::span<const std::byte, 16> salt) {
    ensure_sodium_init();

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (crypto_pwhash(
            reinterpret_cast<unsigned char *>(key.data()),
            key.size(),
            reinterpret_cast<const char *>(password.data()),
            password.size(),
            reinterpret_cast<const unsigned char *>(salt.data()),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw std::runtime_error("Key derivation failed");
    }
    return key;
}

static void build_nonce(
    std::span<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce,
    const std::span<const std::byte, 16> file_id,
    const uint32_t chunk_index) {
    std::memcpy(nonce.data(), file_id.data(), 16);
    nonce[16] = static_cast<unsigned char>(chunk_index & 0xff);
    nonce[17] = static_cast<unsigned char>((chunk_index >> 8) & 0xff);
    nonce[18] = static_cast<unsigned char>((chunk_index >> 16) & 0xff);
    nonce[19] = static_cast<unsigned char>((chunk_index >> 24) & 0xff);
    std::memset(nonce.data() + 20, 0, 4);
}

std::vector<std::byte> encrypt_chunk(
    const std::span<const std::byte> plain,
    const std::span<const std::byte, CRYPTO_KEY_BYTES> key,
    const std::span<const std::byte, 16> file_id,
    const uint32_t chunk_index) {
    ensure_sodium_init();

    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    build_nonce(nonce, file_id, chunk_index);

    const std::size_t cipher_len = plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    std::vector<std::byte> result(CRYPTO_PLAIN_SIZE_HEADER + cipher_len);

    const auto plain_size_le = static_cast<uint32_t>(plain.size());
    result[0] = static_cast<std::byte>(plain_size_le & 0xff);
    result[1] = static_cast<std::byte>((plain_size_le >> 8) & 0xff);
    result[2] = static_cast<std::byte>((plain_size_le >> 16) & 0xff);
    result[3] = static_cast<std::byte>((plain_size_le >> 24) & 0xff);

    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(result.data() + CRYPTO_PLAIN_SIZE_HEADER),
            &written,
            reinterpret_cast<const unsigned char *>(plain.data()),
            plain.size(),
            nullptr,
            0,
            nullptr,
            nonce.data(),
            reinterpret_cast<const unsigned char *>(key.data())) != 0) {
        throw std::runtime_error("Encryption failed");
    }

    result.resize(CRYPTO_PLAIN_SIZE_HEADER + static_cast<std::size_t>(written));
    return result;
}

std::vector<std::byte> decrypt_chunk(
    const std::span<const std::byte> chunk_from_decoder,
    const std::span<const std::byte, CRYPTO_KEY_BYTES> key,
    const std::span<const std::byte, 16> file_id,
    const uint32_t chunk_index) {
    ensure_sodium_init();

    if (chunk_from_decoder.size() < CRYPTO_PLAIN_SIZE_HEADER) {
        throw std::runtime_error("Decryption failed (chunk too small)");
    }

    const uint32_t plain_size = read_plain_size_from_header(chunk_from_decoder);
    const std::size_t cipher_len = plain_size + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    if (plain_size > CHUNK_SIZE_BYTES ||
        chunk_from_decoder.size() < CRYPTO_PLAIN_SIZE_HEADER + cipher_len) {
        throw std::runtime_error("Decryption failed (wrong password or corrupted data)");
    }

    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    build_nonce(nonce, file_id, chunk_index);

    std::vector<std::byte> plain(cipher_len);

    unsigned long long written = 0;
    const auto cipher_span = chunk_from_decoder.subspan(CRYPTO_PLAIN_SIZE_HEADER, cipher_len);
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plain.data()),
            &written,
            nullptr,
            reinterpret_cast<const unsigned char *>(cipher_span.data()),
            cipher_span.size(),
            nullptr,
            0,
            nonce.data(),
            reinterpret_cast<const unsigned char *>(key.data())) != 0) {
        throw std::runtime_error("Decryption failed (wrong password or corrupted data)");
    }

    plain.resize(static_cast<std::size_t>(written));
    return plain;
}

void decrypt_chunk_into(std::span<std::byte> out,
                        const std::span<const std::byte> chunk_from_decoder,
                        const std::span<const std::byte, CRYPTO_KEY_BYTES> key,
                        const std::span<const std::byte, 16> file_id,
                        const uint32_t chunk_index) {
    ensure_sodium_init();

    if (chunk_from_decoder.size() < CRYPTO_PLAIN_SIZE_HEADER) {
        throw std::runtime_error("Decryption failed (chunk too small)");
    }

    const uint32_t plain_size = read_plain_size_from_header(chunk_from_decoder);
    const std::size_t cipher_len = plain_size + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    if (plain_size > CHUNK_SIZE_BYTES ||
        chunk_from_decoder.size() < CRYPTO_PLAIN_SIZE_HEADER + cipher_len ||
        out.size() < plain_size) {
        throw std::runtime_error("Decryption failed (wrong password or corrupted data)");
    }

    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    build_nonce(nonce, file_id, chunk_index);

    unsigned long long written = 0;
    const auto cipher_span = chunk_from_decoder.subspan(CRYPTO_PLAIN_SIZE_HEADER, cipher_len);
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(out.data()),
            &written,
            nullptr,
            reinterpret_cast<const unsigned char *>(cipher_span.data()),
            cipher_span.size(),
            nullptr,
            0,
            nonce.data(),
            reinterpret_cast<const unsigned char *>(key.data())) != 0) {
        throw std::runtime_error("Decryption failed (wrong password or corrupted data)");
    }

    if (written != plain_size) {
        throw std::runtime_error("Decryption failed (size mismatch)");
    }
}

void secure_zero(std::span<std::byte> data) {
    if (!data.empty()) {
        sodium_memzero(data.data(), data.size());
    }
}
