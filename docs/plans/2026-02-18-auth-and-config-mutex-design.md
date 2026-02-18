# Auth, Session Management, and Config Mutex

**Date:** 2026-02-18
**Origin:** Code review findings #1 (config race condition) and #2 (no API authentication)
**Context:** KeyBridge may be used at classic computer festivals with open WiFi and multiple devices on the same network. Unauthenticated API access allows anyone on the network to reconfigure devices, inject keystrokes, or factory reset.

---

## 1. Config Mutex

### Problem

`config` is a global struct read by the main loop, BT callbacks, and USB callbacks across both cores. The web server POST handler writes to it without synchronization, risking partial reads during updates.

### Design

A single `SemaphoreHandle_t config_mutex` created in `setup()`.

**Write path (web server):** Take mutex before `jsonToConfig()` + `saveConfig()`, release after.

**Read paths (main loop, BT callbacks):** Take mutex before reading config fields (pin numbers, timing values, `ansi_mode`), release after. Uses `xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100))` with a timeout — if the lock isn't available within 100ms, the reader proceeds with stale values. This is safe because config changes are human-speed and reads are keypress-speed.

**Files:** `src/keybridge.cpp` — add mutex global, init in `setup()`, wrap reads/writes.

---

## 2. Authentication and Session Management

### Default Device Identity

On first boot, the AP SSID defaults to `KeyBridge-XXXX` where XXXX is the last 4 hex digits of the MAC address. This ensures multiple devices are distinguishable out of the box. The user can rename via the Settings tab (which updates both `wifi_ssid` and `hostname` together).

### Password

On first boot, the device generates a random 6-character alphanumeric password and stores it in NVS (separate from the config blob, so factory reset also resets the password to a new generated value). The password is printed to the serial console on first boot so the owner can read it during initial setup.

The password is changeable from the web UI Settings tab (requires current password + new password).

### Session Tokens

On successful login (`POST /api/login` with `{"password": "..."}`), the server generates a 32-character random hex token and stores it in a fixed-size session table (max 4 concurrent sessions). Each entry holds the token and a last-activity timestamp. Sessions expire after 30 minutes of inactivity; the timestamp refreshes on each authenticated request.

The response returns `{"ok": true, "token": "..."}` with a `Set-Cookie: kb_session=<token>; Path=/; HttpOnly` header.

Token checking on each request: look for cookie `kb_session` first, then fall back to `Authorization: Bearer <token>` header. Cookie works transparently for the web UI; the header supports scripted/programmatic access.

Failed login attempts get a 1-second `delay()` to slow brute force.

### Endpoint Protection

| Endpoint | Auth | Rationale |
|---|---|---|
| `GET /` | No | Serves login page (or UI if already authed) |
| `POST /api/login` | No | This is the login endpoint |
| `GET /api/status` | No | Device name and connection state only, no sensitive data |
| `GET /api/config` | Yes | Exposes full configuration including WiFi credentials |
| `POST /api/config` | Yes | Modifies device configuration |
| `POST /api/bt/pair` | Yes | Initiates Bluetooth pairing scan |
| `POST /api/reset` | Yes | Factory reset — destructive |
| `POST /api/test` | Yes | Sends characters to the terminal |
| `GET /api/log` | Yes | Keypress log could be sensitive |
| `GET /api/preset/*` | Yes | Loads key mapping presets |

Unauthenticated requests to protected endpoints receive `401 Unauthorized` with `{"ok": false, "error": "Unauthorized"}`.

### Login UI

The existing SPA in `web_ui.h` gains a login screen (conditional show/hide, not a separate page). It displays:

- Device name as a heading (so at a festival you know which KeyBridge you're on)
- Password input field
- Login button

On successful login, the cookie is set and the main UI appears. On failure, a "Wrong password" toast.

### Settings Tab Changes

The WiFi/Settings area gains:

- **Device Name** field — updates both `wifi_ssid` (AP SSID) and `hostname` (mDNS) together, so the AP name and `<hostname>.local` stay in sync.
- **Change Password** — current password + new password fields, validated server-side.

### First-Boot Experience

1. User connects to `KeyBridge-A3F2` (MAC-derived default SSID) with the default WiFi password
2. Opens web UI, sees login page showing device name
3. Reads the generated admin password from serial console output
4. Logs in, changes device name and password from Settings

---

## 3. Implementation Scope

### New NVS Storage

- `admin_pass` — stored as a standalone NVS string (not in the config blob), so factory reset generates a fresh password.
- Session table is in-memory only (lost on reboot, which is fine — users just log in again).

### New API Endpoint

- `POST /api/login` — accepts `{"password": "..."}`, returns token + sets cookie.

### Files Modified

- `src/keybridge.cpp` — config mutex, session table, auth middleware, login endpoint, default SSID with MAC suffix, serial password display on first boot
- `src/config.h` — no struct changes needed (device name fields already exist)
- `src/web_ui.h` — login screen, device name + password change in Settings tab

### What This Does NOT Include

- HTTPS (ESP32 resource constraints make this impractical for a web UI)
- CSRF tokens (the session cookie + same-origin policy covers the festival threat model; there's no sensitive cross-origin interaction pattern)
- STA retry loop (finding #3, deferred)
- USB hub enumeration (finding #4, deferred)
