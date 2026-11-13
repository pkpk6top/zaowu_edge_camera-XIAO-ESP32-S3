# 凿物 · 摄像头端固件（XIAO ESP32S3 Sense）

摄像头连接工坊 Wi-Fi，按现场配置把 MJPEG 画面推给边缘代理。机位仍需在平台登记并配置到代理；控制面通信、转码和云端推流由代理负责。摄像头不需要直连云端 HTTPS。

```mermaid
flowchart LR
    Cam["摄像头"] -- "RTSP / MJPEG" --> Edge["现场边缘代理 ingest :8554"]
    Cloud["云端平台"] -. "登记与控制配置" .-> Edge
    Edge -- "转码与直播推流" --> Cloud
```

`camera.json` 中配置已登记机位的 `camera_id`、`workshop_id`、Wi-Fi 和本地 `rtsp_push_url`。
默认启动不调用云端注册、不等待云端响应或 SNTP 校时；即使保留旧 `register_url` 也不会开启云注册。
配置缺少本地推流地址时会明确报错，不会退回云端寻找地址。

历史摄像头直连注册代码保留为默认关闭的 `CONFIG_CAM_LEGACY_CLOUD_REGISTER`，仅供旧设计排查。
该历史路径不作为当前打印、直播和观看的验收前置。
---

## 一、目录结构

```
XIAO-ESP32-S3/
├── CMakeLists.txt            # 工程入口
├── sdkconfig.defaults        # PSRAM / 分区 / 媒体服务裁剪（通用）
├── sdkconfig.defaults.esp32s3# S3 专属：八线 PSRAM、240MHz、缓存行
├── partitions.csv            # 8MB flash 下给应用 4MB
├── camera.json.example       # 配置样例，拷成 camera.json 放到 SD 卡根目录
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml     # esp32-camera / esp_rtsp_service / esp_media_service / esp_service
    ├── Kconfig.projbuild     # 编译期默认值（只在无 SD、无 NVS 时生效）
    ├── app_main.c            # 主流程：配置 → Wi-Fi → 摄像头 → 本地边缘推流 → 保活
    ├── cam_config.c/.h       # 配置三级加载（SD → NVS → 编译期）
    ├── wifi.c/.h             # Wi-Fi STA（SSID/密码来自配置，断线自动重连）
    ├── camera.c/.h           # OV2640 初始化（分辨率/画质/翻转/画面参数来自配置）
    ├── register.c/.h         # 历史云注册（仅显式启用旧模式时调用）
    ├── rtsp_push.c/.h        # MJPEG 源服务 + RTSP SINK 推流
    └── ed25519.c/.h          # 自带 Ed25519 实现（原因见第五节）
```

---

## 二、配置文件

放在 SD 卡根目录，文件名固定 `camera.json`：

```json
{
  "camera_id":   "cam-03",
  "workshop_id": "ws-001",
  "rtsp_push_url": "rtsp://192.168.1.50:8554/cam-03",

  "wifi": { "ssid": "workshop-ap", "password": "xxxxxxxx" },

  "video": {
    "resolution":   "VGA",
    "jpeg_quality": 10,
    "fps":          30,
    "fb_count":     2,
    "vflip":        1,
    "hmirror":      1
  },

  "image": {
    "awb":            1,
    "awb_gain":       1,
    "wb_mode":        0,
    "aec":            1,
    "aec2":           1,
    "ae_level":       0,
    "agc":            1,
    "gainceiling":    1,
    "brightness":     0,
    "contrast":       1,
    "saturation":     0,
    "lenc":           1,
    "bpc":            1,
    "wpc":            1,
    "raw_gma":        1,
    "special_effect": 0
  },

  "codec": "MJPEG",

  "retry": { "register_max_attempts": 10, "register_backoff_ms": 5000 }
}
```

| 字段 | 必填 | 默认 | 说明 |
| --- | --- | --- | --- |
| `camera_id` | ✅ | — | 平台分配的摄像头唯一标识，也是推流路径（`/cam-03`） |
| `workshop_id` | ✅ | — | 所属工坊 ID |
| `rtsp_push_url` | ✅ | — | 本地边缘代理 ingest 的完整地址，例如 `rtsp://192.168.1.50:8554/cam-03` |
| `register_url` | 历史模式 | — | 仅显式启用 `CONFIG_CAM_LEGACY_CLOUD_REGISTER` 时使用的云端注册地址 |
| `wifi.ssid` / `wifi.password` | ✅ / ❌ | — / 空 | 密码留空 = 开放 AP |
| `video.resolution` | ❌ | `VGA` | `QQVGA` / `QVGA` / `VGA`（`480p`） / `SVGA` / `XGA` / `HD`（`720p`） / `SXGA` / `UXGA` / `FHD`（`1080p`） / `QXGA`；超出传感器上限（OV2640→UXGA，OV3660→QXGA）会被夹到上限并告警 |
| `video.jpeg_quality` | ❌ | `10` | 0–63，越小画质越好（12 → 10：码率涨约三成，压缩伪影与颗粒感都轻些） |
| `video.fps` | ❌ | `30` | 采集节流上限，**硬下限 24**；高分辨率档受传感器限制实际会更低 |
| `video.fb_count` | ❌ | `2` | 帧缓冲个数（实际取值以固件的 `CONFIG_CAM_FRAME_BUF_COUNT` 为准） |
| `video.vflip` / `hmirror` | ❌ | `1` / `1` | 画面翻转 / 镜像；两个都开 = 画面转 180°（机位倒装时这么设） |
| `image.*` | ❌ | 见下 | 画面参数（白平衡 / 曝光 / 增益 / 画质），整段可选 |
| `codec` | ❌ | `MJPEG` | 目前只实现了 MJPEG（见第五节） |
| `retry.register_max_attempts` | 历史模式 | `10` | 旧云注册失败重试次数，用尽则重启 |
| `retry.register_backoff_ms` | ❌ | `5000` | 每次重试的间隔 |

### `image` 段（画面参数）

取值语义与 esp32-camera 的 `sensor_t` 一致；**整段可选，只覆盖写了的那几项**。
默认值是按「偏色 + 发灰 + 噪点」三个症状调的一版：

| 字段 | 默认 | 取值 | 调它解决什么 |
| --- | --- | --- | --- |
| `awb` | `1` | 0/1 | 自动白平衡；关掉就是偏色且不会自纠 |
| `awb_gain` | `1` | 0/1 | 白平衡增益；关掉偏色会更明显 |
| `wb_mode` | `0` | 0自动 / 1晴天 / 2阴天 / 3办公室 / 4家 | 自动仍偏色时锁一档试试（阴天最暖） |
| `aec` / `aec2` | `1` / `1` | 0/1 | 自动曝光；关掉就得自己给 `aec_value` |
| `ae_level` | `0` | -2~2 | 曝光补偿：-1 防过曝、+1 救偏暗 |
| `aec_value` | 不设 | 0~1200 | 只在关了 `aec` 时用；不写 = 用自动 |
| `agc` | `1` | 0/1 | 自动增益；关掉就得自己给 `agc_gain` |
| `agc_gain` | 不设 | 0~30 | 只在关了 `agc` 时用；不写 = 用自动 |
| `gainceiling` | `1` | 0=2X 1=4X 2=8X 3=16X 4=32X 5=64X 6=128X | **压噪点**的旋钮：调小噪声少但暗处变黑 |
| `brightness` | `0` | -2~2 | 整体亮度 |
| `contrast` | `1` | -2~2 | +1 治「发灰」 |
| `saturation` | `0` | -2~2 | +1 治「颜色发木」 |
| `lenc` | `1` | 0/1 | 镜头校正：治四周色晕 / 暗角 |
| `bpc` / `wpc` | `1` / `1` | 0/1 | 坏点 / 白点校正 |
| `raw_gma` | `1` | 0/1 | 原始伽马校正；关掉画面偏灰 |
| `special_effect` | `0` | 0无 1负片 2黑白 3偏红 4偏绿 5偏蓝 6复古 | 特效用 |

⚠ **OV2640 上调不了的两项**：`set_sharpness` / `set_denoise` 在驱动里是空实现（标了 not supported），
所以没有锐度 / 降噪旋钮 —— 颗粒感只能靠 `gainceiling` 与 `jpeg_quality` 压。

⚠ **驱动也没有抗闪烁（50Hz 条纹）接口**，要治得直接写寄存器，目前没做。

**三级优先级**：SD 卡上的 `camera.json` → NVS 备份 → 编译期默认值（`menuconfig`）。

- 读到 SD 卡配置后会**自动写一份到 NVS**，所以之后拔卡也能启动；
- NVS 里那份是二进制结构体，固件升级改了结构会被自动忽略（回落到编译期默认值）；
- 级别低的那些只在上一级拿不到时才用，`menuconfig` → `Camera Agent Configuration` 里填。

### 分辨率与帧率（实测）

OV2640（本机那颗，XCLK 24MHz）实测：

| 采集档 | 实测帧率 | 备注 |
| --- | --- | --- |
| **VGA 640×480** | **28 fps** | 像素吞吐约 8.5Mpix/s |
| SVGA 800×600 | 17 fps | 同上，像素数多了 56% |
| XGA / HD / UXGA | ~15 fps | 驱动上这三档与 SVGA 共用同一读出模式，但 JPEG/DSP 成为瓶颈 |

**瓶颈是传感器读出，不是 Wi-Fi**：VGA 与 SVGA 的「像素 × 帧率」几乎一样（≈8.5Mpix/s）。
所以想要「源码流 ≥24fps」，采集档就只能是 **VGA 或更小**；更高分辨率请交给边缘侧转码去缩放。

另外两条容易踩的：

- **`fps` 是请求值，不是保证值**。驱动没有 `set_framerate` 接口，采集任务只能给一个上限，
  实际帧率以传感器为准；固件侧另有 **24fps 硬下限**（配置写更低也按 24 节流）。
- **超过传感器上限的档位会被夹低**：OV2640 最高 UXGA、OV3660 最高 QXGA，超了会在日志里
  告警并夹到上限（同一份配置可以喂不同传感器的机器）。

---

## 三、编译与烧录

默认接线为 XIAO ESP32S3 Sense。ESP32-S3 N16R8 / OV5640 使用已核对的
ESP32S3_EYE 接线时，在独立构建目录追加板型配置：

```sh
idf.py -B build-eye -D SDKCONFIG=sdkconfig.eye -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3-eye" -D IDF_TARGET=esp32s3 build
```

该配置使用 16MB Flash、8MB OPI PSRAM 和 20MHz XCLK；摄像头引脚为
XCLK=15、SDA=4、SCL=5、D7..D0=16/17/18/12/10/8/9/11、VSYNC=6、HREF=7、PCLK=13。
GPIO 7/8/9 与 XIAO 的 SD SPI 定义冲突，因此此模式完全跳过 SD 初始化，
配置只从 NVS 或编译期默认值读取。机位标识、Wi-Fi 及本地边缘推流地址仍须按现场配置，不能使用其他机位的值。

上述命令仅编译。首次迁移须核对接线、串口、分区表与已登记身份，并保留当前固件和 NVS 的回退备份；
不要执行 `erase-flash`。板型适配不改变历史云注册模式的 HTTPS 证书校验。默认本地接入不调用该模式。
只有显式启用历史签名注册时，才先通过 SNTP 限时等待校时；校时失败时不发送带错误时间戳的请求。

旧 NVS 配置的原 904 字节字段布局保持不变；升级读取旧配置时保留 Wi-Fi、机位和画质，新增推流地址使用 `CONFIG_CAM_RTSP_PUSH_URL`。因此无 SD 的旧机位升级前需配置该本地地址，不能擦除 NVS 或设备身份来迁移。

XIAO 原有构建方式：

```bash
idf.py set-target esp32s3
idf.py menuconfig      # 可选：填 camera_id / workshop_id / rtsp_push_url / Wi-Fi 默认值
idf.py build
idf.py -p <串口> flash monitor
```

`menuconfig` 里确认两处：

- `Component config → ESP PSRAM → Support for external, SPI-connected RAM` 已启用
  （**必须**，否则 OV2640 的帧缓冲分配不出来 —— `sdkconfig.defaults` 已默认打开）；
- `Camera Agent Configuration` 下的默认值（只在没有 SD 卡配置时生效）。

启动日志里能看到配置来源与最终生效值：

```
I (1234) cam_cfg: SD 卡已挂载: SD32G
I (1240) cam_cfg: 配置来源：SD 卡
I (1246) cam_cfg: ────────── 摄像头配置 ──────────
I (1252) cam_cfg:   camera_id:   cam-03
I (1258) cam_cfg:   wifi.pass:   ***
I (1264) cam_cfg:   video:       VGA @30fps q=10 fb=2
I (1266) cam_cfg:   flip/mirror: 1/1
I (1268) cam_cfg:   image:       awb=1/1 wb_mode=0 aec=1/1 ae=0 agc=1/ceil=1
I (1270) cam_cfg: ───────────────────────────────
I (1290) wifi: 获取 IP: 192.168.1.77
I (1400) app: 使用本地边缘代理配置，不发起云端注册
I (1500) rtsp_push: 推流会话已建立（RECORD）
```

⚠ **`配置来源：default` 说明 SD 卡与 NVS 都没读到**（日志里还有 `SD 卡挂载失败: ESP_ERR_TIMEOUT`）。
这时生效的是编译期默认值，**你改的 `camera.json` 根本没用上** —— 先查卡（拔插 / 换卡），
或改 `main/cam_config.c` 的 `fill_defaults` 重编烧录。

---

## 四、历史云注册接口（默认关闭）

以下只适用于显式开启 `CONFIG_CAM_LEGACY_CLOUD_REGISTER=y` 的历史构建。正常本地边缘接入不执行这些步骤，也不依赖该接口可用。

`POST {register_url}`，`Content-Type: application/json`：

```json
{
  "camera_id": "cam-03",
  "workshop_id": "ws-001",
  "codec": "MJPEG",
  "resolution": "VGA",
  "fps": 30,
  "public_key": "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEA...\n-----END PUBLIC KEY-----",
  "signature": "<base64(ed25519 signature)>",
  "timestamp": 1790000000
}
```

签名口径与边缘 agent 一致，**被签名的规范串**是四个字段用 `\n` 拼接：

```text
camera_id
workshop_id
public_key
timestamp
```

- `public_key` 是 PEM(PKIX) 的 Ed25519 公钥（OID `1.3.101.112`）；PEM 块内的换行保留，
  只裁掉结尾换行 —— 与 agent 侧 `RegisterRequest.SigningPayload()` 逐字对齐；
- `timestamp` 是 Unix 秒（固件启动时用 SNTP 对时，平台侧按时间窗拒重放）；
- 平台验签：`ed25519.Verify(pub, payload, sig)`，与 agent 侧同一套。

平台应当返回：

```json
{ "edge_ip": "192.168.1.50", "ingest_port": 8554, "rtsp_push_url": "rtsp://192.168.1.50:8554/cam-03" }
```

`rtsp_push_url` 缺省时固件按 `rtsp://{edge_ip}:{ingest_port}/{camera_id}` 自行拼接。

**设备身份**：32 字节种子在**首次启动时生成**并存入 NVS（命名空间 `camid`），不落明文文件。
删掉它就是换了一台新设备，平台上的绑定会失效 —— 这一点与 agent 的 `agent.key` 同理。

不想带签名时把 `menuconfig → Camera Agent Configuration → 注册时带 ed25519 签名` 关掉，
注册体只剩前五个字段。

---

## 五、几个实现上的取舍

**推流为什么用 `esp_rtsp_service` 的 SINK 角色。**
边缘 agent 的 ingest 是**服务端**，只实现 `ANNOUNCE / SETUP / RECORD`（不支持 `DESCRIBE / PLAY`）。
`esp_rtsp_service` 的 SINK 角色走的正是这三条，所以这里只能当推流端，不能做拉流端。
摄像头源是自己写的媒体服务（`rtsp_push.c` 里的 `camera_source_*`）：一条 MJPEG 视频轨，
采集任务把帧写进 track manager，再由 `esp_media_service_link()` 接到 SINK 上。
启动顺序必须是**先 SINK、后源**（官方文档明确要求）。

**为什么只有 MJPEG。**
OV2640 原生只出 JPEG。要推 H.264 得另接 `esp_h264` 软件编码（ESP32-S3 上没有可用的
H.264 硬编码），代价很大；而 MJPEG over RTP 是 RTSP 的标准负载类型，边缘侧不需要额外转码。
所以 `codec` 填 `H264` 时固件会**告警并按 MJPEG 推**，不会静默改变语义。
转成 H.264 的活在**边缘 agent** 上做（`zaowu-edge-agent` 的 `internal/transcode`，ffmpeg），
设备的采集流程因此不用背编码开销。

**为什么采集锁在 VGA。**
见第二节的实测表：OV2640 的像素吞吐只有 ≈8.5Mpix/s，分辨率上去帧率就下来（SVGA 只剩 17fps）。
「源码流 ≥24fps」与「更高分辨率」在传感器层就不共存，所以固件的默认档位定在 VGA，
高分辨率交给边缘侧转码缩放。

**为什么 XCLK 给 24MHz。**
OV2640/OV3660 的 XVCLK 允许 6~27MHz、典型 24MHz。给 20MHz 时 VGA 实测只有 ~23fps（卡在
24fps 下限下），提到 24MHz 后到 ~28fps。⚠ 驱动里的 PLL / JPEG 时序表是按 20MHz 调的，
换 XCLK 后如果画面异常（曝光/色彩/撕裂），先回 20MHz 排除。

**为什么采集循环用 `vTaskDelayUntil` 而不是 `vTaskDelay(interval)`。**
抓帧 + 写帧本身要花几毫秒，固定 sleep 的实际周期 = interval + 处理耗时 ——
24fps 实测只跑到 20.6fps。按周期对齐后耗时从 sleep 里扣，才能真跑到请求值。

**为什么画面参数单独一个 `image` 段。**
这些旋钮（白平衡 / 曝光 / 增益 / 对比度…）是「现场按光线调」的，与「我是谁、怎么连」无关，
所以既不在必填区，也不塞进 `video`（那一块是分辨率/帧率/画质之类决定带宽的量）。
整段可选、缺省走 `fill_defaults` 里那套按症状调好的值。

**画面方向（`vflip` / `hmirror`）。**
机位倒装时**两个都要开**才是「画面转 180°」；只开一个得到的是镜像画面 ——
看人脸看不出来，拍文字会反过来。

**为什么自带一份 Ed25519。**
ESP-IDF v5.5 内置的 mbedtls 3.6.6 只声明了 `PSA_ALG_PURE_EDDSA` 这个算法号，
库层面并没有实现（`library/` 下既没有 eddsa 源文件，也没有 `PURE_EDDSA` 的实现），
公开 API 拿不到 Ed25519 签名。`main/ed25519.c` 因此用 mbedtls 的**大数与 SHA-512**
（已验证过的域算术 / 模幂 / 模逆）按 RFC 8032 写了一层薄实现：

- 曲线常量（p / L / d / sqrt(-1) / 基点）在文件里逐字写出；
- 朴素 double-and-add，不做常数时间、不做预计算表 —— 一次签名为终点，够用；
- 启动时跑一遍 RFC 8032 §7.1 的两组测试向量（`ed25519_selftest()`），
  现场就能确认实现没被改坏；`Ed25519 自检未通过` 出现即说明签名不可信。

**帧缓冲与内存。**
帧缓冲放 PSRAM（`CAMERA_FB_IN_PSRAM`），track manager 用的是 INTERNAL 缓存
（自己持有帧数据副本），所以 `esp_camera_fb_get()` 拿到的帧可以立刻归还。
代价是每帧一次拷贝，换来的是采集任务与推流互不拖死：队列满时**丢帧**而不是阻塞。

---

## 六、运维

- **改配置**：编辑 SD 卡上的 `camera.json` → 插卡重启。没有 SD 卡时用
  `idf.py nvs_partition_gen` 生成 NVS 分区一次性烧录，或临时插卡。
- **推流断了**：主循环每 30 秒检查一次 RTSP 会话，断了就重建推流
  （边缘服务器重启、网络抖动都会让会话消失）。
- **Wi-Fi 断了**：事件回调里无限重连。
- **本地地址缺失**：填写已登记机位对应的 `rtsp_push_url`；固件不会自动改用旧云注册。
- **历史模式注册失败**：仅显式启用旧模式时，按 `retry` 配置退避重试，用尽后重启设备。
- **密码脱敏**：`cam_config_dump()` 只打 `***`，不会把 Wi-Fi 密码写进日志。

---

## 七、还没做的

- **设备侧 H.264 推流**（需要 `esp_h264`；目前转码在边缘 agent 侧做，见第五节）
- **抗闪烁（50Hz 条纹）**：驱动没有这个接口，要治得直接写 OV2640 寄存器（SENSOR 组的
  `COM11` 之类），没做
- **锐度 / 降噪**：OV2640 的 `set_sharpness` / `set_denoise` 在 esp32-camera 里是空实现，
  调不了 —— 颗粒感只能靠 `gainceiling` 与 `jpeg_quality` 压
- **摄像头画质远程配置**：目前固件配置只从 SD/NVS/Kconfig 来，边缘代理尚不能直接覆盖摄像头的
  `resolution` / `fps` / `image` 等参数；此变更不新增代理代注册协议
- **历史注册模式的分级重连**：旧模式注册失败使用固定退避，未实现指数退避


## 八、本地接入回归

使用宿主 GCC 和 ESP-IDF 自带 cJSON 执行，不连接摄像头或云端：

```text
python tests/run_edge_only_host.py --idf-path <ESP-IDF目录>
```

覆盖默认无云注册地址、旧地址不触发云注册、缺失/错误本地地址拒绝、旧 NVS 配置迁移、
新配置往返、初次推流与断线重建，以及显式历史注册模式。宿主回归与双板型编译不代替实机验收。
