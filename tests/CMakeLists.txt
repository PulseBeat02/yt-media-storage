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
        cli_test.cpp
        video_test.cpp
        api_decode_test.cpp
        api_encode_test.cpp
)

target_link_libraries(media_storage_tests PRIVATE
        media_storage_core
        media_storage_lib
        GTest::gtest
        GTest::gtest_main
)

target_compile_definitions(media_storage_tests PRIVATE MEDIA_STORAGE_CLI_PATH="$<TARGET_FILE:media_storage>")
add_dependencies(media_storage_tests media_storage)

if (WIN32)
    add_custom_command(TARGET media_storage_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:media_storage_tests>
            $<TARGET_FILE_DIR:media_storage_tests>
            COMMAND_EXPAND_LISTS
    )
endif ()

include(GoogleTest)
gtest_discover_tests(media_storage_tests DISCOVERY_TIMEOUT 60)

if (BUILD_GUI)
    add_executable(media_storage_gui_tests
            drive_manager_ui_test.cpp
            ${PROJECT_SOURCE_DIR}/src/drive_manager_ui.cpp
            ${PROJECT_SOURCE_DIR}/src/drive_manager_ui.h
    )

    target_include_directories(media_storage_gui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

    target_link_libraries(media_storage_gui_tests PRIVATE
            media_storage_core
            media_storage_lib
            Qt6::Core
            Qt6::Widgets
            GTest::gtest
    )

    set_target_properties(media_storage_gui_tests PROPERTIES AUTOMOC ON)

    if (WIN32)
        add_custom_command(TARGET media_storage_gui_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:media_storage_gui_tests>
                $<TARGET_FILE_DIR:media_storage_gui_tests>
                COMMAND_EXPAND_LISTS
        )
    endif ()

    gtest_discover_tests(media_storage_gui_tests DISCOVERY_TIMEOUT 60)

    add_executable(media_storage_gui_main_tests
            main_gui_test.cpp
            ${PROJECT_SOURCE_DIR}/src/main_gui.cpp
            ${PROJECT_SOURCE_DIR}/src/drive_manager_ui.cpp
            ${PROJECT_SOURCE_DIR}/src/drive_manager_ui.h
    )

    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/main_gui.cpp PROPERTIES
            COMPILE_DEFINITIONS main=run_media_storage_gui
    )

    target_include_directories(media_storage_gui_main_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

    target_link_libraries(media_storage_gui_main_tests PRIVATE
            media_storage_core
            media_storage_lib
            Qt6::Core
            Qt6::Widgets
            GTest::gtest
            GTest::gtest_main
    )

    set_target_properties(media_storage_gui_main_tests PROPERTIES AUTOMOC ON)

    if (WIN32)
        add_custom_command(TARGET media_storage_gui_main_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:media_storage_gui_main_tests>
                $<TARGET_FILE_DIR:media_storage_gui_main_tests>
                COMMAND_EXPAND_LISTS
        )
    endif ()

    gtest_discover_tests(media_storage_gui_main_tests DISCOVERY_TIMEOUT 60)
endif ()
