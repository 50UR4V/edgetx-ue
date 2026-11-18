/*
 * Copyright (C) 2026 Sourav Karmakar (UltraEdge)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
// UltraEdge companion UI mode (UI_COMPANION): minimal stubs for the native config-menu / view / voice
// symbols compiled out when the phone provides the UI. These satisfy link references from the kept
// boot / home / model-select path. Config menus & views are never entered in this mode, so empty
// definitions suffice for a size-measurement build (behaviour refinement is a follow-up).
#if defined(UI_COMPANION)

#include "edgetx.h"
#include "stamp.h"        // GIT_STR — the EdgeTX base commit (s38)
#include "hal/usb_driver.h"   // USB mode on the companion screen: get/setSelectedUsbMode, usbStop (s41, s43)
#include "serial.h"           // SP_VCP / serialStop for switching out of UltraEdge (serial) mode (s43)
#include "thirdparty/ultraedge/ue_version.h"   // UE_FW_VERSION/UE_GIT/UE_BUILD_DATE, written by build-fw.sh (s28, s38)
#include "gui/common/stdlcd/menus_common.h"
#include "translations/tts/tts.h"

// --- every removed screen lands HERE (owner field test 2026-10-12: the radio-menu key called a null handler and
// rebooted; model-select page keys / statistics / telemetry opened blank screens with no way out). One small
// screen that says where the settings are and always lets EXIT go back. ---
#if !defined(UE_USBNET)
// s61 -> s71 (owner D-BAY-5/6, 2026-11-11; every image since s71, not only the bay build): the title stays ULTRAEDGE; one
// "Connect:" line chooses where the app connects: USB UltraEdge / USB Joystick / USB SD card / BLE bay / BLE AUX, only
// the ones this radio has (BLE bay: bay build; BLE AUX: AUX1 in UltraEdge mode). [ENTER] moves the choice, shown as
// pending ("[USB SD card]", "Applies in 2 s, EXIT cancels"); it applies 2 s after the last ENTER, until power-off like
// s43 (s73, radio day F-1: switching at once dropped the app or handed the SD card to the phone mid-cycle). [EXIT]
// while pending keeps the mode in use; the next EXIT leaves. [ENTER long] saves what the line shows (USB default in
// the radio's own settings as s43 does, the wireless route in /UE/LINK.TXT). A USB cable always takes over from BLE (s52 arbitration). The decisions: ue_connect_ui.h.
// A USBNET build (s46, development only) keeps the s43/s46 "USB now:" screen below.
#include "thirdparty/ultraedge/ue_connect_ui.h"
#if defined(UE_AUXMOD)
  #define UE_SCREEN_VARIANT " companion auxmod"   // s60
#else
  #define UE_SCREEN_VARIANT " companion"
#endif
static const char* s_ueConnMsg = nullptr;   // one-shot note after ENTER long ("Saved")
#endif

void menuCompanionOnly(event_t event)
{
#if !defined(UE_USBNET)
  if (event == EVT_ENTRY || event == EVT_ENTRY_UP) {   // s73: a choice left pending by an earlier visit never applies
    ueconn::cancelPending();
    s_ueConnMsg = nullptr;
  }
  if (event == EVT_KEY_BREAK(KEY_EXIT) || event == EVT_KEY_LONG(KEY_EXIT)) {
    killEvents(event);
    s_ueConnMsg = nullptr;
    if (event == EVT_KEY_BREAK(KEY_EXIT) && ueconn::cancelPending()) return;   // s73: keep the mode in use, stay here
    ueconn::cancelPending();
    if (menuLevel > 0) popMenu(); else chainMenu(menuMainView);
    return;
  }
  if (event == EVT_KEY_BREAK(KEY_ENTER)) {
    killEvents(event);
    s_ueConnMsg = nullptr;
    ueconn::press();   // s73: moves the choice only; it applies after the pause
  }
  else if (event == EVT_KEY_LONG(KEY_ENTER)) {
    killEvents(event);
    s_ueConnMsg = ueconn::saveDefault();
  }
  ueconn::pendingTick();
  uint8_t now = ueconn::now(), shown = ueconn::shown(), saved = ueconn::saved();
  const char* pend = ueconn::pendingStatus("EXIT");
  lcdDrawText(LCD_W / 2, 0, "ULTRAEDGE", CENTERED);
  lcdInvertLine(0);
  lcdDrawText(LCD_W / 2, 9, "Settings are in the app.", CENTERED | SMLSIZE);
  // s75 (F-7): the label and the value in the small font, like the Default: row, so "UltraEdge (Serial)" (81 px) fits
  // beside "Connect:" (36 px) on 128 px.
  lcdDrawText(2, 18, "Connect:", SMLSIZE);
  if (pend) {   // s73: pending = bracketed, not inverted
    char b[24] = "[";
    strncat(b, ueconn::NAME[shown], sizeof(b) - 3);
    strcat(b, "]");
    // s75: "[UltraEdge (Serial)]" (89 px) has no room for the brackets: shown bare, still not inverted
    lcdDrawText(LCD_W - 2, 18, getTextWidth(b, 0, SMLSIZE) <= LCD_W - 44 ? b : ueconn::NAME[shown], RIGHT | SMLSIZE);
  }
  else lcdDrawText(LCD_W - 2, 18, ueconn::NAME[now], RIGHT | INVERS | SMLSIZE);
  lcdDrawText(2, 25, "Default:", SMLSIZE);
  lcdDrawText(LCD_W - 2, 25, ueconn::NAME[saved], RIGHT | SMLSIZE);
  lcdDrawText(LCD_W / 2, 33, pend ? pend : s_ueConnMsg ? s_ueConnMsg : ueconn::status(now), CENTERED | SMLSIZE);
  // Identity (s28, s38) on two lines to make room for the status line.
  lcdDrawText(LCD_W / 2, 40, "UE " UE_FW_VERSION UE_SCREEN_VARIANT, CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 47, UE_GIT " " UE_BUILD_DATE " " GIT_STR, CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 56, "ENT next  long:save  EXIT", CENTERED | SMLSIZE);
#else
  if (event == EVT_KEY_BREAK(KEY_EXIT) || event == EVT_KEY_LONG(KEY_EXIT)) {
    killEvents(event);
    if (menuLevel > 0) popMenu(); else chainMenu(menuMainView);
    return;
  }
  // s41/s43 (owner 2026-10-24): USB mode is the one radio setting the phone can't fix (if it isn't UltraEdge the app
  // can't connect), and this build has no Radio Setup page. Flying days must not ask on every connect, and a simulator
  // session must still be easy:
  //   [ENTER]       switch the USB mode NOW (UltraEdge app → Simulator/joystick → SD card), until power-off;
  //   [ENTER long]  make the current mode the saved default.
  // "Ask" as the saved default means UltraEdge in companion builds (main.cpp, s43), so a fresh radio just works.
  static const char* const usbName[] = { "UltraEdge", "Simulator", "SD card", "UltraEdge" };   // by usbMode
  int now = getSelectedUsbMode();
  if (now == USB_UNSELECTED_MODE) now = (g_eeGeneral.USBMode == USB_UNSELECTED_MODE) ? USB_SERIAL_MODE : g_eeGeneral.USBMode;
  if (event == EVT_KEY_BREAK(KEY_ENTER)) {
    killEvents(event);
    int next = (now == USB_SERIAL_MODE) ? USB_JOYSTICK_MODE : (now == USB_JOYSTICK_MODE) ? USB_MASS_STORAGE_MODE : USB_SERIAL_MODE;
    if (usbStarted()) {                 // restart USB in the new mode right away (handleUsbConnection starts it)
      if (getSelectedUsbMode() == USB_SERIAL_MODE) serialStop(SP_VCP);
      usbStop();
    }
    setSelectedUsbMode(next);
    now = next;
  }
  else if (event == EVT_KEY_LONG(KEY_ENTER)) {
    killEvents(event);
    g_eeGeneral.USBMode = (now == USB_SERIAL_MODE) ? USB_UNSELECTED_MODE : now;   // UltraEdge saved as "Ask" = default
    storageDirty(EE_GENERAL);
  }
  int saved = (g_eeGeneral.USBMode == USB_UNSELECTED_MODE) ? USB_SERIAL_MODE : g_eeGeneral.USBMode;
  lcdDrawText(LCD_W / 2, 0, "ULTRAEDGE", CENTERED);
  lcdInvertLine(0);
  lcdDrawText(LCD_W / 2, 9, "Settings are in the app.", CENTERED | SMLSIZE);
  lcdDrawText(2, 17, "USB now:");
  lcdDrawText(LCD_W - 2, 17, usbName[now & 3], RIGHT | INVERS);
  lcdDrawText(2, 25, "Default:", SMLSIZE);
  lcdDrawText(LCD_W - 2, 25, usbName[saved & 3], RIGHT | SMLSIZE);
  // This build has no Version page — show the same identity the Version page and the app show (s28, s38).
  lcdDrawText(LCD_W / 2, 33, "UE " UE_FW_VERSION " companion", CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 40, "git " UE_GIT " " UE_BUILD_DATE, CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 47, "EdgeTX " GIT_STR, CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 56, "ENT switch  long:save  EXIT", CENTERED | SMLSIZE);
#endif
}

// Unfilled tab-table slots are null; menus_common.cpp maps any null handler to menuCompanionOnly in this build.
// --- config-menu entry points / tab tables (referenced by view_main.cpp / model_select.cpp / menus_common.cpp) ---
void menuModelSetup(event_t event) { menuCompanionOnly(event); }
#define UE_PAGE { menuCompanionOnly, nullptr }
const MenuHandler menuTabModel[MENU_MODEL_PAGES_COUNT] = { UE_PAGE };
const MenuHandler menuTabGeneral[MENU_RADIO_PAGES_COUNT] = { UE_PAGE };

// --- native views the home/main loop can jump to (the phone shows these) ---
void menuStatisticsView(event_t event) { menuCompanionOnly(event); }
void menuViewTelemetry(event_t event) { menuCompanionOnly(event); }
void showTelemScreen(uint8_t) {}

// --- calibration: phone-only in this build (owner, 2026-10-12). EdgeTX chains here at boot when the radio has
// never been calibrated; say where to do it and let EXIT continue to the home view (as stock EdgeTX allows). ---
void menuFirstCalib(event_t event)
{
  if (event == EVT_KEY_BREAK(KEY_EXIT)) { chainMenu(menuMainView); return; }
  lcdDrawText(LCD_W / 2, 0, "CALIBRATION NEEDED", CENTERED);
  lcdInvertLine(0);
  lcdDrawText(LCD_W / 2, 18, "Connect the phone and", CENTERED);
  lcdDrawText(LCD_W / 2, 28, "calibrate in UltraEdge:", CENTERED);
  lcdDrawText(LCD_W / 2, 38, "System > Calibration", CENTERED);
  lcdDrawText(LCD_W / 2, 54, "[EXIT] skip", CENTERED);
}
void menuRadioCalibration(event_t event) { menuFirstCalib(event); }

// --- model notes (text viewer) ---
void pushModelNotes() {}
void readModelNotes() {}

// --- shared menu copy/move state (defined in the removed menu_model.cpp; used by kept model_inputs/
// model_mixes). Never exercised in companion mode (those editors aren't entered). ---
uint8_t s_copyMode = 0;
int8_t  s_copySrcRow = 0;
int8_t  s_copyTgtOfs = 0;

// --- language packs: EN-only in companion mode (voice moves to the phone; TRANSLATIONS=EN anyway).
// The kept translations.cpp walks this null-terminated list. ---
const LanguagePack * const languagePacks[] = { &enLanguagePack, nullptr };

#endif // UI_COMPANION
