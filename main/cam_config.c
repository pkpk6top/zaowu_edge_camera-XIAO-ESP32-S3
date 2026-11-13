#include "cam_config.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = "cam_cfg";

/* XIAO ESP32S3 Sense 的 microSD 走 SPI（与摄像头的 SCCB/数据脚不冲突） */
#define SD_PIN_CS    21
#define SD_PIN_SCK   7
#define SD_PIN_MISO  8
#define SD_PIN_MOSI  9
#define SD_MOUNT_DIR "/sdcard"
#define SD_CFG_PATH  SD_MOUNT_DIR "/camera.json"

#define NVS_NAMESPACE "camcfg"
#define NVS_KEY_BLOB  "cfg"

#define CFG_FILE_MAX_SIZE  8192

static cam_config_t s_cfg;

/* ─────────────────────────── 编译期默认值 ─────────────────────────── */

static void str_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void fill_defaults(cam_config_t *c)
{
    memset(c, 0, sizeof(*c));
    str_copy(c->camera_id, sizeof(c->camera_id), CONFIG_CAM_ID);
    str_copy(c->workshop_id, sizeof(c->workshop_id), CONFIG_CAM_WORKSHOP);
    str_copy(c->register_url, sizeof(c->register_url), CONFIG_CAM_REGISTER_URL);
    str_copy(c->rtsp_push_url, sizeof(c->rtsp_push_url), CONFIG_CAM_RTSP_PUSH_URL);
    str_copy(c->wifi_ssid, sizeof(c->wifi_ssid), CONFIG_CAM_WIFI_SSID);
    str_copy(c->wifi_password, sizeof(c->wifi_password), CONFIG_CAM_WIFI_PASSWORD);

    /* 采集档位：这台 OV2640 的像素吞吐约 8.5Mpix/s，分辨率上去了帧率就下来
     * （实测 VGA 640x480 → 28fps、SVGA 800x600 → 17fps、XGA/HD/UXGA → ~15fps）。
     * 「源码流 ≥24fps」这条硬要求把上限锁在 VGA；更高分辨率走转码侧的缩放。 */
    str_copy(c->resolution, sizeof(c->resolution), "VGA");
    str_copy(c->codec, sizeof(c->codec), "MJPEG");
    c->jpeg_quality = 10; /* 12 → 10：压缩伪影小一点，颗粒感也跟着轻（码率涨约三成） */
    /* 目标 30fps：请求值就是采集循环的节流上限，但 tick 粒度 + 抓帧阻塞会损失几个百分点的
     * 实测帧率，所以默认给 30 留余量；固件另有 24fps 硬下限兜底（见 rtsp_push.c）。 */
    c->fps          = 30;
    c->fb_count     = CONFIG_CAM_FRAME_BUF_COUNT;
    /* 这台机位是倒装的：两个轴各翻一下 = 画面转 180° 才是正的。
     * （只翻一个轴会得到「上下颠倒 + 左右镜像」的镜像画面，人脸看不出来，但拍文字会反过来） */
    c->vflip        = 1;
    c->hmirror      = 1;

    /* 画面参数默认值：按「偏色 + 发灰 + 噪点」这三个症状调的一版
     *   - 偏色  → AWB 全自动 + 白平衡增益 + 镜头校正(lenc，四周色晕)
     *   - 发灰  → contrast +1，并确保原始伽马校正开着
     *   - 噪点  → 增益上限收到 4X（默认更松，暗处会拉出彩色噪点）
     * 追求画质优先的，把 gainceiling 放回 2/3，或把 aec_value/agc_gain 锁手动。 */
    c->image.awb            = 1;
    c->image.awb_gain       = 1;
    c->image.wb_mode        = 0;  /* 自动 */
    c->image.aec            = 1;
    c->image.aec2           = 1;
    c->image.ae_level       = 0;
    c->image.aec_value      = -1; /* 不手动锁曝光 */
    c->image.agc            = 1;
    c->image.agc_gain       = -1; /* 不手动锁增益 */
    c->image.gainceiling    = 1;  /* 4X */
    c->image.brightness     = 0;
    c->image.contrast       = 1;
    c->image.saturation     = 0;
    c->image.special_effect = 0;
    c->image.lenc           = 1;
    c->image.bpc            = 1;
    c->image.wpc            = 1;
    c->image.raw_gma        = 1;

    c->register_max_attempts = 10;
    c->register_backoff_ms   = 5000;

    c->source = CAM_CFG_SRC_DEFAULT;
}

/* 同一校验用于 SD、NVS 和默认配置，旧 register_url 不选择运行模式。 */
static esp_err_t validate_config(const cam_config_t *c)
{
    if (c->camera_id[0] == '\0' || c->workshop_id[0] == '\0' || c->wifi_ssid[0] == '\0') {
        ESP_LOGE(TAG, "缺少必填字段：camera_id / workshop_id / wifi.ssid");
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_CAM_LEGACY_CLOUD_REGISTER
    if (c->register_url[0] == '\0') {
        ESP_LOGE(TAG, "历史云注册模式缺少 register_url");
        return ESP_ERR_INVALID_STATE;
    }
#else
    const char *path = strncmp(c->rtsp_push_url, "rtsp://", 7) == 0
                           ? strchr(c->rtsp_push_url + 7, '/') : NULL;
    if (path == NULL || path == c->rtsp_push_url + 7 || path[1] == '\0') {
        ESP_LOGE(TAG, "请配置本地边缘代理 rtsp_push_url（rtsp://<edge>:8554/<camera_id>）；不会回退云注册");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    return ESP_OK;
}

/* ─────────────────────────── SD 卡 ─────────────────────────── */

static esp_err_t mount_sd(void)
{
#if CONFIG_CAM_BOARD_ESP32S3_EYE
    /* XIAO 的 SD GPIO 7/8/9 在此接线中是摄像头信号，不可初始化 SPI。 */
    ESP_LOGI(TAG, "ESP32S3_EYE 无 SD 模式，使用 NVS 或编译期配置");
    return ESP_ERR_NOT_SUPPORTED;
#else
    spi_bus_config_t bus = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    /* 摄像头驱动不使用 SPI 总线（SCCB 走 I2C），所以这里独占 SPI2 */
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = SD_PIN_CS;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_DIR, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡挂载失败: %s（回退到 NVS/默认值）", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD 卡已挂载: %s", card->cid.name);
    return ESP_OK;
#endif
}

/* ─────────────────────────── JSON 解析 ─────────────────────────── */

static void json_str_into(char *dst, size_t dst_size, const cJSON *obj, const char *key, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        str_copy(dst, dst_size, item->valuestring);
    } else if (fallback != NULL) {
        str_copy(dst, dst_size, fallback);
    }
}

static int json_int_or(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

/**
 * 把 camera.json 落到 c 上。只有明确给出的字段才覆盖 —— 缺省字段保留上一层
 * （编译期默认值）已经填好的值，所以三档配置可以只写差异。
 */
static esp_err_t parse_json(cam_config_t *c, const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        ESP_LOGE(TAG, "camera.json 不是合法 JSON");
        return ESP_FAIL;
    }

    json_str_into(c->camera_id, sizeof(c->camera_id), root, "camera_id", NULL);
    json_str_into(c->workshop_id, sizeof(c->workshop_id), root, "workshop_id", NULL);
    json_str_into(c->register_url, sizeof(c->register_url), root, "register_url", NULL);
    json_str_into(c->rtsp_push_url, sizeof(c->rtsp_push_url), root, "rtsp_push_url", NULL);
    json_str_into(c->codec, sizeof(c->codec), root, "codec", NULL);

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        json_str_into(c->wifi_ssid, sizeof(c->wifi_ssid), wifi, "ssid", NULL);
        json_str_into(c->wifi_password, sizeof(c->wifi_password), wifi, "password", NULL);
    }

    const cJSON *video = cJSON_GetObjectItemCaseSensitive(root, "video");
    if (cJSON_IsObject(video)) {
        json_str_into(c->resolution, sizeof(c->resolution), video, "resolution", NULL);
        c->jpeg_quality = json_int_or(video, "jpeg_quality", c->jpeg_quality);
        c->fps          = json_int_or(video, "fps", c->fps);
        c->fb_count     = json_int_or(video, "fb_count", c->fb_count);
        c->vflip        = json_int_or(video, "vflip", c->vflip);
        c->hmirror      = json_int_or(video, "hmirror", c->hmirror);
    }

    /* 画面参数：整段可选，只覆盖 JSON 里明确给出的那几项 */
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(root, "image");
    if (cJSON_IsObject(image)) {
        c->image.awb            = json_int_or(image, "awb", c->image.awb);
        c->image.awb_gain       = json_int_or(image, "awb_gain", c->image.awb_gain);
        c->image.wb_mode        = json_int_or(image, "wb_mode", c->image.wb_mode);
        c->image.aec            = json_int_or(image, "aec", c->image.aec);
        c->image.aec2           = json_int_or(image, "aec2", c->image.aec2);
        c->image.ae_level       = json_int_or(image, "ae_level", c->image.ae_level);
        c->image.aec_value      = json_int_or(image, "aec_value", c->image.aec_value);
        c->image.agc            = json_int_or(image, "agc", c->image.agc);
        c->image.agc_gain       = json_int_or(image, "agc_gain", c->image.agc_gain);
        c->image.gainceiling    = json_int_or(image, "gainceiling", c->image.gainceiling);
        c->image.brightness     = json_int_or(image, "brightness", c->image.brightness);
        c->image.contrast       = json_int_or(image, "contrast", c->image.contrast);
        c->image.saturation     = json_int_or(image, "saturation", c->image.saturation);
        c->image.special_effect = json_int_or(image, "special_effect", c->image.special_effect);
        c->image.lenc           = json_int_or(image, "lenc", c->image.lenc);
        c->image.bpc            = json_int_or(image, "bpc", c->image.bpc);
        c->image.wpc            = json_int_or(image, "wpc", c->image.wpc);
        c->image.raw_gma        = json_int_or(image, "raw_gma", c->image.raw_gma);
    }

    const cJSON *retry = cJSON_GetObjectItemCaseSensitive(root, "retry");
    if (cJSON_IsObject(retry)) {
        c->register_max_attempts = json_int_or(retry, "register_max_attempts", c->register_max_attempts);
        c->register_backoff_ms   = json_int_or(retry, "register_backoff_ms", c->register_backoff_ms);
    }

    cJSON_Delete(root);

    return validate_config(c);
}

static esp_err_t load_from_sd(cam_config_t *c)
{
    FILE *f = fopen(SD_CFG_PATH, "rb");
    if (f == NULL) {
        ESP_LOGI(TAG, "未找到 %s", SD_CFG_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    rewind(f);
    if (size <= 0 || size > CFG_FILE_MAX_SIZE) {
        ESP_LOGW(TAG, "配置文件大小异常: %ld 字节", size);
        fclose(f);
        return ESP_FAIL;
    }

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    esp_err_t err = parse_json(c, buf);
    free(buf);
    return err;
}

/* ─────────────────────────── NVS 备份 ─────────────────────────── */

static esp_err_t load_from_nvs(cam_config_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t size = 0;
    esp_err_t err = nvs_get_blob(h, NVS_KEY_BLOB, NULL, &size);
    /* 旧布局为新增 rtsp_push_url 之前的完整前缀；保留 Wi-Fi、画质及原身份关联。 */
    if (err != ESP_OK || (size != sizeof(*c) && size != offsetof(cam_config_t, rtsp_push_url))) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }
    fill_defaults(c);
    err = nvs_get_blob(h, NVS_KEY_BLOB, c, &size);
    nvs_close(h);
    if (err != ESP_OK || c->camera_id[0] == '\0' || c->wifi_ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    c->source = CAM_CFG_SRC_NVS;
    return ESP_OK;
}

static void save_to_nvs(const cam_config_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打不开 NVS，跳过配置备份");
        return;
    }
    cam_config_t copy = *c;
    if (nvs_set_blob(h, NVS_KEY_BLOB, &copy, sizeof(copy)) == ESP_OK) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

/* ─────────────────────────── 对外 API ─────────────────────────── */

esp_err_t cam_config_load(cam_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fill_defaults(&s_cfg);

    if (mount_sd() == ESP_OK) {
        /* 用默认值打底，只覆盖 camera.json 里显式给出的字段 */
        if (load_from_sd(&s_cfg) == ESP_OK) {
            s_cfg.source = CAM_CFG_SRC_SDCARD;
            save_to_nvs(&s_cfg); /* 下次没插卡也能起来 */
            ESP_LOGI(TAG, "配置来源：SD 卡");
            *out = s_cfg;
            return ESP_OK;
        }
    }

    /* 无论 SD 是否挂上，都试一次 NVS（SD 卡上没有配置文件时也走这里） */
    cam_config_t from_nvs;
    if (load_from_nvs(&from_nvs) == ESP_OK) {
        from_nvs.fb_count = CONFIG_CAM_FRAME_BUF_COUNT; /* 帧缓冲数跟固件走，不跟配置走 */
        s_cfg = from_nvs;
        ESP_LOGI(TAG, "配置来源：NVS 备份");
        *out = s_cfg;
        return validate_config(&s_cfg);
    }

    ESP_LOGW(TAG, "无 SD 卡配置也无 NVS 备份，使用编译期默认值");
    *out = s_cfg;
    return validate_config(&s_cfg);
}

const cam_config_t *cam_config_get(void)
{
    return &s_cfg;
}

void cam_config_dump(const cam_config_t *cfg)
{
    static const char *const src_names[] = {"default", "NVS", "SD"};

    if (cfg == NULL) {
        return;
    }
    ESP_LOGI(TAG, "────────── 摄像头配置 ──────────");
    ESP_LOGI(TAG, "  来源:        %s", src_names[cfg->source]);
    ESP_LOGI(TAG, "  camera_id:   %s", cfg->camera_id);
    ESP_LOGI(TAG, "  workshop_id: %s", cfg->workshop_id);
#if CONFIG_CAM_LEGACY_CLOUD_REGISTER
    ESP_LOGI(TAG, "  接入模式:    历史云注册");
#else
    ESP_LOGI(TAG, "  接入模式:    本地边缘代理（不发起云注册）");
#endif
    ESP_LOGI(TAG, "  wifi.ssid:   %s", cfg->wifi_ssid);
    ESP_LOGI(TAG, "  wifi.pass:   %s", cfg->wifi_password[0] != '\0' ? "***" : "(空)");
    ESP_LOGI(TAG, "  codec:       %s", cfg->codec);
    ESP_LOGI(TAG, "  video:       %s @%dfps q=%d fb=%d", cfg->resolution, cfg->fps, cfg->jpeg_quality,
             cfg->fb_count);
    ESP_LOGI(TAG, "  flip/mirror: %d/%d", cfg->vflip, cfg->hmirror);
    ESP_LOGI(TAG, "  image:       awb=%d/%d wb_mode=%d aec=%d/%d ae=%d agc=%d/ceil=%d", cfg->image.awb,
             cfg->image.awb_gain, cfg->image.wb_mode, cfg->image.aec, cfg->image.aec2,
             cfg->image.ae_level, cfg->image.agc, cfg->image.gainceiling);
    ESP_LOGI(TAG, "               b/c/s=%d/%d/%d lenc=%d bpc=%d wpc=%d gma=%d effect=%d",
             cfg->image.brightness, cfg->image.contrast, cfg->image.saturation, cfg->image.lenc,
             cfg->image.bpc, cfg->image.wpc, cfg->image.raw_gma, cfg->image.special_effect);
    ESP_LOGI(TAG, "  retry:       %d 次 / %dms", cfg->register_max_attempts, cfg->register_backoff_ms);
    ESP_LOGI(TAG, "───────────────────────────────");
}
