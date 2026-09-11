/*
  ============================================================
   ESP32-S3 蓝牙图片显示器 (BLE Photo Display)  -  PlatformIO
  ------------------------------------------------------------
   电脑 (Edge/Chrome) 打开工程外 tools/photo_uploader.html
   -> 蓝牙连接本设备 -> 选择 JPG 上传 -> 屏幕立即显示
   -> 图片保存在 LittleFS，断电重启后自动恢复

   说明: ESP32-S3 仅支持 BLE 低功耗蓝牙 (不支持经典蓝牙)，
         电脑需要有蓝牙功能 (Win10/11 自带，无需装驱动)。

   硬件: ESP32-S3 + 3.6寸 240x400 TFT 彩屏 (ST7793, 8位8080并口)
         横屏使用，分辨率 400x240

   接线 (PANEL_TYPE = 4, ST7793 并口屏, 引脚可在下方"用户配置区"修改):
     屏幕引脚        ESP32-S3
     VCC / VDD   ->  3.3V
     GND         ->  GND
     RST         ->  GPIO16
     CS          ->  GPIO15
     RS (DC/A0)  ->  GPIO14
     WR          ->  GPIO12
     RD          ->  GPIO13  (也可直接接 3.3V，此时 PIN_P_RD 改 -1)
     D0~D7       ->  GPIO4 / 5 / 6 / 7 / 8 / 9 / 10 / 11
     BL/LED/BLK  ->  GPIO17 (或直接接 3.3V 常亮，PIN_P_BL 改 -1)
   注: 并口屏若有 IM0/IM1/IM2 模式选择脚，需按屏厂说明设为 8位8080 模式
       (常见成品模块已在板上固定，无需处理)。

   SPI 旧屏 (PANEL_TYPE = 1/2/3) 接线:
     SCL/SCK/CLK -> GPIO12   SDA/MOSI -> GPIO11   DC/RS -> GPIO9
     CS -> GPIO10            RST -> GPIO8         BL -> GPIO7
  ============================================================
*/

#include <Arduino.h>
#include <LittleFS.h>
// BLE (Bluedroid，arduino-esp32 框架自带，无需额外安装库)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
// 显示
#include <LovyanGFX.hpp>

// ==================== 用户配置区 ====================
// 蓝牙广播名称 (电脑上看到的设备名)
#define BLE_DEVICE_NAME "ESP32S3-Photo"

// 屏幕驱动型号
//    1 = ST7735S  -> 1.8寸 128x160 SPI 屏
//    2 = ST7789   -> 2.0/2.4寸 240x320 SPI 屏
//    3 = ILI9341  -> 2.4寸 240x320 SPI 屏
//    4 = ST7793   -> 3.6寸 240x400 8位8080并口屏 (RST/CS/RS/WR/RD/D0-D7) 【当前】
#define PANEL_TYPE 4

// ---- SPI 屏引脚 (PANEL_TYPE 1/2/3 使用) ----
#define PIN_SCLK  12
#define PIN_MOSI  11
#define PIN_DC     9
#define PIN_CS    10
#define PIN_RST    8
#define PIN_BL     7     // 背光若直接接 3.3V，这里改为 -1

// ---- ST7793 8位并口引脚 (PANEL_TYPE 4 使用) ----
// 全部选用 GPIO4~17：避开 N16R8 的 Flash/PSRAM 引脚(GPIO26~37)、
// strapping 引脚(0/3/45/46) 与 USB(19/20)。
#define PIN_P_D0   4
#define PIN_P_D1   5
#define PIN_P_D2   6
#define PIN_P_D3   7
#define PIN_P_D4   8
#define PIN_P_D5   9
#define PIN_P_D6  10
#define PIN_P_D7  11
#define PIN_P_WR  12    // 屏 WR 写信号
#define PIN_P_RD  13    // 屏 RD 读信号 (只写不读可改 -1，屏侧 RD 接 3.3V)
#define PIN_P_RS  14    // 屏 RS/DC/A0 数据命令选择
#define PIN_P_CS  15    // 片选
#define PIN_P_RST 16    // 复位
#define PIN_P_BL  17    // 背光 (直接接 3.3V 常亮则改 -1)

// ---- 按键 (低功耗切换) ----
// 使用 GPIO1 (对地触发, 需外部上拉 10kΩ 或用 INPUT_PULLUP)。
//   注: GPIO1 非 strapping/Flash/USB 引脚, 可自由使用。
//   若想用板载 BOOT 键可改回 0 (GPIO0 是 strapping, 启动时不能被外部强下拉)。
// 按一下: 启用蓝牙广播(可上传图片); 再按一下: 关闭蓝牙(省电)。
// 启动时蓝牙默认关闭以降低功耗, 屏幕显示图片或提示。
// 如改用其他 GPIO，确保避开 strapping(0/3/45/46)、Flash/PSRAM(26-37)、USB(19/20)。
#define PIN_BTN       1
#define BTN_DEBOUNCE_MS 50
// ===================================================

#define IMG_PATH        "/img.jpg"   // 持久保存的当前图片 (断电保留)
#define TMP_PATH        "/tmp.jpg"   // 上传时的临时文件
#define MAX_UPLOAD_BYTES (1500UL * 1024UL)  // 上传大小限制 1.5MB

// BLE GATT UUID (与 tools/photo_uploader.html 中保持一致)
#define SVC_UUID "8f750001-2c3a-4f9b-8e21-6b5d7c9e0a01"  // 服务
#define RX_UUID  "8f750002-2c3a-4f9b-8e21-6b5d7c9e0a01"  // 电脑->设备 (写)
#define TX_UUID  "8f750003-2c3a-4f9b-8e21-6b5d7c9e0a01"  // 设备->电脑 (通知)

// ---------------- ST7793 3.6寸 240x400 8位并口面板 ----------------
// ST7793 属于老式 R61509 指令族: 寄存器索引为 16 位(高字节在先)，
// 地址/窗口/写显存命令为 0x200/0x201/0x202/0x210~0x213，
// 与 ST7789 的 0x2A/0x2B/0x2C 完全不同，LovyanGFX 无内置驱动，
// 这里继承 Panel_LCD 实现最小子集(初始化/方向/窗口)。
// 初始化序列移植自 MCUFRIEND_kbv 库对 ST7793 的实测代码。
class Panel_ST7793 : public lgfx::Panel_LCD {
public:
  Panel_ST7793(void) {
    auto cfg = config();
    cfg.panel_width   = 240;
    cfg.panel_height  = 400;
    cfg.memory_width  = 240;
    cfg.memory_height = 400;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.offset_rotation = 0;
    cfg.dummy_read_bits = 0;
    cfg.readable   = false;
    cfg.invert     = false;   // 若颜色像照片底片(反色)改为 true
    cfg.rgb_order  = false;   // 面板 8080 接口为 BGR 顺序（实测 RED 显蓝，需 BGR=1）
    cfg.dlen_16bit = false;
    cfg.bus_shared = false;
    config(cfg);
  }

  bool init(bool use_reset) override {
    if (!Panel_LCD::init(use_reset)) return false;   // 硬件复位
    startWrite(true);
    auto wr = [this](uint16_t cmd, uint16_t dat) { writeCmd16(cmd); writeData16(dat); };
    for (uint8_t i = 0; i < 4; i++) wr(0x0000, 0x0000);
    delay(15);
    wr(0x0400, 0x6200);            // NL=49 -> 400 行扫描
    wr(0x0008, 0x0808);            // 显示控制 2
    // 面板 Gamma 寄存器: 跳过, 色彩层次由软件 Gamma 校正处理(见下方 applyGamma)。
    wr(0x0010, 0x0016);            // 帧率 69.5Hz
    wr(0x0011, 0x0101);
    wr(0x0012, 0x0000);
    wr(0x0013, 0x0001);
    wr(0x0100, 0x0330);            // ---- 电源控制 ----
    wr(0x0101, 0x0237);
    wr(0x0103, 0x0D00);
    wr(0x0280, 0x6100);            // VCM
    wr(0x0102, 0xC1B0);
    delay(50);
    wr(0x0001, 0x0100);            // 驱动输出控制
    wr(0x0002, 0x0100);            // LCD 驱动控制
    wr(0x0003, 0x1030);            // 入口模式 (BGR=1，面板出厂 BGR 接口)
    wr(0x0009, 0x0001);
    wr(0x000C, 0x0000);
    wr(0x0090, 0x8000);
    wr(0x000F, 0x0000);
    wr(0x0210, 0x0000);            // 窗口 H 起点
    wr(0x0211, 0x00EF);            // 窗口 H 终点 = 239
    wr(0x0212, 0x0000);            // 窗口 V 起点
    wr(0x0213, 0x018F);            // 窗口 V 终点 = 399
    wr(0x0500, 0x0000);
    wr(0x0501, 0x0000);
    wr(0x0502, 0x005F);
    wr(0x0401, 0x0003);            // VLE|REV: 本面板正常显示极性
    wr(0x0404, 0x0000);
    delay(50);
    wr(0x0007, 0x0100);            // BASEE: 开显示
    delay(50);
    wr(0x0200, 0x0000);            // H 地址
    wr(0x0201, 0x0000);            // V 地址
    endWrite();
    setRotation(_rotation);
    return true;
  }

  void setRotation(uint_fast8_t r) override {
    r &= 7;
    _rotation = r;
    _internal_rotation = ((r + _cfg.offset_rotation) & 3)
                       | ((r & 4) ^ (_cfg.offset_rotation & 4));
    bool landscape = _internal_rotation & 1;
    _width  = landscape ? _cfg.panel_height : _cfg.panel_width;
    _height = landscape ? _cfg.panel_width  : _cfg.panel_height;
    _colstart = 0;
    _rowstart = 0;
    _xs = _xe = _ys = _ye = 0xFFFF;

    // 完全照搬 MCUFRIEND_kbv 对 ST7793/B509 的 setRotation:
    //   直接用硬编码的 MADCTL val 表，不依赖 LovyanGFX 的 getMadCtl
    //   参考: https://github.com/prenticedavid/MCUFRIEND_kbv/blob/master/MCUFRIEND_kbv.cpp#L366
    //
    //   val 位定义 (与 LovyanGFX MADCTL 相同):
    //     bit7 MY → GS (0x0400 bit15)
    //     bit6 MX → SS (0x0001 bit8)
    //     bit5 MV → AM (0x0003 bit3)
    //     bit3    → BGR (0x0003 bit12)
    //
    //   与 MCUFRIEND_kbv 完全一致的关键组合:
    //     横屏时 ①AM=1（计数器沿互换后的 V 方向自增，硬件完成像素转置）
    //            ②setWindow 里同时互换 H/V 寄存器（避免逻辑坐标 399
    //              写入物理 H 寄存器(max 239)被裁剪成 L 形）
    //     二者缺一不可（官方源码 setRotation 第 521 行 + setAddrWindow）。
    //
    //   val 表（与 MCUFRIEND 官方一致，bit3=1 → 0x0003 BGR=1）:
    //     rotation=0 竖屏:    val=0x48 (MX=1, BGR=1)
    //     rotation=1 横屏:    val=0x28 (AM=1, BGR=1, 寄存器互换)   【当前】
    //     rotation=2 竖屏反转: val=0x98 (MY=1, BGR=1)
    //     rotation=3 横屏反转: val=0x88 (MY=1, BGR=1, 寄存器互换)
    static const uint8_t kST7793Val[4] = {0x48, 0x28, 0x98, 0x88};
    uint8_t val = kST7793Val[_internal_rotation & 3];

    uint16_t GS = (val & 0x80) ? (1 << 15) : 0;   // MY → 0x0400 bit15
    uint16_t NL = ((400 / 8) - 1) << 9;             // NL = 0x6200 (400 rows)
    uint16_t SS_v = (val & 0x40) ? (1 << 8) : 0;    // MX → 0x0001 bit8
    uint16_t ORG = (val & 0x20) ? (1 << 3) : 0;     // MV → 0x0003 bit3 (AM)
    if (val & 0x08) ORG |= 0x1000;                   // BGR → 0x0003 bit12
    uint16_t madctl = ORG | 0x0030;                  // I/D0=1, I/D1=1 固定

    startWrite();
    writeCmd16(0x0400); writeData16((uint16_t)(GS | NL));
    writeCmd16(0x0001); writeData16((uint16_t)SS_v);
    writeCmd16(0x0003); writeData16(madctl);
    endWrite();
  }

  void setWindow(uint_fast16_t xs, uint_fast16_t ys,
                 uint_fast16_t xe, uint_fast16_t ye) override {
    // 与 MCUFRIEND_kbv 一致: 横屏时互换 H/V 寄存器地址（面板无 MV_AXIS
    // 能力，官方在 setRotation 第 521 行做同样的互换），配合 AM=1 让
    // 地址计数器沿 V 方向自增，硬件完成像素转置。光标先行。
    if (_internal_rotation & 1) {
      // 横屏: LovyanGFX 给的是 (0,0)-(399,239)，面板需要 (0,0)-(239,399)
      // 把 LGFX 的 x(0-399) 映射到面板 V(0-399)
      // 把 LGFX 的 y(0-239) 映射到面板 H(0-239)
      writeCmd16(0x0200); writeData16(ys);   // H 光标 = LGFX y
      writeCmd16(0x0201); writeData16(xs);   // V 光标 = LGFX x
      writeCmd16(0x0210); writeData16(ys);   // SC: H 窗口起 = LGFX y
      writeCmd16(0x0212); writeData16(xs);   // SP: V 窗口起 = LGFX x
      writeCmd16(0x0211); writeData16(ye);   // EC: H 窗口止 = LGFX yend
      writeCmd16(0x0213); writeData16(xe);   // EP: V 窗口止 = LGFX xend
    } else {
      // 竖屏: 直接写
      writeCmd16(0x0200); writeData16(xs);
      writeCmd16(0x0201); writeData16(ys);
      writeCmd16(0x0210); writeData16(xs);
      writeCmd16(0x0212); writeData16(ys);
      writeCmd16(0x0211); writeData16(xe);
      writeCmd16(0x0213); writeData16(ye);
    }
    writeCmd16(0x0202);   // _MW: 开始写显存
  }

  void setInvert(bool invert) override {
    startWrite();
    // 该面板为 REV_SCREEN 型: 正常显示时 REV=1，反色时 REV=0
    writeCmd16(0x0401); writeData16((uint16_t)(invert ? 0x0002 : 0x0003));
    endWrite();
  }
  void setSleep(bool) override {}       // ST7793 无 0x10/0x11 命令
  void setPowerSave(bool) override {}   // ST7793 无 0x38/0x39 命令
  void update_madctl(void) override {}  // ST7793 无 0x36/0x3A 命令，禁止基类写入

protected:
  void writeCmd16(uint16_t cmd) {   // 16位索引: 先高字节后低字节 (RS=0)
    _bus->writeCommand(cmd >> 8, 8);
    _bus->writeCommand(cmd & 0xFF, 8);
  }
  void writeData16(uint16_t d) {    // 16位参数: 先高字节后低字节 (RS=1)
    _bus->writeData(d >> 8, 8);
    _bus->writeData(d & 0xFF, 8);
  }
};

// ---------------- 屏幕驱动定义 ----------------
class Display : public lgfx::LGFX_Device {
#if PANEL_TYPE == 4
  Panel_ST7793        _panel;
  lgfx::Bus_Parallel8 _bus;
#else
#if PANEL_TYPE == 1
  lgfx::Panel_ST7735S _panel;
#elif PANEL_TYPE == 3
  lgfx::Panel_ILI9341 _panel;
#else
  lgfx::Panel_ST7789  _panel;
#endif
  lgfx::Bus_SPI       _bus;
#endif
  lgfx::Light_PWM     _light;

public:
  Display(void) {
#if PANEL_TYPE == 4
    {  // 8 位 8080 并口总线 (ESP32-S3 LCD_CAM 硬件外设)
      auto cfg = _bus.config();
      cfg.freq_write = 16000000;   // WR 时钟 (20MHz 在面包板/飞线下时序裕量不足会花屏)
      cfg.freq_read  =  8000000;
      cfg.pin_wr = PIN_P_WR;
      cfg.pin_rd = PIN_P_RD;
      cfg.pin_rs = PIN_P_RS;
      cfg.pin_d0 = PIN_P_D0;
      cfg.pin_d1 = PIN_P_D1;
      cfg.pin_d2 = PIN_P_D2;
      cfg.pin_d3 = PIN_P_D3;
      cfg.pin_d4 = PIN_P_D4;
      cfg.pin_d5 = PIN_P_D5;
      cfg.pin_d6 = PIN_P_D6;
      cfg.pin_d7 = PIN_P_D7;
      _bus.config(cfg);
    }
    {  // 面板
      auto cfg = _panel.config();
      cfg.pin_cs   = PIN_P_CS;
      cfg.pin_rst  = PIN_P_RST;
      cfg.pin_busy = -1;
      _panel.config(cfg);
      _panel.setBus(&_bus);
#if PIN_P_BL >= 0
      _panel.setLight(&_light);
#endif
    }
#else
    {  // SPI 总线
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
#if PANEL_TYPE == 1
      cfg.freq_write = 27000000;   // ST7735 建议 ≤27MHz
#else
      cfg.freq_write = 40000000;
#endif
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = true;
      cfg.use_lock   = true;
      cfg.pin_sclk = PIN_SCLK;
      cfg.pin_mosi = PIN_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc   = PIN_DC;
      _bus.config(cfg);
    }
    {  // 面板
      auto cfg = _panel.config();
      cfg.pin_cs   = PIN_CS;
      cfg.pin_rst  = PIN_RST;
      cfg.pin_busy = -1;
#if PANEL_TYPE == 1
      // ST7735S 1.8寸 128x160：本模块物理像素从显存第0行列开始，
      // 故显存直接设为 128x160、偏移归零(与可见区完全重合，无越界扫描)。
      cfg.panel_width   = 128;
      cfg.panel_height  = 160;
      cfg.memory_width  = 128;
      cfg.memory_height = 160;
      cfg.offset_x       = 0;
      cfg.offset_y       = 0;
      // 若出现边缘花屏/错位，说明你的批次显存是 132x162 且带偏移，改回:
      //   memory_width=132; memory_height=162; 并试 offset (0,0)/(2,0)/(0,2)/(2,2)
      // 反相显示: 若颜色像照片底片(反色)则改为 true
      cfg.invert     = false;
#else
      cfg.panel_width   = 240;
      cfg.panel_height  = 320;
      cfg.memory_width  = 240;
      cfg.memory_height = 320;
      cfg.offset_x       = 0;
      cfg.offset_y       = 0;
      cfg.invert     = false;   // 若颜色像照片底片(反色)，改为 true
#endif
      cfg.offset_rotation = 0;
      cfg.dummy_read_bits = 1;
      cfg.readable   = false;
      cfg.rgb_order  = true;    // 红蓝颜色互换(红显蓝/蓝显红)时设为 true
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
      _panel.setBus(&_bus);
#if PIN_BL >= 0
      _panel.setLight(&_light);
#endif
    }
#endif
#if (PANEL_TYPE == 4 && PIN_P_BL >= 0) || (PANEL_TYPE != 4 && PIN_BL >= 0)
    {  // 背光 PWM
      auto cfg = _light.config();
#if PANEL_TYPE == 4
      cfg.pin_bl = PIN_P_BL;
#else
      cfg.pin_bl = PIN_BL;
#endif
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
    }
#endif
    setPanel(&_panel);
  }
};

static Display tft;

// ---------------- 传输状态 ----------------
// 重要: BLE 写回调运行在 Bluedroid 的 BTC_TASK 上，该任务栈很小(约3KB)，
//       不能在回调里做 LittleFS 文件操作(会栈溢出重启)。
//       因此回调只把数据 memcpy 进环形缓冲，写盘由 loop() 主循环完成。
#define RING_SIZE (64 * 1024)   // 64KB: 吸收主循环偶发卡顿, 防止回调阻塞
static uint8_t  s_ring[RING_SIZE];
static volatile uint32_t s_ringHead = 0;   // 回调(生产者)写入位置
static volatile uint32_t s_ringTail = 0;   // loop(消费者)读取位置

static BLECharacteristic* s_pTx = nullptr;
static BLEServer*          s_pServer = nullptr;
static volatile bool s_bleConnected  = false;
static volatile bool s_sessionActive = false;  // 一次上传会话进行中
static volatile bool s_startRequested = false;
static volatile bool s_finishRequested = false;
static volatile bool s_clearRequested  = false;
static volatile bool s_overflow = false;   // 缓冲溢出标志(回调置位, loop 在 F 包检查)
static volatile uint32_t s_startTotal = 0;

// BLE 启用状态(由按键切换): false=关闭省电(不广播,无法连接), true=启用广播
static volatile bool s_bleEnabled = false;
// BLE 是否已初始化过(避免重复 init 导致内存泄漏)
static bool s_bleInited = false;

// 按键去抖状态(只在 loop 主任务访问)
static uint32_t s_btnLastChange = 0;
static bool     s_btnLastLevel  = true;   // HIGH=未按下
static bool     s_btnEvent       = false;  // 检测到的"按下"事件(待 loop 处理)

// Toast 弹窗: 显示 1 秒后自动消失, 恢复之前的显示
static char     s_toastText[48] = {0};
static uint16_t s_toastColor    = TFT_WHITE;
static uint32_t s_toastExpire   = 0;     // 0 表示无 toast 在显示

// 以下仅在 loop() 主循环中访问
static File     s_upFile;
static bool     s_streamOpen = false;
static uint32_t s_recvTotal = 0;
static uint32_t s_recvBytes = 0;

// 生产者: BLE 回调把数据放入环形缓冲。
// 重要: 回调运行在 BTC_TASK(栈~3KB)，绝对不能 busy-wait 等 loop 消费，
//       否则 BLE 协议栈发不出 Write Response，网页端 writeValueWithResponse
//       超时报 "GATT operation failed for unknown reason" 并断开。
//       缓冲满时直接丢弃并置 s_overflow，由 loop 在 F 包时报错让用户重试。
static bool ringPush(const uint8_t* data, uint32_t len) {
  uint32_t head = s_ringHead;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t next = (head + 1 == RING_SIZE) ? 0 : head + 1;
    if (next == s_ringTail) {   // 缓冲满: 不阻塞, 丢弃并标记
      s_overflow = true;
      s_ringHead = head;
      return false;
    }
    s_ring[head] = data[i];
    head = next;
  }
  s_ringHead = head;
  return true;
}

enum UiState { UI_BOOT, UI_CONNECTED, UI_RECEIVING, UI_IMAGE, UI_ERROR };
static UiState s_uiState = UI_BOOT;
static bool    s_uiDrawn = false;
static uint8_t s_pctDrawn = 255;

// ---------------- 通知给电脑 ----------------
static void notifyStr(const char* s) {
  if (s_pTx && s_bleConnected) {
    s_pTx->setValue(s);
    s_pTx->notify();
  }
}

// ---------------- 屏幕界面 (横屏 400x240) ----------------
static void centerText(const char* s, int y, const lgfx::IFont* font, uint32_t color) {
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(middle_center);
  tft.setFont(font);
  tft.drawString(s, tft.width() / 2, y);
}

static void drawBoot() {
  tft.fillScreen(TFT_BLACK);
  centerText("ESP32-S3",      50,  &fonts::Font2, TFT_WHITE);
  centerText("Photo Display", 110, &fonts::Font0, TFT_CYAN);
  if (s_bleEnabled) {
    centerText("BLE Ready",     160, &fonts::Font0, TFT_GREEN);
    centerText(BLE_DEVICE_NAME, 200, &fonts::Font0, TFT_YELLOW);
  } else {
    // 低功耗: 蓝牙已关闭, 提示用户按键启用
    centerText("BLE OFF",       160, &fonts::Font0, TFT_RED);
    centerText("press button",  195, &fonts::Font0, TFT_YELLOW);
    centerText("to enable BLE", 215, &fonts::Font0, TFT_YELLOW);
  }
}

static void drawConnected() {
  tft.fillScreen(TFT_BLACK);
  centerText("ESP32-S3",      50,  &fonts::Font2, TFT_WHITE);
  centerText("BLE Connected", 110, &fonts::Font0, TFT_GREEN);
  centerText("Open web page", 160, &fonts::Font0, TFT_CYAN);
  centerText("pick a JPG",   200, &fonts::Font0, TFT_CYAN);
}

static void drawReceiving(int pct) {
  if (pct == 0) {  // 首次绘制背景
    tft.fillScreen(TFT_BLACK);
    centerText("Receiving...", 60, &fonts::Font0, TFT_CYAN);
    int bw = tft.width() * 3 / 4;
    tft.drawRect((tft.width() - bw) / 2 - 2, 100, bw + 4, 30, TFT_WHITE);
  }
  int bw = tft.width() * 3 / 4;
  int w = bw * pct / 100;
  if (w > 0) tft.fillRect((tft.width() - bw) / 2, 102, w, 26, 0x3B82F6);
  char line[16];
  snprintf(line, sizeof(line), "%d%%", pct);
  tft.fillRect(tft.width()/2 - 30, 170, 60, 22, TFT_BLACK);
  centerText(line, 181, &fonts::Font0, TFT_WHITE);
}

static void drawDecodeError() {
  tft.fillScreen(TFT_BLACK);
  centerText("JPEG failed!",  100, &fonts::Font0, TFT_RED);
  centerText("Use baseline JPG", 150, &fonts::Font0, TFT_YELLOW);
}

// 弹出 Toast 小窗口: 屏幕中央圆角矩形 + 一行文字, 1 秒后由 loop 清除。
// 不主动 fillScreen (避免覆盖正在显示的图片), 只画窗口区域。
static void drawToast(const char* msg, uint16_t fgColor) {
  const int cw = 260, ch = 60;           // 窗口尺寸
  const int cx = (tft.width() - cw) / 2;
  const int cy = (tft.height() - ch) / 2;
  // 半透明背景: 用深色填充模拟 (TFT 无 alpha, 用接近黑色的深蓝)
  tft.fillRoundRect(cx, cy, cw, ch, 8, 0x1082);
  // 边框
  tft.drawRoundRect(cx, cy, cw, ch, 8, fgColor);
  // 文字
  tft.setTextDatum(middle_center);
  tft.setTextColor(fgColor, 0x1082);
  tft.setFont(&fonts::Font2);
  tft.drawString(msg, tft.width() / 2, tft.height() / 2);
}

// ---- 软件 Gamma 校正 ----
// 面板出厂模拟 Gamma 让高光区扁平(浅色和白色分不清)。
// 在数字层面用查表法做 Gamma 1.5 校正: 让接近满值的浅色被压低,
// 与白色拉开层次。例如肉色 G=54(565) → 校正后 ≈ 50, 白色 G=63 → 63 不变。
// gamma > 1: 整体变暗但高光层次更丰富。值越大效果越强, 建议 1.3~2.0。
static const float GAMMA_VAL = 1.5f;
static uint8_t s_gammaR[32];   // 5-bit R/B 查表
static uint8_t s_gammaG[64];   // 6-bit G 查表
static bool s_gammaInited = false;

static void initGammaTables() {
  if (s_gammaInited) return;
  for (int i = 0; i < 32; i++)
    s_gammaR[i] = (uint8_t)roundf(powf((float)i / 31.0f, GAMMA_VAL) * 31.0f);
  for (int i = 0; i < 64; i++)
    s_gammaG[i] = (uint8_t)roundf(powf((float)i / 63.0f, GAMMA_VAL) * 63.0f);
  s_gammaInited = true;
}

// 显示 LittleFS 中指定路径的 JPG：自动等比缩放 + 居中 + 软件 Gamma 校正
// 用 PSRAM sprite 解码 JPEG, 查表校正每个 RGB565 像素后 pushSprite 到屏幕。
static bool displayJpeg(const char* path) {
  initGammaTables();
  tft.fillScreen(TFT_BLACK);

  // 在 PSRAM 创建 sprite, 解码 JPEG 到 sprite (不在屏幕上)
  lgfx::LGFX_Sprite sprite(&tft);
  sprite.setColorDepth(16);
  sprite.setPsram(true);
  if (!sprite.createSprite(tft.width(), tft.height())) {
    Serial.println("[ERR] PSRAM sprite alloc failed, fallback to direct draw");
    // 回退: 直接画到屏幕(无 Gamma 校正)
    bool ok = tft.drawJpgFile(LittleFS, path, 0, 0,
                              tft.width(), tft.height(), 0, 0,
                              0.0f, 0.0f, middle_center);
    if (!ok) { drawDecodeError(); }
    return ok;
  }

  // 解码 JPEG 到 sprite, 自动等比缩放居中
  bool ok = sprite.drawJpgFile(LittleFS, path, 0, 0,
                               tft.width(), tft.height(), 0, 0,
                               0.0f, 0.0f, middle_center);
  if (!ok) {
    Serial.println("[ERR] JPEG decode failed");
    sprite.deleteSprite();
    drawDecodeError();
    return false;
  }

  // 遍历 sprite 像素做 Gamma 查表校正
  // 重要: LovyanGFX sprite buffer 用大端序存 RGB565, ESP32 读 uint16_t 是小端序,
  //       必须 bswap16 字节交换后再提取通道, 否则 R/G/B 错位(肉色变绿).
  uint16_t* buf = (uint16_t*)sprite.getBuffer();
  uint32_t total = (uint32_t)tft.width() * tft.height();
  for (uint32_t i = 0; i < total; i++) {
    uint16_t p = __builtin_bswap16(buf[i]);   // 大端 → 小端, 现在 bit layout 正确
    uint8_t r5 = (p >> 11) & 0x1F;
    uint8_t g6 = (p >>  5) & 0x3F;
    uint8_t b5 =  p        & 0x1F;
    uint16_t corrected = (uint16_t)((s_gammaR[r5] << 11) | (s_gammaG[g6] << 5) | s_gammaR[b5]);
    buf[i] = __builtin_bswap16(corrected);   // 写回大端序, 与 LGFX 内部格式一致
  }

  // 推送到屏幕
  sprite.pushSprite(0, 0);
  sprite.deleteSprite();
  return true;
}

// 显示一个 1 秒的 Toast 弹窗, 不破坏底层显示 (1 秒后由 loop 自动恢复)
static void showToast(const char* msg, uint16_t fgColor) {
  strncpy(s_toastText, msg, sizeof(s_toastText) - 1);
  s_toastText[sizeof(s_toastText) - 1] = '\0';
  s_toastColor = fgColor;
  s_toastExpire = millis() + 1000;
  drawToast(s_toastText, s_toastColor);
}

// ---------------- BLE 回调 ----------------
class PhotoServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    s_bleConnected = true;
    Serial.println("[BLE] client connected");
    if (s_uiState == UI_BOOT || s_uiState == UI_ERROR) {
      s_uiState = UI_CONNECTED;
      s_uiDrawn = false;
    }
  }
  void onDisconnect(BLEServer*) override {
    // 注意: 本回调在 BTC_TASK 中执行，不能碰文件对象/串口打印，只置标志
    s_bleConnected  = false;
    s_sessionActive = false;
    s_startRequested  = false;
    s_finishRequested = false;
    s_clearRequested  = false;
    s_overflow = false;
    s_ringHead = 0;  // 丢弃环形缓冲中未写完的数据
    s_ringTail = 0;
    // 仅在 BLE 仍启用状态下恢复广播(用户按键关闭后不要重启广播)
    if (s_bleEnabled) BLEDevice::startAdvertising();
    if (s_uiState != UI_IMAGE) {
      s_uiState = s_bleEnabled ? UI_BOOT : UI_IMAGE;
      s_uiDrawn = false;
    }
  }
};

class PhotoRxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // 本回调在 BTC_TASK(栈很小) 中执行：禁止文件操作/串口打印，
    // 只做最小工作(置标志 + 数据入环形缓冲)，重活全部交给 loop()。
    size_t len = c->getLength();      // 兼容 Arduino-ESP32 2.x / 3.x
    const uint8_t* d = c->getData();
    if (len == 0 || d == nullptr) return;

    switch ((char)d[0]) {
      case 'S': {  // 开始: 'S' + uint32 LE 总长度
        if (len < 5) break;
        uint32_t total = (uint32_t)d[1] | ((uint32_t)d[2] << 8)
                       | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
        s_ringHead = 0;
        s_ringTail = 0;
        s_startTotal = total;
        s_finishRequested = false;
        s_overflow = false;
        s_sessionActive = true;
        s_startRequested = true;   // loop 负责校验大小/打开文件
        break;
      }
      case 'D': {  // 数据: 'D' + 数据 -> 入环形缓冲
        if (s_sessionActive && len > 1 && !s_overflow) {
          ringPush(d + 1, (uint32_t)len - 1);
        }
        break;
      }
      case 'F': {  // 结束
        if (s_sessionActive) s_finishRequested = true;
        break;
      }
      case 'C': {  // 清空屏幕
        s_clearRequested = true;
        break;
      }
    }
  }
};

// 消费者: 把环形缓冲中的数据写入文件，返回本次写入字节数
static uint32_t pumpRingToFile() {
  uint32_t written = 0;
  uint32_t tail = s_ringTail;
  uint32_t head = s_ringHead;   // 快照；head 之前的数据都已就绪
  while (tail != head) {
    uint32_t seg = (head >= tail) ? (head - tail) : (RING_SIZE - tail);
    s_upFile.write(s_ring + tail, seg);
    written += seg;
    tail += seg;
    if (tail >= RING_SIZE) tail = 0;
  }
  s_ringTail = tail;
  return written;
}

// ---------------- BLE 启用/关闭 (由按键切换) ----------------
// 启动蓝牙协议栈 + 创建 GATT 服务 + 开始广播。
// 仅在 s_bleEnabled 为 false 时执行(避免重复初始化)。
static void bleStart() {
  if (s_bleEnabled) return;
  if (!s_bleInited) {
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(512);
    s_bleInited = true;
  } else {
    // 之前 deinit 过, 重新启动协议栈
    BLEDevice::init(BLE_DEVICE_NAME);
  }
  s_pServer = BLEDevice::createServer();
  s_pServer->setCallbacks(new PhotoServerCB());
  BLEService* svc = s_pServer->createService(SVC_UUID);
  s_pTx = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  s_pTx->addDescriptor(new BLE2902());
  BLECharacteristic* pRx = svc->createCharacteristic(
        RX_UUID, BLECharacteristic::PROPERTY_WRITE
               | BLECharacteristic::PROPERTY_WRITE_NR);
  pRx->setCallbacks(new PhotoRxCB());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  adv->start();
  s_bleEnabled = true;
  Serial.printf("[BLE] enabled, advertising as \"%s\"\n", BLE_DEVICE_NAME);
}

// 关闭蓝牙: 停止广播 + 释放协议栈 + 清理传输状态。
// deinit(false) 不释放内部缓冲内存, 下次 init() 复用, 避免反复分配。
static void bleStop() {
  if (!s_bleEnabled) return;
  BLEDevice::stopAdvertising();
  delay(30);                      // 等待协议栈稳定, 让 BTC 任务收尾
  BLEDevice::deinit(false);       // 关闭 Bluedroid 协议栈(主要省电来源)
  s_pServer = nullptr;
  s_pTx = nullptr;
  s_bleEnabled = false;
  s_bleConnected = false;
  s_sessionActive = false;
  s_startRequested = false;
  s_finishRequested = false;
  s_clearRequested = false;
  s_overflow = false;
  s_ringHead = 0;
  s_ringTail = 0;
  if (s_streamOpen && s_upFile) { s_upFile.close(); s_streamOpen = false; }
  Serial.println("[BLE] disabled (low power)");
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== ESP32-S3 BLE Photo Display (PlatformIO) ===");

  // 按键 (BOOT 按键, 对地触发, 内部上拉)
  pinMode(PIN_BTN, INPUT_PULLUP);

  // 屏幕初始化
  tft.init();
  tft.setRotation(1);          // 横屏 400x240 (0/2 为竖屏)
  // 背光: 满亮度(255)会让高光区视觉溢出, 浅色看起来发白。
  //   降到 220 让色彩更饱和, 高光层次更明显。若仍偏淡可再降到 180。
  tft.setBrightness(220);
  tft.fillScreen(TFT_BLACK);

  // 文件系统 (begin(true): 首次使用自动格式化)
  bool fsOk = LittleFS.begin(true);
  if (!fsOk) {
    Serial.println("[FS] LittleFS mount failed!");
    centerText("FS Error!", 64, &fonts::Font0, TFT_RED);
  } else {
    Serial.printf("[FS] total %u KB, used %u KB\n",
                  LittleFS.totalBytes() / 1024, LittleFS.usedBytes() / 1024);
    // 恢复上次显示的图片
    if (LittleFS.exists(IMG_PATH) && displayJpeg(IMG_PATH)) {
      s_uiState = UI_IMAGE;
      s_uiDrawn = true;
    }
  }

  // 低功耗: CPU 降到 80MHz (ESP32-S3 蓝牙最低稳定频率)
  // 并口屏由 LCD_CAM 硬件外设驱动, 频率与 CPU 无关; JPEG 解码会稍慢但仅在传图结束瞬间
  setCpuFrequencyMhz(80);
  Serial.printf("[CPU] running at %u MHz\n", getCpuFrequencyMhz());

  // 蓝牙默认不启用 (按键按下后启用), 开机即省电
  // 屏幕显示提示用户按 BOOT 键启用蓝牙
  if (s_uiState != UI_IMAGE) { s_uiState = UI_BOOT; s_uiDrawn = false; }
  Serial.println("[BLE] disabled at boot, press BOOT to enable");
}

void loop() {
  // 断开后清理未完成的写盘 (回调在 BTC 任务中不能碰文件对象)
  if (!s_bleConnected && s_streamOpen) {
    if (s_upFile) { s_upFile.flush(); s_upFile.close(); }
    s_streamOpen = false;
    s_sessionActive = false;
    s_startRequested = false;
    s_finishRequested = false;
  }

  // 清空请求
  if (s_clearRequested) {
    s_clearRequested = false;
    if (s_bleConnected) { s_uiState = UI_CONNECTED; s_uiDrawn = false; }
    notifyStr("OK");
  }

  // 上传开始: 校验大小 + 清理旧文件 + 打开临时文件 (全部在主任务做)
  if (s_startRequested) {
    s_startRequested = false;
    s_recvTotal = s_startTotal;
    s_recvBytes = 0;
    s_streamOpen = false;
    Serial.printf("[BLE] upload start, %u bytes\n", s_recvTotal);
    if (s_recvTotal == 0 || s_recvTotal > MAX_UPLOAD_BYTES) {
      s_sessionActive = false;
      notifyStr("ERR:file too large (max 1.5MB)");
    } else {
      // 先清理旧图与临时文件，使峰值 Flash 占用不超过 1.5MB
      LittleFS.remove(IMG_PATH);
      LittleFS.remove(TMP_PATH);
      s_upFile = LittleFS.open(TMP_PATH, "w");
      if (!s_upFile) {
        s_sessionActive = false;
        s_ringHead = 0; s_ringTail = 0;
        notifyStr("ERR:storage error");
      } else {
        s_streamOpen = true;
        s_uiState = UI_RECEIVING;
        s_uiDrawn = false;
        s_pctDrawn = 255;
      }
    }
  }

  // 把环形缓冲中的数据写入 Flash (主任务上下文，栈充足)
  if (s_streamOpen) {
    s_recvBytes += pumpRingToFile();
  }

  // 上传完成: 先排空缓冲，再关文件 -> 解码显示
  if (s_finishRequested) {
    if (s_streamOpen) {
      s_recvBytes += pumpRingToFile();   // 排空最后一段数据
      if (s_ringTail != s_ringHead) {
        delay(2);                        // 极端情况下还有数据，下轮继续
      } else {
        s_finishRequested = false;
        s_sessionActive  = false;
        s_streamOpen     = false;
        s_upFile.flush();
        s_upFile.close();
        Serial.printf("[BLE] upload end, got %u bytes\n", s_recvBytes);
        if (s_recvBytes == 0) {
          LittleFS.remove(TMP_PATH);
          notifyStr("ERR:empty file");
        } else if (s_overflow) {
          // 缓冲曾溢出, 数据不完整, 丢弃并提示重试
          LittleFS.remove(TMP_PATH);
          s_uiState = s_bleConnected ? UI_ERROR : UI_BOOT;
          s_uiDrawn = false;
          Serial.println("[ERR] ring buffer overflow, data incomplete");
          notifyStr("ERR:buffer overflow, please retry");
        } else {
          bool ok = displayJpeg(TMP_PATH);   // 解码在主任务做，不阻塞 BLE 栈
          if (ok) {
            LittleFS.remove(IMG_PATH);
            LittleFS.rename(TMP_PATH, IMG_PATH);
            s_uiState = UI_IMAGE;
            s_uiDrawn = true;
            Serial.println("[OK] image saved and displayed");
            notifyStr("OK");
          } else {
            LittleFS.remove(TMP_PATH);
            s_uiState = s_bleConnected ? UI_ERROR : UI_BOOT;
            s_uiDrawn = false;
            notifyStr("ERR:JPEG decode failed, use baseline JPG");
          }
        }
      }
    } else {
      s_finishRequested = false;  // 文件未成功打开过
      s_sessionActive  = false;
    }
  }

  // 界面刷新
  switch (s_uiState) {
    case UI_BOOT:
    case UI_CONNECTED:
    case UI_ERROR:
      if (!s_uiDrawn) {
        if (s_uiState == UI_BOOT)           drawBoot();
        else if (s_uiState == UI_CONNECTED) drawConnected();
        else                                drawDecodeError();
        s_uiDrawn = true;
      }
      break;
    case UI_RECEIVING: {
      uint8_t pct = s_recvTotal ? (uint8_t)(s_recvBytes * 100 / s_recvTotal) : 0;
      if (pct > 100) pct = 100;
      if (!s_uiDrawn) { drawReceiving(0); s_uiDrawn = true; s_pctDrawn = 0; }
      // 每 5% 才重绘一次, 避免频繁 fillRect 卡住主循环导致缓冲溢出
      if (pct / 5 != s_pctDrawn / 5) { drawReceiving(pct); s_pctDrawn = pct; }
      break;
    }
    default:  // UI_IMAGE: 图片已在屏幕上，不做刷新
      break;
  }

  // ---- 按键扫描 (GPIO1 键, 去抖) ----
  bool curLevel = digitalRead(PIN_BTN);
  if (curLevel != s_btnLastLevel) {
    s_btnLastChange = millis();
    s_btnLastLevel = curLevel;
  }
  if (!s_btnLastLevel && (millis() - s_btnLastChange > BTN_DEBOUNCE_MS)) {
    if (!s_btnEvent) s_btnEvent = true;   // 已稳定按下, 置事件标志
  }
  // ---- 按键事件处理: 切换蓝牙 + 弹 Toast 提示 ----
  if (s_btnEvent) {
    s_btnEvent = false;
    // 等待按键释放 (避免一次按下被多次触发; 超时 1s 自动放弃)
    uint32_t t0 = millis();
    while (!digitalRead(PIN_BTN) && (millis() - t0 < 1000)) { delay(5); }
    if (s_bleEnabled) {
      // 当前启用 -> 关闭 (省电)
      bleStop();
      showToast("BLE OFF", TFT_RED);
    } else {
      // 当前关闭 -> 启用 (可连接上传)
      bleStart();
      showToast("BLE ON",  TFT_GREEN);
    }
  }

  // ---- Toast 过期处理: 1 秒后自动消失, 恢复底层显示 ----
  if (s_toastExpire != 0 && millis() > s_toastExpire) {
    s_toastExpire = 0;
    // 重绘底层: 有图片重画图片, 无图片重画 BOOT 界面
    if (LittleFS.exists(IMG_PATH) && (s_uiState == UI_IMAGE || s_uiState == UI_BOOT)) {
      if (displayJpeg(IMG_PATH)) { s_uiState = UI_IMAGE; s_uiDrawn = true; }
      else { s_uiState = UI_BOOT; s_uiDrawn = false; }
    } else {
      s_uiDrawn = false;   // 触发重绘 BOOT/Connected/Error 界面
    }
  }

  // ---- 自适应延时: 传输期间快速响应, 空闲时降功耗 ----
  if (s_streamOpen || s_startRequested || s_finishRequested || s_sessionActive) {
    delay(1);   // 上传会话期间: 紧凑轮询, 防止环形缓冲溢出
  } else {
    delay(50);  // 空闲: 让 CPU 大量时间在 idle task, 降低功耗与温度
  }
}
