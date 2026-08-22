cmake_minimum_required(VERSION 3.16)
project(light_control CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED OPENWRT_BUILD)
    set(OPENWRT_BUILD OFF)
endif()

if(OPENWRT_BUILD)
    # Определяем переменные окружения для кросс-компиляции
    if(NOT DEFINED CROSS_COMPILE)
        set(CROSS_COMPILE "mips-openwrt-linux-musl-")
    endif()

    if(NOT DEFINED TARGET_ARCH)
        set(TARGET_ARCH "mips")
    endif()

    if(NOT DEFINED STAGING_DIR)
        set(STAGING_DIR "/home/xdcsystems/Projects/light/light_control/externals/openwrt/staging_dir")
    endif()

    # Добавляем путь к инструментам в PATH
    set(ENV{PATH} "${STAGING_DIR}/host/bin:$ENV{PATH}")

    # Настраиваем компиляторы
    set(CMAKE_C_COMPILER "${CROSS_COMPILE}gcc")
    set(CMAKE_CXX_COMPILER "${CROSS_COMPILE}g++")

    # Указываем корневую директорию для поиска библиотек и заголовков
    set(CMAKE_SYSROOT "${STAGING_DIR}/target-mips_24kc_musl")

    # Добавляем пути к include и lib
    include_directories(
        ${STAGING_DIR}/target-mips_24kc_musl/usr/include
    )

    link_directories(
        ${STAGING_DIR}/target-mips_24kc_musl/usr/lib
    )
endif()

# --- ВАЖНО: Указываем пути к библиотекам OpenWrt ---
# Эти пути генерируются автоматически при сборке пакета в OpenWrt
if(DEFINED ENV{STAGING_DIR_TARGET})
    set(OPENWRT_LIB_DIR "$ENV{STAGING_DIR_TARGET}/usr/lib")
    set(OPENWRT_INC_DIR "$ENV{STAGING_DIR_TARGET}/usr/include")
else()
    # Если собираешь локально без переменных OpenWrt (для тестов)
    message(WARNING "OpenWrt staging dir not found. Using default search paths.")
endif()

option(ENABLE_UBUS "Enable UBus support" OFF)

if(OPENWRT_BUILD)
    set(ENABLE_UBUS ON CACHE BOOL "Force UBus for OpenWRT" FORCE)
    add_compile_definitions(OPENWRT_BUILD)
endif()

if(ENABLE_UBUS)
    add_compile_definitions(UBUS_ENABLED)
endif()

add_executable(light_control)

target_sources(light_control PRIVATE
    src/main.cpp
    src/application.cpp
    src/udp_server.cpp
    src/protocol_parser.cpp
    src/logger.cpp
    src/stats_collector.cpp
    src/ubus_exporter.cpp
    src/uci_settings.cpp
    src/brightness_curve.cpp
    src/last_status.cpp
    src/scene_scheduler.cpp
    src/dimmer.cpp
)

target_include_directories(light_control PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libubox
)

# atomic нужен на MIPS для std::atomic
if(OPENWRT_BUILD)
    # Важно: порядок библиотек и явное указание директории поиска
    link_directories(${OPENWRT_LIB_DIR})
    target_link_libraries(light_control PRIVATE atomic uci)
endif()

if(ENABLE_UBUS AND OPENWRT_BUILD)
    target_link_libraries(light_control PRIVATE ubox ubus)
endif()

message(STATUS "OpenWRT build: ${OPENWRT_BUILD}")
message(STATUS "UBus enabled: ${ENABLE_UBUS}")

if(OPENWRT_BUILD)
    install(TARGETS light_control DESTINATION bin)
endif()

if(NOT OPENWRT_BUILD)
    find_package(Threads REQUIRED)
    target_link_libraries(light_control PRIVATE Threads::Threads)

    add_executable(light_control_selftest
        tests/selftest.cpp
        src/protocol_parser.cpp
        src/brightness_curve.cpp
        src/scene_scheduler.cpp
    )
    target_include_directories(light_control_selftest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()