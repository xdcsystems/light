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
    src/pipeline.cpp
)

target_include_directories(light_ai PRIVATE
    include
    /usr/include/rockchip
    ${RKNN_MODEL_ZOO_INCLUDE}
)

target_link_libraries(light_ai PRIVATE
    ${MPP_LIBRARIES}
    ${RGA_LIBRARIES}
    rknnrt
    pthread
)

target_link_directories(light_ai PRIVATE ${RKNN_LIB_DIR})
target_compile_options(light_ai PRIVATE -O2 -Wall -Wextra)
