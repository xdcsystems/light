// Test suites of light_ai_tests: one free function per module under test.
// Each suite is self-contained; test_main.cpp runs them in this order.

#pragma once

class TestRun;

void test_pod_containers( TestRun& t );
void test_box_math( TestRun& t );
void test_geometry( TestRun& t );
void test_jpeg_probe( TestRun& t );
void test_box_smoother( TestRun& t );
void test_tracker( TestRun& t );
void test_box_mapper( TestRun& t );
void test_config( TestRun& t );
void test_signals( TestRun& t );
void test_log( TestRun& t );
void test_inference_pool( TestRun& t );
