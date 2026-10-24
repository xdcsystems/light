#include "net_preview.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/codec_par.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

namespace {

// The packet references the NPU tensor memory; unref must never free it.
void noop_free(void*, uint8_t*) {}

const uint8_t kRed[3]    = {255, 64, 64};
const uint8_t kGreen[3]  = {64, 255, 64};
const uint8_t kYellow[3] = {255, 255, 0};

void hline(uint8_t* img, int w, int h, int stride, int y, int x0, int x1,
           const uint8_t c[3]) {
    if (y < 0 || y >= h || x1 <= x0) return;
    uint8_t* row = img + static_cast<size_t>(y) * stride;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    for (int x = x0; x < x1; ++x) {
        uint8_t* px = row + static_cast<size_t>(x) * 3;
        px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
    }
}

void vline(uint8_t* img, int w, int h, int stride, int x, int y0, int y1,
           const uint8_t c[3]) {
    if (x < 0 || x >= w || y1 <= y0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; ++y) {
        uint8_t* px = img + static_cast<size_t>(y) * stride
                          + static_cast<size_t>(x) * 3;
        px[0] = c[0]; px[1] = c[1]; px[2] = c[2];
    }
}

void rect(uint8_t* img, int w, int h, int stride,
          int x0, int y0, int x1, int y1, const uint8_t c[3]) {
    hline(img, w, h, stride, y0,     x0, x1, c);
    hline(img, w, h, stride, y1 - 1, x0, x1, c);
    vline(img, w, h, stride, x0,     y0, y1, c);
    vline(img, w, h, stride, x1 - 1, y0, y1, c);
}

} // namespace

NetPreview::NetPreview(int w, int h, int stride, int fps, int port)
    : w_(w), h_(h), stride_(stride), fps_(fps > 0 ? fps : 25), port_(port) {
    avformat_network_init();
    pkt_ = av_packet_alloc();
    thread_ = std::thread(&NetPreview::serve, this);
}

NetPreview::~NetPreview() {
    quit_.store(true);
    {
        std::lock_guard<std::mutex> lk(mx_);
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    if (pkt_) {
        AVPacket* pkt = static_cast<AVPacket*>(pkt_);
        av_packet_free(&pkt);
        pkt_ = nullptr;
    }
    avformat_network_deinit();
}

int NetPreview::interrupt_cb(void* self) {
    return static_cast<NetPreview*>(self)->quit_.load() ? 1 : 0;
}

void NetPreview::serve() {
    std::ostringstream os;
    os << "tcp://0.0.0.0:" << port_;
    const std::string url = os.str();

    while (!quit_.load()) {
        error_.store(false);

        AVFormatContext* fc = nullptr;
        if (avformat_alloc_output_context2(&fc, nullptr, "nut", url.c_str()) < 0 || !fc) {
            std::cerr << "net: avformat_alloc_output_context2 failed\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "listen", "1", 0);
        AVIOInterruptCB cb = { &NetPreview::interrupt_cb, this };
        AVIOContext* pb = nullptr;
        // Blocks here until a viewer connects (or the interrupt fires on quit).
        const int ret = avio_open2(&pb, url.c_str(), AVIO_FLAG_WRITE, &cb, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            char err[64];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "net: waiting for viewer: " << err << '\n';
            avformat_free_context(fc);
            if (quit_.load()) break;
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
        // Newer libavformat branches do not allocate codecpar inside
        // avformat_new_stream: it must be allocated explicitly. It stays
        // owned by the stream (avformat_free_context frees it).
        if (!st->codecpar) {
            st->codecpar = avcodec_parameters_alloc();
            if (!st->codecpar) {
                std::cerr << "net: avcodec_parameters_alloc failed\n";
                avio_closep(&pb);
                avformat_free_context(fc);
                continue;
            }
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->format     = AV_PIX_FMT_RGB24;
        st->codecpar->width      = w_;
        st->codecpar->height     = h_;
        st->time_base      = AVRational{1, fps_};
        st->avg_frame_rate = AVRational{fps_, 1};
        // NUT requires an explicit fourcc for rawvideo: "RGB[24]" is the
        // canonical NUT tag for raw rgb24 (0x18424752). Without it the
        // muxer rejects write_header with "No codec tag defined".
        st->codecpar->codec_tag = MKTAG('R', 'G', 'B', 24);

        // Diagnostic: the tag must show 0x18424752 (RGB[24]). If it shows
        // 0 here, we are running a stale binary or the tag got wiped.
        std::cout << "net: write_header attempt, codec_tag=0x"
                  << std::hex << st->codecpar->codec_tag << std::dec << "\n";
        int ret2 = avformat_write_header(fc, nullptr);
        if (ret2 < 0) {
            char err[64];
            av_strerror(ret2, err, sizeof(err));
            std::cerr << "net: write_header failed: " << err << '\n';
            avio_closep(&pb);
            avformat_free_context(fc);
            continue;
        }
        std::cout << "net: viewer connected, streaming " << w_ << 'x' << h_
                  << " rgb24 (nut) on port " << port_ << '\n';

        {
            std::lock_guard<std::mutex> lk(mx_);
            fc_ = fc;
            stream_ = st;
        }
        connected_.store(true, std::memory_order_release);

        // Idle until the publisher signals error or the pipeline quits.
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [this] { return quit_.load() || error_.load(); });
        }
        connected_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(mx_);
            fc_ = nullptr;
            stream_ = nullptr;
        }
        av_write_trailer(fc);   // best effort; on a dead link it just fails
        avio_closep(&pb);
        avformat_free_context(fc);
        if (!quit_.load())
            std::cout << "net: viewer gone, waiting for a new one\n";
    }
}

void NetPreview::publish(const uint8_t* rgb) {
    if (!connected_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mx_);   // held for the write: protects fc_
    if (!fc_ || quit_.load()) return;
    auto* fc = static_cast<AVFormatContext*>(fc_);
    auto* st = static_cast<AVStream*>(stream_);
    auto* p  = static_cast<AVPacket*>(pkt_);

    // Zero-copy: data points into the NPU input tensor (dma-buf).
    AVBufferRef* ref = av_buffer_create(const_cast<uint8_t*>(rgb),
                                        stride_ * h_, noop_free, nullptr, 0);
    if (!ref) return;
    p->buf         = ref;            // packet takes over the reference
    p->data        = ref->data;
    p->size        = stride_ * h_;
    p->stream_index = st->index;
    p->pts = p->dts  = static_cast<int64_t>(pts_);
    p->duration      = 1;
    p->flags        |= AV_PKT_FLAG_KEY;
    ++pts_;

    const int ret = av_write_frame(fc, p);
    av_packet_unref(p);   // drops the buffer ref (no-op free), tensor intact
    if (ret < 0) {
        char err[64];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "net: write failed: " << err << '\n';
        error_.store(true);
        connected_.store(false, std::memory_order_release);
        cv_.notify_all();   // serve() closes and waits for a new viewer
        return;
    }
    avio_flush(fc->pb);     // push the frame out immediately (low latency)
}

void NetPreview::draw_boxes_rgb24(uint8_t* img, int w, int h, int stride,
                                  const std::vector<Detection>& dets,
                                  float model_per_scene, float lb_top) {
    for (const auto& d : dets) {
        const int x0 = static_cast<int>(d.x * model_per_scene);
        const int y0 = static_cast<int>(d.y * model_per_scene + lb_top);
        const int x1 = static_cast<int>((d.x + d.w) * model_per_scene);
        const int y1 = static_cast<int>((d.y + d.h) * model_per_scene + lb_top);
        if (x1 - x0 < 2 || y1 - y0 < 2) continue;
        const uint8_t* c = (d.cls == 0) ? kRed
                         : (d.cls == 56) ? kGreen
                         : kYellow;
        rect(img, w, h, stride, x0, y0, x1, y1, c);
    }
}

