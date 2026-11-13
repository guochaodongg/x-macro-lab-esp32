# Seeed XIAO ESP32-S3 Sense

[简体中文](README_zh.md)

A XiaoZhi firmware configuration for the **XIAO ESP32-S3 Sense** (ESP32-S3R8,
8 MB flash, 8 MB octal PSRAM) with its Sense expansion board fitted. This is a
separate board identity from
[`xiao-esp32s3-respeaker-xvf3800`](../xiao-esp32s3-respeaker-xvf3800/README.md);
do not flash one firmware over the other, because the OTA channel is different.

## What is enabled

| Software | Status | Notes |
| --- | --- | --- |
| Wi-Fi provisioning / XiaoZhi protocol | Yes | `WifiBoard`, BOOT button starts config mode or toggles chat |
| Wake word + AFE audio processing | Yes | Selected by `scripts/build.py` (`--language` / `--wake-word`) |
| Voice input | Yes | On-board MSM261D3526H1CPM PDM microphone, 32 kHz input resampled to 16 kHz |
| Voice output | Requires wiring | External I2S DAC/amplifier, 24 kHz output |
| Camera photo upload | Yes | OV2640, `self.camera.take_photo` MCP tool (no display needed) |
| Display / status LED | No | `NoDisplay` / `NoLed` by design, see conflicts below |
| Home page / music player / photo gallery | No | These LVGL launcher pages need an LCD; disabled to save flash |
| microSD storage | No | SD slot is reserved, not mounted; see "microSD" below |
| Device-side AEC | No | No playback reference channel; not in the Kconfig allowlist |
| Console | USB Serial/JTAG | GPIO43/44 are used by the I2S speaker |

The board has **no on-board speaker**. Without an external I2S amplifier the
firmware builds and the microphone/camera work, but replies are inaudible.

## Hardware wiring

Signals on the Sense expansion board (B2B connector) are fixed and do not need
wiring. Only the I2S amplifier is external.

| Function | GPIO | XIAO pin |
| --- | --- | --- |
| PDM mic CLK | 42 | B2B connector |
| PDM mic DATA | 41 | B2B connector |
| Camera XCLK | 10 | B2B connector |
| Camera SCCB SDA / SCL | 40 / 39 | B2B connector |
| Camera D0-D7 | 15 / 17 / 18 / 16 / 14 / 12 / 11 / 48 | B2B connector |
| Camera VSYNC / HREF / PCLK | 38 / 47 / 13 | B2B connector |
| microSD SCK / MISO / MOSI / CS | 7 / 8 / 9 / 21 | D8 / D9 / D10 / USER_LED |
| I2S BCLK -> amp | 4 | D3 |
| I2S WS/LRCK -> amp | 43 | D6 / U0TXD |
| I2S DOUT -> amp | 44 | D7 / U0RXD |
| BOOT button | 0 | BOOT |

Connect a 3.3 V I2S amplifier such as MAX98357A (BCLK, LRCK, DIN) plus a
speaker, and share GND. If the amplifier has an SD/EN pin, tie it high or drive
it from a free GPIO; this board does not control it.

### Conflicts

- **GPIO21 is both USER_LED and microSD CS.** This board never drives GPIO21 as
  an LED. `GpioLed` also uses `LEDC_CHANNEL_0`, which the camera XCLK occupies,
  so the default `NoLed` is used.
- **GPIO43/44 are UART0.** The console must be USB Serial/JTAG
  (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), otherwise logs and audio conflict.
- **PDM RX only works on I2S_NUM_0** on ESP32-S3. Playback uses I2S_NUM_1 via
  `NoAudioCodecSimplexPdm`.
- **Camera, PDM mic and SD stress PSRAM/DMA and the 3.3 V rail.** Use
  80 MHz octal PSRAM (the ESP32-S3 defaults already do).

## Build and flash

Use ESP-IDF `release/v6.1` (minimum v6.0.1) from the project root:

```powershell
python scripts\build.py --list-boards
python scripts\build.py seeed/xiao-esp32s3-sense --language zh-CN --wake-word nihaoxiaozhi
idf.py -p COM5 flash monitor
```

`seeed/xiao-esp32s3-sense` is the build-script board selector (it contains a
slash); `xiao-esp32s3-sense` is the reported OTA board type/name. The build
script applies 8 MB flash, `partitions/v2/8m.csv`, USB console, OV2640 sensor
config and the no-display feature set.

Camera orientation can be set with the exposed build options, for example:

```powershell
python scripts\build.py seeed/xiao-esp32s3-sense --build-options-json "{\"camera_hmirror\":true,\"camera_vflip\":true}"
```

When flashing a merged image manually, write it at `0x0`, DIO, 80 MHz, 8 MB.
A manual `idf.py menuconfig` build does **not** inherit `config.json`; set the
target, flash size, partition table, USB console and wake word yourself.

## microSD

The card is not mounted. `XIAOZHI_ENABLE_SDCARD` depends on the photo gallery /
music player, which in turn need the LVGL launcher on an LCD; the Sense board
has no display, so mounting the card would have no consumer. To use the slot
after adding a display, enable `CONFIG_XIAOZHI_ENABLE_HOME_PAGE=y`,
`CONFIG_XIAOZHI_ENABLE_SDCARD=y`, `CONFIG_XIAOZHI_SDCARD_MODE_SDSPI=y` and set
`CONFIG_XIAOZHI_SDCARD_PIN_CLK=7`, `..._MISO=8`, `..._MOSI=9`, `..._CS=21`.

## Validation

- Host-side board/build-script tests and formatting checks: see the task report.
- Firmware compilation and all audio/camera/SD behaviour still require a
  physical XIAO ESP32-S3 Sense with an amplifier. A successful host test is not
  hardware validation.
