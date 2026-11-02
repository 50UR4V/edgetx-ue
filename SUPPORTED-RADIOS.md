# Supported radios

**UltraEdge is made for any EdgeTX radio.** The firmware is EdgeTX with the UltraEdge companion overlay built in, so
it builds for every radio EdgeTX supports. This page shows which radios have a ready-made build and who has checked
that it works on the real radio.

Firmware **v1.2.0** for every radio below (app **1.6.2**, or **1.6.3 beta**). EdgeTX base `90bbce17`.

Hardware-verified on v1.2.0 by the owner on 2026-11-01: Pocket, Zorro and QX7 (companion) and TX16S (full). The
other 52 are built, not hardware-tested yet. A radio is marked verified when we, a tester or the community run the
checklist below on the real radio.

## Status key

| Status | Meaning |
|---|---|
| ✅ Verified (owner) | The UltraEdge team ran the full checklist below on this radio. |
| ✅ Verified (tester: name) | A beta tester ran the checklist and reported it. |
| ✅ Verified (community: link) | Someone reported a passing checklist in a public GitHub issue. |
| 🔨 Built, not yet verified | Firmware builds and passes our build checks, but nobody has run it on this radio yet. **Help us verify it.** |
| ❌ Not supported yet (reason) | No working build yet, and why. |

*Smoke-tested* means the owner flashed that build and used it, without running the full checklist.

**Build** says which firmware you get:
- **Companion**: the radio's own model and radio-settings menus are removed and live in the app. The radio still
  flies, shows its main view and lets you pick a model. Used on every black-and-white screen radio, because those
  radios have 512 KB–1 MB of flash and the full menus plus UltraEdge don't fit on most of them.
- **Full**: all of EdgeTX's own menus stay, and the app works alongside them. Used on every colour-screen radio.
- `.uf2` radios (H750 chip) flash a `.uf2` file instead of a `.bin`.

## Radios

**Today: 4 verified · 52 built, not yet verified · 4 not supported.**

| Radio | MCU · flash | Screen | Build | Status |
|---|---|---|---|---|
| BETAFPV LiteRadio 3 Pro | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Fatfish F16 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FlySky Elysium EL18 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FlySky Nirvana NV14 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FlySky Noble NB4+ | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FlySky PA01 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| FlySky Paladin PL18 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FlySky Paladin PL18EV | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FlySky Paladin PL18U | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FlySky ST16 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| FrSky Horus X10 / X10S | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FrSky Horus X10 / X10S Express | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FrSky Horus X12S | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| FrSky Tandem X18 / X18S / X18SE | — | Colour | — | ❌ Not supported (no EdgeTX target; runs FrSky ETHOS only) |
| FrSky Tandem X20 / X20S / X20 HD (Horus X20 family) | — | Colour | — | ❌ Not supported (no EdgeTX target; runs FrSky ETHOS only) |
| FrSky Twin X14 / X14S / X14RS | — | Colour | — | ❌ Not supported (no EdgeTX target; runs FrSky ETHOS only) |
| FrSky Twin X-Lite / X-Lite S / X-Lite RII | — | Colour | — | ❌ Not supported (no EdgeTX target; runs FrSky ETHOS only) |
| FrSky Taranis Q X7 / Q X7S | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | ✅ Verified (owner) on v1.2.0 companion build, 2026-11-01 (see Known issue) |
| FrSky Taranis Q X7 ACCESS | STM32 F407xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X-Lite | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X-Lite S / Pro | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X9 Lite | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X9 Lite S | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X9D | STM32 F205xE · 512 KB | 212×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X9D Plus | STM32 F205xE · 512 KB | 212×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X9D Plus 2019 | STM32 F407xE · 512 KB | 212×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| FrSky Taranis X9E | STM32 F407xE · 512 KB | 212×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| HelloRadioSky V12 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| HelloRadioSky V14 | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| HelloRadioSky V14 LCD | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| HelloRadioSky V16 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| iFlight Commando 14 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| iFlight Commando 8 | STM32 F407xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper Bumblebee | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T-Lite (v1) | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T-Pro (v1) | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T-Pro S | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T-Pro V2 | STM32 F407xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T12 / T12 Pro | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T12 MAX | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T14 | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T15 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| Jumper T15 Pro | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| Jumper T16 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| Jumper T18 | STM32 F429xI · 2 MB | Colour | Full (.bin) | 🔨 Built, not yet verified |
| Jumper T20 | STM32 F407xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T20 V2 | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| Jumper T22 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| RadioMaster Boxer | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| RadioMaster GX12 | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| RadioMaster GX15 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| RadioMaster MT12 | STM32 F407xG · 1 MB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| RadioMaster Pocket | STM32 F407xE · 512 KB | 128×64 mono | Companion (.bin) | ✅ Verified (owner) on v1.2.0 companion build, 2026-11-01 |
| RadioMaster T8 Pro / T8 Lite | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| RadioMaster TX12 | STM32 F205xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| RadioMaster TX12 MKII | STM32 F407xE · 512 KB | 128×64 mono | Companion (.bin) | 🔨 Built, not yet verified |
| RadioMaster TX15 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| RadioMaster TX16S / TX16S MKII | STM32 F429xI · 2 MB | Colour | Full (.bin) | ✅ Verified (owner) on v1.2.0 full build, 2026-11-01 |
| RadioMaster TX16S MK3 | STM32 H750xB · 8 MB QSPI | Colour | Full (.uf2) | 🔨 Built, not yet verified |
| RadioMaster Zorro | STM32 F407xE · 512 KB | 128×64 mono | Companion (.bin) | ✅ Verified (owner) on v1.2.0 companion build, 2026-11-01 |

Notes:
- **FrSky Q X7 vs Q X7 ACCESS are different radios** (STM32F205 vs STM32F407). Flash the file for your model. Only the
  non-ACCESS Q X7 is verified.
- **FrSky ETHOS radios** (Tandem X18 and X20 families, Twin X14 and X-Lite) have no EdgeTX target, so there is no
  UltraEdge build for them. The older Horus X10 / X10S / X12S do run EdgeTX and are listed above.
- RadioMaster TX16S and TX16S MKII use the same firmware. The TX16S MK3 is a different radio with its own `.uf2`.
- Radios that EdgeTX added after our base (`90bbce17`) aren't listed yet. They arrive when UltraEdge moves to a newer
  EdgeTX.

**Known issue:** the QX7 and other STM32F2 radios (MCU column `F205xE`: X9D, X9D Plus, X9 Lite, T12, TX12, T8 and
others) don't identify themselves to the app on v1.2.0, so My Radios and per-radio lists use one shared set for them.
Fixed in firmware v1.2.1.

## Help verify your radio

If your radio says **🔨 Built, not yet verified**, you can move it to ✅ in about 20 minutes.

**Before you start:** back up your SD card and your current firmware. Bench-test only, **props off**. On a
Companion build the radio's own menus are gone; to get them back, flash stock EdgeTX again.

1. **Flash** the file for your radio from the firmware release (EdgeTX Buddy, or the bootloader). Flashing guide:
   `FLASHING.md` on the release.
2. **Version page:** the radio shows `ultraedge-<your radio>` and `UE 1.2.0`. On a Companion build, open any menu:
   the "settings are in the UltraEdge app" screen shows the same version, git and date.
3. **Connect the app:** plug the phone in by USB and open UltraEdge. It connects and shows live channels.
4. **Edit and save a model:** change something small (a mix weight or a model name), save, and check it on the
   radio. Then change it back.
5. **Telemetry:** with a receiver bound, the app's telemetry screen shows live values (RSSI, voltage).
6. **Calibration:** run the app's calibration wizard (System › Hardware › Calibration). Sticks and pots read correctly after.

**Report it:** open a GitHub issue at **[50UR4V/UltraEdge-app](https://github.com/50UR4V/UltraEdge-app/issues)**
titled `Verified: <radio name>` (or `Problem: <radio name>`). Paste:
- the full contents of the `.txt` file that sits next to the firmware you flashed (it holds FW, VERS, GIT#, DATE and
  the SHA-256),
- which of steps 1–6 passed, and anything that didn't,
- your app version (System › About and licences) and phone model.

Tell us how you'd like to be credited (name or handle). We update this table when a report comes in.
