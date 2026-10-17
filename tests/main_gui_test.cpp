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

#include <gtest/gtest.h>

#include <QApplication>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QWidget>

#include <array>
#include <string>

#include "media_storage.h"

int run_media_storage_gui(int argument_count, char* arguments[]);

namespace {

constexpr const char* PLATFORM_VARIABLE = "QT_QPA_PLATFORM";
constexpr const char* HEADLESS_PLATFORM = "offscreen";

constexpr const char* PROGRAM_NAME = "media_storage_gui";
constexpr int ARGUMENT_COUNT = 1;

constexpr const char* APPLICATION_NAME = "YouTube Media Storage";
constexpr const char* DISPLAY_NAME = "Drive Manager";
constexpr const char* ORGANIZATION_NAME = "Media Storage";
constexpr const char* ORGANIZATION_DOMAIN = "brandonli.me";
constexpr const char* STYLE_NAME = "Fusion";
constexpr const char* WINDOW_TITLE = "YouTube Media Storage - Drive Manager";

constexpr int SUCCESS_EXIT_CODE = 0;

struct observed_application {
  QString application_name;
  QString display_name;
  QString version;
  QString organization_name;
  QString organization_domain;
  QString style_name;
  QStringList visible_window_titles;
};

observed_application observed;

QStringList find_visible_window_titles() {
  QStringList titles;
  for (const QWidget* const widget : QApplication::topLevelWidgets()) {
    if (widget->isVisible()) {
      titles.append(widget->windowTitle());
    }
  }

  return titles;
}

void inspect_and_close_windows() {
  const QStyle* const style = QApplication::style();
  observed.application_name = QApplication::applicationName();
  observed.display_name = QApplication::applicationDisplayName();
  observed.version = QApplication::applicationVersion();
  observed.organization_name = QApplication::organizationName();
  observed.organization_domain = QApplication::organizationDomain();
  observed.style_name = style->name();
  observed.visible_window_titles = find_visible_window_titles();

  QApplication::closeAllWindows();
}

void schedule_inspection() {
  QTimer::singleShot(0, inspect_and_close_windows);
}

}

TEST(main_gui_test, should_show_the_drive_manager_and_exit_when_its_window_closes) {
  if (qEnvironmentVariableIsEmpty(PLATFORM_VARIABLE)) {
    qputenv(PLATFORM_VARIABLE, HEADLESS_PLATFORM);
  }

  QStandardPaths::setTestModeEnabled(true);
  qAddPreRoutine(schedule_inspection);
  std::string program_name = PROGRAM_NAME;
  std::array<char*, ARGUMENT_COUNT + 1> arguments = {program_name.data(), nullptr};
  int argument_count = ARGUMENT_COUNT;

  const int exit_code = run_media_storage_gui(argument_count, arguments.data());

  const QStringList expected_titles = {WINDOW_TITLE};

  const QString expected_version = QString::fromUtf8(ms_version());
  const int style_comparison = observed.style_name.compare(STYLE_NAME, Qt::CaseInsensitive);
  EXPECT_EQ(exit_code, SUCCESS_EXIT_CODE);
  EXPECT_EQ(observed.application_name, APPLICATION_NAME);
  EXPECT_EQ(observed.display_name, DISPLAY_NAME);
  EXPECT_EQ(observed.version, expected_version);
  EXPECT_EQ(observed.organization_name, ORGANIZATION_NAME);
  EXPECT_EQ(observed.organization_domain, ORGANIZATION_DOMAIN);
  EXPECT_EQ(style_comparison, 0);
  EXPECT_EQ(observed.visible_window_titles, expected_titles);
}
