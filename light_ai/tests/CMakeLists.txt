# Host-side unit tests for the pure-logic modules of light_ai.
# Standalone project: it needs only a C++17 compiler and pthreads -- no MPP/RGA/RKNN/ffmpeg/EGL --
# so it builds on the board and on any PC:
#   cmake -S tests -B build/tests && cmake --build build/tests && build/tests/light_ai_tests
# (or: bash scripts/test.sh). A module belongs here once it compiles without SDK headers.
cmake_minimum_required(VERSION 3.10)
project(light_ai_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(LIGHT_AI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(light_ai_tests
    test_main.cpp
    test_pod_containers.cpp
    test_box_math.cpp
    test_geometry.cpp
    test_jpeg_probe.cpp
    test_box_smoother.cpp
    test_tracker.cpp
    test_box_mapper.cpp
    test_config.cpp
    test_signals.cpp
    test_log.cpp
    test_inference_pool.cpp
    test_timings.cpp
    test_usb_node_locator.cpp
    test_source_channel.cpp
    test_source_threads.cpp
    test_source_set.cpp
    test_source_prober.cpp
    test_encoder_config.cpp
    test_param_set_cache.cpp
    test_pts_clock.cpp
    test_encode_pacer.cpp
    test_idle_plate.cpp
    test_packet_queue.cpp
    # modules under test
    ${LIGHT_AI_ROOT}/src/core/geometry.cpp
    ${LIGHT_AI_ROOT}/src/core/encoder_config.cpp
    ${LIGHT_AI_ROOT}/src/core/param_set_cache.cpp
    ${LIGHT_AI_ROOT}/src/core/pts_clock.cpp
    ${LIGHT_AI_ROOT}/src/core/encode_pacer.cpp
    ${LIGHT_AI_ROOT}/src/core/bitmap_font.cpp
    ${LIGHT_AI_ROOT}/src/core/idle_plate.cpp
    ${LIGHT_AI_ROOT}/src/box_smoother.cpp
    ${LIGHT_AI_ROOT}/src/tracker.cpp
    ${LIGHT_AI_ROOT}/src/box_mapper.cpp
    ${LIGHT_AI_ROOT}/src/config/cli.cpp
    ${LIGHT_AI_ROOT}/src/config/option_registry.cpp
    ${LIGHT_AI_ROOT}/src/config/config_file.cpp
    ${LIGHT_AI_ROOT}/src/process_env.cpp
    ${LIGHT_AI_ROOT}/src/signal_waiter.cpp
    ${LIGHT_AI_ROOT}/src/cpu_affinity.cpp
    ${LIGHT_AI_ROOT}/src/timings.cpp
    ${LIGHT_AI_ROOT}/externals/inih/ini.c
)
# Same inih switch as the main target: the config-file handler relies on it.
target_compile_definitions(light_ai_tests PRIVATE INI_CALL_HANDLER_ON_NEW_SECTION=1)

target_include_directories(light_ai_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIGHT_AI_ROOT}/include
    ${LIGHT_AI_ROOT}/externals/inih
    ${LIGHT_AI_ROOT}/externals/fonts  # stb_easy_font (header-only), for src/core/bitmap_font.cpp
)

target_compile_options(light_ai_tests PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(light_ai_tests PRIVATE Threads::Threads)

# Threaded suites under ThreadSanitizer (the SourceChannel hand-over, the
# SourceProber thread against a detaching frame thread): a data
# race fails the run. Separate binary: TSan slows everything ~5-15x and must
# not mix with the plain build. Built and run by scripts/test.sh unless SKIP_TSAN=1.
add_executable(light_ai_tests_tsan EXCLUDE_FROM_ALL
    tsan_main.cpp
    test_source_threads.cpp
    test_source_prober.cpp
    test_packet_queue.cpp
    ${LIGHT_AI_ROOT}/src/core/geometry.cpp
)
target_include_directories(light_ai_tests_tsan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIGHT_AI_ROOT}/include
)
target_compile_options(light_ai_tests_tsan PRIVATE -Wall -Wextra -fsanitize=thread -O1 -g)
target_link_options(light_ai_tests_tsan PRIVATE -fsanitize=thread)
target_link_libraries(light_ai_tests_tsan PRIVATE Threads::Threads)

enable_testing()
add_test(NAME light_ai_tests COMMAND light_ai_tests)
