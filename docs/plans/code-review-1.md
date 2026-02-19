# Code Review Findings — KeyBridge

## Overview
- Review date: February 17, 2026
- Reviewer: Codex (GPT-5)
- Scope: `src/keybridge.cpp`, `src/config.h`, web endpoints, Bluetooth/USB workflows, WiFi behavior.

## Findings

### 1. Configuration updates are not synchronized
- `config` is reassigned inside the `/api/config` POST handler without a mutex, while Bluetooth callbacks, scan tasks, and the main loop read the same struct concurrently.
- A write of ~1 KB can race with readers and leave them with partially updated GPIO numbers or timing values, producing undefined behavior or crashes.
- **Suggested fix**: Gate all config mutations through a FreeRTOS mutex or queue changes to the main loop so only one task writes the struct. Consider splitting immutable vs. runtime-changeable fields to minimize copying.

### 2. No authentication or transport security for WiFi APIs
- Default credentials (`KeyBridge` / `terminal50`) ship in plaintext and never expire.
- All API routes (`/api/config`, `/api/test`, `/api/reset`, `/api/bt/pair`, etc.) accept unauthenticated requests from anyone on the AP or LAN.
- A malicious page on the same network can reconfigure the device (CSRF) or inject keystrokes into the terminal.
- **Suggested fix**: Require a per-device admin password/token before servicing POST/PUT routes, support HTTPS when feasible, disable `/api/test` and `/api/reset` unless explicitly enabled, and add CSRF protection (random form token or header requirement).

### 3. Station-mode fallback never retries
- `startWebServer()` tries to join STA once at boot. If the WiFi router is offline or credentials change, the device switches to AP mode permanently until reboot.
- README promises STA→AP fallback, but there is no AP→STA retry loop.
- **Suggested fix**: Run a background task that periodically retries STA when credentials exist, and automatically revert to AP only after repeated failures. Surface the current mode and last error in the status endpoint.

### 4. USB host assumes device address 1
- `usb_host_device_open(…, 1, …)` hard-codes address 1 during enumeration.
- The ESP-IDF host stack assigns addresses dynamically; plugging through hubs or after another peripheral consumes address 1 causes the open call to fail even though a device is present.
- **Suggested fix**: Capture `msg->new_dev.address` inside `usb_client_event_cb()` and open whichever address the host assigned. Keep track of multiple devices if needed and close them properly on `DEV_GONE`.

## Next Steps
1. Design and implement an authentication scheme for the web UI, including password storage, login flow, and CSRF mitigation.
2. Wrap configuration access in a mutex or serialize updates through the main loop.
3. Implement STA reconnect monitoring so the adapter returns to the user’s network automatically.
4. Rework USB enumeration to use the actual device address reported by the USB host stack and test with hubs/multiple devices.
