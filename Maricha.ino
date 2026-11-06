/*
 * ============================================================
 *  MARICHA-ESP32  v1.0
 *  BLE Multi-Device Emulation + HID Keystroke Injection
 *  Port of techchipnet/maricha (original: ESP32-C5/S3 binaries)
 *  Target: ESP32 / ESP32-WROOM / ESP32-S3 (Arduino-ESP32 2.0.x)
 *  Powered by TechChip  |  Ported for Arduino IDE
 * ============================================================
 *  Board: "ESP32 Dev Module"  |  Partition: Default 4MB
 *  Tools > BLE: Bluedroid (default)
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEHIDDevice.h>

// ---------------- Settings ----------------
Preferences prefs;
String devName       = "Maricha-Keyboard";
uint16_t devAppear   = 0x03C1;
String apSSID        = "Maricha";
String apPass        = "12345678";
uint32_t discTimeout = 30;      // seconds
bool swiftPairMode   = true;

// ---------------- BLE objects ----------------
BLEServer         *pServer        = nullptr;
BLEHIDDevice      *hid            = nullptr;
BLECharacteristic *inputKeyboard  = nullptr;
BLECharacteristic *inputMouse     = nullptr;
BLECharacteristic *outputKeyboard = nullptr;
BLEAdvertising    *pAdv           = nullptr;

bool deviceConnected = false;
bool advRunning      = false;
uint32_t advStartMs  = 0;

uint8_t keyboardReport[8] = {0};
uint8_t mouseReport[4]    = {0};
uint8_t swiftHash[4]      = {0};

// ---------------- Profiles ----------------
struct Profile { const char *label; uint16_t appearance; };
const Profile profiles[] = {
  {"Wireless Keyboard",   0x03C1},
  {"Wireless Mouse",      0x03C2},
  {"Joystick",            0x03C3},
  {"Gamepad",             0x03C4},
  {"Digital Pen / Stylus",0x03C9},
  {"Heart Rate Sensor",   0x0341},
  {"Android TV",          0x0C03},
  {"Apple TV",            0x0C02},
  {"AirTag Tracker",      0x0C00},
  {"JioTag Tracker",      0x0C01},
};
const int profileCount = sizeof(profiles) / sizeof(profiles[0]);

// ---------------- Payload engine ----------------
String   payloadScript;
uint32_t payloadPos     = 0;
bool     payloadRunning = false;
uint32_t lastAction     = 0;
uint32_t actionDelay    = 0;

WebServer server(80);

// ---------------- HID report map ----------------
static const uint8_t reportMap[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
  0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
  0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05,
  0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
  0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
  0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
  0xC0,
  0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
  0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
  0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
  0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
  0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
  0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02,
  0x81, 0x06, 0xC0, 0xC0
};

// ---------------- Helpers ----------------
bool hidReady() { return deviceConnected && inputKeyboard; }

void tapKey(uint8_t mod, uint8_t key) {
  if (!hidReady()) return;
  keyboardReport[0] = mod;
  keyboardReport[2] = key;
  inputKeyboard->setValue(keyboardReport, 8);
  inputKeyboard->notify();
  delay(12);
  keyboardReport[0] = 0;
  keyboardReport[2] = 0;
  inputKeyboard->setValue(keyboardReport, 8);
  inputKeyboard->notify();
  delay(10);
}

void releaseAll() {
  if (!inputKeyboard) return;
  memset(keyboardReport, 0, sizeof(keyboardReport));
  inputKeyboard->setValue(keyboardReport, 8);
  if (deviceConnected) inputKeyboard->notify();
}

bool charToKey(char c, uint8_t &mod, uint8_t &key) {
  mod = 0; key = 0;
  if (c >= 'a' && c <= 'z') { key = 0x04 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { mod = 0x02; key = 0x04 + (c - 'A'); return true; }
  if (c >= '1' && c <= '9') { key = 0x1E + (c - '1'); return true; }
  if (c == '0') { key = 0x27; return true; }
  switch (c) {
    case '!': mod = 0x02; key = 0x1E; return true;
    case '@': mod = 0x02; key = 0x1F; return true;
    case '#': mod = 0x02; key = 0x20; return true;
    case '$': mod = 0x02; key = 0x21; return true;
    case '%': mod = 0x02; key = 0x22; return true;
    case '^': mod = 0x02; key = 0x23; return true;
    case '&': mod = 0x02; key = 0x24; return true;
    case '*': mod = 0x02; key = 0x25; return true;
    case '(': mod = 0x02; key = 0x26; return true;
    case ')': mod = 0x02; key = 0x27; return true;
    case ' ': key = 0x2C; return true;
    case '-': key = 0x2D; return true;
    case '_': mod = 0x02; key = 0x2D; return true;
    case '=': key = 0x2E; return true;
    case '+': mod = 0x02; key = 0x2E; return true;
    case '[': key = 0x2F; return true;
    case '{': mod = 0x02; key = 0x2F; return true;
    case ']': key = 0x30; return true;
    case '}': mod = 0x02; key = 0x30; return true;
    case '\\': key = 0x31; return true;
    case '|': mod = 0x02; key = 0x31; return true;
    case ';': key = 0x33; return true;
    case ':': mod = 0x02; key = 0x33; return true;
    case '\'': key = 0x34; return true;
    case '"': mod = 0x02; key = 0x34; return true;
    case '`': key = 0x35; return true;
    case '~': mod = 0x02; key = 0x35; return true;
    case ',': key = 0x36; return true;
    case '<': mod = 0x02; key = 0x36; return true;
    case '.': key = 0x37; return true;
    case '>': mod = 0x02; key = 0x37; return true;
    case '/': key = 0x38; return true;
    case '?': mod = 0x02; key = 0x38; return true;
    case '\n': key = 0x28; return true;
    case '\t': key = 0x2B; return true;
  }
  return false;
}

void typeString(const String &s) {
  for (uint16_t i = 0; i < s.length(); i++) {
    if (!payloadRunning) return;
    uint8_t m, k;
    if (charToKey(s[i], m, k)) tapKey(m, k);
  }
}

// ---------------- Settings storage ----------------
void loadSettings() {
  prefs.begin("maricha", false);
  devName     = prefs.getString("name", devName);
  devAppear   = prefs.getUShort("appear", devAppear);
  apSSID      = prefs.getString("ssid", apSSID);
  apPass      = prefs.getString("pass", apPass);
  discTimeout = prefs.getUInt("dur", discTimeout);
  swiftPairMode = prefs.getBool("swift", swiftPairMode);
  prefs.end();
}

void saveSettings() {
  prefs.begin("maricha", false);
  prefs.putString("name", devName);
  prefs.putUShort("appear", devAppear);
  prefs.putString("ssid", apSSID);
  prefs.putString("pass", apPass);
  prefs.putUInt("dur", discTimeout);
  prefs.putBool("swift", swiftPairMode);
  prefs.end();
}

// ---------------- BLE callbacks ----------------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    deviceConnected = true;
    advRunning = false;
    s->updateConnParams(12, 24, 0, 400);
    BLEDevice::startSecurity(s->getConnId());
    Serial.println("[BLE] Host connected -> HID ready");
  }
  void onDisconnect(BLEServer *s) override {
    deviceConnected = false;
    payloadRunning = false;
    releaseAll();
    Serial.println("[BLE] Disconnected");
    startAdvertising();
  }
};

class OutputCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override { /* LED state - ignored */ }
};

// ---------------- Advertising ----------------
void startAdvertising() {
  if (deviceConnected) return;

  BLEAdvertisementData adv;
  BLEAdvertisementData scan;

  String shortName = devName;
  if (shortName.length() > 20) shortName = shortName.substring(0, 20);

  adv.setFlags(0x06);
  adv.addServiceUUID(BLEUUID((uint16_t)0x1812));
  adv.setAppearance(devAppear);

  if (swiftPairMode) {
    String mfg;
    mfg += (char)0x06; mfg += (char)0x00;   // Microsoft
    mfg += (char)0x03; mfg += (char)0x03;   // Swift Pair
    for (int i = 0; i < 4; i++) mfg += (char)swiftHash[i];
    mfg += (char)0x01; mfg += (char)0x01;
    adv.setManufacturerData(mfg);
  }

  scan.setName(shortName);
  scan.setAppearance(devAppear);

  pAdv->setAdvertisementData(adv);
  pAdv->setScanResponseData(scan);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  pAdv->start();

  advRunning = true;
  advStartMs = millis();
  Serial.printf("[BLE] Advertising as \"%s\" (appearance 0x%04X)\n",
                devName.c_str(), devAppear);
}

void stopAdvertising() {
  pAdv->stop();
  advRunning = false;
  Serial.println("[BLE] Advertising stopped");
}

// ---------------- Payload execution ----------------
void executeLine(String line) {
  actionDelay = 0;
  line.trim();
  if (line.length() == 0) return;
  if (line.startsWith("REM")) return;

  String cmd = line;
  String arg = "";
  int sp = line.indexOf(' ');
  if (sp > 0) { cmd = line.substring(0, sp); arg = line.substring(sp + 1); }
  cmd.toUpperCase();

  uint8_t mod = 0;

  if (cmd == "STRING") { typeString(arg); actionDelay = 40; return; }
  if (cmd == "DELAY") { actionDelay = arg.toInt(); return; }
  if (cmd == "ENTER")     { tapKey(0, 0x28); return; }
  if (cmd == "TAB")       { tapKey(0, 0x2B); return; }
  if (cmd == "ESC")       { tapKey(0, 0x29); return; }
  if (cmd == "BACKSPACE") { tapKey(0, 0x2A); return; }
  if (cmd == "DELETE")    { tapKey(0, 0x4C); return; }
  if (cmd == "HOME")      { tapKey(0, 0x4A); return; }
  if (cmd == "END")       { tapKey(0, 0x4D); return; }
  if (cmd == "LEFT")      { tapKey(0, 0x50); return; }
  if (cmd == "RIGHT")     { tapKey(0, 0x4F); return; }
  if (cmd == "UP")        { tapKey(0, 0x52); return; }
  if (cmd == "DOWN")      { tapKey(0, 0x51); return; }
  if (cmd == "PAGEUP")    { tapKey(0, 0x4B); return; }
  if (cmd == "PAGEDOWN")  { tapKey(0, 0x4E); return; }

  // Modifier combinations: CTRL / ALT / SHIFT / GUI + key
  String rest = line;
  while (true) {
    int s2 = rest.indexOf(' ');
    String tok = (s2 < 0) ? rest : rest.substring(0, s2);
    String up = tok; up.toUpperCase();
    uint8_t bit = 0;
    if (up == "CTRL" || up == "CONTROL") bit = 0x01;
    else if (up == "SHIFT") bit = 0x02;
    else if (up == "ALT")   bit = 0x04;
    else if (up == "GUI" || up == "WIN" || up == "CMD") bit = 0x08;
    if (!bit) break;
    mod |= bit;
    if (s2 < 0) { rest = ""; break; }
    rest = rest.substring(s2 + 1);
  }

  if (mod == 0) return;
  rest.trim();
  if (rest.length() == 0) { tapKey(mod, 0); return; }
  uint8_t m, k;
  if (charToKey(rest[0], m, k)) tapKey(mod | m, k);
}

void payloadTick() {
  if (!payloadRunning) return;
  if (millis() - lastAction < actionDelay) return;

  if (payloadPos >= payloadScript.length()) {
    payloadRunning = false;
    releaseAll();
    Serial.println("[PL] Payload finished");
    return;
  }

  int nl = payloadScript.indexOf('\n', payloadPos);
  String line = (nl < 0) ? payloadScript.substring(payloadPos)
                         : payloadScript.substring(payloadPos, nl);
  payloadPos = (nl < 0) ? payloadScript.length() : nl + 1;

  lastAction = millis();
  executeLine(line);
}

void runPayload(const String &script) {
  if (!hidReady()) { Serial.println("[PL] HID not ready"); return; }
  payloadScript = script;
  payloadPos = 0;
  payloadRunning = true;
  actionDelay = 0;
  lastAction = millis();
  Serial.println("[PL] Payload started");
}

// ---------------- Payload presets ----------------
const char *PL_HELLO =
  "DELAY 800\n"
  "GUI r\n"
  "DELAY 700\n"
  "STRING notepad\n"
  "DELAY 700\n"
  "ENTER\n"
  "DELAY 1200\n"
  "STRING Hello from Maricha!\n"
  "ENTER\n"
  "STRING Powered by TechChip\n";

String buildPsPayload(const String &ps) {
  String s = "DELAY 800\nGUI x\nDELAY 500\nSTRING a\nDELAY 900\n";
  s += "STRING " + ps + "\n";
  s += "ENTER\nDELAY 600\nSTRING exit\nENTER\n";
  return s;
}

// ---------------- Web UI ----------------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Maricha</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3}
header{padding:14px 16px;background:#161b22;border-bottom:1px solid #30363d}
h1{margin:0;font-size:17px;letter-spacing:.5px}h1 span{color:#3fb950}
.tabs{display:flex;background:#161b22;border-bottom:1px solid #30363d}
.tabs button{flex:1;padding:12px;background:none;border:0;color:#8b949e;font-size:13px;font-weight:600}
.tabs button.on{color:#3fb950;border-bottom:2px solid #3fb950}
.pane{display:none;padding:16px}.pane.on{display:block}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px;margin-bottom:12px}
button.b{width:100%;padding:12px;border:0;border-radius:6px;font-size:14px;font-weight:700;cursor:pointer}
.g{background:#238636;color:#fff}.r{background:#da3633;color:#fff}
.b2{background:#21262d;color:#e6edf3;border:1px solid #30363d}
input,select,textarea{width:100%;padding:9px;margin:5px 0 11px;background:#0d1117;color:#e6edf3;
border:1px solid #30363d;border-radius:6px;font-family:inherit;font-size:13px}
textarea{height:170px;font-family:monospace}
label{font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:.6px}
.pill{display:inline-block;padding:3px 9px;border-radius:20px;font-size:11px;font-weight:700}
.off{background:#30363d;color:#8b949e}.ok{background:#238636;color:#fff}
</style></head><body>
<header><h1>MARICHA <span>&#9679;</span></h1></header>
<div class="tabs">
<button class="on" onclick="tab(0,this)">Connect</button>
<button onclick="tab(1,this)">Payloads</button>
<button onclick="tab(2,this)">Settings</button>
</div>

<div class="pane on">
  <div class="card">
    <label>Status</label><br><br>
    <span class="pill off" id="st">Idle</span>
    <p style="font-size:12px;color:#8b949e" id="sub">Not advertising</p>
  </div>
  <div class="card">
    <button class="b g" onclick="cmd('start')">Start discovery</button><br><br>
    <button class="b b2" onclick="cmd('stop')">Stop</button>
  </div>
  <div class="card">
    <label>Mode</label>
    <select id="mode" onchange="setMode()">
      <option value="swift">Microsoft Swift Pair</option>
      <option value="beacon">Standard BLE beacon</option>
    </select>
    <button class="b b2" onclick="cmd('forget')">Forget pairings</button>
  </div>
</div>

<div class="pane">
  <div class="card">
    <label>Quick payloads</label><br><br>
    <button class="b b2" onclick="preset('hello')">Notepad + Hello</button><br><br>
    <input id="ps" placeholder="PowerShell one-liner...">
    <button class="b b2" onclick="preset('ps')">Run as PowerShell</button>
  </div>
  <div class="card">
    <label>Custom script</label>
    <textarea id="scr">DELAY 500
GUI r
DELAY 600
STRING notepad
ENTER</textarea>
    <button class="b g" onclick="run()">Run payload</button><br><br>
    <button class="b r" onclick="cmd('abort')">Stop payload</button>
  </div>
</div>

<div class="pane">
  <div class="card">
    <label>BLE name</label><input id="nm" maxlength="25">
    <label>Appearance</label><select id="ap"></select>
    <label>Discovery timeout (5-60 s)</label><input id="dur" type="number" min="5" max="60">
    <label>Wi-Fi SSID</label><input id="ssid" maxlength="31">
    <label>Wi-Fi password</label><input id="pw" maxlength="63">
    <button class="b g" onclick="save()">Save &amp; reboot</button>
  </div>
</div>

<script>
let profs=[];
function tab(i,el){document.querySelectorAll('.pane').forEach((p,n)=>p.classList.toggle('on',n==i));
document.querySelectorAll('.tabs button').forEach(b=>b.classList.remove('on'));el.classList.add('on');}
function cmd(c){fetch('/api/'+c).then(()=>setTimeout(refresh,300));}
function preset(k){
  if(k=='hello'){runScript('hello');return;}
  let v=document.getElementById('ps').value;
  fetch('/api/ps',{method:'POST',body:v}).then(()=>setTimeout(refresh,300));
}
function run(){runScript(document.getElementById('scr').value);}
function runScript(t){
  fetch('/api/payload',{method:'POST',body:t}).then(()=>setTimeout(refresh,400));
}
function setMode(){fetch('/api/mode?m='+document.getElementById('mode').value);}
function save(){
  let d=new URLSearchParams({name:nm.value,appear:ap.value,dur:dur.value,
    ssid:document.getElementById('ssid').value,pw:document.getElementById('pw').value});
  fetch('/api/settings',{method:'POST',body:d}).then(()=>alert('Saved. Rebooting...'));
}
function refresh(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    const st=document.getElementById('st');
    st.textContent=s.hid?'HID ready':(s.adv?'Advertising':'Idle');
    st.className='pill '+(s.hid?'ok':(s.adv?'ok':'off'));
    document.getElementById('sub').textContent=s.hid?'Paired - payloads will type now':'Not connected';
    document.getElementById('mode').value=s.swift?'swift':'beacon';
  });
}
fetch('/api/config').then(r=>r.json()).then(c=>{
  document.getElementById('nm').value=c.name;
  document.getElementById('dur').value=c.dur;
  document.getElementById('ssid').value=c.ssid;
  document.getElementById('pw').value=c.pass;
  profs=c.profiles;const ap=document.getElementById('ap');
  profs.forEach((p,i)=>{let o=document.createElement('option');o.value=p.appearance;
    o.textContent=p.label;if(p.appearance==c.appear)o.selected=true;ap.add(o);});
});
refresh();setInterval(refresh,2000);
</script></body></html>
)HTML";

// ---------------- Web handlers ----------------
void sendJson(const String &j) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleIndex() { server.send_P(200, "text/html", INDEX_HTML); }

void handleStatus() {
  String j = "{\"adv\":";
  j += advRunning ? "true" : "false";
  j += ",\"hid\":";
  j += hidReady() ? "true" : "false";
  j += ",\"payload\":";
  j += payloadRunning ? "true" : "false";
  j += ",\"swift\":";
  j += swiftPairMode ? "true" : "false";
  j += "}";
  sendJson(j);
}

void handleConfig() {
  String j = "{\"name\":\"" + devName + "\",\"appear\":" + String(devAppear);
  j += ",\"dur\":" + String(discTimeout) + ",\"ssid\":\"" + apSSID;
  j += "\",\"pass\":\"" + apPass + "\",\"profiles\":[";
  for (int i = 0; i < profileCount; i++) {
    if (i) j += ",";
    j += "{\"label\":\"" + String(profiles[i].label) + "\",\"appearance\":" + String(profiles[i].appearance) + "}";
  }
  j += "]}";
  sendJson(j);
}

void handleStart() {
  if (!advRunning && !deviceConnected) startAdvertising();
  sendJson("{\"ok\":true}");
}

void handleStop() {
  stopAdvertising();
  if (deviceConnected) { pServer->disconnect(pServer->getConnId()); }
  payloadRunning = false;
  releaseAll();
  sendJson("{\"ok\":true}");
}

void handleAbort() {
  payloadRunning = false;
  releaseAll();
  sendJson("{\"ok\":true}");
}

void handleForget() {
  int n = esp_ble_get_bond_device_num();
  esp_ble_bond_dev_t *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (list && n > 0) {
    esp_ble_get_bond_device_list(&n, list);
    for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  }
  if (list) free(list);
  sendJson("{\"ok\":true,\"cleared\":" + String(n) + "}");
}

void handleMode() {
  swiftPairMode = (server.arg("m") != "beacon");
  saveSettings();
  if (advRunning) { stopAdvertising(); delay(150); startAdvertising(); }
  sendJson("{\"ok\":true}");
}

void handlePayload() {
  String body = server.arg("plain");
  server.send(200, "text/plain", "ok");
  if (body.length()) runPayload(body);
}

void handlePs() {
  String body = server.arg("plain");
  server.send(200, "text/plain", "ok");
  if (body.length()) runPayload(buildPsPayload(body));
}

void handleHello() {
  server.send(200, "text/plain", "ok");
  runPayload(PL_HELLO);
}

void handleSettings() {
  if (server.hasArg("name") && server.arg("name").length())
    devName = server.arg("name").substring(0, 25);
  if (server.hasArg("appear")) devAppear = (uint16_t)strtol(server.arg("appear").c_str(), nullptr, 10);
  if (server.hasArg("dur")) {
    long d = server.arg("dur").toInt();
    discTimeout = constrain(d, 5, 60);
  }
  if (server.hasArg("ssid") && server.arg("ssid").length())
    apSSID = server.arg("ssid").substring(0, 31);
  if (server.hasArg("pw") && server.arg("pw").length() >= 8)
    apPass = server.arg("pw").substring(0, 63);
  saveSettings();
  server.send(200, "text/plain", "saved");
  delay(400);
  ESP.restart();
}

void setupWeb() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/start", HTTP_GET, handleStart);
  server.on("/api/stop", HTTP_GET, handleStop);
  server.on("/api/abort", HTTP_GET, handleAbort);
  server.on("/api/forget", HTTP_GET, handleForget);
  server.on("/api/mode", HTTP_GET, handleMode);
  server.on("/api/hello", HTTP_GET, handleHello);
  server.on("/api/payload", HTTP_POST, handlePayload);
  server.on("/api/ps", HTTP_POST, handlePs);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
}

// ---------------- BLE setup ----------------
void setupBLE() {
  BLEDevice::init(devName.c_str());
  BLEDevice::setMTU(185);
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  hid = new BLEHIDDevice(pServer);
  inputKeyboard  = hid->inputReport(1);
  inputMouse     = hid->inputReport(2);
  outputKeyboard = hid->outputReport(1);
  outputKeyboard->setCallbacks(new OutputCB());

  hid->manufacturer()->setValue("TechChip");
  hid->pnp(0x02, 0x303A, 0x0001, 0x0110);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap((uint8_t *)reportMap, sizeof(reportMap));
  hid->startServices();

  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  pAdv = BLEDevice::getAdvertising();
  startAdvertising();
}

// ---------------- Serial CLI ----------------
void printMenu() {
  Serial.println(F("\n=== MARICHA CLI ==="));
  Serial.println(F("1/start  0/stop  2/status  3/profiles  4/hid"));
  Serial.println(F("5/hello  name <t>  appearance <hex>  duration <s>"));
  Serial.println(F("mode swift|beacon  wifi  wifi set <s> <p>"));
  Serial.println(F("payload begin  payload run  abort  forget  menu"));
}

void printProfiles() {
  for (int i = 0; i < profileCount; i++)
    Serial.printf("  %d) %s  (0x%04X)\n", i, profiles[i].label, profiles[i].appearance);
}

String cliLine = "";
bool inScriptMode = false;
String cliScript = "";

void cliCommand(String l) {
  l.trim();
  if (l.length() == 0) return;

  if (inScriptMode) {
    if (l == "END") {
      inScriptMode = false;
      Serial.printf("[PL] Script loaded (%u bytes)\n", cliScript.length());
    } else cliScript += l + "\n";
    return;
  }

  String cmd = l, arg = "";
  int sp = l.indexOf(' ');
  if (sp > 0) { cmd = l.substring(0, sp); arg = l.substring(sp + 1); }
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "menu") { printMenu(); return; }
  if (cmd == "1" || cmd == "start") { if (!deviceConnected) startAdvertising(); return; }
  if (cmd == "0" || cmd == "stop") { stopAdvertising(); if (deviceConnected) pServer->disconnect(pServer->getConnId()); return; }
  if (cmd == "2" || cmd == "status") {
    Serial.printf("adv=%d connected=%d hid=%d payload=%d name=%s appear=0x%04X\n",
                  advRunning, deviceConnected, hidReady(), payloadRunning,
                  devName.c_str(), devAppear);
    return;
  }
  if (cmd == "3" || cmd == "profiles") { printProfiles(); return; }
  if (cmd == "4" || cmd == "hid") {
    Serial.println(F("Payloads: 5=hello, payload begin, payload run"));
    return;
  }
  if (cmd == "5" || cmd == "hello") { runPayload(PL_HELLO); return; }
  if (cmd == "name" && arg.length()) { devName = arg.substring(0, 25); saveSettings(); Serial.println("Name set (reboot to apply)"); return; }
  if (cmd == "appearance" && arg.length()) { devAppear = (uint16_t)strtol(arg.c_str(), nullptr, 0); saveSettings(); Serial.println("Appearance set (reboot)"); return; }
  if (cmd == "duration" && arg.length()) { discTimeout = constrain(arg.toInt(), 5, 60); saveSettings(); Serial.println("Duration set"); return; }
  if (cmd == "mode") {
    swiftPairMode = (arg != "beacon");
    saveSettings();
    Serial.println(swiftPairMode ? "Swift Pair mode" : "Beacon mode");
    return;
  }
  if (cmd == "wifi") {
    if (arg.startsWith("set ")) {
      String rest = arg.substring(4);
      int s2 = rest.indexOf(' ');
      if (s2 > 0) {
        apSSID = rest.substring(0, s2);
        apPass = rest.substring(s2 + 1);
        saveSettings();
        Serial.println("Wi-Fi credentials updated");
      } else Serial.println("Usage: wifi set <ssid> <pass>");
    } else Serial.printf("SSID=%s PASS=%s\n", apSSID.c_str(), apPass.c_str());
    return;
  }
  if (cmd == "payload") {
    if (arg == "begin") { inScriptMode = true; cliScript = ""; Serial.println("Script mode - end with END"); }
    else if (arg == "run") { if (cliScript.length()) runPayload(cliScript); else Serial.println("No script loaded"); }
    return;
  }
  if (cmd == "abort") { payloadRunning = false; releaseAll(); Serial.println("Aborted"); return; }
  if (cmd == "forget") { handleForget(); return; }
  Serial.println("Unknown command - type 'help'");
}

void cliTick() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { cliCommand(cliLine); cliLine = ""; }
    else if (cliLine.length() < 512) cliLine += c;
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== MARICHA-ESP32 by TechChip (Arduino port) ==="));

  loadSettings();
  for (int i = 0; i < 4; i++) swiftHash[i] = random(0, 256);

  WiFi.mode(WIFI_AP);
  String mac = WiFi.softAPmacAddress();
  mac.replace(":", "");
  String suffix = mac.substring(mac.length() - 4);
  suffix.toUpperCase();
  String ssid = apSSID + "-" + suffix;
  WiFi.softAP(ssid.c_str(), apPass.c_str(), 1, 0, 4);

  Serial.printf("[WIFI] AP: %s / %s\n", ssid.c_str(), apPass.c_str());
  Serial.println(F("[WIFI] Dashboard: http://192.168.4.1"));

  setupBLE();
  setupWeb();
  printMenu();
}

void loop() {
  server.handleClient();
  cliTick();
  payloadTick();

  if (advRunning && !deviceConnected && (millis() - advStartMs > discTimeout * 1000UL)) {
    stopAdvertising();
  }
  delay(2);
}