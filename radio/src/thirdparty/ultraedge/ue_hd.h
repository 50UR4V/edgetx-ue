// SPDX-License-Identifier: GPL-2.0-only OR LicenseRef-UltraEdge
// UE-HD (ADR-0064 §2.5, owner approved 2026-11-12): the half-duplex link layer between the radio and a bay C3 on the
// one-wire S.Port pin (route B: QX7 and every bay without a full-duplex UART). It carries the UE v4 byte stream
// unchanged: UE-HD never parses v4, the app never sees UE-HD.
//
// ONE SOURCE, TWO COPIES: this exact file is compiled into the radio (radio/src/thirdparty/ultraedge/ue_hd.h, patch
// s62) and into the C3 bridge (wireless/esp32c3-bridge/main/ue_hd.h). Plain C99, header-only, no EdgeTX/IDF headers.
// The host test (ue_hd_test.cpp) runs against both copies and fails if they differ.
//
// Frame (both directions):  SYNC CTRL CREDIT LEN payload[LEN] CRC8
//   SYNC   0xC5 radio->C3, 0x5C C3->radio (an echo of your own frame is never taken for the other side's)
//   CTRL   bit0 SEQ (this payload's sequence), bit1 ACK (last SEQ received intact), bit2 HELLO, bits 3-7 = 0
//   CREDIT free room in the sender's receive buffer, 16-byte units: the other side never sends a longer payload
//   LEN    0..UEHD_MAX
//   CRC8   CRC-8/DVB-S2 (poly 0xD5, the CRSF one) over CTRL..end of payload
// Bus rule: the radio is the only master; the C3 answers each valid radio frame with exactly one frame.
// Reliability: stop-and-wait per direction, 1-bit SEQ. An empty payload carries no sequence. A HELLO frame resets both
// sequences; the C3 answers HELLO with HELLO + one payload byte, UEHD_VERSION (not stream data).
#ifndef UE_HD_H
#define UE_HD_H
#include <stdint.h>
#include <string.h>

#define UEHD_SYNC_R2C   0xC5
#define UEHD_SYNC_C2R   0x5C
#define UEHD_MAX        128
#define UEHD_OVERHEAD   5
#define UEHD_VERSION    1
#define UEHD_F_SEQ      0x01
#define UEHD_F_ACK      0x02
#define UEHD_F_HELLO    0x04

static inline uint8_t uehd_crc8(const uint8_t* d, unsigned n)
{
  uint8_t c = 0;
  for (unsigned i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (uint8_t)(c << 1) ^ 0xD5 : (uint8_t)(c << 1));
  }
  return c;
}

// Writes one frame to out (room for len + UEHD_OVERHEAD), returns its size. len is clipped to UEHD_MAX.
static inline unsigned uehd_encode(uint8_t sync, uint8_t ctrl, uint8_t credit, const uint8_t* p, unsigned len,
                                   uint8_t* out)
{
  if (len > UEHD_MAX) len = UEHD_MAX;
  out[0] = sync; out[1] = ctrl; out[2] = credit; out[3] = (uint8_t)len;
  if (len) memcpy(out + 4, p, len);
  out[4 + len] = uehd_crc8(out + 1, 3 + len);
  return 4 + len + 1;
}

// Byte-at-a-time parser for the frames of ONE direction (sync = the other side's SYNC).
typedef struct {
  uint8_t sync, st, ctrl, credit, len, n;
  uint8_t buf[UEHD_MAX];
  uint32_t ok, crcErr;
} uehd_parser_t;

static inline void uehd_parser_init(uehd_parser_t* p, uint8_t sync) { memset(p, 0, sizeof(*p)); p->sync = sync; }

// Returns 1 when a whole CRC-valid frame is in p (ctrl/credit/len/buf), 0 otherwise. A bad CRC or an impossible
// header drops the frame and hunts for the next SYNC.
static inline int uehd_parser_feed(uehd_parser_t* p, uint8_t b)
{
  switch (p->st) {
    case 0: if (b == p->sync) p->st = 1; return 0;
    case 1: if (b & 0xF8) { p->st = (b == p->sync); return 0; } p->ctrl = b; p->st = 2; return 0;
    case 2: p->credit = b; p->st = 3; return 0;
    case 3: if (b > UEHD_MAX) { p->st = (b == p->sync); return 0; } p->len = b; p->n = 0; p->st = b ? 4 : 5; return 0;
    case 4: p->buf[p->n++] = b; if (p->n == p->len) p->st = 5; return 0;
    default: {
      uint8_t h[3] = { p->ctrl, p->credit, p->len };
      uint8_t c = uehd_crc8(h, 3);
      for (unsigned i = 0; i < p->len; i++) {   // continue the CRC over the payload
        c ^= p->buf[i];
        for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (uint8_t)(c << 1) ^ 0xD5 : (uint8_t)(c << 1));
      }
      p->st = 0;
      if (c == b) { p->ok++; return 1; }
      p->crcErr++;
      return 0;
    }
  }
}

// One cycle's bytes (everything received since this side last sent, or since the line went idle): valid only when
// they hold exactly one frame that ends on the last byte. Exactly one frame per cycle is the bus rule, so trailing
// bytes mean the frame's LEN was hit (it then ends early and its "CRC" is a payload byte: 1 in 256 would pass) and a
// frame that runs past the burst was cut short. With this rule every single-bit error is caught by CRC-8.
static inline int uehd_parse_burst(uehd_parser_t* p, const uint8_t* d, unsigned n)
{
  p->st = 0;
  for (unsigned i = 0; i < n; i++) {
    if (uehd_parser_feed(p, d[i]) == 1) {
      if (i + 1 == n) return 1;
      p->ok--; p->crcErr++;   // counted as an error: bytes after the frame
      p->st = 0;
      return 0;
    }
  }
  p->st = 0;
  return 0;
}

// One end of the stop-and-wait link: the payload in flight (kept until the other side ACKs its SEQ) and the next SEQ
// expected from the other side.
typedef struct {
  uint8_t txSeq, rxExpect;
  uint8_t pend[UEHD_MAX];
  uint8_t pendLen;
  uint32_t delivered, dup, resent;
} uehd_ep_t;

// HELLO: both sequences restart at 0. A payload still in flight is kept and goes again as SEQ 0.
static inline void uehd_ep_reset(uehd_ep_t* e) { e->txSeq = 0; e->rxExpect = 0; }

// CTRL for the next frame this end sends (HELLO added by the caller).
static inline uint8_t uehd_ep_ctrl(const uehd_ep_t* e)
{
  return (uint8_t)((e->txSeq & 1) | ((e->rxExpect ^ 1) & 1) << 1);
}

// A valid frame from the other side (not a HELLO). Clears our in-flight payload when it is ACKed. Returns 1 when the
// frame's payload is new and must be delivered upward, 0 when it is empty or a repeat (its ACK was lost).
static inline int uehd_ep_on_frame(uehd_ep_t* e, uint8_t ctrl, uint8_t len)
{
  if (e->pendLen && ((ctrl >> 1) & 1) == e->txSeq) { e->pendLen = 0; e->txSeq ^= 1; }
  if (!len) return 0;
  if ((ctrl & 1) == e->rxExpect) { e->rxExpect ^= 1; e->delivered++; return 1; }
  e->dup++;
  return 0;
}

// Wire time of n bytes at baud (8N1), rounded up to whole ms.
static inline uint32_t uehd_air_ms(unsigned n, uint32_t baud) { return (uint32_t)((n * 10u * 1000u + baud - 1) / baud); }

#endif
