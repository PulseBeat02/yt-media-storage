# This file is part of yt-media-storage, a tool for encoding media.
# Copyright (C) 2026 Brandon Li <https://brandonli.me/>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

add_executable(media_storage_tests
        test_integrity.cpp
        test_chunker.cpp
        test_codec.cpp
        test_crypto.cpp
        test_roundtrip.cpp
)

target_link_libraries(media_storage_tests PRIVATE
        media_storage_core
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(media_storage_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)

include(GoogleTest)
gtest_discover_tests(media_storage_tests)
