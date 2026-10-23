# Flashing UltraEdge firmware 1.1.19 (beta)

**Beta.** UltraEdge is made for any EdgeTX radio, and 1.1.19 is built for all 56 radios EdgeTX supports. Only
RadioMaster Pocket, Zorro, TX16S and FrSky Q X7 / Q X7S have been checked on real hardware (on earlier builds); every
other radio is **built, not yet verified**. Back up your SD card and your current firmware first, and bench-test with
the propeller off. Pair it with the UltraEdge app **1.4.0 beta** (Google Play open testing).

**Companion** builds (black-and-white screens) remove the radio's own model and radio-settings menus; you change
those in the app. **Full** builds (colour screens) keep every EdgeTX menu.

## Find your file
Flash **only the file for your exact radio**. Similar names are different hardware (for example, the Q X7 ACCESS is not
the Q X7). Each file has a `.txt` beside it that lists exactly what the radio's Version page or UltraEdge screen will
show. Verify downloads with `shasum -a 256 -c SHA256SUMS.txt`.

| Radio | File | Build | Status |
|---|---|---|---|
| BETAFPV LiteRadio 3 Pro | `edgetx-ue-betafpv-lr3pro-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Fatfish F16 | `edgetx-ue-fatfish-f16-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FlySky Elysium EL18 | `edgetx-ue-flysky-el18-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FlySky Nirvana NV14 | `edgetx-ue-flysky-nv14-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FlySky Noble NB4+ | `edgetx-ue-flysky-nb4p-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FlySky PA01 | `edgetx-ue-flysky-pa01-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| FlySky Paladin PL18 | `edgetx-ue-flysky-pl18-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FlySky Paladin PL18EV | `edgetx-ue-flysky-pl18ev-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FlySky Paladin PL18U | `edgetx-ue-flysky-pl18u-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FlySky ST16 | `edgetx-ue-flysky-st16-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| FrSky Horus X10 / X10S | `edgetx-ue-frsky-x10-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FrSky Horus X10 / X10S Express | `edgetx-ue-frsky-x10express-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FrSky Horus X12S | `edgetx-ue-frsky-x12s-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| FrSky Taranis Q X7 / Q X7S | `edgetx-ue-frsky-qx7-v1.1.19-companion.bin` | Companion | ✅ verified (owner) on earlier builds |
| FrSky Taranis Q X7 ACCESS | `edgetx-ue-frsky-qx7-access-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X-Lite | `edgetx-ue-frsky-xlite-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X-Lite S / Pro | `edgetx-ue-frsky-xlites-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X9 Lite | `edgetx-ue-frsky-x9lite-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X9 Lite S | `edgetx-ue-frsky-x9lites-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X9D | `edgetx-ue-frsky-x9d-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X9D Plus | `edgetx-ue-frsky-x9dplus-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X9D Plus 2019 | `edgetx-ue-frsky-x9dplus2019-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| FrSky Taranis X9E | `edgetx-ue-frsky-x9e-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| HelloRadioSky V12 | `edgetx-ue-helloradiosky-v12-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| HelloRadioSky V14 | `edgetx-ue-helloradiosky-v14-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| HelloRadioSky V14 LCD | `edgetx-ue-helloradiosky-v14lcd-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| HelloRadioSky V16 | `edgetx-ue-helloradiosky-v16-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| iFlight Commando 14 | `edgetx-ue-iflight-c14-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| iFlight Commando 8 | `edgetx-ue-iflight-commando8-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper Bumblebee | `edgetx-ue-jumper-bumblebee-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T-Lite (v1) | `edgetx-ue-jumper-tlite-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T-Pro (v1) | `edgetx-ue-jumper-tpro-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T-Pro S | `edgetx-ue-jumper-tpros-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T-Pro V2 | `edgetx-ue-jumper-tprov2-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T12 / T12 Pro | `edgetx-ue-jumper-t12-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T12 MAX | `edgetx-ue-jumper-t12max-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T14 | `edgetx-ue-jumper-t14-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T15 | `edgetx-ue-jumper-t15-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| Jumper T15 Pro | `edgetx-ue-jumper-t15pro-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| Jumper T16 | `edgetx-ue-jumper-t16-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| Jumper T18 | `edgetx-ue-jumper-t18-v1.1.19.bin` | Full | 🔨 built, not yet verified |
| Jumper T20 | `edgetx-ue-jumper-t20-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T20 V2 | `edgetx-ue-jumper-t20v2-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| Jumper T22 | `edgetx-ue-jumper-t22-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| RadioMaster Boxer | `edgetx-ue-radiomaster-boxer-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| RadioMaster GX12 | `edgetx-ue-radiomaster-gx12-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| RadioMaster GX15 | `edgetx-ue-radiomaster-gx15-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| RadioMaster MT12 | `edgetx-ue-radiomaster-mt12-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| RadioMaster Pocket | `edgetx-ue-radiomaster-pocket-v1.1.19-companion.bin` | Companion | ✅ verified (owner) on earlier builds |
| RadioMaster T8 Pro / T8 Lite | `edgetx-ue-radiomaster-t8-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| RadioMaster TX12 | `edgetx-ue-radiomaster-tx12-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| RadioMaster TX12 MKII | `edgetx-ue-radiomaster-tx12mk2-v1.1.19-companion.bin` | Companion | 🔨 built, not yet verified |
| RadioMaster TX15 | `edgetx-ue-radiomaster-tx15-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| RadioMaster TX16S / TX16S MKII | `edgetx-ue-radiomaster-tx16s-v1.1.19.bin` | Full | ✅ verified (owner) on earlier builds |
| RadioMaster TX16S MK3 | `edgetx-ue-radiomaster-tx16smk3-v1.1.19.uf2` | Full | 🔨 built, not yet verified |
| RadioMaster Zorro | `edgetx-ue-radiomaster-zorro-v1.1.19-companion.bin` | Companion | ✅ verified (owner) on earlier builds |

## Flash it
- **`.bin` files:** flash them the way you'd flash official EdgeTX. Easiest is EdgeTX Buddy (<https://buddy.edgetx.org>,
  *Flash local firmware*, radio in DFU/bootloader mode), or copy the file to `/FIRMWARE` on the SD card and use the
  radio's bootloader. Using STM32 DFU tools? Flash at `0x08000000`.
- **`.uf2` files** (STM32H750 radios: TX15, TX16S MK3, GX15, T15 Pro, T22, V12, ST16, PA01, Commando 14): use the
  radio's EdgeTX bootloader exactly as for an official EdgeTX `.uf2` (see the EdgeTX flashing guide for your radio).

## After flashing
1. Check the identity: on full builds, open SYS ▸ Version; on companion builds, open any settings menu to see the
   UltraEdge screen. It should match your `.txt` (`ultraedge-…`, `UE 1.1.19`, GIT#, DATE).
2. Connect the phone over USB (OTG data cable) and open UltraEdge 1.4.0. System ▸ About ▸ Radio firmware shows the
   same identity.
3. **Help verify your radio:** run the checklist in `SUPPORTED-RADIOS.md` and report it as an issue on
   <https://github.com/50UR4V/UltraEdge-app/issues> with your `.txt` contents. Reports move radios to ✅.

Problems? Go back to your previous firmware (stock EdgeTX or UltraEdge v1.1.2.1) the same way.

---
UltraEdge is a community project, not affiliated with or endorsed by EdgeTX or any radio maker. GPL-2.0-only; the
exact source is tag `v1.1.19` on branch `ultraedge-beta`.
