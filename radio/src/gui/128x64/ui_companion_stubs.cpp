// UltraEdge companion UI mode (UI_COMPANION): minimal stubs for the native config-menu / view / voice
// symbols compiled out when the phone provides the UI. These satisfy link references from the kept
// boot / home / model-select path. Config menus & views are never entered in this mode, so empty
// definitions suffice for a size-measurement build (behaviour refinement is a follow-up).
#if defined(UI_COMPANION)

#include "edgetx.h"
#include "gui/common/stdlcd/menus_common.h"
#include "translations/tts/tts.h"

// --- config-menu entry points / tab tables (referenced by edgetx.cpp / main.cpp / model_select.cpp) ---
void menuModelSetup(event_t) {}
const MenuHandler menuTabModel[MENU_MODEL_PAGES_COUNT]   = {};
const MenuHandler menuTabGeneral[MENU_RADIO_PAGES_COUNT] = {};

// --- native views the home/main loop can jump to (phone shows these instead) ---
void menuStatisticsView(event_t) {}
void menuViewTelemetry(event_t) {}
void showTelemScreen(uint8_t) {}

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
