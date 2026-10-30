cmake_minimum_required(VERSION 3.10)
project(light_ai CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPP REQUIRED rockchip_mpp)
pkg_check_modules(RGA REQUIRED librga)

# M3.7: GPU box drawing (Mali G610) — EGL/GBM + GLES2, entry points via
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
# rknn_api.h и librknnrt.so ищутся стандартными средствами CMake.
# Переопределение: cmake -DRKNN_ROOT=/path/to/rknpu2 ..  (или экспорт
# переменной окружения RKNN_ROOT). Старый захардкоженный путь с NVMe
# оставлен только как HINTS — первой подсказкой на текущей машине, на
# чужой CMake молча пойдёт дальше по стандартным путям.
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
    src/net_preview.cpp
    src/net_encoder.cpp
    src/gpu_box_drawer.cpp
)

target_include_directories(light_ai PRIVATE
    include
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

set(TARGET_CPU_FLAGS "-mcpu=cortex-a76.cortex-a55" CACHE STRING
    "CPU flags for the target SoC")

# пробелы -> точки с запятой, чтобы опции скормились компилятору по отдельности
string(REPLACE " " ";" TARGET_CPU_FLAGS_LIST "${TARGET_CPU_FLAGS}")

target_compile_options(light_ai PRIVATE
    ${TARGET_CPU_FLAGS_LIST}
    -O3
    -Wall -Wextra
)
