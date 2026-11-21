// Calibration dump:
// PPM snapshots of the model input, the only diagnostic CPU pixel read in the pipeline.
//   --dump-cal writes a single build/cal.ppm (<cal-dir>/cal.ppm with --cal-dir);
//   --cal N saves up to N frames (one every --cal-interval) into dataset/ (or --cal-dir).
// The two are independent: with both, the single file and the full series
// (one frame may land in both). --cal-dir applies to both.
// Both defaults are relative to the working directory: inside the project when run from light_ai/.
// The cal counters and the file sink live here;
// Pipeline owns the tensor and passes raw bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "config/config.hpp"  // CalConfig
#include "core/log.hpp"
#include "core/status.hpp"

enum class CalKind : uint8_t
{
    Single,  // --dump-cal: one file per run
    Series   // --cal N --cal-interval K: files named by the frame counter
};

// The file a dump of frame frame_idx goes to: Single -> <dir>/cal.ppm
// (dir: cal.dir, default build), Series -> <dir>/cal_%04d.ppm (dir: cal.dir,
// default dataset). frame_idx >= 0. The one place that builds a dump path.
std::string cal_dump_path( const CalConfig& cal, CalKind kind, int frame_idx );

// No preconditions for the caller:
// with CalConfig defaults (dump_one=false, frames=0) or before init(),
// and maybe() is a plain no-op in that state.
// Pipeline calls it unconditionally, once per frame.

class CalDumper
{
public:
    // log: the "calibration dump: ..." (info, single shot) and
    // "calibration dump failed: <path>" (error, once per kind) lines.
    explicit CalDumper( const Log& log ) : log_( log ) {}

    // Called once by Pipeline after the model geometry is known.
    // stride_bytes: the tensor row stride (TensorRef::stride_bytes); 0 or
    // anything below model_w * 3 = tight rows.
    void init( const CalConfig& cal, int model_w, int model_h, uint32_t stride_bytes = 0 );

    // data/size are the NPU input tensor bytes.
    void maybe( int frame_idx, int tensor_fd, const uint8_t* data, size_t size );

private:
    bool series_due( int frame_idx ) const;
    // One P6 file of the model size, written row by row (padded rows drop
    // their padding); Invalid on a short tensor, Failed
    // on an unopenable path or a failed write (the caller logs the path).
    Status write_ppm( const std::string& path, const uint8_t* data, size_t size ) const;

private:
    CalConfig cal_;
    int model_w_ = 0;
    int model_h_ = 0;
    size_t stride_ = 0;  // tensor row stride in bytes (>= model_w_ * 3)
    bool inited_ = false;
    // Single shot: set by the first attempt, a success or a failure (no retry).
    bool single_done_ = false;
    // Series: files written, the frame of the last one, and a failed write stops it.
    int series_saved_ = 0;
    int series_last_ = -1;
    bool series_failed_ = false;
    const Log& log_;  // owned by main(); outlives the Pipeline
};
