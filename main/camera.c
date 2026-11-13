#include "camera.h"

#include <strings.h>

#include "cam_config.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "camera";

#if CONFIG_CAM_BOARD_ESP32S3_EYE
/* ESP32-S3 N16R8 / OV5640 已验证的 ESP32S3_EYE 接线。 */
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5
#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2     8
#define CAM_PIN_D1     9
#define CAM_PIN_D0     11
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
#define CAM_PIN_PCLK   13
#define CAM_XCLK_HZ    20000000
#else
/* XIAO ESP32S3 Sense 的 OV2640 引脚（Seeed 官方定义，与 Sense 扩展板一一对应） */
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   10
#define CAM_PIN_SIOD   40
#define CAM_PIN_SIOC   39

#define CAM_PIN_D7     48
#define CAM_PIN_D6     11
#define CAM_PIN_D5     12
#define CAM_PIN_D4     14
#define CAM_PIN_D3     16
#define CAM_PIN_D2     18
#define CAM_PIN_D1     17
#define CAM_PIN_D0     15

#define CAM_PIN_VSYNC  38
#define CAM_PIN_HREF   47
#define CAM_PIN_PCLK   13
#define CAM_XCLK_HZ    24000000
#endif

typedef struct {
    const char *name;
    framesize_t  size;
    uint16_t     width;
    uint16_t     height;
} res_entry_t;

/* 采集档位：左边是配置/台账里写的名字（大小写不敏感），右边是传感器枚举。
 * 「480p/720p/1080p」是给手写 camera.json 用的别名，映射到传感器原生档位。
 * 注意：360p 没有对应的传感器读出档，属于转码侧的档位（由边缘 agent 缩放）。 */
static const res_entry_t s_res_table[] = {
    {"QQVGA", FRAMESIZE_QQVGA, 160, 120},
    {"QVGA", FRAMESIZE_QVGA, 320, 240},
    {"VGA", FRAMESIZE_VGA, 640, 480},
    {"480p", FRAMESIZE_VGA, 640, 480},
    {"SVGA", FRAMESIZE_SVGA, 800, 600},
    {"XGA", FRAMESIZE_XGA, 1024, 768},
    {"HD", FRAMESIZE_HD, 1280, 720},
    {"720p", FRAMESIZE_HD, 1280, 720},
    {"SXGA", FRAMESIZE_SXGA, 1280, 1024},
    {"UXGA", FRAMESIZE_UXGA, 1600, 1200},
    {"FHD", FRAMESIZE_FHD, 1920, 1080},
    {"1080p", FRAMESIZE_FHD, 1920, 1080},
    {"QXGA", FRAMESIZE_QXGA, 2048, 1536},
};

/* 初始化后**实际生效**的采集尺寸：配的档位可能被传感器上限夹低，推流层要如实上报 */
static uint16_t s_eff_width;
static uint16_t s_eff_height;
static bool     s_eff_valid;

static const char *framesize_name(framesize_t size)
{
    for (size_t i = 0; i < sizeof(s_res_table) / sizeof(s_res_table[0]); i++) {
        if (s_res_table[i].size == size) {
            return s_res_table[i].name;
        }
    }
    return "?";
}

static bool framesize_size(framesize_t size, uint16_t *width, uint16_t *height)
{
    for (size_t i = 0; i < sizeof(s_res_table) / sizeof(s_res_table[0]); i++) {
        if (s_res_table[i].size == size) {
            *width  = s_res_table[i].width;
            *height = s_res_table[i].height;
            return true;
        }
    }
    return false;
}

/* 驱动没有 set_framerate 接口 —— 实际帧率由传感器内部读出时序决定。
 * 高分辨率档共用慢档（OV2640 的 XGA/HD/UXGA 实测约 15fps，OV3660 的 3MP 档同理），
 * 只有小尺寸档才上得去 ~30fps。这里给出「这一档实际能跑多少」的参考上限。 */
static uint16_t sensor_fps_cap(camera_model_t model, framesize_t size)
{
    switch (model) {
    case CAMERA_OV2640:
        return size <= FRAMESIZE_SVGA ? 30 : 15;
    case CAMERA_OV3660:
        return size <= FRAMESIZE_FHD ? 30 : 15;
    default:
        return 0; /* 不认识的传感器不猜 */
    }
}

static framesize_t resolution_to_framesize(const char *name)
{
    for (size_t i = 0; i < sizeof(s_res_table) / sizeof(s_res_table[0]); i++) {
        if (strcasecmp(name, s_res_table[i].name) == 0) {
            return s_res_table[i].size;
        }
    }
    ESP_LOGW(TAG, "不认识的分辨率 \"%s\"，按 VGA 处理", name);
    return FRAMESIZE_VGA;
}

bool camera_resolution_size(const char *name, uint16_t *width, uint16_t *height)
{
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_res_table) / sizeof(s_res_table[0]); i++) {
        if (strcasecmp(name, s_res_table[i].name) == 0) {
            if (width != NULL) {
                *width = s_res_table[i].width;
            }
            if (height != NULL) {
                *height = s_res_table[i].height;
            }
            return true;
        }
    }
    return false;
}

bool camera_effective_size(uint16_t *width, uint16_t *height)
{
    if (!s_eff_valid) {
        return false;
    }
    if (width != NULL) {
        *width = s_eff_width;
    }
    if (height != NULL) {
        *height = s_eff_height;
    }
    return true;
}

/* 把画面参数写到传感器上。顺序有讲究：先摆好「自动 / 手动」开关，再灌具体值，
 * 否则后面那句会把刚设的开关又盖回去。
 * `aec_value` / `agc_gain` 为 -1 表示不手动锁（保持自动）。 */
static void apply_image_params(sensor_t *sensor, const cam_image_cfg_t *img)
{
    sensor->set_whitebal(sensor, img->awb);
    sensor->set_awb_gain(sensor, img->awb_gain);
    sensor->set_wb_mode(sensor, img->wb_mode);

    sensor->set_exposure_ctrl(sensor, img->aec);
    sensor->set_aec2(sensor, img->aec2);
    sensor->set_ae_level(sensor, img->ae_level);
    sensor->set_gain_ctrl(sensor, img->agc);
    sensor->set_gainceiling(sensor, (gainceiling_t)img->gainceiling);
    if (img->aec_value >= 0) {
        sensor->set_aec_value(sensor, img->aec_value);
    }
    if (img->agc_gain >= 0) {
        sensor->set_agc_gain(sensor, img->agc_gain);
    }

    sensor->set_brightness(sensor, img->brightness);
    sensor->set_contrast(sensor, img->contrast);
    sensor->set_saturation(sensor, img->saturation);
    sensor->set_special_effect(sensor, img->special_effect);
    sensor->set_lenc(sensor, img->lenc);
    sensor->set_bpc(sensor, img->bpc);
    sensor->set_wpc(sensor, img->wpc);
    sensor->set_raw_gma(sensor, img->raw_gma);
}

esp_err_t camera_init(void)
{
    const cam_config_t *cfg = cam_config_get();

    if (strcasecmp(cfg->codec, "MJPEG") != 0) {
        /* OV2640 原生只出 JPEG。要推 H.264 得另接 esp_h264 软件编码（开销大），
         * 或者由边缘服务器侧转码 —— 这里按 MJPEG 推，并明确告诉调用方配置没完全兑现。 */
        ESP_LOGW(TAG, "codec=%s 未实现，按 MJPEG 推送（H.264 需要额外编码器）", cfg->codec);
    }

    camera_config_t cam_cfg = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        /* XIAO 保持原 24MHz，ESP32S3_EYE 接线使用已验证的 20MHz。 */
        .xclk_freq_hz = CAM_XCLK_HZ,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = resolution_to_framesize(cfg->resolution),
        .jpeg_quality = cfg->jpeg_quality,
        .fb_count     = cfg->fb_count,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cam_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 传感器能力校验：OV2640 只到 UXGA(1600x1200)，OV3660 到 QXGA(2048x1536)。
     * 配得比上限高就夹到上限（同一份配置可以喂不同传感器的机器）。 */
    framesize_t want   = cam_cfg.frame_size;
    sensor_t   *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_vflip(sensor, cfg->vflip);
        sensor->set_hmirror(sensor, cfg->hmirror);
        apply_image_params(sensor, &cfg->image);

        camera_sensor_info_t *info = esp_camera_sensor_get_info(&sensor->id);
        if (info != NULL) {
            if (want > info->max_size) {
                ESP_LOGW(TAG, "%s 上限 %s，配的 %s 超了，夹到上限", info->name,
                         framesize_name(info->max_size), framesize_name(want));
                if (sensor->set_framesize(sensor, info->max_size) == 0) {
                    want = info->max_size;
                } else {
                    ESP_LOGW(TAG, "夹到 %s 失败，仍按 %s 跑", framesize_name(info->max_size),
                             framesize_name(want));
                }
            }
            uint16_t cap = sensor_fps_cap(info->model, want);
            if (cap != 0 && cfg->fps > cap) {
                ESP_LOGW(TAG, "配 fps=%d，但 %s@%s 实际约 %u fps（驱动无 set_framerate），以实测为准",
                         cfg->fps, info->name, framesize_name(want), (unsigned)cap);
            }
            ESP_LOGI(TAG, "%s 就绪：%s -> %s / JPEG q=%d / fb=%d / 请求 %dfps", info->name,
                     cfg->resolution, framesize_name(want), cfg->jpeg_quality, cfg->fb_count, cfg->fps);
        } else {
            ESP_LOGW(TAG, "没识别出传感器型号，按配置档位 %s 跑", framesize_name(want));
        }
    }

    if (framesize_size(want, &s_eff_width, &s_eff_height)) {
        s_eff_valid = true;
    }
    return ESP_OK;
}
