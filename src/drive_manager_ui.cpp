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
constexpr int STREAM_DECODE_TIMEOUT_SEC = 30;
constexpr int CUSTOM_PLATFORM_INDEX = 2;

QString documentsLocation() {
  return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QString describeFailure(const QString& prefix, const ms_status_t status) {
  QString message = QString("%1: %2").arg(prefix, ms_status_string(status));
  if (const QString detail = QString::fromUtf8(ms_last_error()); !detail.isEmpty()) {
    message += QString(" (%1)").arg(detail);
  }
  return message;
}
}

WorkerThread::WorkerThread(WorkerRequest request, QObject* parent) : QThread(parent), request(std::move(request)) {
}

int WorkerThread::progressCallback(const uint64_t current, const uint64_t total, void* user) {
  auto* thread = static_cast<WorkerThread*>(user);
  if (thread->cancelRequested) {
    return 1;
  }
  const auto [start, span] = thread->progressRange;
  int percentage;
  if (total > 0) {
    percentage = start + static_cast<int>(static_cast<uint64_t>(span) * std::min(current + 1, total) / total);
  } else {
    percentage = start + static_cast<int>(current % static_cast<uint64_t>(span));
  }
  emit thread->progressUpdated(std::min(percentage, start + span));
  return 0;
}

void WorkerThread::run() {
  const std::string input = request.inputPath.toStdString();
  const std::string output = request.outputPath.toStdString();
  const std::string url = request.streamUrl.toStdString();
  const std::string password = request.password.toStdString();

  switch (request.operation) {
  case WorkerRequest::Operation::Encode:
    runEncode(input, output, password);
    break;
  case WorkerRequest::Operation::Decode:
    runDecode(input, output, password);
    break;
  case WorkerRequest::Operation::StreamEncode:
    runStreamEncode(input, url, password);
    break;
  case WorkerRequest::Operation::StreamDecode:
    runStreamDecode(url, output, password);
    break;
  }
}

void WorkerThread::finish(const ms_status_t status, const QString& successMessage, const QString& failurePrefix) {
  if (status == MS_OK) {
    emit progressUpdated(100);
    emit operationCompleted(true, successMessage);
  } else {
    emit operationCompleted(false, describeFailure(failurePrefix, status));
  }
}

void WorkerThread::runEncode(const std::string& input, const std::string& output, const std::string& password) {
  emit statusUpdated("Starting encoding process...");
  emit logMessage("Encoding: " + request.inputPath + " -> " + request.outputPath);
  if (request.encrypt) {
    emit logMessage("Encrypting chunks with password");
  }
  progressRange = {5, 90};
  emit progressUpdated(progressRange.start);

  ms_encode_options_t opts{};
  opts.input_path = input.c_str();
  opts.output_path = output.c_str();
  opts.encrypt = request.encrypt ? 1 : 0;
  opts.password = password.c_str();
  opts.password_len = password.size();
  opts.hash_algorithm = MS_HASH_CRC32;
  opts.progress = progressCallback;
  opts.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_encode(&opts, &result);
  if (status == MS_OK) {
    emit logMessage(QString("Input size: %1 bytes").arg(result.input_size));
    emit logMessage(QString("Chunks: %1").arg(result.total_chunks));
    emit logMessage(QString("Generated %1 packets in %2 frames").arg(result.total_packets).arg(result.total_frames));
  }
  finish(status, "Encoding completed successfully", "Encode error");
}

void WorkerThread::runDecode(const std::string& input, const std::string& output, const std::string& password) {
  emit statusUpdated("Starting decoding process...");
  emit logMessage("Decoding: " + request.inputPath + " -> " + request.outputPath);
  progressRange = {10, 70};
  emit progressUpdated(progressRange.start);

  ms_decode_options_t opts{};
  opts.input_path = input.c_str();
  opts.output_path = output.c_str();
  opts.password = password.c_str();
  opts.password_len = password.size();
  opts.progress = progressCallback;
  opts.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_decode(&opts, &result);
  if (status == MS_OK) {
    emit logMessage(QString("Video size: %1 bytes").arg(result.input_size));
    emit logMessage(QString("Packets extracted: %1").arg(result.total_packets));
    emit logMessage(QString("Chunks decoded: %1").arg(result.total_chunks));
    emit logMessage(QString("Frames: %1").arg(result.total_frames));
  }
  finish(status, "Decoding completed successfully", "Decode error");
}

void WorkerThread::runStreamEncode(const std::string& input, const std::string& url, const std::string& password) {
  emit statusUpdated("Starting stream encode...");
  emit logMessage("Stream encode: " + request.inputPath + " -> " + request.streamUrl);
  emit logMessage(QString("Resolution: %1x%2 (%3 fps)").arg(request.width).arg(request.height).arg(request.fps));
  emit logMessage(QString("Bitrate: %1 kbps").arg(request.bitrateKbps));
  if (request.encrypt) {
    emit logMessage("Encrypting chunks with password");
  }
  progressRange = {5, 90};
  emit progressUpdated(progressRange.start);

  ms_stream_encode_options_t opts{};
  opts.input_path = input.c_str();
  opts.stream_url = url.c_str();
  opts.encrypt = request.encrypt ? 1 : 0;
  opts.password = password.c_str();
  opts.password_len = password.size();
  opts.hash_algorithm = MS_HASH_CRC32;
  opts.bitrate_kbps = request.bitrateKbps;
  opts.width = request.width;
  opts.height = request.height;
  opts.fps = request.fps;
  opts.progress = progressCallback;
  opts.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_stream_encode(&opts, &result);
  if (status == MS_OK) {
    emit logMessage(QString("Input size: %1 bytes").arg(result.input_size));
    emit logMessage(QString("Chunks: %1").arg(result.total_chunks));
    emit logMessage(QString("Streamed %1 packets in %2 frames").arg(result.total_packets).arg(result.total_frames));
  }
  finish(status, "Stream encode completed successfully", "Stream encode error");
}

void WorkerThread::runStreamDecode(const std::string& url, const std::string& output, const std::string& password) {
  emit statusUpdated("Waiting for stream...");
  emit logMessage("Stream decode: " + request.streamUrl + " -> " + request.outputPath);
  progressRange = {10, 85};
  emit progressUpdated(5);

  ms_stream_decode_options_t opts{};
  opts.stream_url = url.c_str();
  opts.output_path = output.c_str();
  opts.password = password.c_str();
  opts.password_len = password.size();
  opts.timeout_sec = STREAM_DECODE_TIMEOUT_SEC;
  opts.progress = progressCallback;
  opts.progress_user = this;

  ms_result_t result{};
  const ms_status_t status = ms_stream_decode(&opts, &result);
  if (status == MS_OK) {
    emit logMessage(QString("Packets extracted: %1").arg(result.total_packets));
    emit logMessage(QString("Chunks decoded: %1").arg(result.total_chunks));
    emit logMessage(QString("Frames: %1").arg(result.total_frames));
    emit logMessage(QString("Output size: %1 bytes").arg(result.output_size));
  }
  finish(status, "Stream decode completed successfully", "Stream decode error");
}

DriveManagerUI::DriveManagerUI(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("YouTube Media Storage - Drive Manager");
  setMinimumSize(1200, 800);

  setupUI();
  setupMenuBar();
  setupStatusBar();
  connectSignals();
  loadSettings();

  resetProgress();
  logMessage("Drive Manager initialized");
}

DriveManagerUI::~DriveManagerUI() {
  if (workerThread) {
    workerThread->requestCancel();
    workerThread->wait();
  }
  saveSettings();
}

void DriveManagerUI::setupUI() {
  centralWidget = new QWidget(this);
  setCentralWidget(centralWidget);

  mainSplitter = new QSplitter(Qt::Horizontal, centralWidget);

  auto* leftPanel = new QWidget();
  auto* leftLayout = new QVBoxLayout(leftPanel);

  fileOperationsGroup = new QGroupBox("File Operations");
  auto* fileOpsLayout = new QGridLayout(fileOperationsGroup);

  fileOpsLayout->addWidget(new QLabel("Input File:"), 0, 0);
  inputFileEdit = new QLineEdit();
  inputFileEdit->setReadOnly(true);
  fileOpsLayout->addWidget(inputFileEdit, 0, 1);

  selectInputButton = new QPushButton("Browse...");
  fileOpsLayout->addWidget(selectInputButton, 0, 2);

  fileOpsLayout->addWidget(new QLabel("Output File:"), 1, 0);
  outputFileEdit = new QLineEdit();
  outputFileEdit->setReadOnly(true);
  fileOpsLayout->addWidget(outputFileEdit, 1, 1);

  selectOutputButton = new QPushButton("Browse...");
  fileOpsLayout->addWidget(selectOutputButton, 1, 2);

  encryptCheckBox = new QCheckBox("Encrypt with password");
  fileOpsLayout->addWidget(encryptCheckBox, 2, 0, 1, 3);

  fileOpsLayout->addWidget(new QLabel("Password:"), 3, 0);
  passwordEdit = new QLineEdit();
  passwordEdit->setPlaceholderText("For encrypt or decrypt");
  passwordEdit->setEchoMode(QLineEdit::Password);
  fileOpsLayout->addWidget(passwordEdit, 3, 1);
  passwordVisibilityButton = new QPushButton("Show");
  passwordVisibilityButton->setFixedWidth(selectInputButton->sizeHint().width());
  fileOpsLayout->addWidget(passwordVisibilityButton, 3, 2);

  encodeButton = new QPushButton("Encode to Video");
  encodeButton->setIcon(QIcon::fromTheme("media-record"));
  fileOpsLayout->addWidget(encodeButton, 4, 0, 1, 3);

  decodeButton = new QPushButton("Decode from Video");
  decodeButton->setIcon(QIcon::fromTheme("media-playback-start"));
  fileOpsLayout->addWidget(decodeButton, 5, 0, 1, 3);

  leftLayout->addWidget(fileOperationsGroup);

  batchGroup = new QGroupBox("Batch Operations");
  auto* batchLayout = new QVBoxLayout(batchGroup);

  fileListWidget = new QListWidget();
  fileListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
  batchLayout->addWidget(fileListWidget);

  auto* batchButtonsLayout = new QHBoxLayout();
  addFilesButton = new QPushButton("Add Files");
  removeFilesButton = new QPushButton("Remove Selected");
  clearFilesButton = new QPushButton("Clear All");
  batchButtonsLayout->addWidget(addFilesButton);
  batchButtonsLayout->addWidget(removeFilesButton);
  batchButtonsLayout->addWidget(clearFilesButton);
  batchLayout->addLayout(batchButtonsLayout);

  auto* batchOutputLayout = new QHBoxLayout();
  batchOutputLayout->addWidget(new QLabel("Output Directory:"));
  batchOutputDirEdit = new QLineEdit();
  batchOutputDirEdit->setReadOnly(true);
  batchOutputButton = new QPushButton("Browse...");
  batchOutputLayout->addWidget(batchOutputDirEdit);
  batchOutputLayout->addWidget(batchOutputButton);
  batchLayout->addLayout(batchOutputLayout);

  batchEncodeButton = new QPushButton("Batch Encode All");
  batchEncodeButton->setIcon(QIcon::fromTheme("document-save-all"));
  batchLayout->addWidget(batchEncodeButton);

  leftLayout->addWidget(batchGroup);

  streamGroup = new QGroupBox("Streaming (Twitch / YouTube)");
  auto* streamLayout = new QGridLayout(streamGroup);

  streamLayout->addWidget(new QLabel("Platform:"), 0, 0);
  platformCombo = new QComboBox();
  platformCombo->addItem("Twitch", "rtmp://live.twitch.tv/app/");
  platformCombo->addItem("YouTube", "rtmp://a.rtmp.youtube.com/live2/");
  platformCombo->addItem("Custom", "");
  streamLayout->addWidget(platformCombo, 0, 1, 1, 2);

  streamLayout->addWidget(new QLabel("RTMP URL:"), 1, 0);
  streamUrlEdit = new QLineEdit();
  streamUrlEdit->setPlaceholderText("rtmp://live.twitch.tv/app/");
  streamUrlEdit->setText(platformCombo->currentData().toString());
  streamUrlEdit->setReadOnly(true);
  streamLayout->addWidget(streamUrlEdit, 1, 1, 1, 2);

  streamLayout->addWidget(new QLabel("Stream Key:"), 2, 0);
  streamKeyEdit = new QLineEdit();
  streamKeyEdit->setPlaceholderText("Your stream key");
  streamKeyEdit->setEchoMode(QLineEdit::Password);
  streamLayout->addWidget(streamKeyEdit, 2, 1, 1, 2);

  streamLayout->addWidget(new QLabel("Resolution:"), 3, 0);
  resolutionCombo = new QComboBox();
  resolutionCombo->addItem("1080p (1920x1080)", QSize(1920, 1080));
  resolutionCombo->addItem("1440p (2560x1440)", QSize(2560, 1440));
  resolutionCombo->addItem("4K (3840x2160)", QSize(3840, 2160));
  resolutionCombo->setCurrentIndex(0);
  streamLayout->addWidget(resolutionCombo, 3, 1, 1, 2);

  streamLayout->addWidget(new QLabel("Bitrate (kbps):"), 4, 0);
  bitrateSpinBox = new QSpinBox();
  bitrateSpinBox->setRange(1000, 50000);
  bitrateSpinBox->setValue(8000);
  bitrateSpinBox->setSingleStep(1000);
  bitrateSpinBox->setSuffix(" kbps");
  streamLayout->addWidget(bitrateSpinBox, 4, 1, 1, 2);

  streamLayout->addWidget(new QLabel("FPS:"), 5, 0);
  fpsSpinBox = new QSpinBox();
  fpsSpinBox->setRange(1, 240);
  fpsSpinBox->setValue(FRAME_FPS);
  fpsSpinBox->setSingleStep(1);
  fpsSpinBox->setSuffix(" fps");
  streamLayout->addWidget(fpsSpinBox, 5, 1, 1, 2);

  streamEncodeButton = new QPushButton("Stream Encode");
  streamEncodeButton->setIcon(QIcon::fromTheme("network-transmit"));
  streamLayout->addWidget(streamEncodeButton, 6, 0, 1, 3);

  streamDecodeButton = new QPushButton("Stream Decode");
  streamDecodeButton->setIcon(QIcon::fromTheme("network-receive"));
  streamLayout->addWidget(streamDecodeButton, 7, 0, 1, 3);

  leftLayout->addWidget(streamGroup);

  auto* rightPanel = new QWidget();
  auto* rightLayout = new QVBoxLayout(rightPanel);

  statusGroup = new QGroupBox("Status");
  auto* statusLayout = new QVBoxLayout(statusGroup);

  progressBar = new QProgressBar();
  progressBar->setRange(0, 100);
  statusLayout->addWidget(progressBar);

  progressLabel = new QLabel("Ready");
  statusLayout->addWidget(progressLabel);

  statusLabel = new QLabel("Status: Idle");
  statusLayout->addWidget(statusLabel);

  rightLayout->addWidget(statusGroup);

  logsGroup = new QGroupBox("Logs");
  auto* logsLayout = new QVBoxLayout(logsGroup);

  logTextEdit = new QTextEdit();
  logTextEdit->setReadOnly(true);
  logsLayout->addWidget(logTextEdit);

  clearLogsButton = new QPushButton("Clear Logs");
  logsLayout->addWidget(clearLogsButton);

  rightLayout->addWidget(logsGroup);

  mainSplitter->addWidget(leftPanel);
  mainSplitter->addWidget(rightPanel);
  mainSplitter->setSizes({600, 600});

  auto* mainLayout = new QHBoxLayout(centralWidget);
  mainLayout->addWidget(mainSplitter);
}

void DriveManagerUI::setupMenuBar() {
  QMenu* fileMenu = menuBar()->addMenu("&File");
  fileMenu->addAction("E&xit", this, &QWidget::close);

  QMenu* toolsMenu = menuBar()->addMenu("&Tools");
  toolsMenu->addAction("&Clear Logs", this, &DriveManagerUI::clearLogs);

  QMenu* helpMenu = menuBar()->addMenu("&Help");
  helpMenu->addAction("&About", this, [this] {
    QMessageBox::about(this, "About",
                       QString("YouTube Media Storage Drive Manager\n\n"
                               "Encode and decode files using video storage technology\n"
                               "Version %1")
                           .arg(ms_version()));
  });
}

void DriveManagerUI::setupStatusBar() {
  permanentStatus = new QLabel("Ready");
  statusBar()->addPermanentWidget(permanentStatus);
}

void DriveManagerUI::connectSignals() {
  connect(selectInputButton, &QPushButton::clicked, this, &DriveManagerUI::selectInputFile);
  connect(selectOutputButton, &QPushButton::clicked, this, &DriveManagerUI::selectOutputFile);
  connect(encodeButton, &QPushButton::clicked, this, &DriveManagerUI::startEncode);
  connect(decodeButton, &QPushButton::clicked, this, &DriveManagerUI::startDecode);

  connect(addFilesButton, &QPushButton::clicked, this, &DriveManagerUI::addBatchFiles);
  connect(removeFilesButton, &QPushButton::clicked, this, &DriveManagerUI::removeSelectedFiles);
  connect(clearFilesButton, &QPushButton::clicked, this, &DriveManagerUI::clearFileList);
  connect(batchOutputButton, &QPushButton::clicked, this, &DriveManagerUI::selectBatchOutputDirectory);
  connect(batchEncodeButton, &QPushButton::clicked, this, &DriveManagerUI::startBatchEncode);

  connect(clearLogsButton, &QPushButton::clicked, this, &DriveManagerUI::clearLogs);
  connect(passwordVisibilityButton, &QPushButton::clicked, this, &DriveManagerUI::togglePasswordVisibility);

  connect(streamEncodeButton, &QPushButton::clicked, this, &DriveManagerUI::startStreamEncode);
  connect(streamDecodeButton, &QPushButton::clicked, this, &DriveManagerUI::startStreamDecode);
  connect(platformCombo, &QComboBox::currentIndexChanged, this, &DriveManagerUI::onPlatformChanged);
  connect(resolutionCombo, &QComboBox::currentIndexChanged, this, &DriveManagerUI::onResolutionChanged);
}

void DriveManagerUI::togglePasswordVisibility() const {
  const bool hidden = passwordEdit->echoMode() == QLineEdit::Password;
  passwordEdit->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
  passwordVisibilityButton->setText(hidden ? "Hide" : "Show");
}

void DriveManagerUI::selectInputFile() {
  const QString fileName = QFileDialog::getOpenFileName(this, "Select Input File", documentsLocation());
  if (!fileName.isEmpty()) {
    inputFileEdit->setText(fileName);
    logMessage("Selected input file: " + fileName);
  }
}

void DriveManagerUI::selectOutputFile() {
  const QString fileName = QFileDialog::getSaveFileName(this, "Select Output File", documentsLocation(),
                                                        "Video Files (*.mkv *.mp4);;All Files (*)");
  if (!fileName.isEmpty()) {
    outputFileEdit->setText(fileName);
    logMessage("Selected output file: " + fileName);
  }
}

void DriveManagerUI::addBatchFiles() {
  const QStringList fileNames = QFileDialog::getOpenFileNames(this, "Select Files to Encode", documentsLocation());
  if (fileNames.isEmpty()) {
    return;
  }

  for (const QString& fileName : fileNames) {
    if (fileListWidget->findItems(fileName, Qt::MatchExactly).isEmpty()) {
      fileListWidget->addItem(fileName);
    }
  }
  logMessage(QString("Added %1 files to batch list").arg(fileNames.size()));
  updateFileList();
}

void DriveManagerUI::selectBatchOutputDirectory() {
  const QString dirName = QFileDialog::getExistingDirectory(this, "Select Output Directory", documentsLocation());
  if (!dirName.isEmpty()) {
    batchOutputDirEdit->setText(dirName);
    logMessage("Selected output directory: " + dirName);
  }
}

bool DriveManagerUI::warnIfBusy() {
  if (isOperationRunning) {
    QMessageBox::warning(this, "Warning", "An operation is already in progress");
  }
  return isOperationRunning;
}

bool DriveManagerUI::validateInputFile() {
  if (inputFileEdit->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please select an input file");
    return false;
  }
  if (!QFile::exists(inputFileEdit->text())) {
    QMessageBox::warning(this, "Warning", "Input file does not exist");
    return false;
  }
  return true;
}

bool DriveManagerUI::validateOutputFile() {
  if (outputFileEdit->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please select an output file");
    return false;
  }
  return true;
}

bool DriveManagerUI::validatePassword() {
  if (encryptCheckBox->isChecked() && passwordEdit->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Password required when encrypting");
    return false;
  }
  return true;
}

void DriveManagerUI::startWorker(const WorkerRequest& request, const QString& operationName) {
  isOperationRunning = true;
  currentOperation = operationName;
  setOperationRunning(true);

  workerThread = std::make_unique<WorkerThread>(request);
  connect(workerThread.get(), &WorkerThread::progressUpdated, this, &DriveManagerUI::onProgressUpdated);
  connect(workerThread.get(), &WorkerThread::statusUpdated, this, &DriveManagerUI::onStatusUpdated);
  connect(workerThread.get(), &WorkerThread::operationCompleted, this, &DriveManagerUI::onOperationCompleted);
  connect(workerThread.get(), &WorkerThread::logMessage, this, &DriveManagerUI::logMessage);
  workerThread->start();
}

void DriveManagerUI::setOperationRunning(const bool running) {
  encodeButton->setEnabled(!running);
  decodeButton->setEnabled(!running);
  batchEncodeButton->setEnabled(!running);
  streamEncodeButton->setEnabled(!running);
  streamDecodeButton->setEnabled(!running);
}

void DriveManagerUI::startEncode() {
  if (warnIfBusy() || !validateInputFile() || !validateOutputFile() || !validatePassword()) {
    return;
  }

  WorkerRequest request;
  request.operation = WorkerRequest::Operation::Encode;
  request.inputPath = inputFileEdit->text();
  request.outputPath = outputFileEdit->text();
  request.encrypt = encryptCheckBox->isChecked();
  request.password = passwordEdit->text();
  startWorker(request, "Encoding");
}

void DriveManagerUI::startDecode() {
  if (warnIfBusy() || !validateInputFile() || !validateOutputFile()) {
    return;
  }

  WorkerRequest request;
  request.operation = WorkerRequest::Operation::Decode;
  request.inputPath = inputFileEdit->text();
  request.outputPath = outputFileEdit->text();
  request.password = passwordEdit->text();
  startWorker(request, "Decoding");
}

void DriveManagerUI::startBatchEncode() {
  if (warnIfBusy()) {
    return;
  }
  if (fileListWidget->count() == 0) {
    QMessageBox::warning(this, "Warning", "No files in batch list");
    return;
  }
  if (batchOutputDirEdit->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please select an output directory");
    return;
  }
  if (!validatePassword()) {
    return;
  }

  batchQueue.clear();
  for (int i = 0; i < fileListWidget->count(); ++i) {
    batchQueue.append(fileListWidget->item(i)->text());
  }
  batchSucceeded = 0;
  batchFailed = 0;
  batchRunning = true;
  logMessage(QString("Batch encoding %1 files").arg(batchQueue.size()));
  startNextBatchItem();
}

void DriveManagerUI::startNextBatchItem() {
  const QString inputPath = batchQueue.takeFirst();
  const QDir outputDir(batchOutputDirEdit->text());

  WorkerRequest request;
  request.operation = WorkerRequest::Operation::Encode;
  request.inputPath = inputPath;
  request.outputPath = outputDir.filePath(QFileInfo(inputPath).fileName() + ".mkv");
  request.encrypt = encryptCheckBox->isChecked();
  request.password = passwordEdit->text();
  startWorker(request, QString("Batch Encoding (%1 remaining)").arg(batchQueue.size()));
}

void DriveManagerUI::finishBatch() {
  batchRunning = false;
  setOperationRunning(false);
  resetProgress();

  const QString summary =
      QString("Batch encoding finished: %1 succeeded, %2 failed").arg(batchSucceeded).arg(batchFailed);
  logMessage(summary);
  if (batchFailed == 0) {
    QMessageBox::information(this, "Success", summary);
    passwordEdit->clear();
  } else {
    QMessageBox::warning(this, "Batch Finished", summary);
  }
}

void DriveManagerUI::onPlatformChanged(const int index) const {
  streamUrlEdit->setText(platformCombo->itemData(index).toString());
  streamUrlEdit->setReadOnly(index != CUSTOM_PLATFORM_INDEX);
}

void DriveManagerUI::onResolutionChanged(const int index) const {
  const QSize resolution = resolutionCombo->itemData(index).toSize();
  if (resolution.height() <= 1080) {
    bitrateSpinBox->setValue(8000);
  } else if (resolution.height() <= 1440) {
    bitrateSpinBox->setValue(16000);
  } else {
    bitrateSpinBox->setValue(35000);
  }
}

void DriveManagerUI::startStreamEncode() {
  if (warnIfBusy() || !validateInputFile()) {
    return;
  }
  if (streamKeyEdit->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please enter your stream key");
    return;
  }
  if (!validatePassword()) {
    return;
  }

  const QSize resolution = resolutionCombo->currentData().toSize();
  WorkerRequest request;
  request.operation = WorkerRequest::Operation::StreamEncode;
  request.inputPath = inputFileEdit->text();
  request.streamUrl = streamUrlEdit->text() + streamKeyEdit->text();
  request.encrypt = encryptCheckBox->isChecked();
  request.password = passwordEdit->text();
  request.bitrateKbps = bitrateSpinBox->value();
  request.width = resolution.width();
  request.height = resolution.height();
  request.fps = fpsSpinBox->value();
  startWorker(request, "Stream Encoding");
}

void DriveManagerUI::startStreamDecode() {
  if (warnIfBusy() || !validateOutputFile()) {
    return;
  }
  if (streamUrlEdit->text().isEmpty()) {
    QMessageBox::warning(this, "Warning", "Please enter a stream URL to decode from");
    return;
  }

  WorkerRequest request;
  request.operation = WorkerRequest::Operation::StreamDecode;
  request.outputPath = outputFileEdit->text();
  request.streamUrl = streamUrlEdit->text() + streamKeyEdit->text();
  request.password = passwordEdit->text();
  startWorker(request, "Stream Decoding");
}

void DriveManagerUI::clearLogs() const {
  logTextEdit->clear();
  logMessage("Logs cleared");
}

void DriveManagerUI::removeSelectedFiles() const {
  for (const QListWidgetItem* item : fileListWidget->selectedItems()) {
    delete fileListWidget->takeItem(fileListWidget->row(item));
  }
  updateFileList();
}

void DriveManagerUI::clearFileList() const {
  fileListWidget->clear();
  updateFileList();
}

void DriveManagerUI::updateFileList() const {
  permanentStatus->setText(QString("Files in queue: %1").arg(fileListWidget->count()));
}

void DriveManagerUI::onOperationCompleted(const bool success, const QString& message) {
  if (workerThread) {
    workerThread->wait();
    workerThread.reset();
  }

  isOperationRunning = false;

  if (batchRunning) {
    logMessage((success ? "✓ " : "✗ ") + message);
    ++(success ? batchSucceeded : batchFailed);
    if (batchQueue.isEmpty()) {
      finishBatch();
    } else {
      startNextBatchItem();
    }
    return;
  }

  setOperationRunning(false);

  if (success) {
    logMessage("✓ " + message);
    QMessageBox::information(this, "Success", message);
    passwordEdit->clear();
  } else {
    logMessage("✗ " + message);
    QMessageBox::critical(this, "Error", message);
  }

  resetProgress();
}

void DriveManagerUI::onProgressUpdated(const int percentage) const {
  progressBar->setValue(percentage);
  progressLabel->setText(QString("%1% - %2").arg(percentage).arg(currentOperation));
}

void DriveManagerUI::onStatusUpdated(const QString& status) const {
  statusLabel->setText("Status: " + status);
  permanentStatus->setText(status);
}

void DriveManagerUI::resetProgress() {
  progressBar->setValue(0);
  progressLabel->setText("Ready");
  statusLabel->setText("Status: Idle");
  currentOperation = "Idle";
}

void DriveManagerUI::logMessage(const QString& message) const {
  const QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
  logTextEdit->append(QString("[%1] %2").arg(timestamp, message));
}

void DriveManagerUI::loadSettings() {
  const QSettings settings;
  restoreGeometry(settings.value("geometry").toByteArray());
  restoreState(settings.value("windowState").toByteArray());
}

void DriveManagerUI::saveSettings() const {
  QSettings settings;
  settings.setValue("geometry", saveGeometry());
  settings.setValue("windowState", saveState());
}
