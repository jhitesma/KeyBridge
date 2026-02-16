/*
 * ============================================================
 * KeyBridge — USB / Bluetooth to Parallel Terminal Adapter
 * ============================================================
 * Universal parallel ASCII keyboard adapter with web-based
 * configuration interface. Supports USB, BT Classic, and BLE
 * keyboards. All settings configurable at runtime via WiFi.
 *
 * Target:    ESP32-S3
 * Framework: Arduino + ESP-IDF (combined)
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// WiFi + Web Server
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <mdns.h>

// USB Host
#include "usb/usb_host.h"

// Bluetooth
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hidh.h"
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

static AdapterConfig config;          // Active configuration
static WebServer server(80);          // Web server
static QueueHandle_t keyQueue = NULL; // Shared key event queue

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
    xQueueSend(keyQueue, &report, 0);
}

// ============================================================
// GPIO OUTPUT
// ============================================================

static int8_t activePins[7] = {0};
static int8_t activeStrobe  = 0;

void setupOutputPins() {
    activePins[0] = config.pin_d0;
    activePins[1] = config.pin_d1;
    activePins[2] = config.pin_d2;
    activePins[3] = config.pin_d3;
    activePins[4] = config.pin_d4;
    activePins[5] = config.pin_d5;
    activePins[6] = config.pin_d6;
    activeStrobe  = config.pin_strobe;

    for (int i = 0; i < 7; i++) {
        if (activePins[i] >= 0) {
            pinMode(activePins[i], OUTPUT);
            digitalWrite(activePins[i], LOW);
        }
    }
    if (activeStrobe >= 0) {
        pinMode(activeStrobe, OUTPUT);
        digitalWrite(activeStrobe, config.strobe_active_low ? HIGH : LOW);
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

void sendChar(uint8_t ascii) {
    ascii &= 0x7F;

    for (int i = 0; i < 7; i++) {
        if (activePins[i] >= 0) {
            digitalWrite(activePins[i], (ascii >> i) & 0x01);
        }
    }
    delayMicroseconds(config.data_setup_us);

    if (activeStrobe >= 0) {
        digitalWrite(activeStrobe, config.strobe_active_low ? LOW : HIGH);
        delayMicroseconds(config.strobe_pulse_us);
        digitalWrite(activeStrobe, config.strobe_active_low ? HIGH : LOW);
    }

    if (ascii >= 0x20 && ascii < 0x7F) {
        logKey("TX: 0x%02X '%c'", ascii, ascii);
    } else {
        logKey("TX: 0x%02X (ctrl)", ascii);
    }
}

void sendString(const char *str) {
    while (*str) {
        sendChar((uint8_t)*str++);
        if (*str) delayMicroseconds(config.inter_char_delay_us);
    }
}

// ============================================================
// HID CONSTANTS
// ============================================================
#define MOD_LEFT_CTRL 0x01
#define MOD_LEFT_SHIFT 0x02
#define MOD_LEFT_ALT 0x04
#define MOD_LEFT_GUI 0x08
#define MOD_RIGHT_CTRL 0x10
#define MOD_RIGHT_SHIFT 0x20
#define MOD_RIGHT_ALT 0x40
#define MOD_RIGHT_GUI 0x80

// ============================================================
// SCANCODE TABLES (built-in, not configurable via web)
// ============================================================

static const uint8_t hid_to_ascii_lower[104] = {
    0x00, 0x00, 0x00, 0x00,   'a',  'b',  'c',  'd',  'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l',  'm',  'n',
    'o',  'p',  'q',  'r',    's',  't',  'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2',  '3',  '4',  '5',  '6',
    '7',  '8',  '9',  '0',    0x0D, 0x1B, 0x08, 0x09, 0x20, '-',  '=',  '[',  ']',  '\\', 0x00, ';',  '\'', '`',
    ',',  '.',  '/',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,   0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, '/',  '*',  '-',  '+',  0x0D, '1',
    '2',  '3',  '4',  '5',    '6',  '7',  '8',  '9',  '0',  '.',  0x00, 0x00, 0x00, 0x00,
};

static const uint8_t hid_to_ascii_upper[104] = {
    0x00, 0x00, 0x00, 0x00, 'A',  'B',  'C',  'D',  'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',  'M',  'N',
    'O',  'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*',  '(',  ')',  0x0D, 0x1B, 0x08, 0x09, 0x20, '_',  '+',  '{',  '}',  '|',  0x00, ':',  '"',  '~',
    '<',  '>',  '?',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, '/',  '*',  '-',  '+',  0x0D, '1',
    '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '.',  0x00, 0x00, 0x00, 0x00,
};

// ============================================================
// KEY TRANSLATION (uses configurable special key mappings)
// ============================================================

static bool capsLockOn = false;

const char *getSpecialSequence(uint8_t keycode) {
    for (int i = 0; i < config.num_special_keys; i++) {
        if (config.special_keys[i].enabled && config.special_keys[i].hid_keycode == keycode) {
            return config.ansi_mode ? config.special_keys[i].seq_ansi : config.special_keys[i].seq_native;
        }
    }
    return NULL;
}

void processKeypress(uint8_t keycode, uint8_t modifiers) {
    if (keycode == 0 || keycode > 0x67) return;

    if (config.pin_led >= 0) digitalWrite(config.pin_led, HIGH);

    if (keycode == 0x39) {
        capsLockOn = !capsLockOn;
        logKey("Caps Lock: %s", capsLockOn ? "ON" : "OFF");
        return;
    }

    bool shift = (modifiers & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT)) != 0;
    bool ctrl  = (modifiers & (MOD_LEFT_CTRL | MOD_RIGHT_CTRL)) != 0;
    bool alt   = (modifiers & (MOD_LEFT_ALT | MOD_RIGHT_ALT)) != 0;

    if (!ctrl && !alt) {
        const char *seq = getSpecialSequence(keycode);
        if (seq && seq[0]) {
            sendString(seq);
            return;
        }
    }

    bool effective_shift = shift;
    if (keycode >= 0x04 && keycode <= 0x1D) {
        effective_shift = shift ^ capsLockOn;
    }

    uint8_t ascii = effective_shift ? hid_to_ascii_upper[keycode] : hid_to_ascii_lower[keycode];

    if (ascii == 0x00) return;

    if (ctrl) {
        if (ascii >= 'a' && ascii <= 'z')
            ascii = ascii - 'a' + 1;
        else if (ascii >= 'A' && ascii <= 'Z')
            ascii = ascii - 'A' + 1;
        else if (ascii >= '[' && ascii <= '_')
            ascii = ascii - '@';
        else if (ascii == '@' || ascii == ' ')
            ascii = 0x00;
        else if (ascii == '?')
            ascii = 0x7F;
    }

    if (alt) {
        sendChar(0x1B);
        delayMicroseconds(config.inter_char_delay_us);
    }

    sendChar(ascii);
}

// ============================================================
// HID REPORT PROCESSING
// ============================================================

static uint8_t prevKeys[6]     = {0};
static uint8_t currentKey      = 0;
static uint8_t currentMods     = 0;
static uint32_t keyDownTime    = 0;
static bool repeating          = false;
static uint32_t lastRepeatTime = 0;
static uint32_t ledOffTime     = 0;

void processHidReport(const KeyReport *report) {
    uint8_t modifiers   = report->modifiers;
    const uint8_t *keys = report->keys;

    for (int i = 0; i < 6; i++) {
        if (keys[i] == 0) continue;
        bool isNew = true;
        for (int j = 0; j < 6; j++) {
            if (keys[i] == prevKeys[j]) {
                isNew = false;
                break;
            }
        }
        if (isNew) {
            processKeypress(keys[i], modifiers);
            ledOffTime  = millis() + 30;
            currentKey  = keys[i];
            currentMods = modifiers;
            keyDownTime = millis();
            repeating   = false;
        }
    }

    if (currentKey != 0) {
        bool stillHeld = false;
        for (int i = 0; i < 6; i++) {
            if (keys[i] == currentKey) {
                stillHeld = true;
                break;
            }
        }
        if (!stillHeld) {
            currentKey = 0;
            repeating  = false;
        } else
            currentMods = modifiers;
    }

    memcpy(prevKeys, keys, 6);
}

void handleAutoRepeat() {
    if (currentKey == 0) return;
    uint32_t now = millis();
    if (!repeating) {
        if (now - keyDownTime >= config.repeat_delay_ms) {
            repeating      = true;
            lastRepeatTime = now;
            processKeypress(currentKey, currentMods);
        }
    } else {
        if (now - lastRepeatTime >= config.repeat_rate_ms) {
            lastRepeatTime = now;
            processKeypress(currentKey, currentMods);
        }
    }
}

// ############################################################
//  USB HOST
// ############################################################

static usb_host_client_handle_t usb_client_hdl = NULL;
static usb_device_handle_t usb_dev_hdl         = NULL;
static usb_transfer_t *usb_xfer_in             = NULL;
static SemaphoreHandle_t usb_device_sem        = NULL;

static void usb_transfer_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes >= 8) {
        submitKeyReport(transfer->data_buffer[0], &transfer->data_buffer[2]);
    }
    if (usb_keyboard_connected) usb_host_transfer_submit(transfer);
}

static void usb_client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            xSemaphoreGive(usb_device_sem);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            usb_keyboard_connected = false;
            if (usb_dev_hdl) {
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
    volatile bool done     = false;
    ctrl->callback         = [](usb_transfer_t *t) { *(volatile bool *)t->context = true; };
    ctrl->context          = (void *)&done;
    usb_host_transfer_submit_control(usb_client_hdl, ctrl);
    for (int i = 0; i < 50 && !done; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    usb_host_transfer_free(ctrl);
}

static void usb_host_daemon_task(void *arg) {
    usb_host_config_t cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    while (true)
        usb_host_lib_handle_events(portMAX_DELAY, NULL);
}

static void usb_keyboard_task(void *arg) {
    usb_host_client_config_t cfg = {.is_synchronous    = false,
                                    .max_num_event_msg = 5,
                                    .async = {.client_event_callback = usb_client_event_cb, .callback_arg = NULL}};
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &usb_client_hdl));

    while (true) {
        usb_host_client_handle_events(usb_client_hdl, pdMS_TO_TICKS(100));
        if (!usb_keyboard_connected && xSemaphoreTake(usb_device_sem, 0) == pdTRUE) {
            if (usb_host_device_open(usb_client_hdl, 1, &usb_dev_hdl) != ESP_OK) continue;

            const usb_config_desc_t *ccfg;
            usb_host_get_active_config_descriptor(usb_dev_hdl, &ccfg);
            const uint8_t *p = (const uint8_t *)ccfg;
            int off = 0, total = ccfg->wTotalLength;
            uint8_t iface = 0;
            bool in_kbd = false, connected = false;

            while (off < total && !connected) {
                uint8_t dlen = p[off], dtype = p[off + 1];
                if (dlen == 0) break;
                if (dtype == 0x04 && dlen >= 9) {
                    iface  = p[off + 2];
                    in_kbd = (p[off + 5] == 3 && p[off + 6] == 1 && p[off + 7] == 1);
                }
                if (dtype == 0x05 && in_kbd && dlen >= 7 && (p[off + 2] & 0x80)) {
                    if (usb_host_interface_claim(usb_client_hdl, usb_dev_hdl, iface, 0) != ESP_OK) break;
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
            if (!connected) {
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
                if (config.pin_bt_led >= 0) digitalWrite(config.pin_bt_led, HIGH);
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
            if (config.pin_bt_led >= 0) digitalWrite(config.pin_bt_led, LOW);
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

void startBluetooth() {
    esp_bt_mode_t mode;
#if CONFIG_BT_CLASSIC_ENABLED
    if (config.enable_bt_classic && config.enable_ble)
        mode = ESP_BT_MODE_BTDM;
    else if (config.enable_bt_classic) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        mode = ESP_BT_MODE_CLASSIC_BT;
    } else
#endif
    {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        mode = ESP_BT_MODE_BLE;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(mode));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    esp_bt_dev_set_device_name("KeyBridge");

#if CONFIG_BT_CLASSIC_ENABLED
    if (config.enable_bt_classic) esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
#endif

    esp_hidh_config_t hidh_cfg = {.callback = hidh_callback, .event_stack_size = 4096};
    ESP_ERROR_CHECK(esp_hidh_init(&hidh_cfg));

    xTaskCreatePinnedToCore(bt_scan_task, "bt_scan", 6144, NULL, 3, NULL, 0);
    logKey("[BT] Ready. Press PAIR to connect.");
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
    WiFi.mode(WIFI_AP);
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

    // Serve the web UI
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", WEB_UI_HTML); });

    // GET config
    server.on("/api/config", HTTP_GET, []() { server.send(200, "application/json", configToJson(config)); });

    // POST config (save)
    server.on("/api/config", HTTP_POST, []() {
        String body          = server.arg("plain");
        AdapterConfig newCfg = config; // Start with current
        if (jsonToConfig(body, newCfg)) {
            config = newCfg;
            if (saveConfig(config)) {
                server.send(200, "application/json", "{\"ok\":true}");
                logKey("[Config] Saved to NVS");

                // Apply runtime-changeable settings immediately
                // (Pin changes need reboot)
            } else {
                server.send(500, "application/json", "{\"ok\":false,\"error\":\"NVS write failed\"}");
            }
        } else {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        }
    });

    // Status endpoint
    server.on("/api/status", HTTP_GET, []() {
        JsonDocument doc;
        doc["usb_connected"] = (bool)usb_keyboard_connected;
        doc["bt_connected"]  = (bool)bt_keyboard_connected;
        doc["ansi_mode"]     = config.ansi_mode;
        doc["uptime_sec"]    = millis() / 1000;
        doc["wifi_mode"]     = wifi_sta_mode ? "STA" : "AP";
        doc["wifi_ip"]       = wifi_sta_mode ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        doc["hostname"]      = config.hostname;
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    // BT pair trigger
    server.on("/api/bt/pair", HTTP_POST, []() {
        bt_scan_requested = true;
        server.send(200, "application/json", "{\"message\":\"Scan initiated — 5 seconds\"}");
    });

    // Factory reset
    server.on("/api/reset", HTTP_POST, []() {
        eraseConfig();
        server.send(200, "application/json", "{\"ok\":true}");
        delay(500);
        ESP.restart();
    });

    // Key log
    server.on("/api/log", HTTP_GET, []() {
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

    // Test character send
    server.on("/api/test", HTTP_POST, []() {
        JsonDocument doc;
        deserializeJson(doc, server.arg("plain"));
        const char *val = doc["char"] | "";
        if (strlen(val) == 1) {
            sendChar((uint8_t)val[0]);
        } else if (strlen(val) == 2) {
            // Interpret as hex
            char byte = (char)strtol(val, NULL, 16);
            sendChar((uint8_t)byte);
        }
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // Presets
    server.on("/api/preset/wyse50", HTTP_GET, []() {
        AdapterConfig tmp;
        setDefaultConfig(tmp);
        JsonDocument doc;
        JsonArray keys = doc["special_keys"].to<JsonArray>();
        for (int i = 0; i < tmp.num_special_keys; i++) {
            JsonObject k        = keys.add<JsonObject>();
            k["keycode"]        = tmp.special_keys[i].hid_keycode;
            k["label"]          = tmp.special_keys[i].label;
            k["native_hex"]     = seqToHex(tmp.special_keys[i].seq_native);
            k["ansi_hex"]       = seqToHex(tmp.special_keys[i].seq_ansi);
            k["native_display"] = seqToReadable(tmp.special_keys[i].seq_native);
            k["ansi_display"]   = seqToReadable(tmp.special_keys[i].seq_ansi);
            k["enabled"]        = true;
        }
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/preset/vt100", HTTP_GET, []() {
        // VT100 uses standard ANSI sequences in both columns
        String out =
            "{\"special_keys\":["
            "{\"keycode\":58,\"label\":\"F1\",\"native_hex\":\"1b4f50\",\"ansi_hex\":\"1b4f50\",\"native_display\":\"ESC O P\",\"ansi_display\":\"ESC O P\",\"enabled\":true},"
            "{\"keycode\":59,\"label\":\"F2\",\"native_hex\":\"1b4f51\",\"ansi_hex\":\"1b4f51\",\"native_display\":\"ESC O Q\",\"ansi_display\":\"ESC O Q\",\"enabled\":true},"
            "{\"keycode\":60,\"label\":\"F3\",\"native_hex\":\"1b4f52\",\"ansi_hex\":\"1b4f52\",\"native_display\":\"ESC O R\",\"ansi_display\":\"ESC O R\",\"enabled\":true},"
            "{\"keycode\":61,\"label\":\"F4\",\"native_hex\":\"1b4f53\",\"ansi_hex\":\"1b4f53\",\"native_display\":\"ESC O S\",\"ansi_display\":\"ESC O S\",\"enabled\":true},"
            "{\"keycode\":82,\"label\":\"Up\",\"native_hex\":\"1b5b41\",\"ansi_hex\":\"1b5b41\",\"native_display\":\"ESC [ A\",\"ansi_display\":\"ESC [ A\",\"enabled\":true},"
            "{\"keycode\":81,\"label\":\"Down\",\"native_hex\":\"1b5b42\",\"ansi_hex\":\"1b5b42\",\"native_display\":\"ESC [ B\",\"ansi_display\":\"ESC [ B\",\"enabled\":true},"
            "{\"keycode\":79,\"label\":\"Right\",\"native_hex\":\"1b5b43\",\"ansi_hex\":\"1b5b43\",\"native_display\":\"ESC [ C\",\"ansi_display\":\"ESC [ C\",\"enabled\":true},"
            "{\"keycode\":80,\"label\":\"Left\",\"native_hex\":\"1b5b44\",\"ansi_hex\":\"1b5b44\",\"native_display\":\"ESC [ D\",\"ansi_display\":\"ESC [ D\",\"enabled\":true},"
            "{\"keycode\":74,\"label\":\"Home\",\"native_hex\":\"1b5b48\",\"ansi_hex\":\"1b5b48\",\"native_display\":\"ESC [ H\",\"ansi_display\":\"ESC [ H\",\"enabled\":true}"
            "]}";
        server.send(200, "application/json", out);
    });

    server.on("/api/preset/adm3a", HTTP_GET, []() {
        // ADM-3A: very simple, arrows are Ctrl codes, no function keys
        String out =
            "{\"special_keys\":["
            "{\"keycode\":82,\"label\":\"Up\",\"native_hex\":\"0b\",\"ansi_hex\":\"0b\",\"native_display\":\"^K\",\"ansi_display\":\"^K\",\"enabled\":true},"
            "{\"keycode\":81,\"label\":\"Down\",\"native_hex\":\"0a\",\"ansi_hex\":\"0a\",\"native_display\":\"^J\",\"ansi_display\":\"^J\",\"enabled\":true},"
            "{\"keycode\":79,\"label\":\"Right\",\"native_hex\":\"0c\",\"ansi_hex\":\"0c\",\"native_display\":\"^L\",\"ansi_display\":\"^L\",\"enabled\":true},"
            "{\"keycode\":80,\"label\":\"Left\",\"native_hex\":\"08\",\"ansi_hex\":\"08\",\"native_display\":\"^H\",\"ansi_display\":\"^H\",\"enabled\":true},"
            "{\"keycode\":74,\"label\":\"Home\",\"native_hex\":\"1e\",\"ansi_hex\":\"1e\",\"native_display\":\"^^\",\"ansi_display\":\"^^\",\"enabled\":true}"
            "]}";
        server.send(200, "application/json", out);
    });

    server.begin();
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

void checkModeJumper() {
    if (!config.use_mode_jumper || config.pin_mode_jp < 0) return;
    bool newMode = (digitalRead(config.pin_mode_jp) == LOW);
    if (newMode != config.ansi_mode) {
        config.ansi_mode = newMode;
        logKey("Mode: %s (jumper)", config.ansi_mode ? "ANSI" : "Native");
    }
}

// ############################################################
//  SETUP AND LOOP
// ############################################################

extern "C" void app_main() {
    initArduino();

    Serial.begin(115200);
    delay(500);

    keyLogMutex = xSemaphoreCreateMutex();
    keyQueue    = xQueueCreate(16, sizeof(KeyReport));

    // Load or create config
    if (!loadConfig(config)) {
        ESP_LOGI(TAG, "No saved config — using defaults");
        setDefaultConfig(config);
        saveConfig(config);
    }

    // Read mode jumper at boot
    if (config.use_mode_jumper && config.pin_mode_jp >= 0) {
        pinMode(config.pin_mode_jp, INPUT_PULLUP);
        config.ansi_mode = (digitalRead(config.pin_mode_jp) == LOW);
    }

    setupOutputPins();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " KeyBridge  v5.0");
    ESP_LOGI(TAG, " Web-configurable | USB + BT + BLE");
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, " Mode:    %s", config.ansi_mode ? "ANSI/VT100" : "Native");
    ESP_LOGI(TAG, " USB:     %s", config.enable_usb ? "ON" : "off");
    ESP_LOGI(TAG, " BT:      %s", config.enable_bt_classic ? "ON" : "off");
    ESP_LOGI(TAG, " BLE:     %s", config.enable_ble ? "ON" : "off");
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
    if (config.enable_usb) startUsbHost();
    if (config.enable_bt_classic || config.enable_ble) startBluetooth();

    ESP_LOGI(TAG, "Ready.");

    // Main loop
    while (true) {
        // Process key events
        KeyReport report;
        while (xQueueReceive(keyQueue, &report, 0) == pdTRUE) {
            processHidReport(&report);
        }

        handleAutoRepeat();
        handlePairButton();

        // Mode jumper check (throttled)
        static uint8_t modeCounter = 0;
        if (++modeCounter >= 100) {
            modeCounter = 0;
            checkModeJumper();
        }

        // Web server
        if (config.enable_wifi) server.handleClient();

        // LED off
        if (config.pin_led >= 0 && ledOffTime && millis() >= ledOffTime) {
            digitalWrite(config.pin_led, LOW);
            ledOffTime = 0;
        }

        delay(1);
    }
}
