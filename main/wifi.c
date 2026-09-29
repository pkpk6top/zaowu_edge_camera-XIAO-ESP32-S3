#include "wifi.h"

#include <string.h>

#include "cam_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_CONNECT_TIMEOUT_MS  15000

static EventGroupHandle_t s_wifi_events;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 断线重连无上限：现场没人按复位键，只能自己爬回来 */
        ESP_LOGW(TAG, "Wi-Fi 断开，重连中...");
        (void)esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_events != NULL) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t wifi_init_sta(void)
{
    const cam_config_t *cfg = cam_config_get();

    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGE(TAG, "配置里没有 wifi.ssid");
        return ESP_FAIL;
    }
    if (strlen(cfg->wifi_ssid) > sizeof(((wifi_sta_config_t *)0)->ssid) ||
        strlen(cfg->wifi_password) > sizeof(((wifi_sta_config_t *)0)->password)) {
        ESP_LOGE(TAG, "Wi-Fi SSID/密码超过 802.11 长度上限");
        return ESP_FAIL;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    memcpy(wifi_cfg.sta.ssid, cfg->wifi_ssid, strlen(cfg->wifi_ssid));
    memcpy(wifi_cfg.sta.password, cfg->wifi_password, strlen(cfg->wifi_password));
    /* 密码为空 = 开放 AP，此时若还要求 WPA2 会连不上 */
    wifi_cfg.sta.threshold.authmode =
        (cfg->wifi_password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "连接 %s 超时（%d ms）", cfg->wifi_ssid, WIFI_CONNECT_TIMEOUT_MS);
        return ESP_FAIL;
    }
    return ESP_OK;
}
