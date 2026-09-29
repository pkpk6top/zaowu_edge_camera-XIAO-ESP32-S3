#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 平台注册后下发的边缘服务器接入信息 */
typedef struct {
    char     edge_ip[64];        /*!< 边缘服务器内网地址 */
    uint16_t ingest_port;        /*!< 摄像头 RTSP 推流入口端口 */
    char     rtsp_push_url[256]; /*!< 完整推流地址（平台没给就按 edge_ip:ingest_port 拼） */
} edge_info_t;

/**
 * @brief 向平台注册本摄像头，换取边缘服务器地址。
 *
 * 请求里带 camera_id / workshop_id / codec / 分辨率 / 帧率；
 * 开启 CONFIG_CAM_REGISTER_USE_SIGNATURE 时另带 public_key / signature / timestamp
 * （被签名的规范串与边缘 agent 同形：camera_id\nworkshop_id\npublic_key\ntimestamp）。
 *
 * @return ESP_OK 注册成功且已解析出 edge_ip / ingest_port
 */
esp_err_t camera_register_to_platform(void);

/** 取上一次注册的结果（camera_register_to_platform 成功后有效） */
const edge_info_t *camera_get_edge_info(void);

/**
 * @brief 取本机公钥（PEM / PKIX），用于日志或离线登记。
 *
 * 首次调用时会生成并持久化设备身份私钥（NVS，不落明文文件）。
 */
esp_err_t camera_identity_public_pem(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
