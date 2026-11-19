// YoloDecoder: the yolov5 postprocess over the raw output heads of the model
// -- decode, confidence threshold, class-aware NMS, the bar filter and the
// mapping into scene coordinates. The RKNN side (YoloDecode,
// yolo_postprocess.hpp) only builds the YoloHead views of the engine outputs.
//
// Letterbox: the visible band occupies rows [lb.top, lb.top + lb.h)
// (landscape) or columns [lb.left, lb.left + lb.w) (portrait) of the model
// input; the mapping to scene coordinates is axial:
// scene = (det - band origin) * cam / band size.
//
// Threads: one instance per NPU worker (InferencePool keeps one copy per
// worker): the candidate buffer is a preallocated member, so an instance is
// never called concurrently.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/detection.hpp"  // Detection, DetList
#include "core/geometry.hpp"   // LbGeom
#include "core/log.hpp"

// One output head as the decoder sees it: fp16 NCHW with N = 1,
// post-sigmoid, ch = 3 anchors x (4 box + 1 obj + classes) channels.
// data == nullptr: the head is not usable (skipped silently).
struct YoloHead
{
    const uint16_t* data = nullptr;
    int ch = 0;
    int h = 0;
    int w = 0;
};

// The shape check of one head, shared by YoloDecoder::anchor_set and the
// startup check (the Pipeline ctor): the width divides the model input, the
// stride is 8/16/32, the height (> 0) matches the model at that stride and ch
// is a 3-anchor layout (ch % 3 == 0) with at least one class (ch >= 18). The strict ch == 255 of the standard 80-class
// model is the startup check's job (a 2-class synthetic head must still
// decode). One warn per failure; on success stride and head (P3/P4/P5) are set
// and true is returned. The data pointer is not touched (nullptr is fine):
// the startup check has no data.
bool yolo_head_shape( const YoloHead& hd, int gi, int mw, int mh, const Log& log, int& stride, int& head );

// The geometry of one frame: source (camera) size, model input size and the
// letterbox band of the frame in the model input.
struct YoloFrame
{
    float cam_w = 0.0f;
    float cam_h = 0.0f;
    float model_w = 0.0f;
    float model_h = 0.0f;
    LbGeom lb {};
};

class YoloDecoder
{
public:
    enum
    {
        kMaxHeads = 3,  // P3 / P4 / P5
        // Candidate buffer capacity: the early cut (obj < conf_thr) keeps the
        // real flow to single digits; the cap is a safety net -- an overflow is
        // counted and reported once per frame, never silent.
        kMaxCand = 4096,
    };

    YoloDecoder( float conf_thr, float iou_thr, const Log& log );

    // Decodes heads[0 .. n_heads) (at most kMaxHeads) into out, in scene
    // coordinates. out is cleared at entry (preallocated buffer contract).
    // The head order does not matter: the stride comes from the map size.
    void decode( const YoloHead* heads, int n_heads, const YoloFrame& f, DetList& out );

private:
    // A candidate box in model-input coordinates (corners).
    struct Box
    {
        float x1, y1, x2, y2;
        float conf;
        int cls;
        int seq;  // scan index: deterministic tie-break without std::stable_sort heap buffer
    };
    // The letterbox band in model-input pixels.
    struct Band
    {
        float left, top, w, h;
    };
    // One head being scanned: element (c, gy, gx) of anchor slot k lives at
    // c * cell_hw + gy * w + gx + k * per * cell_hw.
    struct HeadScan
    {
        const uint16_t* data;
        size_t cell_hw;  // h * w
        int h;
        int w;
        int per;     // channels per anchor: 85 = 4 + 1 + 80
        int stride;  // model-input pixels per map cell
        int anchors; // the anchor set: 0 / 1 / 2 = P3 / P4 / P5
    };

    bool band_fits( const YoloFrame& f ) const noexcept;
    int anchor_set( const YoloHead& hd, int gi, int mw, int mh, int& stride ) const;
    void collect_head( const HeadScan& s, const Band& b ) noexcept;
    bool decode_anchor( const HeadScan& s, size_t off, int k, int gx, int gy, const Band& b, Box& out ) const noexcept;
    float best_class( const HeadScan& s, size_t off, int& best ) const noexcept;
    void push_candidate( const Box& box ) noexcept;
    void suppress() noexcept;
    int emit( const YoloFrame& f, const Band& b, DetList& out ) const noexcept;

private:
    float conf_thr_;
    float iou_thr_;
    const Log& log_;

    // Preallocated per-frame state, reset at the entry of decode(): the
    // ~96 KiB of candidates used to live on the worker's stack, once per frame.
    std::array<Box, kMaxCand> cand_ {};
    std::array<uint8_t, kMaxCand> sup_ {};  // NMS: 1 = suppressed
    int nc_ = 0;                           // candidates in cand_
    int dropped_ = 0;                      // candidates lost to the cap this frame
};
