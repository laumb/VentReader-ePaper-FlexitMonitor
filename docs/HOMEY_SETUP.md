# Homey Setup Guide (Step by Step)

Goal of this guide:
- Make Homey setup fast and predictable for end users
- Minimize manual copy/paste and mapping errors
- Ensure all required data comes from VentReader export

## Quick start (recommended)

1. Verify `Homey/API` is enabled in VentReader admin.
2. Verify data source is configured:
   - `Modbus (experimental, local)`, or
   - `BACnet (local)` with local BACnet config (IP + Device ID).
3. Click **Export Homey setup** in admin.
4. Mobile: use **Share file (mobile)** and send to your own email.
5. Desktop: download file directly.
6. Create Homey virtual devices based on export mapping.
7. Paste `homey_script_js` from export into HomeyScript.
8. Create polling flow every 1-2 minutes.

## Required vs optional

### Required
1. `Homey/API` enabled.
2. Correct Bearer token in endpoint/script.
3. Required virtual devices with correct capability IDs.
4. Polling flow that executes script periodically.

### Optional
1. Modbus alarm virtual device.
2. Status text virtual device.
3. Write control (mode/setpoint).

## Recommended default flow: VentReader export file

Use **Export Homey setup** in VentReader admin.

Export contains:
1. Base URL (`http://<ip>`)
2. API Bearer token
3. Ready HomeyScript template
4. Recommended virtual device/capability mapping
5. Polling flow notes
6. Optional control section (if writes enabled)

Distribution:
1. Mobile/tablet: share/send to email.
2. Desktop/laptop: download locally.

---

## 1) Prepare VentReader

In VentReader admin (`/admin`):
1. Verify `Homey/API` enabled.
2. Verify/update **Homey token (/status)**.
3. Confirm local IP/hostname.

Quick test:
- `GET http://<VENTREADER_IP>/status?pretty=1` with header `Authorization: Bearer <HOMEY_TOKEN>`

## 2) Export Homey setup file

1. Open VentReader admin.
2. Click **Export Homey setup**.
3. Share or download the file.

Expected content:
- `homey_script_js`
- virtual device mapping
- flow notes

## 3) Install required Homey apps

Install:
1. `HomeyScript`
2. A virtual devices app supporting numeric/temperature sensors with Insights

## 4) Create virtual devices in Homey

Minimum recommended:
1. `VentReader - Uteluft` (temperature)
2. `VentReader - Tilluft` (temperature)
3. `VentReader - Avtrekk` (temperature)
4. `VentReader - Avkast` (temperature)
5. `VentReader - Fan %` (percentage/number)
6. `VentReader - Heat %` (percentage/number)
7. `VentReader - Gjenvinning %` (percentage/number)

Optional:
1. `VentReader - Modbus Alarm`
2. `VentReader - Status tekst`

## 5) Import script from export file

1. Copy `homey_script_js` from export file.
2. Create HomeyScript, e.g. `VentReader Poll`.
3. Paste script as-is (avoid manual logic rewrites).

## 6) Create polling flow

1. `When`: every 1-2 minutes
2. `Then`: run `VentReader Poll`

Optional:
1. Push notification flow on script/alarm failure.

## 7) Verify operation

1. Virtual device values update.
2. Insights graphs build over time.
3. Alarm behavior works (if configured).

## 8) Optional control from Homey (mode + setpoint)

Requires in VentReader (one of):
1. Data source = `Modbus` + `Modbus` enabled + `Enable remote control writes (experimental)`
2. Data source = `BACnet` + `Enable BACnet writes (experimental)`

Endpoints:
1. `POST /api/control/mode` + header `Authorization: Bearer <TOKEN>` + `mode=AWAY|HOME|HIGH|FIRE`
2. `POST /api/control/setpoint` + header `Authorization: Bearer <TOKEN>` + `profile=home|away&value=18.5`

## 9) Troubleshooting

- `401 missing/invalid bearer token`: wrong/missing `Authorization: Bearer ...`.
- `403 api disabled`: `Homey/API` disabled.
- `403 control disabled`: write control disabled.
- `403 bacnet write disabled`: BACnet write not enabled.
- `409 modbus disabled`: `Modbus` disabled for write calls.
- `500 write ... failed`: issue on active write source (Modbus or BACnet).
- No updates in Homey: check flow trigger and capability IDs.

## 10) Operations and security

1. Use DHCP reservation for stable IP.
2. Keep Homey and VentReader on same LAN/subnet.
3. Share Homey token only with trusted local automations.
4. Keep writes disabled unless needed.
