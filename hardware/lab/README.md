# ArduinoLab — ESP32 / Arduino をつないで Uno Engine で遊ぶ実験キット

ボードのボタン・ダイヤル・タッチセンサーで画面の中の物を動かし、ゲームの中の出来事で本物の LED を光らせます。
**ESP32 なら部品なし（USB ケーブル 1 本）で 3 つの実験ができます。**

```
 [ESP32]                              [Uno Engine（ArduinoLab プロジェクト）]
  BOOT ボタン (GPIO0)  ── btn ──▶  青い球が跳ねて光る          （実験 1）
  可変抵抗   (GPIO34)  ── pot ──▶  金色の棒が回る              （実験 2）
  導線に触る (GPIO4)   ── touch ─▶ ランプが灯る                （実験 3）
  青い LED   (GPIO2)   ◀── led ── ボタン / 呼吸 / ダイヤル / タッチ（実験 4、1〜4 キーで切替）
```

ボードが無くても動きます（キーボードで代用: Space=ボタン、← →=ダイヤル、T=タッチ）。

## 用意するもの

- Windows 10 / 11 の PC（DirectX 12 が動く GPU）
- ESP32 DevKit（または Arduino Uno / Nano）と、**データ通信できる** USB ケーブル（充電専用ケーブルは不可）
- （あれば）可変抵抗 10kΩ・ジャンパー線

## 手順（3 ステップ）

### 0. このブランチを取ってくる

```powershell
git clone -b arduino-test https://github.com/ryuto-alt/dx12.git
cd dx12\hardware\lab
```

git が無ければ GitHub のブランチのページで **Code → Download ZIP** でも構いません（展開して `hardware\lab` を開く）。

### 1. `setup.bat` をダブルクリック（初回だけ・10 分くらい）

- エンジン（Uno Engine v2.4.0、約 50MB）を `engine\` にダウンロード
  （ソースから自分でビルドした `build\release\DX12Engine.exe` があればそちらを使います）
- arduino-cli を `tools\` にダウンロード（管理者権限は不要）
- ESP32 / Arduino のボード定義を入れる
- つながっているボードの COM ポートを表示

ボードが表示されないときは USB ドライバーを入れてください（ボードの裏の小さいチップの型番で選ぶ）:
CH340 → https://www.wch-ic.com/downloads/CH341SER_EXE.html / CP2102 → https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

### 2. `flash.bat` をダブルクリック（ボードに書き込む）

`firmware\ArduinoLab\ArduinoLab.ino` をコンパイルして書き込みます。ポートは自動で探します。

- Arduino Uno なら、コマンドプロンプトで `flash.bat uno`（Nano なら `flash.bat nano`）
- ESP32 で `Failed to connect` / `Wrong boot mode` が出たら、`Connecting....` の間 **基板の BOOT ボタンを押しっぱなし**にしてもう一度
- **エンジンを起動したままだと書き込めません**（ポートの取り合いになる）。先にエンジンを閉じる

### 3. `start.bat` をダブルクリック → ▶ Play（または F5）

エンジンが ArduinoLab プロジェクトを開いた状態で起動します。▶ Play を押すと、左上の表示が
**「● lab に接続中」** になれば成功。BOOT ボタンを押すと青い球が跳ね、基板の青い LED も光ります。

## 実験

| # | やること | 画面 | 配線（ESP32） |
|---|---|---|---|
| 1 | BOOT ボタンを押す | 青い球が跳ねて光る。回数を数える | 不要（基板のボタン） |
| 2 | 可変抵抗を回す | 金色の棒が回る | 可変抵抗の中央の足 → GPIO34、両端 → 3V3 と GND |
| 3 | 導線の先を指で触る | ランプが灯って床を照らす | GPIO4 にジャンパー線を 1 本挿すだけ |
| 4 | 1〜4 キーで切替 | 基板の LED がボタン / 呼吸 / ダイヤル / タッチに合わせて光る | 不要（基板の LED） |

Arduino Uno のとき: ボタン D2（D2 と GND の間）、可変抵抗 A0（両端は 5V と GND）、LED D9（220Ω を通して GND へ）。
タッチは ESP32 だけです。

### うまく値が出ないとき

- 可変抵抗をつないでいない GPIO34 は値がふらつきます（故障ではない）
- タッチの効き具合は人と線で変わります。`ArduinoLab\assets\hardware.json` の `touch` の `min`（触ったときの値）/ `max`（触っていないときの値）を、
  画面左上の「生の値 touch=…」を見て書き換え、エンジンを開き直してください
- 「未接続」のまま → ボードを挿し直す / 書き込みし直す / Arduino IDE のシリアルモニターなど、同じ COM を開いているものを閉じる

## 自分で改造する

| 変えたいもの | ファイル |
|---|---|
| ボードの動き・つなぐセンサー | `firmware\ArduinoLab\ArduinoLab.ino`（書き換えたら `flash.bat`） |
| 画面の中の反応 | `ArduinoLab\assets\components\Hw*.lua`（保存すると Play 中でも差し替わる） |
| 値の範囲・反転・なめらかさ | `ArduinoLab\assets\hardware.json` の `channels` |
| ボタンをゲームの操作に割り当てる | `hardware.json` の `actions`（例 `"btn": "Jump"`） |

センサーを足す例（スケッチ側）:

```cpp
ulink.addInput("light", UL_INT, 0, 4095);   // setup() で宣言（begin の前）
ulink.send("light", analogRead(35));        // loop() で送る
```

ゲーム側（Lua コンポーネント）:

```lua
local lab = hw.device("lab")
function OnUpdate(self, dt)
  if lab.connected then
    local v = lab:get("light")      -- 0..1 に直った値
    lab:set("led", v)               -- LED へ 0..1 で出す
  end
end
```

出力は 1 秒エンジンから何も来ないと自動で 0 に戻ります（ゲームが落ちても LED やモーターが動きっぱなしにならない）。

## もっと詳しく

- プロトコル・Lua API・hardware.json の全項目: [`docs/HARDWARE.md`](../../docs/HARDWARE.md)
- 通信ライブラリ UnoLink とほかの例: [`hardware/firmware/README.md`](../firmware/README.md)
- Python でボードと直接話す確認ツール: `python ..\firmware\tools\hwprobe.py COM5 --seconds 5`（`pip install pyserial`）

## ソースからビルドする人へ

ルートの `README.md` の手順で `build\release\DX12Engine.exe` を作れば、`start.bat` はそちらを優先して使います
（Visual Studio 2026 + vcpkg が要るので、実験だけならリリース版で十分です）。
