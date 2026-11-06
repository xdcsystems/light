// Calibration dump: P6 PPM of the model input. The only diagnostic CPU
// pixel read. --dump-cal writes a single /tmp/cal.ppm; --cal N saves up
// to N frames (one every --cal-interval) into dataset/ (or --cal-dir).
// Extracted from Pipeline (stage 2): the cal counters and the file sink
// live here, Pipeline owns the tensor and passes raw bytes. Header-only,
// like dma_sync.hpp: one call site, no build-system churn.

#pragma once

#include <cstddef>  // size_t
#include <cstdint>  // uint8_t
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// Disabled by default: dump_cal=false and cal_frames=0 make maybe() a
// no-op, so an un-init()'d dumper is safe to call unconditionally.
class CalDumper
{
public:
    // Called once by Pipeline after the model geometry is known.
    void init( bool dump_cal, int cal_frames, int cal_interval,
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

    // data/size are the NPU input tensor bytes.
    void maybe( int frame_idx, const uint8_t* data, size_t size )
    {
        if ( !inited_ )
            return;
        const bool cal_wanted = dump_cal_ || ( cal_frames_ > 0 && cal_saved_ < cal_frames_ );
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

private:
    bool dump_cal_ = false;
    int cal_frames_ = 0;
    int cal_interval_ = 0;
    std::string cal_dir_;
    int model_w_ = 0;
    int model_h_ = 0;
    bool inited_ = false;
    int cal_saved_ = 0;
    int last_cal_ = -1;
    bool cal_done_ = false;
};

