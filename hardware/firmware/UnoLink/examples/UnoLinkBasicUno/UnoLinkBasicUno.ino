// UnoLinkBasicUno - Arduino Uno / Nano 用の最小例（名前 "unobasic"）
//   入力 btn : D2（INPUT_PULLUP、押すと LOW → 反転して送る）
//   入力 pot : A0（0..1023）
//   出力 led : D9 PWM（0..1）。通信が 1 秒途絶えると 0 に戻る
// AVR は RAM が少ないので UL_MAX_CH は 12 本まで（#define UL_MAX_CH で変更可）。
#include <UnoLink.h>

UnoLink ulink("unobasic", 1);

static void onOutput(const char* name, float v) {
  if (strcmp(name, "led") == 0) analogWrite(9, (int)(v * 255.0f + 0.5f));
}

void setup() {
  pinMode(2, INPUT_PULLUP);
  pinMode(9, OUTPUT);
  ulink.addInput("btn", UL_BOOL);
  ulink.addInput("pot", UL_INT, 0, 1023);
  ulink.addOutput("led", UL_FLOAT, 0, 1, 0);
  ulink.onOutput(onOutput);
  ulink.begin(Serial, 115200);
}

void loop() {
  ulink.send("btn", digitalRead(2) == LOW ? 1 : 0);
  ulink.send("pot", analogRead(A0));
  ulink.update();
}
