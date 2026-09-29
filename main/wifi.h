#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 以 STA 模式连接工坊 Wi-Fi，阻塞到拿到 IP 或超时。
 *
 * SSID / 密码取自 cam_config_load() 加载好的全局配置。
 * 掉线由事件回调自动重连（不在这里管）。
 *
 * @return ESP_OK 拿到 IP；ESP_FAIL 超时或初始化失败
 */
esp_err_t wifi_init_sta(void);

#ifdef __cplusplus
}
#endif
