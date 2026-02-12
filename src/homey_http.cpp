#include "homey_http.h"

#include <cstring>
#include <cstdio>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_system.h>
#include "version.h"
#include "flexit_modbus.h"
#include "flexit_bacnet.h"
#include "ha_mqtt.h"

static WebServer server(80);

static DeviceConfig* g_cfg = nullptr;

// Wizard state (avoid BasicAuth re-login until Finish)
static String g_pending_admin_pass;
static bool   g_pending_admin_set = false;

// Deferred restart after HTTP response
static bool   g_restart_pending = false;
static uint32_t g_restart_at_ms = 0;
static bool   g_refresh_requested = false;

// Cached payload for /status
static FlexitData g_data;
static String g_mb = "MB OFF";

static bool checkAdminAuth();
static String buildStatusJson(bool pretty);
static void redirectTo(const String& path);
static String jsonEscape(const String& s);

static String jsonUnescape(const String& in)
{
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++)
  {
    char c = in[i];
    if (c == '\\' && i + 1 < in.length())
    {
      char n = in[++i];
      if (n == 'n') out += '\n';
      else if (n == 'r') out += '\r';
      else if (n == 't') out += '\t';
      else out += n;
    }
    else out += c;
  }
  return out;
}

static bool jsonFindKey(const String& json, const char* key, int& valStart)
{
  const String pat = String("\"") + key + "\"";
  int p = json.indexOf(pat);
  if (p < 0) return false;
  p = json.indexOf(':', p + pat.length());
  if (p < 0) return false;
  p++;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t')) p++;
  if (p >= (int)json.length()) return false;
  valStart = p;
  return true;
}

static bool jsonGetString(const String& json, const char* key, String& out)
{
  int p = 0;
  if (!jsonFindKey(json, key, p)) return false;
  if (json[p] != '\"') return false;
  p++;
  String raw;
  for (; p < (int)json.length(); p++)
  {
    char c = json[p];
    if (c == '\"' && (p == 0 || json[p - 1] != '\\')) break;
    raw += c;
  }
  out = jsonUnescape(raw);
  return true;
}

static bool jsonGetBool(const String& json, const char* key, bool& out)
{
  int p = 0;
  if (!jsonFindKey(json, key, p)) return false;
  if (json.substring(p, p + 4) == "true") { out = true; return true; }
  if (json.substring(p, p + 5) == "false") { out = false; return true; }
  return false;
}

static bool jsonGetInt(const String& json, const char* key, int& out)
{
  int p = 0;
  if (!jsonFindKey(json, key, p)) return false;
  int e = p;
  while (e < (int)json.length() && (json[e] == '-' || (json[e] >= '0' && json[e] <= '9'))) e++;
  if (e == p) return false;
  out = json.substring(p, e).toInt();
  return true;
}

static String buildConfigExportJson()
{
  String out;
  out.reserve(4096);
  out += "{\n";
  out += "  \"export\": \"ventreader_config_v1\",\n";
  out += "  \"fw\": \"" + jsonEscape(String(FW_VERSION)) + "\",\n";
  out += "  \"config\": {\n";
  out += "    \"wifi_ssid\": \"" + jsonEscape(g_cfg->wifi_ssid) + "\",\n";
  out += "    \"wifi_pass\": \"" + jsonEscape(g_cfg->wifi_pass) + "\",\n";
  out += "    \"api_token\": \"" + jsonEscape(g_cfg->api_token) + "\",\n";
  out += "    \"homey_api_token\": \"" + jsonEscape(g_cfg->homey_api_token) + "\",\n";
  out += "    \"ha_api_token\": \"" + jsonEscape(g_cfg->ha_api_token) + "\",\n";
  out += "    \"model\": \"" + jsonEscape(g_cfg->model) + "\",\n";
  out += "    \"modbus_enabled\": " + String(g_cfg->modbus_enabled ? "true" : "false") + ",\n";
  out += "    \"homey_enabled\": " + String(g_cfg->homey_enabled ? "true" : "false") + ",\n";
  out += "    \"ha_enabled\": " + String(g_cfg->ha_enabled ? "true" : "false") + ",\n";
  out += "    \"display_enabled\": " + String(g_cfg->display_enabled ? "true" : "false") + ",\n";
  out += "    \"ha_mqtt_enabled\": " + String(g_cfg->ha_mqtt_enabled ? "true" : "false") + ",\n";
  out += "    \"control_enabled\": " + String(g_cfg->control_enabled ? "true" : "false") + ",\n";
  out += "    \"data_source\": \"" + jsonEscape(g_cfg->data_source) + "\",\n";
  out += "    \"ha_mqtt_host\": \"" + jsonEscape(g_cfg->ha_mqtt_host) + "\",\n";
  out += "    \"ha_mqtt_port\": " + String((int)g_cfg->ha_mqtt_port) + ",\n";
  out += "    \"ha_mqtt_user\": \"" + jsonEscape(g_cfg->ha_mqtt_user) + "\",\n";
  out += "    \"ha_mqtt_pass\": \"" + jsonEscape(g_cfg->ha_mqtt_pass) + "\",\n";
  out += "    \"ha_mqtt_prefix\": \"" + jsonEscape(g_cfg->ha_mqtt_prefix) + "\",\n";
  out += "    \"ha_mqtt_topic_base\": \"" + jsonEscape(g_cfg->ha_mqtt_topic_base) + "\",\n";
  out += "    \"ha_mqtt_interval_s\": " + String((int)g_cfg->ha_mqtt_interval_s) + ",\n";
  out += "    \"bacnet_ip\": \"" + jsonEscape(g_cfg->bacnet_ip) + "\",\n";
  out += "    \"bacnet_port\": " + String((int)g_cfg->bacnet_port) + ",\n";
  out += "    \"bacnet_device_id\": " + String(g_cfg->bacnet_device_id) + ",\n";
  out += "    \"bacnet_poll_minutes\": " + String((int)g_cfg->bacnet_poll_minutes) + ",\n";
  out += "    \"bacnet_timeout_ms\": " + String((int)g_cfg->bacnet_timeout_ms) + ",\n";
  out += "    \"bacnet_write_enabled\": " + String(g_cfg->bacnet_write_enabled ? "true" : "false") + ",\n";
  out += "    \"bacnet_obj_outdoor\": \"" + jsonEscape(g_cfg->bacnet_obj_outdoor) + "\",\n";
  out += "    \"bacnet_obj_supply\": \"" + jsonEscape(g_cfg->bacnet_obj_supply) + "\",\n";
  out += "    \"bacnet_obj_extract\": \"" + jsonEscape(g_cfg->bacnet_obj_extract) + "\",\n";
  out += "    \"bacnet_obj_exhaust\": \"" + jsonEscape(g_cfg->bacnet_obj_exhaust) + "\",\n";
  out += "    \"bacnet_obj_fan\": \"" + jsonEscape(g_cfg->bacnet_obj_fan) + "\",\n";
  out += "    \"bacnet_obj_heat\": \"" + jsonEscape(g_cfg->bacnet_obj_heat) + "\",\n";
  out += "    \"bacnet_obj_mode\": \"" + jsonEscape(g_cfg->bacnet_obj_mode) + "\",\n";
  out += "    \"bacnet_obj_setpoint_home\": \"" + jsonEscape(g_cfg->bacnet_obj_setpoint_home) + "\",\n";
  out += "    \"bacnet_obj_setpoint_away\": \"" + jsonEscape(g_cfg->bacnet_obj_setpoint_away) + "\",\n";
  out += "    \"bacnet_mode_map\": \"" + jsonEscape(g_cfg->bacnet_mode_map) + "\",\n";
  out += "    \"ui_language\": \"" + jsonEscape(g_cfg->ui_language) + "\",\n";
  out += "    \"modbus_transport_mode\": \"" + jsonEscape(g_cfg->modbus_transport_mode) + "\",\n";
  out += "    \"modbus_serial_format\": \"" + jsonEscape(g_cfg->modbus_serial_format) + "\",\n";
  out += "    \"modbus_baud\": " + String((int)g_cfg->modbus_baud) + ",\n";
  out += "    \"modbus_slave_id\": " + String((int)g_cfg->modbus_slave_id) + ",\n";
  out += "    \"modbus_addr_offset\": " + String((int)g_cfg->modbus_addr_offset) + ",\n";
  out += "    \"poll_interval_ms\": " + String((int)g_cfg->poll_interval_ms) + "\n";
  out += "  }\n";
  out += "}\n";
  return out;
}

static uint64_t nowEpochMs()
{
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return 0;
  return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
}

static String isoFromEpochMs(uint64_t ms)
{
  if (ms < 1000ULL) return "";
  time_t sec = (time_t)(ms / 1000ULL);
  struct tm tmv;
  localtime_r(&sec, &tmv);
  char b[40];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S%z", &tmv);
  return String(b);
}

struct StatusSnapshot
{
  uint64_t ts_epoch_ms = 0;
  float uteluft = NAN;
  float tilluft = NAN;
  float avtrekk = NAN;
  float avkast = NAN;
  int fan = 0;
  int heat = 0;
  int efficiency = 0;
  char mode[16] = {0};
  char modbus[32] = {0};
  bool stale = false;
};

static const size_t HISTORY_CAP = 720; // up to 12h at 60s polling, more at longer intervals
static StatusSnapshot g_hist[HISTORY_CAP];
static size_t g_hist_head = 0;
static size_t g_hist_count = 0;
static uint32_t g_diag_total_updates = 0;
static uint32_t g_diag_mb_ok = 0;
static uint32_t g_diag_mb_err = 0;
static uint32_t g_diag_mb_off = 0;
static uint32_t g_diag_stale = 0;
static String g_diag_last_mb = "MB OFF";
static uint64_t g_diag_last_sample_ms = 0;

static uint32_t historyMemoryBytes()
{
  return (uint32_t)(sizeof(StatusSnapshot) * HISTORY_CAP);
}

static void historyPush(const FlexitData& d, const String& mbStatus)
{
  StatusSnapshot s;
  s.ts_epoch_ms = nowEpochMs();
  s.uteluft = d.uteluft;
  s.tilluft = d.tilluft;
  s.avtrekk = d.avtrekk;
  s.avkast = d.avkast;
  s.fan = d.fan_percent;
  s.heat = d.heat_element_percent;
  s.efficiency = d.efficiency_percent;
  strncpy(s.mode, d.mode.c_str(), sizeof(s.mode) - 1);
  strncpy(s.modbus, mbStatus.c_str(), sizeof(s.modbus) - 1);
  s.stale = (mbStatus.indexOf("stale") >= 0);

  g_hist[g_hist_head] = s;
  g_hist_head = (g_hist_head + 1) % HISTORY_CAP;
  if (g_hist_count < HISTORY_CAP) g_hist_count++;
}


static String jsonEscape(const String& s)
{
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    switch (c)
    {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20)
        {
          char b[7];
          snprintf(b, sizeof(b), "\\u%04x", (unsigned int)(unsigned char)c);
          out += b;
        }
        else
        {
          out += c;
        }
        break;
    }
  }
  return out;
}

static String u64ToString(uint64_t v)
{
  char b[24];
  snprintf(b, sizeof(b), "%llu", (unsigned long long)v);
  return String(b);
}

static String normModel(const String& in)
{
  if (in == "S2_EXP") return "S2_EXP";
  if (in == "S4") return "S4";
  if (in == "S7_EXP") return "S7_EXP";
  if (in == "CL3_EXP") return "CL3_EXP";
  if (in == "CL4_EXP") return "CL4_EXP";
  return "S3";
}

static String normDataSource(const String& in)
{
  if (in == "BACNET") return "BACNET";
  return "MODBUS";
}

static String dataSourceLabel(const String& src)
{
  if (normDataSource(src) == "BACNET") return "BACnet (local)";
  return "Modbus (local)";
}

static String normLang(const String& in)
{
  if (in == "en" || in == "no" || in == "da" || in == "sv" || in == "fi" || in == "uk") return in;
  return "no";
}

static String lang()
{
  if (!g_cfg) return "no";
  return normLang(g_cfg->ui_language);
}

static String tr(const char* key)
{
  const String l = lang();
  const bool en = (l == "en");
  const bool no = (l == "no");
  const bool da = (l == "da");
  const bool sv = (l == "sv");
  const bool fi = (l == "fi");
  const bool uk = (l == "uk");

  if (strcmp(key, "settings") == 0) return en ? "Settings" : no ? "Innstillinger" : da ? "Indstillinger" : sv ? "Inställningar" : fi ? "Asetukset" : "Налаштування";
  if (strcmp(key, "wifi") == 0) return en ? "WiFi" : no ? "WiFi" : da ? "WiFi" : sv ? "WiFi" : fi ? "WiFi" : "WiFi";
  if (strcmp(key, "model") == 0) return en ? "Device model" : no ? "Enhetsmodell" : da ? "Enhedsmodel" : sv ? "Enhetsmodell" : fi ? "Laitemalli" : "Модель пристрою";
  if (strcmp(key, "homey_api") == 0) return en ? "Homey/API" : no ? "Homey/API" : da ? "Homey/API" : sv ? "Homey/API" : fi ? "Homey/API" : "Homey/API";
  if (strcmp(key, "ha_api") == 0) return en ? "Home Assistant/API" : no ? "Home Assistant/API" : da ? "Home Assistant/API" : sv ? "Home Assistant/API" : fi ? "Home Assistant/API" : "Home Assistant/API";
  if (strcmp(key, "headless") == 0) return en ? "Headless mode (no display mounted)" : no ? "Headless-modus (skjerm ikke montert)" : da ? "Headless-tilstand (ingen skærm monteret)" : sv ? "Headless-lage (ingen skarm monterad)" : fi ? "Headless-tila (ei nayttoa asennettu)" : "Headless режим (без екрана)";
  if (strcmp(key, "ha_mqtt") == 0) return en ? "HA MQTT Discovery (native)" : no ? "HA MQTT Discovery (native)" : da ? "HA MQTT Discovery (native)" : sv ? "HA MQTT Discovery (native)" : fi ? "HA MQTT Discovery (native)" : "HA MQTT Discovery (native)";
  if (strcmp(key, "ha_mqtt_help") == 0) return en ? "Native HA entities via standard MQTT discovery. No custom integration needed." : no ? "Native HA-entities via standard MQTT discovery. Ingen custom-integrasjon trengs." : da ? "Native HA-enheder via standard MQTT discovery. Ingen custom-integration nodvendig." : sv ? "Native HA-entiteter via standard MQTT discovery. Ingen custom-integration behovs." : fi ? "Natiivit HA-entiteetit standardin MQTT discoveryn kautta. Ei custom-integraatiota." : "Нативні сутності HA через стандартний MQTT discovery.";
  if (strcmp(key, "ha_mqtt_host") == 0) return en ? "MQTT broker host/IP" : no ? "MQTT broker host/IP" : da ? "MQTT broker host/IP" : sv ? "MQTT broker host/IP" : fi ? "MQTT-valittajan host/IP" : "MQTT broker host/IP";
  if (strcmp(key, "ha_mqtt_port") == 0) return en ? "MQTT port" : no ? "MQTT-port" : da ? "MQTT-port" : sv ? "MQTT-port" : fi ? "MQTT-portti" : "MQTT порт";
  if (strcmp(key, "ha_mqtt_user") == 0) return en ? "MQTT username (optional)" : no ? "MQTT brukernavn (valgfritt)" : da ? "MQTT brugernavn (valgfrit)" : sv ? "MQTT anvandarnamn (valfritt)" : fi ? "MQTT-kayttajanimi (valinnainen)" : "MQTT ім'я користувача (необов'язково)";
  if (strcmp(key, "ha_mqtt_pass") == 0) return en ? "MQTT password (optional)" : no ? "MQTT passord (valgfritt)" : da ? "MQTT adgangskode (valgfrit)" : sv ? "MQTT losenord (valfritt)" : fi ? "MQTT-salasana (valinnainen)" : "MQTT пароль (необов'язково)";
  if (strcmp(key, "ha_mqtt_prefix") == 0) return en ? "Discovery prefix" : no ? "Discovery-prefix" : da ? "Discovery-prefix" : sv ? "Discovery-prefix" : fi ? "Discovery-etuliite" : "Префікс discovery";
  if (strcmp(key, "ha_mqtt_base") == 0) return en ? "State topic base" : no ? "State topic base" : da ? "State topic base" : sv ? "State topic base" : fi ? "State topic -perusta" : "Базовий topic стану";
  if (strcmp(key, "ha_mqtt_interval") == 0) return en ? "MQTT publish interval (sec)" : no ? "MQTT publiseringsintervall (sek)" : da ? "MQTT publiceringsinterval (sek)" : sv ? "MQTT publiceringsintervall (sek)" : fi ? "MQTT-julkaisuväli (s)" : "Інтервал публікації MQTT (с)";
  if (strcmp(key, "ha_mqtt_status") == 0) return en ? "HA MQTT status" : no ? "HA MQTT-status" : da ? "HA MQTT-status" : sv ? "HA MQTT-status" : fi ? "HA MQTT-tila" : "Статус HA MQTT";
  if (strcmp(key, "modbus") == 0) return en ? "Modbus" : no ? "Modbus" : da ? "Modbus" : sv ? "Modbus" : fi ? "Modbus" : "Modbus";
  if (strcmp(key, "advanced_modbus") == 0) return en ? "Advanced Modbus settings" : no ? "Avanserte Modbus-innstillinger" : da ? "Avancerede Modbus-indstillinger" : sv ? "Avancerade Modbus-inställningar" : fi ? "Edistyneet Modbus-asetukset" : "Розширені налаштування Modbus";
  if (strcmp(key, "save") == 0) return en ? "Save" : no ? "Lagre" : da ? "Gem" : sv ? "Spara" : fi ? "Tallenna" : "Зберегти";
  if (strcmp(key, "back") == 0) return en ? "Back" : no ? "Tilbake" : da ? "Tilbage" : sv ? "Tillbaka" : fi ? "Takaisin" : "Назад";
  if (strcmp(key, "setup") == 0) return en ? "Setup" : no ? "Oppsett" : da ? "Opsætning" : sv ? "Installation" : fi ? "Asennus" : "Налаштування";
  if (strcmp(key, "next") == 0) return en ? "Next" : no ? "Neste" : da ? "Næste" : sv ? "Nästa" : fi ? "Seuraava" : "Далі";
  if (strcmp(key, "complete_restart") == 0) return en ? "Complete & restart" : no ? "Fullfør & restart" : da ? "Fuldfør & genstart" : sv ? "Slutför & starta om" : fi ? "Valmis & käynnistä uudelleen" : "Завершити та перезапустити";
  if (strcmp(key, "poll_sec") == 0) return en ? "Update interval (sec)" : no ? "Oppdateringsintervall (sek)" : da ? "Opdateringsinterval (sek)" : sv ? "Uppdateringsintervall (sek)" : fi ? "Päivitysväli (s)" : "Інтервал оновлення (с)";
  if (strcmp(key, "poll_help") == 0) return en ? "Display refresh interval. Minimum 180s when display is enabled; headless can use shorter intervals." : no ? "Oppdateringsintervall for display. Minimum 180 sek når display er aktivert; headless kan bruke kortere intervall." : da ? "Opdateringsinterval for display. Minimum 180 sek nar display er aktiveret; headless kan bruge kortere intervaller." : sv ? "Uppdateringsintervall for display. Minimum 180 sek nar display ar aktiverad; headless kan anvanda kortare intervall." : fi ? "Nayton paivitysvali. Vahintaan 180 s kun naytto on kaytossa; headless voi kayttaa lyhyempaa valia." : "Інтервал оновлення екрана. Мінімум 180 с, коли дисплей увімкнено; у headless можна менше.";
  if (strcmp(key, "data_source") == 0) return en ? "Data source" : no ? "Datakilde" : da ? "Datakilde" : sv ? "Datakälla" : fi ? "Tietolähde" : "Джерело даних";
  if (strcmp(key, "source_modbus") == 0) return en ? "Modbus (experimental, local)" : no ? "Modbus (eksperimentell, lokal)" : da ? "Modbus (eksperimentel, lokal)" : sv ? "Modbus (experimentell, lokal)" : fi ? "Modbus (kokeellinen, paikallinen)" : "Modbus (експериментально, локально)";
  if (strcmp(key, "source_bacnet") == 0) return en ? "BACnet (local)" : no ? "BACnet (lokal)" : da ? "BACnet (lokal)" : sv ? "BACnet (lokal)" : fi ? "BACnet (paikallinen)" : "BACnet (локально)";
  if (strcmp(key, "source_bacnet_help") == 0) return en ? "Uses local BACnet/IP over LAN. Read is production-ready; writes are optional/experimental." : no ? "Bruker lokal BACnet/IP i LAN. Lesing er produksjonsklar; skriving er valgfri/eksperimentell." : da ? "Bruger lokal BACnet/IP pa LAN. Laesning er produktionsklar; skrivning er valgfri/eksperimentel." : sv ? "Anvander lokal BACnet/IP over LAN. Lasning ar produktionsklar; skrivning ar valfri/experimentell." : fi ? "Kayttaa paikallista BACnet/IP-yhteytta LAN-verkossa. Luku on tuotantovalmis; kirjoitus on valinnainen/kokeellinen." : "Використовує локальний BACnet/IP у LAN. Читання готове до продакшну; запис необов'язковий/експериментальний.";
  if (strcmp(key, "source_bacnet_default_map_hint") == 0) return en ? "Default object mapping is preconfigured for most users. Run scan/probe only if values look wrong." : no ? "Standard objektmapping er forhåndsutfylt for de fleste. Kjør scan/probe kun hvis verdiene ser feil ut." : da ? "Standard objektmapping er forudfyldt for de fleste. Kor scan/probe kun hvis vaerdierne ser forkerte ud." : sv ? "Standard objektmappning ar forifylld for de flesta. Kor scan/probe endast om vardena ser fel ut." : fi ? "Oletusobjektikartoitus on esiasetettu useimmille. Kayta scan/probe-toimintoja vain, jos arvot nayttavat vaarilta." : "Стандартне зіставлення об'єктів уже задане для більшості. Запускайте scan/probe лише якщо значення виглядають неправильними.";
  if (strcmp(key, "bac_ip") == 0) return en ? "BACnet device IP" : no ? "BACnet enhets-IP" : da ? "BACnet enheds-IP" : sv ? "BACnet enhets-IP" : fi ? "BACnet laitteen IP" : "IP пристрою BACnet";
  if (strcmp(key, "bac_device_id") == 0) return en ? "BACnet Device ID" : no ? "BACnet Device ID" : da ? "BACnet Device ID" : sv ? "BACnet Device ID" : fi ? "BACnet Device ID" : "BACnet Device ID";
  if (strcmp(key, "bac_port") == 0) return en ? "UDP port" : no ? "UDP-port" : da ? "UDP-port" : sv ? "UDP-port" : fi ? "UDP-portti" : "UDP-порт";
  if (strcmp(key, "bac_poll_min") == 0) return en ? "BACnet polling interval (minutes, 5-60)" : no ? "BACnet polling-intervall (minutter, 5-60)" : da ? "BACnet polling-interval (minutter, 5-60)" : sv ? "BACnet pollingintervall (minuter, 5-60)" : fi ? "BACnet-pollausvali (minuuttia, 5-60)" : "Інтервал опитування BACnet (хвилини, 5-60)";
  if (strcmp(key, "bac_timeout") == 0) return en ? "Request timeout (ms, 300-8000)" : no ? "Timeout per forespørsel (ms, 300-8000)" : da ? "Timeout per foresporgsel (ms, 300-8000)" : sv ? "Timeout per forfragan (ms, 300-8000)" : fi ? "Aikakatkaisu per pyynto (ms, 300-8000)" : "Таймаут запиту (мс, 300-8000)";
  if (strcmp(key, "bac_objects") == 0) return en ? "BACnet object mapping (advanced)" : no ? "BACnet objektmapping (avansert)" : da ? "BACnet objektmapping (avanceret)" : sv ? "BACnet objektmapping (avancerad)" : fi ? "BACnet-objektikartoitus (edistynyt)" : "Мапінг об'єктів BACnet (розширено)";
  if (strcmp(key, "bac_mode_map") == 0) return en ? "Mode enum map" : no ? "Modus enum-mapping" : da ? "Tilstand enum-mapping" : sv ? "Lagesmapping (enum)" : fi ? "Tilakartta (enum)" : "Мапа режимів (enum)";
  if (strcmp(key, "bac_write_enable") == 0) return en ? "Enable BACnet writes (experimental)" : no ? "Aktiver BACnet-skriving (eksperimentell)" : da ? "Aktiver BACnet-skrivning (eksperimentel)" : sv ? "Aktivera BACnet-skrivning (experimentell)" : fi ? "Ota BACnet-kirjoitus kayttoon (kokeellinen)" : "Увімкнути запис BACnet (експериментально)";
  if (strcmp(key, "bac_setpoint_home_obj") == 0) return en ? "Setpoint object (home profile)" : no ? "Settpunkt-objekt (hjem-profil)" : da ? "Setpunkt-objekt (hjem-profil)" : sv ? "Borvarde-objekt (hem-profil)" : fi ? "Asetusarvo-objekti (koti-profiili)" : "Об'єкт уставки (профіль home)";
  if (strcmp(key, "bac_setpoint_away_obj") == 0) return en ? "Setpoint object (away profile)" : no ? "Settpunkt-objekt (borte-profil)" : da ? "Setpunkt-objekt (ude-profil)" : sv ? "Borvarde-objekt (borta-profil)" : fi ? "Asetusarvo-objekti (poissa-profiili)" : "Об'єкт уставки (профіль away)";
  if (strcmp(key, "language") == 0) return en ? "Language" : no ? "Språk" : da ? "Sprog" : sv ? "Språk" : fi ? "Kieli" : "Мова";
  if (strcmp(key, "control_enable") == 0) return en ? "Enable remote control writes (experimental)" : no ? "Aktiver fjernstyring med skriv (experimental)" : da ? "Aktivér fjernstyring med skriv (experimental)" : sv ? "Aktivera fjärrstyrning med skrivning (experimental)" : fi ? "Salli etäohjaus kirjoituksilla (experimental)" : "Увімкнути віддалене керування записом (experimental)";
  if (strcmp(key, "admin") == 0) return en ? "Admin" : no ? "Admin" : da ? "Admin" : sv ? "Admin" : fi ? "Admin" : "Адмін";
  if (strcmp(key, "restart") == 0) return en ? "Restart" : no ? "Restart" : da ? "Genstart" : sv ? "Starta om" : fi ? "Kaynnista uudelleen" : "Перезапуск";
  if (strcmp(key, "factory_reset") == 0) return en ? "Factory reset" : no ? "Factory reset" : da ? "Fabriksnulstilling" : sv ? "Fabriksaterstallning" : fi ? "Tehdasasetusten palautus" : "Скидання до заводських";
  if (strcmp(key, "restart_now") == 0) return en ? "Restart device now" : no ? "Restart enheten nå" : da ? "Genstart enheden nu" : sv ? "Starta om enheten nu" : fi ? "Kaynnista laite uudelleen nyt" : "Перезапустити пристрій зараз";
  if (strcmp(key, "admin_portal") == 0) return en ? "Admin portal" : no ? "Admin portal" : da ? "Admin portal" : sv ? "Adminportal" : fi ? "Yllapitoportaali" : "Адмін портал";
  if (strcmp(key, "status") == 0) return en ? "Status" : no ? "Status" : da ? "Status" : sv ? "Status" : fi ? "Tila" : "Статус";
  if (strcmp(key, "actions") == 0) return en ? "Actions" : no ? "Handlinger" : da ? "Handlinger" : sv ? "Atgarder" : fi ? "Toiminnot" : "Дії";
  if (strcmp(key, "open_admin") == 0) return en ? "Open admin" : no ? "Åpne admin" : da ? "Aben admin" : sv ? "Oppna admin" : fi ? "Avaa admin" : "Відкрити адмін";
  if (strcmp(key, "admin_protected") == 0) return en ? "Admin is password protected. On first login you must change password." : no ? "Admin er beskyttet med passord. Ved første innlogging må du endre passord." : da ? "Admin er adgangskodebeskyttet. Ved første login skal du ændre adgangskode." : sv ? "Admin ar losenskyddad. Vid forsta inloggning maste du byta losenord." : fi ? "Admin on suojattu salasanalla. Vaihda salasana ensimmaisella kirjautumisella." : "Адмін захищений паролем. Під час першого входу змініть пароль.";
  if (strcmp(key, "manual") == 0) return en ? "Manual" : no ? "Brukermanual" : da ? "Brugermanual" : sv ? "Manual" : fi ? "Kayttoopas" : "Інструкція";
  if (strcmp(key, "back_home") == 0) return en ? "Home" : no ? "Til start" : da ? "Til start" : sv ? "Till startsida" : fi ? "Etusivu" : "На головну";
  if (strcmp(key, "saved") == 0) return en ? "Saved" : no ? "Lagret" : da ? "Gemt" : sv ? "Sparat" : fi ? "Tallennettu" : "Збережено";
  if (strcmp(key, "password_updated") == 0) return en ? "Password updated. Log in again with your new password." : no ? "Passord er oppdatert. Logg inn på nytt med nytt passord." : da ? "Adgangskode opdateret. Log ind igen med ny adgangskode." : sv ? "Losenord uppdaterat. Logga in igen med nytt losenord." : fi ? "Salasana paivitetty. Kirjaudu uudelleen uudella salasanalla." : "Пароль оновлено. Увійдіть з новим паролем.";
  if (strcmp(key, "settings_saved_restart_if_needed") == 0) return en ? "Settings saved. Restart the device if WiFi/network settings were changed." : no ? "Innstillinger er lagret. Restart enheten hvis WiFi eller nettverksvalg er endret." : da ? "Indstillinger gemt. Genstart enheden hvis WiFi/netværk er ændret." : sv ? "Installningar sparade. Starta om enheten om WiFi/natverk andrats." : fi ? "Asetukset tallennettu. Kaynnista laite uudelleen jos WiFi/verkko muuttui." : "Налаштування збережено. Перезапустіть пристрій, якщо змінено WiFi/мережу.";
  if (strcmp(key, "to_admin") == 0) return en ? "Back to admin" : no ? "Til admin" : da ? "Til admin" : sv ? "Till admin" : fi ? "Takaisin adminiin" : "До адмін";
  if (strcmp(key, "restarting_now") == 0) return en ? "Device is restarting now." : no ? "Enheten restarter nå." : da ? "Enheden genstarter nu." : sv ? "Enheten startar om nu." : fi ? "Laite kaynnistyy uudelleen nyt." : "Пристрій перезапускається.";
  if (strcmp(key, "factory_reset_now") == 0) return en ? "Configuration is being erased. Device will restart now." : no ? "Konfigurasjon slettes. Enheten restarter nå." : da ? "Konfiguration slettes. Enheden genstarter nu." : sv ? "Konfiguration raderas. Enheten startar om nu." : fi ? "Asetukset poistetaan. Laite kaynnistyy uudelleen nyt." : "Конфігурацію видаляють. Пристрій перезапускається.";
  if (strcmp(key, "ota") == 0) return en ? "OTA" : no ? "OTA" : da ? "OTA" : sv ? "OTA" : fi ? "OTA" : "OTA";
  if (strcmp(key, "ota_upload_title") == 0) return en ? "OTA via file upload (.bin)" : no ? "OTA via filopplasting (.bin)" : da ? "OTA via filupload (.bin)" : sv ? "OTA via filuppladdning (.bin)" : fi ? "OTA tiedoston latauksella (.bin)" : "OTA через завантаження файлу (.bin)";
  if (strcmp(key, "ota_upload_desc") == 0) return en ? "Upload ESP32 firmware file directly from browser." : no ? "Last opp firmwarefil for ESP32 direkte fra nettleser." : da ? "Upload firmwarefil til ESP32 direkte fra browser." : sv ? "Ladda upp firmwarefil for ESP32 direkt fran webblasare." : fi ? "Lataa ESP32-laiteohjelma selaimesta." : "Завантажте прошивку ESP32 з браузера.";
  if (strcmp(key, "firmware_file") == 0) return en ? "Firmware file (.bin)" : no ? "Firmware-fil (.bin)" : da ? "Firmware-fil (.bin)" : sv ? "Firmware-fil (.bin)" : fi ? "Laiteohjelmatiedosto (.bin)" : "Файл прошивки (.bin)";
  if (strcmp(key, "start_update") == 0) return en ? "Start update" : no ? "Start oppdatering" : da ? "Start opdatering" : sv ? "Starta uppdatering" : fi ? "Aloita paivitys" : "Почати оновлення";
  if (strcmp(key, "ota_restart_done") == 0) return en ? "Device restarts automatically when update is done." : no ? "Enheten restarter automatisk når oppdateringen er ferdig." : da ? "Enheden genstarter automatisk nar opdatering er faerdig." : sv ? "Enheten startar om automatiskt nar uppdateringen ar klar." : fi ? "Laite kaynnistyy automaattisesti paivityksen jalkeen." : "Пристрій автоматично перезапуститься після оновлення.";
  if (strcmp(key, "ota_alt_title") == 0) return en ? "Alternative: Arduino OTA" : no ? "Alternativ: Arduino OTA" : da ? "Alternativ: Arduino OTA" : sv ? "Alternativ: Arduino OTA" : fi ? "Vaihtoehto: Arduino OTA" : "Альтернатива: Arduino OTA";
  if (strcmp(key, "ota_alt_desc") == 0) return en ? "You can still use Arduino IDE network port if preferred." : no ? "Du kan fortsatt bruke Arduino IDE nettverksport dersom ønskelig." : da ? "Du kan stadig bruge Arduino IDE netvaerksport, hvis onsket." : sv ? "Du kan fortfarande anvanda Arduino IDE natverksport om du vill." : fi ? "Voit edelleen kayttaa Arduino IDE -verkkoporttia halutessasi." : "За бажанням можна використовувати мережевий порт Arduino IDE.";
  if (strcmp(key, "ota_failed") == 0) return en ? "OTA failed" : no ? "OTA feil" : da ? "OTA fejl" : sv ? "OTA fel" : fi ? "OTA virhe" : "Помилка OTA";
  if (strcmp(key, "back_to_ota") == 0) return en ? "Back to OTA" : no ? "Tilbake til OTA" : da ? "Tilbage til OTA" : sv ? "Tillbaka till OTA" : fi ? "Takaisin OTA:han" : "Назад до OTA";
  if (strcmp(key, "ota_ok") == 0) return en ? "OTA OK" : no ? "OTA OK" : da ? "OTA OK" : sv ? "OTA OK" : fi ? "OTA OK" : "OTA OK";
  if (strcmp(key, "ota_ok_restart") == 0) return en ? "Firmware updated. Device is restarting now." : no ? "Firmware er oppdatert. Enheten restarter nå." : da ? "Firmware er opdateret. Enheden genstarter nu." : sv ? "Firmware ar uppdaterad. Enheten startar om nu." : fi ? "Laiteohjelma paivitetty. Laite kaynnistyy uudelleen nyt." : "Прошивку оновлено. Пристрій перезапускається.";
  if (strcmp(key, "manual_subtitle") == 0) return en ? "Short changelog + simple guide" : no ? "Kort changelog + enkel veiledning" : da ? "Kort changelog + enkel guide" : sv ? "Kort changelog + enkel guide" : fi ? "Lyhyt changelog + yksinkertainen opas" : "Короткий changelog + проста інструкція";
  if (strcmp(key, "changelog_short") == 0) return en ? "Changelog (short)" : no ? "Changelog (kort)" : da ? "Changelog (kort)" : sv ? "Changelog (kort)" : fi ? "Changelog (lyhyt)" : "Changelog (коротко)";
  if (strcmp(key, "manual_simple") == 0) return en ? "Manual (simple)" : no ? "Brukermanual (forenklet)" : da ? "Brugermanual (forenklet)" : sv ? "Manual (forenklad)" : fi ? "Kayttoopas (yksinkertainen)" : "Інструкція (спрощено)";
  if (strcmp(key, "to_admin_page") == 0) return en ? "Back to admin" : no ? "Tilbake til admin" : da ? "Tilbage til admin" : sv ? "Tillbaka till admin" : fi ? "Takaisin adminiin" : "До адмін";
  if (strcmp(key, "graphs") == 0) return en ? "Graphs" : no ? "Grafer" : da ? "Grafer" : sv ? "Grafer" : fi ? "Kaaviot" : "Графіки";
  if (strcmp(key, "quick_control") == 0) return en ? "Quick control" : no ? "Hurtigstyring" : da ? "Hurtigstyring" : sv ? "Snabbstyrning" : fi ? "Pikaohjaus" : "Швидке керування";
  if (strcmp(key, "quick_control_help") == 0) return en ? "Writes mode and setpoint through the active data source (Modbus or BACnet)." : no ? "Skriver modus og settpunkt via aktiv datakilde (Modbus eller BACnet)." : da ? "Skriver tilstand og setpunkt via aktiv datakilde (Modbus eller BACnet)." : sv ? "Skriver lage och borvarde via aktiv datakalla (Modbus eller BACnet)." : fi ? "Kirjoittaa tilan ja asetusarvon aktiivisen datalahteen kautta (Modbus tai BACnet)." : "Записує режим і уставку через активне джерело даних (Modbus або BACnet).";
  if (strcmp(key, "quick_control_bacnet_hint") == 0) return en ? "BACnet write policy can vary by unit. If write fails, use Object/Write probe and verify writable objects." : no ? "BACnet-skrivepolicy varierer per enhet. Hvis skriving feiler, bruk objekt-/write-probe og verifiser skrivbare objekter." : da ? "BACnet-skrivepolitik varierer per enhed. Hvis skrivning fejler, brug objekt-/write-probe og verificer skrivbare objekter." : sv ? "BACnet-skrivpolicy varierar per enhet. Om skrivning misslyckas, anvand objekt-/write-probe och verifiera skrivbara objekt." : fi ? "BACnet-kirjoitusoikeudet vaihtelevat laitteittain. Jos kirjoitus epaonnistuu, kayta objekti-/write-probea ja varmista kirjoitettavat objektit." : "Політика запису BACnet залежить від пристрою. Якщо запис не вдається, використайте Object/Write probe і перевірте об'єкти для запису.";
  if (strcmp(key, "quick_control_bacnet_mode_locked") == 0) return en ? "Mode write is currently locked by Flexit and unavailable." : no ? "Modus-skriving er foreløpig låst av Flexit og utilgjengelig." : da ? "Tilstandsskrivning er i øjeblikket låst af Flexit og utilgængelig." : sv ? "Lageskrivning ar for narvarande last av Flexit och inte tillganglig." : fi ? "Tilan kirjoitus on toistaiseksi Flexitin lukitsema eika ole kaytettavissa." : "Запис режиму наразі заблокований Flexit і недоступний.";
  if (strcmp(key, "enable_control_hint") == 0) return en ? "Enable write control for the active source to use quick control." : no ? "Aktiver skrivekontroll for aktiv kilde for å bruke hurtigstyring." : da ? "Aktiver skrivekontrol for aktiv kilde for at bruge hurtigstyring." : sv ? "Aktivera skrivstyrning for aktiv kalla for att anvanda snabbstyrning." : fi ? "Ota aktiivisen lahteen kirjoitusohjaus kayttoon kayttaaksesi pikaohjausta." : "Увімкніть керування записом для активного джерела, щоб використовувати швидке керування.";
  if (strcmp(key, "mode_away") == 0) return en ? "Away" : no ? "Borte" : da ? "Ude" : sv ? "Borta" : fi ? "Poissa" : "Away";
  if (strcmp(key, "mode_home") == 0) return en ? "Home" : no ? "Hjem" : da ? "Hjemme" : sv ? "Hemma" : fi ? "Koti" : "Home";
  if (strcmp(key, "mode_high") == 0) return en ? "High" : no ? "High" : da ? "Hoj" : sv ? "Hog" : fi ? "Teho" : "High";
  if (strcmp(key, "mode_fire") == 0) return en ? "Fireplace" : no ? "Peis" : da ? "Pejs" : sv ? "Kamin" : fi ? "Takka" : "Fireplace";
  if (strcmp(key, "profile") == 0) return en ? "Profile" : no ? "Profil" : da ? "Profil" : sv ? "Profil" : fi ? "Profiili" : "Профіль";
  if (strcmp(key, "setpoint") == 0) return en ? "Setpoint (10..30 C)" : no ? "Settpunkt (10..30 C)" : da ? "Setpunkt (10..30 C)" : sv ? "Borvarde (10..30 C)" : fi ? "Asetusarvo (10..30 C)" : "Уставка (10..30 C)";
  if (strcmp(key, "apply_setpoint") == 0) return en ? "Apply setpoint" : no ? "Bruk settpunkt" : da ? "Anvend setpunkt" : sv ? "Applicera borvarde" : fi ? "Aseta arvo" : "Застосувати уставку";
  if (strcmp(key, "history_graphs_title") == 0) return en ? "History graphs" : no ? "Historikk-grafer" : da ? "Historik-grafer" : sv ? "Historik-grafer" : fi ? "Historia-kaaviot" : "Графіки історії";
  if (strcmp(key, "history_graphs_subtitle") == 0) return en ? "Local trends from /status/history" : no ? "Lokale trender fra /status/history" : da ? "Lokale trends fra /status/history" : sv ? "Lokala trender fran /status/history" : fi ? "Paikalliset trendit /status/history:sta" : "Локальні тренди з /status/history";
  if (strcmp(key, "refresh") == 0) return en ? "Refresh" : no ? "Oppdater" : da ? "Opdater" : sv ? "Uppdatera" : fi ? "Paivita" : "Оновити";
  if (strcmp(key, "history_limit") == 0) return en ? "Points" : no ? "Punkter" : da ? "Punkter" : sv ? "Punkter" : fi ? "Pisteet" : "Точки";
  if (strcmp(key, "loading") == 0) return en ? "Loading..." : no ? "Laster..." : da ? "Indlaeser..." : sv ? "Laddar..." : fi ? "Ladataan..." : "Завантаження...";
  if (strcmp(key, "mdns_return_hint") == 0) return en ? "If mDNS is enabled on your network, click below to go to your device." : no ? "Om du har mDNS aktivert i nettverket ditt, klikk under for å gå til enheten din." : da ? "Hvis mDNS er aktiveret i dit netværk, klik nedenfor for at gå til din enhed." : sv ? "Om mDNS är aktiverat i ditt nätverk, klicka nedan för att gå till din enhet." : fi ? "Jos mDNS on käytössä verkossasi, klikkaa alta siirtyäksesi laitteellesi." : "Якщо mDNS увімкнено у вашій мережі, натисніть нижче, щоб перейти до вашого пристрою.";
  if (strcmp(key, "mdns_open_device") == 0) return en ? "Open device via mDNS" : no ? "Åpne enheten via mDNS" : da ? "Åbn enheden via mDNS" : sv ? "Öppna enheten via mDNS" : fi ? "Avaa laite mDNS:n kautta" : "Відкрити пристрій через mDNS";
  if (strcmp(key, "temp_graph") == 0) return en ? "Temperatures" : no ? "Temperaturer" : da ? "Temperaturer" : sv ? "Temperaturer" : fi ? "Lampotilat" : "Температури";
  if (strcmp(key, "perf_graph") == 0) return en ? "Performance" : no ? "Ytelse" : da ? "Ydelse" : sv ? "Prestanda" : fi ? "Suorituskyky" : "Продуктивність";
  if (strcmp(key, "export_csv") == 0) return en ? "Export CSV" : no ? "Eksporter CSV" : da ? "Eksporter CSV" : sv ? "Exportera CSV" : fi ? "Vie CSV" : "Експорт CSV";
  if (strcmp(key, "storage_status") == 0) return en ? "Storage status" : no ? "Lagringsstatus" : da ? "Lagerstatus" : sv ? "Lagringsstatus" : fi ? "Tallennustila" : "Стан сховища";
  if (strcmp(key, "history_ram_only") == 0) return en ? "History uses fixed RAM ring-buffer only (no flash writes)." : no ? "Historikk bruker kun fast RAM-ringbuffer (ingen flash-skriving)." : da ? "Historik bruger kun fast RAM-ringbuffer (ingen flash-skrivning)." : sv ? "Historik anvander endast fast RAM-ringbuffert (ingen flash-skrivning)." : fi ? "Historia kayttaa vain kiinteaa RAM-rengaspuskuria (ei flash-kirjoitusta)." : "Історія використовує лише фіксований RAM-буфер (без запису у flash).";
  if (strcmp(key, "history_points") == 0) return en ? "History points" : no ? "Historikkpunkter" : da ? "Historikpunkter" : sv ? "Historikpunkter" : fi ? "Historian pisteet" : "Точки історії";
  if (strcmp(key, "history_mem") == 0) return en ? "History memory" : no ? "Historikkminne" : da ? "Historikhukommelse" : sv ? "Historikminne" : fi ? "Historiamuisti" : "Пам'ять історії";
  if (strcmp(key, "heap_free") == 0) return en ? "Free heap" : no ? "Ledig heap" : da ? "Ledig heap" : sv ? "Ledigt heap" : fi ? "Vapaa heap" : "Вільна heap";
  if (strcmp(key, "heap_min") == 0) return en ? "Min free heap" : no ? "Min ledig heap" : da ? "Min ledig heap" : sv ? "Min ledigt heap" : fi ? "Min vapaa heap" : "Мін. вільна heap";
  if (strcmp(key, "legend") == 0) return en ? "Legend (click to toggle)" : no ? "Forklaring (klikk for av/på)" : da ? "Forklaring (klik for til/fra)" : sv ? "Forklaring (klicka for pa/av)" : fi ? "Selite (napsauta paalle/pois)" : "Легенда (натисніть для вкл/викл)";
  return String(key);
}

static String normTransport(const String& in)
{
  if (in == "MANUAL") return "MANUAL";
  return "AUTO";
}

static String normSerialFmt(const String& in)
{
  if (in == "8E1") return "8E1";
  if (in == "8O1") return "8O1";
  return "8N1";
}

static void applyPostedModbusSettings()
{
  if (server.hasArg("mbtr")) g_cfg->modbus_transport_mode = normTransport(server.arg("mbtr"));
  if (server.hasArg("mbser")) g_cfg->modbus_serial_format = normSerialFmt(server.arg("mbser"));

  if (server.hasArg("mbbaud"))
  {
    uint32_t mbBaud = (uint32_t)server.arg("mbbaud").toInt();
    if (mbBaud >= 1200 && mbBaud <= 115200) g_cfg->modbus_baud = mbBaud;
  }

  if (server.hasArg("mbid"))
  {
    int mbId = server.arg("mbid").toInt();
    if (mbId >= 1 && mbId <= 247) g_cfg->modbus_slave_id = (uint8_t)mbId;
  }

  if (server.hasArg("mboff"))
  {
    int mbOff = server.arg("mboff").toInt();
    if (mbOff < -5) mbOff = -5;
    if (mbOff > 5) mbOff = 5;
    g_cfg->modbus_addr_offset = (int8_t)mbOff;
  }
}

static void applyPostedBACnetSettings()
{
  g_cfg->bacnet_write_enabled = server.hasArg("bacwr");
  if (server.hasArg("bacip"))
  {
    String s = server.arg("bacip");
    s.trim();
    g_cfg->bacnet_ip = s;
  }
  if (server.hasArg("bacport"))
  {
    int p = server.arg("bacport").toInt();
    if (p < 1) p = 1;
    if (p > 65535) p = 65535;
    g_cfg->bacnet_port = (uint16_t)p;
  }
  if (server.hasArg("bacid"))
  {
    uint32_t id = (uint32_t)server.arg("bacid").toInt();
    g_cfg->bacnet_device_id = id;
  }
  if (server.hasArg("bacpoll"))
  {
    int m = server.arg("bacpoll").toInt();
    if (m < 5) m = 5;
    if (m > 60) m = 60;
    g_cfg->bacnet_poll_minutes = (uint8_t)m;
  }
  if (server.hasArg("bacto"))
  {
    int t = server.arg("bacto").toInt();
    if (t < 300) t = 300;
    if (t > 8000) t = 8000;
    g_cfg->bacnet_timeout_ms = (uint16_t)t;
  }
  if (server.hasArg("baout")) g_cfg->bacnet_obj_outdoor = server.arg("baout");
  if (server.hasArg("basup")) g_cfg->bacnet_obj_supply = server.arg("basup");
  if (server.hasArg("baext")) g_cfg->bacnet_obj_extract = server.arg("baext");
  if (server.hasArg("baexh")) g_cfg->bacnet_obj_exhaust = server.arg("baexh");
  if (server.hasArg("bafan")) g_cfg->bacnet_obj_fan = server.arg("bafan");
  if (server.hasArg("baheat")) g_cfg->bacnet_obj_heat = server.arg("baheat");
  if (server.hasArg("bamode")) g_cfg->bacnet_obj_mode = server.arg("bamode");
  if (server.hasArg("bashome")) g_cfg->bacnet_obj_setpoint_home = server.arg("bashome");
  if (server.hasArg("basaway")) g_cfg->bacnet_obj_setpoint_away = server.arg("basaway");
  if (server.hasArg("bamap")) g_cfg->bacnet_mode_map = server.arg("bamap");

  if (g_cfg->bacnet_obj_outdoor.length() == 0) g_cfg->bacnet_obj_outdoor = "ai:1";
  if (g_cfg->bacnet_obj_supply.length() == 0) g_cfg->bacnet_obj_supply = "ai:4";
  if (g_cfg->bacnet_obj_extract.length() == 0) g_cfg->bacnet_obj_extract = "ai:59";
  if (g_cfg->bacnet_obj_exhaust.length() == 0) g_cfg->bacnet_obj_exhaust = "ai:11";
  if (g_cfg->bacnet_obj_fan.length() == 0) g_cfg->bacnet_obj_fan = "ao:3";
  if (g_cfg->bacnet_obj_heat.length() == 0) g_cfg->bacnet_obj_heat = "ao:29";
  if (g_cfg->bacnet_obj_mode.length() == 0) g_cfg->bacnet_obj_mode = "msv:41";
  if (g_cfg->bacnet_obj_setpoint_home.length() == 0) g_cfg->bacnet_obj_setpoint_home = "av:126";
  if (g_cfg->bacnet_obj_setpoint_away.length() == 0) g_cfg->bacnet_obj_setpoint_away = "av:96";
  if (g_cfg->bacnet_mode_map.length() == 0) g_cfg->bacnet_mode_map = "2:AWAY,3:HOME,4:HIGH,5:FIRE";
}

static void applyPostedHAMqttSettings()
{
  g_cfg->ha_mqtt_enabled = server.hasArg("hamqtt");
  if (server.hasArg("hamhost")) g_cfg->ha_mqtt_host = server.arg("hamhost");
  if (server.hasArg("hamuser")) g_cfg->ha_mqtt_user = server.arg("hamuser");
  if (server.hasArg("hampass")) g_cfg->ha_mqtt_pass = server.arg("hampass");
  if (server.hasArg("hampfx")) g_cfg->ha_mqtt_prefix = server.arg("hampfx");
  if (server.hasArg("hambase")) g_cfg->ha_mqtt_topic_base = server.arg("hambase");

  int p = server.arg("hamport").toInt();
  if (p <= 0) p = 1883;
  if (p > 65535) p = 65535;
  g_cfg->ha_mqtt_port = (uint16_t)p;

  int sec = server.arg("hamint").toInt();
  if (sec <= 0) sec = 60;
  if (sec < 10) sec = 10;
  if (sec > 3600) sec = 3600;
  g_cfg->ha_mqtt_interval_s = (uint16_t)sec;

  if (g_cfg->ha_mqtt_prefix.length() == 0) g_cfg->ha_mqtt_prefix = "homeassistant";
  if (g_cfg->ha_mqtt_topic_base.length() == 0)
    g_cfg->ha_mqtt_topic_base = String("ventreader/") + config_chip_suffix4();
}

static bool bacnetIpValid(const String& ipIn)
{
  int a = 0, b = 0, c = 0, d = 0;
  if (sscanf(ipIn.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  return a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255;
}

static bool runBACnetPreflight(const DeviceConfig& cfg, FlexitData* outData = nullptr, String* why = nullptr)
{
  if (cfg.bacnet_ip.length() == 0 || !bacnetIpValid(cfg.bacnet_ip))
  {
    if (why) *why = "Ugyldig eller manglende BACnet IP.";
    return false;
  }
  if (cfg.bacnet_device_id == 0)
  {
    if (why) *why = "Mangler BACnet Device ID.";
    return false;
  }
  if (cfg.bacnet_port == 0)
  {
    if (why) *why = "Ugyldig BACnet UDP-port.";
    return false;
  }

  flexit_bacnet_set_runtime_config(cfg);
  FlexitData t = g_data;
  if (!flexit_bacnet_test(&t, why))
  {
    if (why && why->length() == 0) *why = String(flexit_bacnet_last_error());
    return false;
  }
  if (outData) *outData = t;
  return true;
}

static String fOrNullCompact(float v)
{
  if (isnan(v)) return "null";
  char t[24];
  snprintf(t, sizeof(t), "%.1f", v);
  return String(t);
}

static void handleAdminBACnetTest()
{
  if (!checkAdminAuth()) return;

  DeviceConfig tmp = *g_cfg;
  tmp.data_source = "BACNET";
  if (server.hasArg("bacip")) tmp.bacnet_ip = server.arg("bacip");
  if (server.hasArg("bacid")) tmp.bacnet_device_id = (uint32_t)server.arg("bacid").toInt();
  if (server.hasArg("bacport")) tmp.bacnet_port = (uint16_t)server.arg("bacport").toInt();
  if (server.hasArg("bacpoll")) tmp.bacnet_poll_minutes = (uint8_t)server.arg("bacpoll").toInt();
  if (server.hasArg("bacto")) tmp.bacnet_timeout_ms = (uint16_t)server.arg("bacto").toInt();
  if (server.hasArg("baout")) tmp.bacnet_obj_outdoor = server.arg("baout");
  if (server.hasArg("basup")) tmp.bacnet_obj_supply = server.arg("basup");
  if (server.hasArg("baext")) tmp.bacnet_obj_extract = server.arg("baext");
  if (server.hasArg("baexh")) tmp.bacnet_obj_exhaust = server.arg("baexh");
  if (server.hasArg("bafan")) tmp.bacnet_obj_fan = server.arg("bafan");
  if (server.hasArg("baheat")) tmp.bacnet_obj_heat = server.arg("baheat");
  if (server.hasArg("bamode")) tmp.bacnet_obj_mode = server.arg("bamode");
  if (server.hasArg("bamap")) tmp.bacnet_mode_map = server.arg("bamap");

  String why;
  FlexitData t = g_data;
  if (!runBACnetPreflight(tmp, &t, &why))
  {
    String out = "{\"ok\":false";
    out += ",\"error\":\"" + jsonEscape(why) + "\"";
    out += ",\"ip\":\"" + jsonEscape(tmp.bacnet_ip) + "\"";
    out += ",\"device_id\":" + String(tmp.bacnet_device_id);
    out += ",\"debug\":\"" + jsonEscape(flexit_bacnet_debug_dump_text()) + "\"";
    out += "}";
    server.send(200, "application/json", out);
    return;
  }

  String mb = "BACNET OK (test)";
  String out;
  out.reserve(360);
  out += "{\"ok\":true,\"source_status\":\"BACNET OK (test)\",\"data\":{";
  out += "\"mode\":\"" + jsonEscape(t.mode) + "\",";
  out += "\"uteluft\":" + fOrNullCompact(t.uteluft) + ",";
  out += "\"tilluft\":" + fOrNullCompact(t.tilluft) + ",";
  out += "\"avtrekk\":" + fOrNullCompact(t.avtrekk) + ",";
  out += "\"avkast\":" + fOrNullCompact(t.avkast) + ",";
  out += "\"fan\":" + String(t.fan_percent) + ",";
  out += "\"heat\":" + String(t.heat_element_percent) + ",";
  out += "\"efficiency\":" + String(t.efficiency_percent);
  out += "},\"debug\":\"" + jsonEscape(flexit_bacnet_debug_dump_text()) + "\"}";
  server.send(200, "application/json", out);

  // keep live dashboard fresh after successful test
  g_data = t;
  g_mb = mb;
}

static void handleAdminBACnetDiscover()
{
  if (!checkAdminAuth()) return;
  uint16_t waitMs = 1800;
  if (server.hasArg("wait"))
  {
    int w = server.arg("wait").toInt();
    if (w < 300) w = 300;
    if (w > 8000) w = 8000;
    waitMs = (uint16_t)w;
  }

  String list = flexit_bacnet_autodiscover_json(waitMs);
  String out = "{\"ok\":";
  out += (list != "[]" ? "true" : "false");
  out += ",\"items\":";
  out += list;
  out += ",\"error\":\"" + jsonEscape(String(flexit_bacnet_last_error())) + "\"";
  out += ",\"debug\":\"" + jsonEscape(flexit_bacnet_debug_dump_text()) + "\"}";
  server.send(200, "application/json", out);
}

static void handleAdminBACnetObjectProbe()
{
  if (!checkAdminAuth()) return;

  DeviceConfig tmp = *g_cfg;
  tmp.data_source = "BACNET";
  if (server.hasArg("bacip")) tmp.bacnet_ip = server.arg("bacip");
  if (server.hasArg("bacid")) tmp.bacnet_device_id = (uint32_t)server.arg("bacid").toInt();
  if (server.hasArg("bacport")) tmp.bacnet_port = (uint16_t)server.arg("bacport").toInt();
  if (server.hasArg("bacpoll")) tmp.bacnet_poll_minutes = (uint8_t)server.arg("bacpoll").toInt();
  if (server.hasArg("bacto")) tmp.bacnet_timeout_ms = (uint16_t)server.arg("bacto").toInt();
  if (server.hasArg("baout")) tmp.bacnet_obj_outdoor = server.arg("baout");
  if (server.hasArg("basup")) tmp.bacnet_obj_supply = server.arg("basup");
  if (server.hasArg("baext")) tmp.bacnet_obj_extract = server.arg("baext");
  if (server.hasArg("baexh")) tmp.bacnet_obj_exhaust = server.arg("baexh");
  if (server.hasArg("bafan")) tmp.bacnet_obj_fan = server.arg("bafan");
  if (server.hasArg("baheat")) tmp.bacnet_obj_heat = server.arg("baheat");
  if (server.hasArg("bamode")) tmp.bacnet_obj_mode = server.arg("bamode");
  if (server.hasArg("bamap")) tmp.bacnet_mode_map = server.arg("bamap");

  uint16_t fromInst = server.hasArg("from_inst") ? (uint16_t)server.arg("from_inst").toInt() : 0;
  uint16_t toInst = server.hasArg("to_inst") ? (uint16_t)server.arg("to_inst").toInt() : 200;
  uint16_t timeoutMs = server.hasArg("scan_timeout") ? (uint16_t)server.arg("scan_timeout").toInt() : 450;
  uint16_t maxHits = server.hasArg("scan_max") ? (uint16_t)server.arg("scan_max").toInt() : 220;
  int16_t onlyType = -1;
  if (server.hasArg("otype"))
  {
    String t = server.arg("otype");
    t.trim();
    t.toLowerCase();
    if (t == "ai") onlyType = 0;
    else if (t == "ao") onlyType = 1;
    else if (t == "av") onlyType = 2;
    else if (t == "mi") onlyType = 13;
    else if (t == "mo") onlyType = 14;
    else if (t == "msv") onlyType = 19;
    else if (t.length() > 0) onlyType = (int16_t)t.toInt();
  }

  flexit_bacnet_set_runtime_config(tmp);
  String items = flexit_bacnet_scan_objects_json(fromInst, toInst, timeoutMs, maxHits, onlyType);
  bool hasOk = (items != "[]");
  String out = "{\"ok\":";
  out += (hasOk ? "true" : "false");
  out += ",\"mode\":\"all_readable\"";
  out += ",\"ip\":\"" + jsonEscape(tmp.bacnet_ip) + "\"";
  out += ",\"port\":" + String((int)tmp.bacnet_port);
  out += ",\"device_id\":" + String(tmp.bacnet_device_id);
  out += ",\"items\":" + items;
  out += ",\"error\":\"" + jsonEscape(String(flexit_bacnet_last_error())) + "\"";
  out += ",\"debug\":\"" + jsonEscape(flexit_bacnet_debug_dump_text()) + "\"}";
  server.send(200, "application/json", out);
}

static void handleAdminBACnetObjectScan()
{
  if (!checkAdminAuth()) return;

  DeviceConfig tmp = *g_cfg;
  tmp.data_source = "BACNET";
  if (server.hasArg("bacip")) tmp.bacnet_ip = server.arg("bacip");
  if (server.hasArg("bacid")) tmp.bacnet_device_id = (uint32_t)server.arg("bacid").toInt();
  if (server.hasArg("bacport")) tmp.bacnet_port = (uint16_t)server.arg("bacport").toInt();
  if (server.hasArg("bacto")) tmp.bacnet_timeout_ms = (uint16_t)server.arg("bacto").toInt();
  if (server.hasArg("baout")) tmp.bacnet_obj_outdoor = server.arg("baout");
  if (server.hasArg("basup")) tmp.bacnet_obj_supply = server.arg("basup");
  if (server.hasArg("baext")) tmp.bacnet_obj_extract = server.arg("baext");
  if (server.hasArg("baexh")) tmp.bacnet_obj_exhaust = server.arg("baexh");
  if (server.hasArg("bafan")) tmp.bacnet_obj_fan = server.arg("bafan");
  if (server.hasArg("baheat")) tmp.bacnet_obj_heat = server.arg("baheat");
  if (server.hasArg("bamode")) tmp.bacnet_obj_mode = server.arg("bamode");
  if (server.hasArg("bamap")) tmp.bacnet_mode_map = server.arg("bamap");

  uint16_t fromInst = server.hasArg("from_inst") ? (uint16_t)server.arg("from_inst").toInt() : 0;
  uint16_t toInst = server.hasArg("to_inst") ? (uint16_t)server.arg("to_inst").toInt() : 64;
  uint16_t timeoutMs = server.hasArg("scan_timeout") ? (uint16_t)server.arg("scan_timeout").toInt() : 450;
  uint16_t maxHits = server.hasArg("scan_max") ? (uint16_t)server.arg("scan_max").toInt() : 40;
  int16_t onlyType = -1;
  if (server.hasArg("otype"))
  {
    String t = server.arg("otype");
    t.trim();
    t.toLowerCase();
    if (t == "ai") onlyType = 0;
    else if (t == "ao") onlyType = 1;
    else if (t == "av") onlyType = 2;
    else if (t == "mi") onlyType = 13;
    else if (t == "mo") onlyType = 14;
    else if (t == "msv") onlyType = 19;
    else if (t.length() > 0) onlyType = (int16_t)t.toInt();
  }

  flexit_bacnet_set_runtime_config(tmp);
  String items = flexit_bacnet_scan_objects_json(fromInst, toInst, timeoutMs, maxHits, onlyType);
  bool hasOk = (items != "[]");

  String out = "{\"ok\":";
  out += (hasOk ? "true" : "false");
  out += ",\"ip\":\"" + jsonEscape(tmp.bacnet_ip) + "\"";
  out += ",\"port\":" + String((int)tmp.bacnet_port);
  out += ",\"device_id\":" + String(tmp.bacnet_device_id);
  out += ",\"items\":" + items;
  out += ",\"error\":\"" + jsonEscape(String(flexit_bacnet_last_error())) + "\"";
  out += ",\"debug\":\"" + jsonEscape(flexit_bacnet_debug_dump_text()) + "\"}";
  server.send(200, "application/json", out);
}

static void handleAdminBACnetWriteProbe()
{
  if (!checkAdminAuth()) return;

  DeviceConfig tmp = *g_cfg;
  tmp.data_source = "BACNET";
  tmp.bacnet_write_enabled = true; // probe endpoint is explicitly experimental
  if (server.hasArg("bacip")) tmp.bacnet_ip = server.arg("bacip");
  if (server.hasArg("bacid")) tmp.bacnet_device_id = (uint32_t)server.arg("bacid").toInt();
  if (server.hasArg("bacport")) tmp.bacnet_port = (uint16_t)server.arg("bacport").toInt();
  if (server.hasArg("bacto")) tmp.bacnet_timeout_ms = (uint16_t)server.arg("bacto").toInt();
  if (server.hasArg("bashome")) tmp.bacnet_obj_setpoint_home = server.arg("bashome");
  if (server.hasArg("basaway")) tmp.bacnet_obj_setpoint_away = server.arg("basaway");
  if (server.hasArg("bamode")) tmp.bacnet_obj_mode = server.arg("bamode");
  if (server.hasArg("bamap")) tmp.bacnet_mode_map = server.arg("bamap");

  String why;
  FlexitData t = g_data;
  if (!runBACnetPreflight(tmp, &t, &why))
  {
    String out = "{\"ok\":false,\"error\":\"" + jsonEscape(why) + "\"";
    out += ",\"debug\":\"" + jsonEscape(flexit_bacnet_debug_dump_text()) + "\"}";
    server.send(200, "application/json", out);
    return;
  }

  float probeSet = !isnan(t.set_temp) ? t.set_temp : (!isnan(t.tilluft) ? t.tilluft : 21.0f);
  if (probeSet < 10.0f) probeSet = 10.0f;
  if (probeSet > 30.0f) probeSet = 30.0f;
  const String modeNow = t.mode;

  flexit_bacnet_set_runtime_config(tmp);

  bool modeTried = false;
  bool modeOk = false;
  String modeErr = "skipped (unsupported current mode)";
  String modeUpper = modeNow;
  modeUpper.toUpperCase();
  if (modeUpper == "AWAY" || modeUpper == "HOME" || modeUpper == "HIGH" || modeUpper == "FIRE")
  {
    modeTried = true;
    modeOk = flexit_bacnet_write_mode(modeUpper);
    if (!modeOk) modeErr = String(flexit_bacnet_last_error());
    else modeErr = "OK";
  }

  bool homeOk = flexit_bacnet_write_setpoint("home", probeSet);
  String homeErr = homeOk ? "OK" : String(flexit_bacnet_last_error());
  bool awayOk = flexit_bacnet_write_setpoint("away", probeSet);
  String awayErr = awayOk ? "OK" : String(flexit_bacnet_last_error());

  bool ok = (homeOk || awayOk || modeOk);
  String out = "{\"ok\":";
  out += (ok ? "true" : "false");
  out += ",\"experimental\":true";
  out += ",\"probe_set_temp\":" + fOrNullCompact(probeSet);
  out += ",\"current_mode\":\"" + jsonEscape(modeNow) + "\"";
  out += ",\"results\":{";
  out += "\"mode\":{\"tried\":" + String(modeTried ? "true" : "false") + ",\"ok\":" + String(modeOk ? "true" : "false") + ",\"error\":\"" + jsonEscape(modeErr) + "\"},";
  out += "\"setpoint_home\":{\"ok\":" + String(homeOk ? "true" : "false") + ",\"error\":\"" + jsonEscape(homeErr) + "\"},";
  out += "\"setpoint_away\":{\"ok\":" + String(awayOk ? "true" : "false") + ",\"error\":\"" + jsonEscape(awayErr) + "\"}";
  out += "}";
  if (!ok)
  {
    out += ",\"error\":\"" + jsonEscape(String("No BACnet write variant succeeded")) + "\"";
  }
  out += ",\"debug\":\"" + jsonEscape(flexit_bacnet_debug_dump_text()) + "\"}";
  server.send(200, "application/json", out);
}

static void handleAdminBACnetDebug()
{
  if (!checkAdminAuth()) return;
  server.send(200, "text/plain; charset=utf-8", flexit_bacnet_debug_dump_text());
}

static void handleAdminBACnetDebugClear()
{
  if (!checkAdminAuth()) return;
  flexit_bacnet_debug_clear();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleAdminBACnetDebugMode()
{
  if (!checkAdminAuth()) return;
  bool enable = false;
  if (server.hasArg("enable"))
  {
    String e = server.arg("enable");
    e.toLowerCase();
    enable = (e == "1" || e == "true" || e == "on" || e == "yes");
  }
  flexit_bacnet_debug_set_enabled(enable);
  String out = "{\"ok\":true,\"enabled\":";
  out += (flexit_bacnet_debug_is_enabled() ? "true" : "false");
  out += "}";
  server.send(200, "application/json", out);
}

static bool tokenMatches(const String& got, const String& expected)
{
  return (got.length() > 0 && expected.length() > 0 && got == expected);
}

enum ApiScope : uint8_t {
  API_SCOPE_HOMEY_READ = 1,
  API_SCOPE_HA_READ = 2,
  API_SCOPE_CONTROL_WRITE = 4
};

static bool apiPanicActive()
{
  return g_cfg && g_cfg->api_panic_stop;
}

static String bearerToken()
{
  if (!server.hasHeader("Authorization")) return "";
  String h = server.header("Authorization");
  h.trim();
  if (!h.startsWith("Bearer ")) return "";
  String t = h.substring(7);
  t.trim();
  return t;
}

static bool tokenScopeOK(const String& token, ApiScope scope)
{
  if (!g_cfg) return false;
  if (tokenMatches(token, g_cfg->api_token))
  {
    // Main token can read + write control.
    return true;
  }
  if (scope == API_SCOPE_HOMEY_READ && tokenMatches(token, g_cfg->homey_api_token))
  {
    return true;
  }
  if (scope == API_SCOPE_HA_READ && tokenMatches(token, g_cfg->ha_api_token))
  {
    return true;
  }
  return false;
}

static bool requireApiScope(ApiScope scope)
{
  if (apiPanicActive())
  {
    server.send(503, "text/plain", "api emergency stop active");
    return false;
  }
  const String t = bearerToken();
  if (t.length() == 0)
  {
    server.send(401, "text/plain", "missing/invalid bearer token");
    return false;
  }
  if (!tokenScopeOK(t, scope))
  {
    server.send(401, "text/plain", "missing/invalid bearer token");
    return false;
  }
  return true;
}

static String genWebToken(size_t bytes = 16)
{
  const char* hex = "0123456789abcdef";
  String out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; i++)
  {
    uint8_t b = (uint8_t)(esp_random() & 0xFF);
    out += hex[(b >> 4) & 0x0F];
    out += hex[b & 0x0F];
  }
  return out;
}

static void handleAdminNewToken()
{
  if (!checkAdminAuth()) return;
  String kind = server.hasArg("kind") ? server.arg("kind") : "main";
  kind.toLowerCase();
  if (kind != "main" && kind != "homey" && kind != "ha")
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid kind\"}");
    return;
  }

  const String t = genWebToken(16);
  if (kind == "homey") g_cfg->homey_api_token = t;
  else if (kind == "ha") g_cfg->ha_api_token = t;
  else g_cfg->api_token = t;
  config_save(*g_cfg);

  String out = "{\"ok\":true,\"kind\":\"" + jsonEscape(kind) + "\",\"token\":\"" + jsonEscape(t) + "\"}";
  server.send(200, "application/json", out);
}

static void handleAdminApiEmergencyStop()
{
  if (!checkAdminAuth()) return;
  bool enable = false;
  if (server.hasArg("enable"))
  {
    String e = server.arg("enable");
    e.toLowerCase();
    enable = (e == "1" || e == "true" || e == "on" || e == "yes");
  }
  g_cfg->api_panic_stop = enable;
  config_save(*g_cfg);
  redirectTo("/admin");
}

static void handleAdminApiPreview()
{
  if (!checkAdminAuth()) return;
  bool pretty = false;
  if (server.hasArg("pretty"))
  {
    String v = server.arg("pretty");
    pretty = (v == "1" || v == "true" || v == "yes");
  }
  server.send(200, "application/json", buildStatusJson(pretty));
}

static bool checkAdminAuth()
{
  // First-boot onboarding is intentionally pre-auth until setup is completed.
  if (g_cfg && !g_cfg->setup_completed) return true;
  if (!server.authenticate(g_cfg->admin_user.c_str(), g_cfg->admin_pass.c_str()))
  {
    server.requestAuthentication();
    return false;
  }
  return true;
}

static void redirectTo(const String& path)
{
  server.sendHeader("Location", path, true);
  server.send(302, "text/plain", "Redirect");
}

static String buildStatusJson(bool pretty)
{
  const String mode = jsonEscape(g_data.mode);
  const String filter = jsonEscape(g_data.filter_status);
  const String wifi = jsonEscape(g_data.wifi_status);
  const String ip = jsonEscape(g_data.ip);
  const String screenTime = jsonEscape(g_data.time);
  const String dataTime = jsonEscape(g_data.data_time);
  const String sourceStatus = jsonEscape(g_mb);
  const String src = jsonEscape(normDataSource(g_cfg->data_source));
  const String model = jsonEscape(g_data.device_model);
  const String fw = jsonEscape(String(FW_VERSION));
  const String bacOutdoor = jsonEscape(g_cfg->bacnet_obj_outdoor);
  const String bacSupply = jsonEscape(g_cfg->bacnet_obj_supply);
  const String bacExtract = jsonEscape(g_cfg->bacnet_obj_extract);
  const String bacExhaust = jsonEscape(g_cfg->bacnet_obj_exhaust);
  const String bacFan = jsonEscape(g_cfg->bacnet_obj_fan);
  const String bacHeat = jsonEscape(g_cfg->bacnet_obj_heat);
  const String bacMode = jsonEscape(g_cfg->bacnet_obj_mode);
  const String bacSetHome = jsonEscape(g_cfg->bacnet_obj_setpoint_home);
  const String bacSetAway = jsonEscape(g_cfg->bacnet_obj_setpoint_away);
  const String bacModeMap = jsonEscape(g_cfg->bacnet_mode_map);

  const uint64_t tsEpochMs = nowEpochMs();
  const String tsIso = jsonEscape(isoFromEpochMs(tsEpochMs));
  const bool stale = (g_mb.indexOf("stale") >= 0);

  auto fOrNull = [](float v) -> String {
    if (isnan(v)) return "null";
    char t[24];
    snprintf(t, sizeof(t), "%.2f", v);
    return String(t);
  };
  auto quoted = [](const String& v) -> String {
    return String("\"") + v + "\"";
  };

  const String ut = fOrNull(g_data.uteluft);
  const String ti = fOrNull(g_data.tilluft);
  const String av = fOrNull(g_data.avtrekk);
  const String ak = fOrNull(g_data.avkast);
  const String st = fOrNull(g_data.set_temp);

  String out;
  out.reserve(pretty ? 2600 : 1200);

  if (!pretty)
  {
    out += "{";
    out += "\"ts_epoch_ms\":" + u64ToString(tsEpochMs) + ",";
    out += "\"ts_iso\":" + quoted(tsIso) + ",";
    out += "\"stale\":" + String(stale ? "true" : "false") + ",";
    out += "\"screen_time\":" + quoted(screenTime) + ",";
    out += "\"time\":" + quoted(screenTime) + ",";
    out += "\"data_time\":" + quoted(dataTime) + ",";
    out += "\"mode\":" + quoted(mode) + ",";
    out += "\"filter_status\":" + quoted(filter) + ",";
    out += "\"uteluft\":" + ut + ",";
    out += "\"tilluft\":" + ti + ",";
    out += "\"avtrekk\":" + av + ",";
    out += "\"avkast\":" + ak + ",";
    out += "\"set_temp\":" + st + ",";
    out += "\"fan\":" + String(g_data.fan_percent) + ",";
    out += "\"heat\":" + String(g_data.heat_element_percent) + ",";
    out += "\"efficiency\":" + String(g_data.efficiency_percent) + ",";
    out += "\"model\":" + quoted(model) + ",";
    out += "\"fw\":" + quoted(fw) + ",";
    out += "\"wifi\":" + quoted(wifi) + ",";
    out += "\"ip\":" + quoted(ip) + ",";
    out += "\"source_status\":" + quoted(sourceStatus) + ",";
    out += "\"modbus\":" + quoted(sourceStatus) + ",";
    out += "\"data_source\":" + quoted(src) + ",";
    out += "\"source_points\":{";
    out += "\"bacnet\":{";
    out += "\"ip\":" + quoted(jsonEscape(g_cfg->bacnet_ip)) + ",";
    out += "\"port\":" + String((int)g_cfg->bacnet_port) + ",";
    out += "\"device_id\":" + String((unsigned long)g_cfg->bacnet_device_id) + ",";
    out += "\"outdoor\":" + quoted(bacOutdoor) + ",";
    out += "\"supply\":" + quoted(bacSupply) + ",";
    out += "\"extract\":" + quoted(bacExtract) + ",";
    out += "\"exhaust\":" + quoted(bacExhaust) + ",";
    out += "\"fan\":" + quoted(bacFan) + ",";
    out += "\"heat\":" + quoted(bacHeat) + ",";
    out += "\"mode\":" + quoted(bacMode) + ",";
    out += "\"setpoint_home\":" + quoted(bacSetHome) + ",";
    out += "\"setpoint_away\":" + quoted(bacSetAway) + ",";
    out += "\"mode_map\":" + quoted(bacModeMap);
    out += "},";
    out += "\"modbus\":{";
    out += "\"slave_id\":" + String((int)g_cfg->modbus_slave_id) + ",";
    out += "\"baud\":" + String((unsigned long)g_cfg->modbus_baud) + ",";
    out += "\"serial_format\":" + quoted(jsonEscape(g_cfg->modbus_serial_format)) + ",";
    out += "\"addr_offset\":" + String((int)g_cfg->modbus_addr_offset) + ",";
    out += "\"reg_uteluft_in\":" + String((int)FLEXIT_REG_UTELUFT_IN) + ",";
    out += "\"reg_tilluft_in\":" + String((int)FLEXIT_REG_TILLUFT_IN) + ",";
    out += "\"reg_avtrekk_in\":" + String((int)FLEXIT_REG_AVTREKK_IN) + ",";
    out += "\"reg_avkast_in\":" + String((int)FLEXIT_REG_AVKAST_IN) + ",";
    out += "\"reg_fan_pct_hold\":" + String((int)FLEXIT_REG_FAN_PCT_HOLD) + ",";
    out += "\"reg_heat_pct_hold\":" + String((int)FLEXIT_REG_HEAT_PCT_HOLD) + ",";
    out += "\"reg_mode_in\":" + String((int)FLEXIT_REG_MODE_IN) + ",";
    out += "\"reg_setpoint_home_hold\":" + String((int)FLEXIT_REG_SETPOINT_HOME_HOLD) + ",";
    out += "\"reg_setpoint_away_hold\":" + String((int)FLEXIT_REG_SETPOINT_AWAY_HOLD);
    out += "}";
    out += "},";
    out += "\"homey_enabled\":" + String(g_cfg->homey_enabled ? "true" : "false") + ",";
    out += "\"ha_enabled\":" + String(g_cfg->ha_enabled ? "true" : "false") + ",";
    out += "\"display_enabled\":" + String(g_cfg->display_enabled ? "true" : "false") + ",";
    out += "\"headless\":" + String(g_cfg->display_enabled ? "false" : "true") + ",";
    out += "\"ha_mqtt_enabled\":" + String(g_cfg->ha_mqtt_enabled ? "true" : "false") + ",";
    out += "\"modbus_enabled\":" + String(g_cfg->modbus_enabled ? "true" : "false") + ",";
    out += "\"bacnet_write_enabled\":" + String(g_cfg->bacnet_write_enabled ? "true" : "false");
    out += ",\"api_panic_stop\":" + String(g_cfg->api_panic_stop ? "true" : "false");
    out += "}";
    return out;
  }

  out += "{\n";
  out += "  \"meta\": {\n";
  out += "    \"ts_epoch_ms\": " + u64ToString(tsEpochMs) + ",\n";
  out += "    \"ts_iso\": " + quoted(tsIso) + ",\n";
  out += "    \"screen_time\": " + quoted(screenTime) + ",\n";
  out += "    \"time\": " + quoted(screenTime) + ",\n";
  out += "    \"data_time\": " + quoted(dataTime) + ",\n";
  out += "    \"stale\": " + String(stale ? "true" : "false") + ",\n";
  out += "    \"model\": " + quoted(model) + ",\n";
  out += "    \"fw\": " + quoted(fw) + "\n";
  out += "  },\n";
  out += "  \"source\": {\n";
  out += "    \"data_source\": " + quoted(src) + ",\n";
  out += "    \"source_status\": " + quoted(sourceStatus) + ",\n";
  out += "    \"modbus\": " + quoted(sourceStatus) + ",\n";
  out += "    \"modbus_enabled\": " + String(g_cfg->modbus_enabled ? "true" : "false") + ",\n";
  out += "    \"homey_enabled\": " + String(g_cfg->homey_enabled ? "true" : "false") + ",\n";
    out += "    \"ha_enabled\": " + String(g_cfg->ha_enabled ? "true" : "false") + ",\n";
    out += "    \"display_enabled\": " + String(g_cfg->display_enabled ? "true" : "false") + ",\n";
    out += "    \"headless\": " + String(g_cfg->display_enabled ? "false" : "true") + ",\n";
    out += "    \"ha_mqtt_enabled\": " + String(g_cfg->ha_mqtt_enabled ? "true" : "false") + ",\n";
    out += "    \"bacnet_write_enabled\": " + String(g_cfg->bacnet_write_enabled ? "true" : "false") + ",\n";
    out += "    \"api_panic_stop\": " + String(g_cfg->api_panic_stop ? "true" : "false") + "\n";
  out += "  },\n";
  out += "  \"known_source_points\": {\n";
  out += "    \"bacnet\": {\n";
  out += "      \"ip\": " + quoted(jsonEscape(g_cfg->bacnet_ip)) + ",\n";
  out += "      \"port\": " + String((int)g_cfg->bacnet_port) + ",\n";
  out += "      \"device_id\": " + String((unsigned long)g_cfg->bacnet_device_id) + ",\n";
  out += "      \"outdoor\": " + quoted(bacOutdoor) + ",\n";
  out += "      \"supply\": " + quoted(bacSupply) + ",\n";
  out += "      \"extract\": " + quoted(bacExtract) + ",\n";
  out += "      \"exhaust\": " + quoted(bacExhaust) + ",\n";
  out += "      \"fan\": " + quoted(bacFan) + ",\n";
  out += "      \"heat\": " + quoted(bacHeat) + ",\n";
  out += "      \"mode\": " + quoted(bacMode) + ",\n";
  out += "      \"setpoint_home\": " + quoted(bacSetHome) + ",\n";
  out += "      \"setpoint_away\": " + quoted(bacSetAway) + ",\n";
  out += "      \"mode_map\": " + quoted(bacModeMap) + "\n";
  out += "    },\n";
  out += "    \"modbus\": {\n";
  out += "      \"slave_id\": " + String((int)g_cfg->modbus_slave_id) + ",\n";
  out += "      \"baud\": " + String((unsigned long)g_cfg->modbus_baud) + ",\n";
  out += "      \"serial_format\": " + quoted(jsonEscape(g_cfg->modbus_serial_format)) + ",\n";
  out += "      \"addr_offset\": " + String((int)g_cfg->modbus_addr_offset) + ",\n";
  out += "      \"reg_uteluft_in\": " + String((int)FLEXIT_REG_UTELUFT_IN) + ",\n";
  out += "      \"reg_tilluft_in\": " + String((int)FLEXIT_REG_TILLUFT_IN) + ",\n";
  out += "      \"reg_avtrekk_in\": " + String((int)FLEXIT_REG_AVTREKK_IN) + ",\n";
  out += "      \"reg_avkast_in\": " + String((int)FLEXIT_REG_AVKAST_IN) + ",\n";
  out += "      \"reg_fan_pct_hold\": " + String((int)FLEXIT_REG_FAN_PCT_HOLD) + ",\n";
  out += "      \"reg_heat_pct_hold\": " + String((int)FLEXIT_REG_HEAT_PCT_HOLD) + ",\n";
  out += "      \"reg_mode_in\": " + String((int)FLEXIT_REG_MODE_IN) + ",\n";
  out += "      \"reg_setpoint_home_hold\": " + String((int)FLEXIT_REG_SETPOINT_HOME_HOLD) + ",\n";
  out += "      \"reg_setpoint_away_hold\": " + String((int)FLEXIT_REG_SETPOINT_AWAY_HOLD) + "\n";
  out += "    }\n";
  out += "  },\n";
  out += "  \"network\": {\n";
  out += "    \"wifi\": " + quoted(wifi) + ",\n";
  out += "    \"ip\": " + quoted(ip) + "\n";
  out += "  },\n";
  out += "  \"data\": {\n";
  out += "    \"mode\": " + quoted(mode) + ",\n";
  out += "    \"filter_status\": " + quoted(filter) + ",\n";
  out += "    \"uteluft\": " + ut + ",\n";
  out += "    \"tilluft\": " + ti + ",\n";
  out += "    \"avtrekk\": " + av + ",\n";
  out += "    \"avkast\": " + ak + ",\n";
  out += "    \"set_temp\": " + st + ",\n";
  out += "    \"fan\": " + String(g_data.fan_percent) + ",\n";
  out += "    \"heat\": " + String(g_data.heat_element_percent) + ",\n";
  out += "    \"efficiency\": " + String(g_data.efficiency_percent) + "\n";
  out += "  },\n";
  out += "  \"field_map\": {\n";
  out += "    \"screen_time\": \"Last ePaper refresh (HH:MM)\",\n";
  out += "    \"time\": \"Alias for screen_time (legacy compatibility)\",\n";
  out += "    \"data_time\": \"Last successful datasource update (HH:MM)\",\n";
  out += "    \"source_status\": \"Datasource status string (MB/BACNET + state)\",\n";
  out += "    \"modbus\": \"Alias for source_status (legacy compatibility)\",\n";
  out += "    \"known_source_points\": \"Configured BACnet objects and Modbus register map used by this firmware\",\n";
  out += "    \"display_enabled\": \"True when physical display rendering is enabled\",\n";
  out += "    \"headless\": \"True when running without display rendering\",\n";
  out += "    \"bacnet_write_enabled\": \"True when experimental BACnet writes are enabled\",\n";
  out += "    \"api_panic_stop\": \"True when emergency stop blocks all token-protected API endpoints\",\n";
  out += "    \"mode\": \"Ventilation mode\",\n";
  out += "    \"uteluft\": \"Outdoor temperature (C)\",\n";
  out += "    \"tilluft\": \"Supply temperature (C)\",\n";
  out += "    \"avtrekk\": \"Extract temperature (C)\",\n";
  out += "    \"avkast\": \"Exhaust temperature (C)\",\n";
  out += "    \"set_temp\": \"Active setpoint (C)\",\n";
  out += "    \"fan\": \"Fan control/speed percent\",\n";
  out += "    \"heat\": \"Heater percent\",\n";
  out += "    \"efficiency\": \"Heat recovery efficiency percent\"\n";
  out += "  }\n";
  out += "}\n";
  return out;
}

static String boolLabel(bool v)
{
  return v ? "ON" : "OFF";
}

static String currentBaseUrl()
{
  if (WiFi.status() == WL_CONNECTED) return String("http://") + WiFi.localIP().toString();
  if (g_data.ip.length() > 0) return String("http://") + g_data.ip;
  return "http://ventreader.local";
}

static String buildHomeyExportJson()
{
  const String base = currentBaseUrl();
  const String statusUrl = base + "/status";
  const String activeSrc = normDataSource(g_cfg->data_source);
  const bool controlActive =
      (activeSrc == "MODBUS" && g_cfg->modbus_enabled && g_cfg->control_enabled) ||
      (activeSrc == "BACNET" && g_cfg->bacnet_write_enabled);
  const uint64_t ts = nowEpochMs();
  const String tsIso = isoFromEpochMs(ts);
  const String tsStr = u64ToString(ts);
  const String ctrlModeUrl = base + "/api/control/mode";
  const String ctrlSetpointUrl = base + "/api/control/setpoint";

  String script;
  script.reserve(2600);
  script += "// VentReader -> Homey virtual devices\n";
  script += "const VENTREADER_BASE = '" + base + "';\n";
  script += "const VENTREADER_TOKEN = '" + g_cfg->homey_api_token + "';\n";
  script += "const VENTREADER_URL = VENTREADER_BASE + '/status';\n\n";
  script += "const MAP = {\n";
  script += "  'VentReader - Uteluft': { field: 'uteluft', cap: 'measure_temperature' },\n";
  script += "  'VentReader - Tilluft': { field: 'tilluft', cap: 'measure_temperature' },\n";
  script += "  'VentReader - Avtrekk': { field: 'avtrekk', cap: 'measure_temperature' },\n";
  script += "  'VentReader - Avkast':  { field: 'avkast',  cap: 'measure_temperature' },\n";
  script += "  'VentReader - Fan %':   { field: 'fan', cap: 'measure_percentage' },\n";
  script += "  'VentReader - Heat %':  { field: 'heat', cap: 'measure_percentage' },\n";
  script += "  'VentReader - Gjenvinning %': { field: 'efficiency', cap: 'measure_percentage' }\n";
  script += "};\n\n";
  script += "const ALARM_DEVICE = 'VentReader - Modbus Alarm'; // optional\n";
  script += "const ALARM_CAP = 'alarm_generic';\n";
  script += "const STATUS_DEVICE = 'VentReader - Status tekst'; // optional\n";
  script += "const STATUS_CAP = 'measure_text';\n\n";
  script += "function num(v){ if(v===null||v===undefined) return null; const n=Number(v); return Number.isFinite(n)?n:null; }\n";
  script += "async function setByName(devices,name,capability,value){\n";
  script += "  const d=Object.values(devices).find(x=>x.name===name);\n";
  script += "  if(!d||!d.capabilitiesObj||!d.capabilitiesObj[capability]) return;\n";
  script += "  await d.setCapabilityValue(capability,value);\n";
  script += "}\n\n";
  script += "const res = await fetch(VENTREADER_URL,{headers:{Authorization:`Bearer ${VENTREADER_TOKEN}`}});\n";
  script += "if(!res.ok) throw new Error(`HTTP ${res.status}`);\n";
  script += "const s = await res.json();\n";
  script += "const devices = await Homey.devices.getDevices();\n\n";
  script += "for (const [name,cfg] of Object.entries(MAP)) {\n";
  script += "  const v=num(s[cfg.field]); if(v===null) continue;\n";
  script += "  await setByName(devices,name,cfg.cap,v);\n";
  script += "}\n\n";
  script += "if (ALARM_DEVICE) {\n";
  script += "  const st = String(s.modbus || '');\n";
  script += "  const bad = !(st.startsWith('MB OK') || st.startsWith('BACNET OK'));\n";
  script += "  await setByName(devices,ALARM_DEVICE,ALARM_CAP,bad);\n";
  script += "}\n\n";
  script += "if (STATUS_DEVICE) {\n";
  script += "  const status = `${s.model||'N/A'} | ${s.mode||'N/A'} | ${s.modbus||'N/A'} | ${s.time||'--:--'}`;\n";
  script += "  await setByName(devices,STATUS_DEVICE,STATUS_CAP,status);\n";
  script += "}\n\n";
  script += "return `VentReader OK: ${s.time||'--:--'} ${s.modbus||''}`;\n";

  String out;
  out.reserve(7800);
  out += "{";
  out += "\"export_version\":\"1\",";
  out += "\"generated_at_epoch_ms\":";
  out += tsStr;
  out += ",";
  out += "\"generated_at_iso\":\"";
  out += jsonEscape(tsIso);
  out += "\",";
  out += "\"device\":{";
  out += "\"model\":\"" + jsonEscape(g_cfg->model) + "\",";
  out += "\"fw\":\"" + jsonEscape(String(FW_VERSION)) + "\",";
  out += "\"base_url\":\"" + jsonEscape(base) + "\",";
  out += "\"api_token\":\"" + jsonEscape(g_cfg->homey_api_token) + "\",";
  out += "\"bearer_token\":\"" + jsonEscape(g_cfg->homey_api_token) + "\"";
  out += "},";
  out += "\"modules\":{";
  out += "\"homey_api\":" + String(g_cfg->homey_enabled ? "true" : "false") + ",";
  out += "\"home_assistant_api\":" + String(g_cfg->ha_enabled ? "true" : "false") + ",";
  out += "\"home_assistant_mqtt\":" + String(g_cfg->ha_mqtt_enabled ? "true" : "false") + ",";
  out += "\"display_enabled\":" + String(g_cfg->display_enabled ? "true" : "false") + ",";
  out += "\"headless\":" + String(g_cfg->display_enabled ? "false" : "true") + ",";
  out += "\"modbus\":" + String(g_cfg->modbus_enabled ? "true" : "false") + ",";
  out += "\"control_writes\":" + String(controlActive ? "true" : "false") + ",";
  out += "\"bacnet_write\":" + String(g_cfg->bacnet_write_enabled ? "true" : "false");
  out += "},";
  out += "\"endpoints\":{";
  out += "\"status\":\"" + jsonEscape(statusUrl) + "\",";
  out += "\"mode_write_example\":\"" + jsonEscape(ctrlModeUrl) + "\",";
  out += "\"setpoint_write_example\":\"" + jsonEscape(ctrlSetpointUrl) + "\",";
  out += "\"auth_header_example\":\"Authorization: Bearer <TOKEN>\"";
  out += "},";
  out += "\"recommended_poll_interval_seconds\":180,";
  out += "\"flow_notes\":[";
  out += "\"Trigger: every 3-5 minutes\",";
  out += "\"Action: run HomeyScript VentReader Poll\",";
  out += "\"Optional: alarm/push notification on script failure\"";
  out += "],";
  out += "\"recommended_virtual_devices\":[";
  out += "{\"name\":\"VentReader - Uteluft\",\"capability\":\"measure_temperature\"},";
  out += "{\"name\":\"VentReader - Tilluft\",\"capability\":\"measure_temperature\"},";
  out += "{\"name\":\"VentReader - Avtrekk\",\"capability\":\"measure_temperature\"},";
  out += "{\"name\":\"VentReader - Avkast\",\"capability\":\"measure_temperature\"},";
  out += "{\"name\":\"VentReader - Fan %\",\"capability\":\"measure_percentage\"},";
  out += "{\"name\":\"VentReader - Heat %\",\"capability\":\"measure_percentage\"},";
  out += "{\"name\":\"VentReader - Gjenvinning %\",\"capability\":\"measure_percentage\"},";
  out += "{\"name\":\"VentReader - Modbus Alarm\",\"capability\":\"alarm_generic\",\"optional\":true},";
  out += "{\"name\":\"VentReader - Status tekst\",\"capability\":\"measure_text\",\"optional\":true}";
  out += "],";
  out += "\"control_ready\":";
  out += (controlActive ? "true" : "false");
  out += ",";
  out += "\"homey_script_js\":\"";
  out += jsonEscape(script);
  out += "\"";
  out += "}";
  return out;
}

static String buildHomeyExportText()
{
  const String base = currentBaseUrl();
  const String token = g_cfg->homey_api_token;
  const String statusUrl = base + "/status";
  const String src = normDataSource(g_cfg->data_source);
  const bool controlActive =
      (src == "MODBUS" && g_cfg->modbus_enabled && g_cfg->control_enabled) ||
      (src == "BACNET" && g_cfg->bacnet_write_enabled);

  String out;
  out.reserve(3600);
  out += "VentReader Homey setup\n";
  out += "======================\n\n";
  out += "Generated: " + isoFromEpochMs(nowEpochMs()) + "\n";
  out += "Model: " + g_cfg->model + "\n";
  out += "Firmware: " + String(FW_VERSION) + "\n";
  out += "Base URL: " + base + "\n";
  out += "Bearer token: " + token + "\n\n";
  out += "Enable in VentReader admin\n";
  out += "- Homey/API: " + String(g_cfg->homey_enabled ? "ON" : "OFF") + "\n";
  out += "- Modbus: " + String(g_cfg->modbus_enabled ? "ON" : "OFF") + "\n";
  out += "- Control writes: " + String(controlActive ? "ON" : "OFF") + " (optional)\n\n";
  out += "Status endpoint\n";
  out += "- " + statusUrl + "\n";
  out += "- Header: Authorization: Bearer " + token + "\n\n";
  out += "Recommended virtual devices\n";
  out += "- VentReader - Uteluft (measure_temperature)\n";
  out += "- VentReader - Tilluft (measure_temperature)\n";
  out += "- VentReader - Avtrekk (measure_temperature)\n";
  out += "- VentReader - Avkast (measure_temperature)\n";
  out += "- VentReader - Fan % (measure_percentage)\n";
  out += "- VentReader - Heat % (measure_percentage)\n";
  out += "- VentReader - Gjenvinning % (measure_percentage)\n\n";
  out += "HomeyScript (VentReader Poll)\n";
  out += "-----------------------------\n";
  out += "const VENTREADER_BASE = '" + base + "';\n";
  out += "const VENTREADER_TOKEN = '" + token + "';\n";
  out += "const VENTREADER_URL = VENTREADER_BASE + '/status';\n";
  out += "const MAP = {\n";
  out += "  'VentReader - Uteluft': { field: 'uteluft', cap: 'measure_temperature' },\n";
  out += "  'VentReader - Tilluft': { field: 'tilluft', cap: 'measure_temperature' },\n";
  out += "  'VentReader - Avtrekk': { field: 'avtrekk', cap: 'measure_temperature' },\n";
  out += "  'VentReader - Avkast':  { field: 'avkast',  cap: 'measure_temperature' },\n";
  out += "  'VentReader - Fan %':   { field: 'fan', cap: 'measure_percentage' },\n";
  out += "  'VentReader - Heat %':  { field: 'heat', cap: 'measure_percentage' },\n";
  out += "  'VentReader - Gjenvinning %': { field: 'efficiency', cap: 'measure_percentage' }\n";
  out += "};\n";
  out += "function num(v){ const n=Number(v); return Number.isFinite(n)?n:null; }\n";
  out += "async function setByName(devices,name,capability,value){\n";
  out += "  const d=Object.values(devices).find(x=>x.name===name);\n";
  out += "  if(!d||!d.capabilitiesObj||!d.capabilitiesObj[capability]) return;\n";
  out += "  await d.setCapabilityValue(capability,value);\n";
  out += "}\n";
  out += "const res = await fetch(VENTREADER_URL,{headers:{Authorization:`Bearer ${VENTREADER_TOKEN}`}}); if(!res.ok) throw new Error(`HTTP ${res.status}`);\n";
  out += "const s = await res.json(); const devices = await Homey.devices.getDevices();\n";
  out += "for (const [name,cfg] of Object.entries(MAP)) { const v=num(s[cfg.field]); if(v===null) continue; await setByName(devices,name,cfg.cap,v); }\n";
  out += "return `VentReader OK: ${s.time||'--:--'} ${s.modbus||''}`;\n";
  return out;
}

static void handleHealth()
{
  server.send(200, "text/plain", "ok");
}

static void handleAdminHomeyExportText()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed) { redirectTo("/admin/setup?step=1"); return; }
  const String out = buildHomeyExportText();
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Disposition", "attachment; filename=ventreader_homey_setup.txt");
  server.send(200, "text/plain; charset=utf-8", out);
}

static void handleAdminHomeyExport()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed)
  {
    redirectTo("/admin/setup?step=1");
    return;
  }

  String out = buildHomeyExportJson();
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Disposition", "attachment; filename=ventreader_homey_setup.json");
  server.send(200, "application/json", out);
}

static void handleAdminConfigExport()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed) { redirectTo("/admin/setup?step=1"); return; }
  String out = buildConfigExportJson();
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Disposition", "attachment; filename=ventreader_config_backup.json");
  server.send(200, "application/json", out);
}

static void handleAdminConfigImport()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed) { redirectTo("/admin/setup?step=1"); return; }
  if (!server.hasArg("cfgjson"))
  {
    server.send(400, "text/plain", "missing cfgjson");
    return;
  }
  const String j = server.arg("cfgjson");
  if (j.length() < 2)
  {
    server.send(400, "text/plain", "invalid json");
    return;
  }

  String s;
  bool b;
  int n;

  if (jsonGetString(j, "wifi_ssid", s)) g_cfg->wifi_ssid = s;
  if (jsonGetString(j, "wifi_pass", s)) g_cfg->wifi_pass = s;
  if (jsonGetString(j, "api_token", s) && s.length() >= 16) g_cfg->api_token = s;
  if (jsonGetString(j, "homey_api_token", s) && s.length() >= 16) g_cfg->homey_api_token = s;
  if (jsonGetString(j, "ha_api_token", s) && s.length() >= 16) g_cfg->ha_api_token = s;
  if (jsonGetString(j, "model", s)) g_cfg->model = normModel(s);
  if (jsonGetBool(j, "modbus_enabled", b)) g_cfg->modbus_enabled = b;
  if (jsonGetBool(j, "homey_enabled", b)) g_cfg->homey_enabled = b;
  if (jsonGetBool(j, "ha_enabled", b)) g_cfg->ha_enabled = b;
  if (jsonGetBool(j, "display_enabled", b)) g_cfg->display_enabled = b;
  if (jsonGetBool(j, "ha_mqtt_enabled", b)) g_cfg->ha_mqtt_enabled = b;
  if (jsonGetBool(j, "control_enabled", b)) g_cfg->control_enabled = b;
  if (jsonGetString(j, "data_source", s)) g_cfg->data_source = normDataSource(s);
  if (jsonGetString(j, "ha_mqtt_host", s)) g_cfg->ha_mqtt_host = s;
  if (jsonGetInt(j, "ha_mqtt_port", n) && n > 0 && n <= 65535) g_cfg->ha_mqtt_port = (uint16_t)n;
  if (jsonGetString(j, "ha_mqtt_user", s)) g_cfg->ha_mqtt_user = s;
  if (jsonGetString(j, "ha_mqtt_pass", s)) g_cfg->ha_mqtt_pass = s;
  if (jsonGetString(j, "ha_mqtt_prefix", s)) g_cfg->ha_mqtt_prefix = s;
  if (jsonGetString(j, "ha_mqtt_topic_base", s)) g_cfg->ha_mqtt_topic_base = s;
  if (jsonGetInt(j, "ha_mqtt_interval_s", n) && n >= 10 && n <= 3600) g_cfg->ha_mqtt_interval_s = (uint16_t)n;
  if (jsonGetString(j, "bacnet_ip", s)) g_cfg->bacnet_ip = s;
  if (jsonGetInt(j, "bacnet_port", n) && n > 0 && n <= 65535) g_cfg->bacnet_port = (uint16_t)n;
  if (jsonGetInt(j, "bacnet_device_id", n) && n > 0) g_cfg->bacnet_device_id = (uint32_t)n;
  if (jsonGetInt(j, "bacnet_poll_minutes", n) && n >= 5 && n <= 60) g_cfg->bacnet_poll_minutes = (uint8_t)n;
  if (jsonGetInt(j, "bacnet_timeout_ms", n) && n >= 300 && n <= 8000) g_cfg->bacnet_timeout_ms = (uint16_t)n;
  if (jsonGetBool(j, "bacnet_write_enabled", b)) g_cfg->bacnet_write_enabled = b;
  if (jsonGetString(j, "bacnet_obj_outdoor", s)) g_cfg->bacnet_obj_outdoor = s;
  if (jsonGetString(j, "bacnet_obj_supply", s)) g_cfg->bacnet_obj_supply = s;
  if (jsonGetString(j, "bacnet_obj_extract", s)) g_cfg->bacnet_obj_extract = s;
  if (jsonGetString(j, "bacnet_obj_exhaust", s)) g_cfg->bacnet_obj_exhaust = s;
  if (jsonGetString(j, "bacnet_obj_fan", s)) g_cfg->bacnet_obj_fan = s;
  if (jsonGetString(j, "bacnet_obj_heat", s)) g_cfg->bacnet_obj_heat = s;
  if (jsonGetString(j, "bacnet_obj_mode", s)) g_cfg->bacnet_obj_mode = s;
  if (jsonGetString(j, "bacnet_obj_setpoint_home", s)) g_cfg->bacnet_obj_setpoint_home = s;
  if (jsonGetString(j, "bacnet_obj_setpoint_away", s)) g_cfg->bacnet_obj_setpoint_away = s;
  if (jsonGetString(j, "bacnet_mode_map", s)) g_cfg->bacnet_mode_map = s;
  if (jsonGetString(j, "ui_language", s)) g_cfg->ui_language = normLang(s);
  if (jsonGetString(j, "modbus_transport_mode", s)) g_cfg->modbus_transport_mode = s;
  if (jsonGetString(j, "modbus_serial_format", s)) g_cfg->modbus_serial_format = s;
  if (jsonGetInt(j, "modbus_baud", n) && n >= 1200 && n <= 115200) g_cfg->modbus_baud = (uint32_t)n;
  if (jsonGetInt(j, "modbus_slave_id", n) && n >= 1 && n <= 247) g_cfg->modbus_slave_id = (uint8_t)n;
  if (jsonGetInt(j, "modbus_addr_offset", n) && n >= -5 && n <= 5) g_cfg->modbus_addr_offset = (int8_t)n;
  if (jsonGetInt(j, "poll_interval_ms", n) && n >= 30000 && n <= 3600000) g_cfg->poll_interval_ms = (uint32_t)n;
  if (g_cfg->display_enabled && g_cfg->poll_interval_ms < 180000) g_cfg->poll_interval_ms = 180000;

  if (g_cfg->homey_api_token.length() < 16) g_cfg->homey_api_token = g_cfg->api_token;
  if (g_cfg->ha_api_token.length() < 16) g_cfg->ha_api_token = g_cfg->api_token;
  if (!g_cfg->ha_enabled) g_cfg->ha_mqtt_enabled = false;
  if (g_cfg->ha_mqtt_enabled && g_cfg->ha_mqtt_host.length() == 0)
  {
    server.send(400, "text/plain", "HA MQTT host/IP must be set when HA MQTT is enabled");
    return;
  }
  config_apply_model_modbus_defaults(*g_cfg, false);
  config_save(*g_cfg);
  g_refresh_requested = true;
  redirectTo("/admin");
}

static String buildAdminManualText(bool noLang)
{
  String out;
  out.reserve(4600);
  if (noLang)
  {
    out += "VentReader Manual (kort)\n";
    out += "========================\n\n";
    out += "Quick start\n";
    out += "1) Logg inn pa /admin.\n";
    out += "2) Fullfor setup wizard.\n";
    out += "3) Verifiser /status med header Authorization: Bearer <HOMEY_TOKEN>.\n";
    out += "4) For Homey: bruk Eksporter Homey-oppsett.\n";
    out += "5) For HA: aktiver HA MQTT Discovery (native) i admin (anbefalt).\n";
    out += "   REST fallback: /ha/status med header Authorization: Bearer <HA_TOKEN>.\n";
    out += "6) Datakilde: velg Modbus (eksperimentell, lokal) eller BACnet (lokal).\n";
    out += "7) Uten skjerm: aktiver headless-modus i setup/admin og bruk /admin via IP.\n\n";
    out += "Skriving (valgfritt)\n";
    out += "- Modbus: Aktiver Modbus + Enable remote control writes (experimental)\n";
    out += "- BACnet: Aktiver Enable BACnet writes (experimental)\n";
    out += "- API mode: POST /api/control/mode + header Authorization: Bearer <MAIN_TOKEN> + body/query mode=AWAY|HOME|HIGH|FIRE\n";
    out += "- API setpoint: POST /api/control/setpoint + header Authorization: Bearer <MAIN_TOKEN> + body/query profile=home|away&value=18.5\n\n";
    out += "Feilsoking\n";
    out += "- 401: ugyldig eller manglende Bearer-token\n";
    out += "- 403 api disabled: modul er av\n";
    out += "- 403 control disabled: control writes er av\n";
    out += "- 409 modbus disabled: Modbus er av ved skrivekall\n";
    out += "- 403 bacnet write disabled: BACnet-skriving er av\n";
    out += "- 500 write failed: transport/protokollfeil i aktiv datakilde\n\n";
    out += "Sikkerhet\n";
    out += "- Endre fabrikkpassord\n";
    out += "- Del Homey/HA-token kun med lokale, betrodde integrasjoner\n";
    out += "- Hold skrivestyring avskrudd nar den ikke trengs\n";
  }
  else
  {
    out += "VentReader Manual (short)\n";
    out += "========================\n\n";
    out += "Quick start\n";
    out += "1) Log in at /admin.\n";
    out += "2) Complete setup wizard.\n";
    out += "3) Verify /status with header Authorization: Bearer <HOMEY_TOKEN> returns data.\n";
    out += "4) For Homey: use Export Homey setup.\n";
    out += "5) For HA: enable HA MQTT Discovery (native) in admin (recommended).\n";
    out += "   REST fallback: /ha/status with header Authorization: Bearer <HA_TOKEN>.\n";
    out += "6) Data source: choose Modbus (experimental, local) or BACnet (local).\n";
    out += "7) No display mounted: enable headless mode in setup/admin and use /admin via device IP.\n\n";
    out += "Writes (optional)\n";
    out += "- Modbus: enable Modbus + Enable remote control writes (experimental)\n";
    out += "- BACnet: enable Enable BACnet writes (experimental)\n";
    out += "- API mode: POST /api/control/mode + header Authorization: Bearer <MAIN_TOKEN> + body/query mode=AWAY|HOME|HIGH|FIRE\n";
    out += "- API setpoint: POST /api/control/setpoint + header Authorization: Bearer <MAIN_TOKEN> + body/query profile=home|away&value=18.5\n\n";
    out += "Troubleshooting\n";
    out += "- 401: invalid/missing bearer token\n";
    out += "- 403 api disabled: module is disabled\n";
    out += "- 403 control disabled: control writes disabled\n";
    out += "- 409 modbus disabled: Modbus disabled for write call\n";
    out += "- 403 bacnet write disabled: BACnet write is disabled\n";
    out += "- 500 write failed: transport/protocol issue on active source\n\n";
    out += "Security\n";
    out += "- Change default admin password\n";
    out += "- Share Homey/HA tokens only with trusted local integrations\n";
    out += "- Keep writes disabled unless needed\n";
  }
  return out;
}

static void handleAdminManualText()
{
  if (!checkAdminAuth()) return;
  const bool noLang = (lang() == "no");
  String txt = buildAdminManualText(noLang);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Disposition", "attachment; filename=ventreader_manual.txt");
  server.send(200, "text/plain; charset=utf-8", txt);
}

static void handleStatus()
{
  if (!requireApiScope(API_SCOPE_HOMEY_READ)) return;

  bool pretty = false;
  if (server.hasArg("pretty"))
  {
    String v = server.arg("pretty");
    pretty = (v == "1" || v == "true" || v == "yes");
  }

  server.send(200, "application/json", buildStatusJson(pretty));
}

static void handleStatusHistory()
{
  if (!g_cfg->homey_enabled && !g_cfg->ha_enabled)
  {
    server.send(403, "text/plain", "api disabled");
    return;
  }
  if (!requireApiScope(API_SCOPE_HOMEY_READ)) return;

  int limit = 120;
  if (server.hasArg("limit")) limit = server.arg("limit").toInt();
  if (limit < 1) limit = 1;
  if (limit > (int)HISTORY_CAP) limit = (int)HISTORY_CAP;
  if (limit > (int)g_hist_count) limit = (int)g_hist_count;

  String out;
  out.reserve((size_t)limit * 180 + 256);
  out += "{\"count\":";
  out += String(limit);
  out += ",\"items\":[";

  const size_t start = g_hist_count - (size_t)limit;
  for (size_t i = 0; i < (size_t)limit; i++)
  {
    const size_t logical = start + i;
    const size_t idx = (g_hist_head + HISTORY_CAP - g_hist_count + logical) % HISTORY_CAP;
    const StatusSnapshot& s = g_hist[idx];

    auto fOrNull = [](float v) -> String {
      if (isnan(v)) return "null";
      char t[24];
      snprintf(t, sizeof(t), "%.1f", v);
      return String(t);
    };

    if (i) out += ",";
    out += "{";
    out += "\"ts_epoch_ms\":";
    out += u64ToString(s.ts_epoch_ms);
    out += ",\"ts_iso\":\"";
    out += jsonEscape(isoFromEpochMs(s.ts_epoch_ms));
    out += "\",\"mode\":\"";
    out += jsonEscape(String(s.mode));
    out += "\",\"uteluft\":";
    out += fOrNull(s.uteluft);
    out += ",\"tilluft\":";
    out += fOrNull(s.tilluft);
    out += ",\"avtrekk\":";
    out += fOrNull(s.avtrekk);
    out += ",\"avkast\":";
    out += fOrNull(s.avkast);
    out += ",\"fan\":";
    out += String(s.fan);
    out += ",\"heat\":";
    out += String(s.heat);
    out += ",\"efficiency\":";
    out += String(s.efficiency);
    out += ",\"modbus\":\"";
    out += jsonEscape(String(s.modbus));
    out += "\",\"stale\":";
    out += (s.stale ? "true" : "false");
    out += "}";
  }

  out += "]}";
  server.send(200, "application/json", out);
}

static void handleStatusDiag()
{
  if (!g_cfg->homey_enabled && !g_cfg->ha_enabled)
  {
    server.send(403, "text/plain", "api disabled");
    return;
  }
  if (!requireApiScope(API_SCOPE_HOMEY_READ)) return;

  String out;
  out.reserve(420);
  out += "{";
  out += "\"fw\":\"" + jsonEscape(String(FW_VERSION)) + "\",";
  out += "\"total_updates\":" + String(g_diag_total_updates) + ",";
  out += "\"mb_ok\":" + String(g_diag_mb_ok) + ",";
  out += "\"mb_err\":" + String(g_diag_mb_err) + ",";
  out += "\"mb_off\":" + String(g_diag_mb_off) + ",";
  out += "\"stale\":" + String(g_diag_stale) + ",";
  out += "\"last_mb\":\"" + jsonEscape(g_diag_last_mb) + "\",";
  out += "\"last_sample_epoch_ms\":" + u64ToString(g_diag_last_sample_ms);
  out += "}";
  server.send(200, "application/json", out);
}

static void handleStatusStorage()
{
  if (!g_cfg->homey_enabled && !g_cfg->ha_enabled)
  {
    server.send(403, "text/plain", "api disabled");
    return;
  }
  if (!requireApiScope(API_SCOPE_HOMEY_READ)) return;

  String out;
  out.reserve(360);
  out += "{";
  out += "\"history_cap\":" + String((unsigned long)HISTORY_CAP) + ",";
  out += "\"history_count\":" + String((unsigned long)g_hist_count) + ",";
  out += "\"history_memory_bytes\":" + String(historyMemoryBytes()) + ",";
  out += "\"history_storage\":\"ram_only\",";
  out += "\"free_heap_bytes\":" + String(ESP.getFreeHeap()) + ",";
  out += "\"min_free_heap_bytes\":" + String(ESP.getMinFreeHeap()) + ",";
  out += "\"free_psram_bytes\":" + String(ESP.getFreePsram());
  out += "}";
  server.send(200, "application/json", out);
}

static void handleStatusHistoryCsv()
{
  if (!g_cfg->homey_enabled && !g_cfg->ha_enabled)
  {
    server.send(403, "text/plain", "api disabled");
    return;
  }
  if (!requireApiScope(API_SCOPE_HOMEY_READ)) return;

  int limit = 240;
  if (server.hasArg("limit")) limit = server.arg("limit").toInt();
  if (limit < 1) limit = 1;
  if (limit > (int)HISTORY_CAP) limit = (int)HISTORY_CAP;
  if (limit > (int)g_hist_count) limit = (int)g_hist_count;

  String out;
  out.reserve((size_t)limit * 110 + 200);
  out += "ts_epoch_ms,ts_iso,mode,uteluft,tilluft,avtrekk,avkast,fan,heat,efficiency,modbus,stale\n";

  const size_t start = g_hist_count - (size_t)limit;
  for (size_t i = 0; i < (size_t)limit; i++)
  {
    const size_t logical = start + i;
    const size_t idx = (g_hist_head + HISTORY_CAP - g_hist_count + logical) % HISTORY_CAP;
    const StatusSnapshot& s = g_hist[idx];

    auto fCsv = [](float v) -> String {
      if (isnan(v)) return "";
      char t[24];
      snprintf(t, sizeof(t), "%.1f", v);
      return String(t);
    };

    out += u64ToString(s.ts_epoch_ms) + ",";
    out += isoFromEpochMs(s.ts_epoch_ms) + ",";
    out += String(s.mode) + ",";
    out += fCsv(s.uteluft) + ",";
    out += fCsv(s.tilluft) + ",";
    out += fCsv(s.avtrekk) + ",";
    out += fCsv(s.avkast) + ",";
    out += String(s.fan) + ",";
    out += String(s.heat) + ",";
    out += String(s.efficiency) + ",";
    out += String(s.modbus) + ",";
    out += (s.stale ? "1" : "0");
    out += "\n";
  }

  server.sendHeader("Content-Disposition", "attachment; filename=ventreader_history.csv");
  server.send(200, "text/csv", out);
}

static void applyModbusApiRuntime()
{
  FlexitModbusRuntimeConfig mcfg;
  mcfg.model = g_cfg->model;
  mcfg.baud = g_cfg->modbus_baud;
  mcfg.slave_id = g_cfg->modbus_slave_id;
  mcfg.addr_offset = g_cfg->modbus_addr_offset;
  mcfg.serial_format = g_cfg->modbus_serial_format;
  mcfg.transport_mode = g_cfg->modbus_transport_mode;
  flexit_modbus_set_runtime_config(mcfg);
  flexit_modbus_set_enabled(g_cfg->modbus_enabled);
}

static bool isQuickControlActive()
{
  const String src = normDataSource(g_cfg->data_source);
  return (src == "MODBUS" && g_cfg->modbus_enabled && g_cfg->control_enabled) ||
         (src == "BACNET" && g_cfg->bacnet_write_enabled);
}

static bool bacnetErrAccessDenied(const String& s)
{
  return s.indexOf("class=2 code=40") >= 0;
}

static bool bacnetErrUnknownObject(const String& s)
{
  return s.indexOf("class=1 code=31") >= 0;
}

static void mapBacnetWriteError(const String& op, const String& profile, const String& raw,
                                String& err, int& statusCode)
{
  if (bacnetErrAccessDenied(raw))
  {
    statusCode = 409;
    if (op == "mode")
    {
      err = "bacnet mode write denied by unit policy (" + raw + ")";
      return;
    }
    if (profile == "away")
    {
      err = "bacnet away setpoint write denied by unit policy (" + raw + ")";
      return;
    }
    err = "bacnet setpoint write denied by unit policy (" + raw + ")";
    return;
  }
  if (bacnetErrUnknownObject(raw))
  {
    statusCode = 409;
    err = "bacnet object/property not writable on this unit (" + raw + ")";
    return;
  }
  statusCode = 500;
  err = "write failed: " + raw;
}

static bool runControlWriteMode(const String& mode, String& err, int& statusCode)
{
  const String src = normDataSource(g_cfg->data_source);
  err = "";
  statusCode = 500;

  if (src == "MODBUS")
  {
    if (!g_cfg->control_enabled) { err = "control disabled"; statusCode = 403; return false; }
    if (!g_cfg->modbus_enabled) { err = "modbus disabled"; statusCode = 409; return false; }
    applyModbusApiRuntime();
    if (!flexit_modbus_write_mode(mode))
    {
      err = String("write mode failed: ") + flexit_modbus_last_error();
      statusCode = 500;
      return false;
    }
    return true;
  }

  if (src == "BACNET")
  {
    if (!g_cfg->bacnet_write_enabled) { err = "bacnet write disabled"; statusCode = 403; return false; }
    flexit_bacnet_set_runtime_config(*g_cfg);
    if (!flexit_bacnet_write_mode(mode))
    {
      mapBacnetWriteError("mode", "", String(flexit_bacnet_last_error()), err, statusCode);
      return false;
    }
    return true;
  }

  err = "control disabled for selected data source";
  statusCode = 403;
  return false;
}

static bool runControlWriteSetpoint(const String& profile, float value, String& err, int& statusCode)
{
  const String src = normDataSource(g_cfg->data_source);
  err = "";
  statusCode = 500;

  if (src == "MODBUS")
  {
    if (!g_cfg->control_enabled) { err = "control disabled"; statusCode = 403; return false; }
    if (!g_cfg->modbus_enabled) { err = "modbus disabled"; statusCode = 409; return false; }
    applyModbusApiRuntime();
    if (!flexit_modbus_write_setpoint(profile, value))
    {
      err = String("write setpoint failed: ") + flexit_modbus_last_error();
      statusCode = 500;
      return false;
    }
    return true;
  }

  if (src == "BACNET")
  {
    if (!g_cfg->bacnet_write_enabled) { err = "bacnet write disabled"; statusCode = 403; return false; }
    flexit_bacnet_set_runtime_config(*g_cfg);
    if (!flexit_bacnet_write_setpoint(profile, value))
    {
      mapBacnetWriteError("setpoint", profile, String(flexit_bacnet_last_error()), err, statusCode);
      return false;
    }
    return true;
  }

  err = "control disabled for selected data source";
  statusCode = 403;
  return false;
}

static void appendQuickControlCard(String& s, bool publicPage)
{
  s += "<div class='card'><h2>" + tr("quick_control") + "</h2>";
  if (isQuickControlActive())
  {
    const bool bacnetSrc = (normDataSource(g_cfg->data_source) == "BACNET");
    String modeNow = g_data.mode;
    modeNow.trim();
    modeNow.toUpperCase();
    if (modeNow == "VARME" || modeNow == "HJEM" || modeNow == "HJEMME") modeNow = "HOME";
    if (modeNow == "BORTE") modeNow = "AWAY";
    if (modeNow == "HOY" || modeNow == "HØY") modeNow = "HIGH";

    const bool awayMode = (modeNow == "AWAY");
    const String profileDefault = bacnetSrc ? "home" : (awayMode ? "away" : "home");
    String setVal = "20.0";
    if (!isnan(g_data.set_temp))
    {
      char b[16];
      snprintf(b, sizeof(b), "%.1f", g_data.set_temp);
      setVal = String(b);
    }

    const String awayBtnCls = String("btn secondary") + (modeNow == "AWAY" ? " mode-active" : "");
    const String homeBtnCls = String("btn secondary") + (modeNow == "HOME" ? " mode-active" : "");
    const String highBtnCls = String("btn secondary") + (modeNow == "HIGH" ? " mode-active" : "");
    const String fireBtnCls = String("btn secondary") + (modeNow == "FIRE" ? " mode-active" : "");

    s += "<div class='help'>" + tr("quick_control_help") + "</div>";
    s += "<div class='actions'>";
    s += "<form method='POST' action='/admin/control/mode' style='margin:0'><input type='hidden' name='mode' value='AWAY'><button class='" + awayBtnCls + "' type='submit'>" + tr("mode_away") + "</button></form>";
    s += "<form method='POST' action='/admin/control/mode' style='margin:0'><input type='hidden' name='mode' value='HOME'><button class='" + homeBtnCls + "' type='submit'>" + tr("mode_home") + "</button></form>";
    s += "<form method='POST' action='/admin/control/mode' style='margin:0'><input type='hidden' name='mode' value='HIGH'><button class='" + highBtnCls + "' type='submit'>" + tr("mode_high") + "</button></form>";
    s += "<form method='POST' action='/admin/control/mode' style='margin:0'><input type='hidden' name='mode' value='FIRE'><button class='" + fireBtnCls + "' type='submit'>" + tr("mode_fire") + "</button></form>";
    s += "</div>";
    s += "<div class='sep-gold'></div>";
    s += "<form method='POST' action='/admin/control/setpoint'>";
    s += "<div class='row'>";
    s += "<div><label>" + tr("profile") + "</label><select name='profile'><option value='home'" + String(profileDefault == "home" ? " selected" : "") + ">home</option><option value='away'" + String(profileDefault == "away" ? " selected" : "") + ">away</option></select></div>";
    s += "<div><label>" + tr("setpoint") + "</label><input name='value' type='number' min='10' max='30' step='0.5' value='" + setVal + "'></div>";
    s += "</div>";
    s += "<div class='actions'><button class='btn secondary' type='submit'>" + tr("apply_setpoint") + "</button></div>";
    s += "</form>";
    if (bacnetSrc) s += "<div class='help'>" + tr("quick_control_bacnet_hint") + "</div>";
    if (publicPage) s += "<div class='help'>Krever innlogging i admin ved aktivt system.</div>";
  }
  else
  {
    s += "<div class='help'>" + tr("enable_control_hint") + "</div>";
  }
  s += "</div>";
}

static void handleHaStatus()
{
  if (!g_cfg->ha_enabled)
  {
    server.send(403, "text/plain", "home assistant/api disabled");
    return;
  }
  if (!requireApiScope(API_SCOPE_HA_READ)) return;

  bool pretty = false;
  if (server.hasArg("pretty"))
  {
    String v = server.arg("pretty");
    pretty = (v == "1" || v == "true" || v == "yes");
  }
  server.send(200, "application/json", buildStatusJson(pretty));
}

static void handleHaHistory()
{
  if (!g_cfg->ha_enabled)
  {
    server.send(403, "text/plain", "home assistant/api disabled");
    return;
  }
  if (!requireApiScope(API_SCOPE_HA_READ)) return;

  int limit = 120;
  if (server.hasArg("limit")) limit = server.arg("limit").toInt();
  if (limit < 1) limit = 1;
  if (limit > (int)HISTORY_CAP) limit = (int)HISTORY_CAP;
  if (limit > (int)g_hist_count) limit = (int)g_hist_count;

  String out;
  out.reserve((size_t)limit * 180 + 256);
  out += "{\"count\":";
  out += String(limit);
  out += ",\"items\":[";

  const size_t start = g_hist_count - (size_t)limit;
  for (size_t i = 0; i < (size_t)limit; i++)
  {
    const size_t logical = start + i;
    const size_t idx = (g_hist_head + HISTORY_CAP - g_hist_count + logical) % HISTORY_CAP;
    const StatusSnapshot& s = g_hist[idx];

    auto fOrNull = [](float v) -> String {
      if (isnan(v)) return "null";
      char t[24];
      snprintf(t, sizeof(t), "%.1f", v);
      return String(t);
    };

    if (i) out += ",";
    out += "{";
    out += "\"ts_epoch_ms\":";
    out += u64ToString(s.ts_epoch_ms);
    out += ",\"ts_iso\":\"";
    out += jsonEscape(isoFromEpochMs(s.ts_epoch_ms));
    out += "\",\"mode\":\"";
    out += jsonEscape(String(s.mode));
    out += "\",\"uteluft\":";
    out += fOrNull(s.uteluft);
    out += ",\"tilluft\":";
    out += fOrNull(s.tilluft);
    out += ",\"avtrekk\":";
    out += fOrNull(s.avtrekk);
    out += ",\"avkast\":";
    out += fOrNull(s.avkast);
    out += ",\"fan\":";
    out += String(s.fan);
    out += ",\"heat\":";
    out += String(s.heat);
    out += ",\"efficiency\":";
    out += String(s.efficiency);
    out += ",\"modbus\":\"";
    out += jsonEscape(String(s.modbus));
    out += "\",\"stale\":";
    out += (s.stale ? "true" : "false");
    out += "}";
  }

  out += "]}";
  server.send(200, "application/json", out);
}

static void handleHaHistoryCsv()
{
  if (!g_cfg->ha_enabled)
  {
    server.send(403, "text/plain", "home assistant/api disabled");
    return;
  }
  if (!requireApiScope(API_SCOPE_HA_READ)) return;

  int limit = 240;
  if (server.hasArg("limit")) limit = server.arg("limit").toInt();
  if (limit < 1) limit = 1;
  if (limit > (int)HISTORY_CAP) limit = (int)HISTORY_CAP;
  if (limit > (int)g_hist_count) limit = (int)g_hist_count;

  String out;
  out.reserve((size_t)limit * 110 + 200);
  out += "ts_epoch_ms,ts_iso,mode,uteluft,tilluft,avtrekk,avkast,fan,heat,efficiency,modbus,stale\n";

  const size_t start = g_hist_count - (size_t)limit;
  for (size_t i = 0; i < (size_t)limit; i++)
  {
    const size_t logical = start + i;
    const size_t idx = (g_hist_head + HISTORY_CAP - g_hist_count + logical) % HISTORY_CAP;
    const StatusSnapshot& s = g_hist[idx];

    auto fCsv = [](float v) -> String {
      if (isnan(v)) return "";
      char t[24];
      snprintf(t, sizeof(t), "%.1f", v);
      return String(t);
    };

    out += u64ToString(s.ts_epoch_ms) + ",";
    out += isoFromEpochMs(s.ts_epoch_ms) + ",";
    out += String(s.mode) + ",";
    out += fCsv(s.uteluft) + ",";
    out += fCsv(s.tilluft) + ",";
    out += fCsv(s.avtrekk) + ",";
    out += fCsv(s.avkast) + ",";
    out += String(s.fan) + ",";
    out += String(s.heat) + ",";
    out += String(s.efficiency) + ",";
    out += String(s.modbus) + ",";
    out += (s.stale ? "1" : "0");
    out += "\n";
  }

  server.sendHeader("Content-Disposition", "attachment; filename=ventreader_history.csv");
  server.send(200, "text/csv", out);
}

static void handleControlMode()
{
  if (!requireApiScope(API_SCOPE_CONTROL_WRITE)) return;
  if (!server.hasArg("mode")) { server.send(400, "text/plain", "missing mode"); return; }
  String err;
  int statusCode = 500;
  if (!runControlWriteMode(server.arg("mode"), err, statusCode))
  {
    server.send(statusCode, "text/plain", err);
    return;
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleControlSetpoint()
{
  if (!requireApiScope(API_SCOPE_CONTROL_WRITE)) return;
  if (!server.hasArg("profile") || !server.hasArg("value"))
  {
    server.send(400, "text/plain", "missing profile/value");
    return;
  }

  const String profile = server.arg("profile");
  const float value = server.arg("value").toFloat();
  String err;
  int statusCode = 500;
  if (!runControlWriteSetpoint(profile, value, err, statusCode))
  {
    server.send(statusCode, "text/plain", err);
    return;
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleAdminControlMode()
{
  if (!checkAdminAuth()) return;
  if (!server.hasArg("mode"))
  {
    redirectTo("/admin");
    return;
  }
  String err;
  int statusCode = 500;
  if (!runControlWriteMode(server.arg("mode"), err, statusCode))
  {
    if (statusCode == 403 || statusCode == 409) { redirectTo("/admin"); return; }
    server.send(statusCode, "text/plain", String("control mode failed: ") + err);
    return;
  }
  redirectTo("/admin");
}

static void handleAdminControlSetpoint()
{
  if (!checkAdminAuth()) return;
  if (!server.hasArg("profile") || !server.hasArg("value"))
  {
    redirectTo("/admin");
    return;
  }
  String err;
  int statusCode = 500;
  if (!runControlWriteSetpoint(server.arg("profile"), server.arg("value").toFloat(), err, statusCode))
  {
    if (statusCode == 403 || statusCode == 409) { redirectTo("/admin"); return; }
    server.send(statusCode, "text/plain", String("control setpoint failed: ") + err);
    return;
  }
  redirectTo("/admin");
}

static String pageHeader(const String& title, const String& subtitle = "")
    {
      String s;
      s += "<!doctype html><html><head><meta charset='utf-8'>";
      s += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
      s += "<title>" + title + "</title>";

      // Minimal-fuzz "micro framework" with CSS variables + responsive layout.
      // Theme: Light (default) and Dark (deep gray + gold #C2A17E)
      s += "<style>";
      s += "*,*::before,*::after{box-sizing:border-box;}";
      s += ":root{--bg:#f6f7f9;--card:#ffffff;--text:#111827;--muted:#6b7280;--border:#e5e7eb;--accent:#C2A17E;--btn:#111827;--btnText:#ffffff;--input:#ffffff;--shadow:0 10px 25px rgba(0,0,0,.06);}";

      s += "[data-theme='dark']{--bg:#111315;--card:#1a1d20;--text:#e5e7eb;--muted:#a1a1aa;--border:#2a2f35;--accent:#C2A17E;--btn:#C2A17E;--btnText:#111315;--input:#111315;--shadow:0 10px 25px rgba(0,0,0,.35);}";

      s += "html,body{height:100%;}";
      s += "body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial; background:var(--bg); color:var(--text);}";
      s += ".wrap{max-width:920px;margin:0 auto;padding:18px;}";
      s += ".topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px;}";
      s += ".brand{display:flex;flex-direction:column;line-height:1.15;}";
      s += ".brand h1{font-size:18px;margin:0;letter-spacing:.4px;}";
      s += ".brand small{color:var(--muted);}";
      s += ".brand-actions{display:flex;gap:8px;margin-top:8px;}";
      s += ".btn.mini{padding:6px 10px;border-radius:10px;font-size:12px;line-height:1.2;}";
      s += ".pill{display:inline-flex;align-items:center;gap:8px;padding:8px 10px;border:1px solid var(--border);border-radius:999px;background:var(--card);box-shadow:var(--shadow);}";
      s += ".dot{width:10px;height:10px;border-radius:50%;background:var(--muted);}";
      s += ".dot.ok{background:#22c55e;}.dot.warn{background:#f59e0b;}.dot.bad{background:#ef4444;}";
      s += ".grid{display:grid;grid-template-columns:1fr;gap:14px;}";
      s += "@media(min-width:860px){.grid{grid-template-columns:1fr 1fr;}}";
      s += ".admin-grid{display:block;}";
      s += ".admin-grid .card{margin-bottom:14px;}";
      s += "@media(min-width:980px){.admin-grid{column-count:2;column-gap:14px;}.admin-grid .card{display:inline-block;width:100%;margin:0 0 14px;break-inside:avoid;page-break-inside:avoid;vertical-align:top;}}";
      s += ".card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:14px;box-shadow:var(--shadow);}";
      s += ".card h2{font-size:14px;margin:0 0 10px 0;color:var(--muted);text-transform:uppercase;letter-spacing:.12em;}";
      s += "details{border:1px dashed var(--border);border-radius:12px;padding:8px 10px;margin-top:10px;}";
      s += "summary{cursor:pointer;font-weight:700;color:var(--text);outline:none;}";
      s += "label{display:block;font-size:12px;color:var(--muted);margin-top:10px;}";
      s += "input,select{width:100%;padding:11px 12px;border-radius:12px;border:1px solid var(--border);background:var(--input);color:var(--text);outline:none;}";
      s += "input:focus,select:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(194,161,126,.22);}";
      s += ".row{display:flex;gap:12px;flex-wrap:wrap;}";
      s += ".row > *{flex:1 1 220px;}";
      s += ".btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:11px 14px;border-radius:12px;border:1px solid var(--border);background:var(--btn);color:var(--btnText);font-weight:600;cursor:pointer;}";
      s += ".btn.secondary{background:transparent;color:var(--text);}";
      s += ".btn.mode-active{background:rgba(194,161,126,.20);border-color:var(--accent);color:var(--accent);}";
      s += ".btn.danger{background:#ef4444;border-color:#ef4444;color:#fff;}";
      s += ".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px;}";
      s += ".action-grid{display:grid;grid-template-columns:1fr;gap:10px;}";
      s += "@media(min-width:620px){.action-grid{grid-template-columns:1fr 1fr;}}";
      s += ".muted-title{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.10em;margin:0 0 6px 0;}";
      s += ".help{font-size:12px;color:var(--muted);margin-top:8px;line-height:1.35;}";
      s += ".progress{display:flex;gap:8px;align-items:center;margin-top:10px;}";
      s += ".step{height:8px;flex:1;border-radius:999px;background:var(--border);overflow:hidden;}";
      s += ".step > i{display:block;height:100%;width:0;background:var(--accent);}";
      s += ".kpi{display:flex;gap:14px;flex-wrap:wrap;}";
      s += ".kpi .kv{min-width:140px;}";
      s += ".kpi .k{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.12em;}";
      s += ".kpi .v{font-size:16px;font-weight:700;margin-top:4px;}";
      s += ".sep-gold{height:1px;background:var(--accent);opacity:.65;margin:12px 0;}";
      s += ".sep-gold-dashed{height:0;border-top:1px dashed var(--accent);opacity:.7;margin:12px 0;}";
      s += ".lang{padding:8px 10px;border-radius:10px;border:1px solid var(--border);background:var(--card);color:var(--text);}";
      s += "a{color:var(--accent);text-decoration:none;}";
      s += "code{background:rgba(194,161,126,.18);padding:2px 6px;border-radius:8px;}";
      s += "</style>";

      s += "<script>";
      s += "(function(){var t=localStorage.getItem('theme');if(t){document.documentElement.setAttribute('data-theme',t);}else{var prefers=window.matchMedia&&window.matchMedia('(prefers-color-scheme: dark)').matches;document.documentElement.setAttribute('data-theme',prefers?'dark':'light');}})();";
      s += "function toggleTheme(){var cur=document.documentElement.getAttribute('data-theme')||'light';var nxt=(cur==='dark')?'light':'dark';document.documentElement.setAttribute('data-theme',nxt);localStorage.setItem('theme',nxt);}";
      s += "function toggleSecret(id){var el=document.getElementById(id);if(!el)return;el.type=(el.type==='password')?'text':'password';}";
      s += "async function copySecret(id){var el=document.getElementById(id);if(!el)return;try{if(navigator.clipboard&&navigator.clipboard.writeText){await navigator.clipboard.writeText(el.value);}else{el.select();document.execCommand('copy');}alert('Kopiert');}catch(e){alert('Kopiering feilet');}}";
      s += "</script>";

      s += "</head><body><div class='wrap'>";

      s += "<div class='topbar'>";
      s += "<div class='brand'><h1>" + title + "</h1>";
      if (subtitle.length()) s += "<small>" + subtitle + "</small>";
      if (title != "VentReader")
      {
        s += "<div class='brand-actions'><a class='btn secondary mini' href='/'>" + tr("back_home") + "</a></div>";
      }
      s += "</div>";
      s += "<div style='display:flex;gap:8px;align-items:center;'>";
      if (g_cfg)
      {
        s += "<form method='POST' action='/admin/lang' style='margin:0'>";
        s += "<label style='display:none'>" + tr("language") + "</label>";
        s += "<select class='lang' name='lang' onchange='this.form.submit()'>";
        const String cl = lang();
        s += "<option value='no'" + String(cl == "no" ? " selected" : "") + ">NO</option>";
        s += "<option value='da'" + String(cl == "da" ? " selected" : "") + ">DA</option>";
        s += "<option value='sv'" + String(cl == "sv" ? " selected" : "") + ">SV</option>";
        s += "<option value='fi'" + String(cl == "fi" ? " selected" : "") + ">FI</option>";
        s += "<option value='en'" + String(cl == "en" ? " selected" : "") + ">EN</option>";
        s += "<option value='uk'" + String(cl == "uk" ? " selected" : "") + ">UKR</option>";
        s += "</select></form>";
      }
      s += "<button class='btn secondary' type='button' onclick='toggleTheme()'>☾ / ☀</button>";
      s += "</div>";
      s += "</div>";
      return s;
    }

static String pageFooter(){ return "</div></body></html>"; }

static void handleRoot()
{
  String s = pageHeader("VentReader", tr("admin_portal"));

  bool apOn  = (WiFi.getMode() & WIFI_AP);
  bool staOn = (WiFi.status() == WL_CONNECTED);

  s += "<div class='grid'>";
  s += "<div class='card'><h2>" + tr("status") + "</h2>";
  s += "<div class='kpi'>";
  s += "<div class='kv'><div class='k'>STA WiFi</div><div class='v'>" + String(staOn ? "Connected" : "Offline") + "</div></div>";
  s += "<div class='kv'><div class='k'>Fallback AP</div><div class='v'>" + String(apOn ? "ON" : "OFF") + "</div></div>";
  s += "<div class='kv'><div class='k'>mDNS</div><div class='v'>ventreader.local</div></div>";
  s += "<div class='kv'><div class='k'>Firmware</div><div class='v'>" + String(FW_VERSION) + "</div></div>";
  s += "</div>";
  s += "<div class='help'>API: <code>/status</code> med <code>Authorization: Bearer ...</code> (Homey polling). Debug: <code>?pretty=1</code></div>";
  s += "</div>";

  appendQuickControlCard(s, true);

  auto fOrDash = [](float v) -> String {
    if (isnan(v)) return "-";
    char t[16];
    snprintf(t, sizeof(t), "%.1f", v);
    return String(t);
  };

  s += "<div class='card'><h2>Moduler (offentlig)</h2>";
  s += "<div class='kpi'>";
  s += "<div class='kv'><div class='k'>Datakilde</div><div class='v'>" + dataSourceLabel(g_cfg->data_source) + "</div></div>";
  s += "<div class='kv'><div class='k'>Homey/API</div><div class='v'>" + boolLabel(g_cfg->homey_enabled) + "</div></div>";
  s += "<div class='kv'><div class='k'>Home Assistant/API</div><div class='v'>" + boolLabel(g_cfg->ha_enabled) + "</div></div>";
  s += "<div class='kv'><div class='k'>Display</div><div class='v'>" + String(g_cfg->display_enabled ? "ON" : "HEADLESS") + "</div></div>";
  s += "<div class='kv'><div class='k'>API nødstopp</div><div class='v'>" + String(g_cfg->api_panic_stop ? "ON" : "OFF") + "</div></div>";
  s += "<div class='kv'><div class='k'>HA MQTT Discovery</div><div class='v'>" + boolLabel(g_cfg->ha_mqtt_enabled) + "</div></div>";
  if (g_cfg->ha_mqtt_enabled)
    s += "<div class='kv'><div class='k'>" + tr("ha_mqtt_status") + "</div><div class='v'>" + (ha_mqtt_is_active() ? "CONNECTED" : (ha_mqtt_last_error().length() ? ha_mqtt_last_error() : "CONNECTING")) + "</div></div>";
  s += "<div class='kv'><div class='k'>Modbus</div><div class='v'>" + boolLabel(g_cfg->modbus_enabled) + "</div></div>";
  s += "<div class='kv'><div class='k'>BACnet (local)</div><div class='v'>" + boolLabel(normDataSource(g_cfg->data_source) == "BACNET") + "</div></div>";
  const bool ctrlActive = isQuickControlActive();
  s += "<div class='kv'><div class='k'>Control writes</div><div class='v'>" + boolLabel(ctrlActive) + "</div></div>";
  s += "</div>";
  s += "<div class='help'>Dette er lesbar oversikt uten innlogging. Konfigurasjon krever admin-login.</div>";
  s += "</div>";

  s += "<div class='card'><h2>Live data (offentlig)</h2>";
  s += "<div class='kpi'>";
  s += "<div class='kv'><div class='k'>Model</div><div class='v'>" + jsonEscape(g_data.device_model) + "</div></div>";
  s += "<div class='kv'><div class='k'>Mode</div><div class='v'>" + jsonEscape(g_data.mode) + "</div></div>";
  s += "<div class='kv'><div class='k'>Source status</div><div class='v'>" + jsonEscape(g_mb) + "</div></div>";
  s += "<div class='kv'><div class='k'>Ute / Tilluft</div><div class='v'>" + fOrDash(g_data.uteluft) + " / " + fOrDash(g_data.tilluft) + " C</div></div>";
  s += "</div>";
  s += "<div class='help'>Siste sample fra enheten. Full JSON krever Bearer-token i Authorization-header.</div>";
  s += "</div>";

  s += "<div class='card'><h2>" + tr("actions") + "</h2>";
  s += "<div class='actions'>";
  s += "<a class='btn' href='/admin'>" + tr("open_admin") + "</a>";
  s += "<a class='btn secondary' href='/health'>/health</a>";
  s += "</div>";
  s += "<div class='help'>" + tr("admin_protected") + "</div>";
  s += "</div>";

  s += "</div>"; // grid
  s += pageFooter();
  server.send(200, "text/html", s);
}

static void handleAdminLang()
{
  if (!checkAdminAuth()) return;
  if (server.hasArg("lang"))
  {
    g_cfg->ui_language = normLang(server.arg("lang"));
    config_save(*g_cfg);
  }
  redirectTo("/admin");
}

// Forced setup page (change admin pass on first login)
static void wizardProgress(String& s, int step)
{
  s += "<div class='progress'>";
  for (int i = 1; i <= 3; i++)
  {
    s += "<div class='step'><i style='width:" + String(i <= step ? 100 : 0) + "%'></i></div>";
  }
  s += "</div>";
  s += "<div class='help'>Steg " + String(step) + " av 3</div>";
}

static void handleAdminSetup()
{
  if (!checkAdminAuth()) return;

  int step = 1;
  if (server.hasArg("step")) step = server.arg("step").toInt();
  if (step < 1) step = 1;
  if (step > 3) step = 3;

  String s = pageHeader("Oppsett", "Stegvis konfigurasjon (første login)");
  wizardProgress(s, step);
  s += "<div class='card'><h2>Onboarding</h2>";
  s += "<div class='help'>Førstegangsoppsett er pre-auth: du blir ikke bedt om innlogging før oppsett er fullført. ";
  s += "Steg: 1) nytt admin-passord, 2) WiFi, 3) datakilde/integrasjoner/display.</div>";
  s += "</div>";

  if (step == 1)
  {
    s += "<div class='card'><h2>Admin passord</h2>";
    s += "<form method='POST' action='/admin/setup_save?step=1'>";
    s += "<label>Nytt passord (min 8 tegn)</label><input name='p1' type='password' required>";
    s += "<label>Gjenta passord</label><input name='p2' type='password' required>";
    s += "<div class='actions'><button class='btn' type='submit'>Neste</button></div>";
    s += "</form>";
    s += "<div class='help'>Du kan ikke bruke admin-siden før passordet er endret.</div>";
    s += "</div>";
  }
  else if (step == 2)
  {
    s += "<div class='card'><h2>WiFi</h2>";
    s += "<form method='POST' action='/admin/setup_save?step=2'>";
    s += "<label>SSID</label><input name='ssid' value='" + jsonEscape(g_cfg->wifi_ssid) + "' required>";
    s += "<label>Passord</label><input name='wpass' type='password' value=''>";
    s += "<div class='actions'><a class='btn secondary' href='/admin/setup?step=1'>Tilbake</a><button class='btn' type='submit'>Neste</button></div>";
    s += "</form>";
    s += "<div class='help'>Passord kan stå tomt dersom nettverket er åpent.</div>";
    s += "</div>";
  }
  else // step 3
  {
    const bool forceApiDecision = !g_cfg->setup_completed;
    const bool apiChoiceError = server.hasArg("api_choice_error");
    s += "<div class='card'><h2>Token + moduler</h2>";
    s += "<form id='setup_form' method='POST' action='/admin/setup_save?step=3'>";
    s += "<label>Enhetsmodell</label>";
    s += "<select id='model_setup' name='model' class='input'>";
    s += "<option value='S3'" + String(g_cfg->model == "S3" ? " selected" : "") + ">Nordic S3</option>";
    s += "<option value='S4'" + String(g_cfg->model == "S4" ? " selected" : "") + ">Nordic S4</option>";
    s += "<option value='S2_EXP'" + String(g_cfg->model == "S2_EXP" ? " selected" : "") + ">Nordic S2 (Experimental)</option>";
    s += "<option value='S7_EXP'" + String(g_cfg->model == "S7_EXP" ? " selected" : "") + ">Nordic S7 (Experimental)</option>";
    s += "<option value='CL3_EXP'" + String(g_cfg->model == "CL3_EXP" ? " selected" : "") + ">Nordic CL3 (Experimental)</option>";
    s += "<option value='CL4_EXP'" + String(g_cfg->model == "CL4_EXP" ? " selected" : "") + ">Nordic CL4 (Experimental)</option>";
    s += "</select>";
    s += "<div class='sep-gold'></div>";
    s += "<label>API Bearer-token (main/control)</label><input id='setup_token_main' class='mono' type='password' name='token' value='" + jsonEscape(g_cfg->api_token) + "' required>";
    s += "<div class='actions'><button class='btn secondary' type='button' onclick='toggleSecret(\"setup_token_main\")'>Vis/skjul</button><button class='btn secondary' type='button' onclick='copySecret(\"setup_token_main\")'>Kopier</button></div>";
    s += "<label>Homey Bearer-token (/status)</label><input id='setup_token_homey' class='mono' type='password' name='homey_token' value='" + jsonEscape(g_cfg->homey_api_token) + "' required>";
    s += "<div class='actions'><button class='btn secondary' type='button' onclick='toggleSecret(\"setup_token_homey\")'>Vis/skjul</button><button class='btn secondary' type='button' onclick='copySecret(\"setup_token_homey\")'>Kopier</button></div>";
    s += "<label>Home Assistant Bearer-token (/ha/*)</label><input id='setup_token_ha' class='mono' type='password' name='ha_token' value='" + jsonEscape(g_cfg->ha_api_token) + "' required>";
    s += "<div class='actions'><button class='btn secondary' type='button' onclick='toggleSecret(\"setup_token_ha\")'>Vis/skjul</button><button class='btn secondary' type='button' onclick='copySecret(\"setup_token_ha\")'>Kopier</button></div>";
    s += "<div class='sep-gold'></div>";
    s += "<div class='help'>Velg eksplisitt om Homey/API og Home Assistant/API skal v&aelig;re aktivert eller deaktivert. Du kan deaktivere begge for ren monitor-modus.</div>";
    if (apiChoiceError)
      s += "<div class='help' style='color:#b91c1c'>Du m&aring; velge enten Aktiver eller Deaktiver for b&aring;de Homey/API og Home Assistant/API.</div>";
    s += "<div><strong>" + tr("homey_api") + "</strong></div>";
    s += "<label><input type='radio' name='homey_mode' value='enable'"
         + String(g_cfg->homey_enabled ? " checked" : "")
         + String(forceApiDecision ? " required" : "")
         + "> Aktiver</label>";
    s += "<label><input type='radio' name='homey_mode' value='disable'"
         + String(!g_cfg->homey_enabled ? " checked" : "")
         + "> Deaktiver</label>";
    s += "<div class='sep-gold'></div>";
    s += "<div><strong>" + tr("ha_api") + "</strong></div>";
    s += "<label><input type='radio' name='ha_mode' value='enable'"
         + String(g_cfg->ha_enabled ? " checked" : "")
         + String(forceApiDecision ? " required" : "")
         + "> Aktiver</label>";
    s += "<label><input type='radio' name='ha_mode' value='disable'"
         + String(!g_cfg->ha_enabled ? " checked" : "")
         + "> Deaktiver</label>";
    s += "<div class='sep-gold'></div>";
    s += "<label><input id='hamqtt_setup' type='checkbox' name='hamqtt' " + String(g_cfg->ha_mqtt_enabled ? "checked" : "") + "> " + tr("ha_mqtt") + "</label>";
    s += "<div id='hamqtt_block_setup' style='display:" + String(g_cfg->ha_mqtt_enabled ? "block" : "none") + ";'>";
    s += "<div class='help'>" + tr("ha_mqtt_help") + "</div>";
    if (!ha_mqtt_lib_available()) s += "<div class='help' style='color:#b91c1c'>PubSubClient-bibliotek mangler i firmware-build.</div>";
    s += "<div class='row'>";
    s += "<div><label>" + tr("ha_mqtt_host") + "</label><input name='hamhost' value='" + jsonEscape(g_cfg->ha_mqtt_host) + "'></div>";
    s += "<div><label>" + tr("ha_mqtt_port") + "</label><input name='hamport' type='number' min='1' max='65535' value='" + String((int)g_cfg->ha_mqtt_port) + "'></div>";
    s += "</div>";
    s += "<div class='row'>";
    s += "<div><label>" + tr("ha_mqtt_user") + "</label><input name='hamuser' value='" + jsonEscape(g_cfg->ha_mqtt_user) + "'></div>";
    s += "<div><label>" + tr("ha_mqtt_pass") + "</label><input name='hampass' type='password' value='" + jsonEscape(g_cfg->ha_mqtt_pass) + "'></div>";
    s += "</div>";
    s += "<div class='row'>";
    s += "<div><label>" + tr("ha_mqtt_prefix") + "</label><input name='hampfx' value='" + jsonEscape(g_cfg->ha_mqtt_prefix) + "'></div>";
    s += "<div><label>" + tr("ha_mqtt_interval") + "</label><input name='hamint' type='number' min='10' max='3600' value='" + String((int)g_cfg->ha_mqtt_interval_s) + "'></div>";
    s += "</div>";
    s += "<label>" + tr("ha_mqtt_base") + "</label><input name='hambase' value='" + jsonEscape(g_cfg->ha_mqtt_topic_base) + "'>";
    s += "</div>";
    s += "</div>";
    s += "<div class='card'><h2>Inndata-moduler</h2>";
    s += "<div class='help'>Velg aktiv datakilde. Kun innstillinger for valgt kilde vises.</div>";
    s += "<div class='sep-gold'></div>";
    const bool srcWeb = (normDataSource(g_cfg->data_source) == "BACNET");
    s += "<div><strong>" + tr("data_source") + "</strong></div>";
    s += "<label><input type='radio' name='src' value='MODBUS'" + String(!srcWeb ? " checked" : "") + "> " + tr("source_modbus") + "</label>";
    s += "<label><input type='radio' name='src' value='BACNET'" + String(srcWeb ? " checked" : "") + "> " + tr("source_bacnet") + "</label>";
    s += "<div id='mb_block_setup' style='display:" + String(!srcWeb ? "block" : "none") + ";'>";
    s += "<div class='sep-gold-dashed'></div>";
    s += "<label><input id='mb_toggle_setup' type='checkbox' name='modbus' " + String(g_cfg->modbus_enabled ? "checked" : "") + "> Modbus</label>";
    s += "<div id='mb_adv_setup' style='display:" + String((!srcWeb && g_cfg->modbus_enabled) ? "block" : "none") + ";'>";
    s += "<div class='help'>Avanserte Modbus-innstillinger</div>";
    s += "<label><input type='checkbox' name='ctrl' " + String(g_cfg->control_enabled ? "checked" : "") + "> " + tr("control_enable") + "</label>";
    s += "<select id='mbtr_setup' name='mbtr' class='input'>";
    s += "<option value='AUTO'" + String(g_cfg->modbus_transport_mode == "AUTO" ? " selected" : "") + ">AUTO (anbefalt for auto-dir modul)</option>";
    s += "<option value='MANUAL'" + String(g_cfg->modbus_transport_mode == "MANUAL" ? " selected" : "") + ">MANUAL (DE/RE styres av GPIO)</option>";
    s += "</select>";
    s += "<div class='row'>";
    s += "<div><label>Modbus baud</label><input id='mbbaud_setup' name='mbbaud' type='number' min='1200' max='115200' value='" + String(g_cfg->modbus_baud) + "'></div>";
    s += "<div><label>Slave ID</label><input id='mbid_setup' name='mbid' type='number' min='1' max='247' value='" + String((int)g_cfg->modbus_slave_id) + "'></div>";
    s += "<div><label>Addr offset</label><input id='mboff_setup' name='mboff' type='number' min='-5' max='5' value='" + String((int)g_cfg->modbus_addr_offset) + "'></div>";
    s += "</div>";
    s += "<label>Serial format</label>";
    s += "<select id='mbser_setup' name='mbser' class='input'>";
    s += "<option value='8N1'" + String(g_cfg->modbus_serial_format == "8N1" ? " selected" : "") + ">8N1</option>";
    s += "<option value='8E1'" + String(g_cfg->modbus_serial_format == "8E1" ? " selected" : "") + ">8E1</option>";
    s += "<option value='8O1'" + String(g_cfg->modbus_serial_format == "8O1" ? " selected" : "") + ">8O1</option>";
    s += "</select>";
    s += "</div>";
    s += "</div>";
    s += "<div id='bac_block_setup' style='display:" + String(srcWeb ? "block" : "none") + ";'>";
    s += "<div class='sep-gold-dashed'></div>";
    s += "<div id='fw_adv_setup' style='display:" + String(srcWeb ? "block" : "none") + ";'>";
    s += "<div class='help'>" + tr("source_bacnet_help") + "</div>";
    s += "<div class='help'>" + tr("source_bacnet_default_map_hint") + "</div>";
    s += "<div class='row'>";
    s += "<div><label>" + tr("bac_ip") + "</label><input name='bacip' value='" + jsonEscape(g_cfg->bacnet_ip) + "' required></div>";
    s += "<div><label>" + tr("bac_device_id") + "</label><input name='bacid' type='number' min='1' max='4194303' value='" + String(g_cfg->bacnet_device_id) + "' required></div>";
    s += "</div>";
    s += "<div class='row'>";
    s += "<div><label>" + tr("bac_port") + "</label><input name='bacport' type='number' min='1' max='65535' value='" + String((int)g_cfg->bacnet_port) + "'></div>";
    s += "<div><label>" + tr("bac_poll_min") + "</label><input name='bacpoll' type='number' min='5' max='60' value='" + String((int)g_cfg->bacnet_poll_minutes) + "'></div>";
    s += "<div><label>" + tr("bac_timeout") + "</label><input name='bacto' type='number' min='300' max='8000' value='" + String((int)g_cfg->bacnet_timeout_ms) + "'></div>";
    s += "</div>";
    s += "<label><input type='checkbox' name='bacwr' " + String(g_cfg->bacnet_write_enabled ? "checked" : "") + "> " + tr("bac_write_enable") + "</label>";
    s += "<details style='margin-top:8px'><summary>" + tr("bac_objects") + "</summary>";
    s += "<div class='help'>Format for objekter: <code>ai:1</code>, <code>av:2</code>, <code>msv:1</code></div>";
    s += "<div class='row'><div><label>Uteluft</label><input name='baout' value='" + jsonEscape(g_cfg->bacnet_obj_outdoor) + "'></div><div><label>Tilluft</label><input name='basup' value='" + jsonEscape(g_cfg->bacnet_obj_supply) + "'></div></div>";
    s += "<div class='row'><div><label>Avtrekk</label><input name='baext' value='" + jsonEscape(g_cfg->bacnet_obj_extract) + "'></div><div><label>Avkast</label><input name='baexh' value='" + jsonEscape(g_cfg->bacnet_obj_exhaust) + "'></div></div>";
    s += "<div class='row'><div><label>Fan %</label><input name='bafan' value='" + jsonEscape(g_cfg->bacnet_obj_fan) + "'></div><div><label>Heat %</label><input name='baheat' value='" + jsonEscape(g_cfg->bacnet_obj_heat) + "'></div></div>";
    s += "<div class='row'><div><label>" + tr("bac_setpoint_home_obj") + "</label><input name='bashome' value='" + jsonEscape(g_cfg->bacnet_obj_setpoint_home) + "'></div><div><label>" + tr("bac_setpoint_away_obj") + "</label><input name='basaway' value='" + jsonEscape(g_cfg->bacnet_obj_setpoint_away) + "'></div></div>";
    s += "<div class='row'><div><label>Mode object</label><input name='bamode' value='" + jsonEscape(g_cfg->bacnet_obj_mode) + "'></div><div><label>" + tr("bac_mode_map") + "</label><input name='bamap' value='" + jsonEscape(g_cfg->bacnet_mode_map) + "'></div></div>";
    s += "</details>";
  s += "<div class='actions' style='margin-top:10px'><button class='btn secondary' type='button' onclick='testBACnet(\"setup_form\")'>Test BACnet</button><button class='btn secondary' type='button' onclick='discoverBACnet(\"setup_form\")'>Autodiscover</button><button class='btn secondary' type='button' onclick='probeBACnetObjects(\"setup_form\")'>Object probe</button><button class='btn secondary' type='button' onclick='scanBACnetObjects(\"setup_form\")'>Object scan</button><button class='btn secondary' type='button' onclick='probeBACnetWrite(\"setup_form\")'>Write probe (exp)</button></div>";
    s += "<div id='fw_test_result_setup' class='help'></div>";
    s += "<pre id='probe_values_setup' style='display:none;white-space:pre-wrap;max-height:240px;overflow:auto;border:1px solid #2a3344;border-radius:10px;padding:10px;font-size:12px;line-height:1.35;background:#0b1220;color:#dbeafe;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace'></pre>";
    s += "<div class='actions' style='margin-top:8px'><button class='btn secondary' type='button' onclick='showBACnetDebug(\"setup\")'>Vis debuglogg</button><button class='btn secondary' type='button' onclick='copyBACnetDebug(\"setup\")'>Kopier logg</button><button class='btn secondary' type='button' onclick='clearBACnetDebug(\"setup\")'>Tøm debuglogg</button></div>";
    s += "<pre id='bacnet_debug_setup' data-on='0' style='display:none;white-space:pre-wrap;max-height:220px;overflow:auto;border:1px solid #2a3344;border-radius:10px;padding:10px;font-size:11px;line-height:1.45;background:#0b1220;color:#dbeafe;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace'></pre>";
    s += "</div>";
    s += "</div>";
    s += "<script>(function(){"
         "var t=document.getElementById('mb_toggle_setup');var a=document.getElementById('mb_adv_setup');"
         "var fw=document.getElementById('fw_adv_setup');var mbb=document.getElementById('mb_block_setup');var bcb=document.getElementById('bac_block_setup');"
         "var hm=document.getElementById('hamqtt_setup');var hmb=document.getElementById('hamqtt_block_setup');"
         "var src=document.querySelectorAll('input[name=\"src\"]');"
         "var haModes=document.querySelectorAll('input[name=\"ha_mode\"]');"
         "var m=document.getElementById('model_setup');var tr=document.getElementById('mbtr_setup');"
         "var sf=document.getElementById('mbser_setup');var bd=document.getElementById('mbbaud_setup');"
         "var id=document.getElementById('mbid_setup');var of=document.getElementById('mboff_setup');"
         "if(!t||!a)return;"
         "function srcVal(){for(var i=0;i<src.length;i++){if(src[i].checked)return src[i].value;}return 'MODBUS';}"
         "function haEnabled(){for(var i=0;i<haModes.length;i++){if(haModes[i].checked)return haModes[i].value==='enable';}return true;}"
         "function u(){var useMb=(srcVal()==='MODBUS');if(mbb)mbb.style.display=useMb?'block':'none';if(bcb)bcb.style.display=useMb?'none':'block';a.style.display=(useMb&&t.checked)?'block':'none';if(fw)fw.style.display=useMb?'none':'block';if(hmb)hmb.style.display=(hm&&hm.checked&&haEnabled())?'block':'none';}"
         "function p(model){tr.value='AUTO';sf.value='8E1';bd.value='9600';id.value='1';of.value='0';}"
         "t.addEventListener('change',u);"
         "if(hm)hm.addEventListener('change',function(){if(hm.checked){for(var i=0;i<haModes.length;i++){if(haModes[i].value==='enable'){haModes[i].checked=true;break;}}}u();});"
         "for(var i=0;i<haModes.length;i++){haModes[i].addEventListener('change',u);}"
         "for(var i=0;i<src.length;i++){src[i].addEventListener('change',u);}"
         "if(m){m.addEventListener('change',function(){if(t.checked){p(m.value);}});}"
         "u();"
         "async function prepBACnetDebug(scope){"
         "try{await fetch('/admin/clear_bacnet_debug',{method:'POST',credentials:'same-origin'});}catch(e){}"
         "try{var en1=new URLSearchParams(); en1.append('enable','1'); await fetch('/admin/bacnet_debug_mode',{method:'POST',credentials:'same-origin',body:en1});}catch(e){}"
         "var dbg=document.getElementById(scope==='admin'?'bacnet_debug_admin':'bacnet_debug_setup');"
         "if(dbg && dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent='(samler ny logg...)';}"
         "}"
         "window.testBACnet=async function(formId){"
         "var form=document.getElementById(formId);if(!form)return;"
         "await prepBACnetDebug('setup');"
         "var target=document.getElementById('fw_test_result_setup')||document.getElementById('fw_test_result_admin');"
         "var dbg=document.getElementById('bacnet_debug_setup')||document.getElementById('bacnet_debug_admin');"
         "if(target){target.style.color='var(--muted)';target.textContent='Tester BACnet...';}"
         "try{"
         "var fd=new FormData(form);"
         "var body=new URLSearchParams();"
         "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
         "var r=await fetch('/admin/test_bacnet',{method:'POST',credentials:'same-origin',body:body});"
         "var j=await r.json();"
         "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'(ingen debug)';}"
         "if(!j.ok){if(target){target.style.color='#b91c1c';target.textContent='BACnet test feilet: '+(j.error||'ukjent feil')+(j.ip?(' | ip='+j.ip):'');}return;}"
         "if(target){target.style.color='#166534';target.textContent='BACnet OK. Modus '+(j.data&&j.data.mode?j.data.mode:'N/A')+', tilluft '+(j.data&&j.data.tilluft!==null?j.data.tilluft:'-')+' C.';}"
         "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='BACnet test feilet: '+e.message;}}"
         "};"
         "window.discoverBACnet=async function(formId){"
         "var form=document.getElementById(formId);if(!form)return;"
         "await prepBACnetDebug('setup');"
         "var target=document.getElementById('fw_test_result_setup')||document.getElementById('fw_test_result_admin');"
         "var dbg=document.getElementById('bacnet_debug_setup')||document.getElementById('bacnet_debug_admin');"
         "if(target){target.style.color='var(--muted)';target.textContent='Soker etter BACnet-enheter...';}"
         "try{"
         "var r=await fetch('/admin/discover_bacnet',{method:'POST',credentials:'same-origin'});"
         "var j=await r.json();"
         "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'(ingen debug)';}"
         "if(!j.ok||!j.items||!j.items.length){if(target){target.style.color='#b91c1c';target.textContent='Ingen BACnet-enheter funnet ('+(j.error||'ukjent')+')';}return;}"
         "var i=j.items[0];"
         "var ip=form.querySelector('input[name=\"bacip\"]'); if(ip&&i.ip) ip.value=i.ip;"
         "var id=form.querySelector('input[name=\"bacid\"]'); if(id&&i.device_id) id.value=i.device_id;"
         "if(target){target.style.color='#166534';target.textContent='Fant '+j.items.length+' enhet(er). Fylte inn første: '+i.ip+' / ID '+i.device_id;}"
         "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Autodiscover feilet: '+e.message;}}"
         "};"
         "window.probeBACnetObjects=async function(formId){"
         "var form=document.getElementById(formId);if(!form)return;"
         "await prepBACnetDebug('setup');"
         "var target=document.getElementById('fw_test_result_setup')||document.getElementById('fw_test_result_admin');"
         "var dbg=document.getElementById('bacnet_debug_setup')||document.getElementById('bacnet_debug_admin');"
         "var listEl=document.getElementById('probe_values_setup')||document.getElementById('probe_values_admin');"
         "if(target){target.style.color='var(--muted)';target.textContent='Prober alle lesbare BACnet-objekter...';}"
         "if(listEl){listEl.style.display='none';listEl.textContent='';}"
         "try{"
         "var fd=new FormData(form);"
         "var body=new URLSearchParams();"
         "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
         "body.append('from_inst','0');"
         "body.append('to_inst','200');"
         "body.append('scan_timeout','450');"
         "body.append('scan_max','220');"
         "var r=await fetch('/admin/probe_bacnet_objects',{method:'POST',credentials:'same-origin',body:body});"
         "var raw=await r.text();"
         "var j={}; try{ j=JSON.parse(raw);}catch(pe){ throw new Error('Ugyldig JSON-respons fra /admin/probe_bacnet_objects: '+raw.slice(0,160)); }"
         "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'';}"
         "var items=Array.isArray(j.items)?j.items:[];"
         "if(!j.ok||!items.length){if(target){target.style.color='#b91c1c';target.textContent='Object probe fant ingen lesbare objekter ('+(j.error||'ukjent')+')';}return;}"
         "var all=[]; for(var i=0;i<items.length;i++){var it=items[i]||{};all.push((it.obj||'?')+'='+(it.value!==undefined?it.value:'?'));}"
         "if(listEl){listEl.style.display='block';listEl.textContent=all.join('\\n');}"
         "if(target){target.style.color='#166534';target.textContent='Object probe OK: fant '+items.length+' lesbare objekter. Full liste vises under.';}"
         "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Object probe feilet: '+e.message;}}"
         "};"
         "window.scanBACnetObjects=async function(formId){"
         "var form=document.getElementById(formId);if(!form)return;"
         "await prepBACnetDebug('setup');"
         "var target=document.getElementById('fw_test_result_setup')||document.getElementById('fw_test_result_admin');"
         "var dbg=document.getElementById('bacnet_debug_setup')||document.getElementById('bacnet_debug_admin');"
         "var from=prompt('Start instance', '0'); if(from===null)return;"
         "var to=prompt('Slutt instance', '64'); if(to===null)return;"
         "var otype=prompt('Objekttype (ai/ao/av/msv eller tom for alle)', 'ai'); if(otype===null)return;"
         "if(target){target.style.color='var(--muted)';target.textContent='Scanner BACnet-objekter...';}"
         "try{"
         "var fd=new FormData(form);"
         "var body=new URLSearchParams();"
         "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
         "body.append('from_inst', from);"
         "body.append('to_inst', to);"
         "body.append('scan_timeout','450');"
         "body.append('scan_max','220');"
         "body.append('otype', otype||'');"
         "var r=await fetch('/admin/scan_bacnet_objects',{method:'POST',credentials:'same-origin',body:body});"
         "var j=await r.json();"
         "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'';}"
         "var items=Array.isArray(j.items)?j.items:[];"
         "if(!j.ok||!items.length){if(target){target.style.color='#b91c1c';target.textContent='Object scan fant ingen lesbare objekter ('+(j.error||'ukjent')+')';}return;}"
         "var shown=[]; for(var i=0;i<items.length&&i<16;i++){var it=items[i]||{};shown.push((it.obj||'?')+'='+(it.value!==undefined?it.value:'?'));}"
         "if(target){target.style.color='#166534';target.textContent='Object scan fant '+items.length+' lesbare objekter. Eksempler: '+shown.join(', ');}"
         "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Object scan feilet: '+e.message;}}"
         "};"
         "window.probeBACnetWrite=async function(formId){"
         "var form=document.getElementById(formId);if(!form)return;"
         "await prepBACnetDebug('setup');"
         "var target=document.getElementById('fw_test_result_setup')||document.getElementById('fw_test_result_admin');"
         "var dbg=document.getElementById('bacnet_debug_setup')||document.getElementById('bacnet_debug_admin');"
         "if(target){target.style.color='var(--muted)';target.textContent='Kjører eksperimentell BACnet write-probe...';}"
         "try{"
         "var fd=new FormData(form);"
         "var body=new URLSearchParams();"
         "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
         "var r=await fetch('/admin/probe_bacnet_write',{method:'POST',credentials:'same-origin',body:body});"
         "var j=await r.json();"
         "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'';}"
         "var rr=j.results||{};"
         "var msg='Mode: '+(rr.mode&&rr.mode.ok?'OK':(rr.mode&&rr.mode.error?rr.mode.error:'-'))+' | Home setpoint: '+(rr.setpoint_home&&rr.setpoint_home.ok?'OK':(rr.setpoint_home&&rr.setpoint_home.error?rr.setpoint_home.error:'-'))+' | Away setpoint: '+(rr.setpoint_away&&rr.setpoint_away.ok?'OK':(rr.setpoint_away&&rr.setpoint_away.error?rr.setpoint_away.error:'-'));"
         "if(target){target.style.color=(j.ok?'#166534':'#b91c1c');target.textContent='Write probe (eksperimentell): '+msg;}"
         "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Write probe feilet: '+e.message;}}"
         "};"
         "window.showBACnetDebug=async function(scope){"
         "var dbg=document.getElementById(scope==='admin'?'bacnet_debug_admin':'bacnet_debug_setup');"
         "if(!dbg)return;"
         "try{"
         "var en1=new URLSearchParams(); en1.append('enable','1');"
         "await fetch('/admin/bacnet_debug_mode',{method:'POST',credentials:'same-origin',body:en1});"
         "var r=await fetch('/admin/bacnet_debug.txt',{credentials:'same-origin'});"
         "var t=await r.text();"
         "dbg.dataset.on='1';dbg.style.display='block';dbg.textContent=t&&t.length?t:'(tom logg)';"
         "}catch(e){dbg.style.display='block';dbg.textContent='Kunne ikke hente logg: '+e.message;}"
         "};"
         "window.clearBACnetDebug=async function(scope){"
         "var dbg=document.getElementById(scope==='admin'?'bacnet_debug_admin':'bacnet_debug_setup');"
         "try{await fetch('/admin/clear_bacnet_debug',{method:'POST',credentials:'same-origin'});}catch(e){}"
         "if(dbg){dbg.style.display='block';dbg.textContent='(tom logg)';}"
         "};"
         "window.copyBACnetDebug=async function(scope){"
         "var target=document.getElementById(scope==='admin'?'fw_test_result_admin':'fw_test_result_setup');"
         "try{"
         "var r=await fetch('/admin/bacnet_debug.txt',{credentials:'same-origin'});"
         "var t=await r.text();"
         "if(!t || !t.length) t='(tom logg)';"
         "if(navigator.clipboard && navigator.clipboard.writeText){await navigator.clipboard.writeText(t);}"
         "else{var ta=document.createElement('textarea');ta.value=t;document.body.appendChild(ta);ta.select();document.execCommand('copy');document.body.removeChild(ta);}"
         "if(target){target.style.color='#166534';target.textContent='Debuglogg kopiert til utklippstavlen.';}"
         "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Kopiering av logg feilet: '+e.message;}}"
         "};"
         "})();</script>";
    s += "</div>";

    s += "<div class='card'><h2>Display</h2>";
    s += "<label><input type='checkbox' name='disp' " + String(!g_cfg->display_enabled ? "checked" : "") + "> " + tr("headless") + "</label>";
    s += "<label>" + tr("poll_sec") + "</label><input name='poll' type='number' min='30' max='3600' value='" + String(g_cfg->poll_interval_ms/1000) + "'>";
    s += "<div class='help'>" + tr("poll_help") + "</div>";
    s += "<div class='actions'><a class='btn secondary' href='/admin/setup?step=2'>Tilbake</a><button class='btn' type='submit'>Fullfør &amp; restart</button></div>";
    s += "</form>";
    s += "<div class='help'>Når du fullfører, blir oppsettet lagret og du kan gå til admin.</div>";
    s += "</div>";
  }

  s += pageFooter();
  server.send(200, "text/html", s);
}

static void handleAdminSetupSave()
{
  if (!checkAdminAuth()) return;

  int step = 1;
  if (server.hasArg("step")) step = server.arg("step").toInt();
  if (step < 1) step = 1;
  if (step > 3) step = 3;

  if (step == 1)
  {
    String p1 = server.arg("p1");
    String p2 = server.arg("p2");
    if (p1.length() < 8 || p1 != p2)
    {
      server.send(400, "text/plain", "Passord matcher ikke eller er for kort (min 8 tegn).");
      return;
    }

    // Do NOT apply password yet. We keep current BasicAuth creds until Finish,
    // otherwise the browser will require re-login mid-wizard.
    g_pending_admin_pass = p1;
    g_pending_admin_set  = true;

    redirectTo("/admin/setup?step=2");
    return;
  }
  

  if (!g_pending_admin_set)
  {
    redirectTo("/admin/setup?step=1");
    return;
  }

  if (step == 2)
  {
    g_cfg->wifi_ssid = server.arg("ssid");
    g_cfg->wifi_pass = server.arg("wpass");
    config_save(*g_cfg);
    redirectTo("/admin/setup?step=3");
    return;
  }

  // step 3
  bool modelChanged = false;
  if (server.hasArg("model"))
  {
    String nm = normModel(server.arg("model"));
    modelChanged = (nm != g_cfg->model);
    g_cfg->model = nm;
  }
  if (modelChanged) config_apply_model_modbus_defaults(*g_cfg, true);

  g_cfg->api_token = server.arg("token");
  g_cfg->homey_api_token = server.arg("homey_token");
  g_cfg->ha_api_token = server.arg("ha_token");
  if (g_cfg->homey_api_token.length() < 16) g_cfg->homey_api_token = g_cfg->api_token;
  if (g_cfg->ha_api_token.length() < 16) g_cfg->ha_api_token = g_cfg->api_token;
  g_cfg->modbus_enabled = server.hasArg("modbus");
  String homeyMode = server.arg("homey_mode");
  String haMode = server.arg("ha_mode");
  bool homeyModeValid = (homeyMode == "enable" || homeyMode == "disable");
  bool haModeValid = (haMode == "enable" || haMode == "disable");
  if (!homeyModeValid || !haModeValid)
  {
    redirectTo("/admin/setup?step=3&api_choice_error=1");
    return;
  }
  g_cfg->homey_enabled  = (homeyMode == "enable");
  g_cfg->ha_enabled     = (haMode == "enable");
  g_cfg->display_enabled = !server.hasArg("disp");
  g_cfg->data_source = normDataSource(server.arg("src"));
  g_cfg->control_enabled = (g_cfg->data_source == "MODBUS") ? server.hasArg("ctrl") : false;
  if (server.hasArg("lang")) g_cfg->ui_language = normLang(server.arg("lang"));
  applyPostedModbusSettings();
  applyPostedBACnetSettings();
  applyPostedHAMqttSettings();
  if (g_cfg->ha_mqtt_enabled) g_cfg->ha_enabled = true;
  if (!g_cfg->ha_enabled) g_cfg->ha_mqtt_enabled = false;
  if (g_cfg->ha_mqtt_enabled && !ha_mqtt_lib_available())
  {
    server.send(400, "text/plain", "HA MQTT krever PubSubClient-biblioteket.");
    return;
  }
  if (g_cfg->ha_mqtt_enabled && g_cfg->ha_mqtt_host.length() == 0)
  {
    server.send(400, "text/plain", "HA MQTT host/IP må fylles ut.");
    return;
  }
  if (g_cfg->data_source == "BACNET")
  {
    g_mb = "BACNET configured (not verified)";
  }

  uint32_t pollSec = (uint32_t) server.arg("poll").toInt();
  if (pollSec < 30) pollSec = 30;
  if (g_cfg->display_enabled && pollSec < 180) pollSec = 180;
  if (pollSec > 3600) pollSec = 3600;
  g_cfg->poll_interval_ms = pollSec * 1000UL;

  // Apply pending admin password *now* (finish event)
  if (g_pending_admin_set)
  {
    g_cfg->admin_pass = g_pending_admin_pass;
    g_cfg->admin_pass_changed = true;
  }

  // Wizard is now complete
  g_cfg->setup_completed = true;

  config_save(*g_cfg);
  g_refresh_requested = true;

  // clear pending (avoid re-applying)
  g_pending_admin_pass = "";
  g_pending_admin_set = false;

  String s = pageHeader("Oppsett", "Fullført");
  s += "<div class='card'><h2>Oppsett fullført</h2>";
  s += "<p>Innstillinger er lagret. Enheten restarter nå for å aktivere WiFi og sikkerhetsinnstillinger.</p>";
  s += "<div class='help'>Hvis siden ikke oppdaterer seg automatisk innen 10 sekunder, koble deg til enhetens nye IP/hostname og prøv igjen.</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div class='help'>" + tr("mdns_return_hint") + "</div>";
  s += "<div class='actions'><a class='btn secondary' href='http://ventreader.local/admin'>" + tr("mdns_open_device") + "</a></div>";
  s += "<div class='help'>mDNS: <code>http://ventreader.local/admin</code></div>";
  s += "</div>";
  s += "<div class='card'><h2>Handling</h2>";
  s += "<form method='POST' action='/admin/reboot'><button class='btn' type='submit'>Restart enheten nå</button></form>";
  s += "</div>";
  s += pageFooter();

  server.send(200, "text/html", s);

  // Auto-restart shortly after sending the response
  g_restart_pending = true;
  g_restart_at_ms = millis() + 1500;
}
static void handleAdmin()
{
  if (!checkAdminAuth()) return;

  // Force setup until wizard completed
  if (!g_cfg->setup_completed)
  {
    redirectTo("/admin/setup?step=1");
    return;
  }

  String s = pageHeader("Admin", "Innstillinger");
  s += "<div class='grid admin-grid'>";

  auto fOrDash = [](float v) -> String {
    if (isnan(v)) return "-";
    char t[16];
    snprintf(t, sizeof(t), "%.1f", v);
    return String(t);
  };

  // Admin status + one-click API links
  const bool staOnAdmin  = (WiFi.status() == WL_CONNECTED);
  const bool apOnAdmin   = (WiFi.getMode() & WIFI_AP);
  const String statusApiUrl = "/admin/api/preview";
  const String statusApiPrettyUrl = "/admin/api/preview?pretty=1";
  s += "<div class='card'><h2>Status + API</h2>";
  s += "<div class='kpi'>";
  s += "<div class='kv'><div class='k'>STA WiFi</div><div class='v'>" + String(staOnAdmin ? "Connected" : "Offline") + "</div></div>";
  s += "<div class='kv'><div class='k'>Fallback AP</div><div class='v'>" + String(apOnAdmin ? "ON" : "OFF") + "</div></div>";
  s += "<div class='kv'><div class='k'>Datakilde</div><div class='v'>" + dataSourceLabel(g_cfg->data_source) + "</div></div>";
  s += "<div class='kv'><div class='k'>Kilde-status</div><div class='v'>" + jsonEscape(g_mb) + "</div></div>";
  s += "<div class='kv'><div class='k'>Display</div><div class='v'>" + String(g_cfg->display_enabled ? "ON" : "HEADLESS") + "</div></div>";
  s += "<div class='kv'><div class='k'>API nødstopp</div><div class='v'>" + String(g_cfg->api_panic_stop ? "ON" : "OFF") + "</div></div>";
  s += "<div class='kv'><div class='k'>Ute / Tilluft</div><div class='v'>" + fOrDash(g_data.uteluft) + " / " + fOrDash(g_data.tilluft) + " C</div></div>";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div class='actions'>";
  s += "<a class='btn secondary' target='_blank' rel='noopener' href='" + statusApiUrl + "'>Vanlig API</a>";
  s += "<a class='btn secondary' target='_blank' rel='noopener' href='" + statusApiPrettyUrl + "'>Pretty API</a>";
  s += "<button class='btn secondary' type='button' onclick='refreshNow()'>Hent ferske data + refresh skjerm</button>";
  s += "</div>";
  s += "<div id='refresh_now_result' class='help'></div>";
  s += "<div class='help'>Lenkene er sikre admin-forhandsvisninger. Ekstern API bruker Bearer-token i Authorization-header.</div>";
  s += "</div>";

  appendQuickControlCard(s, false);

  // WiFi
  s += "<div class='card'><h2>WiFi</h2>";
  s += "<form id='admin_form' method='POST' action='/admin/save'>";
  s += "<label>SSID</label><input name='ssid' value='" + jsonEscape(g_cfg->wifi_ssid) + "'>";
  s += "<label>Passord (la tomt for å beholde)</label><input name='wpass' type='password' value=''>";
  s += "<div class='help'>Hvis du endrer WiFi må enheten restartes etterpå.</div>";
  s += "</div>";

  // API + modules
  s += "<div class='card'><h2>API og integrasjoner</h2>";
  s += "<label>Enhetsmodell</label>";
  s += "<select id='model_admin' name='model' class='input'>";
  s += "<option value='S3'" + String(g_cfg->model == "S3" ? " selected" : "") + ">Nordic S3</option>";
  s += "<option value='S4'" + String(g_cfg->model == "S4" ? " selected" : "") + ">Nordic S4</option>";
  s += "<option value='S2_EXP'" + String(g_cfg->model == "S2_EXP" ? " selected" : "") + ">Nordic S2 (Experimental)</option>";
  s += "<option value='S7_EXP'" + String(g_cfg->model == "S7_EXP" ? " selected" : "") + ">Nordic S7 (Experimental)</option>";
  s += "<option value='CL3_EXP'" + String(g_cfg->model == "CL3_EXP" ? " selected" : "") + ">Nordic CL3 (Experimental)</option>";
  s += "<option value='CL4_EXP'" + String(g_cfg->model == "CL4_EXP" ? " selected" : "") + ">Nordic CL4 (Experimental)</option>";
  s += "</select>";
  s += "<div class='sep-gold'></div>";
  s += "<p class='muted-title'>API Bearer-tokens</p>";
  s += "<details><summary>API Bearer-tokens</summary>";
  s += "<label>API Bearer-token (main/control)</label><input id='api_token_main' class='mono' type='password' name='token' value='" + jsonEscape(g_cfg->api_token) + "' required>";
  s += "<div class='actions'><button class='btn secondary' type='button' onclick='toggleSecret(\"api_token_main\")'>Vis/skjul</button><button class='btn secondary' type='button' onclick='copySecret(\"api_token_main\")'>Kopier</button><button class='btn secondary' type='button' onclick='rotateToken(\"main\",\"api_token_main\")'>Roter main-token</button></div>";
  s += "<label>Homey Bearer-token (/status)</label><input id='api_token_homey' class='mono' type='password' name='homey_token' value='" + jsonEscape(g_cfg->homey_api_token) + "' required>";
  s += "<div class='actions'><button class='btn secondary' type='button' onclick='toggleSecret(\"api_token_homey\")'>Vis/skjul</button><button class='btn secondary' type='button' onclick='copySecret(\"api_token_homey\")'>Kopier</button><button class='btn secondary' type='button' onclick='rotateToken(\"homey\",\"api_token_homey\")'>Roter Homey-token</button></div>";
  s += "<label>Home Assistant Bearer-token (/ha/*)</label><input id='api_token_ha' class='mono' type='password' name='ha_token' value='" + jsonEscape(g_cfg->ha_api_token) + "' required>";
  s += "<div class='actions'><button class='btn secondary' type='button' onclick='toggleSecret(\"api_token_ha\")'>Vis/skjul</button><button class='btn secondary' type='button' onclick='copySecret(\"api_token_ha\")'>Kopier</button><button class='btn secondary' type='button' onclick='rotateToken(\"ha\",\"api_token_ha\")'>Roter HA-token</button></div>";
  s += "</details>";
  s += "<div class='sep-gold'></div>";
  s += "<p class='muted-title'>API nødstopp</p>";
  s += "<div class='help'>Nødstopp blokkerer alle token-beskyttede API-kall umiddelbart (status, historikk, control).</div>";
  s += "<div class='actions'>";
  if (g_cfg->api_panic_stop)
    s += "<form method='POST' action='/admin/api_emergency_stop' style='margin:0'><input type='hidden' name='enable' value='0'><button class='btn secondary' type='submit'>Deaktiver nødstopp</button></form>";
  else
    s += "<form method='POST' action='/admin/api_emergency_stop' style='margin:0'><input type='hidden' name='enable' value='1'><button class='btn danger' type='submit'>Aktiver nødstopp</button></form>";
  s += "</div>";
  s += "<div class='help'>Status: <strong>" + String(g_cfg->api_panic_stop ? "AKTIV" : "INAKTIV") + "</strong></div>";
  s += "<div class='sep-gold'></div>";
  s += "<details><summary>Integrasjoner og moduler</summary>";
  s += "<p class='muted-title'>Integrasjoner</p>";
  s += "<div class='actions'>";
  s += "<a class='btn secondary' href='/admin/export/homey.txt'>Eksporter Homey-oppsett (.txt)</a>";
  s += "<button class='btn secondary' type='button' onclick='emailHomeySetup()'>Send til e-post (mobil)</button>";
  s += "</div>";
  s += "<div class='help'>Mobilknappen åpner e-postklient med oppsetttekst i ny e-post.</div>";
  s += "<div class='sep-gold'></div>";
  s += "<p class='muted-title'>Moduler</p>";
  s += "<label><input type='checkbox' name='homey' " + String(g_cfg->homey_enabled ? "checked" : "") + "> " + tr("homey_api") + "</label>";
  s += "<label><input id='ha_toggle_admin' type='checkbox' name='ha' " + String(g_cfg->ha_enabled ? "checked" : "") + "> " + tr("ha_api") + "</label>";
  s += "<label><input id='hamqtt_admin' type='checkbox' name='hamqtt' " + String(g_cfg->ha_mqtt_enabled ? "checked" : "") + "> " + tr("ha_mqtt") + "</label>";
  s += "<div id='hamqtt_block_admin' style='display:" + String((g_cfg->ha_enabled && g_cfg->ha_mqtt_enabled) ? "block" : "none") + ";'>";
  s += "<div class='help'>" + tr("ha_mqtt_help") + "</div>";
  if (!ha_mqtt_lib_available()) s += "<div class='help' style='color:#b91c1c'>PubSubClient-bibliotek mangler i firmware-build.</div>";
  s += "<div class='row'>";
  s += "<div><label>" + tr("ha_mqtt_host") + "</label><input name='hamhost' value='" + jsonEscape(g_cfg->ha_mqtt_host) + "'></div>";
  s += "<div><label>" + tr("ha_mqtt_port") + "</label><input name='hamport' type='number' min='1' max='65535' value='" + String((int)g_cfg->ha_mqtt_port) + "'></div>";
  s += "</div>";
  s += "<div class='row'>";
  s += "<div><label>" + tr("ha_mqtt_user") + "</label><input name='hamuser' value='" + jsonEscape(g_cfg->ha_mqtt_user) + "'></div>";
  s += "<div><label>" + tr("ha_mqtt_pass") + "</label><input name='hampass' type='password' value='" + jsonEscape(g_cfg->ha_mqtt_pass) + "'></div>";
  s += "</div>";
  s += "<div class='row'>";
  s += "<div><label>" + tr("ha_mqtt_prefix") + "</label><input name='hampfx' value='" + jsonEscape(g_cfg->ha_mqtt_prefix) + "'></div>";
  s += "<div><label>" + tr("ha_mqtt_interval") + "</label><input name='hamint' type='number' min='10' max='3600' value='" + String((int)g_cfg->ha_mqtt_interval_s) + "'></div>";
  s += "</div>";
  s += "<label>" + tr("ha_mqtt_base") + "</label><input name='hambase' value='" + jsonEscape(g_cfg->ha_mqtt_topic_base) + "'>";
  s += "</div>";
  s += "</details>";
  s += "</div>";
  s += "<div class='card'><h2>Inndata-moduler</h2>";
  s += "<div class='help'>Velg aktiv datakilde. Kun innstillinger for valgt kilde vises.</div>";
  s += "<div class='sep-gold'></div>";
  const bool srcWeb = (normDataSource(g_cfg->data_source) == "BACNET");
  s += "<p class='muted-title'>Datakilde</p>";
  s += "<div><strong>" + tr("data_source") + "</strong></div>";
  s += "<label><input type='radio' name='src' value='MODBUS'" + String(!srcWeb ? " checked" : "") + "> " + tr("source_modbus") + "</label>";
  s += "<label><input type='radio' name='src' value='BACNET'" + String(srcWeb ? " checked" : "") + "> " + tr("source_bacnet") + "</label>";
  s += "<div id='mb_block_admin' style='display:" + String(!srcWeb ? "block" : "none") + ";'>";
  s += "<div class='sep-gold-dashed'></div>";
  s += "<p class='muted-title'>Modbus</p>";
  s += "<label><input id='mb_toggle_admin' type='checkbox' name='modbus' " + String(g_cfg->modbus_enabled ? "checked" : "") + "> Modbus</label>";
  s += "<div id='mb_adv_admin' style='display:" + String((g_cfg->modbus_enabled && !srcWeb) ? "block" : "none") + ";'>";
  s += "<div class='help'>Avanserte Modbus-innstillinger</div>";
  s += "<label><input type='checkbox' name='ctrl' " + String(g_cfg->control_enabled ? "checked" : "") + "> " + tr("control_enable") + "</label>";
  s += "<label>Modbus transport</label>";
  s += "<select id='mbtr_admin' name='mbtr' class='input'>";
  s += "<option value='AUTO'" + String(g_cfg->modbus_transport_mode == "AUTO" ? " selected" : "") + ">AUTO (anbefalt for auto-dir modul)</option>";
  s += "<option value='MANUAL'" + String(g_cfg->modbus_transport_mode == "MANUAL" ? " selected" : "") + ">MANUAL (DE/RE styres av GPIO)</option>";
  s += "</select>";
  s += "<div class='row'>";
  s += "<div><label>Modbus baud</label><input id='mbbaud_admin' name='mbbaud' type='number' min='1200' max='115200' value='" + String(g_cfg->modbus_baud) + "'></div>";
  s += "<div><label>Slave ID</label><input id='mbid_admin' name='mbid' type='number' min='1' max='247' value='" + String((int)g_cfg->modbus_slave_id) + "'></div>";
  s += "<div><label>Addr offset</label><input id='mboff_admin' name='mboff' type='number' min='-5' max='5' value='" + String((int)g_cfg->modbus_addr_offset) + "'></div>";
  s += "</div>";
  s += "<label>Serial format</label>";
  s += "<select id='mbser_admin' name='mbser' class='input'>";
  s += "<option value='8N1'" + String(g_cfg->modbus_serial_format == "8N1" ? " selected" : "") + ">8N1</option>";
  s += "<option value='8E1'" + String(g_cfg->modbus_serial_format == "8E1" ? " selected" : "") + ">8E1</option>";
  s += "<option value='8O1'" + String(g_cfg->modbus_serial_format == "8O1" ? " selected" : "") + ">8O1</option>";
  s += "</select>";
  s += "</div>";
  s += "</div>";
  s += "<div id='bac_block_admin' style='display:" + String(srcWeb ? "block" : "none") + ";'>";
  s += "<div class='sep-gold-dashed'></div>";
  s += "<div id='fw_adv_admin' style='display:" + String(srcWeb ? "block" : "none") + ";'>";
  s += "<p class='muted-title'>BACnet</p>";
  s += "<div class='help'>" + tr("source_bacnet_help") + "</div>";
  s += "<div class='help'>" + tr("source_bacnet_default_map_hint") + "</div>";
  s += "<div class='row'>";
  s += "<div><label>" + tr("bac_ip") + "</label><input name='bacip' value='" + jsonEscape(g_cfg->bacnet_ip) + "' required></div>";
  s += "<div><label>" + tr("bac_device_id") + "</label><input name='bacid' type='number' min='1' max='4194303' value='" + String(g_cfg->bacnet_device_id) + "' required></div>";
  s += "</div>";
  s += "<div class='row'>";
  s += "<div><label>" + tr("bac_port") + "</label><input name='bacport' type='number' min='1' max='65535' value='" + String((int)g_cfg->bacnet_port) + "'></div>";
  s += "<div><label>" + tr("bac_poll_min") + "</label><input name='bacpoll' type='number' min='5' max='60' value='" + String((int)g_cfg->bacnet_poll_minutes) + "'></div>";
  s += "<div><label>" + tr("bac_timeout") + "</label><input name='bacto' type='number' min='300' max='8000' value='" + String((int)g_cfg->bacnet_timeout_ms) + "'></div>";
  s += "</div>";
  s += "<label><input type='checkbox' name='bacwr' " + String(g_cfg->bacnet_write_enabled ? "checked" : "") + "> " + tr("bac_write_enable") + "</label>";
  s += "<details style='margin-top:8px'><summary>" + tr("bac_objects") + "</summary>";
  s += "<div class='help'>Format for objekter: <code>ai:1</code>, <code>av:2</code>, <code>msv:1</code></div>";
  s += "<div class='row'><div><label>Uteluft</label><input name='baout' value='" + jsonEscape(g_cfg->bacnet_obj_outdoor) + "'></div><div><label>Tilluft</label><input name='basup' value='" + jsonEscape(g_cfg->bacnet_obj_supply) + "'></div></div>";
  s += "<div class='row'><div><label>Avtrekk</label><input name='baext' value='" + jsonEscape(g_cfg->bacnet_obj_extract) + "'></div><div><label>Avkast</label><input name='baexh' value='" + jsonEscape(g_cfg->bacnet_obj_exhaust) + "'></div></div>";
  s += "<div class='row'><div><label>Fan %</label><input name='bafan' value='" + jsonEscape(g_cfg->bacnet_obj_fan) + "'></div><div><label>Heat %</label><input name='baheat' value='" + jsonEscape(g_cfg->bacnet_obj_heat) + "'></div></div>";
  s += "<div class='row'><div><label>" + tr("bac_setpoint_home_obj") + "</label><input name='bashome' value='" + jsonEscape(g_cfg->bacnet_obj_setpoint_home) + "'></div><div><label>" + tr("bac_setpoint_away_obj") + "</label><input name='basaway' value='" + jsonEscape(g_cfg->bacnet_obj_setpoint_away) + "'></div></div>";
  s += "<div class='row'><div><label>Mode object</label><input name='bamode' value='" + jsonEscape(g_cfg->bacnet_obj_mode) + "'></div><div><label>" + tr("bac_mode_map") + "</label><input name='bamap' value='" + jsonEscape(g_cfg->bacnet_mode_map) + "'></div></div>";
  s += "</details>";
  s += "<div class='actions' style='margin-top:10px'><button class='btn secondary' type='button' onclick='testBACnet(\"admin_form\")'>Test BACnet</button><button class='btn secondary' type='button' onclick='discoverBACnet(\"admin_form\")'>Autodiscover</button><button class='btn secondary' type='button' onclick='probeBACnetObjects(\"admin_form\")'>Object probe</button><button class='btn secondary' type='button' onclick='scanBACnetObjects(\"admin_form\")'>Object scan</button><button class='btn secondary' type='button' onclick='probeBACnetWrite(\"admin_form\")'>Write probe (exp)</button></div>";
  s += "<div id='fw_test_result_admin' class='help'></div>";
  s += "<pre id='probe_values_admin' style='display:none;white-space:pre-wrap;max-height:240px;overflow:auto;border:1px solid #2a3344;border-radius:10px;padding:10px;font-size:12px;line-height:1.35;background:#0b1220;color:#dbeafe;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace'></pre>";
  s += "<div class='actions' style='margin-top:8px'><button class='btn secondary' type='button' onclick='showBACnetDebug(\"admin\")'>Vis debuglogg</button><button class='btn secondary' type='button' onclick='copyBACnetDebug(\"admin\")'>Kopier logg</button><button class='btn secondary' type='button' onclick='clearBACnetDebug(\"admin\")'>Tøm debuglogg</button></div>";
  s += "<pre id='bacnet_debug_admin' data-on='0' style='display:none;white-space:pre-wrap;max-height:220px;overflow:auto;border:1px solid #2a3344;border-radius:10px;padding:10px;font-size:11px;line-height:1.45;background:#0b1220;color:#dbeafe;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace'></pre>";
  s += "</div>";
  s += "</div>";
  s += "<script>(function(){"
       "var t=document.getElementById('mb_toggle_admin');var a=document.getElementById('mb_adv_admin');"
       "var fw=document.getElementById('fw_adv_admin');var mbb=document.getElementById('mb_block_admin');var bcb=document.getElementById('bac_block_admin');"
       "var ha=document.getElementById('ha_toggle_admin');var hm=document.getElementById('hamqtt_admin');var hmb=document.getElementById('hamqtt_block_admin');"
       "var src=document.querySelectorAll('input[name=\"src\"]');"
       "var m=document.getElementById('model_admin');var tr=document.getElementById('mbtr_admin');"
       "var sf=document.getElementById('mbser_admin');var bd=document.getElementById('mbbaud_admin');"
       "var id=document.getElementById('mbid_admin');var of=document.getElementById('mboff_admin');"
       "if(!t||!a)return;"
       "function srcVal(){for(var i=0;i<src.length;i++){if(src[i].checked)return src[i].value;}return 'MODBUS';}"
       "function u(){var useMb=(srcVal()==='MODBUS');if(mbb)mbb.style.display=useMb?'block':'none';if(bcb)bcb.style.display=useMb?'none':'block';a.style.display=(useMb&&t.checked)?'block':'none';if(fw)fw.style.display=useMb?'none':'block';if(hmb)hmb.style.display=(ha&&ha.checked&&hm&&hm.checked)?'block':'none';}"
       "function p(model){tr.value='AUTO';sf.value='8E1';bd.value='9600';id.value='1';of.value='0';}"
       "t.addEventListener('change',u);"
       "if(ha)ha.addEventListener('change',u);if(hm)hm.addEventListener('change',function(){if(hm.checked&&ha){ha.checked=true;}u();});"
       "for(var i=0;i<src.length;i++){src[i].addEventListener('change',u);}"
       "if(m){m.addEventListener('change',function(){if(t.checked){p(m.value);}});}"
       "u();"
       "async function prepBACnetDebug(scope){"
       "try{await fetch('/admin/clear_bacnet_debug',{method:'POST',credentials:'same-origin'});}catch(e){}"
       "try{var en1=new URLSearchParams(); en1.append('enable','1'); await fetch('/admin/bacnet_debug_mode',{method:'POST',credentials:'same-origin',body:en1});}catch(e){}"
       "var dbg=document.getElementById(scope==='setup'?'bacnet_debug_setup':'bacnet_debug_admin');"
       "if(dbg && dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent='(samler ny logg...)';}"
       "}"
       "window.testBACnet=async function(formId){"
       "var form=document.getElementById(formId);if(!form)return;"
       "await prepBACnetDebug('admin');"
       "var target=document.getElementById('fw_test_result_admin')||document.getElementById('fw_test_result_setup');"
       "var dbg=document.getElementById('bacnet_debug_admin')||document.getElementById('bacnet_debug_setup');"
       "if(target){target.style.color='var(--muted)';target.textContent='Tester BACnet...';}"
       "try{"
       "var fd=new FormData(form);"
       "var body=new URLSearchParams();"
       "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
       "var r=await fetch('/admin/test_bacnet',{method:'POST',credentials:'same-origin',body:body});"
       "var j=await r.json();"
       "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'(ingen debug)';}"
       "if(!j.ok){if(target){target.style.color='#b91c1c';target.textContent='BACnet test feilet: '+(j.error||'ukjent feil')+(j.ip?(' | ip='+j.ip):'');}return;}"
       "if(target){target.style.color='#166534';target.textContent='BACnet OK. Modus '+(j.data&&j.data.mode?j.data.mode:'N/A')+', tilluft '+(j.data&&j.data.tilluft!==null?j.data.tilluft:'-')+' C.';}"
       "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='BACnet test feilet: '+e.message;}}"
       "};"
       "window.discoverBACnet=async function(formId){"
       "var form=document.getElementById(formId);if(!form)return;"
       "await prepBACnetDebug('admin');"
       "var target=document.getElementById('fw_test_result_admin')||document.getElementById('fw_test_result_setup');"
       "var dbg=document.getElementById('bacnet_debug_admin')||document.getElementById('bacnet_debug_setup');"
       "if(target){target.style.color='var(--muted)';target.textContent='Soker etter BACnet-enheter...';}"
       "try{"
       "var r=await fetch('/admin/discover_bacnet',{method:'POST',credentials:'same-origin'});"
       "var j=await r.json();"
       "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'(ingen debug)';}"
       "if(!j.ok||!j.items||!j.items.length){if(target){target.style.color='#b91c1c';target.textContent='Ingen BACnet-enheter funnet ('+(j.error||'ukjent')+')';}return;}"
       "var i=j.items[0];"
       "var ip=form.querySelector('input[name=\"bacip\"]'); if(ip&&i.ip) ip.value=i.ip;"
       "var id=form.querySelector('input[name=\"bacid\"]'); if(id&&i.device_id) id.value=i.device_id;"
       "if(target){target.style.color='#166534';target.textContent='Fant '+j.items.length+' enhet(er). Fylte inn første: '+i.ip+' / ID '+i.device_id;}"
       "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Autodiscover feilet: '+e.message;}}"
       "};"
       "window.probeBACnetObjects=async function(formId){"
       "var form=document.getElementById(formId);if(!form)return;"
       "await prepBACnetDebug('admin');"
       "var target=document.getElementById('fw_test_result_admin')||document.getElementById('fw_test_result_setup');"
       "var dbg=document.getElementById('bacnet_debug_admin')||document.getElementById('bacnet_debug_setup');"
       "var listEl=document.getElementById('probe_values_admin')||document.getElementById('probe_values_setup');"
       "if(target){target.style.color='var(--muted)';target.textContent='Prober alle lesbare BACnet-objekter...';}"
       "if(listEl){listEl.style.display='none';listEl.textContent='';}"
       "try{"
       "var fd=new FormData(form);"
       "var body=new URLSearchParams();"
       "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
       "body.append('from_inst','0');"
       "body.append('to_inst','200');"
       "body.append('scan_timeout','450');"
       "body.append('scan_max','220');"
       "var r=await fetch('/admin/probe_bacnet_objects',{method:'POST',credentials:'same-origin',body:body});"
       "var raw=await r.text();"
       "var j={}; try{ j=JSON.parse(raw);}catch(pe){ throw new Error('Ugyldig JSON-respons fra /admin/probe_bacnet_objects: '+raw.slice(0,160)); }"
       "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'';}"
       "var items=Array.isArray(j.items)?j.items:[];"
       "if(!j.ok||!items.length){if(target){target.style.color='#b91c1c';target.textContent='Object probe fant ingen lesbare objekter ('+(j.error||'ukjent')+')';}return;}"
       "var all=[]; for(var i=0;i<items.length;i++){var it=items[i]||{};all.push((it.obj||'?')+'='+(it.value!==undefined?it.value:'?'));}"
       "if(listEl){listEl.style.display='block';listEl.textContent=all.join('\\n');}"
       "if(target){target.style.color='#166534';target.textContent='Object probe OK: fant '+items.length+' lesbare objekter. Full liste vises under.';}"
       "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Object probe feilet: '+e.message;}}"
       "};"
       "window.scanBACnetObjects=async function(formId){"
       "var form=document.getElementById(formId);if(!form)return;"
       "await prepBACnetDebug('admin');"
       "var target=document.getElementById('fw_test_result_admin')||document.getElementById('fw_test_result_setup');"
       "var dbg=document.getElementById('bacnet_debug_admin')||document.getElementById('bacnet_debug_setup');"
       "var from=prompt('Start instance', '0'); if(from===null)return;"
       "var to=prompt('Slutt instance', '64'); if(to===null)return;"
       "var otype=prompt('Objekttype (ai/ao/av/msv eller tom for alle)', 'ai'); if(otype===null)return;"
       "if(target){target.style.color='var(--muted)';target.textContent='Scanner BACnet-objekter...';}"
       "try{"
       "var fd=new FormData(form);"
       "var body=new URLSearchParams();"
       "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
       "body.append('from_inst', from);"
       "body.append('to_inst', to);"
       "body.append('scan_timeout','450');"
       "body.append('scan_max','220');"
       "body.append('otype', otype||'');"
       "var r=await fetch('/admin/scan_bacnet_objects',{method:'POST',credentials:'same-origin',body:body});"
       "var j=await r.json();"
       "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'';}"
       "var items=Array.isArray(j.items)?j.items:[];"
       "if(!j.ok||!items.length){if(target){target.style.color='#b91c1c';target.textContent='Object scan fant ingen lesbare objekter ('+(j.error||'ukjent')+')';}return;}"
       "var shown=[]; for(var i=0;i<items.length&&i<16;i++){var it=items[i]||{};shown.push((it.obj||'?')+'='+(it.value!==undefined?it.value:'?'));}"
       "if(target){target.style.color='#166534';target.textContent='Object scan fant '+items.length+' lesbare objekter. Eksempler: '+shown.join(', ');}"
       "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Object scan feilet: '+e.message;}}"
       "};"
       "window.probeBACnetWrite=async function(formId){"
       "var form=document.getElementById(formId);if(!form)return;"
       "await prepBACnetDebug('admin');"
       "var target=document.getElementById('fw_test_result_admin')||document.getElementById('fw_test_result_setup');"
       "var dbg=document.getElementById('bacnet_debug_admin')||document.getElementById('bacnet_debug_setup');"
       "if(target){target.style.color='var(--muted)';target.textContent='Kjører eksperimentell BACnet write-probe...';}"
       "try{"
       "var fd=new FormData(form);"
       "var body=new URLSearchParams();"
       "fd.forEach(function(v,k){if(typeof v==='string')body.append(k,v);});"
       "var r=await fetch('/admin/probe_bacnet_write',{method:'POST',credentials:'same-origin',body:body});"
       "var j=await r.json();"
       "if(dbg&&dbg.dataset.on==='1'){dbg.style.display='block';dbg.textContent=j.debug||'';}"
       "var rr=j.results||{};"
       "var msg='Mode: '+(rr.mode&&rr.mode.ok?'OK':(rr.mode&&rr.mode.error?rr.mode.error:'-'))+' | Home setpoint: '+(rr.setpoint_home&&rr.setpoint_home.ok?'OK':(rr.setpoint_home&&rr.setpoint_home.error?rr.setpoint_home.error:'-'))+' | Away setpoint: '+(rr.setpoint_away&&rr.setpoint_away.ok?'OK':(rr.setpoint_away&&rr.setpoint_away.error?rr.setpoint_away.error:'-'));"
       "if(target){target.style.color=(j.ok?'#166534':'#b91c1c');target.textContent='Write probe (eksperimentell): '+msg;}"
       "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Write probe feilet: '+e.message;}}"
       "};"
       "window.showBACnetDebug=async function(scope){"
       "var dbg=document.getElementById(scope==='setup'?'bacnet_debug_setup':'bacnet_debug_admin');"
       "if(!dbg)return;"
       "try{"
       "var en1=new URLSearchParams(); en1.append('enable','1');"
       "await fetch('/admin/bacnet_debug_mode',{method:'POST',credentials:'same-origin',body:en1});"
       "var r=await fetch('/admin/bacnet_debug.txt',{credentials:'same-origin'});"
       "var t=await r.text();"
       "dbg.dataset.on='1';dbg.style.display='block';dbg.textContent=t&&t.length?t:'(tom logg)';"
       "}catch(e){dbg.style.display='block';dbg.textContent='Kunne ikke hente logg: '+e.message;}"
       "};"
       "window.clearBACnetDebug=async function(scope){"
       "var dbg=document.getElementById(scope==='setup'?'bacnet_debug_setup':'bacnet_debug_admin');"
       "try{await fetch('/admin/clear_bacnet_debug',{method:'POST',credentials:'same-origin'});}catch(e){}"
       "if(dbg){dbg.style.display='block';dbg.textContent='(tom logg)';}"
       "};"
       "window.copyBACnetDebug=async function(scope){"
       "var target=document.getElementById(scope==='setup'?'fw_test_result_setup':'fw_test_result_admin');"
       "try{"
       "var r=await fetch('/admin/bacnet_debug.txt',{credentials:'same-origin'});"
       "var t=await r.text();"
       "if(!t || !t.length) t='(tom logg)';"
       "if(navigator.clipboard && navigator.clipboard.writeText){await navigator.clipboard.writeText(t);}"
       "else{var ta=document.createElement('textarea');ta.value=t;document.body.appendChild(ta);ta.select();document.execCommand('copy');document.body.removeChild(ta);}"
       "if(target){target.style.color='#166534';target.textContent='Debuglogg kopiert til utklippstavlen.';}"
       "}catch(e){if(target){target.style.color='#b91c1c';target.textContent='Kopiering av logg feilet: '+e.message;}}"
       "};"
       "})();</script>";
  s += "</div>";

  // Display settings
  s += "<div class='card'><h2>Display</h2>";
  s += "<label><input type='checkbox' name='disp' " + String(!g_cfg->display_enabled ? "checked" : "") + "> " + tr("headless") + "</label>";
  s += "<label>" + tr("poll_sec") + "</label><input name='poll' type='number' min='30' max='3600' value='" + String(g_cfg->poll_interval_ms/1000) + "'>";
  s += "<div class='help'>" + tr("poll_help") + "</div>";
  s += "</div>";

  // Homey export helper
  s += "<div class='card'><h2>Homey eksport</h2>";
  s += "<div class='help'>Eksporter ferdig oppsettfil med script, mapping og endpoint-data.</div>";
  s += "<div class='actions'>";
  s += "<a class='btn secondary' href='/admin/export/homey'>Eksporter Homey-oppsett (.json)</a>";
  s += "<a class='btn secondary' href='/admin/export/homey.txt'>Eksporter Homey-oppsett (.txt)</a>";
  s += "<button class='btn secondary' type='button' onclick='emailHomeySetup()'>Send til e-post (mobil)</button>";
  s += "</div>";
  s += "<div class='help'>Mobilknappen åpner e-postklient med oppsetttekst i ny e-post.</div>";
  s += "</div>";

  // Admin password
  s += "<div class='card'><h2>Sikkerhet</h2>";
  s += "<label>Nytt admin-passord</label><input name='np1' type='password'>";
  s += "<label>Gjenta nytt passord</label><input name='np2' type='password'>";
  s += "<div class='help'>Min 8 tegn. La tomt for å ikke endre.</div>";
  s += "</div>";

  // Actions
  s += "<div class='card'><h2>Lagre / handlinger</h2>";
  s += "<p class='muted-title'>Lagre</p>";
  s += "<div class='actions' style='margin-top:8px'>";
  s += "<button class='btn' type='submit'>Lagre</button>";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<p class='muted-title'>Navigasjon</p>";
  s += "<div class='action-grid'>";
  s += "<a class='btn secondary' href='/admin/manual'>" + tr("manual") + "</a>";
  s += "<a class='btn secondary' href='/admin/graphs'>" + tr("graphs") + "</a>";
  s += "<a class='btn secondary' href='/admin/ota'>" + tr("ota") + "</a>";
  s += "<a class='btn secondary' href='/'>" + tr("back_home") + "</a>";
  s += "</div>";
  s += "<div class='help'>Tips: bruk eksportknappen over for HomeyScript og mapping i stedet for manuell kopiering.</div>";
  s += "</form>";
  s += "<div class='sep-gold'></div>";
  s += "<p class='muted-title'>System</p>";
  s += "<div class='actions'>";
  s += "<form method='POST' action='/admin/reboot' style='margin:0'><button class='btn secondary' type='submit'>Restart</button></form>";
  s += "<form method='POST' action='/admin/factory_reset' style='margin:0' onsubmit='return confirm(\"Fabrikkreset? Dette sletter alt.\");'>"
       "<button class='btn danger' type='submit'>Fabrikkreset</button></form>";
  s += "</div>";
  s += "<div class='help'>Fabrikkreset kan også trigges ved å holde BOOT (GPIO0) ~6s ved oppstart.</div>";
  s += "</div>";

  s += "<script>(function(){"
       "window.rotateToken=async function(kind,inputId){"
       "var inp=document.getElementById(inputId);"
       "if(!inp) return;"
       "try{"
       "const body=new URLSearchParams(); body.append('kind',kind);"
       "const r=await fetch('/admin/new_token',{method:'POST',credentials:'same-origin',body:body});"
       "const j=await r.json();"
       "if(!j.ok||!j.token) throw new Error(j.error||('HTTP '+r.status));"
       "inp.value=j.token;"
       "}catch(e){alert('Kunne ikke rotere token: '+e.message);}"
       "};"
       "window.emailHomeySetup=async function(){"
       "try{"
       "const r=await fetch('/admin/export/homey.txt',{credentials:'same-origin'});"
       "if(!r.ok) throw new Error('HTTP '+r.status);"
       "const txt=await r.text();"
       "const subject='VentReader Homey setup';"
       "window.location.href='mailto:?subject='+encodeURIComponent(subject)+'&body='+encodeURIComponent(txt);"
       "}catch(e){alert('Homey eksport feilet: '+e.message);}"
       "};"
       "window.refreshNow=async function(){"
       "var out=document.getElementById('refresh_now_result');"
       "if(out){out.style.color='var(--muted)';out.textContent='Henter ferske data...';}"
       "try{"
       "const r=await fetch('/admin/refresh_now',{method:'POST',credentials:'same-origin'});"
       "if(!r.ok) throw new Error('HTTP '+r.status);"
       "if(out){out.style.color='#166534';out.textContent='Oppdatering trigget. Data/visning oppdateres straks.';}"
       "setTimeout(function(){window.location.reload();},1200);"
       "}catch(e){if(out){out.style.color='#b91c1c';out.textContent='Oppdatering feilet: '+e.message;}}"
       "};"
       "})();</script>";

  s += "</div>"; // grid
  s += pageFooter();
  server.send(200, "text/html", s);
}

static void handleAdminManual()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed)
  {
    redirectTo("/admin/setup?step=1");
    return;
  }

  const bool noLang = (lang() == "no");
  String s = pageHeader(tr("manual"), tr("manual_subtitle"));
  s += "<div class='grid'>";

  s += "<div class='card'><h2>" + tr("changelog_short") + "</h2>";
  s += "<div><strong>v4.2.4</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Bugfix onboarding/admin: BACnet-konfigurasjon kan n&aring; lagres uten tvungen test OK. Test/verifisering kj&oslash;res separat via BACnet test-knapp.";
  else
    s += "Bugfix onboarding/admin: BACnet configuration can now be saved without mandatory test OK. Verification is done separately via the BACnet test button.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.2.3</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "API bruker n&aring; Authorization: Bearer-token (ikke token i URL). Ny API nødstopp-knapp i admin kan blokkere alle token-beskyttede API-kall umiddelbart.";
  else
    s += "API now uses Authorization Bearer tokens (no token in URL). New API emergency-stop button in admin can block all token-protected API calls immediately.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.2.2</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Intern refaktorering: felles kontrolllogikk/hurtigstyring, mindre duplisering og lettere videre vedlikehold.";
  else
    s += "Internal refactor: shared control/quick-control logic, less duplication and easier long-term maintenance.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.2.1</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Admin: nytt Display-segment (headless + oppdateringsintervall), hurtigstyring flyttet rett under status, og førstegangsoppsett kjører nå uten auth-prompt.";
  else
    s += "Admin: new Display section (headless + refresh interval), quick control moved directly below status, and first-time setup now runs without auth prompt.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.2.0</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Eksperimentell BACnet-skriving: modus og settpunkt kan n&aring; skrives via aktiv datakilde, inkl. API/hurtigstyring.";
  else
    s += "Experimental BACnet writes: mode and setpoint can now be written via active datasource, including API/quick control.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.1.0</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Ny headless-modus i setup/admin. Enheten kan nå kjøres uten fysisk skjerm og admin har ett-klikk Vanlig/Pretty API-lenker med token.";
  else
    s += "New headless mode in setup/admin. Device can now run without physical display and admin includes one-click regular/pretty API links with token.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.0.4</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Ny native Home Assistant MQTT Discovery-integrasjon med auto entities i HA (uten custom komponent).";
  else
    s += "New native Home Assistant MQTT Discovery integration with auto-created HA entities (no custom component).";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.0.3</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Inndata-moduler er flyttet til egen seksjon. Modbus/BACnet-innstillinger vises kun for aktivt valgt datakilde.";
  else
    s += "Input modules were moved to a dedicated section. Modbus/BACnet settings now show only for the active data source.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.0.2</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "/status returnerer n&aring; komplett datasett uansett aktiv datakilde (MODBUS/BACNET), og pretty=1 viser field_map med lesbar mapping.";
  else
    s += "/status now returns complete data independent of active datasource (MODBUS/BACNET), and pretty=1 shows field_map with readable mapping.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.0.1</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Klokken i toppfelt viser siste skjermrefresh, mens 'siste' under hvert kort viser siste vellykkede dataoppdatering fra datakilde.";
  else
    s += "Header clock now shows last screen refresh, while each card 'updated' time shows last successful datasource update.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v4.0.0</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Ny datakilde: BACnet (lokal, kun lesing) med egen polling (5-60 min), wizard/admin-oppsett, test og autodiscover.";
  else
    s += "New data source: BACnet (local, read-only) with dedicated polling (5-60 min), wizard/admin setup, testing and autodiscovery.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v3.7.0</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Setup-wizard krever n&aring; eksplisitt valg (aktiver/deaktiver) for Homey/API og Home Assistant/API.";
  else
    s += "Setup wizard now requires an explicit enable/disable choice for Homey/API and Home Assistant/API.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>v3.1.0</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Språkvalg i admin/setup, Home Assistant-endepunkt, eksperimentell styring, ekstra modellvalg og dokumentasjon.";
  else
    s += "Language selector in admin/setup, Home Assistant endpoint, experimental control, extra model options and docs.";
  s += "</div>";
  s += "</div>";

  s += "<div class='card'><h2>" + tr("manual_simple") + "</h2>";
  s += "<div><strong>1) " + String(noLang ? "Forstegangsoppsett" : "First setup") + "</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Logg inn p&aring; <code>/admin</code> med fabrikkpassord, fullf&oslash;r wizard og restart enheten.";
  else
    s += "Log in at <code>/admin</code> with factory password, complete wizard, then restart the device.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>2) " + String(noLang ? "Daglig bruk" : "Daily use") + "</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Skjermen viser live verdier. API-status leses fra <code>/status</code> (eller <code>/ha/status</code>) med <code>Authorization: Bearer ...</code>.";
  else
    s += "Display shows live values. Read API status from <code>/status</code> (or <code>/ha/status</code>) with <code>Authorization: Bearer ...</code>.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>3) " + String(noLang ? "Datakilde" : "Data source") + "</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Velg enten <code>Modbus (lokal)</code> eller <code>BACnet (lokal)</code>. BACnet krever lokal IP, Device ID og objektmapping.";
  else
    s += "Choose either <code>Modbus (local)</code> or <code>BACnet (local)</code>. BACnet requires local IP, Device ID and object mapping.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>4) Modbus</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Modbus er AV som standard. N&aring;r Modbus aktiveres, vises avanserte innstillinger automatisk.";
  else
    s += "Modbus is OFF by default. When enabled, advanced settings appear automatically.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>5) Homey / Home Assistant</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Aktiver Homey/API eller Home Assistant/API i admin. Bruk token-beskyttet API lokalt.";
  else
    s += "Enable Homey/API or Home Assistant/API in admin. Use token-protected local API.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>6) " + String(noLang ? "Fjernstyring (eksperimentell)" : "Remote control (experimental)") + "</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Aktiv n&aring;r skrivekontroll er aktivert for valgt datakilde: <code>Modbus</code> eller <code>BACnet</code>.";
  else
    s += "Active when write control is enabled for selected data source: <code>Modbus</code> or <code>BACnet</code>.";
  s += " API: <code>POST /api/control/mode</code>, <code>POST /api/control/setpoint</code>.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>7) " + String(noLang ? "OTA-oppdatering" : "OTA update") + "</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "G&aring; til <code>/admin/ota</code>, last opp firmwarefil (.bin), enheten restarter automatisk.";
  else
    s += "Go to <code>/admin/ota</code>, upload firmware (.bin), device restarts automatically.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>8) " + String(noLang ? "Feilsoking" : "Troubleshooting") + "</strong></div>";
  s += "<div class='help'>";
  if (noLang)
    s += "Sjekk <code>/health</code> og <code>/status?pretty=1</code> med Bearer-header. Ved Modbus-feil brukes siste gyldige data.";
  else
    s += "Check <code>/health</code> and <code>/status?pretty=1</code> with Bearer header. On Modbus errors, last good data is used.";
  s += "</div>";
  s += "<div class='sep-gold'></div>";
  s += "<div><strong>" + String(noLang ? "Del manual" : "Share manual") + "</strong></div>";
  s += "<div class='help'>" + String(noLang ? "Du kan sende manualteksten til egen e-post eller laste den ned som tekstfil." : "You can send the manual text to your own email or download it as a text file.") + "</div>";
  s += "<div class='actions'>";
  s += "<a class='btn secondary' href='/admin/manual.txt'>" + String(noLang ? "Last ned manual (.txt)" : "Download manual (.txt)") + "</a>";
  s += "<button class='btn secondary' type='button' onclick='emailManualText()'>" + String(noLang ? "Send til e-post" : "Send via email") + "</button>";
  s += "</div>";
  s += "<div class='actions' style='margin-top:16px'><a class='btn' href='/admin'>" + tr("to_admin_page") + "</a></div>";
  s += "</div>";

  s += "<script>(function(){"
       "window.emailManualText=async function(){"
       "try{"
       "const r=await fetch('/admin/manual.txt',{credentials:'same-origin'});"
       "if(!r.ok) throw new Error('HTTP '+r.status);"
       "const txt=await r.text();"
       "const subject='VentReader manual';"
       "const body=encodeURIComponent(txt);"
       "if(body.length<14000){window.location.href='mailto:?subject='+encodeURIComponent(subject)+'&body='+body;return;}"
       "const blob=new Blob([txt],{type:'text/plain;charset=utf-8'});"
       "const url=URL.createObjectURL(blob);"
       "const a=document.createElement('a');a.href=url;a.download='ventreader_manual.txt';document.body.appendChild(a);a.click();"
       "setTimeout(function(){URL.revokeObjectURL(url);a.remove();},800);"
       "alert('" + String(noLang ? "Manualen er lang; tekstfil ble lastet ned for videresending på e-post." : "Manual is long; a text file was downloaded for email forwarding.") + "');"
       "}catch(e){alert('Manual export failed: '+e.message);}"
       "};"
       "})();</script>";

  s += "</div>";
  s += pageFooter();
  server.send(200, "text/html", s);
}

static void handleAdminGraphs()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed)
  {
    redirectTo("/admin/setup?step=1");
    return;
  }

  String s = pageHeader(tr("graphs"), tr("history_graphs_subtitle"));
  s += "<style>"
       ".graph-card{background:linear-gradient(180deg,#141922,#10141c);border:1px solid #2a3446;border-radius:16px;padding:16px;box-shadow:inset 0 1px 0 rgba(255,255,255,.03)}"
       ".graph-toolbar{display:flex;flex-wrap:wrap;gap:10px;align-items:end;margin-bottom:10px}"
       ".range-wrap{display:flex;gap:6px;flex-wrap:wrap}"
       ".range-btn{border:1px solid #2d3850;background:#101825;color:#9fb0cb;border-radius:999px;padding:6px 12px;font-size:12px;font-weight:700;cursor:pointer}"
       ".range-btn.active{border-color:#c2a17e;color:#f4debf;background:rgba(194,161,126,.14)}"
       ".legend{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0 12px}"
       ".legend-chip{display:flex;align-items:center;gap:7px;border:1px solid #2d3850;background:#0f1622;color:#cdd7ea;border-radius:999px;padding:6px 10px;font-size:12px;font-weight:700;cursor:pointer}"
       ".legend-chip.off{opacity:.45}"
       ".dot{width:8px;height:8px;border-radius:50%}"
       ".chart-wrap{position:relative}"
       ".chart-canvas{width:100%;display:block;border:1px solid #2a3446;border-radius:14px;background:linear-gradient(180deg,#0e1420,#0c1119)}"
       ".chart-title{font-weight:700;color:#d8e2f6;font-size:13px;margin:0 0 8px}"
       ".stat-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}"
       ".stat-pill{border:1px solid #2a3446;background:#0f1622;border-radius:12px;padding:10px}"
       ".stat-pill .k{font-size:11px;color:#8da0bf;text-transform:uppercase;letter-spacing:.08em}"
       ".stat-pill .v{font-size:16px;color:#e5ecf8;font-weight:700;margin-top:4px}"
       "@media (max-width:720px){.stat-grid{grid-template-columns:1fr}}"
       "</style>";

  s += "<div class='grid'>";
  s += "<div class='card graph-card'><h2>" + tr("history_graphs_title") + "</h2>";
  s += "<div class='graph-toolbar'>";
  s += "<div><label>" + tr("history_limit") + "</label><input id='limit' type='number' min='20' max='720' value='240'></div>";
  s += "<div class='range-wrap'>"
       "<button class='range-btn active' data-range='2h'>2h</button>"
       "<button class='range-btn' data-range='6h'>6h</button>"
       "<button class='range-btn' data-range='24h'>24h</button>"
       "<button class='range-btn' data-range='all'>All</button>"
       "</div>";
  s += "<div style='display:flex;align-items:end;gap:8px'>"
       "<button class='btn secondary' type='button' onclick='loadHist()'>" + tr("refresh") + "</button>"
       "<button class='btn secondary' type='button' onclick='downloadCsv()'>" + tr("export_csv") + "</button>"
       "</div>";
  s += "</div>";
  s += "<div class='help' id='state'>" + tr("loading") + "</div>";
  s += "<div class='legend' id='legend'></div>";
  s += "<p class='chart-title'>" + tr("temp_graph") + "</p>";
  s += "<div class='chart-wrap'><canvas id='temp' class='chart-canvas' width='940' height='280'></canvas></div>";
  s += "<div class='sep-gold'></div>";
  s += "<p class='chart-title'>" + tr("perf_graph") + "</p>";
  s += "<div class='chart-wrap'><canvas id='perf' class='chart-canvas' width='940' height='240'></canvas></div>";
  s += "</div>";

  s += "<div class='card graph-card'><h2>" + tr("storage_status") + "</h2>";
  s += "<div class='stat-grid'>";
  s += "<div class='stat-pill'><div class='k'>" + tr("history_points") + "</div><div id='st_points' class='v'>-</div></div>";
  s += "<div class='stat-pill'><div class='k'>" + tr("history_mem") + "</div><div id='st_mem' class='v'>-</div></div>";
  s += "<div class='stat-pill'><div class='k'>" + tr("heap_free") + "</div><div id='st_heap' class='v'>-</div></div>";
  s += "<div class='stat-pill'><div class='k'>" + tr("heap_min") + "</div><div id='st_heap_min' class='v'>-</div></div>";
  s += "</div>";
  s += "<div class='help' style='margin-top:10px'>" + tr("history_ram_only") + "</div>";
  s += "<div class='actions'><a class='btn' href='/admin'>" + tr("to_admin_page") + "</a></div>";
  s += "</div></div>";

  const String token = jsonEscape(g_cfg->homey_api_token);
  const String loadingTxt = jsonEscape(tr("loading"));
  s += "<script>";
  s += "const API_TOKEN='" + token + "';";
  s += "const API_HEADERS={Authorization:'Bearer '+API_TOKEN};";
  s += "const TXT_LOADING='" + loadingTxt + "';";
  s += R"JS(
const SERIES=[
  {k:'uteluft',c:'#38bdf8',g:'temp',n:'UTELUFT',on:true},
  {k:'tilluft',c:'#34d399',g:'temp',n:'TILLUFT',on:true},
  {k:'avtrekk',c:'#f59e0b',g:'temp',n:'AVTREKK',on:true},
  {k:'avkast',c:'#a78bfa',g:'temp',n:'AVKAST',on:true},
  {k:'fan',c:'#60a5fa',g:'perf',n:'FAN %',on:true},
  {k:'heat',c:'#ef4444',g:'perf',n:'HEAT %',on:true},
  {k:'efficiency',c:'#22c55e',g:'perf',n:'EFF %',on:true}
];
let LAST=[];
let RANGE='24h';
let HOVER={temp:-1,perf:-1};

function tsMs(it,idx){ if(it&&it.ts_epoch_ms) return Number(it.ts_epoch_ms); if(it&&it.ts_iso){const d=Date.parse(it.ts_iso); if(Number.isFinite(d)) return d;} return idx*60000; }
function num(v){ const n=Number(v); return Number.isFinite(n)?n:null; }
function fmt(v){ return (v===null||!Number.isFinite(v))?'--':(Math.round(v*10)/10).toFixed(1); }
function fmtBytes(v){ if(!Number.isFinite(v)) return '-'; if(v>1048576) return (v/1048576).toFixed(2)+' MB'; if(v>1024) return (v/1024).toFixed(1)+' KB'; return v+' B';}
function timeFmt(it,idx){ const t=tsMs(it,idx); const d=new Date(t); return d.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'}); }

function filteredItems(){
  if(!LAST.length) return [];
  if(RANGE==='all') return LAST;
  const dur=RANGE==='2h'?2*3600e3:RANGE==='6h'?6*3600e3:24*3600e3;
  const now=tsMs(LAST[LAST.length-1],LAST.length-1);
  return LAST.filter((it,i)=> now-tsMs(it,i)<=dur);
}

function drawChart(canvasId, group){
  const c=document.getElementById(canvasId), ctx=c.getContext('2d');
  const w=c.width, h=c.height, p={l:56,r:18,t:16,b:28};
  const iw=w-p.l-p.r, ih=h-p.t-p.b;
  ctx.clearRect(0,0,w,h);
  const bg=ctx.createLinearGradient(0,0,0,h); bg.addColorStop(0,'#0f1727'); bg.addColorStop(1,'#0b1220');
  ctx.fillStyle=bg; ctx.fillRect(0,0,w,h);
  const items=filteredItems();
  const active=SERIES.filter(s=>s.g===group&&s.on);
  if(!items.length||!active.length){ ctx.fillStyle='#8aa0c5'; ctx.font='12px sans-serif'; ctx.fillText('No data', p.l, p.t+14); return; }

  let mn=Infinity,mx=-Infinity;
  for(const it of items){
    for(const s of active){
      const v=num(it[s.k]); if(v===null) continue;
      if(v<mn) mn=v; if(v>mx) mx=v;
    }
  }
  if(!Number.isFinite(mn)||!Number.isFinite(mx)){ mn=0; mx=1; }
  if(mx-mn<1){ mx=mn+1; mn=mn-0.5; }

  const yFor=v=>p.t + (1-((v-mn)/(mx-mn)))*ih;
  const xFor=i=>p.l + (items.length<=1?0:(iw*i/(items.length-1)));

  ctx.strokeStyle='rgba(111,138,180,.20)'; ctx.lineWidth=1;
  for(let i=0;i<5;i++){ const y=p.t + ih*(i/4); ctx.beginPath(); ctx.moveTo(p.l,y); ctx.lineTo(w-p.r,y); ctx.stroke(); }
  for(let i=0;i<6;i++){ const x=p.l + iw*(i/5); ctx.beginPath(); ctx.moveTo(x,p.t); ctx.lineTo(x,h-p.b); ctx.stroke(); }

  ctx.fillStyle='#91a5c8'; ctx.font='11px sans-serif';
  ctx.fillText(mx.toFixed(1),6,p.t+4);
  ctx.fillText(mn.toFixed(1),6,h-p.b+4);

  for(const s of active){
    let first=true;
    ctx.beginPath();
    for(let i=0;i<items.length;i++){
      const v=num(items[i][s.k]); if(v===null) continue;
      const x=xFor(i), y=yFor(v);
      if(first){ ctx.moveTo(x,y); first=false; } else ctx.lineTo(x,y);
    }
    ctx.strokeStyle=s.c; ctx.lineWidth=2.2; ctx.shadowBlur=8; ctx.shadowColor=s.c+'66'; ctx.stroke(); ctx.shadowBlur=0;
  }

  const hi=Math.max(0,Math.min(items.length-1,HOVER[group]||0));
  if(items.length && HOVER[group]>=0){
    const x=xFor(hi);
    ctx.strokeStyle='rgba(245,222,191,.45)'; ctx.lineWidth=1;
    ctx.beginPath(); ctx.moveTo(x,p.t); ctx.lineTo(x,h-p.b); ctx.stroke();
    const lines=[timeFmt(items[hi],hi)];
    for(const s of active){ lines.push(`${s.n}: ${fmt(num(items[hi][s.k]))}`); }
    ctx.font='11px sans-serif';
    let tw=0; for(const l of lines) tw=Math.max(tw,ctx.measureText(l).width);
    const boxW=tw+16, boxH=lines.length*14+10;
    let bx=x+10; if(bx+boxW>w-4) bx=x-boxW-10;
    let by=p.t+6; if(by+boxH>h-p.b) by=h-p.b-boxH;
    ctx.fillStyle='rgba(8,14,24,.92)'; ctx.strokeStyle='rgba(245,222,191,.45)';
    ctx.fillRect(bx,by,boxW,boxH); ctx.strokeRect(bx,by,boxW,boxH);
    ctx.fillStyle='#e7eefb';
    for(let i=0;i<lines.length;i++) ctx.fillText(lines[i], bx+8, by+16+i*14);
  }
}

function renderLegend(){
  const host=document.getElementById('legend'); host.innerHTML='';
  for(const s of SERIES){
    const b=document.createElement('button');
    b.className='legend-chip'+(s.on?'':' off'); b.type='button';
    b.innerHTML=`<span class="dot" style="background:${s.c}"></span>${s.n}`;
    b.onclick=()=>{ s.on=!s.on; renderLegend(); renderAll(); };
    host.appendChild(b);
  }
}

function renderAll(){ drawChart('temp','temp'); drawChart('perf','perf'); }

function attachHover(id,key){
  const c=document.getElementById(id);
  c.addEventListener('mousemove',(e)=>{
    const items=filteredItems(); if(!items.length){ HOVER[key]=-1; renderAll(); return; }
    const r=c.getBoundingClientRect(); const x=e.clientX-r.left;
    const pL=56, pR=18; const iw=c.width-pL-pR;
    const ratio=Math.max(0,Math.min(1,(x-pL)/iw));
    HOVER[key]=Math.round(ratio*(items.length-1)); renderAll();
  });
  c.addEventListener('mouseleave',()=>{ HOVER[key]=-1; renderAll(); });
}

async function loadStorage(){
  try{
    const r=await fetch('/status/storage',{headers:API_HEADERS}); if(!r.ok) throw new Error('HTTP '+r.status);
    const j=await r.json();
    document.getElementById('st_points').textContent=(j.history_count||0)+' / '+(j.history_cap||0);
    document.getElementById('st_mem').textContent=fmtBytes(j.history_memory_bytes||0);
    document.getElementById('st_heap').textContent=fmtBytes(j.free_heap_bytes||0);
    document.getElementById('st_heap_min').textContent=fmtBytes(j.min_free_heap_bytes||0);
  }catch(e){ document.getElementById('st_points').textContent='ERR'; }
}

async function downloadCsv(){
  const lim=document.getElementById('limit').value||240;
  try{
    const r=await fetch('/status/history.csv?limit='+encodeURIComponent(lim),{headers:API_HEADERS});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const txt=await r.text();
    const blob=new Blob([txt],{type:'text/csv'}); const url=URL.createObjectURL(blob);
    const a=document.createElement('a'); a.href=url; a.download='ventreader_history.csv'; document.body.appendChild(a); a.click();
    setTimeout(()=>{URL.revokeObjectURL(url);a.remove();},800);
  }catch(e){ alert('CSV feilet: '+e.message); }
}

async function loadHist(){
  const state=document.getElementById('state');
  const lim=document.getElementById('limit').value||240;
  state.textContent=TXT_LOADING;
  try{
    const r=await fetch('/status/history?limit='+encodeURIComponent(lim),{headers:API_HEADERS});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const j=await r.json(); LAST=j.items||[];
    renderAll();
    state.textContent='OK: '+LAST.length+' pts | range '+RANGE;
  }catch(e){ state.textContent='ERR: '+e.message; }
}

document.querySelectorAll('.range-btn').forEach(btn=>{
  btn.addEventListener('click',()=>{
    RANGE=btn.dataset.range;
    document.querySelectorAll('.range-btn').forEach(b=>b.classList.remove('active'));
    btn.classList.add('active');
    renderAll();
  });
});

attachHover('temp','temp');
attachHover('perf','perf');
renderLegend();
loadStorage();
loadHist();
setInterval(loadStorage, 30000);
)JS";
  s += "</script>";
  s += pageFooter();
  server.send(200, "text/html", s);
}

static void handleAdminSave()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->admin_pass_changed) { redirectTo("/admin/setup?step=1"); return; }

  // WiFi
  g_cfg->wifi_ssid = server.arg("ssid");
  String newWifiPass = server.arg("wpass");
  if (newWifiPass.length() > 0) g_cfg->wifi_pass = newWifiPass;

  // Token
  g_cfg->api_token = server.arg("token");
  g_cfg->homey_api_token = server.arg("homey_token");
  g_cfg->ha_api_token = server.arg("ha_token");
  if (g_cfg->homey_api_token.length() < 16) g_cfg->homey_api_token = g_cfg->api_token;
  if (g_cfg->ha_api_token.length() < 16) g_cfg->ha_api_token = g_cfg->api_token;

  // Model
  bool modelChanged = false;
  if (server.hasArg("model"))
  {
    String nm = normModel(server.arg("model"));
    modelChanged = (nm != g_cfg->model);
    g_cfg->model = nm;
  }
  if (modelChanged) config_apply_model_modbus_defaults(*g_cfg, true);

  // Modules
  g_cfg->modbus_enabled = server.hasArg("modbus");
  g_cfg->homey_enabled  = server.hasArg("homey");
  g_cfg->ha_enabled     = server.hasArg("ha");
  g_cfg->display_enabled = !server.hasArg("disp");
  g_cfg->data_source = normDataSource(server.arg("src"));
  g_cfg->control_enabled = (g_cfg->data_source == "MODBUS") ? server.hasArg("ctrl") : false;
  if (server.hasArg("lang")) g_cfg->ui_language = normLang(server.arg("lang"));
  applyPostedModbusSettings();
  applyPostedBACnetSettings();
  applyPostedHAMqttSettings();
  if (g_cfg->ha_mqtt_enabled) g_cfg->ha_enabled = true;
  if (!g_cfg->ha_enabled) g_cfg->ha_mqtt_enabled = false;
  if (g_cfg->ha_mqtt_enabled && !ha_mqtt_lib_available())
  {
    server.send(400, "text/plain", "HA MQTT krever PubSubClient-biblioteket.");
    return;
  }
  if (g_cfg->ha_mqtt_enabled && g_cfg->ha_mqtt_host.length() == 0)
  {
    server.send(400, "text/plain", "HA MQTT host/IP må fylles ut.");
    return;
  }
  if (g_cfg->data_source == "BACNET")
  {
    g_mb = "BACNET configured (not verified)";
  }

  // Poll interval
  uint32_t pollSec = (uint32_t) server.arg("poll").toInt();
  if (pollSec < 30) pollSec = 30;
  if (g_cfg->display_enabled && pollSec < 180) pollSec = 180;
  if (pollSec > 3600) pollSec = 3600;
  g_cfg->poll_interval_ms = pollSec * 1000UL;

  // Admin pass change (optional)
  String np1 = server.arg("np1");
  String np2 = server.arg("np2");
  if (np1.length() > 0 || np2.length() > 0)
  {
    if (np1.length() < 8 || np1 != np2)
    {
      server.send(400, "text/plain", "Admin pass: må matche og være minst 8 tegn.");
      return;
    }
    g_cfg->admin_pass = np1;
    g_cfg->admin_pass_changed = true;
    config_save(*g_cfg);
    g_refresh_requested = true;
    String s = pageHeader(tr("admin"), tr("saved"));
    s += "<div class='card'><h2>" + tr("saved") + "</h2>";
    s += "<p>" + tr("password_updated") + "</p>";
    s += "<div class='actions'><a class='btn' href='/admin'>" + tr("to_admin") + "</a></div>";
    s += "</div>";
    s += pageFooter();
    server.send(200, "text/html", s);
    return;
  }

  config_save(*g_cfg);
  g_refresh_requested = true;
  String s = pageHeader(tr("admin"), tr("saved"));
  s += "<div class='card'><h2>" + tr("saved") + "</h2>";
  s += "<p>" + tr("settings_saved_restart_if_needed") + "</p>";
  s += "<div class='actions'><a class='btn' href='/admin'>" + tr("to_admin") + "</a></div>";
  s += "</div>";
  s += pageFooter();
  server.send(200, "text/html", s);
}

static void handleReboot()
{
  if (!checkAdminAuth()) return;
  String s = pageHeader(tr("admin"), tr("restart"));
  s += "<div class='card'><h2>" + tr("restart") + "</h2><p>" + tr("restarting_now") + "</p>";
  s += "<div class='help'>Vent 10-20 sekunder, og g&aring; deretter tilbake til admin.</div>";
  s += "<div class='actions'><a class='btn' href='/admin'>Tilbake til admin</a></div>";
  s += "</div>";
  s += "<script>setTimeout(function(){window.location.href='/admin';},12000);</script>";
  s += pageFooter();
  server.send(200, "text/html", s);
  delay(300);
  ESP.restart();
}

static void handleFactoryReset()
{
  if (!checkAdminAuth()) return;
  String s = pageHeader(tr("admin"), tr("factory_reset"));
  s += "<div class='card'><h2>" + tr("factory_reset") + "</h2><p>" + tr("factory_reset_now") + "</p></div>";
  s += pageFooter();
  server.send(200, "text/html", s);
  delay(300);

  config_factory_reset(); // wipes NVS keys
  delay(200);
  ESP.restart();
}

static void handleRefreshNow()
{
  if (!checkAdminAuth()) return;
  g_refresh_requested = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleAdminOta()
{
  if (!checkAdminAuth()) return;

  String s = pageHeader(tr("ota"));
  s += "<div class='card'><h2>" + tr("ota_upload_title") + "</h2>";
  s += "<p>" + tr("ota_upload_desc") + "</p>";
  s += "<form method='POST' action='/admin/ota_upload' enctype='multipart/form-data'>";
  s += "<label>" + tr("firmware_file") + "</label>";
  s += "<input type='file' name='firmware' accept='.bin,application/octet-stream' required>";
  s += "<div class='actions'><button class='btn' type='submit'>" + tr("start_update") + "</button></div>";
  s += "</form>";
  s += "<div class='help'>" + tr("ota_restart_done") + "</div>";
  s += "</div>";
  s += "<div class='card'><h2>" + tr("ota_alt_title") + "</h2>";
  s += "<p>" + tr("ota_alt_desc") + "</p>";
  s += "</div>";
  s += "<div class='card'><a class='btn' href='/admin'>" + tr("back") + "</a></div>";
  s += pageFooter();
  server.send(200, "text/html", s);
}

static void handleAdminOtaUploadDone()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed)
  {
    server.send(403, "text/plain", "setup not completed");
    return;
  }

  if (Update.hasError())
  {
    char b[96];
    snprintf(b, sizeof(b), "OTA failed. Update error code: %u", Update.getError());
    String s = pageHeader(tr("admin"), tr("ota_failed"));
    s += "<div class='card'><h2>" + tr("ota_failed") + "</h2><p>";
    s += String(b);
    s += "</p><div class='actions'><a class='btn' href='/admin/ota'>" + tr("back_to_ota") + "</a></div></div>";
    s += pageFooter();
    server.send(500, "text/html", s);
    return;
  }

  String s = pageHeader(tr("admin"), tr("ota"));
  s += "<div class='card'><h2>" + tr("ota_ok") + "</h2><p>" + tr("ota_ok_restart") + "</p></div>";
  s += pageFooter();
  server.send(200, "text/html", s);
  delay(300);
  ESP.restart();
}

static void handleAdminOtaUploadStream()
{
  if (!checkAdminAuth()) return;
  if (!g_cfg->setup_completed) return;

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    if (!g_cfg->setup_completed)
    {
      return;
    }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (!Update.end(true))
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    Update.abort();
  }
}

void webportal_set_data(const FlexitData& data, const String& mbStatus)
{
  g_data = data;
  g_mb = mbStatus;
  historyPush(data, mbStatus);

  g_diag_total_updates++;
  g_diag_last_mb = mbStatus;
  g_diag_last_sample_ms = nowEpochMs();
  if (mbStatus.endsWith("OFF")) g_diag_mb_off++;
  else if (mbStatus.startsWith("MB OK") || mbStatus.startsWith("BACNET OK")) g_diag_mb_ok++;
  else g_diag_mb_err++;
  if (mbStatus.indexOf("stale") >= 0) g_diag_stale++;
}

bool webportal_sta_connected() { return (WiFi.status() == WL_CONNECTED); }
bool webportal_ap_active() { return (WiFi.getMode() & WIFI_AP); }

void webportal_begin(DeviceConfig& cfg)
{
  g_cfg = &cfg;
  const char* headerKeys[] = {"Authorization"};
  server.collectHeaders(headerKeys, 1);

  // mDNS (STA mode only)
  if (WiFi.status() == WL_CONNECTED)
  {    if (MDNS.begin("ventreader"))
    {
      MDNS.addService("http", "tcp", 80);
    }
  }
  else
  {  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/status", HTTP_GET, [](){
    if (!g_cfg->homey_enabled && !g_cfg->ha_enabled)
    {
      server.send(403, "text/plain", "api disabled");
      return;
    }
    handleStatus();
  });
  server.on("/status/history", HTTP_GET, handleStatusHistory);
  server.on("/status/history.csv", HTTP_GET, handleStatusHistoryCsv);
  server.on("/status/diag", HTTP_GET, handleStatusDiag);
  server.on("/status/storage", HTTP_GET, handleStatusStorage);
  server.on("/ha/status", HTTP_GET, handleHaStatus);
  server.on("/ha/history", HTTP_GET, handleHaHistory);
  server.on("/ha/history.csv", HTTP_GET, handleHaHistoryCsv);
  server.on("/api/control/mode", HTTP_POST, handleControlMode);
  server.on("/api/control/setpoint", HTTP_POST, handleControlSetpoint);

  // Admin
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/admin/manual", HTTP_GET, handleAdminManual);
  server.on("/admin/manual.txt", HTTP_GET, handleAdminManualText);
  server.on("/admin/graphs", HTTP_GET, handleAdminGraphs);
  server.on("/admin/export/homey", HTTP_GET, handleAdminHomeyExport);
  server.on("/admin/export/homey.txt", HTTP_GET, handleAdminHomeyExportText);
  server.on("/admin/new_token", HTTP_POST, handleAdminNewToken);
  server.on("/admin/api_emergency_stop", HTTP_POST, handleAdminApiEmergencyStop);
  server.on("/admin/api/preview", HTTP_GET, handleAdminApiPreview);
  server.on("/admin/lang", HTTP_POST, handleAdminLang);
  server.on("/admin/test_bacnet", HTTP_POST, handleAdminBACnetTest);
  server.on("/admin/discover_bacnet", HTTP_POST, handleAdminBACnetDiscover);
  server.on("/admin/probe_bacnet_objects", HTTP_POST, handleAdminBACnetObjectProbe);
  server.on("/admin/scan_bacnet_objects", HTTP_POST, handleAdminBACnetObjectScan);
  server.on("/admin/probe_bacnet_write", HTTP_POST, handleAdminBACnetWriteProbe);
  server.on("/admin/bacnet_debug.txt", HTTP_GET, handleAdminBACnetDebug);
  server.on("/admin/clear_bacnet_debug", HTTP_POST, handleAdminBACnetDebugClear);
  server.on("/admin/bacnet_debug_mode", HTTP_POST, handleAdminBACnetDebugMode);
  server.on("/admin/refresh_now", HTTP_POST, handleRefreshNow);
  server.on("/admin/save", HTTP_POST, handleAdminSave);
  server.on("/admin/control/mode", HTTP_POST, handleAdminControlMode);
  server.on("/admin/control/setpoint", HTTP_POST, handleAdminControlSetpoint);
  server.on("/admin/ota", HTTP_GET, handleAdminOta);
  server.on("/admin/ota_upload", HTTP_POST, handleAdminOtaUploadDone, handleAdminOtaUploadStream);
  server.on("/admin/reboot", HTTP_POST, handleReboot);
  server.on("/admin/factory_reset", HTTP_POST, handleFactoryReset);

  // Forced setup
  server.on("/admin/setup", HTTP_GET, handleAdminSetup);
  server.on("/admin/setup_save", HTTP_POST, handleAdminSetupSave);

  server.begin();
}

void webportal_loop()
{
  server.handleClient();

  if (g_restart_pending && (int32_t)(millis() - g_restart_at_ms) >= 0)
  {
    delay(50);
    ESP.restart();
  }
}

bool webportal_consume_refresh_request()
{
  const bool v = g_refresh_requested;
  g_refresh_requested = false;
  return v;
}
