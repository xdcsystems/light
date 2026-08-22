cmake_minimum_required(VERSION 3.16)
project(light_sensor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host CMake: LINUX / TEST only. ESP32 is the ESP-IDF project in esp32/.
option(PLATFORM_LINUX  "Linux/COKE PI/OpenWrt (UDP POSIX)" OFF)
option(PLATFORM_TEST   "Test mode (NullTransport)" OFF)

set(PLATFORM_COUNT 0)
if(PLATFORM_LINUX)
    math(EXPR PLATFORM_COUNT "${PLATFORM_COUNT} + 1")
endif()
if(PLATFORM_TEST)
    math(EXPR PLATFORM_COUNT "${PLATFORM_COUNT} + 1")
endif()

if(${PLATFORM_COUNT} GREATER 1)
    message(FATAL_ERROR "Choose exactly one platform: PLATFORM_LINUX / PLATFORM_TEST")
endif()
if(${PLATFORM_COUNT} EQUAL 0)
    message(FATAL_ERROR "Specify one platform, e.g. -DPLATFORM_LINUX=ON or -DPLATFORM_TEST=ON")
endif()

if(PLATFORM_LINUX)
    add_compile_definitions(PLATFORM_LINUX=1)
    message(STATUS "Сборка: Linux/COKE PI (UDP POSIX)")
elseif(PLATFORM_TEST)
    add_compile_definitions(PLATFORM_TEST=1)
    message(STATUS "Сборка: Тест (NullTransport)")
endif()

set(BASE_SOURCES
    src/main.cpp
    src/app_core.cpp
    src/sensors/bh1750.cpp
    src/sensors/stub.cpp
)
add_executable(${PROJECT_NAME} ${BASE_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/include)

if(PLATFORM_LINUX)
    target_sources(${PROJECT_NAME} PRIVATE src/transports/udp_posix.cpp)
elseif(PLATFORM_TEST)
    target_sources(${PROJECT_NAME} PRIVATE src/transports/null.cpp)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(${PROJECT_NAME} PRIVATE -O2 -Wall)
endif()
