# This file is part of yt-media-storage, a tool for encoding media.
# Copyright (C) Brandon Li <https://brandonli.me/>
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

cmake_minimum_required(VERSION 3.22)
project(media_storage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenMP REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# Enable Qt MOC, UIC, and RCC for GUI only
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

set(CORE_LINK_LIBS
        PkgConfig::AVCODEC
        PkgConfig::AVFORMAT
        PkgConfig::AVUTIL
        PkgConfig::SWSCALE
        PkgConfig::SWRESAMPLE
        PkgConfig::SODIUM
        OpenMP::OpenMP_CXX
)

add_library(media_storage_core STATIC)
set_target_properties(media_storage_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(media_storage_lib SHARED)
target_compile_definitions(media_storage_lib PRIVATE MEDIA_STORAGE_BUILD_SHARED)

add_executable(media_storage)
add_executable(media_storage_gui)
add_subdirectory(src)

target_link_libraries(media_storage_core PUBLIC ${CORE_LINK_LIBS})

target_link_libraries(media_storage_lib PRIVATE media_storage_core)
target_include_directories(media_storage_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(media_storage_lib PROPERTIES
        OUTPUT_NAME media_storage
        VERSION 1.0.0
        SOVERSION 1
)

target_link_libraries(media_storage PRIVATE media_storage_lib)
target_link_libraries(media_storage_gui PRIVATE
        media_storage_lib
        Qt6::Core
        Qt6::Widgets
)

set_target_properties(media_storage_gui PROPERTIES AUTOMOC ON AUTOUIC ON)
if (MSVC)
    target_compile_options(media_storage_core PRIVATE
            $<$<CONFIG:Release>:/O2>
            /arch:AVX2
    )
    target_compile_options(media_storage_gui PRIVATE
            $<$<CONFIG:Release>:/O2>
            /arch:AVX2
    )
else ()
    target_compile_options(media_storage_core PRIVATE -march=native)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i386")
        target_compile_options(media_storage_gui PRIVATE -O2 -mssse3)
    else ()
        target_compile_options(media_storage_gui PRIVATE -O2 -march=native)
    endif ()
endif ()

include(GNUInstallDirs)

install(TARGETS media_storage_lib
        EXPORT media_storage_targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/media_storage.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT media_storage_targets
        FILE media_storage-config.cmake
        NAMESPACE media_storage::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/media_storage
)
