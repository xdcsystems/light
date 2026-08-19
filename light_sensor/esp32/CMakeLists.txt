# ESP-IDF 5.2+ project for ESP32-WROOM-32 (ESP32-D0WD-V3, 40 MHz, 4 MB flash).
# Sources live in ../src and ../include — this directory is only the IDF wrapper.
cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# IDF 6 compiles esp_libc/realpath.c with -std=gnu23 and no _GNU_SOURCE.
# Newlib then hides strchrnul behind __GNU_VISIBLE → -Werror. Harmless on 5.x.
idf_build_set_property(COMPILE_OPTIONS "-D_GNU_SOURCE" APPEND)
project(light_sensor)
