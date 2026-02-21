/*
 * config.h — Persistent configuration for KeyBridge
 *
 * All settings are stored in ESP32 NVS (Non-Volatile Storage)
 * and survive power cycles. The web interface reads/writes
 * these through a REST API.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Preferences.h>
#include <ArduinoJson.h>

// ============================================================
// CONFIGURATION STRUCTURE
// ============================================================

struct AdapterConfig {
    // --- Pin assignments (scan interface) ---
    int8_t pin_addr[7];    // Address bits A0-A6 from terminal (inputs via TXS0108E)
    int8_t pin_key_return; // Key Return to terminal (output via 2N7000 MOSFET)
    int8_t pin_pair_btn;
    int8_t pin_mode_jp;
    int8_t pin_led;
    int8_t pin_bt_led;

    // --- Terminal settings ---
    bool use_mode_jumper; // true = read from hardware jumper

    // --- Features ---
    bool enable_usb;
    bool enable_bt_classic;
    bool enable_ble;
    bool enable_wifi;

    // --- WiFi AP ---
    char wifi_ssid[33];     // AP mode SSID
    char wifi_password[65]; // AP mode password (empty = open)
    uint8_t wifi_channel;

    // --- WiFi STA ---
    char sta_ssid[33];     // STA network SSID (empty = AP-only)
    char sta_password[65]; // STA network password
    char hostname[33];     // mDNS hostname (default "keybridge")
};

// ============================================================
// DEFAULT CONFIGURATION
// ============================================================

static void setDefaultConfig(AdapterConfig &cfg) {
    // Scan interface pins (match J3 wiring table — avoid GPIOs 6-11 on ESP32)
    cfg.pin_addr[0]    = 4;   // A0 — J3 pin 6
    cfg.pin_addr[1]    = 5;   // A1 — J3 pin 5
    cfg.pin_addr[2]    = 14;  // A2 — J3 pin 4
    cfg.pin_addr[3]    = 15;  // A3 — J3 pin 7
    cfg.pin_addr[4]    = 13;  // A4 — J3 pin 10
    cfg.pin_addr[5]    = 16;  // A5 — J3 pin 8
    cfg.pin_addr[6]    = 17;  // A6 — J3 pin 9
    cfg.pin_key_return = 18;  // Key Return — J3 pin 11 (via 2N7000)
    cfg.pin_pair_btn   = 0;
    cfg.pin_mode_jp    = -1;  // No mode jumper by default on ESP32
    cfg.pin_led        = 2;
    cfg.pin_bt_led     = -1;

    // Terminal
    cfg.use_mode_jumper = false;

    // Features
    cfg.enable_usb        = true;
    cfg.enable_bt_classic = true;
    cfg.enable_ble        = true;
    cfg.enable_wifi       = true;

    // WiFi AP
    strlcpy(cfg.wifi_ssid, "KeyBridge", sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_password, "terminal50", sizeof(cfg.wifi_password));
    cfg.wifi_channel = 6;

    // WiFi STA (empty = AP-only on first boot)
    cfg.sta_ssid[0]     = '\0';
    cfg.sta_password[0] = '\0';
    strlcpy(cfg.hostname, "keybridge", sizeof(cfg.hostname));
}

// ============================================================
// NVS STORAGE
// ============================================================

static Preferences prefs;

bool saveConfig(const AdapterConfig &cfg) {
    prefs.begin("kb_cfg", false);
    size_t written = prefs.putBytes("config", &cfg, sizeof(cfg));
    prefs.putUInt("version", 7);
    prefs.end();
    return (written == sizeof(cfg));
}

bool loadConfig(AdapterConfig &cfg) {
    prefs.begin("kb_cfg", true);
    uint32_t version = prefs.getUInt("version", 0);
    if (version != 7) {
        prefs.end();
        return false; // No saved config or version mismatch
    }
    size_t readLen = prefs.getBytes("config", &cfg, sizeof(cfg));
    prefs.end();
    if (readLen != sizeof(cfg)) return false;

    // Ensure string fields are null-terminated after blob load
    cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1]         = '\0';
    cfg.wifi_password[sizeof(cfg.wifi_password) - 1] = '\0';
    cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1]           = '\0';
    cfg.sta_password[sizeof(cfg.sta_password) - 1]   = '\0';
    cfg.hostname[sizeof(cfg.hostname) - 1]           = '\0';

    return true;
}

void eraseConfig() {
    prefs.begin("kb_cfg", false);
    prefs.clear();
    prefs.end();
}

// ============================================================
// JSON SERIALIZATION (for web API)
// ============================================================

String configToJson(const AdapterConfig &cfg) {
    JsonDocument doc;

    // Pins (scan interface)
    JsonObject pins = doc["pins"].to<JsonObject>();
    for (int i = 0; i < 7; i++) {
        char key[16];
        snprintf(key, sizeof(key), "addr%d", i);
        pins[key] = cfg.pin_addr[i];
    }
    pins["key_return"] = cfg.pin_key_return;
    pins["pair_btn"]   = cfg.pin_pair_btn;
    pins["mode_jp"]    = cfg.pin_mode_jp;
    pins["led"]        = cfg.pin_led;
    pins["bt_led"]     = cfg.pin_bt_led;

    // Terminal
    JsonObject terminal         = doc["terminal"].to<JsonObject>();
    terminal["use_mode_jumper"] = cfg.use_mode_jumper;

    // Features
    JsonObject features    = doc["features"].to<JsonObject>();
    features["usb"]        = cfg.enable_usb;
    features["bt_classic"] = cfg.enable_bt_classic;
    features["ble"]        = cfg.enable_ble;
    features["wifi"]       = cfg.enable_wifi;

    // WiFi
    JsonObject wifi      = doc["wifi"].to<JsonObject>();
    wifi["ap_ssid"]      = cfg.wifi_ssid;
    wifi["ap_password"]  = cfg.wifi_password;
    wifi["ap_channel"]   = cfg.wifi_channel;
    wifi["sta_ssid"]     = cfg.sta_ssid;
    wifi["sta_password"] = cfg.sta_password;
    wifi["hostname"]     = cfg.hostname;

    String output;
    serializeJson(doc, output);
    return output;
}

static bool isValidGPIO(int8_t pin) {
    if (pin == -1) return true;
    if (pin < 0 || pin > 39) return false;       // ESP32 has GPIO 0-39
    if (pin >= 6 && pin <= 11) return false;      // Internal flash (WROOM-32)
    return true;
}

bool jsonToConfig(const String &json, AdapterConfig &cfg) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;

    // Pins (scan interface)
    if (doc.containsKey("pins")) {
        JsonObject pins = doc["pins"];
        for (int i = 0; i < 7; i++) {
            char key[16];
            snprintf(key, sizeof(key), "addr%d", i);
            if (pins.containsKey(key)) {
                int8_t v = pins[key];
                if (isValidGPIO(v)) cfg.pin_addr[i] = v;
            }
        }
        if (pins.containsKey("key_return")) {
            int8_t v = pins["key_return"];
            if (isValidGPIO(v)) cfg.pin_key_return = v;
        }
        if (pins.containsKey("pair_btn")) {
            int8_t v = pins["pair_btn"];
            if (isValidGPIO(v)) cfg.pin_pair_btn = v;
        }
        if (pins.containsKey("mode_jp")) {
            int8_t v = pins["mode_jp"];
            if (isValidGPIO(v)) cfg.pin_mode_jp = v;
        }
        if (pins.containsKey("led")) {
            int8_t v = pins["led"];
            if (isValidGPIO(v)) cfg.pin_led = v;
        }
        if (pins.containsKey("bt_led")) {
            int8_t v = pins["bt_led"];
            if (isValidGPIO(v)) cfg.pin_bt_led = v;
        }
    }

    // Terminal
    if (doc.containsKey("terminal")) {
        JsonObject t = doc["terminal"];
        if (t.containsKey("use_mode_jumper")) cfg.use_mode_jumper = t["use_mode_jumper"];
    }

    // Features
    if (doc.containsKey("features")) {
        JsonObject f = doc["features"];
        if (f.containsKey("usb")) cfg.enable_usb = f["usb"];
        if (f.containsKey("bt_classic")) cfg.enable_bt_classic = f["bt_classic"];
        if (f.containsKey("ble")) cfg.enable_ble = f["ble"];
        if (f.containsKey("wifi")) cfg.enable_wifi = f["wifi"];
    }

    // WiFi
    if (doc.containsKey("wifi")) {
        JsonObject w = doc["wifi"];
        if (w.containsKey("ap_ssid")) strlcpy(cfg.wifi_ssid, w["ap_ssid"] | "", sizeof(cfg.wifi_ssid));
        if (w.containsKey("ap_password")) strlcpy(cfg.wifi_password, w["ap_password"] | "", sizeof(cfg.wifi_password));
        if (w.containsKey("ap_channel")) {
            uint8_t ch = w["ap_channel"];
            if (ch >= 1 && ch <= 13) cfg.wifi_channel = ch;
        }
        if (w.containsKey("sta_ssid")) strlcpy(cfg.sta_ssid, w["sta_ssid"] | "", sizeof(cfg.sta_ssid));
        if (w.containsKey("sta_password")) strlcpy(cfg.sta_password, w["sta_password"] | "", sizeof(cfg.sta_password));
        if (w.containsKey("hostname")) strlcpy(cfg.hostname, w["hostname"] | "", sizeof(cfg.hostname));
    }

    return true;
}

#endif // CONFIG_H
