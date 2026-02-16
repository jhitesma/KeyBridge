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

// Maximum number of custom special key mappings
#define MAX_SPECIAL_KEYS 32

// A single special key mapping: HID scancode -> escape sequence
struct SpecialKeyEntry {
    uint8_t hid_keycode; // USB HID keycode (0x00-0xFF)
    char seq_native[16]; // Native-mode sequence (null-terminated)
    char seq_ansi[16];   // ANSI-mode sequence (null-terminated)
    char label[24];      // Human-readable label (e.g. "F1", "Up")
    bool enabled;
};

struct AdapterConfig {
    // --- Pin assignments ---
    int8_t pin_d0;
    int8_t pin_d1;
    int8_t pin_d2;
    int8_t pin_d3;
    int8_t pin_d4;
    int8_t pin_d5;
    int8_t pin_d6;
    int8_t pin_strobe;
    int8_t pin_pair_btn;
    int8_t pin_mode_jp;
    int8_t pin_led;
    int8_t pin_bt_led;

    // --- Terminal settings ---
    bool ansi_mode;         // false = Native, true = ANSI (default from jumper)
    bool use_mode_jumper;   // true = read from hardware jumper
    bool strobe_active_low; // true = idle HIGH, pulse LOW

    // --- Timing ---
    uint16_t strobe_pulse_us;
    uint16_t data_setup_us;
    uint16_t inter_char_delay_us;
    uint16_t repeat_delay_ms;
    uint16_t repeat_rate_ms;

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

    // --- Special key mappings ---
    uint8_t num_special_keys;
    SpecialKeyEntry special_keys[MAX_SPECIAL_KEYS];
};

// ============================================================
// DEFAULT CONFIGURATION
// ============================================================

static void setDefaultConfig(AdapterConfig &cfg) {
    // Pins
    cfg.pin_d0       = 4;
    cfg.pin_d1       = 5;
    cfg.pin_d2       = 6;
    cfg.pin_d3       = 7;
    cfg.pin_d4       = 15;
    cfg.pin_d5       = 16;
    cfg.pin_d6       = 17;
    cfg.pin_strobe   = 18;
    cfg.pin_pair_btn = 0;
    cfg.pin_mode_jp  = 38;
    cfg.pin_led      = 2;
    cfg.pin_bt_led   = -1;

    // Terminal
    cfg.ansi_mode         = false;
    cfg.use_mode_jumper   = true;
    cfg.strobe_active_low = true;

    // Timing
    cfg.strobe_pulse_us     = 10;
    cfg.data_setup_us       = 5;
    cfg.inter_char_delay_us = 200;
    cfg.repeat_delay_ms     = 500;
    cfg.repeat_rate_ms      = 67;

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

    // Default special key mappings (101-key layout)
    cfg.num_special_keys = 22;

    struct {
        uint8_t kc;
        const char *wy;
        const char *an;
        const char *lb;
    } defaults[] = {
        {0x3A, "\x01@", "\x1b[11~", "F1"},
        {0x3B,
         "\x01"
         "A",
         "\x1b[12~", "F2"},
        {0x3C,
         "\x01"
         "B",
         "\x1b[13~", "F3"},
        {0x3D,
         "\x01"
         "C",
         "\x1b[14~", "F4"},
        {0x3E,
         "\x01"
         "D",
         "\x1b[15~", "F5"},
        {0x3F,
         "\x01"
         "E",
         "\x1b[17~", "F6"},
        {0x40,
         "\x01"
         "F",
         "\x1b[18~", "F7"},
        {0x41,
         "\x01"
         "G",
         "\x1b[19~", "F8"},
        {0x42,
         "\x01"
         "H",
         "\x1b[20~", "F9"},
        {0x43,
         "\x01"
         "I",
         "\x1b[21~", "F10"},
        {0x44,
         "\x01"
         "J",
         "\x1b[23~", "F11"},
        {0x45,
         "\x01"
         "K",
         "\x1b[24~", "F12"},
        {0x52, "\x0b", "\x1b[A", "Up"},
        {0x51, "\x0a", "\x1b[B", "Down"},
        {0x4F, "\x0c", "\x1b[C", "Right"},
        {0x50, "\x08", "\x1b[D", "Left"},
        {0x49,
         "\x1b"
         "q",
         "\x1b[2~", "Insert"},
        {0x4C,
         "\x1b"
         "W",
         "\x1b[3~", "Delete"},
        {0x4A, "\x1e", "\x1b[H", "Home"},
        {0x4D,
         "\x1b"
         "T",
         "\x1b[F", "End"},
        {0x4B,
         "\x1b"
         "J",
         "\x1b[5~", "Page Up"},
        {0x4E,
         "\x1b"
         "K",
         "\x1b[6~", "Page Down"},
    };

    for (int i = 0; i < cfg.num_special_keys; i++) {
        cfg.special_keys[i].hid_keycode = defaults[i].kc;
        strlcpy(cfg.special_keys[i].seq_native, defaults[i].wy, sizeof(cfg.special_keys[i].seq_native));
        strlcpy(cfg.special_keys[i].seq_ansi, defaults[i].an, sizeof(cfg.special_keys[i].seq_ansi));
        strlcpy(cfg.special_keys[i].label, defaults[i].lb, sizeof(cfg.special_keys[i].label));
        cfg.special_keys[i].enabled = true;
    }

    // Clear remaining slots
    for (int i = cfg.num_special_keys; i < MAX_SPECIAL_KEYS; i++) {
        memset(&cfg.special_keys[i], 0, sizeof(SpecialKeyEntry));
    }
}

// ============================================================
// NVS STORAGE
// ============================================================

static Preferences prefs;

bool saveConfig(const AdapterConfig &cfg) {
    prefs.begin("kb_cfg", false);
    size_t written = prefs.putBytes("config", &cfg, sizeof(cfg));
    prefs.putUInt("version", 5);
    prefs.end();
    return (written == sizeof(cfg));
}

bool loadConfig(AdapterConfig &cfg) {
    prefs.begin("kb_cfg", true);
    uint32_t version = prefs.getUInt("version", 0);
    if (version != 5) {
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
    for (int i = 0; i < MAX_SPECIAL_KEYS; i++) {
        cfg.special_keys[i].label[sizeof(cfg.special_keys[i].label) - 1]           = '\0';
        cfg.special_keys[i].seq_native[sizeof(cfg.special_keys[i].seq_native) - 1] = '\0';
        cfg.special_keys[i].seq_ansi[sizeof(cfg.special_keys[i].seq_ansi) - 1]     = '\0';
    }

    // Clamp num_special_keys to valid range
    if (cfg.num_special_keys > MAX_SPECIAL_KEYS) {
        cfg.num_special_keys = MAX_SPECIAL_KEYS;
    }

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

// Helper: encode a binary escape sequence as a printable hex string
// e.g. "\x1b[A" becomes "1b5b41"
String seqToHex(const char *seq) {
    String hex = "";
    while (*seq) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", (uint8_t)*seq);
        hex += buf;
        seq++;
    }
    return hex;
}

// Helper: decode hex string back to binary
void hexToSeq(const char *hex, char *out, size_t maxLen) {
    size_t i = 0, o = 0;
    size_t hexLen = strlen(hex);
    while (i + 1 < hexLen && o < maxLen - 1) {
        char byte_str[3] = {hex[i], hex[i + 1], 0};
        out[o++]         = (char)strtol(byte_str, NULL, 16);
        i += 2;
    }
    out[o] = '\0';
}

// Helper: make a printable representation for the UI
// Shows control chars as ^X notation, ESC as ESC, etc.
String seqToReadable(const char *seq) {
    String readable = "";
    while (*seq) {
        uint8_t c = (uint8_t)*seq;
        if (c == 0x1B)
            readable += "ESC ";
        else if (c < 0x20) {
            readable += "^";
            readable += (char)(c + '@');
            readable += " ";
        } else {
            readable += (char)c;
        }
        seq++;
    }
    readable.trim();
    return readable;
}

String configToJson(const AdapterConfig &cfg) {
    JsonDocument doc;

    // Pins
    JsonObject pins  = doc["pins"].to<JsonObject>();
    pins["d0"]       = cfg.pin_d0;
    pins["d1"]       = cfg.pin_d1;
    pins["d2"]       = cfg.pin_d2;
    pins["d3"]       = cfg.pin_d3;
    pins["d4"]       = cfg.pin_d4;
    pins["d5"]       = cfg.pin_d5;
    pins["d6"]       = cfg.pin_d6;
    pins["strobe"]   = cfg.pin_strobe;
    pins["pair_btn"] = cfg.pin_pair_btn;
    pins["mode_jp"]  = cfg.pin_mode_jp;
    pins["led"]      = cfg.pin_led;
    pins["bt_led"]   = cfg.pin_bt_led;

    // Terminal
    JsonObject terminal           = doc["terminal"].to<JsonObject>();
    terminal["ansi_mode"]         = cfg.ansi_mode;
    terminal["use_mode_jumper"]   = cfg.use_mode_jumper;
    terminal["strobe_active_low"] = cfg.strobe_active_low;

    // Timing
    JsonObject timing             = doc["timing"].to<JsonObject>();
    timing["strobe_pulse_us"]     = cfg.strobe_pulse_us;
    timing["data_setup_us"]       = cfg.data_setup_us;
    timing["inter_char_delay_us"] = cfg.inter_char_delay_us;
    timing["repeat_delay_ms"]     = cfg.repeat_delay_ms;
    timing["repeat_rate_ms"]      = cfg.repeat_rate_ms;

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

    // Special keys
    JsonArray keys = doc["special_keys"].to<JsonArray>();
    int count      = (cfg.num_special_keys <= MAX_SPECIAL_KEYS) ? cfg.num_special_keys : MAX_SPECIAL_KEYS;
    for (int i = 0; i < count; i++) {
        if (!cfg.special_keys[i].enabled && cfg.special_keys[i].hid_keycode == 0) continue;
        JsonObject k        = keys.add<JsonObject>();
        k["keycode"]        = cfg.special_keys[i].hid_keycode;
        k["label"]          = cfg.special_keys[i].label;
        k["native_hex"]     = seqToHex(cfg.special_keys[i].seq_native);
        k["ansi_hex"]       = seqToHex(cfg.special_keys[i].seq_ansi);
        k["native_display"] = seqToReadable(cfg.special_keys[i].seq_native);
        k["ansi_display"]   = seqToReadable(cfg.special_keys[i].seq_ansi);
        k["enabled"]        = cfg.special_keys[i].enabled;
    }

    String output;
    serializeJson(doc, output);
    return output;
}

static bool isValidGPIO(int8_t pin) {
    if (pin == -1) return true;
    if (pin < 0 || pin > 48) return false;
    if (pin >= 26 && pin <= 32) return false; // SPI flash/PSRAM
    return true;
}

bool jsonToConfig(const String &json, AdapterConfig &cfg) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;

    // Pins
    if (doc.containsKey("pins")) {
        JsonObject pins = doc["pins"];
        if (pins.containsKey("d0")) {
            int8_t v = pins["d0"];
            if (isValidGPIO(v)) cfg.pin_d0 = v;
        }
        if (pins.containsKey("d1")) {
            int8_t v = pins["d1"];
            if (isValidGPIO(v)) cfg.pin_d1 = v;
        }
        if (pins.containsKey("d2")) {
            int8_t v = pins["d2"];
            if (isValidGPIO(v)) cfg.pin_d2 = v;
        }
        if (pins.containsKey("d3")) {
            int8_t v = pins["d3"];
            if (isValidGPIO(v)) cfg.pin_d3 = v;
        }
        if (pins.containsKey("d4")) {
            int8_t v = pins["d4"];
            if (isValidGPIO(v)) cfg.pin_d4 = v;
        }
        if (pins.containsKey("d5")) {
            int8_t v = pins["d5"];
            if (isValidGPIO(v)) cfg.pin_d5 = v;
        }
        if (pins.containsKey("d6")) {
            int8_t v = pins["d6"];
            if (isValidGPIO(v)) cfg.pin_d6 = v;
        }
        if (pins.containsKey("strobe")) {
            int8_t v = pins["strobe"];
            if (isValidGPIO(v)) cfg.pin_strobe = v;
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
        if (t.containsKey("ansi_mode")) cfg.ansi_mode = t["ansi_mode"];
        if (t.containsKey("use_mode_jumper")) cfg.use_mode_jumper = t["use_mode_jumper"];
        if (t.containsKey("strobe_active_low")) cfg.strobe_active_low = t["strobe_active_low"];
    }

    // Timing
    if (doc.containsKey("timing")) {
        JsonObject t = doc["timing"];
        if (t.containsKey("strobe_pulse_us")) {
            uint16_t v = t["strobe_pulse_us"];
            if (v >= 1 && v <= 1000) cfg.strobe_pulse_us = v;
        }
        if (t.containsKey("data_setup_us")) {
            uint16_t v = t["data_setup_us"];
            if (v >= 1 && v <= 100) cfg.data_setup_us = v;
        }
        if (t.containsKey("inter_char_delay_us")) {
            uint16_t v = t["inter_char_delay_us"];
            if (v <= 10000) cfg.inter_char_delay_us = v;
        }
        if (t.containsKey("repeat_delay_ms")) {
            uint16_t v = t["repeat_delay_ms"];
            if (v >= 50 && v <= 5000) cfg.repeat_delay_ms = v;
        }
        if (t.containsKey("repeat_rate_ms")) {
            uint16_t v = t["repeat_rate_ms"];
            if (v >= 10 && v <= 1000) cfg.repeat_rate_ms = v;
        }
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

    // Special keys
    if (doc.containsKey("special_keys")) {
        JsonArray keys       = doc["special_keys"];
        cfg.num_special_keys = 0;
        for (JsonObject k : keys) {
            if (cfg.num_special_keys >= MAX_SPECIAL_KEYS) break;
            SpecialKeyEntry &entry = cfg.special_keys[cfg.num_special_keys];
            entry.hid_keycode      = k["keycode"] | 0;
            strlcpy(entry.label, k["label"] | "", sizeof(entry.label));
            hexToSeq(k["native_hex"] | "", entry.seq_native, sizeof(entry.seq_native));
            hexToSeq(k["ansi_hex"] | "", entry.seq_ansi, sizeof(entry.seq_ansi));
            entry.enabled = k["enabled"] | true;
            cfg.num_special_keys++;
        }
    }

    return true;
}

#endif // CONFIG_H
