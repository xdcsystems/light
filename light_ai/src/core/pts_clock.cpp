#include <algorithm>

#include "core/pts_clock.hpp"

namespace
{
    int64_t rebase_or_track_vfr( int64_t ts_us, int64_t last_pts, int64_t& anchor_pts, bool& flush_pending,
                                 int64_t& base_us, int64_t& last_step_90k ) noexcept
    {
        if ( base_us < 0 )
            base_us = ts_us;
        const int64_t p = us_to_90k( ts_us - base_us );
        if ( last_pts < 0 )
            return p;
        if ( flush_pending )
        {
            flush_pending = false;
            const int64_t anchor = anchor_pts >= 0 ? anchor_pts : last_pts;
            const int64_t max_target = std::max( last_pts + 1, anchor + last_step_90k );
            if ( p <= max_target )
                return p;
            base_us = ts_us - ( max_target * 100 + 4 ) / 9;
            return max_target;
        }
        anchor_pts = -1;
        const int64_t dt = p - last_pts;
        if ( dt >= PtsClock::kMinStep90k && dt <= PtsClock::kMaxStep90k )
            last_step_90k = dt;
        return p;
    }
}  // namespace

// us * 90000 / 1000000 = us * 9 / 100; adding half the divisor rounds to
// nearest, and the negative branch mirrors it (halves away from zero).
int64_t us_to_90k( int64_t us ) noexcept
{
    return us >= 0 ? ( us * 9 + 50 ) / 100 : -( ( -us * 9 + 50 ) / 100 );
}

void PtsClock::restart( int64_t frame_step ) noexcept
{
    counter_ = 0;
    base_us_ = -1;
    last_pts_ = -1;
    last_step_90k_ = kDefaultStep90k;
    flush_anchor_pts_ = -1;
    flush_pending_ = false;
    frame_step_ = std::max<int64_t>( 1, frame_step );
}

void PtsClock::on_queue_flush() noexcept
{
    if ( !vfr_ || last_pts_ < 0 )
        return;
    if ( !flush_pending_ )
        flush_anchor_pts_ = last_pts_;
    flush_pending_ = true;
}

void PtsClock::rearm_flush() noexcept
{
    if ( vfr_ && flush_anchor_pts_ >= 0 )
        flush_pending_ = true;
}

void PtsClock::disarm_flush() noexcept
{
    flush_pending_ = false;
    flush_anchor_pts_ = -1;
}

int64_t PtsClock::next( int64_t ts_us, uint64_t frames_sent ) noexcept
{
    if ( !vfr_ )
        return static_cast<int64_t>( counter_ );
    int64_t p = ts_us < 0
                    ? static_cast<int64_t>( frames_sent ) * kDefaultStep90k
                    : rebase_or_track_vfr( ts_us, last_pts_, flush_anchor_pts_, flush_pending_, base_us_,
                                           last_step_90k_ );
    if ( last_pts_ >= 0 && p <= last_pts_ )
    {
        p = last_pts_ + 1;
        ++clamp_cnt_;
    }
    last_pts_ = p;
    return p;
}

void SessionPtsFilter::on_epoch( int64_t frame_step ) noexcept
{
    pts_.restart( frame_step );
    epoch_base_pts_ = last_submitted_pts_ >= 0 ? ( last_submitted_pts_ + 1 ) : 0;
    last_pkt_pts_ = -1;
    awaiting_key_ = true;
    recovery_idr_submitted_ = false;
}

void SessionPtsFilter::on_queue_flush( bool key_queued ) noexcept
{
    pts_.on_queue_flush();
    if ( !key_queued )
    {
        awaiting_key_ = true;
        recovery_idr_submitted_ = false;
    }
}

int64_t SessionPtsFilter::next_frame_pts( int64_t ts_us, uint64_t frames_sent, int64_t& session_fpts,
                                          bool is_key_frame ) noexcept
{
    session_fpts = pts_.next( ts_us, frames_sent );
    const int64_t codec_pts = epoch_base_pts_ + session_fpts;
    if ( codec_pts > last_submitted_pts_ )
        last_submitted_pts_ = codec_pts;
    if ( awaiting_key_ && is_key_frame )
        recovery_idr_submitted_ = true;
    return codec_pts;
}

void SessionPtsFilter::on_submit_failed( bool was_key_frame ) noexcept
{
    if ( was_key_frame )
        recovery_idr_submitted_ = false;
    pts_.rearm_flush();
}

bool SessionPtsFilter::stamp_packet( bool has_pts, int64_t raw_pkt_pts, bool is_key, int64_t session_fpts,
                                     int64_t& mux_pts_out ) noexcept
{
    if ( has_pts && raw_pkt_pts < epoch_base_pts_ )
        return false;  // stale async packet from a prior viewer session

    int64_t rel_pts = session_fpts;
    if ( has_pts )
    {
        const int64_t candidate = raw_pkt_pts - epoch_base_pts_;
        if ( candidate >= 0 && candidate <= session_fpts )
            rel_pts = candidate;
    }

    if ( awaiting_key_ )
    {
        if ( !is_key )
        {
            if ( !recovery_idr_submitted_ )
                pts_.rearm_flush();
            return false;
        }
        awaiting_key_ = false;
        recovery_idr_submitted_ = false;
        pts_.disarm_flush();
    }

    int64_t ts = pts_.packet_ts( rel_pts );
    if ( last_pkt_pts_ >= 0 && ts <= last_pkt_pts_ )
        ts = last_pkt_pts_ + 1;
    last_pkt_pts_ = ts;
    mux_pts_out = ts;
    return true;
}
