#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 OV2640（XIAO ESP32S3 Sense 板载），帧格式 JPEG、缓冲放 PSRAM。
 *
 * 分辨率 / 画质 / 帧缓冲数 / 翻转镜像都取自全局配置。
 *
 * @return ESP_OK 成功；其他为 esp_camera_init 的错误码
 */
esp_err_t camera_init(void);

/**
 * @brief 把配置里的分辨率名（"VGA" 等）换算成像素尺寸。
 *
 * @param[in]   name    分辨率名，大小写不敏感
 * @param[out]  width   宽，可为 NULL
 * @param[out]  height  高，可为 NULL
 * @return false 名字不认识（调用方按 VGA 兜底）
 */
bool camera_resolution_size(const char *name, uint16_t *width, uint16_t *height);

/**
 * @brief 取初始化后**实际生效**的采集尺寸。
 *
 * 配置里的分辨率可能被传感器上限（OV2640→UXGA，OV3660→QXGA）夹低，
 * 该函数给出夹低之后的真实尺寸，供推流层如实上报给平台和页面。
 *
 * @param[out]  width   宽，可为 NULL
 * @param[out]  height  高，可为 NULL
 * @return true 已初始化且拿到尺寸；false 未初始化或尺寸未知
 */
bool camera_effective_size(uint16_t *width, uint16_t *height);

#ifdef __cplusplus
}
#endif
