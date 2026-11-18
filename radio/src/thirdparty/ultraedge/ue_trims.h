// SPDX-License-Identifier: GPL-2.0-only
// s68 (ADR-0066): the pure decisions of the trims work, kept free of EdgeTX headers so the host test (ue_trims_test.cpp)
// checks exactly what the radio runs.
#pragma once
#include <stdint.h>

namespace uetrims {

// MSG_TRIM_OP ops (protocol.h TrimOp mirrors these numbers; the app's Protocol.kt holds the same contract).
enum : uint8_t { OP_RESET = 0, OP_TRIMS_TO_SUBTRIMS = 1, OP_TRIM_TO_SUBTRIM = 2, OP_STICK_TO_SUBTRIM = 3, OP_COUNT = 4 };

// The NACK reasons, by number (protocol.h NackReason), so this header needs no firmware include.
enum : uint8_t { R_OK = 0, R_BAD_LENGTH = 3, R_OUT_OF_RANGE = 4, R_NOT_ATTACHED = 5, R_PUSH_STATE = 6 };

// Should the radio run this MSG_TRIM_OP? 0 = yes, else the NACK reason. `activeSaveOpen`: an app save of the ACTIVE model
// is being received (its reload would bring back the subtrims the op changes). A restore (new file) or an SD file write
// doesn't block. The op's own effect is EdgeTX's code; this is only the gate.
inline uint8_t trimOpCheck(bool attached, uint16_t len, uint8_t op, uint8_t arg, bool activeSaveOpen, uint8_t nChannels)
{
  if (!attached) return R_NOT_ATTACHED;
  if (len < 2) return R_BAD_LENGTH;
  if (activeSaveOpen) return R_PUSH_STATE;
  if (op >= OP_COUNT) return R_OUT_OF_RANGE;
  if ((op == OP_TRIM_TO_SUBTRIM || op == OP_STICK_TO_SUBTRIM) && arg >= nChannels) return R_OUT_OF_RANGE;
  return R_OK;
}

// Trim and GVar VALUES (C2, ADR-0014) kept across the reload that follows an app save of the active model. FMD is
// EdgeTX's FlightModeData (or a host stand-in): trim[t].value (11-bit) + trim[t].mode, and gvars[g]. Only the values are
// kept; each trim's mode and everything else stay as the file has them.
template <uint8_t NFM, uint8_t NTRIM, uint8_t NGV>
struct LiveValues {
  int16_t trim[NFM][NTRIM];
  int16_t gv[NFM][NGV];

  template <class FMD> void take(const FMD* fms)
  {
    for (uint8_t fm = 0; fm < NFM; ++fm) {
      for (uint8_t t = 0; t < NTRIM; ++t) trim[fm][t] = fms[fm].trim[t].value;
      for (uint8_t g = 0; g < NGV; ++g) gv[fm][g] = fms[fm].gvars[g];
    }
  }

  // Puts the values back into the freshly loaded model. True if any differed from the file (the radio must then save
  // the model again, or the file on the SD card keeps the old values).
  template <class FMD> bool putBack(FMD* fms) const
  {
    bool changed = false;
    for (uint8_t fm = 0; fm < NFM; ++fm) {
      for (uint8_t t = 0; t < NTRIM; ++t)
        if (fms[fm].trim[t].value != trim[fm][t]) { fms[fm].trim[t].value = trim[fm][t]; changed = true; }
      for (uint8_t g = 0; g < NGV; ++g)
        if (fms[fm].gvars[g] != gv[fm][g]) { fms[fm].gvars[g] = gv[fm][g]; changed = true; }
    }
    return changed;
  }
};

// ---- The live flight state kept across the same reload (owner 2026-11-14, "fix it in s68 now") ----
// The runtime state (timers, telemetry values + min/max, logical-switch contexts, special-function state) is not in the
// model file, so the reload keeps it whole. Per item, EdgeTX's own menus reset it only when its definition changes in
// one of these ways, and so does the reload:
//   timer:            start value changed                     -> timerReset (the Timer menu's "Start" calls timerSet)
//   logical switch:   function changed, or removed            -> back to its model-load state (sticky persistence too)
//   special function: function changed, or removed           -> its active/last-run state cleared (the SF menu CFN_RESET)
//   sensor:           what its value means changed (custom id/instance/subId, calculated formula, type, unit,
//                     precision), or removed                  -> telemetryItems[i].clear() (the sensor menus do this)
// Anything else (an LS's values or delay, a timer's mode or switch, a sensor's name or ratio) keeps its state, as an
// edit on the radio does.

// What a sensor's value means. Equal keys keep the live value, min and max.
struct SensorKey {
  uint16_t id;      // custom: data id (calculated sensors keep their persistent value here: not part of the key)
  uint8_t  id2;     // custom: instance; calculated: formula
  uint8_t  subId;
  uint8_t  kind;    // type (bit 7: calculated) + unit (bits 0-5)
  uint8_t  prec;
  bool operator==(const SensorKey& o) const { return id == o.id && id2 == o.id2 && subId == o.subId && kind == o.kind && prec == o.prec; }
  bool operator!=(const SensorKey& o) const { return !(*this == o); }
};
inline SensorKey sensorKey(bool calculated, uint16_t id, uint8_t id2, uint8_t subId, uint8_t unit, uint8_t prec)
{
  SensorKey k;
  k.id = calculated ? 0 : id;
  k.id2 = id2; k.subId = subId;
  k.kind = (uint8_t)((calculated ? 0x80 : 0) | (unit & 0x3F));
  k.prec = prec;
  return k;
}

// Calls f(i) for every item whose key differs between before and after.
template <class K, class F>
inline void forChanged(const K* before, const K* after, uint16_t n, F f)
{
  for (uint16_t i = 0; i < n; ++i)
    if (before[i] != after[i]) f(i);
}

// A 16-bit running checksum (CRC16-CCITT) over byte ranges: the "did the model's script set change" test.
inline uint16_t crcAdd(uint16_t crc, const void* data, uint32_t len)
{
  const uint8_t* p = (const uint8_t*)data;
  while (len--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (uint8_t b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// ---- Option b (owner 2026-11-14, "go with b"): the mixer is held while the app-save reload rebuilds the model ----
// No frame is computed or sent from a half-loaded model; the receivers keep the last good values. The hold is bounded
// by EdgeTX's own model-load gap: a model select stops the pulses for 200 ms plus the load (preModelLoad). A hold longer
// than that is never repeated: from then on (this boot) an app save stops the pulses like EdgeTX's load does. A file
// larger than HOLD_MAX_FILE never takes the hold path at all.
enum : uint16_t { HOLD_CAP_MS = 200 };
static const uint32_t HOLD_MAX_FILE = 32768;   // bytes; models are 3-20 KB

struct HoldStats {
  uint16_t last = 0, max = 0, count = 0;   // ms, ms, app-save reloads since boot
  bool lastStopped = false;                // the last reload stopped the pulses (EdgeTX's way) instead of holding
  bool useStop = false;                    // a hold went over HOLD_CAP_MS: stop the pulses from now on
};
inline bool reloadStopsPulses(const HoldStats& s, uint32_t fileBytes) { return s.useStop || fileBytes > HOLD_MAX_FILE; }
inline void holdRecord(HoldStats& s, uint32_t ms, bool stopped)
{
  uint16_t v = ms > 65535 ? 65535 : (uint16_t)ms;
  s.last = v; if (v > s.max) s.max = v;
  if (s.count < 65535) s.count++;
  s.lastStopped = stopped;
  if (!stopped && v > HOLD_CAP_MS) s.useStop = true;
}
// "last=12ms max=40ms saves=3 mode=hold cap=200ms\n" (mode=stop: the pulses were stopped; next=stop: the cap was hit)
inline uint8_t holdLine(char* out, uint8_t cap, const HoldStats& s)
{
  uint8_t n = 0;
  auto put = [&](const char* t) { while (*t && n + 1 < cap) out[n++] = *t++; };
  auto num = [&](uint32_t v) { char b[6]; uint8_t k = 0; do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 6);
                               while (k && n + 1 < cap) out[n++] = b[--k]; };
  put("last="); num(s.last); put("ms max="); num(s.max); put("ms saves="); num(s.count);
  put(s.lastStopped ? " mode=stop" : " mode=hold"); put(" cap="); num(HOLD_CAP_MS); put("ms");
  if (s.useStop) put(" next=stop");
  put("\n");
  out[n] = 0;
  return n;
}

}  // namespace uetrims
