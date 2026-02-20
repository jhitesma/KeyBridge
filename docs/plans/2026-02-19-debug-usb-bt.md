# Debug Session Continuation: USB + BLE Keyboard Connection

## Status
Neither USB nor BLE keyboard connections are working. BLE init completes successfully now but keyboards aren't found during scan. USB host library initializes but never detects a device.

## Hardware Setup
- **Board:** ESP32-S3 DevKitC-1
- **USB keyboard:** Keychron K2 (USB-C), connected via USB-C to USB-C cable to the USB-OTG port (the non-COM port)
- **Serial monitor:** Connected via the COM/UART port
- **BLE keyboard:** Same Keychron K2 in BLE mode (visible from macOS, confirmed in pairing mode)

## What Was Fixed This Session

### BLE (3 issues found and fixed):
1. **`sdkconfig.esp32s3` overriding `sdkconfig.defaults`** — PlatformIO auto-generates this file. Deleted it so defaults take effect. NOTE: It gets regenerated every build. The BT Classic settings in sdkconfig.defaults (CONFIG_BT_CLASSIC_ENABLED, CONFIG_BT_HID_HOST_ENABLED) are silently ignored because **ESP32-S3 only supports BLE, not Classic BT**. Updated sdkconfig.defaults to remove stale Classic BT settings.

2. **Missing GATTC callback registration** — `esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler)` was never called before `esp_hidh_init()`. The esp_hid component's `ble_hidh.c` requires the application to register this callback (see `esp_hidh_gattc.h` header comment). Without it, `esp_ble_hidh_init()` hangs forever at `WAIT_CB()` after `esp_ble_gattc_app_register(0)`. Fixed in `bt_init_task()` in keybridge.cpp.

3. **BLE scan filter too strict** — `esp_hid_gap.c:handle_ble_device_result()` only checked `ESP_BLE_AD_TYPE_16SRV_CMPL` for UUID 0x1812. Added fallback to `ESP_BLE_AD_TYPE_16SRV_PART` (incomplete UUID list) and also accept devices with HID-class appearance (0x03C0-0x03CF).

### USB (1 issue found and fixed):
4. **`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` was enabled** — This claims GPIO19/20 for the USB Serial/JTAG controller, preventing the USB OTG host controller from using those pins. Added `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` to sdkconfig.defaults.

## Current State After Fixes

### BLE — scan runs but Keychron K2 not found:
```
[BT] GAP initialized (heap=168680)
[BT] Initializing HID host...
[BT] HID host initialized (heap=163704)
[BT] Init complete
```
BLE scan finds ~40+ devices (Mi bands, Samsung devices, unnamed random-address devices) but NONE have UUID 0x1812 or HID appearance. The Keychron K2 is likely one of the unnamed RSSI -8 devices but doesn't advertise the HID service UUID or appearance in its advertisement data. Many keyboards only expose HID service UUID after GATT connection + service discovery, not in advertisements.

**Next steps for BLE:**
- The Keychron K2 may be advertising via Classic BT (not BLE) when in pairing mode. ESP32-S3 can't see Classic BT. Need to verify the keyboard is actually in BLE mode specifically.
- If it IS advertising via BLE without HID UUID/appearance, the scan filter needs to be even more permissive — possibly accept all named devices, or let the user pick from all scan results via the web UI.
- Consider connecting to candidate devices and doing GATT service discovery to check for HID service, rather than filtering on advertisement data alone.
- Look at what Keychron K2 actually advertises — could use a BLE scanner app (nRF Connect on phone) to see the exact advertisement data.

### USB — host running but no device detected:
```
[USB] Host library installing...
[USB] Host library ready, waiting for devices
[USB] Client registered, polling for keyboards
```
No `[USB] New device detected` ever appears despite keyboard being plugged into USB-OTG port. The CONFIG_ESP_CONSOLE_SECONDARY fix was applied but hasn't been verified to work yet (needs testing with latest build).

**Next steps for USB:**
- Verify `CONFIG_ESP_CONSOLE_SECONDARY_NONE` actually took effect in the compiled sdkconfig.h (grep the built config)
- Check if the USB PHY is actually detecting a device connection at a lower level — could add USB host library event logging or check the OTG controller registers
- The ESP32-S3 DevKitC-1 USB-OTG port may need external 5V VBUS power delivery that isn't present when powered only via UART port. Check the board schematic for VBUS routing.
- USB-C to USB-C might have CC pin negotiation issues — the DevKitC-1 may not have proper CC pull-downs for host role on its USB-C connector

## Key Files
- `src/keybridge.cpp` — Main firmware, BT init at `bt_init_task()`, USB host at `startUsbHost()`
- `src/esp_hid_gap.c` — BLE/BT GAP initialization and scan logic (modified from ESP-IDF example)
- `src/esp_hid_gap.h` — Header for gap functions
- `sdkconfig.defaults` — ESP-IDF Kconfig overrides (the source of truth; sdkconfig.esp32s3 is auto-generated)
- `platformio.ini` — Build config (combined arduino+espidf framework)

## Build/Flash Command
```bash
~/.platformio/penv/bin/pio run -t upload -t monitor -d /Users/jasonmbp/gits/KeyBridge
```

## Important Constraints
- ESP32-S3 does NOT support Classic Bluetooth — only BLE
- `sdkconfig.esp32s3` is auto-generated by PlatformIO on every build — don't edit it, edit `sdkconfig.defaults` instead
- Do not format `src/web_ui.h` — it has clang-format guards around PROGMEM HTML
- USB host needs `ARDUINO_USB_MODE=0` and `ARDUINO_USB_CDC_ON_BOOT=0` (set via board_build in platformio.ini)
- No USB hub support in current code (open issue)
