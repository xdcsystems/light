// BasicStreamOutput<FakeSink> (include/app/stream_output.hpp) under ThreadSanitizer:
// exercises SPSC worker handoff, drop-oldest across 4 sources, busy_slot_mask(),
// wait_slot_free(), flush_dirty(), idle_tick(), reset_source(), take_perf(),
// take_enc_perf(), and drain() without RKNN/MPP/RGA/FFmpeg/EGL SDK headers.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <thread>

#include "app/stream_output.hpp"
#include "check.hpp"
#include "platform/pi_mutex.hpp"
#include "config/config.hpp"
#include "core/detection.hpp"
#include "core/frame_view.hpp"
#include "core/geometry.hpp"
#include "core/log.hpp"
#include "core/net_perf.hpp"
#include "core/perf_clock.hpp"
#include "core/track.hpp"
#include "core/tracker.hpp"
#include "suites.hpp"

namespace
{
    using namespace std::chrono_literals;

    class FakeSink
    {
    public:
        explicit FakeSink( const Log& ) noexcept {}

        void init( const StreamConfig&, const TensorSet&, int, int, const LbGeom&, int ) noexcept
        {
            enabled_.store( true, std::memory_order_release );
            viewer_live_.store( true, std::memory_order_release );
        }

        bool enabled() const noexcept { return enabled_.load( std::memory_order_acquire ); }
        bool viewer_live() const noexcept { return viewer_live_.load( std::memory_order_acquire ); }

        void reset_source( int src ) noexcept
        {
            if ( src >= 0 && src < reid_cfg::kMaxSources )
                resets_[static_cast<std::size_t>( src )].fetch_add( 1, std::memory_order_relaxed );
        }

        void prepare_overlay( const FrameView& frame, const DetList& dets, const TrajList&, const Tracker&,
                              GpuBoxList& boxes_out, CalloutList& callouts_out ) noexcept
        {
            boxes_out.clear();
            callouts_out.clear();
            if ( dets.n > 0 )
                boxes_out.push( GpuBox { 1.0f, 2.0f, 3.0f, 4.0f, 255, 0, static_cast<uint8_t>( frame.src_idx ), 255 } );
        }

        double render_and_publish( const FrameView& frame, const GpuBoxList&, const CalloutList&, bool ) noexcept
        {
            maybe_wait_gate();
            rendered_cnt_.fetch_add( 1, std::memory_order_relaxed );
            if ( frame.src_idx >= 0 && frame.src_idx < reid_cfg::kMaxSources )
                last_ts_[static_cast<std::size_t>( frame.src_idx )].store( frame.ts_us, std::memory_order_release );
            const std::lock_guard<light_ai::platform::PiMutex> lk( perf_mx_ );
            pending_ovl_.gpu_draw_ms_max = 1.25;
            pending_enc_.enc_admit += 1;
            pending_enc_.add_dt_enc( 12.0 );
            return 1.25;
        }

        void publish_idle( int64_t, const bool*, int ) noexcept
        {
            idle_cnt_.fetch_add( 1, std::memory_order_relaxed );
            const std::lock_guard<light_ai::platform::PiMutex> lk( perf_mx_ );
            pending_enc_.idle_admit += 1;
        }

        void flush_dirty() noexcept { flush_cnt_.fetch_add( 1, std::memory_order_relaxed ); }

        OverlayPerfWindow take_perf() noexcept
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( perf_mx_ );
            const OverlayPerfWindow out = pending_ovl_;
            pending_ovl_.reset();
            return out;
        }

        bool has_enc_perf() const noexcept { return true; }

        EncPerfWindow take_enc_perf() noexcept
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( perf_mx_ );
            const EncPerfWindow out = pending_enc_;
            pending_enc_.reset();
            return out;
        }

        void release_worker_context() noexcept
        {
            if ( released_out_ != nullptr )
                released_out_->store( true, std::memory_order_release );
        }

        void set_released_flag( std::atomic<bool>* out ) noexcept { released_out_ = out; }

        void arm_gate()
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( gate_mx_ );
            hold_ = true;
            in_gate_ = false;
        }

        bool wait_in_gate( std::chrono::milliseconds timeout )
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( gate_mx_ );
            return gate_cv_.wait_for( lk, timeout, [this] { return in_gate_; } );
        }

        void release_gate()
        {
            {
                const std::lock_guard<light_ai::platform::PiMutex> lk( gate_mx_ );
                hold_ = false;
            }
            gate_cv_.notify_all();
        }

        int rendered_cnt() const noexcept { return rendered_cnt_.load( std::memory_order_relaxed ); }
        int idle_cnt() const noexcept { return idle_cnt_.load( std::memory_order_relaxed ); }
        int flush_cnt() const noexcept { return flush_cnt_.load( std::memory_order_relaxed ); }
        int64_t last_ts( int src ) const noexcept
        {
            return last_ts_[static_cast<std::size_t>( src )].load( std::memory_order_acquire );
        }
        int reset_cnt( int src ) const noexcept
        {
            return resets_[static_cast<std::size_t>( src )].load( std::memory_order_relaxed );
        }

    private:
        void maybe_wait_gate()
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( gate_mx_ );
            if ( !hold_ )
                return;
            in_gate_ = true;
            gate_cv_.notify_all();
            gate_cv_.wait_for( lk, 2000ms, [this] { return !hold_; } );
        }

    private:
        std::atomic<bool> enabled_ { false };
        std::atomic<bool> viewer_live_ { false };
        std::atomic<int> rendered_cnt_ { 0 };
        std::atomic<int> idle_cnt_ { 0 };
        std::atomic<int> flush_cnt_ { 0 };
        std::atomic<bool>* released_out_ = nullptr;
        std::array<std::atomic<int64_t>, reid_cfg::kMaxSources> last_ts_ {};
        std::array<std::atomic<int>, reid_cfg::kMaxSources> resets_ {};

        light_ai::platform::PiMutex perf_mx_;
        OverlayPerfWindow pending_ovl_ {};
        EncPerfWindow pending_enc_ {};

        light_ai::platform::PiMutex gate_mx_;
        light_ai::platform::PiCondVar gate_cv_;
        bool hold_ = false;
        bool in_gate_ = false;
    };

    struct FakeSrcs
    {
        int size() const noexcept { return 4; }
        bool absent_mask( std::array<bool, reid_cfg::kMaxSources>& out ) const noexcept
        {
            out = { true, false, true, false };
            return true;
        }
    };

    FrameView make_view( int src, int slot, int64_t ts_us )
    {
        FrameView v {};
        v.src_idx = src;
        v.tensor_idx = slot;
        v.src_w = 640;
        v.src_h = 480;
        v.ts_us = ts_us;
        return v;
    }

    void check_drop_oldest_and_masks( TestRun& t, BasicStreamOutput<FakeSink>& out, const Tracker& tracker,
                                      const DetList& dets, const TrajList& trajs )
    {
        out.sink().arm_gate();
        out.publish_frame( 1, make_view( 0, 0, 10 ), dets, trajs, tracker, 4, false );
        CHECK( t, out.sink().wait_in_gate( 1500ms ) );

        // Worker is active on slot 0; drop-oldest overwrites src 1's slot-1 job with slot-2 job.
        out.publish_frame( 2, make_view( 1, 1, 100 ), dets, trajs, tracker, 4, false );
        out.publish_frame( 3, make_view( 1, 2, 101 ), dets, trajs, tracker, 4, false );
        out.publish_frame( 4, make_view( 2, 1, 200 ), dets, trajs, tracker, 4, true );
        out.publish_frame( 5, make_view( 3, 2, 300 ), dets, trajs, tracker, 4, false );

        CHECK_EQ( t, out.busy_slot_mask(), 0b111u );
        out.flush_dirty();

        std::thread releaser( [&out] {
            std::this_thread::sleep_for( 10ms );
            out.sink().release_gate();
        } );
        CHECK( t, out.wait_slot_free( 0 ) );
        releaser.join();

        CHECK( t, out.wait_slot_free( 1 ) );
        CHECK( t, out.wait_slot_free( 2 ) );
        out.drain();

        CHECK_EQ( t, out.busy_slot_mask(), 0u );
        CHECK_EQ( t, out.sink().rendered_cnt(), 4 );
        CHECK_EQ( t, out.sink().last_ts( 1 ), 101 );
        CHECK_EQ( t, out.sink().last_ts( 2 ), 200 );
        CHECK_EQ( t, out.sink().last_ts( 3 ), 300 );
    }
}  // namespace

void test_stream_output_threads( TestRun& t )
{
    std::ostringstream os;
    std::ostringstream es;
    const Log log( os, es );
    const Tracker tracker;
    DetList dets {};
    dets.push( Detection { 10.0f, 20.0f, 30.0f, 40.0f, 0, 0.9f } );
    const TrajList trajs {};
    const FakeSrcs srcs {};

    std::atomic<bool> released { false };
    {
        BasicStreamOutput<FakeSink> out( log, false );
        out.sink().set_released_flag( &released );
        StreamConfig cfg {};
        cfg.fps = 25;
        const TensorSet tensors {};
        const LbGeom lb { 0, 0, 640, 480 };
        out.init( cfg, tensors, 640, 640, lb, 4 );
        CHECK( t, out.has_enc_perf() );

        check_drop_oldest_and_masks( t, out, tracker, dets, trajs );

        // Concurrent stress across 4 sources and 3 slots with masks, flush, idle, perf, and drain.
        const auto t0 = PerfClock::now() - std::chrono::seconds( 2 );
        uint32_t total_admit = 0;
        double max_gpu = 0.0;
        for ( int i = 0; i < 160; ++i )
        {
            const int src = i % 4;
            const int slot = i % 3;
            if ( ( i % 19 ) == 0 )
                CHECK( t, out.wait_slot_free( slot ) );
            out.publish_frame( i + 10, make_view( src, slot, 1000 + i ), dets, trajs, tracker, 4, ( i % 2 ) == 0 );
            static_cast<void>( out.busy_slot_mask() );
            if ( ( i % 7 ) == 0 )
                out.flush_dirty();
            if ( ( i % 11 ) == 0 )
                out.idle_tick( t0, srcs );
            if ( ( i % 23 ) == 0 )
                out.reset_source( src );
            if ( ( i % 13 ) == 0 )
            {
                const OverlayPerfWindow ov = out.take_perf();
                const EncPerfWindow ev = out.take_enc_perf();
                if ( ov.gpu_draw_ms_max > max_gpu )
                    max_gpu = ov.gpu_draw_ms_max;
                total_admit += ev.enc_admit;
            }
            if ( ( i % 29 ) == 0 )
                out.drain();
        }
        out.idle_tick( PerfClock::now(), srcs );
        out.drain();
        CHECK_EQ( t, out.busy_slot_mask(), 0u );

        const OverlayPerfWindow final_ov = out.take_perf();
        const EncPerfWindow final_ev = out.take_enc_perf();
        if ( final_ov.gpu_draw_ms_max > max_gpu )
            max_gpu = final_ov.gpu_draw_ms_max;
        total_admit += final_ev.enc_admit;

        CHECK( t, max_gpu >= 1.25 );
        CHECK( t, total_admit > 4 );
        CHECK( t, out.sink().flush_cnt() > 0 );
        CHECK( t, out.sink().idle_cnt() > 0 );
        CHECK( t, out.sink().reset_cnt( 0 ) + out.sink().reset_cnt( 1 ) + out.sink().reset_cnt( 2 )
                      + out.sink().reset_cnt( 3 ) > 0 );
        out.summary( 164, 1.0, 0 );
    }
    CHECK( t, released.load( std::memory_order_acquire ) );
    CHECK( t, os.str().find( "Processed 164 frames" ) != std::string::npos );
}
