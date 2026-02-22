/*
 * ============================================================
 * KeyBridge — Bluetooth Keyboard to Wyse 50 Terminal Adapter
 * ============================================================
 * Emulates the Wyse 50 keyboard scan matrix so a modern
 * Bluetooth keyboard can replace the missing original.
 * All settings configurable at runtime via WiFi.
 *
 * Target:    ESP32 (WROOM-32) — Classic BT + BLE + WiFi
 * Framework: Arduino + ESP-IDF (combined)
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include "soc/gpio_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// WiFi + Web Server
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <mdns.h>

// USB Host (ESP32-S3 only — original ESP32 has no USB OTG)
#if CONFIG_SOC_USB_OTG_SUPPORTED
#include "usb/usb_host.h"
#endif

// Bluetooth
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hidh.h"
#include "esp_hidh_gattc.h"
#include "esp_hid_gap.h"
#include "esp_gap_ble_api.h"

// Arduino
#include "Arduino.h"

// Local headers
#include "config.h"
#include "web_ui.h"

static const char *TAG = "KEYBRIDGE";

// ============================================================
// KEY EVENT QUEUE (struct declared early for Arduino prototype generation)
// ============================================================

typedef struct {
    uint8_t modifiers;
    uint8_t keys[6];
} KeyReport;

// ============================================================
// GLOBAL STATE
// ============================================================

static AdapterConfig config;                // Active configuration
static SemaphoreHandle_t config_mutex = NULL; // Protects config reads/writes across tasks
static WebServer server(80);                  // Web server
static DNSServer dnsServer;                   // Captive portal DNS (AP mode only)
static QueueHandle_t keyQueue = NULL;         // Shared key event queue

// Status flags (read by web API)
static volatile bool usb_keyboard_connected = false;
static volatile bool bt_keyboard_connected  = false;
static bool wifi_sta_mode                   = false;

// Key log ring buffer for the monitor tab
#define KEY_LOG_SIZE 64
static char keyLogBuf[KEY_LOG_SIZE][48];
static uint8_t keyLogHead            = 0;
static uint8_t keyLogTail            = 0;
static SemaphoreHandle_t keyLogMutex = NULL;

void logKey(const char *fmt, ...) {
    char buf[48];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (xSemaphoreTake(keyLogMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strlcpy(keyLogBuf[keyLogHead], buf, sizeof(keyLogBuf[0]));
        keyLogHead = (keyLogHead + 1) % KEY_LOG_SIZE;
        if (keyLogHead == keyLogTail) {
            keyLogTail = (keyLogTail + 1) % KEY_LOG_SIZE; // Overwrite oldest
        }
        xSemaphoreGive(keyLogMutex);
    }
}

// ============================================================
// KEY EVENT QUEUE
// ============================================================

void submitKeyReport(uint8_t modifiers, const uint8_t *keys) {
    KeyReport report;
    report.modifiers = modifiers;
    memcpy(report.keys, keys, 6);
    if (xQueueSend(keyQueue, &report, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Key queue full, event dropped");
    }
}

// ============================================================
// KEYBOARD SCAN EMULATION
// ============================================================

// 128-entry table: true = key at this address is currently "pressed"
static volatile uint8_t key_state[128] = {0};

// Cached GPIO pin numbers for fast access in scan loop
static uint8_t scan_addr_pins[7];
static uint8_t scan_return_pin;

// Scan snoop mode — tracks which addresses the terminal is scanning
static volatile bool scan_snoop_mode              = false;
static volatile uint32_t scan_addr_histogram[128] = {0};
static volatile uint32_t scan_last_addr           = 0xFF;
static volatile uint32_t scan_total_count          = 0;

void setupScanPins() {
    // Address inputs (from terminal via TXS0108E)
    for (int i = 0; i < 7; i++) {
        scan_addr_pins[i] = config.pin_addr[i];
        if (config.pin_addr[i] >= 0) {
            pinMode(config.pin_addr[i], INPUT);
        }
    }
    // Key Return output (to terminal via 2N7000 MOSFET)
    scan_return_pin = config.pin_key_return;
    if (config.pin_key_return >= 0) {
        pinMode(config.pin_key_return, OUTPUT);
        digitalWrite(config.pin_key_return, LOW); // MOSFET off = key not pressed
    }

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

// ============================================================
// SCAN RESPONSE TASK (core 0, highest priority)
// ============================================================

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

        // Snoop mode: track address histogram
        if (scan_snoop_mode) {
            scan_addr_histogram[addr]++;
            scan_last_addr = addr;
            scan_total_count++;
        }

        // Drive Key Return based on key state table
        if (key_state[addr]) {
            REG_WRITE(GPIO_OUT_W1TS_REG, return_mask); // HIGH = MOSFET on = key pressed
        } else {
            REG_WRITE(GPIO_OUT_W1TC_REG, return_mask); // LOW = MOSFET off = not pressed
        }

        // Yield briefly every ~10k iterations (~2ms at 240MHz) to let
        // WiFi/BT tasks on core 0 run. At 1kHz tick rate, vTaskDelay(1)
        // blocks for 1ms — the terminal rescans all addresses every ~1ms
        // so missing one cycle is imperceptible.
        if (++yield_counter >= 10000) {
            yield_counter = 0;
            vTaskDelay(1);
        }
    }
}

// ============================================================
// HID-TO-WYSE50 KEY ADDRESS MAPPING
// ============================================================
// Source: MAME wy50kb.cpp (verified against WY-50 maintenance manual schematic)
// Address = (column * 8) + row; bits 6-3 = column (0-12), bits 2-0 = row (0-7)
// 0xFF = no mapping (key not present on Wyse 50)

#define WYSE_SHIFT 0x4A // Col 9, Row 2
#define WYSE_CTRL  0x1F // Col 3, Row 7

// 0xFF = no mapping (key not present on Wyse 50)
static uint8_t hid_to_wyse50[256];

void initKeyMap() {
    memset(hid_to_wyse50, 0xFF, sizeof(hid_to_wyse50));

    // Letters (HID 0x04-0x1D = a-z)
    hid_to_wyse50[0x04] = 0x3F; // a → Col 7, Row 7
    hid_to_wyse50[0x05] = 0x2E; // b → Col 5, Row 6
    hid_to_wyse50[0x06] = 0x4E; // c → Col 9, Row 6
    hid_to_wyse50[0x07] = 0x37; // d → Col 6, Row 7
    hid_to_wyse50[0x08] = 0x30; // e → Col 6, Row 0
    hid_to_wyse50[0x09] = 0x17; // f → Col 2, Row 7
    hid_to_wyse50[0x0A] = 0x0F; // g → Col 1, Row 7
    hid_to_wyse50[0x0B] = 0x07; // h → Col 0, Row 7
    hid_to_wyse50[0x0C] = 0x58; // i → Col 11, Row 0
    hid_to_wyse50[0x0D] = 0x5F; // j → Col 11, Row 7
    hid_to_wyse50[0x0E] = 0x67; // k → Col 12, Row 7
    hid_to_wyse50[0x0F] = 0x2F; // l → Col 5, Row 7
    hid_to_wyse50[0x10] = 0x0E; // m → Col 1, Row 6
    hid_to_wyse50[0x11] = 0x16; // n → Col 2, Row 6
    hid_to_wyse50[0x12] = 0x60; // o → Col 12, Row 0
    hid_to_wyse50[0x13] = 0x51; // p → Col 10, Row 1
    hid_to_wyse50[0x14] = 0x38; // q → Col 7, Row 0
    hid_to_wyse50[0x15] = 0x28; // r → Col 5, Row 0
    hid_to_wyse50[0x16] = 0x4F; // s → Col 9, Row 7
    hid_to_wyse50[0x17] = 0x10; // t → Col 2, Row 0
    hid_to_wyse50[0x18] = 0x00; // u → Col 0, Row 0
    hid_to_wyse50[0x19] = 0x36; // v → Col 6, Row 6
    hid_to_wyse50[0x1A] = 0x48; // w → Col 9, Row 0
    hid_to_wyse50[0x1B] = 0x3E; // x → Col 7, Row 6
    hid_to_wyse50[0x1C] = 0x08; // y → Col 1, Row 0
    hid_to_wyse50[0x1D] = 0x1E; // z → Col 3, Row 6

    // Number row (HID 0x1E-0x27 = 1-0)
    hid_to_wyse50[0x1E] = 0x1B; // 1/! → Col 3, Row 3
    hid_to_wyse50[0x1F] = 0x3B; // 2/@ → Col 7, Row 3
    hid_to_wyse50[0x20] = 0x4B; // 3/# → Col 9, Row 3
    hid_to_wyse50[0x21] = 0x33; // 4/$ → Col 6, Row 3
    hid_to_wyse50[0x22] = 0x2B; // 5/% → Col 5, Row 3
    hid_to_wyse50[0x23] = 0x13; // 6/^ → Col 2, Row 3
    hid_to_wyse50[0x24] = 0x0B; // 7/& → Col 1, Row 3
    hid_to_wyse50[0x25] = 0x03; // 8/* → Col 0, Row 3
    hid_to_wyse50[0x26] = 0x5B; // 9/( → Col 11, Row 3
    hid_to_wyse50[0x27] = 0x63; // 0/) → Col 12, Row 3

    // Common keys
    hid_to_wyse50[0x28] = 0x65; // Return    → Col 12, Row 5
    hid_to_wyse50[0x29] = 0x3C; // Escape    → Col 7, Row 4
    hid_to_wyse50[0x2A] = 0x1A; // Backspace → Col 3, Row 2
    hid_to_wyse50[0x2B] = 0x18; // Tab       → Col 3, Row 0
    hid_to_wyse50[0x2C] = 0x19; // Space     → Col 3, Row 1

    // Punctuation
    hid_to_wyse50[0x2D] = 0x43; // -/_ → Col 8, Row 3
    hid_to_wyse50[0x2E] = 0x53; // =/+ → Col 10, Row 3
    hid_to_wyse50[0x2F] = 0x42; // [/{ → Col 8, Row 2
    hid_to_wyse50[0x30] = 0x45; // ]/} → Col 8, Row 5
    hid_to_wyse50[0x31] = 0x5C; // \/| → Col 11, Row 4
    hid_to_wyse50[0x33] = 0x44; // ;/: → Col 8, Row 4
    hid_to_wyse50[0x34] = 0x46; // '/" → Col 8, Row 6
    hid_to_wyse50[0x35] = 0x4C; // `/~ → Col 9, Row 4
    hid_to_wyse50[0x36] = 0x06; // ,/< → Col 0, Row 6
    hid_to_wyse50[0x37] = 0x5E; // ./> → Col 11, Row 6
    hid_to_wyse50[0x38] = 0x66; // //? → Col 12, Row 6

    // Lock / special
    hid_to_wyse50[0x39] = 0x3A; // Caps Lock → Col 7, Row 2

    // Function keys (F1-F12 map to Wyse F1-F12)
    hid_to_wyse50[0x3A] = 0x1D; // F1  → Col 3, Row 5
    hid_to_wyse50[0x3B] = 0x3D; // F2  → Col 7, Row 5
    hid_to_wyse50[0x3C] = 0x25; // F3  → Col 4, Row 5
    hid_to_wyse50[0x3D] = 0x23; // F4  → Col 4, Row 3
    hid_to_wyse50[0x3E] = 0x20; // F5  → Col 4, Row 0
    hid_to_wyse50[0x3F] = 0x27; // F6  → Col 4, Row 7
    hid_to_wyse50[0x40] = 0x26; // F7  → Col 4, Row 6
    hid_to_wyse50[0x41] = 0x49; // F8  → Col 9, Row 1
    hid_to_wyse50[0x42] = 0x24; // F9  → Col 4, Row 4
    hid_to_wyse50[0x43] = 0x1C; // F10 → Col 3, Row 4
    hid_to_wyse50[0x44] = 0x57; // F11 → Col 10, Row 7
    hid_to_wyse50[0x45] = 0x22; // F12 → Col 4, Row 2

    // Wyse-specific keys mapped to HID keys that don't conflict
    hid_to_wyse50[0x47] = 0x0C; // Scroll Lock → SETUP (Col 1, Row 4) *** CRITICAL ***
    hid_to_wyse50[0x48] = 0x34; // Pause/Break → Break (Col 6, Row 4)
    hid_to_wyse50[0x49] = 0x01; // Insert      → Ins Char/Line (Col 0, Row 1)
    hid_to_wyse50[0x4A] = 0x61; // Home        → Home (Col 12, Row 1)
    hid_to_wyse50[0x4B] = 0x41; // Page Up     → Next/Prev Page (Col 8, Row 1)
    hid_to_wyse50[0x4C] = 0x62; // Delete      → Del 0x7F (Col 12, Row 2)
    hid_to_wyse50[0x4E] = 0x41; // Page Down   → Next/Prev Page (same key, shifted)

    // Arrow keys
    hid_to_wyse50[0x4F] = 0x0A; // Right → Col 1, Row 2
    hid_to_wyse50[0x50] = 0x5A; // Left  → Col 11, Row 2
    hid_to_wyse50[0x51] = 0x05; // Down  → Col 0, Row 5
    hid_to_wyse50[0x52] = 0x4D; // Up    → Col 9, Row 5

    // Keypad
    hid_to_wyse50[0x54] = 0x66; // KP /     → //? (shared)
    hid_to_wyse50[0x56] = 0x31; // KP -     → Col 6, Row 1
    hid_to_wyse50[0x58] = 0x35; // KP Enter → Col 6, Row 5
    hid_to_wyse50[0x59] = 0x12; // KP 1     → Col 2, Row 2
    hid_to_wyse50[0x5A] = 0x02; // KP 2     → Col 0, Row 2
    hid_to_wyse50[0x5B] = 0x52; // KP 3     → Col 10, Row 2
    hid_to_wyse50[0x5C] = 0x11; // KP 4     → Col 2, Row 1
    hid_to_wyse50[0x5D] = 0x2A; // KP 5     → Col 5, Row 2
    hid_to_wyse50[0x5E] = 0x2C; // KP 6     → Col 5, Row 4
    hid_to_wyse50[0x5F] = 0x14; // KP 7     → Col 2, Row 4
    hid_to_wyse50[0x60] = 0x55; // KP 8     → Col 10, Row 5
    hid_to_wyse50[0x61] = 0x59; // KP 9     → Col 11, Row 1
    hid_to_wyse50[0x62] = 0x15; // KP 0     → Col 2, Row 5
    hid_to_wyse50[0x63] = 0x29; // KP .     → Col 5, Row 1
}

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

// ============================================================
// HID REPORT PROCESSING (scan state based)
// ============================================================

static uint32_t ledOffTime = 0;

void processHidReport(const KeyReport *report) {
    uint8_t modifiers   = report->modifiers;
    const uint8_t *keys = report->keys;

    // Track which Wyse addresses are currently held
    static uint8_t prev_wyse_addrs[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static uint8_t prev_modifiers     = 0;

    // Release previous keys not in current report
    for (int i = 0; i < 6; i++) {
        if (prev_wyse_addrs[i] == 0xFF) continue;
        bool still_held = false;
        for (int j = 0; j < 6; j++) {
            uint8_t wa = (keys[j] != 0) ? hid_to_wyse50[keys[j]] : 0xFF;
            if (wa == prev_wyse_addrs[i]) {
                still_held = true;
                break;
            }
        }
        if (!still_held) {
            scanKeyRelease(prev_wyse_addrs[i]);
            prev_wyse_addrs[i] = 0xFF;
        }
    }

    // Press new keys
    for (int i = 0; i < 6; i++) {
        if (keys[i] == 0) {
            prev_wyse_addrs[i] = 0xFF;
            continue;
        }
        uint8_t addr = hid_to_wyse50[keys[i]];
        if (addr == 0xFF) {
            prev_wyse_addrs[i] = 0xFF;
            continue;
        }
        scanKeyPress(addr);
        prev_wyse_addrs[i] = addr;
    }

    // Handle modifier keys — Shift and Ctrl have physical scan addresses
    bool shift_now = (modifiers & 0x22) != 0; // L or R Shift
    bool shift_was = (prev_modifiers & 0x22) != 0;
    if (shift_now && !shift_was) scanKeyPress(WYSE_SHIFT);
    if (!shift_now && shift_was) scanKeyRelease(WYSE_SHIFT);

    bool ctrl_now = (modifiers & 0x11) != 0; // L or R Ctrl
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

// ############################################################
//  USB HOST (ESP32-S3 only — original ESP32 has no USB OTG)
// ############################################################
#if CONFIG_SOC_USB_OTG_SUPPORTED

static usb_host_client_handle_t usb_client_hdl = NULL;
static usb_device_handle_t usb_dev_hdl         = NULL;
static usb_transfer_t *usb_xfer_in             = NULL;
static uint8_t usb_claimed_iface               = 0xFF;
static SemaphoreHandle_t usb_device_sem        = NULL;

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

static void usb_client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            xSemaphoreGive(usb_device_sem);
            break;
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
        default:
            break;
    }
}

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
    ctrl->callback         = [](usb_transfer_t *t) { xSemaphoreGiveFromISR((SemaphoreHandle_t)t->context, NULL); };
    ctrl->context          = (void *)done;
    usb_host_transfer_submit_control(usb_client_hdl, ctrl);
    xSemaphoreTake(done, pdMS_TO_TICKS(500));
    vSemaphoreDelete(done);
    usb_host_transfer_free(ctrl);
}

static void usb_host_daemon_task(void *arg) {
    ESP_LOGI(TAG, "[USB] Host library installing...");
    usb_host_config_t cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    ESP_LOGI(TAG, "[USB] Host library ready, waiting for devices");
    while (true)
        usb_host_lib_handle_events(portMAX_DELAY, NULL);
}

static void usb_keyboard_task(void *arg) {
    usb_host_client_config_t cfg = {.is_synchronous    = false,
                                    .max_num_event_msg = 5,
                                    .async = {.client_event_callback = usb_client_event_cb, .callback_arg = NULL}};
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &usb_client_hdl));
    ESP_LOGI(TAG, "[USB] Client registered, polling for keyboards");

    while (true) {
        usb_host_client_handle_events(usb_client_hdl, pdMS_TO_TICKS(100));
        if (!usb_keyboard_connected && xSemaphoreTake(usb_device_sem, 0) == pdTRUE) {
            ESP_LOGI(TAG, "[USB] New device detected, opening...");
            if (usb_host_device_open(usb_client_hdl, 1, &usb_dev_hdl) != ESP_OK) {
                ESP_LOGW(TAG, "[USB] Failed to open device");
                continue;
            }

            const usb_config_desc_t *ccfg;
            usb_host_get_active_config_descriptor(usb_dev_hdl, &ccfg);
            const uint8_t *p = (const uint8_t *)ccfg;
            int off = 0, total = ccfg->wTotalLength;
            uint8_t iface = 0;
            bool in_kbd = false, connected = false;

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
                    ESP_LOGI(TAG, "[USB] Keyboard connected (iface %d, ep 0x%02x)", iface, p[off + 2]);
                    logKey("[USB] Keyboard connected");
                    usb_host_transfer_submit(usb_xfer_in);
                }
                off += dlen;
            }
            if (!connected) {
                ESP_LOGW(TAG, "[USB] Device is not a boot keyboard, closing");
                usb_host_device_close(usb_client_hdl, usb_dev_hdl);
                usb_dev_hdl = NULL;
            }
        }
    }
}

void startUsbHost() {
    usb_device_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(usb_host_daemon_task, "usb_d", 4096, NULL, 5, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    xTaskCreatePinnedToCore(usb_keyboard_task, "usb_kb", 4096, NULL, 5, NULL, 1);
}

#endif // CONFIG_SOC_USB_OTG_SUPPORTED

// ############################################################
//  BLUETOOTH
// ############################################################

static esp_hidh_dev_t *bt_hid_dev      = NULL;
static volatile bool bt_scan_requested = false;

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    esp_hidh_event_t event       = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
        case ESP_HIDH_OPEN_EVENT:
            if (param->open.status == ESP_OK) {
                bt_hid_dev            = param->open.dev;
                bt_keyboard_connected = true;
                const char *name      = esp_hidh_dev_name_get(param->open.dev);
                logKey("[BT] Connected: %s", name ? name : "unknown");
                if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (config.pin_bt_led >= 0) digitalWrite(config.pin_bt_led, HIGH);
                    xSemaphoreGive(config_mutex);
                }
            }
            break;
        case ESP_HIDH_INPUT_EVENT:
            if (param->input.length >= 8) {
                submitKeyReport(param->input.data[0], &param->input.data[2]);
            } else if (param->input.length >= 3) {
                uint8_t keys[6] = {0};
                int n           = param->input.length - 2;
                if (n > 6) n = 6;
                if (n > 0) memcpy(keys, &param->input.data[2], n);
                submitKeyReport(param->input.data[0], keys);
            }
            break;
        case ESP_HIDH_CLOSE_EVENT:
            bt_hid_dev            = NULL;
            bt_keyboard_connected = false;
            logKey("[BT] Disconnected");
            if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (config.pin_bt_led >= 0) digitalWrite(config.pin_bt_led, LOW);
                xSemaphoreGive(config_mutex);
            }
            break;
        case ESP_HIDH_BATTERY_EVENT:
            logKey("[BT] Battery: %d%%", param->battery.level);
            break;
        default:
            break;
    }
}

static void bt_scan_task(void *arg) {
    while (true) {
        if (!bt_scan_requested) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        bt_scan_requested = false;

        if (bt_hid_dev) {
            esp_hidh_dev_close(bt_hid_dev);
            bt_hid_dev            = NULL;
            bt_keyboard_connected = false;
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        logKey("[BT] Scanning...");
        size_t num                     = 0;
        esp_hid_scan_result_t *results = NULL;
        esp_hid_scan(5, &num, &results);

        if (num > 0) {
            esp_hid_scan_result_t *best = NULL, *r = results;
            while (r) {
                logKey("[BT] Found: %s (RSSI %d)", r->name ? r->name : "?", r->rssi);
                if (!best || r->rssi > best->rssi) best = r;
                r = r->next;
            }
            if (best) {
                logKey("[BT] Connecting: %s", best->name ? best->name : "?");
                esp_hidh_dev_open(best->bda, best->transport,
                                  best->transport == ESP_HID_TRANSPORT_BLE ? best->ble.addr_type : 0);
            }
            esp_hid_scan_results_free(results);
        } else {
            logKey("[BT] No devices found");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void bt_init_task(void *arg) {
    ESP_LOGI(TAG, "[BT] Init starting (heap=%lu)...",
             (unsigned long)esp_get_free_heap_size());

    // Determine BT mode from config
    uint8_t mode = ESP_BT_MODE_BLE;
#if CONFIG_BT_CLASSIC_ENABLED
    if (config.enable_bt_classic && config.enable_ble)
        mode = ESP_BT_MODE_BTDM;
    else if (config.enable_bt_classic)
        mode = ESP_BT_MODE_CLASSIC_BT;
#endif

    // esp_hid_gap_init handles the full BT stack via init_low_level():
    // mem release, controller init/enable, bluedroid init/enable, GAP profile setup
    esp_err_t ret = esp_hid_gap_init(mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[BT] GAP init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "[BT] GAP initialized (heap=%lu)", (unsigned long)esp_get_free_heap_size());

    esp_bt_dev_set_device_name(config.wifi_ssid);

#if CONFIG_BT_CLASSIC_ENABLED
    if (config.enable_bt_classic)
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
#endif

    // Register the GATTC callback BEFORE esp_hidh_init — required by the esp_hid
    // component (see esp_hidh_gattc.h). Without this, GATTC registration events
    // are never delivered and esp_hidh_init hangs forever at WAIT_CB().
    if (mode != ESP_BT_MODE_CLASSIC_BT) {
        ret = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[BT] GATTC register callback failed: %s", esp_err_to_name(ret));
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGI(TAG, "[BT] Initializing HID host...");
    esp_hidh_config_t hidh_cfg = {.callback = hidh_callback, .event_stack_size = 4096};
    ret = esp_hidh_init(&hidh_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[BT] HID host init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "[BT] HID host initialized (heap=%lu)", (unsigned long)esp_get_free_heap_size());

    xTaskCreatePinnedToCore(bt_scan_task, "bt_scan", 6144, NULL, 3, NULL, 0);
    logKey("[BT] Ready. Press PAIR to connect.");
    ESP_LOGI(TAG, "[BT] Init complete");

    vTaskDelete(NULL); // Self-delete — init is done
}

void startBluetooth() {
    xTaskCreatePinnedToCore(bt_init_task, "bt_init", 8192, NULL, 3, NULL, 0);
}

// ============================================================
// ADMIN PASSWORD (NVS, separate from config blob)
// ============================================================

static char admin_password[8]; // up to 6 chars + null; empty = no auth

bool hasPassword() { return admin_password[0] != '\0'; }

bool loadAdminPass() {
    Preferences p;
    p.begin("kb_cfg", true);
    String pass = p.getString("admin_pass", "");
    p.end();
    if (pass.length() == 0) return false;
    strlcpy(admin_password, pass.c_str(), sizeof(admin_password));
    return true;
}

void saveAdminPass() {
    Preferences p;
    p.begin("kb_cfg", false);
    p.putString("admin_pass", admin_password);
    p.end();
}

void clearAdminPass() {
    admin_password[0] = '\0';
    Preferences p;
    p.begin("kb_cfg", false);
    p.remove("admin_pass");
    p.end();
}

// ============================================================
// SESSION MANAGEMENT (in-memory, lost on reboot)
// ============================================================

#define MAX_SESSIONS 4
#define SESSION_TIMEOUT_MS (30UL * 60 * 1000) // 30 minutes

struct Session {
    char token[33]; // 32 hex chars + null
    uint32_t last_activity;
};

static Session sessions[MAX_SESSIONS];

void generateToken(char *out) {
    for (int i = 0; i < 32; i += 8) {
        uint32_t r = esp_random();
        snprintf(out + i, 9, "%08x", r);
    }
    out[32] = '\0';
}

void expireSessions() {
    uint32_t now = millis();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].token[0] && (now - sessions[i].last_activity > SESSION_TIMEOUT_MS)) {
            sessions[i].token[0] = '\0';
        }
    }
}

const char *createSession() {
    expireSessions();

    // Find empty slot (or evict oldest)
    int slot     = -1;
    uint32_t oldest = UINT32_MAX;
    int oldest_slot  = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].token[0] == '\0') {
            slot = i;
            break;
        }
        if (sessions[i].last_activity < oldest) {
            oldest      = sessions[i].last_activity;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot; // Evict oldest

    generateToken(sessions[slot].token);
    sessions[slot].last_activity = millis();
    return sessions[slot].token;
}

// Extract token from cookie header: "kb_session=TOKEN; other=..."
static bool getCookieToken(const String &cookies, String &token) {
    int start = cookies.indexOf("kb_session=");
    if (start < 0) return false;
    start += 11; // strlen("kb_session=")
    int end = cookies.indexOf(';', start);
    token   = (end < 0) ? cookies.substring(start) : cookies.substring(start, end);
    token.trim();
    return token.length() > 0;
}

bool isAuthenticated() {
    if (!hasPassword()) return true; // No password set — auth disabled

    expireSessions();

    String token;

    // Check cookie first
    if (server.hasHeader("Cookie")) {
        if (getCookieToken(server.header("Cookie"), token)) {
            for (int i = 0; i < MAX_SESSIONS; i++) {
                if (sessions[i].token[0] && token.equals(sessions[i].token)) {
                    sessions[i].last_activity = millis();
                    return true;
                }
            }
        }
    }

    // Fall back to Authorization: Bearer
    if (server.hasHeader("Authorization")) {
        String auth = server.header("Authorization");
        if (auth.startsWith("Bearer ")) {
            token = auth.substring(7);
            token.trim();
            for (int i = 0; i < MAX_SESSIONS; i++) {
                if (sessions[i].token[0] && token.equals(sessions[i].token)) {
                    sessions[i].last_activity = millis();
                    return true;
                }
            }
        }
    }

    return false;
}

void sendUnauthorized() {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
}

// ############################################################
//  WEB SERVER + REST API
// ############################################################

bool connectSta() {
    if (config.sta_ssid[0] == '\0') return false;

    ESP_LOGI(TAG, "[WiFi] Connecting to STA network: %s", config.sta_ssid);
    logKey("[WiFi] Connecting to %s...", config.sta_ssid);

    WiFi.setHostname(config.hostname);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.sta_ssid, config.sta_password);

    for (int i = 0; i < 150; i++) { // 15 seconds (150 * 100ms)
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) return true;
        if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) {
            ESP_LOGW(TAG, "[WiFi] STA failed (status %d)", status);
            break;
        }
        delay(100);
    }

    ESP_LOGW(TAG, "[WiFi] STA connection timed out");
    WiFi.disconnect(true);
    return false;
}

void startAp() {
    WiFi.enableSTA(false);
    WiFi.mode(WIFI_AP);
    IPAddress apIp(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIp, apIp, subnet);
    WiFi.softAP(config.wifi_ssid, config.wifi_password, config.wifi_channel);
}

void startMdns() {
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "[mDNS] Init failed");
        return;
    }
    mdns_hostname_set(config.hostname);
    mdns_instance_name_set("KeyBridge Terminal Adapter");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "[mDNS] %s.local", config.hostname);
}

void startWebServer() {
    if (config.sta_ssid[0] != '\0' && connectSta()) {
        wifi_sta_mode = true;
        IPAddress ip  = WiFi.localIP();
        ESP_LOGI(TAG, "[WiFi] STA connected: %s", ip.toString().c_str());
        logKey("[WiFi] STA: %s", ip.toString().c_str());
    } else {
        startAp();
        wifi_sta_mode = false;
        IPAddress ip  = WiFi.softAPIP();
        ESP_LOGI(TAG, "[WiFi] AP started: %s", config.wifi_ssid);
        logKey("[WiFi] AP: %s", ip.toString().c_str());
    }

    startMdns();
    ESP_LOGI(TAG, "[WiFi] http://%s.local/", config.hostname);

    // Captive portal DNS redirect (AP mode only)
    if (!wifi_sta_mode) {
        dnsServer.start(53, "*", WiFi.softAPIP());
        ESP_LOGI(TAG, "[WiFi] Captive portal DNS active");
    }

    // Serve the web UI
    server.on("/", HTTP_GET, []() {
        ESP_LOGI(TAG, "[HTTP] GET / — serving UI (%u bytes)", (unsigned)sizeof(WEB_UI_HTML));
        esp_task_wdt_reset(); // Feed watchdog before long PROGMEM send
        server.send_P(200, "text/html", WEB_UI_HTML);
    });

    // GET config (auth required — exposes WiFi credentials)
    server.on("/api/config", HTTP_GET, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        server.send(200, "application/json", configToJson(config));
    });

    // POST config (save, auth required)
    server.on("/api/config", HTTP_POST, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        String body          = server.arg("plain");
        AdapterConfig newCfg = config; // Start with current
        if (jsonToConfig(body, newCfg)) {
            if (xSemaphoreTake(config_mutex, portMAX_DELAY) == pdTRUE) {
                config     = newCfg;
                bool saved = saveConfig(config);
                xSemaphoreGive(config_mutex);
                if (saved) {
                    server.send(200, "application/json", "{\"ok\":true}");
                    logKey("[Config] Saved to NVS");
                } else {
                    server.send(500, "application/json", "{\"ok\":false,\"error\":\"NVS write failed\"}");
                }
            } else {
                server.send(503, "application/json", "{\"ok\":false,\"error\":\"Config busy\"}");
            }
        } else {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        }
    });

    // Status endpoint (unauthenticated — device name + connection state only)
    server.on("/api/status", HTTP_GET, []() {
        JsonDocument doc;
        doc["usb_connected"] = (bool)usb_keyboard_connected;
        doc["bt_connected"]  = (bool)bt_keyboard_connected;
        doc["uptime_sec"]    = millis() / 1000;
        doc["free_heap"]     = esp_get_free_heap_size();
        doc["wifi_mode"]     = wifi_sta_mode ? "STA" : "AP";
        doc["wifi_ip"]       = wifi_sta_mode ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        doc["hostname"]      = config.hostname;
        doc["device_name"]   = config.wifi_ssid;
        doc["auth_required"] = hasPassword();
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    // BT pair trigger (auth required)
    server.on("/api/bt/pair", HTTP_POST, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        bt_scan_requested = true;
        server.send(200, "application/json", "{\"message\":\"Scan initiated — 5 seconds\"}");
    });

    // Factory reset (auth required — destructive)
    server.on("/api/reset", HTTP_POST, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        eraseConfig();
        server.send(200, "application/json", "{\"ok\":true}");
        delay(500);
        ESP.restart();
    });

    // Key log (auth required — keypress log could be sensitive)
    server.on("/api/log", HTTP_GET, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        JsonDocument doc;
        JsonArray entries = doc["entries"].to<JsonArray>();

        if (xSemaphoreTake(keyLogMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            while (keyLogTail != keyLogHead) {
                entries.add(keyLogBuf[keyLogTail]);
                keyLogTail = (keyLogTail + 1) % KEY_LOG_SIZE;
            }
            xSemaphoreGive(keyLogMutex);
        }

        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    // Scan snoop — start/stop address monitoring
    server.on("/api/scan/snoop", HTTP_POST, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        JsonDocument doc;
        deserializeJson(doc, server.arg("plain"));
        bool enable = doc["enable"] | false;
        if (enable) {
            memset((void *)scan_addr_histogram, 0, sizeof(scan_addr_histogram));
            scan_total_count = 0;
            scan_snoop_mode  = true;
            server.send(200, "application/json", "{\"ok\":true,\"message\":\"Snoop started\"}");
        } else {
            scan_snoop_mode = false;
            server.send(200, "application/json", "{\"ok\":true,\"message\":\"Snoop stopped\"}");
        }
    });

    // Scan histogram — read address frequency data
    server.on("/api/scan/histogram", HTTP_GET, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        scan_snoop_mode = false; // Pause while reading
        JsonDocument doc;
        doc["total_scans"] = scan_total_count;
        doc["last_addr"]   = scan_last_addr;
        JsonArray addrs    = doc["addresses"].to<JsonArray>();
        for (int i = 0; i < 128; i++) {
            if (scan_addr_histogram[i] > 0) {
                JsonObject e = addrs.add<JsonObject>();
                e["addr"]  = i;
                e["count"] = scan_addr_histogram[i];
                e["col"]   = (i >> 3) & 0x0F;
                e["row"]   = i & 0x07;
            }
        }
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    // Scan test — assert a single address for a duration
    server.on("/api/scan/test", HTTP_POST, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        JsonDocument doc;
        deserializeJson(doc, server.arg("plain"));
        uint8_t addr       = doc["addr"] | 0xFF;
        uint16_t duration  = doc["duration_ms"] | 200;
        if (addr >= 128) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"addr must be 0-127\"}");
            return;
        }
        if (duration > 5000) duration = 5000;
        scanKeyPress(addr);
        server.send(200, "application/json", "{\"ok\":true}");
        delay(duration);
        scanKeyRelease(addr);
    });

    // Scan sweep — test a range of addresses sequentially
    server.on("/api/scan/sweep", HTTP_POST, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        JsonDocument doc;
        deserializeJson(doc, server.arg("plain"));
        uint8_t start    = doc["start"] | 0;
        uint8_t end      = doc["end"] | 127;
        uint16_t hold_ms = doc["hold_ms"] | 300;
        uint16_t gap_ms  = doc["gap_ms"] | 200;
        logKey("[SCAN] Sweep %d-%d, hold=%dms", start, end, hold_ms);
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Sweep started\"}");
        for (uint8_t addr = start; addr <= end && addr < 128; addr++) {
            logKey("[SCAN] addr=0x%02X (col=%d row=%d)", addr, (addr >> 3) & 0x0F, addr & 0x07);
            scanKeyPress(addr);
            delay(hold_ms);
            scanKeyRelease(addr);
            delay(gap_ms);
        }
        logKey("[SCAN] Sweep complete");
    });

    // Login
    server.on("/api/login", HTTP_POST, []() {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
            return;
        }
        const char *pass = doc["password"] | "";
        if (strcmp(pass, admin_password) != 0) {
            delay(1000); // Slow brute force
            server.send(401, "application/json", "{\"ok\":false,\"error\":\"Wrong password\"}");
            return;
        }
        const char *token = createSession();
        String cookie     = "kb_session=";
        cookie += token;
        cookie += "; Path=/; HttpOnly";
        server.sendHeader("Set-Cookie", cookie);
        String body = "{\"ok\":true,\"token\":\"";
        body += token;
        body += "\"}";
        server.send(200, "application/json", body);
    });

    // Set or change password
    server.on("/api/password", HTTP_POST, []() {
        if (!isAuthenticated()) { sendUnauthorized(); return; }
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
            return;
        }
        const char *current = doc["current"] | "";
        const char *newpass = doc["new"] | "";

        // If password already set, verify current password
        if (hasPassword() && strcmp(current, admin_password) != 0) {
            server.send(401, "application/json", "{\"ok\":false,\"error\":\"Current password incorrect\"}");
            return;
        }

        // Empty new password = remove password (disable auth)
        if (strlen(newpass) == 0) {
            clearAdminPass();
            server.send(200, "application/json", "{\"ok\":true}");
            return;
        }

        size_t len = strlen(newpass);
        if (len < 4 || len > 6) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Password must be 4-6 characters\"}");
            return;
        }
        strlcpy(admin_password, newpass, sizeof(admin_password));
        saveAdminPass();

        // Create a session so the user stays logged in
        const char *token = createSession();
        String cookie     = "kb_session=";
        cookie += token;
        cookie += "; Path=/; HttpOnly";
        server.sendHeader("Set-Cookie", cookie);
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // Redirect unknown paths to root (helps captive portal detection)
    server.onNotFound([]() {
        ESP_LOGI(TAG, "[HTTP] 302 %s -> /", server.uri().c_str());
        String url = "http://";
        url += wifi_sta_mode ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        url += "/";
        server.sendHeader("Location", url);
        server.send(302, "text/plain", "");
    });

    // Collect auth headers
    const char *headerKeys[] = {"Cookie", "Authorization"};
    server.collectHeaders(headerKeys, 2);

    server.begin();
    ESP_LOGI(TAG, "[WiFi] Web server listening on port 80");
}

// ############################################################
//  PAIR BUTTON + MODE JUMPER
// ############################################################

static bool pairBtnLastState    = HIGH;
static uint32_t pairBtnDownTime = 0;
static bool pairBtnTriggered    = false;

void handlePairButton() {
    if (config.pin_pair_btn < 0) return;
    bool state = digitalRead(config.pin_pair_btn);
    if (state == LOW && pairBtnLastState == HIGH) {
        pairBtnDownTime  = millis();
        pairBtnTriggered = false;
    }
    if (state == LOW && !pairBtnTriggered && millis() - pairBtnDownTime >= 100) {
        pairBtnTriggered  = true;
        bt_scan_requested = true;
        logKey("PAIR button pressed");
    }
    pairBtnLastState = state;
}

// ############################################################
//  SETUP AND LOOP
// ############################################################

extern "C" void app_main() {
    initArduino();

    Serial.begin(115200);
    delay(500);

    keyLogMutex  = xSemaphoreCreateMutex();
    config_mutex = xSemaphoreCreateMutex();
    keyQueue     = xQueueCreate(16, sizeof(KeyReport));

    // Load or create config
    if (!loadConfig(config)) {
        ESP_LOGI(TAG, "No saved config — using defaults");
        setDefaultConfig(config);
        saveConfig(config);
    }

    // Load admin password (empty = no auth until user sets one)
    loadAdminPass();

    initKeyMap();
    setupScanPins();

    // Start scan response on core 0 at highest priority
    xTaskCreatePinnedToCore(scan_response_task, "scan", 4096, NULL,
                            configMAX_PRIORITIES - 1, NULL, 0);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " KeyBridge  v5.0");
    ESP_LOGI(TAG, " Web-configurable | BT Classic + BLE");
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, " BT:      Classic=%s  BLE=%s",
             config.enable_bt_classic ? "ON" : "off",
             config.enable_ble ? "ON" : "off");
    if (config.enable_wifi) {
        if (config.sta_ssid[0] != '\0') {
            ESP_LOGI(TAG, " WiFi:    STA>AP (%s, fallback %s)", config.sta_ssid, config.wifi_ssid);
        } else {
            ESP_LOGI(TAG, " WiFi:    AP (%s)", config.wifi_ssid);
        }
        ESP_LOGI(TAG, " mDNS:    %s.local", config.hostname);
    } else {
        ESP_LOGI(TAG, " WiFi:    off");
    }
    ESP_LOGI(TAG, "========================================");

    if (config.enable_wifi) startWebServer();
#if CONFIG_SOC_USB_OTG_SUPPORTED
    if (config.enable_usb) startUsbHost();
#endif
    if (config.enable_bt_classic || config.enable_ble) startBluetooth();

    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Ready.");

    // Main loop
    static uint32_t lastHeartbeat = 0;
    while (true) {
        // Process key events
        KeyReport report;
        while (xQueueReceive(keyQueue, &report, 0) == pdTRUE) {
            processHidReport(&report);
        }

        handlePairButton();

        // Web server + captive portal DNS
        if (config.enable_wifi) {
            if (!wifi_sta_mode) dnsServer.processNextRequest();
            server.handleClient();
        }

        // LED off
        if (config.pin_led >= 0 && ledOffTime && millis() >= ledOffTime) {
            digitalWrite(config.pin_led, LOW);
            ledOffTime = 0;
        }

        // Periodic heartbeat (every 10 seconds)
        if (millis() - lastHeartbeat >= 10000) {
            lastHeartbeat = millis();
            ESP_LOGI(TAG, "[HEARTBEAT] heap=%lu stations=%d",
                     (unsigned long)esp_get_free_heap_size(),
                     WiFi.softAPgetStationNum());
        }

        delay(1);
    }
}
