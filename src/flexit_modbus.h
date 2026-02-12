#pragma once
#include <Arduino.h>
#include "flexit_types.h"

// ============================================================================
// FLEXIT MODBUS MODULE (Nordic Basic)
// ----------------------------------------------------------------------------
// Default: DISABLED (compiles without ModbusMaster).
//
// When MAX3485 arrives:
//   1) Install library: ModbusMaster (Arduino Library Manager)
//   2) Set FLEXIT_MODBUS_ENABLED to 1 below
//   3) Upload and test; if no response, try FLEXIT_ADDR_OFFSET = -1
// ============================================================================

#ifndef FLEXIT_MODBUS_ENABLED
#define FLEXIT_MODBUS_ENABLED 0
#endif

// RS485 pins (suggested, can be changed)
#ifndef FLEXIT_RS485_RX
#define FLEXIT_RS485_RX 16   // ESP32 RX <- RO
#endif
#ifndef FLEXIT_RS485_TX
#define FLEXIT_RS485_TX 17   // ESP32 TX -> DI
#endif
#ifndef FLEXIT_RS485_EN
#define FLEXIT_RS485_EN 18   // RE+DE tied together
#endif

// Modbus config (verify in Flexit settings if needed)
#ifndef FLEXIT_MODBUS_BAUD
#define FLEXIT_MODBUS_BAUD 19200
#endif
#ifndef FLEXIT_MODBUS_ID
#define FLEXIT_MODBUS_ID 1
#endif

// Address offset (common Modbus gotcha: 0-based vs 1-based)
// Start with 0. If you get timeouts, try -1.
#ifndef FLEXIT_ADDR_OFFSET
#define FLEXIT_ADDR_OFFSET 0
#endif

struct FlexitModbusRuntimeConfig {
  String model = "S3";            // selected in admin
  uint32_t baud = FLEXIT_MODBUS_BAUD;
  uint8_t slave_id = FLEXIT_MODBUS_ID;
  int8_t addr_offset = FLEXIT_ADDR_OFFSET;
  String serial_format = "8N1";   // 8N1 / 8E1 / 8O1
  String transport_mode = "AUTO"; // AUTO (auto-direction transceiver) / MANUAL (DE/RE pin)
};

// ---------------------------------------------------------------------------
// Registers from Flexit: modbus_nordic_basic_2883.xlsx
// 3x = Input registers (FC04)
// 4x = Holding registers (FC03)
// Types are Float32 big-endian word order unless stated.
// ---------------------------------------------------------------------------

// Temperatures (Input, Float32, FC04) — each is 2 registers
#define FLEXIT_REG_UTELUFT_IN   1   // 3x0001 Outside air temperature
#define FLEXIT_REG_TILLUFT_IN   5   // 3x0005 Supply air temperature
#define FLEXIT_REG_AVTREKK_IN   9   // 3x0009 Extract air temperature
#define FLEXIT_REG_AVKAST_IN    13  // 3x0013 Exhaust air temperature

// Fan speed % (Holding, Float32, FC03)
#define FLEXIT_REG_FAN_PCT_HOLD 5   // 4x0005 Supply air fan speed

// Heating coil electric position % (Holding, Float32, FC03)
#define FLEXIT_REG_HEAT_PCT_HOLD 13 // 4x0013 Heating coil electric position

// Mode/state (Input, Uint16, FC04)
#define FLEXIT_REG_MODE_IN      3034 // 3x3034 Heat recovery ventilation state

// Control / write registers (Holding)
#define FLEXIT_REG_ROOM_MODE_HOLD        2013 // 4x2013 Room operating mode (RW)
#define FLEXIT_REG_COMFORT_BUTTON_HOLD   2040 // 4x2040 Comfort button / mode gating
#define FLEXIT_REG_FIREPLACE_TRIG_HOLD   3007 // 4x3007 Trigger temporary fireplace ventilation
#define FLEXIT_REG_SETPOINT_HOME_HOLD    1155 // 4x1155 Setpoint when home (RW)
#define FLEXIT_REG_SETPOINT_AWAY_HOLD    1163 // 4x1163 Setpoint when away (RW)

// Public API
bool flexit_modbus_is_enabled();
void flexit_modbus_set_enabled(bool enabled);
void flexit_modbus_set_runtime_config(const FlexitModbusRuntimeConfig& cfg);

bool flexit_modbus_begin();
bool flexit_modbus_poll(FlexitData& data);
const char* flexit_modbus_last_error();
const char* flexit_modbus_active_map();
bool flexit_modbus_write_mode(const String& modeCmd);
bool flexit_modbus_write_setpoint(const String& profile, float value);

// Mode mapping (from register list)
const char* flexit_mode_to_text(uint16_t v);
