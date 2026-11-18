// SPDX-License-Identifier: GPL-2.0-only
// Host test for s69's /UE/WIRELESS.TXT (ue_wireless.h). Not compiled into the firmware.
//   c++ -std=c++17 -Wall -Wextra -o /tmp/ue_wireless_test ue_wireless_test.cpp && /tmp/ue_wireless_test
#include "ue_wireless.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
using namespace uewl;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// Reads [txt] the way the radio does, in chunks of [chunk] bytes (the radio reads 64 at a time).
static Settings read(const std::string& txt, size_t chunk = 64)
{
  Reader r;
  for (size_t i = 0; i < txt.size(); i += chunk)
    r.feed(txt.data() + i, (uint32_t)std::min(chunk, txt.size() - i));
  r.end();
  return r.s;
}
static std::string arm(const Settings& s) { return std::string(s.arm, s.armLen); }

// The file the radio writes when there is none (ueWirelessFileEnsure): HEAD + arm line + TAIL.
static std::string defaultFile(const char* armName)
{
  char line[32];
  uint8_t n = armLine(armName, line, sizeof(line));
  return std::string(FILE_HEAD) + std::string(line, n) + FILE_TAIL;
}

int main()
{
  // ---- the default file: always on at 2 dBm, no switch; reads back to exactly that, with no issue ----
  {
    std::string f = defaultFile("");
    CHECK(f.find("arm_switch=none\n") != std::string::npos);
    CHECK(f.find("power_dbm=2\n") != std::string::npos);
    for (size_t chunk : {1, 7, 64, 4096}) {
      Settings s = read(f, chunk);
      CHECK(s.armLen == 0 && s.arm[0] == 0);
      CHECK(s.dbm == 2);
      CHECK(s.issues == 0);
    }
    for (char c : f) CHECK((unsigned char)c < 0x80);   // ASCII only
    CHECK(f.size() < MAX_READ);
    // every non-blank line is a note or a key=value line
    size_t keys = 0, p = 0;
    while (p < f.size()) { size_t e = f.find('\n', p); std::string l = f.substr(p, e - p); p = e + 1;
      if (!l.empty() && l[0] != '#') { CHECK(l.find('=') != std::string::npos); ++keys; } }
    CHECK(keys == 2);
  }
  // ---- migration: the old /UE/AUXARM.TXT switch carried into the new file ----
  for (const char* n : { "SF2", "!SA0", "L12", "6P05", "TrimRudLeft" }) {
    Settings s = read(defaultFile(n));
    CHECK(arm(s) == n);
    CHECK(s.dbm == 2 && s.issues == 0);
  }
  { char out[12]; CHECK(armLine("TrimRudLeftXX", out, sizeof(out)) == 0); }   // doesn't fit: nothing
  CHECK(std::strcmp(FILE_PATH, "/UE/WIRELESS.TXT") == 0);
  CHECK(std::strcmp(OLD_ARM_PATH, "/UE/AUXARM.TXT") == 0);

  // ---- what an advanced user may type ----
  CHECK(arm(read("arm_switch=SF2\n")) == "SF2");
  CHECK(arm(read("arm_switch=SF2")) == "SF2");                      // no newline at the end
  CHECK(arm(read("arm_switch=SF2\r\npower_dbm=5\r\n")) == "SF2");   // Windows line ends
  CHECK(read("arm_switch=SF2\r\npower_dbm=5\r\n").dbm == 5);
  CHECK(arm(read("  ARM_SWITCH  =  !SA0   # off when SA is not up\n")) == "!SA0");
  CHECK(arm(read("arm_switch=L5 armed\n")) == "L5");                // a word after the name is ignored
  CHECK(read("arm_switch=NONE\n").armLen == 0);
  CHECK(read("arm_switch=---\n").armLen == 0);
  CHECK(read("arm_switch=off\n").armLen == 0);
  CHECK(read("arm_switch=\n").armLen == 0 && read("arm_switch=\n").issues == 0);
  CHECK(arm(read("\xEF\xBB\xBF" "arm_switch=SF2\n")) == "SF2");     // a UTF-8 BOM (Notepad)
  CHECK(arm(read("arm_switch=SA2\narm_switch=SF2\n")) == "SF2");    // given twice: the last counts
  CHECK(read("power_dbm=-24\n").dbm == -24);
  CHECK(read("power_dbm=20\n").dbm == 20);
  CHECK(read("power_dbm=+8\n").dbm == 8);
  CHECK(read("power_dbm = 2dB\n").dbm == 2);                        // the owner's spelling
  CHECK(read("power_dbm=11 dBm\n").dbm == 11);
  CHECK(read("power_dbm=0\n").dbm == 0);
  CHECK(read("\n\n   \n# just notes\n").issues == 0);

  // ---- bad values: the default, and an issue to TRACE ----
  {
    Settings s = read("power_dbm=21\n");   CHECK(s.dbm == DBM_DEFAULT && (s.issues & ISSUE_BAD_POWER));
    s = read("power_dbm=-25\n");           CHECK(s.dbm == DBM_DEFAULT && (s.issues & ISSUE_BAD_POWER));
    s = read("power_dbm=high\n");          CHECK(s.dbm == DBM_DEFAULT && (s.issues & ISSUE_BAD_POWER));
    s = read("power_dbm=\n");              CHECK(s.dbm == DBM_DEFAULT && (s.issues & ISSUE_BAD_POWER));
    s = read("power_dbm=5mW\n");           CHECK(s.dbm == DBM_DEFAULT && (s.issues & ISSUE_BAD_POWER));
    s = read("power_dbm=99999999999\n");   CHECK(s.dbm == DBM_DEFAULT && (s.issues & ISSUE_BAD_POWER));
    s = read("power_dbm=8\npower_dbm=x\n"); CHECK(s.dbm == DBM_DEFAULT);   // the last one counts, and it is bad
    s = read("arm_switch=AVeryLongSwitchName\n"); CHECK(s.armLen == 0 && (s.issues & ISSUE_BAD_ARM));
    s = read("colour=blue\npower_dbm=5\n"); CHECK(s.dbm == 5 && (s.issues & ISSUE_UNKNOWN_KEY));
    s = read("arm_switch SF2\n");          CHECK(s.armLen == 0 && (s.issues & ISSUE_NO_EQUALS));
    s = read("arm_switch=SF2" + std::string(60, ' ') + "x\npower_dbm=5\n");
    CHECK(s.armLen == 0 && (s.issues & ISSUE_LONG_LINE) && s.dbm == 5);   // a too-long key line: ignored, the rest read
    s = read("#" + std::string(200, 'n') + "\narm_switch=SF2\n");
    CHECK(arm(s) == "SF2" && s.issues == 0);                                // a long note is fine
  }
  // ---- empty / absent file: the defaults ----
  { Settings s = read(""); CHECK(s.armLen == 0 && s.dbm == 2 && s.issues == 0); }

  if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
  std::printf("ue_wireless_test: all passed\n");
  return 0;
}
