#include <sys/socket.h>
#include <sys/time.h>        
#include <netinet/in.h>      
#include <arpa/inet.h>       
#include <unistd.h>          
#include <cstring>
#include <cerrno>            
#include <cstdlib>           
#include <cstdint>           
#include <poll.h>

#include "logger.h"
#include "stats_collector.h"
#include "last_status.h"
#include "udp_server.h"
#include "protocol_parser.h"
#include "uci_settings.h"

UdpServer::UdpServer(StatsCollector& stats, LastStatus& last_status, Logger& logger)
    : stats_(stats), last_status_(last_status), logger_(logger), dimmer_(logger) {}

UdpServer::~UdpServer() {
    close_socket();
}

bool UdpServer::bind_to(const char* addr, uint16_t port) {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        logger_.error("socket() failed: %m");
        return false;
    }

    // SO_REUSEADDR — полезно, но не обязательно для UDP
    int reuse = 1;
    if (setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        // Не считаем это ошибкой: на некоторых ядрах может не поддерживаться
        logger_.debug("SO_REUSEADDR not supported (might be ignored on some kernels)");
    }

    struct sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);

    if (addr && std::strcmp(addr, "0.0.0.0") == 0) {
        local.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (!inet_pton(AF_INET, addr, &local.sin_addr)) {
            logger_.error("invalid IP address: %s", addr);
            close_socket();
            return false;
        }
    }

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        logger_.error("bind() failed: %m");
        close_socket();
        return false;
    }

    bound_port_ = port;
    logger_.debug("UDP server bound to %s:%u", addr, port);
    return true;
}

void UdpServer::reapply_output_locked() {
    if (override_.active()) {
        dimmer_.set_brightness(override_.value());
        last_status_.set_manual(override_.value(), true);
        return;
    }

    LastStatusSnapshot snap = last_status_.snapshot();
    if (!snap.has_packet) {
        last_status_.set_manual(snap.brightness, false);
        return;
    }

    char scene_name[16] = {0};
    const uint8_t brightness = cfg_.policy.apply(snap.lux, scene_name, sizeof(scene_name));
    last_status_.update(snap.device_id, snap.lux, brightness, scene_name,
                        snap.source_ip, snap.source_port, false);
    dimmer_.set_brightness(brightness);
}

void UdpServer::apply_settings(const UciSettings& cfg) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_ = cfg;
    reapply_output_locked();
}

void UdpServer::set_policy(const BrightnessPolicy& policy) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_.policy = policy;
    reapply_output_locked();
}

bool UdpServer::reload_from_uci() {
    UciSettings cfg;
    const bool loaded = load_uci_settings(cfg);
    apply_settings(cfg);

    const BrightnessCurve& curve = cfg.policy.default_curve();
    logger_.info("reload: uci %s, port %u (bound %u), map %u, scenes %u",
                 loaded ? "ok" : "defaults",
                 static_cast<unsigned>(cfg.port),
                 static_cast<unsigned>(bound_port_),
                 static_cast<unsigned>(curve.size()),
                 static_cast<unsigned>(cfg.policy.scheduler().size()));

    if (!cfg.enabled) {
        logger_.warn("reload: enabled=0 in UCI; process still running until restart");
    }
    if (bound_port_ != 0 && cfg.port != bound_port_) {
        logger_.warn("reload: UCI port %u differs from bound %u; restart to apply",
                     static_cast<unsigned>(cfg.port),
                     static_cast<unsigned>(bound_port_));
    }

    return true;
}

void UdpServer::set_brightness_override(uint8_t brightness) {
    std::lock_guard<std::mutex> lock(mu_);
    override_.set(brightness);
    dimmer_.set_brightness(override_.value());
    last_status_.set_manual(override_.value(), true);
    logger_.info("brightness override %u", static_cast<unsigned>(override_.value()));
}

void UdpServer::clear_brightness_override() {
    std::lock_guard<std::mutex> lock(mu_);
    override_.clear();
    logger_.info("brightness override cleared");
    reapply_output_locked();
}

UciSettings UdpServer::copy_settings() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

bool UdpServer::has_override() const {
    std::lock_guard<std::mutex> lock(mu_);
    return override_.active();
}

uint8_t UdpServer::override_value() const {
    std::lock_guard<std::mutex> lock(mu_);
    return override_.value();
}

uint16_t UdpServer::bound_port() const {
    return bound_port_;
}

void UdpServer::run(std::atomic<bool>& stop_flag) {
    char buf[256];
    char ip[INET_ADDRSTRLEN] = {0};
    uint16_t port = 0;

    struct pollfd fds[1] = {
        { .fd = sock_, .events = POLLIN }
    };

    while (!stop_flag.load(std::memory_order_acquire)) {
        // poll с таймаутом 200 мс: поток не зависнет надолго
        int ret = ::poll(fds, 1, 200);

        if (ret < 0) {
            if (errno == EINTR) {
                // Прерван сигналом — просто пробудились, проверим флаг на следующей итерации
                continue;
            }
            logger_.debug("poll error: %m (errno={})", errno);
            continue;
        }

        // Если данные готовы — читаем
        if (fds[0].revents & POLLIN) {
            ssize_t n = recv_packet(buf, sizeof(buf), ip, sizeof(ip), port);
            if (n > 0) {
                stats_.record(ip, port, static_cast<size_t>(n));
                
                // Дампим только для отладки (в проде лучше отключить DEBUG)
                logger_.debug_hex("Raw packet", buf, static_cast<size_t>(n), ip, port);

                auto res = ProtocolParser::parse(buf, static_cast<size_t>(n));

                if (res.is_valid) {
                    char scene_name[16] = {0};
                    uint8_t brightness = 0;
                    bool override_active = false;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        brightness = cfg_.policy.apply(res.lux, scene_name, sizeof(scene_name));
                        override_active = override_.active();
                        brightness = override_.apply(brightness);
                        last_status_.update(res.device_id, res.lux, brightness, scene_name,
                                            ip, port, override_active);
                        dimmer_.set_brightness(brightness);
                    }
                    logger_.debug("Device %d lux=%u brightness=%u scene=%s%s",
                                static_cast<int>(res.device_id),
                                static_cast<unsigned>(res.lux),
                                static_cast<unsigned>(brightness),
                                scene_name[0] != '\0' ? scene_name : "-",
                                override_active ? " override" : "");
                } else {
                    stats_.increment_errors(1);
                    logger_.warn("Protocol error from %s:%u: %s",
                                ip, port, res.error_msg ? res.error_msg : "unknown");
                }
            }
        }
    }

    logger_.debug("UDP receive thread exiting gracefully");
}

ssize_t UdpServer::recv_packet(char* buf, size_t buf_len,
                               char* ip, size_t ip_len,
                               uint16_t& port) {
    struct sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);

    ssize_t n = ::recvfrom(sock_, buf, buf_len, 0,
                           reinterpret_cast<struct sockaddr*>(&addr), &addr_len);

    if (n < 0) {
        // EBADF — сокет закрыт из другого потока (это наш основной путь выхода)
        if (errno == EBADF) {
            logger_.debug("recvfrom: socket closed (EBADF), exiting loop");
            return -1;
        }

        // EAGAIN / EWOULDBLOCK — нормально для неблокирующего режима
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }

        // EINTR — это наш сигнал: сокет закрыли из stop(), надо выйти из recvfrom
        // и дать циклу run() проверить stop_flag
        if (errno == EINTR) {
            // Не считаем это ошибкой: это штатный способ разбудить поток
            return -1;
        }

        // Любые другие ошибки — логируем на debug, чтобы не засорять syslog
        logger_.debug("recvfrom() error: %m (errno={})", errno);
        return -1;
    }

    // Данные получены: заполняем выходные параметры
    port = ntohs(addr.sin_port);
    inet_ntop(AF_INET, &addr.sin_addr, ip, static_cast<socklen_t>(ip_len));

    return n;
}

void UdpServer::close_socket()
{
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
        logger_.debug("UDP socket closed");
    }
}

void UdpServer::stop() { 
    close_socket();
}                        
