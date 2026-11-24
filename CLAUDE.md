# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

viStream32 is an ESP32 MP3 player firmware (PlatformIO, Arduino framework, board `esp32doit-devkit-v1`): SD card → `BackgroundAudioMP3` → I2S PCM5102A DAC, SSD1306 OLED UI via U8g2, physical buttons, and a Wi-Fi AP with an embedded web remote/file manager. Comments, serial logs and the spec are mostly in Polish.

The functional spec / roadmap is `src/vistream32-spec.md` (checklist of features plus a suggested implementation order). Treat it as the source of requirements. `test/vistream32-spec.md` is an older copy.

## Commands

`pio` is not on PATH; it lives at `~/.platformio/penv/Scripts/pio.exe` (or use the PlatformIO VS Code extension).

```s
pio run                      # build
pio run -t upload            # flash
pio device monitor           # serial monitor, 115200 baud
pio run -t upload -t monitor # flash + monitor
```

There are no automated tests. `test/` holds reference material, not Unity tests: earlier working sketches (`test/main.cpp`, `test/main_reference.cpp`, `*.txt` snippets), the original feature list, and `test/vistream32-ui.html` (standalone mockup of the web UI). Verification is done on hardware via serial logs.

## Architecture

Firmware is split into modules under `src/` (includes are relative to `src/`, e.g. `#include "audio/player.h"`):

| Module | Responsibility |
|---|---|
| `main.cpp` | `setup()` init order + task creation, `loop()` (calls the `*Poll()` functions) |
| `config.h` | all pins and tuning constants — the only place for pin numbers |
| `types.h` | `RepeatMode`, `Screen`, `DirectoryEntry` |
| `state.h/.cpp` | state shared by several modules (`extern`) + the three mutexes; module-private state is `static` in that module's `.cpp` |
| `audio/player` | decoder objects, playlist, transport, volume, `audioFeedTask` |
| `audio/mp3_info` | MP3 metadata without decoding: ID3v2/ID3v1 title/artist, first frame (bitrate/sample rate), Xing/Info/VBRI (frame count, TOC) |
| `storage/sd_fs` | path helpers, `scanDirectory`, `removePathRecursive` |
| `storage/persistence` | NVS (`Preferences`): settings, resume state, `requestStateSave` / `persistencePoll` |
| `input/buttons` | ISRs and button logic (`buttonsPoll`) |
| `ui/display` | U8g2, `uiTask`, all `draw*` screens, `setScreen` |
| `ui/browser` | OLED file-browser navigation |
| `net/wifi_ap` | AP on/off from the switch, `wifiTask` |
| `net/web_server` | HTTP routes and handlers |
| `net/ota` | firmware update over HTTP (`POST /update`, `Update` library) |
| `power/battery` | ADC battery measurement |
| `web/index.html` | the web page, embedded via `board_build.embed_txtfiles` |

Key pieces:

**Tasks / cores**
- `audioFeedTask` (Core 1): reads 512-byte chunks from the open SD `File f` and feeds `mp3.write()` while `mp3.availableForWrite() > 512`. On EOF it calls `handleTrackEnd()` (applies repeat/shuffle/auto-next). This loop must never be blocked for long — audio glitches otherwise.
- `uiTask` (Core 0): redraws the OLED every `UI_FRAME_INTERVAL_MS` (40 ms) using full-buffer U8g2. Screens are a state machine (`Screen` enum: `SCREEN_BROWSER`, `SCREEN_NOWPLAYING`; splash shown until `uiReady`). Change screens only via `setScreen()` so transitions animate. Animations are time-based (`millis()`), not frame-count based. A centered overlay (volume bar or a short toast) is drawn on top of either screen via `showVolumeOverlay()` / `showToast(literal, icon)`. Icons are drawn with U8g2 primitives (`icon*` helpers in `display.cpp`), no bitmaps; each `draw*` function documents its Y zones. UI text is English; fonts are Latin-1 only.
- `wifiTask` (Core 0): AP (`configuredSsid`/`configuredPassword` from NVS) and `WebServer` on port 80; the only place that reacts to the Wi-Fi switch pin. The web server is started/stopped together with the AP — calling `WebServer::begin()` while Wi-Fi is off asserts in lwIP.
- Arduino `loop()`: button handling (ISR-set flags → debounced press/long-press/double-click logic), battery ADC sampling, periodic NVS state save.

**Synchronization** — three FreeRTOS mutexes:
- `audioMutex`: guards `mp3` decoder calls and `currentGain`.
- `sdMutex`: guards every SD access (`f`, `SD.open`, directory scans, web upload/download/delete).
- `stateMutex`: guards playlist/position and browser state (`tracks`, `currentTrack`, `shuffleOrder`, `playlistDirectory`, `currentDirectory`, `directoryEntries`, `currentPositionSec`).
- Lock order used throughout: `stateMutex` → `audioMutex` → `sdMutex`. Keep it to avoid deadlocks. Any new code touching SD from the web handlers must take `sdMutex`. Mutexes are non-recursive: `rebuildShuffleOrder()` expects the caller to hold `stateMutex`.
- `playlistDirectory` (playing folder, saved to NVS) is separate from `currentDirectory` (where the OLED browser is). `scanDirectory()` fills a caller-provided vector; only `refreshBrowser()` writes `directoryEntries`.

**Playback model**
- Playlist = the `.mp3` files of one directory (`loadPlaylistFromDirectory`); `tracks` holds full paths. Shuffle uses a precomputed `shuffleOrder` permutation.
- `BackgroundAudioMP3` exposes no duration/time index, so `openTrack()` calls `readMp3Info()` (`audio/mp3_info`): duration comes from the Xing/VBRI frame count or, for CBR, from size and bitrate. The file is left positioned at the first audio frame, so ID3 tags and cover art never reach the decoder. Seeking is in seconds (`seekToSecond` / `seekBySeconds`) via `mp3ByteOffsetForSecond()`, which uses the Xing TOC for VBR and is linear otherwise, followed by `mp3.flush()`. `APPROXIMATE_MP3_BYTES_PER_SECOND` (128 kbps) is only the fallback when no frame header is found. Position is tracked via `millis()` (`updatePlaybackPosition`). Each track logs a `MP3: ...` line with what was parsed.
- ID3 title/artist live in `currentTrackTitle` / `currentTrackArtist`, guarded by `audioMutex` rather than `stateMutex`, because `openTrack()` is called both with and without `stateMutex` held. The OLED font is Latin-1 only, so Polish diacritics don't render there; the web page shows them fine.
- Volume is software gain via `mp3.setGain()` (0 … `MAX_GAIN`, step `GAIN_STEP`).
- State is persisted with `Preferences` (namespace `vistream`): `lastFolder`, `lastTrackIndex`, `lastPositionSec`, `lastVolume`, `shuffle`, `repeatMode`, `wifiSsid`, `wifiPassword`. Call `requestStateSave()` rather than writing NVS directly; `persistencePoll()` in `loop()` flushes it. On boot it restores the last folder/position, else falls back to `/muzyka`, else the browser at `/`.

**Web interface**
- The web page (HTML/CSS/JS in one file) is `src/web/index.html`, embedded into the firmware by `board_build.embed_txtfiles` and referenced in `net/web_server.cpp` as `_binary_src_web_index_html_start`. If the file is renamed/moved, update both `platformio.ini` and that symbol. `test/vistream32-ui.html` is only a mockup.
- Routes are registered in `registerWebRoutes()`: `/status` (JSON), `/play`, `/pause`, `/next`, `/prev`, `/mode`, `/playFile`, `/volume`, `/seek`, `/files`, `/download`, `/upload`, `/delete`, `/mkdir`, `/config` (GET/POST). Path args are URL-decoded via `getDecodedPathArg()`.

**Buttons** (all `INPUT_PULLUP`, active LOW, interrupts on CHANGE)
- NEXT/PREV: short = next/prev track (or move selection in browser), long (`LONG_PRESS_MS`) = seek ±10 s; pressed while PLAY is held = volume up/down.
- PLAY: single click = play/pause (or select entry in browser), double click = cycle mode (Normal → Shuffle → Repeat One → Repeat All), long press = stop.
- BROWSER: toggles browser / now-playing.

## Hardware pin map

I2S: BCLK 26, LRC 25, DOUT 27 · I2C (OLED): SDA 21, SCL 22 · SD CS 5 (VSPI default 18/19/23) · Buttons: NEXT 4, PREV 33, PLAY 32, BROWSER 17 · Wi-Fi switch 16 · Battery ADC 34 (÷2 divider).

I2S DOUT was moved from 22 to 27 to avoid the I2C SCL conflict — older sketches in `test/` still use 22. When adding pins, avoid GPIO 6–11 (flash), and remember GPIO 34–39 are input-only with no internal pull-ups.

`platformio.ini` sets `CONFIG_ESP_TASK_WDT_TIMEOUT_S=15`, but the precompiled Arduino core uses its own task watchdog (5 s on IDLE0). Upload handlers that loop over HTTP body chunks on Core 0 must call `vTaskDelay(1)`; `yield()` does not let IDLE0 run. On error they must close the client connection, otherwise WebServer drains the rest of the body in a tight loop and the watchdog resets the chip.

Partition table is `min_spiffs.csv`: two 1.9 MB OTA app slots, NVS at 0x9000 as before. After building, the firmware can be uploaded from the web page ("Firmware" section) using `.pio/build/esp32doit-devkit-v1/firmware.bin`. Changing the partition table again requires a USB flash.
