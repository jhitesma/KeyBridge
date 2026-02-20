# Wyse 50 Keyboard Scan Interface Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rewrite KeyBridge's output interface to emulate the Wyse 50 keyboard scan matrix so a Bluetooth keyboard (Keychron K2 via Classic BT) can replace the missing original keyboard.

**Architecture:** The Wyse 50's J3 keyboard connector exposes a 7-bit scan address bus (A0-A6) and a single Key Return line. The terminal's 8031 CPU outputs a key address every ~6μs and reads Key Return to detect presses. KeyBridge reads the address via a TXS0108E level shifter, looks up a key state table (populated from Classic BT HID reports), and drives Key Return accordingly via a MOSFET. A dedicated FreeRTOS task on core 0 polls the address bus in a tight loop for guaranteed <1μs response time.

**Tech Stack:** ESP32 (WROOM-32), PlatformIO (arduino+espidf), TXS0108E breakout, 2N7000 N-channel MOSFET

**Why original ESP32 (not ESP32-S3):** The Keychron K2 uses a Broadcom BCM20730 chipset — Classic BT only, hardware-incapable of BLE. The ESP32-S3 only supports BLE (no Classic BT), so it cannot pair with the K2. The original ESP32 supports Classic BT + BLE + WiFi simultaneously with coexistence enabled. The existing `esp_hid_gap.c` already has all the Classic BT code behind `CONFIG_BT_HID_HOST_ENABLED` guards.

---

## Prerequisites / Hardware Setup

Before any firmware work, the following hardware must be prepared:

### Classic BT Setup

The Keychron K2 pairs via Classic Bluetooth. The original ESP32 (WROOM-32) supports Classic BT + BLE + WiFi coexistence. No hardware modifications needed for Bluetooth — just firmware config changes (handled in Task 1).

Put the K2 into pairing mode: hold `Fn + 1` (or `Fn + 2`/`Fn + 3`) for 3 seconds until the LED blinks rapidly.

### Wyse 50 J3 Wiring

J3 keyboard connector pinout (from 88-021-01 maintenance manual):

| J3 Pin | Signal | Direction | ESP32 GPIO (default) |
|--------|--------|-----------|---------------------|
| 1 | Chassis Ground | — | — |
| 2 | Logic Ground | — | GND |
| 3 | +5V | Power | → TXS0108E VB |
| 4 | Address bit 2 | Terminal → ESP32 | GPIO 14 |
| 5 | Address bit 1 | Terminal → ESP32 | GPIO 5 |
| 6 | Address bit 0 | Terminal → ESP32 | GPIO 4 |
| 7 | Address bit 3 | Terminal → ESP32 | GPIO 15 |
| 8 | Address bit 5 | Terminal → ESP32 | GPIO 16 |
| 9 | Address bit 6 | Terminal → ESP32 | GPIO 17 |
| 10 | Address bit 4 | Terminal → ESP32 | GPIO 13 |
| 11 | Key Return | ESP32 → Terminal | GPIO 18 (via 2N7000) |
| 12 | N/C | — | — |

**Note:** GPIOs 6-11 are reserved for internal flash on the original ESP32 (WROOM-32 module). The defaults above avoid those pins. GPIO 2 is also avoided (connected to on-board LED, has pull-down, may affect boot).

### TXS0108E Wiring

The TXS0108E auto-detects direction per channel. Use 7 channels for address inputs (terminal→ESP32) and the 8th channel is unused (Key Return uses a separate MOSFET because the TXS0108E can't reliably sink current against the terminal's pull-up resistor).

```
TXS0108E Breakout:
  VA  → ESP32 3.3V
  VB  → J3 pin 3 (+5V from terminal)
  GND → Common ground (J3 pin 2 + ESP32 GND)
  OE  → ESP32 3.3V (or a GPIO to control enable after boot)

  B1 ← J3 pin 6 (A0)  →  A1 → ESP32 GPIO 4
  B2 ← J3 pin 5 (A1)  →  A2 → ESP32 GPIO 5
  B3 ← J3 pin 4 (A2)  →  A3 → ESP32 GPIO 14
  B4 ← J3 pin 7 (A3)  →  A4 → ESP32 GPIO 15
  B5 ← J3 pin 10 (A4) →  A5 → ESP32 GPIO 13
  B6 ← J3 pin 8 (A5)  →  A6 → ESP32 GPIO 16
  B7 ← J3 pin 9 (A6)  →  A7 → ESP32 GPIO 17
  B8 — unused
```

### Key Return Output (2N7000 MOSFET)

The terminal expects an active-low Key Return with a pull-up to 5V on the terminal side. The TXS0108E can't reliably sink enough current against that pull-up (~1mA limit). A 2N7000 N-channel MOSFET provides a clean solution:

```
ESP32 GPIO 18 → 2N7000 Gate (pin 1)
                2N7000 Source (pin 3) → GND
                2N7000 Drain (pin 2) → J3 pin 11 (Key Return)

(Terminal's internal pull-up pulls Key Return HIGH when MOSFET is off.
 ESP32 drives GPIO 18 HIGH to turn on MOSFET, pulling Key Return LOW = key pressed.)
```

If you don't have a 2N7000, any small N-channel MOSFET with logic-level gate threshold (Vgs < 3.3V) works. A 2N2222 NPN transistor with a 1k base resistor also works.

---

## Task 1: Port to Original ESP32 with Classic BT

Switch the build target from ESP32-S3 to original ESP32 (WROOM-32) and enable Classic BT for Keychron K2 pairing.

**Files:**
- Modify: `platformio.ini`
- Modify: `sdkconfig.defaults`
- Modify: `src/keybridge.cpp`

**Step 1:** Update `platformio.ini` — add a new environment for the original ESP32. Keep the S3 env commented out for reference:

```ini
; [env:esp32s3]
; board = esp32-s3-devkitc-1
; ... (comment out existing S3 env)

[env:esp32dev]
platform = espressif32 @ 6.10.0
board = esp32dev
framework = arduino, espidf
monitor_speed = 115200
board_build.partitions = default.csv
lib_deps =
    bblanchon/ArduinoJson @ ^7.4.1
```

Note: Remove `board_build.arduino.usb_mode` and `cdc_on_boot` — those were ESP32-S3 USB-specific.

**Step 2:** Update `sdkconfig.defaults` — enable Classic BT and HID host:

```
# Bluetooth controller
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_HID_HOST_ENABLED=y

# Classic BT: Secure Simple Pairing
CONFIG_BT_SSP_ENABLED=y

# BLE: use 4.2 scan APIs (required by esp_hid_gap.c)
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set

# WiFi + BT coexistence (required for simultaneous WiFi AP + Classic BT)
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

Remove the `CONFIG_ESP_CONSOLE_SECONDARY_NONE` line (S3-specific, not relevant to original ESP32).

**Step 3:** In `keybridge.cpp`, update `bt_init_task()` — change the BT mode from BLE-only to BTDM (dual mode):

```cpp
// Change:
uint8_t mode = ESP_BT_MODE_BLE;
// To:
uint8_t mode = ESP_BT_MODE_BTDM;
```

The `#if CONFIG_BT_CLASSIC_ENABLED` block that handles Classic BT mode selection will now compile and take effect.

**Step 4:** Guard the USB host code behind `#ifdef` so it compiles on original ESP32 (which has no native USB host). Wrap the entire USB host section (`startUsbHost()`, `usb_host_daemon_task()`, `usb_keyboard_task()`, etc.) in:

```cpp
#if CONFIG_SOC_USB_OTG_SUPPORTED
// ... existing USB host code ...
#endif
```

And in `app_main()`:
```cpp
#if CONFIG_SOC_USB_OTG_SUPPORTED
if (config.enable_usb) startUsbHost();
#endif
```

**Step 5:** Delete the auto-generated `sdkconfig.esp32s3` file (PlatformIO will generate a new one for the esp32dev target):

```bash
rm -f sdkconfig.esp32s3
```

**Step 6:** Build and flash to the original ESP32:

```bash
~/.platformio/penv/bin/pio run -t upload -t monitor -d /Users/jasonmbp/gits/KeyBridge
```

**Step 7:** Put K2 into pairing mode (`Fn + 1` hold 3s). Watch serial output for Classic BT discovery:

```
[BT] GAP initialized
[BT] HID host initialized
[BT] Init complete
```

Press the PAIR button on KeyBridge. Watch for K2 appearing in the scan results.

**Step 8:** Commit.

```bash
git add platformio.ini sdkconfig.defaults src/keybridge.cpp
git commit -m "feat: port to original ESP32 with Classic BT support

ESP32-S3 can't do Classic BT (BLE only). The Keychron K2 uses
Classic BT exclusively. Original ESP32 supports Classic BT + BLE +
WiFi coexistence. USB host code guarded behind CONFIG_SOC_USB_OTG_SUPPORTED."
```

---

## Task 2: Replace Parallel Output Pins with Scan Input Pins in Config

**Files:**
- Modify: `src/config.h`

**Step 1:** Replace the 7 data output pins + strobe pin with 7 address input pins + 1 key return output pin. Change the `AdapterConfig` struct:

Replace these fields:
```cpp
int8_t pin_d0;
int8_t pin_d1;
int8_t pin_d2;
int8_t pin_d3;
int8_t pin_d4;
int8_t pin_d5;
int8_t pin_d6;
int8_t pin_strobe;
```

With:
```cpp
int8_t pin_addr[7];    // Address bits A0-A6 from terminal (inputs)
int8_t pin_key_return; // Key Return to terminal (output, active high = key pressed via MOSFET)
```

**Step 2:** Remove the strobe/timing fields that no longer apply:

Remove:
```cpp
bool strobe_active_low;
uint16_t strobe_pulse_us;
uint16_t data_setup_us;
uint16_t inter_char_delay_us;
```

**Step 3:** Update `setDefaultConfig()` — set default pin assignments:

```cpp
// Scan interface pins (match J3 wiring table above)
cfg.pin_addr[0] = 4;   // A0
cfg.pin_addr[1] = 5;   // A1
cfg.pin_addr[2] = 14;  // A2 (avoid GPIO 6-11 on original ESP32)
cfg.pin_addr[3] = 15;  // A3
cfg.pin_addr[4] = 13;  // A4
cfg.pin_addr[5] = 16;  // A5
cfg.pin_addr[6] = 17;  // A6
cfg.pin_key_return = 18;
```

**Step 4:** Bump the NVS config version from `5` to `6` in `saveConfig()` and `loadConfig()` so old configs get replaced with new defaults.

**Step 5:** Update `configToJson()` and `jsonToConfig()` to use the new pin field names. Update the JSON key names from `d0`..`d6`/`strobe` to `addr0`..`addr6`/`key_return`.

**Step 6:** Build to verify compilation:

```bash
~/.platformio/penv/bin/pio run -d /Users/jasonmbp/gits/KeyBridge
```

**Step 7:** Commit.

```bash
git add src/config.h
git commit -m "refactor: replace parallel output pins with scan input pins in config

The Wyse 50 keyboard connector (J3) uses a scan-response protocol,
not parallel ASCII output. 7 address input pins + 1 key return output."
```

---

## Task 3: Implement the Scan Response Engine

This is the core change — replace `sendChar()` / `sendString()` with a scan-response loop.

**Files:**
- Modify: `src/keybridge.cpp`

**Step 1:** Add a key state table and scan task. At the top of keybridge.cpp (global state section), add:

```cpp
// ============================================================
// KEYBOARD SCAN EMULATION
// ============================================================

// 128-entry table: true = key at this address is currently "pressed"
static volatile uint8_t key_state[128] = {0};

// Cached GPIO pin numbers for fast access in scan loop
static uint8_t scan_addr_pins[7];
static uint8_t scan_return_pin;
```

**Step 2:** Replace `setupOutputPins()` with `setupScanPins()`:

```cpp
void setupScanPins() {
    for (int i = 0; i < 7; i++) {
        scan_addr_pins[i] = config.pin_addr[i];
        if (scan_addr_pins[i] >= 0) {
            pinMode(scan_addr_pins[i], INPUT);
        }
    }
    scan_return_pin = config.pin_key_return;
    if (scan_return_pin >= 0) {
        pinMode(scan_return_pin, OUTPUT);
        digitalWrite(scan_return_pin, LOW); // MOSFET off = key not pressed
    }

    // Keep pair button, mode jumper, LEDs
    if (config.pin_pair_btn >= 0) pinMode(config.pin_pair_btn, INPUT_PULLUP);
    if (config.pin_mode_jp >= 0) pinMode(config.pin_mode_jp, INPUT_PULLUP);
    if (config.pin_led >= 0) {
        pinMode(config.pin_led, OUTPUT);
        digitalWrite(config.pin_led, LOW);
    }
    if (config.pin_bt_led >= 0) {
        pinMode(config.pin_bt_led, OUTPUT);
        digitalWrite(config.pin_bt_led, LOW);
    }
}
```

**Step 3:** Write the scan response task — a tight polling loop pinned to core 0:

```cpp
static void scan_response_task(void *arg) {
    // Remove this task from watchdog (tight loop would trigger it)
    esp_task_wdt_delete(NULL);

    // Pre-compute GPIO register masks for fast address reading
    uint32_t addr_masks[7];
    for (int i = 0; i < 7; i++) {
        addr_masks[i] = (1UL << scan_addr_pins[i]);
    }
    uint32_t return_mask = (1UL << scan_return_pin);

    ESP_LOGI(TAG, "[SCAN] Response task running on core %d", xPortGetCoreID());

    uint32_t yield_counter = 0;
    while (true) {
        // Read all GPIOs in one register read
        uint32_t gpio_in = REG_READ(GPIO_IN_REG);

        // Decode 7-bit address from physical GPIO states
        uint8_t addr = 0;
        for (int i = 0; i < 7; i++) {
            if (gpio_in & addr_masks[i]) {
                addr |= (1 << i);
            }
        }

        // Drive Key Return based on key state table
        if (key_state[addr]) {
            REG_WRITE(GPIO_OUT_W1TS_REG, return_mask); // HIGH = MOSFET on = key pressed
        } else {
            REG_WRITE(GPIO_OUT_W1TC_REG, return_mask); // LOW = MOSFET off = not pressed
        }

        // Yield briefly every ~100k iterations to let other core-0 tasks breathe
        if (++yield_counter >= 100000) {
            yield_counter = 0;
            vTaskDelay(1);
        }
    }
}
```

Note: On the original ESP32, GPIOs 4-18 are all in the 0-31 range so `GPIO_IN_REG` works. If any `pin_addr` values are >= 32, use `GPIO_IN1_REG` instead. Avoid GPIO 6-11 (connected to internal flash on most modules) — the default pin assignments below use GPIO 4, 5, 13, 14, 15, 16, 17 to stay safe on the original ESP32.

**Step 4:** Replace `sendChar()` and `sendString()` with key press/release functions that update `key_state[]`:

```cpp
// Press a key at the given Wyse 50 scan address
void scanKeyPress(uint8_t addr) {
    if (addr < 128) {
        key_state[addr] = 1;
        logKey("PRESS: addr=0x%02X", addr);
    }
}

// Release a key at the given Wyse 50 scan address
void scanKeyRelease(uint8_t addr) {
    if (addr < 128) {
        key_state[addr] = 0;
    }
}

// Release all keys
void scanReleaseAll() {
    memset((void *)key_state, 0, sizeof(key_state));
}
```

**Step 5:** Launch the scan response task in `app_main()`, replacing `setupOutputPins()`:

Replace `setupOutputPins();` with:
```cpp
setupScanPins();

// Start scan response on core 0 at highest priority
xTaskCreatePinnedToCore(scan_response_task, "scan", 4096, NULL,
                        configMAX_PRIORITIES - 1, NULL, 0);
```

**Step 6:** Build and verify compilation.

**Step 7:** Commit.

```bash
git add src/keybridge.cpp
git commit -m "feat: implement scan response engine for Wyse 50 keyboard emulation

Replaces parallel ASCII output with a scan-response loop that reads
7-bit addresses from the terminal's J3 connector and drives Key Return
via a MOSFET. Runs on core 0 with <1μs response time."
```

---

## Task 4: HID-to-Wyse50 Key Address Mapping

The complete key matrix has been decoded from the MAME emulator project (`src/mame/wyse/wy50kb.cpp` in the [mamedev/mame](https://github.com/mamedev/mame) repository). The address formula is `(column * 8) + row`, where bits 6-3 = column (0-12) and bits 2-0 = row (0-7).

**Files:**
- Modify: `src/keybridge.cpp`

**Step 1:** Create the complete mapping table. Every address is known — no placeholders.

```cpp
// Wyse 50 scan address for Shift and Ctrl modifier keys
#define WYSE_SHIFT  0x4A  // Col 9, Row 2
#define WYSE_CTRL   0x1F  // Col 3, Row 7

// Map HID keycode → Wyse 50 scan address (0-127)
// 0xFF = no mapping (key not present on Wyse 50)
// Source: MAME wy50kb.cpp (verified against maintenance manual schematic)
static const uint8_t hid_to_wyse50[256] = {
    [0 ... 255] = 0xFF,  // Default: unmapped

    // Letters (HID 0x04-0x1D = a-z)
    [0x04] = 0x3F, // a → Col 7, Row 7
    [0x05] = 0x2E, // b → Col 5, Row 6
    [0x06] = 0x4E, // c → Col 9, Row 6
    [0x07] = 0x37, // d → Col 6, Row 7
    [0x08] = 0x30, // e → Col 6, Row 0
    [0x09] = 0x17, // f → Col 2, Row 7
    [0x0A] = 0x0F, // g → Col 1, Row 7
    [0x0B] = 0x07, // h → Col 0, Row 7
    [0x0C] = 0x58, // i → Col 11, Row 0
    [0x0D] = 0x5F, // j → Col 11, Row 7
    [0x0E] = 0x67, // k → Col 12, Row 7
    [0x0F] = 0x2F, // l → Col 5, Row 7
    [0x10] = 0x0E, // m → Col 1, Row 6
    [0x11] = 0x16, // n → Col 2, Row 6
    [0x12] = 0x60, // o → Col 12, Row 0
    [0x13] = 0x51, // p → Col 10, Row 1
    [0x14] = 0x38, // q → Col 7, Row 0
    [0x15] = 0x28, // r → Col 5, Row 0
    [0x16] = 0x4F, // s → Col 9, Row 7
    [0x17] = 0x10, // t → Col 2, Row 0
    [0x18] = 0x00, // u → Col 0, Row 0
    [0x19] = 0x36, // v → Col 6, Row 6
    [0x1A] = 0x48, // w → Col 9, Row 0
    [0x1B] = 0x3E, // x → Col 7, Row 6
    [0x1C] = 0x08, // y → Col 1, Row 0
    [0x1D] = 0x1E, // z → Col 3, Row 6

    // Number row (HID 0x1E-0x27 = 1-0)
    [0x1E] = 0x1B, // 1/! → Col 3, Row 3
    [0x1F] = 0x3B, // 2/@ → Col 7, Row 3
    [0x20] = 0x4B, // 3/# → Col 9, Row 3
    [0x21] = 0x33, // 4/$ → Col 6, Row 3
    [0x22] = 0x2B, // 5/% → Col 5, Row 3
    [0x23] = 0x13, // 6/^ → Col 2, Row 3
    [0x24] = 0x0B, // 7/& → Col 1, Row 3
    [0x25] = 0x03, // 8/* → Col 0, Row 3
    [0x26] = 0x5B, // 9/( → Col 11, Row 3
    [0x27] = 0x63, // 0/) → Col 12, Row 3

    // Common keys
    [0x28] = 0x65, // Return    → Col 12, Row 5
    [0x29] = 0x3C, // Escape    → Col 7, Row 4
    [0x2A] = 0x1A, // Backspace → Col 3, Row 2
    [0x2B] = 0x18, // Tab       → Col 3, Row 0
    [0x2C] = 0x19, // Space     → Col 3, Row 1

    // Punctuation
    [0x2D] = 0x43, // -/_ → Col 8, Row 3
    [0x2E] = 0x53, // =/+ → Col 10, Row 3
    [0x2F] = 0x42, // [/{ → Col 8, Row 2
    [0x30] = 0x45, // ]/} → Col 8, Row 5
    [0x31] = 0x5C, // \/| → Col 11, Row 4
    [0x33] = 0x44, // ;/: → Col 8, Row 4
    [0x34] = 0x46, // '/" → Col 8, Row 6
    [0x35] = 0x4C, // `/~ → Col 9, Row 4
    [0x36] = 0x06, // ,/< → Col 0, Row 6
    [0x37] = 0x5E, // ./> → Col 11, Row 6
    [0x38] = 0x66, // //? → Col 12, Row 6

    // Lock / special
    [0x39] = 0x3A, // Caps Lock → Col 7, Row 2

    // Function keys (F1-F12 map to Wyse F1-F12)
    [0x3A] = 0x1D, // F1  → Col 3, Row 5
    [0x3B] = 0x3D, // F2  → Col 7, Row 5
    [0x3C] = 0x25, // F3  → Col 4, Row 5
    [0x3D] = 0x23, // F4  → Col 4, Row 3
    [0x3E] = 0x20, // F5  → Col 4, Row 0
    [0x3F] = 0x27, // F6  → Col 4, Row 7
    [0x40] = 0x26, // F7  → Col 4, Row 6
    [0x41] = 0x49, // F8  → Col 9, Row 1
    [0x42] = 0x24, // F9  → Col 4, Row 4
    [0x43] = 0x1C, // F10 → Col 3, Row 4
    [0x44] = 0x57, // F11 → Col 10, Row 7
    [0x45] = 0x22, // F12 → Col 4, Row 2

    // Wyse-specific keys mapped to HID keys that don't conflict
    [0x47] = 0x0C, // Scroll Lock → SETUP (Col 1, Row 4) *** CRITICAL ***
    [0x48] = 0x34, // Pause/Break → Break (Col 6, Row 4)
    [0x49] = 0x01, // Insert      → Ins Char/Line (Col 0, Row 1)
    [0x4A] = 0x61, // Home        → Home (Col 12, Row 1)
    [0x4B] = 0x41, // Page Up     → Next/Prev Page (Col 8, Row 1)
    [0x4C] = 0x62, // Delete      → Del 0x7F (Col 12, Row 2)
    [0x4E] = 0x41, // Page Down   → Next/Prev Page (same key, shifted)

    // Arrow keys
    [0x4F] = 0x0A, // Right → Col 1, Row 2
    [0x50] = 0x5A, // Left  → Col 11, Row 2
    [0x51] = 0x05, // Down  → Col 0, Row 5
    [0x52] = 0x4D, // Up    → Col 9, Row 5

    // Keypad
    [0x54] = 0x66, // KP /     → //? (shared)
    [0x56] = 0x31, // KP -     → Col 6, Row 1
    [0x58] = 0x35, // KP Enter → Col 6, Row 5
    [0x59] = 0x12, // KP 1     → Col 2, Row 2
    [0x5A] = 0x02, // KP 2     → Col 0, Row 2
    [0x5B] = 0x52, // KP 3     → Col 10, Row 2
    [0x5C] = 0x11, // KP 4     → Col 2, Row 1
    [0x5D] = 0x2A, // KP 5     → Col 5, Row 2
    [0x5E] = 0x2C, // KP 6     → Col 5, Row 4
    [0x5F] = 0x14, // KP 7     → Col 2, Row 4
    [0x60] = 0x55, // KP 8     → Col 10, Row 5
    [0x61] = 0x59, // KP 9     → Col 11, Row 1
    [0x62] = 0x15, // KP 0     → Col 2, Row 5
    [0x63] = 0x29, // KP .     → Col 5, Row 1
};

// Additional Wyse keys with no obvious HID equivalent (accessible via web UI):
// Func        = 0x39 (Col 7, Row 1)
// Clr Line    = 0x04 (Col 0, Row 4) — Shift+Clr = Clr Scrn
// Del Char    = 0x2D (Col 5, Row 5) — Shift+Del Char = Del Line
// Repl/Ins    = 0x32 (Col 6, Row 2)
// Send/Print  = 0x64 (Col 12, Row 4)
// F13         = 0x50 (Col 10, Row 0)
// F14         = 0x54 (Col 10, Row 4)
// F15         = 0x56 (Col 10, Row 6)
// F16         = 0x21 (Col 4, Row 1)
```

**Step 2:** Rewrite `processHidReport()` to handle key press AND release via the scan state table, including modifier keys:

```cpp
void processHidReport(const KeyReport *report) {
    uint8_t modifiers = report->modifiers;
    const uint8_t *keys = report->keys;

    // Track which Wyse addresses are currently held
    static uint8_t prev_wyse_addrs[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static uint8_t prev_modifiers = 0;

    // Release previous keys not in current report
    for (int i = 0; i < 6; i++) {
        if (prev_wyse_addrs[i] == 0xFF) continue;
        bool still_held = false;
        for (int j = 0; j < 6; j++) {
            uint8_t wa = (keys[j] != 0) ? hid_to_wyse50[keys[j]] : 0xFF;
            if (wa == prev_wyse_addrs[i]) { still_held = true; break; }
        }
        if (!still_held) {
            scanKeyRelease(prev_wyse_addrs[i]);
            prev_wyse_addrs[i] = 0xFF;
        }
    }

    // Press new keys
    for (int i = 0; i < 6; i++) {
        if (keys[i] == 0) { prev_wyse_addrs[i] = 0xFF; continue; }
        uint8_t addr = hid_to_wyse50[keys[i]];
        if (addr == 0xFF) { prev_wyse_addrs[i] = 0xFF; continue; }
        scanKeyPress(addr);
        prev_wyse_addrs[i] = addr;
    }

    // Handle modifier keys — Shift and Ctrl have physical scan addresses
    bool shift_now = (modifiers & 0x22) != 0; // L or R Shift
    bool shift_was = (prev_modifiers & 0x22) != 0;
    if (shift_now && !shift_was) scanKeyPress(WYSE_SHIFT);
    if (!shift_now && shift_was) scanKeyRelease(WYSE_SHIFT);

    bool ctrl_now = (modifiers & 0x11) != 0;  // L or R Ctrl
    bool ctrl_was = (prev_modifiers & 0x11) != 0;
    if (ctrl_now && !ctrl_was) scanKeyPress(WYSE_CTRL);
    if (!ctrl_now && ctrl_was) scanKeyRelease(WYSE_CTRL);

    prev_modifiers = modifiers;

    // LED feedback
    for (int i = 0; i < 6; i++) {
        if (keys[i] != 0) {
            if (config.pin_led >= 0) digitalWrite(config.pin_led, HIGH);
            ledOffTime = millis() + 30;
            break;
        }
    }
}
```

**Step 3:** Remove `handleAutoRepeat()` — the terminal handles key repeat natively by seeing the key still held during repeated scans. The BT HID report holds the key in the `keys[]` array as long as it's physically held, which maps directly to the scan state table staying asserted. No software repeat needed.

**Step 4:** Build, flash, verify no crashes.

**Step 5:** Commit.

```bash
git add src/keybridge.cpp
git commit -m "feat: add complete HID-to-Wyse50 key address mapping

All 100+ key addresses derived from MAME wy50kb.cpp (verified against
the WY-50 maintenance manual schematic). Shift/Ctrl modifiers mapped
to their physical scan addresses. No empirical discovery needed."
```

---

## Task 5: Build Scan Verification and Debug Tools

The key addresses are known from MAME, but we still need tools to verify the hardware connection works and debug any wiring issues.

**Files:**
- Modify: `src/keybridge.cpp` (add API endpoints)

**Step 1:** Add a scan snoop mode that logs which addresses the terminal is scanning and at what rate:

```cpp
static volatile bool scan_snoop_mode = false;
static volatile uint32_t scan_addr_histogram[128] = {0};
static volatile uint32_t scan_last_addr = 0xFF;
static volatile uint32_t scan_total_count = 0;
```

In the scan response task, add histogram tracking when snoop mode is active:

```cpp
// Inside scan_response_task loop, after reading addr:
if (scan_snoop_mode) {
    scan_addr_histogram[addr]++;
    scan_last_addr = addr;
    scan_total_count++;
}
```

**Step 2:** Add an API endpoint `POST /api/scan/snoop` to start/stop snoop mode and `GET /api/scan/snoop` to read results:

```cpp
// Start/stop scan snooping
server.on("/api/scan/snoop", HTTP_POST, []() {
    if (!isAuthenticated()) { sendUnauthorized(); return; }
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    bool enable = doc["enable"] | false;
    if (enable) {
        memset((void *)scan_addr_histogram, 0, sizeof(scan_addr_histogram));
        scan_total_count = 0;
        scan_snoop_mode = true;
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Snoop started\"}");
    } else {
        scan_snoop_mode = false;
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Snoop stopped\"}");
    }
});

// Read scan histogram
server.on("/api/scan/histogram", HTTP_GET, []() {
    if (!isAuthenticated()) { sendUnauthorized(); return; }
    scan_snoop_mode = false; // Pause while reading
    JsonDocument doc;
    doc["total_scans"] = scan_total_count;
    doc["last_addr"] = scan_last_addr;
    JsonArray addrs = doc["addresses"].to<JsonArray>();
    for (int i = 0; i < 128; i++) {
        if (scan_addr_histogram[i] > 0) {
            JsonObject e = addrs.add<JsonObject>();
            e["addr"] = i;
            e["count"] = scan_addr_histogram[i];
            e["col"] = i & 0x0F;
            e["row"] = (i >> 4) & 0x07;
        }
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
});
```

**Step 3:** Add a key test endpoint `POST /api/scan/test` that asserts a specific address for a given duration to see what character appears on the terminal:

```cpp
server.on("/api/scan/test", HTTP_POST, []() {
    if (!isAuthenticated()) { sendUnauthorized(); return; }
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    uint8_t addr = doc["addr"] | 0xFF;
    uint16_t duration_ms = doc["duration_ms"] | 200;
    if (addr >= 128) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"addr must be 0-127\"}");
        return;
    }
    if (duration_ms > 5000) duration_ms = 5000;

    // Press and hold the key for the specified duration
    scanKeyPress(addr);
    server.send(200, "application/json", "{\"ok\":true}");
    delay(duration_ms);
    scanKeyRelease(addr);
});
```

**Step 4:** Add a bulk scan test endpoint `POST /api/scan/sweep` that systematically tests all 128 addresses one at a time (for discovering the full keyboard layout):

```cpp
server.on("/api/scan/sweep", HTTP_POST, []() {
    if (!isAuthenticated()) { sendUnauthorized(); return; }
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    uint8_t start = doc["start"] | 0;
    uint8_t end = doc["end"] | 127;
    uint16_t hold_ms = doc["hold_ms"] | 300;
    uint16_t gap_ms = doc["gap_ms"] | 200;

    // Sweep will run in background — just start it
    // The user watches the terminal screen to see what appears
    logKey("[SCAN] Sweep %d-%d, hold=%dms", start, end, hold_ms);
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Sweep started\"}");

    for (uint8_t addr = start; addr <= end && addr < 128; addr++) {
        logKey("[SCAN] Testing addr=0x%02X (col=%d row=%d)", addr, addr & 0x0F, addr >> 4);
        scanKeyPress(addr);
        delay(hold_ms);
        scanKeyRelease(addr);
        delay(gap_ms);
    }
    logKey("[SCAN] Sweep complete");
});
```

**Step 5:** Build, flash, test by connecting to the Wyse 50 and using the web UI to:
1. Start snoop mode → verify the terminal is actually scanning all 104 addresses (13 cols × 8 rows)
2. Test a known address (e.g., `0x3F` = 'a') → verify 'a' appears on terminal
3. Test SETUP (`0x0C`) → verify terminal enters setup mode
4. Run a sweep of a small range to verify address wiring is correct

**Step 6:** Commit.

```bash
git add src/keybridge.cpp
git commit -m "feat: add scan verification and debug tools

API endpoints for verifying the Wyse 50 keyboard interface:
- /api/scan/snoop - monitor which addresses the terminal scans
- /api/scan/histogram - read scan frequency per address
- /api/scan/test - assert a single address to verify wiring
- /api/scan/sweep - test address ranges sequentially"
```

---

## Task 6: Update Web UI for Scan Interface

**Files:**
- Modify: `src/web_ui.h`

**Step 1:** Update the pin configuration section to show the new pin names (addr0-addr6, key_return) instead of d0-d6/strobe.

**Step 2:** Replace the "Test Character" section with a "Scan Test" section that lets you:
- Enter an address (0-127) and press a button to test it
- Run a sweep of an address range
- View the scan snoop histogram

**Step 3:** Add a visual keyboard map builder — a simple grid showing which address maps to which key. Start empty, let the user fill in as they discover addresses from testing.

**Step 4:** Build, flash, verify web UI works.

**Step 5:** Commit.

---

## Task 7: End-to-End Verification with Wyse 50

Connect everything and verify the full chain: BT keyboard → ESP32 → TXS0108E → J3 → terminal.

**Step 1:** Verify scan snoop shows the terminal scanning all expected addresses (should see ~104 active addresses across columns 0-12).

**Step 2:** Use `/api/scan/test` to assert address `0x0C` (SETUP) — verify the terminal enters setup mode. This confirms the entire hardware chain works.

**Step 3:** Pair the Keychron K2 via Classic BT. Press Scroll Lock — should trigger SETUP on the terminal.

**Step 4:** Type letters on the K2 and verify they appear on the terminal screen. Test a-z, 0-9, space, return, backspace.

**Step 5:** Test Shift + letter (should produce uppercase). Test Ctrl + letter. Test arrow keys navigate in setup mode.

**Step 6:** If any key doesn't work, use the scan snoop and test tools to debug. The MAME mapping is authoritative but wiring errors could swap address bits.

**Step 7:** Commit any fixes discovered during testing.

```bash
git add src/keybridge.cpp
git commit -m "fix: correct key address mapping from end-to-end testing"
```

---

## Task 8: Clean Up Removed Code

**Files:**
- Modify: `src/keybridge.cpp`
- Modify: `src/config.h`
- Modify: `src/web_ui.h`

**Step 1:** Remove dead code:
- `sendChar()`, `sendString()` — replaced by scan state table
- `handleAutoRepeat()` — terminal handles repeat natively
- `hid_to_ascii_lower[]`, `hid_to_ascii_upper[]` — no longer needed
- `getSpecialSequence()` and the special key escape sequence mapping — the Wyse 50 keyboard sends raw scan codes, not escape sequences. Special keys (F1-F16, arrows) each have their own scan address.
- `SpecialKeyEntry` struct and related config — the Wyse 50 doesn't use escape sequences from the keyboard; each key is just an address

**Step 2:** Remove the `ansi_mode` concept — this was for choosing between escape sequence sets, but with direct scan matrix emulation, the terminal's own firmware handles all escape sequence generation. The keyboard just sends key addresses.

**Step 3:** Remove the terminal presets API endpoints (`/api/preset/wyse50`, `/api/preset/vt100`, `/api/preset/adm3a`) — these mapped to escape sequences which are no longer relevant.

**Step 4:** Build, verify, commit.

```bash
git add src/keybridge.cpp src/config.h src/web_ui.h
git commit -m "refactor: remove parallel ASCII output code and escape sequence mapping

With scan matrix emulation, the terminal's own firmware handles all
character generation and escape sequences. The keyboard just provides
raw key addresses."
```

---

## Notes

### What about other terminals (VT100, ADM-3A)?

The parallel ASCII approach may have been correct for the ADM-3A (which uses a parallel ASCII keyboard encoder). The VT100 uses a serial UART keyboard interface. This plan focuses exclusively on the Wyse 50. Support for other terminal types would need separate output driver implementations, selectable via config. That's future work.

### What about USB keyboards?

The original ESP32 has no native USB host controller. USB keyboard support would require either: (a) porting back to ESP32-S3 with the D7 diode bridge and OTG adapter hardware fixes, or (b) adding a USB host shield (MAX3421E) via SPI. For now, Classic BT via the Keychron K2 is the primary input method. BLE keyboards (Apple Magic Keyboard, Logitech MX Keys Mini) also work since the original ESP32 supports BLE alongside Classic BT.

### What about ESP32-S3?

The USB host code is preserved behind `#if CONFIG_SOC_USB_OTG_SUPPORTED` guards. If a future use case needs USB host input, the S3 can be re-enabled by uncommenting its env in `platformio.ini`. The scan response engine is identical regardless of input source — only the HID input path differs.

### Key address mapping source

The complete keyboard matrix was extracted from the MAME emulator project: [`src/mame/wyse/wy50kb.cpp`](https://github.com/mamedev/mame/blob/master/src/mame/wyse/wy50kb.cpp) in the [mamedev/mame](https://github.com/mamedev/mame) repository. This file was authored by AJR (a well-known MAME contributor) and represents the decoded schematic from the WY-50 maintenance manual. The address formula is `(column * 8) + row` where bits 6-3 = column (0-12) and bits 2-0 = row (0-7). The Key Return line is active-low (goes low when the addressed key is pressed). Additional community resources: [Deskthority Wyse keyboard protocols](https://deskthority.net/wiki/Wyse_keyboard_protocols), [bitsavers WY-50 manuals](https://bitsavers.org/pdf/wyse/WY-50/).

### Config version bump

Task 2 bumps the NVS config version from 5 to 6. First boot after flashing will reset to defaults. This is expected since the struct layout changes.
