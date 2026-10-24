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

# Self-built ffmpeg (prefix /usr/local) — for the M3.5 phase A0 preview only
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

set(RKNN_LIB_DIR "/usr/lib" CACHE PATH "librknnrt.so location")
# Machine-specific path is now an overridable cache variable
# (-DRKNN_MODEL_ZOO_INCLUDE=... instead of a hard-coded absolute path)
set(RKNN_MODEL_ZOO_INCLUDE "/mnt/nvme_data/work/rknn_model_zoo/3rdparty/rknpu2/include"
    CACHE PATH "directory containing rknn_api.h")

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
)

target_include_directories(light_ai PRIVATE
    include
    /usr/include/rockchip
    ${RKNN_MODEL_ZOO_INCLUDE}
    ${AV_INCLUDE_DIRS}
)

target_link_libraries(light_ai PRIVATE
    ${MPP_LIBRARIES}
    ${RGA_LIBRARIES}
    ${AV_LIBRARIES}
    rknnrt
    pthread
)

target_link_directories(light_ai PRIVATE ${RKNN_LIB_DIR} ${AV_LIBRARY_DIRS})
target_compile_options(light_ai PRIVATE -O2 -Wall -Wextra)
