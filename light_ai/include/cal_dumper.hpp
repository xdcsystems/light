// Calibration dump:
// PPM snapshots of the model input, the only diagnostic CPU pixel read in the pipeline.
//   --dump-cal writes a single build/cal.ppm (<cal-dir>/cal.ppm with --cal-dir);
//   --cal N saves up to N frames (one every --cal-interval) into dataset/ (or --cal-dir).
// Both defaults are relative to the working directory: inside the project when run from light_ai/.
// The cal counters and the file sink live here;
// Pipeline owns the tensor and passes raw bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "config/config.hpp"  // CalConfig
#include "core/log.hpp"

// The file a dump of frame frame_idx goes to: with cal.dump_one the single
// <dir>/cal.ppm (dir: cal.dir, default build), else the series file
// <dir>/cal_%04d.ppm (dir: cal.dir, default dataset). frame_idx >= 0.
std::string cal_dump_path( const CalConfig& cal, int frame_idx );

// No preconditions for the caller:
// with CalConfig defaults (dump_one=false, frames=0) or before init(),
// and maybe() is a plain no-op in that state.
// Pipeline calls it unconditionally, once per frame.

class CalDumper
{
public:
    // log: the "calibration dump: ..." (info) and "calibration dump failed: ..." (error) lines.
    explicit CalDumper( const Log& log ) : log_( log ) {}

    // Called once by Pipeline after the model geometry is known.
    void init( const CalConfig& cal, int model_w, int model_h );

    // data/size are the NPU input tensor bytes.
    void maybe( int frame_idx, int tensor_fd, const uint8_t* data, size_t size );

private:
    CalConfig cal_;
    int model_w_ = 0;
    int model_h_ = 0;
    bool inited_ = false;
    int cal_saved_ = 0;
    int last_cal_ = -1;
    bool cal_done_ = false;
    const Log& log_;  // owned by main(); outlives the Pipeline
};
