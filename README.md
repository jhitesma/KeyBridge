# Wyse 50+ USB / Bluetooth Keyboard Adapter  v4.0

Universal parallel ASCII keyboard adapter with a **web-based configuration
interface**. Connects modern USB or Bluetooth keyboards to vintage terminals
that use a 7-bit parallel ASCII keyboard interface (Wyse 50/50+, ADM-3A,
VT100 with parallel keyboard, and others).

All settings — pin assignments, key mappings, terminal mode, timing — are
configurable at runtime through a WiFi web interface. No reflashing needed.

## Features

- **USB, Bluetooth Classic, and BLE** keyboard support (all simultaneously)
- **Web configuration interface** via WiFi access point
- **Editable key mappings** — customize escape sequences for any terminal
- **Built-in presets** for Wyse 50+, VT100, and ADM-3A
- **Live key monitor** — watch characters as they're sent to the terminal
- **Test mode** — send individual characters from the web UI
- **Hardware controls** — PAIR button for Bluetooth, MODE jumper for Wyse/ANSI
- **Persistent settings** — saved to flash, survives power cycles
- **Factory reset** — via web UI or by erasing NVS

## How It Works

```
                    ┌─────────────────────────────────┐
USB Keyboard ──────>│                                 │
                    │           ESP32-S3               │
BT Keyboard ·····>│                                 ├──> 74HCT245 ──> Terminal
                    │  WiFi AP: "Wyse-Adapter"        │    (3.3V→5V)    DIN port
BLE Keyboard ····>│  Config:  http://192.168.4.1    │
                    │                                 │
                    │  [PAIR btn]  [MODE jumper]       │
                    └─────────────────────────────────┘
```

## Parts List

| Part                        | Qty | Notes                                  |
|-----------------------------|-----|----------------------------------------|
| ESP32-S3 DevKitC (USB-C)   | 1   | Must be S3 for USB Host + BT + WiFi   |
| USB-C to USB-A adapter      | 1   | For USB keyboards                      |
| 74HCT245 octal buffer       | 1   | Level shift 3.3V→5V (DIP-20)          |
| 0.1µF ceramic capacitor     | 1   | Decoupling for 74HCT245               |
| DIN connector                | 1   | Match your terminal's keyboard port    |
| Momentary pushbutton         | 1   | BT PAIR button (normally open)         |
| 2-pin header + jumper shunt  | 1   | MODE select (Wyse/ANSI)               |
| LED + 330Ω resistor          | 1-2 | Optional: activity + BT status         |

**Total: ~$12-18**

## Complete Wiring

### ESP32-S3 → 74HCT245 → Terminal DIN

```
ESP32-S3          74HCT245           Terminal DIN
─────────         ────────           ────────────
GPIO 4  ───────── A1    B1 ───────── Data 0 (D0, LSB)
GPIO 5  ───────── A2    B2 ───────── Data 1 (D1)
GPIO 6  ───────── A3    B3 ───────── Data 2 (D2)
GPIO 7  ───────── A4    B4 ───────── Data 3 (D3)
GPIO 15 ───────── A5    B5 ───────── Data 4 (D4)
GPIO 16 ───────── A6    B6 ───────── Data 5 (D5)
GPIO 17 ───────── A7    B7 ───────── Data 6 (D6, MSB)
GPIO 18 ───────── A8    B8 ───────── Strobe (active LOW)

3.3V    ───────── DIR                (A→B direction)
GND     ───────── OE                 (always enabled)
                  GND ─── GND        (common ground)
                  VCC ─── +5V        (from terminal or external)
```

All pin assignments are changeable through the web interface.

### 74HCT245 Pinout (DIP-20)

```
             ┌──── U ────┐
   DIR (3V3) │ 1      20 │ VCC (+5V)
     A1 (D0) │ 2      19 │ OE (→ GND)
     A2 (D1) │ 3      18 │ B1 (→ DIN D0)
     A3 (D2) │ 4      17 │ B2 (→ DIN D1)
     A4 (D3) │ 5      16 │ B3 (→ DIN D2)
     A5 (D4) │ 6      15 │ B4 (→ DIN D3)
     A6 (D5) │ 7      14 │ B5 (→ DIN D4)
     A7 (D6) │ 8      13 │ B6 (→ DIN D5)
  A8 (Strobe)│ 9      12 │ B7 (→ DIN D6)
         GND │ 10     11 │ B8 (→ DIN Strobe)
             └───────────┘

Place 0.1µF cap between pin 20 (VCC) and pin 10 (GND).
```

### Direct Wiring (No Level Shifter)

Many 5V TTL inputs accept 3.3V as logic high. You can try connecting
ESP32 GPIOs directly to the DIN pins. If characters are garbled or
missing, add the 74HCT245.

### PAIR Button

```
GPIO 0 ────┤ Button ├──── GND
            (N.O.)
```

GPIO 0 is the BOOT button on most dev boards — use it for prototyping.
Press to scan for Bluetooth keyboards.

### MODE Jumper

```
GPIO 38 ────┤ Jumper ├──── GND
```

Open = Wyse native, Closed = ANSI/VT100. Readable at runtime.
Can also be set via the web interface (disable "use hardware jumper"
to control mode purely in software).

### Power

**Option A**: Terminal-powered — connect terminal's +5V to ESP32 VIN pin.

**Option B**: USB-powered — plug ESP32 UART port into USB power/computer.

Don't connect both simultaneously.

### Terminal DIN Connector

**You must verify the pinout for your specific terminal.** Use a
multimeter to find +5V and GND first, then trace data lines on the PCB.

Expected signals: D0-D6 (7 data), Strobe (active-low), +5V, GND.

## Building

### PlatformIO

```bash
pip install platformio
cd wyse_usb_keyboard
pio run -t upload
pio device monitor
```

### Arduino IDE

1. Install ESP32 board support (Arduino Core 3.x)
2. Install **ArduinoJson** library (v7+) via Library Manager
3. Board: **ESP32S3 Dev Module**
4. USB Mode: **USB-OTG (TinyUSB)**
5. USB CDC On Boot: **Disabled**
6. Upload and open Serial Monitor at 115200

## Web Configuration Interface

### Connecting

1. Power up the adapter
2. On your phone/laptop, connect to WiFi network **"Wyse-Adapter"**
   (default password: **terminal50**)
3. Open a browser and go to **http://192.168.4.1**

The web UI has six tabs:

### General Tab
- Terminal mode (Wyse/ANSI) and hardware jumper enable
- Strobe polarity
- Feature toggles (USB, BT Classic, BLE, WiFi)
- Bluetooth pairing button

### Pins Tab
- All GPIO assignments for data lines, strobe, buttons, LEDs
- Changes require reboot

### Timing Tab
- Strobe pulse width and data setup time
- Inter-character delay for escape sequences
- Auto-repeat delay and rate

### Key Mappings Tab
- Full table of special key definitions
- Each row: HID scancode, label, Wyse sequence, ANSI sequence
- Sequences shown in hex and human-readable format
- Add/remove keys, enable/disable individual mappings
- **Presets**: One-click load for Wyse 50+, VT100, or ADM-3A

### WiFi Tab
- Access point SSID, password, and channel
- Changes take effect after reboot

### Monitor Tab
- Live display of characters being sent to the terminal
- Test output: type a character or hex code to send directly

## Using USB Keyboards

Plug a standard USB keyboard into the ESP32-S3's **native USB-C port**
(not the UART/debug port) using a USB-C to USB-A adapter.

Detection is automatic. Hot-plugging works.

## Using Bluetooth Keyboards

Bluetooth does NOT auto-scan. You must initiate pairing:

1. Put your Bluetooth keyboard into **pairing mode**
2. Either press the **PAIR button** on the adapter, or click
   **Scan & Pair** on the web interface
3. The adapter scans for 5 seconds and connects to the strongest signal
4. The Monitor tab shows connection status

To switch keyboards, press PAIR again (disconnects current, rescans).

### WiFi + Bluetooth Coexistence

The ESP32-S3 runs WiFi and Bluetooth simultaneously using hardware
time-division multiplexing on its shared 2.4GHz radio. Both work fine
for this application — keyboard HID reports are tiny and infrequent
compared to the radio's bandwidth, and web config page loads are
occasional.

## Adapting for Other Terminals

The web interface makes it easy to adapt this for any terminal that uses
a parallel ASCII keyboard interface. Just change the key mappings:

1. Connect to the web UI
2. Go to the **Key Mappings** tab
3. Load a preset or manually edit sequences
4. Click **Save & Apply**

### Adding a New Terminal Type

For each special key, you need to know what character or escape sequence
the terminal expects. Check the terminal's programmer's guide. Enter
sequences as hex bytes in the mapping table.

Common sequence formats:
- Single control char: `0b` (Ctrl-K)
- ESC + char: `1b4a` (ESC J)
- CSI sequence: `1b5b41` (ESC [ A)
- Ctrl-A prefix: `0140` (Ctrl-A @)

## Troubleshooting

**Can't connect to WiFi AP**: Default SSID is "Wyse-Adapter", password
"terminal50". If you changed these and forgot, do a factory reset by
erasing NVS (hold BOOT button during flash, or reflash firmware).

**No response from terminal**: Check DIN pinout, strobe polarity (try
both), and voltage levels (add 74HCT245 if needed).

**Garbled characters**: Data lines swapped at DIN — verify D0-D6 order.

**Bluetooth won't connect**: Press PAIR button first, ensure keyboard is
in pairing mode. Check Monitor tab for scan results.

**Function keys wrong**: Check Key Mappings tab matches your terminal's
personality mode. Load the correct preset.

**Settings lost after power cycle**: Should not happen — config is saved
to NVS flash. If it does, check that `saveConfig` succeeded (watch
serial monitor for errors).

## Factory Reset

Three ways to reset to defaults:

1. **Web UI**: General tab → Factory Reset button
2. **Serial**: Send `FACTORY_RESET` at 115200 baud (not yet implemented)
3. **Reflash**: Erase flash with `pio run -t erase` then re-upload

## Files

| File                    | Purpose                                  |
|-------------------------|------------------------------------------|
| `wyse_usb_keyboard.ino` | Main firmware (USB, BT, web server)     |
| `config.h`              | Config structure, NVS storage, JSON API  |
| `web_ui.h`              | Embedded HTML/CSS/JS web interface       |
| `platformio.ini`        | Build configuration                      |
