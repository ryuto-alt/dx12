// UnoLinkGeneric - 汎用スケッチ（名前 "generic"）
// 書き込むだけで、エンジンから `?pin <pin> <mode> <name>` を送ってピンをチャンネルとして生やせる。
//   mode: in / in_pullup (bool・生の 0/1。反転は hardware.json の invert) / adc (int) /
//         out (bool 出力) / pwm (float 0..1 出力) / touch (ESP32 のみ・int)
//   pin : 数字 (例 34)。AVR は A0..A5 も可
// 出力は起動直後・?safe・1000ms 無受信で safe(=0) に戻る（UnoLink が保証）。
#include <UnoLink.h>

#if defined(ARDUINO_ARCH_ESP32)
  #define GEN_ADC_MAX 4095
  #define GEN_HAS_TOUCH 1
#else
  #define GEN_ADC_MAX 1023
  #define GEN_HAS_TOUCH 0
#endif

enum PinMode_ : uint8_t { PM_IN, PM_IN_PULLUP, PM_ADC, PM_OUT, PM_PWM, PM_TOUCH };

struct PinEntry {
  uint8_t pin;
  uint8_t mode;
  int8_t ch;          // UnoLink のチャンネル番号
  int16_t last;       // adc/touch の直近の送信値（ヒステリシス用）
};

static PinEntry pins[UL_MAX_CH];
static uint8_t pinCount = 0;

UnoLink ulink("generic", 1);

static void writePin(const PinEntry& e, float v) {
  if (e.mode == PM_OUT) {
    digitalWrite(e.pin, v != 0 ? HIGH : LOW);
  } else if (e.mode == PM_PWM) {
#if defined(ARDUINO_ARCH_ESP32)
    ledcWrite(e.pin, (uint32_t)(v * 1023.0f + 0.5f));    // arduino-esp32 3.x: ピン指定
#else
    analogWrite(e.pin, (int)(v * 255.0f + 0.5f));
#endif
  }
}

static void onOutput(const char* name, float v) {
  int ch = ulink.findChannel(name);
  for (uint8_t i = 0; i < pinCount; i++)
    if (pins[i].ch == ch) writePin(pins[i], v);
}

static bool parseMode(const char* s, uint8_t& m) {
  if (!strcmp(s, "in")) m = PM_IN;
  else if (!strcmp(s, "in_pullup")) m = PM_IN_PULLUP;
  else if (!strcmp(s, "adc")) m = PM_ADC;
  else if (!strcmp(s, "out")) m = PM_OUT;
  else if (!strcmp(s, "pwm")) m = PM_PWM;
  else if (!strcmp(s, "touch")) m = PM_TOUCH;
  else return false;
  return true;
}

static bool parsePin(const char* s, int& pin) {
#if defined(ARDUINO_ARCH_AVR)
  if ((s[0] == 'A' || s[0] == 'a') && s[1] >= '0' && s[1] <= '9') { pin = A0 + atoi(s + 1); return true; }
#endif
  if (s[0] < '0' || s[0] > '9') return false;
  pin = atoi(s);
  return true;
}

static bool pinUsable(int pin, bool output) {
  (void)output;
#if defined(ARDUINO_ARCH_ESP32)
  if (!digitalPinIsValid(pin)) return false;
#if defined(CONFIG_IDF_TARGET_ESP32)
  if (pin >= 6 && pin <= 11) return false;                 // 内蔵フラッシュ用。触ると即クラッシュする
#endif
  if (output && !digitalPinCanOutput(pin)) return false;   // 34..39 は入力専用
  return true;
#elif defined(ARDUINO_ARCH_AVR)
  return pin >= 2 && pin < NUM_DIGITAL_PINS + 0;   // 0,1 は Serial
#else
  return pin >= 0;
#endif
}

// ?pin <pin> <mode> <name>
static void onCommand(const char* line) {
  char buf[UL_LINE_MAX];
  strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  char* cmd = strtok(buf, " ");
  char* sPin = strtok(nullptr, " ");
  char* sMode = strtok(nullptr, " ");
  char* name = strtok(nullptr, " ");
  if (!cmd || strcmp(cmd, "?pin") != 0 || !sPin || !sMode || !name) { ulink.error(F("usage: ?pin <pin> <mode> <name>")); return; }

  int pin; uint8_t mode;
  if (!parsePin(sPin, pin)) { ulink.error(F("bad pin")); return; }
  if (!parseMode(sMode, mode)) { ulink.error(F("bad mode")); return; }
  bool isOut = (mode == PM_OUT || mode == PM_PWM);
#if !GEN_HAS_TOUCH
  if (mode == PM_TOUCH) { ulink.error(F("touch unsupported")); return; }
#endif
  if (!pinUsable(pin, isOut)) { ulink.error(F("pin unusable")); return; }

  // 同じ設定の再送は無害（@ch を送り直すだけ）
  int existing = ulink.findChannel(name);
  if (existing >= 0) {
    for (uint8_t i = 0; i < pinCount; i++)
      if (pins[i].ch == existing && pins[i].pin == pin && pins[i].mode == mode) { ulink.sendHello(); return; }
    ulink.error(F("name already used")); return;
  }
  for (uint8_t i = 0; i < pinCount; i++)
    if (pins[i].pin == pin) { ulink.error(F("pin already used")); return; }
  if (pinCount >= UL_MAX_CH) { ulink.error(F("too many pins")); return; }

  int ch = -1;
  switch (mode) {
    case PM_IN:        pinMode(pin, INPUT);        ch = ulink.addInput(name, UL_BOOL); break;
    case PM_IN_PULLUP: pinMode(pin, INPUT_PULLUP); ch = ulink.addInput(name, UL_BOOL); break;
    case PM_ADC:       pinMode(pin, INPUT);        ch = ulink.addInput(name, UL_INT, 0, GEN_ADC_MAX); break;
    case PM_TOUCH:     ch = ulink.addInput(name, UL_INT); break;
    case PM_OUT:       pinMode(pin, OUTPUT); digitalWrite(pin, LOW); ch = ulink.addOutput(name, UL_BOOL, 0); break;
    case PM_PWM:
#if defined(ARDUINO_ARCH_ESP32)
      ledcAttach(pin, 5000, 10);
#else
      pinMode(pin, OUTPUT);
#endif
      ch = ulink.addOutput(name, UL_FLOAT, 0, 1, 0); break;
  }
  if (ch < 0) return;   // addChannel が @err を送っている（名前不正など）
  pins[pinCount++] = { (uint8_t)pin, mode, (int8_t)ch, -1000 };
  if (isOut) writePin(pins[pinCount - 1], 0);   // 追加直後も safe
}

static void readInputs() {
  for (uint8_t i = 0; i < pinCount; i++) {
    PinEntry& e = pins[i];
    switch (e.mode) {
      case PM_IN: case PM_IN_PULLUP:
        ulink.sendIndex(e.ch, digitalRead(e.pin) ? 1 : 0);
        break;
      case PM_ADC: {
        int v = analogRead(e.pin);
        // ±2 未満の揺れは無視（端は通す）。ADC ノイズで 200Hz 送り続けないため
        if (e.last == -1000 || abs(v - e.last) >= 2 || v == 0 || v == GEN_ADC_MAX) { e.last = v; }
        ulink.sendIndex(e.ch, e.last);
        break;
      }
#if GEN_HAS_TOUCH
      case PM_TOUCH: ulink.sendIndex(e.ch, (float)touchRead(e.pin)); break;
#endif
    }
  }
}

void setup() {
  ulink.onOutput(onOutput);
  ulink.onCommand(onCommand);
  ulink.begin(Serial, 115200);
}

void loop() {
  readInputs();
  ulink.update();
  delay(1);
}
