# Home Assistant-oppsett (native MQTT Discovery)

VentReader støtter nå **native Home Assistant-integrasjon** via standard MQTT Discovery.
Du trenger ingen custom komponent.

## Anbefalt flyt (native)

1. I VentReader admin (`/admin`):
- Aktiver `Home Assistant/API`
- Aktiver `HA MQTT Discovery (native)`
- Fyll inn MQTT broker host/IP, port, evt. brukernavn/passord
- Behold `Discovery-prefix` som `homeassistant` (anbefalt)
- Behold `State topic base` som standard hvis du ikke trenger eget oppsett

2. Lagre innstillinger.

3. I Home Assistant:
- Sørg for at MQTT-integrasjonen er satt opp og koblet til samme broker.
- Entities dukker opp automatisk via discovery.

4. Legg entities i dashboard.

## Hva som opprettes automatisk

- Sensorer: `Uteluft`, `Tilluft`, `Avtrekk`, `Avkast`
- Sensorer: `Vifte`, `Varme`, `Gjenvinning`
- Sensorer: `Modus`, `Datakilde`, `Siste dataoppdatering`, `Siste skjermrefresh`
- Binary sensor: `Data stale`

## Felter i HA MQTT-seksjonen

- `MQTT broker host/IP`
- `MQTT-port` (standard `1883`)
- `MQTT brukernavn/passord` (valgfritt)
- `Discovery-prefix` (standard `homeassistant`)
- `State topic base` (standard `ventreader/<chip>`)
- `MQTT publiseringsintervall (sek)`

## REST-fallback (fortsatt støttet)

Eksisterende REST API er fortsatt tilgjengelig:
- `GET /ha/status?pretty=1` med header `Authorization: Bearer <HA_TOKEN>`
- `GET /ha/history?limit=120` med header `Authorization: Bearer <HA_TOKEN>`
- `GET /ha/history.csv?limit=120` med header `Authorization: Bearer <HA_TOKEN>`

Bruk dette hvis du heller vil bygge REST-sensorer manuelt.

## Valgfri styring (eksperimentell)

VentReader sine skrive-endepunkt kan brukes fra HA-automatiseringer:
- `POST /api/control/mode` med header `Authorization: Bearer <MAIN_TOKEN>` + `mode=AWAY|HOME|HIGH|FIRE`
- `POST /api/control/setpoint` med header `Authorization: Bearer <MAIN_TOKEN>` + `profile=home|away&value=18.5`

Krav per aktiv kilde:
- Modbus: `Datakilde=Modbus`, `Modbus aktivert` og `Enable remote control writes`.
- BACnet: `Datakilde=BACnet` og `Enable BACnet writes`.

## Feilsøking

- Ingen entities i HA:
  - Sjekk `HA MQTT-status` i VentReader admin
  - Verifiser MQTT broker host/port/bruker/passord
  - Sjekk at MQTT-integrasjonen i Home Assistant er tilkoblet
  - Bekreft at `Discovery-prefix` matcher HA-oppsett
- Feil ved lagring: `HA MQTT krever PubSubClient-biblioteket`:
  - Installer `PubSubClient` i Arduino-biblioteker og bygg firmware på nytt

## Sikkerhet

- Hold HA-token privat selv om du bruker MQTT.
- Bruk autentisering på MQTT-broker hvis den deles.
- Hold skrivekontroll deaktivert med mindre du faktisk trenger den.
