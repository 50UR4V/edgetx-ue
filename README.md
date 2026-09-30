# edgetx-ue — EdgeTX with the UltraEdge companion overlay

**This is a fork of [EdgeTX](https://github.com/EdgeTX/edgetx) that adds the UltraEdge companion link.**
With this firmware flashed, an EdgeTX radio can drive the **UltraEdge** Android app over USB — turning
your phone into a big touchscreen for a radio that only has a small screen and a few buttons.

Your radio always flies on its own. The companion overlay is **dormant until a phone is plugged in** and
is never in the flight-control path — unplug the phone and the radio behaves exactly like stock EdgeTX.
When the companion is compiled out, the build is byte-for-byte identical to upstream EdgeTX.

## Get the app / the ecosystem
- 📱 **UltraEdge app (Google Play):** https://play.google.com/store/apps/details?id=com.ultraedge.companion
- 🔌 **UE Protocol** (the open radio↔phone link this firmware speaks): https://github.com/50UR4V/ue-protocol
- 📖 **App home / manual / support:** https://github.com/50UR4V/UltraEdge-app
- 🖨️ **3D-printable phone mounts:** https://github.com/50UR4V/ultraedge-mounts

## Prebuilt firmware (download & flash)
Grab the `.bin` for your radio from **[Releases](../../releases)**, then follow [`FLASHING.md`](FLASHING.md).

| Radio | MCU | Status |
|---|---|---|
| RadioMaster **Pocket** | STM32F407 | ✅ Verified — prebuilt in Releases |
| FrSky **QX7 / QX7 Access** | STM32F205 | ✅ Verified — prebuilt in Releases |
| RadioMaster **TX16S**, **Zorro**, others | — | 🔜 Planned — **build it yourself** today ([`BUILD-HOWTO.md`](BUILD-HOWTO.md)) |

> Color-LCD radios (TX16S, Zorro, …) use a different UI stack than the mono QX7/Pocket and are not yet
> verified. We add a radio to the prebuilt list only after it has been built **and** sanity-checked on real
> hardware — no untested flight firmware ships here. Want yours next? Build it with the how-to and tell us
> how it goes in **[Issues](../../issues)**.

## Build it yourself
See **[`BUILD-HOWTO.md`](BUILD-HOWTO.md)** for two paths:
- **GitHub Codespaces** (no local toolchain — build in the browser, based on EdgeTX's own Codespaces flow).
- **Native / local** (macOS Arm toolchain or the EdgeTX Docker image).

Both produce a `.bin` you flash exactly like the prebuilt ones.

## Compatibility
This release speaks **UE Protocol v4** and pairs with **UltraEdge app v1.0.0**. Firmware and app negotiate
the protocol version at connect; keep them on matching major releases.

## Relationship to upstream EdgeTX
This fork tracks a pinned EdgeTX base commit and adds one overlay: a self-contained
`radio/src/thirdparty/ultraedge/` module plus small, `#if defined(USB_COMPANION)`-guarded hooks into
audio, telemetry and USB. We intend to keep rebasing onto EdgeTX and to discuss upstreaming the hooks with
the EdgeTX team. This is a **community project and is not affiliated with or endorsed by EdgeTX or FrSky.**

## License
EdgeTX is licensed **GPL-3.0-or-later**; this fork is a derivative work and is distributed under the **same
GPL-3.0** terms. The complete corresponding source — upstream EdgeTX history plus the UltraEdge overlay
commit — is in this repository. See [`LICENSE`](LICENSE).
