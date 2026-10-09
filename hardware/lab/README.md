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

> **Arduino Nano Every + Grove のキットなら [GroveLab](#grovelab--arduino-nano-every--grove-のキット)** へ（サーボ・フルカラー LED・スピーカー・超音波センサ）。

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

## GroveLab — Arduino Nano Every + Grove のキット

v2.5.1 から、ArduinoLab と並んでプロジェクト一覧に **GroveLab** が出ます（`ドキュメント\UnoProjects\GroveLab`）。
手順は ArduinoLab と同じです（開く → 道具が自動で入る → ボードを挿す → 「ハードウェア」窓でボード **Arduino Nano Every** を選んで書き込む → ▶ Play）。

| 部品（Grove Shield for Arduino Nano に挿す） | 端子 | 画面 / 動き |
|---|---|---|
| GROVE ボリューム | A0 | 画面の針と本物のサーボが同じ角度に動く（実験 1） |
| サーボ SG90（2 分岐ケーブル） | A2 | ↑（M キーで自動往復に切替） |
| GROVE 超音波距離センサ | D6 | 手を近づけると画面の球がセンサへ寄る（実験 2） |
| GROVE フルカラー LED v2.0 | D2（クロック D2・データ D3） | 距離で色が変わる: 遠い=青 → 緑 → 近い=赤（実験 3） |
| GROVE マイクロスイッチ | A6 | 押すと時限爆弾が動き出す（実験 4） |
| GROVE スピーカー | D4 | 「ピッ…ピッ…」の間隔がだんだん速くなり、最後は「ピーーー」→ ドカーン。センサに手をかざし続けると解除（「ピロリン」）。LED も赤く点滅 |

- **BGM（ジュークボックス）**: Play 中に 1〜9 でその番号の曲に切り替え、Shift+数字で予約（いまの曲が終わったら順に流す）、
  0 で停止、N で次の曲。時限爆弾が動いている間は一時停止します。曲は `GroveLab\assets\bgm\1.mid`〜`9.mid`
  （最初はクラシック・民謡 7 曲とオリジナル 2 曲）。**好きな MIDI ファイルを同じ名前で置けば差し替えられます**。
  - スピーカーは一度に 1 音しか出せないので、1 つのトラックのメロディだけを鳴らします（指定が無ければ平均の音がいちばん高いトラック）
  - 歌のトラックを鳴らしたいときなどは `assets\bgm\bgm.txt` に `2 = 曲名 ; track=MIKU,1` のように書く
    （`,` で並べると優先順。歌が 0.3 秒以上休む所は次のトラックで埋める）。トラック名は Play したときのログの `[bgm]` 行に出ます
  - 曲の作り直しは `python hardware\lab\tools\make_grove_bgm.py`
- **トロンボーン**: Play 中に T で ON / OFF。センサの前に手を入れると鳴り、遠ざけるほど低く・近づけるほど高く、なめらかに滑ります
  （6〜40cm でソ3〜ソ5）。G で「スライド」⇄「ドレミ（音階に吸い付く）」。ボリュームのつまみでビブラート。ON の間は BGM を一時停止します
- **金庫破り**: Play 中に K で金庫モード。ボリュームのつまみがダイヤル（0〜99）で、隠れた番号が 3 つあります。
  回すと「カチ」、正解に近いと低い音、ぴったりだと「ゴトッ」。LED も近いほど赤 → 黄 → 緑。正解で 0.8 秒止めると「カチャン」とそろいます。
  3 つそろったらスイッチ（ハンドル）を引くと、本物のサーボが回って錠が開き、ファンファーレ。そろう前に引くと「ガチャガチャ」。
  金庫モードの間はサーボが錠になり、スイッチで時限爆弾は動きません。K で閉じると番号が変わります
- **だるまさんがころんだ**: Play 中に D。手をセンサから 35cm より遠くに置いてスイッチでスタート。
  サーボ（鬼の首）が向こうを向いて「だるまさんがころんだ」と歌う間（速さと「ため」は毎回ちがう）に手を近づけ、
  「だ！」で振り向いたら止める。3cm 以上動く・手が見えなくなると見つかってブザー（鬼が首を振る）。
  6cm まで近づいて、鬼が向こうを向いている間にスイッチを押せば勝ち（タイムといちばん速い記録が出る）。
  金庫・だるま・トロンボーンは、後から ON にしたほうに切り替わります
- **カートレース「グローブ・グランプリ」**: Play 中に R でレースのシーンへ（もう一度 R で戻る）。
  **つまみがハンドル**（真ん中がまっすぐ。ずれていたらスタート前に C）、アクセルは自動、スイッチでアイテム（キノコでダッシュ）。
  CPU 3 台と 3 周。信号が緑になる直前にスイッチでロケットスタート（早すぎるとフライング）。
  サーボはスピードメーター、LED は信号・アイテムのルーレット・ダッシュ、スピーカーはエンジン音と効果音。
  インスペクタの `KART_Game` で周回数・CPU の速さ・ハンドルの効きと左右、アクセルをセンサにする（autoGas OFF）を変えられます。
  コースは `python hardware\lab\tools\make_grove_kart.py` で作り直せます（scenes/kart.json と kart/track.txt）
- **スマホで参加（最大 4 人）**: `GroveLab\phone\スマホでつなぐ.bat` を起動すると、レースのスタート画面に QR と 4 桁の部屋番号が出ます。
  スマホで読み取って名前を入れると、青・緑・黄の CPU の代わりに走れます（つまみの人が赤）。スマホを横にして**傾けてハンドル**、
  大きなボタンでアイテム（スタート前はスタート、カウントダウン中はロケット）。傾きが使えない機種は左側を指で左右にドラッグ。
  2 人以上で走ると**画面分割**（2 人は上下、3〜4 人は 4 つ。3 人のときの 4 つ目はコース全体）で、区画ごとに順位・周回・速さ・アイテムが出ます（エンジン v2.5.5 以降）。中継は Cloudflare Workers（`hardware\lab\tools\grove-kart-relay`、
  `npx wrangler deploy`）で、PC 側の `phone-relay.ps1` が `assets\kart\phones.txt`（入力）と `state.txt`（状況）を受け渡します
- ボードが無ければキーボードで代用: ← →=ボリューム、↑ ↓=距離、Space=スイッチ、M=サーボ自動
- **スピーカーの音量**: Play 中に Z（小さく）/ X（大きく）。既定は 35%。インスペクタの `GP_Speaker` の「音量」でも変えられます
  （爆発までの秒数・ピッの高さも同じ所）。v2.5.4 より前に書き込んだボードは音量が効かないので、スケッチを書き込み直してください
- スイッチの押す/離すが逆なら `GroveLab\assets\hardware.json` の `channels` に `"sw": { "invert": true }` を足す
- 距離は 5〜50cm を「近い〜遠い」として使います（`GP_Hand` の GroveDistance のプロパティで変えられる）
- スケッチは `GroveLab\firmware\GroveLab\GroveLab.ino`（Servo ライブラリを使う。道具の自動準備で一緒に入ります）
- 書き込んだあと「未接続」のままなら、エンジンが v2.5.4 以降か確かめてください（それより前は Nano Every が書き込み直後につながらないことがありました。USB を挿し直しても直ります）

## もっと詳しく

- プロトコル・Lua API・hardware.json の全項目: [`docs/HARDWARE.md`](../../docs/HARDWARE.md)
- 通信ライブラリ UnoLink とほかの例: [`hardware/firmware/README.md`](../firmware/README.md)
- Python でボードと直接話す確認ツール: `python ..\firmware\tools\hwprobe.py COM5 --seconds 5`（`pip install pyserial`）

## ソースからビルドする人へ

ルートの `README.md` の手順で `build\release\DX12Engine.exe` を作れば、`start.bat` はそちらを優先して使います
（Visual Studio 2026 + vcpkg が要るので、実験だけならリリース版で十分です）。
