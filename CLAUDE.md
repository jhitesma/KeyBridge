# KeyBridge

ESP32-S3 firmware that bridges modern USB and Bluetooth keyboards to vintage serial terminals (Wyse 50+, VT100, ADM-3A) via parallel ASCII output through a 74HCT245 level shifter.

## Hardware Target

- ESP32-S3 DevKitC-1
- 74HCT245 level shifter (3.3V GPIO → 5V terminal-compatible parallel output)
- 7-bit parallel data bus + strobe line

## Build Commands

```bash
pio run                          # Compile
pio run -t upload                # Compile and flash
pio run -t upload -t monitor     # Flash and open serial monitor
pio device monitor               # Serial monitor only (115200 baud)
pio check                        # Static analysis
```

PlatformIO is used via the VS Code extension. CLI install is optional (`brew install platformio` or `pip install platformio`).

## Architecture

Three-file design:

- **`wyse_usb_keyboard.ino`** — Main firmware: USB host stack, Bluetooth HID (Classic + BLE), WiFi AP with web server, GPIO parallel output, HID report processing, key repeat, FreeRTOS tasks
- **`config.h`** — `AdapterConfig` struct, NVS persistence (`Preferences` library), JSON serialization/deserialization for the web API
- **`web_ui.h`** — Complete single-page web UI embedded as a PROGMEM raw string literal (HTML/CSS/JS, ~480 lines)

## Data Flow

```
USB/BT Keyboard → HID Report → FreeRTOS Queue → processKeypress() → sendChar() → GPIO → 74HCT245 → Terminal
```

Input sources (USB host task, BT HID callback) push key events into a shared FreeRTOS queue. The main processing loop dequeues events, applies key mapping and modifier logic, and outputs 7-bit ASCII via parallel GPIO with a timed strobe pulse.

## REST API

The ESP32 runs a WiFi AP with a web server exposing these endpoints:

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Serve the web UI |
| `/api/config` | GET | Read current configuration |
| `/api/config` | POST | Partial config update (JSON merge) |
| `/api/status` | GET | Connection status, uptime, memory |
| `/api/bt/pair` | POST | Start Bluetooth pairing scan |
| `/api/reset` | POST | Factory reset (erase NVS, reboot) |
| `/api/log` | GET | Recent keypress log (ring buffer) |
| `/api/test` | POST | Send test string to terminal |
| `/api/preset/wyse50` | GET | Load Wyse 50 key mapping preset |
| `/api/preset/vt100` | GET | Load VT100 key mapping preset |
| `/api/preset/adm3a` | GET | Load ADM-3A key mapping preset |

## Coding Conventions

- **Functions**: `camelCase` — `processKeypress()`, `sendChar()`, `loadConfig()`
- **Constants/macros**: `SCREAMING_SNAKE` — `MAX_SPECIAL_KEYS`, `TAG`
- **Structs**: `PascalCase` — `AdapterConfig`, `SpecialKeyEntry`
- **Struct fields**: `snake_case` — `pin_d0`, `ansi_mode`, `hid_keycode`
- **Pointer star on variable**: `const char *TAG` (not `const char* TAG`)
- **Indentation**: 4 spaces, no tabs
- **Braces**: Same line (Attach style)
- **Includes**: Intentionally ordered (system → ESP-IDF → Arduino libs → local headers); do not auto-sort

## Key Patterns

- **PROGMEM raw strings**: The entire web UI lives in `web_ui.h` as a C++ raw string literal stored in flash. Do not run web/HTML formatters on this file.
- **NVS Preferences**: Config is serialized as a raw struct blob with a version number. Changing the `AdapterConfig` struct layout requires bumping the version in `saveConfig()`.
- **FreeRTOS pinned tasks**: USB host runs on core 0 (`xTaskCreatePinnedToCore`), main loop on core 1.
- **Queue-based input decoupling**: USB and BT callbacks push to a shared `keyQueue`; processing happens in a single consumer task.
- **Partial JSON config updates**: `POST /api/config` merges incoming JSON into the existing config (only overwrites fields present in the request body).
- **Ring buffer key log**: Recent keypresses stored in a circular buffer for the `/api/log` endpoint.

## Constraints

- **Do not run web formatters on `web_ui.h`** — The file has `clang-format off/on` guards around the PROGMEM string. Reformatting will break the embedded HTML/CSS/JS.
- **Config struct changes require a version bump** — The NVS blob is read by size; struct layout changes without a version bump will load corrupt data.
- **USB Host needs specific build flags** — `ARDUINO_USB_MODE=0` and `ARDUINO_USB_CDC_ON_BOOT=0` in `platformio.ini` are required for USB host mode. Do not remove these.

## Formatting

clang-format is configured (`.clang-format` in project root). To format source files:

```bash
clang-format -i wyse_usb_keyboard.ino config.h
```

Do not format `web_ui.h` directly — the clang-format guards handle it, but it's safer to format only the files that need it.
