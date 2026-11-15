"""在宿主机运行生产采集函数，验证低帧率、丢帧和重连的真实时间轴。"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
source = (repo / "main/rtsp_push.c").read_text(encoding="utf-8")
start = source.index("static void camera_capture_task(void *arg)")
end = source.index("static esp_err_t camera_source_get_role", start)
production_capture = source[start:end]
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef int TickType_t;
typedef int esp_err_t;
#define ESP_OK 0
#define MIN_SOURCE_FPS 24
#define CAMERA_TRACK_ID 0
#define ESP_MEDIA_TRACK_TYPE_VIDEO 1
#define ESP_MEDIA_FRAME_FLAG_KEY 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef struct {
    void *task, *mngr;
    int running;
    uint16_t width, height, fps;
    int64_t pts_ms; /* 保留旧字段，以便旧生产函数确实运行并失败。 */
} camera_source_t;
typedef struct { void *buf; size_t len; } camera_fb_t;
typedef struct {
    int track_id, type;
    void *data;
    size_t size;
    int64_t pts, dts;
    int flags;
} esp_media_frame_t;
static int frame_index, write_index, fail_capture, fail_write;
static int64_t now_us, actual_us[4], observed[4];
static camera_source_t *current;
int64_t esp_timer_get_time(void) { return now_us; }
static TickType_t xTaskGetTickCount(void) { return 0; }
static void vTaskDelay(int ticks) { (void)ticks; }
static void vTaskDelayUntil(TickType_t *last, TickType_t period) {
    (void)last; (void)period;
}
static void vTaskDelete(void *task) { (void)task; }
static camera_fb_t *esp_camera_fb_get(void) {
    static camera_fb_t fb;
    now_us = actual_us[frame_index];
    int index = frame_index++;
    if (index == fail_capture) return NULL;
    return &fb;
}
static void esp_camera_fb_return(camera_fb_t *fb) { (void)fb; }
static esp_err_t esp_media_track_write_frame(void *mngr, const esp_media_frame_t *f, int timeout) {
    (void)mngr; (void)timeout;
    assert(f->pts == f->dts);
    observed[write_index++] = f->pts;
    if (frame_index == 4) current->running = 0;
    return frame_index - 1 == fail_write ? 1 : ESP_OK;
}
'''
main = r'''
static void run(int capture_failure, int queue_failure, int64_t start_us) {
    camera_source_t source = {.running=1, .fps=30, .width=1920, .height=1080};
    current = &source;
    frame_index = write_index = 0;
    fail_capture = capture_failure;
    fail_write = queue_failure;
    now_us = start_us;
    actual_us[0] = start_us + 100000;
    actual_us[1] = start_us + 350000;
    actual_us[2] = start_us + 700000;
    actual_us[3] = start_us + 1000000;
    camera_capture_task(&source);
    const int64_t expected[] = {100,350,700,1000};
    int n = 0;
    for (int i=0;i<4;i++) {
        if (i == capture_failure) continue;
        assert(observed[n++] == expected[i]);
    }
    assert(write_index == n);
    assert(source.task == NULL);
}
int main(void) {
    run(-1,-1,0);        /* 实际4fps、请求30fps，1秒仍应推进1秒。 */
    run(-1,1,0);         /* 队列丢帧不压缩后续时间。 */
    run(1,-1,0);         /* 抓帧失败后的时间间隔保留。 */
    run(-1,-1,5000000);  /* 重连从新的单调时钟起点计时。 */
    puts("4 capture timing scenarios passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="zaowu-capture-timing-") as temp:
    folder = Path(temp)
    c = folder / "capture_timing.c"
    exe = folder / "capture_timing.exe"
    c.write_text(stub + production_capture + main, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
