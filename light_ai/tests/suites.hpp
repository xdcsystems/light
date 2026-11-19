// Test suites of light_ai_tests: one free function per module under test.
// Each suite is self-contained; test_main.cpp runs them in this order (check_harness first).

#pragma once

class TestRun;

void test_check_harness( TestRun& t );
void test_status( TestRun& t );
void test_stop_flag( TestRun& t );
void test_tcp_retry( TestRun& t );
void test_yolo_head_shape( TestRun& t );
void test_pod_containers( TestRun& t );
void test_box_math( TestRun& t );
void test_fp16( TestRun& t );
void test_rga_limits( TestRun& t );
void test_fixed_fmt( TestRun& t );
void test_line_scan( TestRun& t );
void test_tcp_send( TestRun& t );
void test_yolo_decoder( TestRun& t );
void test_geometry( TestRun& t );
void test_jpeg_probe( TestRun& t );
void test_byte_window( TestRun& t );
void test_http_url( TestRun& t );
void test_http_head( TestRun& t );
void test_multipart_reader( TestRun& t );
void test_buffered_socket( TestRun& t );
void test_mjpeg_slot_pool( TestRun& t );
void test_net_mjpeg_source( TestRun& t );
void test_box_smoother( TestRun& t );
void test_kalman_axis( TestRun& t );
void test_associator( TestRun& t );
void test_auto_orient( TestRun& t );
void test_trail_window( TestRun& t );
void test_tracker( TestRun& t );
void test_glyph_font( TestRun& t );
void test_callout_layout( TestRun& t );
void test_box_mapper( TestRun& t );
void test_reference_chain( TestRun& t );
void test_config( TestRun& t );
void test_signals( TestRun& t );
void test_log( TestRun& t );
void test_inference_pool( TestRun& t );
void test_timings( TestRun& t );
void test_console_reporter( TestRun& t );
void test_cal_dumper( TestRun& t );
void test_usb_node_locator( TestRun& t );
void test_source_channel( TestRun& t );
void test_source_threads( TestRun& t );
void test_source_set( TestRun& t );
void test_source_prober( TestRun& t );
void test_source_prober_threads( TestRun& t );
void test_encoder_config( TestRun& t );
void test_param_set_cache( TestRun& t );
void test_pts_clock( TestRun& t );
void test_encode_pacer( TestRun& t );
void test_session_warmup( TestRun& t );
void test_idle_plate( TestRun& t );
void test_preview_ring( TestRun& t );
void test_preview_ring_threads( TestRun& t );
void test_packet_queue( TestRun& t );
void test_packet_queue_threads( TestRun& t );
void test_session_gate( TestRun& t );
void test_session_gate_threads( TestRun& t );
void test_infer_slot_threads( TestRun& t );
void test_global_track( TestRun& t );
void test_reid_worker_tsan( TestRun& t );
