# Flashing UltraEdge firmware 1.1.22 (beta)

**Beta.** UltraEdge is made for any EdgeTX radio, and 1.1.22 is built for all 56 radios EdgeTX supports at our base
`90bbce17`. Hardware-verified: Pocket, Zorro, TX16S and QX7 on the v1.1.2 full build; 1.1.22 beta (TX16S full, QX7 companion) smoke-tested by the owner on 2026-10-25. Every other radio is **built, not yet verified**, and the Pocket and Zorro
companion builds have not been through the checklist yet. Back up your SD card and your current firmware first, and
bench-test with the propeller off. Pair it with the UltraEdge app **1.4.3 beta** (Google Play open testing).

**Companion** builds (black-and-white screens) remove the radio's own model and radio-settings menus; you change
those in the app. **Full** builds (colour screens) keep every EdgeTX menu.

## Find your file
Flash **only the file for your exact radio**. Similar names are different hardware (for example, the Q X7 ACCESS is not
the Q X7). Each file has a `.txt` beside it that lists exactly what the radio's Version page or UltraEdge screen will
show. Verify downloads with `shasum -a 256 -c SHA256SUMS.txt`.

Files are named `edgetx-ue-<maker>-<radio>-v1.1.22.bin` (full build), `edgetx-ue-<maker>-<radio>-v1.1.22-companion.bin`
(companion build) or `edgetx-ue-<maker>-<radio>-v1.1.22.uf2` (full build, STM32H750 radios). For example, the
RadioMaster Pocket file is `edgetx-ue-radiomaster-pocket-v1.1.22-companion.bin`. The list of all 56 radios, each with
its build type and verification status, is `SUPPORTED-RADIOS.md` on this release.

## Flash it
- **`.bin` files:** flash them the way you'd flash official EdgeTX: EdgeTX Buddy (<https://buddy.edgetx.org>,
  *Flash local firmware*, radio in DFU mode as EdgeTX describes for your radio), or copy the file to `/FIRMWARE` on the
  SD card and use *Write Firmware* in the radio's EdgeTX bootloader. STM32 DFU tools: flash at `0x08000000`.
- **`.uf2` files** (STM32H750 radios: TX15, TX16S MK3, GX15, T15 Pro, T22, V12, ST16, PA01, Commando 14): use the
  radio's EdgeTX bootloader exactly as for an official EdgeTX `.uf2` (see the EdgeTX flashing guide for your radio).

## After flashing
1. Check the identity: on full builds, open SYS › Version; on companion builds, open any settings menu to see the
   UltraEdge screen. It should match your `.txt` (`ultraedge-…`, `UE 1.1.22`, GIT#, DATE).
2. Connect the phone over USB (OTG data cable) and open UltraEdge 1.4.3. System › About & Licenses › Radio firmware
   (1.4.3) / System › About radio (1.5.1) shows the same identity.
3. **Help verify your radio:** run the checklist in `SUPPORTED-RADIOS.md` and report it as an issue on
   <https://github.com/50UR4V/UltraEdge-app/issues> with your `.txt` contents. Reports move radios to ✅.

## USB mode on companion builds
Companion builds have no Radio Setup page, so the USB mode lives on the **UltraEdge** screen (press the radio's menu
key). In 1.1.22 a companion radio connects to the app without asking; ENTER switches the mode until power-off and a
long ENTER saves the default. Details: [USB mode on companion builds](https://github.com/50UR4V/UltraEdge-app/blob/main/UserManual/02-getting-started.md#usb-mode-on-companion-builds)
in the User Manual.

Problems? Go back to your previous firmware (stock EdgeTX or UltraEdge v1.1.2.3) the same way.

---
UltraEdge is a community project, not affiliated with or endorsed by EdgeTX, FrSky or RadioMaster. GPL-2.0-only; the
exact source is tag `v1.1.22` on branch `ultraedge-beta`.
