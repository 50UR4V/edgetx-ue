// Host cross-check for the negotiated sub-mode HELLO sniff (docs/29 / docs/30 step 3).
// NOT built into firmware — compiled on the host only. Mirrors the exact detection the
// wrapper RX path relies on: feed a byte stream through the SAME Decoder the firmware uses;
// confirm a CRC-valid HELLO fires onFrame with MSG_HELLO, and that arbitrary leading
// (non-HELLO) traffic does NOT false-trigger it (so plain Serial is safe until a real HELLO).
#include "protocol.h"
#include <cstdio>
#include <cstring>
#include <cassert>

using namespace ultraedge;

static bool g_sawHello = false;
static int  g_frames = 0;
static void onFrame(const Message& m, void*) {
  g_frames++;
  if (m.type == MSG_HELLO) g_sawHello = true;
}

int main() {
  // 1) Build a valid host HELLO frame (what the app sends on connect).
  uint8_t hello[MAX_FRAME_WIRE];
  Hello h; h.proto_ver = PROTO_VERSION; h.fw_ver = 0x00040000; h.role = ROLE_PHONE; h.caps = CAP_STRUCTURED;
  size_t hn = encodeHello(0, h, /*ack=*/false, hello, sizeof(hello));
  assert(hn > 0);

  Decoder dec; dec.reset(); dec.setCallback(onFrame, nullptr);

  // 2) Feed noise that looks like CLI/telemetry chatter first — must NOT trigger HELLO.
  const char* noise = "help\r\nset foo 1\r\n\xAA\x55 garbage \x7E\x7E partial";
  dec.feed((const uint8_t*)noise, strlen(noise));
  if (g_sawHello) { printf("FAIL: HELLO false-triggered on noise\n"); return 1; }
  printf("ok: %d bytes of noise, no false HELLO (frames seen so far: %d)\n", (int)strlen(noise), g_frames);

  // 3) Now feed the real HELLO — must trigger.
  dec.feed(hello, hn);
  if (!g_sawHello) { printf("FAIL: valid HELLO not detected\n"); return 1; }
  printf("ok: valid HELLO detected after noise -> companion would take over here\n");

  // 4) Feed a HELLO split across two chunks (USB packets fragment) — must still trigger.
  // Use a FRESH decoder: this models a new connection, and avoids leftover partial-frame
  // state from the noise/HELLO already fed to `dec` above (the mid-stream reset() case is
  // exercised in firmware by the transport-down path, not here).
  Decoder dec2; dec2.reset(); dec2.setCallback(onFrame, nullptr);
  g_sawHello = false;
  dec2.feed(hello, hn/2);
  if (g_sawHello) { printf("FAIL: HELLO triggered on half a frame\n"); return 1; }
  dec2.feed(hello + hn/2, hn - hn/2);
  if (!g_sawHello) { printf("FAIL: split HELLO not detected\n"); return 1; }
  printf("ok: fragmented HELLO (%zu+%zu) detected\n", hn/2, hn-hn/2);

  printf("ALL NEGOTIATION CROSS-CHECKS PASSED\n");
  return 0;
}
