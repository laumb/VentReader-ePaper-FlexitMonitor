#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct DeviceConfig {
  String wifi_ssid;
  String wifi_pass;

  String api_token;     // required for /status
  String homey_api_token; // dedicated token for /status and Homey endpoints
  String ha_api_token;    // dedicated token for /ha/* endpoints
  String admin_user;    // default: admin
  String admin_pass;    // default: flexit123 (until changed)

  bool admin_pass_changed; // must be true before allowing normal admin actions

  bool token_generated; // set once after secure token is generated
  bool api_panic_stop;  // emergency stop for all token-protected API endpoints

  // Setup flow
  bool setup_completed; // true after wizard is finished

  // Ventilation unit model (affects Modbus register map)
  // Values: "S3", "S4", and selected experimental variants
  String model;

  bool modbus_enabled;
  bool homey_enabled;
  bool ha_enabled;
  bool display_enabled; // false = headless (no physical display)
  bool ha_mqtt_enabled; // Home Assistant native MQTT Discovery
  bool control_enabled; // allows Modbus write actions via API (experimental)
  String data_source;   // "MODBUS" or "BACNET"

  // Home Assistant MQTT Discovery (native HA entities)
  String ha_mqtt_host;        // broker host/ip
  uint16_t ha_mqtt_port;      // default 1883
  String ha_mqtt_user;        // optional
  String ha_mqtt_pass;        // optional
  String ha_mqtt_prefix;      // discovery prefix, default "homeassistant"
  String ha_mqtt_topic_base;  // state base topic, default "ventreader/<chip>"
  uint16_t ha_mqtt_interval_s;// publish interval seconds

  // Local BACnet/IP source
  String bacnet_ip;             // target device IP (required)
  uint16_t bacnet_port;         // UDP port, default 47808
  uint32_t bacnet_device_id;    // required BACnet device instance
  uint8_t bacnet_poll_minutes;  // 5..60
  uint16_t bacnet_timeout_ms;   // per request timeout
  bool bacnet_write_enabled;    // enables experimental BACnet writes

  // BACnet object mapping (format: "ai:1", "av:2", "msv:3")
  String bacnet_obj_outdoor;
  String bacnet_obj_supply;
  String bacnet_obj_extract;
  String bacnet_obj_exhaust;
  String bacnet_obj_fan;
  String bacnet_obj_heat;
  String bacnet_obj_mode;
  String bacnet_obj_setpoint_home;
  String bacnet_obj_setpoint_away;
  String bacnet_mode_map;       // e.g. "2:AWAY,3:HOME,4:HIGH,5:FIRE"

  String ui_language; // no, da, sv, fi, en, uk

  // Modbus runtime options
  String modbus_transport_mode; // "AUTO" or "MANUAL"
  String modbus_serial_format;  // "8N1", "8E1", "8O1"
  uint32_t modbus_baud;         // e.g. 19200
  uint8_t modbus_slave_id;      // 1..247
  int8_t modbus_addr_offset;    // typically 0 or -1

  uint32_t poll_interval_ms; // data/UI refresh (default 10min)

  // computed helpers
  String ap_ssid() const; // Flexit-Setup-XXXX
};

void config_begin();
DeviceConfig config_load();
void config_save(const DeviceConfig& cfg);
void config_factory_reset();        // wipes config namespace
String config_chip_suffix4();       // last 4 hex of MAC (for SSID)
void config_apply_model_modbus_defaults(DeviceConfig& cfg, bool force = false);
