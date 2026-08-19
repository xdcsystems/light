set(SENSOR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS
        "${SENSOR_ROOT}/src/main.cpp"
        "${SENSOR_ROOT}/src/sensors/bh1750.cpp"
        "${SENSOR_ROOT}/src/transports/wifi_esp32.cpp"
    INCLUDE_DIRS
        "${SENSOR_ROOT}/include"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_driver_i2c
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PLATFORM_ESP32=1)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
