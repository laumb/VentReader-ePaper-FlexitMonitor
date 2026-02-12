# Homey-oppsett (steg for steg)

Denne guiden viser hvordan du kobler VentReader til Homey Pro og får:
- live data i Homey
- historiske grafer (Insights)

Metoden under bruker:
- VentReader `/status` API
- HomeyScript
- Virtuelle sensorer i Homey (fra en virtual devices-app)

## Hurtigstart (anbefalt)

1. I VentReader admin (`/admin`), bekreft at `Homey/API` er aktivert.
2. Bekreft datakilde:
   - `Modbus (eksperimentell, lokal)`, eller
   - `BACnet (lokal)` med lokal BACnet-konfigurasjon (IP + Device ID).
3. Klikk **Eksporter Homey-oppsett**.
4. Mobil/nettbrett: trykk **Send til e-post (mobil)** for å åpne mailklient med ferdig utfylt innhold.
5. PC: last ned `.json` eller `.txt` og åpne filen lokalt.
6. I Homey: lag virtuelle enheter og lim inn scriptet fra eksporten.

## 1) Klargjør VentReader

I VentReader admin (`/admin`):
1. Aktiver `Homey/API` (wizard steg 3 krever eksplisitt aktiver/deaktiver).
2. Sett/bruk sterk **Homey Bearer-token (/status)** i admin.
3. Velg datakilde (`Modbus` eller `BACnet`).
4. Noter lokal IP-adresse til enheten.

Test i nettleser:
- `GET http://<VENTREADER_IP>/status?pretty=1` med header `Authorization: Bearer <HOMEY_TOKEN>`

Du skal få JSON med felter som:
- `uteluft`, `tilluft`, `avtrekk`, `avkast`
- `fan`, `heat`, `efficiency`
- `mode`, `modbus`, `model`, `time`

## 2) Installer apper i Homey Pro

Installer:
1. `HomeyScript` (offisiell Athom-app)
2. En virtual devices-app som kan lage temperatur-/tallsensorer med Insights

## 3) Lag virtuelle enheter i Homey

Lag én virtuell sensor per verdi du vil logge:
1. `VentReader - Uteluft` (temperatur)
2. `VentReader - Tilluft` (temperatur)
3. `VentReader - Avtrekk` (temperatur)
4. `VentReader - Avkast` (temperatur)
5. `VentReader - Fan %` (prosent/tall)
6. `VentReader - Heat %` (prosent/tall)
7. `VentReader - Gjenvinning %` (prosent/tall)
8. `VentReader - Modbus Alarm` (bool/alarm, valgfri)
9. `VentReader - Status tekst` (tekst, valgfri)

Viktig:
- Bruk capabilities som støtter Insights hvis du vil ha grafer.

## 4) Lag HomeyScript

Lag et script i HomeyScript, f.eks. `VentReader Poll`.

Lim inn og rediger:

```javascript
// VentReader -> Homey virtuelle enheter
const BASE = 'http://192.168.1.50';
const TOKEN = 'REPLACE_TOKEN';
const VENTREADER_URL = `${BASE}/status`;

// Mapping: Homey enhetsnavn -> capability id
const MAP = {
  'VentReader - Uteluft': { field: 'uteluft', cap: 'measure_temperature' },
  'VentReader - Tilluft': { field: 'tilluft', cap: 'measure_temperature' },
  'VentReader - Avtrekk': { field: 'avtrekk', cap: 'measure_temperature' },
  'VentReader - Avkast':  { field: 'avkast',  cap: 'measure_temperature' },
  'VentReader - Fan %':   { field: 'fan', cap: 'measure_percentage' },
  'VentReader - Heat %':  { field: 'heat', cap: 'measure_percentage' },
  'VentReader - Gjenvinning %': { field: 'efficiency', cap: 'measure_percentage' },
};

const ALARM_DEVICE = 'VentReader - Modbus Alarm'; // valgfri
const ALARM_CAP = 'alarm_generic'; // juster ved behov

const STATUS_DEVICE = 'VentReader - Status tekst'; // valgfri
const STATUS_CAP = 'measure_text'; // juster ved behov

function num(v) {
  if (v === null || v === undefined) return null;
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
}

async function setByName(devices, name, capability, value) {
  const d = Object.values(devices).find(x => x.name === name);
  if (!d) return;
  if (!d.capabilitiesObj || !d.capabilitiesObj[capability]) return;
  await d.setCapabilityValue(capability, value);
}

const res = await fetch(VENTREADER_URL, { headers: { Authorization: `Bearer ${TOKEN}` } });
if (!res.ok) throw new Error(`HTTP ${res.status}`);
const s = await res.json();

const devices = await Homey.devices.getDevices();

for (const [name, cfg] of Object.entries(MAP)) {
  const v = num(s[cfg.field]);
  if (v === null) continue; // behold forrige verdi hvis mangler
  await setByName(devices, name, cfg.cap, v);
}

if (ALARM_DEVICE) {
  const modbusStr = String(s.modbus || '');
  const bad = !(modbusStr.startsWith('MB OK') || modbusStr.startsWith('BACNET OK'));
  await setByName(devices, ALARM_DEVICE, ALARM_CAP, bad);
}

if (STATUS_DEVICE) {
  const status = `${s.model || 'N/A'} | ${s.mode || 'N/A'} | ${s.modbus || 'N/A'} | ${s.time || '--:--'}`;
  await setByName(devices, STATUS_DEVICE, STATUS_CAP, status);
}

return `VentReader OK: ${s.time || '--:--'} ${s.modbus || ''}`;
```

## 5) Lag polling-flow i Homey

Flow:
1. `Når`: Hvert 1. minutt (eller 2-5 min for mindre last)
2. `Så`: Kjør script `VentReader Poll`

Valgfri alarm-flow:
1. `Når`: Script feiler / alarm-enhet går aktiv
2. `Så`: Send push-varsel

## 6) Verifiser grafer

Etter noen sykluser:
1. Åpne hver virtuell sensor i Homey.
2. Bekreft at verdier oppdateres.
3. Bekreft at Insights-graf bygger seg opp.

## 7) Anbefalt intervall og drift

Anbefalt:
- Homey polling: 1-2 min
- VentReader poll interval: 30-120 sek for responsivitet

Stabilitetstips:
1. Gi VentReader fast IP (DHCP-reservasjon).
2. Hold Homey og VentReader på samme LAN/subnett.
3. Bruk alarm-flow når `modbus` viser feil/stale.

## 8) Alternativ senere

For enklest mulig sluttbrukeropplevelse: lag en dedikert Homey-app (LAN-driver) for VentReader med:
- pairing via IP/token
- native capabilities
- innebygde flowcards og Insights

Per i dag er VentReader-API-et klart for dette.

## 9) Valgfri styring fra Homey (modus + setpunkt)

I VentReader admin (`/admin`), aktiver ett av disse:
1. Datakilde `Modbus` + `Modbus` + `Enable remote control writes (experimental)`
2. Datakilde `BACnet` + `Enable BACnet writes (experimental)`

Styrings-endepunkt:
1. `POST /api/control/mode` med header `Authorization: Bearer <TOKEN>` + `mode=AWAY|HOME|HIGH|FIRE`
2. `POST /api/control/setpoint` med header `Authorization: Bearer <TOKEN>` + `profile=home|away&value=18.5`

Eksempel HomeyScript handling:

```javascript
const BASE = 'http://192.168.1.50';
const TOKEN = 'REPLACE_TOKEN';

// Sett modus HOME
let res = await fetch(`${BASE}/api/control/mode?mode=HOME`, { method: 'POST', headers: { Authorization: `Bearer ${TOKEN}` } });
if (!res.ok) throw new Error(`Mode write failed: HTTP ${res.status}`);

// Sett HOME setpunkt til 20.5 C
res = await fetch(`${BASE}/api/control/setpoint?profile=home&value=20.5`, { method: 'POST', headers: { Authorization: `Bearer ${TOKEN}` } });
if (!res.ok) throw new Error(`Setpoint write failed: HTTP ${res.status}`);

return 'Control writes OK';
```

Sikkerhet:
1. Hold skrivestyring avskrudd når den ikke trengs.
2. Del kun Homey Bearer-token med lokale, pålitelige automasjoner.
