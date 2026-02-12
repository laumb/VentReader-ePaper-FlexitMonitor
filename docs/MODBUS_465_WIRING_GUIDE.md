# MODBUS 465 Wiring Guide (ESP32 + Flexit)

This is a practical hookup guide for the shown MODBUS 465 style RS485-TTL board.

## 1) TTL side (module -> ESP32)

Use these default firmware pins unless you changed them in code:
- ESP32 TX (GPIO17) -> Module `RXD`
- ESP32 RX (GPIO16) -> Module `TXD`
- ESP32 GND -> Module `GND`
- ESP32 3V3 (preferred) -> Module `VCC` (only if module supports 3.3V operation)

If you explicitly use `MANUAL` transport mode in admin:
- ESP32 GPIO18 -> Module DE/RE control input (only relevant on boards that expose it)

Important:
- If module `TXD` is 5V logic and ESP32 RX is unprotected, add level shifting/divider.

## 2) RS485 side (module -> Flexit)

Module screw terminals are typically:
- `A`
- `B`
- `GND` or reference

Connect to Flexit Modbus pair:
- Module `A` -> Flexit `A` (or `D+` depending on label)
- Module `B` -> Flexit `B` (or `D-` depending on label)
- Reference ground if required by installation/site wiring practice

If no response, first troubleshooting step is swapping `A` and `B`.

## 3) Recommended software settings (first boot)

In setup wizard/admin:
- Enable `Modbus`
- Model: choose actual unit (`S3` or `S4`)
- Transport mode:
  - `AUTO` for auto-direction TTL-RS485 boards (recommended for this module type)
  - `MANUAL` only when DE/RE control is needed and wired
- Serial format: start with `8N1`
- Baud: `19200`
- Slave ID: `1`
- Addr offset: `0` (try `-1` if addressing mismatch)

## 4) Commissioning checklist

1. Confirm Modbus is enabled in Flexit controller settings.
2. Apply wiring and power-cycle both sides.
3. Verify API/UI status:
   - `MB OK (...)` means live read success.
   - `MB <ERR> (stale)` means no fresh read, showing last known values.
4. If error persists:
   - swap A/B,
   - verify slave ID,
   - verify parity/format,
   - verify baud.

## 5) Bus quality notes

- Keep twisted pair for A/B.
- Use one termination strategy across the bus (avoid multiple random 120R terminations).
- Keep common ground strategy consistent across devices.

