#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>

#include "cal_dumper.hpp"
#include "dma_sync.hpp"

std::string cal_dump_path( const CalConfig& cal, int frame_idx )
{
    if ( cal.dump_one )
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

void CalDumper::maybe( int frame_idx, int tensor_fd, const uint8_t* data, size_t size )
{
    if ( !inited_ )
        return;
    // --dump-cal contract: ONE model-input frame per run. dump_one
    // is never cleared, so without the cal_done_ guard cal_wanted
    // stayed true for the whole run and the interval condition kept
    // rewriting the file every interval frames.
    // cal_saved_ is the --cal N cap.
    const bool cal_wanted = ( cal_.dump_one && !cal_done_ ) || ( cal_.frames > 0 && cal_saved_ < cal_.frames );
    const bool cal_due = !cal_done_ || ( frame_idx - last_cal_ >= cal_.interval );
    if ( !cal_wanted || !cal_due )
        return;
    const size_t bytes = static_cast<size_t>( model_w_ ) * model_h_ * 3;
    const std::string path = cal_dump_path( cal_, frame_idx );
    std::ofstream out( path, std::ios::binary );
    if ( out && size >= bytes )
    {
        dma::DmaSync rd( tensor_fd, false );
        out << "P6\n" << model_w_ << ' ' << model_h_ << "\n255\n";
        out.write( reinterpret_cast<const char*>( data ), static_cast<std::streamsize>( bytes ) );
        if ( cal_.dump_one )
            log_.info() << "calibration dump: " << path << " (" << model_w_ << 'x' << model_h_ << ")\n";
        ++cal_saved_;
        last_cal_ = frame_idx;
        cal_done_ = true;
    }
    else
    {
        log_.error() << "calibration dump failed: " << path << '\n';
        cal_done_ = true;  // do not retry-spam on a broken sink
    }
}
