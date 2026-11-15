#include <iostream>
#include <sstream>
#include <string>

#include "check.hpp"
#include "console_reporter.h"
#include "suites.hpp"

namespace
{
    // The reporter's stdout for the scope: std::cout redirected into a string.
    class CoutCapture
    {
    public:
        CoutCapture() : old_( std::cout.rdbuf( os_.rdbuf() ) ) {}
        ~CoutCapture() { std::cout.rdbuf( old_ ); }
        CoutCapture( const CoutCapture& ) = delete;
        CoutCapture& operator=( const CoutCapture& ) = delete;
        CoutCapture( CoutCapture&& ) = delete;
        CoutCapture& operator=( CoutCapture&& ) = delete;
        std::string str() const { return os_.str(); }

    private:
        std::ostringstream os_;
        std::streambuf* old_;
    };

    struct Out
    {
        std::string text;
        bool fmt_kept = false;  // the stream's flags and precision are as before the call
    };

    Out report( int frame, const DetList& dets, const TrajList& trajs, int src_idx, int nsrc )
    {
        const ConsoleReporter rep;
        const CoutCapture cap;
        const std::ios_base::fmtflags flags = std::cout.flags();
        const std::streamsize prec = std::cout.precision();
        rep.report( frame, dets, trajs, src_idx, nsrc );
        return { cap.str(), std::cout.flags() == flags && std::cout.precision() == prec };
    }

    Out summary( int ok, double wall, int reconnects )
    {
        const ConsoleReporter rep;
        const CoutCapture cap;
        const std::ios_base::fmtflags flags = std::cout.flags();
        const std::streamsize prec = std::cout.precision();
        rep.summary( ok, wall, reconnects );
        return { cap.str(), std::cout.flags() == flags && std::cout.precision() == prec };
    }

    TrackState track( int id, TrackLife life, float cx, float cy, float vx, float vy, int n_pts )
    {
        return TrackState { id, life, cx, cy, vx, vy, 0.0f, 0.0f, n_pts };
    }

    void reports( TestRun& t )
    {
        DetList dets;
        dets.push( Detection { 10.0f, 20.5f, 30.0f, 40.25f, 0, 0.87f } );
        TrajList trajs;
        trajs.push( track( 4, TRACK_CONF, 100.0f, 50.5f, -1.24f, 0.0f, 7 ) );
        trajs.push( track( 9, TRACK_DEAD, 3.14f, 2.0f, 12.0f, -3.06f, 0 ) );
        trajs.push( track( 11, TRACK_TENT, 0.0f, 0.0f, 0.0f, 0.0f, 1 ) );
        const DetList no_dets;
        const TrajList no_trajs;

        // Only every 15th frame reports.
        CHECK( t, report( 7, dets, trajs, 0, 1 ).text.empty() );
        CHECK( t, report( 30, no_dets, no_trajs, 0, 1 ).text.empty() );

        const Out tracks = report( 30, no_dets, trajs, 0, 1 );
        CHECK( t, tracks.text == "track 30: 3 track(s)\n"
                                "  id=4 conf c=(100.0,50.5) v=(-1.2,0.0) pts=7\n"
                                "  id=9 dead c=(3.1,2.0) v=(12.0,-3.1) pts=0\n"
                                "  id=11 tent c=(0.0,0.0) v=(0.0,0.0) pts=1\n" );
        CHECK( t, tracks.fmt_kept );

        // Several sources: the s<N> tag.
        const Out det = report( 45, dets, no_trajs, 2, 3 );
        CHECK( t, det.text == "frame 45 s2: 1 det(s)\n"
                             "  cls=person conf=0.87 box=(10.00,20.50 30.00x40.25)\n" );
        CHECK( t, det.fmt_kept );

        // Both blocks, tracks first; at most 8 boxes; a class outside COCO-80 prints '?'.
        DetList many;
        for ( int k = 0; k < 10; ++k )
            many.push( Detection { static_cast<float>( k ), 1.0f, 2.0f, 3.0f, k == 1 ? 80 : ( k == 2 ? -1 : k ), 0.5f } );
        TrajList one;
        one.push( track( 1, TRACK_CONF, 1.0f, 2.0f, 3.0f, 4.0f, 5 ) );
        const Out both = report( 0, many, one, 1, 2 );
        CHECK( t, both.text == "track 0 s1: 1 track(s)\n"
                              "  id=1 conf c=(1.0,2.0) v=(3.0,4.0) pts=5\n"
                              "frame 0 s1: 10 det(s)\n"
                              "  cls=person conf=0.50 box=(0.00,1.00 2.00x3.00)\n"
                              "  cls=? conf=0.50 box=(1.00,1.00 2.00x3.00)\n"
                              "  cls=? conf=0.50 box=(2.00,1.00 2.00x3.00)\n"
                              "  cls=motorcycle conf=0.50 box=(3.00,1.00 2.00x3.00)\n"
                              "  cls=airplane conf=0.50 box=(4.00,1.00 2.00x3.00)\n"
                              "  cls=bus conf=0.50 box=(5.00,1.00 2.00x3.00)\n"
                              "  cls=train conf=0.50 box=(6.00,1.00 2.00x3.00)\n"
                              "  cls=truck conf=0.50 box=(7.00,1.00 2.00x3.00)\n" );
        CHECK( t, both.fmt_kept );
    }

    void summaries( TestRun& t )
    {
        const Out plain = summary( 100, 4.0, 0 );
        CHECK( t, plain.text == "Processed 100 frames in 4.000 s -> 25.000 FPS\n" );
        CHECK( t, plain.fmt_kept );
        CHECK( t, summary( 5, 2.5, 2 ).text == "camera disconnects recovered: 2\n"
                                               "Processed 5 frames in 2.500 s -> 2.000 FPS\n" );
        CHECK( t, summary( 0, 0.0, 0 ).text == "Processed 0 frames in 0.000 s -> 0.000 FPS\n" );
    }
}  // namespace

void test_console_reporter( TestRun& t )
{
    reports( t );
    summaries( t );
}
