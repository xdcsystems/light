#pragma once

#if defined(PLATFORM_ESP32)
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"

    inline void platform_delay_ms(int ms) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
#else
    #include <unistd.h>

    inline void platform_delay_ms(int ms) {
        // sleep принимает секунды, округляем вверх
        sleep((ms + 999) / 1000);
    }
#endif
