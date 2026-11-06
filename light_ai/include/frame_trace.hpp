// Per-frame timing record, extracted from pipeline.hpp (stage 5).
// Single storage: dispatch fills cam/dec/rga, the worker fills
// infer, publish fills enc; Pipeline::Timings::add consumes the
// whole record. Plain POD, no dependencies.

#ifndef FRAME_TRACE_HPP
#define FRAME_TRACE_HPP

// Per-frame timing, single storage: dispatch fills cam/dec/rga,
// the worker fills infer, publish fills enc; Timings::add consumes
// the whole record.
struct FrameTrace {
    double cam = 0.0;
    double dec = 0.0;
    double rga = 0.0;
    double infer = 0.0;
    double post = 0.0;
    double enc = 0.0;
};

#endif  // FRAME_TRACE_HPP
