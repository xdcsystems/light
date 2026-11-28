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
// us = 100 q + r (r has the sign of us, |r| < 100): only r is rounded, so
// no step overflows or negates INT64_MIN on the whole int64_t range.
int64_t us_to_90k( int64_t us ) noexcept
{
    const int64_t q = us / 100;
    const int64_t r = us % 100;
    const int64_t rem = r >= 0 ? ( r * 9 + 50 ) / 100 : -( ( -r * 9 + 50 ) / 100 );
    return q * 9 + rem;
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

void SessionPtsFilter::commit_unconfirmed_head() noexcept
{
    while ( !unconfirmed_.empty() )
    {
        const StampedPacket p = unconfirmed_.front();
        unconfirmed_.pop();
        if ( !p.wait_key_discard )
        {
            committed_ = p;
            has_committed_ = true;
        }
    }
}

int64_t SessionPtsFilter::compute_vfr_adj( int64_t rel_pts ) noexcept
{
    const int64_t current_adj = rel_pts - mux_rebase_offset_;
    if ( mux_flush_pending_ )
    {
        mux_flush_pending_ = false;
        int64_t target_adj = 0;
        if ( flush_anchor_mux_pts_ >= 0 )
        {
            const int64_t min_after = flush_anchor_mux_pts_ / frame_step_ + 1;
            target_adj = std::max( min_after, flush_anchor_adj_rel_pts_ + flush_anchor_step_90k_ );
        }
        last_raw_rel_pts_ = rel_pts;
        if ( current_adj <= target_adj )
            return current_adj;
        mux_rebase_offset_ = rel_pts - target_adj;
        return target_adj;
    }
    if ( last_raw_rel_pts_ >= 0 )
    {
        const int64_t dt = rel_pts - last_raw_rel_pts_;
        if ( dt >= PtsClock::kMinStep90k && dt <= PtsClock::kMaxStep90k )
            last_step_90k_ = dt;
    }
    last_raw_rel_pts_ = rel_pts;
    return current_adj;
}

void SessionPtsFilter::on_epoch( int64_t frame_step ) noexcept
{
    pts_.restart( frame_step );
    frame_step_ = std::max<int64_t>( 1, frame_step );
    epoch_base_pts_ = last_submitted_pts_ >= 0 ? ( last_submitted_pts_ + 1 ) : 0;
    last_pkt_pts_ = -1;
    awaiting_key_ = true;
    has_stamped_ = false;

    mux_rebase_offset_ = 0;
    last_raw_rel_pts_ = -1;
    last_step_90k_ = PtsClock::kDefaultStep90k;
    mux_flush_pending_ = false;
    flush_anchor_rel_pts_ = -1;
    flush_anchor_adj_rel_pts_ = 0;
    flush_anchor_mux_pts_ = -1;
    flush_anchor_step_90k_ = PtsClock::kDefaultStep90k;
    unconfirmed_.clear();
    has_committed_ = false;
    committed_ = StampedPacket {};
}

void SessionPtsFilter::set_flush_anchor( bool use_queued_key ) noexcept
{
    if ( use_queued_key || has_committed_ )
    {
        const StampedPacket anchor = use_queued_key ? unconfirmed_.back( 0 ) : committed_;
        flush_anchor_rel_pts_ = anchor.rel_pts;
        flush_anchor_adj_rel_pts_ = anchor.adj_rel_pts;
        flush_anchor_mux_pts_ = anchor.mux_pts;
        flush_anchor_step_90k_ = anchor.step_90k;
        last_pkt_pts_ = anchor.mux_pts;
        last_raw_rel_pts_ = anchor.rel_pts;
        last_step_90k_ = anchor.step_90k;
    }
    else
    {
        flush_anchor_rel_pts_ = -1;
        flush_anchor_adj_rel_pts_ = 0;
        flush_anchor_mux_pts_ = -1;
        flush_anchor_step_90k_ = PtsClock::kDefaultStep90k;
        last_pkt_pts_ = -1;
        last_raw_rel_pts_ = -1;
        last_step_90k_ = PtsClock::kDefaultStep90k;
    }
    mux_flush_pending_ = true;
}

int64_t SessionPtsFilter::compute_rel_pts( bool has_pts, int64_t raw_pkt_pts, int64_t session_fpts ) const noexcept
{
    int64_t rel_pts = session_fpts;
    if ( has_pts )
    {
        const int64_t candidate = raw_pkt_pts - epoch_base_pts_;
        if ( candidate >= 0 && candidate <= session_fpts )
            rel_pts = candidate;
    }
    return rel_pts;
}

void SessionPtsFilter::on_queue_flush( bool key_queued, int flushed_count ) noexcept
{
    if ( !key_queued )
        awaiting_key_ = true;
    if ( !vfr_ || !has_stamped_ )
        return;

    if ( flushed_count > 0 && !unconfirmed_.empty() )
    {
        const StampedPacket triggering = unconfirmed_.back( 0 );
        unconfirmed_.pop_back();
        for ( int i = 0; i < flushed_count && !unconfirmed_.empty(); ++i )
            unconfirmed_.pop_back();
        commit_unconfirmed_head();
        StampedPacket kept = triggering;
        kept.wait_key_discard = !key_queued;
        unconfirmed_.push( kept );
    }
    if ( flushed_count <= 0 )
        commit_unconfirmed_head();

    const bool use_queued_key = key_queued && !unconfirmed_.empty() && !unconfirmed_.back( 0 ).wait_key_discard;
    set_flush_anchor( use_queued_key );
}

int64_t SessionPtsFilter::rebase_flushed_key( bool has_pts, int64_t raw_pkt_pts, int64_t session_fpts,
                                              int flushed_count ) noexcept
{
    awaiting_key_ = false;
    if ( !vfr_ || !has_stamped_ )
        return unconfirmed_.empty() ? last_pkt_pts_ : unconfirmed_.back( 0 ).mux_pts;

    if ( !unconfirmed_.empty() )
        unconfirmed_.pop_back();
    for ( int i = 0; i < flushed_count && !unconfirmed_.empty(); ++i )
        unconfirmed_.pop_back();
    commit_unconfirmed_head();
    set_flush_anchor( false );

    const int64_t rel_pts = compute_rel_pts( has_pts, raw_pkt_pts, session_fpts );
    const int64_t adj_rel_pts = compute_vfr_adj( rel_pts );
    int64_t ts = pts_.packet_ts( adj_rel_pts );
    if ( last_pkt_pts_ >= 0 && ts <= last_pkt_pts_ )
        ts = last_pkt_pts_ + 1;
    last_pkt_pts_ = ts;
    unconfirmed_.push( StampedPacket { rel_pts, adj_rel_pts, ts, last_step_90k_, false } );
    return ts;
}

int64_t SessionPtsFilter::next_frame_pts( int64_t ts_us, uint64_t frames_sent, int64_t& session_fpts,
                                          bool /*is_key_frame*/ ) noexcept
{
    session_fpts = pts_.next( ts_us, frames_sent );
    const int64_t codec_pts = epoch_base_pts_ + session_fpts;
    if ( codec_pts > last_submitted_pts_ )
        last_submitted_pts_ = codec_pts;
    return codec_pts;
}

void SessionPtsFilter::on_submit_failed( bool /*was_key_frame*/ ) noexcept {}

bool SessionPtsFilter::stamp_packet( bool has_pts, int64_t raw_pkt_pts, bool is_key, int64_t session_fpts,
                                     int64_t& mux_pts_out ) noexcept
{
    if ( has_pts && raw_pkt_pts < epoch_base_pts_ )
        return false;  // stale async packet from a prior viewer session

    const int64_t rel_pts = compute_rel_pts( has_pts, raw_pkt_pts, session_fpts );

    if ( awaiting_key_ )
    {
        if ( !is_key )
            return false;
        awaiting_key_ = false;
    }

    const int64_t adj_rel_pts = vfr_ ? compute_vfr_adj( rel_pts ) : rel_pts;
    int64_t ts = pts_.packet_ts( adj_rel_pts );
    if ( last_pkt_pts_ >= 0 && ts <= last_pkt_pts_ )
        ts = last_pkt_pts_ + 1;
    last_pkt_pts_ = ts;
    mux_pts_out = ts;
    has_stamped_ = true;

    if ( unconfirmed_.full() )
    {
        const StampedPacket oldest = unconfirmed_.front();
        unconfirmed_.pop();
        if ( !oldest.wait_key_discard )
        {
            committed_ = oldest;
            has_committed_ = true;
        }
    }
    unconfirmed_.push( StampedPacket { rel_pts, adj_rel_pts, ts, last_step_90k_, false } );
    return true;
}
