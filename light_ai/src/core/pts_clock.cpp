#include <algorithm>

#include "core/pts_clock.hpp"

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
    frame_step_ = std::max<int64_t>( 1, frame_step );
}

int64_t PtsClock::next( int64_t ts_us, uint64_t frames_sent ) noexcept
{
    if ( !vfr_ )
        return static_cast<int64_t>( counter_ );
    int64_t p = 0;
    if ( ts_us < 0 )
    {
        p = static_cast<int64_t>( frames_sent ) * 3600;  // 25 fps fallback grid
    }
    else
    {
        if ( base_us_ < 0 )
            base_us_ = ts_us;  // per-session base on the first timestamp
        p = us_to_90k( ts_us - base_us_ );
    }
    if ( last_pts_ >= 0 && p <= last_pts_ )
        p = last_pts_ + 1;
    last_pts_ = p;
    return p;
}

void SessionPtsFilter::on_epoch( int64_t frame_step ) noexcept
{
    pts_.restart( frame_step );
    epoch_base_pts_ = last_submitted_pts_ >= 0 ? ( last_submitted_pts_ + 1 ) : 0;
    last_pkt_pts_ = -1;
    awaiting_key_ = true;
}

int64_t SessionPtsFilter::next_frame_pts( int64_t ts_us, uint64_t frames_sent, int64_t& session_fpts ) noexcept
{
    session_fpts = pts_.next( ts_us, frames_sent );
    const int64_t codec_pts = epoch_base_pts_ + session_fpts;
    if ( codec_pts > last_submitted_pts_ )
        last_submitted_pts_ = codec_pts;
    return codec_pts;
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
    else if ( awaiting_key_ && !is_key )
    {
        return false;
    }

    if ( awaiting_key_ )
    {
        if ( !is_key )
            return false;
        awaiting_key_ = false;
    }

    int64_t ts = pts_.packet_ts( rel_pts );
    if ( last_pkt_pts_ >= 0 && ts <= last_pkt_pts_ )
        ts = last_pkt_pts_ + 1;
    last_pkt_pts_ = ts;
    mux_pts_out = ts;
    return true;
}
