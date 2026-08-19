#pragma once

#include <atomic>
#include <thread>

#include "logger.h"
#include "stats_collector.h"
#include "last_status.h"
#include "udp_server.h"

#ifdef UBUS_ENABLED
#include "ubus_exporter.h"
#endif

class Application {
public:
    Application();
    ~Application() = default;

    // Инициализирует все компоненты, но НЕ крутит главный цикл.
    // 0 = running, 1 = disabled in UCI, -1 = error.
    int init();

    // Останавливает все компоненты (вызывается из main при SIGTERM)
    void stop();

private:
    Logger logger_;
    StatsCollector stats_;
    LastStatus last_status_;
    UdpServer server_;

    std::atomic<bool> should_stop_{false};
    std::thread receive_thread_;

#ifdef UBUS_ENABLED
    UbusExporter ubus_;
    std::thread ubus_thread_;
    bool ubus_initialized_ { false };
#endif
};
