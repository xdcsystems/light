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
    # modules under test
    ${LIGHT_AI_ROOT}/src/core/geometry.cpp
    ${LIGHT_AI_ROOT}/src/box_smoother.cpp
    ${LIGHT_AI_ROOT}/src/tracker.cpp
    ${LIGHT_AI_ROOT}/src/box_mapper.cpp
    ${LIGHT_AI_ROOT}/src/config/cli.cpp
    ${LIGHT_AI_ROOT}/src/config/option_registry.cpp
    ${LIGHT_AI_ROOT}/src/config/config_file.cpp
    ${LIGHT_AI_ROOT}/src/process_env.cpp
    ${LIGHT_AI_ROOT}/src/signal_waiter.cpp
    ${LIGHT_AI_ROOT}/externals/inih/ini.c
)
# Same inih switch as the main target: the config-file handler relies on it.
target_compile_definitions(light_ai_tests PRIVATE INI_CALL_HANDLER_ON_NEW_SECTION=1)

target_include_directories(light_ai_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIGHT_AI_ROOT}/include
    ${LIGHT_AI_ROOT}/externals/inih
)

target_compile_options(light_ai_tests PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(light_ai_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME light_ai_tests COMMAND light_ai_tests)
