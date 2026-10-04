# Weather Sentinel — firmware

Firmware for the **M5Stack CoreS3 SE** (ESP32-S3), built with PlatformIO and the Arduino
framework. It uses M5Stack's own libraries (M5Unified / M5GFX) for the display, touch,
power management and speaker, plus ArduinoJson 7 to read the server's responses.

The CoreS3 SE needs the M5Stack libraries, not a generic display driver. Its backlight
and its LCD/touch reset go through the AXP2101 power chip and the AW9523B I/O expander,
which M5Unified sets up automatically. A bare display driver gives a black screen with no
error.

## One-time setup

1. Install PlatformIO. The VS Code extension installs PlatformIO Core under
   `%USERPROFILE%\.platformio`.
2. Copy `src/wifi_credentials.h.example` to `src/wifi_credentials.h` in the same folder
   and fill in:
   - `WIFI_SSID` and `WIFI_PASSWORD`;
   - `SERVER_BASE_URL` — the Weather Sentinel server's LAN address and port, for example
     `http://192.168.1.50:8085`, with no trailing slash.

   The build stops with a clear error if `SERVER_BASE_URL` is missing.
   `wifi_credentials.h` is gitignored and must never be committed. A DHCP reservation for
   the server keeps its address from changing.

## Build and upload (PowerShell)

```powershell
$pio = (Get-ChildItem "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" -ErrorAction SilentlyContinue).FullName
& $pio run -d "D:\Documents\GitHub\n4mi-weather-sentinel\firmware"
& $pio run -d "D:\Documents\GitHub\n4mi-weather-sentinel\firmware" -t upload --upload-port COM15
```

- Adjust the path and COM port to your machine. `platformio.ini` must be in this
  `firmware/` folder, because `-d` points PlatformIO here.
- **After an upload, restart the device once more before judging responsiveness.** The
  upload's automatic reset does restart the firmware, but touch is noticeably snappier
  after one more restart. On a bedside charger, with no computer attached, this doesn't
  come up.
- To watch the serial log: `& $pio device monitor -d "<same firmware path>" --port COM15`.
  It runs at 115200 baud. The log includes `[fetch]`, `[alarm]`, `[touch]` and `[diag]`
  lines.

## Environments (`platformio.ini`)

- **`cores3-se` (default):** `board = m5stack-cores3`. This is the one in use.
- **`cores3-se-fallback`:** a generic ESP32-S3 board with explicit PSRAM and USB-CDC
  build flags. It's kept only in case a future `espressif32` platform version stops
  recognising `m5stack-cores3`. It hasn't been needed.

**Library versions are pinned exactly** to the last hardware-confirmed build: M5Unified
0.2.22, M5GFX 0.2.29, ArduinoJson 7.4.3. A library update therefore can't silently change
the device's behaviour. To update one, change its version in `platformio.ini`, then build,
flash and re-run the bench tests before committing.

**The platform is pinned as well:** the
[pioarduino](https://github.com/pioarduino/platform-espressif32) release `55.03.311`
(ESP32 Arduino core 3.3.11, ESP-IDF 5.5.5), given as its download URL in both
environments. Until 2026-10-04 the file said only `platform = espressif32`, which on the
development PC resolved to this same pioarduino package, already installed for other
projects. On a different PC it would have fetched the official PlatformIO platform, which
this firmware has never been built with. Pinning the URL means every build uses the
tested combination. `pio pkg list -d firmware` shows what a build resolves to.

## Hardware facts that shaped the code

Each of these was checked against library source or the real display before the code
relied on it.

- **Text size:** size 1 is 6 px per character and size 2 is 12 px, on a 320 × 240
  screen. Layouts are budgeted from these numbers.
- **Brightness:** `M5.Display.setBrightness(0–255)` gives only 9 visibly different
  levels on this board, and `0` turns the backlight off rather than dimming it. The
  Settings stepper moves between those 9 real levels.
- **Touch events:** a tap's release is `wasClicked()`; there is no `wasReleased()`.
  Coordinates are `touch_detail_t.x` / `.y`.
- **Board identity:** the SE reports `board_M5StackCoreS3SE`, not `board_M5CoreS3`.
- **M5GO Bottom3 LEDs:** 10 RGB LEDs on GPIO5 (M5-Bus pin 8), GRB colour order. LEDs
  1–5 run down the right side and 6–10 up the left. They're driven by M5Unified's own
  `LED_Strip_Class` / `LedBus_RMT`, so no extra library is needed. `M5.Led.setBrightness()`
  is left at 255, because its curve wipes out the smaller colour channel at low levels
  (amber turns red); colours are scaled in the code instead. Level 16 (out of 255) is the
  dimmest level where red and amber still look different.
- **Power loss:** the AXP2101 reads USB voltage at about 5000 mV on wall power and 0
  within a second of unplugging. The M5GO battery reads through the same chip. After
  `M5.Power.powerOff()` on battery, the device starts by itself 2–3 s after USB power
  returns.
- **Battery %** read 100% on battery at 4147 mV, so low-battery shutdown goes by voltage
  instead. The 3.5 V threshold is provisional until a battery run-down test.
- **Missing values in the server's data:** alert text fields are read with ArduinoJson's
  `| ""` default, so a JSON `null` never shows up on screen as the word "null".

## Code conventions worth keeping

- **Shared geometry constants.** Every region that is both drawn and tappable (the
  acknowledge bar, the Now footer, the NWS header block, the Settings nav bar, the Alert
  History rows) uses one set of constants for drawing and hit-testing, so the two can't
  drift apart.
- **Shared freshness rules.** Whether a screen shows clear, alert or unknown is decided
  by shared helpers (`currentNwsView()`, `currentLightningView()`), never by reading raw
  fetched values. Stale data must never read as "clear".
- **The server decides alert priority.** The firmware shows, sounds and acknowledges
  `alerts[0]` and counts the rest as "+N more". It never re-sorts.
- **One layout for alert text.** The NWS Alerts page and the Alert History detail view
  both draw alert text through `drawAlertBody()`, so one alert can't be laid out two
  different ways.

## Diagnostics

`diagnostics/led_power/` (at the repo root) is a separate bench sketch for the LEDs and
the power chip, with no Wi-Fi or server. Its README covers flashing it and its tests,
including the battery run-down test. Flash the real firmware back afterwards.
