/*
 * UltraEdge companion module (HW-3 emit + HW-4 bidirectional).
 *
 * When USB is in Serial (CDC) mode:
 *   - emits CHANNELS at ~10 Hz and an advertising HELLO (radio -> host);
 *   - receives host frames, runs the companion protocol Decoder + LinkManager;
 *   - replies HELLO_ACK to a host HELLO (link -> ATTACHED);
 *   - accepts a KEY only when ATTACHED (link-state gating), replying ACK; a KEY
 *     received while DETACHED is ignored — the on-hardware version of the
 *     "inputs inert until a companion is attached" safety rule.
 *
 * The receive callback runs in USB-interrupt context, so it only pushes bytes
 * into a lock-free single-producer/single-consumer ring; all protocol work runs
 * in ultraedgeCompanionTick() from per10ms() (main context). The control path is
 * never touched.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(USB_COMPANION)

#include "edgetx.h"
#include "hal/usb_driver.h"
#include "timers.h"                 // timersStates[] for the Home screen (docs/13)
#include "mixes.h"                  // insertMix/deleteMix for list edits (docs/13)
#include "curves.h"                 // curveAddress/getCurvePoints for the graph editor (docs/13)
#include "input_mapping.h"          // inputMappingConvertMode/GetThrottle — Home trim/stick slots (docs/13)
#include "strhelpers.h"             // getMainControlLabel — trainer stick names (Set 5)
#include "trainer.h"                // trainerInput[] — trainer calibration snapshot (Set 5)
#include "hal/adc_driver.h"         // ADC_MAIN_LH/LV/RV/RH physical stick slot indices (docs/13)
#include "hal/switch_driver.h"      // switchGetMaxSwitches / switchGetDefaultName — Set 3b switch naming
#include "pulses/pulses_common.h"   // setModuleMode (bind/range) — Set 3c
#include "storage/yaml/yaml_node.h" // YamlIdStr — reuse EdgeTX's own ModuleType table for authoritative labels
#include "telemetry/telemetry.h"    // telemetryItems[] + isTelemetryFieldAvailable (docs/15)
#include "telemetry/crossfire.h"    // MODULE_ADDRESS/COMMAND_ID — CRSF injection (ELRS tool, ADR-0021)
#include "crc.h"                    // crc8 / crc8_BA for the CRSF output frame
#include "audio.h"                  // AUDIO_FILENAME_MAXLEN + PLAY_* for the unified-audio tap (docs/ARCH §4)
#include "storage/sdcard_common.h"  // getModelPath — BULK model-file pull (docs/protocol §8)
#include "storage/sdcard_yaml.h"    // getModelNumberStr/MODELIDX_STRLEN — active model file (non-colorlcd)
#if defined(STORAGE_MODELSLIST)
#include "storage/modelslist.h"     // modelslist.getCurrentModel()->modelFilename (colorlcd)
#include "layout.h"                 // LayoutFactory — custom-screen teardown/reload on model SELECT (Set 6)
#include "mainwindow.h"             // MainWindow::instance()->enableWidgetRefresh — pause refresh during switch
#endif
#include "protocol.h"

// ---------------------------------------------------------------------------
// UE_DEBUG_CRUMBS — power-off-surviving breadcrumbs for diagnosing the colorlcd
// (TX16S) shutdown. The companion tick runs in the 10ms timer-service task; we
// only set a RAM stage code there (cheap, no I/O) and flush it to the SD card
// from the MAIN task (ueCrumbFlush(), called from perMain) where FatFs is safe.
// Entirely compiled out unless -DUE_DEBUG_CRUMBS, so normal builds are unchanged.
// ---------------------------------------------------------------------------
#if defined(UE_DEBUG_CRUMBS)
volatile uint8_t ueCrumbStage = 0;   // last milestone reached in the companion path
volatile uint8_t ueCrumbMsg   = 0;   // last protocol message type dispatched in onFrame
#define UE_CRUMB(n)      do { ueCrumbStage = (uint8_t)(n); } while (0)
#define UE_CRUMB_MSG(t)  do { ueCrumbMsg   = (uint8_t)(t); } while (0)
extern "C" void ueCrumbFlush() {   // called from perMain() — MAIN task, SD-safe
  // High-water mark: each tick re-climbs 10..20, so only log when a NEW furthest stage is
  // reached. The trail ascends monotonically; its LAST line is where the firmware died.
  static uint8_t hiStage = 0, hiMsg = 0;
  uint8_t s = ueCrumbStage, mg = ueCrumbMsg;
  if (s <= hiStage && mg == hiMsg) return;
  if (s > hiStage) hiStage = s;
  hiMsg = mg;
  uint8_t lastStage = s, lastMsg = mg;
  FIL f;
  if (f_open(&f, "/uecrumb.txt", FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) return;
  f_lseek(&f, f_size(&f));
  char line[48];
  int n = 0; const char* p = "stage=";
  while (*p) line[n++] = *p++;
  line[n++] = '0' + (lastStage / 100) % 10;
  line[n++] = '0' + (lastStage / 10) % 10;
  line[n++] = '0' + lastStage % 10;
  p = " msg=0x"; while (*p) line[n++] = *p++;
  const char* hex = "0123456789abcdef";
  line[n++] = hex[(lastMsg >> 4) & 0xF];
  line[n++] = hex[lastMsg & 0xF];
  line[n++] = '\n';
  UINT bw; f_write(&f, line, n, &bw); f_sync(&f); f_close(&f);
}
#else
#define UE_CRUMB(n)      do {} while (0)
#define UE_CRUMB_MSG(t)  do {} while (0)
#endif

extern void mixerTaskStop();
extern void mixerTaskStart();
extern uint8_t getCurvePoints(uint8_t index);   // curves.cpp (no header decl); docs/13

extern void usbSerialPutc(void*, uint8_t c);
extern void usbSerialSetReceiveDataCb(void*, void (*cb)(uint8_t*, uint32_t));
extern uint32_t usbSerialFreeSpace();   // bytes free in the CDC TX ring (docs/09)

using namespace ultraedge;

// ---- Config field registry --------------------------------------------------
// Typed-field access (docs/06-config-prd.md). Each field: a getter that fills a
// CONFIG_VALUE payload, and a validating setter. This list is the safety
// boundary — only model/radio SETTINGS, never the control path. Persistence uses
// EdgeTX's own storageDirty(), the same path the radio's menus use.

// Build a CONFIG_VALUE payload: [field_id(2), type(1), len(1), bytes...].
static size_t cfgValuePayload(uint16_t id, uint8_t type, const uint8_t* data,
                              uint8_t len, uint8_t* out) {
  out[0] = (uint8_t)(id & 0xFF); out[1] = (uint8_t)(id >> 8);
  out[2] = type; out[3] = len;
  for (uint8_t i = 0; i < len; ++i) out[4 + i] = data[i];
  return 4 + len;
}

// ---- Outputs page: per-channel fields via channel-encoded field ids ---------
// id = CFG_OUT_BASE | (channel<<4) | subfield.  Value encoding:
//   reverse: bool
//   subtrim: int percent = offset/10  (offset stored in tenths, ±1000)
//   min:     int percent P; displayed_tenths = stored_min - 1000 => P = (stored-1000)/10, stored = P*10+1000  (P in -150..0)
//   max:     int percent P; displayed_tenths = stored_max + 1000 => P = (stored+1000)/10, stored = P*10-1000   (P in 0..150)
// Ranges are chosen so the 11-bit stored fields never overflow (validated in setter).
static inline uint8_t outChannel(uint16_t id) { return (uint8_t)((id >> 4) & 0xFF); }
static inline uint8_t outSub(uint16_t id)     { return (uint8_t)(id & 0x0F); }

static void putI16(uint8_t* b, int16_t v) { b[0] = (uint8_t)(v & 0xFF); b[1] = (uint8_t)((uint16_t)v >> 8); }

static size_t strTrim(const char* src, uint8_t cap, char* tmp) {
  uint8_t n = cap; for (uint8_t i = 0; i < cap; ++i) tmp[i] = src[i];
  while (n > 0 && (tmp[n-1] == ' ' || tmp[n-1] == '\0')) n--;
  return n;
}

static size_t cfgGetOutput(uint16_t id, uint8_t* out) {
  uint8_t ch = outChannel(id); uint8_t sub = outSub(id);
  if (ch >= OUT_CHANNELS) return 0;
  LimitData* ld = &g_model.limitData[ch];
  switch (sub) {
    case OUT_REVERSE: { uint8_t b = ld->revert ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case OUT_SUBTRIM: { int16_t p = (int16_t)(ld->offset / 10); uint8_t b[2]; putI16(b, p); return cfgValuePayload(id, T_I16, b, 2, out); }
    case OUT_MIN:     { int16_t p = (int16_t)(((int)ld->min - 1000) / 10); uint8_t b[2]; putI16(b, p); return cfgValuePayload(id, T_I16, b, 2, out); }
    case OUT_MAX:     { int16_t p = (int16_t)(((int)ld->max + 1000) / 10); uint8_t b[2]; putI16(b, p); return cfgValuePayload(id, T_I16, b, 2, out); }
    case OUT_NAME:    { char tmp[LEN_CHANNEL_NAME]; size_t n = strTrim(ld->name, LEN_CHANNEL_NAME, tmp); return cfgValuePayload(id, T_STR, (const uint8_t*)tmp, (uint8_t)n, out); }
    case OUT_CURVE:   { int16_t v = (int16_t)ld->curve; uint8_t b[2]; putI16(b, v); return cfgValuePayload(id, T_I16, b, 2, out); }
    case OUT_PPMCENTER:{ int16_t v = (int16_t)(PPM_CENTER + ld->ppmCenter); uint8_t b[2]; putI16(b, v); return cfgValuePayload(id, T_I16, b, 2, out); }
    case OUT_SUBTRIMMODE:{ uint8_t b = ld->symetrical ? 1 : 0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    default: return 0;
  }
}

static uint8_t cfgSetOutput(uint16_t id, uint8_t type, const uint8_t* data, uint8_t len) {
  uint8_t ch = outChannel(id); uint8_t sub = outSub(id);
  if (ch >= OUT_CHANNELS) return NACK_UNKNOWN_FIELD;
  LimitData* ld = &g_model.limitData[ch];
  switch (sub) {
    case OUT_REVERSE: { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; ld->revert = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    case OUT_SUBTRIM: { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t p = (int16_t)(data[0] | (data[1] << 8)); if (p < -100 || p > 100) return NACK_OUT_OF_RANGE; ld->offset = (int)p * 10; storageDirty(EE_MODEL); return 0; }
    case OUT_MIN:     { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t p = (int16_t)(data[0] | (data[1] << 8)); if (p < -150 || p > 0)   return NACK_OUT_OF_RANGE; ld->min = (int)p * 10 + 1000; storageDirty(EE_MODEL); return 0; }
    case OUT_MAX:     { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t p = (int16_t)(data[0] | (data[1] << 8)); if (p < 0 || p > 150)    return NACK_OUT_OF_RANGE; ld->max = (int)p * 10 - 1000; storageDirty(EE_MODEL); return 0; }
    case OUT_NAME:    { if (type != T_STR) return NACK_BAD_TYPE; if (len > LEN_CHANNEL_NAME) return NACK_BAD_LENGTH; for (uint8_t i=0;i<LEN_CHANNEL_NAME;++i) ld->name[i] = (i<len)?(char)data[i]:' '; storageDirty(EE_MODEL); return 0; }
    case OUT_CURVE:   { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v = (int16_t)(data[0] | (data[1] << 8)); if (v < -MAX_CURVES || v > MAX_CURVES) return NACK_OUT_OF_RANGE; ld->curve = (int8_t)v; storageDirty(EE_MODEL); return 0; }
    case OUT_PPMCENTER:{ if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v = (int16_t)(data[0] | (data[1] << 8)); int c = (int)v - PPM_CENTER; if (c < -500 || c > 500) return NACK_OUT_OF_RANGE; ld->ppmCenter = c; storageDirty(EE_MODEL); return 0; }
    case OUT_SUBTRIMMODE:{ if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 1) return NACK_OUT_OF_RANGE; ld->symetrical = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    default: return NACK_UNKNOWN_FIELD;
  }
}

// ---- Timer detail fields: id = CFG_TMR_BASE | (timer<<4) | TmrSub ------------
static inline uint8_t tmrIndex(uint16_t id) { return (uint8_t)((id >> 4) & 0x0F); }
static inline uint8_t tmrSub(uint16_t id)   { return (uint8_t)(id & 0x0F); }

static size_t cfgGetTimer(uint16_t id, uint8_t* out) {
  uint8_t k = tmrIndex(id); uint8_t sub = tmrSub(id);
  if (k >= NUM_TIMERS) return 0;
  TimerData* t = &g_model.timers[k];
  switch (sub) {
    case TMR_NAME:    { char tmp[LEN_TIMER_NAME]; size_t n = strTrim(t->name, LEN_TIMER_NAME, tmp); return cfgValuePayload(id, T_STR, (const uint8_t*)tmp, (uint8_t)n, out); }
    case TMR_MODE:    { uint8_t b = (uint8_t)(t->mode & 0x07); if (b>5) b=0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case TMR_START:   { uint16_t v = (uint16_t)t->start; uint8_t b[2] = {(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case TMR_MINBEEP: { uint8_t b = t->minuteBeep ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case TMR_COUNTDOWN:{ uint8_t b = (uint8_t)(t->countdownBeep & 0x03); return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case TMR_PERSIST: { uint8_t b = (uint8_t)(t->persistent & 0x03); if (b>2) b=0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case TMR_DIR:     { uint8_t b = t->showElapsed ? 1 : 0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    default: return 0;
  }
}

// ---- Set 5 trainer per-channel mix: id = CFG_TRN_BASE | (channel<<4) | TrnSub ----
static inline uint8_t trnCh(uint16_t id)  { return (uint8_t)((id >> 4) & 0x0F); }
static inline uint8_t trnSub(uint16_t id) { return (uint8_t)(id & 0x0F); }
static size_t cfgGetTrainer(uint16_t id, uint8_t* out) {
  uint8_t ch = trnCh(id), sub = trnSub(id);
  if (ch >= 4) return 0;
  TrainerMix* m = &g_eeGeneral.trainer.mix[ch];
  switch (sub) {
    case TRN_SRC:    { uint8_t b = (uint8_t)(m->srcChn & 0x03); return cfgValuePayload(id, T_ENUM, &b, 1, out); }  // 0..3 = CH1..CH4
    case TRN_MUX:    { uint8_t b = (uint8_t)(m->mode & 0x03); if (b > 2) b = 0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case TRN_WEIGHT: { int16_t v = (int16_t)m->studWeight; uint8_t b[2] = {(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)}; return cfgValuePayload(id, T_I16, b, 2, out); }
    default: return 0;
  }
}
static uint8_t cfgSetTrainer(uint16_t id, uint8_t type, const uint8_t* data, uint8_t len) {
  uint8_t ch = trnCh(id), sub = trnSub(id);
  if (ch >= 4) return NACK_UNKNOWN_FIELD;
  TrainerMix* m = &g_eeGeneral.trainer.mix[ch];
  switch (sub) {
    case TRN_SRC:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 3) return NACK_OUT_OF_RANGE; m->srcChn = data[0]; storageDirty(EE_GENERAL); return 0; }  // 0..3 = CH1..CH4
    case TRN_MUX:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; m->mode = data[0]; storageDirty(EE_GENERAL); return 0; }
    case TRN_WEIGHT: { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v = (int16_t)(data[0]|(data[1]<<8)); if (v < -125 || v > 125) return NACK_OUT_OF_RANGE; m->studWeight = (int8_t)v; storageDirty(EE_GENERAL); return 0; }
    default: return NACK_UNKNOWN_FIELD;
  }
}
static uint8_t cfgSetTimer(uint16_t id, uint8_t type, const uint8_t* data, uint8_t len) {
  uint8_t k = tmrIndex(id); uint8_t sub = tmrSub(id);
  if (k >= NUM_TIMERS) return NACK_UNKNOWN_FIELD;
  TimerData* t = &g_model.timers[k];
  switch (sub) {
    case TMR_NAME:    { if (type != T_STR) return NACK_BAD_TYPE; if (len > LEN_TIMER_NAME) return NACK_BAD_LENGTH; for (uint8_t i=0;i<LEN_TIMER_NAME;++i) t->name[i] = (i<len)?(char)data[i]:' '; storageDirty(EE_MODEL); return 0; }
    case TMR_MODE:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 5) return NACK_OUT_OF_RANGE; t->mode = data[0]; storageDirty(EE_MODEL); return 0; }
    case TMR_START:   { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v = data[0]|(data[1]<<8); if (v > 3600) return NACK_OUT_OF_RANGE; t->start = v; storageDirty(EE_MODEL); return 0; }
    case TMR_MINBEEP: { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; t->minuteBeep = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    case TMR_COUNTDOWN:{ if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 3) return NACK_OUT_OF_RANGE; t->countdownBeep = data[0]; storageDirty(EE_MODEL); return 0; }
    case TMR_PERSIST: { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; t->persistent = data[0]; storageDirty(EE_MODEL); return 0; }
    case TMR_DIR:     { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 1) return NACK_OUT_OF_RANGE; t->showElapsed = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    default: return NACK_UNKNOWN_FIELD;
  }
}

// ---- Per-item subsystems (docs/11) -----------------------------------------
// id = <BASE> + item*ITEM_STRIDE + sub. These are model SETTINGS / discrete
// references — never the mixer math itself. Inputs/Mixes expose Name + the
// discrete source/switch/curve/multiplex selectors; their weight/offset (which
// ride EdgeTX's source-value accessor and feed the control path) are deferred to
// the picker pass rather than written with an approximate mapping.
static void putI16v(uint8_t* b, int16_t v) { b[0]=(uint8_t)(v&0xFF); b[1]=(uint8_t)((uint16_t)v>>8); }
static size_t vI16(uint16_t id, int16_t v, uint8_t* out) { uint8_t b[2]; putI16v(b,v); return cfgValuePayload(id,T_I16,b,2,out); }
static size_t vU16(uint16_t id, uint16_t v, uint8_t* out) { uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id,T_U16,b,2,out); }
static size_t vBool(uint16_t id, uint8_t v, uint8_t* out) { uint8_t b=v?1:0; return cfgValuePayload(id,T_BOOL,&b,1,out); }
static size_t vEnum(uint16_t id, uint8_t v, uint8_t* out) { return cfgValuePayload(id,T_ENUM,&v,1,out); }
static size_t vStr(uint16_t id, const char* s, uint8_t cap, uint8_t* out) { char t[24]; uint8_t n=(uint8_t)strTrim(s,cap,t); return cfgValuePayload(id,T_STR,(const uint8_t*)t,n,out); }
static uint8_t setStrField(char* dst, uint8_t cap, uint8_t type, const uint8_t* d, uint8_t len) {
  if (type!=T_STR) return NACK_BAD_TYPE; if (len>cap) return NACK_BAD_LENGTH;
  // NUL-pad, not space-pad (docs/40). EdgeTX stores these names NUL-terminated and its own consumers
  // rely on it: playCustomFunctionFile() copies the whole SF play.name field and appends ".wav", so a
  // space-padded name became "/SOUNDS/en/althld  .wav" — unresolvable on the phone AND a real radio.
  for (uint8_t i=0;i<cap;++i) dst[i]=(i<len)?(char)d[i]:'\0'; storageDirty(EE_MODEL); return 0; }
static int16_t rdI16(const uint8_t* d){ return (int16_t)(d[0]|(d[1]<<8)); }

// ---- Weight/Offset (SourceNumVal fields) — control-path-adjacent, docs/13 -----
// Inputs/Mixes weight & offset are 11-bit SourceNumVal fields: a literal number,
// OR a source/gvar reference (isSource=1). We use EdgeTX's OWN authoritative
// conversions (datastructs_private.h) so the encoding can never drift:
//   read : sourceNumValToLuaInt(rawValue)  -> signed literal, or ±1024+idx for a ref
//   write: luaIntToSourceNumval(val)       -> packed 11-bit rawValue
// SAFETY: this pass edits LITERALS only. A write whose |value| >= 1024 would be a
// source/gvar reference — we NACK it rather than risk clobbering a linked weight.
// So a weight bound to a GVAR/source stays exactly as the radio set it.
static size_t vNumVal(uint16_t id, uint16_t rawValue, uint8_t* out) {
  int v = sourceNumValToLuaInt(rawValue);
  uint8_t b[2]; putI16v(b, (int16_t)v); return cfgValuePayload(id, T_I16, b, 2, out);
}
// Returns 0 and fills *raw on success; else a NACK reason. lo/hi bound the literal.
static uint8_t setNumVal(uint8_t t, const uint8_t* d, uint8_t len, int lo, int hi, uint16_t* raw) {
  if (t != T_I16 || len != 2) return NACK_BAD_TYPE;
  int v = rdI16(d);
  if (v <= -1024 || v >= 1024) return NACK_OUT_OF_RANGE;   // would be a source ref — refuse
  if (v < lo || v > hi) return NACK_OUT_OF_RANGE;
  *raw = luaIntToSourceNumval(v);
  return 0;
}

static uint8_t listUsedExpos();   // fwd decl: used-expo count for the Inputs list (docs/13)

static inline uint16_t itemOf(uint16_t id, uint16_t base){ return (uint16_t)((id-base)/ITEM_STRIDE); }
static inline uint8_t  subOf (uint16_t id, uint16_t base){ return (uint8_t)((id-base)%ITEM_STRIDE); }

// ---- Inputs (ExpoData) ----
static size_t inGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_INPUT_BASE); if(i>=MAX_EXPOS) return 0; ExpoData* e=&g_model.expoData[i];
  switch(subOf(id,CFG_INPUT_BASE)){
    case IN_NAME:  return vStr(id,e->name,LEN_EXPOMIX_NAME,out);
    case IN_WEIGHT:return vNumVal(id,(uint16_t)e->weight,out);
    case IN_OFFSET:return vNumVal(id,(uint16_t)e->offset,out);
    case IN_SRC:   return vI16(id,(int16_t)e->srcRaw,out);
    case IN_SWITCH:return vI16(id,(int16_t)e->swtch,out);
    case IN_CURVE: return vI16(id,(int16_t)e->curve.value,out);
    case IN_LINE_CH:return vU16(id,(uint16_t)e->chn,out);
    default: return 0; } }
static uint8_t inSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_INPUT_BASE); if(i>=MAX_EXPOS) return NACK_UNKNOWN_FIELD; ExpoData* e=&g_model.expoData[i];
  switch(subOf(id,CFG_INPUT_BASE)){
    case IN_NAME:  return setStrField(e->name,LEN_EXPOMIX_NAME,t,d,len);
    case IN_WEIGHT:{uint16_t raw; uint8_t rc=setNumVal(t,d,len,-100,100,&raw); if(rc)return rc; e->weight=raw; storageDirty(EE_MODEL); return 0;}
    case IN_OFFSET:{uint16_t raw; uint8_t rc=setNumVal(t,d,len,-100,100,&raw); if(rc)return rc; e->offset=raw; storageDirty(EE_MODEL); return 0;}
    case IN_SRC:   if(t!=T_I16||len!=2)return NACK_BAD_TYPE; e->srcRaw=rdI16(d); storageDirty(EE_MODEL); return 0;
    case IN_SWITCH:if(t!=T_I16||len!=2)return NACK_BAD_TYPE; e->swtch=rdI16(d); storageDirty(EE_MODEL); return 0;
    case IN_CURVE: if(t!=T_I16||len!=2)return NACK_BAD_TYPE; e->curve.value=rdI16(d); storageDirty(EE_MODEL); return 0;
    default: return NACK_UNKNOWN_FIELD; } }

// ---- Mixes (MixData) ----
static size_t mxGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_MIX_BASE); if(i>=MAX_MIXERS) return 0; MixData* m=&g_model.mixData[i];
  switch(subOf(id,CFG_MIX_BASE)){
    case MX_NAME:  return vStr(id,m->name,LEN_EXPOMIX_NAME,out);
    case MX_WEIGHT:return vNumVal(id,(uint16_t)m->weight,out);
    case MX_OFFSET:return vNumVal(id,(uint16_t)m->offset,out);
    case MX_SRC:   return vI16(id,(int16_t)m->srcRaw,out);
    case MX_DESTCH:return vU16(id,(uint16_t)(m->destCh+1),out);
    case MX_SWITCH:return vI16(id,(int16_t)m->swtch,out);
    case MX_MULT:  return vEnum(id,(uint8_t)m->mltpx,out);
    case MX_CURVE: return vI16(id,(int16_t)m->curve.value,out);
    default: return 0; } }
static uint8_t mxSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_MIX_BASE); if(i>=MAX_MIXERS) return NACK_UNKNOWN_FIELD; MixData* m=&g_model.mixData[i];
  switch(subOf(id,CFG_MIX_BASE)){
    case MX_NAME:  return setStrField(m->name,LEN_EXPOMIX_NAME,t,d,len);
    case MX_WEIGHT:{uint16_t raw; uint8_t rc=setNumVal(t,d,len,MIX_WEIGHT_MIN,MIX_WEIGHT_MAX,&raw); if(rc)return rc; m->weight=raw; storageDirty(EE_MODEL); return 0;}
    case MX_OFFSET:{uint16_t raw; uint8_t rc=setNumVal(t,d,len,MIX_OFFSET_MIN,MIX_OFFSET_MAX,&raw); if(rc)return rc; m->offset=raw; storageDirty(EE_MODEL); return 0;}
    case MX_SRC:   if(t!=T_I16||len!=2)return NACK_BAD_TYPE; m->srcRaw=rdI16(d); storageDirty(EE_MODEL); return 0;
    case MX_DESTCH:if(t!=T_U16||len!=2)return NACK_BAD_TYPE;{uint16_t v=d[0]|(d[1]<<8); if(v<1||v>OUT_CHANNELS)return NACK_OUT_OF_RANGE; m->destCh=v-1;} storageDirty(EE_MODEL); return 0;
    case MX_SWITCH:if(t!=T_I16||len!=2)return NACK_BAD_TYPE; m->swtch=rdI16(d); storageDirty(EE_MODEL); return 0;
    case MX_MULT:  if(t!=T_ENUM||len!=1)return NACK_BAD_TYPE; if(d[0]>2)return NACK_OUT_OF_RANGE; m->mltpx=d[0]; storageDirty(EE_MODEL); return 0;
    case MX_CURVE: if(t!=T_I16||len!=2)return NACK_BAD_TYPE; m->curve.value=rdI16(d); storageDirty(EE_MODEL); return 0;
    default: return NACK_UNKNOWN_FIELD; } }

// ---- Curves (CurveHeader) ----
static size_t cvGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_CURVE_BASE); if(i>=MAX_CURVES) return 0; CurveHeader* c=&g_model.curves[i];
  switch(subOf(id,CFG_CURVE_BASE)){
    case CV_NAME:  return vStr(id,c->name,LEN_CURVE_NAME,out);
    case CV_TYPE:  return vEnum(id,(uint8_t)c->type,out);
    case CV_SMOOTH:return vBool(id,c->smooth,out);
    case CV_POINTS:return vI16(id,(int16_t)(c->points+5),out);
    default: {
      uint8_t sub=subOf(id,CFG_CURVE_BASE);
      if (sub>=CV_PT0 && sub<CV_PT0+MAX_POINTS_PER_CURVE) {   // curve point Y-value (docs/13)
        uint8_t pt=(uint8_t)(sub-CV_PT0); if (pt>=getCurvePoints(i)) return 0;
        int8_t* pts=curveAddress(i); return vI16(id,(int16_t)pts[pt],out);
      }
      return 0; } } }
static uint8_t cvSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_CURVE_BASE); if(i>=MAX_CURVES) return NACK_UNKNOWN_FIELD; CurveHeader* c=&g_model.curves[i];
  switch(subOf(id,CFG_CURVE_BASE)){
    case CV_NAME:  return setStrField(c->name,LEN_CURVE_NAME,t,d,len);
    case CV_TYPE:  if(t!=T_ENUM||len!=1)return NACK_BAD_TYPE; if(d[0]>1)return NACK_OUT_OF_RANGE; c->type=d[0]; storageDirty(EE_MODEL); return 0;
    case CV_SMOOTH:if(t!=T_BOOL||len!=1)return NACK_BAD_TYPE; c->smooth=d[0]?1:0; storageDirty(EE_MODEL); return 0;
    case CV_POINTS:if(t!=T_I16||len!=2)return NACK_BAD_TYPE;{int16_t p=rdI16(d); if(p<3||p>17)return NACK_OUT_OF_RANGE; c->points=p-5;} storageDirty(EE_MODEL); return 0;
    default: {
      uint8_t sub=subOf(id,CFG_CURVE_BASE);
      if (sub>=CV_PT0 && sub<CV_PT0+MAX_POINTS_PER_CURVE) {   // curve point Y-value (docs/13)
        uint8_t pt=(uint8_t)(sub-CV_PT0); if (pt>=getCurvePoints(i)) return NACK_OUT_OF_RANGE;
        if (t!=T_I16||len!=2) return NACK_BAD_TYPE;
        int16_t y=rdI16(d); if (y<-100||y>100) return NACK_OUT_OF_RANGE;
        curveAddress(i)[pt]=(int8_t)y; storageDirty(EE_MODEL); return 0;
      }
      return NACK_UNKNOWN_FIELD; } } }

// ---- Logical switches (LogicalSwitchData) ----
static size_t lsGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_LS_BASE); if(i>=MAX_LOGICAL_SWITCHES) return 0; LogicalSwitchData* s=&g_model.logicalSw[i];
  switch(subOf(id,CFG_LS_BASE)){
    case LS_FUNC:    return vI16(id,(int16_t)s->func,out);
    case LS_V1:      return vI16(id,(int16_t)s->v1,out);
    case LS_V2:      return vI16(id,(int16_t)s->v2,out);
    case LS_V3:      return vI16(id,(int16_t)s->v3,out);
    case LS_AND:     return vI16(id,(int16_t)s->andsw,out);
    case LS_DELAY:   return vU16(id,(uint16_t)s->delay,out);
    case LS_DURATION:return vU16(id,(uint16_t)s->duration,out);
    default: return 0; } }
static uint8_t lsSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_LS_BASE); if(i>=MAX_LOGICAL_SWITCHES) return NACK_UNKNOWN_FIELD; LogicalSwitchData* s=&g_model.logicalSw[i];
  switch(subOf(id,CFG_LS_BASE)){
    case LS_FUNC:    if(t!=T_I16||len!=2)return NACK_BAD_TYPE; s->func=(uint8_t)rdI16(d); storageDirty(EE_MODEL); return 0;
    case LS_V1:      if(t!=T_I16||len!=2)return NACK_BAD_TYPE; s->v1=rdI16(d); storageDirty(EE_MODEL); return 0;
    case LS_V2:      if(t!=T_I16||len!=2)return NACK_BAD_TYPE; s->v2=rdI16(d); storageDirty(EE_MODEL); return 0;
    case LS_V3:      if(t!=T_I16||len!=2)return NACK_BAD_TYPE; s->v3=rdI16(d); storageDirty(EE_MODEL); return 0;
    case LS_AND:     if(t!=T_I16||len!=2)return NACK_BAD_TYPE; s->andsw=rdI16(d); storageDirty(EE_MODEL); return 0;
    case LS_DELAY:   if(t!=T_U16||len!=2)return NACK_BAD_TYPE; s->delay=(uint8_t)(d[0]|(d[1]<<8)); storageDirty(EE_MODEL); return 0;
    case LS_DURATION:if(t!=T_U16||len!=2)return NACK_BAD_TYPE; s->duration=(uint8_t)(d[0]|(d[1]<<8)); storageDirty(EE_MODEL); return 0;
    default: return NACK_UNKNOWN_FIELD; } }

// ---- Special functions (CustomFunctionData) ----
// EdgeTX parity (docs/32): SF_FUNC is a real function picker and SF_PARAM/SF_PARAM2 carry the
// per-function parameter mapped to the correct CustomFunctionData union member, exactly as
// EdgeTX's own SF editor (gui/128x64/model_special_functions.cpp) does via the CFN_* macros:
//   CFN_PARAM = fp.all.val   CFN_CH/GVAR/TIMER_INDEX = fp.all.param   play.name = filename.
enum SfParamKind : uint8_t {
  SFP_NONE=0, SFP_SOUND, SFP_SOURCE, SFP_FILE, SFP_HAPTIC, SFP_SCREEN,
  SFP_RESET, SFP_MODULE, SFP_CHVAL, SFP_TIMERVAL, SFP_GVAR,
};
static SfParamKind sfParamKind(uint8_t func){
  switch(func){
    case FUNC_PLAY_SOUND: return SFP_SOUND;
    case FUNC_PLAY_VALUE: case FUNC_VOLUME: case FUNC_BACKLIGHT: return SFP_SOURCE;
    case FUNC_PLAY_TRACK: case FUNC_BACKGND_MUSIC: case FUNC_PLAY_SCRIPT: case FUNC_RGB_LED: return SFP_FILE;
    case FUNC_HAPTIC:     return SFP_HAPTIC;
    case FUNC_SET_SCREEN: return SFP_SCREEN;
    case FUNC_RESET:      return SFP_RESET;
    case FUNC_RANGECHECK: case FUNC_BIND: return SFP_MODULE;
    case FUNC_OVERRIDE_CHANNEL: return SFP_CHVAL;
    case FUNC_SET_TIMER:  return SFP_TIMERVAL;
    case FUNC_ADJUST_GVAR: return SFP_GVAR;
    default: return SFP_NONE;
  }
}
static size_t sfGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_SF_BASE); if(i>=MAX_SPECIAL_FUNCTIONS) return 0; CustomFunctionData* c=&g_model.customFn[i];
  SfParamKind k=sfParamKind((uint8_t)c->func);
  switch(subOf(id,CFG_SF_BASE)){
    case SF_SWITCH:return vI16(id,(int16_t)c->swtch,out);
    case SF_FUNC:  return vEnum(id,(uint8_t)c->func,out);
    case SF_REPEAT:return vI16(id,(int16_t)c->repeat,out);
    case SF_ENABLE:return vBool(id,c->active,out);
    case SF_PARAM:
      if(k==SFP_FILE)  return vStr(id,c->play.name,sizeof(c->play.name),out);
      if(k==SFP_SOUND||k==SFP_RESET||k==SFP_MODULE) return vEnum(id,(uint8_t)c->all.val,out);
      if(k==SFP_NONE)  return 0;
      return vI16(id,(int16_t)c->all.val,out);           // SOURCE(T_REF)/HAPTIC/SCREEN/CH/TIMER/GVAR value
    case SF_PARAM2:
      if(k==SFP_CHVAL||k==SFP_TIMERVAL||k==SFP_GVAR) return vI16(id,(int16_t)c->all.param,out);
      return 0;
    default: return 0; } }
static uint8_t sfSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_SF_BASE); if(i>=MAX_SPECIAL_FUNCTIONS) return NACK_UNKNOWN_FIELD; CustomFunctionData* c=&g_model.customFn[i];
  SfParamKind k=sfParamKind((uint8_t)c->func);
  switch(subOf(id,CFG_SF_BASE)){
    case SF_SWITCH:if(t!=T_I16||len!=2)return NACK_BAD_TYPE; c->swtch=rdI16(d); storageDirty(EE_MODEL); return 0;
    case SF_FUNC:  if(t!=T_ENUM||len!=1)return NACK_BAD_TYPE; if(d[0]>=FUNC_MAX)return NACK_OUT_OF_RANGE;
      if((uint8_t)c->func!=d[0]){ c->func=d[0]; memset(&c->all,0,sizeof(c->all)); }  // clear stale union on func change (EdgeTX does this)
      storageDirty(EE_MODEL); return 0;
    case SF_REPEAT:if(t!=T_I16||len!=2)return NACK_BAD_TYPE; c->repeat=(int8_t)rdI16(d); storageDirty(EE_MODEL); return 0;
    case SF_ENABLE:if(t!=T_BOOL||len!=1)return NACK_BAD_TYPE; c->active=d[0]?1:0; storageDirty(EE_MODEL); return 0;
    case SF_PARAM:
      if(k==SFP_FILE){ return setStrField(c->play.name,sizeof(c->play.name),t,d,len); }
      if(k==SFP_SOUND||k==SFP_RESET||k==SFP_MODULE){ if(t!=T_ENUM||len!=1)return NACK_BAD_TYPE; c->all.val=d[0]; storageDirty(EE_MODEL); return 0; }
      if(k==SFP_NONE) return NACK_UNKNOWN_FIELD;
      if(t!=T_I16||len!=2)return NACK_BAD_TYPE; c->all.val=rdI16(d); storageDirty(EE_MODEL); return 0;
    case SF_PARAM2:
      if(k!=SFP_CHVAL&&k!=SFP_TIMERVAL&&k!=SFP_GVAR) return NACK_UNKNOWN_FIELD;
      if(t!=T_I16||len!=2)return NACK_BAD_TYPE; c->all.param=(uint8_t)rdI16(d); storageDirty(EE_MODEL); return 0;
    default: return NACK_UNKNOWN_FIELD; } }

// ---- Global variables (GVarData) ----
static size_t gvGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_GVAR_BASE); if(i>=MAX_GVARS) return 0; GVarData* g=&g_model.gvars[i];
  switch(subOf(id,CFG_GVAR_BASE)){
    case GV_NAME: return vStr(id,g->name,LEN_GVAR_NAME,out);
    case GV_MIN:  return vI16(id,(int16_t)((int)g->min-1024),out);
    case GV_MAX:  return vI16(id,(int16_t)(1024-(int)g->max),out);
    case GV_POPUP:return vBool(id,g->popup,out);
    case GV_PREC: return vEnum(id,(uint8_t)g->prec,out);
    case GV_UNIT: return vEnum(id,(uint8_t)(g->unit&1),out);
    default: return 0; } }
static uint8_t gvSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_GVAR_BASE); if(i>=MAX_GVARS) return NACK_UNKNOWN_FIELD; GVarData* g=&g_model.gvars[i];
  switch(subOf(id,CFG_GVAR_BASE)){
    case GV_NAME: return setStrField(g->name,LEN_GVAR_NAME,t,d,len);
    case GV_MIN:  if(t!=T_I16||len!=2)return NACK_BAD_TYPE;{int v=rdI16(d); if(v<-1024||v>1024)return NACK_OUT_OF_RANGE; g->min=v+1024;} storageDirty(EE_MODEL); return 0;
    case GV_MAX:  if(t!=T_I16||len!=2)return NACK_BAD_TYPE;{int v=rdI16(d); if(v<-1024||v>1024)return NACK_OUT_OF_RANGE; g->max=1024-v;} storageDirty(EE_MODEL); return 0;
    case GV_POPUP:if(t!=T_BOOL||len!=1)return NACK_BAD_TYPE; g->popup=d[0]?1:0; storageDirty(EE_MODEL); return 0;
    case GV_PREC: if(t!=T_ENUM||len!=1)return NACK_BAD_TYPE; if(d[0]>1)return NACK_OUT_OF_RANGE; g->prec=d[0]; storageDirty(EE_MODEL); return 0;
    case GV_UNIT: if(t!=T_ENUM||len!=1)return NACK_BAD_TYPE; if(d[0]>1)return NACK_OUT_OF_RANGE; g->unit=d[0]; storageDirty(EE_MODEL); return 0;
    default: return NACK_UNKNOWN_FIELD; } }

// ---- Flight modes (FlightModeData) ----
static size_t fmGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_FM_BASE); if(i>=MAX_FLIGHT_MODES) return 0; FlightModeData* fmd=&g_model.flightModeData[i];
  switch(subOf(id,CFG_FM_BASE)){
    case FM_NAME:   return vStr(id,fmd->name,LEN_FLIGHT_MODE_NAME,out);
    case FM_SWITCH: return vI16(id,(int16_t)fmd->swtch,out);
    case FM_FADEIN: return vU16(id,(uint16_t)fmd->fadeIn,out);
    case FM_FADEOUT:return vU16(id,(uint16_t)fmd->fadeOut,out);
    default: return 0; } }
static uint8_t fmSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_FM_BASE); if(i>=MAX_FLIGHT_MODES) return NACK_UNKNOWN_FIELD; FlightModeData* fmd=&g_model.flightModeData[i];
  switch(subOf(id,CFG_FM_BASE)){
    case FM_NAME:   return setStrField(fmd->name,LEN_FLIGHT_MODE_NAME,t,d,len);
    case FM_SWITCH: if(t!=T_I16||len!=2)return NACK_BAD_TYPE; fmd->swtch=rdI16(d); storageDirty(EE_MODEL); return 0;
    case FM_FADEIN: if(t!=T_U16||len!=2)return NACK_BAD_TYPE; fmd->fadeIn=(uint8_t)(d[0]|(d[1]<<8)); storageDirty(EE_MODEL); return 0;
    case FM_FADEOUT:if(t!=T_U16||len!=2)return NACK_BAD_TYPE; fmd->fadeOut=(uint8_t)(d[0]|(d[1]<<8)); storageDirty(EE_MODEL); return 0;
    default: return NACK_UNKNOWN_FIELD; } }

// ---- Telemetry sensors (TelemetrySensor) ----
static size_t teGet(uint16_t id, uint8_t* out){ uint16_t i=itemOf(id,CFG_TELEM_BASE); if(i>=MAX_TELEMETRY_SENSORS) return 0; TelemetrySensor* s=&g_model.telemetrySensors[i];
  switch(subOf(id,CFG_TELEM_BASE)){
    case TE_LABEL:return vStr(id,s->label,TELEM_LABEL_LEN,out);
    case TE_UNIT: return vU16(id,(uint16_t)s->unit,out);
    case TE_PREC: return vEnum(id,(uint8_t)s->prec,out);
    default: return 0; } }
static uint8_t teSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len){ uint16_t i=itemOf(id,CFG_TELEM_BASE); if(i>=MAX_TELEMETRY_SENSORS) return NACK_UNKNOWN_FIELD; TelemetrySensor* s=&g_model.telemetrySensors[i];
  switch(subOf(id,CFG_TELEM_BASE)){
    case TE_LABEL:return setStrField(s->label,TELEM_LABEL_LEN,t,d,len);
    case TE_UNIT: if(t!=T_U16||len!=2)return NACK_BAD_TYPE; s->unit=(uint8_t)(d[0]|(d[1]<<8)); storageDirty(EE_MODEL); return 0;
    case TE_PREC: if(t!=T_ENUM||len!=1)return NACK_BAD_TYPE; if(d[0]>2)return NACK_OUT_OF_RANGE; s->prec=d[0]; storageDirty(EE_MODEL); return 0;
    default: return NACK_UNKNOWN_FIELD; } }

// ---- Row summaries (T_STR shown on LINK rows in the parent list) ------------
static char* appendStr(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* appendInt(char* p, int v) {
  if (v < 0) { *p++ = '-'; v = -v; }
  char tmp[8]; int n = 0; if (v == 0) tmp[n++] = '0';
  while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
  while (n > 0) *p++ = tmp[--n];
  return p;
}
static const char* const OPTS_TMRMODE_S[] = { "OFF","ON","Strt","THs","TH%","THt" };

// Output channel list-row summary: "<min> <max>" plus a reverse marker.
static size_t outputRowSummary(uint16_t id, uint8_t* out) {
  uint8_t ch = (uint8_t)(id - CFG_OUTROW_BASE);
  if (ch >= OUT_CHANNELS) return 0;
  LimitData* ld = &g_model.limitData[ch];
  char s[32]; char* p = s;
  if (ld->revert) p = appendStr(p, "\xE2\x86\x94 ");           // ↔ reversed
  p = appendInt(p, ((int)ld->min - 1000) / 10); *p++ = ' ';
  p = appendInt(p, ((int)ld->max + 1000) / 10);
  return cfgValuePayload(id, T_STR, (const uint8_t*)s, (uint8_t)(p - s), out);
}

// Timer list-row summary: "OFF" or "<mode> MM:SS".
static size_t timerRowSummary(uint16_t id, uint8_t* out) {
  uint8_t k = (uint8_t)(id - CFG_TMRROW_BASE);
  if (k >= NUM_TIMERS) return 0;
  TimerData* t = &g_model.timers[k];
  char s[24]; char* p = s;
  uint8_t m = (uint8_t)(t->mode & 0x07); if (m > 5) m = 0;
  if (m == 0) { p = appendStr(p, "OFF"); }
  else {
    p = appendStr(p, OPTS_TMRMODE_S[m]); *p++ = ' ';
    int sec = (int)t->start; int mm = sec / 60, ss = sec % 60;
    p = appendInt(p, mm); *p++ = ':'; if (ss < 10) *p++ = '0'; p = appendInt(p, ss);
  }
  return cfgValuePayload(id, T_STR, (const uint8_t*)s, (uint8_t)(p - s), out);
}

// Section list-row summary (Trims / Throttle / Other): empty for now.
static size_t sectionRowSummary(uint16_t id, uint8_t* out) {
  return cfgValuePayload(id, T_STR, (const uint8_t*)"", 0, out);
}

// Generic per-item list-row summary: prefer the item's trimmed name, else a
// short "<tag><n>" placeholder. Keeps the list readable at a glance.
static size_t itemSummaryName(uint16_t id, uint8_t item, const char* name, uint8_t cap,
                              const char* tag, uint8_t* out) {
  char s[24]; char* p = s;
  char tmp[24]; size_t n = strTrim(name, cap, tmp);
  if (n > 0) { for (size_t i=0;i<n;++i) *p++ = tmp[i]; }
  else { p = appendStr(p, tag); p = appendInt(p, item + 1); }
  return cfgValuePayload(id, T_STR, (const uint8_t*)s, (uint8_t)(p - s), out);
}
// EdgeTX-style mix/input LINE blob (docs/14): raw values the app formats itself via
// its NameProvider (keeps source/switch/curve naming on the app). Layout:
//   {i16 src, i16 weight, i8 mplex(-1 n/a), i16 swtch, i16 curve, u8 active, u8 nameLen, name}
static size_t lineBlobPayload(uint16_t id, int16_t src, int16_t weight, int8_t mplex,
                              int16_t swtch, int16_t curve, uint8_t active,
                              const char* name, uint8_t nameCap, uint8_t* out) {
  uint8_t b[32]; size_t o=0;
  b[o++]=(uint8_t)(src&0xFF); b[o++]=(uint8_t)((src>>8)&0xFF);
  b[o++]=(uint8_t)(weight&0xFF); b[o++]=(uint8_t)((weight>>8)&0xFF);
  b[o++]=(uint8_t)mplex;
  b[o++]=(uint8_t)(swtch&0xFF); b[o++]=(uint8_t)((swtch>>8)&0xFF);
  b[o++]=(uint8_t)(curve&0xFF); b[o++]=(uint8_t)((curve>>8)&0xFF);
  b[o++]=active?1:0;
  char tmp[24]; size_t n=strTrim(name,nameCap,tmp); if(n>16)n=16;
  b[o++]=(uint8_t)n; for(size_t i=0;i<n;++i) b[o++]=(uint8_t)tmp[i];
  return cfgValuePayload(id, T_STR, b, (uint8_t)o, out);
}

static size_t subsystemSummary(uint16_t id, uint8_t* out) {
  if (id >= CFG_TELEM_BASE) { uint16_t i=itemOf(id,CFG_TELEM_BASE); if(i>=MAX_TELEMETRY_SENSORS)return 0; return itemSummaryName(id,(uint8_t)i,g_model.telemetrySensors[i].label,TELEM_LABEL_LEN,"Sensor ",out); }
  if (id >= CFG_FM_BASE)   { uint16_t i=itemOf(id,CFG_FM_BASE);   if(i>=MAX_FLIGHT_MODES)return 0;     return itemSummaryName(id,(uint8_t)i,g_model.flightModeData[i].name,LEN_FLIGHT_MODE_NAME,"FM",out); }
  if (id >= CFG_GVAR_BASE) { uint16_t i=itemOf(id,CFG_GVAR_BASE); if(i>=MAX_GVARS)return 0;            return itemSummaryName(id,(uint8_t)i,g_model.gvars[i].name,LEN_GVAR_NAME,"GV",out); }
  if (id >= CFG_SF_BASE)   { uint16_t i=itemOf(id,CFG_SF_BASE);   if(i>=MAX_SPECIAL_FUNCTIONS)return 0; char s[8];char*p=s;p=appendStr(p,"SF");p=appendInt(p,i+1);return cfgValuePayload(id,T_STR,(const uint8_t*)s,(uint8_t)(p-s),out); }
  if (id >= CFG_LS_BASE)   { uint16_t i=itemOf(id,CFG_LS_BASE);   if(i>=MAX_LOGICAL_SWITCHES)return 0;  char s[8];char*p=s;p=appendStr(p,"L");p=appendInt(p,i+1);return cfgValuePayload(id,T_STR,(const uint8_t*)s,(uint8_t)(p-s),out); }
  if (id >= CFG_CURVE_BASE){ uint16_t i=itemOf(id,CFG_CURVE_BASE);if(i>=MAX_CURVES)return 0;            return itemSummaryName(id,(uint8_t)i,g_model.curves[i].name,LEN_CURVE_NAME,"CV",out); }
  if (id >= CFG_MIX_BASE)  { uint16_t i=itemOf(id,CFG_MIX_BASE);  if(i>=MAX_MIXERS)return 0;  MixData* m=&g_model.mixData[i];
    return lineBlobPayload(id,(int16_t)m->srcRaw,(int16_t)sourceNumValToLuaInt(m->weight),(int8_t)m->mltpx,
                           (int16_t)m->swtch,(int16_t)m->curve.value,isMixActive((uint8_t)i),m->name,LEN_EXPOMIX_NAME,out); }
  if (id >= CFG_INPUT_BASE){ uint16_t i=itemOf(id,CFG_INPUT_BASE);if(i>=MAX_EXPOS)return 0;  ExpoData* e=&g_model.expoData[i];
    return lineBlobPayload(id,(int16_t)e->srcRaw,(int16_t)sourceNumValToLuaInt(e->weight),(int8_t)-1,
                           (int16_t)e->swtch,(int16_t)e->curve.value,isExpoActive((uint8_t)i),e->name,LEN_EXPOMIX_NAME,out); }
  return 0;
}

static uint16_t baseOf(uint16_t id) {
  if(id>=CFG_TELEM_BASE)return CFG_TELEM_BASE; if(id>=CFG_FM_BASE)return CFG_FM_BASE;
  if(id>=CFG_GVAR_BASE)return CFG_GVAR_BASE;   if(id>=CFG_SF_BASE)return CFG_SF_BASE;
  if(id>=CFG_LS_BASE)return CFG_LS_BASE;       if(id>=CFG_CURVE_BASE)return CFG_CURVE_BASE;
  if(id>=CFG_MIX_BASE)return CFG_MIX_BASE;     return CFG_INPUT_BASE;
}
static size_t subsystemGet(uint16_t id, uint8_t* out) {
  uint16_t b = baseOf(id);
  if (subOf(id,b)==SUB_SUMMARY) return subsystemSummary(id,out);
  switch(b){ case CFG_TELEM_BASE:return teGet(id,out); case CFG_FM_BASE:return fmGet(id,out);
    case CFG_GVAR_BASE:return gvGet(id,out); case CFG_SF_BASE:return sfGet(id,out);
    case CFG_LS_BASE:return lsGet(id,out); case CFG_CURVE_BASE:return cvGet(id,out);
    case CFG_MIX_BASE:return mxGet(id,out); default:return inGet(id,out); } }
static uint8_t subsystemSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len) {
  uint16_t b = baseOf(id);
  if (subOf(id,b)==SUB_SUMMARY) return NACK_UNKNOWN_FIELD;   // list rows are read-only
  switch(b){ case CFG_TELEM_BASE:return teSet(id,t,d,len); case CFG_FM_BASE:return fmSet(id,t,d,len);
    case CFG_GVAR_BASE:return gvSet(id,t,d,len); case CFG_SF_BASE:return sfSet(id,t,d,len);
    case CFG_LS_BASE:return lsSet(id,t,d,len); case CFG_CURVE_BASE:return cvSet(id,t,d,len);
    case CFG_MIX_BASE:return mxSet(id,t,d,len); default:return inSet(id,t,d,len); } }

// Returns payload length (into out, cap 4+MAX), or 0 if the field is unknown.
// ---- Set 3c: per-module RF config (g_model.moduleData[m]). Module type reuses EdgeTX's own
// enum_ModuleType table, so the option list + raw values are exactly this build's (no #if drift). ----
extern const YamlIdStr enum_ModuleType[];
static uint8_t ueModTypeCount() { uint8_t n = 0; while (enum_ModuleType[n].str) n++; return n; }
static uint8_t ueModTypeIndexOf(uint8_t type) { for (uint8_t i = 0; enum_ModuleType[i].str; i++) if ((uint8_t)enum_ModuleType[i].id == type) return i; return 0; }
static const char* s_modTypeOpts[24];
static uint8_t ueModTypeOpts() { uint8_t n = ueModTypeCount(); if (n > 24) n = 24; for (uint8_t i = 0; i < n; i++) s_modTypeOpts[i] = enum_ModuleType[i].str; return n; }
static const char* const OPTS_FAILSAFE[] = { "Not set", "Hold", "Custom", "No pulses", "Receiver" };  // FailsafeModes 0..4

static size_t modGet(uint16_t id, uint8_t* out) {
  uint8_t m = (uint8_t)((id - CFG_MOD_BASE) >> 4); if (m >= NUM_MODULES) return 0;
  ModuleData* md = &g_model.moduleData[m];
  switch (id & 0x0F) {
    case MOD_TYPE:    { uint8_t b = ueModTypeIndexOf(md->type); return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case MOD_CHANNELS:{ uint16_t v = (uint16_t)(md->channelsCount + 8); uint8_t b[2] = {(uint8_t)(v & 0xFF),(uint8_t)(v >> 8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case MOD_CHSTART: { uint16_t v = (uint16_t)md->channelsStart; uint8_t b[2] = {(uint8_t)(v & 0xFF),(uint8_t)(v >> 8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case MOD_FAILSAFE:{ uint8_t b = (uint8_t)md->failsafeMode; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    default: return 0; } }
static uint8_t modSet(uint16_t id, uint8_t t, const uint8_t* d, uint8_t len) {
  uint8_t m = (uint8_t)((id - CFG_MOD_BASE) >> 4); if (m >= NUM_MODULES) return NACK_UNKNOWN_FIELD;
  ModuleData* md = &g_model.moduleData[m];
  switch (id & 0x0F) {
    case MOD_TYPE:    { if (t != T_ENUM || len != 1) return NACK_BAD_TYPE; if (d[0] >= ueModTypeCount()) return NACK_OUT_OF_RANGE; md->type = (uint8_t)enum_ModuleType[d[0]].id; storageDirty(EE_MODEL); return 0; }
    case MOD_CHANNELS:{ if (t != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v = d[0]|(d[1]<<8); if (v < 8 || v > 16) return NACK_OUT_OF_RANGE; md->channelsCount = (int8_t)((int)v - 8); storageDirty(EE_MODEL); return 0; }
    case MOD_CHSTART: { if (t != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v = d[0]|(d[1]<<8); if (v > 24) return NACK_OUT_OF_RANGE; md->channelsStart = (uint8_t)v; storageDirty(EE_MODEL); return 0; }
    case MOD_FAILSAFE:{ if (t != T_ENUM || len != 1) return NACK_BAD_TYPE; if (d[0] > 4) return NACK_OUT_OF_RANGE; md->failsafeMode = d[0]; storageDirty(EE_MODEL); return 0; }
    default: return NACK_UNKNOWN_FIELD; } }

static size_t cfgGet(uint16_t id, uint8_t* out) {
  switch (id) {
    case CFG_MODEL_NAME: {
      // Trim trailing spaces/nulls for display.
      uint8_t n = 0;
      char tmp[LEN_MODEL_NAME];
      for (uint8_t i = 0; i < LEN_MODEL_NAME; ++i) tmp[i] = g_model.header.name[i];
      n = LEN_MODEL_NAME;
      while (n > 0 && (tmp[n-1] == ' ' || tmp[n-1] == '\0')) n--;
      return cfgValuePayload(id, T_STR, (const uint8_t*)tmp, n, out);
    }
    case CFG_TIMER1_START: {
      uint16_t v = (uint16_t)g_model.timers[0].start;   // seconds
      uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
      return cfgValuePayload(id, T_U16, b, 2, out);
    }
    case CFG_THR_REVERSED:  { uint8_t b = g_model.throttleReversed ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_THR_TRIM:      { uint8_t b = g_model.thrTrim ? 1 : 0;          return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_EXTENDED_TRIMS:{ uint8_t b = g_model.extendedTrims ? 1 : 0;    return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_TRIM_INC:      { uint8_t b = (uint8_t)(g_model.trimInc + 2);   return cfgValuePayload(id, T_ENUM, &b, 1, out); } // trimInc is -2..2 -> 0..4
    case CFG_DISPLAY_TRIMS: { uint8_t b = (uint8_t)g_model.displayTrims;    return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_TIMER1_MODE:   { uint8_t b = (uint8_t)(g_model.timers[0].mode & 0x07); if (b>5) b=0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_TIMER1_PERSIST:{ uint8_t b = (uint8_t)g_model.timers[0].persistent; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_TIMER1_MINBEEP:{ uint8_t b = g_model.timers[0].minuteBeep ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_TIMER2_MODE:   { uint8_t b = (uint8_t)(g_model.timers[1].mode & 0x07); if (b>5) b=0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_TIMER2_START:  { uint16_t v=(uint16_t)g_model.timers[1].start; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case CFG_TIMER3_MODE:   { uint8_t b = (uint8_t)(g_model.timers[2].mode & 0x07); if (b>5) b=0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_TIMER3_START:  { uint16_t v=(uint16_t)g_model.timers[2].start; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case CFG_THR_WARNING:   { uint8_t b = g_model.disableThrottleWarning ? 0 : 1; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_JITTER_FILTER: { uint8_t b = (uint8_t)g_model.jitterFilter; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    // ---- Radio (g_eeGeneral) scalars — PRD-radio-settings Set 2 (direct raw fields) ----
    case CFG_GEN_BL_MODE:    { uint8_t b = (uint8_t)g_eeGeneral.backlightMode; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_BL_BRIGHT:  { uint16_t v=(uint16_t)g_eeGeneral.backlightBright; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case CFG_GEN_KEYS_BL:    { uint8_t b = g_eeGeneral.keysBacklight ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_INACTIVITY: { uint16_t v=(uint16_t)g_eeGeneral.inactivityTimer; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case CFG_GEN_STICK_MODE: { uint8_t b = (uint8_t)g_eeGeneral.stickMode; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_TIMEZONE:   { int16_t v=(int16_t)g_eeGeneral.timezone; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)}; return cfgValuePayload(id, T_I16, b, 2, out); }
    case CFG_GEN_ADJUST_RTC: { uint8_t b = g_eeGeneral.adjustRTC ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_UNITS:      { uint8_t b = g_eeGeneral.imperial ? 1 : 0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_TRN_MODE:   { uint8_t b = (uint8_t)g_model.trainerData.mode; if (b > 9) b = 0; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_GPS_FORMAT: { uint8_t b = (uint8_t)g_eeGeneral.gpsFormat; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_USB_MODE:   { uint8_t b = (uint8_t)g_eeGeneral.USBMode; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_JACK_MODE:  { uint8_t b = (uint8_t)g_eeGeneral.jackMode; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_COUNTRY:    { uint8_t b = (uint8_t)g_eeGeneral.countryCode; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_BATT_WARN:  { uint16_t v=(uint16_t)g_eeGeneral.vBatWarn; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case CFG_GEN_SILENT_BOOT:{ uint8_t b = g_eeGeneral.dontPlayHello ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_AUDIO_MUTE: { uint8_t b = g_eeGeneral.audioMuteEnable ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_RTC_WARN:   { uint8_t b = g_eeGeneral.disableRtcWarning ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_VOLUME:     { uint16_t v=(uint16_t)(g_eeGeneral.speakerVolume + VOLUME_LEVEL_DEF); uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case CFG_GEN_PITCH:      { uint16_t v=(uint16_t)g_eeGeneral.speakerPitch; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
    case CFG_GEN_BEEP_VOL:   { int16_t v=(int16_t)g_eeGeneral.beepVolume; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)}; return cfgValuePayload(id, T_I16, b, 2, out); }
    case CFG_GEN_WAV_VOL:    { int16_t v=(int16_t)g_eeGeneral.wavVolume; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)}; return cfgValuePayload(id, T_I16, b, 2, out); }
    case CFG_GEN_BEEP_MODE:  { uint8_t b = (uint8_t)(g_eeGeneral.beepMode + 2); return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_HAPTIC_MODE:{ uint8_t b = (uint8_t)(g_eeGeneral.hapticMode + 2); return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_HAPTIC_STR: { int16_t v=(int16_t)g_eeGeneral.hapticStrength; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)}; return cfgValuePayload(id, T_I16, b, 2, out); }
    case CFG_GEN_HAPTIC_LEN: { int16_t v=(int16_t)g_eeGeneral.hapticLength; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)}; return cfgValuePayload(id, T_I16, b, 2, out); }
    case CFG_GEN_BEEP_LEN:   { int16_t v=(int16_t)g_eeGeneral.beepLength; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF)}; return cfgValuePayload(id, T_I16, b, 2, out); }
    // Hardware / connectivity / warnings — Set 3
#if !defined(COLORLCD)
    case CFG_GEN_CONTRAST:   { uint16_t v=(uint16_t)g_eeGeneral.contrast; uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; return cfgValuePayload(id, T_U16, b, 2, out); }
#endif  // COLORLCD radios have no LCD contrast setting
    case CFG_GEN_BT_MODE:    { uint8_t b = (uint8_t)g_eeGeneral.bluetoothMode; return cfgValuePayload(id, T_ENUM, &b, 1, out); }
    case CFG_GEN_FAI:        { uint8_t b = g_eeGeneral.fai ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_WARN_MEM:   { uint8_t b = g_eeGeneral.disableMemoryWarning ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_WARN_ALARM: { uint8_t b = g_eeGeneral.disableAlarmWarning ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_WARN_RSSI:  { uint8_t b = g_eeGeneral.disableRssiPoweroffAlarm ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    case CFG_GEN_ALARMS_FLASH:{ uint8_t b = g_eeGeneral.alarmsFlash ? 1 : 0; return cfgValuePayload(id, T_BOOL, &b, 1, out); }
    default:
      if (id >= CFG_SWNAME_BASE && id < CFG_SWNAME_BASE + MAX_SWITCHES) {   // Set 3b: switch custom name
        uint16_t i = id - CFG_SWNAME_BASE; if (i >= switchGetMaxSwitches()) return 0;
        return vStr(id, g_eeGeneral.switchConfig[i].name, LEN_SWITCH_NAME, out);
      }
      if (id >= CFG_MOD_BASE && id < CFG_MOD_BASE + (NUM_MODULES << 4)) return modGet(id, out);  // Set 3c
      if (id >= CFG_TRN_BASE)     return cfgGetTrainer(id, out);   // Set 5 trainer (0xC000 — above CFG_INPUT)
      if (id >= CFG_INPUT_BASE)   return subsystemGet(id, out);
      if (id >= CFG_SECROW_BASE) return sectionRowSummary(id, out);
      if (id >= CFG_TMRROW_BASE)  return timerRowSummary(id, out);
      if (id >= CFG_TMR_BASE)     return cfgGetTimer(id, out);
      if (id >= CFG_OUTROW_BASE)  return outputRowSummary(id, out);
      if (id >= CFG_OUT_BASE)     return cfgGetOutput(id, out);
      return 0;
  }
}

// Validating setter. Returns 0 on success, else a NackReason.
static uint8_t cfgSet(uint16_t id, uint8_t type, const uint8_t* data, uint8_t len) {
  switch (id) {
    case CFG_MODEL_NAME: {
      if (type != T_STR) return NACK_BAD_TYPE;
      if (len > LEN_MODEL_NAME) return NACK_BAD_LENGTH;
      for (uint8_t i = 0; i < LEN_MODEL_NAME; ++i)
        g_model.header.name[i] = (i < len) ? (char)data[i] : ' ';
      storageDirty(EE_MODEL);
      return 0;
    }
    case CFG_TIMER1_START: {
      if (type != T_U16) return NACK_BAD_TYPE;
      if (len != 2) return NACK_BAD_LENGTH;
      uint16_t v = (uint16_t)(data[0] | (data[1] << 8));
      if (v > 3600) return NACK_OUT_OF_RANGE;   // clamp to <= 1 hour for the MVP
      g_model.timers[0].start = v;
      storageDirty(EE_MODEL);
      return 0;
    }
    case CFG_THR_REVERSED:   { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_model.throttleReversed = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    case CFG_THR_TRIM:       { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_model.thrTrim = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    case CFG_EXTENDED_TRIMS: { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_model.extendedTrims = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    case CFG_TRIM_INC:       { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 4) return NACK_OUT_OF_RANGE; g_model.trimInc = (int8_t)data[0] - 2; storageDirty(EE_MODEL); return 0; }
    case CFG_DISPLAY_TRIMS:  { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; g_model.displayTrims = data[0]; storageDirty(EE_MODEL); return 0; }
    case CFG_TIMER1_MODE:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 5) return NACK_OUT_OF_RANGE; g_model.timers[0].mode = data[0]; storageDirty(EE_MODEL); return 0; }
    case CFG_TIMER1_PERSIST: { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; g_model.timers[0].persistent = data[0]; storageDirty(EE_MODEL); return 0; }
    case CFG_TIMER1_MINBEEP: { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_model.timers[0].minuteBeep = data[0] ? 1 : 0; storageDirty(EE_MODEL); return 0; }
    case CFG_TIMER2_MODE:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 5) return NACK_OUT_OF_RANGE; g_model.timers[1].mode = data[0]; storageDirty(EE_MODEL); return 0; }
    case CFG_TIMER2_START:   { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v>3600) return NACK_OUT_OF_RANGE; g_model.timers[1].start=v; storageDirty(EE_MODEL); return 0; }
    case CFG_TIMER3_MODE:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 5) return NACK_OUT_OF_RANGE; g_model.timers[2].mode = data[0]; storageDirty(EE_MODEL); return 0; }
    case CFG_TIMER3_START:   { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v>3600) return NACK_OUT_OF_RANGE; g_model.timers[2].start=v; storageDirty(EE_MODEL); return 0; }
    case CFG_THR_WARNING:    { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_model.disableThrottleWarning = data[0] ? 0 : 1; storageDirty(EE_MODEL); return 0; }
    case CFG_JITTER_FILTER:  { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; g_model.jitterFilter = data[0]; storageDirty(EE_MODEL); return 0; }
    // ---- Radio (g_eeGeneral) scalars — PRD-radio-settings Set 2. Validate, write raw, EE_GENERAL dirty. ----
    case CFG_GEN_BL_MODE:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 4) return NACK_OUT_OF_RANGE; g_eeGeneral.backlightMode = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_BL_BRIGHT:  { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v > 100) return NACK_OUT_OF_RANGE; g_eeGeneral.backlightBright = (uint8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_KEYS_BL:    { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.keysBacklight = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_INACTIVITY: { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v > 250) return NACK_OUT_OF_RANGE; g_eeGeneral.inactivityTimer = (uint8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_STICK_MODE: { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 3) return NACK_OUT_OF_RANGE; g_eeGeneral.stickMode = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_TIMEZONE:   { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v=(int16_t)(data[0]|(data[1]<<8)); if (v < -16 || v > 15) return NACK_OUT_OF_RANGE; g_eeGeneral.timezone = (int8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_ADJUST_RTC: { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.adjustRTC = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_UNITS:      { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 1) return NACK_OUT_OF_RANGE; g_eeGeneral.imperial = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_TRN_MODE:   { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 9) return NACK_OUT_OF_RANGE; g_model.trainerData.mode = data[0]; storageDirty(EE_MODEL); return 0; }
    case CFG_GEN_TRN_CALIB:  { memcpy(g_eeGeneral.trainer.calib, trainerInput, sizeof(g_eeGeneral.trainer.calib)); storageDirty(EE_GENERAL); return 0; }  // EdgeTX "Cal": snapshot live trainer input as centre
    case CFG_GEN_GPS_FORMAT: { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 1) return NACK_OUT_OF_RANGE; g_eeGeneral.gpsFormat = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_USB_MODE:   { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 3) return NACK_OUT_OF_RANGE; g_eeGeneral.USBMode = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_JACK_MODE:  { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.jackMode = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_COUNTRY:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.countryCode = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_BATT_WARN:  { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v < 40 || v > 120) return NACK_OUT_OF_RANGE; g_eeGeneral.vBatWarn = (uint8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_SILENT_BOOT:{ if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.dontPlayHello = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_AUDIO_MUTE: { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.audioMuteEnable = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_RTC_WARN:   { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.disableRtcWarning = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_VOLUME:     { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v > VOLUME_LEVEL_MAX) return NACK_OUT_OF_RANGE; g_eeGeneral.speakerVolume = (int8_t)((int)v - VOLUME_LEVEL_DEF); storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_PITCH:      { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v > 20) return NACK_OUT_OF_RANGE; g_eeGeneral.speakerPitch = (uint8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_BEEP_VOL:   { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v=(int16_t)(data[0]|(data[1]<<8)); if (v < -2 || v > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.beepVolume = (int8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_WAV_VOL:    { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v=(int16_t)(data[0]|(data[1]<<8)); if (v < -2 || v > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.wavVolume = (int8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_BEEP_MODE:  { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 3) return NACK_OUT_OF_RANGE; g_eeGeneral.beepMode = (int8_t)((int)data[0] - 2); storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_HAPTIC_MODE:{ if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 3) return NACK_OUT_OF_RANGE; g_eeGeneral.hapticMode = (int8_t)((int)data[0] - 2); storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_HAPTIC_STR: { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v=(int16_t)(data[0]|(data[1]<<8)); if (v < -2 || v > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.hapticStrength = (int8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_HAPTIC_LEN: { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v=(int16_t)(data[0]|(data[1]<<8)); if (v < -2 || v > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.hapticLength = (int8_t)v; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_BEEP_LEN:   { if (type != T_I16 || len != 2) return NACK_BAD_TYPE; int16_t v=(int16_t)(data[0]|(data[1]<<8)); if (v < -2 || v > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.beepLength = (int8_t)v; storageDirty(EE_GENERAL); return 0; }
    // Hardware / connectivity / warnings — Set 3
#if !defined(COLORLCD)
    case CFG_GEN_CONTRAST:   { if (type != T_U16 || len != 2) return NACK_BAD_TYPE; uint16_t v=data[0]|(data[1]<<8); if (v < LCD_CONTRAST_MIN || v > LCD_CONTRAST_MAX) return NACK_OUT_OF_RANGE; g_eeGeneral.contrast = (uint8_t)v; storageDirty(EE_GENERAL); return 0; }
#endif  // COLORLCD radios have no LCD contrast setting
    case CFG_GEN_BT_MODE:    { if (type != T_ENUM || len != 1) return NACK_BAD_TYPE; if (data[0] > 2) return NACK_OUT_OF_RANGE; g_eeGeneral.bluetoothMode = data[0]; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_FAI:        { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.fai = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_WARN_MEM:   { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.disableMemoryWarning = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_WARN_ALARM: { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.disableAlarmWarning = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_WARN_RSSI:  { if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.disableRssiPoweroffAlarm = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    case CFG_GEN_ALARMS_FLASH:{ if (type != T_BOOL || len != 1) return NACK_BAD_TYPE; g_eeGeneral.alarmsFlash = data[0] ? 1 : 0; storageDirty(EE_GENERAL); return 0; }
    default:
      if (id >= CFG_SWNAME_BASE && id < CFG_SWNAME_BASE + MAX_SWITCHES) {   // Set 3b: switch custom name
        uint16_t i = id - CFG_SWNAME_BASE; if (i >= switchGetMaxSwitches()) return NACK_UNKNOWN_FIELD;
        uint8_t rc = setStrField(g_eeGeneral.switchConfig[i].name, LEN_SWITCH_NAME, type, data, len);
        if (!rc) storageDirty(EE_GENERAL);
        return rc;
      }
      if (id >= CFG_MOD_BASE && id < CFG_MOD_BASE + (NUM_MODULES << 4)) return modSet(id, type, data, len);  // Set 3c
      if (id >= CFG_TRN_BASE)     return cfgSetTrainer(id, type, data, len);   // Set 5 trainer (above CFG_INPUT)
      if (id >= CFG_INPUT_BASE)   return subsystemSet(id, type, data, len);
      if (id >= CFG_SECROW_BASE) return NACK_UNKNOWN_FIELD;      // link rows are read-only
      if (id >= CFG_TMRROW_BASE)  return NACK_UNKNOWN_FIELD;
      if (id >= CFG_TMR_BASE)     return cfgSetTimer(id, type, data, len);
      if (id >= CFG_OUTROW_BASE)  return NACK_UNKNOWN_FIELD;
      if (id >= CFG_OUT_BASE)     return cfgSetOutput(id, type, data, len);
      return NACK_UNKNOWN_FIELD;
  }
}

// ---- Row model (docs/10) ---------------------------------------------------
// A page is a list of rows; each row is a FIELD (editable) or a LINK (opens a
// target page). The radio emits only currently-visible rows (server-side
// isVisible, mirroring EdgeTX colorlcd). buildRowDesc serializes one row;
// pageRow(page, idx) yields the idx-th row with its visibility.
struct RowDef {
  uint16_t id; uint8_t kind; uint8_t type; uint8_t icon;
  int32_t vmin, vmax, vstep;
  const char* label; const char* unit;
  const char* const* opts; uint8_t optCount;
  uint16_t target; bool visible; uint8_t refDomain;
};

static const char* const OPTS_TRIMINC[]     = { "Expo","ExFine","Fine","Medium","Coarse" };
static const char* const OPTS_DISPTRIM[]    = { "No","Change","Yes" };
static const char* const OPTS_TMRMODE[]      = { "OFF","ON","Strt","THs","TH%","THt" };
static const char* const OPTS_PERSIST[]     = { "OFF","Flight","Manual" };
static const char* const OPTS_FILTER[]      = { "Global","Off","On" };
static const char* const OPTS_SUBTRIMMODE[] = { "Center","Symmetric" };
static const char* const OPTS_COUNTDOWN[]   = { "Silent","Beeps","Voice","Haptic" };
static const char* const OPTS_TIMERDIR[]    = { "Remain","Elapsed" };
// Set 5 Trainer (radio trainer page): per-stick multiplex mode. Replicates EdgeTX STR_TRNMODE
// (radio/src/gui/colorlcd/radio/radio_trainer.cpp). Module mode (master/slave/jack) lives on the
// MODEL trainer page in EdgeTX and is not shown here; CFG_GEN_TRN_MODE stays defined for that future page.
static const char* const OPTS_TRNMUX[]       = { "Off","+=","Replace" };
// Radio-settings enums (PRD-radio-settings Set 2). Labels are cosmetic — the WRITTEN value is the index,
// which maps to g_eeGeneral's raw enum, so a mislabel is display-only, never corruption.
static const char* const OPTS_BLMODE[]      = { "Off","Keys","Sticks","All","On" };
static const char* const OPTS_STICKMODE[]   = { "Mode 1","Mode 2","Mode 3","Mode 4" };
static const char* const OPTS_UNITS[]       = { "Metric","Imperial" };
static const char* const OPTS_GPSFMT[]      = { "DMS","NMEA" };
static const char* const OPTS_USBMODE[]     = { "Ask","Joystick","Storage","Serial" };
static const char* const OPTS_JACKMODE[]    = { "Ask","Audio","Trainer" };
static const char* const OPTS_COUNTRY[]     = { "America","Japan","Europe" };
static const char* const OPTS_BEEPMODE[]    = { "Quiet","Alarms","No keys","All" };  // BeeperMode -2..1 → idx 0..3
static const char* const OPTS_BTMODE[]       = { "Off","Telemetry","Trainer" };        // BluetoothModes 0..2
#ifndef VOLUME_LEVEL_DEF
#define VOLUME_LEVEL_DEF 12   // universal EdgeTX default; the X7/Pocket target board.h doesn't define it
#endif
#ifndef LCD_CONTRAST_MIN
#define LCD_CONTRAST_MIN 0
#endif
#ifndef LCD_CONTRAST_MAX
#define LCD_CONTRAST_MAX 45
#endif

static void putStr(uint8_t* p, size_t& o, const char* s) {
  uint8_t n = 0; while (s[n]) n++;
  p[o++] = n; for (uint8_t i = 0; i < n; ++i) p[o++] = (uint8_t)s[i];
}
static void putI32(uint8_t* p, size_t& o, int32_t v) {
  p[o++] = v & 0xFF; p[o++] = (v >> 8) & 0xFF; p[o++] = (v >> 16) & 0xFF; p[o++] = (v >> 24) & 0xFF;
}

// Serialize one row to a FIELD_DESC payload (v2 layout, docs/10). Returns length.
static size_t buildRowDesc(uint16_t page, const RowDef& r, uint8_t* p) {
  size_t o = 0;
  p[o++] = r.id & 0xFF;  p[o++] = r.id >> 8;
  p[o++] = page & 0xFF;  p[o++] = page >> 8;
  p[o++] = r.kind; p[o++] = r.type; p[o++] = r.icon;
  putI32(p, o, r.vmin); putI32(p, o, r.vmax); putI32(p, o, r.vstep);
  putStr(p, o, r.label); putStr(p, o, r.unit ? r.unit : "");
  p[o++] = r.optCount;
  for (uint8_t i = 0; i < r.optCount; ++i) putStr(p, o, r.opts[i]);
  if (r.kind == ROW_LINK || r.kind == ROW_LINE) { p[o++] = r.target & 0xFF; p[o++] = r.target >> 8; }
  if (r.type == T_REF) p[o++] = r.refDomain;
  return o;
}

static void mkField(RowDef& r, uint16_t id, uint8_t type, int32_t mn, int32_t mx,
                    int32_t st, const char* label, const char* unit,
                    const char* const* opts, uint8_t nopt, uint8_t icon) {
  r.id=id; r.kind=ROW_FIELD; r.type=type; r.icon=icon; r.vmin=mn; r.vmax=mx; r.vstep=st;
  r.label=label; r.unit=unit; r.opts=opts; r.optCount=nopt; r.target=0; r.visible=true; r.refDomain=0;
}
static void mkRef(RowDef& r, uint16_t id, uint8_t domain, int32_t mn, int32_t mx,
                  const char* label, uint8_t icon) {
  r.id=id; r.kind=ROW_FIELD; r.type=T_REF; r.icon=icon; r.vmin=mn; r.vmax=mx; r.vstep=1;
  r.label=label; r.unit=""; r.opts=nullptr; r.optCount=0; r.target=0; r.visible=true; r.refDomain=domain;
}
static void mkLink(RowDef& r, uint16_t id, uint16_t target, const char* label, uint8_t icon) {
  r.id=id; r.kind=ROW_LINK; r.type=T_LINK; r.icon=icon; r.vmin=0; r.vmax=0; r.vstep=0;
  r.label=label; r.unit=""; r.opts=nullptr; r.optCount=0; r.target=target; r.visible=true; r.refDomain=0;
}
// Channel/input group header (docs/14): group index in vmin; the app names it.
static const uint16_t CFG_HDR_BASE = 0x0F00;   // pseudo-ids; cfgGet returns 0 (skips value phase)
static void mkHeader(RowDef& r, uint16_t group, const char* label, uint8_t icon) {
  r.id=(uint16_t)(CFG_HDR_BASE+group); r.kind=ROW_HEADER; r.type=T_U8; r.icon=icon;
  r.vmin=group; r.vmax=group; r.vstep=0; r.label=label?label:""; r.unit="";
  r.opts=nullptr; r.optCount=0; r.target=0; r.visible=true; r.refDomain=0;
}
// EdgeTX-style mix/input line (docs/14): value blob formatted app-side.
static void mkLine(RowDef& r, uint16_t id, uint16_t target, uint16_t group, uint8_t icon) {
  r.id=id; r.kind=ROW_LINE; r.type=T_LINK; r.icon=icon; r.vmin=group; r.vmax=group; r.vstep=0;
  r.label=""; r.unit=""; r.opts=nullptr; r.optCount=0; r.target=target; r.visible=true; r.refDomain=0;
}
static const char* chLabel(uint8_t ch1, char* buf) {
  int i=0; buf[i++]='C'; buf[i++]='H';
  if (ch1>=10) buf[i++]=(char)('0'+ch1/10);
  buf[i++]=(char)('0'+ch1%10); buf[i]=0; return buf;
}
// "<prefix> <n>" into buf (n already 1-based).
static const char* numLabel(const char* prefix, uint16_t n, char* buf) {
  int i=0; while (prefix[i]) { buf[i]=prefix[i]; i++; } buf[i++]=' ';
  if (n>=100){ buf[i++]=(char)('0'+n/100); n%=100; buf[i++]=(char)('0'+n/10); buf[i++]=(char)('0'+n%10); }
  else if (n>=10){ buf[i++]=(char)('0'+n/10); buf[i++]=(char)('0'+n%10); }
  else buf[i++]=(char)('0'+n);
  buf[i]=0; return buf;
}
// Build a detail-page field row for a per-item subsystem. Returns false past end.
static bool itemDetailRow(uint16_t base, uint16_t page, uint16_t detailBase, uint16_t idx, RowDef& r) {
  uint16_t i = (uint16_t)(page - detailBase);
  uint16_t b = (uint16_t)(base + i * ITEM_STRIDE);
  static const char* const OPTS_MULT_[]   = { "Add","Multiply","Replace" };
  static const char* const OPTS_CVTYPE_[]  = { "Standard","Custom" };
  static const char* const OPTS_GVPREC_[] = { "0.","0.0" };
  static const char* const OPTS_GVUNIT_[] = { "—","%" };
  if (base == CFG_INPUT_BASE) switch(idx){
    case 0: mkField(r,b|IN_NAME,  T_STR,0,LEN_EXPOMIX_NAME,1,"Name","",nullptr,0,IC_INPUTS); return true;
    case 1: mkRef(r,b|IN_SRC,   DOM_SOURCE,0,2047,"Source",IC_NONE); return true;
    case 2: mkField(r,b|IN_WEIGHT,T_I16,-100,100,1,"Weight","%",nullptr,0,IC_NONE); return true;
    case 3: mkField(r,b|IN_OFFSET,T_I16,-100,100,1,"Offset","%",nullptr,0,IC_NONE); return true;
    case 4: mkRef(r,b|IN_SWITCH,DOM_SWITCH,-300,300,"Switch",IC_NONE); return true;
    case 5: mkRef(r,b|IN_CURVE, DOM_CURVE,-32,32,"Curve",IC_CURVES); return true;
    default: return false; }
  if (base == CFG_MIX_BASE) switch(idx){
    case 0: mkField(r,b|MX_NAME,  T_STR,0,LEN_EXPOMIX_NAME,1,"Name","",nullptr,0,IC_MIXER); return true;
    case 1: mkField(r,b|MX_DESTCH,T_U16,1,OUT_CHANNELS,1,"Dest ch","",nullptr,0,IC_NONE); return true;
    case 2: mkRef(r,b|MX_SRC,   DOM_SOURCE,0,2047,"Source",IC_NONE); return true;
    case 3: mkField(r,b|MX_WEIGHT,T_I16,MIX_WEIGHT_MIN,MIX_WEIGHT_MAX,1,"Weight","%",nullptr,0,IC_NONE); return true;
    case 4: mkField(r,b|MX_OFFSET,T_I16,MIX_OFFSET_MIN,MIX_OFFSET_MAX,1,"Offset","%",nullptr,0,IC_NONE); return true;
    case 5: mkField(r,b|MX_MULT,  T_ENUM,0,2,1,"Multiplex","",OPTS_MULT_,3,IC_NONE); return true;
    case 6: mkRef(r,b|MX_SWITCH,DOM_SWITCH,-300,300,"Switch",IC_NONE); return true;
    case 7: mkRef(r,b|MX_CURVE, DOM_CURVE,-32,32,"Curve",IC_CURVES); return true;
    default: return false; }
  if (base == CFG_CURVE_BASE) {
    switch(idx){
      case 0: mkField(r,b|CV_NAME,  T_STR,0,LEN_CURVE_NAME,1,"Name","",nullptr,0,IC_CURVES); return true;
      case 1: mkField(r,b|CV_TYPE,  T_ENUM,0,1,1,"Type","",OPTS_CVTYPE_,2,IC_NONE); return true;
      case 2: mkField(r,b|CV_SMOOTH,T_BOOL,0,1,1,"Smooth","",nullptr,0,IC_NONE); return true;
      case 3: mkField(r,b|CV_POINTS,T_I16,3,17,1,"Points","",nullptr,0,IC_NONE); return true;
    }
    // Point Y-value rows (docs/13): one per point, up to the curve's point count.
    uint8_t pt = (uint8_t)(idx - 4);
    if (pt < getCurvePoints(i)) {
      static char lbl[8];  // single-threaded describe: one row built at a time
      char* p = lbl; p = appendStr(p, "Pt "); p = appendInt(p, pt+1); *p = 0;
      mkField(r, (uint16_t)(b|(CV_PT0+pt)), T_I16, -100, 100, 1, lbl, "", nullptr, 0, IC_NONE);
      return true;
    }
    return false;
  }
  if (base == CFG_LS_BASE) switch(idx){
    case 0: mkField(r,b|LS_FUNC,    T_I16,0,60,1,"Function (raw)","",nullptr,0,IC_LS); return true;
    case 1: mkField(r,b|LS_V1,      T_I16,-1024,1024,1,"V1 (raw)","",nullptr,0,IC_NONE); return true;
    case 2: mkField(r,b|LS_V2,      T_I16,-1024,1024,1,"V2 (raw)","",nullptr,0,IC_NONE); return true;
    case 3: mkField(r,b|LS_V3,      T_I16,-1024,1024,1,"V3 (raw)","",nullptr,0,IC_NONE); return true;
    case 4: mkRef(r,b|LS_AND,DOM_SWITCH,-300,300,"AND switch",IC_NONE); return true;
    case 5: mkField(r,b|LS_DELAY,   T_U16,0,250,1,"Delay","",nullptr,0,IC_NONE); return true;
    case 6: mkField(r,b|LS_DURATION,T_U16,0,250,1,"Duration","",nullptr,0,IC_NONE); return true;
    default: return false; }
  if (base == CFG_SF_BASE) {
    // EdgeTX-parity Special Functions (docs/32). Function names mirror the Functions enum
    // (dataconstants.h) with the SAME #if guards, so option index == compiled enum value.
    static const char* const OPTS_SF_FUNC[] = {
      "Override","Trainer","Inst Trim","Reset","Set Timer","Adjust GV","Volume","Set Failsafe",
      "Range Check","Bind","Play Sound","Play Track","Play Value","Lua Script","Bg Music",
      "Bg Music Pause","Vario","Haptic","SD Logs","Backlight","Screenshot","Racing Mode",
#if defined(COLORLCD) || defined(CFN_ONLY)
      "No Touch",
#endif
      "Set Main Screen","Audio Amp Off","RGB Led",
#if defined(VIDEO_SWITCH) || defined(CFN_ONLY)
      "LCD to Video",
#endif
#if defined(FUNCTION_SWITCHES) || defined(CFN_ONLY)
      "Push Switch",
#endif
      "No Keys","Test",
    };
    static_assert(sizeof(OPTS_SF_FUNC)/sizeof(OPTS_SF_FUNC[0]) == FUNC_MAX+1,
                  "OPTS_SF_FUNC out of sync with Functions enum");
    // Spot-check a few anchors so any silent reordering upstream is caught at compile time.
    static_assert(FUNC_PLAY_SOUND==10 && FUNC_HAPTIC==17 && FUNC_RESET==3, "Functions enum drift");
    static const char* const OPTS_SF_SOUND[] = {   // TR_FUNCSOUNDS (audio.h AU_SPECIAL_SOUND_*)
      "Beep1","Beep2","Beep3","Warn1","Warn2","Cheep","Ratata","Tick","Siren","Ring",
      "SciFi","Robot","Chirp","Tada","Crickt","AlmClk" };
    static const char* const OPTS_SF_RESET[]  = { "Timer1","Timer2","Timer3","Flight","Telemetry","Trims" };
    static const char* const OPTS_SF_MODULE[] = { "Internal","External" };

    uint8_t func = (uint8_t)g_model.customFn[i].func;
    SfParamKind k = sfParamKind(func);
    // Build the row order for this function: Switch, Function, [param rows], [Repeat], Enabled.
    uint8_t subs[8]; uint8_t n=0;
    subs[n++]=SF_SWITCH; subs[n++]=SF_FUNC;
    if (k==SFP_CHVAL||k==SFP_TIMERVAL||k==SFP_GVAR) { subs[n++]=SF_PARAM2; subs[n++]=SF_PARAM; }
    else if (k!=SFP_NONE) { subs[n++]=SF_PARAM; }
    if (HAS_REPEAT_PARAM(func)) subs[n++]=SF_REPEAT;
    subs[n++]=SF_ENABLE;
    if (idx>=n) return false;
    switch(subs[idx]){
      case SF_SWITCH: mkRef(r,b|SF_SWITCH,DOM_SWITCH,-300,300,"Switch",IC_SF); return true;
      case SF_FUNC:   mkField(r,b|SF_FUNC,T_ENUM,0,FUNC_MAX,1,"Function","",OPTS_SF_FUNC,(uint8_t)(FUNC_MAX+1),IC_SF); return true;
      // Repeat: EdgeTX renders -1="!1x", 0="1x", 1..60="Ns". unit "@rpt" tells the app to label it so
      // (docs/40) — otherwise a raw signed spinner reads as "negative"/"2" instead of "!1x"/"2s".
      case SF_REPEAT: mkField(r,b|SF_REPEAT,T_I16,-1,60,1,"Repeat","@rpt",nullptr,0,IC_NONE); return true;
      case SF_ENABLE: mkField(r,b|SF_ENABLE,T_BOOL,0,1,1,"Enabled","",nullptr,0,IC_NONE); return true;
      case SF_PARAM2:
        if (k==SFP_CHVAL)        mkField(r,b|SF_PARAM2,T_I16,0,MAX_OUTPUT_CHANNELS-1,1,"Channel","",nullptr,0,IC_NONE);
        else if (k==SFP_TIMERVAL)mkField(r,b|SF_PARAM2,T_I16,0,2,1,"Timer","",nullptr,0,IC_NONE);
        else if (k==SFP_GVAR)    mkField(r,b|SF_PARAM2,T_I16,0,MAX_GVARS-1,1,"Global var","",nullptr,0,IC_NONE);
        else return false;
        return true;
      case SF_PARAM:
        switch(k){
          case SFP_SOUND:  mkField(r,b|SF_PARAM,T_ENUM,0,15,1,"Sound","",OPTS_SF_SOUND,16,IC_NONE); return true;
          case SFP_SOURCE: mkRef(r,b|SF_PARAM,DOM_SOURCE,0,2047,"Source",IC_NONE); return true;
          case SFP_FILE:   mkField(r,b|SF_PARAM,T_STR,0,LEN_FUNCTION_NAME,1,"File","",nullptr,0,IC_NONE); return true;
          case SFP_HAPTIC: mkField(r,b|SF_PARAM,T_I16,0,3,1,"Strength","",nullptr,0,IC_NONE); return true;
          case SFP_SCREEN: mkField(r,b|SF_PARAM,T_I16,0,4,1,"Screen","",nullptr,0,IC_NONE); return true;
          case SFP_RESET:  mkField(r,b|SF_PARAM,T_ENUM,0,5,1,"Reset","",OPTS_SF_RESET,6,IC_NONE); return true;
          case SFP_MODULE: mkField(r,b|SF_PARAM,T_ENUM,0,1,1,"Module","",OPTS_SF_MODULE,2,IC_NONE); return true;
          case SFP_CHVAL:  mkField(r,b|SF_PARAM,T_I16,-125,125,1,"Value","",nullptr,0,IC_NONE); return true;
          case SFP_TIMERVAL: mkField(r,b|SF_PARAM,T_I16,0,32767,1,"Value","s",nullptr,0,IC_NONE); return true;
          case SFP_GVAR:   mkField(r,b|SF_PARAM,T_I16,-1024,1024,1,"Value","",nullptr,0,IC_NONE); return true;
          default: return false;
        }
    }
    return false;
  }
  if (base == CFG_GVAR_BASE) switch(idx){
    case 0: mkField(r,b|GV_NAME, T_STR,0,LEN_GVAR_NAME,1,"Name","",nullptr,0,IC_GVARS); return true;
    case 1: mkField(r,b|GV_MIN,  T_I16,-1024,1024,1,"Min","",nullptr,0,IC_NONE); return true;
    case 2: mkField(r,b|GV_MAX,  T_I16,-1024,1024,1,"Max","",nullptr,0,IC_NONE); return true;
    case 3: mkField(r,b|GV_POPUP,T_BOOL,0,1,1,"Popup","",nullptr,0,IC_NONE); return true;
    case 4: mkField(r,b|GV_PREC, T_ENUM,0,1,1,"Precision","",OPTS_GVPREC_,2,IC_NONE); return true;
    case 5: mkField(r,b|GV_UNIT, T_ENUM,0,1,1,"Unit","",OPTS_GVUNIT_,2,IC_NONE); return true;
    default: return false; }
  if (base == CFG_FM_BASE) switch(idx){
    case 0: mkField(r,b|FM_NAME,   T_STR,0,LEN_FLIGHT_MODE_NAME,1,"Name","",nullptr,0,IC_FLIGHTMODES); return true;
    case 1: mkRef(r,b|FM_SWITCH,DOM_SWITCH,-300,300,"Switch",IC_NONE); return true;
    case 2: mkField(r,b|FM_FADEIN, T_U16,0,250,1,"Fade in","",nullptr,0,IC_NONE); return true;
    case 3: mkField(r,b|FM_FADEOUT,T_U16,0,250,1,"Fade out","",nullptr,0,IC_NONE); return true;
    default: return false; }
  if (base == CFG_TELEM_BASE) switch(idx){
    case 0: mkField(r,b|TE_LABEL,T_STR,0,TELEM_LABEL_LEN,1,"Label","",nullptr,0,IC_TELEM); return true;
    case 1: mkField(r,b|TE_UNIT, T_U16,0,60,1,"Unit (raw)","",nullptr,0,IC_NONE); return true;
    case 2: mkField(r,b|TE_PREC, T_ENUM,0,2,1,"Precision","",nullptr,0,IC_NONE); return true;
    default: return false; }
  return false;
}

// ---- EdgeTX-style grouped Mixes/Inputs (docs/14) ---------------------------
// The list is a virtual sequence: a HEADER before the first line of each channel/
// input group, then a LINE per used item (items are packed + group-sorted). We map
// a linear row index onto that sequence (O(n), n small, paced).
static bool mixRowAt(uint16_t idx, RowDef& r, char* buf) {
  uint8_t used = getMixCount(); uint16_t row = 0; int prevCh = -1;
  for (uint8_t i = 0; i < used; ++i) {
    uint8_t ch = g_model.mixData[i].destCh;
    if ((int)ch != prevCh) {
      if (row == idx) { chLabel((uint8_t)(ch+1), buf); mkHeader(r, ch, buf, IC_MIXER); return true; }
      row++; prevCh = ch;
    }
    if (row == idx) { mkLine(r,(uint16_t)(CFG_MIX_BASE+i*ITEM_STRIDE+SUB_SUMMARY),
                              (uint16_t)(PAGE_MIX_BASE+i), ch, IC_MIXER); return true; }
    row++;
  }
  return false;
}
static bool inputRowAt(uint16_t idx, RowDef& r, char* buf) {
  uint8_t used = listUsedExpos(); uint16_t row = 0; int prevIn = -1;
  for (uint8_t i = 0; i < used; ++i) {
    uint8_t in = g_model.expoData[i].chn;
    if ((int)in != prevIn) {
      if (row == idx) { size_t n=strTrim(g_model.inputNames[in],LEN_INPUT_NAME,buf);
        if(!n){char*p=buf;p=appendStr(p,"In");p=appendInt(p,in+1);*p=0;} mkHeader(r,in,buf,IC_INPUTS); return true; }
      row++; prevIn = in;
    }
    if (row == idx) { mkLine(r,(uint16_t)(CFG_INPUT_BASE+i*ITEM_STRIDE+SUB_SUMMARY),
                             (uint16_t)(PAGE_INPUT_BASE+i), in, IC_INPUTS); return true; }
    row++;
  }
  return false;
}

// Fill r with the idx-th row of `page` (buf holds any generated label). Returns
// false when idx is past the last row. Sets r.visible (server-side conditional).
static bool pageRow(uint16_t page, uint16_t idx, RowDef& r, char* buf) {
  // per-item subsystem detail pages (docs/11)
  if (page >= PAGE_INPUT_BASE && page < PAGE_INPUT_BASE + MAX_EXPOS)            return itemDetailRow(CFG_INPUT_BASE, page, PAGE_INPUT_BASE, idx, r);
  if (page >= PAGE_MIX_BASE   && page < PAGE_MIX_BASE   + MAX_MIXERS)           return itemDetailRow(CFG_MIX_BASE,   page, PAGE_MIX_BASE,   idx, r);
  if (page >= PAGE_CURVE_BASE && page < PAGE_CURVE_BASE + MAX_CURVES)           return itemDetailRow(CFG_CURVE_BASE, page, PAGE_CURVE_BASE, idx, r);
  if (page >= PAGE_LS_BASE    && page < PAGE_LS_BASE    + MAX_LOGICAL_SWITCHES) return itemDetailRow(CFG_LS_BASE,    page, PAGE_LS_BASE,    idx, r);
  if (page >= PAGE_SF_BASE    && page < PAGE_SF_BASE    + MAX_SPECIAL_FUNCTIONS)return itemDetailRow(CFG_SF_BASE,    page, PAGE_SF_BASE,    idx, r);
  if (page >= PAGE_GVAR_BASE  && page < PAGE_GVAR_BASE  + MAX_GVARS)            return itemDetailRow(CFG_GVAR_BASE,  page, PAGE_GVAR_BASE,  idx, r);
  if (page >= PAGE_FM_BASE    && page < PAGE_FM_BASE    + MAX_FLIGHT_MODES)     return itemDetailRow(CFG_FM_BASE,    page, PAGE_FM_BASE,    idx, r);
  if (page >= PAGE_TELEM_BASE && page < PAGE_TELEM_BASE + MAX_TELEMETRY_SENSORS)return itemDetailRow(CFG_TELEM_BASE, page, PAGE_TELEM_BASE, idx, r);

  if (page >= PAGE_OUT_BASE && page < PAGE_OUT_BASE + OUT_CHANNELS) {
    uint8_t ch = (uint8_t)(page - PAGE_OUT_BASE);
    uint16_t b = (uint16_t)(CFG_OUT_BASE | (ch << 4));
    switch (idx) {
      case 0: mkField(r, b|OUT_NAME,       T_STR,  0, LEN_CHANNEL_NAME, 1, "Name", "", nullptr,0, IC_CHANNEL); return true;
      case 1: mkField(r, b|OUT_SUBTRIM,    T_I16, -100,100,1, "Subtrim", "", nullptr,0, IC_NONE); return true;
      case 2: mkField(r, b|OUT_MIN,        T_I16, -150,0,1,   "Min", "%", nullptr,0, IC_NONE); return true;
      case 3: mkField(r, b|OUT_MAX,        T_I16, 0,150,1,    "Max", "%", nullptr,0, IC_NONE); return true;
      case 4: mkField(r, b|OUT_REVERSE,    T_BOOL, 0,1,1,     "Inverted", "", nullptr,0, IC_NONE); return true;
      case 5: mkField(r, b|OUT_CURVE,      T_I16, -MAX_CURVES,MAX_CURVES,1, "Curve", "", nullptr,0, IC_CURVES); return true;
      case 6: mkField(r, b|OUT_PPMCENTER,  T_I16, 1000,2000,1,"PPM center", "us", nullptr,0, IC_NONE); return true;
      case 7: mkField(r, b|OUT_SUBTRIMMODE,T_ENUM, 0,1,1,     "Subtrim mode", "", OPTS_SUBTRIMMODE,2, IC_NONE); return true;
      default: return false;
    }
  }
  if (page >= PAGE_TIMER_BASE && page < PAGE_TIMER_BASE + NUM_TIMERS) {
    uint8_t k = (uint8_t)(page - PAGE_TIMER_BASE);
    uint16_t b = (uint16_t)(CFG_TMR_BASE | (k << 4));
    bool on = (g_model.timers[k].mode & 0x07) != 0;
    switch (idx) {
      case 0: mkField(r, b|TMR_NAME,     T_STR, 0, LEN_TIMER_NAME, 1, "Name", "", nullptr,0, IC_TIMERS); return true;
      case 1: mkField(r, b|TMR_MODE,     T_ENUM, 0,5,1, "Mode", "", OPTS_TMRMODE,6, IC_NONE); return true;
      case 2: mkField(r, b|TMR_START,    T_U16, 0,3600,5, "Start", "s", nullptr,0, IC_NONE); r.visible=on; return true;
      case 3: mkField(r, b|TMR_MINBEEP,  T_BOOL, 0,1,1, "Minute beep", "", nullptr,0, IC_NONE); r.visible=on; return true;
      case 4: mkField(r, b|TMR_COUNTDOWN,T_ENUM, 0,3,1, "Countdown", "", OPTS_COUNTDOWN,4, IC_NONE); r.visible=on; return true;
      case 5: mkField(r, b|TMR_PERSIST,  T_ENUM, 0,2,1, "Persistent", "", OPTS_PERSIST,3, IC_NONE); r.visible=on; return true;
      case 6: mkField(r, b|TMR_DIR,      T_ENUM, 0,1,1, "Direction", "", OPTS_TIMERDIR,2, IC_NONE); r.visible=on; return true;
      default: return false;
    }
  }
  // Set 3c — per-module RF detail: type / RX channels / channel start / failsafe (+ app-side Bind/Range).
  if (page >= PAGE_MOD_BASE && page < PAGE_MOD_BASE + NUM_MODULES) {
    uint8_t m = (uint8_t)(page - PAGE_MOD_BASE);
    uint16_t b = (uint16_t)(CFG_MOD_BASE | (m << 4));
    uint8_t nType = ueModTypeOpts();
    switch (idx) {
      case 0: mkField(r, b|MOD_TYPE,     T_ENUM, 0, nType-1, 1, "Type", "", s_modTypeOpts, nType, IC_GENERAL); return true;
      case 1: mkField(r, b|MOD_CHANNELS, T_U16, 8,16,1, "RX channels", "", nullptr,0, IC_NONE); return true;
      case 2: mkField(r, b|MOD_CHSTART,  T_U16, 0,24,1, "First channel", "", nullptr,0, IC_NONE); return true;
      case 3: mkField(r, b|MOD_FAILSAFE, T_ENUM, 0,4,1, "Failsafe", "", OPTS_FAILSAFE,5, IC_NONE); return true;
      default: return false;
    }
  }
  switch (page) {
    case PAGE_MODEL_SETUP:
      switch (idx) {
        case 0: mkField(r, CFG_MODEL_NAME, T_STR, 0, LEN_MODEL_NAME, 1, "Model name", "", nullptr,0, IC_GENERAL); return true;
        case 1: mkLink(r, CFG_TMRROW_BASE+0, PAGE_TIMER_BASE+0, "Timer 1", IC_TIMERS); return true;
        case 2: mkLink(r, CFG_TMRROW_BASE+1, PAGE_TIMER_BASE+1, "Timer 2", IC_TIMERS); return true;
        case 3: mkLink(r, CFG_TMRROW_BASE+2, PAGE_TIMER_BASE+2, "Timer 3", IC_TIMERS); return true;
        case 4: mkLink(r, CFG_SECROW_BASE+SEC_TRIMS,    PAGE_TRIMS,    "Trims", IC_TRIMS); return true;
        case 5: mkLink(r, CFG_SECROW_BASE+SEC_THROTTLE, PAGE_THROTTLE, "Throttle", IC_THROTTLE); return true;
        case 6: mkLink(r, CFG_SECROW_BASE+SEC_OTHER,    PAGE_OTHER,    "Other", IC_GENERAL); return true;
        case 7: mkLink(r, CFG_SECROW_BASE+SEC_RF_INT, PAGE_MOD_BASE+0, "Internal RF", IC_GENERAL); return true;
        case 8: mkLink(r, CFG_SECROW_BASE+SEC_RF_EXT, PAGE_MOD_BASE+1, "External RF", IC_GENERAL); return true;
        default: return false;
      }
    case PAGE_OUTPUTS:
      if (idx >= OUT_CHANNELS) return false;
      mkLink(r, (uint16_t)(CFG_OUTROW_BASE+idx), (uint16_t)(PAGE_OUT_BASE+idx),
             chLabel((uint8_t)(idx+1), buf), IC_CHANNEL);
      return true;
    case PAGE_TRIMS:
      switch (idx) {
        case 0: mkField(r, CFG_TRIM_INC,      T_ENUM, 0,4,1, "Trim increment", "", OPTS_TRIMINC,5, IC_NONE); return true;
        case 1: mkField(r, CFG_DISPLAY_TRIMS, T_ENUM, 0,2,1, "Display trims", "", OPTS_DISPTRIM,3, IC_NONE); return true;
        case 2: mkField(r, CFG_EXTENDED_TRIMS,T_BOOL, 0,1,1, "Extended trims", "", nullptr,0, IC_NONE); return true;
        default: return false;
      }
    case PAGE_THROTTLE:
      switch (idx) {
        case 0: mkField(r, CFG_THR_REVERSED, T_BOOL, 0,1,1, "Reversed", "", nullptr,0, IC_THROTTLE); return true;
        case 1: mkField(r, CFG_THR_TRIM,     T_BOOL, 0,1,1, "Trim", "", nullptr,0, IC_NONE); return true;
        case 2: mkField(r, CFG_THR_WARNING,  T_BOOL, 0,1,1, "Warning", "", nullptr,0, IC_NONE); return true;
        default: return false;
      }
    case PAGE_OTHER:
      switch (idx) {
        case 0: mkField(r, CFG_JITTER_FILTER, T_ENUM, 0,2,1, "ADC jitter filter", "", OPTS_FILTER,3, IC_NONE); return true;
        default: return false;
      }
    // Radio Setup (g_eeGeneral scalars) — PRD-radio-settings Set 2. Direct raw fields; typed writes.
    case PAGE_RADIO_SETUP:
      switch (idx) {
        case 0:  mkField(r, CFG_GEN_BL_MODE,    T_ENUM, 0,4,1, "Backlight mode", "",  OPTS_BLMODE,5,    IC_GENERAL); return true;
        case 1:  mkField(r, CFG_GEN_BL_BRIGHT,  T_U16,  0,100,5,"Brightness",     "",  nullptr,0,        IC_NONE);    return true;
        case 2:  mkField(r, CFG_GEN_KEYS_BL,    T_BOOL, 0,1,1,  "Keys backlight", "",  nullptr,0,        IC_NONE);    return true;
        case 3:  mkField(r, CFG_GEN_INACTIVITY, T_U16,  0,250,1,"Inactivity",     "min",nullptr,0,       IC_NONE);    return true;
        case 4:  mkField(r, CFG_GEN_STICK_MODE, T_ENUM, 0,3,1,  "Stick mode",     "",  OPTS_STICKMODE,4, IC_NONE);    return true;
        case 5:  mkField(r, CFG_GEN_TIMEZONE,   T_I16, -16,15,1,"Timezone",       "h", nullptr,0,        IC_NONE);    return true;
        case 6:  mkField(r, CFG_GEN_ADJUST_RTC, T_BOOL, 0,1,1,  "Adjust RTC (GPS)","", nullptr,0,        IC_NONE);    return true;
        case 7:  mkField(r, CFG_GEN_UNITS,      T_ENUM, 0,1,1,  "Units",          "",  OPTS_UNITS,2,     IC_NONE);    return true;
        case 8:  mkField(r, CFG_GEN_GPS_FORMAT, T_ENUM, 0,1,1,  "GPS format",     "",  OPTS_GPSFMT,2,    IC_NONE);    return true;
        case 9:  mkField(r, CFG_GEN_BATT_WARN,  T_U16,  40,120,1,"Battery low",   "dV",nullptr,0,        IC_NONE);    return true;
        case 10: mkField(r, CFG_GEN_USB_MODE,   T_ENUM, 0,3,1,  "USB mode",       "",  OPTS_USBMODE,4,   IC_NONE);    return true;
        case 11: mkField(r, CFG_GEN_JACK_MODE,  T_ENUM, 0,2,1,  "Jack mode",      "",  OPTS_JACKMODE,3,  IC_NONE);    return true;
        case 12: mkField(r, CFG_GEN_COUNTRY,    T_ENUM, 0,2,1,  "Country",        "",  OPTS_COUNTRY,3,   IC_NONE);    return true;
        case 13: mkField(r, CFG_GEN_AUDIO_MUTE, T_BOOL, 0,1,1,  "Auto audio mute","",  nullptr,0,        IC_NONE);    return true;
        case 14: mkField(r, CFG_GEN_SILENT_BOOT,T_BOOL, 0,1,1,  "Silent boot",    "",  nullptr,0,        IC_NONE);    return true;
        case 15: mkField(r, CFG_GEN_RTC_WARN,   T_BOOL, 0,1,1,  "Disable RTC warn","", nullptr,0,        IC_NONE);    return true;
        // Sound "feel" (Set 2b — CUST-transformed fields, converted in cfgGet/cfgSet)
        case 16: mkField(r, CFG_GEN_VOLUME,     T_U16,  0,VOLUME_LEVEL_MAX,1,"Volume","",  nullptr,0,    IC_NONE);   return true;
        case 17: mkField(r, CFG_GEN_PITCH,      T_U16,  0,20,1, "Speaker pitch",  "",  nullptr,0,        IC_NONE);   return true;
        case 18: mkField(r, CFG_GEN_BEEP_MODE,  T_ENUM, 0,3,1,  "Beeps",          "",  OPTS_BEEPMODE,4,  IC_NONE);   return true;
        case 19: mkField(r, CFG_GEN_BEEP_VOL,   T_I16, -2,2,1,  "Beep volume",    "",  nullptr,0,        IC_NONE);   return true;
        case 20: mkField(r, CFG_GEN_BEEP_LEN,   T_I16, -2,2,1,  "Beep length",    "",  nullptr,0,        IC_NONE);   return true;
        case 21: mkField(r, CFG_GEN_WAV_VOL,    T_I16, -2,2,1,  "Wav volume",     "",  nullptr,0,        IC_NONE);   return true;
        case 22: mkField(r, CFG_GEN_HAPTIC_MODE,T_ENUM, 0,3,1,  "Haptic",         "",  OPTS_BEEPMODE,4,  IC_NONE);   return true;
        case 23: mkField(r, CFG_GEN_HAPTIC_STR, T_I16, -2,2,1,  "Haptic strength","",  nullptr,0,        IC_NONE);   return true;
        case 24: mkField(r, CFG_GEN_HAPTIC_LEN, T_I16, -2,2,1,  "Haptic length",  "",  nullptr,0,        IC_NONE);   return true;
        default: return false;
      }
    // Hardware / connectivity / warnings — Set 3.
    case PAGE_RADIO_HW:
      switch (idx) {
        case 0: mkField(r, CFG_GEN_CONTRAST,    T_U16, LCD_CONTRAST_MIN,LCD_CONTRAST_MAX,1,"LCD contrast","", nullptr,0, IC_GENERAL); return true;
        case 1: mkField(r, CFG_GEN_BT_MODE,     T_ENUM, 0,2,1,  "Bluetooth",         "", OPTS_BTMODE,3,  IC_NONE); return true;
        case 2: mkField(r, CFG_GEN_ALARMS_FLASH,T_BOOL, 0,1,1,  "Alarms flash",      "", nullptr,0,      IC_NONE); return true;
        case 3: mkField(r, CFG_GEN_FAI,         T_BOOL, 0,1,1,  "FAI mode",          "", nullptr,0,      IC_NONE); return true;
        case 4: mkField(r, CFG_GEN_WARN_MEM,    T_BOOL, 0,1,1,  "Disable memory warn","",nullptr,0,      IC_NONE); return true;
        case 5: mkField(r, CFG_GEN_WARN_ALARM,  T_BOOL, 0,1,1,  "Disable alarm warn","", nullptr,0,      IC_NONE); return true;
        case 6: mkField(r, CFG_GEN_WARN_RSSI,   T_BOOL, 0,1,1,  "Disable RSSI-off warn","",nullptr,0,    IC_NONE); return true;
        default: return false;
      }
    // Switch naming (Set 3b): one editable 3-char name row per physical switch (label = default name).
    case PAGE_RADIO_SWITCHES:
      if (idx >= switchGetMaxSwitches()) return false;
      mkField(r, (uint16_t)(CFG_SWNAME_BASE + idx), T_STR, 0, LEN_SWITCH_NAME, 1,
              switchGetDefaultName(idx), "", nullptr, 0, IC_NONE);
      return true;

    case PAGE_RADIO_TRAINER: {   // Set 5: EdgeTX radio trainer page — per-stick [mode|source|weight] table
      // Replicates radio/src/gui/colorlcd/radio/radio_trainer.cpp: one row per main stick, labelled by the
      // stick's control (Ail/Ele/Thr/Rud, stick-mode aware), each carrying the multiplex mode, the source
      // trainer channel (CH1..CH4) and the student weight. Module mode (master/slave/jack) is the MODEL
      // trainer page in EdgeTX, not shown here. Calibration is an action button added app-side.
      uint8_t nSticks = adcGetMaxInputs(ADC_INPUT_MAIN); if (nSticks > 4) nSticks = 4;   // trainer.mix[4]
      if (idx >= nSticks) return false;
      uint8_t ch = inputMappingChannelOrder(idx);              // display order -> mix[] index
      uint16_t fid = (uint16_t)(CFG_TRN_BASE | (ch << 4));     // base id; app derives |TRN_{MUX,SRC,WEIGHT}
      const char* lbl = getMainControlLabel(ch);               // "Ail"/"Ele"/"Thr"/"Rud"
      // ROW_TRN: opts = multiplex modes (Off/+=/Replace); vmin..vmax = source-channel range (0..3 = CH1..CH4).
      mkField(r, fid, T_ENUM, 0, 3, 1, lbl ? lbl : "", "%", OPTS_TRNMUX, 3, IC_NONE);
      r.kind = ROW_TRN;
      return true;
    }
    // ---- per-item list pages (docs/11): one LINK row per item ----
    // Inputs/Mixes are channel/input-grouped EdgeTX-style (docs/14): interleaved
    // HEADER + LINE rows, used items only.
    case PAGE_INPUTS: return inputRowAt(idx, r, buf);
    case PAGE_MIXES:  return mixRowAt(idx, r, buf);
    case PAGE_CURVES: if (idx>=MAX_CURVES) return false;
      mkLink(r,(uint16_t)(CFG_CURVE_BASE+idx*ITEM_STRIDE+SUB_SUMMARY),(uint16_t)(PAGE_CURVE_BASE+idx),numLabel("Curve",idx+1,buf),IC_CURVES); return true;
    case PAGE_LS: if (idx>=MAX_LOGICAL_SWITCHES) return false;
      mkLink(r,(uint16_t)(CFG_LS_BASE+idx*ITEM_STRIDE+SUB_SUMMARY),(uint16_t)(PAGE_LS_BASE+idx),numLabel("LS",idx+1,buf),IC_LS); return true;
    case PAGE_SF: if (idx>=MAX_SPECIAL_FUNCTIONS) return false;
      mkLink(r,(uint16_t)(CFG_SF_BASE+idx*ITEM_STRIDE+SUB_SUMMARY),(uint16_t)(PAGE_SF_BASE+idx),numLabel("SF",idx+1,buf),IC_SF); return true;
    case PAGE_GVARS: if (idx>=MAX_GVARS) return false;
      mkLink(r,(uint16_t)(CFG_GVAR_BASE+idx*ITEM_STRIDE+SUB_SUMMARY),(uint16_t)(PAGE_GVAR_BASE+idx),numLabel("GVar",idx+1,buf),IC_GVARS); return true;
    case PAGE_FMODES: if (idx>=MAX_FLIGHT_MODES) return false;
      mkLink(r,(uint16_t)(CFG_FM_BASE+idx*ITEM_STRIDE+SUB_SUMMARY),(uint16_t)(PAGE_FM_BASE+idx),numLabel("FM",idx+1,buf),IC_FLIGHTMODES); return true;
    case PAGE_TELEM: if (idx>=MAX_TELEMETRY_SENSORS) return false;
      mkLink(r,(uint16_t)(CFG_TELEM_BASE+idx*ITEM_STRIDE+SUB_SUMMARY),(uint16_t)(PAGE_TELEM_BASE+idx),numLabel("Sensor",idx+1,buf),IC_TELEM); return true;
    default: return false;
  }
}

// A write is "structural" if it changes which rows are visible; the app then
// re-describes the page. Timer mode toggles its detail rows + summary.
static bool writeIsStructural(uint16_t id) {
  if (id >= CFG_TMR_BASE && id < CFG_TMRROW_BASE) return (uint8_t)(id & 0x0F) == TMR_MODE;
  // SF: changing the function changes which parameter rows exist -> re-describe the page.
  if (id >= CFG_SF_BASE && id < CFG_SF_BASE + MAX_SPECIAL_FUNCTIONS*ITEM_STRIDE)
    return subOf(id,CFG_SF_BASE) == SF_FUNC;
  return false;
}

// ---- ISR->main receive ring (SPSC, lock-free) ------------------------------
static const uint16_t RX_RING = 256;
static volatile uint8_t  s_rx[RX_RING];
static volatile uint16_t s_rxHead = 0;   // written by ISR (producer)
static volatile uint16_t s_rxTail = 0;   // read by main (consumer)

// ---- Negotiated sub-mode state (docs/29) -----------------------------------
// Passive by default: in Serial mode we DO NOT steal RX. We install a wrapper that
// forwards every byte to whatever consumer EdgeTX already had (CLI / Lua / telemetry
// mirror), while ALSO feeding our own decoder so it can sniff for a companion HELLO.
// Only when a CRC-valid HELLO is decoded do we go "active": stop forwarding, consume.
// On transport-down we restore the prior consumer and Serial is stock again.
// Same (C++) linkage as usbSerialSetReceiveDataCb above — NOT extern "C" — so it matches the
// definition in usbd_cdc.cpp (which lives outside that file's extern "C" block).
extern void (*usbSerialGetReceiveDataCb(void))(uint8_t*, uint32_t);
static void (*s_prevRxCb)(uint8_t*, uint32_t) = nullptr;   // stock consumer to forward to
static volatile bool s_companionActive = false;            // set true once HELLO is sniffed
// Negotiated peer capabilities (docs/protocol/PROTOCOL-V4.md §3.1), from the app's HELLO. Feature
// emit is gated on the INTERSECTION of what this build supports and what the app advertised, so a
// v3 (no-caps) app still gets the legacy wire. This build's own advertised caps:
static const uint32_t UE_LOCAL_CAPS = CAP_STRUCTURED | CAP_AUDIO | CAP_TELEM_TYPED | CAP_MODEL_FILE | CAP_CALIB;

static void emitMsg(uint8_t type, const uint8_t* payload, uint16_t len);   // fwd decl (defined below) — for ueCalTick

// ---- Calibration (PRD-radio-settings Set 4): drive EdgeTX's OWN adcCalib* routine from the phone. The
// app is a thin wizard; the cal math is EdgeTX's proven code, so stick response stays faithful. GATED on
// ATTACHED. START snapshots calib so CANCEL can restore it; STORE commits via EdgeTX. ----
static bool s_calActive = false;   // START..STORE/CANCEL: stream live analog for the wizard bars
static bool s_calMoving = false;   // after SET-MIDPOINT: track extremes each tick (adcCalibSetMinMax)
static CalibData s_calSnapshot[MAX_CALIB_ANALOG_INPUTS];
static uint8_t s_calLiveDiv = 0;

static uint8_t ueCalInputCount() {
  int n = (int)adcGetMaxInputs(ADC_INPUT_MAIN) + (int)adcGetMaxInputs(ADC_INPUT_FLEX);
  if (n > MAX_CALIB_ANALOG_INPUTS) n = MAX_CALIB_ANALOG_INPUTS;
  return (uint8_t)(n < 0 ? 0 : n);
}
static void ueCalHandle(uint8_t op) {
  switch (op) {
    case 0: memcpy(s_calSnapshot, g_eeGeneral.calib, sizeof(s_calSnapshot)); s_calActive = true; s_calMoving = false; break; // START
    case 1: adcCalibSetMidPoint(); s_calMoving = true; break;                                                                  // MIDPOINT
    case 2: s_calMoving = false; s_calActive = false; adcCalibStore(); break;                                                  // STORE
    case 3: s_calMoving = false; s_calActive = false; memcpy(g_eeGeneral.calib, s_calSnapshot, sizeof(s_calSnapshot)); break;  // CANCEL
    default: break;
  }
}
static void ueCalTick() {   // called every 10ms while the companion link is active
  if (!s_calActive) return;
  if (s_calMoving) adcCalibSetMinMax();     // EdgeTX tracks the extremes as the user moves the sticks
  if (++s_calLiveDiv >= 5) {                // ~50ms: stream live raw analog for the wizard bars
    s_calLiveDiv = 0;
    uint8_t nc = ueCalInputCount();
    uint8_t p[1 + MAX_CALIB_ANALOG_INPUTS * 2]; size_t o = 0;
    p[o++] = nc;
    for (uint8_t i = 0; i < nc; ++i) { uint16_t v = getAnalogValue(i); p[o++] = (uint8_t)(v & 0xFF); p[o++] = (uint8_t)(v >> 8); }
    emitMsg(MSG_CAL_LIVE, p, (uint16_t)o);
  }
}
static uint32_t s_peerCaps = 0;                            // what the attached app advertised
static uint8_t  s_peerProto = 0;                           // app's PROTO_VERSION
static inline bool peerHasTypedTelem() { return (s_peerCaps & CAP_TELEM_TYPED) != 0; }
// Map an EdgeTX telemetry unit to a V2 ValueKind (docs/protocol §5). Everything that isn't a known
// compound/text/datetime/cells unit is a plain scalar (VK_I32) formatted by the app from unit+prec.
static inline uint8_t ueValueKind(uint8_t unit) {
  switch (unit) {
    case UNIT_GPS: case UNIT_GPS_LATITUDE: case UNIT_GPS_LONGITUDE: return VK_GPS;
    case UNIT_TEXT:     return VK_TEXT;
    case UNIT_DATETIME: return VK_DATETIME;
    case UNIT_CELLS:    return VK_CELLS;
    default:            return VK_I32;
  }
}

// ---- V2 BULK transfer: model-file pull (docs/protocol/PROTOCOL-V4.md §8-9) --------------------
// Streams the active model /MODELS/<curr>.yml to the app as the source of truth. Chunked across ticks
// (small SD reads, paced by the class-2 byte budget) so it never blocks the main context for long or
// starves live telemetry. Whole-file CRC32 goes in BULK_END; a mismatch → the app re-pulls.
static bool     s_bulkActive = false;
static FIL      s_bulkFile;
static uint16_t s_bulkXferId = 0;
static uint32_t s_bulkOffset = 0, s_bulkTotal = 0;
static uint16_t s_bulkCrc = 0xFFFF;   // CRC16-CCITT over the whole transfer (the one checksum)
static uint8_t  s_bulkKind = 0;
static const uint16_t BULK_CHUNK = 256;              // per SD read
static const uint16_t BULK_BYTES_PER_TICK = 512;     // pacing cap (2 chunks/tick)

static void emitMsg(uint8_t type, const uint8_t* payload, uint16_t len);   // fwd decl (defined below)

static void ueStartModelPull(uint8_t which) {
  UE_CRUMB(40);   // about to touch SD (f_open) from the companion/timer-task context
  if (s_bulkActive) { f_close(&s_bulkFile); s_bulkActive = false; }   // supersede any in-flight xfer
  char path[64];
  const char* fn;
  uint8_t kind;
  // Active model filename — portable across both storage backends (EdgeTX writeModel() does the same):
  //   colorlcd → named files (g_eeGeneral.currModelFilename); else → numbered "modelNN.yml".
  char fnbuf[MODELIDX_STRLEN + sizeof(MODEL_FILENAME_SUFFIX) + 2];
  if (which == BULK_RADIO_YAML) {
    // Radio-level settings file (read-only pull for the app's Radio Settings viewer / backup, ADR
    // PRD-radio-settings). RADIO_SETTINGS_YAML_PATH = "/RADIO/radio.yml".
    strncpy(path, RADIO_SETTINGS_YAML_PATH, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    fn = "radio.yml"; kind = BULK_RADIO_YAML;
  } else {
#if defined(STORAGE_MODELSLIST)
    fn = g_eeGeneral.currModelFilename;
#else
    getModelNumberStr(g_eeGeneral.currModel, fnbuf);
    strcat(fnbuf, MODEL_FILENAME_SUFFIX);
    fn = fnbuf;
#endif
    if (!fn || !fn[0]) return;                        // no active model filename
    // A typed CONFIG_WRITE only marks the model dirty; EdgeTX writes it to SD 1-15 s later. Flush it now
    // so the file we stream reflects every edit already ACKed (Inputs Source/Switch edit → re-pull).
    storageCheck(true);
    getModelPath(path, fn);                           // "/MODELS/<file>.yml"
    kind = BULK_MODEL_YAML;
  }
  if (f_open(&s_bulkFile, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
    uint8_t ab[3] = { (uint8_t)(s_bulkXferId & 0xFF), (uint8_t)(s_bulkXferId >> 8), 1 /*open failed*/ };
    emitMsg(MSG_BULK_ABORT, ab, sizeof(ab));
    return;
  }
  UE_CRUMB(41);   // f_open OK — SD read succeeded from timer-task context
  s_bulkTotal  = (uint32_t)f_size(&s_bulkFile);
  s_bulkOffset = 0; s_bulkCrc = 0xFFFF; s_bulkKind = kind; s_bulkXferId++;
  s_bulkActive = true;
  // BULK_BEGIN {u16 xfer_id, u8 kind, u32 total_len, u8 nameLen, name}
  uint8_t p[2 + 1 + 4 + 1 + LEN_MODEL_FILENAME + 1]; size_t o = 0;
  p[o++] = s_bulkXferId & 0xFF; p[o++] = (s_bulkXferId >> 8) & 0xFF;
  p[o++] = s_bulkKind;
  p[o++] = s_bulkTotal & 0xFF; p[o++] = (s_bulkTotal>>8)&0xFF; p[o++] = (s_bulkTotal>>16)&0xFF; p[o++] = (s_bulkTotal>>24)&0xFF;
  uint8_t nl = 0; while (nl < LEN_MODEL_FILENAME && fn[nl]) nl++;
  p[o++] = nl; for (uint8_t k = 0; k < nl; ++k) p[o++] = (uint8_t)fn[k];
  emitMsg(MSG_BULK_BEGIN, p, (uint16_t)o);
}

// FL-1 (s12): stream an ARBITRARY SD file (e.g. a /LOGS/*.csv) to the app over the same paced BULK
// machinery as the model pull — reused verbatim (the tick reads whatever s_bulkFile is open). Read-only.
static void ueStartFilePull(const char* path, const char* name, uint8_t kind) {
  if (s_bulkActive) { f_close(&s_bulkFile); s_bulkActive = false; }
  if (f_open(&s_bulkFile, path, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
    uint8_t ab[3] = { (uint8_t)(s_bulkXferId & 0xFF), (uint8_t)(s_bulkXferId >> 8), 1 /*open failed*/ };
    emitMsg(MSG_BULK_ABORT, ab, sizeof(ab)); return;
  }
  s_bulkTotal = (uint32_t)f_size(&s_bulkFile);
  s_bulkOffset = 0; s_bulkCrc = 0xFFFF; s_bulkKind = kind; s_bulkXferId++; s_bulkActive = true;
  uint8_t p[2 + 1 + 4 + 1 + 64]; size_t o = 0;
  p[o++] = s_bulkXferId & 0xFF; p[o++] = (s_bulkXferId >> 8) & 0xFF;
  p[o++] = s_bulkKind;
  p[o++] = s_bulkTotal & 0xFF; p[o++] = (s_bulkTotal>>8)&0xFF; p[o++] = (s_bulkTotal>>16)&0xFF; p[o++] = (s_bulkTotal>>24)&0xFF;
  uint8_t nl = 0; while (nl < 48 && name[nl]) nl++;
  p[o++] = nl; for (uint8_t k = 0; k < nl; ++k) p[o++] = (uint8_t)name[k];
  emitMsg(MSG_BULK_BEGIN, p, (uint16_t)o);
}

// USB-interrupt context: keep it tiny — enqueue to our ring, and while we are NOT yet the
// owner of the link, also pass the bytes through to the stock consumer so plain Serial
// (CLI etc.) keeps working until a host explicitly opts in with a HELLO.
extern "C" void ultraedgeCompanionRxCb(uint8_t* buf, uint32_t len)
{
  // Pass-through to the previously-installed consumer until we've taken over.
  if (!s_companionActive && s_prevRxCb) s_prevRxCb(buf, len);

  for (uint32_t i = 0; i < len; ++i) {
    uint16_t next = (uint16_t)((s_rxHead + 1) % RX_RING);
    if (next == s_rxTail) break;          // ring full: drop (never blocks the ISR)
    s_rx[s_rxHead] = buf[i];
    s_rxHead = next;
  }
}

// ---- module state (main context) -------------------------------------------
static Decoder     s_dec;
static LinkManager s_link(ROLE_RADIO);

// ---- V2 model PUSH: phone->radio whole-file write (Phase 3, docs/PRD-phase3-model-push) --------
// The app sends an edited model YAML back as one checksummed file. We stream it to a TEMP file,
// verify the whole-file CRC16 at END, keep the previous model as .bak, then atomically rename
// temp->live and reload so the edit takes effect. Any error/mismatch -> ACK(reason), original intact.
// All writes run in main context (via onFrame in the tick), same as the pull's SD reads.
static const char* const UE_PUSH_TMP = ".uepush.tmp";
static bool     s_pushActive = false;
static FIL      s_pushFile;
static uint16_t s_pushXferId = 0;
static uint32_t s_pushOffset = 0, s_pushTotal = 0;
static uint16_t s_pushCrc = 0xFFFF, s_pushExpCrc = 0;
static char     s_pushFn[LEN_MODEL_FILENAME + 1];
static bool     s_pushIsNew = false;   // ADR-0027: restore (write a NEW model) vs active-model save

#if !defined(STORAGE_MODELSLIST)
static int ueModelIdxFromName(const char* fn);   // defined with the mono model-ops below
static int ueNextFreeModelIdx();
void loadModelHeader(uint8_t id, ModelHeader* header);   // storage.h (fwd-declared to avoid include churn)
#endif

static void ueModelPushAck(uint16_t xfer, uint8_t status) {
  uint8_t p[3] = { (uint8_t)(xfer & 0xFF), (uint8_t)(xfer >> 8), status };
  emitMsg(MSG_MODEL_PUSH_ACK, p, sizeof(p));
}
// ADR-0027: a restore's success ACK carries the final filename the radio chose, so the app can reconcile
// when the requested slot was taken and a fresh one was used: {u16 xfer, u8 status, u8 nameLen, name}.
static void ueModelPushAckNamed(uint16_t xfer, uint8_t status, const char* fn) {
  uint8_t p[4 + LEN_MODEL_FILENAME + 1];
  p[0] = (uint8_t)(xfer & 0xFF); p[1] = (uint8_t)(xfer >> 8); p[2] = status;
  uint8_t nl = 0; if (fn) while (fn[nl] && nl < LEN_MODEL_FILENAME) nl++;
  p[3] = nl; for (uint8_t i = 0; i < nl; ++i) p[4 + i] = (uint8_t)fn[i];
  emitMsg(MSG_MODEL_PUSH_ACK, p, (uint16_t)(4 + nl));
}

// ADR-0027: choose the restore target filename — the requested name if that slot is free, else the next
// free slot. Never overwrites an existing model. Writes "" to [out] if no free slot exists.
static void ueChooseRestoreTarget(const char* reqName, char* out) {
  out[0] = 0;
#if defined(STORAGE_MODELSLIST)
  char path[64]; FILINFO fno;
  getModelPath(path, reqName);
  if (f_stat(path, &fno) != FR_OK) {                        // requested slot free -> use it
    uint8_t i = 0; for (; i < LEN_MODEL_FILENAME && reqName[i]; ++i) out[i] = reqName[i]; out[i] = 0; return;
  }
  char cand[LEN_MODEL_FILENAME + 1]; strcpy(cand, MODEL_FILENAME_PATTERN);   // taken -> next free modelNN.yml
  if (findNextFileIndex(cand, LEN_MODEL_FILENAME, MODELS_PATH)) strcpy(out, cand);
#else
  int reqIdx = ueModelIdxFromName(reqName);
  int idx = (reqIdx >= 0 && !modelExists((uint8_t)reqIdx)) ? reqIdx : ueNextFreeModelIdx();
  if (idx < 0) return;
  char numstr[MODELIDX_STRLEN]; getModelNumberStr((uint8_t)idx, numstr);
  uint8_t i = 0; for (; numstr[i]; ++i) out[i] = numstr[i];
  const char* suf = MODEL_FILENAME_SUFFIX; uint8_t j = 0; for (; suf[j]; ++j) out[i + j] = suf[j]; out[i + j] = 0;
#endif
}

// Abandon an in-flight push and discard the temp file — the live model is never touched here.
static void ueModelPushDiscard() {
  if (s_pushActive) { f_close(&s_pushFile); s_pushActive = false; }
  char tmpPath[64]; getModelPath(tmpPath, UE_PUSH_TMP); f_unlink(tmpPath);
}

// Resolve the active model filename (portable across storage backends; mirrors ueStartModelPull).
static const char* ueActiveModelFn(char* fnbuf) {
#if defined(STORAGE_MODELSLIST)
  return g_eeGeneral.currModelFilename;
#else
  getModelNumberStr(g_eeGeneral.currModel, fnbuf);
  strcat(fnbuf, MODEL_FILENAME_SUFFIX);
  return fnbuf;
#endif
}

static void ueModelPushBegin(const Message& m) {
  // {u16 xfer_id, u8 kind, u32 total_len, u16 crc16, u8 nameLen, name}
  if (m.len < 10) return;
  uint16_t xfer = (uint16_t)(m.payload[0] | (m.payload[1] << 8));
  uint8_t  kind = m.payload[2];
  uint32_t total = (uint32_t)m.payload[3] | ((uint32_t)m.payload[4]<<8) | ((uint32_t)m.payload[5]<<16) | ((uint32_t)m.payload[6]<<24);
  uint16_t expcrc = (uint16_t)(m.payload[7] | (m.payload[8] << 8));
  if (!s_link.companionInputAllowed()) { ueModelPushAck(xfer, NACK_NOT_ATTACHED); return; }
  if (kind != BULK_MODEL_YAML && kind != BULK_MODEL_NEW) { ueModelPushAck(xfer, NACK_PUSH_STATE); return; }
  if (s_pushActive) ueModelPushDiscard();                                                     // supersede
  // Read the app-supplied filename (BULK_MODEL_YAML: must equal the active model; BULK_MODEL_NEW: desired slot).
  uint8_t nl = m.payload[9];
  if (nl == 0 || (uint32_t)(10 + nl) > m.len || nl > LEN_MODEL_FILENAME) { ueModelPushAck(xfer, NACK_PUSH_STATE); return; }
  char reqName[LEN_MODEL_FILENAME + 1];
  for (uint8_t i = 0; i < nl; ++i) reqName[i] = (char)m.payload[10 + i];
  reqName[nl] = 0;
  if (kind == BULK_MODEL_YAML) {
    // Active-model save (ADR-0013): refuse unless the named file is STILL the active model, so a model
    // switch on the radio between pull and save can never overwrite the wrong model. (Unchanged.)
    char fnbuf[MODELIDX_STRLEN + sizeof(MODEL_FILENAME_SUFFIX) + 2];
    const char* fn = ueActiveModelFn(fnbuf);
    if (!fn || !fn[0]) { ueModelPushAck(xfer, NACK_PUSH_STATE); return; }
    if (strlen(fn) != nl || memcmp(fn, reqName, nl) != 0) { ueModelPushAck(xfer, NACK_PUSH_MODEL); return; }
    uint8_t i = 0; for (; i < LEN_MODEL_FILENAME && fn[i]; ++i) s_pushFn[i] = fn[i];
    s_pushFn[i] = 0;
    s_pushIsNew = false;
  } else {
    // ADR-0027 restore: write a NEW model to the requested slot if free, else the next free slot.
    ueChooseRestoreTarget(reqName, s_pushFn);
    if (!s_pushFn[0]) { ueModelPushAck(xfer, NACK_PUSH_IO); return; }   // no free slot
    s_pushIsNew = true;
  }
  char tmpPath[64]; getModelPath(tmpPath, UE_PUSH_TMP);
  if (f_open(&s_pushFile, tmpPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { ueModelPushAck(xfer, NACK_PUSH_IO); return; }
  s_pushXferId = xfer; s_pushTotal = total; s_pushExpCrc = expcrc;
  s_pushOffset = 0; s_pushCrc = 0xFFFF; s_pushActive = true;
  ueModelPushAck(xfer, 0);   // BEGIN accepted; app may stream DATA
}

static void ueModelPushData(const Message& m) {
  // {u16 xfer_id, u32 offset, u16 len, bytes[len]}
  if (m.len < 8) return;
  uint16_t xfer = (uint16_t)(m.payload[0] | (m.payload[1] << 8));
  uint32_t off  = (uint32_t)m.payload[2] | ((uint32_t)m.payload[3]<<8) | ((uint32_t)m.payload[4]<<16) | ((uint32_t)m.payload[5]<<24);
  uint16_t len  = (uint16_t)(m.payload[6] | (m.payload[7] << 8));
  if (!s_pushActive || xfer != s_pushXferId) { ueModelPushAck(xfer, NACK_PUSH_STATE); return; }
  if (off != s_pushOffset || (uint32_t)(8 + len) > m.len) { ueModelPushDiscard(); ueModelPushAck(xfer, NACK_PUSH_STATE); return; }
  const uint8_t* d = m.payload + 8;
  UINT bw = 0;
  if (f_write(&s_pushFile, d, len, &bw) != FR_OK || bw != len) { ueModelPushDiscard(); ueModelPushAck(xfer, NACK_PUSH_IO); return; }
  s_pushCrc = crc16_update(s_pushCrc, d, len);
  s_pushOffset += len;
}

static void ueModelPushEnd(const Message& m) {
  // {u16 xfer_id} -> verify whole-file CRC + length, then atomic swap (keep .bak) + reload.
  if (m.len < 2) return;
  uint16_t xfer = (uint16_t)(m.payload[0] | (m.payload[1] << 8));
  if (!s_pushActive || xfer != s_pushXferId) { ueModelPushAck(xfer, NACK_PUSH_STATE); return; }
  f_close(&s_pushFile); s_pushActive = false;
  char tmpPath[64], livePath[64], bakPath[80], bakfn[LEN_MODEL_FILENAME + 8];
  getModelPath(tmpPath, UE_PUSH_TMP);
  getModelPath(livePath, s_pushFn);
  if (s_pushOffset != s_pushTotal || s_pushCrc != s_pushExpCrc) {   // corrupt/incomplete -> discard, keep original
    f_unlink(tmpPath); ueModelPushAck(xfer, NACK_PUSH_CRC); return;
  }
  if (s_pushIsNew) {
    // ADR-0027 restore: temp -> the chosen NEW file, then register WITHOUT activating (no .bak, no reload).
    if (f_rename(tmpPath, livePath) != FR_OK) { f_unlink(tmpPath); ueModelPushAck(xfer, NACK_PUSH_IO); return; }
#if defined(STORAGE_MODELSLIST)
    ModelCell* nc = modelslist.addModel(s_pushFn, false, nullptr);   // register the cell (filename only)
    if (!nc) { ueModelPushAck(xfer, NACK_PUSH_IO); return; }
    modelslabels.updateModelCell(nc);                               // read name/bitmap/labels/rf from the file
    storageDirty(EE_LABELS); storageCheck(true);                    // flush labels.yml via EdgeTX's own path
#else
    int idx = ueModelIdxFromName(s_pushFn);
    if (idx >= 0) loadModelHeader((uint8_t)idx, &modelHeaders[idx]); // refresh the model-select header cache
#endif
    ueModelPushAckNamed(xfer, 0, s_pushFn);                         // committed; tell the app the final filename
    return;
  }
  // Atomic swap: previous model -> <fn>.bak (replace old bak), temp -> live.
  uint8_t i = 0; for (; i < LEN_MODEL_FILENAME && s_pushFn[i]; ++i) bakfn[i] = s_pushFn[i];
  bakfn[i] = 0; strcat(bakfn, ".bak");
  getModelPath(bakPath, bakfn);
  f_unlink(bakPath);                 // drop previous backup (ignore if none)
  f_rename(livePath, bakPath);       // keep the current model as .bak (ignore if live missing)
  if (f_rename(tmpPath, livePath) != FR_OK) {
    f_rename(bakPath, livePath);     // swap failed -> restore original
    f_unlink(tmpPath);
    ueModelPushAck(xfer, NACK_PUSH_IO); return;
  }
  // Reload the model live so the pushed edit takes effect immediately (same path EdgeTX uses).
  readModel(s_pushFn, (uint8_t*)&g_model, sizeof(g_model), MODELS_PATH);
  postModelLoad(false);
  ueModelPushAck(xfer, 0);           // committed
}

// Throttle-idle flight-safety check for model SELECT (defined in edgetx.cpp; no public header declares it).
bool isThrottleWarningAlertNeeded();

// ---- Set 6: model radio-ops — copy / delete / select (ADR-0025) -------------------------------------
// Addressed by FILENAME (the app has these from labels.yml). colorlcd uses the modelslist API; mono maps
// "modelNN.yml" -> index. Copy/delete change the model STORE, not live control output (select is separate,
// gated, and not in this build). All return 0 on success or a NackReason. Run from the companion tick
// (main-task context, per s14) — the same context EdgeTX's own model-select screen uses.
#if defined(STORAGE_MODELSLIST)
static ModelCell* ueFindModelCell(const char* fn) {
  for (auto* c : modelslabels.getAllModels())
    if (c && strncmp(c->modelFilename, fn, LEN_MODEL_FILENAME) == 0) return c;
  return nullptr;
}
static uint8_t ueModelCopy(const char* src) {
  ModelCell* c = ueFindModelCell(src);
  if (!c) return NACK_NOT_FOUND;
  storageFlushCurrentModel(); storageCheck(true);               // persist current before touching the SD
  char dup[LEN_MODEL_FILENAME + 1]; memcpy(dup, c->modelFilename, LEN_MODEL_FILENAME); dup[LEN_MODEL_FILENAME] = 0;
  if (!findNextFileIndex(dup, LEN_MODEL_FILENAME, MODELS_PATH)) return NACK_PUSH_IO;
  // sdCopyFile returns nullptr on SUCCESS (an error string on failure) — so a non-null result is the
  // failure case. The old `== nullptr` test was inverted and NACK'd every successful duplicate (field
  // report 2026-10-10 #5: NACK reason=7 although the file copied).
  if (sdCopyFile(c->modelFilename, MODELS_PATH, dup, MODELS_PATH) != nullptr) return NACK_PUSH_IO;
  ModelCell* nc = modelslist.addModel(dup, true, c);            // new cell, copies rf/header from c
  if (!nc) return NACK_PUSH_IO;
  for (const auto& lbl : modelslabels.getLabelsByModel(c)) modelslabels.addLabelToModel(lbl, nc);  // keep labels
  storageDirty(EE_LABELS); storageCheck(true);                  // flush labels.yml NOW via EdgeTX's own path (not direct save)
  return 0;
}
static uint8_t ueModelDelete(const char* name) {
  ModelCell* c = ueFindModelCell(name);
  if (!c) return NACK_NOT_FOUND;
  if (c == modelslist.getCurrentModel()) return NACK_ACTIVE;    // never delete the model in use
  if (modelslist.removeModel(c)) return NACK_PUSH_IO;           // removeModel returns FALSE on success, true on failure
  storageDirty(EE_LABELS); storageCheck(true);                  // flush labels.yml NOW via EdgeTX's own path (not direct save)
  return 0;
}
// SELECT (switch active model, colorlcd). Native safety-gate (ADR-0025): refuse while throttle is not idle.
// Replicates the model-select screen's load sequence (model_select.cpp) but with alarms=false so loadModel
// cannot spin a blocking warning dialog from the companion tick (re-entrancy guard; throttle already checked).
static uint8_t ueModelSelect(const char* name) {
  if (isThrottleWarningAlertNeeded()) return NACK_UNSAFE;       // throttle not idle — do not switch
  ModelCell* model = ueFindModelCell(name);
  if (!model) return NACK_NOT_FOUND;
  if (model == modelslist.getCurrentModel()) return 0;          // already active — no-op
  storageFlushCurrentModel();
  storageCheck(true);
  memcpy(g_eeGeneral.currModelFilename, model->modelFilename, LEN_MODEL_FILENAME);
  MainWindow::instance()->enableWidgetRefresh(false);           // pause refresh across the g_model reset
  LayoutFactory::deleteCustomScreens();                         // torn-data guard: drop old model's screens first
  LayoutFactory::deleteTopBarWidgets();
  loadModel(g_eeGeneral.currModelFilename, false);              // alarms=false: no blocking dialog from the tick
  modelslist.setCurrentModel(model);
  LayoutFactory::loadCustomScreens();                           // rebuild for the newly-loaded model
  MainWindow::instance()->enableWidgetRefresh(true);
  storageDirty(EE_GENERAL);
  storageCheck(true);
  return 0;
}
#else
static int ueModelIdxFromName(const char* fn) {
  const char* p = fn; while (*p && (*p < '0' || *p > '9')) p++;   // "modelNN.yml" -> NN
  if (!*p) return -1;
  int v = 0; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  return (v >= 0 && v < MAX_MODELS) ? v : -1;
}
static int ueNextFreeModelIdx() { for (uint8_t i = 0; i < MAX_MODELS; i++) if (!modelExists(i)) return (int)i; return -1; }
static uint8_t ueModelCopy(const char* src) {
  int idx = ueModelIdxFromName(src); if (idx < 0 || !modelExists((uint8_t)idx)) return NACK_NOT_FOUND;
  int dst = ueNextFreeModelIdx();    if (dst < 0) return NACK_PUSH_IO;
  storageFlushCurrentModel(); storageCheck(true);
  return copyModel((uint8_t)dst, (uint8_t)idx) ? 0 : NACK_PUSH_IO;
}
static uint8_t ueModelDelete(const char* name) {
  int idx = ueModelIdxFromName(name); if (idx < 0 || !modelExists((uint8_t)idx)) return NACK_NOT_FOUND;
  if ((uint8_t)idx == g_eeGeneral.currModel) return NACK_ACTIVE;
  return deleteModel((uint8_t)idx) == 0 ? 0 : NACK_PUSH_IO;
}
// SELECT (mono). Native safety-gate; load with alarms=false (no blocking dialog from the companion tick).
static uint8_t ueModelSelect(const char* name) {
  if (isThrottleWarningAlertNeeded()) return NACK_UNSAFE;
  int idx = ueModelIdxFromName(name); if (idx < 0 || !modelExists((uint8_t)idx)) return NACK_NOT_FOUND;
  if ((uint8_t)idx == g_eeGeneral.currModel) return 0;          // already active
  storageFlushCurrentModel();
  storageCheck(true);
  g_eeGeneral.currModel = (uint8_t)idx;
  loadModel((uint8_t)idx, false);
  storageDirty(EE_GENERAL);
  storageCheck(true);
  return 0;
}
#endif
// Read a {u8 nameLen, name} payload into buf (bounded). Returns false on a bad/empty/oversized name.
static bool ueReadModelName(const Message& m, char* buf, uint8_t cap) {
  if (m.len < 1) return false;
  uint8_t nl = m.payload[0];
  if (nl == 0 || nl >= cap || (uint32_t)(1 + nl) > m.len) return false;
  for (uint8_t i = 0; i < nl; ++i) buf[i] = (char)m.payload[1 + i];
  buf[nl] = 0; return true;
}

static bool     s_inited   = false;
static bool     s_cbSet    = false;
static uint8_t  s_seq      = 0;
static uint32_t s_nowMs    = 0;       // coarse ms from per10ms ticks
static uint16_t s_keysAccepted = 0;   // observable: KEYs accepted while ATTACHED

// ---- Subscription table (class-2 streams; docs/08) -------------------------
// A stream is emitted only while subscribed, at its requested rate. Cleared on
// link loss. Keeps the pipe quiet unless the phone is actually watching.
struct Sub { bool active; uint8_t rateHz; uint16_t accumMs; };
static Sub s_subChannels = { false, 0, 0 };
static Sub s_subHome     = { false, 0, 0 };   // Home-screen telemetry (docs/13)
static Sub s_subTelem    = { false, 0, 0 };   // live telemetry sensors (docs/15)
static Sub s_subTelemRaw = { false, 0, 0 };   // raw SPort/CRSF passthrough for ecosystem Lua (docs/32)
static Sub s_subSources  = { false, 0, 0 };   // live input sources: sticks/pots/switches (docs/37)

// ---- B4b: UX handoff (single-owner UX-plane SF) ----------------------------
// The app asserts ownership of UX-plane SF output while attached (MSG_UX_OWNER present=1). When set,
// the mixer-task SF evaluator (functions.cpp, guarded by USB_COMPANION) QUEUES the UX-plane action
// here instead of actuating locally; this tick (main context) drains the queue and sends MSG_SF_FIRED
// so the phone renders it — control-plane SF (channel override, GVAR, trims, failsafe…) is untouched.
// Auto-clears if the companion link drops, so a yanked cable can never leave the radio muted.
static volatile bool s_uxOwner = false;
struct SfEvt { uint8_t idx; uint8_t func; int32_t val; uint8_t nl; char name[16]; };
static SfEvt s_sfQ[8];
static volatile uint8_t s_sfHead = 0;   // producer: mixer task
static volatile uint8_t s_sfTail = 0;   // consumer: companion tick
extern "C" bool ultraedgeUxOwnerPresent() { return s_uxOwner && s_companionActive; }
extern "C" void ultraedgeQueueSfFired(uint8_t idx, uint8_t func, int32_t val, const char* name) {
  if (!s_uxOwner || !s_companionActive) return;
  uint8_t nh = (uint8_t)((s_sfHead + 1) & 7);
  if (nh == s_sfTail) return;                 // queue full — drop (loss-tolerant)
  SfEvt& e = s_sfQ[s_sfHead];
  e.idx = idx; e.func = func; e.val = val;
  uint8_t k = 0; if (name) { while (name[k] && k < 15) { e.name[k] = name[k]; k++; } }
  e.name[k] = 0; e.nl = k;
  s_sfHead = nh;
}

// ---- P2: unified audio (docs/ARCH §4) --------------------------------------
// EdgeTX funnels EVERY sound through AudioQueue::playFile (WAVs: system prompts, number call-outs,
// SF tracks, Lua playFile) and AudioQueue::playTone (beeps). While the app owns the UX plane
// (ultraedgeUxOwnerPresent()), the audio.cpp tap diverts each of those calls here instead of the
// radio speaker, and this tick drains the queue to MSG_AUDIO with a FULLY-RESOLVED path — so the
// phone plays exactly what the radio would have, with no fragile name-guessing (which is why the
// old SF-name path is retired: SF audio now rides this same choke point).
//
// Both producers (playFile/playTone) run in the mixer/audio task; the consumer is the companion
// tick (main context). Single-producer/single-consumer ring, loss-tolerant (drop on overflow — a
// dropped beep is harmless; call-out bursts are short and the ring is sized for them).
struct AudioEvt {
  uint8_t  kind;                              // AudioKind
  uint8_t  flags;                             // EdgeTX PLAY_* low bits
  uint8_t  id;                                // prompt id (for AUDIO_FILE / AUDIO_STOP_ID)
  int8_t   freqIncr;                          // AUDIO_TONE sweep
  uint16_t freq, len, pause;                  // AUDIO_TONE params
  char     path[AUDIO_FILENAME_MAXLEN + 1];   // AUDIO_FILE resolved path
};
static const uint8_t AUDIO_Q_LEN = 16;        // power of two
static AudioEvt s_audioQ[AUDIO_Q_LEN];
static volatile uint8_t s_audHead = 0;        // producers: playFile/playTone (mixer/GUI tasks)
static volatile uint8_t s_audTail = 0;        // consumer: companion tick (main context)
static inline uint8_t audNext(uint8_t i) { return (uint8_t)((i + 1) & (AUDIO_Q_LEN - 1)); }

// Lock-free push. Unlike the SF queue (single mixer-task producer), playFile/playTone are called from
// several task contexts, so producers are not strictly serialized. We keep the push lock-free (safe to
// call from any context, incl. a possible ISR caller) and accept the only race consequence: under
// truly concurrent production one event may be overwritten/dropped. Indices are always masked to
// [0,AUDIO_Q_LEN), so there is NO out-of-bounds/corruption — just a lost beep at worst, which audio
// tolerates. The head-advance is the last write (slot filled first), so the consumer never reads a
// half-written slot.
static bool audioQPush(const AudioEvt& src) {
  if (!s_uxOwner || !s_companionActive) return false;
  uint8_t nh = audNext(s_audHead);
  if (nh == s_audTail) return false;          // full — drop (loss-tolerant)
  s_audioQ[s_audHead] = src;
  s_audHead = nh;
  return true;
}

extern "C" void ultraedgeQueueAudioFile(const char* filename, uint8_t flags, uint8_t id) {
  if (!s_uxOwner || !s_companionActive || !filename) return;
  AudioEvt e; e.kind = AUDIO_FILE; e.flags = flags; e.id = id;
  e.freqIncr = 0; e.freq = e.len = e.pause = 0;
  uint8_t k = 0; while (filename[k] && k < AUDIO_FILENAME_MAXLEN) { e.path[k] = filename[k]; k++; }
  e.path[k] = 0;
  audioQPush(e);
}
extern "C" void ultraedgeQueueAudioTone(uint16_t freq, uint16_t len, uint16_t pause,
                                        uint8_t flags, int8_t freqIncr) {
  if (!s_uxOwner || !s_companionActive) return;
  AudioEvt e; e.kind = AUDIO_TONE; e.flags = flags; e.id = 0;
  e.freqIncr = freqIncr; e.freq = freq; e.len = len; e.pause = pause; e.path[0] = 0;
  audioQPush(e);
}
extern "C" void ultraedgeQueueAudioStop(uint8_t kind, uint8_t id) {
  if (!s_uxOwner || !s_companionActive) return;
  AudioEvt e; e.kind = kind; e.flags = 0; e.id = id;
  e.freqIncr = 0; e.freq = e.len = e.pause = 0; e.path[0] = 0;
  audioQPush(e);
}

// Raw passthrough (docs/32): we register our own TelemetryQueue with EdgeTX, which then feeds it
// the SAME raw frames it feeds the built-in Lua fifo (pushTelemetryDataToQueues). We drain it and
// forward the bytes to the app as a transparent pipe; the app's Lua re-frames them. Registered
// lazily (only while a passthrough script is subscribed) so it costs nothing otherwise.
static TelemetryQueue s_telemQueue;
static bool s_telemQueueReg = false;            // actual registration state (owned by the MAIN task)
static volatile bool s_telemQueueWant = false;  // desired state, set from the companion tick
static uint8_t telemProto() {
  return (isModuleCrossfire(EXTERNAL_MODULE) || isModuleCrossfire(INTERNAL_MODULE)) ? 2 : 1;
}
// CRITICAL (ADR-0020): registerTelemetryQueue/deregisterTelemetryQueue mutate a std::list that the
// telemetry RX path (crossfire/frsky -> pushTelemetryDataToQueues) iterates WITHOUT a lock. The
// companion tick runs in the 10ms timer-service task, so mutating the list there races that iteration
// and corrupts it — fatal on colorlcd (TX16S), where a Lua telemetry widget keeps the list live
// (QX7/Pocket get away with it only because nothing else registers a queue). So the tick NEVER touches
// the list: it only records intent here, and ueCompanionMainSync() (below) performs the actual
// register/deregister from the MAIN task — the same context Lua widgets use.
static void telemQueueSet(bool on) { s_telemQueueWant = on; }
extern "C" void ueCompanionMainSync() {   // called from perMain() — MAIN task
  if (s_telemQueueWant && !s_telemQueueReg) {
    registerTelemetryQueue(&s_telemQueue); s_telemQueueReg = true;
  } else if (!s_telemQueueWant && s_telemQueueReg) {
    deregisterTelemetryQueue(&s_telemQueue); s_telemQueueReg = false; s_telemQueue.clear();
  }
}

// ---- Class-2 byte budget (docs/08 OQ-L1) -----------------------------------
// Per-tick cap on class-2 bytes so streaming can never starve control/config.
// Back to 400 B/tick (docs/39). The 0.55 raise to 1200 was counterproductive: usbSerialPutc()
// disables IRQs per byte, so more bytes/tick = more IRQ-off time = the module-telemetry UART RX is
// starved → the radio's OWN sensor values (attitude/GPS) update late → the app's horizon lagged even
// though transport is fast (E2E sim proved transport ≈ 2–4 ms). A lean all-sensors telemetry frame
// (~150 B) fits fine at 400; raw is separately bounded per tick below so it can't dominate the tick.
static const uint16_t CLASS2_BYTES_PER_TICK = 400;
static const uint16_t RAW_BYTES_PER_TICK    = 192;   // hard cap on raw passthrough per tick (docs/39)
static uint16_t s_class2Budget = 0;   // refilled each tick

static void emitBytes(const uint8_t* b, size_t n)
{
  for (size_t i = 0; i < n; ++i) usbSerialPutc(nullptr, b[i]);
}

// Build one control/reply frame and emit it. Folds the repeated
// `encodeFrame(type, s_seq++, p, len, f, sizeof(f)); emitBytes(f, n);` boilerplate that
// appeared ~10× across onFrame() — a size-only refactor (docs/30 shrink pass), behaviour
// identical. The 64-byte local is fine: all control/reply frames are small.
static void emitMsg(uint8_t type, const uint8_t* payload, uint16_t len)
{
  uint8_t f[64];
  size_t n = encodeFrame(type, s_seq++, payload, len, f, sizeof(f));
  emitBytes(f, n);
}

// Class-2 emit: only if this tick's budget allows; decrements the budget.
static bool emitClass2(const uint8_t* b, size_t n)
{
  if (n > s_class2Budget) return false;   // over budget this tick — skip (loss-tolerant)
  emitBytes(b, n);
  s_class2Budget -= (uint16_t)n;
  return true;
}

// ---- Paced describe-page generator (class-1, reliable; docs/09) -------------
// A page snapshot is many frames (Model Setup 16, Outputs 8ch×4). Emitting them
// all in one tick overruns the USB-CDC TX ring — usbSerialPutc() advances the
// write pointer WITHOUT checking free space, so a burst clobbers unsent bytes and
// the host only sees a few whole frames near the tail. (This is why the Outputs
// page rendered only its last field and Model Setup rendered none on hardware,
// while the cloud encode/decode cross-check — which never touches the FIFO —
// passed.) Fix: build one frame at a time and emit it only while the ring has
// room, spreading the snapshot across ticks.
static bool     s_descActive = false;
static uint16_t s_descPage   = 0;    // page being described (16-bit, docs/10)
static uint16_t s_descRow    = 0;    // current row index
static uint8_t  s_descPhase  = 0;    // 0 = emit FIELD_DESC, 1 = emit CONFIG_VALUE
static bool     s_descEnded  = false;// PAGE_DESC_END emitted
static uint8_t  s_descBuf[MAX_FRAME_WIRE];
static size_t   s_descLen    = 0;    // pending bytes not yet accepted by the FIFO

// Build the next describe frame into s_descBuf; returns its length and advances
// the row/phase cursor. Skips server-side-hidden rows. Returns 0 once the whole
// job (all visible rows + PAGE_DESC_END) is emitted.
static size_t buildNextDescribe() {
  for (;;) {
    RowDef r; char buf[16];
    if (!pageRow(s_descPage, s_descRow, r, buf)) {
      if (s_descEnded) return 0;
      s_descEnded = true;
      uint8_t e[2] = { (uint8_t)(s_descPage & 0xFF), (uint8_t)(s_descPage >> 8) };
      return encodeFrame(MSG_PAGE_DESC_END, s_seq++, e, 2, s_descBuf, sizeof(s_descBuf));
    }
    if (!r.visible) { s_descRow++; s_descPhase = 0; continue; }
    if (s_descPhase == 0) {
      uint8_t dp[200]; size_t dn = buildRowDesc(s_descPage, r, dp);
      s_descPhase = 1;
      return encodeFrame(MSG_FIELD_DESC, s_seq++, dp, (uint16_t)dn, s_descBuf, sizeof(s_descBuf));
    } else {
      uint8_t vp[4 + 72]; size_t vn = cfgGet(r.id, vp);
      s_descRow++; s_descPhase = 0;
      if (vn) return encodeFrame(MSG_CONFIG_VALUE, s_seq++, vp, (uint16_t)vn, s_descBuf, sizeof(s_descBuf));
      // row has no value payload — loop to next row
    }
  }
}

// ---- Home-screen telemetry (class-2 stream, docs/13) -----------------------
// Compact main-view snapshot the app renders as an EdgeTX-style home page:
// gimbal trims, stick positions, running timer values, RSSI, TX battery, flight
// mode. All in EdgeTX internal units; the APP owns every label and scale (no
// strings on the wire, consistent with the app-owns-naming principle, docs/12).
// Layout matches MSG_HOME in protocol.h.
static size_t buildHomePayload(uint8_t* p) {
  size_t o = 0;
  auto put16 = [&](int16_t v){ p[o++]=(uint8_t)(v&0xFF); p[o++]=(uint8_t)((v>>8)&0xFF); };
  auto put32 = [&](int32_t v){ p[o++]=(uint8_t)(v&0xFF); p[o++]=(uint8_t)((v>>8)&0xFF);
                               p[o++]=(uint8_t)((v>>16)&0xFF); p[o++]=(uint8_t)((v>>24)&0xFF); };

  // trims: emitted in DISPLAY-SLOT order LH, LV, RV, RH (docs/13) so the app draws
  // slot i at a fixed screen position regardless of stick mode. This mirrors the
  // colorlcd main view exactly: value at slot s = getTrimValue(fm, convertMode(s)).
  // ADC_MAIN_LH/LV/RV/RH == 0/1/2/3 are the physical slots.
  const uint8_t nTrim = 4;
  p[o++] = nTrim;
  for (uint8_t s = 0; s < nTrim; ++s)
    put16((int16_t)getTrimValue(mixerCurrentFlightMode, inputMappingConvertMode(s)));

  // sticks: 4 gimbal axes in physical slot order LH, LV, RV, RH (calibratedAnalogs).
  // The vertical axis that is the throttle gets the throttle-reversed inversion the
  // radio's own main view applies, so up == high throttle matches the radio.
  const uint8_t nStick = MAX_STICKS;
  p[o++] = nStick;
  for (uint8_t s = 0; s < nStick; ++s) {
    int16_t v = (int16_t)calibratedAnalogs[s];
    bool vert = (s == ADC_MAIN_LV || s == ADC_MAIN_RV);
    if (vert && g_model.throttleReversed &&
        inputMappingConvertMode(s) == inputMappingGetThrottle())
      v = -v;
    put16(v);
  }

  // timers: running value in seconds for each model timer.
  const uint8_t nTimer = MAX_TIMERS;
  p[o++] = nTimer;
  for (uint8_t i = 0; i < nTimer; ++i) put32((int32_t)timersStates[i].val);

  // switch positions: -1/0/1 per physical switch (app names them from CAPS).
  uint8_t nSw = (uint8_t)switchGetMaxAllSwitches();
  if (nSw > 24) nSw = 24;
  p[o++] = nSw;
  for (uint8_t i = 0; i < nSw; ++i) {
    int8_t pos = 0;
    if (SWITCH_EXISTS(i)) {
      switch (switchGetPosition(i)) {   // 0=up,1=mid,2=down -> -1/0/1
        case 0: pos = -1; break; case 1: pos = 0; break; default: pos = 1; break;
      }
    }
    p[o++] = (uint8_t)pos;
  }

  // flags / rssi / battery(0.1V) / flight-mode id / stick mode (0..3 = mode 1..4)
  uint8_t flags = 0;
  p[o++] = flags;
  p[o++] = (uint8_t)TELEMETRY_RSSI();
  p[o++] = (uint8_t)g_vbat100mV;                  // already in 0.1V units
  p[o++] = (uint8_t)mixerCurrentFlightMode;
  p[o++] = (uint8_t)g_eeGeneral.stickMode;        // informational; app draws physical slots
  return o;
}

// ---- Structural list edits (MSG_LIST_OP, docs/13) --------------------------
// Insert-before / append / delete for the variable-length model lists. We use
// EdgeTX's OWN insertMix/deleteMix (they memmove the array under mixerTaskStop),
// and mirror that exact discipline for Expos/LS/SF with a generic array shift so
// the control path is never touched mid-edit. Returns 0 or a NACK reason.
// Counts the used items in a fixed array (an item is "used" per its own validity).
static uint8_t listUsedExpos() { uint8_t n=0; for (uint8_t i=0;i<MAX_EXPOS;i++) if (EXPO_VALID(&g_model.expoData[i])) n++; return n; }

// Generic array shift helpers (item-size agnostic), under mixer stop.
static void arrInsert(void* base, size_t itemSz, uint8_t cap, uint8_t idx) {
  uint8_t* p = (uint8_t*)base + (size_t)idx*itemSz;
  memmove(p+itemSz, p, (size_t)(cap-(idx+1))*itemSz);
  memclear(p, itemSz);
}
static void arrDelete(void* base, size_t itemSz, uint8_t cap, uint8_t idx) {
  uint8_t* p = (uint8_t*)base + (size_t)idx*itemSz;
  memmove(p, p+itemSz, (size_t)(cap-(idx+1))*itemSz);
  memclear((uint8_t*)base + (size_t)(cap-1)*itemSz, itemSz);
}

static uint8_t doListOp(uint16_t base, uint16_t index, uint8_t op, uint16_t arg) {
  switch (base) {
    case CFG_MIX_BASE: {
      uint8_t used = getMixCount();
      if (op == LIST_DELETE) { if (index>=used) return NACK_OUT_OF_RANGE; deleteMix((uint8_t)index); return 0; }
      if (used >= MAX_MIXERS) return NACK_OUT_OF_RANGE;
      // BUG-004 (structural, mixes): "＋ New mix" (APPEND) → a NEW channel (next after the last),
      // consistent with "New input". Per-line "Insert after/before" still keep the ref line's channel
      // (add a line within that channel, which mixes legitimately support).
      uint8_t at, ch;
      if (op == LIST_APPEND) {
        at = used;
        ch = used ? (uint8_t)(g_model.mixData[used-1].destCh + 1) : 0;
        if (ch >= MAX_OUTPUT_CHANNELS) ch = (uint8_t)(MAX_OUTPUT_CHANNELS - 1);
      }
      else {
        if (index >= used && used) return NACK_OUT_OF_RANGE;
        uint8_t ref = (used ? (index < used ? (uint8_t)index : (uint8_t)(used-1)) : 0);
        ch = used ? g_model.mixData[ref].destCh : 0;
        at = (op == LIST_INSERT_AFTER) ? (uint8_t)(ref + 1) : ref;   // after -> ref+1
      }
      if (at > MAX_MIXERS-1) at = MAX_MIXERS-1;
      insertMix(at, ch); return 0;
    }
    case CFG_INPUT_BASE: {
      uint8_t used = listUsedExpos();
      if (op == LIST_DELETE) {
        if (index>=used) return NACK_OUT_OF_RANGE;
        mixerTaskStop(); arrDelete(g_model.expoData, sizeof(ExpoData), MAX_EXPOS, (uint8_t)index); mixerTaskStart();
        storageDirty(EE_MODEL); return 0;
      }
      if (used >= MAX_EXPOS) return NACK_OUT_OF_RANGE;
      // insert on the SAME input channel (chn) as the reference line.
      uint8_t at, chn;
      // BUG-004: "＋ New input" (APPEND) must create a NEW input — the NEXT channel after the last —
      // not another expo line on the last input (that produced IN6 nesting under IN5). INSERT/
      // INSERT_AFTER (from a line's menu) still keep the ref line's chn = add a line within that input.
      if (op == LIST_APPEND) {
        at = used;
        chn = used ? (uint8_t)(g_model.expoData[used-1].chn + 1) : 0;
        if (chn >= MAX_INPUTS) chn = (uint8_t)(MAX_INPUTS - 1);
      }
      else {
        if (index >= used && used) return NACK_OUT_OF_RANGE;
        uint8_t ref = (used ? (index < used ? (uint8_t)index : (uint8_t)(used-1)) : 0);
        chn = used ? g_model.expoData[ref].chn : 0;
        at = (op == LIST_INSERT_AFTER) ? (uint8_t)(ref + 1) : ref;
      }
      if (at > MAX_EXPOS-1) at = MAX_EXPOS-1;
      mixerTaskStop();
      arrInsert(g_model.expoData, sizeof(ExpoData), MAX_EXPOS, at);
      ExpoData* e = &g_model.expoData[at];
      e->srcRaw = MIXSRC_FIRST_STICK;     // sane default; user picks Source next
      e->weight = 100; e->mode = 3;       // pos+neg, 100%
      e->chn = chn;
      mixerTaskStart(); storageDirty(EE_MODEL); return 0;
    }
    case CFG_LS_BASE: {
      if (index>=MAX_LOGICAL_SWITCHES) return NACK_OUT_OF_RANGE;
      if (op == LIST_DELETE) {
        memclear(&g_model.logicalSw[index], sizeof(LogicalSwitchData));
        storageDirty(EE_MODEL); return 0;
      }
      // LS/SF are FIXED-size lists (no insert/append) — clearing is the "delete".
      return NACK_BAD_TYPE;
    }
    case CFG_SF_BASE: {
      if (index>=MAX_SPECIAL_FUNCTIONS) return NACK_OUT_OF_RANGE;
      if (op == LIST_DELETE) {
        memclear(&g_model.customFn[index], sizeof(CustomFunctionData));
        storageDirty(EE_MODEL); return 0;
      }
      return NACK_BAD_TYPE;
    }
    default: return NACK_UNKNOWN_FIELD;
  }
}

// Decoder callback: one decoded, CRC-valid frame.
static void onFrame(const Message& m, void*)
{
  UE_CRUMB(30); UE_CRUMB_MSG(m.type);   // dispatching a decoded frame (msg type recorded)
  // ANY frame that reaches here passed HDLC framing + CRC16 + proto-version — i.e. it is a
  // genuine companion frame (stock CLI / telemetry chatter never decodes as one). So a
  // decoded frame is proof a companion is talking: take over the Serial RX and keep it taken
  // over. Latching on every valid frame (not just HELLO) means a transient reset can't leave
  // us permanently silent — the next heartbeat re-arms emission. (docs/29 negotiation intent
  // preserved: takeover still only happens once real companion traffic appears.)
  s_companionActive = true;

  // Feed link state first (handshake/heartbeat tracking + re-attach on resumed traffic).
  s_link.onMessage(m, s_nowMs);

  uint8_t f[MAX_FRAME_WIRE]; size_t n;

  if (m.type == MSG_HELLO) {
    // Capture the app's advertised capabilities (payload: proto_ver(1), fw_ver(4), role(1), caps(4)).
    if (m.len >= 10) {
      s_peerProto = m.payload[0];
      s_peerCaps  = (uint32_t)m.payload[6] | ((uint32_t)m.payload[7]<<8)
                  | ((uint32_t)m.payload[8]<<16) | ((uint32_t)m.payload[9]<<24);
    }
    // Respond to a host HELLO with HELLO_ACK, advertising THIS build's caps.
    Hello h; h.proto_ver = PROTO_VERSION; h.fw_ver = 0x00040000;
    h.role = ROLE_RADIO; h.caps = UE_LOCAL_CAPS;
    n = encodeHello(s_seq++, h, /*ack=*/true, f, sizeof(f));
    emitBytes(f, n);
  }
  else if (m.type == MSG_KEY) {
    // GATING: act only when ATTACHED. Otherwise ignore (no reply).
    if (s_link.companionInputAllowed()) {
      s_keysAccepted++;
      // ACK carries the running accepted-count in the payload for visibility.
      uint8_t p[3] = { m.seq, (uint8_t)(s_keysAccepted & 0xFF),
                       (uint8_t)(s_keysAccepted >> 8) };
      emitMsg(MSG_ACK, p, sizeof(p));
    }
    // else: DETACHED/HANDSHAKING -> input inert, dropped silently.
  }
  else if (m.type == MSG_CONFIG_READ) {
    // Reads are allowed regardless of link state (no side effects).
    if (m.len >= 2) {
      uint16_t id = (uint16_t)(m.payload[0] | (m.payload[1] << 8));
      uint8_t vp[4 + 64];
      size_t vn = cfgGet(id, vp);
      if (vn > 0) { n = encodeFrame(MSG_CONFIG_VALUE, s_seq++, vp, (uint16_t)vn, f, sizeof(f)); emitBytes(f, n); }
      else {
        uint8_t nk[2] = { m.seq, NACK_UNKNOWN_FIELD };
        emitMsg(MSG_NACK, nk, 2);
      }
    }
  }
  else if (m.type == MSG_CONFIG_WRITE) {
    // GATING: writes only when ATTACHED.
    uint8_t reason;
    if (!s_link.companionInputAllowed()) reason = NACK_NOT_ATTACHED;
    else if (m.len < 4) reason = NACK_BAD_LENGTH;
    else {
      uint16_t id = (uint16_t)(m.payload[0] | (m.payload[1] << 8));
      uint8_t type = m.payload[2], len = m.payload[3];
      if ((size_t)(4 + len) > m.len) reason = NACK_BAD_LENGTH;
      else reason = cfgSet(id, type, &m.payload[4], len);
    }
    if (reason == 0) {
      uint16_t wid = (m.len >= 2) ? (uint16_t)(m.payload[0] | (m.payload[1] << 8)) : 0;
      uint8_t a[2] = { m.seq, (uint8_t)(writeIsStructural(wid) ? ACK_RESTRUCTURE : 0) };
      emitMsg(MSG_ACK, a, 2);
    } else {
      uint8_t nk[2] = { m.seq, reason };
      emitMsg(MSG_NACK, nk, 2);
    }
  }
  else if (m.type == MSG_CAL) {
    // Calibration (Set 4): drive EdgeTX's own adcCalib* via {u8 op}. GATED on ATTACHED (writes g_eeGeneral).
    if (s_link.companionInputAllowed() && m.len >= 1) ueCalHandle(m.payload[0]);
  }
  else if (m.type == MSG_MOD_ACTION) {
    // Set 3c: bind / range / stop on a module via EdgeTX's own setModuleMode. GATED on ATTACHED.
    if (s_link.companionInputAllowed() && m.len >= 2) {
      uint8_t mod = m.payload[0], act = m.payload[1];
      if (mod < NUM_MODULES) {
        ModuleSettingsMode md = (act == 1) ? MODULE_MODE_BIND
                              : (act == 2) ? MODULE_MODE_RANGECHECK : MODULE_MODE_NORMAL;
        setModuleMode(mod, md);
      }
    }
  }
  else if (m.type == MSG_LIST_OP) {
    // Structural list edit (docs/13). GATED on ATTACHED, like writes.
    uint8_t reason;
    if (!s_link.companionInputAllowed()) reason = NACK_NOT_ATTACHED;
    else if (m.len < 7) reason = NACK_BAD_LENGTH;
    else {
      uint16_t base  = (uint16_t)(m.payload[0] | (m.payload[1] << 8));
      uint16_t index = (uint16_t)(m.payload[2] | (m.payload[3] << 8));
      uint8_t  op    = m.payload[4];
      uint16_t arg   = (uint16_t)(m.payload[5] | (m.payload[6] << 8));
      reason = doListOp(base, index, op, arg);
    }
    if (reason == 0) {
      uint8_t a[2] = { m.seq, ACK_RESTRUCTURE };   // always re-describe after a list edit
      emitMsg(MSG_ACK, a, 2);
    } else {
      uint8_t nk[2] = { m.seq, reason };
      emitMsg(MSG_NACK, nk, 2);
    }
  }
  else if (m.type == MSG_SUB_SET) {
    // {stream_id, rate_hz} — subscribe to a class-2 stream.
    if (m.len >= 2) {
      uint8_t stream = m.payload[0], rate = m.payload[1];
      if (rate == 0) rate = 1; if (rate > 50) rate = 50;   // clamp
      if (stream == STREAM_CHANNELS) { s_subChannels.active = true; s_subChannels.rateHz = rate; s_subChannels.accumMs = 0; }
      else if (stream == STREAM_HOME) { s_subHome.active = true; s_subHome.rateHz = rate; s_subHome.accumMs = 0; }
      else if (stream == STREAM_TELEMETRY) { s_subTelem.active = true; s_subTelem.rateHz = rate; s_subTelem.accumMs = 0; }
      else if (stream == STREAM_TELEM_RAW) { s_subTelemRaw.active = true; s_subTelemRaw.rateHz = rate; s_subTelemRaw.accumMs = 0; telemQueueSet(true); }
      else if (stream == STREAM_SOURCES) { s_subSources.active = true; s_subSources.rateHz = rate; s_subSources.accumMs = 0; }
      uint8_t a[1] = { m.seq }; emitMsg(MSG_ACK, a, 1);
    }
  }
  else if (m.type == MSG_SUB_CLEAR) {
    if (m.len >= 1) {
      uint8_t stream = m.payload[0];
      if (stream == STREAM_CHANNELS) s_subChannels.active = false;
      else if (stream == STREAM_HOME) s_subHome.active = false;
      else if (stream == STREAM_TELEMETRY) s_subTelem.active = false;
      else if (stream == STREAM_TELEM_RAW) { s_subTelemRaw.active = false; telemQueueSet(false); }
      else if (stream == STREAM_SOURCES) s_subSources.active = false;
      uint8_t a[1] = { m.seq }; emitMsg(MSG_ACK, a, 1);
    }
  }
  else if (m.type == MSG_UX_OWNER) {
    // B4b: the app claims/relinquishes UX-plane SF ownership. present=1 → radio mutes UX-plane SF and
    // routes them to the phone via MSG_SF_FIRED; present=0 → radio owns UX as normal. (docs/37)
    if (m.len >= 1) { s_uxOwner = (m.payload[0] != 0); uint8_t a[1] = { m.seq }; emitMsg(MSG_ACK, a, 1); }
  }
  else if (m.type == MSG_TELEM_PUSH) {
    // Inject a passthrough frame toward the module (app Lua crossfireTelemetryPush). ADR-0021: enables
    // interactive CRSF tools (ELRS) running in the app's Lua engine to configure the link. Payload is
    // {u8 proto, u8 len, bytes[len]}; proto 2 = CRSF with bytes = [command, payload...]. We replicate
    // EdgeTX's luaCrossfireTelemetryPush (api_general.cpp): frame into outputTelemetryBuffer + CRC(s) +
    // destination. ACK byte = 1 if queued (push() returns true), else 0 (buffer busy / no CRSF module).
    uint8_t ok = 0;
    if (m.len >= 2) {
      uint8_t proto = m.payload[0];
      uint8_t n = m.payload[1];
      const uint8_t* d = (m.len >= (uint16_t)(2 + n)) ? &m.payload[2] : nullptr;
      if (proto == 2 && d && n >= 1) {
        bool internal = isModuleCrossfire(INTERNAL_MODULE);
        bool crsf = internal || isModuleCrossfire(EXTERNAL_MODULE);
        if (crsf && outputTelemetryBuffer.isAvailable() && n <= TELEMETRY_OUTPUT_BUFFER_SIZE) {
          uint8_t command = d[0];
          uint8_t length  = (uint8_t)(n - 1);   // payload bytes after the command
          outputTelemetryBuffer.pushByte(MODULE_ADDRESS);
          outputTelemetryBuffer.pushByte((uint8_t)((command == COMMAND_ID ? 3 : 2) + length));
          outputTelemetryBuffer.pushByte(command);
          for (uint8_t i = 0; i < length; i++) outputTelemetryBuffer.pushByte(d[1 + i]);
          if (command == COMMAND_ID) {
            outputTelemetryBuffer.pushByte(crc8_BA(outputTelemetryBuffer.data + 2, 1 + length));
            outputTelemetryBuffer.pushByte(crc8(outputTelemetryBuffer.data + 2, 2 + length));
          } else {
            outputTelemetryBuffer.pushByte(crc8(outputTelemetryBuffer.data + 2, 1 + length));
          }
          outputTelemetryBuffer.setDestination(internal ? 0 : TELEMETRY_ENDPOINT_SPORT);
          ok = 1;
        }
      }
    }
    uint8_t a[2] = { m.seq, ok };
    emitMsg(MSG_ACK, a, 2);
  }
  else if (m.type == MSG_GET_PAGES) {
    // Top-level pages: {u8 count, [u16 id, u8 icon, str title]} (docs/10,11).
    struct Pg { uint16_t id; uint8_t icon; const char* title; };
    static const Pg PAGES[] = {
      { PAGE_MODEL_SETUP, IC_SETUP,       "Model" },
      { PAGE_INPUTS,      IC_INPUTS,      "Inputs" },
      { PAGE_MIXES,       IC_MIXER,       "Mixes" },
      { PAGE_OUTPUTS,     IC_OUTPUTS,     "Outputs" },
      { PAGE_CURVES,      IC_CURVES,      "Curves" },
      { PAGE_LS,          IC_LS,          "Logic" },
      { PAGE_SF,          IC_SF,          "Funcs" },
      { PAGE_GVARS,       IC_GVARS,       "GVars" },
      { PAGE_FMODES,      IC_FLIGHTMODES, "Modes" },
      { PAGE_TELEM,       IC_TELEM,       "Telem" },
    };
    uint8_t p[256]; size_t o = 0;
    p[o++] = (uint8_t)(sizeof(PAGES)/sizeof(PAGES[0]));
    for (const Pg& pg : PAGES) { p[o++]=pg.id&0xFF; p[o++]=pg.id>>8; p[o++]=pg.icon; putStr(p, o, pg.title); }
    n = encodeFrame(MSG_CONFIG_PAGES, s_seq++, p, (uint16_t)o, f, sizeof(f)); emitBytes(f, n);
  }
  else if (m.type == MSG_GET_CAPS) {
    // Value-space anchors (docs/12). The APP names sources/switches/curves from
    // these — no strings on the wire, so a future stringless MCU works unchanged.
    static const struct { uint8_t key; int16_t val; } CAPS_TAB[] = {
      { CAP_SRC_NONE, MIXSRC_NONE },
      { CAP_SRC_INPUT, MIXSRC_FIRST_INPUT }, { CAP_N_INPUT, MAX_INPUTS },
      { CAP_SRC_STICK, MIXSRC_FIRST_STICK }, { CAP_N_STICK, MAX_STICKS },
      { CAP_SRC_POT, MIXSRC_FIRST_POT },     { CAP_N_POT, MAX_POTS },
      { CAP_SRC_MAX, MIXSRC_MAX },
      { CAP_SRC_TRIM, MIXSRC_FIRST_TRIM },   { CAP_N_TRIM, MAX_TRIMS },
      { CAP_SRC_SW, MIXSRC_FIRST_SWITCH },   { CAP_N_SW, MAX_SWITCHES },
      { CAP_SRC_LS, MIXSRC_FIRST_LOGICAL_SWITCH }, { CAP_N_LS, MAX_LOGICAL_SWITCHES },
      { CAP_SRC_CH, MIXSRC_FIRST_CH },       { CAP_N_CH, MAX_OUTPUT_CHANNELS },
      { CAP_SRC_GV, MIXSRC_FIRST_GVAR },     { CAP_N_GV, MAX_GVARS },
      { CAP_SRC_TELE, MIXSRC_FIRST_TELEM },  { CAP_N_TELE, MAX_TELEMETRY_SENSORS },
      { CAP_SRC_LAST, MIXSRC_LAST },
      { CAP_SW_SW, SWSRC_FIRST_SWITCH },     { CAP_SW_TRIM, SWSRC_FIRST_TRIM },
      { CAP_SW_LS, SWSRC_FIRST_LOGICAL_SWITCH }, { CAP_SW_ON, SWSRC_ON },
      { CAP_SW_LAST, SWSRC_LAST },
      { CAP_N_CURVE, MAX_CURVES },
    };
    uint8_t p[160]; size_t o = 1;
    uint8_t cnt = (uint8_t)(sizeof(CAPS_TAB)/sizeof(CAPS_TAB[0]));
    for (uint8_t i = 0; i < cnt; ++i) {
      p[o++] = CAPS_TAB[i].key;
      p[o++] = (uint8_t)(CAPS_TAB[i].val & 0xFF);
      p[o++] = (uint8_t)((CAPS_TAB[i].val >> 8) & 0xFF);
    }
    // Dynamic cap: passthrough protocol on the active module (docs/32) — runtime, so appended here.
    { int16_t tpv = (int16_t)telemProto(); p[o++]=CAP_TELEM_PROTO; p[o++]=(uint8_t)(tpv&0xFF); p[o++]=(uint8_t)((tpv>>8)&0xFF); cnt++; }
    p[0] = cnt;
    n = encodeFrame(MSG_CAPS, s_seq++, p, (uint16_t)o, f, sizeof(f)); emitBytes(f, n);
  }
  else if (m.type == MSG_GET_TELECAT) {
    // Telemetry sensor catalog (docs/15): {u8 count, [u16 id, u8 unit, u8 prec, str name]}.
    // The APP formats values + owns the unit->string table (no unit strings on the wire).
    // V2 (CAP_TELEM_TYPED): insert value_kind after id so the app knows each sensor's shape.
    bool typed = peerHasTypedTelem();
    uint8_t p[256]; size_t o = 1; uint8_t cnt = 0;
    for (uint8_t i = 0; i < MAX_TELEMETRY_SENSORS; ++i) {
      TelemetrySensor& s = g_model.telemetrySensors[i];
      if (!s.isAvailable()) continue;
      if (o + 6 + TELEM_LABEL_LEN > sizeof(p)) break;
      p[o++] = i & 0xFF; p[o++] = i >> 8;
      if (typed) p[o++] = ueValueKind((uint8_t)s.unit);
      p[o++] = (uint8_t)s.unit; p[o++] = (uint8_t)s.prec;
      char tmp[TELEM_LABEL_LEN+1]; size_t ln = strTrim(s.label, TELEM_LABEL_LEN, tmp);
      p[o++] = (uint8_t)ln; for (size_t k = 0; k < ln; ++k) p[o++] = (uint8_t)tmp[k];
      cnt++;
    }
    p[0] = cnt;
    n = encodeFrame(MSG_TELECAT, s_seq++, p, (uint16_t)o, f, sizeof(f)); emitBytes(f, n);
  }
  else if (m.type == MSG_TELE_DISCOVER) {
    // Bug-1: the app's "Discover" now drives EdgeTX's OWN sensor discovery — the same
    // `allowNewSensors` flag the radio's Telemetry menu toggles. While ON, incoming sensors are
    // registered into g_model.telemetrySensors and persisted (storageDirty) by EdgeTX's own add
    // path, instead of the app merely re-reading the live catalog. {u8 on}. ATTACHED-gated.
    if (s_link.companionInputAllowed() && m.len >= 1) allowNewSensors = (m.payload[0] != 0);
  }
  else if (m.type == MSG_MODEL_PULL) {
    // V2 (docs/protocol §8): stream the active model YAML to the app as the source of truth.
    ueStartModelPull(m.len >= 1 ? m.payload[0] : 0);
  }
  else if (m.type == MSG_SD_LIST) {
    // FL-1 (s12): list an SD directory (e.g. /LOGS) → {u8 count, [u32 size, u8 nameLen, name]}.
    // Files only (dirs skipped), capped to what fits one frame — plenty for a flight-log session.
    char path[80]; uint8_t pl = m.len ? m.payload[0] : 0;
    if (pl == 0 || (uint32_t)(1 + pl) > m.len || pl >= sizeof(path)) pl = 0;
    for (uint8_t i = 0; i < pl; ++i) path[i] = (char)m.payload[1 + i];
    path[pl] = 0;
    uint8_t p[240]; size_t o = 1; uint8_t cnt = 0;
    DIR dir; FILINFO fno;
    if (pl && f_opendir(&dir, path) == FR_OK) {
      // BUG-007: bound TOTAL iterations, not just the file count. The AM_DIR `continue` below skips
      // cnt++, so a directory with many sub-entries (or a corrupt entry f_readdir keeps returning) made
      // the loop never terminate on /MODELS → the SD_LIST reply never went out (hung 2.5 min on-device;
      // /LOGS has no such entries, so it always answered). `guard` caps it regardless.
      uint16_t guard = 0;
      while (cnt < 60 && ++guard <= 400 && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        uint8_t nl = 0; while (nl < 48 && fno.fname[nl]) nl++;
        if (o + 4 + 1 + nl > sizeof(p)) break;
        uint32_t sz = (uint32_t)fno.fsize;
        p[o++] = sz & 0xFF; p[o++] = (sz>>8)&0xFF; p[o++] = (sz>>16)&0xFF; p[o++] = (sz>>24)&0xFF;
        p[o++] = nl; for (uint8_t k = 0; k < nl; ++k) p[o++] = (uint8_t)fno.fname[k];
        cnt++;
      }
      f_closedir(&dir);
    }
    p[0] = cnt;
    emitMsg(MSG_SD_LIST_RESP, p, (uint16_t)o);
  }
  else if (m.type == MSG_SD_PULL) {
    // FL-1 (s12): {u8 pathLen, path} → stream that SD file via BULK (kind=BULK_LOG). Read-only.
    char path[80]; uint8_t pl = m.len ? m.payload[0] : 0;
    if (pl == 0 || (uint32_t)(1 + pl) > m.len || pl >= sizeof(path)) return;
    for (uint8_t i = 0; i < pl; ++i) path[i] = (char)m.payload[1 + i];
    path[pl] = 0;
    const char* base = path; for (const char* q = path; *q; ++q) if (*q == '/') base = q + 1;
    ueStartFilePull(path, base, BULK_LOG);
  }
  else if (m.type == MSG_MODEL_PUSH_BEGIN) { ueModelPushBegin(m); }   // Phase 3: whole-file write-back
  else if (m.type == MSG_MODEL_PUSH_DATA)  { ueModelPushData(m); }
  else if (m.type == MSG_MODEL_PUSH_END)   { ueModelPushEnd(m); }
  else if (m.type == MSG_MODEL_COPY || m.type == MSG_MODEL_DELETE || m.type == MSG_MODEL_SELECT) {
    // Set 6 model radio-ops (ADR-0025). ATTACHED-gated. Copy/delete change the store; SELECT switches the
    // active model (flight-adjacent) behind the native safety-gate (throttle must be idle → else NACK_UNSAFE).
    uint8_t reason;
    char nm[LEN_MODEL_FILENAME + 1];
    if (!s_link.companionInputAllowed()) reason = NACK_NOT_ATTACHED;
    else if (!ueReadModelName(m, nm, sizeof(nm))) reason = NACK_BAD_LENGTH;
    else if (m.type == MSG_MODEL_COPY)   reason = ueModelCopy(nm);
    else if (m.type == MSG_MODEL_DELETE) reason = ueModelDelete(nm);
    else                                 reason = ueModelSelect(nm);
    if (reason == 0) { uint8_t a[1] = { m.seq }; emitMsg(MSG_ACK, a, 1); }
    else { uint8_t nk[2] = { m.seq, reason }; emitMsg(MSG_NACK, nk, 2); }
  }
  else if (m.type == MSG_DESCRIBE_PAGE) {
    // Set up a PACED snapshot job (docs/09,10): visible rows (FIELD_DESC + value,
    // or LINK), then PAGE_DESC_END. Emitted from the tick loop, gated by
    // usbSerialFreeSpace(), so the burst can never overrun the CDC TX ring.
    uint16_t page = (m.len >= 2) ? (uint16_t)(m.payload[0] | (m.payload[1] << 8)) : PAGE_MODEL_SETUP;
    s_descPage   = page;
    s_descRow    = 0;
    s_descPhase  = 0;
    s_descEnded  = false;
    s_descLen    = 0;          // drop any half-built frame from a prior page
    s_descActive = true;
  }
}

// Called from per10ms() (main context, ~100 Hz).
extern "C" void ultraedgeCompanionTick()
{
  if (getSelectedUsbMode() != USB_SERIAL_MODE || !usbStarted()) {
    // Transport gone (unplug / mode change): release the link and RESTORE the stock Serial
    // consumer so plain Serial is byte-for-byte stock again (docs/29 release path).
    if (s_cbSet) {
      usbSerialSetReceiveDataCb(nullptr, s_prevRxCb);
      s_prevRxCb = nullptr;
      s_cbSet = false;
    }
    s_companionActive = false;
    // Reset so a fresh connection re-negotiates.
    s_inited = false;
    s_descActive = false; s_descLen = 0;   // abandon any in-flight describe job
    s_subTelemRaw.active = false; telemQueueSet(false);   // release the telemetry-queue registration
    s_subSources.active = false;
    if (s_pushActive) ueModelPushDiscard();   // Phase 3: abandon any in-flight model push, keep original
    s_uxOwner = false; s_sfHead = s_sfTail = 0;   // B4b: relinquish UX ownership on link down (radio resumes UX)
    s_link.onTransportDown();
    return;
  }

  UE_CRUMB(10);   // SERIAL mode + USB started: companion engaging

  if (!s_inited) {
    s_dec.reset();
    s_dec.setCallback(onFrame, nullptr);
    s_link.onTransportUp();          // DETACHED -> HANDSHAKING (passive: awaiting HELLO)
    s_keysAccepted = 0;
    s_inited = true;
  }
  UE_CRUMB(11);   // decoder/link initialised
  if (!s_cbSet) {
    // Capture whatever consumer EdgeTX already installed for Serial (CLI/Lua/telemetry),
    // then install our wrapper. We forward to the captured one until a HELLO arrives, so
    // stock Serial is NOT hijacked just because USB_COMPANION is built in (docs/29).
    void (*cur)(uint8_t*, uint32_t) = usbSerialGetReceiveDataCb();
    if (cur != ultraedgeCompanionRxCb) s_prevRxCb = cur;   // guard: never capture ourselves
    s_companionActive = false;
    usbSerialSetReceiveDataCb(nullptr, ultraedgeCompanionRxCb);
    s_cbSet = true;
  }
  UE_CRUMB(12);   // serial RX callback captured + swapped

  s_nowMs += 10;

  // Drain the ISR ring into the decoder.
  while (s_rxTail != s_rxHead) {
    uint8_t b = s_rx[s_rxTail];
    s_rxTail = (uint16_t)((s_rxTail + 1) % RX_RING);
    s_dec.feed(&b, 1);
  }
  UE_CRUMB(13);   // RX ring drained into decoder (frames dispatched via onFrame)

  // Link heartbeat/timeout bookkeeping.
  s_link.tick(s_nowMs);

  // NEGOTIATION GATE (docs/29): stay completely silent on TX until a host has opted in with
  // a HELLO (which onFrame() detects and sets s_companionActive). Before that, plain Serial
  // is untouched in BOTH directions — RX forwards to the stock consumer (above), and we emit
  // nothing. The moment onFrame() sees the HELLO it flips active and also emits HELLO_ACK,
  // so the host still gets its immediate reply.
  if (!s_companionActive) return;
  UE_CRUMB(20);   // HELLO seen: companion ACTIVE (host opted in)

  ueCalTick();   // Set 4: drive EdgeTX calibration + stream live analog while a cal session is active

  uint8_t f[MAX_FRAME_WIRE]; size_t n;

  // ---- Class scheduler (docs/08): control (0) first, then config replies are
  // already emitted inline in onFrame() as they arrive (highest-priority path),
  // then class-2 streams only within this tick's byte budget. ----

  // CLASS 0 — advertise: periodic HELLO (~every 1 s) so a listening host sees us.
  static uint8_t helloDiv = 0;
  if (++helloDiv >= 100) {
    helloDiv = 0;
    Hello h; h.proto_ver = PROTO_VERSION; h.fw_ver = 0x00040000;
    h.role = ROLE_RADIO; h.caps = UE_LOCAL_CAPS;
    n = encodeHello(s_seq++, h, /*ack=*/false, f, sizeof(f));
    emitBytes(f, n);   // control class: not budget-limited
  }

  // CLASS 1 — paced config describe (reliable). Emit built frames only while the
  // TX ring has room; usbSerialPutc() does not check, so a burst would overrun it
  // (docs/09). Leave headroom so the periodic HELLO and the channel stream still
  // fit. Bounded per tick so we never hog the main loop.
  if (s_descActive) {
    for (uint8_t guard = 0; guard < 8; ++guard) {
      if (s_descLen == 0) {
        s_descLen = buildNextDescribe();
        if (s_descLen == 0) { s_descActive = false; break; }   // job complete
      }
      if (usbSerialFreeSpace() >= s_descLen + 64) {
        emitBytes(s_descBuf, s_descLen);
        s_descLen = 0;
      } else {
        break;   // ring too full this tick — resume next tick (nothing lost)
      }
    }
  }

  // CLASS 2 — subscription-driven streams, capped by the per-tick byte budget.
  // Nothing streams unless the phone has subscribed (docs/08 Principle 3).
  s_class2Budget = CLASS2_BYTES_PER_TICK;   // refill each tick

  if (s_subChannels.active) {
    s_subChannels.accumMs += 10;
    uint16_t periodMs = (uint16_t)(1000 / s_subChannels.rateHz);
    if (periodMs < 10) periodMs = 10;
    if (s_subChannels.accumMs >= periodMs) {
      s_subChannels.accumMs = 0;
      int16_t ch[16];
      for (int i = 0; i < 16; ++i) ch[i] = channelOutputs[i];
      n = encodeChannels(s_seq++, ch, 16, f, sizeof(f));
      emitClass2(f, n);   // yields if over budget (loss-tolerant)
    }
  }

  if (s_subHome.active) {
    s_subHome.accumMs += 10;
    uint16_t periodMs = (uint16_t)(1000 / s_subHome.rateHz);
    if (periodMs < 20) periodMs = 20;   // Home screen: 50 Hz ceiling, plenty for trims
    if (s_subHome.accumMs >= periodMs) {
      s_subHome.accumMs = 0;
      uint8_t hp[128]; size_t hn = buildHomePayload(hp);
      n = encodeFrame(MSG_HOME, s_seq++, hp, (uint16_t)hn, f, sizeof(f));
      emitClass2(f, n);   // loss-tolerant, shares the class-2 budget
    }
  }

  if (s_subTelem.active) {
    s_subTelem.accumMs += 10;
    uint16_t periodMs = (uint16_t)(1000 / s_subTelem.rateHz);
    if (periodMs < 33) periodMs = 33;   // up to 30 Hz (fast-telemetry, docs/37; was 10 Hz)
    if (s_subTelem.accumMs >= periodMs) {
      s_subTelem.accumMs = 0;
      // {u8 count, [u16 id, i32 value]} — send ALL available sensors EVERY frame (docs/37).
      // MAX_PAYLOAD=512 fits up to 85 sensors, and MAX_TELEMETRY_SENSORS*6+1=361<512, so the whole
      // set ships in one frame. This replaces the PB2 ≤10/frame round-robin (docs/35), which was a
      // correct fix for "late sensors never sent" but capped each sensor's refresh at ~rate/ceil(n/10)
      // — e.g. attitude at ~1.6 Hz with 24 sensors. Now every sensor updates at the full frame rate.
      if (peerHasTypedTelem()) {
        // ---- V2 TYPED telemetry (docs/protocol/PROTOCOL-V4.md §5) --------------------------------
        // ONE frame carries every value kind: {u8 count, [u16 id, u8 kind, u8 len, payload]}. This
        // subsumes the old scalar MSG_TELEMETRY + the GPS/text MSG_TELEMETRY2 bolt-on. The app decodes
        // by kind → scalar / gps table / text / cells / datetime, with no per-kind message.
        static uint8_t tp[MAX_PAYLOAD];
        static uint16_t s_telemStart = 0;   // rotating start so a huge model can't starve later sensors
        size_t to = 1; uint16_t cnt = 0; bool wrapped = false;
        for (uint16_t k = 0; k < MAX_TELEMETRY_SENSORS; ++k) {
          uint16_t i = (uint16_t)((s_telemStart + k) % MAX_TELEMETRY_SENSORS);
          if (!isTelemetryFieldAvailable((uint8_t)i)) continue;
          uint8_t u = g_model.telemetrySensors[i].unit;
          // Reserve worst-case entry size before writing so we never overflow MAX_PAYLOAD.
          if (to + 4 + (2 + 2*MAX_CELLS) > MAX_PAYLOAD) { s_telemStart = i; wrapped = true; break; }
          size_t hdr = to; to += 2; // id filled after
          tp[hdr] = i & 0xFF; tp[hdr+1] = (i >> 8) & 0xFF;
          if (u == UNIT_GPS || u == UNIT_GPS_LATITUDE || u == UNIT_GPS_LONGITUDE) {
            int32_t lat = telemetryItems[i].gps.latitude, lon = telemetryItems[i].gps.longitude;
            tp[to++] = VK_GPS; tp[to++] = 8;
            tp[to++]=lat&0xFF; tp[to++]=(lat>>8)&0xFF; tp[to++]=(lat>>16)&0xFF; tp[to++]=(lat>>24)&0xFF;
            tp[to++]=lon&0xFF; tp[to++]=(lon>>8)&0xFF; tp[to++]=(lon>>16)&0xFF; tp[to++]=(lon>>24)&0xFF;
          } else if (u == UNIT_TEXT) {
            const char* tx = telemetryItems[i].text;
            uint8_t len = 0; while (len < TELEMETRY_SENSOR_TEXT_LENGTH && tx[len]) len++;
            tp[to++] = VK_TEXT; tp[to++] = len;
            for (uint8_t j = 0; j < len; ++j) tp[to++] = (uint8_t)tx[j];
          } else if (u == UNIT_DATETIME) {
            const auto& d = telemetryItems[i].datetime;
            tp[to++] = VK_DATETIME; tp[to++] = 8;
            tp[to++]=d.year&0xFF; tp[to++]=(d.year>>8)&0xFF; tp[to++]=d.month; tp[to++]=d.day;
            tp[to++]=d.hour; tp[to++]=d.min; tp[to++]=d.sec; tp[to++]=0 /*pad*/;
          } else if (u == UNIT_CELLS) {
            uint8_t ncell = telemetryItems[i].cells.count; if (ncell > MAX_CELLS) ncell = MAX_CELLS;
            tp[to++] = VK_CELLS; tp[to++] = (uint8_t)(1 + 2*ncell); tp[to++] = ncell;
            for (uint8_t j = 0; j < ncell; ++j) { uint16_t mv = (uint16_t)(telemetryItems[i].cells.values[j].value * 10); tp[to++]=mv&0xFF; tp[to++]=(mv>>8)&0xFF; }
          } else {
            int32_t v = telemetryItems[i].value;
            tp[to++] = VK_I32; tp[to++] = 4;
            tp[to++]=v&0xFF; tp[to++]=(v>>8)&0xFF; tp[to++]=(v>>16)&0xFF; tp[to++]=(v>>24)&0xFF;
          }
          cnt++;
        }
        if (!wrapped) s_telemStart = 0;   // whole set fit — restart next frame from 0
        if (cnt) { tp[0] = (uint8_t)cnt; n = encodeFrame(MSG_TELEMETRY, s_seq++, tp, (uint16_t)to, f, sizeof(f)); emitClass2(f, n); }
      } else {
        // ---- Legacy (v3 peer, no CAP_TELEM_TYPED): scalar MSG_TELEMETRY + GPS/text MSG_TELEMETRY2 --
        static uint8_t tp[1 + MAX_TELEMETRY_SENSORS * 6];
        size_t to = 1; uint16_t cnt = 0;
        for (uint16_t i = 0; i < MAX_TELEMETRY_SENSORS; ++i) {
          if (isTelemetryFieldAvailable((uint8_t)i)) {
            int32_t v = telemetryItems[i].value;
            tp[to++] = i & 0xFF; tp[to++] = (i >> 8) & 0xFF;
            tp[to++] = v & 0xFF; tp[to++] = (v>>8)&0xFF; tp[to++] = (v>>16)&0xFF; tp[to++] = (v>>24)&0xFF;
            cnt++;
          }
        }
        if (cnt) { tp[0] = (uint8_t)cnt; n = encodeFrame(MSG_TELEMETRY, s_seq++, tp, (uint16_t)to, f, sizeof(f)); emitClass2(f, n); }
        static uint8_t cp[1 + MAX_TELEMETRY_SENSORS * (2 + 1 + 1 + 16)];
        size_t co = 1; uint8_t ccnt = 0;
        for (uint16_t i = 0; i < MAX_TELEMETRY_SENSORS; ++i) {
          if (!isTelemetryFieldAvailable((uint8_t)i)) continue;
          uint8_t u = g_model.telemetrySensors[i].unit;
          if (u == UNIT_GPS || u == UNIT_GPS_LATITUDE || u == UNIT_GPS_LONGITUDE) {
            int32_t lat = telemetryItems[i].gps.latitude, lon = telemetryItems[i].gps.longitude;
            cp[co++] = i & 0xFF; cp[co++] = (i >> 8) & 0xFF; cp[co++] = 1; cp[co++] = 8;
            cp[co++]=lat&0xFF; cp[co++]=(lat>>8)&0xFF; cp[co++]=(lat>>16)&0xFF; cp[co++]=(lat>>24)&0xFF;
            cp[co++]=lon&0xFF; cp[co++]=(lon>>8)&0xFF; cp[co++]=(lon>>16)&0xFF; cp[co++]=(lon>>24)&0xFF;
            ccnt++;
          } else if (u == UNIT_TEXT) {
            const char* tx = telemetryItems[i].text;
            uint8_t len = 0; while (len < TELEMETRY_SENSOR_TEXT_LENGTH && tx[len]) len++;
            cp[co++] = i & 0xFF; cp[co++] = (i >> 8) & 0xFF; cp[co++] = 2; cp[co++] = len;
            for (uint8_t j = 0; j < len; ++j) cp[co++] = (uint8_t)tx[j];
            ccnt++;
          }
        }
        if (ccnt) { cp[0] = ccnt; n = encodeFrame(MSG_TELEMETRY2, s_seq++, cp, (uint16_t)co, f, sizeof(f)); emitClass2(f, n); }
      }
    }
  }

  // Input sources (docs/37): live sticks/pots/switches as {u8 count, [u16 mixsrc, i16 value]} in
  // EdgeTX ±1024. Keyed by MIXSRC id (mode-independent: MIXSRC_Thr is always throttle) so the app
  // resolves getValue('thr'…) and detects "moved source" for auto-select in input pickers.
  if (s_subSources.active) {
    s_subSources.accumMs += 10;
    uint16_t periodMs = (uint16_t)(1000 / s_subSources.rateHz);
    if (periodMs < 33) periodMs = 33;   // up to 30 Hz
    if (s_subSources.accumMs >= periodMs) {
      s_subSources.accumMs = 0;
      static uint8_t sp[1 + (MAX_STICKS + MAX_POTS + MAX_SWITCHES) * 4];
      size_t so = 1; uint16_t scnt = 0;
      auto emitSrc = [&](uint16_t mixsrc) {
        int32_t v = getValue((mixsrc_t)mixsrc);
        if (v >  32767) v =  32767; if (v < -32768) v = -32768;
        sp[so++] = mixsrc & 0xFF; sp[so++] = (mixsrc >> 8) & 0xFF;
        sp[so++] = v & 0xFF; sp[so++] = (v >> 8) & 0xFF; scnt++;
      };
      for (uint16_t i = 0; i < MAX_STICKS;   ++i) emitSrc((uint16_t)(MIXSRC_FIRST_STICK + i));
      for (uint16_t i = 0; i < MAX_POTS;     ++i) emitSrc((uint16_t)(MIXSRC_FIRST_POT + i));
      for (uint16_t i = 0; i < MAX_SWITCHES; ++i) emitSrc((uint16_t)(MIXSRC_FIRST_SWITCH + i));
      if (scnt) { sp[0] = (uint8_t)scnt; n = encodeFrame(MSG_SOURCES, s_seq++, sp, (uint16_t)so, f, sizeof(f)); emitClass2(f, n); }
    }
  }

  // B4b (docs/37): drain UX-plane SF events the mixer task queued while the app owns UX. Emit each as
  // MSG_SF_FIRED {u8 sf_index, u8 func, i32 value, u8 nameLen, name} for the phone to render.
  while (s_sfTail != s_sfHead) {
    SfEvt& e = s_sfQ[s_sfTail];
    uint8_t p[7 + 16]; size_t o = 0;
    p[o++] = e.idx; p[o++] = e.func;
    p[o++] = e.val & 0xFF; p[o++] = (e.val>>8)&0xFF; p[o++] = (e.val>>16)&0xFF; p[o++] = (e.val>>24)&0xFF;
    p[o++] = e.nl; for (uint8_t k = 0; k < e.nl; ++k) p[o++] = (uint8_t)e.name[k];
    emitMsg(MSG_SF_FIRED, p, (uint16_t)o);
    s_sfTail = (uint8_t)((s_sfTail + 1) & 7);
  }

  // P2 (docs/ARCH §4): drain unified-audio events the audio.cpp tap queued while the app owns UX.
  // Emit each as MSG_AUDIO (unbudgeted control) so call-outs stay prompt and in order. FILE carries
  // the fully-resolved path; TONE carries synth params; STOP/FLUSH mirror stopPlay/stopAll/flush.
  while (s_audTail != s_audHead) {
    AudioEvt& e = s_audioQ[s_audTail];
    uint8_t p[4 + AUDIO_FILENAME_MAXLEN + 2]; size_t o = 0;
    p[o++] = e.kind;
    if (e.kind == AUDIO_FILE) {
      p[o++] = e.flags; p[o++] = e.id;
      uint8_t nl = 0; while (e.path[nl] && nl < AUDIO_FILENAME_MAXLEN) nl++;
      p[o++] = nl; for (uint8_t k = 0; k < nl; ++k) p[o++] = (uint8_t)e.path[k];
    } else if (e.kind == AUDIO_TONE) {
      p[o++] = e.flags;
      p[o++] = e.freq & 0xFF;  p[o++] = (e.freq >> 8) & 0xFF;
      p[o++] = e.len & 0xFF;   p[o++] = (e.len >> 8) & 0xFF;
      p[o++] = e.pause & 0xFF; p[o++] = (e.pause >> 8) & 0xFF;
      p[o++] = (uint8_t)e.freqIncr;
    } else if (e.kind == AUDIO_STOP_ID) {
      p[o++] = e.id;
    } // AUDIO_STOP_ALL / AUDIO_FLUSH carry no extra bytes
    emitMsg(MSG_AUDIO, p, (uint16_t)o);
    s_audTail = audNext(s_audTail);
  }

  // Raw passthrough (docs/32): forward telemetry-queue bytes as MSG_TELEM_RAW. Not rate-limited —
  // we push as fast as the class-2 budget allows so the app's Lua pop() sees frames promptly. We
  // check the budget BEFORE draining so we never pop bytes we can't send (which would misframe the
  // app-side SPort/CRSF parser).
  if (s_subTelemRaw.active && s_telemQueueReg) {
    uint8_t proto = telemProto();
    uint16_t rawSent = 0;   // hard per-tick cap so raw can't monopolise IRQ-off time (docs/39)
    while (s_telemQueue.size() > 0) {
      if (rawSent >= RAW_BYTES_PER_TICK) break;   // rest waits for next tick (bounded latency)
      uint16_t avail = (uint16_t)s_telemQueue.size();
      uint8_t nb = avail > 48 ? 48 : (uint8_t)avail;
      if (nb > (uint8_t)(RAW_BYTES_PER_TICK - rawSent)) nb = (uint8_t)(RAW_BYTES_PER_TICK - rawSent);
      if (s_class2Budget < (uint16_t)(nb + 16)) break;   // save the rest for next tick
      uint8_t pl[2 + 48]; pl[0] = proto; pl[1] = nb;
      for (uint8_t j = 0; j < nb; ++j) { uint8_t bb = 0; s_telemQueue.pop(bb); pl[2+j] = bb; }
      n = encodeFrame(MSG_TELEM_RAW, s_seq++, pl, (uint16_t)(2 + nb), f, sizeof(f));
      emitClass2(f, n);
      rawSent += nb;
    }
  }

  // V2 BULK (docs/protocol §8-9): stream the model-pull in small SD reads, paced by the class-2
  // budget so it can't starve telemetry, and bounded per tick so one SD read never stalls the tick.
  if (s_bulkActive) {
    UE_CRUMB(42);   // per-tick SD f_read from timer-task context (bulk streaming)
    uint16_t bulkSent = 0;
    while (s_bulkActive && bulkSent < BULK_BYTES_PER_TICK) {
      if (s_class2Budget < (uint16_t)(BULK_CHUNK + 32)) break;   // save the rest for next tick
      uint8_t chunk[BULK_CHUNK]; UINT br = 0;
      FRESULT r = f_read(&s_bulkFile, chunk, BULK_CHUNK, &br);
      if (r != FR_OK) {
        uint8_t ab[3] = { (uint8_t)(s_bulkXferId & 0xFF), (uint8_t)(s_bulkXferId >> 8), 2 /*read error*/ };
        emitMsg(MSG_BULK_ABORT, ab, sizeof(ab)); f_close(&s_bulkFile); s_bulkActive = false; break;
      }
      if (br > 0) {
        s_bulkCrc = crc16_update(s_bulkCrc, chunk, br);
        // BULK_DATA {u16 xfer_id, u32 offset, u16 len, bytes}
        uint8_t p[2 + 4 + 2 + BULK_CHUNK]; size_t o = 0;
        p[o++] = s_bulkXferId & 0xFF; p[o++] = (s_bulkXferId >> 8) & 0xFF;
        p[o++] = s_bulkOffset & 0xFF; p[o++] = (s_bulkOffset>>8)&0xFF; p[o++] = (s_bulkOffset>>16)&0xFF; p[o++] = (s_bulkOffset>>24)&0xFF;
        p[o++] = br & 0xFF; p[o++] = (br >> 8) & 0xFF;
        for (UINT k = 0; k < br; ++k) p[o++] = chunk[k];
        n = encodeFrame(MSG_BULK_DATA, s_seq++, p, (uint16_t)o, f, sizeof(f)); emitClass2(f, n);
        s_bulkOffset += br; bulkSent += (uint16_t)br;
      }
      if (br < BULK_CHUNK || s_bulkOffset >= s_bulkTotal) {
        // EOF → BULK_END {u16 xfer_id, u8 status, u16 crc16}  (the one checksum, CRC16-CCITT)
        uint16_t fin = s_bulkCrc;
        uint8_t p[2 + 1 + 2]; size_t o = 0;
        p[o++] = s_bulkXferId & 0xFF; p[o++] = (s_bulkXferId >> 8) & 0xFF; p[o++] = 0 /*ok*/;
        p[o++] = fin & 0xFF; p[o++] = (fin>>8)&0xFF;
        emitMsg(MSG_BULK_END, p, (uint16_t)o);
        f_close(&s_bulkFile); s_bulkActive = false; break;
      }
    }
  }
}

#endif // USB_COMPANION
