#include "ha_mqtt.h"

#include <WiFi.h>

#if __has_include(<PubSubClient.h>)
#include <PubSubClient.h>
#define HA_MQTT_LIB 1
#else
#define HA_MQTT_LIB 0
#endif

#if HA_MQTT_LIB
static WiFiClient g_net;
static PubSubClient g_mqtt(g_net);
static bool g_discovery_sent = false;
static bool g_force_publish = true;
static uint32_t g_last_publish_ms = 0;
static uint32_t g_last_reconnect_attempt_ms = 0;
static String g_last_error = "";
static String g_cfg_fingerprint = "";
static String g_device_id = "";

static String escapeJson(const String& s)
{
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

static String sanitizeId(const String& in)
{
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++)
  {
    char c = in[i];
    if ((c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-')
      out += c;
    else if (c >= 'A' && c <= 'Z')
      out += (char)(c - 'A' + 'a');
    else
      out += '_';
  }
  if (out.length() == 0) out = "ventreader";
  return out;
}

static String chipHex()
{
  uint64_t mac = ESP.getEfuseMac();
  char b[13];
  snprintf(b, sizeof(b), "%012llx", (unsigned long long)mac);
  return String(b);
}

static String stateTopic(const DeviceConfig& cfg)
{
  return cfg.ha_mqtt_topic_base + "/state";
}

static String availabilityTopic(const DeviceConfig& cfg)
{
  return cfg.ha_mqtt_topic_base + "/availability";
}

static String discoveryTopic(const DeviceConfig& cfg, const char* component, const char* key)
{
  return cfg.ha_mqtt_prefix + "/" + String(component) + "/" + g_device_id + "/" + key + "/config";
}

static String cfgFingerprint(const DeviceConfig& cfg)
{
  return cfg.ha_mqtt_host + "|" + String(cfg.ha_mqtt_port) + "|" +
         cfg.ha_mqtt_user + "|" + cfg.ha_mqtt_prefix + "|" +
         cfg.ha_mqtt_topic_base + "|" + String(cfg.ha_mqtt_enabled ? "1" : "0");
}

static bool publishRetained(const String& topic, const String& payload)
{
  if (!g_mqtt.connected()) return false;
  bool ok = g_mqtt.publish(topic.c_str(), payload.c_str(), true);
  if (!ok) g_last_error = String("MQTT publish failed: ") + topic;
  return ok;
}

static bool publishEntityConfig(const DeviceConfig& cfg,
                                const char* component,
                                const char* key,
                                const char* name,
                                const char* icon,
                                const char* deviceClass,
                                const char* unit,
                                const char* stateClass,
                                const char* valueTemplate)
{
  String payload = "{";
  payload += "\"name\":\"" + escapeJson(String(name)) + "\",";
  payload += "\"uniq_id\":\"" + escapeJson(g_device_id + "_" + key) + "\",";
  payload += "\"stat_t\":\"" + escapeJson(stateTopic(cfg)) + "\",";
  payload += "\"val_tpl\":\"" + escapeJson(String(valueTemplate)) + "\",";
  if (icon && strlen(icon) > 0) payload += "\"ic\":\"" + escapeJson(String(icon)) + "\",";
  if (deviceClass && strlen(deviceClass) > 0) payload += "\"dev_cla\":\"" + escapeJson(String(deviceClass)) + "\",";
  if (unit && strlen(unit) > 0) payload += "\"unit_of_meas\":\"" + escapeJson(String(unit)) + "\",";
  if (stateClass && strlen(stateClass) > 0) payload += "\"stat_cla\":\"" + escapeJson(String(stateClass)) + "\",";
  payload += "\"obj_id\":\"" + escapeJson(g_device_id + "_" + key) + "\"";
  payload += "}";
  return publishRetained(discoveryTopic(cfg, component, key), payload);
}

static void publishDiscovery(const DeviceConfig& cfg)
{
  if (!g_mqtt.connected()) return;
  bool ok = true;
  ok &= publishEntityConfig(cfg, "sensor", "uteluft", "Uteluft", "mdi:tree-outline", "temperature", "°C", "measurement", "{{ value_json.uteluft }}");
  ok &= publishEntityConfig(cfg, "sensor", "tilluft", "Tilluft", "mdi:fan-chevron-up", "temperature", "°C", "measurement", "{{ value_json.tilluft }}");
  ok &= publishEntityConfig(cfg, "sensor", "avtrekk", "Avtrekk", "mdi:fan-arrow-up", "temperature", "°C", "measurement", "{{ value_json.avtrekk }}");
  ok &= publishEntityConfig(cfg, "sensor", "avkast", "Avkast", "mdi:fan-chevron-down", "temperature", "°C", "measurement", "{{ value_json.avkast }}");
  ok &= publishEntityConfig(cfg, "sensor", "fan", "Vifte", "mdi:fan", nullptr, "%", "measurement", "{{ value_json.fan }}");
  ok &= publishEntityConfig(cfg, "sensor", "heat", "Varme", "mdi:radiator", nullptr, "%", "measurement", "{{ value_json.heat }}");
  ok &= publishEntityConfig(cfg, "sensor", "efficiency", "Gjenvinning", "mdi:heat-wave", nullptr, "%", "measurement", "{{ value_json.efficiency }}");
  ok &= publishEntityConfig(cfg, "sensor", "mode", "Modus", "mdi:home-thermometer", nullptr, nullptr, nullptr, "{{ value_json.mode }}");
  ok &= publishEntityConfig(cfg, "sensor", "source", "Datakilde", "mdi:database", nullptr, nullptr, nullptr, "{{ value_json.data_source }}");
  ok &= publishEntityConfig(cfg, "binary_sensor", "stale", "Data stale", "mdi:clock-alert-outline", nullptr, nullptr, nullptr, "{{ value_json.stale }}");
  ok &= publishEntityConfig(cfg, "sensor", "data_time", "Siste dataoppdatering", "mdi:clock-check-outline", nullptr, nullptr, nullptr, "{{ value_json.data_time }}");
  ok &= publishEntityConfig(cfg, "sensor", "screen_time", "Siste skjermrefresh", "mdi:monitor", nullptr, nullptr, nullptr, "{{ value_json.screen_time }}");
  if (ok) g_discovery_sent = true;
}

static bool ensureConnected(const DeviceConfig& cfg)
{
  if (!cfg.ha_enabled || !cfg.ha_mqtt_enabled) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  if (g_mqtt.connected()) return true;
  const uint32_t now = millis();
  if (now - g_last_reconnect_attempt_ms < 3000) return false;
  g_last_reconnect_attempt_ms = now;

  g_mqtt.setServer(cfg.ha_mqtt_host.c_str(), cfg.ha_mqtt_port);
  String clientId = g_device_id + "_ha";
  bool ok;
  const String av = availabilityTopic(cfg);
  if (cfg.ha_mqtt_user.length() > 0)
  {
    ok = g_mqtt.connect(clientId.c_str(),
                        cfg.ha_mqtt_user.c_str(),
                        cfg.ha_mqtt_pass.c_str(),
                        av.c_str(), 0, true, "offline");
  }
  else
  {
    ok = g_mqtt.connect(clientId.c_str(), av.c_str(), 0, true, "offline");
  }
  if (!ok)
  {
    g_last_error = String("MQTT connect failed rc=") + g_mqtt.state();
    return false;
  }

  publishRetained(av, "online");
  g_force_publish = true;
  return true;
}

static String stateJson(const FlexitData& d, const String& sourceStatus, const DeviceConfig& cfg)
{
  auto f = [](float v) -> String {
    if (isnan(v)) return "null";
    char b[24];
    snprintf(b, sizeof(b), "%.2f", v);
    return String(b);
  };
  String s = "{";
  s += "\"mode\":\"" + escapeJson(d.mode) + "\",";
  s += "\"uteluft\":" + f(d.uteluft) + ",";
  s += "\"tilluft\":" + f(d.tilluft) + ",";
  s += "\"avtrekk\":" + f(d.avtrekk) + ",";
  s += "\"avkast\":" + f(d.avkast) + ",";
  s += "\"fan\":" + String(d.fan_percent) + ",";
  s += "\"heat\":" + String(d.heat_element_percent) + ",";
  s += "\"efficiency\":" + String(d.efficiency_percent) + ",";
  s += "\"data_source\":\"" + escapeJson(cfg.data_source) + "\",";
  s += "\"source_status\":\"" + escapeJson(sourceStatus) + "\",";
  s += "\"stale\":" + String(sourceStatus.indexOf("stale") >= 0 ? "true" : "false") + ",";
  s += "\"data_time\":\"" + escapeJson(d.data_time) + "\",";
  s += "\"screen_time\":\"" + escapeJson(d.time) + "\"";
  s += "}";
  return s;
}
#endif

void ha_mqtt_begin(const DeviceConfig& cfg)
{
#if HA_MQTT_LIB
  g_device_id = sanitizeId(String("ventreader_") + chipHex());
  g_cfg_fingerprint = cfgFingerprint(cfg);
  g_last_error = "";
  g_discovery_sent = false;
  g_force_publish = true;
  g_last_publish_ms = 0;
  g_last_reconnect_attempt_ms = 0;
#else
  (void)cfg;
#endif
}

void ha_mqtt_loop(const DeviceConfig& cfg, const FlexitData& data, const String& sourceStatus)
{
#if HA_MQTT_LIB
  const String fp = cfgFingerprint(cfg);
  if (fp != g_cfg_fingerprint)
  {
    g_cfg_fingerprint = fp;
    g_discovery_sent = false;
    g_force_publish = true;
    if (g_mqtt.connected()) g_mqtt.disconnect();
  }

  if (!cfg.ha_enabled || !cfg.ha_mqtt_enabled || cfg.ha_mqtt_host.length() == 0)
  {
    if (g_mqtt.connected()) g_mqtt.disconnect();
    return;
  }

  if (!ensureConnected(cfg)) return;
  g_mqtt.loop();

  if (!g_discovery_sent) publishDiscovery(cfg);

  uint32_t intMs = (uint32_t)cfg.ha_mqtt_interval_s * 1000UL;
  if (intMs < 10000UL) intMs = 10000UL;
  if (g_force_publish || millis() - g_last_publish_ms >= intMs)
  {
    String st = stateJson(data, sourceStatus, cfg);
    if (publishRetained(stateTopic(cfg), st))
    {
      g_last_publish_ms = millis();
      g_force_publish = false;
      g_last_error = "";
    }
  }
#else
  (void)cfg;
  (void)data;
  (void)sourceStatus;
#endif
}

void ha_mqtt_request_publish_now()
{
#if HA_MQTT_LIB
  g_force_publish = true;
#endif
}

bool ha_mqtt_is_active()
{
#if HA_MQTT_LIB
  return g_mqtt.connected();
#else
  return false;
#endif
}

bool ha_mqtt_lib_available()
{
#if HA_MQTT_LIB
  return true;
#else
  return false;
#endif
}

String ha_mqtt_last_error()
{
#if HA_MQTT_LIB
  return g_last_error;
#else
  return "PubSubClient library not found";
#endif
}
