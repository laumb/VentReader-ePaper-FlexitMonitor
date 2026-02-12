#include "flexit_modbus.h"

#if FLEXIT_MODBUS_ENABLED

#include <HardwareSerial.h>
#include <ModbusMaster.h>

static HardwareSerial& RS485Serial = Serial2;
static ModbusMaster node;

struct FlexitRegisterMap {
  const char* key;
  uint16_t reg_uteluft_in;
  uint16_t reg_tilluft_in;
  uint16_t reg_avtrekk_in;
  uint16_t reg_avkast_in;
  uint16_t reg_fan_pct_hold;
  uint16_t reg_heat_pct_hold;
  uint16_t reg_mode_in;
};

static const FlexitRegisterMap MAP_S3 = {
  "S3",
  FLEXIT_REG_UTELUFT_IN,
  FLEXIT_REG_TILLUFT_IN,
  FLEXIT_REG_AVTREKK_IN,
  FLEXIT_REG_AVKAST_IN,
  FLEXIT_REG_FAN_PCT_HOLD,
  FLEXIT_REG_HEAT_PCT_HOLD,
  FLEXIT_REG_MODE_IN
};

// Current production map for S4 in this project matches Nordic Basic addresses.
static const FlexitRegisterMap MAP_S4 = {
  "S4",
  FLEXIT_REG_UTELUFT_IN,
  FLEXIT_REG_TILLUFT_IN,
  FLEXIT_REG_AVTREKK_IN,
  FLEXIT_REG_AVKAST_IN,
  FLEXIT_REG_FAN_PCT_HOLD,
  FLEXIT_REG_HEAT_PCT_HOLD,
  FLEXIT_REG_MODE_IN
};

static bool started = false;
static bool configDirty = true;
static bool g_enabled = true;
static const char* lastError = "OK";
static const FlexitRegisterMap* activeMap = &MAP_S3;

static FlexitModbusRuntimeConfig runtimeCfg;

static bool isManualDirectionMode()
{
  return runtimeCfg.transport_mode == "MANUAL";
}

static uint32_t parseSerialConfig(const String& fmt)
{
  if (fmt == "8E1") return SERIAL_8E1;
  if (fmt == "8O1") return SERIAL_8O1;
  return SERIAL_8N1;
}

static const FlexitRegisterMap* mapForModel(const String& model)
{
  if (model == "S4") return &MAP_S4;
  return &MAP_S3;
}

static void preTransmission()
{
  if (!isManualDirectionMode()) return;
  digitalWrite(FLEXIT_RS485_EN, HIGH);
  delayMicroseconds(50);
}

static void postTransmission()
{
  if (!isManualDirectionMode()) return;
  delayMicroseconds(50);
  digitalWrite(FLEXIT_RS485_EN, LOW);
}

bool flexit_modbus_is_enabled() { return g_enabled; }
void flexit_modbus_set_enabled(bool enabled) { g_enabled = enabled; }
const char* flexit_modbus_last_error() { return lastError; }
const char* flexit_modbus_active_map() { return activeMap->key; }

void flexit_modbus_set_runtime_config(const FlexitModbusRuntimeConfig& cfg)
{
  bool changed = false;

  if (runtimeCfg.model != cfg.model) changed = true;
  if (runtimeCfg.baud != cfg.baud) changed = true;
  if (runtimeCfg.slave_id != cfg.slave_id) changed = true;
  if (runtimeCfg.addr_offset != cfg.addr_offset) changed = true;
  if (runtimeCfg.serial_format != cfg.serial_format) changed = true;
  if (runtimeCfg.transport_mode != cfg.transport_mode) changed = true;

  if (changed)
  {
    runtimeCfg = cfg;
    activeMap = mapForModel(runtimeCfg.model);
    configDirty = true;
  }
}

bool flexit_modbus_begin()
{
  if (started && !configDirty) return true;

  if (started && configDirty)
  {
    RS485Serial.end();
    started = false;
  }

  if (isManualDirectionMode())
  {
    pinMode(FLEXIT_RS485_EN, OUTPUT);
    digitalWrite(FLEXIT_RS485_EN, LOW); // listen by default
  }

  RS485Serial.begin(
    runtimeCfg.baud,
    parseSerialConfig(runtimeCfg.serial_format),
    FLEXIT_RS485_RX,
    FLEXIT_RS485_TX
  );

  node.begin(runtimeCfg.slave_id, RS485Serial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  started = true;
  configDirty = false;
  lastError = "OK";
  return true;
}

static bool readHolding(uint16_t reg, uint16_t count, uint16_t* out)
{
  uint8_t result = node.readHoldingRegisters(reg, count);
  if (result != node.ku8MBSuccess) return false;
  for (uint16_t i = 0; i < count; i++) out[i] = node.getResponseBuffer(i);
  return true;
}

static bool readInput(uint16_t reg, uint16_t count, uint16_t* out)
{
  uint8_t result = node.readInputRegisters(reg, count);
  if (result != node.ku8MBSuccess) return false;
  for (uint16_t i = 0; i < count; i++) out[i] = node.getResponseBuffer(i);
  return true;
}

static bool writeHoldingSingle(uint16_t reg, uint16_t value)
{
  uint8_t result = node.writeSingleRegister(reg, value);
  return (result == node.ku8MBSuccess);
}

static bool writeHoldingFloat32BE(uint16_t reg, float value)
{
  union { uint32_t u32; float f; } u;
  u.f = value;
  uint16_t hi = (uint16_t)((u.u32 >> 16) & 0xFFFF);
  uint16_t lo = (uint16_t)(u.u32 & 0xFFFF);

  node.clearTransmitBuffer();
  node.setTransmitBuffer(0, hi);
  node.setTransmitBuffer(1, lo);
  uint8_t result = node.writeMultipleRegisters(reg, 2);
  return (result == node.ku8MBSuccess);
}

// Float32 big-endian word order (hi word first)
static float decodeFloat32_BE(uint16_t hi, uint16_t lo)
{
  union { uint32_t u32; float f; } u;
  u.u32 = ((uint32_t)hi << 16) | (uint32_t)lo;
  return u.f;
}

const char* flexit_mode_to_text(uint16_t v)
{
  switch (v)
  {
    case 1: return "OFF";
    case 2: return "AWAY";
    case 3: return "HOME";
    case 4: return "HIGH";
    case 5: return "FUME";
    case 6: return "FIRE";
    case 7: return "TMP HIGH";
    default: return "UNKNOWN";
  }
}

bool flexit_modbus_poll(FlexitData& data)
{
  if (!g_enabled) { lastError = "MB OFF"; return false; }

  if (!flexit_modbus_begin())
  {
    lastError = "BEGIN";
    return false;
  }

  const int off = runtimeCfg.addr_offset;
  uint16_t t[2];

  // Temps (Input, 2 regs)
  if (!readInput((uint16_t)(activeMap->reg_uteluft_in + off), 2, t)) { lastError = "UTELUFT"; return false; }
  data.uteluft = decodeFloat32_BE(t[0], t[1]);

  if (!readInput((uint16_t)(activeMap->reg_tilluft_in + off), 2, t)) { lastError = "TILLUFT"; return false; }
  data.tilluft = decodeFloat32_BE(t[0], t[1]);

  if (!readInput((uint16_t)(activeMap->reg_avtrekk_in + off), 2, t)) { lastError = "AVTREKK"; return false; }
  data.avtrekk = decodeFloat32_BE(t[0], t[1]);

  if (!readInput((uint16_t)(activeMap->reg_avkast_in + off), 2, t)) { lastError = "AVKAST"; return false; }
  data.avkast = decodeFloat32_BE(t[0], t[1]);

  // FAN % (Holding, 2 regs)
  if (!readHolding((uint16_t)(activeMap->reg_fan_pct_hold + off), 2, t)) { lastError = "FAN"; return false; }
  float fanPct = decodeFloat32_BE(t[0], t[1]);
  if (fanPct < 0) fanPct = 0;
  if (fanPct > 100) fanPct = 100;
  data.fan_percent = (int)lroundf(fanPct);

  // HEAT % (Holding, 2 regs)
  if (!readHolding((uint16_t)(activeMap->reg_heat_pct_hold + off), 2, t)) { lastError = "HEAT"; return false; }
  float heatPct = decodeFloat32_BE(t[0], t[1]);
  if (heatPct < 0) heatPct = 0;
  if (heatPct > 100) heatPct = 100;
  data.heat_element_percent = (int)lroundf(heatPct);

  // MODE (Input, 1 reg)
  uint16_t m = 0;
  if (!readInput((uint16_t)(activeMap->reg_mode_in + off), 1, &m)) { lastError = "MODE"; return false; }
  data.mode = flexit_mode_to_text(m);

  // Active setpoint (best-effort): read both profiles and select by mode.
  // Do not fail polling if these reads are unavailable on a specific model/fw.
  float spHome = NAN;
  float spAway = NAN;
  if (readHolding((uint16_t)(FLEXIT_REG_SETPOINT_HOME_HOLD + off), 2, t))
    spHome = decodeFloat32_BE(t[0], t[1]);
  if (readHolding((uint16_t)(FLEXIT_REG_SETPOINT_AWAY_HOLD + off), 2, t))
    spAway = decodeFloat32_BE(t[0], t[1]);

  // Display set-temp should be stable and predictable:
  // use HOME profile setpoint as primary value, AWAY only as fallback.
  data.set_temp = !isnan(spHome) ? spHome : spAway;

  lastError = "OK";
  return true;
}

bool flexit_modbus_write_mode(const String& modeCmd)
{
  if (!g_enabled) { lastError = "MB OFF"; return false; }
  if (!flexit_modbus_begin()) { lastError = "BEGIN"; return false; }

  const int off = runtimeCfg.addr_offset;

  String m = modeCmd;
  m.toUpperCase();

  // Ensure room mode register is active from comfort button logic.
  if (!writeHoldingSingle((uint16_t)(FLEXIT_REG_COMFORT_BUTTON_HOLD + off), 1))
  {
    lastError = "COMFORT";
    return false;
  }

  if (m == "AWAY")
  {
    if (!writeHoldingSingle((uint16_t)(FLEXIT_REG_ROOM_MODE_HOLD + off), 2)) { lastError = "MODE"; return false; }
    lastError = "OK";
    return true;
  }
  if (m == "HOME")
  {
    if (!writeHoldingSingle((uint16_t)(FLEXIT_REG_ROOM_MODE_HOLD + off), 3)) { lastError = "MODE"; return false; }
    lastError = "OK";
    return true;
  }
  if (m == "HIGH")
  {
    if (!writeHoldingSingle((uint16_t)(FLEXIT_REG_ROOM_MODE_HOLD + off), 4)) { lastError = "MODE"; return false; }
    lastError = "OK";
    return true;
  }
  if (m == "FIRE")
  {
    // Trigger temporary fireplace ventilation.
    if (!writeHoldingSingle((uint16_t)(FLEXIT_REG_FIREPLACE_TRIG_HOLD + off), 2)) { lastError = "FIRE"; return false; }
    lastError = "OK";
    return true;
  }

  lastError = "MODE PARAM";
  return false;
}

bool flexit_modbus_write_setpoint(const String& profile, float value)
{
  if (!g_enabled) { lastError = "MB OFF"; return false; }
  if (!flexit_modbus_begin()) { lastError = "BEGIN"; return false; }

  if (value < 10.0f || value > 30.0f)
  {
    lastError = "SETPOINT RANGE";
    return false;
  }

  const int off = runtimeCfg.addr_offset;
  String p = profile;
  p.toLowerCase();

  uint16_t reg = 0;
  if (p == "home") reg = FLEXIT_REG_SETPOINT_HOME_HOLD;
  else if (p == "away") reg = FLEXIT_REG_SETPOINT_AWAY_HOLD;
  else
  {
    lastError = "SETPOINT PROFILE";
    return false;
  }

  if (!writeHoldingFloat32BE((uint16_t)(reg + off), value))
  {
    lastError = "SETPOINT WRITE";
    return false;
  }

  lastError = "OK";
  return true;
}

#else

static const char* lastError = "MODBUS DISABLED";

static bool g_enabled = false;
bool flexit_modbus_is_enabled() { return g_enabled; }
void flexit_modbus_set_enabled(bool enabled) { g_enabled = enabled; }
void flexit_modbus_set_runtime_config(const FlexitModbusRuntimeConfig& cfg) { (void)cfg; }
bool flexit_modbus_begin() { lastError = "MODBUS DISABLED"; return false; }
bool flexit_modbus_poll(FlexitData& data) { (void)data; lastError = "MODBUS DISABLED"; return false; }
const char* flexit_modbus_last_error() { return lastError; }
const char* flexit_modbus_active_map() { return "DISABLED"; }
bool flexit_modbus_write_mode(const String& modeCmd) { (void)modeCmd; lastError = "MODBUS DISABLED"; return false; }
bool flexit_modbus_write_setpoint(const String& profile, float value) { (void)profile; (void)value; lastError = "MODBUS DISABLED"; return false; }
const char* flexit_mode_to_text(uint16_t v) { (void)v; return "DISABLED"; }

#endif
