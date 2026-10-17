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

#ifndef MEDIA_STORAGE_H
#define MEDIA_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef MEDIA_STORAGE_BUILD_SHARED
#define MS_API __declspec(dllexport)
#elif defined(MEDIA_STORAGE_SHARED)
#define MS_API __declspec(dllimport)
#else
#define MS_API
#endif
#else
#if defined(MEDIA_STORAGE_BUILD_SHARED) && defined(__GNUC__)
#define MS_API __attribute__((visibility("default")))
#else
#define MS_API
#endif
#endif

#define MS_VERSION_MAJOR 1
#define MS_VERSION_MINOR 0
#define MS_VERSION_PATCH 0
#define MS_VERSION_STRING "1.0.0"

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
 * A callback that receives the progress of an operation and can cancel it.
 *
 * ms_encode() and ms_stream_encode() call it before each batch of chunks with the index of the first chunk of the
 * batch and the chunk count. ms_decode() and ms_stream_decode() call it before each frame with the frames read so far
 * and the frame count of the input, or 0 when the input does not tell it.
 *
 * @param current the step reached so far, a chunk index or a frame count
 * @param total   the number of steps, or 0 when unknown
 * @param user    the progress_user pointer of the options
 * @return 0 to continue, or any other value to cancel the operation
 */
typedef int (*ms_progress_fn)(uint64_t current, uint64_t total, void* user);

typedef struct {
  const char* input_path;
  const char* output_path;

  int encrypt;
  const char* password;
  size_t password_len;

  ms_hash_algorithm_t hash_algorithm;

  ms_progress_fn progress;
  void* progress_user;
} ms_encode_options_t;

typedef struct {
  const char* input_path;
  const char* output_path;

  const char* password;
  size_t password_len;

  ms_progress_fn progress;
  void* progress_user;
} ms_decode_options_t;

typedef struct {
  const char* input_path;
  const char* stream_url;

  int encrypt;
  const char* password;
  size_t password_len;

  ms_hash_algorithm_t hash_algorithm;
  int bitrate_kbps;
  int width;
  int height;
  int fps;

  ms_progress_fn progress;
  void* progress_user;
} ms_stream_encode_options_t;

typedef struct {
  const char* stream_url;
  const char* output_path;

  const char* password;
  size_t password_len;

  int timeout_sec;

  ms_progress_fn progress;
  void* progress_user;
} ms_stream_decode_options_t;

typedef struct {
  uint64_t input_size;
  uint64_t output_size;
  uint64_t total_chunks;
  uint64_t total_packets;
  uint64_t total_frames;
} ms_result_t;

/**
 * Encodes a file into a lossless video.
 *
 * Fills result only on success. On failure, ms_last_error() describes what went wrong. A non-zero return of the
 * progress callback cancels the encode with MS_ERR_ENCODE_FAILED.
 *
 * @param options the input and output paths, the encryption, the hash algorithm and the progress callback
 * @param result  receives the statistics of the encode, or NULL when they are not needed
 * @return MS_OK, MS_ERR_INVALID_ARGS when options or a path is NULL, encrypt is set without a password or the input is
 *         not a regular file, MS_ERR_FILE_NOT_FOUND when the input does not exist, MS_ERR_IO when the input cannot be
 *         memory-mapped, or MS_ERR_ENCODE_FAILED when encoding fails or the progress callback cancels it
 */
MS_API ms_status_t ms_encode(const ms_encode_options_t* options, ms_result_t* result);

/**
 * Decodes a video made by ms_encode() back into the original file.
 *
 * The password is needed only when the file was encrypted. Fills result only on success. On failure,
 * ms_last_error() describes what went wrong. A non-zero return of the progress callback cancels the decode with
 * MS_ERR_DECODE_FAILED.
 *
 * @param options the input and output paths, the password and the progress callback
 * @param result  receives the statistics of the decode, or NULL when they are not needed
 * @return MS_OK, MS_ERR_INVALID_ARGS when options or a path is NULL, MS_ERR_FILE_NOT_FOUND when the input does not
 *         exist, MS_ERR_DECODE_FAILED when the video cannot be read, holds no packets or the progress callback
 *         cancels the decode, MS_ERR_INCOMPLETE when chunks are missing, MS_ERR_CRYPTO when the file is encrypted and
 *         no password is given or decrypting or writing it fails, or MS_ERR_IO when the output of an unencrypted
 *         file cannot be written
 */
MS_API ms_status_t ms_decode(const ms_decode_options_t* options, ms_result_t* result);

/**
 * Encodes a file and streams it live to a URL, such as the RTMP ingest of Twitch or YouTube.
 *
 * A bitrate_kbps, width, height or fps of 0 or less takes the default: 35000 kbps, 1920 by 1080 pixels and 30 frames
 * per second. Fills result only on success, with an output_size of 0. On failure, ms_last_error() describes what went
 * wrong. A non-zero return of the progress callback cancels the stream with MS_ERR_ENCODE_FAILED.
 *
 * @param options the input path, the stream URL, the encryption, the hash algorithm, the video settings and the
 *                progress callback
 * @param result  receives the statistics of the stream, or NULL when they are not needed
 * @return MS_OK, MS_ERR_INVALID_ARGS when options, the input path or the URL is NULL, encrypt is set without a
 *         password or the input is not a regular file, MS_ERR_FILE_NOT_FOUND when the input does not exist, MS_ERR_IO
 *         when the input cannot be memory-mapped, or MS_ERR_ENCODE_FAILED when encoding or streaming fails or the
 *         progress callback cancels it
 */
MS_API ms_status_t ms_stream_encode(const ms_stream_encode_options_t* options, ms_result_t* result);

/**
 * Decodes a live stream made by ms_stream_encode() back into the original file.
 *
 * Tries to connect once a second, timeout_sec times in all, or 30 times when timeout_sec is 0 or less. The password is
 * needed only when the file was encrypted. Fills result only on success, with an input_size of 0. On failure,
 * ms_last_error() describes what went wrong. A non-zero return of the progress callback cancels the decode with
 * MS_ERR_DECODE_FAILED.
 *
 * @param options the stream URL, the output path, the password, the connect timeout and the progress callback
 * @param result  receives the statistics of the decode, or NULL when they are not needed
 * @return MS_OK, MS_ERR_INVALID_ARGS when options, the URL or the output path is NULL, MS_ERR_DECODE_FAILED when the
 *         stream cannot be opened or read, holds no packets or the progress callback cancels the decode,
 *         MS_ERR_INCOMPLETE when chunks are missing, MS_ERR_CRYPTO when the file is encrypted and no password is
 *         given or decrypting or writing it fails, or MS_ERR_IO when the output of an unencrypted file cannot be
 *         written
 */
MS_API ms_status_t ms_stream_decode(const ms_stream_decode_options_t* options, ms_result_t* result);

/**
 * Returns a short English description of a status code.
 *
 * @param status the status code to describe
 * @return a static string that stays valid for the life of the program, "unknown error" for a value outside
 *         ms_status_t
 */
MS_API const char* ms_status_string(ms_status_t status);

/**
 * Returns what went wrong in the last ms_encode(), ms_decode(), ms_stream_encode() or ms_stream_decode() call on the
 * calling thread.
 *
 * Each of those calls clears the message when it starts, so the message is empty after a call that succeeded.
 *
 * @return the message, valid until the next of those calls on the same thread
 */
MS_API const char* ms_last_error(void);

/**
 * Returns the version of the library, such as "1.0.0".
 *
 * @return MS_VERSION_STRING, a static string
 */
MS_API const char* ms_version(void);

#ifdef __cplusplus
}
#endif

#endif
