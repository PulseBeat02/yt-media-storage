/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MEDIA_STORAGE_DRIVE_MANAGER_UI_H
#define MEDIA_STORAGE_DRIVE_MANAGER_UI_H

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTextEdit>
#include <QThread>

#include <atomic>
#include <cstdint>
#include <memory>

#include "configuration.h"
#include "media_storage.h"

inline constexpr int MAXIMUM_PROGRESS = 100;

struct worker_request {
  enum class operation_type { ENCODE, DECODE, STREAM_ENCODE, STREAM_DECODE };

  operation_type operation = operation_type::ENCODE;
  QString input_path;
  QString output_path;
  QString stream_url;
  bool encrypt = false;
  QString password;
  int bitrate_kilobits_per_second = FRAME_BITRATE;
  int width = FRAME_WIDTH_STREAM;
  int height = FRAME_HEIGHT_STREAM;
  int frames_per_second = FRAMES_PER_SECOND;
};

class worker_thread : public QThread {
  Q_OBJECT

public:
  explicit worker_thread(worker_request request, QObject* parent = nullptr);

  void request_cancel() {
    cancel_requested_ = true;
  }

signals:
  void progress_updated(int percentage);

  void status_updated(const QString& status);

  void operation_completed(bool success, const QString& message);

  void log_message(const QString& message);

protected:
  void run() override;

private:
  struct progress_range {
    int start;
    int span;
  };

  worker_request request_;
  std::atomic<bool> cancel_requested_{false};
  progress_range progress_range_{0, MAXIMUM_PROGRESS};

  static int progress_callback(uint64_t current, uint64_t total, void* user);

  void run_encode(const std::string& input, const std::string& output, const std::string& password);

  void run_decode(const std::string& input, const std::string& output, const std::string& password);

  void run_stream_encode(const std::string& input, const std::string& url, const std::string& password);

  void run_stream_decode(const std::string& url, const std::string& output, const std::string& password);

  void finish(ms_status_t status, const QString& success_message, const QString& failure_prefix);
};

class drive_manager_ui : public QMainWindow {
  Q_OBJECT

public:
  explicit drive_manager_ui(QWidget* parent = nullptr);

  ~drive_manager_ui() override;

private slots:
  void select_input_file();

  void select_output_file();

  void add_batch_files();

  void select_batch_output_directory();

  void start_encode();

  void start_decode();

  void start_batch_encode();

  void start_stream_encode();

  void start_stream_decode();

  void on_platform_changed(int index) const;

  void on_resolution_changed(int index) const;

  void clear_logs() const;

  void show_about();

  void on_operation_completed(bool success, const QString& message);

  void on_progress_updated(int percentage) const;

  void on_status_updated(const QString& status) const;

  void update_file_list() const;

  void remove_selected_files() const;

  void clear_file_list() const;

  void toggle_password_visibility() const;

private:
  void setup_ui();

  void setup_menu_bar();

  void setup_status_bar();

  void connect_signals();

  void start_worker(const worker_request& request, const QString& operation_name);

  void start_next_batch_item();

  void finish_batch();

  void set_operation_running(bool running);

  void reset_progress();

  void log_message(const QString& message) const;

  void load_settings();

  void save_settings() const;

  bool warn_if_busy();

  bool validate_input_file();

  bool validate_output_file();

  bool validate_password();

  QWidget* central_widget_ = nullptr;
  QSplitter* main_splitter_ = nullptr;

  QGroupBox* file_operations_group_ = nullptr;
  QLineEdit* input_file_edit_ = nullptr;
  QLineEdit* output_file_edit_ = nullptr;
  QPushButton* select_input_button_ = nullptr;
  QPushButton* select_output_button_ = nullptr;
  QCheckBox* encrypt_check_box_ = nullptr;
  QLineEdit* password_edit_ = nullptr;
  QPushButton* password_visibility_button_ = nullptr;
  QPushButton* encode_button_ = nullptr;
  QPushButton* decode_button_ = nullptr;

  QGroupBox* batch_group_ = nullptr;
  QListWidget* file_list_widget_ = nullptr;
  QPushButton* add_files_button_ = nullptr;
  QPushButton* remove_files_button_ = nullptr;
  QPushButton* clear_files_button_ = nullptr;
  QPushButton* batch_encode_button_ = nullptr;
  QLineEdit* batch_output_directory_edit_ = nullptr;
  QPushButton* batch_output_button_ = nullptr;

  QGroupBox* stream_group_ = nullptr;
  QComboBox* platform_combo_box_ = nullptr;
  QLineEdit* stream_url_edit_ = nullptr;
  QLineEdit* stream_key_edit_ = nullptr;
  QSpinBox* bitrate_spin_box_ = nullptr;
  QSpinBox* frames_per_second_spin_box_ = nullptr;
  QComboBox* resolution_combo_box_ = nullptr;
  QPushButton* stream_encode_button_ = nullptr;
  QPushButton* stream_decode_button_ = nullptr;

  QGroupBox* status_group_ = nullptr;
  QProgressBar* progress_bar_ = nullptr;
  QLabel* status_label_ = nullptr;
  QLabel* progress_label_ = nullptr;

  QGroupBox* logs_group_ = nullptr;
  QTextEdit* log_text_edit_ = nullptr;
  QPushButton* clear_logs_button_ = nullptr;

  QLabel* permanent_status_ = nullptr;

  std::unique_ptr<worker_thread> worker_thread_;

  bool operation_running_ = false;
  QString current_operation_;

  bool batch_running_ = false;
  QStringList batch_queue_;
  int batch_succeeded_ = 0;
  int batch_failed_ = 0;
};

#endif
