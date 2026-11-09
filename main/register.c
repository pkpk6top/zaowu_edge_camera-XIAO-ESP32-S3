#include "register.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "cam_config.h"
#include "ed25519.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "register";

/* 设备身份私钥（32 字节种子）只进 NVS，不出芯片、不落明文文件 */
#define IDENTITY_NVS_NS    "camid"
#define IDENTITY_NVS_SEED  "seed"

#define HTTP_TIMEOUT_MS    10000
#define NTP_SYNC_TIMEOUT_MS  10000
/* 2020-09-13，用来判断系统时间到底有没有被同步过 */
#define TIME_SANE_THRESHOLD  1600000000

/* Ed25519 公钥的 PKIX/SPKI 前缀：SEQUENCE{ AlgorithmIdentifier{1.3.101.112}, BIT STRING } */
static const uint8_t kEd25519SpkiPrefix[12] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03,
                                               0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};

static edge_info_t s_edge_info;

/* ───────────────────────── 设备身份 ───────────────────────── */

static esp_err_t identity_load_seed(uint8_t seed[ED25519_SEED_LEN])
{
    nvs_handle_t handle;
    if (nvs_open(IDENTITY_NVS_NS, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = ED25519_SEED_LEN;
        esp_err_t err = nvs_get_blob(handle, IDENTITY_NVS_SEED, seed, &size);
        nvs_close(handle);
        if (err == ESP_OK && size == ED25519_SEED_LEN) {
            return ESP_OK;
        }
    }

    /* 首次启动：就地生成。删掉它等于换了一台新设备（平台上的绑定会失效）。 */
    esp_fill_random(seed, ED25519_SEED_LEN);
    if (nvs_open(IDENTITY_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "打不开 NVS，无法保存设备身份");
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_blob(handle, IDENTITY_NVS_SEED, seed, ED25519_SEED_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "首次启动：已生成设备身份密钥并写入 NVS");
    }
    return err;
}

esp_err_t camera_identity_public_pem(char *out, size_t out_size)
{
    if (out == NULL || out_size < 128) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t seed[ED25519_SEED_LEN];
    uint8_t pub[ED25519_PUBKEY_LEN];
    uint8_t der[sizeof(kEd25519SpkiPrefix) + ED25519_PUBKEY_LEN];
    unsigned char b64[96];

    esp_err_t err = identity_load_seed(seed);
    if (err == ESP_OK) {
        err = ed25519_public_key(seed, pub);
    }

    size_t b64_len = 0;
    if (err == ESP_OK) {
        memcpy(der, kEd25519SpkiPrefix, sizeof(kEd25519SpkiPrefix));
        memcpy(der + sizeof(kEd25519SpkiPrefix), pub, sizeof(pub));
        err = mbedtls_base64_encode(b64, sizeof(b64), &b64_len, der, sizeof(der));
    }

    if (err == ESP_OK) {
        /* 与边缘 agent 同口径：PEM 块内的换行保留，只裁掉结尾那个换行 */
        int written = snprintf(out, out_size, "-----BEGIN PUBLIC KEY-----\n%.*s\n-----END PUBLIC KEY-----",
                               (int)b64_len, (const char *)b64);
        if (written < 0 || (size_t)written >= out_size) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    memset(seed, 0, sizeof(seed));
    return err;
}

static esp_err_t identity_sign(const uint8_t *payload, size_t payload_len, char *sig_b64, size_t sig_b64_size)
{
    uint8_t seed[ED25519_SEED_LEN];
    uint8_t sig[ED25519_SIGN_LEN];
    unsigned char b64[128];
    size_t b64_len = 0;

    esp_err_t err = identity_load_seed(seed);
    if (err == ESP_OK) {
        err = ed25519_sign(seed, payload, payload_len, sig);
    }
    if (err == ESP_OK) {
        err = mbedtls_base64_encode(b64, sizeof(b64), &b64_len, sig, sizeof(sig));
    }
    if (err == ESP_OK) {
        int written = snprintf(sig_b64, sig_b64_size, "%.*s", (int)b64_len, (const char *)b64);
        if (written < 0 || (size_t)written >= sig_b64_size) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    memset(seed, 0, sizeof(seed));
    memset(sig, 0, sizeof(sig));
    return err;
}

/* ───────────────────────── 时间 ───────────────────────── */

/** 注册签名带 timestamp，平台按时间窗拒重放 —— 所以必须有真实墙上时间。 */
static esp_err_t ensure_time_synced(void)
{
    static bool sntp_started = false;

    if (time(NULL) > TIME_SANE_THRESHOLD) {
        return ESP_OK;
    }

    if (!sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.start = true;
        cfg.wait_for_sync = true; /* 创建同步信号量，供下面的 sync_wait 限时等待 */
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
            return err;
        }
        sntp_started = true;
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_SYNC_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP 同步超时（%d ms）", NTP_SYNC_TIMEOUT_MS);
        return err;
    }
    ESP_LOGI(TAG, "时间已同步");
    return ESP_OK;
}

/* ───────────────────────── HTTP 响应累积 ───────────────────────── */

#define RESP_INIT_CAP  1024
#define RESP_MAX_CAP   8192

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   overflow;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || rb == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (rb->overflow) {
        return ESP_OK;
    }

    size_t need = rb->len + (size_t)evt->data_len + 1;
    if (need > rb->cap) {
        size_t new_cap = rb->cap * 2;
        while (new_cap < need) {
            new_cap *= 2;
        }
        if (new_cap > RESP_MAX_CAP) {
            ESP_LOGW(TAG, "注册响应超过 %d 字节，忽略后续内容", RESP_MAX_CAP);
            rb->overflow = true;
            return ESP_OK;
        }
        char *grown = realloc(rb->buf, new_cap);
        if (grown == NULL) {
            rb->overflow = true;
            return ESP_OK;
        }
        rb->buf = grown;
        rb->cap = new_cap;
    }

    memcpy(rb->buf + rb->len, evt->data, (size_t)evt->data_len);
    rb->len += (size_t)evt->data_len;
    rb->buf[rb->len] = '\0';
    return ESP_OK;
}

/* ───────────────────────── 注册 ───────────────────────── */

static esp_err_t build_register_body(const cam_config_t *cfg, char **out_body)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    char *pem = NULL;
    char *sig_b64 = NULL;
    char payload[600];

    cJSON_AddStringToObject(root, "camera_id", cfg->camera_id);
    cJSON_AddStringToObject(root, "workshop_id", cfg->workshop_id);
    cJSON_AddStringToObject(root, "codec", cfg->codec);
    cJSON_AddStringToObject(root, "resolution", cfg->resolution);
    cJSON_AddNumberToObject(root, "fps", cfg->fps);

#ifdef CONFIG_CAM_REGISTER_USE_SIGNATURE
    pem = malloc(256);
    sig_b64 = malloc(160);
    if (pem == NULL || sig_b64 == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = camera_identity_public_pem(pem, 256);
    if (err == ESP_OK) {
        err = ensure_time_synced();
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    long timestamp = (long)time(NULL);
    int payload_len = snprintf(payload, sizeof(payload), "%s\n%s\n%s\n%ld", cfg->camera_id, cfg->workshop_id, pem,
                               timestamp);
    if (payload_len < 0 || (size_t)payload_len >= sizeof(payload)) {
        ESP_LOGE(TAG, "签名规范串超出缓冲区");
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = identity_sign((const uint8_t *)payload, (size_t)payload_len, sig_b64, 160);
    if (err != ESP_OK) {
        goto cleanup;
    }

    cJSON_AddStringToObject(root, "public_key", pem);
    cJSON_AddStringToObject(root, "signature", sig_b64);
    cJSON_AddNumberToObject(root, "timestamp", (double)timestamp);
#else
    ESP_LOGW(TAG, "未启用注册签名（CONFIG_CAM_REGISTER_USE_SIGNATURE=n）");
#endif

    *out_body = cJSON_PrintUnformatted(root);
    if (*out_body == NULL) {
        err = ESP_ERR_NO_MEM;
    }

cleanup:
    free(pem);
    free(sig_b64);
    cJSON_Delete(root);
    return err;
}

static esp_err_t parse_register_response(const cam_config_t *cfg, const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "注册响应不是合法 JSON: %s", body);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    const cJSON *edge_ip = cJSON_GetObjectItemCaseSensitive(root, "edge_ip");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "ingest_port");
    const cJSON *push_url = cJSON_GetObjectItemCaseSensitive(root, "rtsp_push_url");

    if (!cJSON_IsString(edge_ip) || edge_ip->valuestring == NULL || !cJSON_IsNumber(port)) {
        ESP_LOGE(TAG, "注册响应缺少 edge_ip / ingest_port: %s", body);
        goto done;
    }

    snprintf(s_edge_info.edge_ip, sizeof(s_edge_info.edge_ip), "%s", edge_ip->valuestring);
    s_edge_info.ingest_port = (uint16_t)port->valueint;

    if (cJSON_IsString(push_url) && push_url->valuestring != NULL) {
        snprintf(s_edge_info.rtsp_push_url, sizeof(s_edge_info.rtsp_push_url), "%s", push_url->valuestring);
    } else {
        /* 平台没给完整 URL 就自己拼：与边缘 agent 的 ingest 约定一致 */
        snprintf(s_edge_info.rtsp_push_url, sizeof(s_edge_info.rtsp_push_url), "rtsp://%s:%u/%s", s_edge_info.edge_ip,
                 (unsigned)s_edge_info.ingest_port, cfg->camera_id);
        ESP_LOGW(TAG, "注册响应没有 rtsp_push_url，按约定拼接");
    }

    err = ESP_OK;

done:
    cJSON_Delete(root);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "边缘服务器: %s:%u", s_edge_info.edge_ip, (unsigned)s_edge_info.ingest_port);
        ESP_LOGI(TAG, "推流地址: %s", s_edge_info.rtsp_push_url);
    }
    return err;
}

esp_err_t camera_register_to_platform(void)
{
    const cam_config_t *cfg = cam_config_get();

    char *body = NULL;
    esp_err_t err = build_register_body(cfg, &body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "构造注册请求失败: %s", esp_err_to_name(err));
        return err;
    }

    resp_buf_t rb = {.buf = calloc(1, RESP_INIT_CAP), .len = 0, .cap = RESP_INIT_CAP, .overflow = false};
    if (rb.buf == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url = cfg->register_url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .timeout_ms = HTTP_TIMEOUT_MS,
        /* 与边缘 agent 一致：普通 HTTPS + 系统 CA 校验 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        free(rb.buf);
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    ESP_LOGI(TAG, "向平台注册：%s", cfg->register_url);
    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册请求失败: %s", esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGE(TAG, "注册被拒：HTTP %d, body=%s", status, rb.buf);
        err = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "注册响应: %s", rb.buf);
        err = parse_register_response(cfg, rb.buf);
    }

    esp_http_client_cleanup(client);
    free(rb.buf);
    free(body);
    return err;
}

const edge_info_t *camera_get_edge_info(void)
{
    return &s_edge_info;
}
