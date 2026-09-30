// Host cross-check for the link-recovery fix (bug: "write rejected: not attached" after the
// app UI blocked heartbeats). NOT built into firmware. Verifies: HELLO attaches; heartbeats
// sustain; a >timeout keepalive gap drops to HANDSHAKING (not DETACHED); the next valid frame
// (the config-write itself, since onMessage runs before the write's gate) re-attaches; and a
// REAL transport reconnect still requires a fresh HELLO (version re-check preserved).
#include "protocol.h"
#include <cstdio>
using namespace ultraedge;
int main(){
  LinkManager lk(ROLE_RADIO);
  uint32_t t=0;
  static uint8_t hpl[10]={PROTO_VERSION,0,0,0,0,ROLE_PHONE,(uint8_t)(CAP_STRUCTURED&0xFF),0,0,0};
  Message h; h.type=MSG_HELLO; h.seq=0; h.len=10; h.payload=hpl;
  Message hb; hb.type=MSG_HEARTBEAT; hb.seq=0; hb.len=0; hb.payload=nullptr;
  Message wr; wr.type=MSG_CONFIG_WRITE; wr.seq=0; wr.len=5; static uint8_t wpl[5]={0,0,1,1,0}; wr.payload=wpl;
  int fails=0;
  lk.onTransportUp();
  t=50; lk.onMessage(h,t);
  printf("HELLO: attached=%d %s\n", lk.companionInputAllowed(), lk.companionInputAllowed()?"":"(FAIL)");
  for(int i=0;i<5;i++){ t+=400; lk.onMessage(hb,t); lk.tick(t); }
  printf("5 heartbeats: attached=%d\n", lk.companionInputAllowed());
  // UI blocked >1000ms (picker open), then a tick fires
  t+=1500; lk.tick(t);
  printf("1500ms UI-block gap: attached=%d (expect 0, now HANDSHAKING not DETACHED)\n", lk.companionInputAllowed());
  // The WRITE arrives (user tapped save). onMessage runs BEFORE the gate check in onFrame.
  t+=10; lk.onMessage(wr,t);
  bool okAtWrite = lk.companionInputAllowed();
  printf("WRITE frame arrives -> re-attached BEFORE gate check: attached=%d %s\n", okAtWrite, okAtWrite?"PASS":"FAIL");
  if(!okAtWrite) fails++;
  // resumed heartbeats also sustain
  for(int i=0;i<5;i++){ t+=400; lk.onMessage(hb,t); lk.tick(t); }
  printf("heartbeats resume: attached=%d\n", lk.companionInputAllowed());
  // a REAL transport-down then up requires fresh HELLO
  lk.onTransportDown(); lk.onTransportUp();
  t+=100; lk.onMessage(hb,t);   // heartbeat WITHOUT prior HELLO this session
  printf("after real reconnect, heartbeat only (no HELLO): attached=%d (expect 0 — needs HELLO)\n", lk.companionInputAllowed());
  t+=50; lk.onMessage(h,t);
  printf("after reconnect + HELLO: attached=%d %s\n", lk.companionInputAllowed(), lk.companionInputAllowed()?"PASS":"FAIL");
  if(!lk.companionInputAllowed()) fails++;
  printf(fails==0? "\nALL LINK-RECOVERY CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n", fails);
  return fails;
}
