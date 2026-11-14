cmake_minimum_required(VERSION 3.10)
project(light_ai C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPP REQUIRED rockchip_mpp)
pkg_check_modules(RGA REQUIRED librga)

# M3.7: GPU box drawing (Mali G610) — EGL/GBM + GLES2/3 (ES3 context, ES2 fallback), entry points via
# eglGetProcAddress at runtime; libdl for the dlopen'ed libgbm.
pkg_check_modules(EGL REQUIRED egl glesv2)

# Self-built ffmpeg (prefix /usr/local) — for the M3.5 phase A0 preview only.
# Pin libav* to the self-built ffmpeg in /usr/local. The distro ffmpeg 4.x
# headers/libs must never leak back into the build: they caused the ABI
# mismatch crashes (codecpar segfault, 'No codec tag defined').
# PKG_CONFIG_LIBDIR *replaces* the pkg-config search list, so the distro
# .pc files cannot be picked up. Override: cmake -DFFMPEG_PKGCONF_DIR=...
if(NOT DEFINED FFMPEG_PKGCONF_DIR)
    set(FFMPEG_PKGCONF_DIR "/usr/local/lib/pkgconfig")
endif()
if(NOT EXISTS "${FFMPEG_PKGCONF_DIR}/libavformat.pc")
    message(FATAL_ERROR "libavformat.pc not found in ${FFMPEG_PKGCONF_DIR}\n"
        "Install the self-built ffmpeg as shared libs first:\n"
        "  cd ~/work/ffmpeg-rockchip && ./configure --enable-shared "
        "--disable-static --prefix=/usr/local <flags>\n"
        "  make -j$(nproc) && sudo make install && sudo ldconfig")
endif()
set(ENV{PKG_CONFIG_LIBDIR} "${FFMPEG_PKGCONF_DIR}")

pkg_check_modules(AV REQUIRED libavformat libavcodec libavutil)

# --- RKNN runtime -----------------------------------------------------------
# rknn_api.h and librknnrt.so are located via standard CMake find calls.
# Override: cmake -DRKNN_ROOT=/path/to/rknpu2 .. (or export the
# RKNN_ROOT environment variable). The legacy hardcoded NVMe path is
# kept only as HINTS — checked first on the dev board, while on any
# other machine CMake silently proceeds to the standard system paths.
find_path(RKNN_API_INCLUDE_DIR rknn_api.h
    HINTS ${RKNN_ROOT}/include $ENV{RKNN_ROOT}/include
          /mnt/nvme_data/work/rknn_model_zoo/3rdparty/rknpu2/include
    PATHS /usr/local/include /usr/include
    PATH_SUFFIXES rknn
)

find_library(RKNN_RT_LIBRARY rknnrt
    HINTS ${RKNN_ROOT}/lib $ENV{RKNN_ROOT}/lib
    PATHS /usr/local/lib /usr/lib
)

if(NOT RKNN_API_INCLUDE_DIR)
    message(FATAL_ERROR
        "rknn_api.h not found.\n"
        "Set -DRKNN_ROOT=/path/to/rknpu2 (the dir containing include/ and lib/),\n"
        "or install the Rockchip NPU runtime system-wide (rknn_api.h in /usr/include[/rknn]).")
endif()
if(NOT RKNN_RT_LIBRARY)
    message(FATAL_ERROR
        "librknnrt.so not found (looked in RKNN_ROOT/lib, /usr/local/lib, /usr/lib).\n"
        "Set -DRKNN_ROOT=/path/to/rknpu2 or install librknnrt.so system-wide.")
endif()
message(STATUS "RKNN include: ${RKNN_API_INCLUDE_DIR}")
message(STATUS "RKNN lib:     ${RKNN_RT_LIBRARY}")

set(CMAKE_BUILD_RPATH "/usr/local/lib")
set(CMAKE_INSTALL_RPATH "/usr/local/lib")

add_executable(light_ai
    src/main.cpp
    src/camera_v4l2.cpp
    src/mpp_decoder.cpp
    src/rga_resizer.cpp
    src/rknn_engine.cpp
    src/yolo_postprocess.cpp
    src/pipeline.cpp
    src/sources.cpp
    src/overlay_sink.cpp
    src/tcp_pull_server.cpp
    src/net_preview.cpp
    src/net_encoder.cpp
    src/tracker.cpp
    src/gpu_box_drawer.cpp
    src/net_mjpeg_source.cpp
    src/box_mapper.cpp
    src/box_smoother.cpp
    src/timings.cpp
    src/console_reporter.cpp
    src/cal_dumper.cpp
    src/config/cli.cpp
    src/config/option_registry.cpp
    src/config/config_file.cpp
    src/core/geometry.cpp
    src/core/encoder_config.cpp
    src/core/param_set_cache.cpp
    src/core/pts_clock.cpp
    src/core/encode_pacer.cpp
    src/core/session_warmup.cpp
    src/core/bitmap_font.cpp
    src/core/idle_plate.cpp
    src/core/http_url.cpp
    src/net/encoder_frame_pool.cpp
    src/net/frame_composer.cpp
    src/net/av_packet_queue.cpp
    src/net/av_packet.cpp
    src/net/mux_session.cpp
    src/net/h264_encoder.cpp
    src/net/network_init.cpp
    src/process_env.cpp
    src/signal_waiter.cpp
    src/cpu_affinity.cpp
    externals/inih/ini.c
)
target_compile_definitions(light_ai PRIVATE INI_CALL_HANDLER_ON_NEW_SECTION=1)

target_include_directories(light_ai PRIVATE
    include
    externals/inih
    externals/fonts
    /usr/include/rockchip
    ${RKNN_API_INCLUDE_DIR}
    ${AV_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
)

target_link_libraries(light_ai PRIVATE
    ${MPP_LIBRARIES}
    ${RGA_LIBRARIES}
    ${AV_LIBRARIES}
    ${RKNN_RT_LIBRARY}
    pthread
    ${EGL_LIBRARIES}
    dl
)

target_link_directories(light_ai PRIVATE ${AV_LIBRARY_DIRS})

# --- CPU flags (RK3588: 4x Cortex-A76 cpu4-7 + 4x Cortex-A55 cpu0-3) --------
#
# Both clusters expose an IDENTICAL feature set (verified on the board 2026-11-03,
# /proc/cpuinfo, cpu0 == cpu4): fp asimd aes sha2 crc32 atomics (LSE)
# fphp asimdhp lrcpc dcpop asimddp (dotprod). A binary built with +dotprod/+crypto 
# is therefore legal on ALL eight cores. Instruction set aside, the hot
# threads (pipeline main + NPU workers) are pinned to the A76 cluster by
# cpu_affinity.cpp (best-effort; 2026-11-12 audit follow-up): a bursty
# cv-woken worker may drift to A55 under CFS/EAS. Network/background
# threads stay with the scheduler.
# LSE atomics (std::atomic stop flag) are valid on A55 as well.
# Clang rejects the dotted "cortex-a76.cortex-a55" form in -mtune (GCC-only big.LITTLE syntax),
# so the compiler branches are separate.
# -DTARGET_CPU_FLAGS="..." from the CLI enters the cache BEFORE this
# block, and DEFINED sees both cache and normal variables: the default is set only
# when no override was given. Previously a plain set() shadowed the cache and
# silently overwrote the CLI value.

if(NOT DEFINED TARGET_CPU_FLAGS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(TARGET_CPU_FLAGS "-mcpu=cortex-a76+crypto+dotprod")
    else()
        set(TARGET_CPU_FLAGS "-mcpu=cortex-a76+dotprod+crypto -mtune=cortex-a76.cortex-a55")
    endif()
    set(TARGET_CPU_FLAGS "${TARGET_CPU_FLAGS}" CACHE STRING
        "CPU flags for the target SoC")
endif()

# spaces -> semicolons so CMake passes each flag to the compiler as a separate argument
string(REPLACE " " ";" TARGET_CPU_FLAGS_LIST "${TARGET_CPU_FLAGS}")

target_compile_options(light_ai PRIVATE
    ${TARGET_CPU_FLAGS_LIST}
    -O3
    -Wall -Wextra
)
