// SPDX-License-Identifier: GPL-2.0-only
// Host test for UE-HD (ue_hd.h, s62). Not compiled into the firmware.
//   c++ -std=c++17 -Wall -Wextra -o /tmp/ue_hd_test ue_hd_test.cpp && /tmp/ue_hd_test [<bridge copy of ue_hd.h>]
// With a path argument it also checks that the C3 bridge's copy is byte-identical to this one (one source, two copies).
// The link test runs a radio end and a C3 end exactly as s62 and the bridge drive them (one radio frame, one C3 reply
// per cycle) over a wire that drops, corrupts and truncates frames, and checks both byte streams arrive whole, in
// order, with no duplicates.
#include "ue_hd.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; }

struct End {   // one side: its endpoint, its outgoing stream, what it received, the peer's last credit
  uehd_ep_t ep;
  uehd_parser_t par;
  std::vector<uint8_t> out;   // to send
  size_t outPos = 0;
  std::vector<uint8_t> in;    // delivered
  unsigned peerCredit = 255;
  unsigned myCreditCap = 255; // what this side advertises (16-byte units)
  bool up = false;
};

// Fill the in-flight payload from the stream, capped by the peer's credit (as both firmwares do).
static void refill(End& e)
{
  if (e.ep.pendLen || e.outPos >= e.out.size()) return;
  size_t cap = e.peerCredit * 16u;
  if (cap > UEHD_MAX) cap = UEHD_MAX;
  size_t n = e.out.size() - e.outPos;
  if (n > cap) n = cap;
  memcpy(e.ep.pend, &e.out[e.outPos], n);
  e.ep.pendLen = (uint8_t)n;
  e.outPos += n;
}

// The wire: drop (lost) / flip a bit / cut short.
static std::vector<uint8_t> wire(const uint8_t* f, unsigned n, int dropPct, int flipPct, int cutPct)
{
  std::vector<uint8_t> v(f, f + n);
  if ((int)(rnd() % 100) < dropPct) return {};
  if ((int)(rnd() % 100) < flipPct) v[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
  if ((int)(rnd() % 100) < cutPct) v.resize(rnd() % n);
  return v;
}

// One cycle's burst to an end (uehd_parse_burst, as both firmwares do); true (and handled) when it held one valid frame.
static bool rxFrame(End& e, const std::vector<uint8_t>& bytes, bool* hello, uint8_t* helloByte)
{
  if (!uehd_parse_burst(&e.par, bytes.data(), (unsigned)bytes.size())) return false;
  e.peerCredit = e.par.credit;
  if (e.par.ctrl & UEHD_F_HELLO) { *hello = true; *helloByte = e.par.len ? e.par.buf[0] : 0; return true; }
  if (uehd_ep_on_frame(&e.ep, e.par.ctrl, e.par.len))
    e.in.insert(e.in.end(), e.par.buf, e.par.buf + e.par.len);
  return true;
}

static void runLink(int drop, int flip, int cut, size_t nR2C, size_t nC2R, unsigned radioCreditCap, const char* name)
{
  End r, c;
  uehd_parser_init(&r.par, UEHD_SYNC_C2R);
  uehd_parser_init(&c.par, UEHD_SYNC_R2C);
  memset(&r.ep, 0, sizeof r.ep); memset(&c.ep, 0, sizeof c.ep);
  for (size_t i = 0; i < nR2C; i++) r.out.push_back((uint8_t)rnd());
  for (size_t i = 0; i < nC2R; i++) c.out.push_back((uint8_t)rnd());
  r.myCreditCap = radioCreditCap;
  uint8_t f[UEHD_MAX + UEHD_OVERHEAD];
  unsigned cycles = 0;
  while ((r.in.size() < nC2R || c.in.size() < nR2C || r.ep.pendLen || c.ep.pendLen) && cycles < 200000) {
    cycles++;
    // radio: HELLO until the C3 has answered one, then data
    unsigned n;
    if (!r.up) n = uehd_encode(UEHD_SYNC_R2C, UEHD_F_HELLO, (uint8_t)r.myCreditCap, nullptr, 0, f);
    else { refill(r); n = uehd_encode(UEHD_SYNC_R2C, uehd_ep_ctrl(&r.ep), (uint8_t)r.myCreditCap, r.ep.pend, r.ep.pendLen, f); }
    // C3: answers every valid radio frame
    bool hello = false; uint8_t hb = 0;
    if (!rxFrame(c, wire(f, n, drop, flip, cut), &hello, &hb)) continue;   // nothing valid: the radio times out
    if (hello) {
      uehd_ep_reset(&c.ep);
      uint8_t v = UEHD_VERSION;
      n = uehd_encode(UEHD_SYNC_C2R, UEHD_F_HELLO, 255, &v, 1, f);
    } else {
      refill(c);
      n = uehd_encode(UEHD_SYNC_C2R, uehd_ep_ctrl(&c.ep), 255, c.ep.pend, c.ep.pendLen, f);
    }
    hello = false;
    bool got = rxFrame(r, wire(f, n, drop, flip, cut), &hello, &hb);
    if (got && hello && hb == UEHD_VERSION && !r.up) { uehd_ep_reset(&r.ep); r.up = true; }
  }
  bool okR = r.in == c.out, okC = c.in == r.out;
  std::printf("  %-28s cycles %6u  r->c %zu/%zu %s  c->r %zu/%zu %s  crcErr %u/%u dup %u/%u\n", name, cycles,
              c.in.size(), r.out.size(), okC ? "ok" : "BAD", r.in.size(), c.out.size(), okR ? "ok" : "BAD",
              (unsigned)r.par.crcErr, (unsigned)c.par.crcErr, (unsigned)r.ep.dup, (unsigned)c.ep.dup);
  CHECK(okR);
  CHECK(okC);
}

int main(int argc, char** argv)
{
  // CRC-8/DVB-S2 check value
  const uint8_t chk[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
  CHECK(uehd_crc8(chk, 9) == 0xBC);

  // encode/parse round trip, every length
  for (unsigned len = 0; len <= UEHD_MAX; len++) {
    uint8_t p[UEHD_MAX], f[UEHD_MAX + UEHD_OVERHEAD];
    for (unsigned i = 0; i < len; i++) p[i] = (uint8_t)(i * 7 + len);
    unsigned n = uehd_encode(UEHD_SYNC_C2R, UEHD_F_ACK, 9, p, len, f);
    CHECK(n == len + UEHD_OVERHEAD);
    uehd_parser_t ps; uehd_parser_init(&ps, UEHD_SYNC_C2R);
    int got = 0;
    for (unsigned i = 0; i < n; i++) got += uehd_parser_feed(&ps, f[i]);
    CHECK(got == 1 && ps.len == len && ps.credit == 9 && ps.ctrl == UEHD_F_ACK && !memcmp(ps.buf, p, len));
    uehd_parser_t pr; uehd_parser_init(&pr, UEHD_SYNC_R2C);   // the other direction's parser ignores it
    got = 0;
    for (unsigned i = 0; i < n; i++) got += uehd_parser_feed(&pr, f[i]);
    CHECK(got == 0);
  }
  // an over-long LEN or reserved CTRL bits are refused
  {
    uint8_t bad[] = { UEHD_SYNC_C2R, 0x08, 1, 0, 0 };
    bad[4] = uehd_crc8(bad + 1, 3);
    uehd_parser_t ps; uehd_parser_init(&ps, UEHD_SYNC_C2R);
    int got = 0; for (uint8_t b : bad) got += uehd_parser_feed(&ps, b);
    CHECK(got == 0);
    uint8_t big[] = { UEHD_SYNC_C2R, 0, 1, UEHD_MAX + 1 };
    uehd_parser_init(&ps, UEHD_SYNC_C2R);
    for (uint8_t b : big) got += uehd_parser_feed(&ps, b);
    CHECK(got == 0 && ps.st == 0);
  }
  // stop-and-wait: a repeat (lost ACK) is not delivered twice; the ACK clears the sender
  {
    uehd_ep_t a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.pendLen = 3;
    uint8_t ca = uehd_ep_ctrl(&a);
    CHECK(uehd_ep_on_frame(&b, ca, 3) == 1);
    CHECK(uehd_ep_on_frame(&b, ca, 3) == 0 && b.dup == 1);   // a's frame again: its ACK got lost
    CHECK(uehd_ep_on_frame(&a, uehd_ep_ctrl(&b), 0) == 0 && a.pendLen == 0 && a.txSeq == 1);
    a.pendLen = 2;   // the next payload is SEQ 1; b's stale ACK (for SEQ 0) must not clear it
    CHECK(uehd_ep_on_frame(&a, (uint8_t)(UEHD_F_ACK * 0), 0) == 0 && a.pendLen == 2);
  }
  // every single-bit error in a whole frame is caught (LEN included: the frame then doesn't end the burst)
  {
    uint8_t p[60], f[UEHD_MAX + UEHD_OVERHEAD];
    for (unsigned i = 0; i < sizeof p; i++) p[i] = (uint8_t)(i * 31 + 5);
    unsigned n = uehd_encode(UEHD_SYNC_R2C, UEHD_F_SEQ, 7, p, sizeof p, f);
    uehd_parser_t ps; uehd_parser_init(&ps, UEHD_SYNC_R2C);
    CHECK(uehd_parse_burst(&ps, f, n) == 1);
    int passed = 0;
    for (unsigned bit = 0; bit < n * 8; bit++) {
      f[bit / 8] ^= (uint8_t)(1u << (bit % 8));
      passed += uehd_parse_burst(&ps, f, n);
      f[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    CHECK(passed == 0);
    for (unsigned cut = 0; cut < n; cut++) CHECK(uehd_parse_burst(&ps, f, cut) == 0);
  }
  CHECK(uehd_air_ms(133, 460800) == 3 && uehd_air_ms(117, 115200) == 11);

  std::printf("link runs:\n");
  runLink(0, 0, 0, 20000, 20000, 7, "clean, radio credit 112");
  runLink(10, 0, 0, 20000, 20000, 7, "10% dropped");
  runLink(0, 10, 0, 20000, 20000, 7, "10% bit errors");
  runLink(0, 0, 10, 20000, 20000, 7, "10% cut short");
  runLink(15, 15, 15, 30000, 30000, 7, "15% of all three");
  runLink(5, 5, 5, 50000, 0, 255, "one way r->c");
  runLink(5, 5, 5, 0, 50000, 1, "one way c->r, credit 16");
  for (int seed = 1; seed <= 20; seed++) { rng = (uint32_t)seed * 7919u; runLink(20, 20, 20, 4000, 4000, 7, "fuzz 20%"); }

  if (argc > 1) {   // the bridge's copy must be this file
    auto slurp = [](const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); };
    std::string h1 = slurp((std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/') + 1) + "ue_hd.h").c_str());
    std::string h2 = slurp(argv[1]);
    CHECK(!h1.empty() && h1 == h2);
    std::printf("bridge copy %s: %s\n", argv[1], (!h1.empty() && h1 == h2) ? "identical" : "DIFFERENT");
  }
  std::printf(fails ? "%d FAILURES\n" : "ok (0 failures)\n", fails);
  return fails ? 1 : 0;
}
