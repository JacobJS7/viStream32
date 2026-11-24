# viStream32

A retro-styled DIY MP3 player built around the ESP32. It plays music from a microSD card through a PCM5102A I2S DAC, shows a 128×64 OLED interface, is driven by four physical buttons, and doubles as a Wi-Fi access point with a built-in web remote, file manager and over-the-air firmware updates.

## Features

**Playback**
- MP3 playback from microSD (`BackgroundAudioMP3`, decoding on a dedicated core)
- Playlist = all `.mp3` files in a folder
- Modes: Normal, Shuffle, Repeat One, Repeat All
- Seeking ±10 s; accurate for both CBR and VBR files (Xing/Info TOC, VBRI)
- Real track duration, bitrate and sample rate read from the MP3 headers
- ID3v2 / ID3v1 title and artist (UTF-8, UTF-16 and Latin-1 tags)
- Software volume control
- Resume after power-off: last folder, track, position, volume and mode are kept in NVS

**OLED interface (SSD1306 128×64)**
- Now Playing: title and artist with marquee scrolling, progress bar, `01:22 / 04:32` time, play state, volume bar and mode badge
- File browser with folder/track icons, a "now playing" marker, item counter and scrollbar
- Volume and mode-change pop-ups
- Boot splash with real progress, "No SD card" and firmware-update screens
- Battery and Wi-Fi client indicators

**Web interface** (Wi-Fi access point, `http://192.168.4.1/`)
- Remote control: play/pause, next/previous, mode, volume, seek
- File manager: browse, upload, download, delete files and folders, create folders
- Wi-Fi settings (SSID / password)
- Firmware update (OTA) from the browser

## Hardware

| Part | Notes |
|---|---|
| ESP32 DevKit V1 (DOIT) | 4 MB flash, no PSRAM required |
| PCM5102A I2S DAC | line/headphone output |
| SSD1306 OLED 128×64, I2C | monochrome |
| microSD module (SPI) | FAT32 card |
| 4 push buttons | NEXT, PREV, PLAY, BROWSER |
| 1 switch | Wi-Fi access point on/off |
| Li-ion cell + 1:2 voltage divider | battery measurement (optional) |

### Pin map

| Function | GPIO |
|---|---|
| I2S BCLK / LRC / DOUT | 26 / 25 / 27 |
| OLED SDA / SCL | 21 / 22 |
| SD CS (SCK 18, MISO 19, MOSI 23) | 5 |
| Button NEXT | 4 |
| Button PREV | 33 |
| Button PLAY | 32 |
| Button BROWSER | 17 |
| Wi-Fi switch | 16 |
| Battery ADC (÷2 divider) | 34 |

Buttons and the switch connect the pin to GND (internal pull-ups are used). All pin numbers live in `src/config.h`.

## Controls

| Button | Short press | Long press | Other |
|---|---|---|---|
| NEXT | next track / move down in browser | seek +10 s | with PLAY held: volume up |
| PREV | previous track / move up in browser | seek −10 s | with PLAY held: volume down |
| PLAY | play/pause / open selected item | stop | double click: cycle playback mode |
| BROWSER | toggle file browser / Now Playing | | |

Wi-Fi switch ON starts the access point (default SSID `vistream32`, password `vistream1234`; changeable from the web page).

## Getting started

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Format a microSD card as FAT32 and copy music into folders, e.g. `/muzyka/` (played automatically on first boot if present).
3. Build and flash over USB:
   ```sh
   pio run -t upload
   pio device monitor        # 115200 baud
   ```
   If the upload fails with "No serial data received", hold the BOOT button when `Connecting...` appears.

> The project uses the `min_spiffs.csv` partition table (two 1.9 MB app slots for OTA). The first flash after changing the partition table must be done over USB.

### Firmware update over Wi-Fi

1. Build: `pio run` → `.pio/build/esp32doit-devkit-v1/firmware.bin`
2. Turn the Wi-Fi switch on, connect to the device's access point and open `http://192.168.4.1/`.
3. In **Network → Firmware** choose `firmware.bin` and click **Update**. Progress is shown on the page and on the OLED; the player restarts when done.

Only `firmware.bin` is used for OTA — `bootloader.bin` and `partitions.bin` are for USB flashing.

## Project structure

```
src/
  main.cpp              setup() order, task creation, loop()
  config.h              pins and tuning constants
  state.h/.cpp          shared state and mutexes
  audio/                player (decoder, playlist, transport), mp3_info (headers, ID3, seeking)
  ui/                   display (OLED screens, uiTask), browser (file navigation)
  input/                buttons
  net/                  wifi_ap, web_server (HTTP routes), ota
  storage/              sd_fs (SD helpers), persistence (NVS)
  power/                battery
  web/index.html        web interface, embedded into the firmware
```

The firmware runs FreeRTOS tasks on both cores: audio feeding on Core 1, OLED UI and Wi-Fi/web server on Core 0. See [CLAUDE.md](CLAUDE.md) for architecture notes, the mutex lock order and other implementation details.

## Known limitations

- OLED fonts are Latin-1 only: Polish and other non-Latin-1 characters are shown without diacritics on the display (the web page shows them correctly).
- MP3 only.
- Battery reading requires the voltage divider on GPIO 34; without it the indicator shows empty.

## License

No license has been chosen yet.
