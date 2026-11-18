// SPDX-License-Identifier: GPL-2.0-only
// Host test for s68's trims decisions (ue_trims.h). Not compiled into the firmware.
//   c++ -std=c++17 -Wall -Wextra -o /tmp/ue_trims_test ue_trims_test.cpp && /tmp/ue_trims_test
#include "ue_trims.h"
#include <cstdio>
#include <cstring>
using namespace uetrims;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// Stand-ins with EdgeTX's layout (datastructs_private.h: trim_t {int16 value:11; uint16 mode:5}; FlightModeData).
struct trim_t { int16_t value:11; uint16_t mode:5; };
struct FlightModeData { trim_t trim[6]; char name[10]; int16_t swtch; uint8_t fadeIn, fadeOut; int16_t gvars[9]; };
enum { NFM = 9, NT = 6, NGV = 9, GVAR_MAX = 1024 };

int main()
{
  // ---- MSG_TRIM_OP gate: the NACK matrix ----
  CHECK(trimOpCheck(false, 2, OP_RESET, 0, false, 32) == R_NOT_ATTACHED);   // gated first, like every actuator message
  CHECK(trimOpCheck(true, 1, OP_RESET, 0, false, 32) == R_BAD_LENGTH);
  CHECK(trimOpCheck(true, 0, OP_RESET, 0, false, 32) == R_BAD_LENGTH);
  CHECK(trimOpCheck(true, 2, OP_RESET, 0, true, 32) == R_PUSH_STATE);       // an active-model save is streaming in
  CHECK(trimOpCheck(true, 2, OP_COUNT, 0, false, 32) == R_OUT_OF_RANGE);    // unknown op
  CHECK(trimOpCheck(true, 2, 0xFF, 0, false, 32) == R_OUT_OF_RANGE);
  CHECK(trimOpCheck(true, 2, OP_RESET, 0xFF, false, 32) == R_OK);           // arg ignored by the whole-model ops
  CHECK(trimOpCheck(true, 2, OP_TRIMS_TO_SUBTRIMS, 0xFF, false, 32) == R_OK);
  CHECK(trimOpCheck(true, 2, OP_TRIM_TO_SUBTRIM, 31, false, 32) == R_OK);   // channels 0..31
  CHECK(trimOpCheck(true, 2, OP_TRIM_TO_SUBTRIM, 32, false, 32) == R_OUT_OF_RANGE);
  CHECK(trimOpCheck(true, 2, OP_STICK_TO_SUBTRIM, 0, false, 32) == R_OK);
  CHECK(trimOpCheck(true, 2, OP_STICK_TO_SUBTRIM, 32, false, 32) == R_OUT_OF_RANGE);
  CHECK(trimOpCheck(true, 3, OP_STICK_TO_SUBTRIM, 5, false, 32) == R_OK);   // a longer payload is fine (future args)

  // ---- live values kept across the reload after an app save ----
  FlightModeData live[NFM]; std::memset(live, 0, sizeof(live));
  // the pilot trimmed on the radio after the app's pull: FM0 aileron +37, FM1 own elevator -12 (extended range),
  // GV1 in FM0 set by a trim used as a GVar, GV3 in FM2 = "use FM0" (GVAR_MAX + 1 + 0)
  live[0].trim[0].value = 37;  live[0].trim[0].mode = 0;
  live[1].trim[1].value = -12; live[1].trim[1].mode = 2;     // FM1 own
  live[1].trim[5].value = 511;                                // T6 on a 6-trim radio, extended max
  live[0].gvars[1] = 250;
  live[2].gvars[3] = GVAR_MAX + 1;
  LiveValues<NFM, NT, NGV> keep;
  keep.take(live);

  // the app's file (its pull, before those changes) + its edits: FM1 elevator mode -> "FM0 + own" (3), FM0 rudder mode
  // -> 3-pos, a renamed FM; readModel wipes the model first and writes what the file has (EdgeTX omits zeros)
  FlightModeData loaded[NFM]; std::memset(loaded, 0, sizeof(loaded));
  loaded[1].trim[1].mode = 3;
  loaded[0].trim[3].mode = 18;          // TRIM_MODE_3POS on 9 FMs
  std::strcpy(loaded[1].name, "Land");
  loaded[0].gvars[1] = 100;             // the value at the app's pull
  CHECK(keep.putBack(loaded));          // differs from the file: the radio must save the model again
  CHECK(loaded[0].trim[0].value == 37);
  CHECK(loaded[1].trim[1].value == -12 && loaded[1].trim[1].mode == 3);   // value kept, the app's mode change lands
  CHECK(loaded[0].trim[3].mode == 18 && loaded[0].trim[3].value == 0);
  CHECK(loaded[1].trim[5].value == 511);
  CHECK(loaded[0].gvars[1] == 250);
  CHECK(loaded[2].gvars[3] == GVAR_MAX + 1);                               // GVar "use FM n" encoding kept whole
  CHECK(std::strcmp(loaded[1].name, "Land") == 0);                         // everything else from the file
  // a second reload of the same values changes nothing (no extra save)
  CHECK(!keep.putBack(loaded));

  // a file whose values already match the radio: nothing to put back, no save
  FlightModeData same[NFM]; std::memcpy(same, live, sizeof(live));
  CHECK(!keep.putBack(same));

  // a trim moved while the file streamed in is kept: the values are taken at END, after the last chunk
  live[0].trim[0].value = 41;
  keep.take(live);
  FlightModeData again[NFM]; std::memset(again, 0, sizeof(again));
  CHECK(keep.putBack(again) && again[0].trim[0].value == 41);

  // ---- live flight state: which items reset after an app save (EdgeTX's menu semantics) ----
  {
    // sensors: what the value means, not its name or ratio
    SensorKey rssi = sensorKey(false, 0xF101, 0x00, 0, 0, 0);
    CHECK(rssi == sensorKey(false, 0xF101, 0x00, 0, 0, 0));
    CHECK(rssi != sensorKey(false, 0xF101, 0x01, 0, 0, 0));          // another instance
    CHECK(rssi != sensorKey(false, 0xF101, 0x00, 0, 1, 0));          // unit
    CHECK(rssi != sensorKey(false, 0xF101, 0x00, 0, 0, 1));          // precision
    CHECK(rssi != sensorKey(true, 0xF101, 0x00, 0, 0, 0));           // custom -> calculated
    // a calculated sensor's id field holds its persistent value: that changing is no redefinition
    CHECK(sensorKey(true, 120, 3, 0, 5, 0) == sensorKey(true, 999, 3, 0, 5, 0));
    CHECK(sensorKey(true, 120, 3, 0, 5, 0) != sensorKey(true, 120, 4, 0, 5, 0));   // formula
    SensorKey none = sensorKey(false, 0, 0, 0, 0, 0);
    CHECK(rssi != none);                                                // removed -> cleared

    // logical switches: function changed or removed -> reset that one only
    uint8_t lsBefore[8] = { 0, 1, 18, 18, 5, 0, 0, 0 };              // 18 = sticky in this sketch
    uint8_t lsAfter[8]  = { 0, 1, 18, 0,  6, 0, 9, 0 };              // L4 removed, L5 a new function, L7 added
    int resets = 0; bool hit[8] = {};
    forChanged(lsBefore, lsAfter, 8, [&](uint16_t i) { hit[i] = true; ++resets; });
    CHECK(resets == 3 && hit[3] && hit[4] && hit[6] && !hit[2]);       // the unchanged sticky L3 keeps its state

    // timers: only a changed start value resets (the Timer menu's "Start" calls timerSet)
    uint32_t tb[3] = { 300, 0, 60 }, ta[3] = { 300, 0, 90 };
    resets = 0; forChanged(tb, ta, 3, [&](uint16_t i) { CHECK(i == 2); ++resets; });
    CHECK(resets == 1);

    // nothing changed -> nothing resets
    resets = 0; forChanged(lsBefore, lsBefore, 8, [&](uint16_t) { ++resets; });
    CHECK(resets == 0);

    // scripts reload only when the script set changes
    const char a[] = "mix1", b[] = "mix2";
    CHECK(crcAdd(0xFFFF, "123456789", 9) == 0x29B1);                  // CRC16-CCITT check value
    CHECK(crcAdd(0xFFFF, a, 4) == crcAdd(0xFFFF, a, 4));
    CHECK(crcAdd(0xFFFF, a, 4) != crcAdd(0xFFFF, b, 4));
  }

  // ---- option b: the reload hold, its cap and the measurement line ----
  {
    HoldStats h;
    CHECK(!reloadStopsPulses(h, 12000));                // a normal model: hold the mixer
    CHECK(reloadStopsPulses(h, HOLD_MAX_FILE + 1));     // an oversized file: EdgeTX's pulse stop
    holdRecord(h, 35, false);
    CHECK(h.last == 35 && h.max == 35 && h.count == 1 && !h.useStop && !h.lastStopped);
    holdRecord(h, 20, false);
    CHECK(h.last == 20 && h.max == 35 && h.count == 2 && !h.useStop);
    holdRecord(h, HOLD_CAP_MS, false);                  // at the cap: still fine
    CHECK(!h.useStop && !reloadStopsPulses(h, 12000));
    holdRecord(h, HOLD_CAP_MS + 1, false);              // over it once: every later save stops the pulses
    CHECK(h.useStop && reloadStopsPulses(h, 12000) && h.max == HOLD_CAP_MS + 1);
    holdRecord(h, 260, true);                           // a stopped reload never re-arms or clears the switch
    CHECK(h.useStop && h.lastStopped && h.max == 260 && h.count == 5);
    holdRecord(h, 100000, true);
    CHECK(h.last == 65535);
    char line[72];
    HoldStats g; holdRecord(g, 12, false); holdRecord(g, 40, false); holdRecord(g, 9, false);
    holdLine(line, sizeof(line), g);
    CHECK(std::strcmp(line, "last=9ms max=40ms saves=3 mode=hold cap=200ms\n") == 0);
    holdLine(line, sizeof(line), h);
    CHECK(std::strcmp(line, "last=65535ms max=65535ms saves=6 mode=stop cap=200ms next=stop\n") == 0);
    char tiny[8]; holdLine(tiny, sizeof(tiny), g);
    CHECK(std::strlen(tiny) == 7);                      // never writes past the buffer
  }

  if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
  std::printf("ue_trims_test: all passed\n");
  return 0;
}
