// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) Brandon Li <https://brandonli.me/>
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

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    // Set application properties
    QApplication::setApplicationName("YouTube Media Storage");
    QApplication::setApplicationDisplayName("Drive Manager");
    QApplication::setApplicationVersion("1.0");
    QApplication::setOrganizationName("Media Storage");
    QApplication::setOrganizationDomain("brandonli.me");
    
    // Set application icon (if available)
    // app.setWindowIcon(QIcon(":/icons/app_icon.png"));
    
    // Enable high DPI scaling (deprecated in Qt6, but kept for compatibility)
    // app.setAttribute(Qt::AA_EnableHighDpiScaling);
    // app.setAttribute(Qt::AA_UseHighDpiPixmaps);
    
    // Set style to a modern look if available
    if (QStyleFactory::keys().contains("Fusion")) {
        QApplication::setStyle("Fusion");
    }
    
    // Create and show the main window
    DriveManagerUI window;
    window.show();
    
    return QApplication::exec();
}
