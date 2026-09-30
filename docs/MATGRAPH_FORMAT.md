# マテリアルグラフ G1 仕様（`.dxmg`・型規則・コード生成・スロット表）

- 対象: Uno Engine（`C:\Users\ryuto\Documents\dx12` / ブランチ `feat_develop`）
- 実装: `src/renderer/matgraph/`（標準ライブラリ + nlohmann-json だけの純ロジック。GPU / D3D / ImGui 非依存）
- 上位の設計: `docs/MATERIAL_GRAPH_DESIGN.md`（§3 コード生成・§4 データ・§7 G1）。本書は G1 で確定した**実装仕様**で、設計書と食い違う点は本書が正。
- テスト: `tests/matgraph_test.cpp`（純ロジック）/ `tests/matgraph_dxc_test.cpp`（DXC + WARP。ctest ラベル `dxc`）/ `tests/data/matgraph/`（ゴールデン）

---

## 1. 全体像

```
 .dxmg（JSON）
   │ LoadDxmg / SaveDxmg            GraphIO.{h,cpp}
   ▼
 MaterialGraph                      GraphModel.{h,cpp}   ノード・接続（入力側）・コメント・設定・変更通知
   │ Analysis()                     GraphAnalysis.{h,cpp} 型推論 + 検証 → Diagnostic[]
   │ CompileGraph()                 Compiler.{h,cpp}     到達可能ノードの後順 → IR（CSE）→ スロット割当 → HLSL
   ▼
 CompileResult { hlsl, slots, diagnostics, sourceMap, stats, ir }
   │ EvaluateCpu()                  CpuEval.{h,cpp}      IR を CPU で評価（参照実装）
   │ PackParamRecord()              Compiler.cpp         スロット表 → float4 レコード
```

| ファイル | 役割 |
|---|---|
| `MatGraphTypes.{h,cpp}` | `ValueType`（型）・`CheckCast`（自動キャスト規則）・`Value`（リテラル）・数値の文字列化・`Diagnostic` と診断コード |
| `NodeLibrary.{h,cpp}` | ノード定義 `NodeDef` と登録表 `NodeLibrary`、`PropView`、`NormalizeProp` |
| `BuiltinNodes.cpp` | 組み込みの 52 ノード（データ駆動。ここへ 1 件足すだけで増える） |
| `GraphModel.{h,cpp}` | `MaterialGraph`（データモデル + UI 向け API + 変更通知 + バージョンカウンタ） |
| `GraphAnalysis.{h,cpp}` | `AnalyzeGraph`（型推論・検証）・`CollectParameters` |
| `Compiler.{h,cpp}` | `CompileGraph`・IR・スロット表・`MapDxcLog`・`PackParamRecord` |
| `CpuEval.{h,cpp}` | `EvaluateCpu`（数値テストの正解） |
| `GraphIO.{h,cpp}` | `.dxmg` の読み書き（正準形・往復バイト一致） |
| `hlsl/UnoMatContractRef.hlsli` | 生成 HLSL の契約の**参照実装**（テスト用。エンジン側 G2a/G2b の手本） |

ビルド: `src/renderer/CMakeLists.txt` の静的ライブラリ `MatGraph`（`nlohmann_json` のみ依存）。インクルードは `#include "renderer/matgraph/…"`。

---

## 2. `.dxmg`（マテリアルグラフ）

### 2.1 例

```json
{
  "version": 1,
  "kind": "material",
  "guid": "3f9a1c2e",
  "name": "rock_wet",
  "settings": { "blendMode": "Opaque", "maskClip": 0.5, "shadingModel": "DefaultLit", "twoSided": false },
  "nodes": {
    "n_071829": { "type": "TextureSample", "in": { "Tex": "n_d4e5f6.Out", "UV": "n_a1b2c3.UV" } },
    "n_9a8b7c": { "type": "ScalarParameter", "props": { "default": 0.6, "group": "Surface", "name": "Roughness", "range": [0.0, 1.0] } },
    "n_a1b2c3": { "type": "TexCoord", "props": { "offset": [0.0, 0.0], "tiling": [2.0, 2.0] } },
    "n_d4e5f6": { "type": "TextureParameter", "props": { "default": "textures/rock/rock_diff.jpg", "name": "Albedo", "sampler": "Color" } },
    "out": { "type": "MaterialOutput", "in": { "BaseColor": "n_071829.RGB", "Metallic": 0.0, "Roughness": "n_9a8b7c.Out" } }
  },
  "comments": {
    "c_11": { "color": "#3a6", "rect": "-560 -20 380 260", "text": "岩の色" }
  },
  "layout": {
    "n_071829": "-140 30",
    "n_9a8b7c": "-140 260",
    "n_a1b2c3": "-380 60",
    "n_d4e5f6": "-380 -100",
    "out": "160 40"
  }
}
```

### 2.2 トップレベル

| キー | 内容 |
|---|---|
| `version` | 整数。現在 **1**。これより大きい版は読み込みを拒否する（「エンジンを更新してください」）。将来版を上げたときは読み込み側がマイグレーションを持つ |
| `kind` | `"material"`。省略可。`"function"`（G4 のマテリアル関数）は G1 では未対応で読み込みを拒否する |
| `guid` | 8 桁 hex 等の文字列（グラフの識別子。`MaterialGraph` 生成時に乱数で振る） |
| `name` | 表示名 |
| `settings` | `blendMode`（`Opaque` / `Masked` / `Translucent` / `Additive`）・`shadingModel`（`DefaultLit` / `Unlit`、予約: `Subsurface` / `ClearCoat` / `Cloth` / `Hair` / `Eye`）・`twoSided`（bool）・`maskClip`（Masked のしきい値）。**HLSL には出ない**（G2b が PSO 選択に使う） |
| `nodes` | ノード ID → ノード（§2.3） |
| `comments` | コメント ID → `{ "color", "rect": "x y w h", "text" }`。IR に影響しない |
| `layout` | ノード ID → `"x y"`（エディタ上の位置）。**全ノードを書く**。ノード移動の差分をこの 1 行に閉じ込めるため |

未知のトップレベルキーは読み捨てる（前方互換のための保持はしない。ノード / プロパティ / ピンは保持する。§2.7）。

### 2.3 ノード

```
"<id>": { "type": "<型 ID>", "in": { "<入力ピン>": <値>, … }, "props": { "<プロパティ>": <値>, … } }
```

- **ID**: `[A-Za-z0-9_]+`（`.` は使えない。`.` は接続表記の区切り）。作成時に `n_` + 6 桁 hex を乱数で振り、**再採番しない**。出力ノードのように人が読める名前（`out`）も可。
- **`in`（接続は入力側に持つ）**: 値が文字列なら**接続** `"<接続元ID>.<出力ピン名>"`（ピン名を省略して `"<ID>"` だけなら接続元の**先頭の出力**）。数値 / 配列 / bool なら**リテラル**（未接続ピンへの直書き。Constant ノードを置かなくてよい）。接続とリテラルは排他。
- **`props`**: そのノードの設定値（型は §5 の登録表の `PropDecl`）。書かれていないプロパティは既定値。未知のプロパティ名 / 不正値は**そのまま保持**して診断（`E_BAD_PROP`）に回す。
- 空の `in` / `props` は書かない。

### 2.4 値の書式（リテラル）

| JSON | 型 | 備考 |
|---|---|---|
| 数値 | `float`（ピンが `int` なら `int`） | `1` と `1.0` は同じ。保存時は必ず `1.0` の形になる |
| `[a, b]` / `[a, b, c]` / `[a, b, c, d]` | `float2` / `float3` / `float4` | |
| `true` / `false` | `bool` | |
| 文字列 | 接続（上記）。テクスチャパスは `props`（`Texture` 型プロパティ）にだけ書く | |

### 2.5 正準形（往復でバイト一致させる規則）

`SaveDxmg(Load(text)) == text`（`text` が正準形のとき）。`Save(Load(Save(g))) == Save(g)` は常に成り立つ（テストで機械判定）。

- 改行は LF、末尾に改行 1 個、インデントは空白 2 個。**BOM なし**（読み込みは BOM を許す）。
- トップレベルのキー順は固定: `version, kind, guid, name, settings, nodes, comments, layout`。
- `nodes` / `comments` / `layout` の各エントリは**辞書順（バイト順）で 1 行 1 エントリ**。ノード内のキー順は `type, in, props`。`in` / `props` / `settings` の中は辞書順、1 行。
- 数値: 浮動小数は **float32 の最短ラウンドトリップ表記**（`0.6` は `0.6`、`0.6000000238…` にならない）。整数値の浮動小数には必ず `.0` を付ける（`2.0`）。`Int` は整数（`3`）。NaN / Inf は書かない（0 / ±最大値に丸める）。
- 文字列は JSON エスケープ（非 ASCII は生の UTF-8 のまま）。
- 内容が同じなら**書かない**（`SaveDxmgFileIfChanged`。mtime を汚さない）。

### 2.6 git 差分が 1 行で済む操作（テストで確認）

| 操作 | 変わる行 |
|---|---|
| ノードの移動 | `layout` の 1 行 |
| スロット値（Constant / Parameter の値）の編集 | そのノードの 1 行 |
| ワイヤの付け替え / 切断 | 接続先ノードの 1 行 |
| リテラルの編集 | そのノードの 1 行 |

### 2.7 互換とバージョニング

- **未知のノード型 / 未知のプロパティ / 宣言に無い入力ピン**は、読んでも書いても**消えない**（新しいエンジンで作ったファイルを古いエンジンが開いて保存しても情報が落ちない）。コンパイル時は診断（`E_UNKNOWN_NODE` / `W`/`E_UNKNOWN_PIN`）になる。
- `version` を上げる変更（キー名の変更・意味の変更）をしたら、読み込み側に旧版のマイグレーションを足す。**ノードの型 ID / ピン名 / プロパティ名 / 出力ピン名は公開仕様**（変えると既存ファイルが壊れる。廃止するときは別名を残す）。

---

## 3. `.dxmat` への `graph` キー拡張（仕様案・G2b で実装）

`.dxmat` は既存のマテリアルアセット（`resource/MaterialAssetIO`）。グラフ材質は **`.dxmat` を「グラフのインスタンス」として使う**。互換の原則は「**`graph` キーが無ければ従来と 1 バイトも変わらない**」。

```json
{
  "version": 2,
  "name": "rock_wet_mossy",
  "graph": "materials/rock_wet.dxmg",
  "params": { "Roughness": 0.8, "Albedo": "textures/moss/moss_diff.jpg", "Tint": [0.6, 0.7, 0.5, 1.0] },
  "uvTiling": [1.0, 1.0]
}
```

| 項目 | 規則 |
|---|---|
| 分岐 | `graph` キーがある → グラフ経路（`version` は 2 以上）。無い → 従来経路（`version:1`、`albedo` / `normal` / `metalRoughness` / `emissive` の 4 枚 + スカラー） |
| `graph` | `.dxmg` の assets 相対パス。読み込みは `LoadDxmgFile` |
| `params` | パラメータ名（`SlotInfo::name`）→ 値。数値 = Scalar、長さ 3〜4 の配列 = Vector（3 のときは A = 1）、文字列 = Texture のパス。`ParamOverrides`（`Compiler.h`）へそのまま写り、`PackParamRecord` がスロットへ書く。**グラフに無い名前は無効な上書きとして警告**（黙って捨てない） |
| 旧キーとの共存 | `graph` があるとき `albedo` 等の 4 枚のパスは**無視**する（あれば警告）。`uvTiling` の扱い（標準テンプレートの `TexCoord.tiling` へ 1 回だけ適用）は G2b で確定 |
| 検証（M2） | 従来の「テクスチャを 1 枚も参照しないと無効」は **`graph` があれば免除**（旧ファイルの判定は不変） |
| 書き出し | 使っているキーだけ書く（旧ファイルの読み → 書き戻しでキーが増えない現行方針と同じ）。`graph` / `params` は書く必要があるときだけ |
| 多段インスタンス | 親がインスタンス（`graph` が `.dxmat` を指す）を許すかは G4。G1 のスロット表は「グラフ 1 個 = レコード形式 1 個」で、多段は上書きのマージだけで実現できる |
| データ構造の追加 | `MaterialAssetData` に `std::string graphPath;` と `ParamOverrides params;` を足す（旧フィールドは触らない） |

---

## 4. 型規則

### 4.1 型

| `ValueType` | HLSL | 備考 |
|---|---|---|
| `F1` `F2` `F3` `F4` | `float` … `float4` | 32bit float |
| `Int` | `int` | `Int` 定数ノードと将来の整数ピン用。演算ノードは受けない（float へ変換して受ける） |
| `Bool` | `bool` | `Compare` の出力 / `Select` の条件。定数は `Bool` ノード。**実行時の真偽値**（StaticSwitch の「コンパイル時定数」は G4） |
| `Tex2D` | `Texture2D<float4>` | 値ではなく「どの SRV か」。`TextureParameter` / 暗黙テクスチャの出力 |
| `Sampler` `Mat3` `Mat4` | — | **予約**。型としては存在するが、現行ノードのピンには使わない。他の型へのキャストは常に禁止 |

### 4.2 自動キャスト規則（`CheckCast(from, to)`）

| from → to | 結果 | 生成コード |
|---|---|---|
| 同じ型 | OK | そのまま |
| `F1` → `F2/F3/F4` | OK（スカラー拡張。警告なし） | スウィズル `v3.xxx` |
| `F(n)` → `F(m)`、`m < n` | OK + **警告 `W_TRUNCATE`**（先頭 m 成分） | `v3.xyz` |
| `F(n)` → `F(m)`、`1 < n < m` | **禁止**（`E_EXPAND_FORBIDDEN`。「Append を使ってください」） | — |
| `Int` → `F1..F4` | OK（警告なし） | `((float3)v4)` |
| `Bool` → `F1..F4` / `Int` | OK（true = 1、false = 0） | `((float)v4)` / `((int)v4)` |
| `F` → `Int`、`F` → `Bool`、`Int` → `Bool` | **禁止**（`E_TYPE_MISMATCH`。「Compare ノードで…」） | — |
| `Tex2D` ↔ 数値 | **禁止**（「テクスチャを float に繋げません。TextureSample を通してください」） | — |
| `Sampler` / `Mat3` / `Mat4` ↔ 他 | **禁止** | — |

`Invalid`（型が決まらない）は常に禁止。禁止のときは日本語の理由（`CastResult::reason`）が必ず入る。

### 4.3 ピンの 3 種類（`PinMode`）

| モード | 意味 | 例 |
|---|---|---|
| `Fixed` | 型が固定。接続元を `CheckCast` で受ける | `Cross.A`（float3）、`TextureSample.UV`（float2）、出力ノードの各ピン |
| `Poly` | 多相（F1〜F4）。**同じ `polyGroup` の入力の最大次元**に揃う。各入力は「同じ次元」か「F1（スカラー拡張）」でなければならない。出力が `Poly` なら結果もその次元 | `Add.A/B`、`Lerp.A/B/Alpha`、`Clamp.X/Min/Max` |
| `Free` | 接続元の型をそのまま受ける（次元を保つ）。Int / Bool は float へ変換。`allowTexture` で `Tex2D` も受ける | `Append.A/B`、`ComponentMask.X`、`Custom.In0..7`、`Reroute.In` |

**次元エラー**: `Add` の `A=float3, B=float2` は `E_DIM_MISMATCH`（メッセージは「Add: A=float3, B=float2。次元が違います」）。接続の時点（`CanConnect`）で拒否される。

### 4.4 未接続ピンの解決順

1. 接続（`in` に文字列）
2. リテラル（`in` に数値 / 配列 / bool）
3. `implicitTexture`（`TextureSample` / `NormalMap` の `Tex`）→ ノードの `texture` プロパティから暗黙のテクスチャスロットを作る
4. ピン宣言の既定値（`hasDefault`）
5. 組み込み入力（`defaultBuiltin`: `uv` / `vertexNormalWS` など。同じ組み込みはグラフ内で 1 回だけ評価される）
6. 必須（`required`）なら `E_MISSING_INPUT`、そうでなければ「無い」（Free の任意入力）

出力ノードの未接続ピンは何も出力せず、`UnoSurfaceDefault()` の値のまま。

### 4.5 出力ノード `MaterialOutput`

| ピン | 型 | `UnoSurface` の欄 | 既定（未接続時） |
|---|---|---|---|
| `BaseColor` | float3 | `baseColor` | (0.5, 0.5, 0.5) |
| `Metallic` | float | `metallic` | 0 |
| `Roughness` | float | `roughness` | 0.5 |
| `Normal` | float3（**接空間**） | `normalTS`（繋ぐと `hasNormal = true`） | (0,0,1)・`hasNormal=false` |
| `Emissive` | float3 | `emissive` | 0 |
| `AmbientOcclusion` | float | `ao` | 1 |
| `Opacity` | float | `opacity` | 1 |
| `OpacityMask` | float | `opacityMask` | 1 |
| **予約（灰色・接続不可 = `E_RESERVED_PIN`）** | | `anisotropy` `subsurfaceColor` `subsurfaceOpacity` `clearCoat` `clearCoatRoughness`（＋ `Specular` `Tangent` `WorldPositionOffset` `PixelDepthOffset` `Refraction` は欄なし） | |

出力ノードは**ちょうど 1 個**（0 個 = `E_NO_OUTPUT`、2 個以上 = `E_MULTI_OUTPUT`）。

### 4.6 診断コード

`Diagnostic { severity, code, nodeId, pin, message, hint, reachable, line }`。**`reachable=false` は出力に繋がらない（死んだ）ノードの診断で、コンパイルの成否に影響しない**。上流にエラーがある下流ノードは連鎖エラーを出さない（エラーは根本に 1 個だけ）。

| コード | 重大度 | 意味 |
|---|---|---|
| `E_UNKNOWN_NODE` | Error | 登録表に無いノード型 |
| `E_UNKNOWN_PIN` | Error / Warning | 接続元に出力ピンが無い（Error）／ノードに宣言されていない入力名がある（Warning・無視される） |
| `E_DANGLING_LINK` | Error | 接続元ノードが存在しない |
| `E_CYCLE` | Error | 循環（メッセージに `a → b → a` の経路） |
| `E_MISSING_INPUT` | Error | 必須入力が未接続 |
| `E_TYPE_MISMATCH` | Error | 型が合わない（テクスチャ ↔ 数値、float → int/bool など） |
| `E_DIM_MISMATCH` | Error | 多相入力の次元が揃わない |
| `E_EXPAND_FORBIDDEN` | Error | float2 → float3 のような拡張 |
| `W_TRUNCATE` | Warning | 切り詰め（float4 → float3 など） |
| `E_APPEND_OVERFLOW` | Error | `Append` の合計が 5 成分以上 |
| `E_MASK_RANGE` | Error | `ComponentMask` が存在しない成分を選んだ／`Split.B` のように存在しない出力を使った |
| `E_MASK_EMPTY` | Error | `ComponentMask` が 0 成分 |
| `E_RESERVED_PIN` | Error | 予約ピンへの接続 / 値 |
| `E_BAD_PROP` | Error | プロパティ値が型に合わない（列挙値の綴りなど） |
| `E_DUP_PARAM` | Error | パラメータ名の重複 / 空（到達可能なパラメータだけ） |
| `E_NO_OUTPUT` / `E_MULTI_OUTPUT` | Error | 出力ノードが 0 個 / 2 個以上 |
| `E_CUSTOM_EMPTY` | Error | `Custom` の本文が空 |
| `E_CPU_UNSUPPORTED` | Error | CPU 評価できないノード（`EvaluateCpu` のみ） |
| `I_DEAD_NODE` | Info | 出力に繋がっていない |
| `E_DXC` | Error | DXC のコンパイルエラー。`MapDxcLog` が `node:<id>:<行>:<列>` から `nodeId` と本文の行番号（`line`）へ逆引きする |

---

## 5. ノード一覧（組み込み 52 種）

`MatGraphTests --list-nodes` で登録表から再生成できる。ピン型の `float(1-4)` は多相 / Free、`*` は必須。プロパティ・出力ピンの詳細は `BuiltinNodes.cpp`（それが正）。

| 型 ID | カテゴリ | 入力（* = 必須） | 出力 | 説明 |
|---|---|---|---|---|
| Bool | 定数 |  | Out:bool | 真偽値の定数。Select の条件に使う。 |
| Float | 定数 |  | Out:float | スカラー定数。 |
| Float2 | 定数 |  | Out:float2 | 2 成分の定数。 |
| Float3 | 定数 |  | Out:float3 | 3 成分の定数（色ピッカー表示は UI 側の指定）。 |
| Float4 | 定数 |  | Out:float4, RGB:float3, A:float | 4 成分の定数（RGBA の色として使える）。srgb を有効にすると、格納時にリニアへ変換される。 |
| Int | 定数 |  | Out:int | 整数定数（HLSL のリテラルとして埋め込まれる）。 |
| ScalarParameter | パラメータ |  | Out:float | 名前つきのスカラーパラメータ。マテリアルインスタンスで値を上書きできる。 |
| TextureParameter | パラメータ |  | Out:texture2D | 名前つきのテクスチャパラメータ。バインドレスの SRV 添字がスロットに入る。 |
| VectorParameter | パラメータ |  | Out:float4, RGB:float3, R:float, G:float, B:float, A:float | 名前つきのベクトル（色）パラメータ。RGBA の各成分を別の出力から取り出せる。 |
| CameraPosition | 座標 |  | Out:float3 | カメラのワールド座標。 |
| CameraVector | 座標 |  | Out:float3 | ピクセルからカメラへ向かう単位ベクトル（ワールド空間）。 |
| TexCoord | 座標 |  | UV:float2 | メッシュの UV に Tiling と Offset を掛けた座標。頂点に焼かれた UV スケールの上に乗る。 |
| VertexColor | 座標 |  | Out:float4, RGB:float3, R:float, G:float, B:float, A:float | 頂点カラー。 |
| VertexNormalWS | 座標 |  | Out:float3 | 法線マップを適用する前の幾何法線（ワールド空間）。 |
| WorldPosition | 座標 |  | Out:float3 | ピクセルのワールド座標。 |
| Time | 時間 |  | Out:float | 起動からの経過秒。 |
| Abs | 数学 | X:float(1-4)* | Out:float(1-4) | 絶対値。 |
| Add | 数学 | A:float(1-4), B:float(1-4) | Out:float(1-4) | A + B。次元が違うときはどちらかがスカラー（float）であること。 |
| Ceil | 数学 | X:float(1-4)* | Out:float(1-4) | 切り上げ。 |
| Clamp | 数学 | X:float(1-4)*, Min:float(1-4), Max:float(1-4) | Out:float(1-4) | X を Min〜Max に制限する。 |
| Cos | 数学 | X:float(1-4)* | Out:float(1-4) | コサイン（ラジアン）。 |
| Divide | 数学 | A:float(1-4), B:float(1-4) | Out:float(1-4) | A / B。safe を有効にすると B を 1e-6 以上に制限してゼロ除算を避ける。 |
| Floor | 数学 | X:float(1-4)* | Out:float(1-4) | 切り捨て。 |
| Frac | 数学 | X:float(1-4)* | Out:float(1-4) | 小数部分（X - Floor(X)）。 |
| Lerp | 数学 | A:float(1-4), B:float(1-4), Alpha:float(1-4) | Out:float(1-4) | A と B の線形補間。Alpha は float または同じ次元。 |
| Max | 数学 | A:float(1-4), B:float(1-4) | Out:float(1-4) | 大きい方。 |
| Min | 数学 | A:float(1-4), B:float(1-4) | Out:float(1-4) | 小さい方。 |
| Multiply | 数学 | A:float(1-4), B:float(1-4) | Out:float(1-4) | A * B（成分ごと）。 |
| OneMinus | 数学 | X:float(1-4)* | Out:float(1-4) | 1 - X。 |
| Power | 数学 | Base:float(1-4)*, Exp:float(1-4) | Out:float(1-4) | Base の Exp 乗。Base は 0 以上に制限される。 |
| Saturate | 数学 | X:float(1-4)* | Out:float(1-4) | 0〜1 に制限する。 |
| Sin | 数学 | X:float(1-4)* | Out:float(1-4) | サイン（ラジアン）。 |
| Smoothstep | 数学 | Min:float(1-4), Max:float(1-4), X:float(1-4)* | Out:float(1-4) | Min〜Max の間をなめらかに 0〜1 へ補間する（Min と Max は別の値にすること）。 |
| Sqrt | 数学 | X:float(1-4)* | Out:float(1-4) | 平方根（負数は 0 として扱う）。 |
| Step | 数学 | Edge:float(1-4), X:float(1-4)* | Out:float(1-4) | X が Edge 以上なら 1、未満なら 0。 |
| Subtract | 数学 | A:float(1-4), B:float(1-4) | Out:float(1-4) | A - B。 |
| Append | ベクトル | A:float(1-4)*, B:float(1-4)* | Out:float(1-4) | ベクトルを連結する（float + float3 → float4 など）。合計 4 成分まで。 |
| ComponentMask | ベクトル | X:float(1-4)* | Out:float(1-4) | ベクトルから R / G / B / A のうち選んだ成分を取り出す。 |
| Cross | ベクトル | A:float3*, B:float3* | Out:float3 | 外積（float3）。 |
| Dot | ベクトル | A:float(1-4)*, B:float(1-4)* | Out:float | 内積。A と B は同じ次元にそろう。 |
| Length | ベクトル | X:float(1-4)* | Out:float | ベクトルの長さ。 |
| Normalize | ベクトル | X:float(1-4)* | Out:float(1-4) | 正規化（長さ 1 に）。長さ 0 のときは 0 に近い値を返し、NaN にならない。 |
| Split | ベクトル | X:float(1-4)* | R:float, G:float, B:float, A:float | ベクトルを R / G / B / A の float に分解する。 |
| NormalMap | テクスチャ | Tex:texture2D, UV:float2, Strength:float | Out:float3 | 法線マップを接空間の法線へ展開する（xy を 2 倍して 1 を引き、強さを掛けて z を再構成。OpenGL 規約）。 |
| TextureSample | テクスチャ | Tex:texture2D, UV:float2, MipBias:float | RGBA:float4, RGB:float3, R:float, G:float, B:float, A:float | テクスチャをサンプルする。Tex が未接続なら、このノードの texture プロパティが使われる（省略記法）。 |
| Desaturation | 色 | Color:float3*, Fraction:float | Out:float3 | 彩度を落とす（Rec.709 の輝度へ補間）。Fraction=1 で完全な白黒。 |
| Fresnel | シェーディング | Normal:float3, Exponent:float, BaseReflect:float | Out:float | 視線と法線のなす角による反射率（Schlick 近似）。縁に近いほど 1 に近づく。 |
| Compare | 制御 | A:float, B:float | Out:bool | A と B を比較して bool を返す。 |
| Custom | 制御 | In0:any, In1:any, In2:any, In3:any, In4:any, In5:any, In6:any, In7:any | Out:float(1-4) | HLSL の式または関数本体を書く。入力は In0〜In7（接続したものだけが引数になる）、本文に return があれば関数本体、無ければ式として扱う。テクスチャは Texture2D として渡される。 |
| Reroute | 制御 | In:any* | Out:float(1-4) | ワイヤの中継点。型はそのまま引き継ぐ。生成コードには現れない。 |
| Select | 制御 | A:float(1-4), B:float(1-4), Cond:bool | Out:float(1-4) | Cond が true なら A、false なら B。両方を評価してから選ぶ（テクスチャサンプルの微分を壊さない）。 |
| MaterialOutput | 出力 | BaseColor:float3, Metallic:float, Roughness:float, Normal:float3, Emissive:float3, AmbientOcclusion:float, Opacity:float, OpacityMask:float, Specular:float(予約), Anisotropy:float(予約), SubsurfaceColor:float3(予約), SubsurfaceOpacity:float(予約), ClearCoat:float(予約), ClearCoatRoughness:float(予約), Tangent:float3(予約), WorldPositionOffset:float3(予約), PixelDepthOffset:float(予約), Refraction:float3(予約) |  | マテリアルの出力。グラフに 1 個だけ置く。未接続のピンは既定のサーフェス値のまま。灰色のピンは将来用の予約（接続不可）。 |

主なプロパティ（型 → プロパティ名: 役割 / 種別）。**種別**は再コンパイルの要否を決める: `Code` = 変えると HLSL が変わる、`Slot` = 値はパラメータプールのスロット（変えても再コンパイル不要。`inline: true` のノードでは Code 扱い）、`Meta` = 名前・グループ・範囲など。

| 型 ID | プロパティ |
|---|---|
| Float / Float2 / Float3 / Float4 | `value`（Slot）、`inline`（Code。true で HLSL リテラルへ畳み込む）、Float4 は `srgb`（Meta） |
| Int / Bool | `value`（Code。HLSL リテラル） |
| ScalarParameter | `name` `range`[min,max] `group` `priority`（Meta）、`default`（Slot） |
| VectorParameter | `name` `srgb` `group` `priority`（Meta）、`default`（Slot・RGBA） |
| TextureParameter | `name` `group` `priority`、`sampler`（Meta。`Color` = sRGB / `LinearColor`・`Mask` = リニア / `Normal` = 法線。`GetOrLoadTexture` の sRGB / usage を決める）、`default`（Slot・パス） |
| TexCoord | `tiling` `offset`（Slot・float2）、`inline`（Code） |
| TextureSample / NormalMap | `samplerState`（Code。`AnisoWrap` `AnisoClamp` `LinearWrap` `LinearClamp` `PointWrap` `PointClamp`）、`texture`（Slot・Tex 未接続時のパス）、`usage`（Meta・同 `sampler`） |
| Divide | `safe`（Code。B を `max(B, 1e-6)` にする） |
| ComponentMask | `r` `g` `b` `a`（Code。取り出す成分） |
| Compare | `mode`（Code。`Less` `LessEqual` `Greater` `GreaterEqual` `Equal` `NotEqual`） |
| Custom | `code`（Code。式または関数本体）、`outputType`（Code。`float`〜`float4`） |

**設計書 §3.7 との対応**: 実装済み = #1〜7（Float / Float2 / Float3 / Float4 / ScalarParameter / VectorParameter / TextureParameter）・#9 TextureSample・#10 NormalMap・#14 TexCoord・#15 WorldPosition・#17 VertexNormalWS・#19 VertexColor・#20 CameraPosition・#21 CameraVector・#22 Time・#25〜32（Add 〜 OneMinus）・#33 Abs・#34 Power・#35 Min・#36 Max・#37 Frac・#38 Sine（→ `Sin` / `Cos`。ラジアン入力で Period は無い）・#39 Dot・#40 Cross・#41 Normalize・#42 Append・#43 ComponentMask・#44 Desaturation・#50 Fresnel・#54 If（→ `Compare` + `Select`）・#55 Custom・#56 Reroute・#60 MaterialOutput。設計書に無い追加 = `Int` `Bool` `Cos` `Floor` `Ceil` `Sqrt` `Step` `Smoothstep` `Length` `Split`（次の候補に挙がっていたもの + 型システム検証用）。**未実装（G3）** = #8 ObjectParameter・#11 TriplanarSample・#12 Flipbook・#13 ParallaxOcclusionMapping・#16 ObjectPosition・#18 VertexTangentWS・#23 Panner・#24 Rotator・#45 BlendMode・#46 sRGB⇄Linear・#47 Noise・#48 Voronoi・#49 Checker・#51 ReflectionVector・#52 NormalFromHeight・#53 BlendNormals。#57〜59（関数）は G4。

---

## 6. ノード登録表の増やし方

`BuiltinNodes.cpp` に登録を 1 件足す。コンパイラ・型推論・モデル・シリアライズ・CPU 評価・テスト（ライブラリ一括検査）は**登録表だけを見る**ので、他のファイルは触らない。

```cpp
// 例: Contrast（コントラスト）。X を 0.5 中心に Amount 倍する
{
    NodeDef d;
    d.type = "Contrast"; d.displayName = "Contrast"; d.category = "色";
    d.description = "0.5 を中心にコントラストを掛ける。Amount=1 で変化なし。";
    d.keywords = "contrast コントラスト";
    d.inputs  = {PolyIn("X", true, 0.0f, "入力"), PolyIn("Amount", false, 1.0f, "倍率")};
    d.outputs = {PolyOut()};
    d.resultMode = PinMode::Poly;                                   // 結果の型 = 多相グループの次元
    d.hlsl = "(({X} - 0.5) * {Amount} + 0.5)";                       // {Pin} = 型キャスト済みの接続元
    d.eval = [](EvalCtx& c) {                                        // CPU 参照実装（HLSL と同じ式を写す）
        const CpuVal& x = c.In("X"); const CpuVal& a = c.In("Amount");
        CpuVal r; r.type = c.ResultType();
        for (int k = 0; k < Dim(r.type); ++k) r.v[k] = (x.v[k] - 0.5f) * a.v[k] + 0.5f;
        return r;
    };
    lib.Register(std::move(d));
}
```

- **テンプレートの記法**（`NodeDef::hlsl`）: `{Pin}` = 入力ピンの式（接続元を型キャスト済み。プライマリ式なので括弧不要）、`{P:prop}` = プロパティを HLSL リテラルで、`{SLOT:prop}` = スロットの読み出し（`pool.F3(4)`。`inline` のときはリテラル）、`{T}` = 結果の HLSL 型。
- テンプレートで書けない分岐は `emit`（`EmitCtx` で `In` / `InType` / `Has` / `InIsLiteral` / `Slot` / `Props` / `Format` が使える）、既定の型推論で足りなければ `infer`（`Append` の 4 成分上限のように診断を出せる）。
- 補助関数が要るときは `helperName` + `helperHlsl`（生成ファイルの先頭に**使われたときだけ・1 回だけ・名前順**で出る。名前は `MG_` で始めること）。
- 値を持つプロパティは `PropRole` を正しく付ける（`Slot` にすると値の編集で再コンパイルされない。`Texture` 型の `Slot` プロパティを持つ Tex2D 出力ノードはテクスチャスロットになる）。
- `ライブラリ一括検査`（`TestLibrary`）が、説明・カテゴリ・ピン名の一意性・HLSL の作り方・CPU 評価の有無・プロパティ既定値の正規化を機械的に確かめる。数値の正しさは `matgraph_dxc_test` が「登録表の全数値ノードを 1 個ずつ自動配線」して WARP で CPU と比べるので、**登録するだけで GPU/CPU の一致検査に乗る**（配線できないノードだけテスト側の除外リストに足す）。
- 型 ID・ピン名・プロパティ名・出力ピン名は `.dxmg` の公開仕様。決めたら変えない。

---

## 7. 生成 HLSL の契約

### 7.1 生成ファイルの形

```hlsl
// @sm 6_6
// GENERATED by Uno MaterialGraph. DO NOT EDIT.  [source=materials/rock_wet.dxmg]  graph=9f3c1a2b4d5e6f70  codegen=1
#include "forward/ForwardGraph.hlsl"                 ← CompileOptions::includeLine。テストは参照契約ヘッダへ差し替える

float3 MG_NormalUnpack(float3 rgb, float strength)    ← 使われたノードの補助関数（名前順）
{ … }

float3 MG_Custom0(float3 In0, float3 In1)             ← Custom ノードの関数（本文の 1 行目 = `#line 1`）
{
#line 1 "node:cu"
    return (In0 * 2.0 + In1);
}

void UnoMatEval(UnoMatInput mi, UnoMatPool pool, out UnoSurface s)
{
    s = UnoSurfaceDefault();
#line 1 "node:n_d4e5f6"
    Texture2D<float4> v0 = pool.Tex(0);
#line 1 "node:n_a1b2c3"
    float2 v1 = (mi.uv * pool.F2(1) + pool.F2(2));
#line 1 "node:n_071829"
    float4 v2 = UnoSample(v0, UNO_SAMP_ANISO_WRAP, v1);
#line 1 "node:n_9a8b7c"
    float v3 = pool.F1(3);
#line 1 "node:out"
    s.baseColor = v2.xyz;
    s.metallic = 0.0;
    s.roughness = v3;
}
```

- **先頭行 `// @sm 6_6`**: `ShaderManager` がプロファイル（`ps_6_6` / `vs_6_6`）を選ぶための規約（G2b で実装）。
- **2 行目のヘッダ**: `graph=` は本体（3 行目以降）の FNV-1a 64。**パス / 時刻 / 値スロットの中身は入らない**。`source=` は `CompileOptions::sourceName` を渡したときだけ。
- **変数名**: 1 ノード出力 = 1 ローカル変数 `v<N>`（N は IR の通し番号）。CSE で合流したものは 1 個。`mi` / `pool` / `s` / `v<N>` は `UnoMatEval` 内で予約（Custom の本文は別関数なので衝突しない）。補助関数・Custom 関数は `MG_` 接頭辞で予約。
- **`#line 1 "node:<id>"`** は各ノード（Op）の直前に出る。DXC のエラーは `node:<id>:<行>:<列>: error: …` になるので `MapDxcLog` で `nodeId` + **Custom 本文内の行番号**へ逆引きできる（テストで確認済み）。`SourceMapEntry`（生成 HLSL の行 → ノード）も同じ情報を持つ。
- **生成コードは cbuffer を宣言しない**（b0 衝突の罠の回避。cbuffer と `PSMain` / `VSMain` はエンジン側の `ForwardGraph.hlsl` が固定で持つ）。`ResourceDescriptorHeap` を直接書かず、テクスチャは必ず `pool.Tex(slot)` 経由。`Sample` を直接書かず、必ず `UnoSample` / `UnoSampleBias` マクロ経由（VG resolve で `SampleGrad` へ差し替える点）。
- 必要な DXC オプション: `-HV 2021` と `ps_6_6`（`ResourceDescriptorHeap` と、ローカル変数としての `Texture2D` のため）。生成コードは警告を出さない（ゴールデン 10 本で確認）。

### 7.2 エンジン側が提供するもの（参照実装 = `src/renderer/matgraph/hlsl/UnoMatContractRef.hlsli`）

| 識別子 | 内容 |
|---|---|
| `struct UnoSurface` | `float3 baseColor; float metallic; float roughness; float3 normalTS; bool hasNormal; float3 emissive; float ao; float opacity; float opacityMask; float3 subsurfaceColor; float subsurfaceOpacity; float clearCoat; float clearCoatRoughness; float anisotropy; uint shadingModel;`（**予約欄も最初から持つ**。後から特殊シェーディングを足しても既存グラフ・生成物は壊れない） |
| `UnoSurfaceDefault()` | 既定値: baseColor=(0.5,0.5,0.5)、metallic=0、roughness=0.5、normalTS=(0,0,1)、hasNormal=false、emissive=0、ao=1、opacity=1、opacityMask=1、その他 0（CPU 側 `SurfaceValues` と同じ） |
| `struct UnoMatInput` | `float2 uv; float3 worldPos; float3 vertexNormalWS; float3 vertexTangentWS; float tangentW; float4 vertexColor; float4 svPos; float3 cameraPos; float time;`。生成コードが現在使うのは `uv` / `worldPos` / `vertexNormalWS` / `cameraPos` / `vertexColor` / `time` |
| `struct UnoMatPool` | メソッド `F1(uint slot)` / `F2` / `F3` / `F4`（float4 スロットの先頭成分を返す）、`Tex(uint slot)`（`Texture2D<float4>`。`ResourceDescriptorHeap[asuint(record[slot].x)]`）。**メソッド名と意味だけ揃えれば実装は自由**（参照実装は `StructuredBuffer` を直読み。エンジンは b2 の recordBase / プール SRV 添字で読む） |
| `UnoSample(tex, samp, uv)` / `UnoSampleBias(tex, samp, uv, bias)` | フォワードでは `tex.Sample` / `SampleBias`。VG resolve では `SampleGrad` へ。`Texture2D` のメソッドとして呼べる式であること（`UnoSample(...).xyz` と書かれる） |
| `UNO_SAMP_ANISO_WRAP` `_ANISO_CLAMP` `_LINEAR_WRAP` `_LINEAR_CLAMP` `_POINT_WRAP` `_POINT_CLAMP` | サンプラーへのマクロ（`TextureSample.samplerState` と 1 対 1）。エンジンの静的サンプラ s0（異方性 WRAP）/ s2（LINEAR CLAMP）/ s4（POINT CLAMP）と、設計書 §3.6 の s6 以降へ割り当てる |

### 7.3 主なノードの HLSL（決定論・ゴールデンで固定）

- 定数 / パラメータ: `pool.F<n>(slot)`（`inline` のときはリテラル）。`Bool` / `Int` は常にリテラル。
- 組み込み入力（`uv` など）: `float2 vN = mi.uv;` を 1 回だけ。
- `TextureSample`: `float4 vN = UnoSample(vTex, UNO_SAMP_<STATE>, vUV);`（`MipBias` が 0 リテラル以外なら `UnoSampleBias`）。出力ピン `RGB` / `R` … は変数のスウィズル。
- `Normalize`: `(X * rsqrt(max(dot(X, X), 1e-12)))`（ゼロ長ガード。スカラーは `X * X`）。`Power`: `pow(max(Base, 0.0), Exp)`。`Sqrt`: `sqrt(max(X, 0.0))`。`Select`: `(Cond ? A : B)`（両枝を評価）。
- `NormalMap`: `MG_NormalUnpack(UnoSample(...).xyz, strength)`（xy を `*2-1` して強さを掛け z を `sqrt(saturate(1 - dot(xy, xy)))` で再構成。**接空間・OpenGL 規約**。z を 0.35 で打ち止めるガードは共通シェーディング側 `PerturbNormal` の担当）。

---

## 8. スロット表（パラメータプールのレコード）

**値の編集で再コンパイルしないための仕掛け**。定数もパラメータもテクスチャも、HLSL に値を埋め込まず**レコード（float4 の並び）のスロット添字**で読む。値を変えるのはレコードの書き換えだけ。

### 8.1 `SlotInfo`

| 欄 | 意味 |
|---|---|
| `slot` | float4 単位の添字。**0 始まり・連続・一意**。並びは IR の順（出力ノードの入力ピン宣言順に後順で辿った順）で決定論 |
| `kind` | `Scalar`（ScalarParameter）/ `Vector`（VectorParameter）/ `Texture`（TextureParameter）/ `Constant`（Float 系ノードの `value`）/ `NodeProp`（`TexCoord` の `tiling` / `offset` など）/ `ImplicitTexture`（Tex 未接続の `TextureSample` / `NormalMap` の `texture`） |
| `type` | `F1`〜`F4` / `Tex2D` |
| `name` `group` `priority` `hasRange` `rangeMin/Max` | パラメータの UI 情報（`Constant` / `NodeProp` / `ImplicitTexture` は `name` が空 = インスタンスで上書きできない） |
| `nodeId` `prop` | 値の出どころ（どのノードのどのプロパティか） |
| `value[4]` | 既定値（利用者が見る値。`srgb` 変換前） |
| `texturePath` `textureUsage` | テクスチャの既定パスと読み込み種別（`Color` = sRGB、`LinearColor` / `Mask` = リニア、`Normal`。`GetOrLoadTexture` の sRGB / usage の決定に使う） |
| `srgb` | true なら格納時に RGB を sRGB → リニアへ変換（A は変換しない） |

### 8.2 レコードのレイアウト（`PackParamRecord`）

`std::vector<float>`（`slotCount × 4`）。1 スロット = float4。

| スロットの型 | x | y | z | w |
|---|---|---|---|---|
| `F1` | 値 | 0 | 0 | 0 |
| `F2` | x | y | 0 | 0 |
| `F3` | x | y | z | 0 |
| `F4` | x | y | z | w |
| `Tex2D` | `asuint` = SRV 添字 | 1 / 幅 | 1 / 高さ | 0（予約） |

- **上書き**: `ParamOverrides`（パラメータ名 → `ParamOverride`）。名前を持つスロットだけ上書きできる。テクスチャの上書きはパス（`ParamOverride::texture`）。
- **テクスチャの解決**: `TextureResolver(path, usage) → { srvIndex, width, height }` を呼び出し側（G2b）が渡す（`ResourceManager::GetOrLoadTexture(path, sRGB, usage…)` の永続 SRV 添字を返す）。`nullptr` なら SRV 添字は 0。
- **sRGB**: `srgb` のスロットは上書き値も含めて変換してから書く。GPU 側では何もしない。CPU 評価も同じ `PackParamRecord` を通るので、GPU と CPU が同じ値を見る。

### 8.3 再コンパイルが要る / 要らない

| 編集 | HLSL | 再コンパイル |
|---|---|---|
| `Slot` プロパティ（定数 / パラメータの値、`TexCoord` の tiling など、テクスチャパス）、パラメータの名前 / グループ / 範囲 | **変わらない**（スロット表の中身だけ変わる） | **不要**（レコードを書き直すだけ） |
| ノード / ワイヤ / リテラル / `Code` プロパティ / `inline` の変更、スロットが増減する変更 | 変わる | 必要 |
| ノードの移動、コメント、`settings` | 変わらない | 不要（`settings` は PSO 選択には効く） |

判定用に `MaterialGraph::StructureVersion()`（HLSL が変わりうる変更でだけ増える）と `CompileResult::hash` / `hlsl` の比較の両方が使える。値を編集した後に `CompileGraph` し直しても `hash` は同じ（テストで確認）。

### 8.4 注意

- スロットを持つのは**出力に繋がっているノードだけ**。死んだパラメータは一覧（`CollectParameters`。`reachable=false`）には出るがスロットは無い。繋ぎ直すとスロットが増えて再コンパイルになる。
- スロットを持つノードは CSE で**合流しない**（同値の Float 定数が 2 個あっても別スロット。独立に編集できる）。`inline` の同値定数は合流する。
- 同名パラメータは `E_DUP_PARAM`（インスタンスの上書きが名前で決まるため）。

---

## 9. 決定論

同じグラフ → 同じ HLSL バイト列。次を**変えても**HLSL は変わらない（テストで確認）: ノードの位置、コメント、`.dxmg` のキー順、スロット値、パラメータ名 / グループ / 範囲。

- 走査順は「出力ノードから入力ピンの**宣言順**に後順 DFS」。`std::map`（ID 辞書順）の走査順には依存しない。
- 数値の文字列化は `FormatFloatShort` / `FormatHlslFloat` に一本化（最短ラウンドトリップ、負数は `(-0.5)`、`-0` は `0`、NaN / Inf は 0 / ±最大値）。
- 補助関数は名前順、Custom 関数は IR 順。
- ID 採番だけは既定で乱数（`MaterialGraph::SetIdSeed` でテスト用に固定できる）。ID は `#line` に出るが、既存ファイルの ID は変わらない。

---

## 10. C++ API 概要

```cpp
#include "renderer/matgraph/Compiler.h"   // GraphModel / GraphAnalysis / NodeLibrary / MatGraphTypes を含む
#include "renderer/matgraph/GraphIO.h"
#include "renderer/matgraph/CpuEval.h"
using namespace dx12e::matgraph;

MaterialGraph g;                                   // 既定は NodeLibrary::Builtin()
LoadDxmgFile("materials/rock.dxmg", g, &err);
CompileResult r = CompileGraph(g);                 // r.ok / r.hlsl / r.slots / r.diagnostics
auto rec = PackParamRecord(r, overrides, resolver);
SaveDxmgFileIfChanged("materials/rock.dxmg", g);
```

UI / MCP 向けの主な API（`MaterialGraph`）:

| 分類 | API |
|---|---|
| ノード | `AddNode(type, x, y, id={})` / `RemoveNode(id, &undo)` / `RestoreNode(undo)` / `MoveNode` / `SetNodeProp(id, key, json)` / `GetNodeProp` |
| 接続 | `CanConnect(from, fromPin, to, toPin)`（`Ok` / `Warn` / `Reject` + 日本語の理由）/ `Connect` / `Disconnect` / `SetLiteral` / `ClearLiteral` / `Binding` / `Edges` / `EdgesFrom` / `EdgesTo` |
| 型 | `QueryPin(node, pin, isOutput)`（存在・解決後の型・接続の有無・宣言）/ `PinType` / `Analysis()`（型推論 + 診断。`Version()` が同じ間キャッシュ。**参照は次の変更で無効**） |
| 通知 | `AddListener` / `RemoveListener`（`GraphChange{ kind, node, pin, affectsStructure }`）/ `Version()` / `StructureVersion()` |
| コメント・設定 | `AddComment` / `SetComment` / `RemoveComment` / `SetSettings` |
| その他 | `Clone()`（通知・キャッシュ無しの複製）/ `GenerateNodeId` / `SetIdSeed` / `NodeLibrary::Search(query)`（パレット用の検索）/ `CollectParameters(g)`（パラメータ一覧） |

**罠**: メソッド名は `SetProp` / `GetProp` にしない（windows.h のマクロ `SetProp` → `SetPropW` と衝突する）。`CanConnect` の多相ピンは試しに繋いで再解析するため、ドラッグ中は「ホバー先が変わったときだけ」呼ぶこと。

---

## 11. テスト

| 対象 | 実行 | 内容 |
|---|---|---|
| `MatGraphTests` | `ctest -R MatGraphTests` | 型規則 / モデル API（通知・バージョン・接続拒否・Undo 復元）/ 診断（18 ケース）/ コード生成（決定論・CSE・死んだノード・Reroute・キャスト・ソースマップ）/ スロット / Custom / CPU 評価（全数値ノード）/ `.dxmg` 往復・差分行数 / 登録表の一括検査 / ゴールデン 10 本 |
| `MatGraphDxcTests`（ラベル `dxc`） | `ctest -L dxc` | ゴールデン 10 本を DXC で `ps_6_6` と `cs_6_6` にコンパイル / DXC エラー → ノード逆引き / WARP で CPU との数値比較（ゴールデン + 登録表の全数値ノード 41 個を自動配線 + ランダムグラフ 40 本 × 96 点）。`dxcompiler.dll` が無ければ `SKIP` で終了コード 0、WARP が SM 6.6 の CS を走らせられなければ GPU 比較だけスキップ |

- ゴールデンの更新: `MATGRAPH_UPDATE_GOLDEN=1 MatGraphTests.exe`（`*.dxmg` を正準形に書き直し、`*.hlsl.expected` を再生成）。**git diff を目視してからコミット**。
- CLI: `MatGraphTests.exe --compile x.dxmg`（診断 + 生成 HLSL）/ `--list-nodes`（§5 の表）。
- 数値の許容: 相対誤差 `|a-b| / (1 + |a| + |b|) ≤ 1e-4`（実測の最大は 2e-5）。不連続点（Floor / Frac / Step）はランダムグラフから除外し、ノード単体のテストでだけ扱う。

---

## 12. 既知の制約（G1）

- `Bool` は実行時の値。コンパイル時定数の `StaticSwitch`（HLSL の分岐でパーミュテーションを作る）は G4。
- `Custom` は CPU 評価できない（`E_CPU_UNSUPPORTED`）。テクスチャ系は `EvalEnv::sampleTexture` を渡したときだけ CPU 評価できる。
- マテリアル関数（`.dxmf`・`FunctionCall`）、`ObjectParameter`（b0 自由枠）、画面微分を使うノード（`NormalFromHeight` / `DDX` / `DDY`。`NodeDef::needsPixelDerivatives` を予約済み）は未対応。
- 生成コードは `UnoSurface` を埋めるだけ。シェーディング尾部の外出し（`ForwardShade.hlsli`）・`ForwardGraph.hlsl`・パラメータプール・b2 の読み替え・`// @sm` の解釈は G2a / G2b。
- `Analysis()` / `CompileGraph` は再帰でグラフを辿る（深さ数千ノードまで想定。それ以上の直列チェーンは想定していない）。
