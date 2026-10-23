#define MODULE_TAG "light_ai"

#include "mpp_decoder.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

MppDecoder::MppDecoder() {
    do {
        if (mpp_create(&ctx_, &mpi_) != MPP_OK) {
            fprintf(stderr, "mpp_create failed\n");
            break;
        }
        if (mpp_init(ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
            fprintf(stderr, "mpp_init failed\n");
            break;
        }

        // Пул для входных пакетов (DRM: декодер читает пакет железом напрямую)
        if (mpp_buffer_group_get(&pkt_group_, MPP_BUFFER_TYPE_DRM,
                                 MPP_BUFFER_INTERNAL, MODULE_TAG, NULL) != MPP_OK) {
            fprintf(stderr, "mpp_buffer_group_get pkt failed\n");
            break;
        }

        // Пул для выходных кадров. Поток камеры 4:2:2, поэтому буфер x2, не x1.5
        if (mpp_buffer_group_get(&out_group_, MPP_BUFFER_TYPE_DRM,
                                 MPP_BUFFER_INTERNAL, MODULE_TAG, NULL) != MPP_OK) {
            fprintf(stderr, "mpp_buffer_group_get failed\n");
            break;
        }

        MppBuffer buf = nullptr;
        if (mpp_buffer_get(out_group_, &buf, 640 * 480 * 2) != MPP_OK) {
            fprintf(stderr, "mpp_buffer_get failed\n");
            break;
        }
        mpp_frame_init(&out_frame_);
        mpp_frame_set_width(out_frame_, 640);
        mpp_frame_set_height(out_frame_, 480);
        mpp_frame_set_fmt(out_frame_, MPP_FMT_YUV422SP);
        mpp_frame_set_buffer(out_frame_, buf);
        mpp_buffer_put(buf);
        ok_ = true;
    } while (0);

    if (!ok_) {
        if (out_frame_) { mpp_frame_deinit(&out_frame_); out_frame_ = nullptr; }
        if (out_group_) { mpp_buffer_group_put(out_group_); out_group_ = nullptr; }
        if (pkt_group_) { mpp_buffer_group_put(pkt_group_); pkt_group_ = nullptr; }
        if (ctx_) { mpp_destroy(ctx_); ctx_ = nullptr; mpi_ = nullptr; }
    }
}

MppDecoder::~MppDecoder() {
    if (!ctx_) return;
    mpi_->reset(ctx_);
    mpp_destroy(ctx_);
    ctx_ = nullptr; mpi_ = nullptr;
    if (pkt_)       { mpp_packet_deinit(&pkt_); pkt_ = nullptr; }   // сначала пакет
    if (out_frame_) { mpp_frame_deinit(&out_frame_); out_frame_ = nullptr; }
    if (out_group_) { mpp_buffer_group_put(out_group_); out_group_ = nullptr; }
    if (pkt_group_) { mpp_buffer_group_put(pkt_group_); pkt_group_ = nullptr; }  // потом группы
}


int MppDecoder::decode(const uint8_t* data, size_t size) {
    static int seq = 0;
    const bool verbose = (seq < 5);

    if (!ok_ || !data || size == 0) {
        fprintf(stderr, "dec[%d]: BAD INPUT ok=%d data=%p size=%zu\n",
                seq, (int)ok_, (const void*)data, size);
        ++seq;
        return -1;
    }
    ++seq;

    if (pkt_) { mpp_packet_deinit(&pkt_); pkt_ = nullptr; }

    MppBuffer pbuf = nullptr;
    if (mpp_buffer_get(pkt_group_, &pbuf, size) != MPP_OK) {
        fprintf(stderr, "dec: pkt buffer alloc failed (%zu)\n", size);
        return -2;
    }
    memcpy(mpp_buffer_get_ptr(pbuf), data, size);
    mpp_packet_init_with_buffer(&pkt_, pbuf);   // пакет держит свою ссылку на буфер
    mpp_buffer_put(pbuf);                       // свою ссылку отпускаем
    mpp_packet_set_pts(pkt_, ++pts_);

    // Вход: ждём свободную задачу
    if (mpi_->poll(ctx_, MPP_PORT_INPUT, MPP_POLL_BLOCK) != MPP_OK) {
        fprintf(stderr, "dec: no free input task\n");
        return -3;
    }
    MppTask task = nullptr;
    if (mpi_->dequeue(ctx_, MPP_PORT_INPUT, &task) != MPP_OK || !task) {
        fprintf(stderr, "dec: input dequeue failed\n");
        return -4;
    }
    mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, pkt_);
    mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, out_frame_);

    MPP_RET ret = mpi_->enqueue(ctx_, MPP_PORT_INPUT, task);
    if (verbose) fprintf(stderr, "dec[%d]: in enq=%d\n", seq - 1, ret);
    if (ret != MPP_OK) {
        fprintf(stderr, "dec: input enqueue ret=%d\n", ret);
        return -5;
    }

    // Выход: ждём готовый кадр
    if (mpi_->poll(ctx_, MPP_PORT_OUTPUT, MPP_POLL_BLOCK) != MPP_OK) {
        fprintf(stderr, "dec: output not ready\n");
        return -6;
    }
    task = nullptr;
    if (mpi_->dequeue(ctx_, MPP_PORT_OUTPUT, &task) != MPP_OK || !task) {
        fprintf(stderr, "dec: output dequeue failed\n");
        return -7;
    }

    int fd = -1;
    MppFrame f = nullptr;
    mpp_task_meta_get_frame(task, KEY_OUTPUT_FRAME, &f);

    if (!f) {
        fprintf(stderr, "dec: output task WITHOUT frame in meta\n");
    } else if (mpp_frame_get_info_change(f)) {
        uint32_t w = mpp_frame_get_width(f);
        uint32_t h = mpp_frame_get_height(f);
        size_t   bs = mpp_frame_get_buf_size(f);
        fprintf(stderr, "dec: info_change %ux%u buf_size=%zu\n", w, h, bs);

        // Задачу возвращаем ДО того, как трогаем out_frame_
        mpi_->enqueue(ctx_, MPP_PORT_OUTPUT, task);

        mpp_frame_deinit(&out_frame_);
        out_frame_ = nullptr;
        mpp_frame_init(&out_frame_);
        mpp_frame_set_width(out_frame_, w);
        mpp_frame_set_height(out_frame_, h);
        mpp_frame_set_fmt(out_frame_, MPP_FMT_YUV422SP);
        MppBuffer b = nullptr;
        if (mpp_buffer_get(out_group_, &b, bs) != MPP_OK) {
            fprintf(stderr, "dec: alloc %zu bytes FAILED\n", bs);
            mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
            return -8;
        }
        mpp_frame_set_buffer(out_frame_, b);
        mpp_buffer_put(b);

        mpi_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        return -1;  // сам кадр info_change пропускаем — это норма
    } else if (mpp_frame_get_errinfo(f) || mpp_frame_get_discard(f)) {
        fprintf(stderr, "dec: DECODE-ERROR errinfo=0x%x discard=%d fmt=%d\n",
                mpp_frame_get_errinfo(f), mpp_frame_get_discard(f),
                (int)mpp_frame_get_fmt(f));
    } else {
        MppBuffer buf = mpp_frame_get_buffer(f);
        if (buf) {
            fd = mpp_buffer_get_fd(buf);
            if (verbose)
                fprintf(stderr, "dec: frame %ux%u fd=%d fmt=%d "
                                "[420SP=%d 422SP=%d YUYV=%d UYVY=%d 420P=%d 422P=%d]\n",
                        mpp_frame_get_width(f), mpp_frame_get_height(f),
                        fd, (int)mpp_frame_get_fmt(f),
                        (int)MPP_FMT_YUV420SP, (int)MPP_FMT_YUV422SP,
                        (int)MPP_FMT_YUV422_YUYV, (int)MPP_FMT_YUV422_UYVY,
                        (int)MPP_FMT_YUV420P, (int)MPP_FMT_YUV422P);
        } else {
            fprintf(stderr, "dec: frame WITHOUT buffer\n");
        }
    }

    mpi_->enqueue(ctx_, MPP_PORT_OUTPUT, task);
    return fd;
}

