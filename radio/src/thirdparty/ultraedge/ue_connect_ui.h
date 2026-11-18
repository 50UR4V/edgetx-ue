// SPDX-License-Identifier: GPL-2.0-only
// s71 (owner D-BAY-5/6, 2026-11-11; docs/analysis/WIRELESS-BAY-MODULE.md §4): the agreed ULTRAEDGE screen in EVERY image.
// One "Connect:" choice cycles USB UltraEdge -> USB Joystick -> USB SD card -> BLE bay -> BLE AUX, showing only what this
// radio has (BLE bay: a bay build; BLE AUX: AUX1 in UltraEdge mode), a saved default, and a status line. Shared by the
// B&W companion screen (gui/128x64/ui_companion_stubs.cpp) and the colour page (gui/colorlcd/radio/radio_setup.cpp,
// SYS › Radio settings › UltraEdge). s73: a choice applies only after a 2 s pause (EXIT cancels), never straight from
// a key. Main (menus/UI) task only. The pure choices are ue_bay_logic.h (host-tested).
// One holder of the companion stream (s52): a USB cable in UltraEdge mode takes over from BLE; unplugging returns to the
// chosen BLE route. USB Network (s46, UE_USBNET) keeps its own screen; release images are built USBNET=OFF.
#pragma once
#include "edgetx.h"
#include "hal/usb_driver.h"
#include "serial.h"
#include "ue_bay_logic.h"

#if defined(UE_AUXLINK)
extern "C" uint8_t ueRouteGet();
extern "C" uint8_t ueRouteSavedGet();
extern "C" bool ueRouteSet(uint8_t r, bool save);
extern "C" bool ueRouteSave(uint8_t r);   // s73: write /UE/LINK.TXT only (a pending choice saved as Default)
extern "C" bool ueAuxAvailable();
extern "C" uint8_t ueWirelessStatus();
#else   // no wireless transport in this build: the three USB choices only
static inline uint8_t ueRouteGet() { return uebay::ROUTE_USB; }
static inline uint8_t ueRouteSavedGet() { return uebay::ROUTE_USB; }
static inline bool ueRouteSet(uint8_t, bool) { return true; }
static inline bool ueRouteSave(uint8_t) { return true; }
static inline bool ueAuxAvailable() { return false; }
static inline uint8_t ueWirelessStatus() { return 3; }
#endif

namespace ueconn {
using namespace uebay;

#if defined(UE_BAYLINK)
static const bool BAY = true;
#else
static const bool BAY = false;
#endif
// s75 (F-7): the app's USB mode has the same name as in the USB popup and the Radio settings field.
static const char* const NAME[C_N] = { "UltraEdge (Serial)", "USB Joystick", "USB SD card", "BLE bay", "BLE AUX" };

inline bool auxAvail() { return ueAuxAvailable(); }
inline uint8_t usbKind(int m) { return m == USB_JOYSTICK_MODE ? USB_KIND_JOY : m == USB_MASS_STORAGE_MODE ? USB_KIND_SD : USB_KIND_SERIAL; }
inline int usbMode(uint8_t k) { return k == USB_KIND_JOY ? USB_JOYSTICK_MODE : k == USB_KIND_SD ? USB_MASS_STORAGE_MODE : USB_SERIAL_MODE; }

// "Ask" as the saved USB mode: UltraEdge in a companion build (main.cpp, s43); a full (colour) build asks on plug.
inline bool askOnPlug()
{
#if defined(UI_COMPANION)
  return false;
#else
  return getSelectedUsbMode() == USB_UNSELECTED_MODE && g_eeGeneral.USBMode == USB_UNSELECTED_MODE;
#endif
}
inline int usbNow()
{
  int m = getSelectedUsbMode();
  if (m == USB_UNSELECTED_MODE) m = (g_eeGeneral.USBMode == USB_UNSELECTED_MODE) ? USB_SERIAL_MODE : g_eeGeneral.USBMode;
  return m;
}
inline uint8_t now() { return connOf(usbKind(usbNow()), ueRouteGet(), BAY, auxAvail()); }
inline uint8_t saved()
{
  int m = (g_eeGeneral.USBMode == USB_UNSELECTED_MODE) ? USB_SERIAL_MODE : g_eeGeneral.USBMode;
  return connOf(usbKind(m), ueRouteSavedGet(), BAY, auxAvail());
}
inline uint8_t next(uint8_t c) { return connNext(c, BAY, auxAvail()); }
inline bool available(uint8_t c) { return connAvailable(c, BAY, auxAvail()); }

// The USB mode for this session (until power-off, s43): restart USB in the new mode right away (handleUsbConnection).
inline void setUsbNow(int mode)
{
  if (getSelectedUsbMode() == mode) return;
  if (usbStarted()) {
    if (getSelectedUsbMode() == USB_SERIAL_MODE) serialStop(SP_VCP);
    usbStop();
  }
  setSelectedUsbMode(mode);
}
// Apply this choice now (s73: only from pendingTick(), after the pause; never straight from a key).
inline void choose(uint8_t c)
{
  if (!available(c)) return;
  setUsbNow(usbMode(connUsb(c)));
  ueRouteSet(routeFor(c, ueRouteGet(), BAY, auxAvail()), false);
}

// s73 (radio day F-1, owner 2026-11-15 "pick, then apply after a pause"): ENTER / a list pick only moves the choice;
// it shows as pending and applies uebay::PEND_TICKS (2 s) after the last ENTER; EXIT (RTN) cancels it and keeps the
// mode in use. The timing is uebay::ConnPending (pure, host-tested). One pending choice for the radio (one screen).
inline ConnPending& pending() { static ConnPending p; return p; }
inline bool isPending() { return pending().active(); }
// What the Connect: line shows: the pending choice, else the mode in use.
inline uint8_t shown() { return pending().shown(now()); }
inline void press() { pending().press(now(), BAY, auxAvail(), g_tmr10ms); }        // B&W ENTER
inline void pick(uint8_t c) { if (available(c)) pending().pick(c, now(), g_tmr10ms); }   // colour list pick
inline bool cancelPending() { return pending().cancel(); }                             // EXIT / RTN
inline void holdPending() { pending().hold(g_tmr10ms); }
// Every UI tick of the screen that shows it: apply the pending choice once its pause is over.
inline void pendingTick()
{
  uint8_t c = pending().due(g_tmr10ms);
  if (c < C_N) {
    TRACE("UE connect: apply %d after the pause", (int)c);
    choose(c);
  }
}
// "Applies in 2 s, EXIT cancels" (nullptr when nothing is pending).
inline const char* pendingStatus(const char* key)
{
  static char buf[40];
  if (!isPending()) return nullptr;
  uint8_t s = pending().secondsLeft(g_tmr10ms);
  return pendingText(buf, s ? s : 1, key);
}

// ENTER long: the choice on the Connect: line becomes the default. USB in the radio's own USB mode (companion:
// UltraEdge saved as "Ask", s43), the wireless route in /UE/LINK.TXT (radio-written, never radio.yml), only where a BLE
// choice is offered. s73: with a choice pending, ENTER long saves THAT choice (what the line shows) as the Default
// without applying it now: the pause restarts, it applies 2 s later like any ENTER, and EXIT still keeps the mode in
// use (the saved Default stays). Nothing ever applies straight from a key.
inline const char* saveDefault()
{
  bool pend = isPending();
  uint8_t c = shown();
  int m = usbMode(connUsb(c));
#if defined(UI_COMPANION)
  g_eeGeneral.USBMode = (m == USB_SERIAL_MODE) ? USB_UNSELECTED_MODE : m;
#else
  g_eeGeneral.USBMode = m;
#endif
  storageDirty(EE_GENERAL);
  if (pend) holdPending();
  bool aux = auxAvail();
  if (!routeSaveWanted(BAY, aux)) return "Saved";
  uint8_t r = routeFor(c, ueRouteGet(), BAY, aux);
  return (pend ? ueRouteSave(r) : ueRouteSet(r, true)) ? "Saved" : "SD card: not saved";
}

inline const char* status(uint8_t c)
{
  switch (c) {
    case C_JOY: return "USB: simulator";
    case C_SD:  return usbPlugged() ? "USB: SD card, radio paused" : "USB: SD card when plugged";
    case C_USB:
      if (askOnPlug()) return "USB: asks when plugged";
      return usbPlugged() ? "USB: app on cable" : "USB: plug in the cable";
    default: break;
  }
  bool bay = (c == C_BAY);
  switch (ueWirelessStatus()) {
    case 1:  return bay ? "Bay: phone linked" : "AUX1: phone linked";
    case 2:  return "Armed: link off (switch)";
    case 3:  return bay ? "Bay: starting" : "AUX1: not UltraEdge";
    case 4:  return "USB cable: app on USB";
    case 5:  return "Bay: no C3 answer";        // s62 (S.Port route): powered, polling, nothing answers yet
    case 6:  return "Bay C3: update it";        // s62: the C3 speaks another UE-HD version
    case 11: return "Bay used by model RF";
    case 12: return "Bay used by trainer";
    case 13: return "Bay port failed";
    case 14: return "Bay: internal RF on";      // s62: the QX7's internal module shares the S.Port UART
    default: return bay ? "Bay on, waiting for phone" : "AUX1 on, waiting for phone";
  }
}

}  // namespace ueconn
