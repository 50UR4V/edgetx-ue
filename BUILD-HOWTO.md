# Build your own UltraEdge firmware

This fork is **EdgeTX + one self-contained overlay** (`radio/src/thirdparty/ultraedge/` plus a few
`#if defined(USB_COMPANION)`-guarded hooks). Building it is the normal EdgeTX build with **one extra
flag: `-DUSB_COMPANION=ON`**. If you can build EdgeTX, you can build this.

There are two paths. **Codespaces** needs nothing installed. **Local** is faster if you already build
EdgeTX on your machine.

> Already have a Pocket or QX7? You don't need to build — grab the prebuilt `.bin` from
> [Releases](../../releases) and see [`FLASHING.md`](FLASHING.md). Build only if
> your radio isn't in the prebuilt list yet, or you want to change the config.

---

## Path A — GitHub Codespaces (in the browser, nothing to install)

EdgeTX ships a dev container, so a Codespace comes with the full toolchain preinstalled. See EdgeTX's own
guide for background: <https://edgetx.org/edgetx/latest/building/codespaces/>.

1. **Fork isn't required** — on this repo, click **`< > Code` ▸ Codespaces ▸ Create codespace on `main`**.
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
| FrSky **QX7 / QX7 Access** | `-DPCB=X7 -DMULTIMODULE=OFF -DHELI=NO -DGHOST=NO -DPXX1=YES` | ✅ Verified. Internal module **is** XJT/PXX1 — keep `PXX1=YES`. |
| RadioMaster **TX16S** | `-DPCB=X10 -DPCBREV=TX16S -DUSB_COMPANION=ON` | 🔜 **Untested.** Color-LCD (480×272) UI stack differs from mono; expect UI-hook work, not just a flag. |
| RadioMaster **Zorro** | `-DPCB=X7 -DPCBREV=ZORRO -DUSB_COMPANION=ON` | 🔜 **Untested.** Mono-ish but unverified — build + sanity-check on hardware before trusting. |

Always append **`-DUSB_COMPANION=ON`**. Leaving it off (or omitting it) builds stock EdgeTX,
byte-for-byte identical to upstream — useful to confirm your toolchain before adding the overlay.

Full EdgeTX board/flag reference: <https://github.com/EdgeTX/edgetx/blob/main/doc/build-options.md>.

---

## Adding a radio to the verified list

Color-LCD radios (TX16S, Zorro, …) aren't verified because their UI stack differs from the mono
QX7/Pocket the overlay was written against. If you build for a new radio:
1. Build with `-DUSB_COMPANION=ON` for your board and flash it.
2. Confirm the radio **boots and flies normally** (companion dormant, no phone), then plug in the
   UltraEdge app and confirm it connects and telemetry/model editing work.
3. Report results (radio, build flags, what worked/didn't) in [Issues](../../issues). Verified targets
   get added to the prebuilt Releases.

**No untested flight firmware ships as "verified"** — that bar is deliberate. Building it yourself and
flying it is your call; the prebuilt list is what we've personally checked on hardware.
