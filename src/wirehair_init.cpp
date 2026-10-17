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

#include "wirehair_init.h"

#include "libs/wirehair/wirehair.h"

#include <mutex>
#include <stdexcept>
#include <string>

void ensure_wirehair_initialized() {
  static std::once_flag init_flag;
  std::call_once(init_flag, [] {
    if (wirehair_init() != Wirehair_Success) {
      const std::string message = "Wirehair could not be initialized!";
      throw std::runtime_error(message);
    }
  });
}
