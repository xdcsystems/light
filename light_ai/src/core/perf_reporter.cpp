#include <algorithm>
#include <cstddef>

#include "core/fixed_fmt.hpp"
#include "core/perf_reporter.hpp"

void PerfReporter::frame( int src, PerfClock::time_point tp ) noexcept
{
    if ( !tracked( src ) )
        return;
    SourcePerfWindow& sp = src_[static_cast<size_t>( src )];
    double gap_ms = 0.0;
    if ( gap_[static_cast<size_t>( src )].lap( tp, gap_ms ) )
        sp.max_dq_gap_ms = std::max( sp.max_dq_gap_ms, gap_ms );
    ++sp.frames;
}

void PerfReporter::decode( int src, double ms ) noexcept
{
    if ( !tracked( src ) )
        return;
    SourcePerfWindow& sp = src_[static_cast<size_t>( src )];
    sp.dec_ms_max = std::max( sp.dec_ms_max, ms );
}

void PerfReporter::orient( int src, double ms ) noexcept
{
    if ( !tracked( src ) )
        return;
    SourcePerfWindow& sp = src_[static_cast<size_t>( src )];
    ++sp.orient_probe_cnt;
    sp.orient_probe_ms_max = std::max( sp.orient_probe_ms_max, ms );
}

void PerfReporter::source_reset( int src ) noexcept
{
    if ( src < 0 || src >= kMaxSources )
        return;
    gap_[static_cast<size_t>( src )].reset();
}

void PerfReporter::log_src_line( int n ) noexcept
{
    Log::Line line = log_.info();
    line << "[perf] src step_max=" << Fixed { step_max_.take(), 1 };
    for ( int s = 0; s < n; ++s )
    {
        SourcePerfWindow& sp = src_[static_cast<size_t>( s )];
        line << " | s" << s << " f=" << sp.frames << " dq_max=" << Fixed { sp.max_dq_gap_ms, 1 }
             << " eagain=" << sp.eagain_cnt << " dec_max=" << Fixed { sp.dec_ms_max, 1 }
             << " or_cnt=" << sp.orient_probe_cnt << " or_max=" << Fixed { sp.orient_probe_ms_max, 1 }
             << " skip=" << sp.skip_parts_cnt;
        sp.reset();
    }
    line << '\n';
}

void PerfReporter::log_enc_line( const EncPerfWindow* ep, const OverlayPerfWindow& op ) const noexcept
{
    Log::Line line = log_.info();
    line << "[perf] ";
    if ( ep != nullptr )
    {
        line << "enc admit=" << ep->enc_admit << " skip=" << ep->enc_skip
             << " idle_a=" << ep->idle_admit << " idle_s=" << ep->idle_skip
             << " dt_max=" << Fixed { ep->max_dt_enc_ms, 1 }
             << " dt_hist=[" << ep->dt_enc_hist[0] << ',' << ep->dt_enc_hist[1] << ','
             << ep->dt_enc_hist[2] << ',' << ep->dt_enc_hist[3] << ','
             << ep->dt_enc_hist[4] << ',' << ep->dt_enc_hist[5] << ']'
             << " h264_max=" << Fixed { ep->h264_ms_max, 1 }
             << " cmp_max=" << Fixed { ep->compose_ms_max, 1 }
             << " eagain=" << ep->eagain_cnt << " clamp=" << ep->pts_clamp_cnt
             << " qflush=" << ep->q_flush_cnt << " | ";
    }
    line << "ovl gpu_max=" << Fixed { op.gpu_draw_ms_max, 1 }
         << " sync_max=" << Fixed { op.sync_cpu_ms_max, 1 }
         << " ftmo=" << op.fence_tmo_cnt << '\n';
}
