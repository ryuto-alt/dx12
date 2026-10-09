#include "UnoLink.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(ARDUINO_ARCH_AVR)
  #include <avr/pgmspace.h>
#endif

// ---------- 数値の文字列化（AVR は printf の %f が無いので自前）----------
static uint8_t fmtInt(char* out, long v) {
  char tmp[12]; uint8_t t = 0, n = 0;
  bool neg = v < 0;
  unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
  do { tmp[t++] = '0' + (u % 10); u /= 10; } while (u);
  if (neg) out[n++] = '-';
  while (t) out[n++] = tmp[--t];
  out[n] = 0;
  return n;
}

// 小数 4 桁まで（末尾の 0 は削る）。NaN は 0、極端な値は ±1e9 に丸める
static uint8_t fmtFloat(char* out, float v) {
  if (v != v) v = 0;
  bool neg = v < 0; float a = neg ? -v : v;
  if (a > 1e9f) a = 1e9f;
  unsigned long ip = (unsigned long)a;
  unsigned long fr = (unsigned long)((a - (float)ip) * 10000.0f + 0.5f);
  if (fr >= 10000UL) { ip++; fr -= 10000UL; }
  uint8_t n = 0;
  if (neg && (ip || fr)) out[n++] = '-';
  n += fmtInt(out + n, (long)ip);
  if (fr) {
    out[n++] = '.';
    char d[4];
    for (int8_t k = 3; k >= 0; k--) { d[k] = '0' + (fr % 10); fr /= 10; }
    uint8_t len = 4; while (len > 0 && d[len - 1] == '0') len--;
    for (uint8_t k = 0; k < len; k++) out[n++] = d[k];
    out[n] = 0;
  }
  return n;
}

static const char* boardName() {
#if defined(ARDUINO_ARCH_ESP32)
  #if defined(CONFIG_IDF_TARGET_ESP32S3)
    return "esp32s3";
  #elif defined(CONFIG_IDF_TARGET_ESP32S2)
    return "esp32s2";
  #elif defined(CONFIG_IDF_TARGET_ESP32C3)
    return "esp32c3";
  #else
    return "esp32";
  #endif
#elif defined(ARDUINO_ARCH_AVR)
  #if defined(ARDUINO_AVR_NANO)
    return "nano";
  #elif defined(ARDUINO_AVR_UNO)
    return "uno";
  #elif defined(ARDUINO_AVR_MEGA2560)
    return "mega2560";
  #elif defined(ARDUINO_AVR_LEONARDO)
    return "leonardo";
  #else
    return "avr";
  #endif
#elif defined(ARDUINO_ARCH_MEGAAVR)
  // Nano Every など ATmega4809 系（ARCH_AVR ではない。RAM 6KB なので既定の UL_MAX_CH 16 のままでよい）
  #if defined(ARDUINO_AVR_NANO_EVERY)
    return "nano_every";
  #else
    return "megaavr";
  #endif
#elif defined(ARDUINO_ARCH_RP2040)
  return "rp2040";
#elif defined(ARDUINO_ARCH_SAMD)
  return "samd";
#else
  return "unknown";
#endif
}

static bool validName(const char* n) {
  if (!n) return false;
  size_t len = strlen(n);
  if (len < 1 || len > UL_NAME_MAX) return false;
  for (size_t i = 0; i < len; i++) {
    char c = n[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

// ---------- 構築 ----------
UnoLink::UnoLink(const char* name, uint8_t fw)
  : _name(name), _fw(fw), _s(nullptr), _count(0), _rxLen(0), _rxOver(false),
    _lastRxMs(0), _lastFullMs(0), _minIntervalMs(1000 / UL_DEFAULT_MAX_HZ), _errCount(0),
    _safeState(true), _failsafe(false), _gotRx(false), _hello(false),
    _outCb(nullptr), _cmdCb(nullptr) {}

int UnoLink::addChannel(const char* name, uint8_t dir, UnoLinkType t, bool range, float mn, float mx, float safeV) {
  if (!validName(name) || _count >= UL_MAX_CH || findChannel(name) >= 0) {
    if (_s) error(F("addChannel failed"));
    return -1;
  }
  Channel& c = _ch[_count];
  strcpy(c.name, name);
  c.dir = dir; c.type = (uint8_t)t;
  c.flags = (range && t != UL_BOOL) ? F_RANGE : 0;
  c.minV = mn; c.maxV = mx;
  c.safeV = (dir == 1) ? safeV : 0;
  c.value = c.safeV; c.lastSent = 0; c.lastSentMs = 0;
  uint8_t idx = _count++;
  if (_s && _hello) sendChannel(idx);   // 後から足したら即 @ch
  return idx;
}

int UnoLink::addInput(const char* n, UnoLinkType t) { return addChannel(n, 0, t, false, 0, 0, 0); }
int UnoLink::addInput(const char* n, UnoLinkType t, float a, float b) { return addChannel(n, 0, t, true, a, b, 0); }
int UnoLink::addOutput(const char* n, UnoLinkType t, float s) { return addChannel(n, 1, t, false, 0, 0, s); }
int UnoLink::addOutput(const char* n, UnoLinkType t, float a, float b, float s) { return addChannel(n, 1, t, true, a, b, s); }

void UnoLink::begin(HardwareSerial& s, unsigned long baud) {
  s.begin(baud);
  begin(static_cast<Stream&>(s));
}

void UnoLink::begin(Stream& s) {
  _s = &s;
  _lastRxMs = millis();
  applySafe();          // 起動直後は安全値
  sendHello();
}

// ---------- 名乗り ----------
void UnoLink::sendChannel(uint8_t i) {
  const Channel& c = _ch[i];
  char b[16];
  _s->print(F("@ch "));
  _s->print(c.name);
  _s->print(c.dir ? F(" out ") : F(" in "));
  _s->print(c.type == UL_BOOL ? F("bool") : (c.type == UL_INT ? F("int") : F("float")));
  if (c.flags & F_RANGE) {
    fmtFloat(b, c.minV); _s->write(' '); _s->print(b);
    fmtFloat(b, c.maxV); _s->write(' '); _s->print(b);
  }
  if (c.dir) { fmtFloat(b, c.safeV); _s->print(F(" safe=")); _s->print(b); }
  _s->write('\n');
}

void UnoLink::sendHello() {
  if (!_s) return;
  _s->print(F("@hello name=")); _s->print(_name);
  _s->print(F(" proto=1 fw=")); _s->print((unsigned)_fw);
  _s->print(F(" board=")); _s->print(boardName());
  _s->write('\n');
  for (uint8_t i = 0; i < _count; i++) sendChannel(i);
  _s->print(F("@ready\n"));
  _hello = true;
  _lastFullMs = millis() - UL_RESEND_MS;   // 次の update() で全入力値を送る
}

// ---------- 入力の送信 ----------
int UnoLink::findChannel(const char* name) const {
  for (uint8_t i = 0; i < _count; i++) if (strcmp(_ch[i].name, name) == 0) return i;
  return -1;
}
const char* UnoLink::channelName(int ch) const { return (ch >= 0 && ch < _count) ? _ch[ch].name : ""; }
bool UnoLink::isOutput(int ch) const { return ch >= 0 && ch < _count && _ch[ch].dir == 1; }

bool UnoLink::sendIndex(int ch, float v, bool force) {
  if (ch < 0 || ch >= _count || _ch[ch].dir != 0) return false;
  Channel& c = _ch[ch];
  c.value = v;
  if (force) c.flags |= F_FORCE;
  return true;
}
bool UnoLink::send(const char* name, float v, bool force) { return sendIndex(findChannel(name), v, force); }

bool UnoLink::sendMany(const char* const* names, const float* values, uint8_t n) {
  bool ok = true;
  for (uint8_t i = 0; i < n; i++) ok &= send(names[i], values[i]);
  return ok;
}

void UnoLink::setMaxRate(uint16_t hz) { _minIntervalMs = hz ? (1000 / hz) : 0; }

void UnoLink::flush() { flushInputs(false); }

void UnoLink::flushInputs(bool all) {
  if (!_s) return;
  uint32_t now = millis();
  char line[UL_LINE_MAX]; uint8_t n = 0;
  for (uint8_t i = 0; i < _count; i++) {
    Channel& c = _ch[i];
    if (c.dir != 0) continue;
    bool changed = !(c.flags & F_SENT) || (c.flags & F_FORCE) || c.value != c.lastSent;
    bool due = all || (changed && (uint32_t)(now - c.lastSentMs) >= _minIntervalMs);
    if (!due) continue;
    char tok[UL_NAME_MAX + 16]; uint8_t t = 0;
    strcpy(tok, c.name); t = strlen(tok); tok[t++] = '=';
    if (c.type == UL_BOOL) tok[t++] = (c.value != 0) ? '1' : '0';
    else if (c.type == UL_INT) t += fmtInt(tok + t, (long)(c.value + (c.value >= 0 ? 0.5f : -0.5f)));
    else t += fmtFloat(tok + t, c.value);
    // 1 行 126 文字 + \n までに収める
    if (n && n + 1 + t > UL_LINE_MAX - 2) { line[n++] = '\n'; _s->write((const uint8_t*)line, n); n = 0; }
    if (n) line[n++] = ' ';
    memcpy(line + n, tok, t); n += t;
    c.lastSent = c.value; c.lastSentMs = now; c.flags = (c.flags | F_SENT) & ~F_FORCE;
  }
  if (n) { line[n++] = '\n'; _s->write((const uint8_t*)line, n); }
}

// ---------- 出力 ----------
float UnoLink::getOutput(const char* name) const { return getOutputIndex(findChannel(name)); }
float UnoLink::getOutputIndex(int ch) const {
  return (ch >= 0 && ch < _count && _ch[ch].dir == 1) ? _ch[ch].value : 0;
}

void UnoLink::setOutput(uint8_t i, float v) {
  Channel& c = _ch[i];
  if (v != v) v = c.safeV;
  if (c.type == UL_BOOL) v = (v != 0) ? 1 : 0;
  else if (c.flags & F_RANGE) {
    float lo = c.minV < c.maxV ? c.minV : c.maxV, hi = c.minV < c.maxV ? c.maxV : c.minV;
    if (v < lo) v = lo; else if (v > hi) v = hi;
  }
  if (c.type == UL_INT) v = (float)(long)(v + (v >= 0 ? 0.5f : -0.5f));
  c.value = v;
  if (_outCb) _outCb(c.name, v);
}

void UnoLink::applySafe() {
  for (uint8_t i = 0; i < _count; i++) {
    if (_ch[i].dir != 1) continue;
    _ch[i].value = _ch[i].safeV;
    if (_outCb) _outCb(_ch[i].name, _ch[i].safeV);
  }
  _safeState = true;
}

bool UnoLink::connected() const { return _gotRx && (uint32_t)(millis() - _lastRxMs) < UL_FAILSAFE_MS; }

// ---------- 受信 ----------
void UnoLink::handleLine(char* line) {
  if (line[0] == '?') {
    if (strcmp(line, "?hello") == 0) { sendHello(); return; }
    if (strncmp(line, "?ping", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
      const char* seq = line + 5;
      while (*seq == ' ') seq++;
      _s->print(F("@pong "));
      if (*seq) { char b[24]; strncpy(b, seq, sizeof(b) - 1); b[sizeof(b) - 1] = 0; _s->print(b); }
      else _s->write('0');
      _s->write('\n');
      return;
    }
    if (_cmdCb) _cmdCb(line); else error(F("unknown command"));
    return;
  }
  if (line[0] != '!') return;
  if (strcmp(line, "!safe") == 0) { applySafe(); return; }
  // !a=1 b=0.5
  char* p = line + 1;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    char* tok = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    char* eq = strchr(tok, '=');
    if (!eq) { error(F("bad assign")); continue; }
    *eq = 0;
    int i = findChannel(tok);
    if (i < 0 || _ch[i].dir != 1) continue;   // 未知・入力チャンネルへの書き込みは無視
    setOutput((uint8_t)i, (float)atof(eq + 1));
    _safeState = false;
  }
}

void UnoLink::update() {
  if (!_s) return;
  while (_s->available() > 0) {
    int ch = _s->read();
    if (ch < 0) break;
    _lastRxMs = millis(); _gotRx = true; _failsafe = false;
    char c = (char)ch;
    if (c == '\r') continue;
    if (c == '\n') {
      if (_rxOver) { _errCount++; _s->print(F("@err line too long\n")); }
      else if (_rxLen) { _rx[_rxLen] = 0; handleLine(_rx); }
      _rxLen = 0; _rxOver = false;
      continue;
    }
    if (_rxOver) continue;
    if (_rxLen >= UL_LINE_MAX - 2) { _rxOver = true; continue; }   // 126 文字まで
    _rx[_rxLen++] = c;
  }

  uint32_t now = millis();
  // 安全義務: 1000ms 無受信で全出力を safe 値へ
  if (!_safeState && (uint32_t)(now - _lastRxMs) >= UL_FAILSAFE_MS) {
    applySafe();
    _failsafe = true;
    log(F("failsafe: outputs safe"));
  }

  if ((uint32_t)(now - _lastFullMs) >= UL_RESEND_MS) { flushInputs(true); _lastFullMs = now; }
  else flushInputs(false);
}

// ---------- ログ ----------
void UnoLink::emitText(bool isErr, const char* text, bool progmem) {
  if (!_s) return;
  if (isErr) _errCount++;
  char line[UL_LINE_MAX]; uint8_t n = 0;
  line[n++] = '@';
  const char* tag = isErr ? "err " : "log ";
  while (*tag) line[n++] = *tag++;
  while (n < UL_LINE_MAX - 2) {
    char c = progmem ? (char)pgm_read_byte(text) : *text;
    if (!c) break;
    text++;
    if (c == '\r' || c == '\n') c = ' ';
    line[n++] = c;
  }
  line[n++] = '\n';
  _s->write((const uint8_t*)line, n);
}

void UnoLink::log(const char* t) { emitText(false, t, false); }
void UnoLink::log(const __FlashStringHelper* t) { emitText(false, reinterpret_cast<const char*>(t), true); }
void UnoLink::error(const char* t) { emitText(true, t, false); }
void UnoLink::error(const __FlashStringHelper* t) { emitText(true, reinterpret_cast<const char*>(t), true); }

void UnoLink::logf(const char* fmt, ...) {
  char buf[UL_LINE_MAX];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  emitText(false, buf, false);
}
