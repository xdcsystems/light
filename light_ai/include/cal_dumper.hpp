// Calibration dump: P6 PPM of the model input. The only diagnostic CPU
// pixel read. --dump-cal writes a single /tmp/cal.ppm; --cal N saves up
// to N frames (one every --cal-interval) into dataset/ (or --cal-dir).
// Extracted from Pipeline (stage 2): the cal counters and the file sink
// live here, Pipeline owns the tensor and passes raw bytes.
// 13k: bodies moved to cal_dumper.cpp; the header keeps declarations
// and member data only.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Disabled by default: dump_cal=false and cal_frames=0 make maybe() a
// no-op, so an un-init()'d dumper is safe to call unconditionally.
class CalDumper
{
public:
    // Called once by Pipeline after the model geometry is known.
    void init( bool dump_cal, int cal_frames, int cal_interval,
            const std::string& cal_dir, int model_w, int model_h );

    // data/size are the NPU input tensor bytes.
    void maybe( int frame_idx, const uint8_t* data, size_t size );

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

