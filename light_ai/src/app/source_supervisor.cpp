#include <algorithm>
#include <string>

#include "app/source_supervisor.hpp"
#include "core/throw_on.hpp"

SourceSupervisor::SourceSupervisor( const StopFlag& stop, const Log& log )
  : stop_( stop )
  , log_( log )
  , locator_( usb_host_, log )
  , srcs_( log, [&log]( int /*idx*/ ) { return SourceChannel( log ); } )
{
}

void SourceSupervisor::configure( const std::vector<SourceEndpoint>& sources, int model_w, int model_h )
{
    srcs_.configure( sources, model_w, model_h );
    srcs_.set_stop( &stop_.raw() );
}

// The first attach of every channel, through the same SourceChannel::attach as
// the prober's. A source that is not there stays non-Live and the prober takes
// it (2 s cadence); a decoder or aux failure is fatal at startup.
void SourceSupervisor::attach_initial( int model_w, int model_h )
{
    using Attach = SourceChannel::Attach;
    bool any_aux = false;
    int max_w = 0;
    int max_h = 0;
    for ( int s = 0; s < srcs_.size(); ++s )
    {
        SourceChannel& ch = srcs_[s];
        Status why;
        const Attach a = ch.attach( srcs_.busy_nodes( s ), locator_, false, why );
        if ( a == Attach::DecoderFailed )
            throw_on( why, "mpp decoder init failed for " + ch.node() );
        if ( a == Attach::AuxFailed )
            throw_on( why, "pipeline: aux buffer alloc failed for source " + ch.node() );
        if ( a != Attach::Live )
        {
            log_.warn() << "source s" << s << ": not started at startup (" << why << ") - prober retries every 2 s\n";
            continue;
        }
        max_w = std::max( max_w, ch.geom().w );
        max_h = std::max( max_h, ch.geom().h );
        any_aux = any_aux || ch.geom().aux_fd >= 0;
    }
    if ( !any_aux )
        log_.info() << "aux not needed: max " << max_w << 'x' << max_h
                    << " fits single-pass into " << model_w << 'x' << model_h << '\n';
}

void SourceSupervisor::reset_stall_clocks( std::chrono::steady_clock::time_point t0 ) noexcept
{
    srcs_.reset_stall_clocks( t0 );
}
