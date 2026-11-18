// SPDX-License-Identifier: GPL-2.0-only
// Host test for s61's bay-link decisions (ue_bay_logic.h). Not compiled into the firmware.
//   c++ -std=c++17 -Wall -Wextra -o /tmp/ue_bay_logic_test ue_bay_logic_test.cpp && /tmp/ue_bay_logic_test
#include "ue_bay_logic.h"
#include <cstdio>
using namespace uebay;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static ModelScan scan(const char* const* lines)
{
  ModelScan s;
  for (; *lines; ++lines) s.line(*lines);
  return s;
}

int main()
{
  // s64: does the model about to load take the bay? (the real model05.yml shape: 3-space indents, "moduleData: ")
  {
    const char* crsf[] = { "semver: 2.11.0\n", "header: \n", "   name: \"Plane\"\n", "moduleData: \n", "   1:\n",
                           "      type: TYPE_CROSSFIRE\n", "      subType: 0\n", "inputNames: \n", "   0:\n",
                           "      val: \"Ail\"\n", nullptr };
    CHECK(scan(crsf).takesBay(false) && scan(crsf).ext);
    const char* none[] = { "header: \n", "   name: \"Glider\"\n", "inputNames: \n", nullptr };   // omitted = Off
    CHECK(!scan(none).takesBay(true));
    const char* off[] = { "moduleData: \n", "   1:\n", "      type: TYPE_NONE\r\n", nullptr };
    CHECK(!scan(off).takesBay(true));
    const char* intOnly[] = { "moduleData: \n", "   0:\n", "      type: TYPE_XJT_PXX1\n", "      subType: 0\n", nullptr };
    CHECK(!scan(intOnly).takesBay(false));     // Pocket: the internal module has its own UART
    CHECK(scan(intOnly).takesBay(true));       // QX7: the internal XJT shares S.Port with the bay
    // s72: a radio built with internal-XJT support (TX16S, X10) whose internal module is a 4-in-1 or ELRS: no S.Port share
    const char* intMulti[] = { "moduleData: \n", "   0:\n", "      type: TYPE_MULTIMODULE\n", nullptr };
    CHECK(!scan(intMulti).takesBay(true) && scan(intMulti).intl && !scan(intMulti).intXjt);
    const char* intCrsf[] = { "moduleData: \n", "   0:\n", "      type: \"TYPE_CROSSFIRE\"\n", nullptr };
    CHECK(!scan(intCrsf).takesBay(true));
    const char* sbus[] = { "trainerData: \n", "   mode: MASTER_SBUS_EXT\n", "   channelsStart: 0\n", nullptr };
    CHECK(scan(sbus).takesBay(false));
    const char* cppm[] = { "trainerData: \n", "   mode: \"MASTER_CPPM_EXT\"\n", nullptr };
    CHECK(scan(cppm).takesBay(false));
    const char* jack[] = { "trainerData: \n", "   mode: MASTER_TRAINER_JACK\n", nullptr };
    CHECK(!scan(jack).takesBay(true));
    // "type:" under another section or module entry, or deeper inside the external entry, doesn't count
    const char* other[] = { "moduleData: \n", "   1:\n", "      mod: \n", "         crsf: \n",
                            "            type: TYPE_PPM\n", "telemetrySensors: \n", "   0:\n", "      type: TYPE_PPM\n", nullptr };
    CHECK(!scan(other).takesBay(true));
    const char* after[] = { "moduleData: \n", "   1:\n", "      type: TYPE_PPM\n", "   0:\n", "      type: TYPE_NONE\n", nullptr };
    CHECK(scan(after).ext && !scan(after).intl);
  }

  // LINK.TXT
  CHECK(parseRoute("BAY\n", 4) == ROUTE_BAY);
  CHECK(parseRoute("BAY", 3) == ROUTE_BAY);
  CHECK(parseRoute("USB\r\n", 5) == ROUTE_USB);
  CHECK(parseRoute("AUX\n", 4) == ROUTE_AUX);
  CHECK(parseRoute("", 0) == ROUTE_AUX);            // absent/empty: the pre-s61 behaviour
  CHECK(parseRoute("BA", 2) == ROUTE_AUX);          // truncated
  CHECK(parseRoute("bay", 3) == ROUTE_AUX);         // the radio writes upper case only
  for (uint8_t r = 0; r <= ROUTE_BAY; ++r) CHECK(parseRoute(routeWord(r), 4) == r);   // write -> read round trip
  CHECK(strlen(routeWord(ROUTE_USB)) == 4 && strlen(routeWord(ROUTE_AUX)) == 4 && strlen(routeWord(ROUTE_BAY)) == 4);

  // The bay is the model's first
  CHECK(bayBlock(false, false, false) == 0);
  CHECK(bayBlock(true, false, false) == 1);
  CHECK(bayBlock(false, true, false) == 1);         // type already NONE, pulses not yet released
  CHECK(bayBlock(false, false, true) == 2);
  CHECK(bayBlock(true, false, true) == 1);          // RF wins the message
  CHECK(bayBlock(false, false, false, true) == 4);  // s62 route B: the internal module shares S.Port
  CHECK(bayBlock(true, false, false, true) == 1);   // the bay's own RF module still wins the message
  CHECK(bayBlock(false, false, true, true) == 2);

  // Connect: display (s71: bayAvail = a bay build, auxAvail = AUX1 exists and is in UltraEdge mode)
  CHECK(connOf(USB_KIND_SERIAL, ROUTE_USB, true, true) == C_USB);
  CHECK(connOf(USB_KIND_SERIAL, ROUTE_BAY, true, false) == C_BAY);
  CHECK(connOf(USB_KIND_SERIAL, ROUTE_BAY, false, true) == C_USB);    // BAY without a bay build: never shown
  CHECK(connOf(USB_KIND_SERIAL, ROUTE_AUX, false, true) == C_AUX);
  CHECK(connOf(USB_KIND_SERIAL, ROUTE_AUX, true, false) == C_USB);    // no LINK.TXT, AUX1 off: plain USB, as before
  CHECK(connOf(USB_KIND_SERIAL, ROUTE_AUX, false, false) == C_USB);
  CHECK(connOf(USB_KIND_JOY, ROUTE_BAY, true, true) == C_JOY);
  CHECK(connOf(USB_KIND_SD, ROUTE_AUX, true, true) == C_SD);

  // ENTER cycle: USB UltraEdge -> Joystick -> SD card -> BLE bay -> BLE AUX -> back, only what this radio has
  CHECK(connNext(C_USB, true, true) == C_JOY);
  CHECK(connNext(C_JOY, true, true) == C_SD);
  CHECK(connNext(C_SD, true, true) == C_BAY);
  CHECK(connNext(C_BAY, true, true) == C_AUX);
  CHECK(connNext(C_AUX, true, true) == C_USB);
  CHECK(connNext(C_BAY, true, false) == C_USB);     // AUX1 not UltraEdge: BLE AUX is skipped
  CHECK(connNext(C_SD, false, true) == C_AUX);      // release image, AUX1 = UltraEdge (TX16S, Pocket/Zorro): no BLE bay
  CHECK(connNext(C_SD, false, false) == C_USB);     // stock QX7 / AUX1 off: the three USB choices only
  auto cycle = [](bool bay, bool aux) { uint8_t c = C_USB; int seen = 0; do { c = connNext(c, bay, aux); ++seen; } while (c != C_USB && seen < 10); return seen; };
  CHECK(cycle(false, false) == 3);
  CHECK(cycle(false, true) == 4);
  CHECK(cycle(true, false) == 4);
  CHECK(cycle(true, true) == 5);
  for (int m = 0; m < 4; ++m) {                     // never lands on an option the radio doesn't have
    bool bay = m & 1, aux = m & 2;
    for (uint8_t c = 0; c < C_N; ++c) CHECK(connAvailable(connNext(c, bay, aux), bay, aux));
  }

  // Choosing an entry, then reading it back, shows the same entry
  for (uint8_t c = 0; c < C_N; ++c) {
    CHECK(connOf(connUsb(c), connRoute(c), true, true) == c);
    CHECK(connOf(connUsb(c), routeFor(c, ROUTE_AUX, true, true), true, true) == c);
  }
  CHECK(connRoute(C_JOY) == ROUTE_USB && connRoute(C_SD) == ROUTE_USB);   // USB-only choices turn wireless off

  // s71: a USB-only choice leaves the route alone where no wireless choice is offered (AUX1 turned on later still works)
  CHECK(routeFor(C_USB, ROUTE_AUX, false, false) == ROUTE_AUX);
  CHECK(routeFor(C_JOY, ROUTE_AUX, false, false) == ROUTE_AUX);
  CHECK(routeFor(C_USB, ROUTE_AUX, false, true) == ROUTE_USB);
  CHECK(routeFor(C_SD, ROUTE_BAY, true, false) == ROUTE_USB);
  CHECK(routeFor(C_BAY, ROUTE_USB, true, false) == ROUTE_BAY);
  CHECK(routeFor(C_AUX, ROUTE_USB, false, true) == ROUTE_AUX);
  CHECK(!routeSaveWanted(false, false) && routeSaveWanted(true, false) && routeSaveWanted(false, true));
  // s71: LINK.TXT "BAY" from a bay image, read by a release image: AUX (the no-file behaviour)
  CHECK(routeInBuild(ROUTE_BAY, false) == ROUTE_AUX);
  CHECK(routeInBuild(ROUTE_BAY, true) == ROUTE_BAY);
  CHECK(routeInBuild(ROUTE_USB, false) == ROUTE_USB && routeInBuild(ROUTE_AUX, false) == ROUTE_AUX);

  // s73 (radio day F-1): pick, then apply after a pause. ENTER only moves the choice; it applies PEND_TICKS after the
  // last ENTER; EXIT cancels. Ticks are g_tmr10ms.
  {
    ConnPending p;
    CHECK(!p.active() && p.shown(C_USB) == C_USB && p.due(0) == C_N && p.secondsLeft(0) == 0);
    // cycle: each ENTER moves what the line shows, nothing applies while ENTERs keep coming
    uint32_t t = 1000;
    CHECK(p.press(C_USB, true, true, t) == C_JOY && p.active() && p.shown(C_USB) == C_JOY);
    CHECK(p.due(t + 50) == C_N);
    t += 50; CHECK(p.press(C_USB, true, true, t) == C_SD && p.shown(C_USB) == C_SD);
    t += 50; CHECK(p.press(C_USB, true, true, t) == C_BAY && p.shown(C_USB) == C_BAY);
    // cycle past SD card without applying: nothing was ever due on the way
    CHECK(p.due(t + PEND_TICKS - 1) == C_N && p.active());
    // idle apply: exactly PEND_TICKS after the last ENTER, once
    CHECK(p.secondsLeft(t) == 2 && p.secondsLeft(t + 100) == 1 && p.secondsLeft(t + 199) == 1);
    CHECK(p.due(t + PEND_TICKS) == C_BAY && !p.active() && p.due(t + PEND_TICKS + 1) == C_N);
  }
  {
    ConnPending p;   // EXIT cancels: the mode in use is kept, a second EXIT has nothing to cancel (leaves the screen)
    p.press(C_USB, false, false, 0);
    p.press(C_USB, false, false, 10);
    CHECK(p.shown(C_USB) == C_SD);
    CHECK(p.cancel() && !p.active() && p.shown(C_USB) == C_USB && p.due(10 + PEND_TICKS) == C_N);
    CHECK(!p.cancel());
  }
  {
    ConnPending p;   // coming back round to the mode in use = nothing pending (stock QX7: 3 choices)
    p.press(C_USB, false, false, 0); p.press(C_USB, false, false, 1);
    CHECK(p.press(C_USB, false, false, 2) == C_USB && !p.active() && p.due(2 + PEND_TICKS) == C_N);
    // the next ENTER starts again from the mode in use
    CHECK(p.press(C_USB, false, false, 3) == C_JOY);
  }
  {
    ConnPending p;   // the whole F-1 walk on a Pocket (USB, Joystick, SD, bay, AUX): five ENTERs, nothing applied
    uint8_t cur = C_USB; uint32_t t = 0; int applied = 0;
    for (int i = 0; i < 5; ++i) { p.press(cur, true, true, t); t += 60; if (p.due(t) != C_N) ++applied; }
    CHECK(applied == 0 && !p.active());   // five presses = full circle back to USB UltraEdge, nothing pending
  }
  {
    ConnPending p;   // hold (ENTER long / colour list open) restarts the pause; pick (colour list) of the mode in use clears
    p.pick(C_SD, C_USB, 0);
    p.hold(150);
    CHECK(p.due(PEND_TICKS) == C_N && p.due(150 + PEND_TICKS) == C_SD);
    p.pick(C_JOY, C_USB, 0); p.pick(C_USB, C_USB, 5);
    CHECK(!p.active());
    p.hold(7); CHECK(!p.active());        // hold never creates a pending choice
    p.pick(C_N, C_USB, 0); CHECK(!p.active());
  }
  {
    ConnPending p;   // g_tmr10ms wraps: the pause still lasts PEND_TICKS
    uint32_t t = 0xFFFFFFFFu - 50;
    p.pick(C_JOY, C_USB, t);
    CHECK(p.due(t + 100) == C_N && p.secondsLeft(t + 100) == 1 && p.due(t + PEND_TICKS) == C_JOY);
  }
  {
    char b[40];
    CHECK(!strcmp(pendingText(b, 2, "EXIT"), "Applies in 2 s, EXIT cancels"));
    CHECK(!strcmp(pendingText(b, 1, "RTN"), "Applies in 1 s, RTN cancels"));
    CHECK(strlen(pendingText(b, 2, "EXIT")) <= 32);   // fits the 128-px B&W status line in SMLSIZE
  }

  std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "ok", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
