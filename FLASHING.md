# Flashing UltraEdge firmware — v1.1.2.3

These `.bin` files are EdgeTX firmware with the UltraEdge companion overlay built in. Flash the file for your radio,
then connect the **UltraEdge** Android app over USB.

**New in v1.1.2.3:** background music started by a Special Function stops and pauses with its switch on the phone too.
The USB mode you pick is kept until you power the radio off, so a loose cable reconnects without asking again.

| File | Radio | MCU | Status |
|---|---|---|---|
| `edgetx-ue-radiomaster-pocket-v1.1.2.3.bin` | RadioMaster Pocket | STM32F407 | ✅ Verified. Internal CRSF; external DIY-multi and external PXX1 left out to fit flash. |
| `edgetx-ue-radiomaster-zorro-v1.1.2.3.bin` | RadioMaster Zorro | STM32F407 | ✅ Verified |
| `edgetx-ue-frsky-qx7-v1.1.2.3.bin` | FrSky QX7 / QX7S (**not** the QX7 ACCESS) | STM32F205 | ✅ Verified |
| `edgetx-ue-radiomaster-tx16s-v1.1.2.3.bin` | RadioMaster TX16S / TX16S MKII | STM32F429 | ✅ Verified. Telemetry screens refresh slower than on mono radios. |

All four are full builds: the radio keeps every EdgeTX menu. Use UltraEdge app **1.3.2 or newer** (Google Play); both
speak UE Protocol v4. Other radios: the beta 1.1.22 pre-release has builds for 56 radios.

Check the download: `shasum -a 256 -c SHA256SUMS.txt`. Back up your SD card and current firmware before flashing.

## Flash it (any one method)
**A. EdgeTX Buddy** (browser, no install): open <https://buddy.edgetx.org> in Chrome or Edge, choose
*Flash local firmware* and pick the `.bin`. Put the radio in **DFU mode** the way EdgeTX describes for your radio (on
most RadioMaster radios: switched off, then plugged in with a USB data cable) and follow Buddy's prompts.

**B. The radio's bootloader** (no computer): copy the `.bin` to `/FIRMWARE` on the SD card. Hold both horizontal trims
toward the centre while switching the radio on to start the EdgeTX bootloader, choose *Write Firmware* and pick the file.

**C. STM32 DFU tools** (STM32CubeProgrammer, dfu-util): radio in DFU mode, flash the `.bin` at `0x08000000`.

## After flashing
1. Open **SYS › Version**. It should match the `.txt` for your radio (`ultraedge-…`, `UE 1.1.2.3`, GIT#, DATE).
   Include these lines in any bug report.
2. The radio flies exactly as EdgeTX. Install **UltraEdge** from Google Play, plug the phone into the radio's USB port
   and allow the USB permission prompt.

## Trouble
- **No DFU device:** use a data cable, not a charge-only one, and check EdgeTX's DFU steps for your radio.
- **App won't connect:** unplug and replug once, and allow the Android USB permission. See the
  [User Manual](https://github.com/50UR4V/UltraEdge-app).

---
UltraEdge is a community project, not affiliated with or endorsed by EdgeTX, FrSky or RadioMaster. This firmware is a
derivative of EdgeTX under GPL-2.0-only; the exact source is tag `v1.1.2.3` on branch `ultraedge`.
