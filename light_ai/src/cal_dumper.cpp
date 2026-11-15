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

void CalDumper::init( const CalConfig& cal, int model_w, int model_h )
{
    cal_ = cal;
    model_w_ = model_w;
    model_h_ = model_h;
    inited_ = true;
}

// Up to --cal N files, one every --cal-interval frames from the first frame seen.
bool CalDumper::series_due( int frame_idx ) const
{
    if ( cal_.frames <= 0 || series_failed_ || series_saved_ >= cal_.frames )
        return false;
    return series_last_ < 0 || frame_idx - series_last_ >= cal_.interval;
}

bool CalDumper::write_ppm( const std::string& path, const uint8_t* data, size_t size ) const
{
    const size_t bytes = static_cast<size_t>( model_w_ ) * model_h_ * 3;
    std::ofstream out( path, std::ios::binary );
    if ( !out || size < bytes )
        return false;
    out << "P6\n" << model_w_ << ' ' << model_h_ << "\n255\n";
    out.write( reinterpret_cast<const char*>( data ), static_cast<std::streamsize>( bytes ) );
    out.close();  // flushes: a failed final write (ENOSPC, EIO) sets failbit here, not in the destructor
    return static_cast<bool>( out );
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
    if ( single )
    {
        single_done_ = true;
        const std::string path = cal_dump_path( cal_, CalKind::Single, frame_idx );
        if ( write_ppm( path, data, size ) )
            log_.info() << "calibration dump: " << path << " (" << model_w_ << 'x' << model_h_ << ")\n";
        else
            log_.error() << "calibration dump failed: " << path << '\n';
    }
    if ( series )
    {
        const std::string path = cal_dump_path( cal_, CalKind::Series, frame_idx );
        if ( write_ppm( path, data, size ) )
        {
            ++series_saved_;
            series_last_ = frame_idx;
        }
        else
        {
            series_failed_ = true;
            log_.error() << "calibration dump failed: " << path << '\n';
        }
    }
}
