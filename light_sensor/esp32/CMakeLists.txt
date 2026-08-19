# ESP-IDF 5.2+ project for ESP32-WROOM-32 (ESP32-D0WD-V3, 40 MHz, 4 MB flash).
# Sources live in ../src and ../include — this directory is only the IDF wrapper.
cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(light_sensor)
