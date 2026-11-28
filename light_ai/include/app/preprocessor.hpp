// Preprocessor: RGA letterbox resize into NPU pool tensors, per-slot band
// tracking, calibration dumps, and per-source AutoOrient probing.
// Extracted from Pipeline so Pipeline only orders stage calls.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "config/config.hpp"
#include "core/auto_orient.hpp"
#include "core/frame_view.hpp"
#include "core/geometry.hpp"
#include "core/global_track.hpp"
#include "core/log.hpp"
#include "core/non_copyable.hpp"
#include "core/perf_reporter.hpp"
#include "core/status.hpp"
#include "core/stop_flag.hpp"
#include "core/timings.hpp"
#include "media/rga_resizer.hpp"
#include "npu/cal_dumper.hpp"
#include "npu/infer_slot.hpp"
#include "source/sources.hpp"

class Preprocessor : private NonMovable<Preprocessor>
{
public:
    enum
    {
        kMaxSources = reid_cfg::kMaxSources,
        kMaxSlots = TensorSet::cap
    };

    // One RGA write into a pool tensor (resize_into_slot): the oriented
    // source size, the band it fills in the tensor, the band RGA reported.
    struct SlotResize
    {
        int w = 0;
        int h = 0;
        LbGeom band {};
        LbGeom rga {};
    };

private:
    struct OrientState
    {
        AutoOrient orient {};
        int cur_rot = 0;
    };

public:
    explicit Preprocessor( const Log& log );
    ~Preprocessor() = default;

    // Initializes model dimensions, fills gray letterbox bars on every pooled
    // tensor, sets up CalDumper, and ensures dataset/ exists when required.
    void init( const CalConfig& cal, int model_w, int model_h, uint32_t input_stride_bytes, const TensorSet& tensors );

    const RgaResizer& rga() const noexcept { return rga_; }
    int model_w() const noexcept { return model_w_; }
    int model_h() const noexcept { return model_h_; }

    void reset_orient( int src, const SourceChannel& ch ) noexcept;
    LbGeom expected_band( int src, const SourceChannel& ch ) const noexcept
    {
        const SourceGeom& ss = ch.geom();
        const int raw_w = ss.raw_w > 0 ? ss.raw_w : ss.w;
        const int raw_h = ss.raw_h > 0 ? ss.raw_h : ss.h;
        int ow = 0;
        int oh = 0;
        auto_orient::oriented_dims( raw_w, raw_h, orient_[static_cast<size_t>( src )].cur_rot, ow, oh );
        return band_geom( ow, oh, model_w_, model_h_ );
    }
    void prepare_tensor_band( int slot, const TensorRef& tn, const LbGeom& eff_lb ) noexcept;

    // The frame fd_src of source ch, rotated by rot, into slot's tensor: the
    // bars re-grayed on a band change, the RGA resize, and on success the
    // slot's band. out: the oriented size and the expected band on every
    // path; out.rga is set only on success.
    Status resize_into_slot( int slot, const SourceChannel& ch, int fd_src, const TensorRef& tn, int rot,
                             SlotResize& out ) noexcept;

    // RGA resize into the slot's tensor, optional cal dump, one-time letterbox
    // drift check, then submit the job to the pool.
    template <class Pool>
    void frame_dispatch( Pool& pool, int slot, int src, const SourceChannel& ch, int frame_idx, int fd_src,
                         int64_t ts_us, double cam_ms, double dec_ms ) noexcept
    {
        const auto t3 = Timings::clk::now();
        SlotResize rs {};
        const TensorRef tn = pool.tensor( slot );
        const Status rga_st = resize_into_slot( slot, ch, fd_src, tn, orient_[static_cast<size_t>( src )].cur_rot, rs );
        if ( rga_st.ok() )
            after_resize_ok( src, frame_idx, tn, rs );
        const auto t3b = Timings::clk::now();

        InferJob job {};
        job.src_idx = src;
        job.src_w = rs.w;
        job.src_h = rs.h;
        job.frame = frame_idx;
        job.ts_us = ts_us;
        job.rga_ok = rga_st.ok();
        job.lb = rs.band;
        job.tr.cam = cam_ms;
        job.tr.dec = dec_ms;
        job.tr.rga = ms( t3, t3b );
        static_cast<void>( pool.submit( slot, job ) );
    }

    template <class Pool, class AcquireFn>
    bool submit_orient_probe( Pool& pool, int src, const SourceChannel& ch, int fd_src, int rot, int job_idx,
                              AcquireFn&& acquire_slot ) noexcept
    {
        const int slot = acquire_slot();
        if ( slot < 0 )
            return false;
        SlotResize rs {};
        const Status rga_st = resize_into_slot( slot, ch, fd_src, pool.tensor( slot ), rot, rs );
        const InferJob job { src, rs.w, rs.h, job_idx, -1, rga_st.ok(), rs.band, {} };
        return pool.submit( slot, job );
    }

    template <class Pool, class PublishFn, class AcquireFn>
    bool probe_rot_batch( Pool& pool, const StopFlag& stop, PerfReporter& perf, int src, const SourceChannel& ch,
                          int fd_src, const int* rots, int n_rots, float* scores, PublishFn&& publish_oldest,
                          AcquireFn&& acquire_slot ) noexcept
    {
        const auto pt0 = Timings::clk::now();
        while ( !pool.empty() && !stop.requested() )
            publish_oldest();
        if ( stop.requested() )
        {
            perf.orient( src, ms( pt0, Timings::clk::now() ) );
            return false;
        }

        for ( int off = 0; off < n_rots && !stop.requested(); )
        {
            const int batch = std::min( pool.size(), n_rots - off );
            int sent = 0;
            for ( int b = 0; b < batch; ++b )
            {
                if ( submit_orient_probe( pool, src, ch, fd_src, rots[off + b], off + b, acquire_slot ) )
                    ++sent;
            }
            for ( int c = 0; c < sent; ++c )
            {
                InferResult pr {};
                if ( pool.collect( pr ) && pr.npu_ok && pr.job.frame >= 0 && pr.job.frame < n_rots )
                    scores[pr.job.frame] = auto_orient::score_detections( pr.dets );
            }
            off += batch;
        }
        perf.orient( src, ms( pt0, Timings::clk::now() ) );
        return !stop.requested();
    }

    // Runs per-source AutoOrient probing when due. Returns true if the active
    // rotation changed (caller resets per-source analytics/output/perf).
    template <class Pool, class PublishFn, class AcquireFn>
    bool update_auto_orient( Pool& pool, const StopFlag& stop, PerfReporter& perf, int src, const SourceChannel& ch,
                             int fd_src, int frame_count, PublishFn&& publish_oldest,
                             AcquireFn&& acquire_slot ) noexcept
    {
        OrientState& os = orient_[static_cast<size_t>( src )];
        if ( !os.orient.enabled() || os.orient.locked() )
            return false;
        const int exif_rot = ch.exif_rot();
        if ( exif_rot >= 0 )
        {
            os.orient.set_exif_hint( exif_rot );
            os.cur_rot = os.orient.active_rot();
        }

        const AutoOrient::Action act = os.orient.next_action( frame_count );
        if ( act == AutoOrient::Action::None )
            return false;
        if ( act == AutoOrient::Action::ProbeSingle )
        {
            const int single_rot = os.orient.watch_angle();
            float single_score = 0.0f;
            if ( !probe_rot_batch( pool, stop, perf, src, ch, fd_src, &single_rot, 1, &single_score, publish_oldest,
                                   acquire_slot ) ||
                 !os.orient.observe_single( single_rot, single_score ) )
                return false;
        }

        constexpr std::array<int, 4> rots { 0, 90, 180, 270 };
        std::array<float, 4> scores {};
        if ( !probe_rot_batch( pool, stop, perf, src, ch, fd_src, rots.data(), 4, scores.data(), publish_oldest,
                               acquire_slot ) )
            return false;
        const AutoOrient::Result res = os.orient.commit_all4( scores.data() );
        os.cur_rot = res.rot_deg;
        if ( res.changed || res.locked )
        {
            log_.info() << "source s" << src << ": auto-rot " << res.rot_deg << " deg (score=" << res.best_score
                        << " vs " << res.second_score << ( res.locked ? ", locked)\n" : ", voting)\n" );
        }
        return res.changed;
    }

private:
    void after_resize_ok( int src, int frame_idx, const TensorRef& tn, const SlotResize& rs ) noexcept;

private:
    const Log& log_;
    RgaResizer rga_;
    std::array<LbGeom, kMaxSlots> tensor_band_ {};
    std::array<OrientState, kMaxSources> orient_ {};
    CalDumper cal_dumper_;
    int model_w_ = 0;
    int model_h_ = 0;
    bool lb_warned_ = false;
};
