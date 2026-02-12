#include <WiFi.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <math.h>

#include "src/flexit_types.h"
#include "src/ui_display.h"
#include "src/data_example.h"
#include "src/flexit_modbus.h"
#include "src/flexit_bacnet.h"
#include "src/config_store.h"
#include "src/homey_http.h" // web portal + /status API
#include "src/ha_mqtt.h"

// Optional hard override for "light/headless build".
// Keep 0 for normal runtime-configurable behavior.
#ifndef VENTREADER_FORCE_HEADLESS
#define VENTREADER_FORCE_HEADLESS 0
#endif

// Keep credentials/tokens out of source control

// WiFi credentials live in secrets.h

// =======================
// NTP / TIME
// =======================
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.cloudflare.com";
// Norway CET/CEST (DST auto)
static const char* TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// =======================
// PRODUCT IDENTITY (no trademarked brand name)
// =======================
// Visible device name for users (WiFi AP / mDNS). Keep this generic for commercialization.
static const char* PRODUCT_DEVICE_NAME = "vent-reader";     // used as hostname/mDNS base
static const char* PRODUCT_AP_PREFIX   = "Ventreader";      // shown to user in WiFi list
static const char* PRODUCT_AP_PASS     = "ventreader";      // WPA2 (>=8 chars). Change in config later if desired.


static bool timeReady = false;

static void setupTimeNTP()
{
  configTime(0, 0, NTP1, NTP2);
  setenv("TZ", TZ_INFO, 1);
  tzset();

  tm t;
  for (int i = 0; i < 40; i++)
  {
    if (getLocalTime(&t, 250))
    {
      timeReady = true;
      Serial.println("NTP time OK");
      return;
    }
    delay(250);
  }
  Serial.println("NTP time NOT ready (yet)");
  timeReady = false;
}

static String nowHHMM()
{
  tm t;
  if (!getLocalTime(&t, 50)) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}


static String buildApSsid()
{
  // Example: Vent-reader-115 (last IP octet) is not available yet in AP, so use MAC suffix
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s-%02X%02X%02X", PRODUCT_AP_PREFIX, mac[3], mac[4], mac[5]);
  return String(buf);
}

static String apIp()
{
  IPAddress ip = WiFi.softAPIP();
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return String(buf);
}

static String ipLastOctetDot()
{
  if (WiFi.status() != WL_CONNECTED) return "";
  IPAddress ip = WiFi.localIP();
  char buf[8];
  snprintf(buf, sizeof(buf), ".%u", ip[3]);
  return String(buf);
}

static int calcEtaPercent(float uteluft, float tilluft, float avtrekk)
{
  float denom = (avtrekk - uteluft);
  if (fabsf(denom) < 0.01f) return 0;
  float eta = ((tilluft - uteluft) / denom) * 100.0f;
  if (eta < 0) eta = 0;
  if (eta > 100) eta = 100;
  return (int)lroundf(eta);
}

// =======================
// GLOBAL STATE
// =======================
static DeviceConfig cfg;
static FlexitData data;
static String mbStatus = "MB OFF";
static FlexitData lastGoodModbusData;
static bool hasLastGoodModbusData = false;
static FlexitData lastGoodBacnetData;
static bool hasLastGoodBacnetData = false;
static uint32_t lastBacnetPollMs = 0;

// UI refresh every 10 minutes
static uint32_t UI_REFRESH_MS = 10UL * 60UL * 1000UL;
static uint32_t lastRefresh = 0;
static String lastUiLanguageApplied = "";

static bool displayActive()
{
  return (cfg.display_enabled && VENTREADER_FORCE_HEADLESS == 0);
}

static String modelLabel(const String& key)
{
  if (key == "S4") return "S4";
  if (key == "S2_EXP") return "S2 EXP";
  if (key == "S7_EXP") return "S7 EXP";
  if (key == "CL3_EXP") return "CL3 EXP";
  if (key == "CL4_EXP") return "CL4 EXP";
  return "S3";
}

static String normDataSource(const String& in)
{
  if (in == "BACNET" || in == "FLEXITWEB") return "BACNET";
  return "MODBUS";
}

static bool useBacnetSource()
{
  return normDataSource(cfg.data_source) == "BACNET";
}

static uint32_t bacnetPollIntervalMs()
{
  uint32_t m = cfg.bacnet_poll_minutes;
  if (m < 5) m = 5;
  if (m > 60) m = 60;
  return m * 60UL * 1000UL;
}

static void updateCommonMeta()
{
  data.wifi_status = (WiFi.status() == WL_CONNECTED) ? "OK" : "NO";
  data.ip = ipLastOctetDot();
  data.device_model = modelLabel(cfg.model);
}

static void markDataFreshNow()
{
  data.data_time = nowHHMM();
}

static void markDataUnknownTime()
{
  data.data_time = "--:--";
}

static void markScreenRefreshNow()
{
  data.time = nowHHMM();
}

static void applyRuntimeModbusConfig()
{
  FlexitModbusRuntimeConfig mcfg;
  mcfg.model = cfg.model;
  mcfg.baud = cfg.modbus_baud;
  mcfg.slave_id = cfg.modbus_slave_id;
  mcfg.addr_offset = cfg.modbus_addr_offset;
  mcfg.serial_format = cfg.modbus_serial_format;
  mcfg.transport_mode = cfg.modbus_transport_mode;
  flexit_modbus_set_runtime_config(mcfg);
}

static void clearModbusDataUnknown()
{
  data.uteluft = NAN;
  data.tilluft = NAN;
  data.avtrekk = NAN;
  data.avkast = NAN;
  data.set_temp = NAN;
  data.fan_percent = 0;
  data.heat_element_percent = 0;
  data.mode = "N/A";
  markDataUnknownTime();
}

static void recomputeEfficiency()
{
  if (!isnan(data.uteluft) && !isnan(data.tilluft) && !isnan(data.avtrekk))
    data.efficiency_percent = calcEtaPercent(data.uteluft, data.tilluft, data.avtrekk);
  else
    data.efficiency_percent = 0;
}

static void updateDataFromModbusOrFallback()
{
  updateCommonMeta();

  flexit_modbus_set_enabled(cfg.modbus_enabled);
  applyRuntimeModbusConfig();

  if (!flexit_modbus_is_enabled())
  {
    mbStatus = "MB OFF";
    data_example_fill(data);
    markDataUnknownTime();
    data.device_model = cfg.model;
    hasLastGoodModbusData = false;
  }
  else
  {
    if (flexit_modbus_poll(data))
    {
      markDataFreshNow();
      mbStatus = String("MB OK (") + flexit_modbus_active_map() + ")";
      lastGoodModbusData = data;
      hasLastGoodModbusData = true;
    }
    else
    {
      const char* err = flexit_modbus_last_error();
      if (hasLastGoodModbusData)
      {
        data = lastGoodModbusData;
        updateCommonMeta();
        mbStatus = String("MB ") + (err ? err : "ERR") + " (stale)";
      }
      else
      {
        clearModbusDataUnknown();
        updateCommonMeta();
        mbStatus = String("MB ") + (err ? err : "ERR");
      }
    }
  }
  recomputeEfficiency();
}

static void updateDataFromBacnet(bool forceRead)
{
  updateCommonMeta();
  flexit_bacnet_set_runtime_config(cfg);

  const uint32_t nowMs = millis();
  const uint32_t srcIntMs = bacnetPollIntervalMs();
  const bool shouldRead = forceRead || !hasLastGoodBacnetData || (nowMs - lastBacnetPollMs >= srcIntMs);

  if (!shouldRead)
  {
    data = lastGoodBacnetData;
    updateCommonMeta();
    mbStatus = "BACNET OK (cached)";
  }
  else if (flexit_bacnet_poll(data))
  {
    markDataFreshNow();
    mbStatus = "BACNET OK";
    lastGoodBacnetData = data;
    hasLastGoodBacnetData = true;
    lastBacnetPollMs = nowMs;
  }
  else
  {
    const char* err = flexit_bacnet_last_error();
    if (hasLastGoodBacnetData)
    {
      data = lastGoodBacnetData;
      updateCommonMeta();
      mbStatus = String("BACNET ") + (err ? err : "ERR") + " (stale)";
    }
    else
    {
      clearModbusDataUnknown();
      updateCommonMeta();
      mbStatus = String("BACNET ") + (err ? err : "ERR");
    }
  }

  recomputeEfficiency();
}

static void updateDataFromActiveSource(bool forceRead)
{
  if (useBacnetSource())
    updateDataFromBacnet(forceRead);
  else
    updateDataFromModbusOrFallback();
}

void setup()
{
  Serial.begin(115200);
  delay(250);

  // ---- Factory reset via BOOT button (GPIO0) ----
  // Hold BOOT during power-on for ~6 seconds to wipe config and restore defaults.
  pinMode(0, INPUT_PULLUP);
  uint32_t t0 = millis();
  while (digitalRead(0) == LOW && millis() - t0 < 6000) { delay(10); }
  if (digitalRead(0) == LOW) {
    Serial.println("FACTORY RESET (BOOT held)");
    config_begin();
    config_factory_reset();
    delay(200);
    ESP.restart();
  }

  config_begin();
  cfg = config_load();

  const bool setup_done = cfg.setup_completed;

// TVUNGEN reset av e-paper etter flashing / cold boot
  if (displayActive())
    ui_epaper_hard_clear();

  if (displayActive())
  {
    ui_init();
    ui_set_language(cfg.ui_language);
    lastUiLanguageApplied = cfg.ui_language;
  }
  else
  {
    lastUiLanguageApplied = cfg.ui_language;
  }

  // Apply poll interval from config
  UI_REFRESH_MS = cfg.poll_interval_ms;
  if (displayActive() && UI_REFRESH_MS < 180000UL) UI_REFRESH_MS = 180000UL;


  // ---- WiFi connect (STA) ----
  WiFi.setHostname(PRODUCT_DEVICE_NAME);
  bool sta_ok = false;
if (cfg.wifi_ssid.length() > 0)
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  sta_ok = (WiFi.status() == WL_CONNECTED);
}

  // ---- Onboarding / Setup mode ----
  // If setup is not completed yet, ALWAYS start the provisioning AP and show the onboarding screen,
  // even if STA connects successfully (so Q/A can verify the customer-friendly info screen).
  if (!setup_done)
  {
    WiFi.mode(WIFI_AP_STA);
    String apSsid = buildApSsid();
    WiFi.softAP(apSsid.c_str(), PRODUCT_AP_PASS);

    // Prefer showing STA IP if connected, otherwise AP IP
    String ip = sta_ok ? WiFi.localIP().toString() : apIp();
    String url = String("http://") + ip + "/admin/setup";

    if (displayActive())
      ui_render_onboarding(apSsid, PRODUCT_AP_PASS, ip, url);
    else
      Serial.println(String("HEADLESS setup ready: open ") + url);
  }
  else if (!sta_ok)
  {
    // setup done, but STA failed => keep AP as fallback for recovery
    WiFi.mode(WIFI_AP_STA);
    String apSsid = buildApSsid();
    WiFi.softAP(apSsid.c_str(), PRODUCT_AP_PASS);
  }

if (WiFi.status() == WL_CONNECTED)
  setupTimeNTP();

  // Enable OTA only when device is configured and has STA connectivity
  if (setup_done && sta_ok)
  {
    ArduinoOTA.setHostname(PRODUCT_DEVICE_NAME);
    ArduinoOTA.begin();
  }

  // Start web portal (admin + /status API)
webportal_begin(cfg);
  ha_mqtt_begin(cfg);

  updateDataFromActiveSource(true);
  if (!displayActive()) data.time = "--:--";

  // Only show dashboard after wizard is completed
  if (setup_done && displayActive())
  {
    ui_set_language(cfg.ui_language);
    lastUiLanguageApplied = cfg.ui_language;
    markScreenRefreshNow();
    ui_render(data, mbStatus);
  }
  webportal_set_data(data, mbStatus);
  ha_mqtt_request_publish_now();

  lastRefresh = millis();
}

void loop()
{
  // Handle incoming HTTP requests for /status
  webportal_loop();
  ha_mqtt_loop(cfg, data, mbStatus);

  // Keep runtime refresh interval in sync with config changes from admin.
  UI_REFRESH_MS = cfg.poll_interval_ms;
  if (displayActive() && UI_REFRESH_MS < 180000UL) UI_REFRESH_MS = 180000UL;

  // Arduino OTA (enabled when on network)
  ArduinoOTA.handle();

  // Apply language changes from admin immediately on ePaper, without waiting for poll interval.
  if (cfg.setup_completed && displayActive() && cfg.ui_language != lastUiLanguageApplied)
  {
    ui_set_language(cfg.ui_language);
    lastUiLanguageApplied = cfg.ui_language;
    markScreenRefreshNow();
    ui_render(data, mbStatus);
  }

  // Force immediate source refresh after admin test/save changes.
  if (webportal_consume_refresh_request())
  {
    updateDataFromActiveSource(true);
    if (!displayActive()) data.time = "--:--";
    if (cfg.setup_completed && displayActive())
    {
      ui_set_language(cfg.ui_language);
      lastUiLanguageApplied = cfg.ui_language;
      markScreenRefreshNow();
      ui_render(data, mbStatus);
    }
    webportal_set_data(data, mbStatus);
    ha_mqtt_request_publish_now();
    lastRefresh = millis();
  }

  if (millis() - lastRefresh >= UI_REFRESH_MS)
  {
    lastRefresh = millis();

    if (!timeReady && WiFi.status() == WL_CONNECTED)
      setupTimeNTP();

    updateDataFromActiveSource(false);
    if (!displayActive()) data.time = "--:--";
    if (cfg.setup_completed && displayActive())
    {
      ui_set_language(cfg.ui_language);
      lastUiLanguageApplied = cfg.ui_language;
      markScreenRefreshNow();
      ui_render(data, mbStatus);
    }

    // Optional: send to Homey (stub for now)
    // homey_post_status(data);
    webportal_set_data(data, mbStatus);
    ha_mqtt_request_publish_now();

    // Optional push (not needed for read-only):
    // homey_post_status(data);
  }

  delay(50);
}
