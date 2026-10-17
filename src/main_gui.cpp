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

#include <QApplication>
#include <QStyleFactory>

#include "drive_manager_ui.h"
#include "media_storage.h"

namespace {
constexpr const char* APPLICATION_NAME = "YouTube Media Storage";
constexpr const char* APPLICATION_DISPLAY_NAME = "Drive Manager";
constexpr const char* ORGANIZATION_NAME = "Media Storage";
constexpr const char* ORGANIZATION_DOMAIN = "brandonli.me";

constexpr const char* STYLE_NAME = "Fusion";
}

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);

  QApplication::setApplicationName(APPLICATION_NAME);
  QApplication::setApplicationDisplayName(APPLICATION_DISPLAY_NAME);
  QApplication::setApplicationVersion(ms_version());
  QApplication::setOrganizationName(ORGANIZATION_NAME);
  QApplication::setOrganizationDomain(ORGANIZATION_DOMAIN);

  if (QStyleFactory::keys().contains(STYLE_NAME)) {
    QApplication::setStyle(STYLE_NAME);
  }

  drive_manager_ui window;
  window.show();

  return QApplication::exec();
}
