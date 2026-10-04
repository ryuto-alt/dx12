// UnoLink - Uno Engine (dx12) とシリアルでつなぐ Arduino / ESP32 ライブラリ
// プロトコル v1 の仕様: dx12/docs/HARDWARE.md §1
//
//   UnoLink ulink("radio", 1);   // ※ 変数名 link は ESP32 で POSIX link() と衝突するので避ける
//   link.addInput("dial", UL_INT, 0, 4095);
//   link.addOutput("led", UL_FLOAT, 0, 1, 0);
//   link.onOutput([](const char* n, float v){ ... });
//   link.begin(Serial, 115200);
//   loop(){ ulink.send("dial", analogRead(34)); ulink.update(); }
#pragma once
#include <Arduino.h>
#include <stdarg.h>

// ---- 設定（スケッチの #include より前に -D / #define で上書き可）----
#ifndef UL_MAX_CH
  #if defined(ARDUINO_ARCH_AVR)
    #define UL_MAX_CH 12        // AVR は RAM 2KB なので少なめ
  #else
    #define UL_MAX_CH 16
  #endif
#endif
#ifndef UL_NAME_MAX
  #if defined(ARDUINO_ARCH_AVR)
    #define UL_NAME_MAX 16      // プロトコル上限は 24
  #else
    #define UL_NAME_MAX 24
  #endif
#endif
#define UL_LINE_MAX      128    // 1 行の最大バイト数（\n 含む）。プロトコル固定値
#define UL_FAILSAFE_MS   1000   // この時間無受信で全出力を safe 値へ
#define UL_RESEND_MS     500    // 入力全値の再送間隔
#define UL_DEFAULT_MAX_HZ 200   // 1 チャンネルあたりの送信レート上限

enum UnoLinkType : uint8_t { UL_BOOL = 0, UL_INT = 1, UL_FLOAT = 2 };

class UnoLink {
public:
  typedef void (*OutputCallback)(const char* name, float value);
  typedef void (*CommandCallback)(const char* line);   // 未知の ? 行（?pin など）

  // name は [a-z0-9_]{1,24}。文字列はスケッチ側が保持（リテラル推奨）
  explicit UnoLink(const char* name, uint8_t fw = 1);

  // チャンネル宣言。戻り値はチャンネル番号、失敗（名前不正・重複・満杯）は -1。
  // begin() 後に足したら @ch を即送る。
  int addInput(const char* name, UnoLinkType type);
  int addInput(const char* name, UnoLinkType type, float minV, float maxV);
  int addOutput(const char* name, UnoLinkType type, float safeV = 0);
  int addOutput(const char* name, UnoLinkType type, float minV, float maxV, float safeV = 0);

  void begin(HardwareSerial& s, unsigned long baud = 115200);
  void begin(Stream& s);          // USB CDC など baud 不要なもの
  void update();                  // loop() で毎回呼ぶ

  // ---- 入力（デバイス → エンジン）----
  // 値を記録する。update() が「変化したチャンネルだけ」を 1 行にまとめて送る
  // （send を続けて呼べば同じ行＝同時刻）。force=true で同値でも送る。
  bool send(const char* name, float value, bool force = false);
  bool sendIndex(int ch, float value, bool force = false);
  bool sendMany(const char* const* names, const float* values, uint8_t n);
  void flush();                   // 変化分を今すぐ送る（レート上限は守る）
  void setMaxRate(uint16_t hz);   // 1 チャンネルの上限 Hz（既定 200）

  // ---- 出力（エンジン → デバイス）----
  void onOutput(OutputCallback cb) { _outCb = cb; }
  void onCommand(CommandCallback cb) { _cmdCb = cb; }
  float getOutput(const char* name) const;   // 無ければ 0
  float getOutputIndex(int ch) const;

  // ---- 状態 ----
  int  findChannel(const char* name) const;  // 無ければ -1
  uint8_t channelCount() const { return _count; }
  const char* channelName(int ch) const;
  bool isOutput(int ch) const;
  bool connected() const;         // 直近 1000ms 以内に何か受信した
  bool failsafe() const { return _failsafe; }
  uint16_t errorCount() const { return _errCount; }

  // ---- ログ ----
  void log(const char* text);                  // @log
  void log(const __FlashStringHelper* text);
  void logf(const char* fmt, ...);
  void error(const char* text);                // @err（エラー数に数える）
  void error(const __FlashStringHelper* text);

  void sendHello();               // @hello → @ch... → @ready

private:
  struct Channel {
    char name[UL_NAME_MAX + 1];
    uint8_t dir;      // 0=in 1=out
    uint8_t type;
    uint8_t flags;    // bit0 hasRange, bit1 sent 済み, bit2 force
    float minV, maxV, safeV;
    float value;      // 入力: 現在値 / 出力: 最後に受けた値
    float lastSent;
    uint32_t lastSentMs;
  };
  enum { F_RANGE = 1, F_SENT = 2, F_FORCE = 4 };

  int addChannel(const char* name, uint8_t dir, UnoLinkType t, bool range, float mn, float mx, float safeV);
  void sendChannel(uint8_t i);
  void flushInputs(bool all);
  void handleLine(char* line);
  void setOutput(uint8_t i, float v);
  void applySafe();
  void emitText(bool isErr, const char* text, bool progmem);

  const char* _name; uint8_t _fw;
  Stream* _s;
  Channel _ch[UL_MAX_CH];
  uint8_t _count;
  char _rx[UL_LINE_MAX]; uint8_t _rxLen; bool _rxOver;
  uint32_t _lastRxMs, _lastFullMs;
  uint16_t _minIntervalMs;
  uint16_t _errCount;
  bool _safeState, _failsafe, _gotRx, _hello;
  OutputCallback _outCb; CommandCallback _cmdCb;
};
