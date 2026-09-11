# ESP32-S3 蓝牙图片显示器

基于 ESP32-S3 + 3.6 寸 ST7793 并口 TFT 的蓝牙图片显示方案。电脑/手机通过 Web Bluetooth 将图片发送到设备显示，图片持久化保存在 LittleFS 中，断电重启后自动恢复上次的画面。

## 目录结构

```
esp32s3-ble-photo-frame/
├── README.md                        本说明文档
├── image_display/                   PlatformIO 固件工程
│   ├── src/main.cpp                 主程序（屏驱 + BLE + JPEG 解码）
│   ├── platformio.ini               PlatformIO 工程配置
│   ├── backup/
│   │   └── main_st7735s_spi_20260910.cpp.bak   旧 1.8寸 SPI 屏备份
│   └── data/                        LittleFS 数据分区（运行时生成）
└── tools/                           网页端工具
    ├── photo_uploader.html          蓝牙图片上传网页
    └── heic2any.min.js              HEIC/HEIF → JPEG 本地转换库
```

## 硬件清单

| 部件 | 规格 | 备注 |
|---|---|---|
| 主控 | ESP32-S3-DevKitC-1 N16R8 | 16MB Flash + 8MB OPI PSRAM |
| 显示屏 | 3.6 寸 240×400 TFT | ST7793，8 位 8080 并口接口 |
| 蓝牙适配器 | BLE 5.0 USB 适配器（CSR8510 或更新） | Atheros AR3012 等 4.0 适配器兼容性差 |
| 电源 | 5V USB 供电 | 屏背光由 ESP32-S3 3.3V LDO 提供 |

## 接线说明（ST7793 并口屏）

| 屏幕引脚 | ESP32-S3 GPIO | 说明 |
|---|---|---|
| VCC / VDD | 3.3V | — |
| GND | GND | — |
| RST | GPIO16 | 复位 |
| CS  | GPIO15 | 片选 |
| RS / DC / A0 | GPIO14 | 数据/命令选择 |
| WR  | GPIO12 | 写信号 |
| RD  | GPIO13 | 读信号（只写不读可改 -1，屏侧 RD 接 3.3V） |
| D0~D7 | GPIO4 / 5 / 6 / 7 / 8 / 9 / 10 / 11 | 8 位数据线 |
| BL / LED / BLK | GPIO17 | 背光（常亮可改 -1，屏侧 BL 接 3.3V） |

> 引脚选择已避开 N16R8 的 Flash/PSRAM 引脚（GPIO26~37）、strapping 引脚（0/3/45/46）与 USB 引脚（19/20）。
> 屏模块若有 IM0/IM1/IM2 模式选择脚，需按屏厂说明设为 8 位 8080 模式（成品模块通常已固定）。

## 软件依赖

- **PlatformIO**（VSCode 插件）—— 构建烧录
- **LovyanGFX**（`lovyan03/LovyanGFX`）—— 显示驱动，platformio.ini 中自动安装
- **arduino-esp32 框架自带**：BLE (Bluedroid)、LittleFS、TJpgDec
- **浏览器**：Edge / Chrome（需支持 Web Bluetooth）
- **网页端本地库**：`heic2any.min.js`（已放在 `tools/` 目录，处理 iPhone HEIC 照片）

## 构建与烧录

1. **克隆/拷贝工程到纯英文路径**
   由于 PlatformIO 在含中文/空格的路径下会出现链接错误，工程已通过 `e:\piobuild_id` 目录联接（junction）到英文路径编译。打开工程时优先使用英文路径。

2. **确认分区表与 PSRAM 配置**
   [platformio.ini](image_display/platformio.ini) 默认为 16MB Flash + OPI PSRAM：
   ```ini
   board_upload.flash_size = 16MB
   board_build.partitions  = default_16MB.csv
   board_build.arduino.memory_type = qio_opi
   build_flags = -DBOARD_HAS_PSRAM
   ```
   8MB/4MB Flash 模块按文件内注释切换。

3. **烧录**
   在 VSCode 中打开 `image_display/` 文件夹，PlatformIO 会自动识别。点击底部工具栏的 `→` 上传按钮即可编译并烧录。

## 使用方法

1. **打开网页工具**
   双击打开 [tools/photo_uploader.html](tools/photo_uploader.html)（Edge/Chrome）。无需 Web 服务器，本地文件直接运行。

2. **连接设备**
   - 上电后屏幕显示 `ESP32-S3 Photo Display / BLE Ready / ESP32S3-Photo`
   - 网页点 `1. 连接 ESP32-S3`，浏览器弹出设备选择窗口，选择 `ESP32S3-Photo`
   - 连接成功后屏幕切换到 `BLE Connected` 界面

3. **选择并裁剪图片**
   - 点击文件框或拖入图片（支持 JPG/PNG/WebP/BMP/GIF/AVIF/HEIC/HEIF）
   - 裁剪视口固定为 5:3（即屏幕 400×240）
   - 拖动平移、滑块/双指捏合缩放，点 `⟳ 旋转` 可无损旋转 90°
   - 点 `确定裁剪` 自动导出 400×240 标准 baseline JPEG

4. **上传显示**
   - 点 `2. 上传并显示`，网页分包发送，屏幕显示 `Receiving...` 和进度条
   - 传输完成后设备解码并显示，图片同时保存到 LittleFS（断电保留）

5. **清空屏幕**
   - 点 `清空屏幕`，设备清屏并回到 `BLE Connected` 界面

## 通讯协议

BLE GATT 使用自定义 UUID：

| 名称 | UUID | 属性 |
|---|---|---|
| Service | `8f750001-2c3a-4f9b-8e21-6b5d7c9e0a01` | — |
| RX | `8f750002-...` | Write / WriteNoResponse（电脑 → 设备） |
| TX | `8f750003-...` | Notify（设备 → 电脑） |

分包协议（单字节命令字前缀）：

| 命令 | 载荷 | 含义 |
|---|---|---|
| `S` (0x53) | `uint32 LE` 总字节数 | 开始上传会话 |
| `D` (0x44) | 1~176 字节 | 数据包 |
| `F` (0x46) | 无 | 上传结束，触发解码显示 |
| `C` (0x43) | 无 | 清空屏幕 |

设备端收到 `F` 后解码 JPEG，结果通过 TX Notify 返回 `OK` 或 `ERR:...`。
图片大小限制 1.5MB，BLE MTU 协商为 512（每包 176 字节数据 + 1 字节命令）。

## 固件关键实现

### ST7793 自定义驱动
ST7793 属于老式 R61509 指令族（16 位寄存器索引，命令 0x200/0x201/0x202/0x210~0x213），LovyanGFX 无内置驱动。在 [main.cpp](image_display/src/main.cpp#L98-L268) 中通过继承 `lgfx::Panel_LCD` 实现 `Panel_ST7793` 类：

- **初始化序列**：寄存器 0x0001/0x0002/0x0003/0x0400/0x0300~0x0309 等，移植自 MCUFRIEND_kbv 库对 ST7793 的实测代码
- **横屏实现**：`setRotation` 采用 MCUFRIEND_kbv 的 val 表 `{0x48, 0x28, 0x98, 0x88}`（BGR=1），同时设置 AM=1（0x0003 bit3）硬件行列互换 + `setWindow` 中互换 H/V 寄存器地址
- **BGR 色序**：面板出厂为 BGR 8080 接口（`0x0003=0x1030`，BGR bit12=1），`cfg.rgb_order=false`，不可改为 RGB
- **并口总线**：`Bus_Parallel8`，WR 频率 16MHz（20MHz 在面包板/飞线下时序裕量不足会花屏）

### BLE 数据路径
Bluedroid 的写回调运行在 BTC_TASK（栈约 3KB），不能直接做 LittleFS 文件操作。固件使用 16KB 环形缓冲解耦：

```
BLE 回调(生产者)  ─memcpy→  s_ring[16KB]  ─主循环pumpRingToFile→  LittleFS 文件
```

- 回调只置标志 + 数据入环形缓冲
- `loop()` 中消费者把数据写入 `/tmp.jpg`
- 收到 `F` 后排空缓冲 → 关闭文件 → `drawJpgFile` 解码 → 重命名为 `/img.jpg`

### JPEG 解码显示
使用 LovyanGFX 内置 `drawJpgFile`，自动等比缩放 + 居中适配 400×240 区域。仅支持标准 baseline JPEG（TJpgDec 不支持 progressive）。

## 网页端关键实现

[photo_uploader.html](tools/photo_uploader.html) 是单文件应用，无后端依赖：

- **格式兼容**：JPG/PNG/WebP/BMP/GIF/AVIF 原生解码；HEIC/HEIF 用本地 `heic2any.min.js` 转 JPEG
- **EXIF 方向**：`createImageBitmap({imageOrientation:'from-image'})` 自动应用手机照片方向
- **大图缩放**：最大边限制 4096px，避免超过 canvas 面积上限
- **无损旋转**：维护 `workCanvas` 工作画布，旋转只做 canvas 像素转置，避免重复 JPEG 重编码
- **导出**：`toBlob('image/jpeg', 0.92)` 替代 `toDataURL`，避免大图 base64 内存问题；透明区域铺白底

## 已知问题与注意事项

- **蓝牙适配器兼容性**：Qualcomm/Atheros 4.0 适配器（如 AR3012）在 Windows 上与 ESP32-S3 BLE 兼容性差，建议使用 CSR8510 或 BLE 5.0 适配器
- **路径要求**：PlatformIO 工程路径必须全英文，否则链接器报错。当前通过 `e:\piobuild_id` junction 编译
- **屏幕花屏排查**：若出现花屏/彩点，首先检查 WR 频率（降到 16MHz 或更低）；颜色异常用 `fillScreen(RED/GREEN/BLUE)` 纯色测试验证
- **iPhone HEIC**：若 heic2any 转换失败，建议在「设置→相机→格式」选「兼容性最好」后重存为 JPG
- **断电恢复**：图片以 `/img.jpg` 保存在 LittleFS，开机自动恢复。上传过程先写 `/tmp.jpg`，解码成功后原子重命名，避免半截图片覆盖旧图

## 屏幕驱动切换

[main.cpp](image_display/src/main.cpp#L49-L54) 顶部 `PANEL_TYPE` 宏控制：

| 值 | 驱动 | 适用屏幕 |
|---|---|---|
| 1 | ST7735S | 1.8 寸 128×160 SPI |
| 2 | ST7789  | 2.0/2.4 寸 240×320 SPI |
| 3 | ILI9341 | 2.4 寸 240×320 SPI |
| 4 | ST7793  | 3.6 寸 240×400 并口（当前） |

切换时同时需要修改对应的引脚 `#define`（SPI 屏用 `PIN_SCLK/PIN_MOSI/...`，并口屏用 `PIN_P_D0~PIN_P_BL`）。旧 ST7735S SPI 版本备份在 [backup/main_st7735s_spi_20260910.cpp.bak](image_display/backup/main_st7735s_spi_20260910.cpp.bak)。
