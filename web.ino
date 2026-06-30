extern const char MAIN_PAGE_HTML[] PROGMEM;
extern const char SCHEDULE_PAGE_HTML[] PROGMEM;

void setupRoutes() {
  // Main dashboard (you had a big dashboard: here we serve a simplified version + reuse)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send_P(200, "text/html", MAIN_PAGE_HTML);
  });

  // New schedule page
  server.on("/schedule", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send_P(200, "text/html", SCHEDULE_PAGE_HTML);
  });

  // API: get schedules JSON
  server.on("/api/schedules", HTTP_GET, [](AsyncWebServerRequest * request) {
    DynamicJsonDocument doc(16000);
    JsonArray arr = doc.to<JsonArray>();
    for (auto &s : schedules) {
      JsonObject o = arr.createNestedObject();
      o["deviceId"] = s.deviceId;
      o["hour"] = s.hour;
      o["minute"] = s.minute;
      o["type"] = s.type;
      o["data"] = s.data;
      o["brightness"] = s.brightness;
      o["enabled"] = s.enabled;
    }
    String out;
    serializeJson(arr, out);
    logMsg(out);
    request->send(200, "application/json", out);
  });

  // API: save schedules (expects full array, overwrite existing)
server.on("/api/schedules", HTTP_POST, [](AsyncWebServerRequest *request){},
    NULL, // no upload
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        
       // Append the current chunk to global buffer
    if(index == 0) scheduleBody = ""; // first chunk, reset
for (size_t i = 0; i < len; i++) scheduleBody += (char)data[i];

    // Only process when full body received
    if(index + len == total){
          logMsg(scheduleBody);

        DynamicJsonDocument doc(16000); // increase if needed
        DeserializationError err = deserializeJson(doc, scheduleBody);
        if(err){
            request->send(400, "text/plain", "Bad JSON: " + String(err.c_str()));
            return;
        }

        JsonArray arr = doc.as<JsonArray>();
        schedules.clear();
        for(JsonObject obj : arr){
            ScheduleEntry s;
            s.deviceId = obj["deviceId"] | 0;
            s.hour = obj["hour"] | 0;
            s.minute = obj["minute"] | 0;
            s.type = obj["type"] ? String((const char*)obj["type"]) : "on";
            s.data = obj["data"] ? String((const char*)obj["data"]) : "";
            s.brightness = obj["brightness"] | 255;
            s.enabled = obj["enabled"] | true;
            schedules.push_back(s);
        }
        saveSchedulesToFS();
        request->send(200, "text/plain", "saved");
    }
});



  // status endpoint (small)
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest * request) {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("devices");
    for (int i = 0; i < DEVICE_COUNT; i++) {
      JsonObject o = arr.createNestedObject();
      o["name"] = deviceNames[i];
      if (i < 4) o["state"] = map(deviceStates[i], 0, PWM_MAX, 0, 255);
      else if (i == 4) o["state"] = stepper.currentPosition();
      else if (i == 5 || i == 6) {
        uint8_t idx = i - 5;
        JsonObject neo = o.createNestedObject("state");
        char buf[8];
        snprintf(buf, sizeof(buf), "#%06X", (unsigned int)(lastNeoColor[idx] & 0xFFFFFF)); // last color
        neo["color"] = buf;
        neo["brightness"] = neoStrips[idx]->getBrightness();
      }
    }
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    char timestr[16];
    snprintf(timestr, sizeof(timestr), "%02d:%02d", t->tm_hour, t->tm_min);
    doc["deviceTime"] = timestr;
    doc["waterLevel"] = waterLevelHigh ? 1 : 0;
    if (!isnan(currentTempC)) doc["tempC"] = currentTempC; // omitted until first valid read
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // manual control - similar to original
  server.on("/control", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("id", true)) {
      request->send(400, "text/plain", "missing id");
      return;
    }
    int id = request->getParam("id", true)->value().toInt();
    if (id < 0 || id >= DEVICE_COUNT) {
      request->send(400, "text/plain", "bad id");
      return;
    }

    if (id == 5 || id == 6) { // NeoPixel strips
      uint8_t idx = id - 5;
      String colorStr = "#FF0000";
      uint8_t brightness = 255;
      if (request->hasParam("color", true)) colorStr = request->getParam("color", true)->value();
      if (request->hasParam("brightness", true)) brightness = request->getParam("brightness", true)->value().toInt();
      uint32_t color = strtoul(colorStr.substring(1).c_str(), nullptr, 16);
      applyNeoPixelColor(idx, color, brightness);
    } else {
      if (!request->hasParam("value", true)) {
        request->send(400, "text/plain", "missing value");
        return;
      }
      int val = request->getParam("value", true)->value().toInt();
      setDeviceLevel(id, val);
    }
    request->send(200, "text/plain", "ok");
  });

  // stepper control
  server.on("/stepper", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("dir", true)) {
      request->send(400, "text/plain", "missing");
      return;
    }
    String dir = request->getParam("dir", true)->value();
    int steps = 200;
    if (request->hasParam("steps", true)) steps = request->getParam("steps", true)->value().toInt();
    if (dir == "fwd" || dir == "forward") {
      stepperMove(steps);
    } else if (dir == "back" || dir == "backward") {
      stepperMove(-steps);
    } else if (dir == "stop") {
      stepper.stop();
    }
    request->send(200, "text/plain", "ok");
  });

  // thunder effect (kept from original)
  server.on("/thunder", HTTP_POST, [](AsyncWebServerRequest * request) {
    int flashes = 5;
    if (request->hasParam("times", true)) flashes = request->getParam("times", true)->value().toInt();
    for (int f = 0; f < flashes; f++) {
      uint8_t b = random(180, 255);
      neoPixel.setBrightness(b);
      for (int i = 0; i < NUMPIXELS; i++) neoPixel.setPixelColor(i, neoPixel.Color(255, 255, 255));
      neoPixel.show();
      delay(random(50, 150));
      neoPixel.clear();
      neoPixel.show();
      delay(random(100, 300));
    }
    request->send(200, "text/plain", "Thunderstorm triggered");
  });


  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP Logs</title>
</head>
<body>
  <h2>ESP WebSocket Logs</h2>
  <pre id="log"></pre>
  <script>
    var ws = new WebSocket('ws://' + location.hostname + '/ws');
    ws.onmessage = function(event) {
      var logElem = document.getElementById('log');
      logElem.textContent += event.data + '\n';
      logElem.scrollTop = logElem.scrollHeight; // auto-scroll
    };
  </script>
</body>
</html>
)rawliteral");
});
}

// ----------------------------- HTML pages -----------------------------
// Stored in PROGMEM and served via send_P. Building these as heap Strings
// returned empty bodies on ESP8266 once the heap got fragmented.
const char MAIN_PAGE_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0a84ff">
<title>SmartTank</title>
<style>
:root{
  --bg:#f0f4f8;--card:#fff;--text:#1a1a1a;--muted:#6b7280;
  --accent:#0a84ff;--accent-2:#00b4d8;--success:#22c55e;--danger:#ef4444;
  --border:#e5e7eb;--shadow:0 4px 14px rgba(0,0,0,.06);
}
@media (prefers-color-scheme:dark){
  :root{--bg:#0b1320;--card:#131c2e;--text:#e5e7eb;--muted:#9ca3af;
    --border:#1f2937;--shadow:0 4px 14px rgba(0,0,0,.4);}
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  margin:0;padding:0;background:var(--bg);color:var(--text);font-size:16px}
header{background:linear-gradient(135deg,var(--accent),var(--accent-2));
  color:#fff;padding:18px 16px;padding-top:calc(18px + env(safe-area-inset-top));
  position:sticky;top:0;z-index:10;box-shadow:0 2px 12px rgba(0,0,0,.15)}
h1{margin:0;font-size:1.25rem;text-align:center;font-weight:600}
.nav{display:flex;justify-content:center;gap:4px;margin-top:8px;font-size:.9rem}
.nav a{color:#fff;text-decoration:none;opacity:.9;padding:4px 10px;border-radius:6px}
.nav a.active{background:rgba(255,255,255,.2)}
main{max-width:720px;margin:0 auto;padding:16px}
.status-bar{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:8px;background:var(--card);
  border-radius:12px;padding:12px;margin-bottom:16px;box-shadow:var(--shadow)}
.status-item{display:flex;align-items:center;gap:8px;font-size:.9rem}
.status-item .label{color:var(--muted)}
.status-item .value{font-weight:600}
.dot{width:10px;height:10px;border-radius:50%;display:inline-block;background:var(--muted)}
.dot.high{background:var(--success);box-shadow:0 0 8px var(--success)}
.card{background:var(--card);border-radius:14px;padding:16px;margin-bottom:14px;
  box-shadow:var(--shadow)}
.card-head{display:flex;justify-content:space-between;align-items:center;
  margin-bottom:12px;gap:8px;flex-wrap:wrap}
.card-title{font-size:1.05rem;font-weight:600;display:flex;align-items:center;
  gap:6px;margin:0}
.btn-group{display:flex;gap:6px}
.btn{background:var(--accent);color:#fff;border:none;border-radius:8px;
  padding:8px 14px;font-size:.9rem;cursor:pointer;font-weight:500;
  min-height:38px;transition:transform .08s}
.btn:active{transform:scale(.95)}
.btn.off{background:var(--muted)}
.slider-row{display:flex;align-items:center;gap:10px;margin-top:10px}
input[type=range]{-webkit-appearance:none;appearance:none;flex:1;height:6px;
  background:var(--border);border-radius:3px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;
  width:22px;height:22px;border-radius:50%;background:var(--accent);
  cursor:pointer;border:3px solid var(--card);box-shadow:0 1px 3px rgba(0,0,0,.2)}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;
  background:var(--accent);cursor:pointer;border:3px solid var(--card)}
.value-input{width:72px;padding:6px 8px;border:1px solid var(--border);
  border-radius:8px;background:var(--bg);color:var(--text);font-size:.9rem;
  text-align:center}
.color-row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
input[type=color]{width:52px;height:48px;padding:0;border:2px solid var(--border);
  border-radius:12px;background:transparent;cursor:pointer}
.preview-chip{width:40px;height:40px;border-radius:50%;border:2px solid var(--border);
  display:inline-block;transition:.25s}
.toast{position:fixed;bottom:20px;left:50%;
  transform:translateX(-50%) translateY(80px);background:var(--card);
  color:var(--text);padding:10px 18px;border-radius:24px;
  box-shadow:0 8px 24px rgba(0,0,0,.2);opacity:0;transition:.25s;
  pointer-events:none;z-index:100;font-size:.9rem;border:1px solid var(--border)}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
.toast.error{color:var(--danger)}
footer{text-align:center;color:var(--muted);font-size:.8rem;padding:20px}
</style>
</head>
<body>
<header>
  <h1>🐠 Smart Fish Tank</h1>
  <div class="nav">
    <a href="/" class="active">🏠 Home</a>
    <a href="/schedule">🕒 Schedules</a>
    <a href="/logs">📜 Logs</a>
  </div>
</header>
<main>
  <div class="status-bar">
    <div class="status-item">
      <span class="label">🕓 Time</span>
      <span class="value" id="deviceTime">--</span>
    </div>
    <div class="status-item">
      <span class="label">💧 Water</span>
      <span class="dot" id="waterDot"></span>
      <span class="value" id="waterLevel">--</span>
    </div>
    <div class="status-item">
      <span class="label">🌡️ Temp</span>
      <span class="value" id="waterTemp">--</span>
    </div>
  </div>
  <div id="devices"></div>
  <div id="neoStrips"></div>
  <div class="card">
    <div class="card-head">
      <h3 class="card-title">🍽️ Auto Feeder</h3>
      <div class="btn-group">
        <button class="btn" onclick="step('back',STEPS_PER_REV)">Feed</button>
        <button class="btn off" onclick="step('stop',0)">Stop</button>
      </div>
    </div>
    <div style="color:var(--muted);font-size:.85rem">Position: <span id="stepPos">0</span></div>
  </div>
</main>
<div class="toast" id="toast"></div>
<footer>SmartTank © 2025</footer>
<script>
const VMAX = 255;
// Steps for one full Feed turn. 28BYJ-48 full-step ≈ 2048/rev.
// If one Feed isn't a full 360°, adjust this single number.
const STEPS_PER_REV = 2048;
const DEVICES = [
  {id:0, name:'💦 Water Pump'},
  {id:1, name:'🌬️ Air Pump'},
  {id:2, name:'💡 LED Light'},
  {id:3, name:'🔆 UV Light'}
];
const container = document.getElementById('devices');
DEVICES.forEach(d => {
  const card = document.createElement('div');
  card.className = 'card';
  card.innerHTML =
    '<div class="card-head">' +
      '<h3 class="card-title">' + d.name + '</h3>' +
      '<div class="btn-group">' +
        '<button class="btn" onclick="setVal(' + d.id + ',' + VMAX + ')">ON</button>' +
        '<button class="btn off" onclick="setVal(' + d.id + ',0)">OFF</button>' +
      '</div>' +
    '</div>' +
    '<div class="slider-row">' +
      '<input type="range" min="0" max="' + VMAX + '" id="slider' + d.id + '" value="0">' +
      '<input type="number" min="0" max="' + VMAX + '" id="val' + d.id + '" value="0" class="value-input">' +
    '</div>';
  container.appendChild(card);
  const slider = document.getElementById('slider' + d.id);
  const input = document.getElementById('val' + d.id);
  let pending = null;
  function sendDebounced(val){
    clearTimeout(pending);
    pending = setTimeout(() => setVal(d.id, val, false), 80);
  }
  slider.addEventListener('input', e => {
    input.value = e.target.value;
    sendDebounced(e.target.value);
  });
  input.addEventListener('change', e => {
    let v = Math.max(0, Math.min(VMAX, parseInt(e.target.value) || 0));
    e.target.value = v; slider.value = v;
    setVal(d.id, v, false);
  });
});
const NEO = [
  {id:5, name:'🌈 NeoPixel 1'},
  {id:6, name:'🌈 NeoPixel 2'}
];
const neoContainer = document.getElementById('neoStrips');
NEO.forEach(n => {
  const card = document.createElement('div');
  card.className = 'card';
  card.innerHTML =
    '<div class="card-head">' +
      '<h3 class="card-title">' + n.name + '</h3>' +
      '<button class="btn" onclick="setNeo(' + n.id + ')">Apply</button>' +
    '</div>' +
    '<div class="color-row">' +
      '<input type="color" id="neoColor' + n.id + '" value="#FF0000">' +
      '<span class="preview-chip" id="preview' + n.id + '" style="background:#FF0000;box-shadow:0 0 14px #FF0000"></span>' +
      '<span style="color:var(--muted);font-size:.85rem;margin-left:auto">Brightness</span>' +
    '</div>' +
    '<div class="slider-row">' +
      '<input type="range" min="0" max="255" value="255" id="neoBrightness' + n.id + '">' +
      '<input type="number" min="0" max="255" value="255" id="neoBrightnessInput' + n.id + '" class="value-input">' +
    '</div>';
  neoContainer.appendChild(card);
  const nb = document.getElementById('neoBrightness' + n.id);
  const nbi = document.getElementById('neoBrightnessInput' + n.id);
  nb.addEventListener('input', e => nbi.value = e.target.value);
  nbi.addEventListener('change', e => {
    let v = Math.max(0, Math.min(255, parseInt(e.target.value) || 0));
    e.target.value = v; nb.value = v;
  });
  document.getElementById('neoColor' + n.id).addEventListener('input', e => {
    const p = document.getElementById('preview' + n.id);
    p.style.background = e.target.value;
    p.style.boxShadow = '0 0 14px ' + e.target.value;
  });
});
function toast(msg, isError){
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.toggle('error', !!isError);
  t.classList.add('show');
  clearTimeout(t._h);
  t._h = setTimeout(() => t.classList.remove('show'), 1800);
}
async function setVal(id, val, notify){
  const valEl = document.getElementById('val' + id);
  const slEl = document.getElementById('slider' + id);
  if (valEl) valEl.value = val;
  if (slEl) slEl.value = val;
  try{
    await fetch('/control', {method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'id=' + id + '&value=' + val});
    if (notify !== false) toast('Updated');
  }catch(e){ toast('Network error', true); }
}
async function setNeo(id){
  const color = document.getElementById('neoColor' + id).value;
  const br = document.getElementById('neoBrightness' + id).value;
  try{
    await fetch('/control', {method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'id=' + id + '&color=' + encodeURIComponent(color) + '&brightness=' + br});
    toast('NeoPixel updated');
  }catch(e){ toast('Network error', true); }
}
function step(dir, steps){
  fetch('/stepper', {method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'dir=' + dir + '&steps=' + steps})
   .then(() => toast(dir === 'stop' ? 'Stopped' : 'Feeding…'))
   .catch(() => toast('Network error', true));
}
async function loadStatus(){
  try{
    const res = await fetch('/status');
    const data = await res.json();
    data.devices.forEach((dev, i) => {
      if (i <= 3){
        const v = document.getElementById('val' + i);
        const s = document.getElementById('slider' + i);
        if (v && document.activeElement !== v) v.value = dev.state;
        if (s) s.value = dev.state;
      } else if (i === 4){
        document.getElementById('stepPos').innerText = dev.state;
      } else if (i === 5 || i === 6){
        const color = (dev.state && dev.state.color) || '#FF0000';
        const br = (dev.state && dev.state.brightness) || 255;
        const cEl = document.getElementById('neoColor' + i);
        const bEl = document.getElementById('neoBrightness' + i);
        const biEl = document.getElementById('neoBrightnessInput' + i);
        if (cEl) cEl.value = color;
        if (bEl) bEl.value = br;
        if (biEl) biEl.value = br;
        const p = document.getElementById('preview' + i);
        if (p){ p.style.background = color; p.style.boxShadow = '0 0 14px ' + color; }
      }
    });
    if (data.deviceTime) document.getElementById('deviceTime').innerText = data.deviceTime;
    if (data.waterLevel !== undefined){
      const high = !!data.waterLevel;
      document.getElementById('waterLevel').innerText = high ? 'HIGH' : 'LOW';
      document.getElementById('waterDot').className = 'dot ' + (high ? 'high' : '');
    }
    if (data.tempC !== undefined && data.tempC !== null){
      document.getElementById('waterTemp').innerText = data.tempC.toFixed(1) + '°C';
    }
  }catch(e){}
}
window.onload = loadStatus;
setInterval(loadStatus, 10000);
</script>
</body>
</html>
)rawliteral";

// The schedule page: single UI to view/add/edit/delete schedules
const char SCHEDULE_PAGE_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0a84ff">
<title>Schedules</title>
<style>
:root{
  --bg:#f0f4f8;--card:#fff;--text:#1a1a1a;--muted:#6b7280;
  --accent:#0a84ff;--accent-2:#00b4d8;--success:#22c55e;--danger:#ef4444;
  --border:#e5e7eb;--shadow:0 4px 14px rgba(0,0,0,.06);
}
@media (prefers-color-scheme:dark){
  :root{--bg:#0b1320;--card:#131c2e;--text:#e5e7eb;--muted:#9ca3af;
    --border:#1f2937;--shadow:0 4px 14px rgba(0,0,0,.4);}
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  margin:0;padding:0 0 100px;background:var(--bg);color:var(--text);font-size:16px}
header{background:linear-gradient(135deg,var(--accent),var(--accent-2));
  color:#fff;padding:18px 16px;padding-top:calc(18px + env(safe-area-inset-top));
  position:sticky;top:0;z-index:10;box-shadow:0 2px 12px rgba(0,0,0,.15)}
h1{margin:0;font-size:1.25rem;text-align:center;font-weight:600}
.nav{display:flex;justify-content:center;gap:4px;margin-top:8px;font-size:.9rem}
.nav a{color:#fff;text-decoration:none;opacity:.9;padding:4px 10px;border-radius:6px}
.nav a.active{background:rgba(255,255,255,.2)}
main{max-width:720px;margin:0 auto;padding:16px}
.schedule-card{background:var(--card);border-radius:14px;padding:14px;
  margin-bottom:12px;box-shadow:var(--shadow);transition:opacity .2s}
.schedule-card.disabled{opacity:.55}
.row{display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap}
.row.hidden{display:none}
.row > label{font-size:.85rem;color:var(--muted);min-width:70px}
.field{flex:1;min-width:120px}
select, input[type=text], input[type=number]{
  width:100%;padding:8px 10px;border:1px solid var(--border);border-radius:8px;
  background:var(--bg);color:var(--text);font-size:.95rem;
}
input[type=color]{width:50px;height:40px;padding:0;border:1px solid var(--border);
  border-radius:8px;cursor:pointer;background:transparent}
input[type=range]{-webkit-appearance:none;appearance:none;flex:1;height:6px;
  background:var(--border);border-radius:3px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;
  width:22px;height:22px;border-radius:50%;background:var(--accent);
  cursor:pointer;border:3px solid var(--card)}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;
  background:var(--accent);cursor:pointer;border:3px solid var(--card)}
.time-row{display:flex;gap:6px;align-items:center;margin-left:auto}
.time-row input{width:60px;text-align:center}
.btn{background:var(--accent);color:#fff;border:none;border-radius:8px;
  padding:10px 16px;font-size:.9rem;cursor:pointer;font-weight:500;
  min-height:40px;transition:transform .08s}
.btn:active{transform:scale(.95)}
.btn.danger{background:var(--danger)}
.btn.ghost{background:transparent;color:var(--accent);border:1px solid var(--accent)}
.toggle{width:46px;height:26px;border-radius:13px;background:var(--border);
  position:relative;cursor:pointer;transition:.2s;flex-shrink:0}
.toggle.on{background:var(--success)}
.toggle::after{content:'';position:absolute;top:3px;left:3px;width:20px;height:20px;
  border-radius:50%;background:#fff;transition:.2s;box-shadow:0 1px 3px rgba(0,0,0,.3)}
.toggle.on::after{transform:translateX(20px)}
.color-preview{width:30px;height:30px;border-radius:6px;border:1px solid var(--border);
  display:inline-block;vertical-align:middle}
.card-actions{display:flex;justify-content:flex-end;margin-top:10px}
.value-label{color:var(--muted);font-size:.9rem;min-width:48px;text-align:right;
  font-variant-numeric:tabular-nums}
.empty{text-align:center;color:var(--muted);padding:40px 20px}
.sticky-bottom{position:fixed;bottom:0;left:0;right:0;background:var(--card);
  border-top:1px solid var(--border);padding:12px 16px;display:flex;gap:8px;
  box-shadow:0 -4px 14px rgba(0,0,0,.08);
  padding-bottom:calc(12px + env(safe-area-inset-bottom));z-index:5}
.sticky-bottom .btn{flex:1}
.toast{position:fixed;bottom:100px;left:50%;
  transform:translateX(-50%) translateY(80px);background:var(--card);
  color:var(--text);padding:10px 18px;border-radius:24px;
  box-shadow:0 8px 24px rgba(0,0,0,.2);opacity:0;transition:.25s;
  pointer-events:none;z-index:100;font-size:.9rem;border:1px solid var(--border)}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
.toast.error{color:var(--danger)}
.hidden{display:none !important}
</style>
</head>
<body>
<header>
  <h1>🕒 Schedules</h1>
  <div class="nav">
    <a href="/">🏠 Home</a>
    <a href="/schedule" class="active">🕒 Schedules</a>
    <a href="/logs">📜 Logs</a>
  </div>
</header>
<main>
  <div id="list"></div>
  <div class="empty" id="empty">No schedules yet. Tap ➕ Add to create one.</div>
</main>
<div class="sticky-bottom">
  <button class="btn ghost" onclick="addEmpty()">➕ Add</button>
  <button class="btn" onclick="saveAll()">💾 Save All</button>
</div>
<div class="toast" id="toast"></div>
<script>
const VMAX = 255;
const DEV_NAMES = ["💦 Water pump","🌬️ Air pump","💡 LED","🔆 UV","🍽️ Auto feeder","🌈 Neo Pixel","🌈 Neo Pixel 2"];
let schedules = [];

function toast(msg, isError){
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.toggle('error', !!isError);
  t.classList.add('show');
  clearTimeout(t._h);
  t._h = setTimeout(() => t.classList.remove('show'), 1800);
}
function pad2(n){return String(n).padStart(2,'0')}

async function load(){
  try{
    const res = await fetch('/api/schedules');
    schedules = await res.json();
  }catch(e){
    schedules = [];
    toast('Failed to load', true);
  }
  render();
}

function addEmpty(){
  schedules.push({deviceId:0, hour:8, minute:0, type:'on', data:'', brightness:255, enabled:true});
  render();
  setTimeout(() => window.scrollTo(0, document.body.scrollHeight), 50);
}

function delItem(i){
  schedules.splice(i, 1);
  render();
}

function render(){
  const list = document.getElementById('list');
  list.innerHTML = '';
  document.getElementById('empty').classList.toggle('hidden', schedules.length > 0);

  schedules.forEach((s, i) => {
    const card = document.createElement('div');
    card.className = 'schedule-card' + (s.enabled ? '' : ' disabled');

    const devOptions = DEV_NAMES.map((n, idx) =>
      '<option value="' + idx + '"' + (s.deviceId == idx ? ' selected' : '') + '>' + n + '</option>').join('');

    const typeOptions = ['on','off','value','color','stepper'].map(t =>
      '<option value="' + t + '"' + (s.type === t ? ' selected' : '') + '>' + t.toUpperCase() + '</option>').join('');

    card.innerHTML =
      '<div class="row">' +
        '<div class="toggle ' + (s.enabled ? 'on' : '') + '" data-i="' + i + '" data-act="toggle"></div>' +
        '<span style="color:var(--muted);font-size:.85rem">' + (s.enabled ? 'Enabled' : 'Disabled') + '</span>' +
        '<div class="time-row">' +
          '<input type="number" min="0" max="23" value="' + s.hour + '" data-i="' + i + '" data-field="hour">' +
          '<span>:</span>' +
          '<input type="number" min="0" max="59" value="' + pad2(s.minute) + '" data-i="' + i + '" data-field="minute">' +
        '</div>' +
      '</div>' +
      '<div class="row">' +
        '<label>Device</label>' +
        '<div class="field"><select data-i="' + i + '" data-field="deviceId">' + devOptions + '</select></div>' +
      '</div>' +
      '<div class="row">' +
        '<label>Action</label>' +
        '<div class="field"><select data-i="' + i + '" data-field="type">' + typeOptions + '</select></div>' +
      '</div>' +
      '<div class="row" id="data-row-' + i + '"></div>' +
      '<div class="row" id="br-row-' + i + '"></div>' +
      '<div class="card-actions">' +
        '<button class="btn danger" data-i="' + i + '" data-act="del">🗑 Delete</button>' +
      '</div>';
    list.appendChild(card);
    renderDataRow(i);
    renderBrightnessRow(i);
  });

  list.querySelectorAll('[data-field]').forEach(el => {
    el.addEventListener('change', e => {
      const i = +el.dataset.i;
      const field = el.dataset.field;
      let v = el.value;
      if (field === 'hour' || field === 'minute' || field === 'deviceId') v = parseInt(v) || 0;
      schedules[i][field] = v;
      if (field === 'type'){
        schedules[i].data = '';
        renderDataRow(i);
        renderBrightnessRow(i);
      }
    });
  });
  list.querySelectorAll('[data-act="toggle"]').forEach(el => {
    el.addEventListener('click', () => {
      const i = +el.dataset.i;
      schedules[i].enabled = !schedules[i].enabled;
      render();
    });
  });
  list.querySelectorAll('[data-act="del"]').forEach(el => {
    el.addEventListener('click', () => delItem(+el.dataset.i));
  });
}

function renderDataRow(i){
  const s = schedules[i];
  const row = document.getElementById('data-row-' + i);
  if (s.type === 'on' || s.type === 'off'){
    row.innerHTML = '';
    row.classList.add('hidden');
    return;
  }
  row.classList.remove('hidden');
  if (s.type === 'value'){
    const v = Math.max(0, Math.min(VMAX, parseInt(s.data) || 0));
    row.innerHTML =
      '<label>Value</label>' +
      '<input type="range" min="0" max="' + VMAX + '" value="' + v + '">' +
      '<span class="value-label" id="vlabel-' + i + '">' + v + '</span>';
    const slider = row.querySelector('input[type=range]');
    const label = row.querySelector('#vlabel-' + i);
    slider.addEventListener('input', e => {
      label.textContent = e.target.value;
      schedules[i].data = e.target.value;
    });
    schedules[i].data = String(v);
  } else if (s.type === 'color'){
    const c = (s.data && s.data.length) ? (s.data.startsWith('#') ? s.data : '#' + s.data) : '#FF0000';
    row.innerHTML =
      '<label>Color</label>' +
      '<input type="color" value="' + c + '">' +
      '<span class="color-preview" style="background:' + c + '"></span>';
    const inp = row.querySelector('input[type=color]');
    const preview = row.querySelector('.color-preview');
    inp.addEventListener('input', e => {
      schedules[i].data = e.target.value;
      preview.style.background = e.target.value;
    });
    schedules[i].data = c;
  } else if (s.type === 'stepper'){
    const steps = s.data || '2048';
    row.innerHTML =
      '<label>Steps</label>' +
      '<div class="field"><input type="number" value="' + steps + '"></div>';
    row.querySelector('input').addEventListener('input', e => {
      schedules[i].data = e.target.value;
    });
    schedules[i].data = steps;
  }
}

function renderBrightnessRow(i){
  const s = schedules[i];
  const row = document.getElementById('br-row-' + i);
  if (s.type !== 'color'){
    row.innerHTML = '';
    row.classList.add('hidden');
    return;
  }
  row.classList.remove('hidden');
  const br = (s.brightness !== undefined) ? s.brightness : 255;
  row.innerHTML =
    '<label>Brightness</label>' +
    '<input type="range" min="0" max="255" value="' + br + '">' +
    '<span class="value-label" id="brlabel-' + i + '">' + br + '</span>';
  const slider = row.querySelector('input[type=range]');
  const label = row.querySelector('#brlabel-' + i);
  slider.addEventListener('input', e => {
    label.textContent = e.target.value;
    schedules[i].brightness = parseInt(e.target.value);
  });
}

async function saveAll(){
  const out = schedules.map(s => {
    let data = s.data || '';
    if (s.type === 'color' && data.startsWith('#')) data = data.substring(1);
    return {
      deviceId: parseInt(s.deviceId) || 0,
      hour: parseInt(s.hour) || 0,
      minute: parseInt(s.minute) || 0,
      type: s.type,
      data: String(data),
      brightness: parseInt(s.brightness) || 255,
      enabled: !!s.enabled
    };
  });
  try{
    const res = await fetch('/api/schedules', {method:'POST',
      headers:{'Content-Type':'application/json'}, body: JSON.stringify(out)});
    if (res.ok) toast('Saved ✓');
    else toast('Save failed', true);
  }catch(e){ toast('Network error', true); }
}

window.onload = load;
</script>
</body>
</html>
)rawliteral";
