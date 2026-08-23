#pragma once

#include <atomic>

#ifdef UBUS_ENABLED

#include <libubus.h>
#include <libubox/blob.h>
#include <libubox/uloop.h>
#include <libubox/utils.h>

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

    StatsCollector& get_stats() { return stats_; }
    LastStatus& get_last_status() { return last_status_; }
    UdpServer& get_server() { return server_; }
    blob_buf& reply_buf() { return session_.reply; }

    static UbusExporter* from_object(ubus_object* obj);
    static UbusExporter* from_timeout(uloop_timeout* timeout);

private:
    // POD под C-API ubus/uloop: offsetof по этому типу — standard-layout.
    struct Session {
        ubus_object obj{};
        ubus_object_type type{};
        uloop_timeout reconnect_timer{};
        blob_buf reply{};
        UbusExporter* owner = nullptr;
    };

    void retry_connect();
    void reset_context();
    void schedule_reconnect(const char* reason);
    void setup_object();

    ubus_context* ctx_{nullptr};
    Session session_{};
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
