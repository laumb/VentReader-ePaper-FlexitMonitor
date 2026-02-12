#pragma once
#include <Arduino.h>
#include "flexit_types.h"
#include "config_store.h"

// =========================================================
// WEB PORTAL (Product Admin + Homey/HA API)
// ---------------------------------------------------------
// - GET  /health -> ok (no auth)
// - GET  /status[?pretty=1] -> JSON (Authorization: Bearer <token>)
// - GET  /status/history[?limit=120] -> JSON history buffer (Bearer)
// - GET  /status/history.csv[?limit=120] -> CSV history export (Bearer)
// - GET  /status/diag -> JSON diagnostics counters (Bearer)
// - GET  /status/storage -> JSON storage/heap status (Bearer)
// - GET  /ha/status[?pretty=1] -> JSON (Bearer + HA enabled)
// - GET  /ha/history[?limit=120] -> JSON history buffer (Bearer)
// - GET  /ha/history.csv[?limit=120] -> CSV history export (Bearer)
// - POST /api/control/mode?mode=AWAY|HOME|HIGH|FIRE (Bearer)
// - POST /api/control/setpoint?profile=home|away&value=18.5 (Bearer)
//   (control endpoints use active source:
//    - Modbus requires Modbus + control writes
//    - BACnet requires BACnet writes enabled)
// - POST /admin/api_emergency_stop (admin auth) toggles API kill switch
// - Admin UI (Basic Auth):
//     GET  /admin
//     GET  /admin/setup        (forced on first login until password changed)
//     POST /admin/setup_save
//     POST /admin/save         (normal settings)
//     POST /admin/reboot
//     POST /admin/factory_reset
//
// Fallback AP:
// - If STA WiFi can't connect, device starts AP:
//     SSID: Flexit-Setup-XXXX (XXXX = MAC suffix)
//     PASS: product AP password (default: ventreader)
//
// First boot onboarding:
// - API bearer tokens are auto-generated securely on first boot (stored in NVS).
// Default admin creds are same for all units (as requested):
//     user: admin
//     pass: ventreader
// - Setup wizard is pre-auth until setup_completed=true.
// - User is still forced to set new admin password in setup step 1.
//
// Factory reset:
// - Via admin UI button
// - Via BOOT button (GPIO0) hold at power-on (optional, implemented in .ino)
//
// =========================================================

void webportal_begin(DeviceConfig& cfg);
void webportal_loop();
void webportal_set_data(const FlexitData& data, const String& mbStatus);
bool webportal_consume_refresh_request();

// Helpers
bool webportal_sta_connected();
bool webportal_ap_active();
