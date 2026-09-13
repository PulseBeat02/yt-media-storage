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

cmake_minimum_required(VERSION 3.22)
project(media_storage VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build test suite" OFF)
option(MEDIA_STORAGE_STATIC_BINARIES "Build fully static executables" OFF)
option(MEDIA_STORAGE_NATIVE_ARCH "Optimize for the build machine's CPU (binaries may not run on older CPUs)" ON)
option(BUILD_GUI "Build the Qt GUI executable" ON)

find_package(PkgConfig REQUIRED)
find_package(OpenMP REQUIRED)
if (BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
endif ()

pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

if (MSVC)
    add_compile_options(/utf-8)
endif ()

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

if (MEDIA_STORAGE_STATIC_BINARIES)
    add_library(media_storage_lib STATIC)
else ()
    add_library(media_storage_lib SHARED)
    target_compile_definitions(media_storage_lib
            PRIVATE MEDIA_STORAGE_BUILD_SHARED
            INTERFACE MEDIA_STORAGE_SHARED
    )
endif ()

add_executable(media_storage)
if (BUILD_GUI)
    add_executable(media_storage_gui)
endif ()
add_subdirectory(src)

target_link_libraries(media_storage_core PUBLIC ${CORE_LINK_LIBS})

target_link_libraries(media_storage_lib PRIVATE $<BUILD_INTERFACE:media_storage_core>)
target_include_directories(media_storage_lib PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(media_storage_lib PROPERTIES
        OUTPUT_NAME media_storage
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_link_libraries(media_storage PRIVATE media_storage_lib)
if (BUILD_GUI)
    target_link_libraries(media_storage_gui PRIVATE
            media_storage_lib
            Qt6::Core
            Qt6::Widgets
    )
    set_target_properties(media_storage_gui PROPERTIES AUTOMOC ON)
endif ()

if (MEDIA_STORAGE_STATIC_BINARIES)
    if (NOT MSVC AND NOT APPLE)
        target_link_options(media_storage PRIVATE -static-libgcc -static-libstdc++)
        if (BUILD_GUI)
            target_link_options(media_storage_gui PRIVATE -static-libgcc -static-libstdc++)
        endif ()
    endif ()
    if (MSVC)
        set(_msvc_static_targets media_storage_core media_storage_lib media_storage)
        if (BUILD_GUI)
            list(APPEND _msvc_static_targets media_storage_gui)
        endif ()
        set_property(TARGET ${_msvc_static_targets}
                PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif ()
    if (BUILD_GUI AND COMMAND qt_finalize_executable)
        qt_finalize_executable(media_storage_gui)
    endif ()
endif ()

if (MEDIA_STORAGE_NATIVE_ARCH)
    if (MSVC)
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86|x86|i[36]86")
            target_compile_options(media_storage_core PRIVATE /arch:AVX2)
        endif ()
    else ()
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-march=native MEDIA_STORAGE_HAS_MARCH_NATIVE)
        if (MEDIA_STORAGE_HAS_MARCH_NATIVE)
            target_compile_options(media_storage_core PRIVATE -march=native)
        endif ()
    endif ()
endif ()

if (BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if (NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
                googletest
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG v1.15.2
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif ()
    add_subdirectory(tests)
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
