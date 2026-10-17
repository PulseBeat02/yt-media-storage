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

#pragma once

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

struct WorkerRequest {
  enum class Operation { Encode, Decode, StreamEncode, StreamDecode };

  Operation operation = Operation::Encode;
  QString inputPath;
  QString outputPath;
  QString streamUrl;
  bool encrypt = false;
  QString password;
  int bitrateKbps = FRAME_BITRATE;
  int width = FRAME_WIDTH_STREAM;
  int height = FRAME_HEIGHT_STREAM;
  int fps = FRAME_FPS;
};

class WorkerThread : public QThread {
  Q_OBJECT

public:
  explicit WorkerThread(WorkerRequest request, QObject* parent = nullptr);

  void requestCancel() {
    cancelRequested = true;
  }

signals:
  void progressUpdated(int percentage);

  void statusUpdated(const QString& status);

  void operationCompleted(bool success, const QString& message);

  void logMessage(const QString& message);

protected:
  void run() override;

private:
  struct ProgressRange {
    int start;
    int span;
  };

  WorkerRequest request;
  std::atomic<bool> cancelRequested{false};
  ProgressRange progressRange{0, 100};

  static int progressCallback(uint64_t current, uint64_t total, void* user);

  void runEncode(const std::string& input, const std::string& output, const std::string& password);

  void runDecode(const std::string& input, const std::string& output, const std::string& password);

  void runStreamEncode(const std::string& input, const std::string& url, const std::string& password);

  void runStreamDecode(const std::string& url, const std::string& output, const std::string& password);

  void finish(ms_status_t status, const QString& successMessage, const QString& failurePrefix);
};

class DriveManagerUI : public QMainWindow {
  Q_OBJECT

public:
  explicit DriveManagerUI(QWidget* parent = nullptr);

  ~DriveManagerUI() override;

private slots:
  void selectInputFile();

  void selectOutputFile();

  void addBatchFiles();

  void selectBatchOutputDirectory();

  void startEncode();

  void startDecode();

  void startBatchEncode();

  void startStreamEncode();

  void startStreamDecode();

  void onPlatformChanged(int index) const;

  void onResolutionChanged(int index) const;

  void clearLogs() const;

  void onOperationCompleted(bool success, const QString& message);

  void onProgressUpdated(int percentage) const;

  void onStatusUpdated(const QString& status) const;

  void updateFileList() const;

  void removeSelectedFiles() const;

  void clearFileList() const;

  void togglePasswordVisibility() const;

private:
  void setupUI();

  void setupMenuBar();

  void setupStatusBar();

  void connectSignals();

  void startWorker(const WorkerRequest& request, const QString& operationName);

  void startNextBatchItem();

  void finishBatch();

  void setOperationRunning(bool running);

  void resetProgress();

  void logMessage(const QString& message) const;

  void loadSettings();

  void saveSettings() const;

  bool warnIfBusy();

  bool validateInputFile();

  bool validateOutputFile();

  bool validatePassword();

  QWidget* centralWidget = nullptr;
  QSplitter* mainSplitter = nullptr;

  QGroupBox* fileOperationsGroup = nullptr;
  QLineEdit* inputFileEdit = nullptr;
  QLineEdit* outputFileEdit = nullptr;
  QPushButton* selectInputButton = nullptr;
  QPushButton* selectOutputButton = nullptr;
  QCheckBox* encryptCheckBox = nullptr;
  QLineEdit* passwordEdit = nullptr;
  QPushButton* passwordVisibilityButton = nullptr;
  QPushButton* encodeButton = nullptr;
  QPushButton* decodeButton = nullptr;

  QGroupBox* batchGroup = nullptr;
  QListWidget* fileListWidget = nullptr;
  QPushButton* addFilesButton = nullptr;
  QPushButton* removeFilesButton = nullptr;
  QPushButton* clearFilesButton = nullptr;
  QPushButton* batchEncodeButton = nullptr;
  QLineEdit* batchOutputDirEdit = nullptr;
  QPushButton* batchOutputButton = nullptr;

  QGroupBox* streamGroup = nullptr;
  QComboBox* platformCombo = nullptr;
  QLineEdit* streamUrlEdit = nullptr;
  QLineEdit* streamKeyEdit = nullptr;
  QSpinBox* bitrateSpinBox = nullptr;
  QSpinBox* fpsSpinBox = nullptr;
  QComboBox* resolutionCombo = nullptr;
  QPushButton* streamEncodeButton = nullptr;
  QPushButton* streamDecodeButton = nullptr;

  QGroupBox* statusGroup = nullptr;
  QProgressBar* progressBar = nullptr;
  QLabel* statusLabel = nullptr;
  QLabel* progressLabel = nullptr;

  QGroupBox* logsGroup = nullptr;
  QTextEdit* logTextEdit = nullptr;
  QPushButton* clearLogsButton = nullptr;

  QLabel* permanentStatus = nullptr;

  std::unique_ptr<WorkerThread> workerThread;

  bool isOperationRunning = false;
  QString currentOperation;

  bool batchRunning = false;
  QStringList batchQueue;
  int batchSucceeded = 0;
  int batchFailed = 0;
};
