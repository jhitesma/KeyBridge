/*
 * web_ui.h — Embedded web interface for KeyBridge
 *
 * Single-page config app served from ESP32 flash.
 * Uses vanilla JS + fetch API, no external dependencies.
 */

#ifndef WEB_UI_H
#define WEB_UI_H

// clang-format off
const char WEB_UI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>KeyBridge</title>
<style>
:root{--bg:#1a1a2e;--card:#16213e;--accent:#0f3460;--hi:#e94560;--text:#eee;--dim:#888;--ok:#4ecca3;--border:#2a2a4a;--input-bg:#0d1b2a}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);line-height:1.5;padding:12px;max-width:900px;margin:0 auto}
h1{font-size:1.3em;margin-bottom:4px;color:var(--hi)}
.subtitle{color:var(--dim);font-size:.85em;margin-bottom:16px}
.status-bar{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:16px;font-size:.8em}
.status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:4px;vertical-align:middle}
.dot-on{background:var(--ok)}.dot-off{background:#555}
.tabs{display:flex;gap:2px;margin-bottom:12px;flex-wrap:wrap}
.tab{padding:8px 16px;background:var(--accent);border:none;color:var(--dim);cursor:pointer;border-radius:6px 6px 0 0;font-size:.85em}
.tab.active{background:var(--card);color:var(--text)}
.panel{display:none;background:var(--card);border-radius:0 6px 6px 6px;padding:16px}
.panel.active{display:block}
.group{margin-bottom:16px}
.group-title{font-size:.9em;font-weight:600;color:var(--hi);margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px}
.row{display:flex;gap:8px;align-items:center;margin-bottom:6px;flex-wrap:wrap}
.row label{min-width:140px;font-size:.85em;color:var(--dim)}
.row input[type=number],.row input[type=text],.row select{background:var(--input-bg);border:1px solid var(--border);color:var(--text);padding:5px 8px;border-radius:4px;font-size:.85em;width:100px}
.row input[type=text]{width:200px}
.row input[type=checkbox]{accent-color:var(--hi)}
.hint{font-size:.75em;color:var(--dim);margin-left:4px}
button{padding:7px 18px;border:none;border-radius:4px;cursor:pointer;font-size:.85em;font-weight:500}
.btn-primary{background:var(--hi);color:#fff}
.btn-primary:hover{opacity:.9}
.btn-secondary{background:var(--accent);color:var(--text)}
.btn-danger{background:#8b0000;color:#fff}
.btn-sm{padding:4px 10px;font-size:.8em}
.actions{display:flex;gap:8px;margin-top:16px;flex-wrap:wrap}
table{width:100%;border-collapse:collapse;font-size:.82em}
th{text-align:left;padding:6px 8px;border-bottom:2px solid var(--border);color:var(--dim);font-weight:500}
td{padding:5px 8px;border-bottom:1px solid var(--border)}
td input{background:var(--input-bg);border:1px solid var(--border);color:var(--text);padding:3px 6px;border-radius:3px;font-size:.82em}
td input[type=text]{width:100%}
td input[type=number]{width:50px}
.mono{font-family:'SF Mono',Consolas,monospace;font-size:.8em}
.toast{position:fixed;bottom:20px;right:20px;padding:10px 20px;border-radius:6px;font-size:.85em;z-index:999;transition:opacity .3s;opacity:0;pointer-events:none}
.toast.show{opacity:1}.toast-ok{background:var(--ok);color:#000}.toast-err{background:var(--hi);color:#fff}
#keyLog{background:var(--input-bg);border:1px solid var(--border);border-radius:4px;padding:8px;font-family:monospace;font-size:.8em;height:120px;overflow-y:auto;white-space:pre;color:var(--ok);margin-top:8px}
@media(max-width:600px){.row{flex-direction:column;align-items:flex-start}.row label{min-width:auto}}
</style>
</head>
<body>

<h1>&#x2328; KeyBridge</h1>
<p class="subtitle">USB / Bluetooth &rarr; Parallel ASCII Terminal Interface</p>

<div class="status-bar" id="statusBar">
  <span><span class="status-dot dot-off" id="dotUsb"></span>USB</span>
  <span><span class="status-dot dot-off" id="dotBt"></span>Bluetooth</span>
  <span><span class="status-dot dot-off" id="dotWifi"></span>WiFi</span>
  <span>Mode: <strong id="modeLabel">—</strong></span>
</div>

<div class="tabs">
  <button class="tab active" onclick="showTab('general')">General</button>
  <button class="tab" onclick="showTab('pins')">Pins</button>
  <button class="tab" onclick="showTab('timing')">Timing</button>
  <button class="tab" onclick="showTab('keys')">Key Mappings</button>
  <button class="tab" onclick="showTab('wifi')">WiFi</button>
  <button class="tab" onclick="showTab('monitor')">Monitor</button>
</div>

<!-- GENERAL TAB -->
<div class="panel active" id="panel-general">
  <div class="group">
    <div class="group-title">Terminal Mode</div>
    <div class="row">
      <label>Mode</label>
      <select id="ansi_mode">
        <option value="false">Native</option>
        <option value="true">ANSI / VT100</option>
      </select>
    </div>
    <div class="row">
      <label>Use hardware jumper</label>
      <input type="checkbox" id="use_mode_jumper">
      <span class="hint">Overrides software setting when enabled</span>
    </div>
    <div class="row">
      <label>Strobe polarity</label>
      <select id="strobe_active_low">
        <option value="true">Active LOW (idle high)</option>
        <option value="false">Active HIGH (idle low)</option>
      </select>
    </div>
  </div>

  <div class="group">
    <div class="group-title">Features</div>
    <div class="row"><label>USB Host</label><input type="checkbox" id="feat_usb"></div>
    <div class="row"><label>Bluetooth Classic</label><input type="checkbox" id="feat_bt"></div>
    <div class="row"><label>Bluetooth LE</label><input type="checkbox" id="feat_ble"></div>
    <div class="row"><label>WiFi config server</label><input type="checkbox" id="feat_wifi">
      <span class="hint">Disabling requires reflash to re-enable</span></div>
  </div>

  <div class="group">
    <div class="group-title">Bluetooth</div>
    <button class="btn-primary" onclick="triggerPair()">&#x1F50D; Scan &amp; Pair</button>
    <span class="hint" id="btStatus"></span>
  </div>

  <div class="actions">
    <button class="btn-primary" onclick="saveAll()">&#x1F4BE; Save &amp; Apply</button>
    <button class="btn-danger" onclick="factoryReset()">Factory Reset</button>
  </div>
</div>

<!-- PINS TAB -->
<div class="panel" id="panel-pins">
  <div class="group">
    <div class="group-title">Data Output (to terminal via 74HCT245)</div>
    <div class="row"><label>D0 (bit 0, LSB)</label><input type="number" id="pin_d0" min="-1" max="48"></div>
    <div class="row"><label>D1 (bit 1)</label><input type="number" id="pin_d1" min="-1" max="48"></div>
    <div class="row"><label>D2 (bit 2)</label><input type="number" id="pin_d2" min="-1" max="48"></div>
    <div class="row"><label>D3 (bit 3)</label><input type="number" id="pin_d3" min="-1" max="48"></div>
    <div class="row"><label>D4 (bit 4)</label><input type="number" id="pin_d4" min="-1" max="48"></div>
    <div class="row"><label>D5 (bit 5)</label><input type="number" id="pin_d5" min="-1" max="48"></div>
    <div class="row"><label>D6 (bit 6, MSB)</label><input type="number" id="pin_d6" min="-1" max="48"></div>
    <div class="row"><label>Strobe</label><input type="number" id="pin_strobe" min="-1" max="48"></div>
  </div>
  <div class="group">
    <div class="group-title">Control Inputs</div>
    <div class="row"><label>PAIR button</label><input type="number" id="pin_pair_btn" min="-1" max="48"></div>
    <div class="row"><label>MODE jumper</label><input type="number" id="pin_mode_jp" min="-1" max="48"></div>
  </div>
  <div class="group">
    <div class="group-title">Status LEDs (-1 = disabled)</div>
    <div class="row"><label>Activity LED</label><input type="number" id="pin_led" min="-1" max="48"></div>
    <div class="row"><label>Bluetooth LED</label><input type="number" id="pin_bt_led" min="-1" max="48"></div>
  </div>
  <div class="actions">
    <button class="btn-primary" onclick="saveAll()">&#x1F4BE; Save &amp; Apply</button>
  </div>
  <p class="hint" style="margin-top:8px">Pin changes require reboot to take effect.</p>
</div>

<!-- TIMING TAB -->
<div class="panel" id="panel-timing">
  <div class="group">
    <div class="group-title">Strobe Timing</div>
    <div class="row"><label>Data setup (µs)</label><input type="number" id="data_setup_us" min="1" max="100"></div>
    <div class="row"><label>Strobe pulse (µs)</label><input type="number" id="strobe_pulse_us" min="1" max="100"></div>
    <div class="row"><label>Inter-char delay (µs)</label><input type="number" id="inter_char_delay_us" min="10" max="5000">
      <span class="hint">Between chars in escape sequences</span></div>
  </div>
  <div class="group">
    <div class="group-title">Auto-Repeat</div>
    <div class="row"><label>Initial delay (ms)</label><input type="number" id="repeat_delay_ms" min="100" max="2000"></div>
    <div class="row"><label>Repeat rate (ms)</label><input type="number" id="repeat_rate_ms" min="20" max="500">
      <span class="hint">Lower = faster (67ms ≈ 15/sec)</span></div>
  </div>
  <div class="actions">
    <button class="btn-primary" onclick="saveAll()">&#x1F4BE; Save &amp; Apply</button>
  </div>
</div>

<!-- KEY MAPPINGS TAB -->
<div class="panel" id="panel-keys">
  <div class="group">
    <div class="group-title">Special Key Mappings</div>
    <p class="hint" style="margin-bottom:8px">Define what each special key sends. Sequences are hex-encoded bytes (e.g. "1b5b41" = ESC [ A). Use the readable column as a guide.</p>
    <table>
      <thead>
        <tr><th>On</th><th>Label</th><th>HID Code</th><th>Native Seq (hex)</th><th>Readable</th><th>ANSI Seq (hex)</th><th>Readable</th><th></th></tr>
      </thead>
      <tbody id="keyTableBody"></tbody>
    </table>
    <div class="actions" style="margin-top:8px">
      <button class="btn-secondary btn-sm" onclick="addKeyRow()">+ Add Key</button>
      <button class="btn-primary" onclick="saveAll()">&#x1F4BE; Save &amp; Apply</button>
    </div>
  </div>
  <div class="group" style="margin-top:12px">
    <div class="group-title">Presets</div>
    <div class="actions">
      <button class="btn-secondary btn-sm" onclick="loadPreset('wyse50')">Wyse 50/50+</button>
      <button class="btn-secondary btn-sm" onclick="loadPreset('vt100')">VT100</button>
      <button class="btn-secondary btn-sm" onclick="loadPreset('adm3a')">ADM-3A</button>
    </div>
  </div>
</div>

<!-- WIFI TAB -->
<div class="panel" id="panel-wifi">
  <div class="group">
    <div class="group-title">Access Point Settings</div>
    <p class="hint" style="margin-bottom:8px">The adapter creates its own WiFi network. Connect to it from your phone or laptop to access this page.</p>
    <div class="row"><label>Network name (SSID)</label><input type="text" id="wifi_ssid" maxlength="32"></div>
    <div class="row"><label>Password</label><input type="text" id="wifi_password" maxlength="63">
      <span class="hint">Min 8 chars, or empty for open</span></div>
    <div class="row"><label>Channel</label><input type="number" id="wifi_channel" min="1" max="13"></div>
  </div>
  <div class="actions">
    <button class="btn-primary" onclick="saveAll()">&#x1F4BE; Save &amp; Apply</button>
  </div>
  <p class="hint" style="margin-top:8px">WiFi changes take effect after reboot.</p>
</div>

<!-- MONITOR TAB -->
<div class="panel" id="panel-monitor">
  <div class="group">
    <div class="group-title">Live Key Monitor</div>
    <p class="hint">Shows characters as they're sent to the terminal (updates every second).</p>
    <div id="keyLog">Waiting for keypresses...</div>
    <div class="actions" style="margin-top:8px">
      <button class="btn-secondary btn-sm" onclick="clearLog()">Clear</button>
    </div>
  </div>
  <div class="group" style="margin-top:12px">
    <div class="group-title">Test Output</div>
    <p class="hint">Type a character to send directly to the terminal for testing.</p>
    <div class="row">
      <label>ASCII char or hex</label>
      <input type="text" id="testChar" maxlength="4" placeholder="A or 1b" style="width:80px">
      <button class="btn-secondary btn-sm" onclick="sendTest()">Send</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
let cfg = {};
let logPoll = null;

function showTab(name) {
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.getElementById('panel-'+name).classList.add('active');
  event.target.classList.add('active');
  if (name === 'monitor' && !logPoll) startLogPoll();
  if (name !== 'monitor' && logPoll) { clearInterval(logPoll); logPoll = null; }
}

function toast(msg, ok) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show ' + (ok ? 'toast-ok' : 'toast-err');
  setTimeout(() => t.classList.remove('show'), 2500);
}

// --- Load config from device ---
async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    cfg = await r.json();
    populateForm();
    updateStatus();
  } catch(e) { toast('Failed to load config', false); }
}

function populateForm() {
  // General
  sel('ansi_mode', cfg.terminal?.ansi_mode);
  chk('use_mode_jumper', cfg.terminal?.use_mode_jumper);
  sel('strobe_active_low', cfg.terminal?.strobe_active_low);
  chk('feat_usb', cfg.features?.usb);
  chk('feat_bt', cfg.features?.bt_classic);
  chk('feat_ble', cfg.features?.ble);
  chk('feat_wifi', cfg.features?.wifi);

  // Pins
  val('pin_d0', cfg.pins?.d0); val('pin_d1', cfg.pins?.d1);
  val('pin_d2', cfg.pins?.d2); val('pin_d3', cfg.pins?.d3);
  val('pin_d4', cfg.pins?.d4); val('pin_d5', cfg.pins?.d5);
  val('pin_d6', cfg.pins?.d6); val('pin_strobe', cfg.pins?.strobe);
  val('pin_pair_btn', cfg.pins?.pair_btn);
  val('pin_mode_jp', cfg.pins?.mode_jp);
  val('pin_led', cfg.pins?.led);
  val('pin_bt_led', cfg.pins?.bt_led);

  // Timing
  val('data_setup_us', cfg.timing?.data_setup_us);
  val('strobe_pulse_us', cfg.timing?.strobe_pulse_us);
  val('inter_char_delay_us', cfg.timing?.inter_char_delay_us);
  val('repeat_delay_ms', cfg.timing?.repeat_delay_ms);
  val('repeat_rate_ms', cfg.timing?.repeat_rate_ms);

  // WiFi
  val('wifi_ssid', cfg.wifi?.ssid);
  val('wifi_password', cfg.wifi?.password);
  val('wifi_channel', cfg.wifi?.channel);

  // Key mappings
  populateKeyTable();
}

function populateKeyTable() {
  const body = document.getElementById('keyTableBody');
  body.innerHTML = '';
  (cfg.special_keys || []).forEach((k, i) => {
    body.innerHTML += keyRow(i, k);
  });
}

function keyRow(i, k) {
  return `<tr id="kr${i}">
    <td><input type="checkbox" ${k.enabled?'checked':''} data-i="${i}" data-f="enabled"></td>
    <td><input type="text" value="${esc(k.label)}" data-i="${i}" data-f="label" style="width:70px"></td>
    <td><input type="number" value="${k.keycode}" data-i="${i}" data-f="keycode" style="width:50px" class="mono"></td>
    <td><input type="text" value="${esc(k.native_hex)}" data-i="${i}" data-f="native_hex" class="mono" style="width:90px"></td>
    <td class="mono" style="color:var(--ok)">${esc(k.native_display||'')}</td>
    <td><input type="text" value="${esc(k.ansi_hex)}" data-i="${i}" data-f="ansi_hex" class="mono" style="width:90px"></td>
    <td class="mono" style="color:var(--ok)">${esc(k.ansi_display||'')}</td>
    <td><button class="btn-danger btn-sm" onclick="delKeyRow(${i})">&#x2716;</button></td>
  </tr>`;
}

function addKeyRow() {
  if (!cfg.special_keys) cfg.special_keys = [];
  cfg.special_keys.push({keycode:0,label:'',native_hex:'',ansi_hex:'',native_display:'',ansi_display:'',enabled:true});
  populateKeyTable();
}

function delKeyRow(i) {
  cfg.special_keys.splice(i, 1);
  populateKeyTable();
}

// --- Gather form data ---
function gatherConfig() {
  cfg.terminal = {
    ansi_mode: gsel('ansi_mode') === 'true',
    use_mode_jumper: gchk('use_mode_jumper'),
    strobe_active_low: gsel('strobe_active_low') === 'true'
  };
  cfg.features = {
    usb: gchk('feat_usb'), bt_classic: gchk('feat_bt'),
    ble: gchk('feat_ble'), wifi: gchk('feat_wifi')
  };
  cfg.pins = {
    d0: gnum('pin_d0'), d1: gnum('pin_d1'), d2: gnum('pin_d2'), d3: gnum('pin_d3'),
    d4: gnum('pin_d4'), d5: gnum('pin_d5'), d6: gnum('pin_d6'), strobe: gnum('pin_strobe'),
    pair_btn: gnum('pin_pair_btn'), mode_jp: gnum('pin_mode_jp'),
    led: gnum('pin_led'), bt_led: gnum('pin_bt_led')
  };
  cfg.timing = {
    strobe_pulse_us: gnum('strobe_pulse_us'), data_setup_us: gnum('data_setup_us'),
    inter_char_delay_us: gnum('inter_char_delay_us'),
    repeat_delay_ms: gnum('repeat_delay_ms'), repeat_rate_ms: gnum('repeat_rate_ms')
  };
  cfg.wifi = { ssid: gval('wifi_ssid'), password: gval('wifi_password'), channel: gnum('wifi_channel') };

  // Gather key table
  cfg.special_keys = [];
  const rows = document.querySelectorAll('#keyTableBody tr');
  rows.forEach(row => {
    const inputs = row.querySelectorAll('input');
    const entry = {};
    inputs.forEach(inp => {
      const f = inp.dataset.f;
      if (f === 'enabled') entry.enabled = inp.checked;
      else if (f === 'keycode') entry.keycode = parseInt(inp.value) || 0;
      else if (f) entry[f] = inp.value;
    });
    if (entry.keycode !== undefined) cfg.special_keys.push(entry);
  });
}

// --- Save ---
async function saveAll() {
  gatherConfig();
  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify(cfg)
    });
    const result = await r.json();
    if (result.ok) {
      toast('Saved! Changes applied.', true);
      loadConfig();
    } else {
      toast('Save failed: ' + (result.error || 'unknown'), false);
    }
  } catch(e) { toast('Save failed: ' + e, false); }
}

async function factoryReset() {
  if (!confirm('Reset all settings to factory defaults? This cannot be undone.')) return;
  try {
    await fetch('/api/reset', {method:'POST'});
    toast('Reset complete. Rebooting...', true);
    setTimeout(() => location.reload(), 3000);
  } catch(e) { toast('Reset failed', false); }
}

async function triggerPair() {
  document.getElementById('btStatus').textContent = 'Scanning...';
  try {
    const r = await fetch('/api/bt/pair', {method:'POST'});
    const result = await r.json();
    document.getElementById('btStatus').textContent = result.message || 'Scan initiated';
  } catch(e) { document.getElementById('btStatus').textContent = 'Error'; }
}

async function sendTest() {
  const v = document.getElementById('testChar').value.trim();
  if (!v) return;
  try {
    await fetch('/api/test', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({char: v})});
    toast('Sent', true);
  } catch(e) { toast('Send failed', false); }
}

// --- Status polling ---
async function updateStatus() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    dot('dotUsb', s.usb_connected);
    dot('dotBt', s.bt_connected);
    dot('dotWifi', true);
    document.getElementById('modeLabel').textContent = s.ansi_mode ? 'ANSI' : 'Native';
  } catch(e) {}
}

function startLogPoll() {
  logPoll = setInterval(async () => {
    try {
      const r = await fetch('/api/log');
      const data = await r.json();
      if (data.entries && data.entries.length > 0) {
        const log = document.getElementById('keyLog');
        data.entries.forEach(e => { log.textContent += e + '\n'; });
        log.scrollTop = log.scrollHeight;
      }
    } catch(e) {}
  }, 1000);
}

function clearLog() { document.getElementById('keyLog').textContent = ''; }

// --- Terminal presets ---
async function loadPreset(name) {
  try {
    const r = await fetch('/api/preset/' + name);
    const data = await r.json();
    if (data.special_keys) {
      cfg.special_keys = data.special_keys;
      populateKeyTable();
      toast('Preset loaded — click Save to apply', true);
    }
  } catch(e) { toast('Preset not found', false); }
}

// --- Helpers ---
function $(id) { return document.getElementById(id); }
function val(id, v) { if ($(id) && v !== undefined) $(id).value = v; }
function sel(id, v) { if ($(id) && v !== undefined) $(id).value = String(v); }
function chk(id, v) { if ($(id)) $(id).checked = !!v; }
function gval(id) { return $(id) ? $(id).value : ''; }
function gnum(id) { return $(id) ? parseInt($(id).value) || 0 : 0; }
function gsel(id) { return $(id) ? $(id).value : ''; }
function gchk(id) { return $(id) ? $(id).checked : false; }
function dot(id, on) { const d = $(id); if(d) d.className = 'status-dot ' + (on?'dot-on':'dot-off'); }
function esc(s) { return String(s||'').replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;'); }

// Init
loadConfig();
setInterval(updateStatus, 5000);
</script>
</body>
</html>
)rawliteral";
// clang-format on

#endif // WEB_UI_H
