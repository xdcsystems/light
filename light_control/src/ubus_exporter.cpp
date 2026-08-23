#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <syslog.h>

#include "logger.h"
#include "ubus_exporter.h"

#ifdef UBUS_ENABLED

#include "stats_collector.h"
#include "last_status.h"
#include "udp_server.h"
#include "uci_settings.h"
#include "scene_scheduler.h"
#include "brightness_curve.h"

#include <libubox/blobmsg.h>

namespace {

int get_stats_cb(ubus_context* ctx, ubus_object* obj, ubus_request_data* req,
                 const char* method, blob_attr* msg) {
    (void)method;
    (void)msg;

    UbusExporter* exporter = UbusExporter::from_object(obj);
    if (!exporter) {
        syslog(LOG_ERR, "UBUS: get_stats_cb: exporter instance is null");
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    StatsCollector& stats = exporter->get_stats();
    blob_buf& bb = exporter->reply_buf();

    blob_buf_init(&bb, 0);
    blobmsg_add_u64(&bb, "packets_received", stats.packets_received());
    blobmsg_add_u64(&bb, "bytes_received", stats.bytes_received());
    blobmsg_add_u64(&bb, "errors", stats.errors());

    const int ret = ubus_send_reply(ctx, req, bb.head);
    if (ret != 0) {
        syslog(LOG_ERR, "ubus_send_reply failed: %d", ret);
    }
    blob_buf_free(&bb);
    return ret == 0 ? UBUS_STATUS_OK : UBUS_STATUS_UNKNOWN_ERROR;
}

int get_status_cb(ubus_context* ctx, ubus_object* obj, ubus_request_data* req,
                  const char* method, blob_attr* msg) {
    (void)method;
    (void)msg;

    UbusExporter* exporter = UbusExporter::from_object(obj);
    if (!exporter) {
        syslog(LOG_ERR, "UBUS: get_status_cb: exporter instance is null");
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    LastStatusSnapshot snap = exporter->get_last_status().snapshot();
    blob_buf& bb = exporter->reply_buf();

    blob_buf_init(&bb, 0);
    blobmsg_add_u8(&bb, "has_packet", snap.has_packet ? 1 : 0);
    blobmsg_add_u32(&bb, "device_id", snap.device_id);
    blobmsg_add_u32(&bb, "lux", snap.lux);
    blobmsg_add_u32(&bb, "brightness", snap.brightness);
    blobmsg_add_u8(&bb, "override", snap.override ? 1 : 0);
    blobmsg_add_string(&bb, "scene", snap.scene);
    blobmsg_add_string(&bb, "source_ip", snap.source_ip);
    blobmsg_add_u32(&bb, "source_port", snap.source_port);
    blobmsg_add_u64(&bb, "unix_time", snap.unix_time);

    const int ret = ubus_send_reply(ctx, req, bb.head);
    if (ret != 0) {
        syslog(LOG_ERR, "ubus_send_reply failed: %d", ret);
    }
    blob_buf_free(&bb);
    return ret == 0 ? UBUS_STATUS_OK : UBUS_STATUS_UNKNOWN_ERROR;
}

void add_map_entry(blob_buf* buf, uint16_t lux_below, uint8_t brightness,
                   const char* set_name) {
    void* table = blobmsg_open_table(buf, nullptr);
    blobmsg_add_u32(buf, "lux_below", lux_below);
    blobmsg_add_u32(buf, "brightness", brightness);
    if (set_name && set_name[0] != '\0') {
        blobmsg_add_string(buf, "set", set_name);
    }
    blobmsg_close_table(buf, table);
}

int get_config_cb(ubus_context* ctx, ubus_object* obj, ubus_request_data* req,
                  const char* method, blob_attr* msg) {
    (void)method;
    (void)msg;

    UbusExporter* exporter = UbusExporter::from_object(obj);
    if (!exporter) {
        syslog(LOG_ERR, "UBUS: get_config_cb: exporter instance is null");
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    UdpServer& server = exporter->get_server();
    const UciSettings cfg = server.copy_settings();
    blob_buf& bb = exporter->reply_buf();

    blob_buf_init(&bb, 0);
    blobmsg_add_u8(&bb, "enabled", cfg.enabled ? 1 : 0);
    blobmsg_add_u32(&bb, "port", cfg.port);
    blobmsg_add_u32(&bb, "bound_port", server.bound_port());
    blobmsg_add_string(&bb, "interface", cfg.iface);
    blobmsg_add_u8(&bb, "override", server.has_override() ? 1 : 0);
    blobmsg_add_u32(&bb, "override_brightness", server.override_value());

    void* maps = blobmsg_open_array(&bb, "map");
    const BrightnessCurve& curve = cfg.policy.default_curve();
    for (size_t i = 0; i < curve.size(); ++i) {
        const BrightnessCurve::Entry& entry = curve.at(i);
        add_map_entry(&bb, entry.lux_below, entry.brightness, nullptr);
    }
    for (size_t i = 0; i < cfg.policy.named_count(); ++i) {
        const BrightnessPolicy::NamedCurve& named = cfg.policy.named_at(i);
        for (size_t j = 0; j < named.curve.size(); ++j) {
            const BrightnessCurve::Entry& entry = named.curve.at(j);
            add_map_entry(&bb, entry.lux_below, entry.brightness, named.name);
        }
    }
    blobmsg_close_array(&bb, maps);

    void* scenes = blobmsg_open_array(&bb, "scenes");
    const SceneScheduler& scheduler = cfg.policy.scheduler();
    for (size_t i = 0; i < scheduler.size(); ++i) {
        const Scene& scene = scheduler.at(i);
        char from_time[8] = {0};
        char to_time[8] = {0};
        format_hhmm(scene.from_min, from_time, sizeof(from_time));
        format_hhmm(scene.to_min, to_time, sizeof(to_time));

        void* table = blobmsg_open_table(&bb, nullptr);
        blobmsg_add_string(&bb, "name", scene.name);
        blobmsg_add_string(&bb, "from_time", from_time);
        blobmsg_add_string(&bb, "to_time", to_time);
        if (scene.map_set[0] != '\0') {
            blobmsg_add_string(&bb, "map_set", scene.map_set);
        }
        if (scene.has_min) {
            blobmsg_add_u32(&bb, "min_brightness", scene.min_brightness);
        }
        if (scene.has_max) {
            blobmsg_add_u32(&bb, "max_brightness", scene.max_brightness);
        }
        blobmsg_close_table(&bb, table);
    }
    blobmsg_close_array(&bb, scenes);

    const int ret = ubus_send_reply(ctx, req, bb.head);
    if (ret != 0) {
        syslog(LOG_ERR, "ubus_send_reply failed: %d", ret);
    }
    blob_buf_free(&bb);
    return ret == 0 ? UBUS_STATUS_OK : UBUS_STATUS_UNKNOWN_ERROR;
}

int reload_cb(ubus_context* ctx, ubus_object* obj, ubus_request_data* req,
              const char* method, blob_attr* msg) {
    (void)method;
    (void)msg;

    UbusExporter* exporter = UbusExporter::from_object(obj);
    if (!exporter) {
        syslog(LOG_ERR, "UBUS: reload_cb: exporter instance is null");
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    UdpServer& server = exporter->get_server();
    server.reload_from_uci();
    const UciSettings cfg = server.copy_settings();
    blob_buf& bb = exporter->reply_buf();

    blob_buf_init(&bb, 0);
    blobmsg_add_u8(&bb, "ok", 1);
    blobmsg_add_u8(&bb, "enabled", cfg.enabled ? 1 : 0);
    blobmsg_add_u32(&bb, "port", cfg.port);
    blobmsg_add_u32(&bb, "bound_port", server.bound_port());
    blobmsg_add_u8(&bb, "override", server.has_override() ? 1 : 0);

    const int ret = ubus_send_reply(ctx, req, bb.head);
    if (ret != 0) {
        syslog(LOG_ERR, "ubus_send_reply failed: %d", ret);
    }
    blob_buf_free(&bb);
    return ret == 0 ? UBUS_STATUS_OK : UBUS_STATUS_UNKNOWN_ERROR;
}

enum {
    SET_BR_VALUE,
    SET_BR_RELEASE,
    SET_BR_MAX
};

const blobmsg_policy kSetBrightnessPolicy[SET_BR_MAX] = {
    {.name = "value", .type = BLOBMSG_TYPE_INT32},
    {.name = "release", .type = BLOBMSG_TYPE_BOOL},
};

int set_brightness_cb(ubus_context* ctx, ubus_object* obj, ubus_request_data* req,
                      const char* method, blob_attr* msg) {
    (void)method;

    UbusExporter* exporter = UbusExporter::from_object(obj);
    if (!exporter) {
        syslog(LOG_ERR, "UBUS: set_brightness_cb: exporter instance is null");
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    blob_attr* tb[SET_BR_MAX] = {};
    if (msg) {
        blobmsg_parse(kSetBrightnessPolicy, SET_BR_MAX, tb, blob_data(msg), blob_len(msg));
    }

    const bool release = tb[SET_BR_RELEASE] && blobmsg_get_bool(tb[SET_BR_RELEASE]);
    UdpServer& server = exporter->get_server();

    if (release) {
        server.clear_brightness_override();
    } else {
        if (!tb[SET_BR_VALUE]) {
            return UBUS_STATUS_INVALID_ARGUMENT;
        }
        const int32_t value = static_cast<int32_t>(blobmsg_get_u32(tb[SET_BR_VALUE]));
        if (value < 0) {
            return UBUS_STATUS_INVALID_ARGUMENT;
        }
        const uint8_t brightness = value > 100 ? 100 : static_cast<uint8_t>(value);
        server.set_brightness_override(brightness);
    }

    LastStatusSnapshot snap = exporter->get_last_status().snapshot();
    blob_buf& bb = exporter->reply_buf();
    blob_buf_init(&bb, 0);
    blobmsg_add_u8(&bb, "override", snap.override ? 1 : 0);
    blobmsg_add_u32(&bb, "brightness", snap.brightness);

    const int ret = ubus_send_reply(ctx, req, bb.head);
    if (ret != 0) {
        syslog(LOG_ERR, "ubus_send_reply failed: %d", ret);
    }
    blob_buf_free(&bb);
    return ret == 0 ? UBUS_STATUS_OK : UBUS_STATUS_UNKNOWN_ERROR;
}

const ubus_method kMethods[] = {
    UBUS_METHOD_NOARG("get_stats", get_stats_cb),
    UBUS_METHOD_NOARG("get_status", get_status_cb),
    UBUS_METHOD_NOARG("get_config", get_config_cb),
    UBUS_METHOD_NOARG("reload", reload_cb),
    UBUS_METHOD("set_brightness", set_brightness_cb, kSetBrightnessPolicy),
};

}  // namespace

UbusExporter* UbusExporter::from_object(ubus_object* obj) {
    if (!obj) {
        return nullptr;
    }
    Session* session = reinterpret_cast<Session*>(
        reinterpret_cast<char*>(obj) - offsetof(Session, obj));
    return session->owner;
}

UbusExporter* UbusExporter::from_timeout(uloop_timeout* timeout) {
    if (!timeout) {
        return nullptr;
    }
    Session* session = reinterpret_cast<Session*>(
        reinterpret_cast<char*>(timeout) - offsetof(Session, reconnect_timer));
    return session->owner;
}

UbusExporter::UbusExporter(StatsCollector& stats, LastStatus& last_status,
                           UdpServer& server, Logger& logger)
    : stats_(stats), last_status_(last_status), server_(server), logger_(logger) {
    setup_object();
}

UbusExporter::~UbusExporter() {
    if (ctx_) {
        ubus_remove_object(ctx_, &session_.obj);
        reset_context();
    }
}

void UbusExporter::setup_object() {
    session_.owner = this;
    session_.type.name = "light_control";
    session_.type.id = 0;
    session_.type.methods = kMethods;
    session_.type.n_methods = ARRAY_SIZE(kMethods);
    session_.obj.name = "light_control";
    session_.obj.type = &session_.type;
    session_.obj.methods = kMethods;
    session_.obj.n_methods = ARRAY_SIZE(kMethods);
}

void UbusExporter::schedule_reconnect(const char* reason) {
    session_.reconnect_timer.cb = [](uloop_timeout* t) {
        UbusExporter* exp = UbusExporter::from_timeout(t);
        if (exp) {
            exp->retry_connect();
        } else {
            syslog(LOG_WARNING, "UBus reconnect timer fired but exporter instance is gone");
        }
    };

    uloop_timeout_set(&session_.reconnect_timer, 1000);
    logger_.debug("%s, scheduled reconnect in 1 second", reason);
}

void UbusExporter::reset_context() {
    if (ctx_) {
        ubus_free(ctx_);
        ctx_ = nullptr;
        logger_.debug("UBus context freed and reset to nullptr");
    }
}

bool UbusExporter::init() {
    ctx_ = ubus_connect(nullptr);
    if (!ctx_) {
        const int err = errno;
        logger_.debug("UBus connect failed on init: %d (%s)", err, strerror(err));
        schedule_reconnect("Failed to connect to UBus");
        return false;
    }

    logger_.debug("UBus connected on first attempt");

    const int ret = ubus_add_object(ctx_, &session_.obj);
    if (ret == 0) {
        logger_.debug("UBus object 'light_control' registered successfully");
        return true;
    }

    logger_.error("ubus_add_object failed: %d (%m)", ret);
    reset_context();
    schedule_reconnect("Registration failed, will retry");
    return false;
}

void UbusExporter::retry_connect() {
    if (ctx_) {
        return;
    }

    ctx_ = ubus_connect(nullptr);
    if (!ctx_) {
        const int err = errno;
        logger_.debug("UBus reconnect failed: %d (%s)", err, strerror(err));
        uloop_timeout_set(&session_.reconnect_timer, 1000);
        return;
    }

    logger_.debug("UBus connected after retry");

    const int ret = ubus_add_object(ctx_, &session_.obj);
    if (ret == 0) {
        logger_.debug("UBus object 'light_control' registered successfully after retry");
        return;
    }

    logger_.error("ubus_add_object failed on retry: %d (%m)", ret);
    reset_context();
    uloop_timeout_set(&session_.reconnect_timer, 1000);
}

void UbusExporter::run(std::atomic<bool>& stop_flag) {
    logger_.debug("UBus run(): starting event loop");

    uloop_init();

    if (ctx_) {
        ubus_add_uloop(ctx_);
    } else {
        logger_.warn("UBus context is not ready yet; waiting for reconnect timer");
    }

    while (!stop_flag.load(std::memory_order_acquire)) {
        uloop_run_timeout(200);
    }
    uloop_done();
    logger_.debug("UBus event loop stopped");
}

void UbusExporter::stop() {
    uloop_end();
    logger_.debug("UBus exporter cleaned up");
}

#endif
