# Smart Photo Frame — codex.md

## 目的

本文件是给 Codex 使用的项目协作说明，内容以 `CLAUDE.md` 为基线，并补充面向当前仓库的执行规则。

## 项目概述

面向养老院老人的远程照片相框。家人通过微信小程序发送照片，相框实时显示。

## 双模式照片显示

- MQTT 直播模式：设备订阅 `device/{device_id}/image`，收到云端图片通知后下载 `/sdcard/current.bmp` 并立即刷新屏幕。
- 本地定时模式：长按应用按键切换到本地定时轮播，定时从 `/sdcard/LOCAL` 选择 BMP 显示；此模式下新收到的 MQTT 图片只写入本地图库，不抢占当前显示。
- 两种模式共享同一个 `display_photo()` 显示入口和互斥锁，屏幕刷新串行执行，并保留最小停留时间，避免电子墨水屏被连续刷新打断。
- MQTT 图片下载成功后会缓存到 `/sdcard/LOCAL`，文件名使用 `P0000001.BMP` 这类 8.3 短文件名，确保在未启用 FATFS 长文件名时也能正常创建和轮播。
- 本地定时模式支持两种服务器控制：`interval_seconds` 表示每隔 N 秒切图；`daily_time` / `switch_time` / `switch_at` 表示每天固定北京时间切图，例如 `{"daily_time":"08:30"}`。服务端接口为 `POST /api/devices/{device_id}/local-schedule`。

## 系统架构

## Build Ownership

- 固件编译、烧录、串口监视由用户手动执行。
- Codex 不主动运行 `idf.py build`、`idf.py flash`、`idf.py monitor`。
- 只有当用户在当前对话里明确要求时，Codex 才可以执行这些命令。

```text
微信小程序（家人手机）
       │  POST /api/upload
       ▼
腾讯云服务器（Python FastAPI）
  ├── 图片转换：任意格式 → 800×480 6色 BMP（Pillow）
  ├── 设备绑定表：devices.json
  └── 图片存储：photos/{device_id}/current.bmp
       │  GET /api/photo/{device_id}/latest.bmp（每 30 秒轮询）
       ▼
ESP32-S3-PhotoPainter（相框）
  ├── WiFi STA 连接养老院网络
  ├── HTTP 下载 BMP → 写入 SD 卡
  └── GUI_ReadBmp_RGB_6Color() → epaper_port_display
       ▼
7.3 寸 E6 全彩电子墨水屏（800×480，6 色）
```

## 硬件信息

- 主控：ESP32-S3
- 屏幕：7.3 寸 E6 全彩电子墨水屏（Spectra 6）
- 分辨率：800 × 480
- 色彩：黑 / 白 / 绿 / 蓝 / 红 / 黄
- 存储：MicroSD（4-bit SDMMC）

### 电子墨水屏 SPI 引脚

- `DC = GPIO8`
- `CS = GPIO9`
- `SCK = GPIO10`
- `MOSI = GPIO11`
- `RST = GPIO12`
- `BUSY = GPIO13`

### SDMMC 引脚

- `CLK = GPIO39`
- `CMD = GPIO41`
- `D0 = GPIO40`
- `D1 = GPIO1`
- `D2 = GPIO2`
- `D3 = GPIO38`

## 图片格式

- 分辨率：`800 x 480`
- 格式：BMP，无压缩
- 显示函数：`GUI_ReadBmp_RGB_6Color()`
- 6 色调色板：
  - 黑：`(0, 0, 0)`
  - 白：`(255, 255, 255)`
  - 黄：`(255, 255, 0)`
  - 红：`(255, 0, 0)`
  - 蓝：`(0, 0, 255)`
  - 绿：`(0, 128, 0)`

## 目录重点

- 固件入口：`firmware/main/main.c`
- WiFi：`firmware/main/wifi_sta.c`
- 配置读取：`firmware/main/frame_config.c`
- 图片下载：`firmware/components/photo_client/photo_client.c`
- 屏驱动：`firmware/components/epaper_port/epaper_port.c`
- BMP 解码：`firmware/components/epaper_src/GUI_BMPfile.c`
- 服务端图片转换：`server/image_processor.py`
- 问题记录：`docs/ISSUES.md`

## Codex 工作规则

- 先读代码和文档，再做改动；不要凭记忆假设板级实现
- 优先做最小修改，尽量沿用厂家 Demo 的已验证链路
- 找到根因或关键排查结论后，及时更新 `docs/ISSUES.md`
- 涉及显示异常时，优先排查三段链路：
  - 图片格式是否符合 800×480 6 色 BMP
  - `GUI_ReadBmp_RGB_6Color()` 是否正确映射颜色
  - 面板供电、busy、SPI 初始化是否与厂家板级流程一致

## 协作约定

- 固件编译、烧录、串口监视默认由用户手动执行
- Codex 默认不主动运行 `idf.py build`、`idf.py flash`、`idf.py monitor`，除非用户明确要求
- Codex 负责：
  - 修改代码
  - 分析日志
  - 对照厂家源码排查差异
  - 更新 `docs/ISSUES.md` / 相关文档
  - 给出用户需要执行的验证步骤和关键日志点

## 开发环境

- 固件：ESP-IDF v5.x，目标芯片 `esp32s3`
- 服务器：Python 3.x，FastAPI + Pillow + Uvicorn
- 小程序：微信开发者工具

## 注意事项

- 电子墨水屏整屏刷新约需 15 秒，属于正常现象
- 图片尽量避免大面积渐变，E6 6 色屏对颜色非常敏感
- `esp_log` 和 `heap` 在 ESP-IDF v5.x 中已内置，不需要在组件 `REQUIRES` 中单独声明
- 如果怀疑屏幕没显示但日志正常，优先核对板级供电初始化，而不是先重写 `epaper_port.c`
