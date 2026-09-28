/*
 * ESP32-S3-N16R8 + 3.5" ILI9488 (480x320) SPI TFT
 * Web-controlled Clock + Weather station
 *
 * WIRING (as per your 7200.jpg)
 *   3V3->VCC  GND->GND  GPIO10->CS  GPIO12->RESET  GPIO11->DC/RS
 *   GPIO13->SDI(MOSI)  GPIO14->SCK  GPIO9->SDO(MISO)  GPIO8->LED(backlight)
 *
 * ARDUINO IDE SETTINGS
 *   Board: "ESP32S3 Dev Module"   (esp32 core 3.x)
 *   PSRAM: "OPI PSRAM"            Flash Size: 16MB   USB CDC On Boot: Enabled
 *   Libraries: LovyanGFX (by lovyan03), ArduinoJson 7.x
 *
 * FEATURES
 *   - 5 clock styles: 1 Digital | 2 Analog | 3 Neon Rings | 4 Split | 5 Dashboard
 *   - Weather (Open-Meteo, no API key) with drawn icons, fetched in a background task
 *   - Web panel: http://esp32clock.local  (or the IP shown on screen / serial)
 *   - Style, mode (clock/weather/auto), theme, brightness, 12/24h, seconds, C/F,
 *     city/lat/lon, timezone - all saved to flash
 *   - If WiFi fails: hotspot "ESP32-Clock" (pass 12345678) -> open 192.168.4.1
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------------- WiFi defaults (can also be set from the AP page) ----------------
const char* DEF_SSID = "Home Network";
const char* DEF_PASS = "tamim24@#";

// ---------------- Display driver ----------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host   = SPI2_HOST;
      c.spi_mode   = 0;
      c.freq_write = 40000000;   // lower to 27000000 / 20000000 if you see glitches
      c.freq_read  = 16000000;
      c.spi_3wire  = false;
      c.use_lock   = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 9;
      c.pin_dc   = 11;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs   = 10;
      c.pin_rst  = 12;
      c.pin_busy = -1;
      c.panel_width  = 320;
      c.panel_height = 480;
      c.offset_x = 0;
      c.offset_y = 0;
      c.offset_rotation = 0;
      c.readable  = false;
      c.invert    = false;       // if colours look inverted set true
      c.rgb_order = false;       // if red/blue swapped set true
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl = 8;
      c.invert = false;
      c.freq = 44100;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
lgfx::LGFX_Sprite cv(&tft);   // full-screen back buffer -> flicker-free

// ---------------- Settings ----------------
struct Settings {
  uint8_t style = 0;        // 0..4
  uint8_t mode = 0;         // 0 clock, 1 weather, 2 auto
  uint8_t bright = 200;
  bool h24 = false;
  bool showSec = true;
  bool celsius = true;
  uint8_t theme = 0;
  uint16_t autoSecs = 10;
  uint16_t wxMins = 15;
  float lat = 24.3745f;     // Rajshahi
  float lon = 88.6042f;
  String city = "Rajshahi";
  String tz = "<+06>-6";    // Bangladesh
} cfg;

Preferences prefs;
bool prefsDirty = false;
uint32_t prefsDirtyAt = 0;

void loadSettings() {
  prefs.begin("clock", true);
  cfg.style   = prefs.getUChar("style", 0);
  cfg.mode    = prefs.getUChar("mode", 0);
  cfg.bright  = prefs.getUChar("bri", 200);
  cfg.h24     = prefs.getBool("h24", false);
  cfg.showSec = prefs.getBool("sec", true);
  cfg.celsius = prefs.getBool("cel", true);
  cfg.theme   = prefs.getUChar("theme", 0);
  cfg.autoSecs= prefs.getUShort("auto", 10);
  cfg.wxMins  = prefs.getUShort("wxm", 15);
  cfg.lat     = prefs.getFloat("lat", 24.3745f);
  cfg.lon     = prefs.getFloat("lon", 88.6042f);
  cfg.city    = prefs.getString("city", "Rajshahi");
  cfg.tz      = prefs.getString("tz", "<+06>-6");
  prefs.end();
}
void saveSettings() {
  prefs.begin("clock", false);
  prefs.putUChar("style", cfg.style);
  prefs.putUChar("mode", cfg.mode);
  prefs.putUChar("bri", cfg.bright);
  prefs.putBool("h24", cfg.h24);
  prefs.putBool("sec", cfg.showSec);
  prefs.putBool("cel", cfg.celsius);
  prefs.putUChar("theme", cfg.theme);
  prefs.putUShort("auto", cfg.autoSecs);
  prefs.putUShort("wxm", cfg.wxMins);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("city", cfg.city);
  prefs.putString("tz", cfg.tz);
  prefs.end();
  prefsDirty = false;
}
void markDirty() { prefsDirty = true; prefsDirtyAt = millis(); }

// ---------------- Themes ----------------
struct Theme { uint8_t r, g, b; };
const Theme THEMES[] = {
  {25,195,230}, {255,176,32}, {61,220,132}, {224,64,251}, {255,77,77}, {255,255,255}
};
uint16_t cACC, cBG, cTXT, cDIM, cTRK;

void applyTheme() {
  const Theme& t = THEMES[cfg.theme % 6];
  cACC = tft.color565(t.r, t.g, t.b);
  cBG  = tft.color565(8, 10, 16);
  cTXT = 0xFFFF;
  cDIM = tft.color565(140, 150, 165);
  cTRK = tft.color565(30, 36, 50);
}

// ---------------- Weather ----------------
struct Weather {
  bool valid = false;
  float temp = 0, feels = 0, wind = 0, hi = 0, lo = 0;
  int hum = 0, code = 0;
  bool isDay = true;
  time_t updated = 0;
} wx;
volatile bool wxForce = false;
volatile bool needRedraw = true;

const char* wxText(int c) {
  if (c == 0) return "Clear sky";
  if (c == 1) return "Mainly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 65) return "Rain";
  if (c == 66 || c == 67) return "Freezing rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Rain showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c >= 95) return "Thunderstorm";
  return "Unknown";
}

bool fetchWeather() {
  char url[330];
  snprintf(url, sizeof url,
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
    "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1",
    cfg.lat, cfg.lon);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(9000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject c = doc["current"];
  if (c.isNull()) return false;
  Weather t;
  t.temp  = c["temperature_2m"] | 0.0f;
  t.feels = c["apparent_temperature"] | 0.0f;
  t.hum   = c["relative_humidity_2m"] | 0;
  t.wind  = c["wind_speed_10m"] | 0.0f;
  t.code  = c["weather_code"] | 0;
  t.isDay = (c["is_day"] | 1) == 1;
  t.hi    = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  t.lo    = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  t.updated = time(nullptr);
  t.valid = true;
  wx = t;
  needRedraw = true;
  return true;
}

void weatherTask(void*) {
  uint32_t nextTry = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (wxForce || (int32_t)(millis() - nextTry) >= 0)) {
      wxForce = false;
      bool ok = fetchWeather();
      nextTry = millis() + (ok ? (uint32_t)cfg.wxMins * 60000UL : 30000UL);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---------------- Time ----------------
struct tm tmNow;
bool timeOK = false;
int lastSec = -1;
uint32_t secStart = 0;
float subsec = 0;

void applyTZ() { setenv("TZ", cfg.tz.c_str(), 1); tzset(); }

bool updateTime() {   // returns true when the second changed
  timeOK = getLocalTime(&tmNow, 0) && tmNow.tm_year > 120;
  if (!timeOK) return false;
  bool ch = false;
  if (tmNow.tm_sec != lastSec) { lastSec = tmNow.tm_sec; secStart = millis(); ch = true; }
  float s = (millis() - secStart) / 1000.0f;
  subsec = s > 1 ? 1 : s;
  return ch;
}

const char* DAYS[]   = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
const char* DAYS3[]  = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char* MONTHS3[]= {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
const char* MONTHS[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};

void fmtHM(char* b) {
  int h = tmNow.tm_hour;
  if (!cfg.h24) { h %= 12; if (h == 0) h = 12; }
  snprintf(b, 8, cfg.h24 ? "%02d:%02d" : "%d:%02d", h, tmNow.tm_min);
}
int hour12() { int h = tmNow.tm_hour % 12; return h == 0 ? 12 : h; }
float dispT(float c) { return cfg.celsius ? c : c * 9.0f / 5.0f + 32.0f; }

// ---------------- Drawing helpers ----------------
void txt(const lgfx::IFont* f, float sz, textdatum_t d, uint16_t col, int x, int y, const char* s) {
  cv.setFont(f); cv.setTextSize(sz); cv.setTextDatum(d); cv.setTextColor(col);
  cv.drawString(s, x, y);
}
void bar(int x, int y, int w, int h, float f, uint16_t col) {
  cv.fillRoundRect(x, y, w, h, h / 2, cTRK);
  if (f > 0) { if (f > 1) f = 1; cv.fillRoundRect(x, y, max(h, (int)(w * f)), h, h / 2, col); }
}
void polar(int cx, int cy, float len, float angDeg, int& x, int& y) {
  float r = (angDeg - 90.0f) * DEG_TO_RAD;
  x = cx + (int)(cosf(r) * len);
  y = cy + (int)(sinf(r) * len);
}

// Temperature with a drawn degree sign, centred at x
void drawTempC(int x, int y, const lgfx::IFont* f, float sz, uint16_t col) {
  char n[8];
  if (wx.valid) snprintf(n, sizeof n, "%d", (int)lroundf(dispT(wx.temp))); else strcpy(n, "--");
  cv.setFont(f); cv.setTextSize(sz); cv.setTextColor(col);
  int w = cv.textWidth(n), h = cv.fontHeight();
  int r = max(2, h / 12);
  const char* u = cfg.celsius ? "C" : "F";
  int uw = cv.textWidth(u);
  int total = w + 4 + 2 * r + 2 + uw;
  int sx = x - total / 2;
  cv.setTextDatum(middle_left);
  cv.drawString(n, sx, y);
  cv.drawCircle(sx + w + 4 + r, y - h / 4, r, col);
  cv.drawCircle(sx + w + 4 + r, y - h / 4, r - 1, col);
  cv.drawString(u, sx + w + 4 + 2 * r + 2, y);
}

// Weather icon drawn with primitives
void drawWxIcon(int cx, int cy, int r, int code, bool day) {
  uint16_t sunC  = cv.color565(255, 196, 40);
  uint16_t cloudC= cv.color565(205, 215, 230);
  uint16_t darkC = cv.color565(115, 125, 145);
  uint16_t rainC = cv.color565(70, 150, 255);
  uint16_t boltC = cv.color565(255, 224, 50);
  uint16_t moonC = cv.color565(235, 235, 200);

  auto sun = [&](int x, int y, int rr) {
    cv.fillCircle(x, y, rr * 0.55f, sunC);
    for (int i = 0; i < 8; i++) {
      float a = i * 45 * DEG_TO_RAD;
      cv.drawWideLine(x + cosf(a) * rr * 0.75f, y + sinf(a) * rr * 0.75f,
                      x + cosf(a) * rr * 1.0f,  y + sinf(a) * rr * 1.0f, max(1, rr / 14), sunC);
    }
  };
  auto moon = [&](int x, int y, int rr) {
    cv.fillCircle(x, y, rr * 0.55f, moonC);
    cv.fillCircle(x + rr * 0.25f, y - rr * 0.18f, rr * 0.5f, cBG);
  };
  auto cloud = [&](int x, int y, int rr, uint16_t c) {
    cv.fillCircle(x - rr * 0.45f, y + rr * 0.1f, rr * 0.35f, c);
    cv.fillCircle(x + rr * 0.05f, y - rr * 0.15f, rr * 0.5f, c);
    cv.fillCircle(x + rr * 0.5f, y + rr * 0.12f, rr * 0.32f, c);
    cv.fillRect(x - rr * 0.45f, y + rr * 0.1f, rr * 0.95f, rr * 0.35f, c);
  };

  if (code == 0) {
    if (day) sun(cx, cy, r); else moon(cx, cy, r);
  } else if (code == 1 || code == 2) {
    if (day) sun(cx - r * 0.3f, cy - r * 0.3f, r * 0.7f); else moon(cx - r * 0.3f, cy - r * 0.3f, r * 0.7f);
    cloud(cx + r * 0.1f, cy + r * 0.2f, r * 0.85f, cloudC);
  } else if (code == 3) {
    cloud(cx - r * 0.2f, cy - r * 0.2f, r * 0.7f, darkC);
    cloud(cx + r * 0.1f, cy + r * 0.1f, r * 0.9f, cloudC);
  } else if (code == 45 || code == 48) {
    cloud(cx, cy - r * 0.25f, r * 0.9f, cloudC);
    for (int i = 0; i < 3; i++)
      cv.drawWideLine(cx - r * 0.6f + i * 6, cy + r * 0.35f + i * r * 0.2f, cx + r * 0.6f - i * 6, cy + r * 0.35f + i * r * 0.2f, max(1, r / 14), darkC);
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    cloud(cx, cy - r * 0.25f, r * 0.9f, darkC);
    for (int i = 0; i < 3; i++) {
      float x = cx - r * 0.4f + i * r * 0.4f;
      cv.drawWideLine(x, cy + r * 0.35f, x - r * 0.15f, cy + r * 0.75f, max(1, r / 12), rainC);
    }
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    cloud(cx, cy - r * 0.25f, r * 0.9f, cloudC);
    for (int i = 0; i < 3; i++)
      cv.fillCircle(cx - r * 0.4f + i * r * 0.4f, cy + r * 0.55f + (i % 2) * r * 0.15f, max(2, r / 10), 0xFFFF);
  } else if (code >= 95) {
    cloud(cx, cy - r * 0.25f, r * 0.9f, darkC);
    int bx = cx, by = cy + r * 0.2f;
    cv.fillTriangle(bx - r * 0.05f, by, bx + r * 0.25f, by, bx - r * 0.1f, by + r * 0.6f, boltC);
    cv.fillTriangle(bx - r * 0.2f, by + r * 0.3f, bx + r * 0.2f, by + r * 0.3f, bx - r * 0.02f, by + r * 0.85f, boltC);
  } else {
    cloud(cx, cy, r * 0.9f, cloudC);
  }
}

void drawWxStrip(int y) {
  cv.fillRoundRect(12, y, 456, 58, 12, cTRK);
  if (!wx.valid) { txt(&fonts::FreeSans12pt7b, 1, middle_center, cDIM, 240, y + 29, "Loading weather..."); return; }
  drawWxIcon(52, y + 29, 20, wx.code, wx.isDay);
  drawTempC(140, y + 29, &fonts::FreeSansBold18pt7b, 1, cTXT);
  txt(&fonts::FreeSans9pt7b, 1, middle_left, cTXT, 205, y + 18, wxText(wx.code));
  txt(&fonts::FreeSans9pt7b, 1, middle_left, cDIM, 205, y + 42, cfg.city.c_str());
  char b[32];
  snprintf(b, sizeof b, "H %d   L %d", (int)lroundf(dispT(wx.hi)), (int)lroundf(dispT(wx.lo)));
  txt(&fonts::FreeSans9pt7b, 1, middle_right, cTXT, 456, y + 18, b);
  snprintf(b, sizeof b, "Humidity %d%%", wx.hum);
  txt(&fonts::FreeSans9pt7b, 1, middle_right, cDIM, 456, y + 42, b);
}

// side panels used by analog & rings styles
void drawSides() {
  txt(&fonts::FreeSansBold18pt7b, 1, middle_center, cACC, 45, 105, DAYS3[tmNow.tm_wday]);
  char d[8]; snprintf(d, sizeof d, "%d", tmNow.tm_mday);
  txt(&fonts::Font7, 1, middle_center, cTXT, 45, 160, d);
  txt(&fonts::FreeSansBold12pt7b, 1, middle_center, cDIM, 45, 205, MONTHS3[tmNow.tm_mon]);
  if (wx.valid) {
    drawWxIcon(435, 100, 30, wx.code, wx.isDay);
    drawTempC(435, 160, &fonts::FreeSansBold18pt7b, 1, cTXT);
    char b[20]; snprintf(b, sizeof b, "%d%% hum", wx.hum);
    txt(&fonts::FreeSans9pt7b, 1, middle_center, cDIM, 435, 200, b);
  }
}

// ---------------- Clock style 1: Digital ----------------
void drawDigital() {
  cv.fillScreen(cBG);
  char hm[8]; fmtHM(hm);
  txt(&fonts::Font8, 1.55f, middle_center, cACC, 240, 100, hm);
  if (!cfg.h24) txt(&fonts::FreeSansBold12pt7b, 1, top_right, cDIM, 468, 14, tmNow.tm_hour >= 12 ? "PM" : "AM");
  if (cfg.showSec) {
    bar(60, 178, 360, 8, (tmNow.tm_sec + subsec) / 60.0f, cACC);
    char s[8]; snprintf(s, sizeof s, "%02d", tmNow.tm_sec);
    txt(&fonts::FreeSansBold12pt7b, 1, top_left, cDIM, 16, 14, s);
  }
  char d[40]; snprintf(d, sizeof d, "%s, %d %s %d", DAYS[tmNow.tm_wday], tmNow.tm_mday, MONTHS[tmNow.tm_mon], tmNow.tm_year + 1900);
  txt(&fonts::FreeSansBold12pt7b, 1.15f, middle_center, cTXT, 240, 218, d);
  drawWxStrip(250);
}

// ---------------- Clock style 2: Analog ----------------
void drawAnalog() {
  cv.fillScreen(cBG);
  const int cx = 240, cy = 160, R = 150;
  cv.fillCircle(cx, cy, R, cACC);
  cv.fillCircle(cx, cy, R - 3, cBG);
  for (int i = 0; i < 60; i++) {
    int x1, y1, x2, y2;
    bool big = (i % 5 == 0);
    polar(cx, cy, big ? R - 20 : R - 11, i * 6, x1, y1);
    polar(cx, cy, R - 6, i * 6, x2, y2);
    if (big) cv.drawWideLine(x1, y1, x2, y2, 2, cTXT); else cv.drawLine(x1, y1, x2, y2, cDIM);
  }
  for (int i = 1; i <= 12; i++) {
    int x, y; polar(cx, cy, R - 36, i * 30, x, y);
    char n[4]; snprintf(n, sizeof n, "%d", i);
    txt(&fonts::FreeSansBold9pt7b, 1, middle_center, cTXT, x, y, n);
  }
  float hA = ((tmNow.tm_hour % 12) + tmNow.tm_min / 60.0f) * 30.0f;
  float mA = (tmNow.tm_min + (tmNow.tm_sec + subsec) / 60.0f) * 6.0f;
  float sA = (tmNow.tm_sec + subsec) * 6.0f;
  int x, y;
  polar(cx, cy, R * 0.50f, hA, x, y); cv.drawWideLine(cx, cy, x, y, 5, cTXT);
  polar(cx, cy, R * 0.75f, mA, x, y); cv.drawWideLine(cx, cy, x, y, 3.5f, cACC);
  if (cfg.showSec) {
    uint16_t sc = tft.color565(255, 80, 80);
    polar(cx, cy, R * 0.82f, sA, x, y);
    int tx, ty; polar(cx, cy, -R * 0.15f, sA, tx, ty);
    cv.drawWideLine(tx, ty, x, y, 1.5f, sc);
    cv.fillCircle(cx, cy, 6, sc);
  } else cv.fillCircle(cx, cy, 6, cACC);
  cv.fillCircle(cx, cy, 2, cBG);
  drawSides();
}

// ---------------- Clock style 3: Neon Rings ----------------
void ringArc(int cx, int cy, int r0, int r1, float f, uint16_t col) {
  cv.fillCircle(cx, cy, r1, cTRK);
  cv.fillCircle(cx, cy, r0, cBG);
  if (f <= 0) return;
  if (f >= 0.999f) { cv.fillCircle(cx, cy, r1, col); cv.fillCircle(cx, cy, r0, cBG); return; }
  float a = 270 + 360 * f;
  if (a <= 360) cv.fillArc(cx, cy, r0, r1, 270, a, col);
  else { cv.fillArc(cx, cy, r0, r1, 270, 360, col); cv.fillArc(cx, cy, r0, r1, 0, a - 360, col); }
}
void drawRings() {
  cv.fillScreen(cBG);
  const int cx = 240, cy = 160;
  float sF = (tmNow.tm_sec + subsec) / 60.0f;
  float mF = (tmNow.tm_min + tmNow.tm_sec / 60.0f) / 60.0f;
  float hF = ((tmNow.tm_hour % 12) + tmNow.tm_min / 60.0f) / 12.0f;
  uint16_t c2 = tft.color565(THEMES[cfg.theme].r * 0.7f, THEMES[cfg.theme].g * 0.7f, THEMES[cfg.theme].b * 0.7f);
  uint16_t c3 = tft.color565(THEMES[cfg.theme].r * 0.45f, THEMES[cfg.theme].g * 0.45f, THEMES[cfg.theme].b * 0.45f);
  ringArc(cx, cy, 138, 150, cfg.showSec ? sF : 0, cACC);
  ringArc(cx, cy, 120, 132, mF, c2);
  ringArc(cx, cy, 102, 114, hF, c3 == cBG ? cACC : c3);
  char hm[8]; fmtHM(hm);
  txt(&fonts::Font7, 1.0f, middle_center, cTXT, cx, cy - 6, hm);
  txt(&fonts::FreeSansBold9pt7b, 1, middle_center, cACC, cx, cy - 52, DAYS[tmNow.tm_wday]);
  if (cfg.showSec) { char s[8]; snprintf(s, sizeof s, "%02d", tmNow.tm_sec); txt(&fonts::FreeSansBold12pt7b, 1, middle_center, cDIM, cx, cy + 40, s); }
  if (!cfg.h24) txt(&fonts::FreeSans9pt7b, 1, middle_center, cDIM, cx, cy + 62, tmNow.tm_hour >= 12 ? "PM" : "AM");
  drawSides();
}

// ---------------- Clock style 4: Split ----------------
void drawSplit() {
  cv.fillScreen(cBG);
  char h[4], m[4];
  int hh = cfg.h24 ? tmNow.tm_hour : hour12();
  snprintf(h, sizeof h, "%02d", hh);
  snprintf(m, sizeof m, "%02d", tmNow.tm_min);
  txt(&fonts::Font8, 1.25f, middle_center, cTXT, 125, 88, h);
  txt(&fonts::Font8, 1.25f, middle_center, cACC, 125, 222, m);
  cv.drawFastVLine(250, 20, 280, cTRK);
  txt(&fonts::FreeSansBold18pt7b, 1, middle_center, cACC, 365, 40, DAYS[tmNow.tm_wday]);
  char d[32]; snprintf(d, sizeof d, "%d %s %d", tmNow.tm_mday, MONTHS[tmNow.tm_mon], tmNow.tm_year + 1900);
  txt(&fonts::FreeSans12pt7b, 1, middle_center, cDIM, 365, 76, d);
  if (cfg.showSec) {
    char s[24]; snprintf(s, sizeof s, "%02d s%s", tmNow.tm_sec, cfg.h24 ? "" : (tmNow.tm_hour >= 12 ? "  PM" : "  AM"));
    txt(&fonts::FreeSansBold12pt7b, 1, middle_center, cTXT, 365, 108, s);
  }
  if (wx.valid) {
    drawWxIcon(305, 178, 40, wx.code, wx.isDay);
    drawTempC(410, 172, &fonts::FreeSansBold24pt7b, 1, cTXT);
    txt(&fonts::FreeSans12pt7b, 1, middle_center, cTXT, 365, 236, wxText(wx.code));
    char b[32]; snprintf(b, sizeof b, "H %d  L %d  |  %d%%", (int)lroundf(dispT(wx.hi)), (int)lroundf(dispT(wx.lo)), wx.hum);
    txt(&fonts::FreeSans9pt7b, 1, middle_center, cDIM, 365, 268, b);
    txt(&fonts::FreeSans9pt7b, 1, middle_center, cDIM, 365, 290, cfg.city.c_str());
  } else txt(&fonts::FreeSans12pt7b, 1, middle_center, cDIM, 365, 190, "Loading weather...");
  cv.fillRect(0, 316, (int)(480 * (tmNow.tm_sec + subsec) / 60.0f), 4, cACC);
}

// ---------------- Clock style 5: Dashboard ----------------
void drawDash() {
  cv.fillScreen(cBG);
  char hm[8]; fmtHM(hm);
  txt(&fonts::Font7, 1.5f, top_left, cACC, 16, 12, hm);
  int w = cv.textWidth(hm);
  if (cfg.showSec) { char s[8]; snprintf(s, sizeof s, ":%02d", tmNow.tm_sec); txt(&fonts::FreeSansBold18pt7b, 1, bottom_left, cDIM, 16 + w + 8, 80, s); }
  txt(&fonts::FreeSansBold18pt7b, 1, top_right, cTXT, 464, 10, DAYS[tmNow.tm_wday]);
  char d[32]; snprintf(d, sizeof d, "%d %s %d", tmNow.tm_mday, MONTHS3[tmNow.tm_mon], tmNow.tm_year + 1900);
  txt(&fonts::FreeSans12pt7b, 1, top_right, cDIM, 464, 44, d);
  if (!cfg.h24) txt(&fonts::FreeSansBold12pt7b, 1, top_right, cACC, 464, 74, tmNow.tm_hour >= 12 ? "PM" : "AM");
  float sod = tmNow.tm_hour * 3600.0f + tmNow.tm_min * 60.0f + tmNow.tm_sec;
  float dayF = sod / 86400.0f;
  float weekF = (tmNow.tm_wday * 86400.0f + sod) / (7 * 86400.0f);
  float yearF = (tmNow.tm_yday + dayF) / 365.0f;
  const char* L[] = {"Day", "Week", "Year"};
  float F[] = {dayF, weekF, yearF};
  for (int i = 0; i < 3; i++) {
    int y = 112 + i * 42;
    char p[8]; snprintf(p, sizeof p, "%d%%", (int)(F[i] * 100));
    txt(&fonts::FreeSans12pt7b, 1, middle_left, cDIM, 16, y + 10, L[i]);
    bar(90, y + 3, 290, 14, F[i], cACC);
    txt(&fonts::FreeSansBold12pt7b, 1, middle_right, cTXT, 464, y + 10, p);
  }
  drawWxStrip(250);
}

// ---------------- Weather screen ----------------
void drawWeatherScreen() {
  cv.fillScreen(cBG);
  txt(&fonts::FreeSansBold12pt7b, 1, top_left, cACC, 16, 12, cfg.city.c_str());
  char hm[8]; fmtHM(hm);
  txt(&fonts::FreeSansBold12pt7b, 1, top_right, cDIM, 464, 12, hm);
  if (!wx.valid) {
    txt(&fonts::FreeSansBold18pt7b, 1, middle_center, cDIM, 240, 160, "Fetching weather...");
    return;
  }
  drawWxIcon(105, 130, 78, wx.code, wx.isDay);
  char n[8]; snprintf(n, sizeof n, "%d", (int)lroundf(dispT(wx.temp)));
  cv.setFont(&fonts::Font8); cv.setTextSize(1.3f);
  int w = cv.textWidth(n);
  int sx = 250;
  txt(&fonts::Font8, 1.3f, middle_left, cTXT, sx, 112, n);
  int dx = sx + w + 14;
  cv.drawCircle(dx, 84, 7, cACC); cv.drawCircle(dx, 84, 6, cACC); cv.drawCircle(dx, 84, 5, cACC);
  txt(&fonts::FreeSansBold24pt7b, 1, middle_left, cACC, dx + 12, 90, cfg.celsius ? "C" : "F");
  txt(&fonts::FreeSansBold18pt7b, 1, middle_center, cTXT, 320, 185, wxText(wx.code));

  const char* lab[4] = {"Feels like", "Humidity", "Wind", "Today H / L"};
  char v[4][20];
  snprintf(v[0], 20, "%d %s", (int)lroundf(dispT(wx.feels)), cfg.celsius ? "C" : "F");
  snprintf(v[1], 20, "%d %%", wx.hum);
  snprintf(v[2], 20, "%d km/h", (int)lroundf(wx.wind));
  snprintf(v[3], 20, "%d / %d", (int)lroundf(dispT(wx.hi)), (int)lroundf(dispT(wx.lo)));
  for (int i = 0; i < 4; i++) {
    int x = 12 + i * 116;
    cv.fillRoundRect(x, 232, 108, 74, 12, cTRK);
    txt(&fonts::FreeSans9pt7b, 1, middle_center, cDIM, x + 54, 252, lab[i]);
    txt(&fonts::FreeSansBold12pt7b, 1, middle_center, cTXT, x + 54, 282, v[i]);
  }
  if (wx.updated) {
    struct tm u; localtime_r(&wx.updated, &u);
    char b[24]; snprintf(b, sizeof b, "Updated %02d:%02d", u.tm_hour, u.tm_min);
    txt(&fonts::FreeSans9pt7b, 1, bottom_center, cDIM, 240, 318, b);
  }
}

void drawWait(const char* l1, const char* l2) {
  cv.fillScreen(cBG);
  txt(&fonts::FreeSansBold18pt7b, 1, middle_center, cACC, 240, 130, l1);
  txt(&fonts::FreeSans12pt7b, 1, middle_center, cDIM, 240, 185, l2);
}

// ---------------- Web server ----------------
WebServer server(80);
bool apMode = false;

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>ESP32 Clock</title>
<style>
:root{color-scheme:dark}
body{font-family:system-ui,sans-serif;background:#0b0f17;color:#e8ecf3;margin:0 auto;padding:14px;max-width:560px}
h1{font-size:20px;margin:6px 0 2px}h3{margin:0 0 10px;font-size:13px;color:#8fa0b8;text-transform:uppercase;letter-spacing:.06em}
.card{background:#141a26;border-radius:14px;padding:14px;margin:12px 0}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
button,select,input{font:inherit;border-radius:10px;border:1px solid #2a3448;background:#1b2333;color:inherit;padding:9px 12px}
button{cursor:pointer}button.on{background:#19c3e6;color:#00121a;border-color:#19c3e6;font-weight:600}
input[type=range]{width:100%;padding:0}input[type=text],input[type=number],select{flex:1;min-width:110px}
#st{font-size:15px;line-height:1.6}.big{font-size:28px;font-weight:700}
.sm{color:#8fa0b8;font-size:12px}
</style></head><body>
<h1>ESP32 Clock &amp; Weather</h1><div class=sm id=ip></div>
<div class=card><h3>Live</h3><div id=st>loading...</div></div>
<div class=card><h3>Clock style</h3><div class=row id=style></div></div>
<div class=card><h3>Screen</h3><div class=row id=mode></div>
<div class=row style="margin-top:10px"><span class=sm>Auto switch every</span>
<select id=auto onchange="set('auto='+this.value)"><option>5</option><option>10</option><option>20</option><option>30</option><option>60</option></select><span class=sm>sec</span></div></div>
<div class=card><h3>Theme</h3><div class=row id=theme></div></div>
<div class=card><h3>Brightness</h3><input type=range id=bri min=5 max=255 onchange="set('bright='+this.value)"></div>
<div class=card><h3>Options</h3><div class=row id=tog></div></div>
<div class=card><h3>Location &amp; time zone</h3>
<div class=row><input type=text id=city placeholder=City><input type=number step=any id=lat placeholder=Lat><input type=number step=any id=lon placeholder=Lon></div>
<div class=row style="margin-top:8px"><select id=tz></select><button onclick=saveLoc()>Save</button></div>
<div class=row style="margin-top:8px"><span class=sm>Weather refresh</span><select id=wxm onchange="set('wxmins='+this.value)"><option>5</option><option>10</option><option>15</option><option>30</option><option>60</option></select><span class=sm>min</span>
<button onclick="fetch('/api/refresh').then(()=>setTimeout(poll,2500))">Refresh now</button></div></div>
<div class=card><h3>System</h3><div class=row><a href=/wifi><button>WiFi settings</button></a><button onclick="if(confirm('Reboot?'))fetch('/api/reboot')">Reboot</button></div></div>
<script>
const $=id=>document.getElementById(id);
const TC=['#19c3e6','#ffb020','#3ddc84','#e040fb','#ff4d4d','#ffffff'];
const TZ=[['Dhaka (UTC+6)','<+06>-6'],['Kolkata (UTC+5:30)','<+0530>-5:30'],['Dubai (UTC+4)','<+04>-4'],['Singapore (UTC+8)','<+08>-8'],['Tokyo (UTC+9)','JST-9'],['London','GMT0BST,M3.5.0/1,M10.5.0'],['New York','EST5EDT,M3.2.0,M11.1.0'],['UTC','UTC0']];
let S={};
async function set(q){const r=await fetch('/api/set?'+q);S=await r.json();ui()}
function btns(id,a,k,cur){$(id).innerHTML=a.map((n,i)=>`<button class="${i==cur?'on':''}" onclick="set('${k}=${i}')">${n}</button>`).join('')}
function ui(){
btns('style',['1 Digital','2 Analog','3 Rings','4 Split','5 Dash'],'style',S.style);
btns('mode',['Clock','Weather','Auto'],'mode',S.mode);
$('theme').innerHTML=TC.map((c,i)=>`<button style="background:${c};width:38px;height:38px;${i==S.theme?'outline:3px solid #fff':''}" onclick="set('theme=${i}')"></button>`).join('');
$('bri').value=S.bright;$('auto').value=S.auto;$('wxm').value=S.wxmins;
const T=[['h24','24-hour'],['sec','Seconds'],['cel','Celsius']];
$('tog').innerHTML=T.map(([k,n])=>`<button class="${S[k]?'on':''}" onclick="set('${k}=${S[k]?0:1}')">${n}: ${S[k]?'ON':'OFF'}</button>`).join('');
if(document.activeElement.id!='city'&&document.activeElement.id!='lat'&&document.activeElement.id!='lon'){$('city').value=S.city;$('lat').value=S.lat;$('lon').value=S.lon}
if(!$('tz').options.length){$('tz').innerHTML=TZ.map(z=>`<option value="${z[1].replace(/</g,'&lt;')}">${z[0]}</option>`).join('')}
$('tz').value=S.tz;
$('ip').textContent=S.ip+'  |  WiFi '+S.rssi+' dBm';
const w=S.wx;$('st').innerHTML='<span class=big>'+S.time+'</span><br>'+(w.ok?w.text+' &middot; '+w.temp+'&deg;C (feels '+w.feels+'&deg;) &middot; '+w.hum+'% &middot; '+w.wind+' km/h &middot; H '+w.hi+' / L '+w.lo:'Weather not loaded yet')}
function saveLoc(){set('city='+encodeURIComponent($('city').value)+'&lat='+$('lat').value+'&lon='+$('lon').value+'&tz='+encodeURIComponent($('tz').value))}
async function poll(){try{S=await (await fetch('/api/state')).json();ui()}catch(e){}}
poll();setInterval(poll,5000);
</script></body></html>)rawliteral";

String stateJson() {
  char t[16] = "--:--:--";
  if (timeOK) strftime(t, sizeof t, "%H:%M:%S", &tmNow);
  Weather w = wx;
  char b[800];
  snprintf(b, sizeof b,
    "{\"style\":%d,\"mode\":%d,\"bright\":%d,\"h24\":%d,\"sec\":%d,\"cel\":%d,\"theme\":%d,\"auto\":%d,\"wxmins\":%d,"
    "\"lat\":%.4f,\"lon\":%.4f,\"city\":\"%s\",\"tz\":\"%s\",\"time\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
    "\"wx\":{\"ok\":%d,\"temp\":%.1f,\"feels\":%.1f,\"hum\":%d,\"wind\":%.1f,\"code\":%d,\"text\":\"%s\",\"hi\":%.1f,\"lo\":%.1f}}",
    cfg.style, cfg.mode, cfg.bright, cfg.h24, cfg.showSec, cfg.celsius, cfg.theme, cfg.autoSecs, cfg.wxMins,
    cfg.lat, cfg.lon, cfg.city.c_str(), cfg.tz.c_str(), t,
    (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str(), WiFi.RSSI(),
    w.valid, w.temp, w.feels, w.hum, w.wind, w.code, wxText(w.code), w.hi, w.lo);
  return String(b);
}

void handleSet() {
  for (uint8_t i = 0; i < server.args(); i++) {
    String k = server.argName(i), v = server.arg(i);
    if (k == "style") cfg.style = constrain(v.toInt(), 0, 4);
    else if (k == "mode") cfg.mode = constrain(v.toInt(), 0, 2);
    else if (k == "bright") { cfg.bright = constrain(v.toInt(), 5, 255); tft.setBrightness(cfg.bright); }
    else if (k == "h24") cfg.h24 = v.toInt();
    else if (k == "sec") cfg.showSec = v.toInt();
    else if (k == "cel") cfg.celsius = v.toInt();
    else if (k == "theme") { cfg.theme = constrain(v.toInt(), 0, 5); applyTheme(); }
    else if (k == "auto") cfg.autoSecs = constrain(v.toInt(), 3, 300);
    else if (k == "wxmins") cfg.wxMins = constrain(v.toInt(), 1, 240);
    else if (k == "lat") { cfg.lat = v.toFloat(); wxForce = true; }
    else if (k == "lon") { cfg.lon = v.toFloat(); wxForce = true; }
    else if (k == "city") { v.replace("\"", ""); v.replace("\\", ""); cfg.city = v.substring(0, 24); }
    else if (k == "tz") { cfg.tz = v.substring(0, 48); applyTZ(); }
  }
  needRedraw = true;
  markDirty();
  server.send(200, "application/json", stateJson());
}

const char WIFI_HTML[] PROGMEM = R"rawliteral(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>WiFi</title>
<style>body{font-family:system-ui;background:#0b0f17;color:#e8ecf3;padding:20px;max-width:420px;margin:auto}
input,button{display:block;width:100%;box-sizing:border-box;margin:10px 0;padding:12px;border-radius:10px;border:1px solid #2a3448;background:#1b2333;color:#fff;font-size:16px}
button{background:#19c3e6;color:#00121a;font-weight:700}</style></head><body>
<h2>WiFi setup</h2><form method=POST action=/wifi><input name=ssid placeholder="WiFi name" required>
<input name=pass type=password placeholder="Password"><button>Save &amp; reboot</button></form></body></html>)rawliteral";

void handleWifiPost() {
  prefs.begin("clock", false);
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("pass", server.arg("pass"));
  prefs.end();
  server.send(200, "text/html", "<meta name=viewport content='width=device-width'><body style='font-family:system-ui;background:#0b0f17;color:#fff;padding:20px'>Saved. Rebooting...</body>");
  delay(800);
  ESP.restart();
}

// ---------------- Setup / loop ----------------
void bootMsg(const char* a, const char* b = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(middle_center);
  tft.setTextColor(0xFFFF);
  tft.setFont(&fonts::FreeSansBold18pt7b);
  tft.drawString(a, 240, 130);
  tft.setFont(&fonts::FreeSans12pt7b);
  tft.setTextColor(tft.color565(150, 160, 175));
  tft.drawString(b, 240, 185);
}

bool connectWiFi() {
  prefs.begin("clock", true);
  String ssid = prefs.getString("ssid", DEF_SSID);
  String pass = prefs.getString("pass", DEF_PASS);
  prefs.end();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);           // keeps the web panel snappy
  WiFi.setHostname("esp32clock");
  WiFi.begin(ssid.c_str(), pass.c_str());
  bootMsg("Connecting WiFi", ssid.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
  loadSettings();
  tft.init();
  tft.setRotation(1);             // 480x320 landscape (use 3 to flip 180 degrees)
  tft.setBrightness(cfg.bright);
  tft.fillScreen(TFT_BLACK);
  applyTheme();

  cv.setPsram(true);
  cv.setColorDepth(16);
  if (!cv.createSprite(480, 320)) {           // no PSRAM? fall back to 8-bit internal RAM
    cv.setPsram(false);
    cv.setColorDepth(8);
    cv.createSprite(480, 320);
  }

  if (connectWiFi()) {
    Serial.printf("Connected: http://%s\n", WiFi.localIP().toString().c_str());
    configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    MDNS.begin("esp32clock");
    server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
    xTaskCreatePinnedToCore(weatherTask, "wx", 10240, nullptr, 1, nullptr, 0);
    bootMsg("Connected", WiFi.localIP().toString().c_str());
    delay(1500);
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-Clock", "12345678");
    server.on("/", []() { server.send_P(200, "text/html", WIFI_HTML); });
    bootMsg("WiFi failed", "Join 'ESP32-Clock' -> 192.168.4.1");
  }
  server.on("/wifi", HTTP_GET, []() { server.send_P(200, "text/html", WIFI_HTML); });
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/state", []() { server.send(200, "application/json", stateJson()); });
  server.on("/api/set", handleSet);
  server.on("/api/refresh", []() { wxForce = true; server.send(200, "text/plain", "ok"); });
  server.on("/api/reboot", []() { server.send(200, "text/plain", "ok"); delay(300); ESP.restart(); });
  server.begin();
}

void render() {
  bool showWx = (cfg.mode == 1) || (cfg.mode == 2 && wx.valid && ((millis() / 1000 / cfg.autoSecs) & 1));
  if (apMode) drawWait("Setup mode", "Join 'ESP32-Clock'  ->  192.168.4.1");
  else if (!timeOK) drawWait("Syncing time...", WiFi.localIP().toString().c_str());
  else if (showWx) drawWeatherScreen();
  else switch (cfg.style) {
    case 0: drawDigital(); break;
    case 1: drawAnalog();  break;
    case 2: drawRings();   break;
    case 3: drawSplit();   break;
    default: drawDash();   break;
  }
  cv.pushSprite(0, 0);
}

void loop() {
  server.handleClient();
  if (prefsDirty && millis() - prefsDirtyAt > 1500) saveSettings();

  static uint32_t lastFrame = 0;
  bool changed = updateTime();
  bool showWx = (cfg.mode == 1) || (cfg.mode == 2 && wx.valid && ((millis() / 1000 / cfg.autoSecs) & 1));
  bool smooth = timeOK && !showWx && cfg.showSec && (cfg.style == 1 || cfg.style == 2 || cfg.style == 3 || cfg.style == 0 || cfg.style == 4);
  // analog/rings/split/digital have sweeping bars or hands -> ~10 fps; dash only needs 1 fps
  if (cfg.style == 4) smooth = false;
  if (needRedraw || changed || (!timeOK && millis() - lastFrame > 500) || (smooth && millis() - lastFrame >= 90)) {
    needRedraw = false;
    render();
    lastFrame = millis();
  }
  delay(2);
}
