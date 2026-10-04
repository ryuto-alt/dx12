// ※ 変数名を link にしない: ESP32 では POSIX の link() と衝突する
// UnoLinkDemo - ESP32 用デモ（名前 "demo"）
//   入力 btn : GPIO0 (BOOT ボタン)。INPUT_PULLUP で押すと 0 なので反転して送る
//   入力 pot : GPIO34 の ADC (0..4095)。ボリューム等をつなぐ（無接続だとふらつく）
//   出力 led : GPIO2 の内蔵 LED を PWM で（0..1）。通信が 1 秒途絶えると 0 に戻る
#include <UnoLink.h>

static const int PIN_BTN = 0;
static const int PIN_POT = 34;
static const int PIN_LED = 2;

UnoLink ulink("demo", 1);

static void onOutput(const char* name, float v) {
  if (strcmp(name, "led") == 0) {
    ledcWrite(PIN_LED, (uint32_t)(v * 1023.0f + 0.5f));   // 10bit
    ulink.logf("led=%d.%02d", (int)v, (int)(v * 100) % 100);  // 確認用（エンジンのログに出る）
  }
}

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  ledcAttach(PIN_LED, 5000, 10);        // arduino-esp32 3.x。2.x の ledcSetup は無い
  ledcWrite(PIN_LED, 0);

  ulink.addInput("btn", UL_BOOL);
  ulink.addInput("pot", UL_INT, 0, 4095);
  ulink.addOutput("led", UL_FLOAT, 0, 1, /*safe*/0);
  ulink.onOutput(onOutput);              // begin の前に（起動直後の safe 適用も通知される）
  ulink.begin(Serial, 115200);
}

void loop() {
  ulink.send("btn", digitalRead(PIN_BTN) == LOW ? 1 : 0);
  ulink.send("pot", analogRead(PIN_POT));
  ulink.update();                        // 受信・?ping 応答・心拍監視・変化分の送信
  delay(2);
}
