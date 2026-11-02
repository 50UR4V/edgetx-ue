# Build your own UltraEdge firmware

This fork is **EdgeTX + one self-contained overlay** (`radio/src/thirdparty/ultraedge/` plus a few
`#if defined(USB_COMPANION)`-guarded hooks). Building it is the normal EdgeTX build with **one extra
flag: `-DUSB_COMPANION=ON`**.

**Codespaces** needs nothing installed. **Local** is faster if you already build EdgeTX.

> Most radios don't need a build: [Releases](../../releases) has v1.2.0 for every radio in SUPPORTED-RADIOS (v1.1.2.4
> full builds as the fallback for Pocket, Zorro and QX7). Build only if your radio isn't there or you want to change the config.

---

## Path A — GitHub Codespaces (in the browser, nothing to install)

EdgeTX ships a dev container, so a Codespace comes with the full toolchain preinstalled. See EdgeTX's own
guide for background: <https://edgetx.org/edgetx/latest/building/codespaces/>.

1. **Fork isn't required** — on this repo, click **`< > Code` ▸ Codespaces ▸ Create codespace** on branch `ultraedge`
   (tag `v1.2.0` is the current release, companion and full builds alike).
   Wait for the container to finish building (first run takes a few minutes).
2. In the Codespace terminal, configure + build for your radio (example: **RadioMaster Pocket**):
   ```bash
   cmake -S . -B build -G Ninja \
     -DPCB=X7 -DPCBREV=POCKET \
     -DTRANSLATIONS=EN -DDEFAULT_TEMPLATE_SETUP=17 \
     -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=NO \
     -DUSB_COMPANION=ON
   cmake --build build --target firmware
   ```
   The firmware lands at `build/arm-none-eabi/firmware.bin`.
3. **Download it:** right-click `firmware.bin` in the Codespaces file tree ▸ **Download**. Flash per
   [`FLASHING.md`](FLASHING.md).

Swap the flags for your radio using the table below.

---

## Version page (FW / VERS / GIT# / DATE)

UltraEdge firmware names itself on the radio's Version page (`FW: ultraedge-<radio>`, `VERS: UE <version>`, `GIT#`,
`DATE`). The values come from `radio/src/thirdparty/ultraedge/ue_version.h`. A self-build shows
`UE <version>-selfbuild`; edit the file if you want your own label. To reproduce an official `.bin` byte-for-byte, copy
the four values from the `.txt` next to that `.bin` on the release page into this file before building.

## Path B — Local build

### Option B1 — EdgeTX Docker image (any OS with Docker)
```bash
git clone <this-repo-url> edgetx-ue && cd edgetx-ue
docker run --rm -v "$PWD":/src -w /src ghcr.io/edgetx/edgetx-dev:latest bash -euxc '
  cmake -S /src -B build \
    -DPCB=X7 -DPCBREV=POCKET \
    -DTRANSLATIONS=EN -DDEFAULT_TEMPLATE_SETUP=17 \
    -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=NO \
    -DUSB_COMPANION=ON
  cmake --build build --target firmware -- -j$(nproc)
'
# -> build/arm-none-eabi/firmware.bin
```
On Apple Silicon the amd64 image runs under emulation (slower, build serially with `-j1`).

### Option B2 — Native toolchain (fastest, e.g. on macOS)
Install the **Arm GNU Toolchain 14.2.rel1** + CMake/Ninja + a Python venv (EdgeTX's standard build deps).
Once `arm-none-eabi-gcc` is on your `PATH`, the same `cmake … && cmake --build` block from B1 runs directly
(add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` if you're on CMake 4.x). The firmware lands at
`build/arm-none-eabi/firmware.bin`.

---

## Per-radio flags

Every build uses `-DTRANSLATIONS=EN -DDEFAULT_TEMPLATE_SETUP=17 -DUSB_COMPANION=ON` plus the radio's flags below:
the flags the v1.2.0 release files were built with. `-DPCB` / `-DPCBREV` select the board. On the 512 KB Pocket,
Zorro and QX7 the companion sits near the flash ceiling, so `MULTIMODULE`, `HELI` and `GHOST` are trimmed there. Keep
**`-DLUA` on**: the companion's raw-telemetry passthrough uses EdgeTX's LUA-gated TelemetryQueue and won't compile
without it.

**Companion build:** `-DUI_COMPANION=YES` removes the radio's own model and radio-settings menus; those settings are
then made in the app. The release uses it on every black-and-white radio (128×64 and 212×64 screens). Leave it out
for a full build that keeps every EdgeTX menu (it may not fit in 512 KB of flash). Colour radios always build full.

| Radio | Flags | Notes |
|---|---|---|
| RadioMaster Pocket | `-DPCB=X7 -DPCBREV=POCKET -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=NO -DUI_COMPANION=YES` | Verified on v1.2.0 (2026-11-01), companion build. Internal CROSSFIRE; external multi and PXX1 left out to fit flash. |
| RadioMaster Zorro | `-DPCB=X7 -DPCBREV=ZORRO -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=NO -DUI_COMPANION=YES` | Verified on v1.2.0 (2026-11-01), companion build. Tight flash fit, like the Pocket. |
| FrSky Taranis Q X7 / Q X7S | `-DPCB=X7 -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=YES -DUI_COMPANION=YES` | Verified on v1.2.0 (2026-11-01), companion build. The internal module is XJT/PXX1, so keep `PXX1=YES`. |
| RadioMaster TX16S / TX16S MKII | `-DPCB=X10 -DPCBREV=TX16S` | Verified on v1.2.0 (2026-11-01), full build. |
| FrSky Taranis Q X7 ACCESS | `-DPCB=X7 -DPCBREV=ACCESS -DPXX1=YES -DUI_COMPANION=YES` |  |
| RadioMaster Boxer | `-DPCB=X7 -DPCBREV=BOXER -DUI_COMPANION=YES` |  |
| RadioMaster GX12 | `-DPCB=X7 -DPCBREV=GX12 -DUI_COMPANION=YES` |  |
| RadioMaster MT12 | `-DPCB=X7 -DPCBREV=MT12 -DUI_COMPANION=YES` |  |
| RadioMaster TX12 | `-DPCB=X7 -DPCBREV=TX12 -DUI_COMPANION=YES` |  |
| RadioMaster TX12 MKII | `-DPCB=X7 -DPCBREV=TX12MK2 -DUI_COMPANION=YES` |  |
| RadioMaster T8 Pro / T8 Lite | `-DPCB=X7 -DPCBREV=T8 -DUI_COMPANION=YES` |  |
| Jumper T12 / T12 Pro | `-DPCB=X7 -DPCBREV=T12 -DINTERNAL_MODULE_MULTI=ON -DUI_COMPANION=YES` |  |
| Jumper T12 MAX | `-DPCB=X7 -DPCBREV=T12MAX -DUI_COMPANION=YES` |  |
| Jumper T14 | `-DPCB=X7 -DPCBREV=T14 -DUI_COMPANION=YES` |  |
| Jumper T20 | `-DPCB=X7 -DPCBREV=T20 -DUI_COMPANION=YES` |  |
| Jumper T20 V2 | `-DPCB=X7 -DPCBREV=T20V2 -DUI_COMPANION=YES` |  |
| Jumper T-Lite (v1) | `-DPCB=X7 -DPCBREV=TLITE -DUI_COMPANION=YES` |  |
| Jumper T-Pro (v1) | `-DPCB=X7 -DPCBREV=TPRO -DUI_COMPANION=YES` |  |
| Jumper T-Pro V2 | `-DPCB=X7 -DPCBREV=TPROV2 -DUI_COMPANION=YES` |  |
| Jumper T-Pro S | `-DPCB=X7 -DPCBREV=TPROS -DUI_COMPANION=YES` |  |
| Jumper Bumblebee | `-DPCB=X7 -DPCBREV=BUMBLEBEE -DUI_COMPANION=YES` |  |
| BETAFPV LiteRadio 3 Pro | `-DPCB=X7 -DPCBREV=LR3PRO -DUI_COMPANION=YES` |  |
| iFlight Commando 8 | `-DPCB=X7 -DPCBREV=COMMANDO8 -DUI_COMPANION=YES` |  |
| HelloRadioSky V14 | `-DPCB=X7 -DPCBREV=V14 -DUI_COMPANION=YES` |  |
| HelloRadioSky V14 LCD | `-DPCB=X7 -DPCBREV=V14LCD -DUI_COMPANION=YES` |  |
| FrSky Taranis X9 Lite | `-DPCB=X9LITE -DUI_COMPANION=YES` |  |
| FrSky Taranis X9 Lite S | `-DPCB=X9LITES -DUI_COMPANION=YES` |  |
| FrSky Taranis X-Lite | `-DPCB=XLITE -DUI_COMPANION=YES` |  |
| FrSky Taranis X-Lite S / Pro | `-DPCB=XLITES -DUI_COMPANION=YES` |  |
| FrSky Taranis X9D | `-DPCB=X9D -DUI_COMPANION=YES` |  |
| FrSky Taranis X9D Plus | `-DPCB=X9D+ -DUI_COMPANION=YES` |  |
| FrSky Taranis X9D Plus 2019 | `-DPCB=X9D+ -DPCBREV=2019 -DUSE_FW_LTO=y -DUI_COMPANION=YES` |  |
| FrSky Taranis X9E | `-DPCB=X9E -DUI_COMPANION=YES` |  |
| FrSky Horus X10 / X10S | `-DPCB=X10` |  |
| FrSky Horus X10 / X10S Express | `-DPCB=X10 -DPCBREV=EXPRESS -DPXX1=YES` |  |
| FrSky Horus X12S | `-DPCB=X12S` |  |
| Jumper T15 | `-DPCB=X10 -DPCBREV=T15 -DINTERNAL_MODULE_CRSF=ON` |  |
| Jumper T16 | `-DPCB=X10 -DPCBREV=T16 -DINTERNAL_MODULE_MULTI=ON` |  |
| Jumper T18 | `-DPCB=X10 -DPCBREV=T18` |  |
| Fatfish F16 | `-DPCB=X10 -DPCBREV=F16` |  |
| HelloRadioSky V16 | `-DPCB=X10 -DPCBREV=V16` |  |
| FlySky Nirvana NV14 | `-DPCB=PL18 -DPCBREV=NV14` |  |
| FlySky Elysium EL18 | `-DPCB=PL18 -DPCBREV=EL18` |  |
| FlySky Paladin PL18 | `-DPCB=PL18` |  |
| FlySky Paladin PL18EV | `-DPCB=PL18 -DPCBREV=PL18EV` |  |
| FlySky Paladin PL18U | `-DPCB=PL18 -DPCBREV=PL18U` |  |
| FlySky Noble NB4+ | `-DPCB=PL18 -DPCBREV=NB4P` |  |
| RadioMaster TX15 | `-DPCB=TX15` |  |
| RadioMaster GX15 | `-DPCB=GX15` |  |
| RadioMaster TX16S MK3 | `-DPCB=TX16SMK3` |  |
| Jumper T15 Pro | `-DPCB=T15PRO` |  |
| Jumper T22 | `-DPCB=T22` |  |
| HelloRadioSky V12 | `-DPCB=V12` |  |
| FlySky ST16 | `-DPCB=ST16` |  |
| FlySky PA01 | `-DPCB=PA01` |  |
| iFlight Commando 14 | `-DPCB=C14` |  |

Without **`-DUSB_COMPANION=ON`** you get stock EdgeTX, byte-for-byte identical to upstream, which is a good way to
check your toolchain first.

Full EdgeTX board/flag reference: <https://github.com/EdgeTX/edgetx/blob/main/docs/building/compilation-options.md>.

---

## Adding a radio to the verified list

A radio is marked verified only after the checklist in `SUPPORTED-RADIOS.md` (section *Help verify your radio*) passes
on real hardware. Run that checklist on your build and report the result, with your build flags, in
[UltraEdge-app issues](https://github.com/50UR4V/UltraEdge-app/issues).

Nothing is marked verified without a hardware check. Flying a build you made yourself is your call.
