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

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "drive_manager_ui.h"
#include "fixtures.h"

// gtest finds PrintTo through argument-dependent lookup, so it lives beside QString
void PrintTo(const QString& value, std::ostream* stream) {
  *stream << value.toStdString();
}

namespace {

constexpr int DIALOG_POLL_MILLISECONDS = 10;
constexpr int OPERATION_TIMEOUT_MILLISECONDS = 120000;
constexpr int EVENT_WAIT_MILLISECONDS = 20;

constexpr std::size_t SMALL_INPUT_SIZE = 2000;
constexpr std::size_t MULTI_CHUNK_INPUT_SIZE = 3 * 1024 * 1024;

constexpr int TWITCH_INDEX = 0;
constexpr int YOUTUBE_INDEX = 1;
constexpr int CUSTOM_INDEX = 2;

constexpr int RESOLUTION_1080P_INDEX = 0;
constexpr int RESOLUTION_1440P_INDEX = 1;
constexpr int RESOLUTION_4K_INDEX = 2;

constexpr int BITRATE_1080P = 8000;
constexpr int BITRATE_1440P = 16000;
constexpr int BITRATE_4K = 35000;

constexpr int STREAM_WIDTH = 1280;
constexpr int STREAM_HEIGHT = 720;
constexpr int STREAM_FRAMES_PER_SECOND = 30;
constexpr int STREAM_BITRATE = 8000;

constexpr const char* PASSWORD = "gui-password";

constexpr const char* PLATFORM_VARIABLE = "QT_QPA_PLATFORM";
constexpr const char* HEADLESS_PLATFORM = "offscreen";
constexpr const char* TEST_ORGANIZATION = "yt-media-storage-tests";

constexpr int STARTING_PERCENTAGE = 5;
constexpr int COMPLETE_PERCENTAGE = 100;

struct shown_message {
  QString title;
  QString text;
};

class dialog_driver : public QObject {
public:
  dialog_driver() {
    connect(&timer_, &QTimer::timeout, this, &dialog_driver::answer_modal_widget);
    timer_.start(DIALOG_POLL_MILLISECONDS);
  }

  void queue_file_answer(const std::optional<QStringList>& files) {
    file_answers_.push_back(files);
  }

  [[nodiscard]] const std::vector<shown_message>& messages() const {
    return messages_;
  }

  [[nodiscard]] bool has_message(const QString& title, const QString& text) const {
    return std::ranges::any_of(
        messages_, [&](const shown_message& message) { return message.title == title && message.text.contains(text); });
  }

  void clear() {
    messages_.clear();
  }

private:
  void answer_modal_widget() {
    QWidget* const modal = QApplication::activeModalWidget();
    if (auto* const box = qobject_cast<QMessageBox*>(modal)) {
      answer_message_box(box);
      return;
    }

    if (auto* const dialog = qobject_cast<QFileDialog*>(modal)) {
      answer_file_dialog(dialog);
    }
  }

  void answer_message_box(QMessageBox* box) {
    messages_.push_back({box->windowTitle(), box->text()});
    box->done(QMessageBox::Ok);
  }

  void answer_file_dialog(QFileDialog* dialog) {
    if (file_answers_.empty() || !file_answers_.front()) {
      if (!file_answers_.empty()) {
        file_answers_.pop_front();
      }
      dialog->reject();
      return;
    }

    const QStringList files = *file_answers_.front();
    file_answers_.pop_front();
    if (QWidget* const focused = QApplication::focusWidget()) {
      focused->clearFocus();
    }
    dialog->setDirectory(QFileInfo(files.front()).absolutePath());
    dialog->selectFile(selection_text(files));
    QDialog* const base = dialog;
    base->accept();
  }

  static QString selection_text(const QStringList& files) {
    if (files.size() == 1) {
      return QFileInfo(files.front()).fileName();
    }

    QStringList quoted;
    for (const QString& file : files) {
      quoted.append("\"" + QFileInfo(file).fileName() + "\"");
    }
    return quoted.join(" ");
  }

  QTimer timer_;
  std::deque<std::optional<QStringList>> file_answers_;
  std::vector<shown_message> messages_;
};

bool wait_until(const std::function<bool()>& condition) {
  const QDeadlineTimer deadline(OPERATION_TIMEOUT_MILLISECONDS);
  while (!condition() && !deadline.hasExpired()) {
    QEventLoop loop;
    QTimer::singleShot(EVENT_WAIT_MILLISECONDS, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return condition();
}

struct worker_recorder {
  explicit worker_recorder(worker_thread& worker) {
    QObject::connect(&worker, &worker_thread::progress_updated, &context,
                     [this](const int value) { progress.push_back(value); });
    QObject::connect(&worker, &worker_thread::log_message, &context,
                     [this](const QString& message) { logs.append(message); });
    QObject::connect(&worker, &worker_thread::operation_completed, &context,
                     [this](const bool succeeded) { success = succeeded; });
  }

  [[nodiscard]] bool wait_for_completion() const {
    return wait_until([this] { return success.has_value(); });
  }

  QObject context;
  std::vector<int> progress;
  QStringList logs;
  std::optional<bool> success;
};

QString to_qstring(const fixtures::temporary_file& file) {
  return QString::fromStdString(file.string());
}

class drive_manager_ui_test : public testing::Test {
protected:
  drive_manager_ui_test()
      : input_(".bin"), video_(".mkv"), output_(".out"), input_data_(fixtures::make_pattern(SMALL_INPUT_SIZE)) {
    fixtures::write_bytes(input_.path(), input_data_);
    window_ = std::make_unique<drive_manager_ui>();
    window_->show();
  }

  ~drive_manager_ui_test() override {
    window_.reset();
  }

  QGroupBox* group(const QString& title) const {
    for (QGroupBox* const box : window_->findChildren<QGroupBox*>()) {
      if (box->title() == title) {
        return box;
      }
    }
    return nullptr;
  }

  QPushButton* button(const QString& group_title, const QString& text, const int occurrence = 0) const {
    int seen = 0;
    for (QPushButton* const candidate : group(group_title)->findChildren<QPushButton*>()) {
      if (candidate->text() == text && seen++ == occurrence) {
        return candidate;
      }
    }
    return nullptr;
  }

  QLineEdit* line_edit(const QString& group_title, const int index) const {
    return group(group_title)->findChildren<QLineEdit*>().at(index);
  }

  QLineEdit* input_edit() const {
    return line_edit("File Operations", 0);
  }

  QLineEdit* output_edit() const {
    return line_edit("File Operations", 1);
  }

  QLineEdit* password_edit() const {
    return line_edit("File Operations", 2);
  }

  QCheckBox* encrypt_box() const {
    return group("File Operations")->findChild<QCheckBox*>();
  }

  QListWidget* batch_list() const {
    return group("Batch Operations")->findChild<QListWidget*>();
  }

  QLineEdit* batch_directory_edit() const {
    return line_edit("Batch Operations", 0);
  }

  QComboBox* platform_combo_box() const {
    return group("Streaming (Twitch / YouTube)")->findChildren<QComboBox*>().at(0);
  }

  QComboBox* resolution_combo_box() const {
    return group("Streaming (Twitch / YouTube)")->findChildren<QComboBox*>().at(1);
  }

  QLineEdit* stream_url_edit() const {
    return line_edit("Streaming (Twitch / YouTube)", 0);
  }

  QLineEdit* stream_key_edit() const {
    return line_edit("Streaming (Twitch / YouTube)", 1);
  }

  QSpinBox* bitrate_box() const {
    return group("Streaming (Twitch / YouTube)")->findChildren<QSpinBox*>().at(0);
  }

  QSpinBox* frames_per_second_box() const {
    return group("Streaming (Twitch / YouTube)")->findChildren<QSpinBox*>().at(1);
  }

  QString log_text() const {
    return group("Logs")->findChild<QTextEdit*>()->toPlainText();
  }

  QString permanent_status() const {
    return window_->statusBar()->findChild<QLabel*>()->text();
  }

  bool encode_button_enabled() const {
    return button("File Operations", "Encode to Video")->isEnabled();
  }

  void select_files(const QString& input, const QString& output) const {
    input_edit()->setText(input);
    output_edit()->setText(output);
  }

  bool wait_for_message(const QString& title, const QString& text) const {
    return wait_until([&] { return driver_.has_message(title, text); });
  }

  void encode_input_to_video(const bool encrypt) {
    select_files(to_qstring(input_), to_qstring(video_));
    encrypt_box()->setChecked(encrypt);
    password_edit()->setText(encrypt ? PASSWORD : "");
    button("File Operations", "Encode to Video")->click();

    const bool encode_succeeded = wait_for_message("Success", "Encoding completed successfully");
    ASSERT_TRUE(encode_succeeded);

    driver_.clear();
  }

  void stream_input_to(const fixtures::temporary_file& stream) {
    platform_combo_box()->setCurrentIndex(CUSTOM_INDEX);
    const QString path = to_qstring(stream);
    const qsizetype split = path.size() - 1;
    stream_url_edit()->setText(path.left(split));
    stream_key_edit()->setText(path.mid(split));
    input_edit()->setText(to_qstring(input_));
    button("Streaming (Twitch / YouTube)", "Stream Encode")->click();

    const bool stream_encode_succeeded = wait_for_message("Success", "Stream encode completed successfully");
    ASSERT_TRUE(stream_encode_succeeded);

    driver_.clear();
  }

  fixtures::temporary_file input_;
  fixtures::temporary_file video_;
  fixtures::temporary_file output_;
  std::vector<std::byte> input_data_;
  dialog_driver driver_;
  std::unique_ptr<drive_manager_ui> window_;
};

}

TEST_F(drive_manager_ui_test, should_start_idle) {
  const QString title = window_->windowTitle();
  const QString status = group("Status")->findChildren<QLabel*>().at(1)->text();

  const QString idle_status = permanent_status();
  const bool mentions_drive_manager_initialized = log_text().contains("Drive Manager initialized");
  const QString stream_url = stream_url_edit()->text();
  const int frames_per_second = frames_per_second_box()->value();
  EXPECT_EQ(title, "YouTube Media Storage - Drive Manager");
  EXPECT_EQ(status, "Status: Idle");
  EXPECT_EQ(idle_status, "Ready");
  EXPECT_TRUE(mentions_drive_manager_initialized);
  EXPECT_EQ(stream_url, "rtmp://live.twitch.tv/app/");
  EXPECT_EQ(frames_per_second, FRAMES_PER_SECOND);
}

TEST_F(drive_manager_ui_test, should_toggle_password_visibility) {
  QPushButton* const toggle = button("File Operations", "Show");

  toggle->click();

  const QLineEdit::EchoMode shown_mode = password_edit()->echoMode();
  const QString shown_label = toggle->text();
  EXPECT_EQ(shown_mode, QLineEdit::Normal);
  EXPECT_EQ(shown_label, "Hide");

  toggle->click();

  const QLineEdit::EchoMode hidden_mode = password_edit()->echoMode();
  const QString hidden_label = toggle->text();
  EXPECT_EQ(hidden_mode, QLineEdit::Password);
  EXPECT_EQ(hidden_label, "Show");
}

TEST_F(drive_manager_ui_test, should_select_input_and_output_files) {
  driver_.queue_file_answer(QStringList{to_qstring(input_)});
  driver_.queue_file_answer(QStringList{to_qstring(video_)});

  button("File Operations", "Browse...", 0)->click();
  button("File Operations", "Browse...", 1)->click();

  const std::string selected_input = input_edit()->text().toStdString();
  const std::string selected_output = output_edit()->text().toStdString();
  const std::string expected_selected_input = input_.string();
  const std::string expected_selected_output = video_.string();
  const bool mentions_selected_input_file = log_text().contains("Selected input file: ");
  const bool mentions_selected_output_file = log_text().contains("Selected output file: ");
  EXPECT_EQ(selected_input, expected_selected_input);
  EXPECT_EQ(selected_output, expected_selected_output);
  EXPECT_TRUE(mentions_selected_input_file);
  EXPECT_TRUE(mentions_selected_output_file);
}

TEST_F(drive_manager_ui_test, should_keep_paths_when_file_dialogs_are_cancelled) {
  driver_.queue_file_answer(std::nullopt);
  driver_.queue_file_answer(std::nullopt);
  driver_.queue_file_answer(std::nullopt);
  driver_.queue_file_answer(std::nullopt);

  button("File Operations", "Browse...", 0)->click();
  button("File Operations", "Browse...", 1)->click();
  button("Batch Operations", "Add Files")->click();
  button("Batch Operations", "Browse...")->click();

  const bool input_empty = input_edit()->text().isEmpty();
  const bool output_empty = output_edit()->text().isEmpty();
  const int batch_list_count = batch_list()->count();
  const bool batch_directory_empty = batch_directory_edit()->text().isEmpty();
  EXPECT_TRUE(input_empty);
  EXPECT_TRUE(output_empty);
  EXPECT_EQ(batch_list_count, 0);
  EXPECT_TRUE(batch_directory_empty);
}

TEST_F(drive_manager_ui_test, should_add_batch_files_once_and_remove_them) {
  const fixtures::temporary_file second(".bin");
  fixtures::write_bytes(second.path(), input_data_);
  driver_.queue_file_answer(QStringList{to_qstring(input_), to_qstring(second)});
  driver_.queue_file_answer(QStringList{to_qstring(input_)});

  button("Batch Operations", "Add Files")->click();
  button("Batch Operations", "Add Files")->click();

  const int added_count = batch_list()->count();
  const QString added_status = permanent_status();
  const bool mentions_two_added = log_text().contains("Added 2 files to batch list");
  const bool mentions_one_added = log_text().contains("Added 1 files to batch list");
  EXPECT_EQ(added_count, 2);
  EXPECT_EQ(added_status, "Files in queue: 2");
  EXPECT_TRUE(mentions_two_added);
  EXPECT_TRUE(mentions_one_added);

  batch_list()->item(0)->setSelected(true);
  button("Batch Operations", "Remove Selected")->click();

  const int remaining_count = batch_list()->count();
  const QString remaining_status = permanent_status();
  EXPECT_EQ(remaining_count, 1);
  EXPECT_EQ(remaining_status, "Files in queue: 1");

  button("Batch Operations", "Clear All")->click();

  const int cleared_count = batch_list()->count();
  const QString cleared_status = permanent_status();
  EXPECT_EQ(cleared_count, 0);
  EXPECT_EQ(cleared_status, "Files in queue: 0");
}

TEST_F(drive_manager_ui_test, should_select_a_batch_output_directory) {
  const fixtures::temporary_file directory("");
  std::filesystem::create_directories(directory.path());
  driver_.queue_file_answer(QStringList{to_qstring(directory)});

  button("Batch Operations", "Browse...")->click();

  const bool batch_directory_empty = batch_directory_edit()->text().isEmpty();
  const bool mentions_selected_output_directory = log_text().contains("Selected output directory: ");
  EXPECT_FALSE(batch_directory_empty);
  EXPECT_TRUE(mentions_selected_output_directory);
}

TEST_F(drive_manager_ui_test, should_switch_stream_platforms) {
  platform_combo_box()->setCurrentIndex(YOUTUBE_INDEX);

  const QString youtube_url = stream_url_edit()->text();
  const bool youtube_read_only = stream_url_edit()->isReadOnly();
  EXPECT_EQ(youtube_url, "rtmp://a.rtmp.youtube.com/live2/");
  EXPECT_TRUE(youtube_read_only);

  platform_combo_box()->setCurrentIndex(CUSTOM_INDEX);

  const bool custom_url_empty = stream_url_edit()->text().isEmpty();
  const bool custom_read_only = stream_url_edit()->isReadOnly();
  EXPECT_TRUE(custom_url_empty);
  EXPECT_FALSE(custom_read_only);

  platform_combo_box()->setCurrentIndex(TWITCH_INDEX);

  const QString twitch_url = stream_url_edit()->text();
  const bool twitch_read_only = stream_url_edit()->isReadOnly();
  EXPECT_EQ(twitch_url, "rtmp://live.twitch.tv/app/");
  EXPECT_TRUE(twitch_read_only);
}

TEST_F(drive_manager_ui_test, should_pick_a_bitrate_for_each_resolution) {
  resolution_combo_box()->setCurrentIndex(RESOLUTION_4K_INDEX);

  const int bitrate_4k = bitrate_box()->value();
  EXPECT_EQ(bitrate_4k, BITRATE_4K);

  resolution_combo_box()->setCurrentIndex(RESOLUTION_1440P_INDEX);

  const int bitrate_1440p = bitrate_box()->value();
  EXPECT_EQ(bitrate_1440p, BITRATE_1440P);

  resolution_combo_box()->setCurrentIndex(RESOLUTION_1080P_INDEX);

  const int bitrate_1080p = bitrate_box()->value();
  EXPECT_EQ(bitrate_1080p, BITRATE_1080P);
}

TEST_F(drive_manager_ui_test, should_clear_logs_from_the_button_and_the_menu) {
  button("Logs", "Clear Logs")->click();

  const bool mentions_drive_manager_initialized = log_text().contains("Drive Manager initialized");
  const bool button_cleared_logs = log_text().contains("Logs cleared");
  EXPECT_FALSE(mentions_drive_manager_initialized);
  EXPECT_TRUE(button_cleared_logs);

  QAction* const action = window_->menuBar()->actions().at(1)->menu()->actions().at(0);
  group("Logs")->findChild<QTextEdit*>()->clear();
  action->trigger();

  const bool menu_cleared_logs = log_text().contains("Logs cleared");
  EXPECT_TRUE(menu_cleared_logs);
}

TEST_F(drive_manager_ui_test, should_show_the_about_box_and_exit_from_the_menu) {
  QAction* const about = window_->menuBar()->actions().at(2)->menu()->actions().at(0);
  QAction* const exit = window_->menuBar()->actions().at(0)->menu()->actions().at(0);

  about->trigger();

  const bool about_shown = driver_.has_message("About", QString("Version %1").arg(ms_version()));
  EXPECT_TRUE(about_shown);

  exit->trigger();

  const bool window_visible = window_->isVisible();
  EXPECT_FALSE(window_visible);
}

TEST_F(drive_manager_ui_test, should_validate_files_before_encoding) {
  QPushButton* const encode = button("File Operations", "Encode to Video");

  encode->click();

  const bool input_warning_shown = driver_.has_message("Warning", "Please select an input file");
  EXPECT_TRUE(input_warning_shown);

  input_edit()->setText(to_qstring(input_) + ".missing");
  encode->click();

  const bool missing_file_warning_shown = driver_.has_message("Warning", "Input file does not exist");
  EXPECT_TRUE(missing_file_warning_shown);

  input_edit()->setText(to_qstring(input_));
  encode->click();

  const bool output_warning_shown = driver_.has_message("Warning", "Please select an output file");
  EXPECT_TRUE(output_warning_shown);

  output_edit()->setText(to_qstring(video_));
  encrypt_box()->setChecked(true);
  encode->click();

  const bool password_warning_shown = driver_.has_message("Warning", "Password required when encrypting");
  const bool encode_enabled = encode_button_enabled();
  EXPECT_TRUE(password_warning_shown);
  EXPECT_TRUE(encode_enabled);
}

TEST_F(drive_manager_ui_test, should_validate_files_before_decoding) {
  QPushButton* const decode = button("File Operations", "Decode from Video");

  decode->click();

  const bool input_warning_shown = driver_.has_message("Warning", "Please select an input file");
  EXPECT_TRUE(input_warning_shown);

  input_edit()->setText(to_qstring(video_));
  fixtures::write_bytes(video_.path(), input_data_);
  decode->click();

  const bool output_warning_shown = driver_.has_message("Warning", "Please select an output file");
  EXPECT_TRUE(output_warning_shown);
}

TEST_F(drive_manager_ui_test, should_encode_and_decode_an_encrypted_file) {
  encode_input_to_video(true);

  const bool mentions_encrypting_chunks_with_password = log_text().contains("Encrypting chunks with password");
  const bool mentions_input_size = log_text().contains(QString("Input size: %1 bytes").arg(SMALL_INPUT_SIZE));
  const bool mentions_chunk_count = log_text().contains("Chunks: 1");
  const bool password_empty = password_edit()->text().isEmpty();
  const bool encode_enabled = encode_button_enabled();
  EXPECT_TRUE(mentions_encrypting_chunks_with_password);
  EXPECT_TRUE(mentions_input_size);
  EXPECT_TRUE(mentions_chunk_count);
  EXPECT_TRUE(password_empty);
  EXPECT_TRUE(encode_enabled);

  select_files(to_qstring(video_), to_qstring(output_));
  password_edit()->setText(PASSWORD);
  button("File Operations", "Decode from Video")->click();

  const bool decode_succeeded = wait_for_message("Success", "Decoding completed successfully");
  const bool mentions_packets_extracted = log_text().contains("Packets extracted: ");
  const bool mentions_chunks_decoded = log_text().contains("Chunks decoded: 1");
  ASSERT_TRUE(decode_succeeded);
  EXPECT_TRUE(mentions_packets_extracted);
  EXPECT_TRUE(mentions_chunks_decoded);

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output_.path());
  const int progress = group("Status")->findChild<QProgressBar*>()->value();
  EXPECT_EQ(output_bytes, input_data_);
  EXPECT_EQ(progress, 0);
}

TEST_F(drive_manager_ui_test, should_report_a_failed_decode) {
  encode_input_to_video(true);

  select_files(to_qstring(video_), to_qstring(output_));
  password_edit()->setText("wrong");
  button("File Operations", "Decode from Video")->click();

  const bool decode_failed = wait_for_message("Error", "Decode error: encryption/decryption error");
  const bool mentions_decode_error = log_text().contains("✗ Decode error");
  ASSERT_TRUE(decode_failed);
  EXPECT_TRUE(mentions_decode_error);

  const QString password = password_edit()->text();
  EXPECT_EQ(password, "wrong");
}

TEST_F(drive_manager_ui_test, should_warn_when_an_operation_is_already_running) {
  select_files(to_qstring(input_), to_qstring(video_));
  QPushButton* const encode = button("File Operations", "Encode to Video");

  encode->click();

  const bool encode_enabled = encode_button_enabled();
  EXPECT_FALSE(encode_enabled);

  QMetaObject::invokeMethod(window_.get(), "start_decode");

  const bool busy_warning_shown = driver_.has_message("Warning", "An operation is already in progress");
  const bool encode_succeeded = wait_for_message("Success", "Encoding completed successfully");
  EXPECT_TRUE(busy_warning_shown);
  ASSERT_TRUE(encode_succeeded);
}

TEST_F(drive_manager_ui_test, should_warn_when_a_batch_starts_while_an_operation_is_running) {
  select_files(to_qstring(input_), to_qstring(video_));
  QPushButton* const encode = button("File Operations", "Encode to Video");

  encode->click();
  QMetaObject::invokeMethod(window_.get(), "start_batch_encode");

  const bool busy_warning_shown = driver_.has_message("Warning", "An operation is already in progress");
  const bool encode_succeeded = wait_for_message("Success", "Encoding completed successfully");
  EXPECT_TRUE(busy_warning_shown);
  ASSERT_TRUE(encode_succeeded);
}

TEST_F(drive_manager_ui_test, should_report_progress_while_running) {
  select_files(to_qstring(input_), to_qstring(video_));
  QProgressBar* const progress = group("Status")->findChild<QProgressBar*>();
  std::vector<int> values;
  QObject::connect(progress, &QProgressBar::valueChanged, [&](const int value) { values.push_back(value); });

  button("File Operations", "Encode to Video")->click();

  const bool encode_succeeded = wait_for_message("Success", "Encoding completed successfully");
  ASSERT_TRUE(encode_succeeded);

  const bool reached_complete = std::ranges::find(values, COMPLETE_PERCENTAGE) != values.end();
  EXPECT_TRUE(reached_complete);
}

TEST_F(drive_manager_ui_test, should_validate_a_batch_before_encoding) {
  QPushButton* const batch = button("Batch Operations", "Batch Encode All");

  batch->click();

  const bool empty_batch_warning_shown = driver_.has_message("Warning", "No files in batch list");
  EXPECT_TRUE(empty_batch_warning_shown);

  batch_list()->addItem(to_qstring(input_));
  batch->click();

  const bool directory_warning_shown = driver_.has_message("Warning", "Please select an output directory");
  EXPECT_TRUE(directory_warning_shown);

  batch_directory_edit()->setText(QString::fromStdString(std::filesystem::temp_directory_path().string()));
  encrypt_box()->setChecked(true);
  batch->click();

  const bool password_warning_shown = driver_.has_message("Warning", "Password required when encrypting");
  EXPECT_TRUE(password_warning_shown);
}

TEST_F(drive_manager_ui_test, should_encode_a_batch) {
  const fixtures::temporary_file directory("");
  std::filesystem::create_directories(directory.path());
  const fixtures::temporary_file second(".bin");
  fixtures::write_bytes(second.path(), input_data_);
  batch_list()->addItem(to_qstring(input_));
  batch_list()->addItem(to_qstring(second));
  batch_directory_edit()->setText(to_qstring(directory));
  encrypt_box()->setChecked(true);
  password_edit()->setText(PASSWORD);

  button("Batch Operations", "Batch Encode All")->click();

  const bool batch_succeeded = wait_for_message("Success", "Batch encoding finished: 2 succeeded, 0 failed");
  const bool mentions_two_files = log_text().contains("Batch encoding 2 files");
  const bool first_video_exists =
      std::filesystem::exists(directory.path() / (input_.path().filename().string() + ".mkv"));
  const bool second_video_exists =
      std::filesystem::exists(directory.path() / (second.path().filename().string() + ".mkv"));
  ASSERT_TRUE(batch_succeeded);
  EXPECT_TRUE(mentions_two_files);
  EXPECT_TRUE(first_video_exists);
  EXPECT_TRUE(second_video_exists);

  const bool password_empty = password_edit()->text().isEmpty();
  const bool encode_enabled = encode_button_enabled();
  EXPECT_TRUE(password_empty);
  EXPECT_TRUE(encode_enabled);
}

TEST_F(drive_manager_ui_test, should_count_failures_in_a_batch) {
  const fixtures::temporary_file directory("");
  std::filesystem::create_directories(directory.path());
  batch_list()->addItem(to_qstring(input_) + ".missing");
  batch_list()->addItem(to_qstring(input_));
  batch_directory_edit()->setText(to_qstring(directory));

  button("Batch Operations", "Batch Encode All")->click();

  const bool batch_finished = wait_for_message("Batch Finished", "Batch encoding finished: 1 succeeded, 1 failed");
  const bool mentions_encode_file_not_found = log_text().contains("✗ Encode error: file not found");
  const bool mentions_encoding_completed_successfully = log_text().contains("✓ Encoding completed successfully");
  ASSERT_TRUE(batch_finished);
  EXPECT_TRUE(mentions_encode_file_not_found);
  EXPECT_TRUE(mentions_encoding_completed_successfully);
}

TEST_F(drive_manager_ui_test, should_validate_a_stream_encode) {
  QPushButton* const stream = button("Streaming (Twitch / YouTube)", "Stream Encode");

  stream->click();

  const bool input_warning_shown = driver_.has_message("Warning", "Please select an input file");
  EXPECT_TRUE(input_warning_shown);

  input_edit()->setText(to_qstring(input_));
  stream->click();

  const bool stream_key_warning_shown = driver_.has_message("Warning", "Please enter your stream key");
  EXPECT_TRUE(stream_key_warning_shown);

  stream_key_edit()->setText("key");
  encrypt_box()->setChecked(true);
  stream->click();

  const bool password_warning_shown = driver_.has_message("Warning", "Password required when encrypting");
  EXPECT_TRUE(password_warning_shown);
}

TEST_F(drive_manager_ui_test, should_validate_a_stream_decode) {
  QPushButton* const stream = button("Streaming (Twitch / YouTube)", "Stream Decode");

  stream->click();

  const bool output_warning_shown = driver_.has_message("Warning", "Please select an output file");
  EXPECT_TRUE(output_warning_shown);

  output_edit()->setText(to_qstring(output_));
  platform_combo_box()->setCurrentIndex(CUSTOM_INDEX);
  stream->click();

  const bool url_warning_shown = driver_.has_message("Warning", "Please enter a stream URL to decode from");
  EXPECT_TRUE(url_warning_shown);
}

TEST_F(drive_manager_ui_test, should_stream_encode_and_decode_a_file) {
  const fixtures::temporary_file stream(".flv");
  resolution_combo_box()->setCurrentIndex(RESOLUTION_1080P_INDEX);
  encrypt_box()->setChecked(true);
  password_edit()->setText(PASSWORD);

  stream_input_to(stream);

  const bool mentions_resolution = log_text().contains("Resolution: 1920x1080 (30 fps)");
  const bool mentions_bitrate = log_text().contains(QString("Bitrate: %1 kbps").arg(BITRATE_1080P));
  const bool mentions_encrypting_chunks_with_password = log_text().contains("Encrypting chunks with password");
  const bool mentions_streamed = log_text().contains("Streamed ");
  EXPECT_TRUE(mentions_resolution);
  EXPECT_TRUE(mentions_bitrate);
  EXPECT_TRUE(mentions_encrypting_chunks_with_password);
  EXPECT_TRUE(mentions_streamed);

  output_edit()->setText(to_qstring(output_));
  password_edit()->setText(PASSWORD);
  button("Streaming (Twitch / YouTube)", "Stream Decode")->click();

  const bool stream_decode_succeeded = wait_for_message("Success", "Stream decode completed successfully");
  const bool mentions_output_size = log_text().contains(QString("Output size: %1 bytes").arg(SMALL_INPUT_SIZE));
  ASSERT_TRUE(stream_decode_succeeded);
  EXPECT_TRUE(mentions_output_size);

  const std::vector<std::byte> output_bytes = fixtures::read_bytes(output_.path());
  EXPECT_EQ(output_bytes, input_data_);
}

TEST_F(drive_manager_ui_test, should_report_a_failed_stream_encode_and_decode) {
  const fixtures::temporary_file stream(".flv");
  input_edit()->setText(to_qstring(input_));
  platform_combo_box()->setCurrentIndex(CUSTOM_INDEX);
  stream_url_edit()->setText(to_qstring(stream) + ".missing-directory/");
  stream_key_edit()->setText("stream.flv");

  button("Streaming (Twitch / YouTube)", "Stream Encode")->click();

  const bool stream_encode_failed = wait_for_message("Error", "Stream encode error: encoding failed");
  ASSERT_TRUE(stream_encode_failed);

  encrypt_box()->setChecked(true);
  password_edit()->setText(PASSWORD);
  stream_input_to(stream);
  output_edit()->setText(to_qstring(output_));
  button("Streaming (Twitch / YouTube)", "Stream Decode")->click();

  const bool stream_decode_failed = wait_for_message("Error", "Stream decode error: encryption/decryption error");
  const bool output_exists = std::filesystem::exists(output_.path());
  ASSERT_TRUE(stream_decode_failed);
  EXPECT_FALSE(output_exists);
}

TEST_F(drive_manager_ui_test, should_cancel_a_running_operation_when_closed) {
  fixtures::write_bytes(input_.path(), fixtures::make_pattern(MULTI_CHUNK_INPUT_SIZE));
  select_files(to_qstring(input_), to_qstring(video_));

  button("File Operations", "Encode to Video")->click();
  window_.reset();

  const bool messages_empty = driver_.messages().empty();
  EXPECT_TRUE(messages_empty);
}

TEST(worker_thread_test, should_report_progress_and_completion_for_an_encode) {
  const fixtures::temporary_file input(".bin");
  const fixtures::temporary_file video(".mkv");
  fixtures::write_bytes(input.path(), fixtures::make_pattern(SMALL_INPUT_SIZE));
  worker_request request;
  request.operation = worker_request::operation_type::ENCODE;
  request.input_path = to_qstring(input);
  request.output_path = to_qstring(video);
  worker_thread worker(request);
  worker_recorder recorder(worker);

  worker.start();

  const bool worker_finished = worker.wait(OPERATION_TIMEOUT_MILLISECONDS);
  ASSERT_TRUE(worker_finished);

  const bool completed = recorder.wait_for_completion();
  ASSERT_TRUE(completed);
  EXPECT_TRUE(*recorder.success);

  const int first_progress = recorder.progress.front();
  const int last_progress = recorder.progress.back();
  EXPECT_EQ(first_progress, STARTING_PERCENTAGE);
  EXPECT_EQ(last_progress, COMPLETE_PERCENTAGE);
}

TEST(worker_thread_test, should_stream_with_the_requested_settings) {
  const fixtures::temporary_file input(".bin");
  const fixtures::temporary_file stream(".flv");
  fixtures::write_bytes(input.path(), fixtures::make_pattern(SMALL_INPUT_SIZE));
  worker_request request;
  request.operation = worker_request::operation_type::STREAM_ENCODE;
  request.input_path = to_qstring(input);
  request.stream_url = to_qstring(stream);
  request.width = STREAM_WIDTH;
  request.height = STREAM_HEIGHT;
  request.frames_per_second = STREAM_FRAMES_PER_SECOND;
  request.bitrate_kilobits_per_second = STREAM_BITRATE;
  worker_thread worker(request);
  worker_recorder recorder(worker);

  worker.start();

  const bool worker_finished = worker.wait(OPERATION_TIMEOUT_MILLISECONDS);
  ASSERT_TRUE(worker_finished);

  const bool completed = recorder.wait_for_completion();
  const bool mentions_resolution = recorder.logs.contains("Resolution: 1280x720 (30 fps)");
  ASSERT_TRUE(completed);
  EXPECT_TRUE(*recorder.success);
  EXPECT_TRUE(mentions_resolution);
}

int main(int argument_count, char** arguments) {
  if (qEnvironmentVariableIsEmpty(PLATFORM_VARIABLE)) {
    qputenv(PLATFORM_VARIABLE, HEADLESS_PLATFORM);
  }

  QStandardPaths::setTestModeEnabled(true);
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication application(argument_count, arguments);
  QApplication::setOrganizationName(TEST_ORGANIZATION);
  QApplication::setQuitOnLastWindowClosed(false);
  testing::InitGoogleTest(&argument_count, arguments);
  return RUN_ALL_TESTS();
}
