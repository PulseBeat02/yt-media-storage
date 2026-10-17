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

#ifndef MEDIA_STORAGE_GRAY_LOOKUP_H
#define MEDIA_STORAGE_GRAY_LOOKUP_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

constexpr std::size_t GRAY_LEVELS = 256;

using gray_lookup_table = std::array<uint8_t, GRAY_LEVELS>;

// empty unless the scaler turns every luma byte into one gray value, whatever its neighbours and chroma
std::optional<gray_lookup_table> calibrate_gray_lookup_table(SwsContext* context, AVPixelFormat source_format,
                                                             int width, int height);

#endif
