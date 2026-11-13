# Seeed XIAO ESP32-S3 Sense

[English](README.md)

本目录是 **XIAO ESP32-S3 Sense**（ESP32-S3R8、8 MB Flash、8 MB 八线 PSRAM，
装好 Sense 扩展板）的小智固件配置。它是独立板型，与
[`xiao-esp32s3-respeaker-xvf3800`](../xiao-esp32s3-respeaker-xvf3800/README_zh.md)
的 OTA 通道不同，不能互相覆盖烧录。

## 这套软件包含什么

| 软件能力 | 状态 | 说明 |
| --- | --- | --- |
| Wi-Fi 配网 / 小智协议 | 启用 | `WifiBoard`；BOOT 键在启动阶段配网，平时切换对话 |
| 唤醒词 + AFE 音频处理 | 启用 | 由 `scripts/build.py` 按 `--language` / `--wake-word` 选择 |
| 语音输入 | 启用 | 板载 MSM261D3526H1CPM PDM 麦克风，32 kHz 输入重采样到 16 kHz |
| 语音输出 | 需外接 | 外部 I2S DAC/功放，24 kHz 输出 |
| 摄像头拍照上传 | 启用 | OV2640，`self.camera.take_photo` MCP 工具，不依赖屏幕 |
| 显示屏 / 状态灯 | 不启用 | 默认 `NoDisplay` / `NoLed`，原因见下方冲突说明 |
| 主页启动器 / 音乐播放器 / 相册 | 不启用 | 这些 LVGL 页面需要 LCD，关闭以节省 Flash |
| microSD 存储 | 不启用 | 卡槽引脚保留但不挂载，见「microSD」一节 |
| 设备端 AEC | 不启用 | 无播放参考通道，也不在 Kconfig 白名单中 |
| 控制台 | USB Serial/JTAG | GPIO43/44 已分配给 I2S 功放 |

**板子没有板载喇叭**。不接外部 I2S 功放时固件仍可编译、麦克风与摄像头仍
可用，但听不到回复。

## 接线与引脚

Sense 扩展板上的信号通过 B2B 连接器固定，无需接线；只有 I2S 功放需要外接。

| 功能 | GPIO | XIAO 引脚 |
| --- | --- | --- |
| PDM 麦克风 CLK | 42 | B2B 连接器 |
| PDM 麦克风 DATA | 41 | B2B 连接器 |
| 摄像头 XCLK | 10 | B2B 连接器 |
| 摄像头 SCCB SDA / SCL | 40 / 39 | B2B 连接器 |
| 摄像头 D0-D7 | 15 / 17 / 18 / 16 / 14 / 12 / 11 / 48 | B2B 连接器 |
| 摄像头 VSYNC / HREF / PCLK | 38 / 47 / 13 | B2B 连接器 |
| microSD SCK / MISO / MOSI / CS | 7 / 8 / 9 / 21 | D8 / D9 / D10 / USER_LED |
| I2S BCLK -> 功放 | 4 | D3 |
| I2S WS/LRCK -> 功放 | 43 | D6 / U0TXD |
| I2S DOUT -> 功放 | 44 | D7 / U0RXD |
| BOOT 按键 | 0 | BOOT |

外接 3.3 V I2S 功放（如 MAX98357A）的 BCLK、LRCK、DIN 并接喇叭，务必共地。
功放若有 SD/EN 引脚，请自行拉高或接到空闲 GPIO，本驱动不控制它。

### 引脚冲突

- **GPIO21 同时是 USER_LED 和 microSD CS**。本驱动不把 GPIO21 当 LED 驱动；
  且 `GpioLed` 使用的 `LEDC_CHANNEL_0` 会被摄像头 XCLK 占用，因此采用默认
  的 `NoLed`。
- **GPIO43/44 是 UART0**。控制台必须使用 USB Serial/JTAG
  （`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），否则日志与音频冲突。
- **ESP32-S3 的 PDM RX 只能使用 I2S_NUM_0**。播放通过
  `NoAudioCodecSimplexPdm` 使用 I2S_NUM_1。
- 摄像头、PDM 麦克风、SD 卡同时使用会占用较多 PSRAM/DMA 与 3.3 V 电流，
  保持八线 PSRAM 80 MHz（ESP32-S3 默认已配置）。

## 编译与烧录

在项目根目录使用 ESP-IDF `release/v6.1`（最低 v6.0.1）终端：

```powershell
python scripts\build.py --list-boards
python scripts\build.py seeed/xiao-esp32s3-sense --language zh-CN --wake-word nihaoxiaozhi
idf.py -p COM5 flash monitor
```

`seeed/xiao-esp32s3-sense` 是构建脚本的板选择器（含 `/`），
`xiao-esp32s3-sense` 是上报的 OTA 板型/变体名。构建脚本会自动应用 8 MB
Flash、`partitions/v2/8m.csv`、USB 控制台、OV2640 传感器配置与无屏功能组合。

摄像头方向可通过构建选项调整，例如：

```powershell
python scripts\build.py seeed/xiao-esp32s3-sense --build-options-json "{\"camera_hmirror\":true,\"camera_vflip\":true}"
```

手工烧录合并镜像时：偏移 `0x0`、DIO、80 MHz、8 MB。直接用
`idf.py menuconfig` 构建不会继承 `config.json`，需自行设置 target、Flash
大小、分区表、USB 控制台和唤醒词。

## microSD

当前不挂载 SD 卡。`XIAOZHI_ENABLE_SDCARD` 依赖相册 / 音乐播放器，而它们又
依赖 LCD 上的 LVGL 启动器；Sense 板没有显示屏，挂载后也没有使用者。日后若
加装显示屏，可开启 `CONFIG_XIAOZHI_ENABLE_HOME_PAGE=y`、
`CONFIG_XIAOZHI_ENABLE_SDCARD=y`、`CONFIG_XIAOZHI_SDCARD_MODE_SDSPI=y`，并
设置 `CONFIG_XIAOZHI_SDCARD_PIN_CLK=7`、`..._MISO=8`、`..._MOSI=9`、
`..._CS=21`。

## 验证

- 主机侧板级/构建脚本测试与格式检查：见任务报告。
- 固件编译以及录音、播放、唤醒、拍照、SD 卡等行为仍需在实际
  XIAO ESP32-S3 Sense（接好功放）上验证；主机测试不代表硬件验证。
