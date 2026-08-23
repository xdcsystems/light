#include "transports/wifi_esp32.h"

#include <cerrno>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace {
constexpr const char* kTag = "WifiEsp32Tr";
constexpr EventBits_t kGotIpBit = BIT0;
constexpr EventBits_t kFailBit = BIT1;
constexpr int kMaxRetry = 20;
constexpr TickType_t kConnectTimeout = pdMS_TO_TICKS(30000);
}  // namespace

void WifiEsp32Transport::event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<WifiEsp32Transport*>(arg);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->_retry < kMaxRetry) {
            ++self->_retry;
            ESP_LOGW(kTag, "Wi-Fi disconnected, retry %d/%d", self->_retry, kMaxRetry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(kTag, "Wi-Fi connect failed after %d retries", kMaxRetry);
            xEventGroupSetBits(self->_events, kFailBit);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        self->_retry = 0;
        xEventGroupSetBits(self->_events, kGotIpBit);
    }
}

WifiEsp32Transport::~WifiEsp32Transport() {
    if (_sock != -1) {
        close(_sock);
        _sock = -1;
    }
    // Wi-Fi не останавливаем здесь — это ответственность верхнего уровня
}

bool WifiEsp32Transport::init() {
    if (_initialized) {
        return true;
    }

    if (!_ssid || !_ssid[0] || !_ip || !_ip[0]) {
        ESP_LOGE(kTag, "Missing SSID or controller IP");
        ESP_LOGE(kTag, "Set CONFIG_LIGHT_SENSOR_WIFI_SSID via menuconfig / sdkconfig");
        return false;
    }
    if (!_pass) {
        ESP_LOGE(kTag, "Missing password pointer (use empty string for open network)");
        return false;
    }

    constexpr size_t kSsidLimit = 32;
    constexpr size_t kPassLimit = 64;
    if (std::strlen(_ssid) > kSsidLimit || std::strlen(_pass) > kPassLimit) {
        ESP_LOGE(kTag, "SSID or password length is too long");
        return false;
    }

    if (_events == nullptr) {
        _events = xEventGroupCreateStatic(&_events_mem);
    }
    xEventGroupClearBits(_events, kGotIpBit | kFailBit);
    _retry = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEsp32Transport::event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEsp32Transport::event_handler, this, nullptr));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), _ssid, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), _pass,
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (!_pass[0]) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        _events, kGotIpBit | kFailBit, pdFALSE, pdFALSE, kConnectTimeout);
    if ((bits & kGotIpBit) == 0) {
        ESP_LOGE(kTag, "Wi-Fi connect timeout or failure");
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (_sock < 0) {
        ESP_LOGE(kTag, "socket() failed: %s", std::strerror(errno));
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    _addr = sockaddr_in{};
    _addr.sin_family = AF_INET;
    _addr.sin_port = htons(_port);
    if (inet_pton(AF_INET, _ip, &_addr.sin_addr) <= 0) {
        ESP_LOGE(kTag, "Invalid controller IP: %s", _ip);
        close(_sock);
        _sock = -1;
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    _initialized = true;
    ESP_LOGI(kTag, "UDP -> %s:%u", _ip, static_cast<unsigned>(_port));
    return true;
}

bool WifiEsp32Transport::send(const unsigned char* data, std::size_t len) {
    if (_sock == -1 && !init()) {
        return false;
    }

    const ssize_t sent = sendto(_sock, data, len, 0,
                                reinterpret_cast<struct sockaddr*>(&_addr),
                                sizeof(_addr));
    if (sent != static_cast<ssize_t>(len)) {
        ESP_LOGW(kTag, "sendto failed");
        return false;
    }
    return true;
}
