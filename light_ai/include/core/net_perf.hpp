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
        constexpr std::array<double, 5> edges { 10.0, 30.0, 50.0, 100.0, 250.0 };
        size_t idx = 0;  // the edges ms is not below (NaN is below none: the last bucket)
        for ( const double edge : edges )
            idx += static_cast<size_t>( !( ms < edge ) );
        ++dt_enc_hist[idx];
    }

    void merge( const EncPerfWindow& o ) noexcept
    {
        enc_admit += o.enc_admit;
        enc_skip += o.enc_skip;
        idle_admit += o.idle_admit;
        idle_skip += o.idle_skip;
        for ( size_t i = 0; i < dt_enc_hist.size(); ++i )
            dt_enc_hist[i] += o.dt_enc_hist[i];
        max_dt_enc_ms = std::max( max_dt_enc_ms, o.max_dt_enc_ms );
        h264_ms_max = std::max( h264_ms_max, o.h264_ms_max );
        compose_ms_max = std::max( compose_ms_max, o.compose_ms_max );
        eagain_cnt += o.eagain_cnt;
        pts_clamp_cnt += o.pts_clamp_cnt;
        q_flush_cnt += o.q_flush_cnt;
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
    void merge( const OverlayPerfWindow& o ) noexcept
    {
        gpu_draw_ms_max = std::max( gpu_draw_ms_max, o.gpu_draw_ms_max );
        sync_cpu_ms_max = std::max( sync_cpu_ms_max, o.sync_cpu_ms_max );
        fence_tmo_cnt += o.fence_tmo_cnt;
    }

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
        constexpr std::array<double, 3> edges { 5.0, 20.0, 100.0 };
        size_t idx = 0;  // the edges ms is above (NaN is at none: the last bucket)
        for ( const double edge : edges )
            idx += static_cast<size_t>( !( ms <= edge ) );
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

struct ReidPerfWindow
{
    void merge( const ReidPerfWindow& o ) noexcept
    {
        crops += o.crops;
        drops += o.drops;
        matches += o.matches;
        ms_max = std::max( ms_max, o.ms_max );
    }

    void reset() noexcept { *this = ReidPerfWindow {}; }

public:
    uint32_t crops = 0;
    uint32_t drops = 0;
    uint32_t matches = 0;
    double ms_max = 0.0;
};
