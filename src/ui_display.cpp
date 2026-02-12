#include "ui_display.h"

#include <WiFi.h>
#include <SPI.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

// QR removed (product decision): onboarding shows URL as plain text.
#define UI_HAS_QR 0

// =======================
// E-paper pins (fixed to your wiring)
// =======================
#define PIN_CS   10
#define PIN_DC    9
#define PIN_RST   8
#define PIN_BUSY  7
#define PIN_SCK  12
#define PIN_MOSI 11

// Display driver
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
  GxEPD2_420_GDEY042T81(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY)
);

static uint32_t refreshCount = 0;
static String g_ui_lang = "no";

static String normLang(const String& in)
{
  if (in == "no" || in == "da" || in == "sv" || in == "fi" || in == "en" || in == "uk") return in;
  return "no";
}

static String tr(const char* key)
{
  String k = String(key);
  const String l = g_ui_lang;
  const bool en = (l == "en");
  const bool no = (l == "no");
  const bool da = (l == "da");
  const bool sv = (l == "sv");
  const bool fi = (l == "fi");
  const bool uk = (l == "uk");

  if (k == "mode") return en ? "MODE" : no ? "MODUS" : da ? "TILSTAND" : sv ? "LAGE" : fi ? "TILA" : "MODE";
  if (k == "fan") return en ? "FAN" : no ? "VIFTE" : da ? "BLAES" : sv ? "FLAKT" : fi ? "PUHALLIN" : "FAN";
  if (k == "eff") return en ? "EFF" : no ? "GJENV" : da ? "GENV" : sv ? "ATERV" : fi ? "HYOTY" : "EFF";
  if (k == "outdoor") return en ? "OUTSIDE" : no ? "UTELUFT" : da ? "UDELUFT" : sv ? "UTELUFT" : fi ? "ULKOILMA" : "OUTSIDE";
  if (k == "supply") return en ? "SUPPLY" : no ? "TILLUFT" : da ? "TILLUFT" : sv ? "TILLUFT" : fi ? "TULOILMA" : "SUPPLY";
  if (k == "extract") return en ? "EXTRACT" : no ? "AVTREKK" : da ? "AFTRAEK" : sv ? "FRANLUFT" : fi ? "POISTOILMA" : "EXTRACT";
  if (k == "exhaust") return en ? "EXHAUST" : no ? "AVKAST" : da ? "AFKAST" : sv ? "AVLUFT" : fi ? "JATEILMA" : "EXHAUST";
  if (k == "updated") return en ? "upd" : no ? "siste" : da ? "sidst" : sv ? "senast" : fi ? "paiv" : "upd";
  if (k == "heat") return en ? "HEAT" : no ? "VARME" : da ? "VARME" : sv ? "VARME" : fi ? "LAMPO" : "HEAT";
  if (k == "setup") return en ? "Setup" : no ? "Oppsett" : da ? "Opsaetning" : sv ? "Installation" : fi ? "Asennus" : "Setup";
  if (k == "shipping_msg") return en ? "Device shipped without support." : no ? "Enheten leveres uten support." : da ? "Enheden leveres uden support." : sv ? "Enheten levereras utan support." : fi ? "Laite toimitetaan ilman tukea." : "Device shipped without support.";
  if (k == "onboard_hint") return en ? "Connect WiFi and open URL in browser" : no ? "Koble til WiFi og apne URL i nettleser" : da ? "Tilslut WiFi og abn URL i browser" : sv ? "Anslut WiFi och oppna URL i webblasare" : fi ? "Yhdista WiFiin ja avaa URL selaimessa" : "Connect WiFi and open URL in browser";
  return k;
}

static String trModeValue(const String& rawMode)
{
  String m = rawMode;
  m.trim();
  m.toUpperCase();

  // Normalize common aliases/legacy values
  if (m == "VARME") m = "HOME";
  if (m == "HJEM") m = "HOME";
  if (m == "HJEMME") m = "HOME";
  if (m == "BORTE") m = "AWAY";
  if (m == "HOY") m = "HIGH";
  if (m == "HØY") m = "HIGH";

  const String l = g_ui_lang;

  if (m == "OFF")   return (l == "en") ? "OFF"  : (l == "no") ? "AV"      : (l == "da") ? "FRA"      : (l == "sv") ? "AV"       : (l == "fi") ? "POIS"     : "OFF";
  if (m == "AWAY")  return (l == "en") ? "AWAY" : (l == "no") ? "BORTE"   : (l == "da") ? "UDE"      : (l == "sv") ? "BORTA"    : (l == "fi") ? "POISSA"   : "AWAY";
  if (m == "HOME")  return (l == "en") ? "HOME" : (l == "no") ? "HJEM"  : (l == "da") ? "HJEMME"   : (l == "sv") ? "HEMMA"    : (l == "fi") ? "KOTI"     : "HOME";
  if (m == "HIGH")  return (l == "en") ? "HIGH" : (l == "no") ? "HIGH"    : (l == "da") ? "HOJ"      : (l == "sv") ? "HOG"      : (l == "fi") ? "TEHO"     : "HIGH";
  if (m == "FUME")  return (l == "en") ? "FUME" : (l == "no") ? "MATLAGING" : (l == "da") ? "MADLAVNING" : (l == "sv") ? "MATLAGNING" : (l == "fi") ? "RUUANLAITTO" : "FUME";
  if (m == "FIRE")  return (l == "en") ? "FIRE" : (l == "no") ? "PEIS"    : (l == "da") ? "PEJS"     : (l == "sv") ? "KAMIN"    : (l == "fi") ? "TAKKA"    : "FIRE";
  if (m == "TMP HIGH") return (l == "en") ? "TMP HIGH" : (l == "no") ? "MIDL HIGH" : (l == "da") ? "MIDL HOJ" : (l == "sv") ? "TILLF HOG" : (l == "fi") ? "VALIAIK TEHO" : "TMP HIGH";
  if (m == "UNKNOWN") return (l == "en") ? "UNKNOWN" : (l == "no") ? "UKJENT" : (l == "da") ? "UKENDT" : (l == "sv") ? "OKAND" : (l == "fi") ? "TUNTEMATON" : "UNKNOWN";

  return rawMode;
}

static inline void setTextBlack() { display.setTextColor(GxEPD_BLACK); }
static inline void setTextWhite() { display.setTextColor(GxEPD_WHITE); }

enum IconType { ICON_OUTDOOR, ICON_SUPPLY, ICON_EXTRACT, ICON_EXHAUST };

static void drawChip(int x, int y, int w, int h, const String& txt)
{
  display.fillRoundRect(x, y, w, h, 8, GxEPD_BLACK);
  setTextWhite();
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 10, y + 16);
  display.print(txt);
  setTextBlack();
}

static int textPixelWidth(const String& txt)
{
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  display.setFont(&FreeSans9pt7b);
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}

static String fitTextToWidth(const String& txt, int maxW)
{
  if (textPixelWidth(txt) <= maxW) return txt;
  String t = txt;
  while (t.length() > 3 && textPixelWidth(t + "...") > maxW)
    t.remove(t.length() - 1);
  return t + "...";
}

static int textPixelWidthMono(const String& txt)
{
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  display.setFont(&FreeMonoBold9pt7b);
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}

static void drawIconV2(int x, int y, IconType t)
{
  switch (t)
  {
    case ICON_OUTDOOR: { // tree
      display.fillRect(x + 20, y + 28, 6, 14, GxEPD_BLACK); // trunk
      display.fillTriangle(x + 23, y + 6,  x + 10, y + 24, x + 36, y + 24, GxEPD_BLACK);
      display.fillTriangle(x + 23, y + 12, x + 8,  y + 30, x + 38, y + 30, GxEPD_BLACK);
      display.fillTriangle(x + 23, y + 18, x + 6,  y + 36, x + 40, y + 36, GxEPD_BLACK);
      break;
    }
    case ICON_SUPPLY: { // clean right arrow
      display.fillRect(x + 8, y + 20, 18, 6, GxEPD_BLACK);
      display.fillTriangle(x + 26, y + 12, x + 26, y + 34, x + 44, y + 23, GxEPD_BLACK);
      break;
    }
    case ICON_EXTRACT: { // up arrow
      display.fillTriangle(x+22, y+10, x+10, y+26, x+34, y+26, GxEPD_BLACK);
      display.fillRect(x+18, y+26, 8, 16, GxEPD_BLACK);
      break;
    }
    case ICON_EXHAUST: { // down arrow
      display.fillTriangle(x+10, y+22, x+34, y+22, x+22, y+38, GxEPD_BLACK);
      display.fillRect(x+18, y+6, 8, 16, GxEPD_BLACK);
      break;
    }
  }
}

static void drawDegreeSymbol(int x, int y, int r = 3)
{
  // small ring, visually like "°"
  display.drawCircle(x, y, r, GxEPD_BLACK);
  display.drawCircle(x, y, r-1, GxEPD_BLACK);
}

static void drawHeader(const FlexitData& d)
{
  display.fillRect(-2, 0, display.width()+4, 32, GxEPD_BLACK);
  setTextWhite();
  display.setFont(&FreeMonoBold9pt7b);
  String model = d.device_model.length() ? d.device_model : "S3";
  String modelTxt = String("NORDIC ") + model;
  String clockTxt = d.time;

  String setTxt = "SET --.-C";
  if (!isnan(d.set_temp))
  {
    char sb[24];
    snprintf(sb, sizeof(sb), "SET %.1fC", d.set_temp);
    setTxt = String(sb);
  }

  int modelW = textPixelWidthMono(modelTxt);
  display.setFont(&FreeSans9pt7b); // cleaner digit shapes in header (notably "4")
  const int setW = textPixelWidth(setTxt);
  display.setFont(&FreeMonoBold9pt7b);
  const int clockW = textPixelWidthMono(clockTxt);
  const int leftPad = 8;
  const int rightPad = 8;
  const int headerGap = 10;

  int clockX = display.width() - rightPad - clockW;
  int setX = (display.width() - setW) / 2;
  const int minSetX = leftPad + modelW + headerGap;
  const int maxSetX = clockX - headerGap - setW;
  if (setX < minSetX) setX = minSetX;
  if (setX > maxSetX) setX = maxSetX;
  if (setX < leftPad + 70) setX = leftPad + 70;

  int maxModelW = setX - leftPad - headerGap;
  if (maxModelW < 40) maxModelW = 40;
  if (modelW > maxModelW)
  {
    while (modelTxt.length() > 3 && textPixelWidthMono(modelTxt + "...") > maxModelW)
      modelTxt.remove(modelTxt.length() - 1);
    modelTxt += "...";
    modelW = textPixelWidthMono(modelTxt);
  }

  display.setCursor(leftPad, 20);
  display.print(modelTxt);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(setX, 20);
  display.print(setTxt);
  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(clockX, 20);
  display.print(clockTxt);
  setTextBlack();

  // chips line (stable widths so percentages are always visible)
  const int y = 38;
  const int h = 22;
  const int startX = 6;
  const int chipGap = 6;
  const int avail = display.width() - (startX * 2) - (chipGap * 2);
  const int minMode = 140;
  const int minFan = 96;
  const int minEff = 96;
  const String modeTxt = tr("mode") + String(": ") + trModeValue(d.mode);
  const String fanTxt = tr("fan") + String(" ") + d.fan_percent + "%";
  const String effTxt = tr("eff") + String(" ") + d.efficiency_percent + "%";

  int wFan = textPixelWidth(fanTxt) + 20;
  int wEff = textPixelWidth(effTxt) + 20;
  if (wFan < minFan) wFan = minFan;
  if (wEff < minEff) wEff = minEff;

  int dynamicModeMin = textPixelWidth(modeTxt) + 24;
  if (dynamicModeMin < minMode) dynamicModeMin = minMode;
  int wMode = avail - wFan - wEff;
  if (wMode < dynamicModeMin)
  {
    int need = dynamicModeMin - wMode;
    int fanHeadroom = wFan - minFan;
    int effHeadroom = wEff - minEff;
    int totalHeadroom = fanHeadroom + effHeadroom;
    if (totalHeadroom > 0)
    {
      int takeFan = (need * fanHeadroom) / totalHeadroom;
      int takeEff = need - takeFan;
      if (takeFan > fanHeadroom) takeFan = fanHeadroom;
      if (takeEff > effHeadroom) takeEff = effHeadroom;
      wFan -= takeFan;
      wEff -= takeEff;
    }
    if (wFan < minFan) wFan = minFan;
    if (wEff < minEff) wEff = minEff;
    wMode = avail - wFan - wEff;
    if (wMode < dynamicModeMin) wMode = dynamicModeMin;
  }

  const int x1 = startX;
  const int x2 = x1 + wMode + chipGap;
  const int x3 = x2 + wFan + chipGap;

  drawChip(x1, y, wMode, h, modeTxt);
  drawChip(x2, y, wFan,  h, fanTxt);
  drawChip(x3, y, wEff,  h, effTxt);
}

static void drawTempCard(int x, int y, const char* label, float value, IconType icon, const FlexitData& d)
{
  const int cardW = 190;
  const int cardH = 92;

  display.drawRoundRect(x, y, cardW, cardH, 12, GxEPD_BLACK);

  drawIconV2(x + 12, y + 18, icon);

  setTextBlack();
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 70, y + 22);
  display.print(label);

  display.setFont(&FreeSansBold18pt7b);
  display.setCursor(x + 70, y + 62);
  if (isnan(value)) display.print("--.-");
  else display.print(value, 1);

  // degree symbol and C
  int degX = x + 70 + 78;
  int degY = y + 48;
  drawDegreeSymbol(degX, degY, 3);
  display.setCursor(degX + 7, y + 62);
  display.print("C");

  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 62, y + 84);
  display.print(tr("updated"));
  display.print(" ");
  display.print(d.data_time);
}

static void drawFooter(const FlexitData& d, const String& mbStatus)
{
  const int h = 30;
  const int y = display.height() - h;
  const int colW = display.width() / 3;
  const int innerPad = 6;

  display.fillRect(-2, y, display.width()+4, h, GxEPD_BLACK);
  display.drawFastVLine(colW, y + 5, h - 10, GxEPD_WHITE);
  display.drawFastVLine(colW * 2, y + 5, h - 10, GxEPD_WHITE);
  setTextWhite();
  display.setFont(&FreeSans9pt7b);

  String leftTxt = tr("heat") + String(" ") + d.heat_element_percent + "%";
  String midTxt = mbStatus;
  String rightTxt = String("WiFi ") + d.wifi_status + d.ip;

  leftTxt = fitTextToWidth(leftTxt, colW - (innerPad * 2));
  midTxt = fitTextToWidth(midTxt, colW - (innerPad * 2));
  rightTxt = fitTextToWidth(rightTxt, colW - (innerPad * 2));

  int leftX = (colW - textPixelWidth(leftTxt)) / 2;
  if (leftX < innerPad) leftX = innerPad;
  int midX = colW + ((colW - textPixelWidth(midTxt)) / 2);
  if (midX < colW + innerPad) midX = colW + innerPad;
  int rightX = (colW * 2) + ((colW - textPixelWidth(rightTxt)) / 2);
  if (rightX < (colW * 2) + innerPad) rightX = (colW * 2) + innerPad;

  display.setCursor(leftX, y + 20);
  display.print(leftTxt);
  display.setCursor(midX, y + 20);
  display.print(midTxt);
  display.setCursor(rightX, y + 20);
  display.print(rightTxt);

  setTextBlack();
}

void ui_init()
{
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  display.init(115200);
  // User preference: rotate display 180 degrees from previous orientation.
  display.setRotation(0);
  // Keep panel in low-power state between refreshes for longevity.
  display.powerOff();
}

void ui_set_language(const String& lang)
{
  g_ui_lang = normLang(lang);
}

void ui_render(const FlexitData& d, const String& mbStatus)
{
  display.setFullWindow();

  refreshCount++;
  if (refreshCount % 10 == 0)
  {
    display.clearScreen();
    delay(200);
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    setTextBlack();

    drawHeader(d);

    drawTempCard(5,   70,  tr("outdoor").c_str(), d.uteluft, ICON_OUTDOOR, d);
    drawTempCard(205, 70,  tr("supply").c_str(),  d.tilluft, ICON_SUPPLY,  d);
    drawTempCard(5,   168, tr("extract").c_str(), d.avtrekk, ICON_EXTRACT, d);
    drawTempCard(205, 168, tr("exhaust").c_str(), d.avkast,  ICON_EXHAUST, d);

    drawFooter(d, mbStatus);

  } while (display.nextPage());

  // Put panel in low-power state when not actively refreshing.
  display.powerOff();
}

static void drawOnboardingCard(int x, int y, int w, int h, const String& title, const String& body)
{
  display.drawRoundRect(x, y, w, h, 12, GxEPD_BLACK);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(x + 12, y + 22);
  display.print(title);

  display.setFont(&FreeMonoBold9pt7b);
  int lineY = y + 45;
  int lineH = 16;

  // split body by \n
  int start = 0;
  while (start < (int)body.length())
  {
    int nl = body.indexOf('\n', start);
    String line = (nl == -1) ? body.substring(start) : body.substring(start, nl);
    display.setCursor(x + 12, lineY);
    display.print(line);
    lineY += lineH;
    if (nl == -1) break;
    start = nl + 1;
  }
}

#if UI_HAS_QR
static void drawQr(int x, int y, int sizePx, const String& text)
{
  // QRCode lib produces module matrix; we draw it as filled squares.
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, ECC_MEDIUM, text.c_str());

  int modules = qrcode.size;
  int scale = sizePx / modules;
  if (scale < 1) scale = 1;

  // quiet zone
  int pad = 2;
  int qrW = modules * scale;
  display.drawRect(x-1, y-1, qrW + pad*2 + 2, qrW + pad*2 + 2, GxEPD_BLACK);

  for (int yy = 0; yy < modules; yy++)
  {
    for (int xx = 0; xx < modules; xx++)
    {
      if (qrcode_getModule(&qrcode, xx, yy))
      {
        display.fillRect(x + pad + xx*scale, y + pad + yy*scale, scale, scale, GxEPD_BLACK);
      }
    }
  }
}
#endif

void ui_render_onboarding(const String& apSsid, const String& apPass, const String& ip, const String& url)
{
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    setTextBlack();

    // Top header
    display.fillRect(-2, 0, display.width()+4, 32, GxEPD_BLACK);
    setTextWhite();
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 20);
    display.print("VENT READER FOR FLEXIT");
    setTextBlack();

    // Title
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(10, 60);
    display.print(tr("setup"));

    // Cards
    drawOnboardingCard(10, 75, 380, 68, "WiFi (AP)", String("SSID: ") + apSsid + "\nPASS: " + apPass);
    drawOnboardingCard(10, 150, 380, 68, "Adresse", String("IP: ") + ip + "\nUSER: admin  PASS: ventreader");

#if UI_HAS_QR
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 240);
    display.print("Scan QR for admin-side:");
    drawQr(250, 206, 110, url);
#else
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 240);
    display.print(tr("shipping_msg"));
#endif

    // Footer hint
    display.fillRect(-2, display.height()-30, display.width()+4, 30, GxEPD_BLACK);
    setTextWhite();
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, display.height()-10);
    display.print(tr("onboard_hint"));

    setTextBlack();
  } while (display.nextPage());

  // Onboarding screen is static for long periods; keep panel powered down.
  display.powerOff();
}

void ui_epaper_hard_clear()
{
  // Hard reset + re-init of panel
  display.init(115200, true, 2, false);

  // Full clear (important: e-paper keeps old image across flashes)
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());

  // Put panel in stable low-power state
  display.hibernate();
}
