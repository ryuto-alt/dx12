// ArduinoLab — Uno Engine の実験用スケッチ（名前 "lab"）
//   エンジン側は hardware/lab/ArduinoLab プロジェクト（assets/hardware.json の "lab"）。
//   ここを書き換えて自由に実験してよい。チャンネルを足したら、ゲーム側は Lua で hw.device("lab"):get("名前")。
//
//   ESP32 DevKit（部品なしで btn / touch / led の 3 つが試せる）
//     btn   : GPIO0 = 基板の BOOT ボタン（押すと 1）
//     pot   : GPIO34 = 可変抵抗の中央の足（両端は 3V3 と GND）。つながないと値がふらつく
//     touch : GPIO4  = ジャンパー線を 1 本挿して、反対側の金属を指で触る（生の値。触ると小さくなる）
//     led   : GPIO2  = 基板の青い LED（0..1 の明るさ）
//   Arduino Uno / Nano
//     btn   : D2（ボタンを D2 と GND の間に。押すと 1）
//     pot   : A0（可変抵抗の中央の足。両端は 5V と GND）
//     led   : D9 に LED（+ 220Ω）で明るさ、基板の L(D13) は 0.5 以上で点灯
//
// ※変数名を link にしない（ESP32 では POSIX の link() と衝突する）
// ※UnoLink は 1 秒エンジンから何も来ないと出力を safe(=0) に戻す（ゲームが落ちても LED が点きっぱなしにならない）
#include <UnoLink.h>

#if defined(ARDUINO_ARCH_ESP32)
static const int PIN_BTN = 0;
static const int PIN_POT = 34;
static const int PIN_TOUCH = 4;
static const int PIN_LED = 2;
static const int POT_MAX = 4095;
#else
static const int PIN_BTN = 2;
static const int PIN_POT = A0;
static const int PIN_LED = 9;
static const int POT_MAX = 1023;
#endif

UnoLink ulink("lab", 1);

static void onOutput(const char* name, float v) {
  if (strcmp(name, "led") == 0) {
#if defined(ARDUINO_ARCH_ESP32)
    ledcWrite(PIN_LED, (uint32_t)(v * 1023.0f + 0.5f));   // 10bit PWM
#else
    analogWrite(PIN_LED, (int)(v * 255.0f + 0.5f));
    digitalWrite(LED_BUILTIN, v >= 0.5f ? HIGH : LOW);
#endif
  }
}

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
#if defined(ARDUINO_ARCH_ESP32)
  ledcAttach(PIN_LED, 5000, 10);         // arduino-esp32 3.x
  ledcWrite(PIN_LED, 0);
#else
  pinMode(PIN_LED, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  ulink.addInput("btn", UL_BOOL);
  ulink.addInput("pot", UL_INT, 0, POT_MAX);
#if defined(ARDUINO_ARCH_ESP32)
  ulink.addInput("touch", UL_INT);       // 範囲は hardware.json の channels.touch で校正する
#endif
  ulink.addOutput("led", UL_FLOAT, 0, 1, /*safe*/0);
  ulink.onOutput(onOutput);              // begin の前に
  ulink.begin(Serial, 115200);
}

static int lastPot = -100;

void loop() {
  ulink.send("btn", digitalRead(PIN_BTN) == LOW ? 1 : 0);

  int pot = analogRead(PIN_POT);
  if (abs(pot - lastPot) >= 8) { lastPot = pot; ulink.send("pot", pot); }   // ADC の細かい揺れは送らない

#if defined(ARDUINO_ARCH_ESP32)
  ulink.send("touch", (int)touchRead(PIN_TOUCH));
#endif

  ulink.update();                        // 受信・?ping 応答・心拍監視・変化分の送信
  delay(5);
}
