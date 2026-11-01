#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# UltraEdge self-build: build ONE radio from an UltraEdge firmware source tree (the public fork 50UR4V/edgetx-ue at a
# release tag). Used by the "Build UltraEdge firmware" GitHub Actions form, by Codespaces, and locally. ADR-0049.
#
# The fork's tree already contains the UltraEdge overlay, so this is EdgeTX's normal build plus three things:
#   1. the per-radio flags UltraEdge ships with (targets.tsv, generated from firmware/v4/build-fw-all-radios.sh),
#   2. the firmware identity (FW/VERS/GIT#/DATE, ADR-0040) written fresh for this build and marked "-selfbuild",
#   3. the same identity gate as the official builds: a binary without its identity is not handed out.
#
# Usage:
#   tools/ultraedge/ue-build.sh <radio> [options]      # e.g. ue-build.sh tx16s
#   tools/ultraedge/ue-build.sh list                   # radios this source tree supports
# Options:
#   --src DIR          UltraEdge firmware source tree (default: the git top level of the current directory)
#   --out DIR          where the firmware + identity .txt go (default: ./out)
#   --ui FULL|COMPANION  radio UI (default: COMPANION on black-and-white radios where the version supports it)
#   --label TEXT       version suffix shown on the radio, [A-Za-z0-9._-], max 12 (default: selfbuild)
#   --lang XX          TRANSLATIONS (default EN)
#   --opt -DNAME=VAL   an extra EdgeTX build option (repeatable; e.g. --opt -DBLUETOOTH=YES). Can't override the
#                      radio, USB_COMPANION or UI. Extra options can stop a 512 KB radio fitting.
#   --jobs N           parallel compile jobs (default: all cores)
#   --official         release build (no "-selfbuild" label). Only in GitHub Actions on the repo compat.json names
#                      (the release-build workflow, ADR-0049 D5).
# Every run writes out/<radio>.result: "OK|<file>|<bytes>|<free>" or "FAIL|<reason>" (the release summary reads it).
# --official names the files as the GitHub Release does (edgetx-ue-<name>-v<version>[-companion], release-names.tsv) and
# writes the release's .txt format (FW/VERS/GIT#/DATE/BASE/USBNET/SHA256 + BUILD). USB Ethernet (s46) is never built here:
# the public source has none, and -DUE_USBNET can't be passed.
# Needs arm-none-eabi-gcc, cmake and python3 on PATH (the ghcr.io/edgetx/edgetx-dev image and a Codespace have them).
set -euo pipefail

RECIPE="$(cd "$(dirname "$0")" && pwd)"
SRC=""; OUT="$PWD/out"; UI_REQ=""; LABEL="selfbuild"; LANG_="EN"; JOBS=""; EXTRA=(); OFFICIAL=0
RADIO="${1:-}"; [ $# -gt 0 ] && shift
while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --ui) UI_REQ="$(echo "$2" | tr a-z A-Z)"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    --lang) LANG_="$(echo "$2" | tr a-z A-Z)"; shift 2 ;;
    --opt) EXTRA+=("$2"); shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --official) OFFICIAL=1; shift ;;
    *) echo "!! unknown option '$1' (see the header of $0)"; exit 1 ;;
  esac
done
RESULT=""   # out/<radio>.result, set once the radio is known
die() { echo "!! $*" >&2; [ -n "$RESULT" ] && printf 'FAIL|%s\n' "$*" > "$RESULT"; exit 2; }
SRC="${SRC:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"; SRC="$(cd "$SRC" && pwd)"
UEDIR="$SRC/radio/src/thirdparty/ultraedge"
[ -f "$UEDIR/ue_version.h" ] || die "$SRC is not an UltraEdge firmware tree (no radio/src/thirdparty/ultraedge/ue_version.h)"

# ---- Radio table: tools/ultraedge/targets.tsv in the fork; in the monorepo, read build-fw-all-radios.sh directly ----
# Columns: key | label | PCB | PCBREV | flash-fit trims | extra | flash KB | screen | MCU | radio name | status | lines
# ("-" = empty). status (verified | community-unverified) and lines are added at staging from SUPPORTED-RADIOS.md, the
# one place that says which radios are verified; both are empty when run straight from the monorepo. status is for the
# main (3.x) line; lines lists the versions with their own SUPPORTED-RADIOS section (compat 'verified_section') that
# this radio is verified on.
TARGETS_FILE="$RECIPE/targets.tsv"
if [ -f "$TARGETS_FILE" ]; then TARGETS="$(grep -v "^#" "$TARGETS_FILE")"
elif [ -f "$RECIPE/../v4/build-fw-all-radios.sh" ]; then
  TARGETS="$(awk "/^TARGETS='/{f=1;next} f&&/^'/{exit} f" "$RECIPE/../v4/build-fw-all-radios.sh")"
else die "no targets.tsv next to $0"; fi
trow()   { printf '%s\n' "$TARGETS" | awk -F'|' -v k="$1" '$1==k'; }
tfield() { trow "$1" | awk -F'|' -v n="$2" '{v=$n; if (v=="-") v=""; print v}'; }

# ---- Which UltraEdge version is this tree? The compat row says which radios and UI modes it supports. ----
UE_BASE="$(sed -nE 's/^#define UE_FW_VERSION "([^"]*)".*/\1/p' "$UEDIR/ue_version.h" | sed -E 's/-selfbuild.*$//')"
UE_GIT="$(sed -nE 's/^#define UE_GIT "([^"]*)".*/\1/p' "$UEDIR/ue_version.h" | sed -E 's/\+.*$//')"
[ -n "$UE_BASE" ] && [ -n "$UE_GIT" ] || die "can't read UE_FW_VERSION / UE_GIT from ue_version.h"
COMPAT="$(python3 - "$RECIPE/compat.json" "$UE_BASE" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); row = next((v for v in d["versions"] if v["id"] == sys.argv[2]), None)
if row is None: print("UNKNOWN|||||||||"); sys.exit()
r = row["radios"]; e = row["edgetx"]; ex = row.get("exclude", {}); wa = row.get("warn", {})
print("%s|%s|%s|%s %s|%s|%s|%s|%s|%s" % (row["channel"], "*" if r == "*" else " ".join(r), " ".join(row["ui_companion"]),
      e["label"], e["commit"][:8], " ".join(ex), "\t".join(f"{k}={v}" for k, v in ex.items()),
      "\t".join(f"{k}={v}" for k, v in wa.items()), e.get("version_tag", ""), "1" if row.get("verified_section") else ""))
PY
)"
IFS='|' read -r CHANNEL ALLOWED COMP_SCREENS EDGETX_BASE EXCLUDED EXCL_WHY WARN_WHY ETX_TAG OWN_LIST <<< "$COMPAT"
[ "$CHANNEL" = UNKNOWN ] && die "UltraEdge $UE_BASE isn't in compat.json — use a published release tag (compat.json lists them)"
allowed() { [[ " $EXCLUDED " != *" $1 "* ]] && { [ "$ALLOWED" = "*" ] || [[ " $ALLOWED " == *" $1 "* ]]; }; }
why() { printf '%s\n' "$2" | tr '\t' '\n' | sed -n "s/^$1=//p"; }   # why <radio> <EXCL_WHY|WARN_WHY>
status() {  # status <radio>: verified on THIS version's line, else community-unverified (empty from the monorepo)
  if [ -z "$OWN_LIST" ]; then tfield "$1" 11
  elif [[ " $(tfield "$1" 12) " == *" $UE_BASE "* ]]; then echo verified
  elif [ -n "$(tfield "$1" 11)" ]; then echo community-unverified; fi; }

if [ "$RADIO" = list ] || [ -z "$RADIO" ]; then
  echo "UltraEdge $UE_BASE ($CHANNEL) — radios this tree builds:"
  printf '%s\n' "$TARGETS" | awk -F'|' 'NF>1' | while IFS='|' read -r k _ _ _ _ _ kb scr _ name _; do
    allowed "$k" && printf '  %-11s %-44s %5s KB  %-7s %s\n' "$k" "$name" "$kb" "$scr" "$(status "$k")"; done
  [ -z "$RADIO" ] && exit 1 || exit 0
fi
[[ "$RADIO" =~ ^[a-z0-9]+$ ]] && [ -n "$(trow "$RADIO")" ] || die "unknown radio '$RADIO' — run: $0 list"
mkdir -p "$OUT"; RESULT="$OUT/$RADIO.result"
allowed "$RADIO" || die "UltraEdge $UE_BASE ($CHANNEL) can't build '$RADIO'${EXCL_WHY:+: $(why "$RADIO" "$EXCL_WHY")}. Try the beta ($0 list)."
STATUS="$(status "$RADIO")"; STATUS="${STATUS:-unknown}"
[ "$STATUS" = verified ] || echo "** $RADIO is community-unverified on UltraEdge $UE_BASE: nobody has run the checklist on it yet (SUPPORTED-RADIOS.md). Bench-test before flying."
w="$(why "$RADIO" "$WARN_WHY")"; [ -n "$w" ] && echo "** UltraEdge $UE_BASE on $RADIO: $w"

flav="$(tfield "$RADIO" 2)"; pcb="$(tfield "$RADIO" 3)"; pcbrev="$(tfield "$RADIO" 4)"; fit="$(tfield "$RADIO" 5)"
extra_row="$(tfield "$RADIO" 6)"; flash=$(( $(tfield "$RADIO" 7) * 1024 )); screen="$(tfield "$RADIO" 8)"
UI="$UI_REQ"
if [ -z "$UI" ]; then [[ " $COMP_SCREENS " == *" $screen "* ]] && UI=COMPANION || UI=FULL; fi
case "$UI" in FULL) ;; COMPANION) [[ " $COMP_SCREENS " == *" $screen "* ]] || die "UltraEdge $UE_BASE has no companion build for $screen screens";; *) die "--ui must be FULL or COMPANION";; esac

# ---- Validate user input (it reaches a cmake command line and the radio's Version page) ----
[[ "$LABEL" =~ ^[A-Za-z0-9._-]{1,12}$ ]] || die "--label: 1-12 characters from A-Z a-z 0-9 . _ -"
[[ "$LANG_" =~ ^[A-Z]{2}$ ]] || die "--lang: a two-letter EdgeTX translation code (EN, DE, FR, …)"
for o in ${EXTRA[@]+"${EXTRA[@]}"}; do
  [[ "$o" =~ ^-D[A-Z][A-Z0-9_]*=[A-Za-z0-9_.]+$ ]] || die "--opt '$o': expected -DNAME=VALUE"
  case "${o%%=*}" in -DPCB|-DPCBREV|-DUSB_COMPANION|-DUI_COMPANION|-DTRANSLATIONS|-DUE_USBNET) die "--opt ${o%%=*} is set by this script";; esac
done

# ---- Identity (ADR-0040): label "<version>-selfbuild", GIT# = the UltraEdge source commit (+mod when edited) ----
VER="$UE_BASE-$LABEL"; KIND="self-build"
if [ "$OFFICIAL" = 1 ]; then
  REPO="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["repo"])' "$RECIPE/compat.json")"
  [ "${GITHUB_ACTIONS:-}" = true ] && [ "${GITHUB_REPOSITORY:-}" = "$REPO" ] || die "--official runs only in Actions on $REPO"
  VER="$UE_BASE"; KIND="official CI"
fi
# Edited sources make "+mod" — everything except the identity header (rewritten below) and build output.
if git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1 && [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no -- radio cmake CMakeLists.txt \
     ':!radio/src/thirdparty/ultraedge/ue_version.h' 2>/dev/null)" ]; then UE_GIT="$UE_GIT+mod"; fi
bdate="$(date -u '+%Y-%m-%d')"; btime="$(date -u '+%H:%M:%S')"
SAVED="$(mktemp)"; cp "$UEDIR/ue_version.h" "$SAVED"
trap 'cp "$SAVED" "$UEDIR/ue_version.h"; rm -f "$SAVED"' EXIT     # leave the tree as we found it
printf '// Generated by ue-build.sh (UltraEdge self-build, ADR-0049) — firmware identity (ADR-0040).\n#define UE_FW_VERSION "%s"\n#define UE_GIT "%s"\n#define UE_BUILD_DATE "%s"\n#define UE_BUILD_TIME "%s"\n' \
  "$VER" "$UE_GIT" "$bdate" "$btime" > "$UEDIR/ue_version.h"

UIFLAG=""; UISFX=""; [ "$UI" = COMPANION ] && { UIFLAG="-DUI_COMPANION=YES"; UISFX="-companion"; }
builddir="$SRC/build-ue-$RADIO$UISFX"
# A builddir keeps old -D values in its CMakeCache; start clean when the options changed (ADR-0041 lesson).
# A stable EdgeTX base names itself (compat edgetx.version_tag) in EdgeTX's own version string, which the bootloader
# and EdgeTX Buddy show; otherwise EdgeTX's default "pre-<x.y.z>-selfbuild" stays (the 3.0-dev lines).
if [ -n "$ETX_TAG" ]; then export EDGETX_VERSION_TAG="$ETX_TAG"; else unset EDGETX_VERSION_TAG; fi
OPTS="$(echo -DPCB=$pcb ${pcbrev:+-DPCBREV=$pcbrev} -DTRANSLATIONS=$LANG_ -DDEFAULT_TEMPLATE_SETUP=17 $fit $extra_row -DUSB_COMPANION=ON $UIFLAG ${EXTRA[*]+${EXTRA[*]}} | tr -s ' ')"
[ -f "$builddir/.ue-opts" ] && [ "$(cat "$builddir/.ue-opts")" != "$OPTS ${ETX_TAG}" ] && rm -rf "$builddir"
mkdir -p "$builddir"; echo "$OPTS ${ETX_TAG}" > "$builddir/.ue-opts"
gen=(); command -v ninja >/dev/null && gen=(-G Ninja)
echo ">> UltraEdge $VER ($CHANNEL) · $flav · UI=$UI · EdgeTX $EDGETX_BASE"
echo ">> cmake $OPTS"
# shellcheck disable=SC2086  # OPTS is a validated, space-separated option list
cmake -S "$SRC" -B "$builddir" ${gen[@]+"${gen[@]}"} $OPTS -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev \
  || die "[$flav] cmake configure failed"
LOG="$OUT/$RADIO.log"
if ! cmake --build "$builddir" --target firmware ${JOBS:+--parallel "$JOBS"} 2>&1 | tee "$LOG"; then
  # Same classification as build-fw-all-radios.sh: linker overflow (bytes over) vs the first compile error.
  over="$(grep -aoE "overflowed by [0-9]+ bytes" "$LOG" | head -1 | grep -oE '[0-9]+' || true)"
  [ -n "$over" ] && die "[$flav] doesn't fit: $over bytes over $((flash / 1024)) KB"
  die "[$flav] compile error: $(grep -aE 'error:|FATAL_ERROR|CMake Error' "$LOG" | head -1 | sed -E 's#.*/radio/src/##' | cut -c1-200)"
fi

bin="$builddir/arm-none-eabi/firmware.bin"; uf2="$builddir/arm-none-eabi/firmware.uf2"; ext=bin
[ -f "$bin" ] && [ "$bin" -nt "$UEDIR/ue_version.h" ] || die "[$flav] build produced no firmware"
[ -f "$uf2" ] && [ "$uf2" -nt "$UEDIR/ue_version.h" ] && ext=uf2   # H750 radios flash a .uf2
for want in "ultraedge-" "$VER" "$UE_GIT" "$bdate" "$btime"; do     # the official identity gate (build-fw.sh)
  LC_ALL=C grep -aqF -- "$want" "$bin" || die "[$flav] identity '$want' missing from the binary — not handed out"
done
name="UltraEdge-fw-$flav-$VER$UISFX.$ext"
if [ "$OFFICIAL" = 1 ]; then
  pub="$(awk -F'|' -v k="$RADIO" '!/^#/ && $1==k {print $2}' "$RECIPE/release-names.tsv" 2>/dev/null)"
  [ -n "$pub" ] || die "[$flav] no public file name for '$RADIO' in release-names.tsv"
  name="edgetx-ue-$pub-v$VER$UISFX.$ext"
fi
[ "$ext" = uf2 ] && cp "$uf2" "$OUT/$name" || cp "$bin" "$OUT/$name"
size=$(stat -c%s "$bin" 2>/dev/null || stat -f%z "$bin")
sha="$( (sha256sum "$OUT/$name" 2>/dev/null || shasum -a 256 "$OUT/$name") | cut -d' ' -f1)"
fwid="$(LC_ALL=C grep -aoE 'ultraedge-[a-z0-9+]+' "$bin" | head -1)"
if [ "$OFFICIAL" = 1 ]; then  # the release's own .txt format, as build-fw.sh writes it for the hardware-tested radios
  printf 'FW   : %s\nVERS : UE %s%s\nGIT# : %s\nDATE : %s %s\nBASE : EdgeTX %s\nUSBNET: off\nSHA256: %s\nBUILD: GitHub Actions %s (UTC), not hardware-tested unless SUPPORTED-RADIOS says verified\n' \
    "$fwid" "$VER" "${UISFX:+ companion}" "$UE_GIT" "$bdate" "$btime" "${EDGETX_BASE##* }" "$sha" "${UE_BUILD_URL:-local}" \
    > "$OUT/${name%.*}.txt"
else
{ printf 'FW    : %s\nVERS  : UE %s%s\nGIT#  : %s\nDATE  : %s %s (UTC)\n' \
    "$fwid" "$VER" "${UISFX:+ companion}" "$UE_GIT" "$bdate" "$btime"
  printf 'BASE  : EdgeTX %s\nUSBNET: off\nSOURCE: %s (commit %s)\nBUILD : %s (%s, %s)\nRADIO : %s\nOPTS  : %s\nSIZE  : %s bytes of %s KB\nSHA256: %s\n' \
    "$EDGETX_BASE" \
    "${UE_SOURCE:-$(git -C "$SRC" config --get remote.origin.url 2>/dev/null || echo local) @ $(git -C "$SRC" describe --tags --always 2>/dev/null || echo '?')}" \
    "$(git -C "$SRC" rev-parse --short=8 HEAD 2>/dev/null || echo '?')" "$KIND" "$CHANNEL" "${UE_BUILD_URL:-local}" \
    "$STATUS (see SUPPORTED-RADIOS.md)" "$OPTS" "$size" "$((flash / 1024))" "$sha"
} > "$OUT/${name%.*}.txt"
fi
printf 'OK|%s|%s|%s\n' "$name" "$size" "$((flash - size))" > "$RESULT"
echo ">> [$flav] OK -> $OUT/$name  ($size bytes, $((flash - size)) bytes free of $((flash / 1024)) KB)"
cat "$OUT/${name%.*}.txt"
