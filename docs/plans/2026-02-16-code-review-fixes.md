# Code Review Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Address all substantive issues found in the comprehensive code review across all source files.

**Architecture:** Six task groups ordered by dependency — build config first (foundation), then config persistence (data integrity), USB host (critical bugs), Bluetooth safety (security), web UI (user-facing), and remaining firmware hardening. Each task group targets one file to minimize merge conflicts.

**Tech Stack:** C/C++ (ESP-IDF + Arduino), PlatformIO, ArduinoJson 7.x, FreeRTOS, vanilla JS

**Verification:** After each task, run `pio run` to confirm the project compiles. No unit test framework is present — hardware testing is manual.

---

### Task 1: Pin platform and library versions in platformio.ini

**Files:**
- Modify: `platformio.ini`

**Why:** Unpinned platform means non-reproducible builds. ArduinoJson 7.0.0 has known ESP32 memory issues.

**Step 1: Pin platform and ArduinoJson versions**

```ini
platform = espressif32@^6.9.0
```

```ini
lib_deps =
    bblanchon/ArduinoJson@~7.4.0
```

**Step 2: Compile to verify**

Run: `pio run`
Expected: Successful build

**Step 3: Commit**

```
fix: pin platform and ArduinoJson versions for reproducible builds
```

---

### Task 2: Simplify partition table (remove unused OTA/SPIFFS/coredump)

**Files:**
- Modify: `partitions.csv`

**Why:** Current table wastes ~3MB on unused features (dual OTA with no OTA code, SPIFFS with no filesystem code, coredump unused). A single app partition is simpler and provides more headroom.

**Step 1: Replace partition table with single-app layout**

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x7E0000,
```

This gives a single ~8MB app partition (plenty for USB+BT+WiFi firmware). The `otadata` partition is kept because the Arduino-ESP-IDF combined framework expects it.

**Step 2: Compile to verify**

Run: `pio run`
Expected: Successful build, linker reports plenty of flash remaining

**Step 3: Commit**

```
fix: simplify partition table to single app partition

Remove unused dual-OTA, SPIFFS, and coredump partitions.
The firmware has no OTA update code or filesystem usage.
```

---

### Task 3: Harden config persistence in config.h

**Files:**
- Modify: `src/config.h`

**Why:** Multiple data integrity issues — version mismatch loads corrupt data, no input validation on dangerous parameters, no bounds checks on serialization, no null-termination guarantee after NVS load.

**Step 1: Fix version check in loadConfig()**

In `loadConfig()` (line 238), change `version == 0` to `version != 5`:

```cpp
if (version != 5) {
    prefs.end();
    return false; // No saved config or version mismatch
}
```

**Step 2: Add null-termination after NVS load**

After the `readLen` check in `loadConfig()`, add explicit null-termination for all string fields:

```cpp
if (readLen != sizeof(cfg)) return false;

// Ensure string fields are null-terminated after blob load
cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
cfg.wifi_password[sizeof(cfg.wifi_password) - 1] = '\0';
cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1] = '\0';
cfg.sta_password[sizeof(cfg.sta_password) - 1] = '\0';
cfg.hostname[sizeof(cfg.hostname) - 1] = '\0';
for (int i = 0; i < MAX_SPECIAL_KEYS; i++) {
    cfg.special_keys[i].label[sizeof(cfg.special_keys[i].label) - 1] = '\0';
    cfg.special_keys[i].seq_native[sizeof(cfg.special_keys[i].seq_native) - 1] = '\0';
    cfg.special_keys[i].seq_ansi[sizeof(cfg.special_keys[i].seq_ansi) - 1] = '\0';
}

// Clamp num_special_keys to valid range
if (cfg.num_special_keys > MAX_SPECIAL_KEYS) {
    cfg.num_special_keys = MAX_SPECIAL_KEYS;
}

return true;
```

**Step 3: Add input validation to jsonToConfig()**

Add a validation helper above `jsonToConfig()`:

```cpp
static bool isValidGPIO(int8_t pin) {
    if (pin == -1) return true;
    if (pin < 0 || pin > 48) return false;
    if (pin >= 26 && pin <= 32) return false; // SPI flash/PSRAM
    return true;
}
```

Wrap each pin assignment in `jsonToConfig()` with validation:

```cpp
if (pins.containsKey("d0")) { int8_t v = pins["d0"]; if (isValidGPIO(v)) cfg.pin_d0 = v; }
if (pins.containsKey("d1")) { int8_t v = pins["d1"]; if (isValidGPIO(v)) cfg.pin_d1 = v; }
if (pins.containsKey("d2")) { int8_t v = pins["d2"]; if (isValidGPIO(v)) cfg.pin_d2 = v; }
if (pins.containsKey("d3")) { int8_t v = pins["d3"]; if (isValidGPIO(v)) cfg.pin_d3 = v; }
if (pins.containsKey("d4")) { int8_t v = pins["d4"]; if (isValidGPIO(v)) cfg.pin_d4 = v; }
if (pins.containsKey("d5")) { int8_t v = pins["d5"]; if (isValidGPIO(v)) cfg.pin_d5 = v; }
if (pins.containsKey("d6")) { int8_t v = pins["d6"]; if (isValidGPIO(v)) cfg.pin_d6 = v; }
if (pins.containsKey("strobe")) { int8_t v = pins["strobe"]; if (isValidGPIO(v)) cfg.pin_strobe = v; }
if (pins.containsKey("pair_btn")) { int8_t v = pins["pair_btn"]; if (isValidGPIO(v)) cfg.pin_pair_btn = v; }
if (pins.containsKey("mode_jp")) { int8_t v = pins["mode_jp"]; if (isValidGPIO(v)) cfg.pin_mode_jp = v; }
if (pins.containsKey("led")) { int8_t v = pins["led"]; if (isValidGPIO(v)) cfg.pin_led = v; }
if (pins.containsKey("bt_led")) { int8_t v = pins["bt_led"]; if (isValidGPIO(v)) cfg.pin_bt_led = v; }
```

Add WiFi channel validation:

```cpp
if (w.containsKey("ap_channel")) {
    uint8_t ch = w["ap_channel"];
    if (ch >= 1 && ch <= 13) cfg.wifi_channel = ch;
}
```

Add timing parameter validation:

```cpp
if (t.containsKey("strobe_pulse_us")) { uint16_t v = t["strobe_pulse_us"]; if (v >= 1 && v <= 1000) cfg.strobe_pulse_us = v; }
if (t.containsKey("data_setup_us")) { uint16_t v = t["data_setup_us"]; if (v >= 1 && v <= 100) cfg.data_setup_us = v; }
if (t.containsKey("inter_char_delay_us")) { uint16_t v = t["inter_char_delay_us"]; if (v <= 10000) cfg.inter_char_delay_us = v; }
if (t.containsKey("repeat_delay_ms")) { uint16_t v = t["repeat_delay_ms"]; if (v >= 50 && v <= 5000) cfg.repeat_delay_ms = v; }
if (t.containsKey("repeat_rate_ms")) { uint16_t v = t["repeat_rate_ms"]; if (v >= 10 && v <= 1000) cfg.repeat_rate_ms = v; }
```

**Step 4: Bounds-check num_special_keys in configToJson()**

In `configToJson()` (line 353), cap the loop:

```cpp
int count = (cfg.num_special_keys <= MAX_SPECIAL_KEYS) ? cfg.num_special_keys : MAX_SPECIAL_KEYS;
for (int i = 0; i < count; i++) {
```

**Step 5: Compile to verify**

Run: `pio run`
Expected: Successful build

**Step 6: Commit**

```
fix: harden config persistence and add input validation

- Reject mismatched config versions in loadConfig()
- Null-terminate all string fields after NVS blob load
- Validate GPIO pins against ESP32-S3 reserved pins
- Validate WiFi channel (1-13) and timing parameters
- Bounds-check num_special_keys in serialization
```

---

### Task 4: Fix USB host resource management in keybridge.cpp

**Files:**
- Modify: `src/keybridge.cpp`

**Why:** Three critical bugs: memory leak on disconnect (transfer buffer never freed), USB interface never released, and race condition in transfer callback.

**Step 1: Track claimed interface number**

Add a global after line 353:

```cpp
static uint8_t usb_claimed_iface = 0xFF;
```

**Step 2: Fix the disconnect handler to release resources**

Replace the `USB_HOST_CLIENT_EVENT_DEV_GONE` case (lines 368-375):

```cpp
case USB_HOST_CLIENT_EVENT_DEV_GONE:
    usb_keyboard_connected = false;
    if (usb_xfer_in) {
        usb_host_transfer_free(usb_xfer_in);
        usb_xfer_in = NULL;
    }
    if (usb_dev_hdl) {
        if (usb_claimed_iface != 0xFF) {
            usb_host_interface_release(usb_client_hdl, usb_dev_hdl, usb_claimed_iface);
            usb_claimed_iface = 0xFF;
        }
        usb_host_device_close(usb_client_hdl, usb_dev_hdl);
        usb_dev_hdl = NULL;
    }
    logKey("[USB] Disconnected");
    break;
```

**Step 3: Record claimed interface on connect**

At line 437, after the `usb_host_interface_claim` call, record the interface:

```cpp
if (usb_host_interface_claim(usb_client_hdl, usb_dev_hdl, iface, 0) != ESP_OK) break;
usb_claimed_iface = iface;
```

**Step 4: Harden the transfer callback against races**

Replace the transfer callback (lines 356-361):

```cpp
static void usb_transfer_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes >= 8) {
        submitKeyReport(transfer->data_buffer[0], &transfer->data_buffer[2]);
    }
    if (usb_keyboard_connected && usb_dev_hdl != NULL) {
        if (usb_host_transfer_submit(transfer) != ESP_OK) {
            usb_keyboard_connected = false;
        }
    }
}
```

**Step 5: Replace busy-wait in usb_set_boot_protocol with semaphore**

Replace the function (lines 381-402):

```cpp
static void usb_set_boot_protocol(usb_device_handle_t dev, uint8_t iface) {
    usb_transfer_t *ctrl;
    usb_host_transfer_alloc(64, 0, &ctrl);
    ctrl->num_bytes        = 8;
    ctrl->data_buffer[0]   = 0x21;
    ctrl->data_buffer[1]   = 0x0B;
    ctrl->data_buffer[2]   = 0x00;
    ctrl->data_buffer[3]   = 0x00;
    ctrl->data_buffer[4]   = iface;
    ctrl->data_buffer[5]   = 0x00;
    ctrl->data_buffer[6]   = 0x00;
    ctrl->data_buffer[7]   = 0x00;
    ctrl->device_handle    = dev;
    ctrl->bEndpointAddress = 0x00;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    ctrl->callback         = [](usb_transfer_t *t) {
        xSemaphoreGiveFromISR((SemaphoreHandle_t)t->context, NULL);
    };
    ctrl->context = (void *)done;
    usb_host_transfer_submit_control(usb_client_hdl, ctrl);
    xSemaphoreTake(done, pdMS_TO_TICKS(500));
    vSemaphoreDelete(done);
    usb_host_transfer_free(ctrl);
}
```

**Step 6: Add bounds checking to USB descriptor parsing**

In the descriptor parsing loop (lines 429-451), add safety checks:

```cpp
while (off < total && !connected) {
    if (off + 1 >= total) break;
    uint8_t dlen = p[off], dtype = p[off + 1];
    if (dlen < 2 || off + dlen > total) break;
    if (dtype == 0x04 && dlen >= 9) {
        iface  = p[off + 2];
        in_kbd = (p[off + 5] == 3 && p[off + 6] == 1 && p[off + 7] == 1);
    }
    if (dtype == 0x05 && in_kbd && dlen >= 7 && (p[off + 2] & 0x80)) {
        if (usb_host_interface_claim(usb_client_hdl, usb_dev_hdl, iface, 0) != ESP_OK) break;
        usb_claimed_iface = iface;
        usb_set_boot_protocol(usb_dev_hdl, iface);
        usb_host_transfer_alloc(64, 0, &usb_xfer_in);
        usb_xfer_in->device_handle    = usb_dev_hdl;
        usb_xfer_in->bEndpointAddress = p[off + 2];
        usb_xfer_in->callback         = usb_transfer_cb;
        usb_xfer_in->num_bytes        = 8;
        usb_xfer_in->timeout_ms       = 0;
        usb_keyboard_connected        = true;
        connected                     = true;
        logKey("[USB] Keyboard connected");
        usb_host_transfer_submit(usb_xfer_in);
    }
    off += dlen;
}
```

**Step 7: Compile to verify**

Run: `pio run`
Expected: Successful build

**Step 8: Commit**

```
fix: USB host resource management and safety

- Free transfer buffer and release interface on disconnect
- Check transfer submit return value in callback
- Replace busy-wait with semaphore in boot protocol setup
- Add bounds checking to USB descriptor parsing
```

---

### Task 5: Fix Bluetooth safety issues in esp_hid_gap.c

**Files:**
- Modify: `src/esp_hid_gap.c`

**Why:** Stack buffer overflow from untrusted BLE name data, race condition corrupting scan result linked list, missing concurrent scan protection.

**Step 1: Fix BLE name buffer overflow**

In `handle_ble_device_result()` (lines 369-372), add bounds check:

```cpp
if (adv_name != NULL && adv_name_len) {
    if (adv_name_len >= sizeof(name)) adv_name_len = sizeof(name) - 1;
    memcpy(name, adv_name, adv_name_len);
    name[adv_name_len] = 0;
}
```

**Step 2: Cap BT Classic name length**

In `handle_bt_device_result()` (lines 323-325), after extracting name from EIR, cap the length:

```cpp
if (data && len) {
    name = data;
    name_len = (len > 248) ? 248 : len; // BT spec max
```

**Step 3: Fix scan result list traversal (use local pointer)**

In `esp_hid_scan()` (lines 839-847), replace the code that mutates the global `bt_scan_results`:

```cpp
*num_results = num_bt_scan_results + num_ble_scan_results;
*results = bt_scan_results;
if (num_bt_scan_results && bt_scan_results != NULL) {
    esp_hid_scan_result_t *tail = bt_scan_results;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = ble_scan_results;
} else {
    *results = ble_scan_results;
}

num_bt_scan_results = 0;
bt_scan_results = NULL;
num_ble_scan_results = 0;
ble_scan_results = NULL;
```

**Step 4: Add scan mutex for concurrent scan protection**

Add a static mutex near the other semaphores (after line 44):

```c
static SemaphoreHandle_t scan_mutex = NULL;
```

In `esp_hid_gap_init()`, after creating the other semaphores (after line 800):

```c
scan_mutex = xSemaphoreCreateMutex();
if (scan_mutex == NULL) {
    ESP_LOGE(TAG, "xSemaphoreCreateMutex failed!");
    vSemaphoreDelete(bt_hidh_cb_semaphore);
    bt_hidh_cb_semaphore = NULL;
    vSemaphoreDelete(ble_hidh_cb_semaphore);
    ble_hidh_cb_semaphore = NULL;
    return ESP_FAIL;
}
```

In `esp_hid_scan()`, wrap the entire function body with the mutex:

```c
esp_err_t esp_hid_scan(uint32_t seconds, size_t *num_results, esp_hid_scan_result_t **results)
{
    if (xSemaphoreTake(scan_mutex, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Scan already in progress");
        return ESP_FAIL;
    }

    if (num_bt_scan_results || bt_scan_results || num_ble_scan_results || ble_scan_results) {
        ESP_LOGE(TAG, "There are old scan results. Free them first!");
        xSemaphoreGive(scan_mutex);
        return ESP_FAIL;
    }

    // ... existing scan logic (BLE scan, BT scan, result merging) ...

    xSemaphoreGive(scan_mutex);
    return ESP_OK;
}
```

Make sure the early `return ESP_FAIL` paths in the BLE/BT scan sections also call `xSemaphoreGive(scan_mutex)` before returning.

**Step 5: Compile to verify**

Run: `pio run`
Expected: Successful build

**Step 6: Commit**

```
fix: Bluetooth safety — buffer overflow, race conditions, scan mutex

- Bounds-check BLE advertisement name to prevent stack overflow
- Cap BT Classic name length to 248 bytes (spec max)
- Use local pointer for scan result list traversal
- Add mutex to prevent concurrent scan corruption
```

---

### Task 6: Fix web UI issues in web_ui.h

**Files:**
- Modify: `src/web_ui.h`

**Why:** Tab switching uses undefined `event` variable (works via deprecated `window.event` but not reliable), missing HTTP response status checks, save success detection is fragile, factory reset reload timing.

**Step 1: Fix showTab() event parameter**

Replace the `showTab` function (line 260-267):

```javascript
function showTab(name, evt) {
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.getElementById('panel-'+name).classList.add('active');
  if (evt && evt.target) evt.target.classList.add('active');
  if (name === 'monitor' && !logPoll) startLogPoll();
  if (name !== 'monitor' && logPoll) { clearInterval(logPoll); logPoll = null; }
}
```

Update all tab button onclick handlers (lines 74-79) to pass event:

```html
<button class="tab active" onclick="showTab('general',event)">General</button>
<button class="tab" onclick="showTab('pins',event)">Pins</button>
<button class="tab" onclick="showTab('timing',event)">Timing</button>
<button class="tab" onclick="showTab('keys',event)">Key Mappings</button>
<button class="tab" onclick="showTab('wifi',event)">WiFi</button>
<button class="tab" onclick="showTab('monitor',event)">Monitor</button>
```

**Step 2: Add HTTP response status checks**

Update `loadConfig()` (lines 277-284):

```javascript
async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    cfg = await r.json();
    populateForm();
    updateStatus();
  } catch(e) { toast('Failed to load config', false); }
}
```

Update `saveAll()` to check HTTP status (lines 401-416):

```javascript
async function saveAll() {
  gatherConfig();
  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify(cfg)
    });
    if (!r.ok) {
      const text = await r.text();
      toast('Save failed: ' + text, false);
      return;
    }
    const result = await r.json();
    if (result.ok) {
      toast('Saved! Changes applied.', true);
      loadConfig();
    } else {
      toast('Save failed: ' + (result.error || 'unknown'), false);
    }
  } catch(e) { toast('Save failed: ' + e, false); }
}
```

**Step 3: Improve factory reset reliability**

Replace `factoryReset()` (lines 419-426):

```javascript
async function factoryReset() {
  if (!confirm('Reset all settings to factory defaults? This cannot be undone.')) return;
  try {
    await fetch('/api/reset', {method:'POST'});
    toast('Reset complete. Rebooting...', true);
    setTimeout(async () => {
      for (let i = 0; i < 20; i++) {
        try { await fetch('/api/status'); location.reload(); return; }
        catch(e) { await new Promise(r => setTimeout(r, 500)); }
      }
      toast('Device may have changed IP. Reconnect manually.', false);
    }, 3000);
  } catch(e) { toast('Reset failed', false); }
}
```

**Step 4: Add status check to updateStatus() and other fetches**

Update `updateStatus()` (lines 448-460):

```javascript
async function updateStatus() {
  try {
    const r = await fetch('/api/status');
    if (!r.ok) return;
    const s = await r.json();
    dot('dotUsb', s.usb_connected);
    dot('dotBt', s.bt_connected);
    dot('dotWifi', true);
    document.getElementById('modeLabel').textContent = s.ansi_mode ? 'ANSI' : 'Native';
    if (s.wifi_mode) document.getElementById('wifiModeLabel').textContent = s.wifi_mode;
    if (s.wifi_ip) document.getElementById('wifiIpLabel').textContent = s.wifi_ip;
    if (s.hostname) document.getElementById('wifiHostLabel').textContent = s.hostname + '.local';
  } catch(e) {}
}
```

Update `startLogPoll()` inner fetch (lines 462-474):

```javascript
function startLogPoll() {
  logPoll = setInterval(async () => {
    try {
      const r = await fetch('/api/log');
      if (!r.ok) return;
      const data = await r.json();
      if (data.entries && data.entries.length > 0) {
        const log = document.getElementById('keyLog');
        data.entries.forEach(e => { log.textContent += e + '\n'; });
        log.scrollTop = log.scrollHeight;
      }
    } catch(e) {}
  }, 1000);
}
```

Update `loadPreset()` (lines 479-489):

```javascript
async function loadPreset(name) {
  try {
    const r = await fetch('/api/preset/' + encodeURIComponent(name));
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const data = await r.json();
    if (data.special_keys) {
      cfg.special_keys = data.special_keys;
      populateKeyTable();
      toast('Preset loaded \u2014 click Save to apply', true);
    }
  } catch(e) { toast('Preset not found', false); }
}
```

**Step 5: Compile to verify**

Run: `pio run`
Expected: Successful build

**Step 6: Commit**

```
fix: web UI — tab switching, HTTP status checks, factory reset

- Pass event explicitly to showTab() instead of using window.event
- Check response.ok before parsing JSON on all fetch calls
- Improve factory reset to poll for device availability
- Add encodeURIComponent to preset name in URL
```

---

### Task 7: Remaining keybridge.cpp hardening

**Files:**
- Modify: `src/keybridge.cpp`

**Why:** Key queue drops are silent, HID keycode bounds check is fragile, /api/test endpoint has no validation, submitKeyReport should log drops.

**Step 1: Log queue overflow in submitKeyReport()**

Replace `submitKeyReport()` (lines 98-103):

```cpp
void submitKeyReport(uint8_t modifiers, const uint8_t *keys) {
    KeyReport report;
    report.modifiers = modifiers;
    memcpy(report.keys, keys, 6);
    if (xQueueSend(keyQueue, &report, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Key queue full, event dropped");
    }
}
```

**Step 2: Defensive bounds check on keycode lookup**

In `processKeypress()` (line 252), add a bounds check right before the array access:

```cpp
if (keycode >= sizeof(hid_to_ascii_lower)) return;
uint8_t ascii = effective_shift ? hid_to_ascii_upper[keycode] : hid_to_ascii_lower[keycode];
```

**Step 3: Add validation to /api/test endpoint**

Replace the `/api/test` handler (lines 722-734):

```cpp
server.on("/api/test", HTTP_POST, []() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    const char *val = doc["char"] | "";
    if (strlen(val) == 1) {
        sendChar((uint8_t)val[0]);
    } else if (strlen(val) == 2) {
        char *endptr;
        long byte = strtol(val, &endptr, 16);
        if (endptr != val + 2 || byte < 0 || byte > 127) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid hex (00-7F)\"}");
            return;
        }
        sendChar((uint8_t)byte);
    } else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Expected 1 char or 2-char hex\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
});
```

**Step 4: Compile to verify**

Run: `pio run`
Expected: Successful build

**Step 5: Commit**

```
fix: key queue overflow logging, bounds checks, test endpoint validation
```

---

### Task 8: Final compile and format check

**Files:**
- All modified files

**Step 1: Full clean build**

Run: `pio run -t clean && pio run`
Expected: Successful build with no warnings related to our changes

**Step 2: Format C/C++ source files**

Run: `clang-format -i src/keybridge.cpp src/config.h`

Do NOT format `src/web_ui.h` (per CLAUDE.md — the clang-format guards handle it).
Do NOT format `src/esp_hid_gap.c` (Espressif example code, preserve original style).

**Step 3: Final compile after formatting**

Run: `pio run`
Expected: Successful build

**Step 4: Commit**

```
style: format source files
```

---

## Issues intentionally NOT addressed

These were flagged by reviewers but are false positives or not worth the risk/complexity:

- **logKey() buffer overflow (#4)**: `vsnprintf()` always null-terminates when size > 0. Not a real issue.
- **Log polling duplicates (#11)**: The server-side `/api/log` handler advances `keyLogTail`, consuming entries. Each poll only returns NEW entries. The client append logic is correct.
- **BT HID signed/unsigned (#33)**: The `>= 3` guard ensures `length - 2 >= 1`, making the signed int safe.
- **Struct packing (#28)**: Adding `__attribute__((packed))` to `AdapterConfig` could cause unaligned access penalties and is fragile. The `static_assert` approach is better but requires knowing the exact expected size, which varies by compiler. The version check fix (Task 3) already protects against cross-version corruption.
- **Hardcoded BLE passkey (#23)**: The `iocap` is set to `ESP_IO_CAP_IO` (numeric comparison), so the static passkey at line 651 is only used when `ESP_IO_CAP_OUT` is active. With the current config, pairing uses numeric comparison (user confirms matching numbers on both devices), which is secure. The passkey is vestigial from the example code.
- **Stack overflow in app_main (#34)**: The default Arduino ESP32 main task stack is 8192 bytes. Refactoring to a separate task adds complexity with minimal benefit since the code works today. Monitor with `uxTaskGetStackHighWaterMark()` if issues arise.
