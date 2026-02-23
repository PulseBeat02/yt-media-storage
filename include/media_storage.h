/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) Brandon Li <https://brandonli.me/>
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

#ifndef MEDIA_STORAGE_H
#define MEDIA_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#   ifdef MEDIA_STORAGE_BUILD_SHARED
#       define MS_API __declspec(dllexport)
#   elif defined(MEDIA_STORAGE_SHARED)
#       define MS_API __declspec(dllimport)
#   else
#       define MS_API
#   endif
#else
#   if defined(MEDIA_STORAGE_BUILD_SHARED) && defined(__GNUC__)
#       define MS_API __attribute__((visibility("default")))
#   else
#       define MS_API
#   endif
#endif

typedef enum {
    MS_OK = 0,
    MS_ERR_INVALID_ARGS = 1,
    MS_ERR_FILE_NOT_FOUND = 2,
    MS_ERR_IO = 3,
    MS_ERR_ENCODE_FAILED = 4,
    MS_ERR_DECODE_FAILED = 5,
    MS_ERR_CRYPTO = 6,
    MS_ERR_INCOMPLETE = 7,
} ms_status_t;

typedef enum {
    MS_HASH_CRC32 = 0,
    MS_HASH_XXHASH32 = 1,
} ms_hash_algorithm_t;

/**
 * Progress callback invoked during encode/decode.
 *
 * @param current  Current step (e.g. chunk index or frame index).
 * @param total    Total steps (0 if unknown).
 * @param user     User-supplied context pointer.
 * @return         0 to continue, non-zero to cancel.
 */
typedef int (*ms_progress_fn)(uint64_t current, uint64_t total, void *user);

typedef struct {
    const char *input_path;
    const char *output_path;

    int encrypt;
    const char *password;
    size_t password_len;

    ms_hash_algorithm_t hash_algorithm;

    ms_progress_fn progress;
    void *progress_user;
} ms_encode_options_t;

typedef struct {
    const char *input_path;
    const char *output_path;

    const char *password;
    size_t password_len;

    ms_progress_fn progress;
    void *progress_user;
} ms_decode_options_t;

typedef struct {
    uint64_t input_size;
    uint64_t output_size;
    uint64_t total_chunks;
    uint64_t total_packets;
    uint64_t total_frames;
} ms_result_t;

/**
 * Encode a file into a lossless video.
 *
 * @param options  Encoding parameters (input/output paths, encryption, etc.).
 * @param result   Optional pointer to receive statistics about the operation.
 * @return         MS_OK on success, or an error code.
 */
MS_API ms_status_t ms_encode(const ms_encode_options_t *options, ms_result_t *result);

/**
 * Decode a video back into the original file.
 *
 * @param options  Decoding parameters (input/output paths, password, etc.).
 * @param result   Optional pointer to receive statistics about the operation.
 * @return         MS_OK on success, or an error code.
 */
MS_API ms_status_t ms_decode(const ms_decode_options_t *options, ms_result_t *result);

/**
 * Return a human-readable string for the given status code.
 * The returned pointer is valid for the lifetime of the program.
 */
MS_API const char *ms_status_string(ms_status_t status);

/**
 * Return the library version string (e.g. "1.0.0").
 */
MS_API const char *ms_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_STORAGE_H */
