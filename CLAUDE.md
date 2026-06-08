# Smart Photo Frame — CLAUDE.md

## 项目概述

面向养老院老人的远程照片相框。家人通过微信小程序发送照片，相框实时显示。

## 系统架构

```
微信小程序（家人手机）
       │  POST /api/upload
       ▼
腾讯云服务器（Python FastAPI）
  ├── 图片转换：任意格式 → 800×480 6色BMP（Pillow）
  ├── 设备绑定表：devices.json
  └── 图片存储：photos/{device_id}/current.bmp
       │  GET /api/photo/{device_id}/latest.bmp（每30秒轮询）
       ▼
ESP32-S3-PhotoPainter（相框）
  ├── WiFi STA 连接养老院网络
  ├── HTTP 下载 BMP → 写入 SD 卡
  └── GUI_ReadBmp_RGB_6Color() → epaper_port_display
       ▼
7.3寸 E6 全彩电子墨水屏（800×480，6色）
```

## 硬件规格

| 参数 | 规格 |
|------|------|
| 主控 | ESP32-S3（双核 LX7，240MHz） |
| 屏幕 | 7.3寸 E6 全彩电子墨水屏（Spectra 6） |
| 分辨率 | 800 × 480 |
| 显示色彩 | 6色：黑 / 白 / 绿 / 蓝 / 红 / 黄 |
| 无线 | 2.4GHz Wi-Fi + BLE 5 |
| 存储 | MicroSD 卡（4-bit SDMMC） |
| 外壳 | 实木相框 |
| 功耗 | 超低待机（电子墨水断电保持画面） |

### SPI 引脚（电子墨水屏）

| 信号 | GPIO |
|------|------|
| DC   | 8    |
| CS   | 9    |
| SCK  | 10   |
| MOSI | 11   |
| RST  | 12   |
| BUSY | 13   |

### SDMMC 引脚

| 信号 | GPIO |
|------|------|
| CLK  | 39   |
| CMD  | 41   |
| D0   | 40   |
| D1   | 1    |
| D2   | 2    |
| D3   | 38   |

## 图片格式规范

| 参数 | 要求 |
|------|------|
| 分辨率 | 800 × 480 像素 |
| 格式 | BMP（无压缩） |
| 色彩模式 | 6色调色板（Floyd-Steinberg 抖动） |
| 文件大小 | 约 192KB |
| 显示函数 | `GUI_ReadBmp_RGB_6Color()` |

### 6色调色板

| 索引 | 颜色 | RGB |
|------|------|-----|
| 0 | 黑 | (0, 0, 0) |
| 1 | 白 | (255, 255, 255) |
| 2 | 黄 | (255, 255, 0) |
| 3 | 红 | (255, 0, 0) |
| 5 | 蓝 | (0, 0, 255) |
| 6 | 绿 | (0, 128, 0) |

## 目录结构

```
smart-frame/
├── firmware/                  # ESP32 固件（ESP-IDF v5.x）
│   ├── CMakeLists.txt
│   ├── main/
│   │   ├── main.c             # 主程序入口
│   │   ├── frame_config.c/h   # 读取 SD 卡 config.txt
│   │   ├── wifi_sta.c/h       # WiFi STA 连接
│   │   └── photo_client.c/h   # HTTP 图片下载（在 components/photo_client/）
│   └── components/
│       ├── epaper_src/        # 电子墨水屏驱动 + BMP 解码
│       ├── epaper_port/       # SPI 硬件抽象层
│       ├── sdcard_bsp/        # SD 卡读写
│       ├── photo_client/      # 图片下载组件
│       ├── http_server_bsp/   # AP 配网 HTTP 服务器
│       ├── json_bsp/          # ArduinoJson v7
│       └── ListLib/           # 链表库
├── server/                    # 后端服务器
│   ├── main.py                # FastAPI 应用
│   ├── image_processor.py     # 图片转换（Pillow）
│   └── requirements.txt
├── miniprogram/               # 微信小程序
│   ├── pages/index/           # 发送照片页
│   └── pages/bind/            # 绑定相框页
└── docs/
    ├── README.md
    └── ISSUES.md              # 问题记录
```

## 关键代码位置

| 功能 | 文件 |
|------|------|
| 主程序入口 | `firmware/main/main.c` |
| WiFi 连接 | `firmware/main/wifi_sta.c` |
| 配置读取 | `firmware/main/frame_config.c` |
| 图片下载 | `firmware/components/photo_client/photo_client.c` |
| 电子墨水屏驱动 | `firmware/components/epaper_port/epaper_port.c` |
| BMP 解码显示 | `firmware/components/epaper_src/GUI_BMPfile.c` |
| 图片转换（服务器） | `server/image_processor.py` |
| API 接口（服务器） | `server/main.py` |
| 发送照片（小程序） | `miniprogram/pages/index/index.js` |
| 绑定相框（小程序） | `miniprogram/pages/bind/bind.js` |

## SD 卡 config.txt 格式

## Build Ownership

- 固件编译、烧录、串口监视由用户手动执行。
- Claude 不主动运行 `idf.py build`、`idf.py flash`、`idf.py monitor`。
- 只有当用户在当前对话里明确要求时，Claude 才可以执行这些命令。

```json
{
  "wifi_ssid":     "养老院WiFi名称",
  "wifi_password": "WiFi密码",
  "server_url":    "http://1.2.3.4:8000/api/photo",
  "device_id":     "frame_001",
  "poll_interval": 30
}
```

## 开发环境

- 固件：ESP-IDF v5.x，目标芯片 `esp32s3`
- 服务器：Python 3.x，FastAPI + Pillow + Uvicorn
- 小程序：微信开发者工具

## 注意事项

- 固件编译、烧录、串口监视默认由用户手动执行；协作代理负责改代码、查问题、更新文档，并给出需要观察的日志点与验证步骤
- 除非用户明确要求，否则不要主动在本地执行 `idf.py build`、`idf.py flash`、`idf.py monitor`
- 电子墨水屏每次刷新约需 **15秒**，属正常现象
- 图片建议色彩鲜明，避免大面积渐变（6色限制）
- `esp_log` 和 `heap` 在 ESP-IDF v5.x 中已内置，**不需要**在组件 CMakeLists.txt 的 REQUIRES 中声明
- 问题记录见 `docs/ISSUES.md`
