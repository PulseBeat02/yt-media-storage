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
        test_stream.cpp
        test_dct.cpp
        test_api.cpp
)

target_link_libraries(media_storage_tests PRIVATE
        media_storage_core
        media_storage_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(media_storage_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)

if (WIN32)
    add_custom_command(TARGET media_storage_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:media_storage_tests>
            $<TARGET_FILE_DIR:media_storage_tests>
            COMMAND_EXPAND_LISTS
    )
endif ()

include(GoogleTest)
gtest_discover_tests(media_storage_tests)
