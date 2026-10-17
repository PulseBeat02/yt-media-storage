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

#include "drive_manager_ui.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

namespace {
constexpr int STREAM_DECODE_TIMEOUT_SECONDS = 30;
constexpr int CUSTOM_PLATFORM_INDEX = 2;

QString documents_location() {
  return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QString describe_failure(const QString& prefix, const ms_status_t status) {
  QString message = QString("%1: %2").arg(prefix, ms_status_string(status));
  if (const QString detail = QString::fromUtf8(ms_last_error()); !detail.isEmpty()) {
    message += QString(" (%1)").arg(detail);
  }
  return message;
}
}

worker_thread::worker_thread(worker_request request, QObject* parent) : QThread(parent), request_(std::move(request)) {
}

int worker_thread::progress_callback(const uint64_t current, const uint64_t total, void* user) {
  auto* thread = static_cast<worker_thread*>(user);
  if (thread->cancel_requested_) {
    return 1;
  }
  const auto [start, span] = thread->progress_range_;
  int percentage;
  if (total > 0) {
    percentage = start + static_cast<int>(static_cast<uint64_t>(span) * std::min(current + 1, total) / total);
  } else {
    percentage = start + static_cast<int>(current % static_cast<uint64_t>(span));
  }
  emit thread->progress_updated(std::min(percentage, start + span));
  return 0;
}

void worker_thread::run() {
  const std::string input = request_.input_path.toStdString();
  const std::string output = request_.output_path.toStdString();
  const std::string url = request_.stream_url.toStdString();
  const std::string password = request_.password.toStdString();

  switch (request_.operation) {
  case worker_request::operation_type::ENCODE:
    run_encode(input, output, password);
    break;
  case worker_request::operation_type::DECODE:
    run_decode(input, output, password);
    break;
  case worker_request::operation_type::STREAM_ENCODE:
    run_stream_encode(input, url, password);
    break;
  case worker_request::operation_type::STREAM_DECODE:
    run_stream_decode(url, output, password);
    break;
  }
}

void worker_thread::finish(const ms_status_t status, const QString& success_message, const QString& failure_prefix) {
  if (status == MS_OK) {
    emit progress_updated(100);
    emit operation_completed(true, success_message);
  } else {
    emit operation_completed(false, describe_failure(failure_prefix, status));
  }
}

void worker_thread::run_encode(const std::string& input, const std::string& output, const std::string& password) {
  emit status_updated("Starting encoding process...");
  emit log_message("Encoding: " + request_.input_path + " -> " + request_.output_path);
  if (request_.encrypt) {
    emit log_message("Encrypting chunks with password");
  }
  progress_range_ = {5, 90};
  emit progress_updated(progress_range_.start);

  ms_encode_options_t options{};
  options.input_path = input.c_str();
  options.output_path = output.c_str();
  options.encrypt = request_.encrypt ? 1 : 0;
  options.password = password.c_str();
  options.password_len = password.size();
  options.hash_algorithm = MS_HASH_CRC32;
  options.progress = progress_callback;
  options.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_encode(&options, &result);
  if (status == MS_OK) {
    emit log_message(QString("Input size: %1 bytes").arg(result.input_size));
    emit log_message(QString("Chunks: %1").arg(result.total_chunks));
    emit log_message(QString("Generated %1 packets in %2 frames").arg(result.total_packets).arg(result.total_frames));
  }
  finish(status, "Encoding completed successfully", "Encode error");
}

void worker_thread::run_decode(const std::string& input, const std::string& output, const std::string& password) {
  emit status_updated("Starting decoding process...");
  emit log_message("Decoding: " + request_.input_path + " -> " + request_.output_path);
  progress_range_ = {10, 70};
  emit progress_updated(progress_range_.start);

  ms_decode_options_t options{};
  options.input_path = input.c_str();
  options.output_path = output.c_str();
  options.password = password.c_str();
  options.password_len = password.size();
  options.progress = progress_callback;
  options.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_decode(&options, &result);
  if (status == MS_OK) {
    emit log_message(QString("Video size: %1 bytes").arg(result.input_size));
    emit log_message(QString("Packets extracted: %1").arg(result.total_packets));
    emit log_message(QString("Chunks decoded: %1").arg(result.total_chunks));
    emit log_message(QString("Frames: %1").arg(result.total_frames));
  }
  finish(status, "Decoding completed successfully", "Decode error");
}

void worker_thread::run_stream_encode(const std::string& input, const std::string& url, const std::string& password) {
  emit status_updated("Starting stream encode...");
  emit log_message("Stream encode: " + request_.input_path + " -> " + request_.stream_url);
  emit log_message(
      QString("Resolution: %1x%2 (%3 fps)").arg(request_.width).arg(request_.height).arg(request_.frames_per_second));
  emit log_message(QString("Bitrate: %1 kbps").arg(request_.bitrate_kilobits_per_second));
  if (request_.encrypt) {
    emit log_message("Encrypting chunks with password");
  }
  progress_range_ = {5, 90};
  emit progress_updated(progress_range_.start);

  ms_stream_encode_options_t options{};
  options.input_path = input.c_str();
  options.stream_url = url.c_str();
  options.encrypt = request_.encrypt ? 1 : 0;
  options.password = password.c_str();
  options.password_len = password.size();
  options.hash_algorithm = MS_HASH_CRC32;
  options.bitrate_kbps = request_.bitrate_kilobits_per_second;
  options.width = request_.width;
  options.height = request_.height;
  options.fps = request_.frames_per_second;
  options.progress = progress_callback;
  options.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_stream_encode(&options, &result);
  if (status == MS_OK) {
    emit log_message(QString("Input size: %1 bytes").arg(result.input_size));
    emit log_message(QString("Chunks: %1").arg(result.total_chunks));
    emit log_message(QString("Streamed %1 packets in %2 frames").arg(result.total_packets).arg(result.total_frames));
  }
  finish(status, "Stream encode completed successfully", "Stream encode error");
}

void worker_thread::run_stream_decode(const std::string& url, const std::string& output, const std::string& password) {
  emit status_updated("Waiting for stream...");
  emit log_message("Stream decode: " + request_.stream_url + " -> " + request_.output_path);
  progress_range_ = {10, 85};
  emit progress_updated(5);

  ms_stream_decode_options_t options{};
  options.stream_url = url.c_str();
  options.output_path = output.c_str();
  options.password = password.c_str();
  options.password_len = password.size();
  options.timeout_sec = STREAM_DECODE_TIMEOUT_SECONDS;
  options.progress = progress_callback;
  options.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_stream_decode(&options, &result);
  if (status == MS_OK) {
    emit log_message(QString("Packets extracted: %1").arg(result.total_packets));
    emit log_message(QString("Chunks decoded: %1").arg(result.total_chunks));
    emit log_message(QString("Frames: %1").arg(result.total_frames));
    emit log_message(QString("Output size: %1 bytes").arg(result.output_size));
  }
  finish(status, "Stream decode completed successfully", "Stream decode error");
}

drive_manager_ui::drive_manager_ui(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("YouTube Media Storage - Drive Manager");
  setMinimumSize(1200, 800);

  setup_ui();
  setup_menu_bar();
  setup_status_bar();
  connect_signals();
  load_settings();

  reset_progress();
  log_message("Drive Manager initialized");
}

drive_manager_ui::~drive_manager_ui() {
  if (worker_thread_) {
    worker_thread_->request_cancel();
    worker_thread_->wait();
  }
  save_settings();
}

void drive_manager_ui::setup_ui() {
  central_widget_ = new QWidget(this);
  setCentralWidget(central_widget_);

  main_splitter_ = new QSplitter(Qt::Horizontal, central_widget_);

  auto* left_panel = new QWidget();
  auto* left_layout = new QVBoxLayout(left_panel);

  file_operations_group_ = new QGroupBox("File Operations");
  auto* file_operations_layout = new QGridLayout(file_operations_group_);

  file_operations_layout->addWidget(new QLabel("Input File:"), 0, 0);
  input_file_edit_ = new QLineEdit();
  input_file_edit_->setReadOnly(true);
  file_operations_layout->addWidget(input_file_edit_, 0, 1);

  select_input_button_ = new QPushButton("Browse...");
  file_operations_layout->addWidget(select_input_button_, 0, 2);

  file_operations_layout->addWidget(new QLabel("Output File:"), 1, 0);
  output_file_edit_ = new QLineEdit();
  output_file_edit_->setReadOnly(true);
  file_operations_layout->addWidget(output_file_edit_, 1, 1);

  select_output_button_ = new QPushButton("Browse...");
  file_operations_layout->addWidget(select_output_button_, 1, 2);

  encrypt_check_box_ = new QCheckBox("Encrypt with password");
  file_operations_layout->addWidget(encrypt_check_box_, 2, 0, 1, 3);

  file_operations_layout->addWidget(new QLabel("Password:"), 3, 0);
  password_edit_ = new QLineEdit();
  password_edit_->setPlaceholderText("For encrypt or decrypt");
  password_edit_->setEchoMode(QLineEdit::Password);
  file_operations_layout->addWidget(password_edit_, 3, 1);
  password_visibility_button_ = new QPushButton("Show");
  password_visibility_button_->setFixedWidth(select_input_button_->sizeHint().width());
  file_operations_layout->addWidget(password_visibility_button_, 3, 2);

  encode_button_ = new QPushButton("Encode to Video");
  encode_button_->setIcon(QIcon::fromTheme("media-record"));
  file_operations_layout->addWidget(encode_button_, 4, 0, 1, 3);

  decode_button_ = new QPushButton("Decode from Video");
  decode_button_->setIcon(QIcon::fromTheme("media-playback-start"));
  file_operations_layout->addWidget(decode_button_, 5, 0, 1, 3);

  left_layout->addWidget(file_operations_group_);

  batch_group_ = new QGroupBox("Batch Operations");
  auto* batch_layout = new QVBoxLayout(batch_group_);

  file_list_widget_ = new QListWidget();
  file_list_widget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  batch_layout->addWidget(file_list_widget_);

  auto* batch_buttons_layout = new QHBoxLayout();
  add_files_button_ = new QPushButton("Add Files");
  remove_files_button_ = new QPushButton("Remove Selected");
  clear_files_button_ = new QPushButton("Clear All");
  batch_buttons_layout->addWidget(add_files_button_);
  batch_buttons_layout->addWidget(remove_files_button_);
  batch_buttons_layout->addWidget(clear_files_button_);
  batch_layout->addLayout(batch_buttons_layout);

  auto* batch_output_layout = new QHBoxLayout();
  batch_output_layout->addWidget(new QLabel("Output Directory:"));
  batch_output_directory_edit_ = new QLineEdit();
  batch_output_directory_edit_->setReadOnly(true);
  batch_output_button_ = new QPushButton("Browse...");
  batch_output_layout->addWidget(batch_output_directory_edit_);
  batch_output_layout->addWidget(batch_output_button_);
  batch_layout->addLayout(batch_output_layout);

  batch_encode_button_ = new QPushButton("Batch Encode All");
  batch_encode_button_->setIcon(QIcon::fromTheme("document-save-all"));
  batch_layout->addWidget(batch_encode_button_);

  left_layout->addWidget(batch_group_);

  stream_group_ = new QGroupBox("Streaming (Twitch / YouTube)");
  auto* stream_layout = new QGridLayout(stream_group_);

  stream_layout->addWidget(new QLabel("Platform:"), 0, 0);
  platform_combo_box_ = new QComboBox();
  platform_combo_box_->addItem("Twitch", "rtmp://live.twitch.tv/app/");
  platform_combo_box_->addItem("YouTube", "rtmp://a.rtmp.youtube.com/live2/");
  platform_combo_box_->addItem("Custom", "");
  stream_layout->addWidget(platform_combo_box_, 0, 1, 1, 2);

  stream_layout->addWidget(new QLabel("RTMP URL:"), 1, 0);
  stream_url_edit_ = new QLineEdit();
  stream_url_edit_->setPlaceholderText("rtmp://live.twitch.tv/app/");
  stream_url_edit_->setText(platform_combo_box_->currentData().toString());
  stream_url_edit_->setReadOnly(true);
  stream_layout->addWidget(stream_url_edit_, 1, 1, 1, 2);

  stream_layout->addWidget(new QLabel("Stream Key:"), 2, 0);
  stream_key_edit_ = new QLineEdit();
  stream_key_edit_->setPlaceholderText("Your stream key");
  stream_key_edit_->setEchoMode(QLineEdit::Password);
  stream_layout->addWidget(stream_key_edit_, 2, 1, 1, 2);

  stream_layout->addWidget(new QLabel("Resolution:"), 3, 0);
  resolution_combo_box_ = new QComboBox();
  resolution_combo_box_->addItem("1080p (1920x1080)", QSize(1920, 1080));
  resolution_combo_box_->addItem("1440p (2560x1440)", QSize(2560, 1440));
  resolution_combo_box_->addItem("4K (3840x2160)", QSize(3840, 2160));
  resolution_combo_box_->setCurrentIndex(0);
  stream_layout->addWidget(resolution_combo_box_, 3, 1, 1, 2);

  stream_layout->addWidget(new QLabel("Bitrate (kbps):"), 4, 0);
  bitrate_spin_box_ = new QSpinBox();
  bitrate_spin_box_->setRange(1000, 50000);
  bitrate_spin_box_->setValue(8000);
  bitrate_spin_box_->setSingleStep(1000);
  bitrate_spin_box_->setSuffix(" kbps");
  stream_layout->addWidget(bitrate_spin_box_, 4, 1, 1, 2);

  stream_layout->addWidget(new QLabel("FPS:"), 5, 0);
  frames_per_second_spin_box_ = new QSpinBox();
  frames_per_second_spin_box_->setRange(1, 240);
  frames_per_second_spin_box_->setValue(FRAMES_PER_SECOND);
  frames_per_second_spin_box_->setSingleStep(1);
  frames_per_second_spin_box_->setSuffix(" fps");
  stream_layout->addWidget(frames_per_second_spin_box_, 5, 1, 1, 2);

  stream_encode_button_ = new QPushButton("Stream Encode");
  stream_encode_button_->setIcon(QIcon::fromTheme("network-transmit"));
  stream_layout->addWidget(stream_encode_button_, 6, 0, 1, 3);

  stream_decode_button_ = new QPushButton("Stream Decode");
  stream_decode_button_->setIcon(QIcon::fromTheme("network-receive"));
  stream_layout->addWidget(stream_decode_button_, 7, 0, 1, 3);

  left_layout->addWidget(stream_group_);

  auto* right_panel = new QWidget();
  auto* right_layout = new QVBoxLayout(right_panel);

  status_group_ = new QGroupBox("Status");
  auto* status_layout = new QVBoxLayout(status_group_);

  progress_bar_ = new QProgressBar();
  progress_bar_->setRange(0, 100);
  status_layout->addWidget(progress_bar_);

  progress_label_ = new QLabel("Ready");
  status_layout->addWidget(progress_label_);

  status_label_ = new QLabel("Status: Idle");
  status_layout->addWidget(status_label_);

  right_layout->addWidget(status_group_);

  logs_group_ = new QGroupBox("Logs");
  auto* logs_layout = new QVBoxLayout(logs_group_);

  log_text_edit_ = new QTextEdit();
  log_text_edit_->setReadOnly(true);
  logs_layout->addWidget(log_text_edit_);

  clear_logs_button_ = new QPushButton("Clear Logs");
  logs_layout->addWidget(clear_logs_button_);

  right_layout->addWidget(logs_group_);

  main_splitter_->addWidget(left_panel);
  main_splitter_->addWidget(right_panel);
  main_splitter_->setSizes({600, 600});

  auto* main_layout = new QHBoxLayout(central_widget_);
  main_layout->addWidget(main_splitter_);
}

void drive_manager_ui::setup_menu_bar() {
  QMenu* file_menu = menuBar()->addMenu("&File");
  file_menu->addAction("E&xit", this, &QWidget::close);

  QMenu* tools_menu = menuBar()->addMenu("&Tools");
  tools_menu->addAction("&Clear Logs", this, &drive_manager_ui::clear_logs);

  QMenu* help_menu = menuBar()->addMenu("&Help");
  help_menu->addAction("&About", this, [this] {
    QMessageBox::about(this, "About",
                       QString("YouTube Media Storage Drive Manager\n\n"
                               "Encode and decode files using video storage technology\n"
                               "Version %1")
                           .arg(ms_version()));
  });
}

void drive_manager_ui::setup_status_bar() {
  permanent_status_ = new QLabel("Ready");
  statusBar()->addPermanentWidget(permanent_status_);
}

void drive_manager_ui::connect_signals() {
  connect(select_input_button_, &QPushButton::clicked, this, &drive_manager_ui::select_input_file);
  connect(select_output_button_, &QPushButton::clicked, this, &drive_manager_ui::select_output_file);
  connect(encode_button_, &QPushButton::clicked, this, &drive_manager_ui::start_encode);
  connect(decode_button_, &QPushButton::clicked, this, &drive_manager_ui::start_decode);

  connect(add_files_button_, &QPushButton::clicked, this, &drive_manager_ui::add_batch_files);
  connect(remove_files_button_, &QPushButton::clicked, this, &drive_manager_ui::remove_selected_files);
  connect(clear_files_button_, &QPushButton::clicked, this, &drive_manager_ui::clear_file_list);
  connect(batch_output_button_, &QPushButton::clicked, this, &drive_manager_ui::select_batch_output_directory);
  connect(batch_encode_button_, &QPushButton::clicked, this, &drive_manager_ui::start_batch_encode);

  connect(clear_logs_button_, &QPushButton::clicked, this, &drive_manager_ui::clear_logs);
  connect(password_visibility_button_, &QPushButton::clicked, this, &drive_manager_ui::toggle_password_visibility);

  connect(stream_encode_button_, &QPushButton::clicked, this, &drive_manager_ui::start_stream_encode);
  connect(stream_decode_button_, &QPushButton::clicked, this, &drive_manager_ui::start_stream_decode);
  connect(platform_combo_box_, &QComboBox::currentIndexChanged, this, &drive_manager_ui::on_platform_changed);
  connect(resolution_combo_box_, &QComboBox::currentIndexChanged, this, &drive_manager_ui::on_resolution_changed);
}

void drive_manager_ui::toggle_password_visibility() const {
  const bool hidden = password_edit_->echoMode() == QLineEdit::Password;
  password_edit_->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
  password_visibility_button_->setText(hidden ? "Hide" : "Show");
}

void drive_manager_ui::select_input_file() {
  const QString file_name = QFileDialog::getOpenFileName(this, "Select Input File", documents_location());
  if (!file_name.isEmpty()) {
    input_file_edit_->setText(file_name);
    log_message("Selected input file: " + file_name);
  }
}

void drive_manager_ui::select_output_file() {
  const QString file_name = QFileDialog::getSaveFileName(this, "Select Output File", documents_location(),
                                                         "Video Files (*.mkv *.mp4);;All Files (*)");
  if (!file_name.isEmpty()) {
    output_file_edit_->setText(file_name);
    log_message("Selected output file: " + file_name);
  }
}

void drive_manager_ui::add_batch_files() {
  const QStringList file_names = QFileDialog::getOpenFileNames(this, "Select Files to Encode", documents_location());
  if (file_names.isEmpty()) {
    return;
  }

  for (const QString& file_name : file_names) {
    if (file_list_widget_->findItems(file_name, Qt::MatchExactly).isEmpty()) {
      file_list_widget_->addItem(file_name);
    }
  }
  log_message(QString("Added %1 files to batch list").arg(file_names.size()));
  update_file_list();
}

void drive_manager_ui::select_batch_output_directory() {
  const QString directory_name =
      QFileDialog::getExistingDirectory(this, "Select Output Directory", documents_location());
  if (!directory_name.isEmpty()) {
    batch_output_directory_edit_->setText(directory_name);
    log_message("Selected output directory: " + directory_name);
  }
}

bool drive_manager_ui::warn_if_busy() {
  if (operation_running_) {
    QMessageBox::warning(this, "Warning", "An operation is already in progress");
  }
  return operation_running_;
}

bool drive_manager_ui::validate_input_file() {
  if (input_file_edit_->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please select an input file");
    return false;
  }
  if (!QFile::exists(input_file_edit_->text())) {
    QMessageBox::warning(this, "Warning", "Input file does not exist");
    return false;
  }
  return true;
}

bool drive_manager_ui::validate_output_file() {
  if (output_file_edit_->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please select an output file");
    return false;
  }
  return true;
}

bool drive_manager_ui::validate_password() {
  if (encrypt_check_box_->isChecked() && password_edit_->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Password required when encrypting");
    return false;
  }
  return true;
}

void drive_manager_ui::start_worker(const worker_request& request, const QString& operation_name) {
  operation_running_ = true;
  current_operation_ = operation_name;
  set_operation_running(true);

  worker_thread_ = std::make_unique<worker_thread>(request);
  connect(worker_thread_.get(), &worker_thread::progress_updated, this, &drive_manager_ui::on_progress_updated);
  connect(worker_thread_.get(), &worker_thread::status_updated, this, &drive_manager_ui::on_status_updated);
  connect(worker_thread_.get(), &worker_thread::operation_completed, this, &drive_manager_ui::on_operation_completed);
  connect(worker_thread_.get(), &worker_thread::log_message, this, &drive_manager_ui::log_message);
  worker_thread_->start();
}

void drive_manager_ui::set_operation_running(const bool running) {
  encode_button_->setEnabled(!running);
  decode_button_->setEnabled(!running);
  batch_encode_button_->setEnabled(!running);
  stream_encode_button_->setEnabled(!running);
  stream_decode_button_->setEnabled(!running);
}

void drive_manager_ui::start_encode() {
  if (warn_if_busy() || !validate_input_file() || !validate_output_file() || !validate_password()) {
    return;
  }

  worker_request request;
  request.operation = worker_request::operation_type::ENCODE;
  request.input_path = input_file_edit_->text();
  request.output_path = output_file_edit_->text();
  request.encrypt = encrypt_check_box_->isChecked();
  request.password = password_edit_->text();
  start_worker(request, "Encoding");
}

void drive_manager_ui::start_decode() {
  if (warn_if_busy() || !validate_input_file() || !validate_output_file()) {
    return;
  }

  worker_request request;
  request.operation = worker_request::operation_type::DECODE;
  request.input_path = input_file_edit_->text();
  request.output_path = output_file_edit_->text();
  request.password = password_edit_->text();
  start_worker(request, "Decoding");
}

void drive_manager_ui::start_batch_encode() {
  if (warn_if_busy()) {
    return;
  }
  if (file_list_widget_->count() == 0) {
    QMessageBox::warning(this, "Warning", "No files in batch list");
    return;
  }
  if (batch_output_directory_edit_->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please select an output directory");
    return;
  }
  if (!validate_password()) {
    return;
  }

  batch_queue_.clear();
  for (int i = 0; i < file_list_widget_->count(); ++i) {
    batch_queue_.append(file_list_widget_->item(i)->text());
  }
  batch_succeeded_ = 0;
  batch_failed_ = 0;
  batch_running_ = true;
  log_message(QString("Batch encoding %1 files").arg(batch_queue_.size()));
  start_next_batch_item();
}

void drive_manager_ui::start_next_batch_item() {
  const QString input_path = batch_queue_.takeFirst();
  const QDir output_directory(batch_output_directory_edit_->text());

  worker_request request;
  request.operation = worker_request::operation_type::ENCODE;
  request.input_path = input_path;
  request.output_path = output_directory.filePath(QFileInfo(input_path).fileName() + ".mkv");
  request.encrypt = encrypt_check_box_->isChecked();
  request.password = password_edit_->text();
  start_worker(request, QString("Batch Encoding (%1 remaining)").arg(batch_queue_.size()));
}

void drive_manager_ui::finish_batch() {
  batch_running_ = false;
  set_operation_running(false);
  reset_progress();

  const QString summary =
      QString("Batch encoding finished: %1 succeeded, %2 failed").arg(batch_succeeded_).arg(batch_failed_);
  log_message(summary);
  if (batch_failed_ == 0) {
    QMessageBox::information(this, "Success", summary);
    password_edit_->clear();
  } else {
    QMessageBox::warning(this, "Batch Finished", summary);
  }
}

void drive_manager_ui::on_platform_changed(const int index) const {
  stream_url_edit_->setText(platform_combo_box_->itemData(index).toString());
  stream_url_edit_->setReadOnly(index != CUSTOM_PLATFORM_INDEX);
}

void drive_manager_ui::on_resolution_changed(const int index) const {
  const QSize resolution = resolution_combo_box_->itemData(index).toSize();
  if (resolution.height() <= 1080) {
    bitrate_spin_box_->setValue(8000);
  } else if (resolution.height() <= 1440) {
    bitrate_spin_box_->setValue(16000);
  } else {
    bitrate_spin_box_->setValue(35000);
  }
}

void drive_manager_ui::start_stream_encode() {
  if (warn_if_busy() || !validate_input_file()) {
    return;
  }
  if (stream_key_edit_->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please enter your stream key");
    return;
  }
  if (!validate_password()) {
    return;
  }

  const QSize resolution = resolution_combo_box_->currentData().toSize();
  worker_request request;
  request.operation = worker_request::operation_type::STREAM_ENCODE;
  request.input_path = input_file_edit_->text();
  request.stream_url = stream_url_edit_->text() + stream_key_edit_->text();
  request.encrypt = encrypt_check_box_->isChecked();
  request.password = password_edit_->text();
  request.bitrate_kilobits_per_second = bitrate_spin_box_->value();
  request.width = resolution.width();
  request.height = resolution.height();
  request.frames_per_second = frames_per_second_spin_box_->value();
  start_worker(request, "Stream Encoding");
}

void drive_manager_ui::start_stream_decode() {
  if (warn_if_busy() || !validate_output_file()) {
    return;
  }
  if (stream_url_edit_->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please enter a stream URL to decode from");
    return;
  }

  worker_request request;
  request.operation = worker_request::operation_type::STREAM_DECODE;
  request.output_path = output_file_edit_->text();
  request.stream_url = stream_url_edit_->text() + stream_key_edit_->text();
  request.password = password_edit_->text();
  start_worker(request, "Stream Decoding");
}

void drive_manager_ui::clear_logs() const {
  log_text_edit_->clear();
  log_message("Logs cleared");
}

void drive_manager_ui::remove_selected_files() const {
  for (const QListWidgetItem* item : file_list_widget_->selectedItems()) {
    delete file_list_widget_->takeItem(file_list_widget_->row(item));
  }
  update_file_list();
}

void drive_manager_ui::clear_file_list() const {
  file_list_widget_->clear();
  update_file_list();
}

void drive_manager_ui::update_file_list() const {
  permanent_status_->setText(QString("Files in queue: %1").arg(file_list_widget_->count()));
}

void drive_manager_ui::on_operation_completed(const bool success, const QString& message) {
  if (worker_thread_) {
    worker_thread_->wait();
    worker_thread_.reset();
  }

  operation_running_ = false;

  if (batch_running_) {
    log_message((success ? "✓ " : "✗ ") + message);
    ++(success ? batch_succeeded_ : batch_failed_);
    if (batch_queue_.isEmpty()) {
      finish_batch();
    } else {
      start_next_batch_item();
    }
    return;
  }

  set_operation_running(false);

  if (success) {
    log_message("✓ " + message);
    QMessageBox::information(this, "Success", message);
    password_edit_->clear();
  } else {
    log_message("✗ " + message);
    QMessageBox::critical(this, "Error", message);
  }

  reset_progress();
}

void drive_manager_ui::on_progress_updated(const int percentage) const {
  progress_bar_->setValue(percentage);
  progress_label_->setText(QString("%1% - %2").arg(percentage).arg(current_operation_));
}

void drive_manager_ui::on_status_updated(const QString& status) const {
  status_label_->setText("Status: " + status);
  permanent_status_->setText(status);
}

void drive_manager_ui::reset_progress() {
  progress_bar_->setValue(0);
  progress_label_->setText("Ready");
  status_label_->setText("Status: Idle");
  current_operation_ = "Idle";
}

void drive_manager_ui::log_message(const QString& message) const {
  const QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
  log_text_edit_->append(QString("[%1] %2").arg(timestamp, message));
}

void drive_manager_ui::load_settings() {
  const QSettings settings;
  restoreGeometry(settings.value("geometry").toByteArray());
  restoreState(settings.value("windowState").toByteArray());
}

void drive_manager_ui::save_settings() const {
  QSettings settings;
  settings.setValue("geometry", saveGeometry());
  settings.setValue("windowState", saveState());
}
