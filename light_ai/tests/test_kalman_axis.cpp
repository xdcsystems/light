// KalmanAxis against hand-computed values: r = 25, q = 2000, dt = 0.1 s
// (the Tracker tuning), a spawn-like start P = diag(25, 900).

#include "core/kalman_axis.hpp"
#include "check.hpp"
#include "suites.hpp"

void test_kalman_axis( TestRun& t )
{
    KalmanAxis k;
    k.reset( 10.0f, 25.0f, 900.0f );
    CHECK( t, k.x() == 10.0f && k.v() == 0.0f );
    CHECK( t, k.p()[0] == 25.0f && k.p()[1] == 0.0f && k.p()[2] == 0.0f && k.p()[3] == 900.0f );

    // predict: v = 0, so x stays; P00 = 25 + 0.01 * 900 + 2000 * 0.001 / 3,
    // P01 = P10 = 0.1 * 900 + 2000 * 0.01 / 2, P11 = 900 + 2000 * 0.1.
    k.predict( 0.1f, 2000.0f );
    CHECK( t, k.x() == 10.0f );
    CHECK_NEAR( t, k.p()[0], 34.666667f, 1e-3f );
    CHECK_NEAR( t, k.p()[1], 100.0f, 1e-3f );
    CHECK_NEAR( t, k.p()[2], 100.0f, 1e-3f );
    CHECK_NEAR( t, k.p()[3], 1100.0f, 1e-3f );

    // update with z = 20: s = 59.666667, k0 = 0.581006, k1 = 1.675978, y = 10.
    k.update( 20.0f, 25.0f, 500.0f );
    CHECK_NEAR( t, k.x(), 15.810056f, 1e-3f );
    CHECK_NEAR( t, k.v(), 16.759777f, 1e-3f );
    CHECK_NEAR( t, k.p()[0], 14.525140f, 1e-3f );  // (1 - k0) * P00
    CHECK_NEAR( t, k.p()[1], 41.899441f, 1e-3f );  // (1 - k0) * P01
    CHECK_NEAR( t, k.p()[2], 41.899441f, 1e-3f );  // P10 - k1 * P00 (the old P00)
    CHECK_NEAR( t, k.p()[3], 932.402235f, 1e-2f ); // P11 - k1 * P01

    // predict moves the state by v * dt; predict_cov does not.
    k.predict( 0.1f, 2000.0f );
    CHECK_NEAR( t, k.x(), 17.486034f, 1e-3f );
    const float x_before = k.x();
    const float p00_before = k.p()[0];
    k.predict_cov( 0.1f, 2000.0f );
    CHECK( t, k.x() == x_before );
    CHECK( t, k.p()[0] > p00_before );

    // damp: v *= factor.
    const float v_before = k.v();
    k.damp( 0.9f );
    CHECK_NEAR( t, k.v(), v_before * 0.9f, 1e-6f );

    // The |v| clamp: a wild measurement (k1 * y = 1676 px/s) stops at vmax.
    KalmanAxis wild;
    wild.reset( 0.0f, 25.0f, 900.0f );
    wild.predict( 0.1f, 2000.0f );
    wild.update( 1000.0f, 25.0f, 500.0f );
    CHECK( t, wild.v() == 500.0f );
    KalmanAxis wild_neg;
    wild_neg.reset( 0.0f, 25.0f, 900.0f );
    wild_neg.predict( 0.1f, 2000.0f );
    wild_neg.update( -1000.0f, 25.0f, 500.0f );
    CHECK( t, wild_neg.v() == -500.0f );
}
