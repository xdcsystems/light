// Per-box interval-envelope stabilizer for stationary YOLO/tracker boxes.
// Collects raw edge bounds [lo, hi] over a short frame window; while raw edges
// stay within the jitter span, locks output to the window mean and holds it
// completely still as long as incoming tensor edges stay within [lo - pad, hi + pad].
// Any breakout resets the lock immediately so real object motion responds cleanly.

#pragma once

#include <algorithm>  // max, min
#include <cmath>      // fabs

struct BoxEnvelope
{
    enum
    {
        kWindowFrames = 8,
    };

    // A non-finite or empty box (M-14) leaves the envelope as it was: reset
    // keeps the old state, step holds the last output.
    bool usable( float x0, float y0, float x1, float y1 ) const noexcept
    {
        return std::isfinite( x0 ) && std::isfinite( y0 ) && std::isfinite( x1 ) && std::isfinite( y1 ) && x1 > x0 && y1 > y0;
    }

    void reset( float x0, float y0, float x1, float y1 ) noexcept
    {
        if ( !usable( x0, y0, x1, y1 ) )
            return;
        const float v[4] = { x0, y0, x1, y1 };
        std::copy_n( v, 4, lo );
        std::copy_n( v, 4, hi );
        std::copy_n( v, 4, sum );
        std::copy_n( v, 4, out );
        std::copy_n( v, 4, first );
        std::copy_n( v, 4, prev );
        std::fill_n( path, 4, 0.0f );
        count = 1;
        locked = false;
    }

    void step( float x0, float y0, float x1, float y1, float move_alpha ) noexcept
    {
        if ( !usable( x0, y0, x1, y1 ) )
            return;
        if ( count <= 0 )
        {
            reset( x0, y0, x1, y1 );
            return;
        }
        const float v[4] = { x0, y0, x1, y1 };
        const float rw = std::max( out[2] - out[0], x1 - x0 );
        const float rh = std::max( out[3] - out[1], y1 - y0 );
        const float sx = std::max( 14.0f, 0.08f * rw );
        const float sy = std::max( 14.0f, 0.08f * rh );
        const float max_span[4] = { sx, sy, sx, sy };

        if ( locked )
        {
            if ( within_bounds( v, max_span ) )
                return;
            unlock_step( v, move_alpha );
            return;
        }
        step_unlocked( v, max_span, move_alpha );
    }

    float x() const noexcept { return out[0]; }
    float y() const noexcept { return out[1]; }
    float w() const noexcept { return std::max( 0.0f, out[2] - out[0] ); }
    float h() const noexcept { return std::max( 0.0f, out[3] - out[1] ); }
    float cx() const noexcept { return 0.5f * ( out[0] + out[2] ); }
    float cy() const noexcept { return 0.5f * ( out[1] + out[3] ); }
    float bw() const noexcept { return w(); }
    float bh() const noexcept { return h(); }

private:
    bool within_bounds( const float v[4], const float max_span[4] ) const noexcept
    {
        for ( int i = 0; i < 4; ++i )
        {
            const float pad = std::max( 5.0f, 0.25f * ( hi[i] - lo[i] ) );
            if ( v[i] < lo[i] - pad || v[i] > hi[i] + pad )
                return false;
            if ( std::fabs( v[i] - out[i] ) > 0.75f * max_span[i] )
                return false;
        }
        return true;
    }

    void unlock_step( const float v[4], float move_alpha ) noexcept
    {
        // Blend halfway from held output on lock breakout so motion resumes without a sharp jump.
        const float eff_a = locked ? std::max( move_alpha, 0.5f ) : move_alpha;
        std::transform( out, out + 4, v, out, [eff_a]( float o, float x ) {
            return eff_a * o + ( 1.0f - eff_a ) * x;
        } );
        std::copy_n( v, 4, lo );
        std::copy_n( v, 4, hi );
        std::copy_n( v, 4, sum );
        std::copy_n( v, 4, first );
        std::copy_n( v, 4, prev );
        std::fill_n( path, 4, 0.0f );
        count = 1;
        locked = false;
    }

    void step_unlocked( const float v[4], const float max_span[4], float move_alpha ) noexcept
    {
        float nlo[4] {};
        float nhi[4] {};
        float npath[4] {};
        for ( int i = 0; i < 4; ++i )
        {
            nlo[i] = std::min( lo[i], v[i] );
            nhi[i] = std::max( hi[i], v[i] );
            npath[i] = path[i] + std::fabs( v[i] - prev[i] );
            const float net = std::fabs( v[i] - first[i] );
            // Break out on steady monotonic drift so slow motion never stair-steps.
            const bool drift = ( count >= 2 && net >= 0.9f * npath[i] && net >= 0.8f );
            if ( nhi[i] - nlo[i] > max_span[i] || drift )
            {
                unlock_step( v, move_alpha );
                return;
            }
        }
        ++count;
        const float inv = 1.0f / static_cast<float>( count );
        for ( int i = 0; i < 4; ++i )
        {
            lo[i] = nlo[i];
            hi[i] = nhi[i];
            prev[i] = v[i];
            path[i] = npath[i];
            sum[i] += v[i];
            out[i] = move_alpha * out[i] + ( 1.0f - move_alpha ) * v[i];
        }
        if ( count >= kWindowFrames )
        {
            locked = true;
            for ( int i = 0; i < 4; ++i )
                out[i] = sum[i] * inv;
        }
    }

private:
    float lo[4] {};
    float hi[4] {};
    float sum[4] {};
    float out[4] {};
    float first[4] {};
    float prev[4] {};
    float path[4] {};
    int count = 0;
    bool locked = false;
};
