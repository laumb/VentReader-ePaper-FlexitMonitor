# VentReader – Manufacturing & QA Guide

Dette dokumentet er for produksjon, QA og service.

---

## Flashing firmware

Standard flashing (Arduino IDE / esptool):

- NVS beholdes
- Brukes for OTA / oppdateringer

Full wipe (factory):

- Hold BOOT under oppstart
- Eller bruk `esptool.py erase_flash`

---

## Manufacturing mode

Enheten styres av setup-status:

- `setup_completed = false`

Dette betyr:
- Setup wizard vises
- Dashboard vises ikke
- Default admin-passord aktivt
- Arduino OTA deaktivert (STA + setup kreves)

Status settes til `true` når setup fullføres.

---

## Default factory state

| Parameter | Verdi |
|--------|------|
| setup_completed | false |
| admin_pass | ventreader |
| WiFi | AP aktiv |
| Display | Onboarding |

---

## QA-checkliste

1. Flash firmware
2. Strøm på
3. Skjerm viser onboarding
4. WiFi AP synlig
5. Admin login fungerer
6. Setup kan fullføres
7. Restart → dashboard vises
8. OTA fungerer
9. Factory reset fungerer

---

## Komponenter

- ESP32-S3 (Dev / SuperMini)
- MAX3485 RS-485
- 4.2" ePaper (SSD1683)
- USB-C strøm

---

## Juridisk

- Ingen Flexit-logo
- Ikke bruk Flexit i SSID/hostname
- Produktnavn: VentReader

---

## Service / RMA

- BOOT-hold reset
- Reflash firmware
- Klar for ny kunde
