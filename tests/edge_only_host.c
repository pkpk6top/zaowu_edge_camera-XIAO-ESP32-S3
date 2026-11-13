/* 真实配置解析、旧 NVS 迁移和 app_main 的宿主行为回归。 */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../main/cam_config.c"
#include "register.h"

static unsigned char stored[sizeof(cam_config_t)];
static size_t stored_size;
static int registrations, selftests, pushes, stops, erased;
static char pushed_url[CAM_CFG_URL_MAX];
static jmp_buf running;

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ (void)ns; (void)mode; *h = 1; return stored_size ? ESP_OK : ESP_ERR_NOT_FOUND; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    (void)h; (void)key;
    if (!out) { *size = stored_size; return ESP_OK; }
    assert(*size >= stored_size);
    memcpy(out, stored, stored_size); *size = stored_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t size)
{ (void)h; (void)key; assert(size <= sizeof(stored)); memcpy(stored, in, size); stored_size = size; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { erased++; return ESP_OK; }
esp_err_t media_lib_add_default_adapter(void) { return ESP_OK; }
esp_err_t wifi_init_sta(void) { return ESP_OK; }
esp_err_t camera_init(void) { return ESP_OK; }
esp_err_t ed25519_selftest(void) { selftests++; return ESP_OK; }
void esp_restart(void) { assert(!"unexpected restart"); }
void vTaskDelay(unsigned int ms)
{
    assert(ms == 30000);
    if (pushes == 2) longjmp(running, 1);
}
esp_err_t rtsp_push_start(const char *url)
{ pushes++; str_copy(pushed_url, sizeof(pushed_url), url); return ESP_OK; }
esp_err_t rtsp_push_stop(void) { stops++; return ESP_OK; }
bool rtsp_push_session_alive(void) { return false; }
#if CONFIG_CAM_LEGACY_CLOUD_REGISTER
esp_err_t camera_register_to_platform(void) { registrations++; return ESP_OK; }
const edge_info_t *camera_get_edge_info(void)
{ static const edge_info_t edge = {.rtsp_push_url = "rtsp://192.168.1.51:8554/legacy"}; return &edge; }
/* 本地构建故意不提供云注册符号：任何非受控调用都会链接失败。 */
#endif
void app_main(void);

int main(void)
{
    cam_config_t c;
    fill_defaults(&c);
#if CONFIG_CAM_LEGACY_CLOUD_REGISTER
    assert(validate_config(&c) == ESP_ERR_INVALID_STATE);
    assert(parse_json(&c, "{\"register_url\":\"https://cloud.example/register\"}") == ESP_OK);
    c.rtsp_push_url[0] = '\0';
    assert(validate_config(&c) == ESP_OK);
#else
    assert(c.register_url[0] == '\0');
    assert(cam_config_load(&c) == ESP_OK); /* 默认配置没有云注册地址也可加载。 */
    assert(validate_config(&c) == ESP_OK);
    assert(parse_json(&c, "{\"rtsp_push_url\":\"rtsp://192.168.1.60:8554/other\"}") == ESP_OK);
    assert(strcmp(c.rtsp_push_url, "rtsp://192.168.1.60:8554/other") == 0);
    assert(parse_json(&c, "{\"register_url\":\"https://old.example/register\"}") == ESP_OK);
    assert(parse_json(&c, "{\"rtsp_push_url\":\"\"}") == ESP_ERR_INVALID_STATE);
    assert(c.register_url[0]); /* 有旧注册地址仍然不会回退云注册。 */
    assert(parse_json(&c, "{\"rtsp_push_url\":\"https://cloud.example/live\"}") == ESP_ERR_INVALID_STATE);
    assert(parse_json(&c, "{\"rtsp_push_url\":\"rtsp:///cam\"}") == ESP_ERR_INVALID_STATE);
    assert(parse_json(&c, "{\"rtsp_push_url\":\"rtsp://edge/\"}") == ESP_ERR_INVALID_STATE);
#endif
    fill_defaults(&c);
    str_copy(c.camera_id, sizeof(c.camera_id), "old-camera");
    str_copy(c.wifi_ssid, sizeof(c.wifi_ssid), "old-wifi");
    str_copy(c.wifi_password, sizeof(c.wifi_password), "old-test-only");
    str_copy(c.register_url, sizeof(c.register_url), "https://old.example/register");
    c.image.contrast = 2;
    /* 固定历史 ABI 大小，防止误改前缀使已有 Wi-Fi/身份关联错位。 */
    assert(offsetof(cam_config_t, rtsp_push_url) == 904);
    stored_size = offsetof(cam_config_t, rtsp_push_url);
    memcpy(stored, &c, stored_size);
    assert(cam_config_load(&c) == ESP_OK);
    assert(c.source == CAM_CFG_SRC_NVS);
    assert(strcmp(c.camera_id, "old-camera") == 0);
    assert(strcmp(c.wifi_ssid, "old-wifi") == 0);
    assert(strcmp(c.wifi_password, "old-test-only") == 0);
    assert(c.image.contrast == 2);
    assert(strcmp(c.rtsp_push_url, CONFIG_CAM_RTSP_PUSH_URL) == 0);
    assert(erased == 0);

#if !CONFIG_CAM_LEGACY_CLOUD_REGISTER
    /* 缺少本地地址的新 NVS 不能退回旧 register_url，也不能丢掉 Wi-Fi。 */
    c.rtsp_push_url[0] = '\0';
    save_to_nvs(&c);
    assert(cam_config_load(&c) == ESP_ERR_INVALID_STATE);
    assert(strcmp(c.wifi_ssid, "old-wifi") == 0);
    assert(c.register_url[0] && registrations == 0);
#endif
    /* 新布局往返；仅改配置，不碰 camid 命名空间。 */
    str_copy(c.rtsp_push_url, sizeof(c.rtsp_push_url), "rtsp://192.168.1.70:8554/new");
    save_to_nvs(&c);
    assert(cam_config_load(&c) == ESP_OK);
    assert(strcmp(c.rtsp_push_url, "rtsp://192.168.1.70:8554/new") == 0);
    if (setjmp(running) == 0) app_main();
    assert(pushes == 2 && stops == 1); /* 首次推流与断线重建均复用同一来源。 */
#if CONFIG_CAM_LEGACY_CLOUD_REGISTER
    assert(registrations == 1 && selftests == 1);
    assert(strcmp(pushed_url, "rtsp://192.168.1.51:8554/legacy") == 0);
    puts("legacy: explicit registration, NVS compatibility and reconnect passed");
#else
    assert(registrations == 0 && selftests == 0);
    assert(strcmp(pushed_url, "rtsp://192.168.1.70:8554/new") == 0);
    puts("local: no cloud registration, NVS compatibility and reconnect passed");
#endif
    assert(erased == 0);
    return 0;
}
