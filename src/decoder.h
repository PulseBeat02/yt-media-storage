/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "configuration.h"
#include "crypto.h"
#include "integrity.h"
#include "packet_format.h"

struct ChunkDecodeResult {
  uint32_t chunk_index = 0;
  std::vector<std::byte> data;
  Sha256Digest sha256{};
  bool success = false;
};

class ChunkDecoder {
public:
  explicit ChunkDecoder(uint32_t chunk_index, uint32_t chunk_size, uint32_t k, uint16_t symbol_size);

  ChunkDecoder(ChunkDecoder&&) noexcept = default;

  ChunkDecoder& operator=(ChunkDecoder&&) noexcept = default;

  [[nodiscard]] bool add_packet(uint32_t esi, std::span<const std::byte> payload);

  [[nodiscard]] bool is_complete() const {
    return decoded_;
  }

  [[nodiscard]] std::vector<std::byte> get_decoded_data() const;

  [[nodiscard]] std::vector<std::byte> consume_decoded_data();

  [[nodiscard]] uint32_t chunk_index() const {
    return chunk_index_;
  }

  [[nodiscard]] uint32_t num_source_symbols() const {
    return k_;
  }

  [[nodiscard]] uint32_t packets_received() const {
    return packets_received_;
  }

private:
  struct CodecDeleter {
    void operator()(void* codec) const;
  };

  uint32_t chunk_index_;
  uint32_t chunk_size_;
  uint32_t k_;
  uint16_t symbol_size_;
  std::unique_ptr<void, CodecDeleter> codec_;
  bool decoded_ = false;
  uint32_t packets_received_ = 0;
  std::vector<std::byte> decoded_data_;
};

class Decoder {
public:
  using FileId = ::FileId;

  Decoder() = default;

  [[nodiscard]] std::optional<ChunkDecodeResult> process_packet(std::span<const std::byte> packet_data,
                                                                bool compute_sha256 = true);

  [[nodiscard]] std::optional<ChunkDecodeResult> process_verified_packet(const ParsedPacket& packet,
                                                                         bool compute_sha256 = true);

  [[nodiscard]] bool is_chunk_complete(uint32_t chunk_index) const;

  [[nodiscard]] std::optional<std::vector<std::byte>> get_chunk_data(uint32_t chunk_index) const;

  [[nodiscard]] std::optional<FileId> file_id() const {
    return file_id_;
  }

  [[nodiscard]] std::size_t total_packets_received() const {
    return total_packets_;
  }

  [[nodiscard]] std::size_t chunks_completed() const {
    return completed_count_;
  }

  [[nodiscard]] uint32_t max_chunk_index() const {
    return max_chunk_index_;
  }

  [[nodiscard]] std::optional<uint32_t> last_chunk_index() const {
    return last_chunk_index_;
  }

  [[nodiscard]] std::vector<uint32_t> completed_chunk_indices() const;

  [[nodiscard]] std::optional<std::vector<std::byte>> assemble_file(uint32_t expected_chunks) const;

  [[nodiscard]] bool write_assembled_file(const std::string& output_path, uint32_t expected_chunks) const;

  void set_decrypt_key(std::span<const std::byte, CRYPTO_KEY_BYTES> key);

  void clear_decrypt_key();

  [[nodiscard]] bool is_encrypted() const {
    return encrypted_;
  }

private:
  std::optional<FileId> file_id_;
  bool encrypted_ = false;
  CryptoKey decrypt_key_{};
  bool decrypt_key_set_ = false;
  std::vector<std::optional<ChunkDecoder>> active_decoders_;
  std::vector<std::optional<std::vector<std::byte>>> completed_chunks_;
  std::size_t completed_count_ = 0;
  std::size_t total_packets_ = 0;
  uint32_t max_chunk_index_ = 0;
  std::optional<uint32_t> last_chunk_index_;

  [[nodiscard]] bool ready_to_assemble(uint32_t expected_chunks) const;

  [[nodiscard]] std::optional<std::vector<std::size_t>> plain_chunk_sizes(uint32_t expected_chunks) const;

  [[nodiscard]] bool decrypt_chunk_to(uint32_t chunk_index, std::span<std::byte> out) const;
};
