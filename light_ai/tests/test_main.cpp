// light_ai_tests: every suite of tests/suites.hpp, in this order. Arguments (a suite filter, --repeat N):
// tests/suite_runner.hpp. scripts/test.sh runs it with none: all suites, once.

#include <iostream>

#include "check.hpp"
#include "suite_runner.hpp"
#include "suites.hpp"

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the type of main's argv as char**
int main( int argc, char** argv )
{
    const Suite suites[] = {
        { "check_harness", &test_check_harness },
        { "pod_containers", &test_pod_containers },
        { "box_math", &test_box_math },
        { "fp16", &test_fp16 },
        { "rga_limits", &test_rga_limits },
        { "fixed_fmt", &test_fixed_fmt },
        { "line_scan", &test_line_scan },
        { "yolo_decoder", &test_yolo_decoder },
        { "geometry", &test_geometry },
        { "jpeg_probe", &test_jpeg_probe },
        { "byte_window", &test_byte_window },
        { "status", &test_status },
        { "stop_flag", &test_stop_flag },
        { "joining_thread", &test_joining_thread },
        { "tcp_retry", &test_tcp_retry },
        { "tcp_send", &test_tcp_send },
        { "yolo_head_shape", &test_yolo_head_shape },
        { "http_url", &test_http_url },
        { "http_head", &test_http_head },
        { "multipart_reader", &test_multipart_reader },
        { "buffered_socket", &test_buffered_socket },
        { "mjpeg_slot_pool", &test_mjpeg_slot_pool },
        { "net_mjpeg_source", &test_net_mjpeg_source },
        { "box_filter", &test_box_filter },
        { "box_smoother", &test_box_smoother },
        { "kalman_axis", &test_kalman_axis },
        { "associator", &test_associator },
        { "auto_orient", &test_auto_orient },
        { "trail_window", &test_trail_window },
        { "tracker", &test_tracker },
        { "glyph_font", &test_glyph_font },
        { "callout_layout", &test_callout_layout },
        { "overlay_raster", &test_overlay_raster },
        { "box_mapper", &test_box_mapper },
        { "reference_chain", &test_reference_chain },
        { "config", &test_config },
        { "signals", &test_signals },
        { "log", &test_log },
        { "cpu_affinity", &test_cpu_affinity },
        { "inference_pool", &test_inference_pool },
        { "timings", &test_timings },
        { "perf_clock", &test_perf_clock },
        { "perf_reporter", &test_perf_reporter },
        { "console_reporter", &test_console_reporter },
        { "cal_dumper", &test_cal_dumper },
        { "usb_node_locator", &test_usb_node_locator },
        { "source_channel", &test_source_channel },
        { "source_threads", &test_source_threads },
        { "source_set", &test_source_set },
        { "source_prober", &test_source_prober },
        { "source_prober_threads", &test_source_prober_threads },
        { "encoder_config", &test_encoder_config },
        { "param_set_cache", &test_param_set_cache },
        { "pts_clock", &test_pts_clock },
        { "encode_pacer", &test_encode_pacer },
        { "session_warmup", &test_session_warmup },
        { "idle_plate", &test_idle_plate },
        { "preview_ring", &test_preview_ring },
        { "preview_ring_threads", &test_preview_ring_threads },
        { "packet_queue", &test_packet_queue },
        { "packet_queue_threads", &test_packet_queue_threads },
        { "session_gate", &test_session_gate },
        { "session_gate_threads", &test_session_gate_threads },
        { "infer_slot_threads", &test_infer_slot_threads },
        { "global_track", &test_global_track },
        { "reid_worker_tsan", &test_reid_worker_tsan },
        { "pi_mutex", &test_pi_mutex },
        { "sd_watchdog", &test_sd_watchdog },
    };
    return run_suites( "light_ai_tests", suites, argc, argv, std::cout, std::cerr );
}
