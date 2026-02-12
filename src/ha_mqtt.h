#pragma once

#include <Arduino.h>
#include "config_store.h"
#include "flexit_types.h"

// Native Home Assistant integration via MQTT Discovery.
// Safe no-op if PubSubClient is unavailable at compile time.
void ha_mqtt_begin(const DeviceConfig& cfg);
void ha_mqtt_loop(const DeviceConfig& cfg, const FlexitData& data, const String& sourceStatus);
void ha_mqtt_request_publish_now();
bool ha_mqtt_is_active();
bool ha_mqtt_lib_available();
String ha_mqtt_last_error();
