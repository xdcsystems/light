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

class KalmanAxis
{
public:
    // A fresh filter at x with no velocity and the diagonal covariance (p00, p11).
    void reset( float x, float p00, float p11 )
    {
        x_ = x;
        v_ = 0.0f;
        p_ = { p00, 0.0f, 0.0f, p11 };
    }

    // Full predict: the state moves by v * dt, the covariance grows.
    void predict( float dt, float q )
    {
        x_ += v_ * dt;
        predict_cov( dt, q );
    }

    // Covariance-only predict: grows P exactly like the full predict step but leaves the state
    // untouched. Used while a confirmed track is unmatched (miss >= 2): the position stays
    // frozen so the IoU gate can still re-associate the reappearing object at the last seen spot,
    // but the covariance keeps growing -- otherwise P sat at its post-update size for the whole
    // gap and the tiny Kalman gain made the track crawl to the new measurement instead of snapping.
    void predict_cov( float dt, float q )
    {
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
    // unphysical speed.
    void update( float z, float r, float vmax )
    {
        const float s = p_[0] + r;
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
        p_[0] = ( 1.0f - k0 ) * p00;
        p_[1] = ( 1.0f - k0 ) * p01;
        p_[2] = p10 - k1 * p00;  // original p00 -- the x update above must not leak in
        p_[3] = p11 - k1 * p01;
    }

    // Velocity decay (a coasting track): v *= factor.
    void damp( float factor ) { v_ *= factor; }

    float x() const { return x_; }
    float v() const { return v_; }
    const std::array<float, 4>& p() const { return p_; }

private:
    float x_ = 0.0f;
    float v_ = 0.0f;
    std::array<float, 4> p_ = { 10000.0f, 0.0f, 0.0f, 10000.0f };
};
