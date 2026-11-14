#include <iostream>

#include "check.hpp"
#include "suites.hpp"

namespace
{
    void run_suite( TestRun& t, const char* name, void ( *suite )( TestRun& ) )
    {
        const int checks0 = t.checks();
        const int fails0 = t.failures();
        suite( t );
        std::cout << ( t.failures() == fails0 ? "ok    " : "FAIL  " ) << name << " (" << t.checks() - checks0 << " checks)\n";
    }
}  // namespace

int main()
{
    TestRun t;
    run_suite( t, "pod_containers", &test_pod_containers );
    run_suite( t, "box_math", &test_box_math );
    run_suite( t, "geometry", &test_geometry );
    run_suite( t, "jpeg_probe", &test_jpeg_probe );
    run_suite( t, "box_smoother", &test_box_smoother );
    run_suite( t, "tracker", &test_tracker );
    run_suite( t, "box_mapper", &test_box_mapper );
    run_suite( t, "config", &test_config );
    run_suite( t, "signals", &test_signals );
    run_suite( t, "log", &test_log );
    run_suite( t, "inference_pool", &test_inference_pool );
    run_suite( t, "timings", &test_timings );
    run_suite( t, "usb_node_locator", &test_usb_node_locator );
    run_suite( t, "source_channel", &test_source_channel );
    run_suite( t, "source_threads", &test_source_threads );
    run_suite( t, "source_set", &test_source_set );
    run_suite( t, "source_prober", &test_source_prober );
    run_suite( t, "source_prober_threads", &test_source_prober_threads );
    std::cout << "light_ai_tests: " << t.checks() << " checks, " << t.failures() << " failed\n";
    return t.failures() == 0 ? 0 : 1;
}
