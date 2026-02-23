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

#include "drive_manager_ui.h"
#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "encoder.h"
#include "decoder.h"
#include "video_encoder.h"
#include "video_decoder.h"

#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
#include <QHeaderView>
#include <QFileInfo>
#include <QDateTime>
#include <fstream>

WorkerThread::WorkerThread(Operation op, const QString& input, const QString& output,
                         bool encrypt, const QString& password, QObject* parent)
    : QThread(parent), operation(op), inputPath(input), outputPath(output),
      encrypt(encrypt), password(password) {
}

void WorkerThread::run() {
    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    bool key_used = false;
    try {
        if (operation == Encode) {
            emit statusUpdated("Starting encoding process...");
            emit logMessage("Encoding: " + inputPath + " -> " + outputPath);
            
            if (!std::filesystem::exists(inputPath.toStdString())) {
                emit operationCompleted(false, "Input file does not exist");
                return;
            }
            
            const auto input_size = std::filesystem::file_size(inputPath.toStdString());
            emit logMessage(QString("Input size: %1 bytes").arg(input_size));
            
            emit progressUpdated(5);
            const std::size_t chunk_size = encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
            const FileChunkReader reader(inputPath.toStdString().c_str(), chunk_size);
            const std::size_t num_chunks = reader.num_chunks();
            emit logMessage(QString("Chunks: %1").arg(num_chunks));
            
            if (encrypt) {
                emit logMessage("Encrypting chunks with password");
            }
            const std::array<std::byte, 16> file_id = []{
                std::array<std::byte, 16> id{};
                for (int i = 0; i < 16; ++i) {
                    id[i] = static_cast<std::byte>(i);
                }
                return id;
            }();
            if (encrypt) {
                const std::string pw = password.toStdString();
                const std::span<const std::byte> pw_span(reinterpret_cast<const std::byte*>(pw.data()), pw.size());
                key = derive_key(pw_span, file_id);
                key_used = true;
            }
            
            const Encoder encoder(file_id);
            std::size_t total_packets = 0;
            
            emit statusUpdated("Encoding chunks...");

            VideoEncoder video_encoder(outputPath.toStdString());

            for (std::size_t i = 0; i < num_chunks; ++i) {
                auto chunk_data = reader.read_chunk(i);
                std::span<const std::byte> data_to_encode(chunk_data);
                std::vector<std::byte> encrypted_buf;
                if (encrypt) {
                    encrypted_buf = encrypt_chunk(data_to_encode, key, file_id, static_cast<uint32_t>(i));
                    data_to_encode = encrypted_buf;
                }
                const bool is_last = (i == num_chunks - 1);
                auto [chunk_packets, manifest] = encoder.encode_chunk(
                    static_cast<uint32_t>(i), data_to_encode, is_last, encrypt);
                total_packets += chunk_packets.size();
                video_encoder.encode_packets(chunk_packets);

                int progress = 5 + (90 * static_cast<int>(i + 1) / static_cast<int>(num_chunks));
                emit progressUpdated(progress);
            }

            video_encoder.finalize();
            
            if (encrypt) {
                secure_zero(std::span<std::byte>(key));
            }

            emit logMessage(QString("Generated %1 packets").arg(total_packets));
            emit progressUpdated(100);
            emit operationCompleted(true, "Encoding completed successfully");
            
        } else if (operation == Decode) {
            emit statusUpdated("Starting decoding process...");
            emit logMessage("Decoding: " + inputPath + " -> " + outputPath);
            
            if (!std::filesystem::exists(inputPath.toStdString())) {
                emit operationCompleted(false, "Input video does not exist");
                return;
            }
            
            const auto video_size = std::filesystem::file_size(inputPath.toStdString());
            emit logMessage(QString("Video size: %1 bytes").arg(video_size));
            
            emit progressUpdated(10);
            Decoder decoder;
            std::size_t total_extracted = 0;
            std::size_t decoded_chunks = 0;
            uint32_t max_chunk_index = 0;
            bool found_last_chunk = false;
            uint32_t last_chunk_index = 0;
            
            VideoDecoder video_decoder(inputPath.toStdString());
            const int64_t total_frames = video_decoder.total_frames();
            emit logMessage(QString("Total frames: %1").arg(total_frames >= 0 ? QString::number(total_frames) : "unknown"));
            
            emit statusUpdated("Extracting packets from video...");
            std::size_t valid_frames = 0;
            
            while (!video_decoder.is_eof()) {
                if (auto frame_packets = video_decoder.decode_next_frame(); !frame_packets.empty()) {
                    ++valid_frames;
                    for (auto& pkt_data : frame_packets) {
                        ++total_extracted;
                        
                        if (pkt_data.size() >= HEADER_SIZE) {
                            const auto flags = static_cast<uint8_t>(pkt_data[FLAGS_OFF]);
                            uint32_t chunk_idx = 0;
                            std::memcpy(&chunk_idx, pkt_data.data() + CHUNK_INDEX_OFF, sizeof(chunk_idx));
                            if (chunk_idx > max_chunk_index)
                                max_chunk_index = chunk_idx;
                            if (flags & LastChunk) {
                                found_last_chunk = true;
                                last_chunk_index = chunk_idx;
                            }
                        }
                        
                        const std::span<const std::byte> data(pkt_data.data(), pkt_data.size());
                        if (auto result = decoder.process_packet(data); result && result->success) {
                            ++decoded_chunks;
                        }
                    }
                    
                    if (total_frames > 0) {
                        int progress = 10 + (70 * valid_frames / static_cast<int>(total_frames));
                        emit progressUpdated(progress);
                    }
                }
            }
            
            emit logMessage(QString("Valid frames: %1").arg(valid_frames));
            emit logMessage(QString("Packets extracted: %1").arg(total_extracted));
            
            if (total_extracted == 0) {
                emit operationCompleted(false, "No packets could be extracted from the video");
                return;
            }
            
            emit progressUpdated(80);
            emit statusUpdated("Assembling file...");
            
            uint32_t expected_chunks;
            if (found_last_chunk) {
                expected_chunks = last_chunk_index + 1;
            } else {
                expected_chunks = max_chunk_index + 1;
            }
            
            emit logMessage(QString("Chunks decoded: %1/%2").arg(decoded_chunks).arg(expected_chunks));
            
            if (decoded_chunks < expected_chunks) {
                emit operationCompleted(false, QString("Only decoded %1 of %2 chunks").arg(decoded_chunks).arg(expected_chunks));
                return;
            }
            
            if (decoder.is_encrypted()) {
                emit logMessage("Decrypting content with password");
                if (password.isEmpty()) {
                    emit operationCompleted(false, "Content is encrypted. Please enter the password.");
                    return;
                }
                const std::string pw = password.toStdString();
                const std::span<const std::byte> pw_span(reinterpret_cast<const std::byte*>(pw.data()), pw.size());
                auto dec_key = derive_key(pw_span, *decoder.file_id());
                decoder.set_decrypt_key(dec_key);
                secure_zero(std::span<std::byte>(dec_key));
            }
            
            if (!decoder.write_assembled_file(outputPath.toStdString(), expected_chunks)) {
                if (decoder.is_encrypted()) {
                    decoder.clear_decrypt_key();
                }
                emit operationCompleted(false, "Failed to assemble file (wrong password or corrupted data)");
                return;
            }
            
            if (decoder.is_encrypted()) {
                decoder.clear_decrypt_key();
            }
            
            emit progressUpdated(100);
            emit operationCompleted(true, "Decoding completed successfully");
        }
    } catch (const std::exception& e) {
        if (key_used) {
            secure_zero(std::span<std::byte>(key));
        }
        emit operationCompleted(false, QString("Error: %1").arg(e.what()));
    }
}

DriveManagerUI::DriveManagerUI(QWidget* parent)
    : QMainWindow(parent), isOperationRunning(false) {
    setWindowTitle("YouTube Media Storage - Drive Manager");
    setMinimumSize(1200, 800);
    
    loadSettings();
    setupUI();
    setupMenuBar();
    setupStatusBar();
    connectSignals();
    
    resetProgress();
    logMessage("Drive Manager initialized");
}

DriveManagerUI::~DriveManagerUI() {
    if (workerThread && workerThread->isRunning()) {
        workerThread->quit();
        workerThread->wait();
    }
    saveSettings();
}

void DriveManagerUI::setupUI() {
    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    
    mainSplitter = new QSplitter(Qt::Horizontal, centralWidget);
    
    // Left panel
    QWidget* leftPanel = new QWidget();
    QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
    
    // File operations group
    fileOperationsGroup = new QGroupBox("File Operations");
    QGridLayout* fileOpsLayout = new QGridLayout(fileOperationsGroup);
    
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
    
    // Batch operations group
    batchGroup = new QGroupBox("Batch Operations");
    QVBoxLayout* batchLayout = new QVBoxLayout(batchGroup);
    
    fileListWidget = new QListWidget();
    fileListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    batchLayout->addWidget(fileListWidget);
    
    QHBoxLayout* batchButtonsLayout = new QHBoxLayout();
    addFilesButton = new QPushButton("Add Files");
    removeFilesButton = new QPushButton("Remove Selected");
    clearFilesButton = new QPushButton("Clear All");
    batchButtonsLayout->addWidget(addFilesButton);
    batchButtonsLayout->addWidget(removeFilesButton);
    batchButtonsLayout->addWidget(clearFilesButton);
    batchLayout->addLayout(batchButtonsLayout);
    
    QHBoxLayout* batchOutputLayout = new QHBoxLayout();
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
    
    // Right panel
    QWidget* rightPanel = new QWidget();
    QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
    
    // Status group
    statusGroup = new QGroupBox("Status");
    QVBoxLayout* statusLayout = new QVBoxLayout(statusGroup);
    
    progressBar = new QProgressBar();
    progressBar->setRange(0, 100);
    statusLayout->addWidget(progressBar);
    
    progressLabel = new QLabel("Ready");
    statusLayout->addWidget(progressLabel);
    
    statusLabel = new QLabel("Status: Idle");
    statusLayout->addWidget(statusLabel);
    
    rightLayout->addWidget(statusGroup);
    
    // Logs group
    logsGroup = new QGroupBox("Logs");
    QVBoxLayout* logsLayout = new QVBoxLayout(logsGroup);
    
    logTextEdit = new QTextEdit();
    logTextEdit->setReadOnly(true);
    // logTextEdit->setMaximumBlockCount(1000); // Commented out - not available in Qt6
    logsLayout->addWidget(logTextEdit);
    
    clearLogsButton = new QPushButton("Clear Logs");
    logsLayout->addWidget(clearLogsButton);
    
    rightLayout->addWidget(logsGroup);
    
    // Add panels to splitter
    mainSplitter->addWidget(leftPanel);
    mainSplitter->addWidget(rightPanel);
    mainSplitter->setSizes({600, 600});
    
    // Main layout
    QHBoxLayout* mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->addWidget(mainSplitter);
}

void DriveManagerUI::setupMenuBar() {
    // Menu setup - using QMainWindow's built-in menuBar
    QMenu* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("E&xit", this, &QWidget::close);
    
    QMenu* toolsMenu = menuBar()->addMenu("&Tools");
    toolsMenu->addAction("&Clear Logs", this, &DriveManagerUI::clearLogs);
    
    QMenu* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&About", [this]() {
        QMessageBox::about(this, "About", 
            "YouTube Media Storage Drive Manager\n\n"
            "Encode and decode files using video storage technology\n"
            "Version 1.0");
    });
}

void DriveManagerUI::setupStatusBar() {
    // Status bar setup - using QMainWindow's built-in statusBar
    permanentStatus = new QLabel("Ready");
    statusBar()->addPermanentWidget(permanentStatus);
}

void DriveManagerUI::connectSignals() {
    connect(selectInputButton, &QPushButton::clicked, this, &DriveManagerUI::selectInputFile);
    connect(selectOutputButton, &QPushButton::clicked, this, &DriveManagerUI::selectOutputFile);
    connect(encodeButton, &QPushButton::clicked, this, &DriveManagerUI::startEncode);
    connect(decodeButton, &QPushButton::clicked, this, &DriveManagerUI::startDecode);
    
    connect(addFilesButton, &QPushButton::clicked, this, &DriveManagerUI::selectInputDirectory);
    connect(removeFilesButton, &QPushButton::clicked, this, &DriveManagerUI::removeSelectedFiles);
    connect(clearFilesButton, &QPushButton::clicked, this, &DriveManagerUI::clearFileList);
    connect(batchOutputButton, &QPushButton::clicked, this, &DriveManagerUI::selectOutputDirectory);
    connect(batchEncodeButton, &QPushButton::clicked, this, &DriveManagerUI::startBatchEncode);
    
    connect(clearLogsButton, &QPushButton::clicked, this, &DriveManagerUI::clearLogs);
    connect(passwordVisibilityButton, &QPushButton::clicked, this, &DriveManagerUI::togglePasswordVisibility);
}

void DriveManagerUI::togglePasswordVisibility() {
    if (passwordEdit->echoMode() == QLineEdit::Password) {
        passwordEdit->setEchoMode(QLineEdit::Normal);
        passwordVisibilityButton->setText("Hide");
    } else {
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordVisibilityButton->setText("Show");
    }
}

void DriveManagerUI::selectInputFile() {
    QString fileName = QFileDialog::getOpenFileName(this, "Select Input File", 
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (!fileName.isEmpty()) {
        inputFileEdit->setText(fileName);
        logMessage("Selected input file: " + fileName);
    }
}

void DriveManagerUI::selectOutputFile() {
    QString fileName = QFileDialog::getSaveFileName(this, "Select Output File",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "Video Files (*.mkv *.mp4);;All Files (*)");
    if (!fileName.isEmpty()) {
        outputFileEdit->setText(fileName);
        logMessage("Selected output file: " + fileName);
    }
}

void DriveManagerUI::selectInputDirectory() {
    QStringList fileNames = QFileDialog::getOpenFileNames(this, "Select Files to Encode",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    
    for (const QString& fileName : fileNames) {
        if (!fileName.isEmpty() && !fileListWidget->findItems(fileName, Qt::MatchExactly).count()) {
            fileListWidget->addItem(fileName);
        }
    }
    
    if (!fileNames.isEmpty()) {
        logMessage(QString("Added %1 files to batch list").arg(fileNames.size()));
        updateFileList();
    }
}

void DriveManagerUI::selectOutputDirectory() {
    QString dirName = QFileDialog::getExistingDirectory(this, "Select Output Directory",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (!dirName.isEmpty()) {
        batchOutputDirEdit->setText(dirName);
        logMessage("Selected output directory: " + dirName);
    }
}

void DriveManagerUI::startEncode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
        return;
    }
    
    if (!validatePaths()) {
        return;
    }
    
    const bool encrypt = encryptCheckBox->isChecked();
    if (encrypt && passwordEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Password required when encrypting");
        return;
    }
    
    isOperationRunning = true;
    currentOperation = "Encoding";
    encodeButton->setEnabled(false);
    decodeButton->setEnabled(false);
    
    workerThread = std::make_unique<WorkerThread>(WorkerThread::Encode, 
        inputFileEdit->text(), outputFileEdit->text(), encrypt, passwordEdit->text(), this);
    
    connect(workerThread.get(), &WorkerThread::progressUpdated, 
        this, &DriveManagerUI::onProgressUpdated);
    connect(workerThread.get(), &WorkerThread::statusUpdated, 
        this, &DriveManagerUI::onStatusUpdated);
    connect(workerThread.get(), &WorkerThread::operationCompleted, 
        this, &DriveManagerUI::onOperationCompleted);
    connect(workerThread.get(), &WorkerThread::logMessage, 
        this, &DriveManagerUI::onLogMessage);
    
    workerThread->start();
}

void DriveManagerUI::startDecode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
        return;
    }
    
    if (!validatePaths()) {
        return;
    }
    
    isOperationRunning = true;
    currentOperation = "Decoding";
    encodeButton->setEnabled(false);
    decodeButton->setEnabled(false);
    
    workerThread = std::make_unique<WorkerThread>(WorkerThread::Decode, 
        inputFileEdit->text(), outputFileEdit->text(), false, passwordEdit->text(), this);
    
    connect(workerThread.get(), &WorkerThread::progressUpdated, 
        this, &DriveManagerUI::onProgressUpdated);
    connect(workerThread.get(), &WorkerThread::statusUpdated, 
        this, &DriveManagerUI::onStatusUpdated);
    connect(workerThread.get(), &WorkerThread::operationCompleted, 
        this, &DriveManagerUI::onOperationCompleted);
    connect(workerThread.get(), &WorkerThread::logMessage, 
        this, &DriveManagerUI::onLogMessage);
    
    workerThread->start();
}

void DriveManagerUI::startBatchEncode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
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
    
    logMessage("Batch encoding not yet implemented - processing first file only");
    
    QListWidgetItem* firstItem = fileListWidget->item(0);
    if (firstItem) {
        QString inputPath = firstItem->text();
        QFileInfo fileInfo(inputPath);
        QString outputPath = batchOutputDirEdit->text() + "/" + fileInfo.baseName() + ".mkv";
        
        inputFileEdit->setText(inputPath);
        outputFileEdit->setText(outputPath);
        
        startEncode();
    }
}

void DriveManagerUI::clearLogs() {
    logTextEdit->clear();
    logMessage("Logs cleared");
}

void DriveManagerUI::removeSelectedFiles() {
    QList<QListWidgetItem*> selectedItems = fileListWidget->selectedItems();
    for (QListWidgetItem* item : selectedItems) {
        delete fileListWidget->takeItem(fileListWidget->row(item));
    }
    updateFileList();
}

void DriveManagerUI::clearFileList() {
    fileListWidget->clear();
    updateFileList();
}

void DriveManagerUI::updateFileList() {
    permanentStatus->setText(QString("Files in queue: %1").arg(fileListWidget->count()));
}

void DriveManagerUI::onOperationCompleted(bool success, const QString& message) {
    isOperationRunning = false;
    encodeButton->setEnabled(true);
    decodeButton->setEnabled(true);
    
    if (success) {
        logMessage("✓ " + message);
        QMessageBox::information(this, "Success", message);
        passwordEdit->clear();
    } else {
        logMessage("✗ " + message);
        QMessageBox::critical(this, "Error", message);
    }
    
    resetProgress();
    workerThread.reset();
}

void DriveManagerUI::onProgressUpdated(int percentage) {
    progressBar->setValue(percentage);
    progressLabel->setText(QString("%1% - %2").arg(percentage).arg(currentOperation));
}

void DriveManagerUI::onStatusUpdated(const QString& status) {
    statusLabel->setText("Status: " + status);
    permanentStatus->setText(status);
}

void DriveManagerUI::onLogMessage(const QString& message) {
    logMessage(message);
}

void DriveManagerUI::resetProgress() {
    progressBar->setValue(0);
    progressLabel->setText("Ready");
    statusLabel->setText("Status: Idle");
    currentOperation = "Idle";
}

void DriveManagerUI::logMessage(const QString& message) {
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    logTextEdit->append(QString("[%1] %2").arg(timestamp, message));
}

bool DriveManagerUI::validatePaths() {
    if (inputFileEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please select an input file");
        return false;
    }
    
    if (outputFileEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please select an output file");
        return false;
    }
    
    if (!QFile::exists(inputFileEdit->text())) {
        QMessageBox::warning(this, "Warning", "Input file does not exist");
        return false;
    }
    
    return true;
}

void DriveManagerUI::loadSettings() {
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
}

void DriveManagerUI::saveSettings() {
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
}
