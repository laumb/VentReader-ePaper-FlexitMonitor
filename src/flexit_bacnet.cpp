#include "flexit_bacnet.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

static DeviceConfig g_cfg;
static String g_last_error = "OFF";
static uint8_t g_invoke = 1;
static String g_debug_log;
static const size_t DEBUG_MAX_CHARS = 20000;
static bool g_debug_enabled = false;
static bool g_quiet_data_errors = false;
static FlexitData g_last_partial_data;
static bool g_has_last_partial_data = false;

static void dbg(const String& msg)
{
  if (!g_debug_enabled) return;
  String line = String(millis()) + "ms | " + msg + "\n";
  g_debug_log += line;
  if (g_debug_log.length() > DEBUG_MAX_CHARS)
  {
    g_debug_log.remove(0, g_debug_log.length() - DEBUG_MAX_CHARS);
  }
}

static String hexDump(const uint8_t* p, size_t n, size_t maxBytes = 48)
{
  if (!p || n == 0) return "";
  if (n > maxBytes) n = maxBytes;
  String out;
  out.reserve(n * 3 + 8);
  const char* hx = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++)
  {
    uint8_t b = p[i];
    out += hx[(b >> 4) & 0x0F];
    out += hx[b & 0x0F];
    if (i + 1 < n) out += ' ';
  }
  return out;
}

static String normalizeIpLike(const String& in)
{
  String s = in;
  s.trim();
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if ((c >= '0' && c <= '9') || c == '.') out += c;
  }
  return out;
}

static String trimCopy(const String& in)
{
  String s = in;
  s.trim();
  return s;
}

static void setErr(const String& stage, const String& msg)
{
  g_last_error = stage + ": " + msg;
  if (g_quiet_data_errors && stage == "DATA") return;
  dbg("ERR " + g_last_error);
}

static String stripStagePrefix(const String& s)
{
  int p = s.indexOf(':');
  if (p < 0) return s;
  if (p + 1 >= (int)s.length()) return "";
  String r = s.substring(p + 1);
  r.trim();
  return r;
}

struct ObjRef
{
  uint16_t type = 0;
  uint32_t instance = 0;
  bool valid = false;
};

static bool parseUint(const String& s, uint32_t& out)
{
  if (s.length() == 0) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c < '0' || c > '9') return false;
    v = (v * 10U) + (uint32_t)(c - '0');
  }
  out = v;
  return true;
}

static bool parseObjType(const String& s, uint16_t& out)
{
  String t = trimCopy(s);
  t.toLowerCase();
  if (t == "ai" || t == "analog-input" || t == "analoginput") { out = 0; return true; }
  if (t == "ao" || t == "analog-output" || t == "analogoutput") { out = 1; return true; }
  if (t == "av" || t == "analog-value" || t == "analogvalue") { out = 2; return true; }
  if (t == "bi" || t == "binary-input" || t == "binaryinput") { out = 3; return true; }
  if (t == "bo" || t == "binary-output" || t == "binaryoutput") { out = 4; return true; }
  if (t == "bv" || t == "binary-value" || t == "binaryvalue") { out = 5; return true; }
  if (t == "mi" || t == "multi-state-input" || t == "multistateinput") { out = 13; return true; }
  if (t == "mo" || t == "multi-state-output" || t == "multistateoutput") { out = 14; return true; }
  if (t == "msv" || t == "multi-state-value" || t == "multistatevalue") { out = 19; return true; }

  uint32_t n = 0;
  if (parseUint(t, n) && n < 1024)
  {
    out = (uint16_t)n;
    return true;
  }
  return false;
}

static ObjRef parseObjRef(const String& specIn)
{
  ObjRef r;
  String spec = trimCopy(specIn);
  if (spec.length() == 0) return r;

  int sep = spec.indexOf(':');
  if (sep < 0) sep = spec.indexOf('/');
  if (sep < 0) sep = spec.indexOf('.');
  if (sep < 0) return r;

  String t = spec.substring(0, sep);
  String i = spec.substring(sep + 1);
  uint16_t type = 0;
  uint32_t inst = 0;
  if (!parseObjType(t, type)) return r;
  if (!parseUint(trimCopy(i), inst)) return r;
  if (inst > 4194303UL) return r;

  r.type = type;
  r.instance = inst;
  r.valid = true;
  return r;
}

static bool parseIP(const String& in, IPAddress& out)
{
  String s = normalizeIpLike(in);
  int a = 0, b = 0, c = 0, d = 0;
  if (sscanf(s.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  out = IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
  return true;
}

static void putU16BE(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)((v >> 8) & 0xFF);
  p[1] = (uint8_t)(v & 0xFF);
}

static void putU32BE(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)((v >> 24) & 0xFF);
  p[1] = (uint8_t)((v >> 16) & 0xFF);
  p[2] = (uint8_t)((v >> 8) & 0xFF);
  p[3] = (uint8_t)(v & 0xFF);
}

static uint16_t u16BE(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t u32BE(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool locateApdu(const uint8_t* p, size_t n, size_t& apduPos, size_t& bvlcLen)
{
  apduPos = 0;
  bvlcLen = 0;
  if (!p || n < 6) return false;
  if (p[0] != 0x81) return false; // BVLL for BACnet/IP
  const uint8_t bvlcFn = p[1];
  uint16_t blen = u16BE(p + 2);
  if (blen < 6 || blen > n) return false;
  bvlcLen = blen;

  size_t i = 4;
  // Forwarded-NPDU has 6-byte originating BACnet/IP address
  if (bvlcFn == 0x04)
  {
    if (i + 6 > blen) return false;
    i += 6;
  }
  if (i + 2 > blen) return false;
  if (p[i] != 0x01) return false; // NPDU version
  uint8_t ctrl = p[i + 1];
  i += 2;

  // Destination specifier present
  if (ctrl & 0x20)
  {
    if (i + 3 > blen) return false; // DNET(2) + DLEN(1)
    i += 2;
    uint8_t dlen = p[i++];
    if (i + dlen > blen) return false;
    i += dlen; // DADR
    // Hop count is required when destination is present
    if (i + 1 > blen) return false;
    i += 1;
  }

  // Source specifier present
  if (ctrl & 0x08)
  {
    if (i + 3 > blen) return false; // SNET(2) + SLEN(1)
    i += 2;
    uint8_t slen = p[i++];
    if (i + slen > blen) return false;
    i += slen; // SADR
  }

  // Network layer message (no APDU payload for our use)
  if (ctrl & 0x80) return false;

  if (i >= blen) return false;
  apduPos = i;
  return true;
}

static bool sendWhoIs(WiFiUDP& udp, const IPAddress& ip, uint16_t port, bool broadcast)
{
  uint8_t pkt[16];
  // BVLC
  pkt[0] = 0x81;
  pkt[1] = broadcast ? 0x0B : 0x0A; // original-broadcast / original-unicast
  putU16BE(pkt + 2, 8);
  // NPDU + APDU
  pkt[4] = 0x01;
  pkt[5] = 0x00;
  pkt[6] = 0x10; // Unconfirmed request
  pkt[7] = 0x08; // Who-Is

  if (!udp.beginPacket(ip, port)) return false;
  size_t w = udp.write(pkt, 8);
  if (w != 8) { udp.endPacket(); return false; }
  bool ok = (udp.endPacket() == 1);
  dbg(String("TX Who-Is -> ") + ip.toString() + ":" + String(port) + (broadcast ? " (broadcast)" : " (unicast)") + (ok ? " ok" : " fail"));
  return ok;
}

static bool sendReadProperty(WiFiUDP& udp, const IPAddress& ip, uint16_t port,
                             uint8_t invoke, uint16_t objType, uint32_t objInst,
                             uint32_t propId)
{
  uint8_t pkt[32];
  size_t n = 0;

  // BVLC
  pkt[n++] = 0x81;
  pkt[n++] = 0x0A; // original-unicast
  pkt[n++] = 0x00; // len hi (fill later)
  pkt[n++] = 0x00; // len lo

  // NPDU (expecting reply)
  pkt[n++] = 0x01;
  pkt[n++] = 0x04;

  // APDU confirmed request (no segmentation, max APDU accepted up to 1476)
  pkt[n++] = 0x00;
  pkt[n++] = 0x05;
  pkt[n++] = invoke;
  pkt[n++] = 0x0C; // ReadProperty

  // Context 0: object identifier (4 bytes)
  pkt[n++] = 0x0C;
  uint32_t oid = (((uint32_t)objType & 0x3FFUL) << 22) | (objInst & 0x3FFFFFUL);
  putU32BE(pkt + n, oid); n += 4;

  // Context 1: property identifier
  if (propId <= 0xFF)
  {
    pkt[n++] = 0x19;
    pkt[n++] = (uint8_t)propId;
  }
  else if (propId <= 0xFFFF)
  {
    pkt[n++] = 0x1A;
    pkt[n++] = (uint8_t)((propId >> 8) & 0xFF);
    pkt[n++] = (uint8_t)(propId & 0xFF);
  }
  else
  {
    pkt[n++] = 0x1C;
    putU32BE(pkt + n, propId); n += 4;
  }

  putU16BE(pkt + 2, (uint16_t)n);

  if (!udp.beginPacket(ip, port)) return false;
  size_t w = udp.write(pkt, n);
  if (w != n) { udp.endPacket(); return false; }
  bool ok = (udp.endPacket() == 1);
  dbg(String("TX ReadProperty inv=") + invoke +
      " obj=" + String(objType) + ":" + String(objInst) +
      " prop=" + String(propId) +
      " -> " + ip.toString() + ":" + String(port) +
      (ok ? " ok" : " fail"));
  return ok;
}

static bool sendWritePropertyReal(WiFiUDP& udp, const IPAddress& ip, uint16_t port,
                                  uint8_t invoke, uint16_t objType, uint32_t objInst,
                                  uint32_t propId, float value, int priority = -1)
{
  uint8_t pkt[48];
  size_t n = 0;
  pkt[n++] = 0x81;
  pkt[n++] = 0x0A; // original-unicast
  pkt[n++] = 0x00;
  pkt[n++] = 0x00;
  pkt[n++] = 0x01; // NPDU version
  pkt[n++] = 0x04; // expecting reply

  pkt[n++] = 0x00; // confirmed req
  pkt[n++] = 0x05; // max segments/apdu
  pkt[n++] = invoke;
  pkt[n++] = 0x0F; // WriteProperty service choice

  pkt[n++] = 0x0C; // context 0, len 4
  uint32_t oid = (((uint32_t)objType & 0x3FFUL) << 22) | (objInst & 0x3FFFFFUL);
  putU32BE(pkt + n, oid); n += 4;

  if (propId <= 0xFF)
  {
    pkt[n++] = 0x19; pkt[n++] = (uint8_t)propId;
  }
  else if (propId <= 0xFFFF)
  {
    pkt[n++] = 0x1A;
    pkt[n++] = (uint8_t)((propId >> 8) & 0xFF);
    pkt[n++] = (uint8_t)(propId & 0xFF);
  }
  else
  {
    pkt[n++] = 0x1C;
    putU32BE(pkt + n, propId); n += 4;
  }

  pkt[n++] = 0x3E; // opening tag 3 (property value)
  pkt[n++] = 0x44; // real, 4 bytes
  uint32_t raw = 0;
  memcpy(&raw, &value, sizeof(float));
  putU32BE(pkt + n, raw); n += 4;
  pkt[n++] = 0x3F; // closing tag 3

  // Optional BACnet command priority (1..16) as context tag 4.
  if (priority >= 1 && priority <= 16)
  {
    pkt[n++] = 0x49; // context tag 4, len 1
    pkt[n++] = (uint8_t)priority;
  }

  putU16BE(pkt + 2, (uint16_t)n);
  if (!udp.beginPacket(ip, port)) return false;
  size_t w = udp.write(pkt, n);
  if (w != n) { udp.endPacket(); return false; }
  bool ok = (udp.endPacket() == 1);
  dbg(String("TX WriteProperty(real) inv=") + invoke +
      " obj=" + String(objType) + ":" + String(objInst) +
      " prop=" + String(propId) +
      " val=" + String(value, 2) +
      (priority >= 1 && priority <= 16 ? (String(" prio=") + String(priority)) : "") +
      " -> " + ip.toString() + ":" + String(port) +
      (ok ? " ok" : " fail"));
  return ok;
}

static bool sendWritePropertyEnum(WiFiUDP& udp, const IPAddress& ip, uint16_t port,
                                  uint8_t invoke, uint16_t objType, uint32_t objInst,
                                  uint32_t propId, uint32_t enumValue)
{
  uint8_t pkt[48];
  size_t n = 0;
  pkt[n++] = 0x81;
  pkt[n++] = 0x0A; // original-unicast
  pkt[n++] = 0x00;
  pkt[n++] = 0x00;
  pkt[n++] = 0x01; // NPDU version
  pkt[n++] = 0x04; // expecting reply

  pkt[n++] = 0x00; // confirmed req
  pkt[n++] = 0x05; // max segments/apdu
  pkt[n++] = invoke;
  pkt[n++] = 0x0F; // WriteProperty

  pkt[n++] = 0x0C; // context 0, len 4
  uint32_t oid = (((uint32_t)objType & 0x3FFUL) << 22) | (objInst & 0x3FFFFFUL);
  putU32BE(pkt + n, oid); n += 4;

  if (propId <= 0xFF)
  {
    pkt[n++] = 0x19; pkt[n++] = (uint8_t)propId;
  }
  else if (propId <= 0xFFFF)
  {
    pkt[n++] = 0x1A;
    pkt[n++] = (uint8_t)((propId >> 8) & 0xFF);
    pkt[n++] = (uint8_t)(propId & 0xFF);
  }
  else
  {
    pkt[n++] = 0x1C;
    putU32BE(pkt + n, propId); n += 4;
  }

  pkt[n++] = 0x3E; // opening tag 3
  uint8_t bytes[4];
  size_t len = 1;
  if (enumValue <= 0xFFUL) { len = 1; bytes[0] = (uint8_t)enumValue; }
  else if (enumValue <= 0xFFFFUL) { len = 2; bytes[0] = (uint8_t)(enumValue >> 8); bytes[1] = (uint8_t)enumValue; }
  else if (enumValue <= 0xFFFFFFUL) { len = 3; bytes[0] = (uint8_t)(enumValue >> 16); bytes[1] = (uint8_t)(enumValue >> 8); bytes[2] = (uint8_t)enumValue; }
  else { len = 4; bytes[0] = (uint8_t)(enumValue >> 24); bytes[1] = (uint8_t)(enumValue >> 16); bytes[2] = (uint8_t)(enumValue >> 8); bytes[3] = (uint8_t)enumValue; }
  if (len <= 4)
  {
    pkt[n++] = (uint8_t)(0x90 | len); // enum app tag
    for (size_t i = 0; i < len; i++) pkt[n++] = bytes[i];
  }
  pkt[n++] = 0x3F; // closing tag 3

  putU16BE(pkt + 2, (uint16_t)n);
  if (!udp.beginPacket(ip, port)) return false;
  size_t w = udp.write(pkt, n);
  if (w != n) { udp.endPacket(); return false; }
  bool ok = (udp.endPacket() == 1);
  dbg(String("TX WriteProperty(enum) inv=") + invoke +
      " obj=" + String(objType) + ":" + String(objInst) +
      " prop=" + String(propId) +
      " val=" + String(enumValue) +
      " -> " + ip.toString() + ":" + String(port) +
      (ok ? " ok" : " fail"));
  return ok;
}

struct IAmInfo
{
  bool ok = false;
  IPAddress ip;
  uint16_t port = 0;
  uint32_t device_id = 0;
  uint16_t vendor_id = 0;
};

static bool parseUnsignedApp(const uint8_t* p, size_t n, uint32_t& out, size_t& used)
{
  used = 0;
  if (!p || n < 2) return false;
  uint8_t tag = p[0];
  if (tag & 0x08) return false; // application tag only
  uint8_t appTag = (uint8_t)((tag >> 4) & 0x0F);
  if (appTag != 2) return false; // unsigned
  uint8_t lvt = (uint8_t)(tag & 0x07);
  size_t pos = 1;
  size_t len = lvt;
  if (lvt == 5)
  {
    if (n < 2) return false;
    len = p[pos++];
  }
  if (len < 1 || len > 4) return false;
  if (pos + len > n) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) v = (v << 8) | p[pos + i];
  out = v;
  used = pos + len;
  return true;
}

static bool parseEnumApp(const uint8_t* p, size_t n, uint32_t& out, size_t& used)
{
  used = 0;
  if (!p || n < 2) return false;
  uint8_t tag = p[0];
  if (tag & 0x08) return false; // application tag only
  uint8_t appTag = (uint8_t)((tag >> 4) & 0x0F);
  if (appTag != 9) return false; // enumerated
  uint8_t lvt = (uint8_t)(tag & 0x07);
  size_t pos = 1;
  size_t len = lvt;
  if (lvt == 5)
  {
    if (n < 2) return false;
    len = p[pos++];
  }
  if (len < 1 || len > 4) return false;
  if (pos + len > n) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) v = (v << 8) | p[pos + i];
  out = v;
  used = pos + len;
  return true;
}

static bool parseIAm(const uint8_t* p, size_t n, IAmInfo& out)
{
  if (!p || n < 12) return false;
  size_t apduPos = 0, blen = 0;
  if (!locateApdu(p, n, apduPos, blen)) return false;
  size_t i = apduPos;
  if (i + 2 > blen) return false;
  if (p[i] != 0x10 || p[i + 1] != 0x00) return false; // unconfirmed request + I-Am
  i += 2;

  // I-Am payload uses APPLICATION tags:
  // object-id, unsigned(max-apdu), enum(segmentation), unsigned(vendor-id)
  // object-id (application tag 12, len 4). Flexit replies with 0xC4.
  if (i + 5 > blen) return false;
  if (p[i] != 0xC4 && p[i] != 0x0C) return false;
  uint32_t oid = u32BE(p + i + 1);
  uint16_t type = (uint16_t)((oid >> 22) & 0x3FF);
  uint32_t inst = oid & 0x3FFFFF;
  if (type != 8) return false; // device object
  i += 5;

  uint32_t maxApdu = 0, seg = 0, vendor = 0;
  size_t used = 0;
  if (!parseUnsignedApp(p + i, blen - i, maxApdu, used)) return false;
  i += used;

  // Some devices include one extra app/null byte before segmentation enum.
  // Scan forward for first enum(tag9) and then first unsigned(tag2) after that.
  bool segOk = false;
  size_t j = i;
  for (; j < blen; j++)
  {
    size_t u = 0;
    if (parseEnumApp(p + j, blen - j, seg, u))
    {
      segOk = true;
      j += u;
      break;
    }
  }
  if (!segOk) return false;

  bool vendorOk = false;
  for (; j < blen; j++)
  {
    size_t u = 0;
    if (parseUnsignedApp(p + j, blen - j, vendor, u))
    {
      vendorOk = true;
      break;
    }
  }
  if (!vendorOk) return false;

  out.ok = true;
  out.device_id = inst;
  out.vendor_id = (uint16_t)(vendor & 0xFFFF);
  dbg(String("RX I-Am dev=") + String(inst) + " vendor=" + String((uint16_t)(vendor & 0xFFFF)) + " maxAPDU=" + String(maxApdu) + " seg=" + String(seg));
  return true;
}

struct ValueResult
{
  bool ok = false;
  float number = NAN;
  uint32_t enum_value = 0;
  bool is_enum = false;
};

static bool parseApplicationValue(const uint8_t* p, size_t n, ValueResult& out, size_t* consumed = nullptr)
{
  if (n < 2) return false;
  uint8_t tag = p[0];
  if (tag & 0x08) return false; // must be application tag
  uint8_t appTag = (uint8_t)((tag >> 4) & 0x0F);
  uint8_t lvt = (uint8_t)(tag & 0x07);

  size_t len = lvt;
  size_t pos = 1;
  if (lvt == 5)
  {
    if (n < 2) return false;
    uint8_t ext = p[pos++];
    len = ext;
  }

  if (pos + len > n) return false;
  if (consumed) *consumed = pos + len;

  if (appTag == 4 && len == 4)
  {
    uint32_t v = u32BE(p + pos);
    float f;
    memcpy(&f, &v, sizeof(float));
    out.ok = true;
    out.number = f;
    return true;
  }

  if ((appTag == 2 || appTag == 9) && len >= 1 && len <= 4)
  {
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) v = (v << 8) | p[pos + i];
    out.ok = true;
    out.number = (float)v;
    out.enum_value = v;
    out.is_enum = (appTag == 9);
    return true;
  }

  if (appTag == 3 && len >= 1 && len <= 4)
  {
    int32_t v = 0;
    for (size_t i = 0; i < len; i++) v = (v << 8) | p[pos + i];
    // Sign extension
    int shift = (4 - (int)len) * 8;
    v = (v << shift) >> shift;
    out.ok = true;
    out.number = (float)v;
    return true;
  }

  return false;
}

static bool parseContextNumericValue(const uint8_t* p, size_t n, ValueResult& out, size_t* consumed = nullptr)
{
  if (!p || n < 2) return false;
  uint8_t tag = p[0];
  if (!(tag & 0x08)) return false; // context-specific only
  uint8_t lvt = (uint8_t)(tag & 0x07);
  if (lvt == 6 || lvt == 7) return false; // opening/closing tag

  size_t len = lvt;
  size_t pos = 1;
  if (lvt == 5)
  {
    if (n < 2) return false;
    len = p[pos++];
  }
  if (len < 1 || len > 4) return false;
  if (pos + len > n) return false;
  if (consumed) *consumed = pos + len;

  // Some BACnet devices encode numeric propertyValue using context-primitive tags.
  // Treat len=4 as REAL first, otherwise unsigned integer fallback.
  if (len == 4)
  {
    uint32_t raw = u32BE(p + pos);
    float f = NAN;
    memcpy(&f, &raw, sizeof(float));
    if (!isnan(f) && isfinite(f) && fabsf(f) < 1000000.0f)
    {
      out.ok = true;
      out.number = f;
      return true;
    }
  }

  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) v = (v << 8) | p[pos + i];
  out.ok = true;
  out.number = (float)v;
  out.enum_value = v;
  out.is_enum = false;
  return true;
}

static bool packetInvokeInfo(const uint8_t* p, size_t n, uint8_t invoke, uint8_t* apduOut = nullptr, uint8_t* serviceOut = nullptr)
{
  size_t apduPos = 0, blen = 0;
  if (!locateApdu(p, n, apduPos, blen)) return false;
  size_t i = apduPos;
  if (i >= blen) return false;
  uint8_t apdu = p[i++];
  if (apduOut) *apduOut = apdu;

  auto setSvc = [&](uint8_t s) {
    if (serviceOut) *serviceOut = s;
  };

  // Simple-ACK / Error / Reject / Abort share invoke as first byte after APDU.
  if ((apdu & 0xF0) == 0x20 || (apdu & 0xF0) == 0x50 || (apdu & 0xF0) == 0x60 || (apdu & 0xF0) == 0x70)
  {
    if (i + 2 > blen) return false;
    uint8_t gotInvoke = p[i++];
    uint8_t serviceOrReason = p[i++];
    setSvc(serviceOrReason);
    return gotInvoke == invoke;
  }

  // Complex-ACK: segmented variant includes seq+window before service.
  if ((apdu & 0xF0) == 0x30)
  {
    if (apdu & 0x08)
    {
      if (i + 4 > blen) return false;
      uint8_t gotInvoke = p[i++];
      (void)p[i++]; // seq
      (void)p[i++]; // window
      uint8_t service = p[i++];
      setSvc(service);
      return gotInvoke == invoke;
    }
    if (i + 2 > blen) return false;
    uint8_t gotInvoke = p[i++];
    uint8_t service = p[i++];
    setSvc(service);
    return gotInvoke == invoke;
  }

  return false;
}

static bool parseReadPropertyAckValue(const uint8_t* p, size_t n, uint8_t invoke, ValueResult& out)
{
  if (!p || n < 12) return false;
  size_t apduPos = 0, blen = 0;
  if (!locateApdu(p, n, apduPos, blen)) return false;
  size_t i = apduPos;
  if (i + 3 > blen) return false;
  uint8_t apdu = p[i++];
  if ((apdu & 0xF0) != 0x30)
  {
    // 0x50 = Error-PDU (common for unknown object/property/read-access-denied)
    if ((apdu & 0xF0) == 0x50)
    {
      if (i + 2 <= blen)
      {
        uint8_t gotInvoke2 = p[i++];
        uint8_t service = p[i++];
        if (gotInvoke2 == invoke)
        {
          uint32_t errClass = 0, errCode = 0;
          size_t used = 0;
          if (parseEnumApp(p + i, blen - i, errClass, used))
          {
            i += used;
            if (parseEnumApp(p + i, blen - i, errCode, used))
            {
              setErr("DATA", String("BACnet Error-PDU class=") + errClass + " code=" + errCode + " service=" + service);
              return false;
            }
          }
          setErr("DATA", String("BACnet Error-PDU for invoke ") + invoke + " service=" + service);
          return false;
        }
      }
    }
    dbg(String("RX non-ACK APDU type=0x") + String(apdu, HEX) + " for invoke " + String(invoke));
    return false;
  }
  uint8_t gotInvoke = p[i++];
  if (gotInvoke != invoke) return false;
  uint8_t service = p[i++];
  if (service != 0x0C) return false; // ReadProperty-ACK

  // Tolerant parse:
  // ReadProperty-ACK can include optional array index and different tag-length encodings.
  // Find context opening tag 3 (propertyValue), then parse first application value inside.
  size_t open3 = (size_t)-1;
  for (size_t j = i; j < blen; j++)
  {
    if (p[j] == 0x3E) { open3 = j; break; }
  }
  if (open3 != (size_t)-1)
  {
    // Do NOT scan for first raw 0x3F byte as closing-tag; 0x3F can appear inside REAL payload bytes.
    // Instead, parse value directly after opening tag and accept first valid numeric token.
    if (open3 + 1 < blen)
    {
      size_t used = 0;
      if (parseApplicationValue(p + open3 + 1, blen - (open3 + 1), out, &used) && out.ok)
        return true;
      if (parseContextNumericValue(p + open3 + 1, blen - (open3 + 1), out, &used) && out.ok)
        return true;
    }
    return false;
  }

  // Fallback: some devices return payload where application value is directly parseable.
  size_t j = i;
  while (j < blen)
  {
    size_t used = 0;
    if (parseApplicationValue(p + j, blen - j, out, &used) && out.ok)
      return true;
    if (parseContextNumericValue(p + j, blen - j, out, &used) && out.ok)
      return true;
    if (used == 0) used = 1;
    j += used;
  }
  return false;
}

// Return: 1=success, -1=definitive failure for this invoke, 0=not relevant/keep waiting.
static int parseWritePropertyResult(const uint8_t* p, size_t n, uint8_t invoke, String& errOut)
{
  errOut = "";
  // Minimal BACnet/IP Simple-ACK can be 9 bytes:
  // BVLC(4) + NPDU(2) + APDU(3)
  if (!p || n < 9) return 0;
  size_t apduPos = 0, blen = 0;
  if (!locateApdu(p, n, apduPos, blen)) return 0;
  size_t i = apduPos;
  if (i >= blen) return 0;
  uint8_t apdu = p[i++];

  // Simple-ACK
  if ((apdu & 0xF0) == 0x20)
  {
    if (i + 2 > blen) return 0;
    uint8_t gotInvoke = p[i++];
    uint8_t service = p[i++];
    if (service == 0x0F)
    {
      // Some devices appear to reply with a valid Simple-ACK but odd invoke-id.
      // We accept this as success because writes are serialized on this UDP socket.
      if (gotInvoke != invoke)
      {
        dbg(String("WRITE ACK invoke mismatch accepted got=") + gotInvoke + " expected=" + invoke);
      }
      return 1;
    }
    if (gotInvoke != invoke) return 0;
    errOut = String("Simple-ACK for unexpected service=") + service;
    return -1;
  }

  // Error-PDU
  if ((apdu & 0xF0) == 0x50)
  {
    if (i + 2 > blen) return 0;
    uint8_t gotInvoke = p[i++];
    uint8_t service = p[i++];
    if (gotInvoke != invoke) return 0;
    if (service != 0x0F)
    {
      errOut = String("BACnet Error-PDU for service=") + service + " (expected 15)";
      return -1;
    }

    uint32_t errClass = 0, errCode = 0;
    size_t used = 0;
    if (parseEnumApp(p + i, blen - i, errClass, used))
    {
      i += used;
      if (parseEnumApp(p + i, blen - i, errCode, used))
      {
        errOut = String("BACnet Error-PDU class=") + errClass + " code=" + errCode + " service=" + service;
        return -1;
      }
    }
    errOut = String("BACnet Error-PDU service=") + service;
    return -1;
  }

  // Reject-PDU
  if ((apdu & 0xF0) == 0x60)
  {
    if (i + 2 > blen) return 0;
    uint8_t gotInvoke = p[i++];
    uint8_t reason = p[i++];
    if (gotInvoke != invoke) return 0;
    errOut = String("BACnet Reject-PDU reason=") + reason + " service=15";
    return -1;
  }

  // Abort-PDU
  if ((apdu & 0xF0) == 0x70)
  {
    if (i + 2 > blen) return 0;
    uint8_t gotInvoke = p[i++];
    uint8_t reason = p[i++];
    if (gotInvoke != invoke) return 0;
    errOut = String("BACnet Abort-PDU reason=") + reason + " service=15";
    return -1;
  }

  return 0;
}

String flexit_bacnet_debug_dump_text()
{
  if (!g_debug_enabled) return "Debug logging is disabled.";
  return g_debug_log;
}

void flexit_bacnet_debug_clear()
{
  g_debug_log = "";
}

void flexit_bacnet_debug_set_enabled(bool enabled)
{
  g_debug_enabled = enabled;
  if (!g_debug_enabled) g_debug_log = "";
}

bool flexit_bacnet_debug_is_enabled()
{
  return g_debug_enabled;
}

static String mapModeFromEnum(uint32_t v)
{
  String map = g_cfg.bacnet_mode_map;
  int pos = 0;
  while (pos < (int)map.length())
  {
    int comma = map.indexOf(',', pos);
    if (comma < 0) comma = map.length();
    String part = map.substring(pos, comma);
    int col = part.indexOf(':');
    if (col > 0)
    {
      String k = trimCopy(part.substring(0, col));
      String val = trimCopy(part.substring(col + 1));
      uint32_t key = 0;
      if (parseUint(k, key) && key == v && val.length() > 0) return val;
    }
    pos = comma + 1;
  }
  return String(v);
}

static bool mapModeToEnum(const String& modeIn, uint32_t& outEnum)
{
  String wanted = trimCopy(modeIn);
  wanted.toUpperCase();
  if (wanted.length() == 0) return false;

  String map = g_cfg.bacnet_mode_map;
  int pos = 0;
  while (pos < (int)map.length())
  {
    int comma = map.indexOf(',', pos);
    if (comma < 0) comma = map.length();
    String part = map.substring(pos, comma);
    int col = part.indexOf(':');
    if (col > 0)
    {
      String k = trimCopy(part.substring(0, col));
      String val = trimCopy(part.substring(col + 1));
      val.toUpperCase();
      uint32_t key = 0;
      if (val == wanted && parseUint(k, key))
      {
        outEnum = key;
        return true;
      }
    }
    pos = comma + 1;
  }
  return false;
}

static bool readOneNumeric(WiFiUDP& udp, const IPAddress& ip, uint16_t port,
                           const ObjRef& ref, float& out)
{
  if (!ref.valid)
  {
    setErr("CFG", "bad object mapping");
    return false;
  }

  uint8_t invoke = g_invoke++;
  if (!sendReadProperty(udp, ip, port, invoke, ref.type, ref.instance, 85))
  {
    setErr("NET", "send failed");
    return false;
  }

  const uint32_t t0 = millis();
  const uint16_t tout = g_cfg.bacnet_timeout_ms;
  bool gotFromTarget = false;
  bool gotMatchingInvoke = false;
  while ((uint32_t)(millis() - t0) < tout)
  {
    int len = udp.parsePacket();
    if (len <= 0) { delay(2); continue; }
    uint8_t buf[300];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int rd = udp.read(buf, len);
    if (rd <= 0) continue;
    dbg(String("RX(num) from ") + udp.remoteIP().toString() + ":" + String(udp.remotePort()) + " len=" + String(rd));
    if (udp.remoteIP() != ip) continue;
    gotFromTarget = true;

    ValueResult vr;
    if (parseReadPropertyAckValue(buf, (size_t)rd, invoke, vr))
    {
      out = vr.number;
      return true;
    }
    uint8_t apdu = 0, svc = 0;
    if (packetInvokeInfo(buf, (size_t)rd, invoke, &apdu, &svc))
    {
      gotMatchingInvoke = true;
      dbg(String("RX(num) invoke-match not parsed apdu=0x") + String(apdu, HEX) + " svc=" + String(svc));
      if (g_debug_enabled) dbg(String("RX(num) hex: ") + hexDump(buf, (size_t)rd));
    }
    if (g_last_error.startsWith("DATA: BACnet Error-PDU"))
      return false;
  }

  if (gotMatchingInvoke)
  {
    setErr("DATA", "reply for invoke not understood");
    return false;
  }
  if (gotFromTarget)
  {
    setErr("DATA", "reply received but not understood (object/property mismatch)");
    return false;
  }
  setErr("DATA", "read timeout");
  return false;
}

static bool readOneEnum(WiFiUDP& udp, const IPAddress& ip, uint16_t port,
                        const ObjRef& ref, uint32_t& outEnum)
{
  if (!ref.valid)
  {
    setErr("CFG", "bad mode object mapping");
    return false;
  }

  uint8_t invoke = g_invoke++;
  if (!sendReadProperty(udp, ip, port, invoke, ref.type, ref.instance, 85))
  {
    setErr("NET", "send failed");
    return false;
  }

  const uint32_t t0 = millis();
  const uint16_t tout = g_cfg.bacnet_timeout_ms;
  bool gotFromTarget = false;
  bool gotMatchingInvoke = false;
  while ((uint32_t)(millis() - t0) < tout)
  {
    int len = udp.parsePacket();
    if (len <= 0) { delay(2); continue; }
    uint8_t buf[300];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int rd = udp.read(buf, len);
    if (rd <= 0) continue;
    dbg(String("RX(enum) from ") + udp.remoteIP().toString() + ":" + String(udp.remotePort()) + " len=" + String(rd));
    if (udp.remoteIP() != ip) continue;
    gotFromTarget = true;

    ValueResult vr;
    if (parseReadPropertyAckValue(buf, (size_t)rd, invoke, vr))
    {
      outEnum = vr.is_enum ? vr.enum_value : (uint32_t)lroundf(vr.number);
      return true;
    }
    uint8_t apdu = 0, svc = 0;
    if (packetInvokeInfo(buf, (size_t)rd, invoke, &apdu, &svc))
    {
      gotMatchingInvoke = true;
      dbg(String("RX(enum) invoke-match not parsed apdu=0x") + String(apdu, HEX) + " svc=" + String(svc));
      if (g_debug_enabled) dbg(String("RX(enum) hex: ") + hexDump(buf, (size_t)rd));
    }
    if (g_last_error.startsWith("DATA: BACnet Error-PDU"))
      return false;
  }

  if (gotMatchingInvoke)
  {
    setErr("DATA", "mode reply for invoke not understood");
    return false;
  }
  if (gotFromTarget)
  {
    setErr("DATA", "mode reply received but not understood (object/property mismatch)");
    return false;
  }
  setErr("DATA", "mode read timeout");
  return false;
}

static bool readDeviceVendorId(WiFiUDP& udp, const IPAddress& ip, uint16_t port,
                               uint32_t deviceId, uint32_t& outVendor)
{
  // Device object (type 8), property vendor-identifier (120)
  uint8_t invoke = g_invoke++;
  if (!sendReadProperty(udp, ip, port, invoke, 8, deviceId, 120))
  {
    setErr("NET", "send vendor-id read failed");
    return false;
  }

  const uint32_t t0 = millis();
  const uint16_t tout = g_cfg.bacnet_timeout_ms;
  bool gotFromTarget = false;
  bool gotMatchingInvoke = false;
  while ((uint32_t)(millis() - t0) < tout)
  {
    int len = udp.parsePacket();
    if (len <= 0) { delay(2); continue; }
    uint8_t buf[300];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int rd = udp.read(buf, len);
    if (rd <= 0) continue;
    dbg(String("RX(vendor) from ") + udp.remoteIP().toString() + ":" + String(udp.remotePort()) + " len=" + String(rd));
    if (udp.remoteIP() != ip) continue;
    gotFromTarget = true;

    ValueResult vr;
    if (parseReadPropertyAckValue(buf, (size_t)rd, invoke, vr))
    {
      outVendor = (uint32_t)lroundf(vr.number);
      return true;
    }
    if (g_last_error.startsWith("DATA: BACnet Error-PDU"))
      return false;
  }

  if (gotFromTarget)
  {
    setErr("AUTH", "device replied but vendor-id parse failed");
    return false;
  }
  setErr("AUTH", "no response to vendor-id read");
  return false;
}

static bool waitForIAm(WiFiUDP& udp, const IPAddress& expectedIp, uint32_t expectedDeviceId, uint16_t timeoutMs)
{
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < timeoutMs)
  {
    int len = udp.parsePacket();
    if (len <= 0) { delay(2); continue; }
    uint8_t buf[256];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int rd = udp.read(buf, len);
    if (rd <= 0) continue;
    dbg(String("RX(IAm?) from ") + udp.remoteIP().toString() + ":" + String(udp.remotePort()) + " len=" + String(rd));

    IAmInfo ia;
    if (!parseIAm(buf, (size_t)rd, ia))
    {
      dbg(String("RX(IAm?) hex: ") + hexDump(buf, (size_t)rd));
      dbg("RX(IAm?) packet not parsed as I-Am");
      continue;
    }
    IPAddress rip = udp.remoteIP();
    if (rip != expectedIp) continue;
    if (expectedDeviceId != 0 && ia.device_id != expectedDeviceId)
    {
      setErr("AUTH", String("device-id mismatch, got ") + ia.device_id);
      return false;
    }
    return true;
  }
  return false;
}

static bool probeTarget(WiFiUDP& udp, const IPAddress& ip, uint16_t port, uint32_t expectedDeviceId)
{
  // Many BACnet stacks answer Who-Is only on broadcast.
  // Try unicast first, then subnet broadcast fallback.
  if (!sendWhoIs(udp, ip, port, false))
  {
    setErr("NET", "who-is send failed");
    return false;
  }
  if (waitForIAm(udp, ip, expectedDeviceId, g_cfg.bacnet_timeout_ms))
    return true;

  IPAddress local = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress bcast((uint32_t)local | ~((uint32_t)mask));
  if (!sendWhoIs(udp, bcast, port, true))
  {
    setErr("NET", "who-is broadcast failed");
    return false;
  }
  if (waitForIAm(udp, ip, expectedDeviceId, g_cfg.bacnet_timeout_ms))
    return true;

  setErr("AUTH", "no I-Am from target (unicast+broadcast)");
  return false;
}

void flexit_bacnet_set_runtime_config(const DeviceConfig& cfg)
{
  g_cfg = cfg;
}

bool flexit_bacnet_is_ready()
{
  return trimCopy(g_cfg.bacnet_ip).length() > 0 &&
         g_cfg.bacnet_port > 0 &&
         g_cfg.bacnet_device_id > 0;
}

bool flexit_bacnet_test(FlexitData* outData, String* reason)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    setErr("NET", "no WiFi");
    if (reason) *reason = g_last_error;
    return false;
  }
  if (!flexit_bacnet_is_ready())
  {
    setErr("CFG", "missing IP/device-id");
    if (reason) *reason = g_last_error;
    return false;
  }

  IPAddress ip;
  if (!parseIP(g_cfg.bacnet_ip, ip))
  {
    setErr("CFG", "bad BACnet IP");
    if (reason) *reason = g_last_error;
    return false;
  }

  ObjRef oOut = parseObjRef(g_cfg.bacnet_obj_outdoor);
  ObjRef oSup = parseObjRef(g_cfg.bacnet_obj_supply);
  ObjRef oExt = parseObjRef(g_cfg.bacnet_obj_extract);
  ObjRef oExh = parseObjRef(g_cfg.bacnet_obj_exhaust);
  ObjRef oFan = parseObjRef(g_cfg.bacnet_obj_fan);
  ObjRef oHeat = parseObjRef(g_cfg.bacnet_obj_heat);
  ObjRef oMode = parseObjRef(g_cfg.bacnet_obj_mode);
  ObjRef oSetHome = parseObjRef(g_cfg.bacnet_obj_setpoint_home);
  ObjRef oSetAway = parseObjRef(g_cfg.bacnet_obj_setpoint_away);
  if (!oOut.valid || !oSup.valid || !oExt.valid || !oExh.valid || !oFan.valid || !oHeat.valid || !oMode.valid)
  {
    setErr("CFG", "one or more BACnet object mappings invalid");
    if (reason) *reason = g_last_error;
    return false;
  }

  WiFiUDP udp;
  if (!udp.begin(g_cfg.bacnet_port))
  {
    setErr("NET", "udp begin failed");
    if (reason) *reason = g_last_error;
    return false;
  }

  // Probe with Who-Is/I-Am, but allow data-read fallback if unit does not answer discovery.
  bool discovered = probeTarget(udp, ip, g_cfg.bacnet_port, g_cfg.bacnet_device_id);
  String probeErr = g_last_error;

  uint32_t vendorId = 0;
  if (!readDeviceVendorId(udp, ip, g_cfg.bacnet_port, g_cfg.bacnet_device_id, vendorId))
  {
    udp.stop();
    if (reason) *reason = g_last_error;
    return false;
  }

  FlexitData t;
  bool gotOut = readOneNumeric(udp, ip, g_cfg.bacnet_port, oOut, t.uteluft);
  bool gotSup = readOneNumeric(udp, ip, g_cfg.bacnet_port, oSup, t.tilluft);
  bool gotExt = readOneNumeric(udp, ip, g_cfg.bacnet_port, oExt, t.avtrekk);
  bool gotExh = readOneNumeric(udp, ip, g_cfg.bacnet_port, oExh, t.avkast);
  if (!gotExh)
  {
    // Keep exhaust fallback intentionally small and predictable.
    const char* exFallback[] = {"ai:11", "ai:60", "ai:61", "av:130"};
    for (size_t i = 0; i < (sizeof(exFallback) / sizeof(exFallback[0])); i++)
    {
      if (String(exFallback[i]) == g_cfg.bacnet_obj_exhaust) continue;
      ObjRef alt = parseObjRef(String(exFallback[i]));
      if (!alt.valid) continue;
      if (readOneNumeric(udp, ip, g_cfg.bacnet_port, alt, t.avkast))
      {
        gotExh = true;
        dbg(String("EXHAUST fallback hit: ") + exFallback[i]);
        break;
      }
      // keep trying candidates even if one gave parse/protocol error
      g_last_error = "OK";
    }
  }
  float fan = 0.0f;
  float heat = 0.0f;
  bool gotFan = readOneNumeric(udp, ip, g_cfg.bacnet_port, oFan, fan);
  bool gotHeat = readOneNumeric(udp, ip, g_cfg.bacnet_port, oHeat, heat);
  if (gotFan) t.fan_percent = (int)lroundf(fan);
  if (gotHeat) t.heat_element_percent = (int)lroundf(heat);

  uint32_t modeEnum = 0;
  bool gotMode = readOneEnum(udp, ip, g_cfg.bacnet_port, oMode, modeEnum);
  t.mode = gotMode ? mapModeFromEnum(modeEnum) : "N/A";
  if (gotMode)
  {
    dbg(String("MODE read obj=") + g_cfg.bacnet_obj_mode + " enum=" + String(modeEnum) + " mapped=" + t.mode);
  }
  else
  {
    dbg(String("MODE read failed for obj=") + g_cfg.bacnet_obj_mode + " err=" + g_last_error);
  }

  // Optional setpoint reads (best-effort); keep main BACnet poll green even if missing.
  float spHome = NAN;
  float spAway = NAN;
  if (oSetHome.valid)
  {
    if (!readOneNumeric(udp, ip, g_cfg.bacnet_port, oSetHome, spHome))
    {
      spHome = NAN;
      g_last_error = "OK";
    }
  }
  if (oSetAway.valid)
  {
    if (!readOneNumeric(udp, ip, g_cfg.bacnet_port, oSetAway, spAway))
    {
      spAway = NAN;
      g_last_error = "OK";
    }
  }
  // Keep dashboard set-temp stable: HOME setpoint first, AWAY as fallback.
  t.set_temp = !isnan(spHome) ? spHome : spAway;

  udp.stop();

  // Reuse previously known values for fields that failed in this cycle.
  if (g_has_last_partial_data)
  {
    if (!gotOut) t.uteluft = g_last_partial_data.uteluft;
    if (!gotSup) t.tilluft = g_last_partial_data.tilluft;
    if (!gotExt) t.avtrekk = g_last_partial_data.avtrekk;
    if (!gotExh) t.avkast = g_last_partial_data.avkast;
    if (!gotFan) t.fan_percent = g_last_partial_data.fan_percent;
    if (!gotHeat) t.heat_element_percent = g_last_partial_data.heat_element_percent;
    if (!gotMode) t.mode = g_last_partial_data.mode;
    if (isnan(t.set_temp)) t.set_temp = g_last_partial_data.set_temp;
  }

  // Accept partial data to avoid dashboard lockups on transient object failures.
  // Require at least supply + fan + one other temperature.
  const bool ok = gotSup && gotFan && (gotOut || gotExt || gotExh);
  if (!ok)
  {
    if (!discovered && probeErr.length() > 0)
      setErr("AUTH", stripStagePrefix(probeErr) + String(" + DATA: ") + stripStagePrefix(g_last_error));
    if (reason) *reason = g_last_error;
    return false;
  }

  g_last_partial_data = t;
  g_has_last_partial_data = true;
  g_last_error = "OK";
  if (outData) *outData = t;
  return true;
}

bool flexit_bacnet_poll(FlexitData& out)
{
  String why;
  FlexitData t;
  if (!flexit_bacnet_test(&t, &why))
    return false;
  out.uteluft = t.uteluft;
  out.tilluft = t.tilluft;
  out.avtrekk = t.avtrekk;
  out.avkast = t.avkast;
  out.fan_percent = t.fan_percent;
  out.heat_element_percent = t.heat_element_percent;
  out.mode = t.mode;
  out.set_temp = t.set_temp;
  return true;
}

const char* flexit_bacnet_last_error()
{
  return g_last_error.c_str();
}

static bool writeOneModeEnum(WiFiUDP& udp, const IPAddress& ip, uint16_t port, const ObjRef& ref, uint32_t modeValue)
{
  if (!ref.valid)
  {
    setErr("CFG", "bad mode object mapping");
    return false;
  }

  uint8_t invoke = g_invoke++;
  if (!sendWritePropertyEnum(udp, ip, port, invoke, ref.type, ref.instance, 85, modeValue))
  {
    setErr("NET", "send mode write failed");
    return false;
  }

  const uint32_t t0 = millis();
  const uint16_t tout = g_cfg.bacnet_timeout_ms;
  bool gotFromTarget = false;
  bool gotMatchingInvoke = false;
  while ((uint32_t)(millis() - t0) < tout)
  {
    int len = udp.parsePacket();
    if (len <= 0) { delay(2); continue; }
    uint8_t buf[300];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int rd = udp.read(buf, len);
    if (rd <= 0) continue;
    if (udp.remoteIP() != ip) continue;
    gotFromTarget = true;
    if (g_debug_enabled)
    {
      dbg(String("RX(write-mode) from ") + udp.remoteIP().toString() + ":" + String(udp.remotePort()) + " len=" + String(rd));
      dbg(String("RX(write-mode) hex: ") + hexDump(buf, (size_t)rd));
    }

    String err;
    int wr = parseWritePropertyResult(buf, (size_t)rd, invoke, err);
    if (wr == 1) return true;
    if (wr == -1)
    {
      setErr("WRITE", err);
      return false;
    }
    if (err.length() > 0) gotMatchingInvoke = true;
  }

  if (gotMatchingInvoke) { setErr("WRITE", "mode write reply for invoke not understood"); return false; }
  if (gotFromTarget) { setErr("WRITE", "mode write got unrelated BACnet traffic only"); return false; }
  setErr("WRITE", "mode write timeout");
  return false;
}

static bool writeOneRealAttempt(WiFiUDP& udp, const IPAddress& ip, uint16_t port,
                                const ObjRef& ref, float value, int priority, String& outErr)
{
  outErr = "";
  if (!ref.valid) { outErr = "bad setpoint object mapping"; return false; }

  uint8_t invoke = g_invoke++;
  if (!sendWritePropertyReal(udp, ip, port, invoke, ref.type, ref.instance, 85, value, priority))
  {
    outErr = "send setpoint write failed";
    return false;
  }

  const uint32_t t0 = millis();
  const uint16_t tout = g_cfg.bacnet_timeout_ms;
  bool gotFromTarget = false;
  bool gotMatchingInvoke = false;
  while ((uint32_t)(millis() - t0) < tout)
  {
    int len = udp.parsePacket();
    if (len <= 0) { delay(2); continue; }
    uint8_t buf[300];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int rd = udp.read(buf, len);
    if (rd <= 0) continue;
    if (udp.remoteIP() != ip) continue;
    gotFromTarget = true;
    if (g_debug_enabled)
    {
      dbg(String("RX(write-setpoint) from ") + udp.remoteIP().toString() + ":" + String(udp.remotePort()) + " len=" + String(rd));
      dbg(String("RX(write-setpoint) hex: ") + hexDump(buf, (size_t)rd));
    }

    String err;
    int wr = parseWritePropertyResult(buf, (size_t)rd, invoke, err);
    if (wr == 1) return true;
    if (wr == -1)
    {
      outErr = err;
      return false;
    }
    if (err.length() > 0) gotMatchingInvoke = true;
  }

  if (gotMatchingInvoke) { outErr = "setpoint write reply for invoke not understood"; return false; }
  if (gotFromTarget) { outErr = "setpoint write got unrelated BACnet traffic only"; return false; }
  outErr = "setpoint write timeout";
  return false;
}

static bool writeOneReal(WiFiUDP& udp, const IPAddress& ip, uint16_t port, const ObjRef& ref, float value)
{
  if (!ref.valid)
  {
    setErr("CFG", "bad setpoint object mapping");
    return false;
  }

  String err;
  if (writeOneRealAttempt(udp, ip, port, ref, value, -1, err)) return true;
  String firstErr = err;
  String lastErr = err;

  // Flexit variants may require priority on WriteProperty, or expose writable sibling object type.
  const bool accessDenied = (firstErr.indexOf("class=2 code=40") >= 0);
  if (accessDenied)
  {
    dbg("WRITE setpoint fallback: retry with priority=16");
    if (writeOneRealAttempt(udp, ip, port, ref, value, 16, err)) return true;
    lastErr = err;

    if (ref.type == 2 || ref.type == 1) // av <-> ao sibling fallback
    {
      ObjRef alt = ref;
      alt.type = (ref.type == 2) ? 1 : 2;
      dbg(String("WRITE setpoint fallback: retry alt object type ") + String(alt.type) + ":" + String(alt.instance));
      if (writeOneRealAttempt(udp, ip, port, alt, value, 16, err)) return true;
      lastErr = err;
      if (writeOneRealAttempt(udp, ip, port, alt, value, -1, err)) return true;
      lastErr = err;
    }
  }

  if (lastErr.length() > 0 && lastErr != firstErr)
    setErr("WRITE", firstErr + String(" | fallback: ") + lastErr);
  else
    setErr("WRITE", firstErr);
  return false;
}

bool flexit_bacnet_write_mode(const String& modeCmd)
{
  if (WiFi.status() != WL_CONNECTED) { setErr("NET", "no WiFi"); return false; }
  if (!flexit_bacnet_is_ready()) { setErr("CFG", "missing IP/device-id"); return false; }
  if (!g_cfg.bacnet_write_enabled) { setErr("CFG", "bacnet write disabled"); return false; }

  uint32_t enumValue = 0;
  if (!mapModeToEnum(modeCmd, enumValue))
  {
    setErr("CFG", "mode not in BACnet mode map");
    return false;
  }

  IPAddress ip;
  if (!parseIP(g_cfg.bacnet_ip, ip)) { setErr("CFG", "bad BACnet IP"); return false; }

  ObjRef modeObj = parseObjRef(g_cfg.bacnet_obj_mode);
  WiFiUDP udp;
  if (!udp.begin(0)) { setErr("NET", "udp begin failed"); return false; }
  bool ok = writeOneModeEnum(udp, ip, g_cfg.bacnet_port, modeObj, enumValue);
  udp.stop();
  if (ok) g_last_error = "OK";
  return ok;
}

bool flexit_bacnet_write_setpoint(const String& profile, float value)
{
  if (WiFi.status() != WL_CONNECTED) { setErr("NET", "no WiFi"); return false; }
  if (!flexit_bacnet_is_ready()) { setErr("CFG", "missing IP/device-id"); return false; }
  if (!g_cfg.bacnet_write_enabled) { setErr("CFG", "bacnet write disabled"); return false; }
  if (value < 10.0f || value > 30.0f) { setErr("CFG", "setpoint range"); return false; }

  String p = profile;
  p.toLowerCase();
  String objSpec;
  if (p == "home") objSpec = g_cfg.bacnet_obj_setpoint_home;
  else if (p == "away") objSpec = g_cfg.bacnet_obj_setpoint_away;
  else { setErr("CFG", "setpoint profile"); return false; }

  IPAddress ip;
  if (!parseIP(g_cfg.bacnet_ip, ip)) { setErr("CFG", "bad BACnet IP"); return false; }
  ObjRef setpObj = parseObjRef(objSpec);

  WiFiUDP udp;
  if (!udp.begin(0)) { setErr("NET", "udp begin failed"); return false; }
  bool ok = writeOneReal(udp, ip, g_cfg.bacnet_port, setpObj, value);
  udp.stop();
  if (ok) g_last_error = "OK";
  return ok;
}

String flexit_bacnet_autodiscover_json(uint16_t wait_ms)
{
  String out = "[]";

  if (WiFi.status() != WL_CONNECTED)
  {
    g_last_error = "NET: no WiFi";
    return out;
  }

  WiFiUDP udp;
  if (!udp.begin(0))
  {
    g_last_error = "NET: udp begin failed";
    return out;
  }

  IPAddress local = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress bcast((uint32_t)local | ~((uint32_t)mask));

  bool sentAny = false;
  const uint16_t p1 = 47808;
  const uint16_t p2 = (g_cfg.bacnet_port > 0) ? g_cfg.bacnet_port : 47808;
  if (sendWhoIs(udp, bcast, p1, true)) sentAny = true;
  if (p2 != p1 && sendWhoIs(udp, bcast, p2, true)) sentAny = true;

  if (!sentAny)
  {
    udp.stop();
    g_last_error = "NET: who-is broadcast failed";
    return out;
  }

  struct Hit { IPAddress ip; uint16_t port; uint32_t dev; uint16_t vendor; };
  Hit hits[8];
  int hitCount = 0;

  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < wait_ms)
  {
    int len = udp.parsePacket();
    if (len <= 0) { delay(2); continue; }

    uint8_t buf[300];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int rd = udp.read(buf, len);
    if (rd <= 0) continue;
    dbg(String("RX(discover) from ") + udp.remoteIP().toString() + ":" + String(udp.remotePort()) + " len=" + String(rd));

    IAmInfo ia;
    if (!parseIAm(buf, (size_t)rd, ia))
    {
      dbg("RX(discover) packet not parsed as I-Am");
      continue;
    }
    ia.ip = udp.remoteIP();
    ia.port = udp.remotePort();

    bool exists = false;
    for (int i = 0; i < hitCount; i++)
    {
      if (hits[i].dev == ia.device_id || hits[i].ip == ia.ip)
      {
        exists = true;
        break;
      }
    }
    if (exists || hitCount >= 8) continue;
    hits[hitCount].ip = ia.ip;
    hits[hitCount].port = ia.port;
    hits[hitCount].dev = ia.device_id;
    hits[hitCount].vendor = ia.vendor_id;
    hitCount++;
  }

  udp.stop();

  out = "[";
  for (int i = 0; i < hitCount; i++)
  {
    if (i) out += ",";
    out += "{\"ip\":\"" + hits[i].ip.toString() + "\"";
    out += ",\"port\":" + String(hits[i].port);
    out += ",\"device_id\":" + String(hits[i].dev);
    out += ",\"vendor_id\":" + String(hits[i].vendor);
    out += "}";
  }
  out += "]";

  g_last_error = (hitCount > 0) ? "OK" : "DISCOVERY: no BACnet devices found";
  return out;
}

static String jsonEscapeLocal(const String& in)
{
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++)
  {
    char c = in[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

String flexit_bacnet_probe_configured_objects_json()
{
  String out = "[]";
  if (WiFi.status() != WL_CONNECTED)
  {
    g_last_error = "NET: no WiFi";
    return out;
  }

  IPAddress ip;
  if (!parseIP(g_cfg.bacnet_ip, ip))
  {
    g_last_error = "CFG: bad BACnet IP";
    return out;
  }
  if (g_cfg.bacnet_port == 0)
  {
    g_last_error = "CFG: bad BACnet port";
    return out;
  }
  if (g_cfg.bacnet_device_id == 0)
  {
    g_last_error = "CFG: missing BACnet device-id";
    return out;
  }

  struct ProbeItem
  {
    const char* key;
    String spec;
    bool expectEnum;
  };

  ProbeItem items[] = {
      {"outdoor", g_cfg.bacnet_obj_outdoor, false},
      {"supply",  g_cfg.bacnet_obj_supply,  false},
      {"extract", g_cfg.bacnet_obj_extract, false},
      {"exhaust", g_cfg.bacnet_obj_exhaust, false},
      {"fan",     g_cfg.bacnet_obj_fan,     false},
      {"heat",    g_cfg.bacnet_obj_heat,    false},
      {"mode",    g_cfg.bacnet_obj_mode,    true},
  };

  WiFiUDP udp;
  if (!udp.begin(0))
  {
    g_last_error = "NET: udp begin failed";
    return out;
  }

  uint32_t vendor = 0;
  bool vendorOk = readDeviceVendorId(udp, ip, g_cfg.bacnet_port, g_cfg.bacnet_device_id, vendor);
  if (!vendorOk)
  {
    dbg("Object probe continuing despite vendor-id read failure");
  }

  int okCount = 0;
  out = "[";
  for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++)
  {
    auto addTried = [&](String& bucket, const String& spec) {
      if (spec.length() == 0) return;
      if (bucket.length() > 0) bucket += ",";
      bucket += spec;
    };

    auto tryOne = [&](const String& spec, bool expectEnum, float& numOut, uint32_t& enumOut, String& errOut) -> bool {
      ObjRef ref = parseObjRef(spec);
      if (!ref.valid)
      {
        errOut = "invalid object spec";
        return false;
      }
      bool okLocal = false;
      if (expectEnum)
      {
        okLocal = readOneEnum(udp, ip, g_cfg.bacnet_port, ref, enumOut);
        if (okLocal) numOut = (float)enumOut;
      }
      else
      {
        okLocal = readOneNumeric(udp, ip, g_cfg.bacnet_port, ref, numOut);
      }
      if (!okLocal)
      {
        errOut = stripStagePrefix(g_last_error);
        if (errOut.length() == 0) errOut = "read failed";
      }
      return okLocal;
    };

    auto probeWithCandidates = [&](const ProbeItem& item, float& numOut, uint32_t& enumOut, String& resolvedSpec,
                                   String& triedCsv, String& errOut, bool& usedFallback) -> bool
    {
      resolvedSpec = "";
      triedCsv = "";
      errOut = "";
      usedFallback = false;

      // Candidate lists from observed Nordic S3 BACnet mappings.
      static const char* C_OUTDOOR[] = {"ai:1", "av:102", "av:104"};
      static const char* C_SUPPLY[]  = {"ai:4", "av:5", "ai:75", "av:100"};
      static const char* C_EXTRACT[] = {"ai:59", "av:131"};
      static const char* C_EXHAUST[] = {"ai:11", "ai:60", "ai:61", "av:130"};
      static const char* C_FAN[]     = {"ao:3", "ao:4", "av:56", "av:57"};
      static const char* C_HEAT[]    = {"ao:29", "ao:28", "av:58"};
      static const char* C_MODE[]    = {"msv:41", "msv:42", "msv:14", "msv:19", "av:0", "msv:60", "msv:61", "msv:62", "msv:63", "msv:64", "msv:65"};

      const char* const* cand = nullptr;
      size_t candN = 0;
      if (strcmp(item.key, "outdoor") == 0) { cand = C_OUTDOOR; candN = sizeof(C_OUTDOOR) / sizeof(C_OUTDOOR[0]); }
      else if (strcmp(item.key, "supply") == 0) { cand = C_SUPPLY; candN = sizeof(C_SUPPLY) / sizeof(C_SUPPLY[0]); }
      else if (strcmp(item.key, "extract") == 0) { cand = C_EXTRACT; candN = sizeof(C_EXTRACT) / sizeof(C_EXTRACT[0]); }
      else if (strcmp(item.key, "exhaust") == 0) { cand = C_EXHAUST; candN = sizeof(C_EXHAUST) / sizeof(C_EXHAUST[0]); }
      else if (strcmp(item.key, "fan") == 0) { cand = C_FAN; candN = sizeof(C_FAN) / sizeof(C_FAN[0]); }
      else if (strcmp(item.key, "heat") == 0) { cand = C_HEAT; candN = sizeof(C_HEAT) / sizeof(C_HEAT[0]); }
      else if (strcmp(item.key, "mode") == 0) { cand = C_MODE; candN = sizeof(C_MODE) / sizeof(C_MODE[0]); }

      // 1) Try configured spec first.
      if (item.spec.length() > 0)
      {
        addTried(triedCsv, item.spec);
        if (tryOne(item.spec, item.expectEnum, numOut, enumOut, errOut))
        {
          resolvedSpec = item.spec;
          return true;
        }
      }

      // 2) Try known candidates.
      for (size_t ci = 0; ci < candN; ci++)
      {
        String s = String(cand[ci]);
        if (s.length() == 0) continue;
        if (item.spec.length() > 0 && s == item.spec) continue;
        addTried(triedCsv, s);
        if (tryOne(s, item.expectEnum, numOut, enumOut, errOut))
        {
          resolvedSpec = s;
          usedFallback = true;
          return true;
        }
      }
      return false;
    };

    if (i) out += ",";
    out += "{\"key\":\"";
    out += items[i].key;
    out += "\",\"spec\":\"";
    out += jsonEscapeLocal(items[i].spec);
    out += "\"";

    bool ok = false;
    float n = NAN;
    uint32_t e = 0;
    String err;
    String resolved;
    String tried;
    bool fallback = false;
    ok = probeWithCandidates(items[i], n, e, resolved, tried, err, fallback);

    if (!ok)
    {
      if (err.length() == 0) err = "read failed";
      out += ",\"ok\":false,\"error\":\"" + jsonEscapeLocal(err) + "\"";
      if (tried.length() > 0) out += ",\"tried\":\"" + jsonEscapeLocal(tried) + "\"";
      out += "}";
      continue;
    }

    okCount++;
    out += ",\"resolved_spec\":\"" + jsonEscapeLocal(resolved) + "\"";
    out += ",\"source\":\"" + String(fallback ? "candidate" : "configured") + "\"";
    out += ",\"ok\":true,\"value\":";
    if (isnan(n))
    {
      out += "null";
    }
    else
    {
      char b[24];
      snprintf(b, sizeof(b), "%.2f", n);
      out += b;
    }
    if (items[i].expectEnum) out += ",\"enum\":" + String(e);
    if (tried.length() > 0) out += ",\"tried\":\"" + jsonEscapeLocal(tried) + "\"";
    out += "}";
  }
  out += "]";
  udp.stop();

  if (okCount > 0)
  {
    g_last_error = "OK";
  }
  else if (g_last_error == "OK")
  {
    g_last_error = "PROBE: no configured objects readable";
  }
  return out;
}

static const char* objTypeShortName(uint16_t t)
{
  switch (t)
  {
    case 0: return "ai";
    case 1: return "ao";
    case 2: return "av";
    case 3: return "bi";
    case 4: return "bo";
    case 5: return "bv";
    case 13: return "mi";
    case 14: return "mo";
    case 19: return "msv";
    default: return "";
  }
}

String flexit_bacnet_scan_objects_json(uint16_t inst_from, uint16_t inst_to,
                                       uint16_t timeout_ms, uint16_t max_hits,
                                       int16_t only_type)
{
  String out = "[]";
  if (WiFi.status() != WL_CONNECTED)
  {
    g_last_error = "NET: no WiFi";
    return out;
  }

  IPAddress ip;
  if (!parseIP(g_cfg.bacnet_ip, ip))
  {
    g_last_error = "CFG: bad BACnet IP";
    return out;
  }
  if (g_cfg.bacnet_port == 0)
  {
    g_last_error = "CFG: bad BACnet port";
    return out;
  }
  if (g_cfg.bacnet_device_id == 0)
  {
    g_last_error = "CFG: missing BACnet device-id";
    return out;
  }

  if (inst_from > inst_to)
  {
    uint16_t t = inst_from;
    inst_from = inst_to;
    inst_to = t;
  }
  if (inst_to > 512) inst_to = 512;
  if (timeout_ms < 250) timeout_ms = 250;
  if (timeout_ms > 2000) timeout_ms = 2000;
  if (max_hits < 1) max_hits = 1;
  if (max_hits > 400) max_hits = 400;

  WiFiUDP udp;
  if (!udp.begin(0))
  {
    g_last_error = "NET: udp begin failed";
    return out;
  }

  const uint16_t oldTimeout = g_cfg.bacnet_timeout_ms;
  g_cfg.bacnet_timeout_ms = timeout_ms;

  const uint16_t scanTypesAll[] = {0, 1, 2, 13, 14, 19};
  uint16_t hitCount = 0;
  uint16_t byTypeAI = 0, byTypeAO = 0, byTypeAV = 0, byTypeMSV = 0, byTypeOther = 0;
  const bool collectDebug = g_debug_enabled;
  String preview;
  String allValues;
  if (collectDebug)
  {
    dbg(String("Object scan start ip=") + ip.toString() + ":" + String(g_cfg.bacnet_port) +
        " inst=" + String(inst_from) + "-" + String(inst_to) +
        " timeout=" + String(timeout_ms) + " max=" + String(max_hits));
  }

  g_quiet_data_errors = true;
  out = "[";
  for (size_t t = 0; t < sizeof(scanTypesAll) / sizeof(scanTypesAll[0]); t++)
  {
    const uint16_t objType = scanTypesAll[t];
    if (only_type >= 0 && objType != (uint16_t)only_type) continue;
    for (uint16_t inst = inst_from; inst <= inst_to; inst++)
    {
      ObjRef ref;
      ref.type = objType;
      ref.instance = inst;
      ref.valid = true;

      float v = NAN;
      if (!readOneNumeric(udp, ip, g_cfg.bacnet_port, ref, v))
      {
        continue;
      }

      if (hitCount > 0) out += ",";
      const char* shortName = objTypeShortName(ref.type);
      out += "{\"obj\":\"";
      if (shortName && shortName[0])
      {
        out += shortName;
        out += ":";
        out += String(ref.instance);
      }
      else
      {
        out += String(ref.type);
        out += ":";
        out += String(ref.instance);
      }
      out += "\",\"type\":";
      out += String(ref.type);
      out += ",\"instance\":";
      out += String(ref.instance);
      out += ",\"value\":";
      if (isnan(v))
      {
        out += "null";
      }
      else
      {
        char b[24];
        snprintf(b, sizeof(b), "%.2f", v);
        out += b;
      }
      out += "}";

      hitCount++;
      if (ref.type == 0) byTypeAI++;
      else if (ref.type == 1) byTypeAO++;
      else if (ref.type == 2) byTypeAV++;
      else if (ref.type == 19) byTypeMSV++;
      else byTypeOther++;
      if (collectDebug && preview.length() < 280)
      {
        if (preview.length() > 0) preview += ", ";
        const char* sn = objTypeShortName(ref.type);
        preview += (sn && sn[0]) ? String(sn) : String(ref.type);
        preview += ":";
        preview += String(ref.instance);
        preview += "=";
        if (isnan(v)) preview += "null";
        else preview += String(v, 2);
      }
      if (collectDebug && allValues.length() < 12000)
      {
        if (allValues.length() > 0) allValues += ", ";
        const char* sn2 = objTypeShortName(ref.type);
        allValues += (sn2 && sn2[0]) ? String(sn2) : String(ref.type);
        allValues += ":";
        allValues += String(ref.instance);
        allValues += "=";
        if (isnan(v)) allValues += "null";
        else allValues += String(v, 2);
      }
      if (hitCount >= max_hits) break;
    }
    if (hitCount >= max_hits) break;
  }
  out += "]";
  g_quiet_data_errors = false;

  g_cfg.bacnet_timeout_ms = oldTimeout;
  udp.stop();

  if (collectDebug)
  {
    dbg(String("Object scan summary hits=") + String(hitCount) +
        " [ai=" + String(byTypeAI) +
        ", ao=" + String(byTypeAO) +
        ", av=" + String(byTypeAV) +
        ", msv=" + String(byTypeMSV) +
        ", other=" + String(byTypeOther) + "]");
    if (allValues.length() > 0) dbg(String("Object scan values: ") + allValues);
    if (preview.length() > 0) dbg(String("Object scan sample: ") + preview);
  }

  g_last_error = (hitCount > 0) ? "OK" : "SCAN: no readable objects found in range";
  return out;
}
