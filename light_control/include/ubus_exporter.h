#pragma once

#include <atomic>

#ifdef UBUS_ENABLED

#include <libubus.h>
#include <libubox/utils.h>
#include <libubox/blob.h>
#include <libubox/uloop.h>

class StatsCollector;
class LastStatus;
class Logger;
class UdpServer;

class UbusExporter {
public:
    explicit UbusExporter(StatsCollector& stats, LastStatus& last_status,
                          UdpServer& server, Logger& logger);
    ~UbusExporter();

    bool init();
    void run(std::atomic<bool>& stop_flag);
    void stop();

    // Публичные геттеры для доступа из C-колбэков
    StatsCollector& get_stats() { return stats_; }
    LastStatus& get_last_status() { return last_status_; }
    UdpServer& get_server() { return server_; }

private:
    void retry_connect();
    void reset_context();
    void schedule_reconnect(const char* reason);

private:    
    ubus_context* ctx_ { nullptr };
    uloop_timeout reconnect_timer_ {};
    StatsCollector& stats_;
    LastStatus& last_status_;
    UdpServer& server_;
    Logger& logger_;
};

#else

class UbusExporter {
public:
    explicit UbusExporter() {}
    ~UbusExporter() = default;
    bool init() { return true; }
    void run(std::atomic<bool>&) {}
    void stop() {}
};

#endif
