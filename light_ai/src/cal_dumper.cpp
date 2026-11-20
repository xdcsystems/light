#include <cerrno>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>

#include "cal_dumper.hpp"
#include "dma_sync.hpp"

std::string cal_dump_path( const CalConfig& cal, CalKind kind, int frame_idx )
{
    if ( kind == CalKind::Single )
        return ( cal.dir.empty() ? std::string( "build" ) : cal.dir ) + "/cal.ppm";

    // The series name follows the FRAME counter, as documented
    // (README / tools/oracle_check.py protocol):
    // with --cal-interval K the files are frames 0, K, 2K, ....
    // Series directory: --cal-dir overrides the default dataset/.
    std::ostringstream name;
    name << ( cal.dir.empty() ? std::string( "dataset" ) : cal.dir ) << "/cal_" << std::setw( 4 ) << std::setfill( '0' )
         << frame_idx << ".ppm";
    return name.str();
}

void CalDumper::init( const CalConfig& cal, int model_w, int model_h, uint32_t stride_bytes )
{
    cal_ = cal;
    model_w_ = model_w;
    model_h_ = model_h;
    const size_t tight = static_cast<size_t>( model_w ) * 3;
    stride_ = stride_bytes > tight ? stride_bytes : tight;
    inited_ = true;
}

// Up to --cal N files, one every --cal-interval frames from the first frame seen.
bool CalDumper::series_due( int frame_idx ) const
{
    if ( cal_.frames <= 0 || series_failed_ || series_saved_ >= cal_.frames )
        return false;
    return series_last_ < 0 || frame_idx - series_last_ >= cal_.interval;
}

Status CalDumper::write_ppm( const std::string& path, const uint8_t* data, size_t size ) const
{
    if ( !data )
        return make_status( Errc::Invalid, "null tensor data" );
    const size_t row = static_cast<size_t>( model_w_ ) * 3;
    const size_t bytes = model_h_ > 0 ? stride_ * static_cast<size_t>( model_h_ - 1 ) + row : 0;
    if ( model_h_ <= 0 || size < bytes )
        return make_status( Errc::Invalid, "tensor shorter than the model frame" );
    errno = 0;  // ofstream does not clear errno on success: keep the sys() honest
    std::ofstream out( path, std::ios::binary );
    if ( !out )
        return errno_status( Errc::Failed, "open failed", errno ? errno : EIO );
    out << "P6\n" << model_w_ << ' ' << model_h_ << "\n255\n";
    for ( int y = 0; y < model_h_ && out; ++y )
        out.write( reinterpret_cast<const char*>( data + stride_ * static_cast<size_t>( y ) ), static_cast<std::streamsize>( row ) );
    out.close();  // flushes: a failed final write (ENOSPC, EIO) sets failbit here, not in the destructor
    if ( out.fail() )
        return errno_status( Errc::Failed, "write failed", errno ? errno : EIO );
    return {};
}

void CalDumper::maybe( int frame_idx, int tensor_fd, const uint8_t* data, size_t size )
{
    if ( !inited_ )
        return;
    // --dump-cal contract: ONE model-input frame per run (single_done_);
    // the series runs on its own counters, so neither kind changes the other's
    // files. A failure is logged once per kind: no retry spam on a broken sink.
    const bool single = cal_.dump_one && !single_done_;
    const bool series = series_due( frame_idx );
    if ( !single && !series )
        return;

    const dma::DmaSync rd( tensor_fd, false );
    // Invalidate CPU cache lines only on dumped frames instead of every frame.
    dma::cache_drop( data, size );
    if ( single )
    {
        single_done_ = true;
        const std::string path = cal_dump_path( cal_, CalKind::Single, frame_idx );
        if ( const Status st = write_ppm( path, data, size ); st.ok() )
            log_.info() << "calibration dump: " << path << " (" << model_w_ << 'x' << model_h_ << ")\n";
        else
            log_.error() << "calibration dump failed: " << path << ": " << st << '\n';
    }
    if ( series )
    {
        const std::string path = cal_dump_path( cal_, CalKind::Series, frame_idx );
        if ( const Status st = write_ppm( path, data, size ); st.ok() )
        {
            ++series_saved_;
            series_last_ = frame_idx;
        }
        else
        {
            series_failed_ = true;
            log_.error() << "calibration dump failed: " << path << ": " << st << '\n';
        }
    }
}
