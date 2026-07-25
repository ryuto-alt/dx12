# オーサリングガイド（人間 & Claude Code 両対応）

このエンジンは **「ゲームの中身は全部データ（シーン JSON + `.lua` コンポーネント）」** という方針。
だから同じものを **人間はエディタでポチポチ**、**Claude Code はテキスト（JSON/Lua）を書くだけ** で作れる。

- スクリプトコンポーネント（プロパティ付き `.lua`）とプレハブ → [`SCRIPT_COMPONENTS.md`](SCRIPT_COMPONENTS.md)
- このファイル → **エフェクト配置（ParticleEmitter）** / **イベント配置（Trigger + Action）** / **エンティティ参照** / **イベントバス** / **`--validate` 検証** / **地形（`.hf`）とスカルプト（`.smsh`）**

プロジェクトのフォルダ規約（`testengine` / `skiptime2` などと同じ）:
```
<project>/
  <project>.dx12proj         # defaultScene を指す
  assets/
    scenes/*.json            # シーン（エンティティ配置）
    components/*.lua          # 貼り付ける部品（properties 付き）
    game.lua                 # 共有グローバル（任意）
    prefabs/*.prefab
    terrain/*.hf             # 地形の高さ配列（バイナリ。10.5）
    sculpt/*.smsh            # スカルプトの頂点配列（バイナリ。10.6）
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
| `texturePath` | assets 相対パス。指定すると `kind` の数式模様の代わりに画像をビルボード貼り付け。空=プロシージャル | "" |

**炎=** kind1 blend0 gravity+ / **煙=** kind2 blend1 size大 colorEnd暗 / **魔法=** kind4 blend0 / **火花=** kind3 stretch>0。
**テクスチャ貼り付け=** `texturePath` に画像を指定（例 `"vfx/spark.png"`）。色は寿命カーブの頂点色で乗算、
アルファは画像のアルファをそのまま使用（straight alpha、Sprite2D と同じ規約）。GPU パーティクル(`gpu:true`)は非対応。

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

---

## 6. カスタムシェーダー（プロジェクト独自 HLSL）

`assets/shaders/` にプロジェクト独自の `.hlsl` を置ける。**保存すると自動でホットリロード**される
（0.5秒ポーリング → 実行時コンパイル → PSO 差し替え。GUI 無しで確認可能）。用途は2種類:

1. **エンジン組み込みシェーダーの上書き**: `assets/shaders/forward/Forward.hlsl` のように
   エンジン側 `shaders/` と同じ相対パスに置くと、そのシェーダーが**プロジェクト全体で**差し替わる。
2. **自作シェーダー（メッシュ単位で割当）**: 上記以外のパスに置いた `.hlsl` は「カスタムシェーダー」
   として扱われ、`MeshRenderer` の Inspector「Shader」欄で個別のメッシュに割り当てられる
   （静的メッシュのみ対応。スキンド/インスタンシングは既定 Forward へフォールバック）。
   エントリポイントは `VSMain`/`PSMain` 固定（`vs_6_0`/`ps_6_0`）。テンプレは
   Toolbar「ファイル > 新規シェーダー」またはアセットブラウザ右クリックから生成できる。

Claude Code から直接編集する場合:
```
<project>/assets/shaders/<name>.hlsl   # 新規 = 自作、既存パスと同名 = 上書き
```
を書く/直すだけでよい（コンパイル可否はエディタ起動中ならコンソールパネルに赤字で出る）。
`MeshRenderer` への割当はシーン JSON の `"shader"` フィールド（アセット相対パス）で行う:
```json
"meshRenderer": { "modelPath": "models/foo.gltf" },
"shader": "myfx/Glow.hlsl"
```

**半透明にしたい場合（アルファブレンド）**: PS が `float4` の alpha に 1 未満の値を書いても、
既定では PSO が不透明固定（`BlendEnable=FALSE`）になっているため画面には反映されない。
Inspector の「Shader」欄直下にある**「アルファブレンド有効」**チェックボックスを ON にするか、
シーン JSON に `"shaderAlphaBlend": true` を追加する（`SrcAlpha`/`InvSrcAlpha` の通常アルファ
ブレンド、`DepthWrite` は OFF になる＝半透明物の定石）。既定は `false`（不透明固定）。
```json
"shader": "myfx/Glass.hlsl",
"shaderAlphaBlend": true
```

**進捗/強度値を渡したい場合（`effectValue`）**: `cbuffer PerObjectConstants : register(b0)` に
`float4x4 mvp; float4x4 model;` の後ろへ `float effectValue;` を足すと、Sprite2D の `effectValue` と
同じ役割の汎用フロートをメッシュにも渡せる（ディゾルブの進捗・パルスの強さ等）。
既定シェーダー(Forward)はこの3つ目のフィールドを読まないため、追加しても他のメッシュ/シェーダーには
影響しない。Lua `scene:setMeshEffect(entity, value)` で実行時に書き換え可能（ルート定数なので毎フレーム
呼んでも安価、GPU同期・VB再生成なし）。シーン JSON では `"shaderEffectValue": 0.5` で初期値を指定できる
（`shader` フィールドと併用時のみ意味を持つ）。
```hlsl
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    float    effectValue;   // 追加分。0..1等、意味はシェーダー依存
    float4   shaderParams;  // さらに追加分。汎用パラメーター4つ(cbuffer パッキングで自動的に次の16バイト境界に載る)
};
```

**パラメーターを複数渡したい場合（`shaderParams`）**: 上記のように `effectValue` の後ろへ
`float4 shaderParams;` を足すと、汎用パラメーター4つ（色・速度・しきい値など意味はシェーダー依存）を
渡せる。Inspector の「Shader」セクション（エフェクト値の下の4連スライダー）で調整でき、
Lua `scene:setMeshParams(entity, x, y, z, w)` で実行時にも書き換えられる（effectValue と同じ
ルート定数経路 = 毎フレーム安価）。シーン JSON では `"shaderParams": [x, y, z, w]` で初期値を指定できる。

**MCP 経由（エディタ起動中、ファイル直書き不要）**: `dx12_create_shader({name, code})` で
`assets/shaders/<name>.hlsl` を作成/上書きし、書き込み直後に実行時コンパイルを試して
`{path, compiled, error?}` を返す（Lua の `dx12_create_lua_component` と違い書く前の静的検証は
できないため、失敗しても書いたファイルは残る＝`error` を見て直し、再度 `dx12_create_shader` を
撃ち直す反復修正が前提）。既存シェーダーの読み直しは `dx12_read_shader({path})`、メッシュへの割当は
`dx12_set_mesh_shader({entity, shaderPath, alphaBlend})`（shaderPath 空文字/省略で既定 Forward に
戻す。alphaBlend:true で上記の半透明ブレンドを有効化）。
詳細は [`MCP.md`](MCP.md) 参照。

`DX12Engine.exe --build`（配布ビルド）でも、コンパイル済みバイトコードが `game.pak` に封入され
プレイ時に反映される。プロジェクトシェーダーが壊れているとビルド自体が失敗する（黙って古い版を
出荷しない設計）。

### 6.1 Sprite2D のカスタムシェーダー（メッシュ用とは別契約）

`Sprite2D`（world-space の2Dスプライト）にも同じ仕組みでカスタム `.hlsl` を割り当てられる
（保存で自動ホットリロード、MeshRenderer と同じ `assets/shaders/` 配下、Registry 一致パスは
プロジェクト全体の上書き用途という区別も同じ）。**ただし頂点フォーマットとルートシグネチャが
メッシュ用シェーダーと異なる**ため、`arrow.hlsl`/`Forward.hlsl` 系のテンプレをそのまま使い回せない:

```hlsl
// Sprite2D カスタムシェーダーの契約（VSMain/PSMain 固定・vs_6_0/ps_6_0 固定）
cbuffer SpriteCB : register(b0)
{
    float4x4 gTransform;  // viewProj(転置済み)。頂点は既にCPUでワールド座標変換済み
    float    gTime;       // 経過秒（既定Sprite.hlslは未使用、アニメーション演出用）
};

struct VSIn
{
    float3 pos    : POSITION;
    float2 uv     : TEXCOORD0;
    float4 col    : COLOR0;
    float  effect : TEXCOORD1;  // Sprite2D::effectValue（頂点属性として補間される汎用進捗値）
    float4 params : TEXCOORD2;  // Sprite2D::shaderParams（汎用パラメーター4つ。読まないなら省略可）
};

struct PSIn
{
    float4 pos    : SV_POSITION;
    float2 uv     : TEXCOORD0;
    float4 col    : COLOR0;
    float  effect : TEXCOORD1;
    float4 params : TEXCOORD2;
};

Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);

PSIn VSMain(VSIn v) { /* pos を gTransform で変換し、他フィールドをそのまま渡す */ }
float4 PSMain(PSIn p) : SV_TARGET { /* gTex.Sample(gSamp, p.uv) を p.effect/gTime で加工 */ }
```

- 割当はシーン JSON の `sprite2d` 内に(MeshRendererのような別キー化はしない、`entt::meta` の
  自動シリアライズにそのまま乗る):
  ```json
  "sprite2d": { "texturePath": "textures/wall.png", "shaderPath": "myfx/Dissolve.hlsl",
                "shaderAlphaBlend": true, "effectValue": 0.0 }
  ```
- **`effectValue`** は Lua から実行時に書き換えられる（頂点属性なので GPU 同期・VB 再生成なし、
  毎フレーム呼んでも安価）: `scene:setSpriteEffect(e, value)`。ディゾルブの進捗やパルスの強さなど
  「今どれくらい効果がかかっているか」を送るのに使う。
- **`shaderParams`**（汎用パラメーター4つ、TEXCOORD2）も同様: Inspector の Shader セクションの
  4連スライダーで調整、Lua は `scene:setSpriteParams(e, x, y, z, w)`。effectValue と同じ頂点属性
  経路なのでバッチを壊さず毎フレーム安価。シーン JSON では `"shaderParams": [x, y, z, w]`。
- **worldSpace のスプライトのみ対応**（HUD スプライトは既定シェーダー固定）。
- MCP: `dx12_set_sprite_shader({entity, shaderPath, alphaBlend})`。Inspector は Sprite2D の
  「Shader」セクション（コンボ+アルファブレンド+effectValueスライダー、MeshRenderer と同じ操作感）。

## 7. マテリアルのテクスチャ割当（アセットブラウザから D&D）

Unity/Unreal 風に、アセットブラウザのテクスチャをドラッグ&ドロップでメッシュのマテリアルへ割り当てられる。
2通りの入口がある:

1. **SceneView へドロップ**: ドロップした位置にあるメッシュ(サブメッシュ単位)の **Albedo** に自動割当。
2. **Inspector の「Material」欄**: `Albedo`/`Normal`/`MetalRoughness` の3スロットへ個別にドロップ可能
   （サブメッシュが複数ある場合はサブメッシュごとに3スロットが並ぶ）。割り当てたスロットの右の
   「x」ボタンで解除できる。

**重要な設計**: モデルは `ResourceManager` のキャッシュ経由で同一 `modelPath` の全インスタンスが
同じ `Material` を共有している。そのため、テクスチャ割当は `Material` 自体を書き換えず、
`MeshRenderer::overrideAlbedoTexture`/`overrideNormalTexture`/`overrideMetalRoughnessTexture`
（サブメッシュ単位の `std::vector<std::string>`、値はアセット相対パス）に**インスタンス単位**で保持する。
これにより、同じモデルを複製して置いた他のエンティティには影響しない。描画側
(`Application::EnsureMaterialOverrideSrv`)が上書き分だけ専用の SRV ブロックを合成して差し替える。
Metallic/Roughness の数値上書き(`overrideMetallic`/`overrideRoughness`)と同じ「Material に触らない」方針。

シーン JSON には `"materialTextureOverrides"`（サブメッシュ数ぶんの配列、各要素は
`{"albedo": "...", "normal": "...", "metalRoughness": "..."}` のうち設定されたキーのみ）として保存される。

## 8. マテリアルアセット（`.dxmat`、Unreal のマテリアルインスタンス相当）

上記7節のテクスチャ上書きは「エンティティ1体に対してその場でテクスチャを差し替える」その場限りの
操作。複数のメッシュ/シーンをまたいで**再利用できる名前付きマテリアル**（地面・壁・石材といった
テンプレート）が欲しい場合は `.dxmat` アセットを使う。

**フォーマット**（`assets/materials/<name>.dxmat`、パスは assets 相対）:
```json
{
  "version": 1,
  "name": "red_brick_03",
  "albedo": "textures/red_brick_03/red_brick_03_diff.jpg",
  "normal": "textures/red_brick_03/red_brick_03_nor_gl.png",
  "metalRoughness": "textures/red_brick_03/red_brick_03_arm.png",
  "metallic": 1.0,
  "roughness": 1.0,
  "uvTiling": [1.0, 1.0],
  "source": "Poly Haven",
  "license": "CC0"
}
```
`metallic`/`roughness` はテクスチャ値に掛かる**係数**（glTF 意味論。テクスチャが無ければそのまま
定数値として使われる）。`metalRoughness` は glTF の ORM 規約と同じ **G=Roughness / B=Metallic**
（Poly Haven の ARM パックテクスチャがそのまま使える。R=AO は現状未使用）。法線マップは
**OpenGL 規約**（`PBR.hlsli` の `PerturbNormal` が G 反転しない）。Poly Haven からは `nor_gl` を選ぶこと
（`nor_dx` は使わない）。

**優先度**: `MeshRenderer::materialAsset`(サブメッシュ単位、`.dxmat` の assets相対パス) が設定されて
いれば、7節の `overrideXxxTexture` より**常に優先**される。どちらも未設定ならモデル焼き込みの
`Material` を使う。Metallic/Roughness は `overrideMetallic`/`overrideRoughness`(>=0 のとき) がさらに
`.dxmat` の係数より優先される(エンティティ単位の微調整用)。

**エディタでの使い方**:
- **マテリアルライブラリ**（ツール > マテリアルライブラリ (Poly Haven)）: CC0・APIキー不要の
  Poly Haven テクスチャセットをエディタ内から検索・ダウンロードできる（「テンプレート」タブに
  地面/壁/石材等の厳選候補あり）。ダウンロードすると `assets/textures/<id>/` へ diff(albedo)・
  nor_gl(normal)・arm(metalRoughness) の3枚を保存し、対応する `assets/materials/<id>.dxmat` を自動生成、
  `assets/ASSET_MANIFEST.md` にも出所を記録する（Poly Haven は CC0 なので帰属は不要）。
- **マテリアルエディタ**（ツール > マテリアルエディタ、またはアセットブラウザで `.dxmat` をダブル
  クリック）: テクスチャ3スロット・Metallic/Roughness・UVタイリングを編集できる。既存マテリアルは
  スライダードラッグ中も即座にシーンへプレビュー反映され（`MaterialAssetManager::UpdateScalarsOnly`、
  SRV再構築なしの軽量パス）、指を離すとディスクへ保存される。外部エディタで `.dxmat` を直接編集した
  場合も0.5秒間隔でホットリロードされる。
- **Inspector**: サブメッシュごとの「Material Asset」欄にアセットブラウザから `.dxmat` を D&D、または
  クリックしてピッカーから選択。割当中は7節のテクスチャ個別上書きスロットが無効表示になる
  （優先度がひと目でわかるように）。

シーン JSON には `"materialAssets"`（サブメッシュ数ぶんの配列、`.dxmat` の assets相対パス。空文字列 = 未割当）
として保存される。`--validate` は参照している `.dxmat` の実在チェックも行う。

---

## 9. UIアニメーション（`.uianim`、タイムラインで作る）

UI をキーフレームで動かす仕組み。`UIAnimator`（フェード/ポップ等のプリセット）や Lua の
`scene:tweenUi{}`（1本ずつの補間）では作れない「複数要素を時間軸で振り付けた」演出を担当する。

**作り方**（ツール > UIアニメーション、またはアセットブラウザで `.uianim` をダブルクリック）:
1. ヒエラルキー/UIエディタで演出のルートになる要素を選び、「選択中を対象に」
2. 動かしたい子要素を選んで「＋ トラック追加」→ プロパティを選ぶ（位置/回転/スケール/色/
   グループアルファ/フィル量/文字サイズ/スプライトフレーム/表示 など）
3. 再生ヘッドを動かして「○ 録画」を ON にし、Inspector や UIエディタで値をいじる
   → その時刻にキーが自動で打たれる（どこで値を変えても拾う）
4. 保存すると `assets/uianim/<名前>.uianim` へ書かれ、再生中のシーンにも即反映される

**タイムライン操作**: 左ドラッグ = 再生ヘッド / キーを掴んで時刻移動、Ctrl+クリック = その位置に
キーを打つ、右クリック = イージング変更・削除、ホイール = ズーム、中ドラッグ = 横スクロール、
Ctrl+Z / Ctrl+Y = パネル内 Undo（ECS の Undo とは別勘定）。

**再生**: エンティティに `UIAnimPlayer` を付けて `clipPath` を指すだけ。`playOnStart` で Play 開始時に
自動再生、`finishEvent` に名前を入れると完了時に EventBus へ飛ぶ。Lua からは
`e:playUiAnim("uianim/menu_open.uianim")` / `e:stopUiAnim()` / `e:setUiAnimTime(t)` / `e:setUiAnimSpeed(s)`。

**トラックの target** はルートからの名前パス（`""` = ルート自身、`"Panel/Title"` = 子孫）。要素をリネーム
すると解決できなくなり、パネル上で赤く出る。

**合成規約**: 位置・回転・色などは**コンポーネント値を直接書く**ので、エディタのスクラブがそのまま
プレビューに出る（見たままが保存される）。スケールとグループアルファは `UIRect::scaleX/scaleY/alpha`
（見た目だけ変える正式なプロパティ、Inspector からも触れる）に書き、`UIAnimator` や tween の実行時
アニメとは**掛け算**で合流する。

## 10. スプライト連番アニメ（`.spranim`）

`Sprite2D::animFrames` / `UIImage::animFrames` が「等分割・連続 N コマ」しか作れないのに対し、
`.spranim` は **任意のコマ順・コマ別の表示時間・名前付き複数シーケンス**（idle / run / attack …）を
1 アセットに持てる。Sprite2D と UIImage の両方に効く。

**作り方**（ツール > スプライトシート）: テクスチャを指定（アセットブラウザから D&D 可）→ 列×行を
入れる（「行×列を推定」で当てにいける）→ シーケンスを追加 → シートのコマを左クリックで拾う
（Ctrl+左クリックで除去）→ fps・再生モード（ループ/単発/往復）を決めて保存。右側でプレビュー再生と
「選択中のエンティティに割り当て」ができる。

**再生**: `SpriteAnimator` の `sheetPath` / `currentSeq` を設定。Lua は `e:playSprite("run")` /
`e:stopSprite()` / `e:setSpriteSheet(path)`。単発シーケンスは完了時に `finishEvent` を EventBus へ飛ばす。
`SpriteAnimator` が動いている間は旧 `animFrames` 経路を止める（二重駆動しない）。

## 10.5 地形（ハイトフィールド / `.hf`）

山・谷・丘を「彫って」作るステージ地形。Unity Terrain / UE Landscape と同じ **ハイトフィールド**方式
（正方グリッドの高さ配列）で、**普通の Mesh を持つ普通のエンティティ**として実装してある
＝描画・フラスタムカリング・影・ピッキング・`.dxmat` 割当が全部そのまま効く。

**作り方**（ヒエラルキー「＋エンティティ追加 → Terrain（地形・山を作る）」で **地形ツール**窓が開く）:
1. 解像度（32〜512）とサイズ(m)を決めて「＋ 地形を作成」
2. 「山を生成（fBm）」でプリセット（なだらかな丘 / 峡谷 / 険しい山脈）を選び「この設定で生成」
3. 「スカルプトブラシ」で盛る/削る/ならす/平らに/ノイズ/浸食 を選んでビューポートを左ドラッグ
4. 「全体に適用 → 浸食をかける」で山肌を安息角まで崩して自然にする

**ビューポート操作**: 左ドラッグ = 塗る / **Shift** = 逆方向（盛る↔削る）/ **Ctrl** = 一時的に「ならす」/
**`[` `]`** = 半径。ブラシの円は地形の起伏に沿って描かれる。Undo/Redo は**ストローク単位**（Ctrl+Z 1 回で
1 ストロークぶん戻る）。窓を閉じるか「ブラシ有効」を OFF にすると通常の選択に戻る。

**当たり判定**: `Terrain` を持つエンティティには静的 `RigidBody` が自動で付き、Play 時に Jolt の
`HeightFieldShape` として登録される。剛体が乗る/めり込まない/斜面で滑る、`CharacterController` の
斜面登坂、`physics:raycast` / `dx12_raycast` が全部そのまま効く。**彫るとコライダーも追従する**
（描画メッシュと同じ高さ配列を見ているため）。作り直しは**ストロークを離した時にだけ予約**され、
次の物理ステップ（＝ Play 中）で消化される。だから Play しながら彫っても 1 フレーム後には反映され、
編集モードで彫った形は Play を押した時点の形で登録される。ドラッグ中に毎フレーム形状を作ることはない。

**高さデータの置き場**: 解像度 256 で 6 万要素になるのでシーン JSON には入れず、
`<project>/assets/terrain/<名前>.hf`（`"DXHF"` + version + 解像度 + サイズ + `f32` 配列）に保存する。
**ストロークが終わるたび自動保存**されるので、彫ったあと Play しても内容は失われない。
読み込みは vfs 経由なので配布ビルドでは `game.pak` から復号して読む。

シーン JSON（エンティティの `terrain` ブロック。`meshRenderer` / `primitive` は書かない）:
```json
{
  "name": "Terrain",
  "transform": { "position": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1] },
  "terrain": {
    "resolution": 128,
    "worldSize": 200.0,
    "maxHeight": 200.0,
    "heightmapPath": "terrain/Terrain.hf",
    "uvScale": 24.0,
    "color": [0.42, 0.50, 0.32, 1.0]
  },
  "rigidBody": { "motionType": 0 }
}
```

| フィールド | 意味 | 既定 |
|---|---|---|
| `resolution` | 1 辺のサンプル数。4 の倍数へ丸められる（Jolt の HeightFieldShape の制約） | 128 |
| `worldSize` | 1 辺のワールド長(m)。セル幅 = `worldSize/(resolution-1)`。原点中心（±半分） | 200 |
| `maxHeight` | ブラシで彫れる高さの絶対値の上限 | 200 |
| `heightmapPath` | assets 相対の `.hf`。空 = 平坦で復元 | "" |
| `uvScale` | 地形全体で UV が何回繰り返すか（`.dxmat` のタイリング用） | 24 |
| `color` | 頂点色 RGBA（マテリアル未割当時の見た目） | 草色 |

> 地形の Transform は**平行移動だけ**が効く（回転・スケールはハイトフィールドの前提として無視）。
> `.dxmat` を割り当てたい場合は Inspector の Material Asset 欄（サブメッシュ 0）へ D&D する。

## 10.6 スカルプト・異形（メッシュ頂点編集 / `.smsh`）

ハイトフィールド地形は「XZ グリッドの高さ」しか持てない＝**オーバーハングが作れない**。
洞窟・アーチ・せり出した崖・岩・柱といった**異形**はこっちが担当する。ZBrush / UE の VSculpt 相当で、
メッシュの**頂点位置を直接動かす**方式（三角形の張り方＝トポロジは一切変えない）。
出来上がるのは**普通の Mesh を持つ普通のエンティティ**なので、描画・カリング・影・ピッキング・
`.dxmat` 割当が地形と同じくそのまま効く。

### 作り方

ヒエラルキー「✚ エンティティ追加 → **Sculpt（異形・洞窟・アーチ・岩）**」で**スカルプト窓**が開く
（`SculptMesh` を持つエンティティを選んでも自動で開く）。

1. **素体を作る** … 形（箱 / 球 / 板 / 円柱）・分割数・大きさ(m) を決めて「＋ 素体を作成」。
   岩は球、アーチ/柱は円柱、崖は箱から彫るのが早い。**分割数 16〜24 が使いやすい**
   （細かいほど彫り込めるが重い）。作成すると半径・強さが素体の大きさに合わせて自動調整される
2. あるいは **既存モデルを彫る** … メッシュを選んで「選択中のモデルを編集可能にする」。
   全サブメッシュを 1 つに畳んだ**編集用のコピー**が新しいエンティティとして生まれ、
   元と同じ姿勢に置かれる。**元の `.glb` 等には一切書き戻さない**（読むだけ）
3. **スカルプトブラシ**で種類を選んでビューポートを左ドラッグ
4. **見た目・当たり判定・保存**で頂点色 / UV倍率 / 当たり判定を決める

### ブラシ（8 種）

| 種類 | 動き |
|---|---|
| 盛る Draw | 面の法線方向へ盛る（Shift で凹む） |
| 引っぱる Pull | 当たった面の法線方向へ引っぱる |
| 押し込む Push | 同・逆方向へ押し込む |
| ならす Smooth | 隣接頂点の平均へ寄せる（ラプラシアン平滑化） |
| 平らに Flatten | 影響範囲の平均平面へ寄せる |
| つまむ Pinch | ブラシ中心へ寄せる（稜線を立てる） |
| ノイズ Noise | fBm ノイズを法線方向へ足す（岩肌のざらつき。周波数/オクターブ/尾根を出せる） |
| 掴む Grab | 掴んで引っぱる（フォールオフ付きの平行移動） |

**ビューポート操作**（地形ブラシと同じキー）: 左ドラッグ = 彫る / **Shift** = 逆方向 /
**Ctrl** = 押している間だけ「ならす」/ **`[` `]`** = 半径。ブラシの円は表面に沿って描かれる。
Undo/Redo は**ストローク単位**で、触った頂点の差分だけを持つ（全頂点スナップショットは採らない）。
窓を閉じるか「ブラシ有効」を OFF にすると通常の選択に戻る。

**「掴む Grab」だけ挙動が違う**: 押した瞬間の視線に垂直な平面上でカーソルの移動量をそのまま
引っぱり量にするので、**表面から外れても掴み続けられる**（アーチを引き伸ばす時に使う）。

> **地形ツール窓とスカルプト窓は同時に開かないこと。** ビューポートのクリックは登録順
> （ライトハンドル → 地形 → スカルプト）で先に食った方が勝つので、両方開いていると地形の上に
> 置いたスカルプト対象をクリックしても**地形ブラシが先に取る**（奥行きは比較していない）。
> `[` `]` も両方のブラシ半径が同時に変わる。使わない側の窓は閉じるか「ブラシ有効」を OFF にする。

**対称（ミラー）**: X / Y / Z を個別にチェックできる。**メッシュのローカル軸**が基準で、3 つ同時に
立てると最大 8 個の筆が同時に走る（左右対称のアーチや、放射状の岩を一発で作る）。

### 当たり判定

`SculptMesh` を持つエンティティには**静的 `RigidBody` が自動で付く**。「当たり判定を作る」が ON なら
Jolt の **`MeshShape`（三角形メッシュ形状）**として登録される＝**彫った通りに当たる**
（洞窟の中に入れる・アーチの下をくぐれる）。Transform のスケールは形状へ焼き込まれる。

- 作り直しのタイミングは地形と同じ（**ストロークを離した時に予約 → 次の物理ステップで反映**）
- **動く剛体（Static 以外）に付けると `MeshShape` は使えない**（Jolt の制約）。自動で**凸包**へ
  フォールバックし、警告が出る。**彫った凹み・穴は当たり判定では埋まる**ので、
  洞窟やアーチにしたいなら剛体は Static のままにすること
- 「当たり判定を作る」を OFF にすると**物理ボディそのものを作らない**（見えない箱は湧かない）

### 頂点データの置き場（`.smsh`）

頂点配列はシーン JSON に入れず、`<project>/assets/sculpt/<名前>.smsh`
（`"SMSH"` + version + 頂点数 + インデックス数 + 各配列）に保存する。**ストロークが終わるたび自動保存**
されるので、彫ったあと Play しても内容は失われない（窓の「メッシュを保存 (.smsh)」は手動の念押し）。
読み込みは vfs 経由なので配布ビルドでは `game.pak` から復号して読む。

シーン JSON（エンティティの `sculpt` ブロック。`meshRenderer` / `primitive` は書かない）:
```json
{
  "name": "Arch",
  "transform": { "position": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1] },
  "sculpt": {
    "meshPath": "sculpt/Arch.smsh",
    "uvScale": 1.0,
    "collision": true,
    "color": [0.72, 0.70, 0.66, 1.0]
  },
  "rigidBody": { "motionType": 0 }
}
```

| フィールド | 意味 | 既定 |
|---|---|---|
| `meshPath` | assets 相対の `.smsh`。空 or 読めない = 素体（球）で復元＝シーンは必ず開ける | "" |
| `uvScale` | 元の UV に掛ける倍率（`.dxmat` のタイリング用） | 1.0 |
| `collision` | `MeshShape` コライダーを作るか。false なら物理ボディを作らない | true |
| `color` | 頂点色 RGBA（マテリアル未割当時の見た目） | 石色 |

> **溶接（weld）について**: GLB 等の DCC 出力は UV/法線シームで「同じ位置の頂点」が複数に割れている。
> そのまま彫ると継ぎ目がパックリ開くので、位置が一致する頂点を 1 グループにまとめて代表を決め、
> ブラシは代表単位で動かす。だから imported なモデルでも破綻せず彫れる。
> **スキン付きモデルを編集可能にするとボーン追従は落ちる**（静的な形として彫る前提。警告が出る）。

### 手順まるごと: 山を作って → 洞窟を彫って → 当たり判定を確認する

1. ヒエラルキー「✚ エンティティ追加 → **Terrain**」→ 解像度 `128` / サイズ `200` で「＋ 地形を作成」
2. 「山を生成（fBm）」→ プリセット **険しい山脈** →「この設定で生成（上書き）」。
   気に入らなければ「サイコロ（シードを変えて生成）」を連打する
3. 「全体に適用 → 浸食をかける」（安息角 34° / 24 回）で山肌を崩して自然にする
4. ブラシ **平らに Flatten** で、後で洞窟を掘る山腹を少しならす（`[` `]` で半径を調整）
5. **地形ツール窓を閉じる**（開いたままだと地形ブラシがクリックを取る）。
   ヒエラルキー「✚ エンティティ追加 → **Sculpt**」→ 形 **円柱** / 分割数 `20` / 大きさ `8` で「＋ 素体を作成」
6. ギズモ（`W`）で素体を山腹へ埋め込む。`E` で回して穴の向きを合わせる
7. スカルプト窓でブラシ **押し込む Push**、**Xミラー ON**、半径を素体の 1/4 くらいにして、
   円柱の端面をぐりぐり押し込む＝洞窟の口が開く。**掴む Grab** で奥へ引っぱると通り抜けられる長さになる
8. **ノイズ Noise**（周波数 1.5 / オクターブ 4）を軽く当てて岩肌のざらつきを出す
9. 「見た目・当たり判定・保存」で**「当たり判定を作る」が ON**・剛体が **Static** であることを確認
   （Inspector の RigidBody で確認できる。Static でないと凸包に落ちて穴が塞がる）
10. `Ctrl+S` でシーン保存 → **Play**。プレイヤーを洞窟へ歩かせて、めり込まず通り抜けられるか見る
11. うまく当たらない時は `ツール > エンジン診断 > 🔬 超詳細診断` を回す。`terrain` / `picking` 検査が
    「`.hf` を複数の地形が共有している」「CPU 頂点キャッシュが無い」等を日本語 1 行で教えてくれる

## 11. プレハブのリンク（適用 / 元に戻す）

`.prefab` を展開したエンティティのルートには `PrefabLink` が付き、Inspector の最上段に
「プレハブ」ヘッダーが出る。ここから:

- **適用 Apply**: このインスタンスの今の姿を元の `.prefab` へ書き戻す
- **元に戻す Revert**: 元の `.prefab` の状態へ作り直す（外側の親だけ維持。Ctrl+Z で戻せる）
- **他のインスタンスも更新**: 同じ `.prefab` から作った他のインスタンスを保存済みの内容で作り直す
  （「適用」の後に押す。この操作は Undo できない）
- **変更点**: 元の `.prefab` との差分をコンポーネント・フィールド単位で一覧表示する
  （名前は展開時に連番が付くので比較対象外）

UI 要素を「プレハブにする」と `assets/prefabs/ui/` へ保存され、**UIエディタ左下のパレット**に並ぶ。
そこからキャンバスへドラッグすると、落とした位置の UI ノードの子として配置される。
