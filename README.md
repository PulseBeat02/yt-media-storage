[![CodeFactor](https://www.codefactor.io/repository/github/pulsebeat02/yt-media-storage/badge)](https://www.codefactor.io/repository/github/pulsebeat02/yt-media-storage)
[![TeamCity Full Build Status](https://img.shields.io/teamcity/build/s/YtMediaStorage_Build?server=https%3A%2F%2Fci.brandonli.me
)](https://ci.brandonli.me/project/YtMediaStorage)

# Media Storage

**Explanation Video**: 
- https://youtu.be/l03Os5uwWmk?si=xgZPNMrvs_aDcWE5  
- https://youtu.be/Qmds7-mwCMg?si=04TzoLp7p-LdRyLX
**YC Hacker News**: https://news.ycombinator.com/item?id=47012964

Stores files onto YouTube by encoding them into lossless video and decoding them back to the original file. Supports
both a command-line interface and a graphical user interface.

## Features

- **File Encoding/Decoding**: Encode any file into a lossless video (FFV1/MKV) and then decode it back to the original
  file
- **Live Streaming**: Stream-encode files to Twitch/YouTube via RTMP (H.264) and decode them back from the stream or VOD
- **Fountain Codes**: Uses [Wirehair](https://github.com/catid/wirehair) fountain codes for redundancy and repair
- **Optional Encryption**: Encrypt files with a password using libsodium (XChaCha20-Poly1305)
- **Selectable Checksum**: Choose between CRC32 (default) and [xxHash32](https://github.com/Cyan4973/xxHash) for packet
  integrity verification
- **Batch Processing**: Queue multiple files for batch encoding (GUI)
- **Progress Tracking**: Real-time progress bars and status updates (GUI)

## CI/CD Pipeline

Visit my [CI/CD pipeline](https://ci.brandonli.me), and click "Login as Guest". Visit the yt-media-storage project,
click on the latest passing build, and click "Artifacts" to download the latest build artifacts for both the CLI and
GUI. You may need to install some shared libraries (FFmpeg, Qt6, libsodium) to run the executables.

## Requirements

- CMake 3.22
- C++23 compiler
- FFmpeg (with libx264 for streaming)
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
vcpkg install ffmpeg libsodium openmp qt6 gtest
```

Or install Qt6 separately via the [Qt Online Installer](https://www.qt.io/download-qt-installer) and FFmpeg/libsodium
via vcpkg.

## Building

```bash
cmake -B build
cmake --build build
```

This produces two executables and one shared library:

- `media_storage` — Command-line interface
- `media_storage_gui` — Graphical user interface
- `libmedia_storage.so` / `media_storage.dll` — Embeddable shared library

## Testing

Tests use [Google Test](https://github.com/google/googletest).

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

Or run the test binary directly for verbose output:

```bash
./build/tests/media_storage_tests
```

## Usage

### CLI

#### Lossless Video (Local Files)

```
./media_storage encode --input <file> --output <video> [--encrypt --password <pwd>] [--hash <crc32|xxhash>]
./media_storage decode --input <video> --output <file> [--password <pwd>]
```

#### Live Streaming (Twitch / YouTube)

```
./media_storage stream-encode --input <file> --url <rtmp://...> [--bitrate <kbps>] [--width <w> --height <h>] [--encrypt --password <pwd>]
./media_storage stream-decode --url <stream_url> --output <file> [--password <pwd>]
```

Stream-decode supports a 30-second retry window, so you can start it before the encoder begins streaming.

**Example — stream to Twitch at 1080p:**

```bash
# Terminal 1: start decoder first (waits for stream)
./media_storage stream-decode --url stream_playback_url --output decoded.bin

# Terminal 2: start encoder
# You must use yt-dlp to get the raw Twitch or YouTube Stream URL
./media_storage stream-encode --input myfile.bin --url rtmp://... --width 1920 --height 1080 --bitrate 8000
```

#### Options

| Flag         | Short | Description                                                     |
|--------------|-------|-----------------------------------------------------------------|
| `--input`    | `-i`  | Input file path (required for encode)                           |
| `--output`   | `-o`  | Output file path (required for decode)                          |
| `--url`      | `-u`  | RTMP stream URL (streaming only)                                |
| `--bitrate`  | `-b`  | Stream bitrate in kbps (default: 8000 for 1080p)                |
| `--width`    |       | Stream video width (default: 1920)                              |
| `--height`   |       | Stream video height (default: 1080)                             |
| `--encrypt`  | `-e`  | Enable encryption (encode only)                                 |
| `--password` | `-p`  | Password for encryption/decryption                              |
| `--hash`     | `-H`  | Checksum algorithm: `crc32` (default) or `xxhash` (encode only) |

The checksum algorithm is embedded in each packet's flags, so `decode` automatically detects which algorithm was used —
no need to specify it again.

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

#### Streaming

1. Select a platform (Twitch, YouTube, or Custom)
2. Enter your stream key
3. Choose a resolution (1080p, 1440p, or 4K), and change bitrate if needed
4. Select an input file and click "Stream Encode" to start streaming
5. To decode a stream, enter the stream URL and click "Stream Decode"

#### Monitoring

- The progress bar shows the current operation progress
- Status label displays current operation status
- Logs panel provides detailed information about each step
- All operations run in separate threads to keep the UI responsive

### Library (Embedding in Your App)

The shared library exposes a C API (`media_storage.h`) so you can integrate encoding/decoding. Link against
`libmedia_storage` and include the header:

```c
#include <media_storage.h>

int progress(uint64_t current, uint64_t total, void *user) {
    printf("Progress: %llu / %llu\n", current, total);
    return 0; // return non-zero to cancel
}

int main(void) {
    ms_encode_options_t opts = {0};
    opts.input_path  = "myfile.bin";
    opts.output_path = "myfile.mkv";
    opts.hash_algorithm = MS_HASH_CRC32;
    opts.progress = progress;

    ms_result_t result;
    ms_status_t status = ms_encode(&opts, &result);
    if (status != MS_OK) {
        fprintf(stderr, "Error: %s\n", ms_status_string(status));
        return 1;
    }

    printf("Encoded %llu bytes -> %llu bytes (%llu frames)\n",
           result.input_size, result.output_size, result.total_frames);
    return 0;
}
```

#### CMake Integration

After installing with `cmake --install build`, use `find_package`:

```cmake
find_package(media_storage REQUIRED)
target_link_libraries(your_app PRIVATE media_storage::media_storage_lib)
```

Or add this repository as a subdirectory:

```cmake
add_subdirectory(media-storage)
target_link_libraries(your_app PRIVATE media_storage_lib)
```

## Technical Details

- **Encoding**: Files are chunked, encoded with fountain codes, and embedded into video frames
- **Decoding**: Packets are extracted from video frames and reconstructed into the original file
- **Lossless Format**: FFV1 codec in MKV container at 3840x2160 (4K), 30 FPS
- **Streaming Format**: H.264 (libx264) in FLV container via RTMP, with configurable resolution and bitrate
- **Encryption**: Optional XChaCha20-Poly1305 via libsodium
- **Checksums**: CRC32-MPEG2 (default) or xxHash32 per packet; algorithm is stored in the packet flags for
  self-describing decode

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
- **Encode Error: failed to write header**: Make sure you have at least FFMPEG version 8 in-order to use FFV1 encoder on
  mp4. Otherwise, use mkv instead.

### Memory Usage

- **Encoding** streams chunks and uses only a few MB of RAM regardless of input file size.
- **Decoding** holds all decoded chunks in memory before writing the output file.

## License

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version.
