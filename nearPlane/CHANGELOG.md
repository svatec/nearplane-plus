# nearPlane+ – Changelog & Controls / Zmeny a ovládanie

nearPlane+ · M5StickC Plus2 ADS-B tracker · Firmware `nearPlane.ino` (v4.2)

**Language / Jazyk:** [English](#english) · [Slovenčina](#slovenčina)

---

<a id="english"></a>
# English

## 1. Fixes

| # | Problem | Cause | Fix |
|---|---|---|---|
| 1 | HTTP **403** – "User-Agent too generic" | `HTTPClient` always sends its own `User-Agent: ESP32HTTPClient`; `addHeader("User-Agent", …)` only added a second header, so the server read the generic one | Use `http.setUserAgent(userAgent)` with a real contact from the setup form; `USER_AGENT` is only a fallback |
| 2 | TLS connection setup | Plain `HTTPClient` without a secure client | `WiFiClientSecure` with `setInsecure()` (no certificate validation) |
| 3 | Route API called every 2 s | `lastFlightCodeForDetails` was never assigned, so the "already fetched" check never matched | Route is now fetched once per aircraft and cached |
| 4 | Misleading setup form | adsb.lol radius is in **nautical miles** (max 250), form said km | Label changed to "Scan Radius (NM, max 250)" |
| 5 | HTTP **429** – rate limit exceeded | Polling every 2 s | Longer intervals (see table below); on 429 the last data stays on screen and polling backs off; final backoff and fallback behavior: see #10 |
| 6 | Compile error `StreamReader was not declared` | Arduino IDE auto-generates function prototypes above the first function; the class was defined after it | `StreamReader` moved above all functions, next to the other types |
| 7 | HTTP **426** – Upgrade Required | `useHTTP10(true)` was rejected by the server (HTTP/1.1 required) | Removed; the stream reader now decodes `Transfer-Encoding: chunked` itself |
| 8 | No output in the serial console | Shortened startup; monitor may attach late | Firmware waits up to 3 s for the serial port and prints a start banner (`nearPlane+: start (v4)`) |
| 9 | Noisy debug output | Full response body printed on every poll | Only compact status lines; body printed on errors only |
| 10 | Device seems to "never start" after boot: log shows HTTP 429 and the screen stays on "Fetching aircraft…" for 120 s | adsb.lol rate limit hit on the first request, no feedback on screen, fixed 120 s wait | "Rate limited – retry in N s" countdown, exponential backoff 20→120 s, `Retry-After` support, automatic fallback to `opendata.adsb.fi` (airplanes.live was tried first but blocks unknown clients with HTTP 403) |

## 2. Added features

### 2.1 Direction & distance page (page 7)
- Arrow pointing to the aircraft (north is up – the device has no compass).
- Distance in km, updated every second via **dead reckoning** (position extrapolated from ground speed and track between API polls).
- Trend: **APPROACHING** (green) / **RECEDING** (red) / **STEADY** (yellow), based on radial velocity.
- Bearing in degrees plus compass point (e.g. `BRG 245 SW`).

### 2.2 CPA – closest point of approach
- Computed from track and ground speed.
- Shows `CPA 3.2 km` and time to it (`in 0:45`), or `PASSED` when the aircraft is already moving away.

### 2.3 Alerts for interesting aircraft
| Level | Colour | Trigger |
|---|---|---|
| 3 – EMERGENCY | red | emergency flag or squawk 7500 / 7600 / 7700 |
| 2 – MILITARY | orange | `dbFlags` military bit |
| 1 – RARE | cyan | A380, B747-400/-8/SP, An-124, An-225, Beluga/BelugaXL, Dreamlifter, C-5M, Il-76, A340-600 |

- Coloured banner behind the flight number (white text on red, black text on orange/cyan for readability), label in the footer, distinct sound per level.
- The alerted aircraft is selected automatically and the display jumps to page 1.
- The same aircraft does not trigger the alert twice (last 8 are remembered).
- Alerted aircraft are always kept in the list of 5, even if they are not the closest.

### 2.4 Multiple aircraft
- Endpoint changed from `/v2/closest` to `/v2/point`; the 5 closest aircraft are tracked.
- The response is **streamed and parsed one aircraft at a time**, so memory stays low even with dozens of aircraft in range.
- Footer indicator `AC 2/5`; selection survives data refreshes (matched by ICAO hex).

### 2.5 Radar map (page 8)
- Top-down view, north up, you in the centre, two range rings.
- Up to 40 aircraft as dots with a short heading line; positions move every second.
- Colour follows alert level; selected aircraft is circled in yellow.
- If the selected aircraft is outside the current scale, a yellow dot is shown on the rim in its direction and `SEL 24.1 km` in the footer.
- **Scale cycle:** full configured radius → 25 km → 12.5 km → 7.5 km → repeat (inner ring is always half). Steps larger than the configured radius are skipped.

### 2.6 Nearby list (page 9)
- 5 closest aircraft with callsign, distance (green = approaching, red = receding) and altitude (`FL360`, `3200ft`, `GND`).
- Selected row highlighted; alert aircraft use their alert colour.

### 2.7 Daily statistics (page 10)
- Aircraft count (unique ICAO hex), closest, highest and fastest aircraft of the day.
- Saved to flash (NVS) every 10 minutes and before sleep; survives reboots; resets at midnight.
- Requires NTP time sync.

### 2.8 Type and airline names
- ICAO type code → readable name (`B738` → `Boeing 737-800`), about 100 types built in.
- Callsign prefix → airline (`RYR1234` → `Ryanair`), about 80 airlines built in.
- Header line shows `Airline | Type`; when the airline is unknown it shows the registration instead. Lines that do not fit scroll as a marquee (40 px/s, 1.5 s pause at the start).
- Unknown types fall back to the API `desc` field, then to the ICAO code. Extend the tables `TYPE_NAMES` / `AIRLINES` in the source.

### 2.9 Buzzer on/off
- Hold **BtnC** for 0.75 s (750 ms) to mute or unmute all sounds (startup beep, new-aircraft beep, alert sounds).
- A "Sound ON / Sound OFF" message is shown; while muted a crossed-out speaker icon appears next to the battery.
- The setting is stored in flash and survives reboots; a settings reset restores sound ON.

### 2.10 Configurable User-Agent
- New **User-Agent** field in the setup web form (device configuration), stored in flash.
- The form and server require an e-mail or URL, reject the placeholder and cap the value at 120 characters; line breaks are stripped.
- The built-in `USER_AGENT` constant is only a dummy fallback (`changeme@example.com`); it is not a valid contact.
- To change a saved value, reset the settings and run the setup again.

### 2.11 GPS module (optional)
- Supports the M5Stack GPS Unit v1.1 (AT6668 + MAX2659) on the Grove port; the older AT6558 unit works as well.
- Wire orientation (G32/G33) and baud rate (115200 / 9600) are detected automatically from valid NMEA sentences (checksum verified).
- The live GPS position replaces the saved coordinates: the API query, distances, bearings and the radar map use it; a fix up to 10 min old is kept, then the saved position is used.
- Setup form: latitude/longitude are prefilled from the GPS, new checkbox **Use GPS module if connected**.
- Display indicator `GPS` (live fix) / `GPS?` (module found, no fix) below the clock; the serial log shows `gps=...` and the position source.

### 2.12 Other changes
- Page 5: the `CAT` row was replaced by `REG` (registration), because the header no longer shows it when the airline is known.
- Route (`DEP > ARR`) is fetched only for the selected aircraft and only once.
- BtnC hold time for the buzzer toggle shortened from 2 s to 0.75 s (750 ms).

## 3. Controls

| Button | Action | Where |
|---|---|---|
| **BtnA** (front) short | next page | all pages |
| **BtnA** long (0.6 s) | jump to detail of the selected aircraft (page 1) | radar map |
| **BtnC** (power button, side) short | change map scale | radar map |
| **BtnC** hold 0.75 s | buzzer on/off (saved) | all pages |
| **BtnB** (side) short | next aircraft | all aircraft pages |
| **BtnB** short | next aircraft **within current scale** | radar map |
| **BtnB** hold 1 s → 5 s | countdown screen, then settings reset | everywhere (release early to cancel) |

BtnC is the third button of the M5StickC Plus2 (the power button, exposed as `M5.BtnPWR`).

## 4. Pages

| # | Content |
|---|---|
| 1 | Altitude, ground speed, route |
| 2 | Heading, vertical speed, roll |
| 3 | Mach, IAS/TAS, geometric altitude |
| 4 | Autopilot altitude, heading, modes |
| 5 | Squawk, ICAO hex, registration |
| 6 | OAT/TAT, wind, RSSI |
| 7 | Arrow, distance, trend, CPA |
| 8 | Radar map |
| 9 | Nearby list |
| 10 | Daily statistics |

## 5. Polling intervals

| Situation | Interval |
|---|---|
| Aircraft found | 15 s |
| No aircraft | 30 s |
| Other error (parse, 5xx, timeout) | 60 s |
| HTTP 429 | 20 s, doubling up to 120 s (`Retry-After` wins) + fallback server |
| Page 7/8/9 redraw (local calculation, no API call) | 1 s |

Constants are at the top of the file (`SHORT_POLL_INTERVAL` etc.).

## 6. Notes & limitations

- The sketch must be saved as `nearPlane/nearPlane.ino` (folder name = file name), otherwise the linker reports undefined `setup`/`loop`.
- The device has no compass: arrows and the map are always north-up, also when the GPS position changes.
- The GPS needs a satellite fix (about 25 s outdoors); until then, or when no module is detected, the coordinates saved in setup are used.
- adsb.lol requires a real contact in the User-Agent – enter it in the setup form (field **User-Agent**).
- Radius is entered in nautical miles (1 NM = 1.852 km).
- Stats and the clock need a working NTP connection.
- `setInsecure()` disables TLS certificate validation; acceptable for public flight data, not for sensitive traffic.

## 7. v4.1 – reliability and build

### 7.1 Fixes

| # | Problem | Cause | Fix |
|---|---|---|---|
| 1 | Setup access point ran forever and drained the battery | `configMode` branch in `loop()` never checked the inactivity timeout | Setup AP switches off after 10 minutes of inactivity (paused while charging) and the device goes to deep sleep |
| 2 | After a Wi-Fi outage the device never reconnected | `WiFi.begin()` was called only once during boot | `ensureWifi()` reconnects with a 5–60 s backoff; after 10 minutes without a connection it reopens the setup AP |
| 3 | The display froze for up to 10 s after `BtnB` | Route lookup ran synchronously inside the button handler | Route lookup moved to a background task; the UI and buttons keep running |
| 4 | A single route timeout left `- > -` permanently | `routeTried` was set before the request | Failed lookups are retried after 60 s, at most 3 times per aircraft |
| 5 | Entering setup mode again lost the stored values | The form was not prefilled | SSID, radius and User-Agent are prefilled from NVS (password is never prefilled) |
| 6 | Invalid values could be stored (`lat=abc`, radius 9999) | No server-side validation | Strict parsing and range checks with an error page |
| 7 | Clock wrong twice a year | Fixed UTC offset in the sketch | `configTzTime()` with `TZ_INFO` (`CET-1CEST,M3.5.0,M10.5.0/3`), daylight saving automatic |
| 8 | Firmware version was not visible anywhere | Only a hardcoded serial string | `FW_VERSION` shown in the footer, the setup page and the serial banner |

### 7.2 Build

- `platformio.ini` – reproducible build with pinned `M5Unified` and `ArduinoJson` versions, 8 MB partition table for the Plus 2.
- GitHub Actions workflow compiles the firmware on every push and pull request.
- Build artifacts (`nearPlane/build/`) and `.DS_Store` removed from git and ignored.

### 7.3 Behaviour changes

- The setup access point switches off after 10 minutes of inactivity (paused while charging).
- Wi-Fi is reconnected automatically; after 10 minutes without a connection the device returns to setup mode.
- The route lookup runs in a background task, so the display and buttons stay responsive; failed lookups are retried (60 s, max 3 attempts).
- The setup form is prefilled from the stored settings and validated on the device.
- The footer shows the firmware version; the clock handles daylight saving automatically.

## 8. v4.2 – data quality (phase 1 of 3)

### 8.1 What changed

| # | Before | Now |
|---|---|---|
| 1 | `category` was parsed and thrown away | Page 11 shows the emitter category with a readable label (`A3 Large`, `A7 Rotorcraft`, `B6 UAV`, `C1 Emergency veh`…) |
| 2 | Only the military bit of `dbFlags` was used | `PIA` and `LADD` privacy flags are shown as badges; military alerts still work |
| 3 | Position source was invisible | `ADS-B` / `MLAT` / `TIS-B` / `ADS-C` is derived from `type` and the `mlat` / `tisb` arrays; shown on page 11 |
| 4 | Stale targets stayed on screen with dead reckoning | Positions older than 60 s are dimmed; aircraft older than 180 s are dropped (`seen_pos`) |
| 5 | Units were hardcoded to km/ft | `BtnC` short press toggles km/ft ↔ NM/FL (outside the map), stored in NVS; also selectable in setup |
| 6 | Ground vehicles and parked aircraft could be the "closest" | Setup option **Hide aircraft on the ground** (default on) |
| 7 | No data quality view | New page 11: source, age, NIC/RC, NACp/SIL, ADS-B version, QNH/FMS altitude, wind components, registration country |
| 8 | Country was unknown | Derived from the registration prefix (`OK-`, `D-`, `OM-`, `N-`, …); no unverified hex table |
| 9 | No flight phase | Page 11 shows `CLB`, `CRZ`, `DES`, `APP`, `TAXI` from altitude and vertical rate |

### 8.2 New API fields

`type`, `seen_pos`, `mlat`, `tisb`, `nic`, `rc`, `nac_p`, `sil`, `sil_type`, `version`, `nav_qnh`, `nav_altitude_fms`, `geom_rate`, `alert`, `spi`. `seen` is requested for diagnostics.

### 8.3 Notes

- `seen_pos` is a floating-point value in the API (e.g. `0.096`); the firmware stores whole seconds.
- Emitter category, NIC/RC and NACp/SIL are only present when the data source provides them; otherwise the page shows `---`.
- Country is intentionally derived from the registration, not from the ICAO hex range, because no authoritative hex table was available for verification.

### 8.4 UI clean-up (after first hardware test)

- The bottom status line was removed completely on user request: page number, firmware version, `AC x/y` counter, flight phase and quality badges are no longer drawn. The display now contains only aircraft data; the top alert banner stays.
- Flight phase moved to page 11 (first row: source + phase + position age).
- The altitude label on page 1 reads `ALT (FL)` in NM/FL mode and `ALT (ft)` otherwise, so the unit switch is visible.
- The "Units" toast is shown for 2.5 s and every toggle is logged as `[UI] units=...`.

## 9. v4.2 – watchlist and alert engine (phase 2 of 3)

### 9.1 Watchlist

- Comma separated rules in the setup form (max 10): `OK-*` (registration prefix), `@DLH` (callsign prefix), `A388` (exact type), `4CA123` (exact hex). A trailing `*` means prefix match, otherwise exact.
- Matching is case-insensitive across registration, callsign, type and hex; stored in NVS under `watch`.
- A match raises level `WATCH` (magenta), selects the aircraft and plays a dedicated sound. Parsed rules are logged as `[WATCH] N rule(s)`.

### 9.2 Alert engine

| Priority | Event | Colour | Trigger |
|---|---|---|---|
| 1 | EMERGENCY | red | emergency flag or squawk 7500/7600/7700 |
| 2 | CPA | red | CPA within the configured distance/time (default 2 km / 2 min) |
| 3 | OVERHEAD | yellow | elevation above the configured angle (default 70°) |
| 4 | MILITARY | orange | `dbFlags` military bit |
| 5 | WATCH | magenta | watchlist match |
| 6 | RARE | cyan | hardcoded rare type list |

- All events are evaluated after every poll; the highest priority wins and only one banner is shown.
- Dedupe is per aircraft **and** event type for 5 minutes (ring of 12), so a CPA is not repeated every 15 s.
- CPA and overhead values are shown in the header line (`CPA 1.4 km / 90s`, `OVERHEAD 78deg 35000 ft`) and the banner stays for 10 s.
- Quiet hours (default 22:00–07:00, local time from NTP) suppress sounds only; banners and selection still happen.
- Every alert type has its own on/off switch in the setup form; thresholds are validated on the device.

### 9.3 Notes

- The legacy 8-entry `alertedHex` ring was replaced by a 12-entry ring with an event type and expiry.
- New NVS keys: `watch`, `cpaKm`, `cpaMin`, `elev`, `qfrom`, `qto`, `flags`.
- New serial lines: `[WATCH]`, `[ALERT] <callsign> <text>`.

## 10. v4.2 – storage, caches and OTA (phase 3 of 3)

### 10.1 Flash layout

- Custom `partitions_8mb_ota.csv`: NVS 20 kB, otadata 8 kB, app0/app1 3 MB each, LittleFS 1.94 MB.
- OTA is only offered when a second application slot exists (`esp_ota_get_next_update_partition()`), so the firmware also runs on the old single-slot layout with OTA disabled.
- The first flash of the new layout must be done over USB; NVS (settings) is preserved because its offset does not change.

### 10.2 Sightings log (LittleFS)

- `/log.csv`: `time,hex,callsign,reg,type,alt,gs,dist_km,source`, one row per aircraft per 5 minutes.
- Rotation at 256 kB (`/log.csv` → `/log.old.csv`, the previous old file is deleted).
- Setup page: `GET /log.csv` (download), `POST /log/clear`, log size in kB and an on/off switch (NVS `log`).
- Rows are written only after NTP synchronisation; LittleFS is mounted even when logging is disabled so the file can still be downloaded or cleared.

### 10.3 Caches

- Route cache: 24 entries `callsign → dep/arr`, 7 days, NVS `rcache`. A cache hit sets `routeTried` without any network request.
- Enrichment cache: 24 entries `hex → long type name`, 30 days, NVS `ecache`.
- Both caches are loaded at boot and logged as `[CACHE]`.

### 10.4 Enrichment

- When the selected aircraft has no long type name and its type is missing from `TYPE_NAMES`, the background task requests `api.adsb.lol/v2/hex/{hex}` and caches `desc` (fallback `ownOp`).
- Only one enrichment request per selected aircraft, skipped while a route request is in flight and while the remaining quota is below 50.

### 10.5 Data-source quota

- `x-ratelimit-remaining` and `x-ratelimit-reset` are parsed from every response.
- Below 20 remaining requests the primary source is blocked and the fallback is used; below 5 polling slows to 60 s.
- All changes are visible in the serial log as `[RATE]`.

### 10.6 OTA

- Setup mode offers `/update` (GET page, POST multipart upload) using `Update.h`; the device reboots after a successful upload.
- Holding `BtnA` while powering on enters setup mode without a factory reset, so OTA and reconfiguration do not need a settings wipe.
- Firmware version and OTA availability are shown on the update page.

### 10.7 Notes

- Batch `routeset` requests were **not** implemented: the response mapping could not be verified, so the cache reduces repeat requests instead.
- New NVS keys: `log`, `rcache`, `ecache`.
- New serial prefixes: `[LOG]`, `[CACHE]`, `[ENRICH]`, `[RATE]`, `[OTA]`, `[SETUP]`.

---

<a id="slovenčina"></a>
# Slovenčina

## 1. Opravy

| # | Problém | Príčina | Oprava |
|---|---|---|---|
| 1 | HTTP **403** – „User-Agent too generic“ | `HTTPClient` vždy posiela vlastný `User-Agent: ESP32HTTPClient`; `addHeader("User-Agent", …)` pridal len druhú hlavičku, server čítal tú všeobecnú | Použité `http.setUserAgent(userAgent)` s reálnym kontaktom z formulára; `USER_AGENT` je len záloha |
| 2 | Nastavenie TLS spojenia | Čistý `HTTPClient` bez zabezpečeného klienta | `WiFiClientSecure` so `setInsecure()` (bez overenia certifikátu) |
| 3 | Route API sa volalo každé 2 s | `lastFlightCodeForDetails` sa nikde nenastavovala, kontrola „už stiahnuté“ nikdy nesedela | Trasa sa sťahuje raz pre každé lietadlo a ukladá sa |
| 4 | Zavádzajúci formulár | Rádius v adsb.lol je v **nautických míľach** (max 250), formulár písal km | Popisok zmenený na „Scan Radius (NM, max 250)“ |
| 5 | HTTP **429** – rate limit | Dopyt každé 2 s | Dlhšie intervaly (tabuľka nižšie); pri 429 ostanú posledné dáta na displeji a polling sa spomalí; finálne správanie backoffu a fallbacku: viď #10 |
| 6 | Chyba kompilácie `StreamReader was not declared` | Arduino IDE generuje prototypy funkcií nad prvou funkciou; trieda bola definovaná až za ňou | `StreamReader` presunutý nad všetky funkcie k ostatným typom |
| 7 | HTTP **426** – Upgrade Required | Server odmietol `useHTTP10(true)` (vyžaduje HTTP/1.1) | Odstránené; čítač streamu si `Transfer-Encoding: chunked` dekóduje sám |
| 8 | Žiadny výpis v sériovej konzole | Skrátený štart; monitor sa mohol pripojiť neskoro | Firmvér čaká až 3 s na sériový port a vypíše úvodný riadok (`nearPlane+: start (v4)`) |
| 9 | Príliš veľa ladiacich výpisov | Pri každom dopyte sa vypisovalo celé telo odpovede | Len stručné stavové riadky; telo odpovede iba pri chybe |
| 10 | Zariadenie sa po štarte „nerozbehne“: v logu je HTTP 429 a displej 120 s ostane na „Fetching aircraft…“ | Pri prvom dopyte sa prekročil limit adsb.lol, displej nič neukázal, pevná čakacia doba 120 s | Odpočet „Rate limited – retry in N s“, exponenciálny backoff 20→120 s, podpora `Retry-After` a automatický prechod na záložný server `opendata.adsb.fi` (airplanes.live sa skúšalo ako prvé, ale neznámych klientov blokuje kódom 403) |

## 2. Pridané funkcie

### 2.1 Stránka smeru a vzdialenosti (stránka 7)
- Šípka smerujúca k lietadlu (sever je hore, zariadenie nemá kompas).
- Vzdialenosť v km, aktualizovaná každú sekundu pomocou **dead reckoning** (poloha sa medzi API volaniami extrapoluje z rýchlosti a kurzu).
- Trend: **APPROACHING** (zelená) / **RECEDING** (červená) / **STEADY** (žltá) podľa radiálnej rýchlosti.
- Azimut v stupňoch a svetová strana (napr. `BRG 245 SW`).

### 2.2 CPA – najbližšie priblíženie
- Počíta sa z kurzu a rýchlosti.
- Zobrazí `CPA 3.2 km` a čas do priblíženia (`in 0:45`), alebo `PASSED`, ak sa lietadlo už vzďaľuje.

### 2.3 Alerty pre zaujímavé lietadlá
| Úroveň | Farba | Spúšťač |
|---|---|---|
| 3 – EMERGENCY | červená | núdzový príznak alebo squawk 7500 / 7600 / 7700 |
| 2 – MILITARY | oranžová | vojenský bit v `dbFlags` |
| 1 – RARE | azúrová | A380, B747-400/-8/SP, An-124, An-225, Beluga/BelugaXL, Dreamlifter, C-5M, Il-76, A340-600 |

- Farebný pruh za číslom letu (biely text na červenej, čierny na oranžovej a azúrovej kvôli čitateľnosti), popis v pätičke, iný zvuk pre každú úroveň.
- Lietadlo s alertom sa automaticky vyberie a displej skočí na stránku 1.
- To isté lietadlo alert nespustí dvakrát (pamätá sa posledných 8).
- Lietadlá s alertom sa držia v zozname 5 aj vtedy, keď nie sú najbližšie.

### 2.4 Viac lietadiel
- Endpoint zmenený z `/v2/closest` na `/v2/point`; sleduje sa 5 najbližších lietadiel.
- Odpoveď sa **číta streamom a parsuje po jednom lietadle**, takže pamäť ostáva nízka aj pri desiatkach lietadiel v dosahu.
- Indikátor v pätičke `AC 2/5`; výber sa pri obnove dát zachová (podľa ICAO hex).

### 2.5 Radar mapa (stránka 8)
- Pohľad zhora, sever hore, ty v strede, dve kružnice vzdialenosti.
- Až 40 lietadiel ako bodky s krátkou čiarou smeru; polohy sa hýbu každú sekundu.
- Farba podľa úrovne alertu; vybrané lietadlo je obkrúžené žltou.
- Ak je vybrané lietadlo mimo aktuálnej mierky, na okraji kružnice je žltý bod v jeho smere a v pätičke `SEL 24.1 km`.
- **Cyklus mierky:** celý nastavený rádius → 25 km → 12,5 km → 7,5 km → odznova (vnútorná kružnica je vždy polovica). Kroky väčšie ako nastavený rádius sa preskočia.

### 2.6 Zoznam blízkych lietadiel (stránka 9)
- 5 najbližších lietadiel s volacím znakom, vzdialenosťou (zelená = približuje sa, červená = vzďaľuje) a výškou (`FL360`, `3200ft`, `GND`).
- Vybraný riadok je zvýraznený; lietadlá s alertom majú farbu alertu.

### 2.7 Denná štatistika (stránka 10)
- Počet lietadiel (unikátne ICAO hex), najbližšie, najvyššie a najrýchlejšie lietadlo dňa.
- Ukladá sa do flash pamäte (NVS) každých 10 minút a pred uspatím; prežije reštart; o polnoci sa vynuluje.
- Vyžaduje synchronizovaný čas (NTP).

### 2.8 Názvy typov a aerolínií
- ICAO kód typu → čitateľný názov (`B738` → `Boeing 737-800`), vstavaných približne 100 typov.
- Prefix volacieho znaku → aerolínia (`RYR1234` → `Ryanair`), vstavaných približne 80 aerolínií.
- Hlavička ukazuje `Aerolínia | Typ`; ak aerolínia nie je známa, ukáže sa registrácia. Riadok, ktorý sa nezmestí, sa posúva ako bežiaci text (40 px/s, 1,5 s pauza na začiatku).
- Neznámy typ sa nahradí poľom `desc` z API, potom ICAO kódom. Tabuľky `TYPE_NAMES` / `AIRLINES` môžeš rozšíriť v zdrojovom kóde.

### 2.9 Zapnutie/vypnutie bzučiaka
- Podržaním **BtnC** na 0,75 s (750 ms) stlmíš alebo zapneš všetky zvuky (štartovací tón, nové lietadlo, alerty).
- Zobrazí sa hlásenie „Sound ON / Sound OFF“; pri vypnutom zvuku je vedľa batérie prečiarknutý reproduktor.
- Nastavenie sa ukladá do flash pamäte a prežije reštart; reset nastavení vráti zvuk zapnutý.

### 2.10 Nastaviteľný User-Agent
- Nové pole **User-Agent** vo webovom formulári nastavenia zariadenia, ukladá sa do flash pamäte.
- Formulár aj server vyžadujú e-mail alebo URL, zámerne zamietnu vzorovú hodnotu a limit 120 znakov; zalomenia riadkov sa odstránia.
- Vstavaná konštanta `USER_AGENT` je už len dummy záloha (`changeme@example.com`) a nie je platný kontakt.
- Uloženú hodnotu zmeníš resetom nastavení a novým spustením setupu.

### 2.11 GPS modul (voliteľný)
- Podporuje M5Stack GPS Unit v1.1 (AT6668 + MAX2659) na porte Grove; funguje aj staršia jednotka s AT6558.
- Orientácia vodičov (G32/G33) a rýchlosť (115200 / 9600 baud) sa zistia automaticky z platných NMEA viet (kontroluje sa kontrolný súčet).
- Živá poloha z GPS nahradí uložené súradnice: dopyt na API, vzdialenosti, smery aj radar mapa ju používajú; fix starý do 10 min sa drží, potom sa použije uložená poloha.
- Formulár nastavenia: zemepisná šírka a dĺžka sa predvyplnia z GPS, nový checkbox **Use GPS module if connected**.
- Indikátor na displeji `GPS` (živý fix) / `GPS?` (modul nájdený, bez fixu) pod hodinami; sériový log ukazuje `gps=...` a zdroj polohy.

### 2.12 Ostatné zmeny
- Stránka 5: riadok `CAT` nahradený riadkom `REG` (registrácia), keďže ju hlavička pri známej aerolínii už neukazuje.
- Trasa (`DEP > ARR`) sa sťahuje len pre vybrané lietadlo a len raz.
- Doba držania BtnC pre prepnutie bzučiaka skrátená z 2 s na 0,75 s (750 ms).

## 3. Ovládanie

| Tlačidlo | Akcia | Kde |
|---|---|---|
| **BtnA** (predné) krátko | ďalšia stránka | všetky stránky |
| **BtnA** dlho (0,6 s) | skok na detail vybraného lietadla (stránka 1) | radar mapa |
| **BtnC** (vypínacie tlačidlo, bočné) krátko | zmena mierky mapy | radar mapa |
| **BtnC** držať 0,75 s | zapnutie/vypnutie bzučiaka (uloží sa) | všetky stránky |
| **BtnB** (bočné) krátko | ďalšie lietadlo | všetky stránky s lietadlom |
| **BtnB** krátko | ďalšie lietadlo **v rámci aktuálnej mierky** | radar mapa |
| **BtnB** držať 1 s → 5 s | odpočet, potom reset nastavení | všade (predčasným pustením sa zruší) |

BtnC je tretie tlačidlo M5StickC Plus2 (vypínacie tlačidlo, v knižnici `M5.BtnPWR`).

## 4. Stránky

| # | Obsah |
|---|---|
| 1 | Výška, rýchlosť voči zemi, trasa |
| 2 | Kurz, vertikálna rýchlosť, náklon |
| 3 | Mach, IAS/TAS, geometrická výška |
| 4 | Výška, kurz a režimy autopilota |
| 5 | Squawk, ICAO hex, registrácia |
| 6 | OAT/TAT, vietor, RSSI |
| 7 | Šípka, vzdialenosť, trend, CPA |
| 8 | Radar mapa |
| 9 | Zoznam blízkych lietadiel |
| 10 | Denná štatistika |

## 5. Intervaly volania API

| Situácia | Interval |
|---|---|
| Lietadlo nájdené | 15 s |
| Žiadne lietadlo | 30 s |
| Iná chyba (parse, 5xx, timeout) | 60 s |
| HTTP 429 | 20 s, zdvojnásobuje sa až na 120 s (`Retry-After` má prednosť) + záložný server |
| Prekreslenie stránok 7/8/9 (lokálny výpočet, bez API) | 1 s |

Konštanty sú na začiatku súboru (`SHORT_POLL_INTERVAL` a ďalšie).

## 6. Poznámky a obmedzenia

- Skeč musí byť uložený ako `nearPlane/nearPlane.ino` (názov priečinka = názov súboru), inak linker hlási nedefinované `setup`/`loop`.
- Zariadenie nemá kompas: šípky a mapa sú vždy orientované severom nahor, aj keď sa mení poloha z GPS.
- GPS potrebuje fix zo satelitov (približne 25 s vonku); dovtedy, alebo keď modul nie je nájdený, sa používajú súradnice uložené v nastavení.
- adsb.lol vyžaduje v User-Agent reálny kontakt – zadaj ho vo formulári nastavenia (pole **User-Agent**).
- Rádius sa zadáva v nautických míľach (1 NM = 1,852 km).
- Štatistika a hodiny potrebujú funkčné NTP pripojenie.
- `setInsecure()` vypína overovanie TLS certifikátu; pre verejné letové dáta je to prijateľné, pre citlivú komunikáciu nie.

## 7. v4.1 – robustnosť a build

### 7.1 Opravy

| # | Problém | Príčina | Oprava |
|---|---|---|---|
| 1 | Setup AP bežal donekonečna a vybíjal batériu | Vetva `configMode` v `loop()` nekontrolovala timeout neaktivity | Setup AP sa po 10 min neaktivity vypne (pri nabíjaní nie) a zariadenie ide do deep sleep |
| 2 | Po výpadku Wi-Fi sa zariadenie už nepripojilo | `WiFi.begin()` sa volal len raz pri boote | `ensureWifi()` reconnectuje s backoffom 5–60 s; po 10 min bez spojenia znovu otvorí setup AP |
| 3 | Displej zamrzol až na 10 s po `BtnB` | Route lookup bežal synchrónne v handleri tlačidla | Route lookup presunutý do background tasku; UI a tlačidlá idú ďalej |
| 4 | Jeden timeout trasy nechal `- > -` navždy | `routeTried` sa nastavil pred requestom | Neúspešný lookup sa opakuje po 60 s, max. 3× pre lietadlo |
| 5 | Opätovný vstup do setupu stratil uložené hodnoty | Formulár sa nepredvypĺňal | SSID, radius a User-Agent sa predvypĺňajú z NVS (heslo sa nepredvypĺňa) |
| 6 | Do NVS sa dali uložiť nezmysly (`lat=abc`, radius 9999) | Chýbala server-side validácia | Striktné parsovanie a kontroly rozsahov s chybovou stránkou |
| 7 | Hodiny boli 2× ročne zle | Pevný UTC offset v skeči | `configTzTime()` s `TZ_INFO` (`CET-1CEST,M3.5.0,M10.5.0/3`), letný čas automaticky |
| 8 | Verzia firmvéru nebola nikde vidieť | Len natvrdo v serial output | `FW_VERSION` v pätičke, v setup stránke a v serial banneri |

### 7.2 Build

- `platformio.ini` – reprodukovateľný build s pinovanými verziami `M5Unified` a `ArduinoJson`, 8 MB partition table pre Plus 2.
- GitHub Actions workflow kompiluje firmware pri každom pushi a pull requeste.
- Build artefakty (`nearPlane/build/`) a `.DS_Store` odstránené z gitu a ignorované.

### 7.3 Zmeny správania

- Setup access point sa po 10 min neaktivity vypne (pri nabíjaní nie).
- Wi-Fi sa obnovuje automaticky; po 10 min bez spojenia sa zariadenie vráti do setup módu.
- Route lookup beží v background tasku, takže displej a tlačidlá reagujú; neúspešné pokusy sa opakujú (60 s, max. 3×).
- Setup formulár sa predvypĺňa z uložených hodnôt a validuje priamo v zariadení.
- Pätička zobrazuje verziu firmvéru; hodiny zvládajú letný/zimný čas automaticky.

## 8. v4.2 – kvalita dát (fáza 1 z 3)

### 8.1 Čo sa zmenilo

| # | Predtým | Teraz |
|---|---|---|
| 1 | `category` sa parsovala a zahadzovala | Stránka 11 zobrazuje kategóriu s čitateľným popisom (`A3 Large`, `A7 Rotorcraft`, `B6 UAV`, `C1 Emergency veh`…) |
| 2 | Z `dbFlags` sa používal len military bit | `PIA` a `LADD` privacy flagy sú zobrazené na stránke 11; military alerty fungujú ďalej |
| 3 | Zdroj pozície nebol vidieť | `ADS-B` / `MLAT` / `TIS-B` / `ADS-C` z poľa `type` a polí `mlat` / `tisb`; zobrazené na stránke 11 |
| 4 | Zastarané ciele ostávali na obrazovke s dead reckoning | Pozícia staršia ako 60 s je zosivená; lietadlo staršie ako 180 s sa vyradí (`seen_pos`) |
| 5 | Jednotky boli natvrdo km/ft | `BtnC` short prepína km/ft ↔ NM/FL (mimo mapy), uložené v NVS; aj v setup formulári |
| 6 | Ground vozidlá a odstavené lietadlá mohli byť „najbližšie" | Setup voľba **Hide aircraft on the ground** (default on) |
| 7 | Nebol prehľad o kvalite dát | Nová stránka 11: zdroj, vek, NIC/RC, NACp/SIL, ADS-B verzia, QNH/FMS výška, zložky vetra, krajina registrácie |
| 8 | Krajina nebola známa | Odvodená z prefixu registrácie (`OK-`, `D-`, `OM-`, `N-`, …); žiadna neoverená hex tabuľka |
| 9 | Nebola fáza letu | Stránka 11 zobrazuje `CLB`, `CRZ`, `DES`, `APP`, `TAXI` z výšky a vertikálnej rýchlosti |

### 8.2 Nové polia API

`type`, `seen_pos`, `mlat`, `tisb`, `nic`, `rc`, `nac_p`, `sil`, `sil_type`, `version`, `nav_qnh`, `nav_altitude_fms`, `geom_rate`, `alert`, `spi`. `seen` sa žiada pre diagnostiku.

### 8.3 Poznámky

- `seen_pos` je v API desatinné číslo (napr. `0.096`); firmware ukladá celé sekundy.
- Kategória, NIC/RC a NACp/SIL sú dostupné len ak ich zdroj dát poskytuje; inak stránka zobrazí `---`.
- Krajina sa zámerne odvodzuje z registrácie, nie z ICAO hex rozsahu – overiteľná hex tabuľka nebola k dispozícii.

### 8.4 Vyčistenie UI (po prvom HW teste)

- Spodný stavový riadok bol na žiadosť odstránený celý: číslo strany, verzia firmvéru, počítadlo `AC x/y`, fáza letu ani quality badge sa už nekreslia. Displej obsahuje len dátové polia; horný alert banner ostáva.
- Fáza letu sa presunula na stránku 11 (prvý riadok: zdroj + fáza + vek pozície).
- Label výšky na stránke 1 je v režime NM/FL `ALT (FL)`, inak `ALT (ft)`, takže prepnutie jednotiek je vidieť.
- Toast „Units" je zobrazený 2,5 s a každé prepnutie sa loguje ako `[UI] units=...`.

## 9. v4.2 – watchlist a alert engine (fáza 2 z 3)

### 9.1 Watchlist

- Pravidlá oddelené čiarkou v setup formulári (max. 10): `OK-*` (prefix registrácie), `@DLH` (prefix callsignu), `A388` (presný typ), `4CA123` (presný hex). Hviezdička na konci = prefix, inak presná zhoda.
- Porovnanie je case-insensitive cez registráciu, callsign, typ a hex; ukladá sa do NVS pod kľúčom `watch`.
- Zhoda vyvolá level `WATCH` (magenta), vyberie lietadlo a prehrá vlastný tón. Parsované pravidlá sa logujú ako `[WATCH] N rule(s)`.

### 9.2 Alert engine

| Priorita | Event | Farba | Spúšťač |
|---|---|---|---|
| 1 | EMERGENCY | červená | emergency flag alebo squawk 7500/7600/7700 |
| 2 | CPA | červená | CPA v nastavenej vzdialenosti/čase (default 2 km / 2 min) |
| 3 | OVERHEAD | žltá | elevácia nad nastavený uhol (default 70°) |
| 4 | MILITARY | oranžová | military bit v `dbFlags` |
| 5 | WATCH | magenta | zhoda s watchlistom |
| 6 | RARE | azúrová | pevný zoznam vzácnych typov |

- Všetky eventy sa vyhodnocujú po každom pollе; vyhráva najvyššia priorita a zobrazí sa len jeden banner.
- Dedupe je pre lietadlo **a** typ eventu na 5 minút (ring 12), takže CPA sa neopakuje každých 15 s.
- Hodnoty CPA a overhead sú v riadku pod callsignom (`CPA 1.4 km / 90s`, `OVERHEAD 78deg 35000 ft`), banner ostáva 10 s.
- Tiché hodiny (default 22:00–07:00, lokálny čas z NTP) potláčajú len zvuk; banner a výber fungujú ďalej.
- Každý typ alertu má vlastný prepínač v setup formulári; prahy sa validujú v zariadení.

### 9.3 Poznámky

- Pôvodný 8-prvkový ring `alertedHex` nahradil 12-prvkový ring s typom eventu a expiráciou.
- Nové NVS kľúče: `watch`, `cpaKm`, `cpaMin`, `elev`, `qfrom`, `qto`, `flags`.
- Nové serial riadky: `[WATCH]`, `[ALERT] <callsign> <text>`.

## 10. v4.2 – úložisko, cache a OTA (fáza 3 z 3)

### 10.1 Layout flash

- Vlastná `partitions_8mb_ota.csv`: NVS 20 kB, otadata 8 kB, app0/app1 po 3 MB, LittleFS 1,94 MB.
- OTA sa ponúka len ak existuje druhý app slot (`esp_ota_get_next_update_partition()`), takže firmware beží aj na starom single-slot layoute s vypnutou OTA.
- Prvý flash nového layoutu musí ísť cez USB; NVS (nastavenia) ostáva, pretože offset sa nemení.

### 10.2 Log pozorovaní (LittleFS)

- `/log.csv`: `time,hex,callsign,reg,type,alt,gs,dist_km,source`, jeden riadok na lietadlo za 5 minút.
- Rotácia pri 256 kB (`/log.csv` → `/log.old.csv`, predchádzajúci old sa maže).
- Setup stránka: `GET /log.csv` (download), `POST /log/clear`, veľkosť logu v kB a on/off prepínač (NVS `log`).
- Riadky sa zapisujú len po NTP synchronizácii; LittleFS sa mountuje aj pri vypnutom logovaní, aby sa dal súbor stiahnuť alebo zmazať.

### 10.3 Cache

- Route cache: 24 položiek `callsign → dep/arr`, 7 dní, NVS `rcache`. Cache hit nastaví `routeTried` bez sieťového requestu.
- Enrichment cache: 24 položiek `hex → dlhý názov typu`, 30 dní, NVS `ecache`.
- Obe cache sa načítajú pri boote a logujú ako `[CACHE]`.

### 10.4 Enrichment

- Ak vybrané lietadlo nemá dlhý názov typu a jeho typ chýba v `TYPE_NAMES`, background task zavolá `api.adsb.lol/v2/hex/{hex}` a uloží `desc` (fallback `ownOp`).
- Max. jeden enrichment request na vybrané lietadlo, vynechá sa počas route requestu a pri zvyšnej kvóte pod 50.

### 10.5 Kvóta zdroja dát

- `x-ratelimit-remaining` a `x-ratelimit-reset` sa parsujú z každej odpovede.
- Pod 20 zvyšných requestov sa primary zablokuje a použije sa fallback; pod 5 sa polling spomalí na 60 s.
- Všetky zmeny sú v serial logu ako `[RATE]`.

### 10.6 OTA

- Setup mód ponúka `/update` (GET stránka, POST multipart upload) cez `Update.h`; po úspešnom uploade sa zariadenie rebootne.
- Podržanie `BtnA` pri zapnutí vstúpi do setup módu bez factory resetu, takže OTA ani rekonfigurácia nevyžadujú vymazanie nastavení.
- Verzia firmvéru a dostupnosť OTA sú zobrazené na update stránke.

### 10.7 Poznámky

- Batch `routeset` requesty sa **neimplementovali**: mapovanie odpovede sa nedalo overiť, preto opakovanie requestov rieši cache.
- Nové NVS kľúče: `log`, `rcache`, `ecache`.
- Nové serial prefixy: `[LOG]`, `[CACHE]`, `[ENRICH]`, `[RATE]`, `[OTA]`, `[SETUP]`.
