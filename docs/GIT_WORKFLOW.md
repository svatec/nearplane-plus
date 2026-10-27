# Git workflow – nearPlane+

Interná dokumentácia pre vývoj firmware. Platí pre solo vývoj aj prípadných ďalších prispievateľov.

## Princípy

- `main` je vždy stabilná, otestovaná vetva. Nikdy do nej necommituj priamo.
- Nová práca ide do krátkej vetvy z `main`, cez PR a CI build.
- Release sa robí tagom `v*`; `release.yml` z neho automaticky zostaví OTA binárku a vytvorí GitHub Release.
- Verzia firmware je v `FW_VERSION` v `nearPlane/nearPlane.ino` a v `nearPlane/CHANGELOG.md`.
- Build artefakty (`.pio/`, `firmware.bin`) sa necommitujú.

## Stratégia

Používame **GitHub Flow**:

1. `main` = produkčná stabilná verzia.
2. Krátka `feature/*`, `fix/*` alebo `experiment/*` vetva.
3. PR do `main`, CI musí byť zelené.
4. Hardvérový test z vetvy.
5. Merge do `main`.
6. Tag `vX.Y.Z` spustí release workflow.

`develop` ani plný Git Flow nepotrebujeme. `release/*` vetvy zavádzaj až vtedy, keď bude treba paralelne podporovať staršie vydania.

## Názvy vetiev

| Typ | Príklad | Účel |
|---|---|---|
| Feature | `feature/v4.5-power` | nová funkcia alebo väčší celok |
| Fix | `fix/gps-timeout` | oprava chyby |
| Experiment | `experiment/power-tuning` | meranie a ladenie, môže sa zahodiť |
| Docs | `docs/git-workflow` | dokumentácia a procesy |

## Bežný postup

```bash
cd /Users/svatec/Documents/Git/nearplane-plus

# 1. aktuálna stabilná vetva
git switch main
git pull --ff-only

# 2. nová vetva
git switch -c feature/nazov

# 3. zmeny + lokálny build
~/.platformio/penv/bin/pio run

# 4. commit (štýl repa: "Add ...", "Fix ...", "Update ...")
git add nearPlane/nearPlane.ino README.md nearPlane/CHANGELOG.md
git commit -m "Add krátky popis zmeny"

# 5. push a PR
git push -u origin feature/nazov
```

PR otvor v GitHub UI alebo cez `gh` (ak je nainštalovaný):

```bash
gh pr create --base main --head feature/nazov \
  --title "Add krátky popis zmeny" \
  --body "Čo mení, ako testované, prípadné riziká."
```

Pre prácu, ktorá ešte nie je otestovaná na hardvéri, použi **draft PR** (`--draft`).

## Pred merge – hardvérový test

- [ ] `pio run` prejde aj v CI.
- [ ] Zariadenie sa pripojí k Wi-Fi a načíta lietadlá.
- [ ] Ovládanie: `BtnA`, `BtnB`, `BtnC`, reset, podsvietenie/auto-dim.
- [ ] Zmenená funkcia funguje podľa očakávania (detaily v PR).
- [ ] Pri power zmenách: profily, `/power.csv`, deep sleep, nespí pri nabíjaní.
- [ ] Pri GPS zmenách: cold boot s modulom, bez modulu, deep-sleep wake s uloženým fixom.
- [ ] Žiadny watchdog, reboot loop ani strata NVS nastavení po upgrade.
- [ ] Aktuálna stabilná verzia ostáva dostupná na rollback.

## Merge a release

```bash
# merge cez GitHub (Squash and merge) a zmazanie vetvy
# alebo: gh pr merge --squash --delete-branch

git switch main
git pull --ff-only

# tag až po úspešnom teste a merge; formát vX.Y.Z
git tag -a v4.5.0 -m "nearPlane+ v4.5.0"
git push origin v4.5.0
```

Tag musí byť na `main` (na merge commite), nie na feature vetve. `release.yml` reaguje na tagy `v*` a vytvorí:

- `nearPlane-plus-<tag>-m5stickc-plus2-ota.bin`
- `SHA256SUMS`

Release je OTA application image. Prvý USB flash stále vyžaduje source build a správnu partition tabuľku.

## Verzovanie

- `FW_VERSION` nesie verziu v tvare `v4.5`; tag nesie `v4.5.0` (rovnaký vzor ako `FW_VERSION = v4.3` a tag `v4.3.0`).
- Opravná verzia: `FW_VERSION = v4.5.1`, tag `v4.5.1`.
- Tag nikdy nepresúvaj a neprepisuj. Ak release obsahuje chybu, vydaj novší patch tag.
- Pred tagom aktualizuj `CHANGELOG.md` (SK aj EN) a `FW_VERSION`.

## Rollback

- Stabilná verzia je tag `v4.3.0`; jej OTA binárku drž bokom.
- Pri zlom release flashni predchádzajúcu OTA binárku alebo použij USB source build.
- Novšie zariadenia s OTA partition majú dva app sloty; over, že rollback slot je funkčný.

## CI a ochrana main

- `.github/workflows/build.yml` beží na každý push a PR – PlatformIO build.
- `.github/workflows/release.yml` beží na tag `v*` – build + GitHub Release.
- V GitHub Settings → Branches nastav pre `main`:
  - require pull request before merging,
  - require status check `PlatformIO build`,
  - disallow force pushes.

## Commit štýl

Repo používa krátke imperatívne správy bez prefixov:

- `Add MIL TODAY page and power profiles (v4.5)`
- `Fix radar map scale to use radius percentages`
- `Update CHANGELOG for v4.5`

Jeden commit = jeden logický celok. Ak sa zmeny týkajú kódu aj dokumentácie, môžu byť v jednom PR, ale dokumentáciu je vhodné commitnúť samostatne.

## Power meranie z vetvy

1. Nabite zariadenie a odpojte USB.
2. V setup zapnite `Log power samples to flash`, zvoľte profil, uložte.
3. Nechajte zariadenie bežať definovaný čas (napr. 30 min).
4. V setup stiahnite `/power.csv`, spriemerujte stĺpec `mA`.
5. Rovnaký scenár zopakujte pre porovnávané profily.
6. Výsledky a `content-length` z `[AIRCRAFT]` logu pripoj k PR.
