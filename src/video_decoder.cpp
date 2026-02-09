#include "video_decoder.h"
#include "video_encoder.h"
#include "configuration.h"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

constexpr double PI = 3.14159265358979323846;

static const auto &get_cosine_table() {
    static std::array<std::array<double, 8>, 8> table = []() {
        std::array<std::array<double, 8>, 8> t{};
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                t[i][j] = std::cos((2.0 * i + 1.0) * j * PI / 16.0);
            }
        }
        return t;
    }();
    return table;
}

static double alpha(const int u) {
    return u == 0 ? 1.0 / std::sqrt(2.0) : 1.0;
}

static void forward_dct_8x8(const double input[8][8], double output[8][8]) {
    const auto &cos_table = get_cosine_table();
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            double sum = 0.0;
            for (int x = 0; x < 8; ++x) {
                for (int y = 0; y < 8; ++y) {
                    sum += input[x][y] * cos_table[x][u] * cos_table[y][v];
                }
            }
            output[u][v] = 0.25 * alpha(u) * alpha(v) * sum;
        }
    }
}


static constexpr std::pair<int, int> EMBED_POSITIONS[] = {
    {0, 1},
    {1, 0},
    {1, 1},
    {0, 2},
};

VideoDecoder::VideoDecoder(const std::string &input_path) {
    init_decoder(input_path);
}

VideoDecoder::~VideoDecoder() {
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (av_packet_) av_packet_free(&av_packet_);
    if (gray_frame_) av_frame_free(&gray_frame_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (format_ctx_) avformat_close_input(&format_ctx_);
}

void VideoDecoder::init_decoder(const std::string &input_path) {
    int ret = avformat_open_input(&format_ctx_, input_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open input file");
    }

    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to find stream info");
    }

    for (unsigned i = 0; i < format_ctx_->nb_streams; ++i) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_index_ < 0) {
        throw std::runtime_error("No video stream found");
    }

    const AVStream *stream = format_ctx_->streams[video_stream_index_];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        throw std::runtime_error("Failed to find decoder");
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        throw std::runtime_error("Failed to allocate codec context");
    }

    ret = avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to open codec");
    }

    frame_ = av_frame_alloc();
    gray_frame_ = av_frame_alloc();
    av_packet_ = av_packet_alloc();

    if (!frame_ || !gray_frame_ || !av_packet_) {
        throw std::runtime_error("Failed to allocate frame/packet");
    }

    gray_buffer_.resize(static_cast<std::size_t>(FRAME_WIDTH) * FRAME_HEIGHT);

    gray_frame_->format = AV_PIX_FMT_GRAY8;
    gray_frame_->width = codec_ctx_->width;
    gray_frame_->height = codec_ctx_->height;

    ret = av_frame_get_buffer(gray_frame_, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to allocate gray frame buffer");
    }

    sws_ctx_ = sws_getContext(
        codec_ctx_->width, codec_ctx_->height, codec_ctx_->pix_fmt,
        codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_GRAY8,
        SWS_POINT, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
        throw std::runtime_error("Failed to create swscale context");
    }
}

int64_t VideoDecoder::total_frames() const {
    if (video_stream_index_ >= 0) {
        const AVStream *stream = format_ctx_->streams[video_stream_index_];
        if (stream->nb_frames > 0) {
            return stream->nb_frames;
        }
        if (stream->duration > 0 && stream->time_base.den > 0) {
            const double duration_sec = static_cast<double>(stream->duration) *
                                        av_q2d(stream->time_base);
            return static_cast<int64_t>(duration_sec * FRAME_FPS);
        }
    }
    return -1;
}

std::vector<std::byte> VideoDecoder::extract_data_from_frame() const {
    const auto layout = compute_frame_layout();

    std::vector<std::byte> data;
    data.reserve(layout.bytes_per_frame);

    uint8_t current_byte = 0;
    int bit_count = 0;
    for (int block_row = 0; block_row < layout.blocks_per_col; ++block_row) {
        for (int block_col = 0; block_col < layout.blocks_per_row; ++block_col) {
            double block[8][8];
            const int base_x = block_col * 8;
            const int base_y = block_row * 8;

            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    block[y][x] = static_cast<double>(gray_buffer_[(base_y + y) * FRAME_WIDTH + base_x + x]);
                }
            }

            double dct[8][8];
            forward_dct_8x8(block, dct);
            for (int b = 0; b < BITS_PER_BLOCK; ++b) {
                const auto [coef_y, coef_x] = EMBED_POSITIONS[b];
                const int bit = dct[coef_y][coef_x] > 0 ? 1 : 0;
                current_byte = (current_byte << 1) | bit;
                ++bit_count;
                if (bit_count == 8) {
                    data.push_back(static_cast<std::byte>(current_byte));
                    current_byte = 0;
                    bit_count = 0;
                }
            }
        }
    }

    return data;
}

std::vector<std::vector<std::byte> > VideoDecoder::extract_packets_from_frame() const {
    const auto raw_data = extract_data_from_frame();
    std::vector<std::vector<std::byte> > packets;

    constexpr std::size_t packet_size = HEADER_SIZE + SYMBOL_SIZE_BYTES;
    std::size_t offset = 0;
    while (offset + packet_size <= raw_data.size()) {
        if (offset + 4 <= raw_data.size()) {
            uint32_t magic = 0;
            std::memcpy(&magic, raw_data.data() + offset, sizeof(magic));
            if (magic != MAGIC_ID) {
                break;
            }
        }

        std::vector packet(raw_data.begin() + offset,
                           raw_data.begin() + offset + packet_size);
        packets.push_back(std::move(packet));
        offset += packet_size;
    }

    return packets;
}

std::vector<std::vector<std::byte> > VideoDecoder::decode_next_frame() {
    if (eof_) {
        return {};
    }

    while (av_read_frame(format_ctx_, av_packet_) >= 0) {
        if (av_packet_->stream_index != video_stream_index_) {
            av_packet_unref(av_packet_);
            continue;
        }

        int ret = avcodec_send_packet(codec_ctx_, av_packet_);
        av_packet_unref(av_packet_);

        if (ret < 0) {
            continue;
        }

        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret == AVERROR_EOF) {
            eof_ = true;
            return {};
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving frame");
        }

        sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height,
                  gray_frame_->data, gray_frame_->linesize);
        for (int y = 0; y < frame_->height; ++y) {
            std::memcpy(gray_buffer_.data() + y * FRAME_WIDTH,
                        gray_frame_->data[0] + y * gray_frame_->linesize[0],
                        FRAME_WIDTH);
        }

        ++frame_index_;

        return extract_packets_from_frame();
    }

    eof_ = true;
    return {};
}

std::vector<std::vector<std::byte> > VideoDecoder::decode_all_frames() {
    std::vector<std::vector<std::byte> > results;
    while (!eof_) {
        for (auto packets = decode_next_frame(); auto &pkt: packets) {
            results.push_back(std::move(pkt));
        }
    }
    return results;
}
