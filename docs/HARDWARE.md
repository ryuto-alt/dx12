# 物理ハードウェア連携（Arduino / ESP32）

Arduino や ESP32 をエンジンにつなぎ、ボタン・ダイヤル・センサーを入力に、LED・振動・ペルチェなどを出力に使う。
エディタだけでなく**配布ゲーム（Game.exe）でも動く**（展示で使うのは Game.exe なので）。

```
 [Arduino / ESP32]                         [エンジン src/hardware/]
  UnoLink ライブラリ ──USBシリアル──┐
  (自作スケッチ or 汎用スケッチ)    ├─▶ IOスレッド ─▶ キュー ─▶ フレーム頭で確定(BeginFrame)
  ESP32 ──Wi-Fi UDP(第3段)────────┘        │                     │
                                     自動再接続・心拍         Lua: hw.*   MCP: hw_*
                                                              actions への割当
```

## 1. プロトコル v1（エンジンとファームの唯一の約束）

- 1 行 1 メッセージ。ASCII、`\n` 終端（`\r` は無視）。**1 行は最大 128 バイト**（超えた行は捨ててエラー数を数える）
- 既定 115200bps / 8N1
- 1 行目の先頭文字で種類が決まる。値の行だけは先頭が英字

### デバイス → エンジン

| 行 | 意味 |
|---|---|
| `@hello name=<id> proto=1 fw=<n> board=<str>` | 名乗り。起動直後と `?hello` を受けたときに送る。`name` は `[a-z0-9_]{1,24}` |
| `@ch <name> <dir> <type> [min max] [safe=<v>]` | チャンネル宣言。`dir`=`in`/`out`、`type`=`bool`/`int`/`float`。`safe` は出力の安全値（既定 0） |
| `@ready` | 宣言の終わり。これ以降の値を使う |
| `<name>=<v> [<name>=<v> ...]` | 入力値。空白区切りで複数可（同じ行の値は同時刻として扱う） |
| `@pong <seq>` | `?ping` への応答（RTT 計測） |
| `@log <text>` | エンジンのログへ（info） |
| `@err <text>` | エンジンのログへ（warn）。エラー数に数える |

### エンジン → デバイス

| 行 | 意味 |
|---|---|
| `?hello` | 名乗り直しを要求（`@hello` → `@ch`… → `@ready` を再送） |
| `?ping <seq>` | 心拍。**IO スレッドから 200ms ごと**（メインスレッドの引っかかりで切れないように） |
| `!<name>=<v> [<name>=<v> ...]` | 出力値。1 フレームに 1 行まで（同じチャンネルは最新値だけ送る） |
| `!safe` | 全出力を安全値へ（Play 停止・切断時） |
| `?pin <pin> <mode> <name>` | **汎用スケッチ専用**。ピンをチャンネルとして生やす。`mode`=`in`/`in_pullup`/`adc`/`out`/`pwm`/`touch`(ESP32) |

### 安全（ファーム側の義務）

- **最後の `?ping` から 1000ms 何も受け取らなければ、全出力を安全値にする**（エンジンが落ちてもペルチェやモーターが止まる）
- 起動直後の出力は安全値

### 値の表し方

- `bool` は `0`/`1`、`int` は 10 進整数、`float` は `.` 区切りの小数
- エンジン側の正規化値 `value` は `[min,max]` → `[0,1]`（宣言に min/max が無ければ生値をそのまま）

### 例（ダイヤル 1 個・ボタン 1 個・LED 1 個）

```
→ @hello name=radio proto=1 fw=1 board=esp32
→ @ch dial in int 0 4095
→ @ch btn in bool
→ @ch led out float 0 1 safe=0
→ @ready
→ dial=2048 btn=0
← ?ping 17
→ @pong 17
← !led=0.40
```

## 2. 接続と再接続（エンジン側）

- **COM 番号で識別しない**（挿し直すと変わる）。`hardware.json` の `match`（`hello` 名 / VID / PID / port）で照合する
- 開く → `@hello` を最大 3 秒待つ（Uno/ESP32 は DTR/RTS でリセットされ、ブートに 1〜2 秒かかる）→ 来なければ `?hello` を送ってさらに 1 秒 → だめなら閉じて次の走査へ
- 切断（読み書きエラー / 3 秒無受信）を検出したら閉じ、1 秒ごとに走査して自動で開き直す
- 開くときは DTR=off / RTS=off（ESP32 を書き込みモードに落とさないため。リセットが要るときだけ RTS を一瞬立てる）

## 3. hardware.json（`assets/hardware.json`、pak に入る）

```json
{
  "devices": [
    {
      "name": "radio",
      "transport": "serial",
      "match": { "hello": "radio", "vid": "1A86", "pid": "7523" },
      "baud": 115200,
      "pins": [ { "pin": 34, "mode": "adc", "name": "dial" } ],
      "channels": {
        "dial": { "min": 0, "max": 4095, "deadzone": 0.01, "smooth": 0.2, "invert": false },
        "heat": { "dangerous": true, "maxValue": 0.6 }
      },
      "actions": { "btn": "Jump" }
    }
  ]
}
```

- トップレベルの `arduinoCli`（任意）は `hw_flash` が使う arduino-cli.exe の場所（無ければ PATH → `C:\Program Files\Arduino CLIrduino-cli.exe`）
- `pins` は汎用スケッチのときだけ使う（接続時に `?pin` を送る）
- `channels` の `min`/`max` は宣言より優先（校正値）。`smooth` は指数平滑の係数（0=なし）
- `dangerous` の出力は MCP の `hw_write` で confirm が要る。`maxValue` で上限を切る
- `actions` は `actions.*` のアクション名へ割り当てる（キーボードと同じアクションで遊べる＝実機なしで開発でき、展示中の故障時の代替にもなる）。
  実装は、チャンネルに擬似キーコード（`kHwKeyBase = 0x10000` + バインド番号）を割り当てて `ActionMap` へ束ね、`actions.get/down/pressed` の述語が
  擬似キーだけハードウェアの `Down` / `Pressed` へ回す（`InputSystem::IsKeyDown` は 256 未満しか受けない）。`get` の数値は押している間 1。
  `Application::ApplyHardwareActionBindings` が `LoadActionBindings` の後・設定の作り直しの後に付け直す。**`actions.save()` は擬似キーを `input_bindings.json` へ書かない**（正は hardware.json）

## 4. Lua API

```lua
local radio = hw.device("radio")
if radio.connected then
  local f = radio:get("dial")          -- 正規化値 0..1（校正・平滑済み）
  local r = radio:raw("dial")          -- 生値
  if radio:pressed("btn") then end     -- このフレームで 0→1
  if radio:released("btn") then end
  if radio:down("btn") then end
  radio:set("led", 0.4)                -- 出力（0..1 の正規化値。dangerous は maxValue で切られる）
end
for _, d in ipairs(hw.list()) do print(d.name, d.connected) end
```

## 5. MCP（エンジンの method。`dx12_tool_search {query:"hardware"}` で引ける）

| method | 内容 | effect |
|---|---|---|
| `hw_list_ports` | COM 一覧・VID/PID・推定ボード・使用中のデバイス | read |
| `hw_status` | デバイスごとの接続状態・受信レート・RTT・エラー・チャンネル・最新値 | read |
| `hw_connect` / `hw_disconnect` | 接続・切断（設定に無い名前で `hw_connect` するとデバイスを足して接続） | runtime |
| `hw_read` | 指定ミリ秒だけ集計して min/max/平均/ジッタ（応答をフレーム境界まで遅らせる＝メインスレッドを止めない） | read |
| `hw_monitor` | 生の送受信ログの末尾 | read |
| `hw_write` | 出力（正規化値）。`dangerous` のチャンネルを含むと拒否し、fix で `hw_write_dangerous` を案内 | runtime |
| `hw_write_dangerous` | `dangerous` 出力へ書く（`maxValue` で切る）。confirm が要る | guarded |
| `hw_simulate` | 仮想値を流し込む（実機なしで Lua を確かめる）。`channels` を渡すと仮想デバイスを作る | runtime |
| `hw_calibrate` | `start` で min/max 追跡 → `finish` で hardware.json に保存 | write_file |
| `hw_config_get` / `hw_config_set` | hardware.json を丸ごと読む / 書いて作り直す（パースエラーは書かずにエラー） | read / write_file |
| `hw_flash` / `hw_flash_status` | arduino-cli でコンパイル+書き込み（ポートを放して、終わったら再接続。非同期・同時 1 本） | guarded / read |

引数と返り値は [`docs/MCP.md`](MCP.md) §4-19。Lua の `hw.device(name)` は `connected` / `name` が**プロパティ**（`h.connected`）、`get` / `raw` / `down` / `pressed` / `released` / `set` がメソッド。

## 6. ファームウェア

`hardware/firmware/` に置く。

- `UnoLink/` … Arduino ライブラリ（AVR / ESP32 両対応）。`UnoLink link("radio"); link.addInput("dial", UL_INT, 0, 4095); link.send("dial", v); link.onOutput(...)`
- `UnoLinkGeneric/` … 汎用スケッチ。書くだけでエンジンから `?pin` でピンを生やせる

## 7. 進め方

1. 第1段: USB シリアル・プロトコル・UnoLink・Lua・MCP・hardware.json・アクション割当・仮想デバイス・ctest
2. 第2段: hw_flash・校正・PlaySession への記録・Game.exe での確認
3. 第3段: ESP32 の Wi-Fi UDP（同じ行を UDP で）・複数台・ノーコード割当コンポーネント

## 8. ArduinoLab と自動セットアップ（v2.5.0〜）

配布物（zip / インストーラ）には実験キットとファームウェアが入っている。

| 配布物の中 | 使い道 |
|---|---|
| `samples/ArduinoLab/` | 初回起動時に `ドキュメント\UnoProjects\ArduinoLab` へ 1 回だけ置き、最近のプロジェクトに足す（記録は `%LOCALAPPDATA%\UnoEngine\samples_installed.json`。消しても置き直さない） |
| `hardware/firmware/UnoLink` ほか | 書き込み時の UnoLink ライブラリ（`FindRepoRoot` が exe の隣で見つける） |

- **書き込み道具**: シリアルのデバイスを持つプロジェクトを開くと、arduino-cli（無ければ `%LOCALAPPDATA%\UnoEngine\arduino-cli\` へダウンロード）と
  `esp32:esp32` / `arduino:avr` のボード定義を裏で入れる。記録は同じフォルダの `cores.json`
- **ハードウェア窓**（ツール > ハードウェア）: デバイスの状態、書き込み道具の状態、ボードへの書き込み（スケッチはプロジェクトの `firmware/<名前>/<名前>.ino`）。
  書き込みのボタンは `hw_flash` と同じ処理（ポートを放す → arduino-cli → つなぎ直す）
- まだ名乗らないボード（既知の USB 変換チップ）が挿さったままなら、ハードウェア窓を 1 回だけ開いて書き込みを勧める。**勝手には書き込まない**
- MCP `hw_setup {action: status | install_toolchain | install_samples}`
- ソースは `hardware/lab/`（`ArduinoLab/` がそのまま `samples/ArduinoLab/` になる。bat での手順は `hardware/lab/README.md`）