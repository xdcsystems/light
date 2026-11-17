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

    void reset( float x0, float y0, float x1, float y1 ) noexcept
    {
        const float v[4] = { x0, y0, x1, y1 };
        for ( int i = 0; i < 4; ++i )
        {
            lo[i] = v[i];
            hi[i] = v[i];
            sum[i] = v[i];
            out[i] = v[i];
        }
        count = 1;
        locked = false;
    }

    void step( float x0, float y0, float x1, float y1, float move_alpha ) noexcept
    {
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
        for ( int i = 0; i < 4; ++i )
        {
            out[i] = move_alpha * out[i] + ( 1.0f - move_alpha ) * v[i];
            lo[i] = v[i];
            hi[i] = v[i];
            sum[i] = v[i];
        }
        count = 1;
        locked = false;
    }

    void step_unlocked( const float v[4], const float max_span[4], float move_alpha ) noexcept
    {
        float nlo[4] {};
        float nhi[4] {};
        for ( int i = 0; i < 4; ++i )
        {
            nlo[i] = std::min( lo[i], v[i] );
            nhi[i] = std::max( hi[i], v[i] );
            if ( nhi[i] - nlo[i] > max_span[i] )
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
            sum[i] += v[i];
            out[i] = sum[i] * inv;
        }
        if ( count >= kWindowFrames )
            locked = true;
    }

private:
    float lo[4] {};
    float hi[4] {};
    float sum[4] {};
    float out[4] {};
    int count = 0;
    bool locked = false;
};
