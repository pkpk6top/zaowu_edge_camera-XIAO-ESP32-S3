#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把摄像头画面以 RTSP 推流（SINK 角色）送到边缘服务器。
 *
 * 内部做两件事：
 *   ① 建一个 MJPEG 视频源的媒体服务（采集任务 + track manager）；
 *   ② 建 esp_rtsp_service 的 SINK 实例，链接 源 → SINK，先起 SINK 再起源。
 * 推流地址来自平台注册（rtsp://<edge_ip>:<ingest_port>/<camera_id>）。
 *
 * @param[in]  push_url  推流地址
 * @return ESP_OK 已启动（真正推上去是异步的，会话状态见 rtsp_push_session_alive）
 */
esp_err_t rtsp_push_start(const char *push_url);

/** 停止推流并释放源与 SINK（可重复调用） */
esp_err_t rtsp_push_stop(void);

/**
 * @brief 推流会话是否还在。
 *
 * RTSP 会话被对端 TEARDOWN 掉时会变为 false；主循环据此重建推流。
 */
bool rtsp_push_session_alive(void);

#ifdef __cplusplus
}
#endif
