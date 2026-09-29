/*
 * 摄像头 → 边缘服务器的 RTSP 推流。
 *
 * 数据流：camera_capture_task 抓帧 → track manager（内存队列）→ esp_rtsp_service(SINK)
 *         → ANNOUNCE / SETUP / RECORD → 边缘 agent 的 ingest 服务端。
 *
 * 边缘 agent 的 ingest 只实现 ANNOUNCE / SETUP / RECORD（不做 DESCRIBE / PLAY），
 * 正好就是 SINK 角色走的这三条，所以这里只能当推流端。
 */
#include "rtsp_push.h"

#include <stdlib.h>
#include <string.h>

#include "cam_config.h"
#include "camera.h"
#include "adf_event_hub.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_fourcc.h"
#include "esp_log.h"
#include "esp_media_provider.h"
#include "esp_media_service.h"
#include "esp_media_track.h"
#include "esp_media_track_mngr.h"
#include "esp_netif.h"
#include "esp_rtsp_service.h"
#include "esp_rtsp_service_ops.h"
#include "esp_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rtsp_push";

#define CAMERA_SOURCE_NAME  "camera_src"
#define CAMERA_TRACK_ID     0
#define CAMERA_TASK_STACK   4096
#define CAMERA_TASK_PRIO    5
#define CAMERA_TASK_CORE    1
/* RTP 帧缓冲（一帧 JPEG 允许的最大尺寸）。
 * 别按「未压缩尺寸的一半」算：VGA 会得到 150KB，而这块缓冲走内部 RAM（media_lib 默认
 * malloc）—— 150KB 连续内存基本要不到，表现是「RECORD 成功但一个 RTP 包都不发」，
 * 对端看起来像没推流。VGA JPEG（q=12）实测 20~30KB，48KB 留足余量。 */
#define RTP_FRAME_SIZE  (48 * 1024)

/*
 * 派生服务都把基类放在第一个成员（esp_media_service_t 的 base、esp_rtsp_service 的 media），
 * 所以从派生句柄直接转成 esp_service_t * 是合法的。官方例程用的就是这个语义的 ESP_SERVICE_BASE()，
 * 但那个宏并不在 esp_service 的公开头里（只有例程与组件内部可见），所以这里自带一份。
 */
#ifndef ESP_SERVICE_BASE
#define ESP_SERVICE_BASE(service)  ((esp_service_t *)(service))
#endif

/* ───────────────────────── 摄像头 MJPEG 源服务 ───────────────────────── */

typedef struct {
    esp_media_service_t     media; /*!< 必须是第一个成员 */
    esp_media_track_mngr_t *mngr;
    TaskHandle_t            task;
    volatile bool           running;
    uint16_t                width;
    uint16_t                height;
    uint16_t                fps;
    int64_t                 pts_ms;
} camera_source_t;

static camera_source_t s_source;
static esp_rtsp_service_t *s_rtsp;
static volatile bool s_session_alive;
static bool s_source_ready;
static bool s_linked;

static void fill_sta_ip(char *buf, size_t buf_size)
{
    buf[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(buf, buf_size, IPSTR, IP2STR(&ip.ip));
    }
}

/* 源码流帧率下限：采集循环节流间隔按它算，低于这个值也提上来。
 * 注意这只是「请求值」——高分辨率档受传感器读出时序限制实际可能只有 ~15fps，
 * 固件在 camera_init 里会对这种情况打告警，页面按实测数字显示。 */
#define MIN_SOURCE_FPS 24

static void camera_capture_task(void *arg)
{
    camera_source_t *src = (camera_source_t *)arg;
    uint32_t interval_ms = 1000U / (src->fps != 0 ? src->fps : MIN_SOURCE_FPS);

    /* 按**周期**对齐，而不是「固定 sleep 一段」：抓帧 + 写帧本身要花几毫秒，
     * 用 vTaskDelay(interval) 的话实际周期 = interval + 处理耗时，24fps 会掉到 ~20fps。
     * vTaskDelayUntil 让周期只由 interval 决定（耗时从 sleep 里扣）。 */
    TickType_t period_ticks = pdMS_TO_TICKS(interval_ms);
    if (period_ticks == 0) {
        period_ticks = 1;
    }
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "采集任务启动：%ux%u @%ufps", src->width, src->height, (unsigned)src->fps);

    while (src->running) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "抓帧失败");
            vTaskDelay(pdMS_TO_TICKS(100));
            last_wake = xTaskGetTickCount(); /* 重新对齐，别追帧 */
            continue;
        }

        esp_media_frame_t frame = {
            .track_id = CAMERA_TRACK_ID,
            .type = ESP_MEDIA_TRACK_TYPE_VIDEO,
            .data = fb->buf,
            .size = fb->len,
            .pts = src->pts_ms,
            .dts = src->pts_ms,
            /* MJPEG 每帧都能独立解码，一律当关键帧 */
            .flags = ESP_MEDIA_FRAME_FLAG_KEY,
        };

        /* 队列满就丢这一帧（timeout 0）：推流和采集不能互相拖死 */
        esp_err_t err = esp_media_track_write_frame(src->mngr, &frame, 0);
        if (err == ESP_OK) {
            src->pts_ms += interval_ms;
        } else {
            ESP_LOGD(TAG, "丢帧：%s", esp_err_to_name(err));
        }

        esp_camera_fb_return(fb);
        vTaskDelayUntil(&last_wake, period_ticks);
    }

    src->task = NULL;
    ESP_LOGI(TAG, "采集任务退出");
    vTaskDelete(NULL);
}

static esp_err_t camera_source_get_role(esp_service_t *service, esp_media_role_t *out_role)
{
    (void)service;
    *out_role = ESP_MEDIA_ROLE_SRC;
    return ESP_OK;
}

static esp_err_t camera_source_get_provider(esp_service_t *service, esp_media_stream_id_t stream,
                                            esp_media_provider_t *out_provider)
{
    camera_source_t *src = (camera_source_t *)service;
    if (stream != ESP_MEDIA_DEFAULT_STREAM) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_media_track_mngr_get_provider(src->mngr, out_provider);
}

static esp_err_t camera_source_on_start(esp_service_t *service)
{
    camera_source_t *src = (camera_source_t *)service;

    /* 上一次会话的残留（abort 状态、排队帧）清掉，重新计时 */
    esp_media_track_clear_abort(src->mngr);
    src->pts_ms = 0;
    src->running = true;

    if (xTaskCreatePinnedToCore(camera_capture_task, "cam_capture", CAMERA_TASK_STACK, src, CAMERA_TASK_PRIO,
                                &src->task, CAMERA_TASK_CORE) != pdPASS) {
        src->running = false;
        src->task = NULL;
        ESP_LOGE(TAG, "采集任务创建失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t camera_source_on_stop(esp_service_t *service)
{
    camera_source_t *src = (camera_source_t *)service;
    src->running = false;

    /* 等采集任务自己退出，之后才不会再有人往 mngr 里写 */
    for (int i = 0; i < 50 && src->task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_media_track_write_abort(src->mngr);
    return ESP_OK;
}

static const esp_service_ops_t s_source_service_ops = {
    .on_start = camera_source_on_start,
    .on_stop  = camera_source_on_stop,
};

static const esp_media_service_ops_t s_source_media_ops = {
    .get_role     = camera_source_get_role,
    .get_provider = camera_source_get_provider,
};

static esp_err_t camera_source_init(void)
{
    const cam_config_t *cfg = cam_config_get();

    camera_source_t *src = &s_source;
    memset(src, 0, sizeof(*src));

    uint16_t width = 640, height = 480;
    if (!camera_effective_size(&width, &height)) {
        /* 摄像头还没初始化（或传感器没识别出来）：退回按配置里的名字换算 */
        if (!camera_resolution_size(cfg->resolution, &width, &height)) {
            /* 名字不认识：摄像头那边已按 VGA 处理，这里跟着对齐 */
            width = 640;
            height = 480;
        }
    }
    src->width = width;
    src->height = height;

    uint16_t fps = (uint16_t)(cfg->fps > 0 ? cfg->fps : MIN_SOURCE_FPS);
    if (fps < MIN_SOURCE_FPS) {
        ESP_LOGW(TAG, "配置 fps=%u 低于下限 %d，已按 %d 节流", fps, MIN_SOURCE_FPS, MIN_SOURCE_FPS);
        fps = MIN_SOURCE_FPS;
    }
    src->fps = fps;

    /* 一条 MJPEG 视频轨；INTERNAL 缓存让 track manager 自己持有帧数据副本，
     * 于是 esp_camera 的 fb 可以立刻归还 —— 代价是每帧一次拷贝。 */
    esp_media_track_mngr_cfg_t mngr_cfg = {
        .max_track_num = 1,
    };
    esp_err_t err = esp_media_track_mngr_create(&mngr_cfg, &src->mngr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "track manager 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    esp_media_track_info_t track_info = {
        .id = CAMERA_TRACK_ID,
        .type = ESP_MEDIA_TRACK_TYPE_VIDEO,
    };
    track_info.info.video.codec = ESP_FOURCC_MJPG;
    track_info.info.video.width = width;
    track_info.info.video.height = height;
    track_info.info.video.fps = src->fps;

    esp_media_track_mngr_track_cfg_t track_cfg = {
        .info = track_info,
        .cache_cfg =
            {
                .cache_type = ESP_MEDIA_TRACK_CACHE_INTERNAL,
                .track_cache =
                    {
                        /* 两帧未压缩尺寸的余量：JPEG 一定小于它 */
                        .cache_size = (size_t)width * height * 2,
                        .addr_align = 4,
                        .size_align = 0,
                    },
            },
    };
    err = esp_media_track_mngr_add_track(src->mngr, &track_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加视频轨失败: %s", esp_err_to_name(err));
        esp_media_track_mngr_destroy(src->mngr);
        src->mngr = NULL;
        return err;
    }

    esp_media_service_config_t media_cfg = ESP_MEDIA_SERVICE_CONFIG_DEFAULT();
    media_cfg.name = CAMERA_SOURCE_NAME;
    media_cfg.service_ops = &s_source_service_ops;
    media_cfg.media_ops = &s_source_media_ops;

    err = esp_media_service_init(&src->media, &media_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "媒体服务初始化失败: %s", esp_err_to_name(err));
        esp_media_track_mngr_destroy(src->mngr);
        src->mngr = NULL;
        return err;
    }
    s_source_ready = true;
    return ESP_OK;
}

/* ───────────────────────── RTSP 事件 ───────────────────────── */

static void on_rtsp_event(const adf_event_t *event, void *ctx)
{
    (void)ctx;

    if (event->event_id == ESP_RTSP_SERVICE_EVENT_RECORD) {
        ESP_LOGI(TAG, "推流会话已建立（RECORD）");
        s_session_alive = true;
    } else if (event->event_id == ESP_RTSP_SERVICE_EVENT_TEARDOWN) {
        ESP_LOGW(TAG, "推流会话已断开（TEARDOWN）");
        s_session_alive = false;
    } else if (event->event_id == ESP_RTSP_SERVICE_EVENT_SETUP) {
        ESP_LOGI(TAG, "SETUP 完成");
    }
}

/* ───────────────────────── 对外 API ───────────────────────── */

esp_err_t rtsp_push_start(const char *push_url)
{
    if (push_url == NULL || push_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rtsp != NULL || s_source_ready) {
        return ESP_ERR_INVALID_STATE; /* 已启动；重启请先 rtsp_push_stop() */
    }

    esp_err_t err = camera_source_init();
    if (err != ESP_OK) {
        return err;
    }

    esp_rtsp_service_cfg_t rtsp_cfg = ESP_RTSP_SERVICE_CFG_DEFAULT(ESP_RTSP_SERVICE_ROLE_SINK);
    err = esp_rtsp_service_create(&rtsp_cfg, &s_rtsp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTSP SINK 创建失败: %s", esp_err_to_name(err));
        goto fail;
    }

    {
        adf_event_subscribe_info_t sub = ADF_EVENT_SUBSCRIBE_INFO_DEFAULT();
        sub.event_id = ADF_EVENT_ANY_ID;
        sub.handler = on_rtsp_event;
        err = esp_service_event_subscribe(ESP_SERVICE_BASE(s_rtsp), &sub);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "订阅 RTSP 事件失败: %s", esp_err_to_name(err));
            goto fail;
        }
    }

    {
        esp_rtsp_service_setup_t setup = ESP_RTSP_SERVICE_SETUP_DEFAULT();
        /*
         * 控制面走 TCP。默认值是 RTSP_TRANSPORT_UDP，实测在这台设备上表现为：
         * 日志打 `Connecting...` 之后 4ms 就 `Connection failed, error: Success`，
         * 而 agent 那边 TCP/UDP、554/8554 四个口**一个包都收不到** —— 连接在
         * 「发出去之前」就失败了（客户端要为 RTP 另开 socket 去绑同一个本地端口 554）。
         * 另外 agent 的 ingest 本来就是 TCP 的（gortsplib 服务端），走 TCP 才对得上。
         */
        setup.transport = RTSP_TRANSPORT_TCP;
        setup.vid_frame_size = RTP_FRAME_SIZE;
        err = esp_rtsp_service_setup(s_rtsp, &setup);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    {
        char ip[16];
        fill_sta_ip(ip, sizeof(ip));
        if (ip[0] != '\0') {
            (void)esp_rtsp_service_set_ip(s_rtsp, ip);
        }
    }

    err = esp_rtsp_service_set_url(s_rtsp, push_url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "推流地址不合法（%s）: %s", push_url, esp_err_to_name(err));
        goto fail;
    }

    err = esp_media_service_link(ESP_SERVICE_BASE(&s_source.media), ESP_MEDIA_DEFAULT_STREAM, ESP_SERVICE_BASE(s_rtsp),
                                 ESP_MEDIA_DEFAULT_STREAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "源与 SINK 链接失败: %s", esp_err_to_name(err));
        goto fail;
    }
    s_linked = true;

    /* 必须先起消费端（SINK）再起生产端（源）—— 官方文档明确要求这个顺序 */
    err = esp_service_start(ESP_SERVICE_BASE(s_rtsp));
    if (err == ESP_OK) {
        err = esp_service_start(ESP_SERVICE_BASE(&s_source.media));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动失败: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "RTSP 推流已启动: %s", push_url);
    return ESP_OK;

fail:
    (void)rtsp_push_stop();
    return err;
}

esp_err_t rtsp_push_stop(void)
{
    /* 顺序与启动相反：先停生产端，再停消费端，最后解链/销毁 */
    if (s_source_ready) {
        (void)esp_service_stop(ESP_SERVICE_BASE(&s_source.media));
    }
    if (s_rtsp != NULL) {
        (void)esp_service_stop(ESP_SERVICE_BASE(s_rtsp));
    }

    if (s_linked && s_rtsp != NULL) {
        (void)esp_media_service_unlink(ESP_SERVICE_BASE(&s_source.media), ESP_MEDIA_DEFAULT_STREAM,
                                       ESP_SERVICE_BASE(s_rtsp), ESP_MEDIA_DEFAULT_STREAM);
        s_linked = false;
    }

    if (s_rtsp != NULL) {
        (void)esp_media_service_deinit(ESP_SERVICE_BASE(s_rtsp));
        free(s_rtsp);
        s_rtsp = NULL;
    }

    if (s_source_ready) {
        (void)esp_media_service_deinit(ESP_SERVICE_BASE(&s_source.media));
        s_source_ready = false;
    }

    if (s_source.mngr != NULL) {
        /* track manager 必须在确认没人再持有帧之后再销毁 */
        (void)esp_media_track_mngr_destroy(s_source.mngr);
        s_source.mngr = NULL;
    }

    s_session_alive = false;
    return ESP_OK;
}

bool rtsp_push_session_alive(void)
{
    return s_session_alive;
}
