// KalmanAxis: a constant-velocity Kalman filter on one axis -- state (x, v),
// covariance 2x2 flattened (P00, P01, P10, P11). Tracker keeps two per track.
// The tuning (process noise q, measurement noise r, the |v| clamp) belongs to
// the caller and comes in per call.
//
// Header-only on purpose: the filter steps stay in the caller's translation
// unit (tracker.cpp), as they were before the split -- the compiler sees the
// same code to inline and contract.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

class KalmanAxis
{
public:
    // A fresh filter at x with no velocity and the diagonal covariance (p00, p11).
    void reset( float x, float p00, float p11 ) noexcept
    {
        if ( !std::isfinite( x ) || !std::isfinite( p00 ) || !std::isfinite( p11 ) || p00 < 0.0f || p11 < 0.0f )
            return;
        x_ = x;
        v_ = 0.0f;
        p_ = { p00, 0.0f, 0.0f, p11 };
    }

    // Full predict: the state moves by v * dt, the covariance grows.
    void predict( float dt, float q ) noexcept
    {
        if ( !std::isfinite( dt ) || !std::isfinite( q ) || dt < 0.0f || q < 0.0f )
            return;
        x_ += v_ * dt;
        predict_cov( dt, q );
    }

    // Covariance-only predict: grows P exactly like the full predict step but leaves the state
    // untouched. Used while a confirmed track is unmatched (miss >= 2): the position stays
    // frozen so the IoU gate can still re-associate the reappearing object at the last seen spot,
    // but the covariance keeps growing -- otherwise P sat at its post-update size for the whole
    // gap and the tiny Kalman gain made the track crawl to the new measurement instead of snapping.
    void predict_cov( float dt, float q ) noexcept
    {
        if ( !std::isfinite( dt ) || !std::isfinite( q ) || dt < 0.0f || q < 0.0f )
            return;
        const float p00 = p_[0];
        const float p01 = p_[1];
        const float p10 = p_[2];
        const float p11 = p_[3];
        const float dt2 = dt * dt;
        p_[0] = p00 + dt * ( p10 + p01 ) + dt2 * p11 + q * dt2 * dt / 3.0f;
        p_[1] = p01 + dt * p11 + q * dt2 / 2.0f;
        p_[2] = p10 + dt * p11 + q * dt2 / 2.0f;
        p_[3] = p11 + q * dt;
    }

    // Measurement update with position z (noise r); the velocity is clamped
    // to [-vmax, vmax]: a single wild measurement must not give the filter an
    // unphysical speed. Uses Joseph form P = (I-KH)P(I-KH)^T + K R K^T to keep
    // covariance symmetric positive semi-definite in FP32.
    void update( float z, float r, float vmax ) noexcept
    {
        if ( !std::isfinite( z ) || !std::isfinite( r ) || !std::isfinite( vmax ) || r < 0.0f || vmax < 0.0f )
            return;
        const float s = p_[0] + r;
        if ( !std::isfinite( s ) || !( s > 1e-9f ) )
            return;
        const float k0 = p_[0] / s;
        const float k1 = p_[2] / s;
        const float y = z - x_;
        x_ += k0 * y;
        v_ += k1 * y;
        v_ = std::clamp( v_, -vmax, vmax );
        const float p00 = p_[0];
        const float p01 = p_[1];
        const float p10 = p_[2];
        const float p11 = p_[3];
        const float a = 1.0f - k0;
        const float b0 = p10 - k1 * p00;
        const float b1 = p11 - k1 * p01;
        p_[0] = a * a * p00 + k0 * k0 * r;
        p_[1] = a * ( p01 - k1 * p00 ) + k0 * k1 * r;
        p_[2] = b0 * a + k1 * k0 * r;
        p_[3] = -k1 * b0 + b1 + k1 * k1 * r;
        const float p01_sym = 0.5f * ( p_[1] + p_[2] );
        p_[1] = p01_sym;
        p_[2] = p01_sym;
        p_[0] = std::max( 0.0f, p_[0] );
        p_[3] = std::max( 0.0f, p_[3] );
    }

    // Velocity decay (a coasting track): v *= factor.
    void damp( float factor ) noexcept
    {
        if ( std::isfinite( factor ) )
            v_ *= factor;
    }

    float x() const { return x_; }
    float v() const { return v_; }
    const std::array<float, 4>& p() const { return p_; }

private:
    float x_ = 0.0f;
    float v_ = 0.0f;
    std::array<float, 4> p_ = { 10000.0f, 0.0f, 0.0f, 10000.0f };
};
