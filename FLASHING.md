# Flashing UltraEdge firmware v1.2.0

v1.2.0 is the firmware for both the production app (**UltraEdge 1.6.0**, Google Play) and the beta (**1.6.1**, open
testing), for every EdgeTX radio we build. Four are hardware-tested: RadioMaster Pocket, Zorro and TX16S, and FrSky
QX7. The other 52 are built, not hardware-tested (see *Status* below). Back up your SD card and your current firmware
first, and bench-test with the propeller off.

**Companion** builds (every black-and-white screen) remove the radio's own model and radio-settings
menus; you change those in the app. **Full** builds (every colour screen) keep every EdgeTX menu. Want the radio's own menus on a
black-and-white radio? Use the [v1.1.2.4](https://github.com/50UR4V/edgetx-ue/releases/tag/v1.1.2.4) full builds.

## Find your file
Flash **only the file for your exact radio**. The Q X7 ACCESS is not the Q X7 and has its own file. Each file has a
`.txt` beside it that lists exactly what the radio's Version page or UltraEdge screen will show. Verify downloads with
`shasum -a 256 -c SHA256SUMS.txt`.

| File | Radio | Build | Status |
|---|---|---|---|
| `edgetx-ue-betafpv-lr3pro-v1.2.0-companion.bin` | BETAFPV LiteRadio 3 Pro | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-fatfish-f16-v1.2.0.bin` | Fatfish F16 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-el18-v1.2.0.bin` | FlySky Elysium EL18 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-nv14-v1.2.0.bin` | FlySky Nirvana NV14 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-nb4p-v1.2.0.bin` | FlySky Noble NB4+ | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-pa01-v1.2.0.uf2` | FlySky PA01 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-pl18-v1.2.0.bin` | FlySky Paladin PL18 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-pl18ev-v1.2.0.bin` | FlySky Paladin PL18EV | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-pl18u-v1.2.0.bin` | FlySky Paladin PL18U | full | 🔨 built, not hardware-tested |
| `edgetx-ue-flysky-st16-v1.2.0.uf2` | FlySky ST16 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x10-v1.2.0.bin` | FrSky Horus X10 / X10S | full | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x10express-v1.2.0.bin` | FrSky Horus X10 / X10S Express | full | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x12s-v1.2.0.bin` | FrSky Horus X12S | full | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-qx7-v1.2.0-companion.bin` | FrSky QX7 / QX7S (**not** the QX7 ACCESS) | companion | ✅ hardware-tested (owner) |
| `edgetx-ue-frsky-qx7-access-v1.2.0-companion.bin` | FrSky Taranis Q X7 ACCESS | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-xlite-v1.2.0-companion.bin` | FrSky Taranis X-Lite | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-xlites-v1.2.0-companion.bin` | FrSky Taranis X-Lite S / Pro | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x9lite-v1.2.0-companion.bin` | FrSky Taranis X9 Lite | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x9lites-v1.2.0-companion.bin` | FrSky Taranis X9 Lite S | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x9d-v1.2.0-companion.bin` | FrSky Taranis X9D | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x9dplus-v1.2.0-companion.bin` | FrSky Taranis X9D Plus | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x9dplus2019-v1.2.0-companion.bin` | FrSky Taranis X9D Plus 2019 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-frsky-x9e-v1.2.0-companion.bin` | FrSky Taranis X9E | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-helloradiosky-v12-v1.2.0.uf2` | HelloRadioSky V12 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-helloradiosky-v14-v1.2.0-companion.bin` | HelloRadioSky V14 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-helloradiosky-v14lcd-v1.2.0-companion.bin` | HelloRadioSky V14 LCD | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-helloradiosky-v16-v1.2.0.bin` | HelloRadioSky V16 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-iflight-c14-v1.2.0.uf2` | iFlight Commando 14 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-iflight-commando8-v1.2.0-companion.bin` | iFlight Commando 8 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-bumblebee-v1.2.0-companion.bin` | Jumper Bumblebee | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-tlite-v1.2.0-companion.bin` | Jumper T-Lite (v1) | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-tpro-v1.2.0-companion.bin` | Jumper T-Pro (v1) | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-tpros-v1.2.0-companion.bin` | Jumper T-Pro S | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-tprov2-v1.2.0-companion.bin` | Jumper T-Pro V2 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t12-v1.2.0-companion.bin` | Jumper T12 / T12 Pro | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t12max-v1.2.0-companion.bin` | Jumper T12 MAX | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t14-v1.2.0-companion.bin` | Jumper T14 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t15-v1.2.0.bin` | Jumper T15 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t15pro-v1.2.0.uf2` | Jumper T15 Pro | full | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t16-v1.2.0.bin` | Jumper T16 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t18-v1.2.0.bin` | Jumper T18 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t20-v1.2.0-companion.bin` | Jumper T20 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t20v2-v1.2.0-companion.bin` | Jumper T20 V2 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-jumper-t22-v1.2.0.uf2` | Jumper T22 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-boxer-v1.2.0-companion.bin` | RadioMaster Boxer | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-gx12-v1.2.0-companion.bin` | RadioMaster GX12 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-gx15-v1.2.0.uf2` | RadioMaster GX15 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-mt12-v1.2.0-companion.bin` | RadioMaster MT12 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-pocket-v1.2.0-companion.bin` | RadioMaster Pocket | companion | ✅ hardware-tested (owner) |
| `edgetx-ue-radiomaster-t8-v1.2.0-companion.bin` | RadioMaster T8 Pro / T8 Lite | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-tx12-v1.2.0-companion.bin` | RadioMaster TX12 | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-tx12mk2-v1.2.0-companion.bin` | RadioMaster TX12 MKII | companion | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-tx15-v1.2.0.uf2` | RadioMaster TX15 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-tx16s-v1.2.0.bin` | RadioMaster TX16S / TX16S MKII | full | ✅ hardware-tested (owner) |
| `edgetx-ue-radiomaster-tx16smk3-v1.2.0.uf2` | RadioMaster TX16S MK3 | full | 🔨 built, not hardware-tested |
| `edgetx-ue-radiomaster-zorro-v1.2.0-companion.bin` | RadioMaster Zorro | companion | ✅ hardware-tested (owner) |

## Flash it
Flash the `.bin` the way you'd flash official EdgeTX: EdgeTX Buddy (<https://buddy.edgetx.org>, *Flash local
firmware*, radio in DFU mode as EdgeTX describes for your radio), or copy the file to `/FIRMWARE` on the SD card and use
*Write Firmware* in the radio's EdgeTX bootloader. STM32 DFU tools: flash at `0x08000000`.
**`.uf2` files** (STM32H750 radios: TX15, TX16S MK3, GX15, T15 Pro, T22, V12, ST16, PA01, Commando 14): use the
radio's EdgeTX bootloader exactly as for an official EdgeTX `.uf2` (see the EdgeTX flashing guide for your radio).

## After flashing
1. Check the identity: on full builds, open SYS › Version; on companion builds, open any settings menu to see the
   UltraEdge screen. It should match your `.txt` (`ultraedge-…`, `UE 1.2.0`, GIT#, DATE).
2. Connect the phone over USB (OTG data cable) and open UltraEdge 1.6. System › About radio shows the same identity.

## USB mode on companion builds
Companion builds have no Radio Setup page, so the USB mode lives on the **UltraEdge** screen (press the radio's menu
key). A companion radio connects to the app without asking; ENTER switches the mode until power-off and a
long ENTER saves the default. Details: [USB mode on companion builds](https://github.com/50UR4V/UltraEdge-app/blob/main/UserManual/02-getting-started.md#usb-mode-on-companion-builds)
in the User Manual.

Problems? Go back to your previous firmware (stock EdgeTX, UltraEdge v1.1.2.4 or v1.1.2.3) the same way.

---
UltraEdge is a community project, not affiliated with or endorsed by EdgeTX, FrSky or RadioMaster. GPL-2.0-only; the
exact source is tag `v1.2.0` on branch `ultraedge`.
