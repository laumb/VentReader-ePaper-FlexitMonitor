# BACnet/IP Integration Guide (Vendor-Neutral, Reusable)

Scope: Practical BACnet/IP guide for developers building their own integrations, gateways, dashboards, or automations.

This document is intentionally implementation-agnostic. It does not assume any specific firmware, UI, or codebase.

## 1) What BACnet/IP Is

BACnet is a building-automation protocol. BACnet/IP transports BACnet messages over UDP (typically port `47808`, also known as `0xBAC0`).

A typical integration flow is:
1. Discover target device (`Who-Is` / `I-Am`) or use known IP + device-id.
2. Read relevant objects (`ReadProperty`).
3. Optionally write controllable objects (`WriteProperty`).
4. Normalize values into your app model (temps, mode, setpoint, fan, heat, etc.).

## 2) Core BACnet Concepts

## 2.1 Object model

Data lives in objects identified by:
- `object type` (e.g. analog-input, analog-value)
- `instance` (numeric id)

Canonical object id examples:
- `ai:1` (`analog-input`, instance 1)
- `ao:3` (`analog-output`, instance 3)
- `av:5` (`analog-value`, instance 5)
- `msv:60` (`multi-state-value`, instance 60)

## 2.2 Important object types

- `ai` (`0`): Analog Input, usually read-only measured values
- `ao` (`1`): Analog Output, often writable control values
- `av` (`2`): Analog Value, can be writable or read-only by vendor design
- `bi/bo/bv` (`3/4/5`): Binary points
- `mi/mo/msv` (`13/14/19`): Multi-state points (often mode enums)
- `device` (`8`): Device object (identity/meta)

## 2.3 Common properties

- `present-value` (`85`): primary runtime value for most points
- `vendor-identifier` (`120`) on `device`
- (optional) `object-name` (`77`), `units` (`117`), `description` (`28`)

In practice, many field integrations start with only `present-value` and add metadata later.

## 2.4 APDU services used most

- `ReadProperty` (service `12`)
- `WriteProperty` (service `15`)
- Discovery: `Who-Is` / `I-Am`

## 3) Network Requirements

- BACnet device and your integration host must be L3 reachable (same LAN/VLAN or routed).
- UDP `47808` open both directions (or configured BACnet UDP port).
- Broadcast may be blocked by network policy; unicast reads can still work.

## 4) Authentication and Access Reality

BACnet itself often has no user/password login like REST APIs.

Access control is usually enforced by:
- Network boundaries (VLAN/firewall)
- Device role/permissions
- Object-level write permissions

You can have read success but write denied on same device.

## 5) Read Workflow (Robust)

Recommended sequence:
1. Validate target config (`ip`, `port`, `device-id`).
2. Optional discovery check (`Who-Is` unicast + broadcast fallback).
3. Validate device identity by reading `device:<id>.vendor-identifier`.
4. Read each configured object via `ReadProperty(present-value=85)`.
5. Parse app tags to numeric/enum.
6. Keep last-good sample for stale fallback.

If discovery fails but direct reads succeed, continue with direct reads.

## 6) Write Workflow (Robust)

Recommended sequence:
1. Validate write feature enabled in your app.
2. Validate value range (e.g. setpoint `10..30` C).
3. Attempt `WriteProperty` to configured object (`present-value`).
4. If write denied, retry with BACnet write priority (commonly `16`).
5. If vendor uses alternate object type, try sibling mapping (`av` <-> `ao`) on same instance.
6. Return explicit write error details.

## 6.1 Why fallback matters

Vendors differ in how writable points are exposed:
- some expose writable control under `ao:*`
- some under `av:*`
- some require priority to accept writes
- some reject writes entirely outside specific operating state

## 7) Error Model You Should Implement

Use stage-prefixed errors for clarity:
- `CFG`: invalid/missing config
- `NET`: socket/transport failures
- `AUTH`: discovery/identity mismatch (not user auth)
- `DATA`: read parse/object/property issues
- `WRITE`: write response/permission issues

Typical BACnet Error-PDU examples:
- `class=1 code=31 service=12`: read of unsupported object/property or invalid combination
- `class=2 code=40 service=15`: write access denied

Practical interpretation:
- service `12` = `ReadProperty`
- service `15` = `WriteProperty`

## 8) Minimal Integration Data Contract (Recommended)

Even if you build your own API layer, expose normalized fields like:

- `outdoor_temp_c`
- `supply_temp_c`
- `extract_temp_c`
- `exhaust_temp_c`
- `set_temp_c` (active target setpoint if available)
- `fan_percent`
- `heat_percent`
- `mode`
- `data_time`
- `source_status`
- `stale`

Include both:
- normalized business fields (easy for apps)
- raw BACnet mapping metadata (easy for troubleshooting)

## 9) Known Flexit Nordic S3 Mapping (Observed Baseline)

The following mapping has been observed in real-world testing and is a strong starting point for Nordic S3-like setups:

| Semantic | Candidate BACnet object |
|---|---|
| Outdoor temperature | `ai:1` |
| Supply temperature | `av:5` |
| Extract temperature | `ai:59` |
| Exhaust temperature | `ai:11` |
| Fan percent | `ao:3` |
| Heat percent | `ao:29` |
| Mode | `msv:41` |
| Setpoint (home) | `av:126` |
| Setpoint (away) | `av:96` |

Default mode map commonly used:
- `2:AWAY,3:HOME,4:HIGH,5:FIRE`

Important:
- This is not guaranteed for all models/firmware.
- Always verify with object scan/probe on your target.

## 10) Object Probe/Scan Strategy

When mappings are unknown:
1. Scan selected type ranges first (`ai`, `ao`, `av`, `msv`).
2. Compare live values against official app/HMI values.
3. Lock mapping candidates.
4. Re-test after reboot and under different operating modes.

Recommended practical scan ranges:
- start with `0..200` instances
- expand only if needed

## 11) Write Capability Probe Strategy

A safe write probe can be:
1. Read current mode and setpoint-like value.
2. Attempt no-op write (same value) to setpoint home.
3. Attempt no-op write to setpoint away.
4. Attempt mode write only if mode map is known.
5. Report per-attempt result (`ok/error`) without auto-persisting config.

This prevents destructive toggles while still validating write capability.

## 12) Interoperability Checklist

Before production:
1. Read path works for all required values.
2. Error handling distinguishes config/network/protocol/write errors.
3. Stale fallback implemented.
4. Time stamps captured for last successful datasource update.
5. Write path guarded by explicit feature flag.
6. Write attempts logged with full reason on failure.
7. Mapping can be overridden without reflashing.

## 13) Security Recommendations (BACnet Integrations)

- Treat BACnet network as sensitive OT zone.
- Restrict UDP BACnet traffic by VLAN/firewall rules.
- Disable writes by default; enable explicitly.
- Add emergency stop in your adapter layer.
- Keep integration credentials/tokens separate from BACnet transport settings.

## 14) Machine-Readable Mapping Example (JSON)

```json
{
  "device": {
    "ip": "10.0.0.64",
    "port": 47808,
    "device_id": 2
  },
  "objects": {
    "outdoor": "ai:1",
    "supply": "av:5",
    "extract": "ai:59",
    "exhaust": "ai:11",
    "fan": "ao:3",
    "heat": "ao:29",
    "mode": "msv:41",
    "setpoint_home": "av:126",
    "setpoint_away": "av:96"
  },
  "mode_map": {
    "2": "AWAY",
    "3": "HOME",
    "4": "HIGH",
    "5": "FIRE"
  }
}
```

## 15) Example Normalized Output (JSON)

```json
{
  "timestamp": "2026-02-08T12:34:56+01:00",
  "stale": false,
  "source_status": "BACNET OK",
  "mode": "HOME",
  "outdoor_temp_c": -4.6,
  "supply_temp_c": 22.0,
  "extract_temp_c": 25.2,
  "exhaust_temp_c": 1.9,
  "set_temp_c": 22.5,
  "fan_percent": 55,
  "heat_percent": 48
}
```

## 16) Key Takeaway

For BACnet integrations, success is mostly about:
- correct object mapping,
- robust error handling,
- safe write fallbacks,
- and clear normalization for downstream systems.

If you design around those four points, the integration becomes portable across devices and software stacks.
