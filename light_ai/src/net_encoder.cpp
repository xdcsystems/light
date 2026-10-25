#include "net_encoder.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

#include <im2d.h>
#include <rga.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

namespace {

void noop_free(void*, uint8_t*) {}

} // namespace

NetEncoder::NetEncoder(int w, int h, int fps, int port,
                       const std::string& host, int bitrate_kbps)
    : w_(w), h_(h), fps_(fps > 0 ? fps : 25), port_(port),
      bitrate_kbps_(bitrate_kbps > 0 ? bitrate_kbps : 12000), host_(host) {
    avformat_network_init();

    // Two NV12 dma-bufs from an MPP DRM pool: ping-pong so a plane held by
    // the encoder is never overwritten by RGA on the next frame.
    if (mpp_buffer_group_get(&buf_group_, MPP_BUFFER_TYPE_DRM,
                             MPP_BUFFER_INTERNAL, nullptr, nullptr) != MPP_OK) {
        throw std::runtime_error("net encoder: mpp_buffer_group_get failed");
    }
    const size_t bytes = static_cast<size_t>(w_) * h_ * 3 / 2;
    for (auto& b : bufs_) {
        if (mpp_buffer_get(buf_group_, &b, bytes) != MPP_OK) {
            mpp_buffer_group_put(buf_group_);
            buf_group_ = nullptr;
            throw std::runtime_error("net encoder: mpp_buffer_get failed");
        }
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("h264_rkmpp");
    if (!codec) {
        throw std::runtime_error("net encoder: h264_rkmpp not found in this ffmpeg");
    }
    auto* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        throw std::runtime_error("net encoder: avcodec_alloc_context3 failed");
    }
    enc_ = ctx;
    ctx->width      = w_;
    ctx->height     = h_;
    ctx->pix_fmt    = AV_PIX_FMT_NV12;
    ctx->time_base  = AVRational{1, fps_};
    ctx->framerate  = AVRational{fps_, 1};
    ctx->bit_rate   = static_cast<int64_t>(bitrate_kbps_) * 1000;
    ctx->gop_size   = fps_;      // 1 s: a late joiner waits at most one GOP
    ctx->max_b_frames = 0;       // no reorder: minimal latency
    // Non-fatal: option names differ between ffmpeg-rockchip builds.
    if (av_opt_set(ctx, "rc_mode", "CBR", 0) < 0) {
        std::cout << "enc: rc_mode option not accepted, encoder default stays\n";
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        throw std::runtime_error("net encoder: avcodec_open2(h264_rkmpp) failed");
    }
    std::cout << "enc: h264_rkmpp open: " << w_ << 'x' << h_ << ", "
              << bitrate_kbps_ << " kbps, gop " << ctx->gop_size << '\n';

    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!frame_ || !pkt_) {
        throw std::runtime_error("net encoder: av_frame/av_packet_alloc failed");
    }

    thread_ = std::thread(&NetEncoder::serve, this);
}

NetEncoder::~NetEncoder() {
    quit_.store(true);
    {
        std::lock_guard<std::mutex> lk(mx_);
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    if (enc_)    avcodec_free_context(reinterpret_cast<AVCodecContext**>(&enc_));
    if (frame_)  av_frame_free(reinterpret_cast<AVFrame**>(&frame_));
    if (pkt_)    av_packet_free(reinterpret_cast<AVPacket**>(&pkt_));
    for (auto& b : bufs_) {
        if (b) { mpp_buffer_put(b); b = nullptr; }
    }
    if (buf_group_) { mpp_buffer_group_put(buf_group_); buf_group_ = nullptr; }
    avformat_network_deinit();
}

void NetEncoder::serve() {
    std::ostringstream os;
    os << "udp://" << host_ << ':' << port_ << "?pkt_size=1316";
    const std::string url = os.str();

    while (!quit_.load()) {
        error_.store(false);

        AVFormatContext* fc = nullptr;
        if (avformat_alloc_output_context2(&fc, nullptr, "mpegts", url.c_str()) < 0 || !fc) {
            std::cerr << "enc: avformat_alloc_output_context2 failed\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        AVIOContext* pb = nullptr;
        // UDP: no handshake, no listener — the socket is opened once and
        // packets are pushed whether a viewer listens or not.
        const int ret = avio_open(&pb, url.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char err[64];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "enc: udp open failed: " << err << '\n';
            avformat_free_context(fc);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        fc->pb = pb;

        AVStream* st = avformat_new_stream(fc, nullptr);
        if (!st) {
            avio_closep(&pb);
            avformat_free_context(fc);
            continue;
        }
        if (!st->codecpar) {
            st->codecpar = avcodec_parameters_alloc();
            if (!st->codecpar) {
                avio_closep(&pb);
                avformat_free_context(fc);
                continue;
            }
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id   = AV_CODEC_ID_H264;
        st->codecpar->format     = AV_PIX_FMT_NV12;
        st->codecpar->width      = w_;
        st->codecpar->height     = h_;
        st->codecpar->bit_rate   = static_cast<AVCodecContext*>(enc_)->bit_rate;
        st->time_base      = AVRational{1, fps_};
        st->avg_frame_rate = AVRational{fps_, 1};

        int ret2 = avformat_write_header(fc, nullptr);
        if (ret2 < 0) {
            char err[64];
            av_strerror(ret2, err, sizeof(err));
            std::cerr << "enc: write_header failed: " << err << '\n';
            avio_closep(&pb);
            avformat_free_context(fc);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        const AVRational tb = st->time_base;
        frame_step_ = av_rescale_q(1, AVRational{1, fps_}, tb);
        if (frame_step_ < 1) frame_step_ = 1;
        std::cout << "enc: stream time_base " << tb.num << '/' << tb.den
                  << ", pts step " << frame_step_ << "\n";
        std::cout << "enc: udp streaming " << w_ << 'x' << h_
                  << " h264 (mpegts) -> " << host_ << ':' << port_ << '\n';

        {
            std::lock_guard<std::mutex> lk(mx_);
            fc_ = fc;
            st_ = st;
            pts_ = 0;
        }

        // Park until quit or a write error; UDP writes do not block on a
        // dead viewer, so a drop just falls out on the next write failure.
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [this] { return quit_.load() || error_.load(); });
        }
        {
            std::lock_guard<std::mutex> lk(mx_);
            fc_ = nullptr;
            st_ = nullptr;
        }
        av_write_trailer(fc);   // best effort
        avio_closep(&pb);
        avformat_free_context(fc);
        if (!quit_.load()) {
            std::cout << "enc: udp path reset, reopening in 500 ms\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void NetEncoder::publish(int tensor_fd) {
    if (quit_.load()) return;
    std::lock_guard<std::mutex> lk(mx_);
    if (!fc_ || !enc_) return;
    tensor_fd_ = tensor_fd;   // RGA source for this frame
    if (!send_frame(fc_, st_)) {
        error_.store(true);
        cv_.notify_all();   // serve() closes and reopens the UDP path
    }
}

bool NetEncoder::send_frame(void* fcp, void* stp) {
    auto* fc = static_cast<AVFormatContext*>(fcp);
    auto* st = static_cast<AVStream*>(stp);
    auto* ctx = static_cast<AVCodecContext*>(enc_);

    // Ping-pong: RGA never writes into a plane the previous encode still
    // holds. The 7th improcess argument is the usage mask, not a sync
    // flag (IM_HAL_TRANSFORM_ROT_90 = 1<<0); 0 means "no transform".
    // Single-op calls run synchronously (im2d_single.h: sync defaults
    // to 1), so the encoder reads a finished buffer.
    cur_ ^= 1u;
    MppBuffer mb = bufs_[cur_];

    rga_buffer_t src = wrapbuffer_fd(tensor_fd_, w_, h_, RK_FORMAT_RGB_888);
    rga_buffer_t dst = wrapbuffer_fd(mpp_buffer_get_fd(mb), w_, h_,
                                     RK_FORMAT_YCbCr_420_SP);
    const IM_STATUS rs = improcess(src, dst, {}, {}, {}, {}, 0);
    if ((int)rs <= 0) {
        std::cerr << "enc: RGA RGB->NV12 failed: " << imStrError(rs) << '\n';
        return false;
    }

    AVFrame* fr = static_cast<AVFrame*>(frame_);
    uint8_t* base = static_cast<uint8_t*>(mpp_buffer_get_ptr(mb));
    fr->format = AV_PIX_FMT_NV12;
    fr->width  = w_;
    fr->height = h_;
    fr->data[0] = base;
    fr->data[1] = base + static_cast<size_t>(w_) * h_;
    fr->linesize[0] = w_;
    fr->linesize[1] = w_;
    fr->pts = static_cast<int64_t>(pts_);
    fr->buf[0] = av_buffer_create(base,
                                  static_cast<size_t>(w_) * h_ * 3 / 2,
                                  noop_free, nullptr, 0);
    if (!fr->buf[0]) {
        std::cerr << "enc: av_buffer_create failed\n";
        return false;
    }

    int ret = avcodec_send_frame(ctx, fr);
    av_frame_unref(fr);   // drop the buffer ref; the tensor/MPP memory stays
    if (ret < 0) {
        char err[64];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "enc: send_frame: " << err << '\n';
        return false;
    }

    AVPacket* pkt = static_cast<AVPacket*>(pkt_);
    for (;;) {
        ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char err[64];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "enc: receive_packet: " << err << '\n';
            return false;
        }
        pkt->stream_index = st->index;
        pkt->pts = pkt->dts = static_cast<int64_t>(pts_ * frame_step_);
        const int r = av_interleaved_write_frame(fc, pkt);
        av_packet_unref(pkt);
        if (r < 0) {
            char err[64];
            av_strerror(r, err, sizeof(err));
            std::cerr << "enc: write failed: " << err << '\n';
            return false;
        }
    }
    ++pts_;
    return true;
}
