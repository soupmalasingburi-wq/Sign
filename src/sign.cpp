// ============================================================
//  SIGN  -  "ชั้น 2 ว่างนะครับ" sign for ESP32-2432S028 (CYD)
//  - shows a big Thai sign (pre-rendered in sign_data.h)
//  - hold a finger on the screen for 1 second to switch  ว่าง <-> เต็ม
//  - or open  http://sign.local  (or the board's IP) on a phone
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <LovyanGFX.hpp>
#include "sign_data.h"

#define PANEL_ST7789 1   // 1 = CYD with 2 USB ports (ST7789), 0 = 1 USB port (ILI9341)
#define PANEL_INVERT 0   // set to 1 if the colours look inverted (green looks pink)

class LGFX : public lgfx::LGFX_Device {
#if PANEL_ST7789
  lgfx::Panel_ST7789 _panel;
#else
  lgfx::Panel_ILI9341 _panel;
#endif
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;
public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 40000000; c.freq_read = 16000000;
      c.spi_3wire = false; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14; c.pin_mosi = 13; c.pin_miso = 12; c.pin_dc = 2;
      _bus.config(c); _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = 15; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 240; c.panel_height = 320;
      c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.rgb_order = false; c.dlen_16bit = false; c.bus_shared = false;
      c.invert = PANEL_INVERT ? true : false;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl = 21; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light);
    }
    {
      auto c = _touch.config();
      c.x_min = 300; c.x_max = 3900; c.y_min = 200; c.y_max = 3700;
      c.pin_int = 36; c.bus_shared = false; c.offset_rotation = 0;
      c.spi_host = SPI3_HOST; c.freq = 1000000;
      c.pin_sclk = 25; c.pin_mosi = 32; c.pin_miso = 39; c.pin_cs = 33;
      _touch.config(c); _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX lcd;
WebServer server(80);
Preferences prefs;

const int W = 320, H = 240;
uint8_t* mask = nullptr;      // decoded text mask of the sign on screen (320 x 240, 0..255)
String ssid, pass;
int state = 0;                // 0 = vacant (ว่าง), 1 = full (เต็ม)
bool setupOnly = false;

const uint8_t* const RLE[2] = { SIGN_VACANT, SIGN_FULL };
const uint32_t RLE_LEN[2] = { SIGN_VACANT_LEN, SIGN_FULL_LEN };
const int BG_TOP[2][3] = { { 24, 178, 110 }, { 214, 60, 60 } };   // wallpaper colour at the top
const int BG_BOT[2][3] = { { 8, 100, 76 }, { 118, 20, 40 } };     // ... and at the bottom

inline uint16_t p565(int r, int g, int b) {
  if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
  if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void thickLine(int x0, int y0, int x1, int y1, uint16_t col) {
  for (int dx = -1; dx <= 1; dx++)
    for (int dy = -1; dy <= 1; dy++) lcd.drawLine(x0 + dx, y0 + dy, x1 + dx, y1 + dy, col);
}

void drawSign(int s) {
  if (!mask) return;
  // 1) unpack the run-length encoded text mask
  const uint8_t* d = RLE[s];
  size_t n = RLE_LEN[s], pos = 0, total = (size_t)W * H;
  for (size_t i = 0; i + 1 < n && pos < total; i += 2) {
    uint8_t a = d[i], r = d[i + 1];
    while (r-- && pos < total) mask[pos++] = a;
  }
  // 2) paint row by row: gradient + soft shadow + white text, drawn as runs of equal colour
  static uint16_t col[W];
  lcd.startWrite();
  for (int y = 0; y < H; y++) {
    int r0 = BG_TOP[s][0] + (BG_BOT[s][0] - BG_TOP[s][0]) * y / (H - 1);
    int g0 = BG_TOP[s][1] + (BG_BOT[s][1] - BG_TOP[s][1]) * y / (H - 1);
    int b0 = BG_TOP[s][2] + (BG_BOT[s][2] - BG_TOP[s][2]) * y / (H - 1);
    for (int x = 0; x < W; x++) {
      int r = r0, g = g0, b = b0;
      if (x >= 3 && y >= 3) {
        int sh = mask[(y - 3) * W + (x - 3)];
        r -= r * sh * 90 / 65025; g -= g * sh * 90 / 65025; b -= b * sh * 90 / 65025;
      }
      int a = mask[y * W + x];
      r += (255 - r) * a / 255; g += (255 - g) * a / 255; b += (255 - b) * a / 255;
      col[x] = p565(r, g, b);
    }
    int x0 = 0;
    for (int x = 1; x <= W; x++) {
      if (x == W || col[x] != col[x0]) { lcd.drawFastHLine(x0, y, x - x0, col[x0]); x0 = x; }
    }
  }
  // 3) frame and a small tick / cross badge
  lcd.drawRoundRect(6, 6, 308, 228, 20, 0xFFFF);
  lcd.drawRoundRect(7, 7, 306, 226, 19, 0xFFFF);
  lcd.fillCircle(284, 36, 20, 0xFFFF);
  uint16_t c = p565(BG_TOP[s][0], BG_TOP[s][1], BG_TOP[s][2]);
  if (s == 0) { thickLine(274, 36, 281, 44, c); thickLine(281, 44, 295, 28, c); }
  else { thickLine(275, 27, 293, 45, c); thickLine(275, 45, 293, 27, c); }
  lcd.endWrite();
}

void saveState() { prefs.begin("sign", false); prefs.putInt("state", state); prefs.end(); }
void setState(int s) { state = s ? 1 : 0; saveState(); drawSign(state); }

void drawSetupScreen() {
  lcd.fillScreen(p565(14, 16, 40));
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(0xFFFF);
  lcd.setFont(&fonts::FreeSansBold18pt7b); lcd.drawString("Sign setup", 160, 70);
  lcd.setFont(&fonts::FreeSans12pt7b);
  lcd.drawString("1. Join Wi-Fi: SIGN-SETUP", 160, 125);
  lcd.drawString("2. Open 192.168.4.1", 160, 160);
  lcd.drawString("3. Enter your Wi-Fi", 160, 195);
}

// ---------------- web pages ----------------
const char CTRL_HTML[] PROGMEM = R"HTML(<!doctype html><html lang=th><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>ป้ายชั้น 2</title>
<style>body{margin:0;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:16px;font:20px system-ui,sans-serif;background:#10161f;color:#fff;padding:20px;box-sizing:border-box}
button{width:min(100%,360px);padding:26px;border:0;border-radius:22px;font-size:26px;font-weight:700;color:#fff;opacity:.55}
#a{background:#18b26e}#b{background:#d63c3c}button.on{opacity:1;outline:5px solid #fff}h2{margin:0}small{color:#9aa4b0}</style>
<h2>ป้ายชั้น 2</h2>
<button id=a onclick=go(0)>ว่างนะครับ</button>
<button id=b onclick=go(1)>เต็มแล้วครับ</button>
<small id=m></small>
<script>
function st(j){a.className=j.state==0?'on':'';b.className=j.state==1?'on':'';m.textContent=j.state==0?'ตอนนี้: ว่าง':'ตอนนี้: เต็ม'}
function go(s){fetch('/set?s='+s).then(function(r){return r.json()}).then(st)}
fetch('/state').then(function(r){return r.json()}).then(st)
</script>)HTML";

const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html><html lang=th><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>ตั้งค่า Wi-Fi</title>
<style>body{margin:0;padding:24px;font:18px system-ui,sans-serif;background:#10161f;color:#fff;max-width:460px;margin:auto}
input,button{width:100%;padding:14px;margin:6px 0 16px;border-radius:12px;border:1px solid #345;background:#1b2635;color:#fff;font-size:18px;box-sizing:border-box}
button{background:#18b26e;border:0;font-weight:700}label{color:#9aa4b0;font-size:14px}</style>
<h2>ตั้งค่า Wi-Fi</h2>
<form method=post action=/save>
<label>ชื่อ Wi-Fi (ต้องเป็น 2.4GHz)</label><input name=ssid value="%SSID%" autocomplete=off>
<label>รหัสผ่าน Wi-Fi</label><input name=pass type=password autocomplete=off>
<button>บันทึกและรีสตาร์ท</button></form>)HTML";

void hRoot() {
  if (setupOnly) { server.sendHeader("Location", "/setup"); server.send(302, "text/plain", ""); return; }
  server.send_P(200, "text/html", CTRL_HTML);
}
void hState() { server.send(200, "application/json", String("{\"state\":") + state + "}"); }
void hSet() {
  setState(server.arg("s").toInt());
  hState();
}
void hSetup() {
  String h = String(SETUP_HTML);
  h.replace("%SSID%", ssid);
  server.send(200, "text/html", h);
}
void hSave() {
  String s = server.arg("ssid"); s.trim();
  prefs.begin("sign", false);
  prefs.putString("ssid", s);
  if (server.arg("pass").length()) prefs.putString("pass", server.arg("pass"));
  prefs.end();
  server.send(200, "text/html", "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><body style='font:20px system-ui;background:#10161f;color:#fff;padding:24px'>บันทึกแล้ว กำลังรีสตาร์ท...");
  delay(1000);
  ESP.restart();
}

void setup() {
  lcd.init(); lcd.setRotation(1); lcd.setBrightness(255);
  mask = (uint8_t*)malloc((size_t)W * H);
  prefs.begin("sign", true);
  ssid = prefs.getString("ssid", ""); pass = prefs.getString("pass", ""); state = prefs.getInt("state", 0);
  prefs.end();

  server.on("/", hRoot); server.on("/state", hState); server.on("/set", hSet);
  server.on("/setup", hSetup); server.on("/save", HTTP_POST, hSave);

  delay(150);
  int32_t tx, ty;
  bool force = lcd.getTouch(&tx, &ty);        // hold the screen while powering on = Wi-Fi setup
  if (ssid == "" || force) {
    setupOnly = true;
    WiFi.mode(WIFI_AP_STA); WiFi.softAP("SIGN-SETUP");
    drawSetupScreen();
    server.begin();
    return;
  }
  drawSign(state);                            // show the sign straight away, Wi-Fi connects in the background
  WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(), pass.c_str());
  server.begin();
}

void loop() {
  server.handleClient();
  if (setupOnly) { delay(5); return; }

  // hold a finger for 1 s to switch the sign
  static bool was = false, done = false;
  static uint32_t t0 = 0, rel = 0;
  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    if (!was) { if (millis() - rel > 80) { was = true; done = false; t0 = millis(); } }
    else if (!done && millis() - t0 > 1000) { done = true; setState(1 - state); }
  } else { if (was) rel = millis(); was = false; }

  // keep Wi-Fi alive and publish the name sign.local
  static bool wasUp = false;
  static uint32_t lastTry = 0;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wasUp) { MDNS.begin("sign"); MDNS.addService("http", "tcp", 80); }
  if (!up && millis() - lastTry > 20000) {
    lastTry = millis();
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  wasUp = up;
  delay(5);
}
