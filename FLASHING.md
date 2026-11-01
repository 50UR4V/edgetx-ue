# Flashing UltraEdge firmware v1.2.0

v1.2.0 is the firmware for both the production app (**UltraEdge 1.6.0**, Google Play) and the beta (**1.6.1**, open
testing). It is built for the four radios we test on: RadioMaster Pocket, Zorro and TX16S, and FrSky QX7. Every other
radio stays on [1.1.22](https://github.com/50UR4V/edgetx-ue/releases/tag/v1.1.22) for now. Back up your SD card and
your current firmware first, and bench-test with the propeller off.

**Companion** builds (black-and-white screens: Pocket, Zorro, QX7) remove the radio's own model and radio-settings
menus; you change those in the app. **Full** builds (TX16S) keep every EdgeTX menu. Want the radio's own menus on a
black-and-white radio? Use the [v1.1.2.4](https://github.com/50UR4V/edgetx-ue/releases/tag/v1.1.2.4) full builds.

## Find your file
Flash **only the file for your exact radio**. The Q X7 ACCESS is not the Q X7 and has no v1.2.0 file. Each file has a
`.txt` beside it that lists exactly what the radio's Version page or UltraEdge screen will show. Verify downloads with
`shasum -a 256 -c SHA256SUMS.txt`.

| File | Radio | Build |
|---|---|---|
| `edgetx-ue-radiomaster-pocket-v1.2.0-companion.bin` | RadioMaster Pocket | companion |
| `edgetx-ue-radiomaster-zorro-v1.2.0-companion.bin` | RadioMaster Zorro | companion |
| `edgetx-ue-frsky-qx7-v1.2.0-companion.bin` | FrSky QX7 / QX7S (**not** the QX7 ACCESS) | companion |
| `edgetx-ue-radiomaster-tx16s-v1.2.0.bin` | RadioMaster TX16S / TX16S MKII | full |

## Flash it
Flash the `.bin` the way you'd flash official EdgeTX: EdgeTX Buddy (<https://buddy.edgetx.org>, *Flash local
firmware*, radio in DFU mode as EdgeTX describes for your radio), or copy the file to `/FIRMWARE` on the SD card and use
*Write Firmware* in the radio's EdgeTX bootloader. STM32 DFU tools: flash at `0x08000000`.

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
