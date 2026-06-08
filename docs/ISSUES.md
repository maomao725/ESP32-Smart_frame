# 问题记录 ISSUES.md

开发过程中遇到的问题、Bug 及解决方案。

---

## 模板

```
### [BUG-XXX] 问题标题

- **日期**：YYYY-MM-DD
- **模块**：firmware / server / miniprogram
- **状态**：已解决 / 未解决 / 绕过

**现象**：
描述问题表现。

**原因**：
根本原因分析。

**解决方案**：
具体修复步骤或代码改动。

**参考**：
相关链接或文档。
```

---

## 已解决

### [BUG-001] esp_log / heap 组件在 ESP-IDF v5.x 中无法解析

- **日期**：2026-04-14
- **模块**：firmware / components/photo_client
- **状态**：已解决

**现象**：
执行 `idf.py set-target esp32s3` 时 CMake 报错：
```
Failed to resolve component 'esp_log' required by component 'photo_client': unknown name.
```

**原因**：
ESP-IDF v5.x 对组件进行了重构，`esp_log` 和 `heap` 已内置到核心中，不再作为独立组件暴露给 `REQUIRES`。

**解决方案**：
删除 `firmware/components/photo_client/CMakeLists.txt` 中的 `esp_log` 和 `heap` 依赖：
```cmake
# 修改前
REQUIRES
    esp_http_client
    sdcard_bsp
    esp_log
    heap

# 修改后
REQUIRES
    esp_http_client
    sdcard_bsp
```

### [BUG-002] i2c_equipment 组件无法解析导致 json_bsp 编译失败

- **日期**：2026-04-14
- **模块**：firmware / components/json_bsp
- **状态**：已解决

**现象**：
执行 `idf.py set-target esp32s3` 时 CMake 报错：
```
Failed to resolve component 'i2c_equipment' required by component 'json_bsp': unknown name.
```

**原因**：
`json_bsp` 是从微雪原版 Demo（天气时钟相框）复用过来的组件，原版依赖 `i2c_equipment`（RTC/I2C 设备驱动）。智能相框项目不需要天气/RTC 功能，该组件不存在于 components 目录中。

**解决方案**：
1. 从 `main/CMakeLists.txt` 的 `REQUIRES` 中删除 `json_bsp`
2. 精简 `json_bsp/CMakeLists.txt`，移除 `REQUIRES i2c_equipment`
3. 精简 `json_data.h`，删除 `#include "i2c_equipment.h"` 及天气相关结构体
4. 精简 `json_data.cpp`，删除天气解析函数，只保留 `json_sdcard_txt_aimodel()`

### [BUG-003] 直链下载 BMP 时 HTTP RX buffer 申请失败，导致图片无法落盘

- **日期**：2026-04-15
- **模块**：firmware / main / components/photo_client
- **状态**：已解决

**现象**：
设备已经连上 Wi-Fi，HTTP 请求也返回 `200`，但下载开始后立刻报错：
```text
Failed to allocate HTTP RX buffer
ESP_ERR_NO_MEM
Direct photo fetch failed
```
随后图片既没有保存到 SD 卡，也无法进入墨水屏刷新流程。

**原因**：
工程最初未启用 ESP32-S3 的 PSRAM。墨水屏显示缓冲长期占用内部 RAM，而 Wi-Fi / HTTP 客户端缓冲也默认从内部 8-bit heap 申请，联网后可用内部内存不足，最终导致 `photo_client` 申请接收缓冲失败。

**解决方案**：
1. 在 `sdkconfig` / `sdkconfig.defaults` 中启用 PSRAM，并切到与厂家 Demo 接近的 `OCT + 80M` 配置
2. 将墨水屏显示缓冲改为优先分配到 PSRAM
3. 将 HTTP RX buffer、文件 I/O buffer 改为优先分配到 PSRAM，并缩小默认缓冲尺寸
4. 下调 Wi-Fi / LWIP / mbedTLS 的内部内存占用
5. 修复后日志已能稳定完成 `BMP validated -> Photo saved -> Display refreshed`

### [BUG-004] BMP 下载与刷新日志成功，但 7.3 寸 E6 全彩墨水屏没有可见显示

- **日期**：2026-04-15
- **模块**：firmware / epaper_port / board power
- **状态**：已解决

**现象**：
串口日志已经显示：
- `BMP validated: 800x480, 24-bit`
- `Photo saved to /sdcard/current.bmp`
- `Display refreshed: /sdcard/current.bmp`

SD 卡中也能看到新的 BMP 文件，但屏幕本身没有出现预期画面。

**原因**：
排查后确认，当前工程中的 `epaper_port.c`、`GUI_Paint.c`、`GUI_ReadBmp_RGB_6Color()` 主显示链路与厂家 `ESP32-S3-PhotoPainter-Demo` 基本一致，问题不在主屏驱动协议。  
真正根因是当前 `smart-frame` 工程缺少厂家 Demo 的板级初始化流程：厂家在进入 `epaper_port_init()` 前，会先执行 `i2c_master_Init()`、`axp_i2c_prot_init()`、`axp_cmd_init()` 对 AXP2101 PMU 做初始化与电源通道配置；当前工程缺少这层逻辑，导致面板相关供电或上电时序没有准备好。

**解决方案**：
1. 新增最小 `i2c_bsp` 组件，只接 AXP2101 所需 I2C 通道
2. 新增最小 `axp2101_bsp` 组件，补齐关键 PMU 初始化、3.3V 电源轨配置和上电日志
3. 在 `app_main()` 中将板级 PMU 初始化接到 `epaper_port_init()` 之前
4. 用户重新编译烧录后，屏幕已确认可以成功显示图片

### [BUG-005] `esp_efuse_get_chip_id()` 在 ESP-IDF v5.x 中不存在

- **日期**：2026-04-17
- **模块**：firmware / main / frame_config.c
- **状态**：已解决

**现象**：
编译时报错：
```
implicit declaration of function 'esp_efuse_get_chip_id'; did you mean 'esp_cpu_get_core_id'?
```

**原因**：
参考资料中出现的 `esp_efuse_get_chip_id()` 在官方 ESP-IDF v5.x 中并不存在，该 API 为误导性示例。

**解决方案**：
改用 `esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 64)` 读取 ESP32-S3 eFuse Block3 中的 128-bit 出厂唯一 ID（取前 64 bit），格式化为 16 字符大写十六进制字符串。需在 `main/CMakeLists.txt` 的 `REQUIRES` 中添加 `efuse` 组件，头文件改为 `esp_efuse.h` + `esp_efuse_table.h`。

---

### [BUG-006] config.txt MQTT 字段名错误且缺少 JSON 逗号

- **日期**：2026-04-17
- **模块**：firmware / sdcard / config.txt
- **状态**：已解决

**现象**：
MQTT 连接失败，无用户名/密码认证。串口日志显示 MQTT broker 拒绝连接。

**原因**：
1. `mqtt_broker_url` 行末缺少逗号，导致 JSON 解析失败
2. 字段名写成 `"user"` / `"passwd"`，而 `frame_config.c` 读取的是 `"mqtt_username"` / `"mqtt_password"`

**解决方案**：
修正 config.txt：补逗号，字段名改为 `mqtt_username` / `mqtt_password`。

---

### [BUG-007] 墨水屏显示红蓝反色

- **日期**：2026-04-17
- **模块**：firmware / components/epaper_src / GUI_BMPfile.c；server / image_processor.py
- **状态**：已解决

**现象**：
图片中红色区域显示为蓝色，蓝色区域显示为红色。黄色也显示异常（偏青色）。

**原因**：
`GUI_ReadBmp_RGB_6Color()` 从 BMP 文件正确读取了 BGR 字节顺序（`b=row[0], g=row[1], r=row[2]`），但后续颜色比较条件全部写反：
- 把 `r==0 && b==255`（蓝像素）映射到 index 3（Red），结果蓝色显示为红
- 把 `r==255 && b==0`（红像素）映射到 index 5（Blue），结果红色显示为蓝
- Yellow 条件 `r==0,g==255,b==255` 实为青色，应为 `r==255,g==255,b==0`
- Green 条件 `g==255` 应为 `g==128`

同时 `image_processor.py` 中 index 2（黄色）调色板值写成 `(0,255,0)`（纯绿），且 `quantize()` 输出 8-bit 调色板 BMP，而 `GUI_ReadBmp_RGB_6Color` 仅接受 24-bit BMP。

**解决方案**：
1. 修正 `GUI_BMPfile.c` 中 `GUI_ReadBmp_RGB_6Color` 的颜色匹配条件（Red、Blue、Yellow、Green 四条规则）
2. 修正 `image_processor.py` 调色板：Yellow 改为 `(255,255,0)`；输出改为 `quantized.convert("RGB").save(...)` 输出 24-bit BMP

---

### [BUG-008] SoftAP 配网组件在 ESP-IDF v5.4.3 下编译失败

- **日期**：2026-04-20
- **模块**：firmware / components/softap_prov
- **状态**：已解决

**现象**：
新增 `softap_prov` 组件后，编译报错包含两类问题：
1. `esp_wifi_scan_get_ap_records()` 第一个参数要求 `uint16_t *`，但代码传入的是 `int *`
2. `DOT_PIXEL`、`DOT_STYLE`、`LINE_STYLE`、`DRAW_FILL`、`sFONT`、`EPD_7IN3E_WHITE` 等类型和常量无法识别

**原因**：
1. `softap_prov_ctx_t.ap_count` 定义成了 `int`，与 ESP-IDF v5.4.3 的 Wi-Fi 扫描接口签名不匹配
2. `softap_prov.c` 直接用 `extern` 手写了墨水屏绘图函数声明，但没有包含 `GUI_Paint.h` 和 `epaper_port.h`，组件 `CMakeLists.txt` 里也缺少 `epaper_port` 依赖
3. `Paint_DrawString_EN()` 的本地声明把前景色和背景色参数顺序写反了，会导致后续显示参数传递错误

**解决方案**：
1. 将 `ap_count` 改为 `uint16_t`
2. 在 `softap_prov.c` 中改为正式包含 `GUI_Paint.h` 和 `epaper_port.h`，移除不完整的本地 `extern` 依赖
3. 在 `firmware/components/softap_prov/CMakeLists.txt` 的 `REQUIRES` 中补上 `epaper_port`
4. 按 `GUI_Paint.h` 的真实签名修正 `Paint_DrawString_EN()` 的颜色参数顺序

**参考**：
- `firmware/components/softap_prov/softap_prov.c`
- `firmware/components/softap_prov/CMakeLists.txt`
- `firmware/components/epaper_src/GUI_Paint.h`
- `firmware/components/epaper_port/epaper_port.h`

---

### [BUG-009] 遗留 `ble_prov` 组件在 SoftAP 方案下仍参与构建，导致蓝牙头文件报错

- **日期**：2026-04-20
- **模块**：firmware / components/ble_prov / main / sdkconfig.defaults
- **状态**：已解决

**现象**：
切换到 SoftAP 配网后，编译仍然进入 `ble_prov.c`，并报错：
`fatal error: nimble/nimble_port.h: No such file or directory`

**原因**：
1. 主流程已经切换到 `softap_prov`，但遗留 `ble_prov` 组件仍位于 `firmware/components/` 下，被构建系统扫描
2. 当前 `sdkconfig` 已关闭蓝牙：`CONFIG_BT_ENABLED is not set`
3. `ble_prov/CMakeLists.txt` 仍无条件编译 `ble_prov.c`
4. `sdkconfig.defaults` 里还保留着旧的 NimBLE 默认配置，后续重建配置时容易再次把 BLE 拉回构建链
5. `main/CMakeLists.txt` 里仍保留了 `bt` 依赖，与 SoftAP 现状不一致

**解决方案**：
1. 将 `ble_prov` 组件改为仅在 `CONFIG_BT_ENABLED` 且 `CONFIG_BT_NIMBLE_ENABLED` 时才编译源码
2. 在蓝牙关闭时将 `ble_prov` 注册为空组件，避免旧文件阻塞当前 SoftAP 构建
3. 从 `main/CMakeLists.txt` 中移除不再需要的 `bt` 依赖
4. 从 `sdkconfig.defaults` 中移除旧 BLE/NimBLE 默认配置，避免后续 `fullclean` 或重配后问题复发

**参考**：
- `firmware/components/ble_prov/CMakeLists.txt`
- `firmware/main/CMakeLists.txt`
- `firmware/sdkconfig.defaults`
- `firmware/main/main.c`

---

### [BUG-010] SoftAP 启动阶段 `main` 任务栈溢出，导致热点未广播即崩溃

- **日期**：2026-04-20
- **模块**：firmware / main / softap_prov / sdkconfig
- **状态**：已解决

**现象**：
设备检测到 `config.txt` 缺失后，确实进入了 SoftAP 配网路径，但串口在 Wi-Fi 协议栈初始化阶段很快出现：
`***ERROR*** A stack overflow in task main has been detected.`

随后设备 panic，手机侧也看不到 `SmartFrame-XXXX` 热点。

**原因**：
1. 当前 `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 仅为 `3584`
2. `enter_softap_prov_mode()` 在 `main` 任务栈上同时保留了较大的局部对象，例如 `frame_config_t` 和 `bind_code_result_t`
3. 在主任务栈本就偏小的情况下，再叠加 `esp_wifi_init()` / `esp_wifi_start()` 的调用深度，最终在 SoftAP 启动阶段触发栈溢出

**解决方案**：
1. 将 `enter_softap_prov_mode()` 中的大对象从栈上改为堆分配，减少主任务栈压力
2. 将 `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 从 `3584` 提高到 `8192`
3. 保留前一轮对 SoftAP 纯 `AP` 模式的调整，确保修复栈问题后热点可以正常广播

**参考**：
- `firmware/main/main.c`
- `firmware/sdkconfig`
- `firmware/sdkconfig.defaults`
- `firmware/components/softap_prov/softap_prov.c`

---

### [BUG-011] SoftAP 页面可访问，但 Wi-Fi 列表扫描在纯 AP 模式下失败

- **日期**：2026-04-20
- **模块**：firmware / components/softap_prov
- **状态**：已解决

**现象**：
设备已经能启动热点并访问配网页面，但点击“刷新 Wi-Fi 列表”后接口返回 `500`，串口日志显示：
`Scan start failed: ESP_FAIL`

**原因**：
为了解决前一轮热点不可见问题，SoftAP 启动模式被调整为纯 `WIFI_MODE_AP`。但 ESP-IDF 的 `esp_wifi_scan_start()` 只支持在 `WIFI_MODE_STA` 或 `WIFI_MODE_APSTA` 下执行，因此在纯 AP 模式下调用扫描会直接失败。

**解决方案**：
1. 保持设备启动阶段先用纯 `AP` 模式，优先确保热点稳定广播
2. 当用户在网页中触发扫描时，先动态将 Wi-Fi 模式切换为 `WIFI_MODE_APSTA`
3. 再调用 `esp_wifi_scan_start()` 扫描附近 Wi-Fi，兼顾“热点可见”和“网页扫描可用”
4. 扫描失败路径补充状态复位，避免页面一直停留在 scanning 状态

**参考**：
- `firmware/components/softap_prov/softap_prov.c`
- `C:/esp/v5.4.3/esp-idf/components/esp_wifi/include/esp_wifi.h`

### [BUG-012] SoftAP 提交后浏览器报 `Failed to fetch`，且 MQTT 自定义参数没有真正写入配置

- **日期**：2026-04-20
- **模块**：firmware / components/softap_prov / main
- **状态**：已解决

**现象**：
1. 手机已连上设备热点，网页点击“连接 Wi-Fi”后浏览器直接提示 `Failed to fetch`
2. 页面里填写了 MQTT broker / 用户名 / 密码，但设备保存到 `config.txt` 的仍然是固件里写死的默认值

**原因**：
1. `/api/connect` 返回成功响应后，主流程马上收到凭据并切换到 STA 模式，SoftAP 和 HTTP 连接过早断开，浏览器把这次请求当成网络失败
2. 前端虽然提交了 `mqtt_url` / `mqtt_user` / `mqtt_pass`，但 `softap_prov_credentials_t` 和 `connect_handler()` 只接收 `ssid/password`，自定义 MQTT 参数在固件入口被直接丢弃

**解决方案**：
1. 在 `httpd_resp_send()` 成功后短暂保留 SoftAP，再触发 Wi-Fi 连接流程，避免浏览器过早断线
2. 扩展 `softap_prov_credentials_t`，完整接收并传递自定义 MQTT 配置
3. 调整配网页面交互：默认使用内置 MQTT，客户只有在切到“自定义 MQTT”时才覆盖默认值
4. 放宽 `wifi_sta` 的认证门槛为 `WIFI_AUTH_OPEN`，提升开放热点和混合认证热点的兼容性

**参考**：
- `firmware/components/softap_prov/softap_prov.h`
- `firmware/components/softap_prov/softap_prov.c`
- `firmware/components/softap_prov/html/index.html`
- `firmware/main/main.c`
- `firmware/main/wifi_sta.c`

### [BUG-013] SoftAP 配网状态页在墨水屏上出现黑条，原因是中文文案走了英文字体渲染链路

- **日期**：2026-04-20
- **模块**：firmware / components/softap_prov
- **状态**：已解决

**现象**：
墨水屏在 SoftAP 配网阶段虽然能显示热点名和 IP，但中文提示区域出现两行黑条或异常块状内容。

**原因**：
`softap_prov_draw_screen()` 中的状态文案包含中文 UTF-8 字节流，但实际调用的是 `Paint_DrawString_EN()` + `Font24` 英文字体。英文点阵库无法正确解释多字节中文，最终把这些字节当作连续 ASCII 点阵绘制成黑条。

**解决方案**：
将 SoftAP 配网阶段的墨水屏状态文案统一改成 ASCII 英文，继续复用现有 `Paint_DrawString_EN()` 渲染链路，避免新增未覆盖字符集的中文点阵表。

**参考**：
- `firmware/components/softap_prov/softap_prov.c`
- `firmware/components/epaper_src/GUI_Paint.h`

### [BUG-014] SoftAP 成功关联路由器后 `dhcp client start failed` 并触发 `InstrFetchProhibited` panic

- **日期**：2026-04-20
- **模块**：firmware / main / wifi_sta / softap_prov
- **状态**：已解决

**现象**：
SoftAP 页面提交正确的 Wi-Fi 名称和密码后，串口日志显示设备已成功关联路由器：
`connected with <ssid>`  
但紧接着出现：
`esp_netif_lwip: dhcp client start failed`
随后触发 `Guru Meditation Error: InstrFetchProhibited`，回溯落在 `esp_netif_receive()`。

**原因**：
原实现试图在 `softap_prov` 已初始化并运行中的 Wi-Fi 驱动上，直接从 SoftAP/APSTA 模式切到 STA 模式，并复用 `wifi_sta_connect_after_softap()` 继续联网。  
在该路径下，STA 对应的 `esp_netif` / LWIP 输入回调没有稳定完成官方默认初始化链路，导致：
1. DHCP client 启动失败
2. 收到首个 STA 数据包时进入 `esp_netif_receive()`
3. `esp_netif->lwip_input_fn` 为空，最终跳到空地址触发 panic

**解决方案**：
1. 收到配网参数后，先完整执行 `softap_prov_stop()`，彻底停止 HTTP 服务、销毁 AP netif，并反初始化 Wi-Fi
2. 再走正常 `wifi_sta_connect()` 全量 STA 初始化路径，重新创建默认 STA netif、初始化 Wi-Fi、启动 DHCP
3. 连接前保留一次墨水屏 “Connecting Wi-Fi / Please wait...” 提示，避免用户误判设备无响应

**参考**：
- `firmware/main/main.c`
- `firmware/main/wifi_sta.c`
- `C:/esp/v5.4.3/esp-idf/components/esp_netif/lwip/esp_netif_lwip.c`
- `C:/esp/v5.4.3/esp-idf/components/esp_wifi/src/wifi_default.c`

### [BUG-015] 首次配网重启后先显示缓存图，导致待绑定设备在绑定前就刷出本地图片

- **日期**：2026-04-21
- **模块**：firmware / main / mqtt_photo_client
- **状态**：已解决

**现象**：
设备首次 SoftAP 配网成功并重启后，墨水屏会先显示 `current.bmp` 中的缓存图片，再进入 MQTT 联网流程。  
如果 SD 卡里残留了出厂图、调试图或旧用户图片，就会出现“还没绑定，先看到一张本地图片”的错误体验。

**原因**：
`app_main()` 在正常模式下会先执行 `show_cached_photo_or_clear()`，随后才启动 MQTT。  
此前启动流程没有把“首次待绑定”与“已绑定正常显示”分成两个状态：
1. `bind_status = PENDING` 时仍然沿用已绑定设备的开机显示链路
2. MQTT 客户端启动时总会订阅图片 topic，无法做“只收绑定码、不收图片”的待绑定模式

**解决方案**：
1. 启动流程新增“首次待绑定”分支：`bind_status = PENDING/EXPIRED` 时先显示绑定码或等待绑定界面，不显示缓存图
2. MQTT 客户端新增 `subscribe_photo_topic` 开关，待绑定阶段仅订阅 `device/<uid>/bound`，继续接收动态绑定码并上报注册，不订阅图片 topic
3. 待绑定阶段定时请求 `/api/status/{device_id}`，确认云端 `bound=true` 后将本地状态写成 `BIND_STATUS_BOUND`，短暂提示后自动重启进入图片模式
4. 进入首次待绑定态时主动作废 `/sdcard/current.bmp`，避免旧图在绑定完成后的下一次上电被误当成有效开机图

**参考**：
- `firmware/main/main.c`
- `firmware/components/mqtt_photo_client/mqtt_photo_client.c`
- `firmware/components/mqtt_photo_client/mqtt_photo_client.h`

---

### [BUG-016] 小图切换后未清空显示缓冲区，导致墨水屏残留上一张图片

- **日期**：2026-04-23
- **模块**：firmware / main / epaper display
- **状态**：已解决

**现象**：当上一张图片只占屏幕的一部分时，下一张图片刷新后，墨水屏会同时残留上一张和当前图片的内容。  
**原因**：`display_photo()` 复用了全局显示缓冲区 `s_img_buf`，但在解码新 BMP 前没有先清空缓冲区。`GUI_ReadBmp_RGB_6Color()` 只会覆盖新图片实际占用的像素区域，未覆盖区域会保留上一帧数据，整帧送屏后就出现“两个图片同时显示”的现象。  
**解决方案**：在 `firmware/main/main.c` 的 `display_photo()` 中，调用 `GUI_ReadBmp_RGB_6Color()` 前先执行 `Paint_Clear(EPD_7IN3E_WHITE)`，将显示缓冲区恢复成白底，再绘制新图并正常刷新。这样可以消除残留，同时避免额外增加一次硬件整屏刷新。  
**参考**：`firmware/main/main.c`

---

### [BUG-017] MQTT 首绑与发图链路未闭环，导致设备卡在绑定码界面或上传后不刷新

- **日期**：2026-04-23
- **模块**：firmware / mqtt_photo_client / main / server
- **状态**：已解决

**现象**：设备首绑成功后，墨水屏仍停留在绑定码界面；小程序上传新图片后，服务端文件已更新，但设备不主动刷新。部分场景下串口还会出现 `esp-tls: select() timeout`、`HTTP_CLIENT: Connection failed`。  
**原因**：当前仓库里 MQTT 链路只做到了“设备侧可接收图片通知”，但服务端 `/api/upload` 并没有发布图片刷新消息；同时设备退出首绑页面主要依赖 HTTP `/api/status/{device_id}` 轮询，缺少 MQTT 的绑定成功通知兜底。再叠加配置里通常只写了 `mqtt_broker_url`，未写 `server_url`，如果外部系统下发的是 `https://` 图片地址，设备就会走 TLS 连接并在目标站点不可达或证书链不匹配时超时。  
**解决方案**：  
1. 服务端在 `/api/bind` 成功后，向 `device/{device_id}/bound` 发布 `device_bound` 事件。  
2. 固件在 `mqtt_photo_client` 中新增对 `device_bound` 的识别，收到后立即将 `bind_status` 持久化为 `BOUND`，刷新提示页并重启进入照片模式。  
3. 服务端在 `/api/upload` 成功后，向 `device/{device_id}/image` 发布绝对 HTTP 地址 `http://.../api/photo/{device_id}/latest.bmp`，让设备无需依赖 `server_url` 本地配置，也避免优先走 `https` 下载。  

**参考**：
- `firmware/components/mqtt_photo_client/mqtt_photo_client.c`
- `firmware/components/mqtt_photo_client/mqtt_photo_client.h`
- `firmware/main/main.c`
- `server/main.py`

---

### [BUG-018] 首绑等待阶段持续依赖 HTTP 状态轮询，导致 MQTT 方案下仍反复报连接超时

- **日期**：2026-04-23
- **模块**：firmware / main
- **状态**：已解决

**现象**：设备已经进入 MQTT 首绑模式并订阅了 `device/{device_id}/bound`，但在等待绑定完成时仍周期性打印 `Bind status request failed: ESP_ERR_HTTP_CONNECT`、`esp-tls: select() timeout`，同时停留在绑定码页面。  
**原因**：固件虽然已经支持通过 MQTT `device_bound` 事件退出绑定页，但 `wait_for_bind_completion()` 仍默认执行 HTTP `/api/status/{device_id}` 轮询。这样在 MQTT 方案下，固件一边等 MQTT，一边继续做无意义的 HTTP 状态查询，既带来误导日志，也会在服务端地址不可达或被错误引到 HTTPS 时反复超时。  
**解决方案**：当设备存在 MQTT 配置并处于首绑等待态时，改为只等待 MQTT `device_bound` 事件，不再执行 HTTP 绑定状态轮询；仅在未配置 MQTT 的场景下保留原有 HTTP 轮询兜底。  

**参考**：
- `firmware/main/main.c`

---

### [BUG-019] 服务端未消费设备侧 `reg_new_device` 握手消息，导致纯 MQTT 首绑等待态无法自恢复

- **日期**：2026-04-23
- **模块**：server / MQTT bind
- **状态**：已解决

**现象**：设备进入首绑等待态后，日志会显示已订阅 `device/{device_id}/bound` 并成功发布 `reg_new_device`，随后只收到自己回环的消息并打印 `Ignoring echoed reg_new_device payload on bound topic`，但始终收不到 `device_bound`，墨水屏卡在绑定码界面。  
**原因**：固件侧已经在连接 MQTT 后向 `device/{device_id}/bound` 发布 `reg_new_device` 作为握手信号，但服务端 MQTT 线程只订阅了 `smartframe/bind/request`，没有订阅 `device/+/bound`，因此不会在“设备已上线且数据库里已经绑定”的场景下主动回发 `device_bound`。  
**解决方案**：服务端新增订阅 `device/+/bound`，收到 `reg_new_device` 后查询 `devices.json`：如果该设备已经存在且 `owner_openid` 非空，就立刻向同一 topic 发布 `device_bound`；同时把 `device_bound` 改为 retained 消息，避免设备在订阅时序上偶发错过。  

**参考**：
- `server/main.py`

---

### [BUG-020] 外部平台只下发图片 topic，不发送 `device_bound`，导致首绑等待态永远卡在绑定码界面

- **日期**：2026-04-23
- **模块**：firmware / main / mqtt bind flow
- **状态**：已解决

**现象**：设备在首绑等待态时已经成功连接 MQTT，但只收到 `device/{device_id}/image` 上的图片消息，不会收到 `device/{device_id}/bound` 上的 `device_bound`。结果设备一直停留在绑定码界面，无法进入照片模式。  
**原因**：仓库内原始设计假设“绑定完成”与“图片下发”是两条独立事件：先收到 `device_bound`，再订阅图片 topic。但实际线上平台已经把“开始下发图片”作为设备可用的事实信号，却没有单独发送 `device_bound`。固件若在首绑阶段关闭图片 topic 订阅，就会错过这条唯一有效信号。  
**解决方案**：在首绑等待阶段也订阅图片 topic；只要设备在等待绑定时成功收到并显示第一张图片，就将其视为隐式绑定完成，持久化 `BIND_STATUS_BOUND`，唤醒等待流程并进入后续照片模式。  

**参考**：
- `firmware/main/main.c`

---

### [BUG-021] MQTT 直播与轮播消息互抢导致电子墨水屏连续刷新

- **日期**：2026-04-28
- **模块**：firmware / main / MQTT photo display
- **状态**：已修复，待实机验证

**现象**：设备收到图片后，电子墨水屏刚出现图片轮廓就立即进入下一次刷新，无法稳定显示完整一张照片。

**原因**：直播模式下收到 playlist/slideshow 消息时，`on_playlist_ready()` 会直接把 `photo_mode` 改成 `PHOTO_MODE_SLIDESHOW` 并唤醒轮播任务；如果云端同时下发直播图片和轮播消息，两条显示链路会交替触发刷新。另一方面，`display_photo()` 对连续刷新没有最小停留时间保护，在短时间多次收到图片通知时会立即开始下一次整屏刷新。

**解决方案**：
1. 当本地配置为 `PHOTO_MODE_MQTT_LIVE` 时，忽略 playlist/slideshow 显示请求，不再把设备强行切换到轮播模式。
2. 在 `display_photo()` 中加入 20 秒最小照片停留时间，避免上一张图刚显影就被下一次刷新打断。
3. 直播模式下收到 playlist 仍可作为云端送达信号，用于完成首次绑定状态持久化，但不触发显示刷新。

**参考**：
- `firmware/main/main.c`

---

### [BUG-022] 480x800 竖图被按原尺寸写屏导致只显示一半

- **日期**：2026-04-28
- **模块**：firmware / components/epaper_src / BMP display
- **状态**：已修复，待实机验证

**现象**：上传的 BMP 为 `480x800` 竖图时，7.3 寸墨水屏只显示其中一半内容。

**原因**：屏幕逻辑尺寸是 `800x480`，`GUI_ReadBmp_RGB_6Color()` 原先按 BMP 原始宽高直接写入显示缓冲区。竖图高度 800 超出屏幕高度 480，超出部分被边界检查丢弃。

**解决方案**：在 6 色 BMP 解码时检测 `480x800` 这类宽高与屏幕相反的图片，自动旋转为横屏坐标后再调用 `Paint_SetPixel()`，横屏 `800x480` 图片继续走原有路径。

**参考**：
- `firmware/components/epaper_src/GUI_BMPfile.c`

---

### [BUG-023] MQTT 缺少设备在线心跳与公网 IP 上报

- **日期**：2026-04-29
- **模块**：firmware / components/mqtt_photo_client
- **状态**：已修复，待实机验证

**现象**：云平台需要通过 `device/<DEVICE_UID>/beat` 接收设备在线心跳，但固件当前只在 MQTT 连接后发布 `reg_new_device`，没有按心跳规范持续上报在线状态和 IP 地址。

**原因**：`mqtt_photo_client` 只实现了绑定 topic 和图片 topic，缺少独立的 beat topic、周期性心跳任务和公网出口 IP 获取逻辑。

**解决方案**：
1. 新增 `device/<DEVICE_UID>/beat` topic，MQTT 连接成功后立即发布一次心跳。
2. 新增 60 秒周期心跳任务，payload 使用 `event=heart_beat`、`device_uid`、`ip_address`、`timestamp`。
3. 心跳发送前通过公网 IP 查询接口获取当前 Wi-Fi 出口公网 IP。
4. 不再回退上报 STA 内网 IP；公网 IP 获取失败时跳过本次心跳，避免云端地理位置被覆盖为“内网地址”。

**参考**：
- `firmware/components/mqtt_photo_client/mqtt_photo_client.c`
- `firmware/components/mqtt_photo_client/CMakeLists.txt`
- `12-设备心跳MQTT数据规范-2026-04-28.md`

---

### [BUG-024] 双模式切换后本地定时图库可能为空，且连续刷新保护未真正生效

- **日期**：2026-05-13
- **模块**：firmware / main / dual photo mode
- **状态**：已修复，待实机验证

**现象**：设备已经具备 `PHOTO_MODE_MQTT_LIVE` 和 `PHOTO_MODE_LOCAL_TIMED` 两个显示状态，但从 MQTT 直播切到本地定时模式后，`/sdcard/local_photos` 可能没有可轮播照片；同时历史问题记录里提到的 20 秒最小停留时间在当前 `display_photo()` 实现中缺失，短时间多次图片通知仍可能触发连续整屏刷新。实机日志还出现 `Failed to create local photo dir: /sdcard/local_photos errno=22`。

**原因**：`on_photo_ready()` 过去只在本地定时模式下把 `/sdcard/current.bmp` 复制到本地图库，直播模式下新图只刷新屏幕，不进入 `/sdcard/local_photos`。因此本地定时模式依赖手动预置文件或切换后再收到新图，模式间数据来源没有完全解耦。另一个问题是 `display_photo()` 没有记录上次刷新 tick，也没有按最小停留时间延后下一次刷新。当前固件配置为 `CONFIG_FATFS_LFN_NONE=y`，未启用长文件名，`local_photos` 和 `photo_00000001.bmp` 都不符合 FATFS 8.3 短文件名限制，会导致目录或文件创建失败。

**解决方案**：MQTT 图片下载成功后统一缓存到 `/sdcard/LOCAL`，直播模式继续即时显示，本地定时模式只缓存不抢占当前显示；同时在 `display_photo()` 内加入 20 秒最小停留保护，并继续使用同一个显示互斥锁串行刷新。本地图库落盘时使用 `P0000001.BMP` 这类 8.3 短文件名并递增查找未占用文件名，避免设备重启后覆盖旧照片。

**参考**：
- `firmware/main/main.c`
- `codex.md`

---

## 未解决

_暂无_

---

## 待观察

_暂无_
