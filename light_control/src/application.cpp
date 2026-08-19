#include <thread>
#include <atomic>

#include "application.h"
#include "uci_settings.h"
#include "brightness_curve.h"
#include "scene_scheduler.h"

Application::Application()
    : logger_("light_control")
    , stats_()
    , last_status_()
    , server_(stats_, last_status_, logger_)
#ifdef UBUS_ENABLED
    , ubus_(stats_, last_status_, server_, logger_)
#endif
{
}

int Application::init() {
    UciSettings cfg;
    load_uci_settings(cfg);

    if (!cfg.enabled) {
        logger_.info("Service disabled in UCI, not starting");
        return 1;
    }

    const BrightnessCurve& curve = cfg.policy.default_curve();
    logger_.debug("Listening port: %u, interface: %s, map entries: %u, scenes: %u",
                  cfg.port, cfg.iface,
                  static_cast<unsigned>(curve.size()),
                  static_cast<unsigned>(cfg.policy.scheduler().size()));
    for (size_t i = 0; i < curve.size(); ++i) {
        const BrightnessCurve::Entry& entry = curve.at(i);
        logger_.debug("map: lux < %u -> brightness %u",
                      static_cast<unsigned>(entry.lux_below),
                      static_cast<unsigned>(entry.brightness));
    }
    for (size_t i = 0; i < cfg.policy.named_count(); ++i) {
        const BrightnessPolicy::NamedCurve& named = cfg.policy.named_at(i);
        logger_.debug("map set '%s': %u entries",
                      named.name, static_cast<unsigned>(named.curve.size()));
        for (size_t j = 0; j < named.curve.size(); ++j) {
            const BrightnessCurve::Entry& entry = named.curve.at(j);
            logger_.debug("map set '%s': lux < %u -> brightness %u",
                          named.name,
                          static_cast<unsigned>(entry.lux_below),
                          static_cast<unsigned>(entry.brightness));
        }
    }
    for (size_t i = 0; i < cfg.policy.scheduler().size(); ++i) {
        const Scene& scene = cfg.policy.scheduler().at(i);
        logger_.debug("scene %s: %02u:%02u-%02u:%02u map_set=%s min=%d max=%d",
                      scene.name,
                      static_cast<unsigned>(scene.from_min / 60),
                      static_cast<unsigned>(scene.from_min % 60),
                      static_cast<unsigned>(scene.to_min / 60),
                      static_cast<unsigned>(scene.to_min % 60),
                      scene.map_set[0] != '\0' ? scene.map_set : "-",
                      scene.has_min ? static_cast<int>(scene.min_brightness) : -1,
                      scene.has_max ? static_cast<int>(scene.max_brightness) : -1);
    }

    server_.apply_settings(cfg);

    if (!server_.bind_to("0.0.0.0", cfg.port)) {
        logger_.error("Failed to bind UDP server");
        return -1;
    }

#ifdef UBUS_ENABLED
    ubus_thread_ = std::thread([this]() {
        if (!ubus_.init()) {
            logger_.debug("UBus not ready yet — will retry via uloop timer");
            // Не выходим: таймеры уже поставлены, дальше запускаем цикл
        } else {
            logger_.debug("UBus ready immediately");
        }

        ubus_.run(should_stop_);
    });
#else
    logger_.debug("UBus disabled at compile time");
#endif

    logger_.debug("Starting UDP receive thread...");

    receive_thread_ = std::thread([this]() {
        server_.run(should_stop_);
    });

    logger_.debug("Application initialized, waiting for events...");
    return 0;
}

void Application::stop() {
    logger_.debug("Stopping application components...");

    server_.stop(); // Закрываем сокет (это также прервёт recvfrom, если он завис)

    // Явно ставим флаг в true с release-семантикой
    should_stop_.store(true, std::memory_order_release);

    if (receive_thread_.joinable()) {
        receive_thread_.join();
        logger_.debug("UDP receive thread joined");
    } else {
        logger_.debug("UDP receive thread was not joinable (already finished?)");
    }

#ifdef UBUS_ENABLED
    ubus_.stop();
    if (ubus_thread_.joinable()) {
        ubus_thread_.join(); 
        logger_.debug("UBus receive thread joined");
    } else {
        logger_.debug("UBus receive thread was not joinable (already finished?)");
    }
#endif

    logger_.debug("Application stopped");
}