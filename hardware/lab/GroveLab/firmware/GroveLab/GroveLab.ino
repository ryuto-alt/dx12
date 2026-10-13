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
//     rpm      : D4  エンジン音（回転数。0 で止まる。tone が鳴っている間は tone が優先）
//     load     : D4  エンジンの負荷（0..1。アクセル。抜くと細く静かになり、高回転ではたまに「パン」と鳴る）
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

// 1 色送るのに 96 クロック。半周期 20µs だと 3.8ms 止まって受信があふれやすいので 5µs（約 1ms。P9813 は 15MHz まで受ける）
static void p9813Clock() {
  digitalWrite(PIN_LED_CLK, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_LED_CLK, HIGH);
  delayMicroseconds(5);
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

// ---- エンジン音 ----
// 半分 ON の矩形波だと「ピー」というブザーにしかならない。本物の排気音は爆発ごとの短い圧力の脈の列なので、
// 爆発 1 回ごとに細いパルスを 1 本出す（4 気筒 4 ストローク: 1 秒の爆発数 = 回転数 / 30）。
// 気筒ごとに強さを変えると 1/4 の低いうなり（ドロドロ）が出て、間隔と強さに毎回むらを足すとガラガラした粗さになる
static volatile bool engMode = false;
static volatile uint32_t engPeriod = 0, engOnBase = 0, engJitter = 0;
static volatile bool engPop = false;
static volatile uint16_t engRng = 0xACE1;
static volatile uint8_t engCyl = 0;
static unsigned int engRpm = 0;
static float engLoad = 0;
static const uint8_t ENG_SHAPE[4] = { 255, 200, 235, 180 };   // 気筒ごとの強さ（/256）

// 爆発 1 回ぶんの ON / OFF の長さを決める（割り込みの中で、パルスを立てる直前に呼ぶ）
static inline void engNextCycle() {
  uint16_t r = engRng;
  r ^= r << 7; r ^= r >> 9; r ^= r << 8;                            // xorshift16
  engRng = r;
  uint32_t on = (engOnBase * ENG_SHAPE[engCyl]) >> 8;
  engCyl = (engCyl + 1) & 3;
  on = (on * (224 + (r & 31))) >> 8;                                 // 脈ごとの強さのむら（0〜-12%）
  if (engPop && (r >> 12) == 0) on <<= 1;                            // アクセルを抜いた高回転でたまに「パン」（1/16）
  if (on < 64) on = 64;
  const int32_t jit = (((int32_t)((r >> 4) & 0xFF) - 128) * (int32_t)engJitter) >> 7;   // 間隔のむら ±engJitter
  int32_t off = (int32_t)engPeriod - (int32_t)on + jit;
  if (off < 64) off = 64;
  spkOnTicks = on;
  spkOffTicks = (uint32_t)off;
}

ISR(TCB0_INT_vect) {
  TCB0.INTFLAGS = TCB_CAPT_bm;
  uint32_t t;
  if (spkRemain > 0) {
    t = spkRemain;                                    // 16 ビットに収まらない長い区間の続き
  } else {
    spkOn = !spkOn;
    if (spkOn) {
      if (engMode) engNextCycle();
      spkPort->OUTSET = spkMask;
    } else {
      spkPort->OUTCLR = spkMask;
    }
    t = spkOn ? spkOnTicks : spkOffTicks;
  }
  const uint32_t n = t > 60000UL ? 60000UL : t;
  spkRemain = t - n;
  TCB0.CCMP = (uint16_t)(n - 1);
}

static void speakerApply() {
  const bool toneOn = spkHz >= 31 && spkVol > 0;
  const bool engOn = !toneOn && engRpm >= 300 && spkVol > 0;
  if (!toneOn && !engOn) {
    TCB0.INTCTRL = 0;
    TCB0.CTRLA = 0;
    spkPort->OUTCLR = spkMask;
    spkOn = false;
    spkRunning = false;
    engMode = false;
    return;
  }
  // 音量は耳の感じ方に合わせて 2 乗（50 で約 -12dB、20 で約 -28dB）
  const float a = (spkVol / 100.0f) * (spkVol / 100.0f);
  const float duty = asinf(a) / (float)PI;                           // a=1 で 0.5
  uint32_t on = 0, off = 0, ePeriod = 0, eOn = 0;
  if (toneOn) {
    const uint32_t period = 8000000UL / spkHz;
    on = (uint32_t)(period * duty);
    if (on < 64) on = 64;                                            // 割り込みが間に合う最短（8µs）
    off = period > on + 64 ? period - on : 64;
  } else {
    ePeriod = 240000000UL / engRpm;                                  // 8MHz × 30 / 回転数
    // パルスの太さ = 強さ。全開で周期の 30%、アクセルを抜くと 8%（音量 100 のとき）
    eOn = (uint32_t)(ePeriod * (duty * 2.0f) * (0.08f + 0.22f * engLoad));
  }
  noInterrupts();
  engMode = !toneOn;
  if (toneOn) {
    spkOnTicks = on;
    spkOffTicks = off;
  } else {
    engPeriod = ePeriod;
    engOnBase = eOn;
    engJitter = ePeriod / 40;                                        // ±2.5%
    engPop = engLoad < 0.25f && engRpm > 4000;
    if (!spkRunning) { spkOnTicks = 64; spkOffTicks = ePeriod; }
  }
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
  } else if (strcmp(name, "rpm") == 0) {
    engRpm = (unsigned int)(v + 0.5f);       // 300 未満は止める
    speakerApply();
  } else if (strcmp(name, "load") == 0) {
    engLoad = v < 0 ? 0 : (v > 1 ? 1 : v);
    speakerApply();
  }
}

// ---- 超音波距離センサ（Grove は 1 本の線で送って受ける）----
// pulseIn で待つと跳ね返りの間（最大 25ms）loop が止まり、その間に PC から来た命令が受信バッファ（64 バイト）から
// あふれて音や LED の命令が欠ける（音が飛ぶ）。打ったら割り込みで返りのパルス幅を測り、loop は止めない
static volatile unsigned long sonicRiseUs = 0, sonicWidthUs = 0;
static volatile bool sonicRose = false, sonicDone = false;
static bool sonicBusy = false;
static unsigned long sonicStartMs = 0;

static void onSonicEdge() {
  const unsigned long t = micros();
  if (digitalRead(PIN_SONIC) == HIGH) { sonicRiseUs = t; sonicRose = true; }
  else if (sonicRose && !sonicDone) { sonicWidthUs = t - sonicRiseUs; sonicDone = true; }
}

static void sonicTrigger() {
  pinMode(PIN_SONIC, OUTPUT);
  digitalWrite(PIN_SONIC, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_SONIC, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_SONIC, LOW);
  pinMode(PIN_SONIC, INPUT);
  sonicRose = false;
  sonicDone = false;
  attachInterrupt(digitalPinToInterrupt(PIN_SONIC), onSonicEdge, CHANGE);
  sonicBusy = true;
  sonicStartMs = millis();
}

// 測り終わったら true（cm。測れないときは 0）
static bool sonicPoll(long& cm) {
  if (!sonicBusy) return false;
  if (sonicDone) {
    cm = (long)(sonicWidthUs / 29 / 2);
  } else if (millis() - sonicStartMs >= 30) {   // 約 4m を超えた / センサが無い
    cm = 0;
  } else {
    return false;
  }
  detachInterrupt(digitalPinToInterrupt(PIN_SONIC));
  sonicBusy = false;
  return true;
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
  ulink.addOutput("rpm", UL_INT, 0, 12000, 0);
  ulink.addOutput("load", UL_FLOAT, 0, 1, 0);
  ulink.onOutput(onOutput);              // begin の前に（起動直後の safe 適用も通知される）
  ulink.begin(Serial, 115200);
}

static int lastKnob = -100;
static unsigned long lastSonicMs = 0;

void loop() {
  const int knob = analogRead(PIN_KNOB);
  if (abs(knob - lastKnob) >= 4) { lastKnob = knob; ulink.send("knob", knob); }   // 細かい揺れは送らない

  ulink.send("sw", analogRead(PIN_SW) > 512 ? 1 : 0);

  long cm;
  if (sonicPoll(cm)) ulink.send("distance", cm);
  const unsigned long now = millis();
  if (!sonicBusy && now - lastSonicMs >= 60) {   // 超音波は 60ms ごと（前の音の跳ね返りと混ざらないように）
    lastSonicMs = now;
    sonicTrigger();
  }

  if (ledDirty) { ledDirty = false; p9813Send(ledR, ledG, ledB); }

  ulink.update();                         // 受信・?ping 応答・心拍監視・変化分の送信
  delay(2);
}
