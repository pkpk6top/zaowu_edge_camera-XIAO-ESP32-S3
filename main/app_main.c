/*
 * 摄像头端主流程。
 *
 * 摄像头按现场配置直接向边缘代理推流，由边缘代理对接云端。
 */
#include <string.h>

#include "cam_config.h"
#include "camera.h"
#include "ed25519.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "nvs_flash.h"
#include "register.h"
#include "rtsp_push.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char *TAG = "app";

/* 主循环多久查一次推流会话 */
#define PUSH_HEALTH_CHECK_MS  30000
#define RESTART_DELAY_MS      5000

static void restart_after(const char *reason)
{
    ESP_LOGE(TAG, "%s，%d ms 后重启", reason, RESTART_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

void app_main(void)
{
    /*
     * ── 0. media_lib 的默认适配（socket / netif / tls / crypt）──
     *
     * media_lib_sal 只有一个 constructor 自动装上 **OS** 层（malloc / 线程 / 信号量），
     * socket 等实现必须应用侧显式装（它的源文件里写着：Still call
     * media_lib_add_default_adapter() before using socket/TLS/netif/crypt）。
     *
     * 不装的表现（踩了很久）：esp_rtsp 客户端打 `Connecting...` 之后 3ms 就
     * `Connection failed, error: Success` —— media_lib_socket_connect() 在实现为空时
     * 直接返回 ESP_ERR_NOT_SUPPORTED，**不碰网络也不设 errno**，于是 strerror(errno)
     * 打出来是 “Success”，而抓包/对端一个包都看不到。
     *
     * 这里不打 ESP_ERROR_CHECK：某个子适配（tls / crypt）失败不该拦住摄像头干活。
     */
    esp_err_t ml_ret = media_lib_add_default_adapter();
    if (ml_ret != ESP_OK) {
        ESP_LOGW(TAG, "media_lib 默认适配安装返回 %s，推流可能不可用", esp_err_to_name(ml_ret));
    }

    /* ── 1. NVS：配置备份与设备身份都放这里 ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. 配置：SD 卡 → NVS → 编译期默认值 ── */
    cam_config_t cfg;
    ESP_ERROR_CHECK(cam_config_load(&cfg));
    cam_config_dump(&cfg);

#if CONFIG_CAM_LEGACY_CLOUD_REGISTER && CONFIG_CAM_REGISTER_USE_SIGNATURE
    /* ── 3. 密码学自检：注册要用 ed25519 签名，先在本地确认实现没被改坏 ── */
    if (ed25519_selftest() != ESP_OK) {
        ESP_LOGE(TAG, "Ed25519 自检未通过，注册签名不可信");
    }
#endif

    /* ── 4. Wi-Fi ── */
    ESP_LOGI(TAG, "连接 Wi-Fi %s ...", cfg.wifi_ssid);
    if (wifi_init_sta() != ESP_OK) {
        restart_after("Wi-Fi 连接失败");
    }

    /* ── 5. 摄像头 ── */
    ESP_LOGI(TAG, "初始化 OV2640 ...");
    if (camera_init() != ESP_OK) {
        restart_after("摄像头初始化失败");
    }

    const char *push_url = cfg.rtsp_push_url;
#if CONFIG_CAM_LEGACY_CLOUD_REGISTER
    /* 仅显式启用历史模式时，才直连云端注册。 */
    for (int attempt = 1;; attempt++) {
        ESP_LOGI(TAG, "向平台注册（第 %d 次）...", attempt);
        if (camera_register_to_platform() == ESP_OK) {
            break;
        }
        if (attempt >= cfg.register_max_attempts) {
            restart_after("注册重试次数用尽");
        }
        ESP_LOGW(TAG, "%d ms 后重试", cfg.register_backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(cfg.register_backoff_ms));
    }

    push_url = camera_get_edge_info()->rtsp_push_url;
#else
    ESP_LOGI(TAG, "使用本地边缘代理配置，不发起云端注册");
#endif

    /* ── 7. 推流 ── */
    if (rtsp_push_start(push_url) != ESP_OK) {
        restart_after("RTSP 推流启动失败");
    }
    ESP_LOGI(TAG, "摄像头端已就绪");

    /* ── 8. 主循环：会话断了就重建（边缘服务器重启、网络抖动都会把会话收掉）── */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PUSH_HEALTH_CHECK_MS));

        if (rtsp_push_session_alive()) {
            continue;
        }

        ESP_LOGW(TAG, "推流会话不在，重建中...");
        rtsp_push_stop();
        if (rtsp_push_start(push_url) != ESP_OK) {
            restart_after("推流重建失败");
        }
    }
}
