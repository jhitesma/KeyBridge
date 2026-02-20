# KeyBridge — Bluetooth Keyboard to Vintage Terminal Adapter

> **WARNING: This project is incomplete and non-functional.** The firmware
> compiles and boots but does not yet successfully connect a keyboard to a
> terminal. It is an experiment in AI-assisted vibe coding (built primarily
> with Claude Code). Use at your own risk — expect bugs, wrong assumptions,
> and incomplete features. Contributions and reality checks welcome.

Bridges a modern Bluetooth keyboard to a vintage **Wyse 50** terminal by
emulating the original keyboard's scan matrix interface. The ESP32 reads
scan addresses from the terminal's keyboard connector (J3), looks up which
keys are currently pressed (from Bluetooth HID input), and drives the Key
Return line accordingly.

A WiFi web interface provides runtime configuration, scan diagnostics, and
key mapping tools — no reflashing needed after initial setup.

## Current Status

**What works:**
- ESP32 boots, creates WiFi AP, serves the web configuration UI
- Web UI for configuration, status monitoring, key log
- NVS-persistent configuration with JSON API
- Captive portal in AP mode
- Optional password authentication for web UI

**What doesn't work yet:**
- Bluetooth keyboard pairing and HID input (Classic BT code exists but untested on original ESP32)
- Wyse 50 scan matrix output (planned, not yet implemented)
- HID-to-Wyse50 key address mapping (addresses unknown, need empirical discovery)
- USB keyboard input (ESP32-S3 only; original ESP32 has no USB host)

**Known issues:**
- USB host code assumes device address 1, breaks with hubs (see `docs/plans/code-review-1.md`)
- BLE scan filters may miss keyboards that don't advertise HID UUID in advertisements
- Station-mode WiFi has no automatic reconnect after failure

## Architecture

```
                    ┌────────────────────────────────────┐
                    │           ESP32 (WROOM-32)         │
BT Keyboard ·····> │                                    │
(Classic BT)       │  Scan response task (core 0):      │
                    │    Read 7-bit address from J3      │
                    │    Look up key_state[addr]         │  TXS0108E    Wyse 50
                    │    Drive Key Return via MOSFET  ───┼──(5V↔3.3V)──► J3 port
                    │                                    │
                    │  WiFi AP or STA mode               │
                    │  Config: http://keybridge.local    │
                    └────────────────────────────────────┘
```

### Why original ESP32 (not ESP32-S3)?

The Keychron K2 (our test keyboard) uses a Broadcom BCM20730 chipset —
**Classic Bluetooth only**, hardware-incapable of BLE. The ESP32-S3 only
supports BLE, so it can't pair with the K2 or many other popular BT
keyboards. The original ESP32 supports Classic BT + BLE + WiFi
simultaneously.

### Why scan matrix (not parallel ASCII)?

The Wyse 50 keyboard connector (J3) is **not** a parallel ASCII input. The
terminal's 8031 CPU scans a 7-bit address bus and reads a single Key Return
line to detect which keys are pressed — the same protocol used by the
original keyboard's passive switch matrix. KeyBridge emulates this matrix.

## Hardware

### Parts

| Part | Qty | Notes |
|------|-----|-------|
| ESP32 DevKit (WROOM-32) | 1 | Original ESP32, not S3 (need Classic BT) |
| TXS0108E breakout | 1 | Bidirectional level shifter (3.3V ↔ 5V) |
| 2N7000 N-channel MOSFET | 1 | Drives Key Return line |
| Wire / headers | — | For J3 connector wiring |

### Wyse 50 J3 Keyboard Connector

| J3 Pin | Signal | Direction | ESP32 GPIO |
|--------|--------|-----------|------------|
| 1 | Chassis Ground | — | — |
| 2 | Logic Ground | — | GND |
| 3 | +5V | Power | TXS0108E VB |
| 4 | Address bit 2 | Terminal > ESP32 | GPIO 14 |
| 5 | Address bit 1 | Terminal > ESP32 | GPIO 5 |
| 6 | Address bit 0 | Terminal > ESP32 | GPIO 4 |
| 7 | Address bit 3 | Terminal > ESP32 | GPIO 15 |
| 8 | Address bit 5 | Terminal > ESP32 | GPIO 16 |
| 9 | Address bit 6 | Terminal > ESP32 | GPIO 17 |
| 10 | Address bit 4 | Terminal > ESP32 | GPIO 13 |
| 11 | Key Return | ESP32 > Terminal | GPIO 18 (via 2N7000) |
| 12 | N/C | — | — |

GPIOs 6-11 are reserved for internal flash on the original ESP32 — the
defaults above avoid those pins. All pin assignments are configurable via
the web UI.

### Wiring

See `docs/plans/2026-02-19-wyse50-keyboard-interface.md` for detailed
TXS0108E and MOSFET wiring diagrams.

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run                          # Compile
pio run -t upload                # Compile and flash
pio run -t upload -t monitor     # Flash and open serial monitor
pio device monitor               # Serial monitor only (115200 baud)
```

## Web Interface

**First boot (AP mode):**

1. Power up the adapter
2. Connect to WiFi network **"KeyBridge"** (default password: **terminal50**)
3. A captive portal should redirect to the config page automatically
4. Or browse to **http://keybridge.local**

The web UI provides:
- **General** — Feature toggles, Bluetooth pairing, terminal mode
- **Pins** — GPIO assignments (configurable)
- **Timing** — Strobe and repeat timing parameters
- **Key Mappings** — Special key definitions and terminal presets
- **WiFi** — AP/STA settings, hostname, mDNS
- **Monitor** — Live key log and test output

Authentication is optional — set a password via the web UI to enable it.

## Planned Work

The implementation plan is in `docs/plans/2026-02-19-wyse50-keyboard-interface.md`. Summary:

1. **Port to original ESP32 with Classic BT** — new PlatformIO env, sdkconfig changes, guard USB code
2. **Replace parallel output with scan input pins** — update config struct for 7 address inputs + key return
3. **Implement scan response engine** — tight polling loop on core 0 reading address bus, driving key return
4. **HID-to-Wyse50 key mapping** — lookup table from HID keycodes to Wyse 50 scan addresses
5. **Scan snoop / discovery tools** — API endpoints for empirically mapping key addresses
6. **Update web UI** — scan test interface, address sweep, visual key map builder
7. **Populate key map** — manual testing with real Wyse 50 hardware
8. **Clean up dead code** — remove parallel ASCII output, escape sequence mapping

## Files

| File | Purpose |
|------|---------|
| `src/keybridge.cpp` | Main firmware (BT, WiFi, web server, GPIO) |
| `src/config.h` | Config structure, NVS storage, JSON API |
| `src/web_ui.h` | Embedded HTML/CSS/JS web interface |
| `src/esp_hid_gap.c` | BLE/Classic BT GAP and scan logic |
| `sdkconfig.defaults` | ESP-IDF Kconfig overrides |
| `platformio.ini` | Build configuration |
| `docs/plans/` | Implementation plans and code review notes |

## License

MIT
