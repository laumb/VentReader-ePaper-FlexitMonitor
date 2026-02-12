# Modbus Integration Check (Flexit + ESP32 + RS485 Module)

Date: 2026-02-06  
Scope: Static review of firmware + Flexit register sheet + provided hardware/schematic photos.

## Overall readiness

Current implementation is a good prototype, but not yet "plug-and-forget robust" for first field hookup without a few safeguards.

## Findings (risk -> impact -> fix)

### [High] Compile-time Modbus is disabled by default
- Evidence: `FLEXIT_MODBUS_ENABLED` defaults to `0` in `flexit_modbus.h`.
- Impact: Device can look functional but will never read real Modbus data unless compile flag is changed.
- Recommended fix:
  1. Decide build profile for production (`FLEXIT_MODBUS_ENABLED=1`).
  2. Add a boot log line showing whether Modbus binary support is compiled in.

### [High] RS485 transceiver control mode mismatch risk (manual DE/RE vs auto-direction module)
- Evidence: Code expects `RE+DE` pin control (`FLEXIT_RS485_EN`) in `flexit_modbus.cpp`.
- Hardware note: The shown MODBUS 465 board appears to be a TTL-RS485 converter with TXD/RXD and likely auto direction.
- Impact: If hardware mode does not match code assumptions, communication can fail or become unstable.
- Recommended fix:
  1. Add a config/compile switch for transceiver mode: `MANUAL_DE_RE` vs `AUTO_DIR`.
  2. In `AUTO_DIR` mode, skip DE/RE toggling and use only UART TX/RX.

### [High] TTL voltage compatibility risk to ESP32 RX
- Evidence: Provided RS485 board likely supports 5V supply; many such boards output near VCC on TXD.
- Impact: 5V TXD into ESP32 RX can permanently damage GPIO.
- Recommended fix:
  1. Confirm board chip and logic-level behavior.
  2. Run module at 3.3V only if supported, or add level shifting/divider on module `TXD -> ESP32 RX`.

### [High] One register failure drops entire dataset to demo values
- Evidence: Any failed read in `flexit_modbus_poll()` returns false; caller fills `data_example`.
- Impact: UI/API can show believable but fake data during comm errors, masking real faults.
- Recommended fix:
  1. Keep last known good values per field.
  2. Mark unavailable fields explicitly (`NaN`/`null`) and expose error state.
  3. Disable example data fallback outside explicit demo mode.

### [Medium] Model setting (`S3`/`S4`) is stored but not used for register map selection
- Evidence: `cfg.model` exists in config/admin but Modbus code uses one static register map.
- Impact: Future S4 map differences can silently break values.
- Recommended fix:
  1. Implement per-model register tables.
  2. Validate model on boot and print active map.

### [Medium] Bus wiring ambiguity (A/B naming, D+/D- naming)
- Evidence: Flexit docs/images use naming that may differ from converter board labels.
- Impact: No response if A/B polarity is swapped.
- Recommended fix:
  1. First-boot wizard checklist: if timeout, swap A/B.
  2. Keep this as explicit installation step in user docs.

### [Medium] Unknown serial framing/parity in target installation
- Evidence: Code uses fixed `SERIAL_8N1`.
- Impact: If unit is set to different parity/framing, all reads fail.
- Recommended fix:
  1. Add admin settings for baud + parity + stop bits.
  2. Fallback scan option (e.g., 8N1, 8E1) with clear warning/logging.

### [Medium] Poll cycle can become slow under repeated Modbus timeouts
- Evidence: Multiple sequential register calls per cycle; no grouped read optimization.
- Impact: Long blocking cycles, stale UI/API updates.
- Recommended fix:
  1. Read contiguous blocks where possible.
  2. Configure Modbus timeout/retry strategy explicitly.
  3. Add total cycle timing in diagnostics.

### [Low] Termination/bias network may be wrong for final bus topology
- Evidence: Typical RS485 converter boards may include fixed bias/termination components.
- Impact: Reflections/load issues on some cable lengths/topologies.
- Recommended fix:
  1. Confirm single 120R termination at bus ends only.
  2. Ensure only one bias source on bus (or designed equivalent).

## Practical pre-commission checklist

1. Confirm hardware electrical safety:
   - ESP32 GPIO never exposed to 5V logic.
   - Common ground strategy is correct.
2. Confirm correct Flexit Modbus port and role settings in unit menu.
3. Build firmware with Modbus enabled.
4. Test with known-good register read (`3x0001`) first.
5. If timeout:
   - swap A/B once,
   - check slave ID,
   - check parity/framing,
   - check baud.
6. Verify values against Flexit local panel for 3-5 live points.
7. Simulate cable disconnect and verify API/UI shows communication fault (not fake values).

## Suggested first code hardening tasks

1. Add `modbus_transport_mode` (`manual_de_re` / `auto_dir`) and serial framing settings to config.
2. Replace demo fallback with "last-good + explicit error flags".
3. Add per-model register map abstraction.
4. Add Modbus diagnostics endpoint fields: timeout count, last error code, last success timestamp.

