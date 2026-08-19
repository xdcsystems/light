#include "transports/wifi_esp32.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

static const char* TAG = "WifiEsp32Tr";

static constexpr EventBits_t kGotIpBit = BIT0;
static constexpr EventBits_t kFailBit = BIT1;
static constexpr int kMaxRetry = 20;
static constexpr TickType_t kConnectTimeout = pdMS_TO_TICKS(30000);

static StaticEventGroup_t s_events_mem;
static EventGroupHandle_t s_events = nullptr;
static int s_retry = 0;

static bool init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

static void wifi_event_handler(void* /*arg*/, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < kMaxRetry) {
            ++s_retry;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", s_retry, kMaxRetry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi connect failed after %d retries", kMaxRetry);
            xEventGroupSetBits(s_events, kFailBit);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, kGotIpBit);
    }
}

WifiEsp32Transport::~WifiEsp32Transport() {
    if (_sock != -1) {
        close(_sock);
        _sock = -1;
    }
}

bool WifiEsp32Transport::init() {
    if (_initialized) {
        return true;
    }

    if (!_ssid || !_ssid[0] || !_ip || !_ip[0]) {
        ESP_LOGE(TAG, "Missing SSID or controller IP");
        return false;
    }
    if (std::strcmp(_ssid, "YOUR_SSID") == 0 || std::strcmp(_pass ? _pass : "", "YOUR_PASSWORD") == 0) {
        ESP_LOGE(TAG, "Set WIFI_SSID / WIFI_PASS in include/config.hpp before flashing");
        return false;
    }

    if (!init_nvs()) {
        ESP_LOGE(TAG, "nvs_flash_init failed");
        return false;
    }

    if (s_events == nullptr) {
        s_events = xEventGroupCreateStatic(&s_events_mem);
    }
    xEventGroupClearBits(s_events, kGotIpBit | kFailBit);
    s_retry = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), _ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (_pass != nullptr) {
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), _pass,
                     sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (_pass == nullptr || !_pass[0]) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_events, kGotIpBit | kFailBit, pdFALSE, pdFALSE, kConnectTimeout);
    if ((bits & kGotIpBit) == 0) {
        ESP_LOGE(TAG, "Wi-Fi connect timeout or failure");
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    _addr = sockaddr_in{};
    _addr.sin_family = AF_INET;
    _addr.sin_port = htons(_port);
    if (inet_pton(AF_INET, _ip, &_addr.sin_addr) <= 0) {
        ESP_LOGE(TAG, "Invalid controller IP: %s", _ip);
        close(_sock);
        _sock = -1;
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    _initialized = true;
    ESP_LOGI(TAG, "UDP -> %s:%u", _ip, static_cast<unsigned>(_port));
    return true;
}

bool WifiEsp32Transport::send(const unsigned char* data, std::size_t len) {
    if (_sock < 0 && !init()) {
        return false;
    }

    const ssize_t sent = sendto(_sock, data, len, 0,
                                reinterpret_cast<struct sockaddr*>(&_addr),
                                sizeof(_addr));
    if (sent != static_cast<ssize_t>(len)) {
        ESP_LOGW(TAG, "sendto failed");
        return false;
    }
    return true;
}
