# VentReader Technical Notes (Modbus + Display)

This note is a compact reference for:
- Current Modbus registers used in firmware
- Recommended next Modbus extensions
- E-paper driver and hardware checklist

## 1) Active Modbus Register Map (currently used)

Source: `modbus_nordic_basic_2883.xlsx` (Flexit Nordic Basic register map, base-0 addressing).

| Field in firmware | Flexit register | Type | FC | Access | Notes |
|---|---|---|---|---|---|
| `uteluft` | `3x0001` | Float32 (BE word order) | `FC04` | R | Outside air temp |
| `tilluft` | `3x0005` | Float32 (BE word order) | `FC04` | R | Supply air temp |
| `avtrekk` | `3x0009` | Float32 (BE word order) | `FC04` | R | Extract air temp |
| `avkast` | `3x0013` | Float32 (BE word order) | `FC04` | R | Exhaust air temp |
| `fan_percent` | `4x0005` | Float32 (BE word order) | `FC03` | R | Supply fan speed (%) |
| `heat_element_percent` | `4x0013` | Float32 (BE word order) | `FC03` | R | Electric heating position (%) |
| `mode` | `3x3034` | Uint16 | `FC04` | R | Ventilation state (1..7) |

### Addressing convention in firmware
- Code uses the base-0 numbers from the Flexit sheet directly.
- `FLEXIT_ADDR_OFFSET` can be set to `-1` if site setup expects 1-based addressing behavior.

## 2) Recommended Modbus Extensions (read-only)

These are useful additions for diagnostics and richer Homey cards:

| Priority | Register | Meaning | Type | FC | Why |
|---|---|---|---|---|---|
| High | `3x3001`, `3x3002` | A-alarm indication/state | Uint16 | FC04 | Fast alarm visibility |
| High | `3x3003`, `3x3004` | B-alarm indication/state | Uint16 | FC04 | Extra fault channel |
| High | `3x1021`, `3x1023` | A/B alarm code | Uint16 | FC04 | Human-readable fault mapping |
| Medium | `4x1271`, `4x1269` | Filter runtime / replacement timer | Uint32/Uint16 per map | FC03 | Filter maintenance cards |
| Medium | `3x1025`, `3x1027`, `3x1029` | Fault counter/list navigation code | Uint16 | FC04 | Better service workflow |
| Medium | `3x0021`, `3x0025` | Fan feedback rpm values | Float32 | FC04 | Real fan diagnostics |
| Optional | `3x0061`, `3x0149` | RH / air quality (if installed) | Float32 | FC04 | IAQ insights |

Implementation guidance:
- Keep integration read-only.
- Add per-register feature flags in config if some installations do not expose optional sensors.
- Return `null` for unavailable registers instead of failing the full poll.

## 3) E-paper Driver Checklist (4.2" BW panel)

Based on the panel datasheet structure (`英瑞达E042A87（BW）.pdf`) and current code behavior:

### SPI + control
- `CS`, `D/C`, `RST`, `BUSY` are mandatory and already modeled by GxEPD2 driver.
- Respect `BUSY` high state during update/temperature/LUT actions.
- Use clean reset sequence after flashing or power cycle (already done by `ui_epaper_hard_clear()`).

### Update strategy
- Keep periodic partial updates for normal operation.
- Perform full clear periodically (already every 10th refresh) to reduce ghosting.
- Do not push update interval too low; e-paper life and readability are better with moderate cadence.

### Power and stability
- Ensure panel supply is stable during refresh spikes.
- Keep short SPI wires and a solid common ground with MCU.
- Avoid concurrent high-noise loads on the same supply rail during display refresh.

### Suggested acceptance test
1. Cold boot -> full clear -> first dashboard render.
2. 20+ refresh cycles -> check for ghosting/drift.
3. WiFi reconnect and Modbus error/recover path -> verify display remains stable.
4. Power cycle during active update -> verify next boot clears and recovers.

## 4) RS485 Module (MODBUS 465 board on image) - wiring sanity notes

The shown board appears to be a TTL-to-RS485 module with:
- TTL side: `VCC`, `RXD`, `TXD`, `GND`
- Bus side: `A`, `B`, and reference/ground terminal

Quick wiring checklist:
1. MCU `TX` -> module `RXD`, MCU `RX` -> module `TXD`, and common `GND`.
2. RS485 `A`/`B` must match Flexit side exactly (if no response, first swap A/B).
3. Add bus reference ground if Flexit installation expects it.
4. One end-of-line termination only where appropriate (avoid multiple 120R terminations).
5. Verify module logic level compatibility with ESP32 (3.3V-safe on RX pin).

