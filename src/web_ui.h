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
.mono{font-family:'SF Mono',Consolas,monospace;font-size:.8em}
.toast{position:fixed;bottom:20px;right:20px;padding:10px 20px;border-radius:6px;font-size:.85em;z-index:999;transition:opacity .3s;opacity:0;pointer-events:none}
.toast.show{opacity:1}.toast-ok{background:var(--ok);color:#000}.toast-err{background:var(--hi);color:#fff}
#keyLog{background:var(--input-bg);border:1px solid var(--border);border-radius:4px;padding:8px;font-family:monospace;font-size:.8em;height:120px;overflow-y:auto;white-space:pre;color:var(--ok);margin-top:8px}
#histogramBox{background:var(--input-bg);border:1px solid var(--border);border-radius:4px;padding:8px;font-family:monospace;font-size:.75em;max-height:300px;overflow-y:auto;white-space:pre;color:var(--ok);margin-top:8px}
@media(max-width:600px){.row{flex-direction:column;align-items:flex-start}.row label{min-width:auto}}
#loginScreen{display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:60vh}
.login-card{background:var(--card);border-radius:8px;padding:32px;text-align:center;width:100%;max-width:360px}
.login-card h2{color:var(--hi);margin-bottom:4px;font-size:1.2em}
.login-card .device-id{color:var(--dim);font-size:.85em;margin-bottom:20px}
.login-card input[type=password]{width:100%;padding:10px;background:var(--input-bg);border:1px solid var(--border);color:var(--text);border-radius:4px;font-size:1em;margin-bottom:12px;text-align:center}
.login-card button{width:100%;padding:10px}
</style>
</head>
<body>

<div id="loginScreen">
  <div class="login-card">
    <h2>&#x2328; KeyBridge</h2>
    <div class="device-id" id="loginDeviceName">Connecting...</div>
    <input type="password" id="loginPass" placeholder="Password" autocomplete="current-password">
    <button class="btn-primary" onclick="doLogin()">Log In</button>
  </div>
</div>

<div id="mainUI" style="display:none">
<h1>&#x2328; KeyBridge</h1>
<p class="subtitle">Bluetooth &rarr; Wyse 50 Keyboard Scan Matrix</p>

<div class="status-bar" id="statusBar">
  <span><span class="status-dot dot-off" id="dotBt"></span>Bluetooth</span>
  <span><span class="status-dot dot-off" id="dotWifi"></span>WiFi</span>
  <span>Uptime: <strong id="uptimeLabel">&mdash;</strong></span>
  <span>Heap: <strong id="heapLabel">&mdash;</strong></span>
</div>

<div class="tabs">
  <button class="tab active" onclick="showTab('general',event)">General</button>
  <button class="tab" onclick="showTab('pins',event)">Pins</button>
  <button class="tab" onclick="showTab('scan',event)">Scan Test</button>
  <button class="tab" onclick="showTab('wifi',event)">Settings</button>
  <button class="tab" onclick="showTab('monitor',event)">Monitor</button>
</div>

<!-- GENERAL TAB -->
<div class="panel active" id="panel-general">
  <div class="group">
    <div class="group-title">Features</div>
    <div class="row"><label>Bluetooth Classic</label><input type="checkbox" id="feat_bt"></div>
    <div class="row"><label>Bluetooth LE</label><input type="checkbox" id="feat_ble"></div>
    <div class="row"><label>WiFi config server</label><input type="checkbox" id="feat_wifi">
      <span class="hint">Disabling requires reflash to re-enable</span></div>
    <div class="row"><label>Use hardware jumper</label><input type="checkbox" id="use_mode_jumper">
      <span class="hint">Mode jumper input on GPIO below</span></div>
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
    <div class="group-title">Scan Address Inputs (from terminal via level shifter)</div>
    <p class="hint" style="margin-bottom:8px">7-bit address from the Wyse 50 keyboard connector J3. Active-high after TXS0108E level shifting (5V &rarr; 3.3V).</p>
    <div class="row"><label>Addr 0 (bit 0, LSB)</label><input type="number" id="pin_addr0" min="-1" max="39"></div>
    <div class="row"><label>Addr 1 (bit 1)</label><input type="number" id="pin_addr1" min="-1" max="39"></div>
    <div class="row"><label>Addr 2 (bit 2)</label><input type="number" id="pin_addr2" min="-1" max="39"></div>
    <div class="row"><label>Addr 3 (bit 3)</label><input type="number" id="pin_addr3" min="-1" max="39"></div>
    <div class="row"><label>Addr 4 (bit 4)</label><input type="number" id="pin_addr4" min="-1" max="39"></div>
    <div class="row"><label>Addr 5 (bit 5)</label><input type="number" id="pin_addr5" min="-1" max="39"></div>
    <div class="row"><label>Addr 6 (bit 6, MSB)</label><input type="number" id="pin_addr6" min="-1" max="39"></div>
  </div>
  <div class="group">
    <div class="group-title">Key Return Output</div>
    <div class="row"><label>Key Return</label><input type="number" id="pin_key_return" min="-1" max="39">
      <span class="hint">Active-high GPIO &rarr; 2N7000 inverts to active-low for terminal</span></div>
  </div>
  <div class="group">
    <div class="group-title">Control Inputs</div>
    <div class="row"><label>PAIR button</label><input type="number" id="pin_pair_btn" min="-1" max="39"></div>
    <div class="row"><label>MODE jumper</label><input type="number" id="pin_mode_jp" min="-1" max="39"></div>
  </div>
  <div class="group">
    <div class="group-title">Status LEDs (-1 = disabled)</div>
    <div class="row"><label>Activity LED</label><input type="number" id="pin_led" min="-1" max="39"></div>
    <div class="row"><label>Bluetooth LED</label><input type="number" id="pin_bt_led" min="-1" max="39"></div>
  </div>
  <div class="actions">
    <button class="btn-primary" onclick="saveAll()">&#x1F4BE; Save &amp; Apply</button>
  </div>
  <p class="hint" style="margin-top:8px">Pin changes require reboot. Avoid GPIOs 6-11 (internal flash on ESP32).</p>
</div>

<!-- SCAN TEST TAB -->
<div class="panel" id="panel-scan">
  <div class="group">
    <div class="group-title">Address Test</div>
    <p class="hint" style="margin-bottom:8px">Assert a single scan address to verify the terminal sees the key. Key Return goes active for the specified duration.</p>
    <div class="row">
      <label>Address (0-127)</label>
      <input type="number" id="scanTestAddr" min="0" max="127" value="12" style="width:70px" oninput="updateScanInfo()">
      <span class="hint mono" id="scanTestInfo">col=1 row=4</span>
    </div>
    <div class="row">
      <label>Duration (ms)</label>
      <input type="number" id="scanTestDuration" min="50" max="5000" value="200" style="width:80px">
    </div>
    <div class="actions" style="margin-top:8px">
      <button class="btn-primary btn-sm" onclick="scanTest()">Test Address</button>
    </div>
  </div>

  <div class="group" style="margin-top:12px">
    <div class="group-title">Address Sweep</div>
    <p class="hint" style="margin-bottom:8px">Sequentially assert a range of addresses. Watch the terminal to see which produce visible characters.</p>
    <div class="row">
      <label>Start address</label>
      <input type="number" id="sweepStart" min="0" max="127" value="0" style="width:70px">
    </div>
    <div class="row">
      <label>End address</label>
      <input type="number" id="sweepEnd" min="0" max="127" value="103" style="width:70px">
    </div>
    <div class="row">
      <label>Hold (ms)</label>
      <input type="number" id="sweepHold" min="50" max="5000" value="300" style="width:80px">
    </div>
    <div class="row">
      <label>Gap (ms)</label>
      <input type="number" id="sweepGap" min="50" max="5000" value="200" style="width:80px">
    </div>
    <div class="actions" style="margin-top:8px">
      <button class="btn-secondary btn-sm" onclick="scanSweep()">Run Sweep</button>
    </div>
  </div>

  <div class="group" style="margin-top:12px">
    <div class="group-title">Scan Snoop</div>
    <p class="hint" style="margin-bottom:8px">Monitor which addresses the terminal is scanning. Start snoop, wait a few seconds, then read the histogram to see active scan addresses.</p>
    <div class="actions">
      <button class="btn-secondary btn-sm" onclick="snoopStart()">Start Snoop</button>
      <button class="btn-secondary btn-sm" onclick="snoopRead()">Read Histogram</button>
    </div>
    <div id="histogramBox" style="display:none">Waiting...</div>
  </div>
</div>

<!-- WIFI / SETTINGS TAB -->
<div class="panel" id="panel-wifi">
  <div class="group">
    <div class="group-title">Device</div>
    <div class="row"><label>Device name</label><input type="text" id="device_name" maxlength="32">
      <span class="hint">Sets AP SSID + hostname</span></div>
  </div>
  <div class="group">
    <div class="group-title">Current Status</div>
    <div class="row"><label>WiFi mode</label><strong id="wifiModeLabel">--</strong></div>
    <div class="row"><label>IP address</label><strong id="wifiIpLabel">--</strong></div>
    <div class="row"><label>Hostname</label><strong id="wifiHostLabel">--</strong></div>
  </div>
  <div class="group">
    <div class="group-title">Station Mode</div>
    <p class="hint" style="margin-bottom:8px">Connect to an existing WiFi network. Leave SSID empty for AP-only mode.</p>
    <div class="row"><label>Network SSID</label><input type="text" id="sta_ssid" maxlength="32"></div>
    <div class="row"><label>Password</label><input type="text" id="sta_password" maxlength="64"></div>
  </div>
  <div class="group">
    <div class="group-title">Access Point</div>
    <p class="hint" style="margin-bottom:8px">WiFi password and channel for AP mode.</p>
    <div class="row"><label>AP password</label><input type="text" id="ap_password" maxlength="63">
      <span class="hint">Min 8 chars, or empty for open</span></div>
    <div class="row"><label>AP channel</label><input type="number" id="ap_channel" min="1" max="13"></div>
  </div>
  <div class="actions">
    <button class="btn-primary" onclick="saveAll()">&#x1F4BE; Save &amp; Apply</button>
  </div>
  <p class="hint" style="margin-top:8px">WiFi changes take effect after reboot.</p>
  <div class="group" style="margin-top:16px" id="passwordGroup">
    <div class="group-title" id="passGroupTitle">Password</div>
    <div class="row" id="curPassRow"><label>Current password</label><input type="password" id="cur_pass" maxlength="6" autocomplete="current-password" style="width:160px"></div>
    <div class="row"><label>New password</label><input type="password" id="new_pass" maxlength="6" autocomplete="new-password" style="width:160px">
      <span class="hint">4-6 characters, or empty to remove</span></div>
    <div class="actions" style="margin-top:8px">
      <button class="btn-secondary" id="passBtn" onclick="changePassword()">Set Password</button>
    </div>
  </div>
</div>

<!-- MONITOR TAB -->
<div class="panel" id="panel-monitor">
  <div class="group">
    <div class="group-title">Live Key Monitor</div>
    <p class="hint">Shows key events as they are processed (updates every second).</p>
    <div id="keyLog">Waiting for keypresses...</div>
    <div class="actions" style="margin-top:8px">
      <button class="btn-secondary btn-sm" onclick="clearLog()">Clear</button>
    </div>
  </div>
</div>
</div><!-- /mainUI -->

<div class="toast" id="toast"></div>

<script>
let cfg = {};
let logPoll = null;

function showTab(name, evt) {
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.getElementById('panel-'+name).classList.add('active');
  if (evt && evt.target) evt.target.classList.add('active');
  if (name === 'monitor' && !logPoll) startLogPoll();
  if (name !== 'monitor' && logPoll) { clearInterval(logPoll); logPoll = null; }
}

function toast(msg, ok) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show ' + (ok ? 'toast-ok' : 'toast-err');
  setTimeout(() => t.classList.remove('show'), 2500);
}

// --- Auth / Login ---
function showLogin() {
  document.getElementById('loginScreen').style.display = 'flex';
  document.getElementById('mainUI').style.display = 'none';
  if (logPoll) { clearInterval(logPoll); logPoll = null; }
  fetch('/api/status').then(r => r.json()).then(s => {
    document.getElementById('loginDeviceName').textContent = s.device_name || s.hostname || 'KeyBridge';
  }).catch(() => {});
  setTimeout(() => document.getElementById('loginPass').focus(), 100);
}

function showMain() {
  document.getElementById('loginScreen').style.display = 'none';
  document.getElementById('mainUI').style.display = 'block';
}

async function doLogin() {
  const pass = document.getElementById('loginPass').value;
  if (!pass) return;
  try {
    const r = await fetch('/api/login', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({password: pass})
    });
    const result = await r.json();
    if (result.ok) {
      document.getElementById('loginPass').value = '';
      showMain();
      loadConfig();
    } else {
      toast(result.error || 'Login failed', false);
      document.getElementById('loginPass').value = '';
      document.getElementById('loginPass').focus();
    }
  } catch(e) { toast('Connection error', false); }
}

document.getElementById('loginPass').addEventListener('keydown', e => {
  if (e.key === 'Enter') doLogin();
});

// --- Load config from device ---
async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    if (r.status === 401) { showLogin(); return; }
    if (!r.ok) throw new Error('HTTP ' + r.status);
    cfg = await r.json();
    populateForm();
    updateStatus();
  } catch(e) { toast('Failed to load config', false); }
}

function populateForm() {
  // General
  chk('use_mode_jumper', cfg.terminal?.use_mode_jumper);
  chk('feat_bt', cfg.features?.bt_classic);
  chk('feat_ble', cfg.features?.ble);
  chk('feat_wifi', cfg.features?.wifi);

  // Pins
  for (let i = 0; i < 7; i++) val('pin_addr'+i, cfg.pins?.['addr'+i]);
  val('pin_key_return', cfg.pins?.key_return);
  val('pin_pair_btn', cfg.pins?.pair_btn);
  val('pin_mode_jp', cfg.pins?.mode_jp);
  val('pin_led', cfg.pins?.led);
  val('pin_bt_led', cfg.pins?.bt_led);

  // WiFi / Settings
  val('device_name', cfg.wifi?.ap_ssid);
  val('sta_ssid', cfg.wifi?.sta_ssid);
  val('sta_password', cfg.wifi?.sta_password);
  val('ap_password', cfg.wifi?.ap_password);
  val('ap_channel', cfg.wifi?.ap_channel);
}

// --- Gather form data ---
function gatherConfig() {
  cfg.terminal = {
    use_mode_jumper: gchk('use_mode_jumper')
  };
  cfg.features = {
    bt_classic: gchk('feat_bt'),
    ble: gchk('feat_ble'), wifi: gchk('feat_wifi')
  };
  cfg.pins = {
    key_return: gnum('pin_key_return'),
    pair_btn: gnum('pin_pair_btn'), mode_jp: gnum('pin_mode_jp'),
    led: gnum('pin_led'), bt_led: gnum('pin_bt_led')
  };
  for (let i = 0; i < 7; i++) cfg.pins['addr'+i] = gnum('pin_addr'+i);

  const devName = gval('device_name');
  const hostName = devName.toLowerCase().replace(/[^a-z0-9-]/g, '-').replace(/-+/g, '-').replace(/^-|-$/g, '');
  cfg.wifi = {
    sta_ssid: gval('sta_ssid'), sta_password: gval('sta_password'),
    hostname: hostName || 'keybridge',
    ap_ssid: devName, ap_password: gval('ap_password'), ap_channel: gnum('ap_channel')
  };
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

async function triggerPair() {
  document.getElementById('btStatus').textContent = 'Scanning...';
  try {
    const r = await fetch('/api/bt/pair', {method:'POST'});
    const result = await r.json();
    document.getElementById('btStatus').textContent = result.message || 'Scan initiated';
  } catch(e) { document.getElementById('btStatus').textContent = 'Error'; }
}

// --- Scan test tools ---
function updateScanInfo() {
  const addr = parseInt(document.getElementById('scanTestAddr').value) || 0;
  document.getElementById('scanTestInfo').textContent =
    'col=' + ((addr >> 3) & 0x0F) + ' row=' + (addr & 0x07);
}

async function scanTest() {
  const addr = parseInt(document.getElementById('scanTestAddr').value);
  const dur = parseInt(document.getElementById('scanTestDuration').value) || 200;
  if (isNaN(addr) || addr < 0 || addr > 127) { toast('Address must be 0-127', false); return; }
  try {
    const r = await fetch('/api/scan/test', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({addr: addr, duration_ms: dur})
    });
    const result = await r.json();
    if (result.ok) toast('Addr 0x' + addr.toString(16).toUpperCase().padStart(2,'0') + ' asserted ' + dur + 'ms', true);
    else toast(result.error || 'Failed', false);
  } catch(e) { toast('Error: ' + e, false); }
}

async function scanSweep() {
  const start = parseInt(document.getElementById('sweepStart').value) || 0;
  const end = parseInt(document.getElementById('sweepEnd').value) || 127;
  const hold = parseInt(document.getElementById('sweepHold').value) || 300;
  const gap = parseInt(document.getElementById('sweepGap').value) || 200;
  try {
    const r = await fetch('/api/scan/sweep', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({start: start, end: end, hold_ms: hold, gap_ms: gap})
    });
    const result = await r.json();
    toast(result.message || 'Sweep started', true);
  } catch(e) { toast('Error: ' + e, false); }
}

async function snoopStart() {
  try {
    await fetch('/api/scan/snoop', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({enable: true})
    });
    toast('Snoop started — wait a few seconds, then Read Histogram', true);
  } catch(e) { toast('Error: ' + e, false); }
}

async function snoopRead() {
  try {
    const r = await fetch('/api/scan/histogram');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const data = await r.json();
    const box = document.getElementById('histogramBox');
    box.style.display = 'block';
    let lines = 'Total scans: ' + (data.total_scans||0) + '  Last addr: 0x'
              + ((data.last_addr||0).toString(16).toUpperCase().padStart(2,'0')) + '\n';
    lines += 'Addr  Col Row  Count\n';
    lines += '----  --- ---  -----\n';
    (data.addresses || []).forEach(a => {
      lines += '0x' + a.addr.toString(16).toUpperCase().padStart(2,'0')
            + '   ' + String(a.col).padStart(2) + '   ' + a.row
            + '    ' + a.count + '\n';
    });
    box.textContent = lines;
  } catch(e) { toast('Error: ' + e, false); }
}

// --- Password ---
async function changePassword() {
  const cur = document.getElementById('cur_pass').value;
  const nw = document.getElementById('new_pass').value;
  const isSet = document.getElementById('curPassRow').style.display !== 'none';
  if (isSet && !cur) { toast('Enter current password', false); return; }
  if (nw.length > 0 && (nw.length < 4 || nw.length > 6)) { toast('Password must be 4-6 characters', false); return; }
  if (!nw && !isSet) { toast('Enter a password', false); return; }
  try {
    const r = await fetch('/api/password', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({current: cur, new: nw})
    });
    const result = await r.json();
    if (result.ok) {
      toast(nw ? 'Password set' : 'Password removed', true);
      document.getElementById('cur_pass').value = '';
      document.getElementById('new_pass').value = '';
      updatePasswordUI(nw.length > 0);
    } else {
      toast(result.error || 'Failed', false);
    }
  } catch(e) { toast('Error: ' + e, false); }
}

// --- Status polling ---
async function updateStatus() {
  try {
    const r = await fetch('/api/status');
    if (!r.ok) return;
    const s = await r.json();
    dot('dotBt', s.bt_connected);
    dot('dotWifi', true);
    if (s.uptime_sec !== undefined) {
      const h = Math.floor(s.uptime_sec / 3600);
      const m = Math.floor((s.uptime_sec % 3600) / 60);
      document.getElementById('uptimeLabel').textContent = h + 'h ' + m + 'm';
    }
    if (s.free_heap !== undefined) {
      document.getElementById('heapLabel').textContent = Math.round(s.free_heap / 1024) + ' KB';
    }
    if (s.wifi_mode) document.getElementById('wifiModeLabel').textContent = s.wifi_mode;
    if (s.wifi_ip) document.getElementById('wifiIpLabel').textContent = s.wifi_ip;
    if (s.hostname) document.getElementById('wifiHostLabel').textContent = s.hostname + '.local';
  } catch(e) {}
}

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

function clearLog() { document.getElementById('keyLog').textContent = ''; }

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

function updatePasswordUI(authRequired) {
  const curRow = document.getElementById('curPassRow');
  const title = document.getElementById('passGroupTitle');
  const btn = document.getElementById('passBtn');
  if (authRequired) {
    curRow.style.display = 'flex';
    title.textContent = 'Change Password';
    btn.textContent = 'Change Password';
  } else {
    curRow.style.display = 'none';
    title.textContent = 'Set Password';
    btn.textContent = 'Set Password';
  }
}

// Init — check auth by trying to load config
async function init() {
  try {
    const r = await fetch('/api/config');
    if (r.status === 401) { showLogin(); return; }
    if (r.ok) {
      cfg = await r.json();
      showMain();
      populateForm();
      const sr = await fetch('/api/status');
      if (sr.ok) {
        const s = await sr.json();
        updatePasswordUI(s.auth_required);
      }
      updateStatus();
      setInterval(updateStatus, 5000);
    } else {
      showLogin();
    }
  } catch(e) { showLogin(); }
}
init();
</script>
</body>
</html>
)rawliteral";
// clang-format on

#endif // WEB_UI_H
