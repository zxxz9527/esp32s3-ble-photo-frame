# ESP32-S3 蓝牙图片显示器

基于 ESP32-S3 + 3.6 寸 ST7793 并口 TFT 的蓝牙图片显示方案。电脑/手机通过 Web Bluetooth 将图片发送到设备显示，图片持久化保存在 LittleFS 中，断电重启后自动恢复上次的画面。

## 目录结构

```
图片展示/
├── README.md                        本说明文档
├── image_display/                   PlatformIO 固件工程
│   ├── src/main.cpp                 主程序（屏驱 + BLE + JPEG 解码 + 低功耗）
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
| 按键（可选） | 轻触按键 + 10kΩ 上拉电阻 | 低功耗模式切换蓝牙用，接 GPIO1 |
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

## 接线说明（低功耗按键）

| 按键一端 | 按键另一端 | 说明 |
|---|---|---|
| GPIO1 | GND | 按下时拉低，内部上拉已启用 |

> GPIO1 不是 strapping 引脚，启动时可安全使用。若想改用板载 BOOT 键，改 `PIN_BTN` 为 0 即可，但 GPIO0 是 strapping 引脚，启动时不能被外部电路强下拉。

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

2. **启用蓝牙**
   - 上电后屏幕显示上次的图片（若有）或 `BLE OFF / press button to enable BLE` 提示
   - **按一下 GPIO1 按键**，屏幕弹出 `BLE ON` 绿色 Toast 小窗口（1 秒后消失），蓝牙开始广播
   - 再次按下 → 弹出 `BLE OFF` 红色 Toast → 蓝牙关闭省电

3. **连接设备**
   - 网页点 `1. 连接 ESP32-S3`，浏览器弹出设备选择窗口，选择 `ESP32S3-Photo`
   - 连接成功后屏幕切换到 `BLE Connected` 界面

4. **选择并裁剪图片**
   - 点击文件框或拖入图片（支持 JPG/PNG/WebP/BMP/GIF/AVIF/HEIC/HEIF）
   - 裁剪视口固定为 5:3（即屏幕 400×240）
   - 拖动平移、滑块/双指捏合缩放，点 `⟳ 旋转` 可无损旋转 90°
   - 点 `确定裁剪` 自动导出 400×240 高质量 JPEG（quality 0.98）

5. **上传显示**
   - 点 `2. 上传并显示`，网页分包发送，屏幕显示 `Receiving...` 和进度条
   - 传输完成后设备解码、软件 Gamma 校正并显示，图片同时保存到 LittleFS（断电保留）

6. **清空屏幕**
   - 点 `清空屏幕`，设备清屏并回到 `BLE Connected` 界面

## 低功耗模式

蓝牙持续广播是发热主因之一。固件支持按键开关蓝牙：

| 状态 | CPU 频率 | BLE | 功耗 |
|---|---|---|---|
| 开机默认 | 80MHz | 关闭 | 最低（仅屏幕刷新） |
| 按键启用 | 80MHz | 广播 + 可连接 | 较高（可上传图片） |
| 传输期间 | 80MHz | 活跃 | 最高 |
| 按键关闭 | 80MHz | `deinit` 释放协议栈 | 低 |

- 开机默认蓝牙关闭，CPU 降频到 80MHz（ESP32-S3 蓝牙最低稳定频率）
- 空闲时主循环 `delay(50)`，让 CPU 大量时间在 idle task
- 传输期间自动切换到 `delay(1)` 紧凑轮询，防止环形缓冲溢出
- 并口屏由 LCD_CAM 硬件外设独立驱动，CPU 降频不影响显示

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
ST7793 属于老式 R61509 指令族（16 位寄存器索引，命令 0x200/0x201/0x202/0x210~0x213），LovyanGFX 无内置驱动。在 `Panel_ST7793` 类中实现：

- **初始化序列**：寄存器 0x0001/0x0002/0x0003/0x0400 等，移植自 MCUFRIEND_kbv 库对 ST7793 的实测代码
- **横屏实现**：`setRotation` 采用 MCUFRIEND_kbv 的 val 表 `{0x48, 0x28, 0x98, 0x88}`（BGR=1），同时设置 AM=1（0x0003 bit3）硬件行列互换 + `setWindow` 中互换 H/V 寄存器地址
- **BGR 色序**：面板出厂为 BGR 8080 接口（`0x0003=0x1030`，BGR bit12=1），`cfg.rgb_order=false`，不可改为 RGB
- **并口总线**：`Bus_Parallel8`，WR 频率 16MHz（20MHz 在面包板/飞线下时序裕量不足会花屏）
- **Gamma**：跳过面板 Gamma 寄存器写入，色彩层次由软件 Gamma 校正处理

### BLE 数据路径
Bluedroid 的写回调运行在 BTC_TASK（栈约 3KB），不能直接做 LittleFS 文件操作。固件使用 **64KB 非阻塞环形缓冲** 解耦：

```
BLE 回调(生产者)  ─memcpy→  s_ring[64KB]  ─主循环pumpRingToFile→  LittleFS 文件
```

- 回调只置标志 + 数据入环形缓冲，**永不阻塞**
- `loop()` 中消费者把数据写入 `/tmp.jpg`
- 收到 `F` 后排空缓冲 → 关闭文件 → 解码显示 → 重命名为 `/img.jpg`

> **关键设计**：早期版本环形缓冲为 16KB 且满时 busy-wait 等待主循环消费。由于 BLE 写回调在 BTC_TASK（栈仅 ~3KB）中执行，一旦阻塞将导致协议栈无法发出 Write Response，网页端 `writeValueWithResponse` 超时报 `GATT operation failed for unknown reason` 并断开。现改为：
> - 缓冲扩至 64KB，吸收主循环偶发卡顿（进度条重绘、`LittleFS.remove` 等）
> - `ringPush` 非阻塞：缓冲满时直接置 `s_overflow` 标志并丢弃后续包，回调秒返回
> - `loop()` 收到 `F` 后检查 `s_overflow`，若溢出则丢弃文件并通知网页端重试
> - 进度条 5% 步进重绘、主循环自适应延时（传输 1ms / 空闲 50ms）

### JPEG 解码与软件 Gamma 校正
RGB565 面板出厂模拟 Gamma 往往让高光区扁平（浅色和白色分不清）。固件在数字层面做 Gamma 1.5 校正：

1. **PSRAM Sprite 解码**：JPEG 不解码到屏幕，先解码到 PSRAM 中 400×240 的 RGB565 sprite（192KB，8MB OPI PSRAM 足够）
2. **查表校正**：对每个像素做 `new = pow(old/max, 1.5) * max`，让接近满值的浅色被压低，与纯白色拉开层次
3. **字节序注意**：LovyanGFX sprite buffer 以**大端序**存储 RGB565，但 ESP32 读 `uint16_t` 是小端序。每像素必须先 `__builtin_bswap16` 交换字节后再提取 R/G/B 通道，校正后再 bswap 写回。遗漏此步会导致 R/G/B 通道错位（典型表现：肤色变绿）
4. **推送到屏幕**：校正后 `pushSprite(0, 0)`

Gamma 值可调：`GAMMA_VAL` 默认 1.5，越大高光层次越丰富但整体越暗。建议范围 1.3~2.0。

### 低功耗按键控制
- GPIO1 配 `INPUT_PULLUP`，对地触发
- 按键扫描带 50ms 去抖 + 释放等待
- 切换 BLE 启用状态后弹出 Toast 小窗口提示
- Toast 1 秒后自动消失，重绘底层图片或 BOOT 界面

### Toast 弹窗
屏幕中央 260×60 半透明圆角矩形 + 一行文字提示。不 fillScreen（不破坏底层图片），1 秒后由主循环检查过期时间戳触发恢复重绘。

## 网页端关键实现

[photo_uploader.html](tools/photo_uploader.html) 是单文件应用，无后端依赖：

- **格式兼容**：JPG/PNG/WebP/BMP/GIF/AVIF 原生解码；HEIC/HEIF 用本地 `heic2any.min.js` 转 JPEG
- **EXIF 方向**：`createImageBitmap({imageOrientation:'from-image'})` 自动应用手机照片方向
- **大图缩放**：最大边限制 4096px，避免超过 canvas 面积上限
- **无损旋转**：维护 `workCanvas` 工作画布，旋转只做 canvas 像素转置，避免重复 JPEG 重编码
- **导出**：`toBlob('image/jpeg', 0.98)` 高质量导出，减少浅色色度通道压缩；透明区域铺白底

## 已知问题与注意事项

- **上传时 GATT 断开（`GATT operation failed for unknown reason`）**：早期固件的 BLE 回调在环形缓冲满时 busy-wait，导致协议栈发不出 Write Response 而被网页端超时断开。当前版本已改为 64KB 非阻塞缓冲 + 溢出丢弃重试机制，正常情况下不会再出现。若仍断开，优先检查蓝牙适配器（见下条）
- **蓝牙适配器兼容性**：Qualcomm/Atheros 4.0 适配器（如 AR3012）在 Windows 上与 ESP32-S3 BLE 兼容性差，建议使用 CSR8510 或 BLE 5.0 适配器
- **路径要求**：PlatformIO 工程路径必须全英文，否则链接器报错。当前通过 `e:\piobuild_id` junction 编译
- **屏幕花屏排查**：若出现花屏/彩点，首先检查 WR 频率（降到 16MHz 或更低）；颜色异常用 `fillScreen(RED/GREEN/BLUE)` 纯色测试验证
- **屏幕颜色偏淡**：面板出厂 Gamma 可能压缩高光区层次。固件已做软件 Gamma 1.5 校正，若仍偏淡可增大 `GAMMA_VAL`（最高 2.0）或降低 VCM 值（`0x0280` 寄存器，试 `0x6080`/`0x6040`）。若肤色变绿，说明 Gamma 校正的字节序处理有问题，检查 `__builtin_bswap16` 是否双向应用
- **iPhone HEIC**：若 heic2any 转换失败，建议在「设置→相机→格式」选「兼容性最好」后重存为 JPG
- **断电恢复**：图片以 `/img.jpg` 保存在 LittleFS，开机自动恢复。上传过程先写 `/tmp.jpg`，解码成功后原子重命名，避免半截图片覆盖旧图
- **BOOT 按键注意**：若用 GPIO0（板载 BOOT 键）作为按键，该引脚是 strapping 引脚，启动时会被外部上拉/下拉影响启动模式。建议用 GPIO1 等非 strapping 引脚

## 屏幕驱动切换

`main.cpp` 顶部 `PANEL_TYPE` 宏控制：

| 值 | 驱动 | 适用屏幕 |
|---|---|---|
| 1 | ST7735S | 1.8 寸 128×160 SPI |
| 2 | ST7789  | 2.0/2.4 寸 240×320 SPI |
| 3 | ILI9341 | 2.4 寸 240×320 SPI |
| 4 | ST7793  | 3.6 寸 240×400 并口（当前） |

切换时同时需要修改对应的引脚 `#define`（SPI 屏用 `PIN_SCLK/PIN_MOSI/...`，并口屏用 `PIN_P_D0~PIN_P_BL`）。旧 ST7735S SPI 版本备份在 [backup/main_st7735s_spi_20260910.cpp.bak](image_display/backup/main_st7735s_spi_20260910.cpp.bak)。
