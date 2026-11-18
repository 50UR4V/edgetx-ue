// SPDX-License-Identifier: GPL-2.0-only
// s61 (ADR-0064, prototype): the pure decisions of the bay link and the Connect: line (every image since s71), kept free of EdgeTX headers so
// the host test (ue_bay_logic_test.cpp) checks exactly what the radio runs.
#pragma once
#include <stdint.h>
#include <string.h>

namespace uebay {

enum Route : uint8_t { ROUTE_USB = 0, ROUTE_AUX = 1, ROUTE_BAY = 2 };

// /UE/LINK.TXT: the first word. Missing file or anything unknown = AUX, which is the s52/s59 behaviour before s61
// (the AUX transport runs only if AUX1 is in UltraEdge mode, so a radio without the file behaves exactly as before).
inline uint8_t parseRoute(const char* b, unsigned n)
{
  if (n >= 3 && !strncmp(b, "BAY", 3)) return ROUTE_BAY;
  if (n >= 3 && !strncmp(b, "USB", 3)) return ROUTE_USB;
  return ROUTE_AUX;
}
inline const char* routeWord(uint8_t r) { return r == ROUTE_BAY ? "BAY\n" : r == ROUTE_USB ? "USB\n" : "AUX\n"; }

// Why the bay is not the companion's: 0 = free, 1 = the model's external RF module (set, or pulses still holds it),
// 2 = a bay trainer (CPPM/SBUS on the heartbeat pin), 4 = s62 route B only: the internal module, which shares the
// S.Port UART with the bay on the QX7 (XJT telemetry). 3 is the port-init failure (set by the caller).
inline uint8_t bayBlock(bool extModuleSet, bool pulsesHoldsBay, bool bayTrainer, bool intModuleOnSport = false)
{
  if (extModuleSet || pulsesHoldsBay) return 1;
  if (bayTrainer) return 2;
  if (intModuleOnSport) return 4;
  return 0;
}

enum Conn : uint8_t { C_USB = 0, C_JOY, C_SD, C_BAY, C_AUX, C_N };
enum UsbKind : uint8_t { USB_KIND_SERIAL = 0, USB_KIND_JOY, USB_KIND_SD };

// s71 (owner D-BAY-5/6, 2026-11-11, in EVERY image): which wireless choices this radio offers. BLE bay only in a bay
// build (UE_BAYLINK); BLE AUX only when AUX1 exists AND is in UltraEdge mode (s59: set from the app, or SYS › Hardware).
// A radio with neither shows the three USB choices.
// What the Connect: line shows for a USB mode + route.
inline uint8_t connOf(uint8_t usb, uint8_t route, bool bayAvail, bool auxAvail)
{
  if (usb == USB_KIND_JOY) return C_JOY;
  if (usb == USB_KIND_SD) return C_SD;
  if (route == ROUTE_BAY && bayAvail) return C_BAY;
  if (route == ROUTE_AUX && auxAvail) return C_AUX;
  return C_USB;
}
inline bool connAvailable(uint8_t c, bool bayAvail, bool auxAvail)
{
  return c < C_N && (c != C_BAY || bayAvail) && (c != C_AUX || auxAvail);
}
inline uint8_t connNext(uint8_t c, bool bayAvail, bool auxAvail)
{
  uint8_t n = c;
  do { n = (uint8_t)((n + 1) % C_N); } while (!connAvailable(n, bayAvail, auxAvail));
  return n;
}
// s71: the route a LINK.TXT word means in this build. "BAY" left by a bay image on a radio now running a release image
// falls back to AUX (no file), so the radio behaves as before s61 instead of holding a route it doesn't have.
inline uint8_t routeInBuild(uint8_t r, bool bayBuilt) { return (r == ROUTE_BAY && !bayBuilt) ? (uint8_t)ROUTE_AUX : r; }
// s71: the route a choice sets. A USB-only choice turns wireless off only when a wireless choice is on offer; otherwise
// the route is left as it is (AUX = the s52/s59 default), so turning AUX1 to UltraEdge later from the app still works.
inline uint8_t routeFor(uint8_t c, uint8_t curRoute, bool bayAvail, bool auxAvail)
{
  if (c == C_BAY) return ROUTE_BAY;
  if (c == C_AUX) return ROUTE_AUX;
  return (bayAvail || auxAvail) ? (uint8_t)ROUTE_USB : curRoute;
}
// s71: ENTER long writes /UE/LINK.TXT only where a wireless choice is on offer (a stock QX7 never gets the file).
inline bool routeSaveWanted(bool bayAvail, bool auxAvail) { return bayAvail || auxAvail; }
// What choosing a Connect: entry sets: the USB mode and the wireless route (USB-only choices turn wireless off).
inline uint8_t connUsb(uint8_t c) { return c == C_JOY ? USB_KIND_JOY : c == C_SD ? USB_KIND_SD : USB_KIND_SERIAL; }
inline uint8_t connRoute(uint8_t c) { return c == C_BAY ? ROUTE_BAY : c == C_AUX ? ROUTE_AUX : ROUTE_USB; }

// s73 (radio day F-1, owner 2026-11-15 "pick, then apply after a pause"): ENTER only moves the Connect: choice; the
// choice shows as pending ("[USB SD card]", "Applies in 2 s" on the status line) and applies PEND_TICKS after the last
// ENTER. EXIT while it is pending cancels it and keeps the current mode. So cycling past USB Joystick or USB SD card
// with the cable in no longer drops the app or hands the SD card to the computer. Ticks are g_tmr10ms (10 ms, wraps).
static const uint32_t PEND_TICKS = 200;   // 2.0 s
struct ConnPending {
  uint8_t choice = C_N;   // C_N = nothing pending
  uint32_t at = 0;        // tick of the last ENTER (or of the last hold)
  bool active() const { return choice < C_N; }
  // What the Connect: line shows: the pending choice, else the mode in use.
  uint8_t shown(uint8_t cur) const { return active() ? choice : cur; }
  // B&W ENTER: the next choice after the one shown. Coming back round to the mode in use = nothing pending.
  uint8_t press(uint8_t cur, bool bayAvail, bool auxAvail, uint32_t t)
  {
    uint8_t n = connNext(shown(cur), bayAvail, auxAvail);
    pick(n, cur, t);
    return n;
  }
  // Colour list pick (and press): pending unless it is the mode in use.
  void pick(uint8_t c, uint8_t cur, uint32_t t)
  {
    if (c >= C_N || c == cur) { choice = C_N; return; }
    choice = c; at = t;
  }
  // Restart the pause: ENTER long (save as Default) and, on the colour page, while its list or another window is open.
  void hold(uint32_t t) { if (active()) at = t; }
  // EXIT: true when it cancelled something (the screen then stays; the next EXIT leaves).
  bool cancel() { bool was = active(); choice = C_N; return was; }
  // Every UI tick: the choice to apply now, once (C_N = nothing to apply yet).
  uint8_t due(uint32_t t)
  {
    if (!active() || (uint32_t)(t - at) < PEND_TICKS) return C_N;
    uint8_t c = choice;
    choice = C_N;
    return c;
  }
  // Whole seconds left, rounded up, for "Applies in 2 s" (0 = nothing pending or due now).
  uint8_t secondsLeft(uint32_t t) const
  {
    uint32_t e = (uint32_t)(t - at);
    if (!active() || e >= PEND_TICKS) return 0;
    return (uint8_t)((PEND_TICKS - e + 99) / 100);
  }
};
// "Applies in 2 s, EXIT cancels" into buf (>= 40 bytes); key = the radio's name for the cancel key (EXIT / RTN).
inline const char* pendingText(char* buf, uint8_t secs, const char* key)
{
  static const char a[] = "Applies in ";
  char* p = buf;
  memcpy(p, a, sizeof(a) - 1); p += sizeof(a) - 1;
  *p++ = (char)('0' + (secs > 9 ? 9 : secs));
  memcpy(p, " s, ", 4); p += 4;
  size_t k = strlen(key); if (k > 8) k = 8;
  memcpy(p, key, k); p += k;
  memcpy(p, " cancels", 9);
  return buf;
}

// s64 (ADR-0064 decision 10): what a model file does with the bay, read line by line (f_gets on the radio) before the
// radio loads it. EdgeTX writes 3-space indents and omits all-zero structs, so a model without a `moduleData` entry 1
// has External RF Off. A bay trainer (SBUS/CPPM from the external module) takes the bay too, and on the QX7 so does
// Internal RF (its XJT shares the S.Port UART, s62). Feed only whole lines (skip the rest of an over-long line).
struct ModelScan {
  uint8_t sec = 0;    // top-level section: 1 moduleData, 2 trainerData
  int8_t mod = -1;    // moduleData entry being read (0 internal, 1 external)
  bool ext = false, intl = false, intXjt = false, trainer = false;   // s72: intXjt = the internal module is an XJT (PXX1)

  static bool key(const char* s, const char* k) { return !strncmp(s, k, strlen(k)); }
  // The value after "key:" is exactly w (quotes and spaces around it ignored).
  static bool val(const char* s, const char* w)
  {
    while (*s == ' ' || *s == '"') s++;
    size_t n = strlen(w);
    if (strncmp(s, w, n)) return false;
    char c = s[n];
    return c == 0 || c == '"' || c == ' ' || c == '\r' || c == '\n';
  }
  static bool blank(const char* s)
  {
    while (*s == ' ' || *s == '"') s++;
    return *s == 0 || *s == '\r' || *s == '\n';
  }
  void line(const char* s)
  {
    unsigned ind = 0;
    while (s[ind] == ' ') ind++;
    const char* k = s + ind;
    if (!*k || *k == '\r' || *k == '\n') return;
    if (ind == 0) { sec = key(k, "moduleData:") ? 1 : key(k, "trainerData:") ? 2 : 0; mod = -1; return; }
    if (sec == 1 && ind == 3) { mod = (k[0] == '0' || k[0] == '1') && k[1] == ':' ? (int8_t)(k[0] - '0') : -1; return; }
    if (sec == 1 && ind == 6 && mod >= 0 && key(k, "type:")) {
      bool on = !val(k + 5, "TYPE_NONE") && !blank(k + 5);
      if (mod == 1) ext = on; else { intl = on; intXjt = val(k + 5, "TYPE_XJT_PXX1"); }
      return;
    }
    if (sec == 2 && ind == 3 && key(k, "mode:")) trainer = val(k + 5, "MASTER_SBUS_EXT") || val(k + 5, "MASTER_CPPM_EXT");
  }
  // s72: only an internal XJT shares S.Port (QX7, X9D...); a 4-in-1 or ELRS internal module has its own UART.
  bool takesBay(bool internalSharesSport) const { return ext || trainer || (internalSharesSport && intXjt); }
};

}  // namespace uebay
