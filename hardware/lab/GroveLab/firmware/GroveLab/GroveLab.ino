// GroveLab — Uno Engine の実験用スケッチ（名前 "grove"）
//   ボード: Arduino Nano Every + Grove Shield for Arduino Nano
//   エンジン側は GroveLab プロジェクト（assets/hardware.json の "grove"）。ゲーム側は Lua で hw.device("grove")。
//
//   入力
//     knob     : A0  GROVE ボリューム（0..1023）
//     sw       : A6  GROVE マイクロスイッチ（押すと 1。逆なら hardware.json の channels.sw に "invert": true）
//     distance : D6  GROVE 超音波距離センサ（cm。測れないときは 0）
//   出力
//     servo    : A2  SG90（0..1 = 0..180 度）
//     r, g, b  : D2(クロック) / D3(データ)  GROVE フルカラー LED v2.0（P9813。各 0..1）
//     tone     : D4  GROVE スピーカー（音の高さ Hz。0 で消音）
//     vol      : D4  スピーカーの音量（0..100。既定 35）
//
// ※UnoLink は 1 秒エンジンから何も来ないと出力を safe に戻す（LED と音は消え、サーボは真ん中へ）
#include <UnoLink.h>
#include <Servo.h>

static const int PIN_KNOB = A0;
static const int PIN_SERVO = A2;
static const int PIN_SW = A6;
static const int PIN_LED_CLK = 2;
static const int PIN_LED_DATA = 3;
static const int PIN_SPEAKER = 4;
static const int PIN_SONIC = 6;

UnoLink ulink("grove", 1);
Servo servo;

// ---- GROVE フルカラー LED（P9813）を自前で送る（ライブラリ不要）----
static uint8_t ledR = 0, ledG = 0, ledB = 0;
static bool ledDirty = true;

static void p9813Clock() {
  digitalWrite(PIN_LED_CLK, LOW);
  delayMicroseconds(20);
  digitalWrite(PIN_LED_CLK, HIGH);
  delayMicroseconds(20);
}

static void p9813Byte(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(PIN_LED_DATA, (b & 0x80) ? HIGH : LOW);
    p9813Clock();
    b <<= 1;
  }
}

static void p9813Send(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 0; i < 4; i++) p9813Byte(0x00);            // 開始（0 を 32 ビット）
  uint8_t flag = 0xC0;
  if ((b & 0x80) == 0) flag |= 0x20;
  if ((b & 0x40) == 0) flag |= 0x10;
  if ((g & 0x80) == 0) flag |= 0x08;
  if ((g & 0x40) == 0) flag |= 0x04;
  if ((r & 0x80) == 0) flag |= 0x02;
  if ((r & 0x40) == 0) flag |= 0x01;
  p9813Byte(flag);
  p9813Byte(b);
  p9813Byte(g);
  p9813Byte(r);
  for (uint8_t i = 0; i < 4; i++) p9813Byte(0x00);            // 終わり
}

// ---- 音量つきのスピーカー（tone() の代わり）----
// tone() は常に ON と OFF が半分ずつ（デューティ 50%）で、いちばん大きな音しか出ない。
// ON の時間を短くすると音の芯（基本波）が sin(π×デューティ) に比例して弱まり、小さく聞こえる。
// TCB0 の割り込みで ON / OFF の長さを別々に数える（TCB1 は tone、TCB2 は Servo、TCB3 は millis が使う。
// TCB0 は D6 の PWM 用だが、D6 は超音波センサで PWM を使わないので空いている）。8MHz（CLK_PER/2）で数える
static PORT_t* spkPort;
static uint8_t spkMask;
static volatile uint32_t spkOnTicks = 0, spkOffTicks = 0, spkRemain = 0;
static volatile bool spkOn = false;
static bool spkRunning = false;
static unsigned int spkHz = 0;
static uint8_t spkVol = 35;

ISR(TCB0_INT_vect) {
  TCB0.INTFLAGS = TCB_CAPT_bm;
  uint32_t t;
  if (spkRemain > 0) {
    t = spkRemain;                                    // 16 ビットに収まらない長い区間の続き
  } else {
    spkOn = !spkOn;
    if (spkOn) spkPort->OUTSET = spkMask; else spkPort->OUTCLR = spkMask;
    t = spkOn ? spkOnTicks : spkOffTicks;
  }
  const uint32_t n = t > 60000UL ? 60000UL : t;
  spkRemain = t - n;
  TCB0.CCMP = (uint16_t)(n - 1);
}

static void speakerApply() {
  if (spkHz < 31 || spkVol == 0) {
    TCB0.INTCTRL = 0;
    TCB0.CTRLA = 0;
    spkPort->OUTCLR = spkMask;
    spkOn = false;
    spkRunning = false;
    return;
  }
  // 音量は耳の感じ方に合わせて 2 乗（50 で約 -12dB、20 で約 -28dB）
  const float a = (spkVol / 100.0f) * (spkVol / 100.0f);
  const float duty = asinf(a) / (float)PI;                           // a=1 で 0.5
  const uint32_t period = 8000000UL / spkHz;
  uint32_t on = (uint32_t)(period * duty);
  if (on < 64) on = 64;                                              // 割り込みが間に合う最短（8µs）
  uint32_t off = period > on + 64 ? period - on : 64;
  noInterrupts();
  spkOnTicks = on;
  spkOffTicks = off;
  if (!spkRunning) {
    spkRemain = 0;
    spkOn = false;
    TCB0.CTRLA = 0;
    TCB0.CTRLB = TCB_CNTMODE_INT_gc;
    TCB0.CNT = 0;
    TCB0.CCMP = 1000;
    TCB0.INTFLAGS = TCB_CAPT_bm;
    TCB0.INTCTRL = TCB_CAPT_bm;
    TCB0.CTRLA = TCB_CLKSEL_CLKDIV2_gc | TCB_ENABLE_bm;
    spkRunning = true;
  }
  interrupts();
}

// ---- 出力を受け取ったとき ----
static void onOutput(const char* name, float v) {
  if (strcmp(name, "servo") == 0) {
    servo.write((int)(v * 180.0f + 0.5f));
  } else if (strcmp(name, "r") == 0) {
    ledR = (uint8_t)(v * 255.0f + 0.5f); ledDirty = true;
  } else if (strcmp(name, "g") == 0) {
    ledG = (uint8_t)(v * 255.0f + 0.5f); ledDirty = true;
  } else if (strcmp(name, "b") == 0) {
    ledB = (uint8_t)(v * 255.0f + 0.5f); ledDirty = true;
  } else if (strcmp(name, "tone") == 0) {
    spkHz = (unsigned int)(v + 0.5f);       // 31Hz 未満は消音
    speakerApply();
  } else if (strcmp(name, "vol") == 0) {
    spkVol = (uint8_t)(v + 0.5f);
    speakerApply();
  }
}

// ---- 超音波距離センサ（Grove は 1 本の線で送って受ける）----
static long measureCm() {
  pinMode(PIN_SONIC, OUTPUT);
  digitalWrite(PIN_SONIC, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_SONIC, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_SONIC, LOW);
  pinMode(PIN_SONIC, INPUT);
  const unsigned long us = pulseIn(PIN_SONIC, HIGH, 25000UL);   // 約 4m まで。来なければ 0
  return (long)(us / 29 / 2);
}

void setup() {
  pinMode(PIN_LED_CLK, OUTPUT);
  pinMode(PIN_LED_DATA, OUTPUT);
  pinMode(PIN_SPEAKER, OUTPUT);
  spkPort = digitalPinToPortStruct(PIN_SPEAKER);
  spkMask = digitalPinToBitMask(PIN_SPEAKER);
  servo.attach(PIN_SERVO);
  servo.write(90);
  p9813Send(0, 0, 0);

  ulink.addInput("knob", UL_INT, 0, 1023);
  ulink.addInput("sw", UL_BOOL);
  ulink.addInput("distance", UL_INT, 0, 400);
  ulink.addOutput("servo", UL_FLOAT, 0, 1, /*safe*/0.5f);
  ulink.addOutput("r", UL_FLOAT, 0, 1, 0);
  ulink.addOutput("g", UL_FLOAT, 0, 1, 0);
  ulink.addOutput("b", UL_FLOAT, 0, 1, 0);
  ulink.addOutput("tone", UL_INT, 0, 4000, 0);
  ulink.addOutput("vol", UL_INT, 0, 100, 35);
  ulink.onOutput(onOutput);              // begin の前に（起動直後の safe 適用も通知される）
  ulink.begin(Serial, 115200);
}

static int lastKnob = -100;
static unsigned long lastSonicMs = 0;

void loop() {
  const int knob = analogRead(PIN_KNOB);
  if (abs(knob - lastKnob) >= 4) { lastKnob = knob; ulink.send("knob", knob); }   // 細かい揺れは送らない

  ulink.send("sw", analogRead(PIN_SW) > 512 ? 1 : 0);

  const unsigned long now = millis();
  if (now - lastSonicMs >= 60) {          // 超音波は 60ms ごと（前の音の跳ね返りと混ざらないように）
    lastSonicMs = now;
    ulink.send("distance", measureCm());
  }

  if (ledDirty) { ledDirty = false; p9813Send(ledR, ledG, ledB); }

  ulink.update();                         // 受信・?ping 応答・心拍監視・変化分の送信
  delay(2);
}
