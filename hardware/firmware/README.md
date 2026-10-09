# ファームウェア（UnoLink）

Uno Engine と Arduino / ESP32 をシリアルでつなぐ側。プロトコルの正は [`docs/HARDWARE.md`](../../docs/HARDWARE.md) §1（プロトコル v1）。

| 場所 | 内容 |
|---|---|
| `UnoLink/` | Arduino ライブラリ（AVR の Uno/Nano と ESP32 の両対応） |
| `UnoLink/examples/UnoLinkDemo` | ESP32 用デモ（名前 `demo`）。BOOT ボタン + ADC(GPIO34) + 内蔵 LED(GPIO2, PWM) |
| `UnoLink/examples/UnoLinkBasicUno` | Uno/Nano 用の最小例 |
| `UnoLinkGeneric/` | 汎用スケッチ。書くだけで、エンジンから `?pin` でピンをチャンネルにできる |
| `tools/hwprobe.py` | 実機確認ツール（pyserial） |
| `../lab/` | **ArduinoLab 実験キット**（セットアップ・書き込み・起動の bat と、すぐ遊べるエンジンのプロジェクト） |

## 配線（UnoLinkDemo / ESP32 DevKit）

- ボタン: GPIO0（BOOT ボタン。INPUT_PULLUP、押すと 0 なので反転して `btn` に送る）
- ダイヤル: GPIO34 の ADC（可変抵抗の中点を 34、両端を 3V3 / GND。無接続だと値がふらつく）
- LED: GPIO2（内蔵 LED）。出力 `led` は 0..1 の PWM

## 書き込み（arduino-cli）

```powershell
$cli = "C:\Program Files\Arduino CLI\arduino-cli.exe"
& $cli core install esp32:esp32        # ESP32 は arduino-esp32 3.x（ledcAttach を使う）
& $cli core install arduino:avr        # Uno / Nano

$lib = "<dx12>\hardware\firmware\UnoLink"
# ESP32 デモ
& $cli compile --upload -p COM8 --fqbn esp32:esp32:esp32 --library $lib --jobs 4 UnoLink\examples\UnoLinkDemo
# 汎用スケッチ
& $cli compile --upload -p COM8 --fqbn esp32:esp32:esp32 --library $lib --jobs 4 UnoLinkGeneric
# Uno
& $cli compile --upload -p COM3 --fqbn arduino:avr:uno --library $lib UnoLink\examples\UnoLinkBasicUno
```

ライブラリを Arduino IDE から使うなら `UnoLink` フォルダーを `Documents\Arduino\libraries\` へコピーする。
**変数名を `link` にしない**（ESP32 では POSIX の `link()` と衝突してコンパイルエラーになる。例は `ulink`）。

## 動作確認

```powershell
python tools\hwprobe.py COM8 --seconds 5                       # @hello〜@ready と値の流れ
python tools\hwprobe.py COM8 "!led=0.5" --ping-stop 2 --seconds 6   # 2 秒後に ?ping を止め、1 秒後に safe へ戻るか
python tools\hwprobe.py COM8 "?pin 2 pwm led" "?pin 34 adc dial"    # 汎用スケッチ
```

## ライブラリ API

```cpp
UnoLink ulink("radio", /*fw*/1);
ulink.addInput("dial", UL_INT, 0, 4095);       // 戻り値はチャンネル番号（失敗 -1）
ulink.addInput("btn", UL_BOOL);
ulink.addOutput("led", UL_FLOAT, 0, 1, /*safe*/0);
ulink.onOutput([](const char* name, float v){ ... });   // begin の前に
ulink.begin(Serial, 115200);
// loop:
ulink.send("dial", v);      // 値を記録。update() が変化分だけを 1 行にまとめて送る
ulink.update();             // 毎回呼ぶ（受信・?hello/?ping・心拍監視・送信）
```

- `send(name, v, force)` / `sendIndex(ch, v)` / `sendMany(names, values, n)` / `flush()` / `setMaxRate(hz)`
- `getOutput(name)`、`onCommand(cb)`（`?pin` など未知の `?` 行）、`connected()` / `failsafe()`
- `log("...")`（`@log`）、`logf(fmt, ...)`、`error("...")`（`@err`）
- 入力は変化時のみ送り、500ms ごとに全値を再送。1 チャンネルは最大 200Hz
- **1000ms 何も受信しなければ全出力を safe 値にして `onOutput` を呼ぶ**（`@log failsafe: outputs safe` も出る）。起動直後も safe
- `UL_MAX_CH`（ESP32 16 / AVR 12）、`UL_NAME_MAX`（ESP32 24 / AVR 16）は `-D` で変更可

## プロトコルの要約

デバイス → エンジン: `@hello name=.. proto=1 fw=.. board=..` / `@ch <name> <in|out> <bool|int|float> [min max] [safe=v]` / `@ready` / `name=v ...` / `@pong <seq>` / `@log` / `@err`
エンジン → デバイス: `?hello` / `?ping <seq>` / `!name=v ...` / `!safe` / `?pin <pin> <mode> <name>`（汎用のみ）

詳細は HARDWARE.md §1。

## 決めたこと（仕様で曖昧だった点）

- `@ready` の後に `@ch` を足したとき（汎用スケッチの `?pin`、begin 後の addInput）は、`@hello` は送らず `@ch` 1 行だけを即送る。同じ `?pin` の再送は `@hello` から全部送り直す
- 出力値は宣言の `[min,max]` に丸める。`bool` は 0/1、`int` は四捨五入。実数は小数 4 桁まで
- 汎用スケッチの `in`/`in_pullup` は生の 0/1（反転は hardware.json の `invert`）。`adc` は ±2 未満の揺れを送らない。`touch` は範囲宣言なし
- 汎用スケッチは ESP32 の GPIO6..11（フラッシュ）、AVR の D0/D1、入力専用ピンへの出力を `@err pin unusable` で断る（GPIO6 を ledcAttach すると実機で落ちる）
- 行が長すぎる（126 文字超）受信は捨てて `@err line too long`
