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
#include "thirdparty/ultraedge/ue_version.h"   // UE_FW_VERSION/UE_GIT/UE_BUILD_DATE, written by build-fw.sh (s28, s38)
#include "gui/common/stdlcd/menus_common.h"
#include "translations/tts/tts.h"

// --- every removed screen lands HERE (owner field test 2026-10-12: the radio-menu key called a null handler and
// rebooted; model-select page keys / statistics / telemetry opened blank screens with no way out). One small
// screen that says where the settings are and always lets EXIT go back. ---
void menuCompanionOnly(event_t event)
{
  if (event == EVT_KEY_BREAK(KEY_EXIT) || event == EVT_KEY_LONG(KEY_EXIT)) {
    killEvents(event);
    if (menuLevel > 0) popMenu(); else chainMenu(menuMainView);
    return;
  }
  lcdDrawText(LCD_W / 2, 0, "ULTRAEDGE", CENTERED);
  lcdInvertLine(0);
  lcdDrawText(LCD_W / 2, 11, "Model & radio settings", CENTERED);
  lcdDrawText(LCD_W / 2, 20, "are in the UltraEdge app.", CENTERED);
  // This build has no Version page — show the same identity the Version page and the app show (s28, s38).
  lcdDrawText(LCD_W / 2, 30, "UE " UE_FW_VERSION " companion", CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 37, "git " UE_GIT " " UE_BUILD_DATE, CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 44, "EdgeTX " GIT_STR, CENTERED | SMLSIZE);
  lcdDrawText(LCD_W / 2, 54, "[EXIT] back", CENTERED);
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
