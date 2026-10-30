/*
 * SmallTV Ultra - Orologio + Meteo
 * Hardware: ESP8266 (4 MB flash) + TFT ST7789 240x240
 *
 *  - Portale Wi-Fi (captive portal) al primo avvio o se la rete non si collega
 *  - Ora automatica via NTP, fuso Europe/Rome con ora legale
 *  - Meteo da Open-Meteo (gratuito, nessuna chiave API)
 *  - Interfaccia web di configurazione: http://smalltv.local  (oppure l'IP mostrato allo schermo)
 *  - Aggiornamento firmware da browser: /update
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "webui.h"

#define FW_VERSION  "1.0.0"
#define HOSTNAME    "smalltv"
#define AP_SSID     "SmallTV-Setup"
#define AP_PASS     "smalltv1234"
#define TZ_ROME     "CET-1CEST,M3.5.0,M10.5.0/3"

#ifndef BL_PIN
#define BL_PIN 5
#endif
#ifndef BL_ACTIVE_LOW
#define BL_ACTIVE_LOW 1
#endif

// ------------------------------------------------------------------
//  Layout (schermo 240x240)
// ------------------------------------------------------------------
static const int HEADER_H = 28;
static const int TIME_Y   = 28;    // top del testo dell'ora (font 8, alto 75 px)
static const int BAR_Y    = 107;   // barra dei secondi
static const int CARD_X   = 6;
static const int CARD_Y   = 116;
static const int CARD_W   = 228;
static const int CARD_H   = 122;

// ------------------------------------------------------------------
//  Oggetti globali
// ------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
DNSServer dns;

// ------------------------------------------------------------------
//  Configurazione persistente
// ------------------------------------------------------------------
struct Config {
  char ssid[33];
  char pass[65];
  char city[24];
  float lat;
  float lon;
  uint8_t theme;
  uint8_t bright;     // 5..100
  uint8_t wxMinutes;  // intervallo aggiornamento meteo
  bool flip;          // ruota 180 gradi
  bool invert;        // inverti colori pannello
  bool swapRB;        // scambia rosso/blu
};
Config cfg;

void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.city, "ROMA", sizeof(cfg.city));
  cfg.lat = 41.8919f;
  cfg.lon = 12.5113f;
  cfg.theme = 0;
  cfg.bright = 80;
  cfg.wxMinutes = 15;
  cfg.flip = false;
  cfg.invert = true;
  cfg.swapRB = false;
}

// copia solo caratteri ASCII stampabili (i font del display sono ASCII)
void asciiCopy(char* dst, size_t n, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src && src[i] && j < n - 1; i++) {
    uint8_t c = (uint8_t)src[i];
    if (c >= 32 && c < 127) dst[j++] = (char)c;
  }
  dst[j] = 0;
}

// ------------------------------------------------------------------
//  Temi (0xRRGGBB)
// ------------------------------------------------------------------
struct Theme {
  uint32_t bg, card, a1, a2, text, dim, band;
};
const Theme THEMES[] = {
  // bg        card      a1        a2        text      dim       band
  {0x05060F, 0x0E1230, 0x00E5FF, 0xFF2E93, 0xFFFFFF, 0x6B7399, 0x2A1470},  // Neon
  {0x030F0E, 0x0A2422, 0x2DFFB2, 0x7C5CFF, 0xEFFFFA, 0x5F8F86, 0x0B4A48},  // Aurora
  {0x120610, 0x2A0F26, 0xFF9F1C, 0xFF3D81, 0xFFF4E6, 0x9C6B7E, 0x6A1A4A},  // Tramonto
  {0x06101C, 0x0F2338, 0x7FD1FF, 0xA78BFA, 0xF2F8FF, 0x6F8CAB, 0x14487A},  // Ghiaccio
  {0x000000, 0x14100A, 0xFFB000, 0xFF6A00, 0xFFE9B8, 0x7A5C1E, 0x4A3400},  // Ambra
};
const uint8_t THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
const Theme* th = &THEMES[0];

// ------------------------------------------------------------------
//  Colori
// ------------------------------------------------------------------
uint16_t col(uint32_t rgb) {
  uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  return cfg.swapRB ? tft.color565(b, g, r) : tft.color565(r, g, b);
}

// t=0 -> a, t=255 -> b
uint32_t mixRGB(uint32_t a, uint32_t b, uint8_t t) {
  int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  int r = (ar * (255 - t) + br * t) / 255;
  int g = (ag * (255 - t) + bg * t) / 255;
  int bl = (ab * (255 - t) + bb * t) / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

// ------------------------------------------------------------------
//  Stato
// ------------------------------------------------------------------
bool apMode = false;
bool needRedraw = false;
bool wxDirty = true;
bool pendingReboot = false;
uint32_t rebootAt = 0;
uint32_t apStart = 0;
uint32_t apLastClient = 0;
uint32_t nextWx = 0;

struct Weather {
  bool valid;
  float temp;
  int hum;
  int code;
  bool day;
  float tmin;
  float tmax;
  uint32_t fetchedAt;
};
Weather wx = {false, 0, 0, 0, true, 0, 0, 0};

uint8_t timeFont = 8;
int timeY = TIME_Y;

time_t lastNow = 0;
int lastDigitsKey = -1;
int lastHdrKey = -1000;
int lastSec = -1;
uint32_t lastDot = 0;

const char* const DAYS[] = {"DOM", "LUN", "MAR", "MER", "GIO", "VEN", "SAB"};
const char* const MONTHS[] = {"GEN", "FEB", "MAR", "APR", "MAG", "GIU", "LUG", "AGO", "SET", "OTT", "NOV", "DIC"};

// ------------------------------------------------------------------
//  File di configurazione
// ------------------------------------------------------------------
bool loadConfig() {
  cfgDefaults();
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;
  StaticJsonDocument<768> d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return false;

  strlcpy(cfg.ssid, d["ssid"] | "", sizeof(cfg.ssid));
  strlcpy(cfg.pass, d["pass"] | "", sizeof(cfg.pass));
  asciiCopy(cfg.city, sizeof(cfg.city), d["city"] | "ROMA");
  if (cfg.city[0] == 0) strlcpy(cfg.city, "ROMA", sizeof(cfg.city));
  cfg.lat = d["lat"] | 41.8919f;
  cfg.lon = d["lon"] | 12.5113f;
  int v = d["theme"] | 0;
  cfg.theme = constrain(v, 0, (int)THEME_COUNT - 1);
  v = d["bright"] | 80;
  cfg.bright = constrain(v, 5, 100);
  v = d["wx"] | 15;
  cfg.wxMinutes = constrain(v, 5, 120);
  cfg.flip = d["flip"] | false;
  cfg.invert = d["invert"] | true;
  cfg.swapRB = d["swap"] | false;
  return true;
}

bool saveConfig() {
  StaticJsonDocument<768> d;
  d["ssid"] = cfg.ssid;
  d["pass"] = cfg.pass;
  d["city"] = cfg.city;
  d["lat"] = cfg.lat;
  d["lon"] = cfg.lon;
  d["theme"] = cfg.theme;
  d["bright"] = cfg.bright;
  d["wx"] = cfg.wxMinutes;
  d["flip"] = cfg.flip;
  d["invert"] = cfg.invert;
  d["swap"] = cfg.swapRB;
  File f = LittleFS.open("/config.json", "w");
  if (!f) return false;
  serializeJson(d, f);
  f.close();
  return true;
}

// ------------------------------------------------------------------
//  Retroilluminazione
// ------------------------------------------------------------------
void setBacklight(uint8_t pct) {
  int p = constrain((int)pct, 0, 100);
  int duty = map(p, 0, 100, 0, 255);
#if BL_ACTIVE_LOW
  duty = 255 - duty;
#endif
  analogWrite(BL_PIN, duty);
}

// ------------------------------------------------------------------
//  Icone meteo (disegnate con primitive, nessuna immagine)
// ------------------------------------------------------------------
enum Icon { IC_SUN, IC_MOON, IC_PARTLY, IC_PARTLY_NIGHT, IC_CLOUD, IC_FOG, IC_RAIN, IC_SNOW, IC_STORM };

Icon iconFor(int code, bool day) {
  if (code == 0 || code == 1) return day ? IC_SUN : IC_MOON;
  if (code == 2) return day ? IC_PARTLY : IC_PARTLY_NIGHT;
  if (code == 3) return IC_CLOUD;
  if (code == 45 || code == 48) return IC_FOG;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return IC_RAIN;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return IC_SNOW;
  if (code >= 95) return IC_STORM;
  return IC_CLOUD;
}

const char* descFor(int c) {
  switch (c) {
    case 0: return "Sereno";
    case 1: return "Quasi sereno";
    case 2: return "Parz. nuvoloso";
    case 3: return "Coperto";
    case 45: case 48: return "Nebbia";
    case 51: case 53: case 55: return "Pioviggine";
    case 56: case 57: return "Pioggia gelata";
    case 61: return "Pioggia debole";
    case 63: return "Pioggia";
    case 65: return "Pioggia forte";
    case 66: case 67: return "Pioggia gelata";
    case 71: return "Neve debole";
    case 73: return "Neve";
    case 75: return "Neve forte";
    case 77: return "Nevischio";
    case 80: case 81: return "Rovesci";
    case 82: return "Rovesci forti";
    case 85: case 86: return "Neve a rovesci";
    case 95: return "Temporale";
    case 96: case 99: return "Grandine";
    default: return "--";
  }
}

void drawSunShape(int cx, int cy, int r, uint16_t c) {
  tft.fillCircle(cx, cy, r, c);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4.0f;
    float ca = cosf(a), sa = sinf(a);
    int x1 = cx + (int)(ca * (r + 4)), y1 = cy + (int)(sa * (r + 4));
    int x2 = cx + (int)(ca * (r + 10)), y2 = cy + (int)(sa * (r + 10));
    tft.drawLine(x1, y1, x2, y2, c);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, c);
    tft.drawLine(x1, y1 + 1, x2, y2 + 1, c);
  }
}

void drawMoonShape(int cx, int cy, int r, uint16_t c, uint16_t bgc) {
  tft.fillCircle(cx, cy, r, c);
  tft.fillCircle(cx + r / 2 + 1, cy - r / 3, r - 2, bgc);
}

void drawCloudShape(int cx, int cy, uint16_t c) {
  tft.fillRoundRect(cx - 22, cy + 1, 46, 15, 7, c);
  tft.fillCircle(cx - 7, cy - 2, 10, c);
  tft.fillCircle(cx + 7, cy - 6, 13, c);
}

void drawWeatherIcon(Icon ic, int cx, int cy, uint16_t cardc) {
  const uint16_t sun = col(0xFFC531), moon = col(0xE8EEFF), cl = col(0xD3DEF0), dk = col(0x93A5BF);
  const uint16_t rn = col(0x4FB8FF), sn = col(0xFFFFFF), bolt = col(0xFFE14A), fog = col(0xB5C3D8);
  tft.fillRect(10, 128, 74, 62, cardc);
  switch (ic) {
    case IC_SUN:
      drawSunShape(cx, cy, 13, sun);
      break;
    case IC_MOON:
      drawMoonShape(cx - 2, cy, 17, moon, cardc);
      tft.drawFastHLine(cx + 14, cy - 14, 5, moon);
      tft.drawFastVLine(cx + 16, cy - 16, 5, moon);
      tft.fillCircle(cx + 18, cy + 8, 1, moon);
      break;
    case IC_PARTLY:
      drawSunShape(cx + 8, cy - 9, 9, sun);
      drawCloudShape(cx - 4, cy + 6, cl);
      break;
    case IC_PARTLY_NIGHT:
      drawMoonShape(cx + 8, cy - 9, 12, moon, cardc);
      drawCloudShape(cx - 4, cy + 6, cl);
      break;
    case IC_CLOUD:
      drawCloudShape(cx, cy, cl);
      break;
    case IC_FOG:
      drawCloudShape(cx, cy - 10, cl);
      tft.fillRoundRect(cx - 22, cy + 8, 44, 4, 2, fog);
      tft.fillRoundRect(cx - 16, cy + 15, 40, 4, 2, fog);
      tft.fillRoundRect(cx - 22, cy + 22, 34, 4, 2, fog);
      break;
    case IC_RAIN:
      drawCloudShape(cx, cy - 8, dk);
      for (int i = 0; i < 4; i++) {
        int x = cx - 14 + i * 10;
        int y = cy + 14 + ((i & 1) ? 5 : 0);
        tft.drawLine(x, y, x - 3, y + 8, rn);
        tft.drawLine(x + 1, y, x - 2, y + 8, rn);
      }
      break;
    case IC_SNOW:
      drawCloudShape(cx, cy - 8, dk);
      tft.fillCircle(cx - 14, cy + 15, 2, sn);
      tft.fillCircle(cx - 4, cy + 23, 2, sn);
      tft.fillCircle(cx + 6, cy + 15, 2, sn);
      tft.fillCircle(cx + 16, cy + 23, 2, sn);
      break;
    case IC_STORM:
      drawCloudShape(cx, cy - 10, dk);
      tft.fillTriangle(cx + 5, cy + 5, cx - 6, cy + 17, cx + 2, cy + 17, bolt);
      tft.fillTriangle(cx - 1, cy + 13, cx + 9, cy + 13, cx - 4, cy + 26, bolt);
      break;
  }
}

// ------------------------------------------------------------------
//  Disegno schermate
// ------------------------------------------------------------------
int wifiLevel() {
  if (apMode || WiFi.status() != WL_CONNECTED) return 0;
  int r = WiFi.RSSI();
  if (r > -55) return 4;
  if (r > -65) return 3;
  if (r > -75) return 2;
  return 1;
}

void drawBand() {
  for (int y = 0; y < HEADER_H - 1; y++) {
    uint8_t t = (uint8_t)(y * 255 / (HEADER_H - 2));
    tft.drawFastHLine(0, y, 240, col(mixRGB(th->band, th->bg, t)));
  }
  tft.drawFastHLine(0, HEADER_H - 1, 240, col(mixRGB(th->bg, th->a1, 110)));
}

void drawHeader(const struct tm* ti, bool synced) {
  drawBand();
  tft.setTextDatum(TL_DATUM);
  if (synced && ti) {
    tft.setTextColor(col(th->a1));
    tft.drawString(DAYS[ti->tm_wday], 10, 1, 4);
    int w = tft.textWidth(DAYS[ti->tm_wday], 4);
    char buf[16];
    snprintf(buf, sizeof(buf), " %d %s", ti->tm_mday, MONTHS[ti->tm_mon]);
    tft.setTextColor(col(th->text));
    tft.drawString(buf, 10 + w, 1, 4);
  } else {
    tft.setTextColor(col(th->dim));
    tft.drawString("Sincronizzo ora...", 10, 6, 2);
  }
  int lvl = wifiLevel();
  for (int i = 0; i < 4; i++) {
    int h = 5 + i * 4;
    int x = 204 + i * 7;
    uint32_t c = (i < lvl) ? th->text : mixRGB(th->band, th->dim, 120);
    tft.fillRect(x, 22 - h, 4, h, col(c));
  }
}

void initTimeFont() {
  int wD = tft.textWidth("00", 8);
  int wC = tft.textWidth(":", 8);
  if (wD > 0 && (2 * wD + wC) <= 228) {
    timeFont = 8;
    timeY = TIME_Y;
  } else {  // il font 8 non entra: ripiego sul 7 segmenti
    timeFont = 7;
    timeY = TIME_Y + 14;
  }
}

void timeGeom(int& x0, int& wD, int& wC) {
  wD = tft.textWidth("00", timeFont);
  wC = tft.textWidth(":", timeFont);
  x0 = (240 - (2 * wD + wC)) / 2;
}

void drawTimeDigits(int hh, int mm, bool synced) {
  int x0, wD, wC;
  timeGeom(x0, wD, wC);
  uint16_t bg = col(th->bg);
  char b[4];
  tft.setTextDatum(TL_DATUM);
  snprintf(b, sizeof(b), "%02d", hh);
  tft.setTextColor(col(synced ? th->text : th->dim), bg);
  tft.drawString(b, x0, timeY, timeFont);
  snprintf(b, sizeof(b), "%02d", mm);
  tft.setTextColor(col(synced ? th->a1 : th->dim), bg);
  tft.drawString(b, x0 + wD + wC, timeY, timeFont);
}

void drawColon(bool on) {
  int x0, wD, wC;
  timeGeom(x0, wD, wC);
  uint16_t bg = col(th->bg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(on ? col(th->a2) : bg, bg);
  tft.drawString(":", x0 + wD, timeY, timeFont);
}

void drawSecBar(int sec) {
  int w = (sec + 1) * 224 / 60;
  tft.fillRect(8, BAR_Y, w, 4, col(th->a1));
  tft.fillRect(8 + w, BAR_Y, 224 - w, 4, col(mixRGB(th->bg, th->dim, 70)));
  if (w > 6) tft.fillRect(8 + w - 6, BAR_Y, 6, 4, col(th->a2));
}

uint32_t dotColor() {
  if (!wx.valid) return 0xFF4D4D;
  uint32_t age = millis() - wx.fetchedAt;
  uint32_t limit = (uint32_t)cfg.wxMinutes * 60000UL * 2UL + 60000UL;
  return (age > limit) ? 0xFFB020 : 0x3DDC84;
}

void drawCard() {
  uint16_t cbg = col(th->card);
  tft.setTextDatum(TL_DATUM);

  // titolo: citta' + pallino di stato
  tft.fillRect(CARD_X + 14, CARD_Y + 4, CARD_W - 28, 18, cbg);
  char city[24];
  strlcpy(city, cfg.city, sizeof(city));
  for (char* p = city; *p; p++) *p = toupper((unsigned char)*p);
  tft.setTextColor(col(th->a2), cbg);
  tft.drawString(city, CARD_X + 14, CARD_Y + 5, 2);
  lastDot = dotColor();
  tft.fillCircle(CARD_X + CARD_W - 18, CARD_Y + 14, 4, col(lastDot));

  // icona
  Icon ic = wx.valid ? iconFor(wx.code, wx.day) : IC_CLOUD;
  drawWeatherIcon(ic, 46, 160, cbg);

  // temperatura
  const int tempY = CARD_Y + 21;
  tft.fillRect(88, tempY - 2, 138, 50, cbg);
  char tb[8];
  if (wx.valid) snprintf(tb, sizeof(tb), "%d", (int)lroundf(wx.temp));
  else strlcpy(tb, "--", sizeof(tb));
  int tw = tft.textWidth(tb, 6);
  int blockW = tw + 8 + 8 + 16;
  int x0 = 88 + (138 - blockW) / 2;
  tft.setTextColor(col(th->text), cbg);
  tft.drawString(tb, x0, tempY, 6);
  tft.drawCircle(x0 + tw + 9, tempY + 8, 5, col(th->a1));
  tft.drawCircle(x0 + tw + 9, tempY + 8, 4, col(th->a1));
  tft.setTextColor(col(th->a1), cbg);
  tft.drawString("C", x0 + tw + 18, tempY + 3, 4);

  // descrizione
  tft.fillRect(CARD_X + 6, 188, CARD_W - 12, 26, cbg);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col(th->text), cbg);
  tft.drawString(wx.valid ? descFor(wx.code) : "Carico meteo...", CARD_X + CARD_W / 2, 188, 4);

  // riga min / max / umidita'
  tft.fillRect(CARD_X + 16, 218, CARD_W - 32, 16, cbg);
  if (wx.valid) {
    char s[16];
    tft.setTextColor(col(0x5CC8FF), cbg);
    snprintf(s, sizeof(s), "MIN %d", (int)lroundf(wx.tmin));
    tft.drawString(s, 44, 218, 2);
    tft.setTextColor(col(0xFF8A5C), cbg);
    snprintf(s, sizeof(s), "MAX %d", (int)lroundf(wx.tmax));
    tft.drawString(s, 120, 218, 2);
    tft.setTextColor(col(th->a1), cbg);
    snprintf(s, sizeof(s), "UM %d%%", wx.hum);
    tft.drawString(s, 196, 218, 2);
    uint16_t sep = col(mixRGB(th->card, th->dim, 120));
    tft.drawFastVLine(82, 220, 12, sep);
    tft.drawFastVLine(158, 220, 12, sep);
  }
  tft.setTextDatum(TL_DATUM);
}

void fullRedraw() {
  th = &THEMES[cfg.theme];
  tft.setRotation(cfg.flip ? 2 : 0);
  tft.invertDisplay(cfg.invert);
  tft.fillScreen(col(th->bg));
  tft.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 14, col(th->card));
  tft.drawRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 14, col(mixRGB(th->card, th->a1, 100)));
  lastDigitsKey = -1;
  lastHdrKey = -1000;
  lastSec = -1;
  lastNow = 0;
  wxDirty = true;
}

void drawSplash(const char* msg) {
  static bool first = true;
  uint16_t bg = col(th->bg);
  if (first) {
    first = false;
    tft.fillScreen(bg);
    tft.setTextSize(2);
    tft.setTextDatum(TL_DATUM);
    int w1 = tft.textWidth("SMALL", 4);
    int w2 = tft.textWidth("TV", 4);
    int gap = 12;
    int x0 = (240 - (w1 + gap + w2)) / 2;
    tft.setTextColor(col(th->a1));
    tft.drawString("SMALL", x0, 62, 4);
    tft.setTextColor(col(th->a2));
    tft.drawString("TV", x0 + w1 + gap, 62, 4);
    tft.setTextSize(1);
    for (int i = 0; i < 40; i++) {
      tft.fillRect(40 + i * 4, 128, 4, 3, col(mixRGB(th->a1, th->a2, (uint8_t)(i * 255 / 39))));
    }
    tft.setTextColor(col(th->dim));
    tft.setTextDatum(TC_DATUM);
    tft.drawString("U L T R A", 120, 142, 2);
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextPadding(220);
  tft.setTextColor(col(th->text), bg);
  tft.drawString(msg, 120, 200, 2);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

void drawApScreen() {
  uint16_t bg = col(th->bg);
  tft.fillScreen(bg);
  drawBand();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col(th->text));
  tft.drawString("CONFIGURA WI-FI", 120, 1, 4);

  tft.setTextColor(col(th->dim));
  tft.drawString("1. Collegati alla rete Wi-Fi", 120, 42, 2);
  tft.setTextColor(col(th->a2));
  tft.drawString(AP_SSID, 120, 62, 4);
  tft.setTextColor(col(th->dim));
  tft.drawString("password", 120, 96, 2);
  tft.setTextColor(col(th->text));
  tft.drawString(AP_PASS, 120, 112, 4);

  tft.setTextColor(col(th->dim));
  tft.drawString("2. Si apre la pagina di setup", 120, 154, 2);
  tft.drawString("(altrimenti vai su)", 120, 174, 2);
  tft.setTextColor(col(th->a1));
  tft.drawString("192.168.4.1", 120, 196, 4);
  tft.setTextDatum(TL_DATUM);
}

void drawIpScreen(const char* ip) {
  uint16_t bg = col(th->bg);
  tft.fillScreen(bg);
  drawBand();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(col(th->text));
  tft.drawString("CONNESSO", 120, 1, 4);
  tft.setTextColor(col(th->dim));
  tft.drawString("Personalizza da browser:", 120, 70, 2);
  tft.setTextColor(col(th->a1));
  tft.drawString("smalltv.local", 120, 96, 4);
  tft.setTextColor(col(th->dim));
  tft.drawString("oppure", 120, 136, 2);
  tft.setTextColor(col(th->a2));
  tft.drawString(ip, 120, 156, 4);
  tft.setTextDatum(TL_DATUM);
}

// ------------------------------------------------------------------
//  Orologio
// ------------------------------------------------------------------
void updateClock() {
  time_t now = time(nullptr);
  if (now == lastNow) return;
  lastNow = now;
  bool synced = now > 1700000000;
  struct tm ti;
  localtime_r(&now, &ti);

  int lvl = wifiLevel();
  int hdrKey = synced ? ((ti.tm_mon * 32 + ti.tm_mday) * 8 + lvl) : (-2 - lvl);
  if (hdrKey != lastHdrKey) {
    drawHeader(&ti, synced);
    lastHdrKey = hdrKey;
  }

  int digitsKey = synced ? (ti.tm_hour * 60 + ti.tm_min) : -5;
  if (digitsKey != lastDigitsKey) {
    drawTimeDigits(synced ? ti.tm_hour : 0, synced ? ti.tm_min : 0, synced);
    lastDigitsKey = digitsKey;
  }

  drawColon((ti.tm_sec & 1) == 0);
  int sec = synced ? ti.tm_sec : 0;
  if (sec != lastSec) {
    drawSecBar(sec);
    lastSec = sec;
  }

  if (dotColor() != lastDot) wxDirty = true;
}

// ------------------------------------------------------------------
//  Meteo (Open-Meteo)
// ------------------------------------------------------------------
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  bool mfln = client.probeMaxFragmentLength("api.open-meteo.com", 443, 1024);
  if (mfln) {
    client.setBufferSizes(1024, 1024);
  } else if (ESP.getFreeHeap() < 26000) {
    Serial.println("Meteo: RAM insufficiente per TLS");
    return false;
  }

  char url[330];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,is_day"
           "&daily=temperature_2m_max,temperature_2m_min&timezone=Europe%%2FRome&forecast_days=1",
           cfg.lat, cfg.lon);

  HTTPClient http;
  http.setTimeout(8000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Meteo: HTTP %d\n", code);
    http.end();
    return false;
  }

  StaticJsonDocument<256> filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["relative_humidity_2m"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["is_day"] = true;
  filter["daily"]["temperature_2m_max"][0] = true;
  filter["daily"]["temperature_2m_min"][0] = true;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("Meteo: JSON %s\n", err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;
  wx.temp = cur["temperature_2m"] | 0.0f;
  wx.hum = cur["relative_humidity_2m"] | 0;
  wx.code = cur["weather_code"] | 0;
  wx.day = (cur["is_day"] | 1) != 0;
  wx.tmax = doc["daily"]["temperature_2m_max"][0] | wx.temp;
  wx.tmin = doc["daily"]["temperature_2m_min"][0] | wx.temp;
  wx.fetchedAt = millis();
  wx.valid = true;
  wxDirty = true;
  Serial.printf("Meteo OK: %.1f C codice %d\n", wx.temp, wx.code);
  return true;
}

// ------------------------------------------------------------------
//  Web server
// ------------------------------------------------------------------
void noCache() { server.sendHeader("Cache-Control", "no-store"); }

void sendJsonDoc(const JsonDocument& d) {
  String out;
  serializeJson(d, out);
  noCache();
  server.send(200, "application/json", out);
}

void handleConfigGet() {
  StaticJsonDocument<384> d;
  d["city"] = cfg.city;
  d["lat"] = cfg.lat;
  d["lon"] = cfg.lon;
  d["theme"] = cfg.theme;
  d["bright"] = cfg.bright;
  d["wx"] = cfg.wxMinutes;
  d["flip"] = cfg.flip;
  d["invert"] = cfg.invert;
  d["swap"] = cfg.swapRB;
  d["ssid"] = cfg.ssid;
  sendJsonDoc(d);
}

void handleConfigPost() {
  StaticJsonDocument<512> d;
  if (deserializeJson(d, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  uint8_t oldTheme = cfg.theme;
  bool oldFlip = cfg.flip, oldInv = cfg.invert, oldSwap = cfg.swapRB;
  float oldLat = cfg.lat, oldLon = cfg.lon;

  if (d.containsKey("city")) {
    asciiCopy(cfg.city, sizeof(cfg.city), d["city"] | "ROMA");
    if (cfg.city[0] == 0) strlcpy(cfg.city, "ROMA", sizeof(cfg.city));
  }
  if (d.containsKey("lat")) cfg.lat = constrain(d["lat"].as<float>(), -90.0f, 90.0f);
  if (d.containsKey("lon")) cfg.lon = constrain(d["lon"].as<float>(), -180.0f, 180.0f);
  if (d.containsKey("theme")) cfg.theme = constrain(d["theme"].as<int>(), 0, (int)THEME_COUNT - 1);
  if (d.containsKey("bright")) cfg.bright = constrain(d["bright"].as<int>(), 5, 100);
  if (d.containsKey("wx")) cfg.wxMinutes = constrain(d["wx"].as<int>(), 5, 120);
  if (d.containsKey("flip")) cfg.flip = d["flip"].as<bool>();
  if (d.containsKey("invert")) cfg.invert = d["invert"].as<bool>();
  if (d.containsKey("swap")) cfg.swapRB = d["swap"].as<bool>();

  saveConfig();
  setBacklight(cfg.bright);

  if (cfg.theme != oldTheme || cfg.flip != oldFlip || cfg.invert != oldInv || cfg.swapRB != oldSwap) needRedraw = true;
  else wxDirty = true;
  if (fabsf(cfg.lat - oldLat) > 0.0001f || fabsf(cfg.lon - oldLon) > 0.0001f) {
    wx.valid = false;
    nextWx = millis();
  }
  noCache();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus() {
  StaticJsonDocument<512> d;
  time_t now = time(nullptr);
  bool synced = now > 1700000000;
  struct tm ti;
  localtime_r(&now, &ti);
  char tb[12], db[12];
  strftime(tb, sizeof(tb), "%H:%M:%S", &ti);
  strftime(db, sizeof(db), "%Y-%m-%d", &ti);
  d["fw"] = FW_VERSION;
  d["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["rssi"] = WiFi.RSSI();
  d["heap"] = ESP.getFreeHeap();
  d["up"] = millis() / 1000;
  d["synced"] = synced;
  d["time"] = tb;
  d["date"] = db;
  JsonObject w = d.createNestedObject("wx");
  w["ok"] = wx.valid;
  if (wx.valid) {
    w["t"] = wx.temp;
    w["hum"] = wx.hum;
    w["code"] = wx.code;
    w["min"] = wx.tmin;
    w["max"] = wx.tmax;
  }
  sendJsonDoc(d);
}

void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true, true);
    noCache();
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    noCache();
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  DynamicJsonDocument d(2048);
  JsonArray arr = d.createNestedArray("networks");
  int count = 0;
  for (int i = 0; i < n && count < 14; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (JsonObject o : arr) {
      if (s == o["ssid"].as<const char*>()) { dup = true; break; }
    }
    if (dup) continue;
    JsonObject o = arr.createNestedObject();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["enc"] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
    count++;
  }
  WiFi.scanDelete();
  sendJsonDoc(d);
}

void scheduleReboot(uint32_t ms) {
  pendingReboot = true;
  rebootAt = millis() + ms;
}

void handleWifiSave() {
  StaticJsonDocument<256> d;
  if (deserializeJson(d, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  const char* ssid = d["ssid"] | "";
  if (strlen(ssid) == 0 || strlen(ssid) > 32) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
  strlcpy(cfg.pass, d["pass"] | "", sizeof(cfg.pass));
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
  scheduleReboot(1500);
}

void handleNotFound() {
  if (apMode) {  // captive portal: tutto cio' che non conosciamo porta alla pagina di setup
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Non trovato");
  }
}

void setupWebRoutes() {
  server.on("/", HTTP_GET, []() {
    noCache();
    server.send_P(200, "text/html", apMode ? WIFI_HTML : INDEX_HTML);
  });
  server.on("/wifi", HTTP_GET, []() {
    noCache();
    server.send_P(200, "text/html", WIFI_HTML);
  });
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/refresh", HTTP_POST, []() {
    nextWx = millis();
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/reboot", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    scheduleReboot(800);
  });
  server.on("/api/forget", HTTP_POST, []() {
    cfg.ssid[0] = 0;
    cfg.pass[0] = 0;
    saveConfig();
    server.send(200, "application/json", "{\"ok\":true}");
    scheduleReboot(800);
  });
  server.onNotFound(handleNotFound);
  if (!apMode) httpUpdater.setup(&server, "/update");
  server.begin();
}

// ------------------------------------------------------------------
//  Wi-Fi
// ------------------------------------------------------------------
void startAP() {
  apMode = true;
  apStart = millis();
  apLastClient = millis();
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  IPAddress ip(192, 168, 4, 1);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", ip);
  WiFi.scanNetworks(true, true);  // scansione in anticipo: la pagina trova gia' le reti
  drawApScreen();
  Serial.println("Modalita' setup: rete SmallTV-Setup, pagina 192.168.4.1");
}

bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t t0 = millis();
  int dots = 0;
  char msg[40];
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000UL) {
    delay(250);
    if (((millis() - t0) / 500) != (uint32_t)dots) {
      dots = (int)((millis() - t0) / 500);
      snprintf(msg, sizeof(msg), "Connessione%.*s", (dots % 4), "...");
      drawSplash(msg);
    }
  }
  return WiFi.status() == WL_CONNECTED;
}

// ------------------------------------------------------------------
//  setup / loop
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("SmallTV Ultra " FW_VERSION);

  analogWriteRange(255);
  pinMode(BL_PIN, OUTPUT);
  setBacklight(0);

  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();
  th = &THEMES[cfg.theme];

  tft.init();
  tft.setRotation(cfg.flip ? 2 : 0);
  tft.invertDisplay(cfg.invert);
  tft.fillScreen(col(th->bg));
  initTimeFont();
  setBacklight(cfg.bright);
  drawSplash("Avvio...");

  bool online = false;
  if (cfg.ssid[0] != 0) online = connectWifi();

  if (!online) {
    startAP();
  } else {
    Serial.print("Connesso, IP ");
    Serial.println(WiFi.localIP());
    configTime(TZ_ROME, "pool.ntp.org", "time.cloudflare.com");
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    drawIpScreen(WiFi.localIP().toString().c_str());
    delay(3500);
    fullRedraw();
  }
  setupWebRoutes();
}

void loop() {
  server.handleClient();

  if (pendingReboot && (int32_t)(millis() - rebootAt) >= 0) {
    delay(100);
    ESP.restart();
  }

  if (apMode) {
    dns.processNextRequest();
    if (WiFi.softAPgetStationNum() > 0) apLastClient = millis();
    // rete salvata ma non raggiunta: dopo 3 minuti senza nessuno collegato riprovo
    if (cfg.ssid[0] != 0 && (millis() - apLastClient) > 180000UL) ESP.restart();
    delay(2);
    return;
  }

  MDNS.update();

  if (needRedraw) {
    needRedraw = false;
    fullRedraw();
  }

  if (WiFi.status() == WL_CONNECTED && (int32_t)(millis() - nextWx) >= 0) {
    bool ok = fetchWeather();
    nextWx = millis() + (ok ? (uint32_t)cfg.wxMinutes * 60000UL : 60000UL);
  }

  updateClock();
  if (wxDirty) {
    wxDirty = false;
    drawCard();
  }
  delay(2);
}
