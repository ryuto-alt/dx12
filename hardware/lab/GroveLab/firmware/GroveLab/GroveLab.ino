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
    const unsigned int hz = (unsigned int)(v + 0.5f);
    if (hz >= 31) tone(PIN_SPEAKER, hz);   // tone は 31Hz 未満を出せない
    else noTone(PIN_SPEAKER);
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
