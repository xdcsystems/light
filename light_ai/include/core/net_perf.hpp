// Zero-heap POD telemetry windows for 5-second live-stream [perf] summaries
// (--net active only). SDK/GL/OS-free.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

struct SourcePerfWindow
{
    void reset() noexcept { *this = SourcePerfWindow {}; }

public:
    double max_dq_gap_ms = 0.0;
    double dec_ms_max = 0.0;
    double orient_probe_ms_max = 0.0;
    uint32_t eagain_cnt = 0;
    uint32_t orient_probe_cnt = 0;
    uint32_t skip_parts_cnt = 0;
    uint32_t frames = 0;
};

struct EncPerfWindow
{
    // Buckets: <10, 10-30, 30-50, 50-100, 100-250, >=250 ms.
    void add_dt_enc( double ms ) noexcept
    {
        max_dt_enc_ms = std::max( max_dt_enc_ms, ms );
        size_t idx = 5;
        if ( ms < 10.0 )
            idx = 0;
        else if ( ms < 30.0 )
            idx = 1;
        else if ( ms < 50.0 )
            idx = 2;
        else if ( ms < 100.0 )
            idx = 3;
        else if ( ms < 250.0 )
            idx = 4;
        ++dt_enc_hist[idx];
    }

    void reset() noexcept { *this = EncPerfWindow {}; }

public:
    uint32_t enc_admit = 0;
    uint32_t enc_skip = 0;
    uint32_t idle_admit = 0;
    uint32_t idle_skip = 0;
    std::array<uint32_t, 6> dt_enc_hist {};
    double max_dt_enc_ms = 0.0;
    double h264_ms_max = 0.0;
    double compose_ms_max = 0.0;
    uint32_t eagain_cnt = 0;
    uint32_t pts_clamp_cnt = 0;
    uint32_t q_flush_cnt = 0;
};

struct OverlayPerfWindow
{
    void reset() noexcept { *this = OverlayPerfWindow {}; }

public:
    double gpu_draw_ms_max = 0.0;
    double sync_cpu_ms_max = 0.0;
    uint32_t fence_tmo_cnt = 0;
};

struct MuxPerfWindow
{
    // Buckets: <=5, 5-20, 20-100, >100 ms.
    void add_mux_write( double ms ) noexcept
    {
        size_t idx = 3;
        if ( ms <= 5.0 )
            idx = 0;
        else if ( ms <= 20.0 )
            idx = 1;
        else if ( ms <= 100.0 )
            idx = 2;
        ++mux_write_hist[idx];
    }

    void reset() noexcept { *this = MuxPerfWindow {}; }

public:
    double pop_wait_ms_max = 0.0;
    std::array<uint32_t, 4> mux_write_hist {};
    uint32_t wait_writable_cnt = 0;
    uint32_t pkt_q_max_depth = 0;
    uint32_t pkts = 0;
};
