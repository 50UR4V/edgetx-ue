# Flashing UltraEdge companion firmware — v1.0.1

These `.bin` files are **EdgeTX firmware with the UltraEdge companion overlay** built in. Flash the file
that matches your radio, then pair it with the **UltraEdge** Android app over USB.

> **Verified radios (this release):** RadioMaster **Pocket** (STM32F407), FrSky **QX7 / QX7 Access**
> (STM32F205), and RadioMaster **TX16S** (STM32F407, color LCD). Other radios are **build-it-yourself**
> for now — see `BUILD-HOWTO.md`. More prebuilt targets land here as each is verified on real hardware.

| File | Radio | MCU | Notes |
|---|---|---|---|
| `edgetx-ue-radiomaster-pocket-v1.0.1.bin` | RadioMaster Pocket | STM32F407 (512 KB) | Internal CROSSFIRE; external DIY-multi left out to fit flash. |
| `edgetx-ue-frsky-qx7-v1.0.1.bin` | FrSky QX7 / QX7 Access | STM32F205 | |
| `edgetx-ue-radiomaster-tx16s-v1.0.1.bin` | RadioMaster TX16S | STM32F407 (2 MB, color LCD) | First color-LCD target. Known: telemetry refresh slower than mono. |

Firmware ↔ app compatibility: **UE Protocol v4** (matches UltraEdge app **v1.0.0**).
Verify your download against `SHA256SUMS.txt`:

```bash
shasum -a 256 -c SHA256SUMS.txt
```

## Flash it (any one method)

**Safety first:** back up your current firmware and your SD card before flashing. Flashing replaces the
firmware only; your models/settings on the SD card are untouched, but a backup is cheap insurance.

### A. EdgeTX Buddy (easiest — browser, no install)
1. Go to <https://buddy.edgetx.org> in Chrome/Edge.
2. Choose **"Flash local firmware"** and select the `.bin` for your radio.
3. Put the radio in **DFU/bootloader mode** (power on while holding both horizontal trims inward), plug
   in USB, and follow Buddy's prompts.

### B. Radio's built-in SD flasher (no computer)
1. Copy the `.bin` into the **`/FIRMWARE`** folder on the radio's SD card.
2. On the radio: long-press **MENU → SD-CARD → FIRMWARE →** your `.bin` **→ Flash firmware**.

### C. Desktop (STM32CubeProgrammer / dfu-util)
1. Radio into DFU mode (both horizontal trims inward while powering on), plug USB.
2. Flash the `.bin` at address `0x08000000` with your usual STM32 DFU tool.

## After flashing
1. The radio reboots into EdgeTX as normal — it flies exactly as stock EdgeTX. The UltraEdge overlay is
   **dormant until a phone is connected**; unplug the phone and nothing about how the radio flies changes.
2. Install the **UltraEdge** app (Google Play), plug the phone into the radio's USB port, allow the USB
   permission prompt, and the app connects.

## Trouble
- **Radio not detected in DFU:** try a different USB cable (must be data, not charge-only); some radios
  need the trims-inward power-on timing to be exact.
- **App won't connect:** unplug/replug once; grant the Android USB permission when prompted. See the
  UltraEdge User Manual (<https://github.com/50UR4V/UltraEdge-app>).

---
UltraEdge is a community project, not affiliated with or endorsed by EdgeTX or FrSky. EdgeTX is licensed
GPL-3.0; this firmware is a derivative and is offered under the same terms — full source in this repo.
