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

## 手順（Uno Engine を入れるだけ）

### 1. Uno Engine v2.5.0 以降を入れる

- まだ入っていない: [Releases](https://github.com/ryuto-alt/dx12/releases/latest) から `dx12-engine-v*.zip` を落として好きな場所に展開し、`DX12Engine.exe` を起動
- もう入っている: 起動すれば自動で更新されます

更新後に最初に起動すると、ランチャーのプロジェクト一覧に **ArduinoLab** が出ます
（実体は `ドキュメント\UnoProjects\ArduinoLab`。最初の 1 回だけ置き、消しても置き直しません）。

### 2. ArduinoLab を開く → ボードを USB でつなぐ

開くと、書き込みに使う道具（arduino-cli と ESP32 / Arduino のボード定義）を**裏で自動で入れます**
（初回だけ数分。右下に「準備しています」→「準備できました」と出ます。管理者権限は要りません）。

まだ書き込んでいないボードを挿すと **「ハードウェア」窓が自動で開きます**（メニュー「ツール > ハードウェア」でもいつでも開けます）。

### 3. 「ボードに書き込む」を押す

ポートとボードの種類は自動で選ばれます（違っていたら選び直す）。エンジンを開いたままで書き込めます。

- ESP32 で書き込みが始まらない / `Failed to connect` → `Connecting....` の間 **基板の BOOT ボタンを押しっぱなし**にしてもう一度
- 書き込みが終わると自動でつなぎ直し、デバイス `lab` が「接続中」になります

### 4. ▶ Play（または F5）

左上の表示が **「● lab に接続中」** になれば成功。BOOT ボタンを押すと青い球が跳ね、基板の青い LED も光ります。

ボードが「ハードウェア」窓のポート一覧に出ないときは USB ドライバーを入れてください（ボードの裏の小さいチップの型番で選ぶ）:
CH340 → https://www.wch-ic.com/downloads/CH341SER_EXE.html / CP2102 → https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
充電専用の USB ケーブルでは通信できません。

### （別の方法）bat でやる

このフォルダの bat でも同じことができます（エンジンのソースを clone して試す人向け）。

1. `setup.bat` … エンジン（リリース版）と arduino-cli を `engine\` `tools\` に落とし、ボード定義を入れる
2. `flash.bat`（Uno なら `flash.bat uno`）… `ArduinoLab\firmware\ArduinoLab` を書き込む。エンジンを閉じてから
3. `start.bat` … ArduinoLab を開いた状態でエンジンを起動

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
| ボードの動き・つなぐセンサー | `ArduinoLab\firmware\ArduinoLab\ArduinoLab.ino`（書き換えたら「ハードウェア」窓で書き込み） |
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
