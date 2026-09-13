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

#include "decoder.h"

#include "libs/wirehair/wirehair.h"
#include "wirehair_init.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <stdexcept>

void ChunkDecoder::CodecDeleter::operator()(void *codec) const {
    wirehair_free(static_cast<WirehairCodec>(codec));
}

ChunkDecoder::ChunkDecoder(const uint32_t chunk_index, const uint32_t chunk_size, const uint32_t k,
                           const uint16_t symbol_size)
    : chunk_index_(chunk_index)
      , chunk_size_(chunk_size)
      , k_(k)
      , symbol_size_(symbol_size) {
    ensure_wirehair_initialized();
    codec_.reset(wirehair_decoder_create(nullptr, chunk_size_, symbol_size_));
    if (!codec_) {
        throw std::runtime_error("wirehair_decoder_create failed");
    }
}

bool ChunkDecoder::add_packet(const uint32_t esi, const std::span<const std::byte> payload) {
    if (decoded_) {
        return true;
    }
    if (!codec_) {
        throw std::runtime_error("codec is null");
    }

    ++packets_received_;

    const auto codec = static_cast<WirehairCodec>(codec_.get());
    const WirehairResult result = wirehair_decode(codec, esi, payload.data(),
                                                  static_cast<uint32_t>(payload.size()));
    if (result == Wirehair_NeedMore) {
        return false;
    }
    if (result != Wirehair_Success) {
        throw std::runtime_error("wirehair_decode failed with error");
    }

    decoded_data_.resize(chunk_size_);
    if (wirehair_recover(codec, decoded_data_.data(), chunk_size_) != Wirehair_Success) {
        throw std::runtime_error("wirehair_recover failed");
    }
    decoded_ = true;
    return true;
}

std::vector<std::byte> ChunkDecoder::get_decoded_data() const {
    if (!decoded_) {
        throw std::runtime_error("data not yet decoded");
    }
    return decoded_data_;
}

std::vector<std::byte> ChunkDecoder::consume_decoded_data() {
    if (!decoded_) {
        throw std::runtime_error("data not yet decoded");
    }
    return std::move(decoded_data_);
}

std::optional<ChunkDecodeResult> Decoder::process_packet(const std::span<const std::byte> packet_data,
                                                         const bool compute_sha256) {
    const auto parsed = parse_and_verify_packet(packet_data);
    if (!parsed) {
        ++total_packets_;
        return std::nullopt;
    }
    return process_verified_packet(*parsed, compute_sha256);
}

std::optional<ChunkDecodeResult> Decoder::process_verified_packet(const ParsedPacket &packet,
                                                                  const bool compute_sha256) {
    ++total_packets_;

    const PacketHeader &hdr = packet.header;
    if (!file_id_) {
        file_id_ = hdr.file_id;
        encrypted_ = (hdr.flags & FLAG_ENCRYPTED) != 0;
    }

    max_chunk_index_ = std::max(max_chunk_index_, hdr.chunk_index);
    if (hdr.flags & FLAG_LAST_CHUNK) {
        last_chunk_index_ = hdr.chunk_index;
    }

    if (is_chunk_complete(hdr.chunk_index)) {
        return std::nullopt;
    }

    if (hdr.chunk_index >= active_decoders_.size()) {
        active_decoders_.resize(hdr.chunk_index + 1);
    }
    std::optional<ChunkDecoder> &slot = active_decoders_[hdr.chunk_index];
    if (!slot) {
        slot.emplace(hdr.chunk_index, hdr.chunk_size, hdr.k, hdr.symbol_size);
    }

    if (!slot->add_packet(hdr.esi, packet.payload)) {
        return std::nullopt;
    }

    ChunkDecodeResult result;
    result.chunk_index = hdr.chunk_index;
    result.data = slot->consume_decoded_data();
    result.data.resize(std::min(static_cast<uint32_t>(result.data.size()), hdr.original_size));
    if (compute_sha256) {
        result.sha256 = sha256(result.data);
    }
    result.success = true;

    if (hdr.chunk_index >= completed_chunks_.size()) {
        completed_chunks_.resize(hdr.chunk_index + 1);
    }
    completed_chunks_[hdr.chunk_index] = result.data;
    ++completed_count_;
    slot.reset();

    return result;
}

bool Decoder::is_chunk_complete(const uint32_t chunk_index) const {
    return chunk_index < completed_chunks_.size() && completed_chunks_[chunk_index].has_value();
}

std::optional<std::vector<std::byte> > Decoder::get_chunk_data(const uint32_t chunk_index) const {
    if (is_chunk_complete(chunk_index)) {
        return *completed_chunks_[chunk_index];
    }
    return std::nullopt;
}

std::vector<uint32_t> Decoder::completed_chunk_indices() const {
    std::vector<uint32_t> indices;
    indices.reserve(completed_count_);
    for (uint32_t i = 0; i < completed_chunks_.size(); ++i) {
        if (completed_chunks_[i].has_value()) {
            indices.push_back(i);
        }
    }
    return indices;
}

void Decoder::set_decrypt_key(const std::span<const std::byte, CRYPTO_KEY_BYTES> key) {
    std::memcpy(decrypt_key_.data(), key.data(), CRYPTO_KEY_BYTES);
    decrypt_key_set_ = true;
}

void Decoder::clear_decrypt_key() {
    if (decrypt_key_set_) {
        secure_zero(decrypt_key_);
        decrypt_key_set_ = false;
    }
}

bool Decoder::ready_to_assemble(const uint32_t expected_chunks) const {
    if (completed_count_ != expected_chunks || completed_chunks_.size() > expected_chunks) {
        return false;
    }
    if (!file_id_ || (encrypted_ && !decrypt_key_set_)) {
        return false;
    }
    for (uint32_t i = 0; i < expected_chunks; ++i) {
        if (!is_chunk_complete(i)) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<std::size_t> > Decoder::plain_chunk_sizes(const uint32_t expected_chunks) const {
    if (!ready_to_assemble(expected_chunks)) {
        return std::nullopt;
    }

    std::vector<std::size_t> sizes(expected_chunks);
    for (uint32_t i = 0; i < expected_chunks; ++i) {
        const auto &chunk = *completed_chunks_[i];
        if (!encrypted_) {
            sizes[i] = chunk.size();
            continue;
        }
        if (chunk.size() < CRYPTO_PLAIN_SIZE_HEADER) {
            return std::nullopt;
        }
        sizes[i] = read_plain_size_from_header(chunk);
        if (sizes[i] > CHUNK_SIZE_BYTES) {
            return std::nullopt;
        }
    }
    return sizes;
}

bool Decoder::decrypt_chunk_to(const uint32_t chunk_index, const std::span<std::byte> out) const {
    try {
        decrypt_chunk_into(out, *completed_chunks_[chunk_index], decrypt_key_, *file_id_, chunk_index);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::vector<std::byte> > Decoder::assemble_file(const uint32_t expected_chunks) const {
    const auto sizes = plain_chunk_sizes(expected_chunks);
    if (!sizes) {
        return std::nullopt;
    }

    std::vector<std::size_t> offsets(expected_chunks + 1, 0);
    for (uint32_t i = 0; i < expected_chunks; ++i) {
        offsets[i + 1] = offsets[i] + (*sizes)[i];
    }

    std::vector<std::byte> result(offsets[expected_chunks]);
    std::atomic<bool> failed{false};

#pragma omp parallel for schedule(static)
    for (int i = 0; i < static_cast<int>(expected_chunks); ++i) {
        const std::span<std::byte> out(result.data() + offsets[i], (*sizes)[i]);
        if (encrypted_) {
            if (!decrypt_chunk_to(static_cast<uint32_t>(i), out)) {
                failed = true;
            }
        } else {
            std::memcpy(out.data(), completed_chunks_[i]->data(), out.size());
        }
    }

    if (failed) {
        return std::nullopt;
    }
    return result;
}

bool Decoder::write_assembled_file(const std::string &output_path, const uint32_t expected_chunks) const {
    const auto sizes = plain_chunk_sizes(expected_chunks);
    if (!sizes) {
        return false;
    }

    std::vector<std::vector<std::byte> > decrypted;
    if (encrypted_) {
        decrypted.resize(expected_chunks);
        std::atomic<bool> failed{false};

#pragma omp parallel for schedule(static)
        for (int i = 0; i < static_cast<int>(expected_chunks); ++i) {
            decrypted[i].resize((*sizes)[i]);
            if (!decrypt_chunk_to(static_cast<uint32_t>(i), decrypted[i])) {
                failed = true;
            }
        }

        if (failed) {
            return false;
        }
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        return false;
    }

    for (uint32_t i = 0; i < expected_chunks; ++i) {
        const std::vector<std::byte> &chunk = encrypted_ ? decrypted[i] : *completed_chunks_[i];
        out.write(reinterpret_cast<const char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        if (!out.good()) {
            return false;
        }
    }
    return true;
}
