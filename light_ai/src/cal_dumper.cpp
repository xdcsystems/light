#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "cal_dumper.hpp"
#include "dma_sync.hpp"

void CalDumper::init( bool dump_cal, int cal_frames, int cal_interval,
        const std::string& cal_dir, int model_w, int model_h )
{
    dump_cal_ = dump_cal;
    cal_frames_ = cal_frames;
    cal_interval_ = cal_interval;
    cal_dir_ = cal_dir;
    model_w_ = model_w;
    model_h_ = model_h;
    inited_ = true;
}

void CalDumper::maybe( int frame_idx, int tensor_fd, const uint8_t* data, size_t size )
{
    if ( !inited_ )
        return;
    // --dump-cal contract: ONE model-input frame per run. dump_cal_
    // is never cleared, so without the cal_done_ guard cal_wanted
    // stayed true for the whole run and the interval condition kept
    // rewriting the file every cal_interval_ frames.
    const bool cal_wanted = ( dump_cal_ && !cal_done_ )
                            || ( cal_frames_ > 0 && cal_saved_ < cal_frames_ );
    const bool cal_due = !cal_done_ || ( frame_idx - last_cal_ >= cal_interval_ );
    if ( !cal_wanted || !cal_due )
        return;
    const size_t bytes = static_cast<size_t>( model_w_ ) * model_h_ * 3;
    std::string path = "/tmp/cal.ppm";
    if ( !dump_cal_ )
    {
        std::ostringstream name;

        // The series name follows the FRAME counter, as documented
        // (README / tools/oracle_check.py protocol):
        // with --cal-interval K the files are frames 0, K, 2K, ....
        // cal_saved_ stays as the --cal N cap. Series directory:
        // --cal-dir overrides the default dataset/.
        name << ( cal_dir_.empty() ? std::string( "dataset/cal_" ) : cal_dir_ + "/cal_" )
             << std::setw( 4 ) << std::setfill( '0' ) << frame_idx << ".ppm";
        path = name.str();
    }
    std::ofstream out( path, std::ios::binary );
    if ( out && size >= bytes )
    {
        dma::DmaSync rd( tensor_fd, false );
        out << "P6\n" << model_w_ << ' ' << model_h_ << "\n255\n";
        out.write( reinterpret_cast<const char*>( data ), static_cast<std::streamsize>( bytes ) );
        if ( dump_cal_ )
            std::cout << "calibration dump: " << path << " (" << model_w_ << 'x'
                      << model_h_ << ")\n";
        ++cal_saved_;
        last_cal_ = frame_idx;
        cal_done_ = true;
    }
    else
    {
        std::cerr << "calibration dump failed\n";
        cal_done_ = true;  // do not retry-spam on a broken sink
    }
}

