# VentReader – Brukerveiledning (v4.3.0)

VentReader er en liten enhet som leser data fra Flexit ventilasjonsanlegg
(Nordic S3 / S4 + utvalgte eksperimentelle modeller) og viser informasjon på skjerm og i nettleser.

Standardoppsett er lesefokusert. Eksperimentell styring via Modbus/BACnet-skriv kan aktiveres i admin.

## Changelog (kort)

### v4.3.0

- Samlet og stabilisert hele 4.2.10-4.2.20-løpet i én release.
- BACnet-lesing er gjort merkbart mer robust:
  - Fikset parserkanttilfelle for `ReadProperty-ACK` med REAL-verdi (bl.a. `ai:11`/avkast).
  - Forenklet avkast-fallback til deterministiske kandidater (`ai:11`, `ai:60`, `ai:61`, `av:130`).
  - Bedre feilsøkingslogg for invoke-svar som ikke parse’es (APDU/service + hexdump når debug er aktiv).
- UI/admin-forbedringer fra 4.2.x beholdes:
  - Modernisert grafside med hover/crosshair og tidsfiltre.
  - Bedre lesbarhet i dashboard (chips/footer) og ryddigere handlinger i admin.
- Herdet ePaper-levetid (i tråd med anbefalt praksis for Waveshare/GDEY):
  - Skjermen settes i lavstrøm av-modus (`powerOff`) etter hver render/onboarding-visning.
  - Minste effektive skjermoppdateringsintervall er 180 sek når display er aktivert.
  - Oppdateringsintervall i runtime følger nå endringer uten reboot.

### v4.2.9

- Fikset BACnet modus-tolkning for MSV-baserte modusobjekter når eldre enum-mapping fortsatt var lagret (`HIGH` vises ikke lenger som `PEIS` etter overgang til `msv:41`-default).

### v4.2.8

- Oppdatert BACnet default for modus på Nordic S3: modusobjekt bruker nå `msv:41`.
- Oppdatert default enum-mapping for BACnet modus til `2:AWAY,3:HOME,4:HIGH,5:FIRE`.
- Utvidet BACnet mode-probe kandidatliste med prioritet på observerte `msv`-objekter (`msv:41`, `msv:42`, `msv:14`, `msv:19`).
- Oppdatert BACnet API-referansen slik at dokumenterte defaults matcher firmware.

### v4.2.7

- Lagt til enkel hjem-knapp i topplinjen på admin/setup-sider.
- Lagt til admin-handling for å hente ferske data og refreshe skjerm uten reboot (`/admin/refresh_now`).
- BACnet-defaults er justert etter verifisert Nordic S3-mapping (tilluft default `av:5` og konsistente fallback-verdier).
- Forbedret BACnet write-diagnostikk og parser for minimale Simple-ACK-responser.
- `Tøm debuglogg` tømmer nå kun innhold og lukker ikke/skjuler ikke loggvinduet.

### v4.2.6

- Toppfelt på skjermen er komprimert: viser nå `NORDIC <modell>` og aktiv set-temp (`SET xx.xC`) mellom modellnavn og klokke.
- `set_temp` er lagt til i `/status` og `pretty` JSON (aktivt settpunkt fra datakilde, best-effort).
- Oppsetts-SSID bruker nå prefiks `Ventreader` (uten bindestrek i navneprefiks).
- Ny eksperimentell BACnet-knapp `Write probe` i setup/admin for å teste skrivekapasitet (modus/settpunkt) uten å lagre innstillinger.
- Oppdatert BACnet setpoint-default/migrering for observert Nordic S3-mapping: `home=av:126`, `away=av:96` (legacy `av:5`/`av:100` migreres automatisk).

### v4.2.4

- Bugfix onboarding/admin: BACnet-innstillinger kan nå lagres uten at `Test BACnet` må være vellykket først.
- BACnet-verifisering er fortsatt tilgjengelig som separat test etter lagring.

### v4.2.3

- API-autentisering bruker nå `Authorization: Bearer <token>` (token i URL er fjernet fra API-endepunktene).
- Ny API-nødstopp i admin som blokkerer alle token-beskyttede API-kall umiddelbart.
- Ny sikker API-forhåndsvisning i admin (`/admin/api/preview` + `?pretty=1`).
- Status-JSON inkluderer nå `api_panic_stop`.

### v4.2.2

- Intern refaktorering for bedre vedlikehold: felles kontrollmotor for skriving + felles renderer for hurtigstyring.
- Setup/admin/offentlig side gjenbruker nå samme kontrolllogikk, med mindre duplisering og lavere feilrisiko ved videre utvikling.
- BACnet object-scan er optimalisert slik at tung strengbygging unngås når debuglogging er av.
- Ingen tilsiktede funksjonsendringer.

### v4.2.1

- Admin/settings har nå eget `Display`-punkt med `Headless-modus` + display oppdateringsintervall samlet.
- Hurtigstyring er flyttet rett under status på både offentlig side og admin-side.
- Førstegangsoppsett er nå pre-auth (ingen auth-prompt før setup er fullført).

### v4.2.0

- Ny eksperimentell BACnet-skrivestøtte for modus og settpunkt.
- Nytt BACnet-valg i setup/admin: `Enable BACnet writes (experimental)`.
- Nye BACnet-objektfelt for setpunkt (`home`/`away`).
- `/api/control/*` og hurtigstyring i admin skriver nå via aktiv datakilde (Modbus eller BACnet).

### v4.1.0

- Nytt valg `Headless-modus (skjerm ikke montert)` i setupguide og admin.
- Firmware hopper nå over all ePaper-init/render i headless-modus, slik at enheten kjører stabilt uten fysisk skjerm.
- Ny statusblokk i innlogget admin med ett-klikk `Vanlig API` og `Pretty API`.
- Status-JSON inkluderer nå `display_enabled` og `headless`.

### v4.0.4

- Ny native Home Assistant MQTT Discovery-integrasjon (standard MQTT entities, ingen custom HA-komponent).
- Nye HA MQTT-innstillinger i setup/admin: broker, auth, discovery-prefix, state-topic base og publiseringsintervall.
- Admin viser nå HA MQTT runtime-status, samtidig som eksisterende `/ha/*` REST-endepunkter fortsatt støttes.

### v4.0.3

- Inndata-moduler i setup/admin er flyttet til egen seksjon, separat fra API/integrasjoner.
- Avanserte Modbus-innstillinger vises nå kun når `Modbus` er valgt datakilde og aktivert.
- BACnet-innstillinger vises nå kun når `BACnet` er valgt datakilde.
- Lagt til stiplet gull-separator mellom inndata-blokker for tydeligere visuelt skille.

### v4.0.2

- `/status` returnerer nå komplett datasett uavhengig av aktiv datakilde (`MODBUS` eller `BACNET`).
- `pretty=1` inkluderer nå strukturert gruppering og lesbar `field_map`.
- Beholder legacy-alias (`time`, `modbus`) for bakoverkompatibilitet.

### v4.0.1

- Tidsvisning i display er tydeliggjort:
  - Klokke i toppfelt viser siste skjermrefresh.
  - `siste` under hvert datakort viser siste vellykkede dataoppdatering fra datakilde.
- Status-JSON inkluderer nå `data_time` som eksplisitt tid for siste dataoppdatering.

### v4.0.0

- Ny valgfri datakilde: `BACnet (lokal)` som alternativ til lokal Modbus.
- Nye BACnet-innstillinger i wizard/admin: enhets-IP, Device ID, objektmapping, polling `5-60 min`, test og autodiscover.
- Styringsskriving er nå kun tilgjengelig når datakilde er `Modbus`.
- Egne API-tokens for `main/control`, `Homey (/status)` og `Home Assistant (/ha/*)` + rotasjonsknapper i admin.
- `Modbus` er merket som eksperimentell datakilde. `BACnet` lesing er produksjonsklar.

### v3.7.0

- Oppsettguide steg 3 krever nå eksplisitt valg av `Aktiver` eller `Deaktiver` for både `Homey/API` og `Home Assistant/API`.
- Ny Homey-eksport i admin: ferdig `.json`/`.txt` med script, mapping og endepunkter.
- Mobil-knapp for deling via e-post (`mailto`) med ferdig utfylt innhold.
- Forside og admin har tydeligere statuskort og bedre handlingslayout.

### v3.6.0

- Språkvalg gjelder nå også admin-undersider og ePaper-tekster
- Dashboardets modusverdier oversettes nå etter valgt språk
- Status-JSON inkluderer nå tidsfeltene `ts_epoch_ms` og `ts_iso` for logging/grafer
- Ny lokal historikk-API for grafer (`/status/history`, `/ha/history`)
- Ny diagnostikk-API (`/status/diag`) med Modbus-kvalitetstellere
- Ny hurtigstyring i admin (modus + setpunkt) når kontrollskriving er aktivert
- Hurtigstyring i admin er nå språkstyrt
- Ny graf-side i admin (`/admin/graphs`) som viser lokale historikktrender
- Graf-siden har toggle per serie + CSV-eksport
- Ny storage-sjekk (`/status/storage`) for å verifisere avgrenset RAM-bruk
- Ny manual/changelog-side i admin (`/admin/manual`)
- Mer konsistente kvitteringssider i admin (lagre/restart/OTA)

---

## Første oppstart

Når enheten startes første gang:

- Skjermen viser oppsettsinformasjon
- Enheten starter sitt eget WiFi-nettverk
- Du må gjennom et oppsett i nettleser

---

## Koble til enheten

1. Koble mobilen eller PC til WiFi-nettverket som vises på skjermen
2. Bruk passordet som står på skjermen
3. Åpne nettleser og gå til IP-adressen som vises
   (du blir automatisk sendt til oppsett)

## Headless hurtigstart (uten skjerm)

1. Start enheten.
2. Koble til AP `Ventreader-XXXXXX` (passord `ventreader`).
3. Åpne `http://192.168.4.1/admin/setup`.
4. I steg 3: aktiver `Headless-modus (skjerm ikke montert)`.
5. Fullfør oppsett og restart.

For allerede konfigurert enhet:
- `http://<enhet-ip>/admin`
- innlogging `admin` + ditt admin-passord

---

## Innlogging

Standard innlogging (kun første gang):

- Bruker: `admin`
- Passord: `ventreader`

Du må endre passordet under oppsett.

---

## Oppsett (wizard)

Oppsettet består av 3 steg:

1. Velg nytt admin-passord
2. Koble enheten til ditt WiFi
3. Velg modell og funksjoner (Modbus, Homey/API, Home Assistant/API)
   samt datakilde (`Modbus (eksperimentell)` eller `BACnet`),
   og eventuelt `Headless-modus` hvis skjerm ikke er montert.

Når du trykker **Fullfør og restart**, lagres alt og enheten starter på nytt.

---

## Normal bruk

Når oppsett er fullført:

- Skjermen viser ventilasjonsdata
- Nettgrensesnitt er tilgjengelig på enhetens IP
- Enheten oppdaterer seg automatisk
- Aktiv datakilde vises i offentlig status/admin.

---

## BACnet lokal datakilde (valgfritt)

Brukes når du vil hente data uten Modbus-kabling.

Påkrevd:
- Enhets-IP (samme LAN/VLAN)
- BACnet Device ID
- BACnet polling-intervall (`5-60 min`)

Valgfritt:
- Objektmapping i avanserte felt
- Autodiscover for å finne IP + Device ID

---


## Homey-oppsett

Full steg-for-steg guider:

- Norsk: `docs/HOMEY_SETUP_NO.md`
- English: `docs/HOMEY_SETUP.md`

## Home Assistant-oppsett

Full steg-for-steg guider:

- Norsk: `docs/HOME_ASSISTANT_SETUP_NO.md`
- English: `docs/HOME_ASSISTANT_SETUP.md`

## BACnet/API full referanse

- Komplett referanse (for mennesker + maskiner): `docs/BACNET_API.md`

Anbefalt metode er nå native MQTT Discovery i Home Assistant (ingen custom komponent).

## API-endepunkt

- Helse:
  - `GET /health`
- Status:
  - `GET /status` med header `Authorization: Bearer <HOMEY_TOKEN>`
  - `GET /ha/status` med header `Authorization: Bearer <HA_TOKEN>` (krever `Home Assistant/API` aktivert)
  - Inkluderer `ts_epoch_ms` og `ts_iso` i hver datapakke for tidsserier/grafer
  - Inkluderer `stale`-felt, `ha_mqtt_enabled`, `display_enabled`, `headless` og `api_panic_stop` per datapakke
- Historikk:
  - `GET /status/history?limit=120` med Bearer-header
  - `GET /ha/history?limit=120` med Bearer-header
  - `GET /status/history.csv?limit=120` med Bearer-header
  - `GET /ha/history.csv?limit=120` med Bearer-header
- Diagnostikk:
  - `GET /status/diag` med Bearer-header
  - `GET /status/storage` med Bearer-header
- Styring (eksperimentelt, via aktiv datakilde):
  - Modbus: krever datakilde=`Modbus`, `Modbus` + `Control writes`
  - BACnet: krever datakilde=`BACNET` + `Enable BACnet writes`
  - `POST /api/control/mode` med Bearer-header + `mode=AWAY|HOME|HIGH|FIRE`
  - `POST /api/control/setpoint` med Bearer-header + `profile=home|away&value=18.5`

---

## OTA-oppdatering

To måter:

1. Nettleser-opplasting i admin (`/admin/ota`) med `.bin`-fil
2. Arduino OTA via nettverksport (kun når STA WiFi er oppe)

OTA sletter ikke konfigurasjon.

---

## Tilbakestille til fabrikkinnstillinger

1. Slå av strømmen
2. Hold inne **BOOT-knappen**
3. Slå på strømmen
4. Hold BOOT i ca. 6 sekunder

Alt slettes og enheten starter på nytt som ny.

---

## Sikkerhet

- Standardpassord brukes kun før oppsett
- Etter oppsett må eget passord brukes
- Ingen data sendes til sky automatisk (lokal BACnet og lokale API-kall)

---

VentReader er ikke tilknyttet Flexit.
