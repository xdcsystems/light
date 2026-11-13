// Calibration dump: 
// PPM snapshots of the model input, the only diagnostic CPU pixel read in the pipeline.
//   --dump-cal writes a single /tmp/cal.ppm;
//   --cal N saves up to N frames (one every --cal-interval) into dataset/ (or --cal-dir).
// The cal counters and the file sink live here;
// Pipeline owns the tensor and passes raw bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "config/config.hpp"  // CalConfig

// No preconditions for the caller:
// with CalConfig defaults (dump_one=false, frames=0) or before init(),
// and maybe() is a plain no-op in that state.
// Pipeline calls it unconditionally, once per frame.

class CalDumper
{
public:
    // Called once by Pipeline after the model geometry is known.
    void init( const CalConfig& cal, int model_w, int model_h );

    // data/size are the NPU input tensor bytes.
    void maybe( int frame_idx, int tensor_fd, const uint8_t* data, size_t size );

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

