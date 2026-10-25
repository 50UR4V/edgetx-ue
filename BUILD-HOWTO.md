# Build your own UltraEdge firmware

This fork is **EdgeTX + one self-contained overlay** (`radio/src/thirdparty/ultraedge/` plus a few
`#if defined(USB_COMPANION)`-guarded hooks). Building it is the normal EdgeTX build with **one extra
flag: `-DUSB_COMPANION=ON`**. If you can build EdgeTX, you can build this.

**Codespaces** needs nothing installed. **Local** is faster if you already build EdgeTX.

> Most radios don't need a build: [Releases](../../releases) has v1.1.2.3 for Pocket, Zorro, TX16S and QX7, and the
> 1.1.22 beta for 56 radios. Build only if your radio isn't there or you want to change the config.

---

## Path A — GitHub Codespaces (in the browser, nothing to install)

EdgeTX ships a dev container, so a Codespace comes with the full toolchain preinstalled. See EdgeTX's own
guide for background: <https://edgetx.org/edgetx/latest/building/codespaces/>.

1. **Fork isn't required** — on this repo, click **`< > Code` ▸ Codespaces ▸ Create codespace** on branch `ultraedge`
   (production) or `ultraedge-beta` (beta).
   Wait for the container to finish building (first time ~a few minutes).
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

`-DPCB` / `-DPCBREV` select the board; the rest are the **flash-fit** levers (Pocket/QX7 are 512 KB-class
and the companion sits near the ceiling, so trim unused subsystems). Keep **`-DLUA` on** — the companion's
raw-telemetry passthrough uses EdgeTX's LUA-gated TelemetryQueue and won't compile without it.

| Radio | Key flags | Notes |
|---|---|---|
| RadioMaster **Pocket** | `-DPCB=X7 -DPCBREV=POCKET -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=NO` | ✅ Verified. Internal CROSSFIRE, so external PXX1 is dropped for space. ~10 KB flash headroom. |
| FrSky **QX7 / QX7S (not the ACCESS model)** | `-DPCB=X7 -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=YES` | ✅ Verified. Internal module **is** XJT/PXX1 — keep `PXX1=YES`. |
| RadioMaster **TX16S** | `-DPCB=X10 -DPCBREV=TX16S` | ✅ Verified (v1.1.2). First color-LCD target; 2 MB flash so no trims needed. Prebuilt in Releases. |
| RadioMaster **Zorro** | `-DPCB=X7 -DPCBREV=ZORRO -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=NO` | ✅ Verified (v1.1.2). Mono, Pocket-twin; tight flash fit. |

Always append **`-DUSB_COMPANION=ON`**. Without it you get stock EdgeTX, byte-for-byte identical to upstream, which is
a good way to check your toolchain first.

**Companion build** (branch `ultraedge-beta` only): add **`-DUI_COMPANION=YES`** to remove the radio's own model and
radio-settings menus. The beta uses it for black-and-white radios to free flash; those settings are then made in the
app. The flags used for each of the 56 beta radios are in `SUPPORTED-RADIOS.md` on the 1.1.22 release.

Full EdgeTX board/flag reference: <https://github.com/EdgeTX/edgetx/blob/main/docs/building/compilation-options.md>.

---

## Adding a radio to the verified list

A radio is marked verified only after the checklist in `SUPPORTED-RADIOS.md` passes on real hardware. If you build for
a radio that isn't verified yet:
1. Build with `-DUSB_COMPANION=ON` for your board and flash it.
2. Confirm the radio **boots and flies normally** (companion dormant, no phone), then plug in the
   UltraEdge app and confirm it connects and telemetry/model editing work.
3. Report the result (radio, build flags, what worked and what didn't) in
   [UltraEdge-app issues](https://github.com/50UR4V/UltraEdge-app/issues).

Nothing is marked verified without a hardware check. Flying a build you made yourself is your call.
