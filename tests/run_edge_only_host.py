"""用宿主 GCC 执行真实配置与启动代码；不连接设备或网络。"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--idf-path", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
gcc = shutil.which("gcc")
if not gcc:
    raise SystemExit("需要 PATH 中的 GCC")
headers = {
    "esp_err.h": """#pragma once
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_NOT_SUPPORTED 5
#define ESP_ERR_NVS_NO_FREE_PAGES 6
#define ESP_ERR_NVS_NEW_VERSION_FOUND 7
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) abort(); } while (0)
static inline const char *esp_err_to_name(int n) { (void)n; return "error"; }
""",
    "esp_log.h": """#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
""",
    "sdkconfig.h": """#pragma once
#define CONFIG_CAM_BOARD_ESP32S3_EYE 1
#define CONFIG_CAM_ID "cam-host"
#define CONFIG_CAM_WORKSHOP "ws-host"
#define CONFIG_CAM_REGISTER_URL ""
#define CONFIG_CAM_RTSP_PUSH_URL "rtsp://192.168.1.50:8554/cam-host"
#define CONFIG_CAM_WIFI_SSID "host-test"
#define CONFIG_CAM_WIFI_PASSWORD "test-only"
#define CONFIG_CAM_FRAME_BUF_COUNT 2
#define CONFIG_CAM_REGISTER_USE_SIGNATURE 1
""",
    "nvs.h": """#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
""",
    "nvs_flash.h": """#pragma once
#include "esp_err.h"
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
""",
    "esp_system.h": "void esp_restart(void);",
    "media_lib_adapter.h": '#include "esp_err.h"\nesp_err_t media_lib_add_default_adapter(void);',
    "freertos/FreeRTOS.h": "#define pdMS_TO_TICKS(x) (x)",
    "freertos/task.h": "void vTaskDelay(unsigned int);",
}
for header in ("driver/sdspi_host.h", "driver/spi_common.h", "esp_vfs_fat.h", "sdmmc_cmd.h"):
    headers[header] = "#pragma once"
cjson = args.idf_path / "components/json/cJSON"
with tempfile.TemporaryDirectory(prefix="camera-edge-test-") as temp:
    out = Path(temp)
    for name, text in headers.items():
        dest = out / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(text, encoding="utf-8")
    for legacy in (False, True):
        executable = out / ("legacy.exe" if legacy else "local.exe")
        command = [gcc, "-std=c11", "-I", str(out), "-I", str(repo / "main"), "-I", str(cjson)]
        if legacy:
            command += ["-DCONFIG_CAM_LEGACY_CLOUD_REGISTER=1"]
        command += [str(repo / "tests/edge_only_host.c"), str(repo / "main/app_main.c"), str(cjson / "cJSON.c"), "-o", str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
