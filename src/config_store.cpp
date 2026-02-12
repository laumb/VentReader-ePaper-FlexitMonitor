#include "config_store.h"

static String gen_token(size_t bytes_len = 16)
{
  // 16 bytes => 32 hex chars
  String out;
  out.reserve(bytes_len * 2);
  for (size_t i = 0; i < bytes_len; i++)
  {
    uint8_t b = (uint8_t)(esp_random() & 0xFF);
    const char* hex = "0123456789abcdef";
    out += hex[(b >> 4) & 0xF];
    out += hex[b & 0xF];
  }
  return out;
}


static Preferences prefs;

static bool is_supported_model(const String& model)
{
  return model == "S3" ||
         model == "S4" ||
         model == "S2_EXP" ||
         model == "S7_EXP" ||
         model == "CL3_EXP" ||
         model == "CL4_EXP";
}

static String normalize_lang(const String& in)
{
  if (in == "no" || in == "da" || in == "sv" || in == "fi" || in == "en" || in == "uk") return in;
  return "no";
}

static String normalize_data_source(const String& in)
{
  // Legacy migration: FLEXITWEB is mapped to local BACnet module.
  if (in == "BACNET" || in == "FLEXITWEB") return "BACNET";
  return "MODBUS";
}

void config_begin() {
  prefs.begin("flexit", false);
}

static String chip_suffix4() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t s = (uint16_t)(mac & 0xFFFF);
  char buf[5];
  snprintf(buf, sizeof(buf), "%04X", s);
  return String(buf);
}

String config_chip_suffix4() { return chip_suffix4(); }

String DeviceConfig::ap_ssid() const {
  return String("VentReader-Setup-") + chip_suffix4();
}

void config_apply_model_modbus_defaults(DeviceConfig& c, bool force)
{
  // Current recommended defaults for Nordic Basic Modbus family.
  // Keep model switch explicit so future models can diverge safely.
  if (force || c.modbus_transport_mode.length() == 0) c.modbus_transport_mode = "AUTO";
  if (force || c.modbus_serial_format.length() == 0) c.modbus_serial_format = "8E1";
  if (force || c.modbus_baud == 0) c.modbus_baud = 9600;
  if (force || c.modbus_slave_id == 0) c.modbus_slave_id = 1;
  if (force) c.modbus_addr_offset = 0;
}

DeviceConfig config_load() {
  DeviceConfig c;

  c.wifi_ssid = prefs.getString("ssid", "");
  c.wifi_pass = prefs.getString("wpass", "");

  c.api_token = prefs.getString("token", "");
  c.homey_api_token = prefs.getString("htoken", "");
  c.ha_api_token = prefs.getString("hatoken", "");
  c.token_generated = prefs.getBool("tgen", false);
  c.api_panic_stop = prefs.getBool("apikill", false);
  if (!c.token_generated || c.api_token.length() < 16 || c.api_token == "CHANGE_ME")
  {
    c.api_token = gen_token(16);
    c.token_generated = true;
    // persist immediately
    prefs.putString("token", c.api_token);
    prefs.putBool("tgen", true);
  }
  if (c.homey_api_token.length() < 16) c.homey_api_token = c.api_token;
  if (c.ha_api_token.length() < 16) c.ha_api_token = c.api_token;
  c.admin_user = prefs.getString("auser", "admin");
  c.admin_pass = prefs.getString("apass", "ventreader");
  c.admin_pass_changed = prefs.getBool("apchg", false);

  // wizard / onboarding
  c.setup_completed = prefs.getBool("setup", false);

  // model selection (Modbus map)
  c.model = prefs.getString("model", "S3");
  if (!is_supported_model(c.model)) c.model = "S3";

  c.modbus_enabled = prefs.getBool("modbus", false);
  c.homey_enabled  = prefs.getBool("homey", true);
  c.ha_enabled     = prefs.getBool("ha", true);
  c.display_enabled = prefs.getBool("disp", true);
  c.ha_mqtt_enabled = prefs.getBool("hamqtt", false);
  c.control_enabled = prefs.getBool("ctrl", false);
  c.data_source = normalize_data_source(prefs.getString("src", "MODBUS"));
  c.ha_mqtt_host = prefs.getString("hamhost", "");
  c.ha_mqtt_port = (uint16_t)prefs.getUInt("hamport", 1883);
  if (c.ha_mqtt_port == 0) c.ha_mqtt_port = 1883;
  c.ha_mqtt_user = prefs.getString("hamuser", "");
  c.ha_mqtt_pass = prefs.getString("hampass", "");
  c.ha_mqtt_prefix = prefs.getString("hampfx", "homeassistant");
  if (c.ha_mqtt_prefix.length() == 0) c.ha_mqtt_prefix = "homeassistant";
  c.ha_mqtt_topic_base = prefs.getString("hambase", "");
  if (c.ha_mqtt_topic_base.length() == 0)
    c.ha_mqtt_topic_base = String("ventreader/") + config_chip_suffix4();
  int hamInt = prefs.getInt("hamint", 60);
  if (hamInt < 10) hamInt = 10;
  if (hamInt > 3600) hamInt = 3600;
  c.ha_mqtt_interval_s = (uint16_t)hamInt;
  c.bacnet_ip = prefs.getString("bacip", "");
  c.bacnet_port = (uint16_t)prefs.getUInt("bacport", 47808);
  if (c.bacnet_port == 0) c.bacnet_port = 47808;
  c.bacnet_device_id = prefs.getUInt("bacid", 2);
  if (c.bacnet_device_id == 0) c.bacnet_device_id = 2;
  int bcp = prefs.getInt("bacpoll", 10);
  if (bcp < 5) bcp = 5;
  if (bcp > 60) bcp = 60;
  c.bacnet_poll_minutes = (uint8_t)bcp;
  int bct = prefs.getInt("bacto", 1500);
  if (bct < 300) bct = 300;
  if (bct > 8000) bct = 8000;
  c.bacnet_timeout_ms = (uint16_t)bct;
  c.bacnet_write_enabled = prefs.getBool("bacwr", false);
  c.bacnet_obj_outdoor = prefs.getString("baout", "ai:1");
  c.bacnet_obj_supply  = prefs.getString("basup", "ai:4");
  c.bacnet_obj_extract = prefs.getString("baext", "ai:59");
  c.bacnet_obj_exhaust = prefs.getString("baexh", "ai:11");
  c.bacnet_obj_fan     = prefs.getString("bafan", "ao:3");
  c.bacnet_obj_heat    = prefs.getString("baheat", "ao:29");
  c.bacnet_obj_mode    = prefs.getString("bamode", "msv:41");
  c.bacnet_obj_setpoint_home = prefs.getString("bashome", "av:126");
  c.bacnet_obj_setpoint_away = prefs.getString("basaway", "av:96");
  c.bacnet_mode_map    = prefs.getString("bamap", "2:AWAY,3:HOME,4:HIGH,5:FIRE");

  // Migrate old BACnet placeholder defaults to better Nordic S3 candidates.
  if (c.bacnet_obj_supply == "ai:2") c.bacnet_obj_supply = "ai:4";
  if (c.bacnet_obj_extract == "ai:3") c.bacnet_obj_extract = "ai:59";
  if (c.bacnet_obj_exhaust == "ai:4") c.bacnet_obj_exhaust = "ai:11";
  if (c.bacnet_obj_fan == "av:1") c.bacnet_obj_fan = "ao:3";
  if (c.bacnet_obj_heat == "av:2") c.bacnet_obj_heat = "ao:29";
  if (c.bacnet_obj_mode == "msv:1" || c.bacnet_obj_mode == "av:0") c.bacnet_obj_mode = "msv:41";
  // If mode object is MSV-based but old enum map is still present, shift to the observed Nordic S3 mapping.
  if (c.bacnet_obj_mode.startsWith("msv:") && c.bacnet_mode_map == "1:AWAY,2:HOME,3:HIGH,4:FIRE")
    c.bacnet_mode_map = "2:AWAY,3:HOME,4:HIGH,5:FIRE";
  // Nordic S3 observed mapping: home setpoint -> av:126, away setpoint -> av:96.
  // Migrate legacy defaults from earlier firmware generations.
  if (c.bacnet_obj_setpoint_home == "av:5") c.bacnet_obj_setpoint_home = "av:126";
  if (c.bacnet_obj_setpoint_away == "av:100") c.bacnet_obj_setpoint_away = "av:96";
  if (c.bacnet_obj_setpoint_home.length() == 0) c.bacnet_obj_setpoint_home = "av:126";
  if (c.bacnet_obj_setpoint_away.length() == 0) c.bacnet_obj_setpoint_away = "av:96";
  c.ui_language = normalize_lang(prefs.getString("lang", "no"));
  c.modbus_transport_mode = prefs.getString("mbtr", "");
  if (c.modbus_transport_mode != "AUTO" && c.modbus_transport_mode != "MANUAL")
    c.modbus_transport_mode = "";

  c.modbus_serial_format = prefs.getString("mbser", "");
  if (c.modbus_serial_format != "8N1" &&
      c.modbus_serial_format != "8E1" &&
      c.modbus_serial_format != "8O1")
    c.modbus_serial_format = "";

  c.modbus_baud = prefs.getUInt("mbbaud", 0);
  if (c.modbus_baud != 0 && (c.modbus_baud < 1200 || c.modbus_baud > 115200)) c.modbus_baud = 0;

  c.modbus_slave_id = (uint8_t)prefs.getUChar("mbid", 0);
  if (c.modbus_slave_id != 0 && (c.modbus_slave_id < 1 || c.modbus_slave_id > 247)) c.modbus_slave_id = 0;

  c.modbus_addr_offset = (int8_t)prefs.getChar("mboff", 0);
  if (c.modbus_addr_offset < -5 || c.modbus_addr_offset > 5) c.modbus_addr_offset = 0;

  // Fill missing values with model defaults.
  config_apply_model_modbus_defaults(c, false);

  c.poll_interval_ms = prefs.getULong("pollms", 10UL * 60UL * 1000UL);

  return c;
}

void config_save(const DeviceConfig& c) {
  prefs.putString("ssid", c.wifi_ssid);
  prefs.putString("wpass", c.wifi_pass);

  prefs.putString("token", c.api_token);
  prefs.putString("htoken", c.homey_api_token);
  prefs.putString("hatoken", c.ha_api_token);
  prefs.putBool("apikill", c.api_panic_stop);
  prefs.putString("auser", c.admin_user);
  prefs.putString("apass", c.admin_pass);
  prefs.putBool("apchg", c.admin_pass_changed);

  prefs.putBool("setup", c.setup_completed);
  prefs.putString("model", c.model);

  prefs.putBool("modbus", c.modbus_enabled);
  prefs.putBool("homey", c.homey_enabled);
  prefs.putBool("ha", c.ha_enabled);
  prefs.putBool("disp", c.display_enabled);
  prefs.putBool("hamqtt", c.ha_mqtt_enabled);
  prefs.putBool("ctrl", c.control_enabled);
  prefs.putString("src", normalize_data_source(c.data_source));
  prefs.putString("hamhost", c.ha_mqtt_host);
  uint32_t hamPort = c.ha_mqtt_port;
  if (hamPort == 0) hamPort = 1883;
  prefs.putUInt("hamport", hamPort);
  prefs.putString("hamuser", c.ha_mqtt_user);
  prefs.putString("hampass", c.ha_mqtt_pass);
  String hamPfx = c.ha_mqtt_prefix;
  if (hamPfx.length() == 0) hamPfx = "homeassistant";
  prefs.putString("hampfx", hamPfx);
  String hamBase = c.ha_mqtt_topic_base;
  if (hamBase.length() == 0) hamBase = String("ventreader/") + config_chip_suffix4();
  prefs.putString("hambase", hamBase);
  int hamInt = (int)c.ha_mqtt_interval_s;
  if (hamInt < 10) hamInt = 10;
  if (hamInt > 3600) hamInt = 3600;
  prefs.putInt("hamint", hamInt);
  prefs.putString("bacip", c.bacnet_ip);
  uint32_t port = c.bacnet_port;
  if (port == 0) port = 47808;
  prefs.putUInt("bacport", port);
  prefs.putUInt("bacid", c.bacnet_device_id);
  int bcp = (int)c.bacnet_poll_minutes;
  if (bcp < 5) bcp = 5;
  if (bcp > 60) bcp = 60;
  prefs.putInt("bacpoll", bcp);
  int bct = (int)c.bacnet_timeout_ms;
  if (bct < 300) bct = 300;
  if (bct > 8000) bct = 8000;
  prefs.putInt("bacto", bct);
  prefs.putBool("bacwr", c.bacnet_write_enabled);
  prefs.putString("baout", c.bacnet_obj_outdoor);
  prefs.putString("basup", c.bacnet_obj_supply);
  prefs.putString("baext", c.bacnet_obj_extract);
  prefs.putString("baexh", c.bacnet_obj_exhaust);
  prefs.putString("bafan", c.bacnet_obj_fan);
  prefs.putString("baheat", c.bacnet_obj_heat);
  prefs.putString("bamode", c.bacnet_obj_mode);
  prefs.putString("bashome", c.bacnet_obj_setpoint_home);
  prefs.putString("basaway", c.bacnet_obj_setpoint_away);
  prefs.putString("bamap", c.bacnet_mode_map);
  prefs.putString("lang", normalize_lang(c.ui_language));
  prefs.putString("mbtr", c.modbus_transport_mode);
  prefs.putString("mbser", c.modbus_serial_format);
  prefs.putUInt("mbbaud", c.modbus_baud);
  prefs.putUChar("mbid", c.modbus_slave_id);
  prefs.putChar("mboff", c.modbus_addr_offset);

  prefs.putULong("pollms", c.poll_interval_ms);
}

void config_factory_reset() {
  // wipe all keys in this namespace
  prefs.clear();
}
