[![CodeFactor](https://www.codefactor.io/repository/github/pulsebeat02/yt-media-storage/badge)](https://www.codefactor.io/repository/github/pulsebeat02/yt-media-storage)
[![TeamCity Full Build Status](https://img.shields.io/teamcity/build/s/YtMediaStorage_Build?server=https%3A%2F%2Fci.brandonli.me
)](https://ci.brandonli.me/project/YtMediaStorage)

# Media Storage

[![Star History Chart](https://api.star-history.com/svg?repos=PulseBeat02/yt-media-storage&type=date&legend=top-left)](https://www.star-history.com/#PulseBeat02/yt-media-storage&type=date&legend=top-left)

**Explanation Video**: https://youtu.be/l03Os5uwWmk?si=xgZPNMrvs_aDcWE5  
**YC Hacker News**: https://news.ycombinator.com/item?id=47012964

Stores files on YouTube by encoding them into a lossless video and decoding them back to the original format.

## Features

- **File Encoding/Decoding**: Encode any file into a lossless video (FFV1/MKV) and decode it back to the original file.
- **Fountain Codes**: Uses [Wirehair](https://github.com/catid/wirehair) fountain codes for redundancy and repair
- **Optional Encryption**: Encrypt files with a password using libsodium (XChaCha20-Poly1305)
- **Batch Processing**: Queue multiple files for batch encoding (GUI)
- **Progress Tracking**: Real-time progress bars and status updates (GUI)

## CI/CD Pipeline

Visit my [CI/CD pipeline](https://ci.brandonli.me), and click "Login as Guest". Visit the yt-media-storage project,
click on the latest passing build, and click "Artifacts" to download the latest build artifacts for both the CLI and
GUI. You may need to install some shared libraries (FFmpeg, Qt6, libsodium) to run the executables.

## Requirements

- CMake 3.22
- C++23 compiler
- FFmpeg
- libsodium
- OpenMP
- Qt6 (Core and Widgets)

## Installation

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install cmake build-essential qt6-base-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  libsodium-dev libomp-dev ffmpeg
```

### Fedora/CentOS

```bash
sudo dnf install cmake gcc-c++ qt6-qtbase-devel ffmpeg-devel libsodium-devel libgomp
```

### Arch Linux

```bash
sudo pacman -S cmake qt6-base ffmpeg libsodium openmp
```

### macOS (Homebrew)

```bash
brew install cmake qt@6 ffmpeg libsodium libomp
```

### Windows (vcpkg)

```powershell
vcpkg install ffmpeg libsodium openmp qt6
```

Or install Qt6 separately via the [Qt Online Installer](https://www.qt.io/download-qt-installer) and FFmpeg/libsodium
via vcpkg.

## Building

```bash
mkdir build
cmake -B build
cmake --build build
```

This produces two executables:

- `media_storage` — Command-line interface
- `media_storage_gui` — Graphical user interface

## Usage

### CLI

```
./media_storage encode --input <file> --output <video> [--encrypt --password <pwd>]
./media_storage decode --input <video> --output <file>
```

### GUI

```
./media_storage_gui
```

#### Single File Operations

1. **Encode a file to video**:
    - Click "Browse..." next to "Input File" to select the file you want to encode
    - Click "Browse..." next to "Output File" to choose where to save the video
    - Click "Encode to Video" to start the process

2. **Decode a video to file**:
    - Click "Browse..." next to "Input File" to select the video file
    - Click "Browse..." next to "Output File" to choose where to save the decoded file
    - Click "Decode from Video" to start the process

#### Batch Operations

1. Click "Add Files" to add multiple files to the batch queue
2. Select an output directory for all encoded videos
3. Click "Batch Encode All" to process all files in sequence

#### Monitoring

- The progress bar shows the current operation progress
- Status label displays current operation status
- Logs panel provides detailed information about each step
- All operations run in separate threads to keep the UI responsive

## Technical Details

- **Encoding**: Files are chunked, encoded with fountain codes, and embedded into video frames
- **Decoding**: Packets are extracted from video frames and reconstructed into the original file
- **Video Format**: FFV1 codec in MKV container (lossless)
- **Frame Resolution**: 3840x2160 (4K) at 30 FPS
- **Encryption**: Optional XChaCha20-Poly1305 via libsodium

## Troubleshooting

### Build Issues

- **Qt6 not found**: Ensure Qt6 development packages are installed
- **FFmpeg libraries missing**: Install FFmpeg development packages
- **libsodium missing**: Install libsodium development packages
- **OpenMP errors**: Install OpenMP development packages

### Runtime Issues

- **Cannot open input file**: Check file permissions and paths
- **Encoding fails**: Ensure sufficient disk space for output video
- **Decoding fails**: Verify the input file is a valid encoded video
- **Encode Error: failed to write header**: Make sure you have at least FFMPEG version 8 in-order to use FFV1 encoder on mp4. Otherwise, use mkv instead.

## License

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version.
