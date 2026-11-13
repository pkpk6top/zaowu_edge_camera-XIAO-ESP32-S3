/*
 * 摄像头端配置：SD 卡 → NVS → 编译期默认值 三级加载。
 *
 * 摄像头只连接现场边缘代理；保留平台机位登记，控制面及云端推流由代理负责。
 * 旧云端注册仅供显式启用的历史模式使用。
 *
 * ⚠ 类型名刻意不叫 camera_config_t —— esp32-camera 的 esp_camera.h 已经占用了这个名字。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_CFG_STR_MAX  128
#define CAM_CFG_URL_MAX  256

/** 配置实际生效的来源（排查用，会打日志） */
typedef enum {
    CAM_CFG_SRC_DEFAULT = 0, /*!< 编译期默认值（Kconfig） */
    CAM_CFG_SRC_NVS,         /*!< NVS 里的备份 */
    CAM_CFG_SRC_SDCARD,      /*!< SD 卡根目录的 camera.json，优先级最高 */
} cam_cfg_source_t;

/**
 * 画面参数（对应 camera.json 的 "image" 段）。
 *
 * 取值语义与 esp32-camera 的 `sensor_t` 一致；标了「-1 = 不手动锁」的用 -1。
 * ⚠ OV2640 上 `set_sharpness` / `set_denoise` 是空实现（驱动里标了 not supported），
 *   所以这里没有锐度 / 降噪两项 —— 颗粒感只能靠 gainceiling 与 JPEG 质量压。
 */
typedef struct {
    /* 白平衡（偏色） */
    int awb;            /*!< 自动白平衡 0/1 */
    int awb_gain;       /*!< 白平衡增益 0/1 */
    int wb_mode;        /*!< 0=自动 1=晴天 2=阴天 3=办公室 4=家 */
    /* 曝光 / 增益（偏暗、噪点） */
    int aec;            /*!< 自动曝光 0/1 */
    int aec2;           /*!< AEC 的 DSP 算法 0/1 */
    int ae_level;       /*!< 曝光补偿 -2~2 */
    int aec_value;      /*!< 手动曝光 0~1200；-1 = 用自动 */
    int agc;            /*!< 自动增益 0/1 */
    int agc_gain;       /*!< 手动增益 0~30；-1 = 用自动 */
    int gainceiling;    /*!< 增益上限 0=2X 1=4X 2=8X 3=16X 4=32X 5=64X 6=128X */
    /* 画质（发灰、四周色晕） */
    int brightness;     /*!< -2~2 */
    int contrast;       /*!< -2~2 */
    int saturation;     /*!< -2~2 */
    int special_effect; /*!< 0=无 1=负片 2=黑白 3=偏红 4=偏绿 5=偏蓝 6=复古 */
    int lenc;           /*!< 镜头校正（四周色晕 / 暗角） 0/1 */
    int bpc;            /*!< 坏点校正 0/1 */
    int wpc;            /*!< 白点校正 0/1 */
    int raw_gma;        /*!< 原始伽马校正（发灰） 0/1 */
} cam_image_cfg_t;

typedef struct {
    /* ── 必填：我是谁 ── */
    char camera_id[CAM_CFG_STR_MAX];    /*!< 平台分配的摄像头唯一标识 */
    char workshop_id[CAM_CFG_STR_MAX];  /*!< 所属工坊 ID */

    /* ── 必填：怎么连 ── */
    char register_url[CAM_CFG_URL_MAX]; /*!< 仅历史云注册模式使用 */
    char wifi_ssid[CAM_CFG_STR_MAX];
    char wifi_password[CAM_CFG_STR_MAX];

    /* ── 视频参数（可选，缺省见 cam_config.c 的 fill_defaults） ── */
    char resolution[16];               /*!< QQVGA / QVGA / VGA / SVGA / XGA / HD */
    int  jpeg_quality;                 /*!< 0-63，越小画质越好 */
    int  fps;                          /*!< 推流帧率上限 */
    int  fb_count;                     /*!< 帧缓冲个数 */
    int  vflip;
    int  hmirror;

    /* ── 画面参数（可选，对应 camera.json 的 "image" 段）── */
    cam_image_cfg_t image;

    char codec[16];                    /*!< MJPEG / H264（H264 需要额外编码器） */

    /* ── 注册重试策略（可选） ── */
    int register_max_attempts;
    int register_backoff_ms;

    cam_cfg_source_t source;

    /* 只追加新字段，保留历史 NVS blob 的字段布局。 */
    char rtsp_push_url[CAM_CFG_URL_MAX]; /*!< 本地边缘代理 RTSP ingest 地址 */
} cam_config_t;

/**
 * @brief  按优先级加载配置：SD 卡 → NVS → 编译期默认值。
 *
 *         读到 SD 卡配置后会把结果写回 NVS，作为下次没插卡时的回退。
 *
 * @param[out]  out  输出配置
 * @return
 *       - ESP_OK                 加载成功（含「用编译期默认值但必填项齐全」）
 *       - ESP_ERR_INVALID_ARG    out 为 NULL
 *       - ESP_ERR_INVALID_STATE  三级都拿不到必填项
 */
esp_err_t cam_config_load(cam_config_t *out);

/** 取全局只读配置（必须先 cam_config_load） */
const cam_config_t *cam_config_get(void);

/** 打印当前配置（密码脱敏） */
void cam_config_dump(const cam_config_t *cfg);

#ifdef __cplusplus
}
#endif
