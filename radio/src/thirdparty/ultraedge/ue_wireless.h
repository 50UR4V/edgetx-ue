// SPDX-License-Identifier: GPL-2.0-only
// UltraEdge s69 (ADR-0062 amendment, owner 2026-11-14): the wireless link's settings file, /UE/WIRELESS.TXT.
// "just keep the RF on the C3 module on always... of course at the lowest power.... the text file ... should default to
// 'always on at 2dB - no switch'... so the switch and the power can be changed manually by advanced users".
// The radio writes the file itself at the first start (defaults, with a note per option) and never rewrites it; a pilot
// who wants an arm switch or another power edits it. Lines are `key=value`, `#` starts a note. Unknown keys and bad values
// fall back to the default (the caller TRACEs them). A radio that had s52's /UE/AUXARM.TXT gets its switch carried over.
// The pure parts, free of EdgeTX headers, so the host test (ue_wireless_test.cpp) checks exactly what the radio runs.
#pragma once
#include <stdint.h>

namespace uewl {

static const char* const FILE_PATH = "/UE/WIRELESS.TXT";
static const char* const OLD_ARM_PATH = "/UE/AUXARM.TXT";   // s52's arm-switch file: read once, to carry the switch over
static const uint32_t RELOAD_MS = 5000;   // s52's cadence: an edit takes effect without a reboot
static const uint16_t MAX_READ = 2048;    // bytes read per reload (the radio's own file is ~800)

// The C3's Bluetooth power (wireless/esp32c3-bridge ble_link.c set_power): 3 dB steps from -24 dBm, the step at or below
// the number; +20 is the top step. 2 = the owner's default (2026-11-11 RF test level).
static const int8_t DBM_DEFAULT = 2, DBM_MIN = -24, DBM_MAX = 20;

// What a reload found wrong (TRACE only; each one falls back to the default).
enum : uint8_t { ISSUE_UNKNOWN_KEY = 1, ISSUE_BAD_POWER = 2, ISSUE_BAD_ARM = 4, ISSUE_LONG_LINE = 8, ISSUE_NO_EQUALS = 16 };

struct Settings {
  char    arm[16];   // the switch's model.yml name ("SF2", "!SA0", "L5"); "" = none (the link stays up when armed)
  uint8_t armLen;
  int8_t  dbm;
  uint8_t issues;
};

inline bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
inline bool sameWord(const char* a, uint8_t n, const char* b)   // case-insensitive, whole word
{
  uint8_t i = 0;
  for (; i < n && b[i]; ++i) if (lower(a[i]) != lower(b[i])) return false;
  return i == n && b[i] == 0;
}

// power_dbm value: an optional sign, digits, then optionally "dB" or "dBm" (any case, spaces allowed before it).
// False when it isn't a number in DBM_MIN..DBM_MAX.
inline bool parseDbm(const char* v, uint8_t n, int8_t* out)
{
  uint8_t i = 0; bool neg = false;
  if (i < n && (v[i] == '-' || v[i] == '+')) { neg = v[i] == '-'; ++i; }
  if (i >= n || v[i] < '0' || v[i] > '9') return false;
  int32_t x = 0;
  while (i < n && v[i] >= '0' && v[i] <= '9') { x = x * 10 + (v[i] - '0'); if (x > 1000) return false; ++i; }
  while (i < n && isBlank(v[i])) ++i;
  if (i < n && !(sameWord(v + i, (uint8_t)(n - i), "db") || sameWord(v + i, (uint8_t)(n - i), "dbm"))) return false;
  if (neg) x = -x;
  if (x < DBM_MIN || x > DBM_MAX) return false;
  *out = (int8_t)x;
  return true;
}

// arm_switch value: the first word. "none", "---" and "off" mean no switch (0 returned, out = ""). Returns the name's
// length, or 0xFF when the word doesn't fit [max] (the caller treats it as a bad value). Whether EdgeTX knows the name
// is the radio's check (ueParseSwitchName).
inline uint8_t armWord(const char* v, uint8_t n, char* out, uint8_t max)
{
  uint8_t l = 0;
  while (l < n && (uint8_t)v[l] > ' ') l++;
  if (l == 0 || sameWord(v, l, "none") || sameWord(v, l, "---") || sameWord(v, l, "off")) { out[0] = 0; return 0; }
  if (l + 1 > max) { out[0] = 0; return 0xFF; }
  for (uint8_t i = 0; i < l; ++i) out[i] = v[i];
  out[l] = 0;
  return l;
}

// Reads the file in any chunk size: feed() the bytes, then end(). Lines longer than the buffer are notes if they start
// with '#', otherwise ignored (ISSUE_LONG_LINE). CR LF, spaces around the key, the '=' and the value, a UTF-8 BOM and
// notes after the value ("power_dbm = 2   # low") are all fine. A key given twice: the last one counts.
class Reader {
 public:
  Settings s;
  Reader() : n_(0), over_(false), first_(true) { s.arm[0] = 0; s.armLen = 0; s.dbm = DBM_DEFAULT; s.issues = 0; }
  void feed(const char* d, uint32_t len)
  {
    for (uint32_t i = 0; i < len; ++i) {
      char c = d[i];
      if (c == '\n') { line(); continue; }
      if (n_ < sizeof(buf_)) buf_[n_++] = c; else over_ = true;
    }
  }
  void end() { if (n_ || over_) line(); }

 private:
  char buf_[48];
  uint8_t n_;
  bool over_, first_;
  void line()
  {
    uint8_t a = 0, e = n_;
    if (first_ && e >= 3 && (uint8_t)buf_[0] == 0xEF && (uint8_t)buf_[1] == 0xBB && (uint8_t)buf_[2] == 0xBF) a = 3;
    first_ = false;
    bool over = over_;
    n_ = 0; over_ = false;
    while (a < e && isBlank(buf_[a])) a++;
    if (a == e || buf_[a] == '#') return;               // blank line or a note
    if (over) { s.issues |= ISSUE_LONG_LINE; return; }
    uint8_t eq = a;
    while (eq < e && buf_[eq] != '=') eq++;
    if (eq == e) { s.issues |= ISSUE_NO_EQUALS; return; }
    uint8_t ke = eq;
    while (ke > a && isBlank(buf_[ke - 1])) ke--;
    uint8_t v = (uint8_t)(eq + 1), ve = v;
    while (ve < e && buf_[ve] != '#') ve++;             // a note after the value
    while (v < ve && isBlank(buf_[v])) v++;
    while (ve > v && isBlank(buf_[ve - 1])) ve--;
    const char* key = buf_ + a; uint8_t kl = (uint8_t)(ke - a);
    if (sameWord(key, kl, "arm_switch")) {
      uint8_t l = armWord(buf_ + v, (uint8_t)(ve - v), s.arm, sizeof(s.arm));
      if (l == 0xFF) { s.issues |= ISSUE_BAD_ARM; s.armLen = 0; } else s.armLen = l;
    } else if (sameWord(key, kl, "power_dbm")) {
      if (!parseDbm(buf_ + v, (uint8_t)(ve - v), &s.dbm)) { s.issues |= ISSUE_BAD_POWER; s.dbm = DBM_DEFAULT; }
    } else {
      s.issues |= ISSUE_UNKNOWN_KEY;
    }
  }
};

// The file the radio writes when there is none: HEAD, the arm_switch line, TAIL. ASCII only, so any editor opens it.
static const char FILE_HEAD[] =
  "# UltraEdge wireless settings. The radio wrote this file and reads it again every 5 seconds;\n"
  "# it never changes it. Lines starting with # are notes. Delete the file to get the defaults back.\n"
  "#\n"
  "# arm_switch: while this switch is on, the wireless link is off.\n"
  "#   none = the link is always on (the default: RF tests showed no effect on the RC link).\n"
  "#   Names as in model.yml: SF2 = SF down, SA0 = SA up, SA1 = SA middle, !SB2 = SB not down,\n"
  "#   L5 = logical switch 5.\n";
static const char FILE_TAIL[] =
  "#\n"
  "# power_dbm: the wireless module's Bluetooth power, -24 to 20 (dBm). Default 2, the RF test level.\n"
  "#   The module uses its 3 dB step at or below the number (2 -> 0 dBm, 20 -> 18 dBm).\n"
  "#   The radio sends it to the module; module firmware from 2026-11-14 on applies it.\n"
  "power_dbm=2\n";

// The arm_switch line for [name] ("" = none). Returns its length, or 0 when it doesn't fit [max].
inline uint8_t armLine(const char* name, char* out, uint8_t max)
{
  static const char K[] = "arm_switch=";
  uint8_t n = 0;
  const char* v = (name && name[0]) ? name : "none";
  for (uint8_t i = 0; K[i]; ++i) { if (n + 1 >= max) return 0; out[n++] = K[i]; }
  for (uint8_t i = 0; v[i]; ++i) { if (n + 1 >= max) return 0; out[n++] = v[i]; }
  if (n + 2 > max) return 0;
  out[n++] = '\n';
  out[n] = 0;
  return n;
}

}  // namespace uewl
