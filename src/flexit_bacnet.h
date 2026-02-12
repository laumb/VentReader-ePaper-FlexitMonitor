#pragma once

#include <Arduino.h>
#include "flexit_types.h"
#include "config_store.h"

void flexit_bacnet_set_runtime_config(const DeviceConfig& cfg);
bool flexit_bacnet_poll(FlexitData& out);
const char* flexit_bacnet_last_error();
bool flexit_bacnet_is_ready();
bool flexit_bacnet_write_mode(const String& modeCmd);
bool flexit_bacnet_write_setpoint(const String& profile, float value);

// Active test against configured BACnet target.
// Returns false and fills reason on transport/protocol/object errors.
bool flexit_bacnet_test(FlexitData* outData, String* reason);

// Broadcast Who-Is and return compact JSON array with discovered peers.
// Example item: {"ip":"192.168.1.50","port":47808,"device_id":12345,"vendor_id":5}
String flexit_bacnet_autodiscover_json(uint16_t wait_ms = 1800);
String flexit_bacnet_probe_configured_objects_json();
String flexit_bacnet_scan_objects_json(uint16_t inst_from = 0, uint16_t inst_to = 64,
                                       uint16_t timeout_ms = 450, uint16_t max_hits = 40,
                                       int16_t only_type = -1);

// Debug helpers for admin diagnostics.
String flexit_bacnet_debug_dump_text();
void flexit_bacnet_debug_clear();
void flexit_bacnet_debug_set_enabled(bool enabled);
bool flexit_bacnet_debug_is_enabled();
