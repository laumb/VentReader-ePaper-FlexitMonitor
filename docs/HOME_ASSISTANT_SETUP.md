# Home Assistant Setup Guide (Native MQTT Discovery)

VentReader now supports **native Home Assistant integration** using standard MQTT Discovery.
No custom component is required.

## Recommended flow (native)

1. In VentReader admin (`/admin`):
- Enable `Home Assistant/API`
- Enable `HA MQTT Discovery (native)`
- Fill MQTT broker host/IP, port, optional username/password
- Keep `Discovery prefix` as `homeassistant` (recommended)
- Keep `State topic base` default unless you need custom topic layout

2. Save settings.

3. In Home Assistant:
- Ensure MQTT integration is installed and connected to the same broker.
- Entities will auto-appear from discovery topics.

4. Add entities to dashboard (temperatures, fan, heat, efficiency, mode, source).

## What gets created automatically

- Sensors: `Uteluft`, `Tilluft`, `Avtrekk`, `Avkast`
- Sensors: `Vifte`, `Varme`, `Gjenvinning`
- Sensors: `Modus`, `Datakilde`, `Siste dataoppdatering`, `Siste skjermrefresh`
- Binary sensor: `Data stale`

## VentReader MQTT fields

In admin, HA MQTT section:
- `MQTT broker host/IP`
- `MQTT port` (default `1883`)
- `MQTT username/password` (optional)
- `Discovery prefix` (default `homeassistant`)
- `State topic base` (default `ventreader/<chip>`)
- `MQTT publish interval (sec)`

## Fallback (REST API)

The existing REST API is still available:
- `GET /ha/status?pretty=1` with header `Authorization: Bearer <HA_TOKEN>`
- `GET /ha/history?limit=120` with header `Authorization: Bearer <HA_TOKEN>`
- `GET /ha/history.csv?limit=120` with header `Authorization: Bearer <HA_TOKEN>`

Use this if you prefer REST sensors/templates.

## Optional control writes (experimental)

VentReader write endpoints can be used from HA automations:
- `POST /api/control/mode` with header `Authorization: Bearer <MAIN_TOKEN>` + `mode=AWAY|HOME|HIGH|FIRE`
- `POST /api/control/setpoint` with header `Authorization: Bearer <MAIN_TOKEN>` + `profile=home|away&value=18.5`

Active source requirements:
- Modbus: `Data source=Modbus`, `Modbus enabled`, and `Enable remote control writes`.
- BACnet: `Data source=BACnet` and `Enable BACnet writes`.

## Troubleshooting

- No entities in HA:
  - Check VentReader admin `HA MQTT status`
  - Verify MQTT broker host/port/user/pass
  - Confirm Home Assistant MQTT integration is connected
  - Check that `Discovery prefix` matches HA MQTT discovery settings
- `HA MQTT requires PubSubClient` in save response:
  - Install `PubSubClient` in Arduino libraries and rebuild firmware

## Security

- Keep HA token private even when using MQTT.
- Use MQTT auth if broker is shared.
- Keep write control disabled unless explicitly needed.
