# オーサリングガイド（人間 & Claude Code 両対応）

このエンジンは **「ゲームの中身は全部データ（シーン JSON + `.lua` コンポーネント）」** という方針。
だから同じものを **人間はエディタでポチポチ**、**Claude Code はテキスト（JSON/Lua）を書くだけ** で作れる。

- スクリプトコンポーネント（プロパティ付き `.lua`）とプレハブ → [`SCRIPT_COMPONENTS.md`](SCRIPT_COMPONENTS.md)
- このファイル → **エフェクト配置（ParticleEmitter）** / **イベント配置（Trigger + Action）** / **エンティティ参照** / **イベントバス** / **`--validate` 検証**

プロジェクトのフォルダ規約（`testengine` / `skiptime2` などと同じ）:
```
<project>/
  <project>.dx12proj         # defaultScene を指す
  assets/
    scenes/*.json            # シーン（エンティティ配置）
    components/*.lua          # 貼り付ける部品（properties 付き）
    game.lua                 # 共有グローバル（任意）
    prefabs/*.prefab
```

---

## 0. Claude Code 向けワークフロー

1. `assets/scenes/<scene>.json` と `assets/components/*.lua` を**直接編集**する。
2. **検証**する（GUI 不要・ヘッドレス）:
   ```
   DX12Engine.exe --validate path/to/assets/scenes/<scene>.json
   ```
   → 参照切れ・スクリプト不在・action target 不正などを `validate_report.txt` と標準出力に吐く。
   `RESULT: PASS` / `RESULT: FAIL`、終了コード 0/1。
3. FAIL を直して 2 に戻る。PASS したら人間が Play で確認。

> エンティティ参照・Trigger の target / filter は **エンティティ名（NameTag）** で結ぶ。
> だから JSON に名前を書けばつながる。名前は重複させないこと（`--validate` が warn する）。

---

## 1. エフェクトを置く: `ParticleEmitter`

配置できるパーティクル放出器。Transform のワールド位置から、既存のパーティクル系に毎フレーム放出する。
**エディタでも常時プレビュー表示**される（置いたら見える）。Play では `playOnStart` に従う。
Trigger の `PlayEffect` / `StopEffect` で発火・停止できる。

エディタ: Hierarchy「＋追加 → Particle Emitter」 or Inspector「＋コンポーネント追加 → Particle Emitter」。
見た目を確認しながら作りたい場合は **ツール > パーティクルエディタ**（専用プレビュー窓・色/サイズのグラデーション
編集・`assets/vfx/*.json` への名前付き保存・選択エンティティへの適用・Lua `fx:burst{}` コード生成に対応）。

シーン JSON（エンティティの `particleEmitter` ブロック）:
```json
{
  "name": "Torch",
  "transform": { "position": [0, 1.2, 0], "rotation": [0,0,0], "scale": [1,1,1] },
  "particleEmitter": {
    "kind": 1, "blend": 0, "rate": 40,
    "playOnStart": true, "looping": true, "duration": 1.0,
    "dir": [0,1,0], "spread": 0.35,
    "speed": 2.5, "speedVar": 0.4,
    "size": 0.35, "sizeEnd": 0.0,
    "life": 0.7, "lifeVar": 0.3,
    "color": [1.0, 0.6, 0.2], "colorEnd": [1.0, 0.1, 0.05],
    "intensity": 4.0, "gravity": 0.5, "drag": 1.0, "up": 0.0, "stretch": 0.0
  }
}
```

| フィールド | 意味 | 既定 |
|---|---|---|
| `kind` | 見た目 0=Glow 1=Fire 2=Smoke 3=Spark 4=Magic 5=Electric 6=Ring 7=Star | 0 |
| `blend` | 0=加算（光物） 1=アルファ（煙） | 0 |
| `rate` | 連続放出レート（個/秒）。0 で連続放出しない | 30 |
| `playOnStart` | Play 開始で放出 ON | true |
| `looping` / `duration` | false なら `duration` 秒だけ放出（ワンショット） | true / 1.0 |
| `dir` `spread` | 放出方向 / 拡がり(0=集中,1=全球) | [0,1,0] / 0.4 |
| `speed` `speedVar` | 初速 / ばらつき | 3 / 0.4 |
| `size` `sizeMid` `sizeEnd` | 開始/中間/終了サイズ（`sizeMid`は-1で無効、0以上で3キーカーブ） | 0.3 / -1 / 0 |
| `life` `lifeVar` | 寿命秒 / ばらつき | 0.8 / 0.3 |
| `color` `colorMid` `colorEnd` `hasColorMid` | 開始/中間/終了色（RGB 0..1）。`hasColorMid`で中間色を使うか | 炎色 / false |
| `intensity` | HDR 輝度（>1 で白熱→ブルーム） | 3 |
| `gravity` `drag` `up` `stretch` | y加速 / 減衰 / 上向きバイアス / 速度方向の伸び | 0/1/0/0 |
| `turbStrength` `turbFreq` | カールノイズ乱流の強さ / 空間周波数（煙・炎の有機的な揺らぎ） | 0 / 1 |
| `flicker` `flickerFreq` | 発光明滅の強さ(0..1) / 速さ | 0 / 18 |
| `distort` `light` `lightRange` | 画面歪み量 / ポイントライト化 / 光の到達距離 | 0 / false / 3 |

**炎=** kind1 blend0 gravity+ / **煙=** kind2 blend1 size大 colorEnd暗 / **魔法=** kind4 blend0 / **火花=** kind3 stretch>0。

---

## 2. イベントを置く: `Trigger` + `Action`

範囲（箱/球）に **入った/出た/居る** とき、宣言的な **アクション列** を実行する部品。
「X したら Y する」をコードを書かずに配線できる。データだけ（評価は Play 中エンジンが駆動）。

エディタ: Hierarchy「＋追加 → Trigger」。Inspector で形・対象・アクションを組む。選択すると範囲がワイヤーフレーム表示。

シーン JSON（エンティティの `trigger` ブロック）:
```json
{
  "name": "GoalZone",
  "transform": { "position": [0, 1, 8], "rotation": [0,0,0], "scale": [1,1,1] },
  "trigger": {
    "shape": 0,
    "halfExtents": [1.5, 1.5, 1.5],
    "radius": 1.0,
    "offset": [0,0,0],
    "filter": "Player",
    "once": true,
    "actions": [
      { "when": 0, "type": 4,  "target": "GoalBurst" },
      { "when": 0, "type": 10, "str": "reachedGoal", "num": 1 },
      { "when": 0, "type": 8,  "str": "scenes/clear.json", "num": 0.6 }
    ]
  }
}
```

### Trigger フィールド
| フィールド | 意味 |
|---|---|
| `shape` | 0=Box（`halfExtents`×Transform.scale） / 1=Sphere（`radius`×scale最大成分） |
| `offset` | 判定中心のローカルオフセット |
| `filter` | 反応する対象エンティティ名。**空なら "Player"** |
| `once` | true なら一度 Enter したら無効化 |
| `actions` | 下表のアクション列 |

### Action
`when`: **0=Enter（入った瞬間）/ 1=Exit（出た瞬間）/ 2=Stay（居る間 毎フレーム）**
`target`: 対象エンティティ名（**空なら Filter 対象に作用**）

| `type` | 名前 | 効果 | 使うパラメータ |
|---|---|---|---|
| 0 | Enable | target の LuaScript を有効化 | target |
| 1 | Disable | target の LuaScript を無効化 | target |
| 2 | Destroy | target を削除 | target |
| 3 | Move | target の位置を `vec` だけ動かす（相対） | target, vec |
| 4 | PlayEffect | target の ParticleEmitter を放出開始 | target |
| 5 | StopEffect | target の ParticleEmitter を停止 | target |
| 6 | PlaySound | target の AudioSource を再生 | target |
| 7 | LoadScene | `str` のシーンへ即切替 | str |
| 8 | FadeToScene | `str` のシーンへフェード切替 | str, num(秒) |
| 9 | SetProperty | target の実行中スクリプトの `self[str] = num` | target, str, num |
| 10 | EmitEvent | イベントバスへ `events:emit(str, {value=num, target=...})` | str, num, target |

> ゲーム固有の処理（残り時間 +5、状態遷移 など）は **EmitEvent** で投げて、ゲーム側 Lua が
> `events:on("addTime", fn)` で受ける。これでエンジンは汎用のまま、ゲーム差分は Lua に閉じる。

---

## 3. エンティティ参照プロパティ（`type="entity"`）

スクリプトの `properties` で **他のエンティティを指す** 型。エディタではコンボで選ぶ、JSON では名前を書く。
Play 時に `self.<name>` に **Entity** が注入される（`:isValid()` / `.transform` などが使える）。

```lua
properties = {
  { name = "door",  type = "entity", label = "開ける扉" },
  { name = "speed", type = "float",  default = 6.0 },
}
function OnUpdate(self, dt)
  if self.door and self.door:isValid() then
    -- self.door.transform.position ... など
  end
end
```

シーン JSON 側（`luaScript.props` の 1 要素）:
```json
{ "name": "door", "type": "entity", "value": "Gate1" }
```
`value` は**参照先エンティティ名**。`--validate` がその名前の存在をチェックする。

---

## 4. イベントバス（`events`）

疎結合の発火/購読。どのコンポーネントからでも使える（グローバル）。Play 開始時に購読はクリアされる。

> **重要**: `events:on` / `events:emit` は **Playing 中のみ有効**。
> エディタ停止中（OnStart の前）に `events:on` を呼んでも購読は登録されず、警告が出る。
> 購読は必ずスクリプトの **`OnStart(self)` 内**で登録すること。

```lua
-- 購読する側（司令塔スクリプトの OnStart 内で登録する）
function OnStart(self)
    events:on("addTime", function(d) ST.remaining = ST.remaining + (d.value or 0) end)
    events:on("reachedGoal", function() ST.state = "clear" end)
end

-- 発火する側（OnUpdate 等から）
events:emit("addTime", { value = 5 })
```
Trigger の **EmitEvent アクション**（type 10）も C++ 側からこの `emit` を呼ぶ。
`data` は `{ value = <num>, target = <target名> }`。

---

## 5. 検証（`--validate`）

```
DX12Engine.exe --validate <project>/assets/scenes/<scene>.json
```
チェック内容:
- JSON が妥当か
- `luaScript.scriptPath` のファイルが assets 下に存在するか
- `type="entity"` プロパティの参照先名がシーンに存在するか
- Trigger の `filter` / action `target` が存在するか
- LoadScene/FadeToScene の `str` シーンパスが存在するか（warn）

結果は `validate_report.txt` と標準出力に `RESULT: PASS/FAIL`。終了コード 0/1。
**Claude Code はこれを回して、参照切れを GUI 無しで自己修正できる。**
