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

file(GLOB_RECURSE SRC_CPP CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

file(GLOB_RECURSE SRC_HDR CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

file(GLOB_RECURSE LIB_CPP CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/*.cpp"
)

file(GLOB_RECURSE LIB_HDR CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/*.hpp"
)

list(REMOVE_ITEM SRC_CPP "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
list(REMOVE_ITEM SRC_CPP "${CMAKE_CURRENT_SOURCE_DIR}/drive_manager_ui.cpp")
list(REMOVE_ITEM SRC_CPP "${CMAKE_CURRENT_SOURCE_DIR}/main_gui.cpp")
list(REMOVE_ITEM SRC_CPP "${CMAKE_CURRENT_SOURCE_DIR}/media_storage_api.cpp")

target_sources(media_storage_core PRIVATE
        ${SRC_CPP} ${SRC_HDR}
        ${LIB_CPP} ${LIB_HDR}
)

target_include_directories(media_storage_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs"
)

target_sources(media_storage_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/media_storage_api.cpp"
)

target_sources(media_storage PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)

target_sources(media_storage_gui PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/drive_manager_ui.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/drive_manager_ui.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/main_gui.cpp"
)

target_include_directories(media_storage_gui PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
