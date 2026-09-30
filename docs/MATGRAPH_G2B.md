# マテリアルグラフ G2b: 生成 HLSL を既存のフォワード描画へ接続する

- 対象: Uno Engine（ブランチ `feat_develop`）
- 上位の設計: `docs/MATERIAL_GRAPH_DESIGN.md`（§3 コード生成・§4 データ・§7 G2b・§7.4 VG 整合）／G1 の仕様: `docs/MATGRAPH_FORMAT.md`／G2a: `docs/MATGRAPH_G2A.md`
- 性格: **グラフ材質（`.dxmat` の `graph` キー）を実際に描画へ載せる**。グラフを使わないシーンの絵は 1 ピクセルも変わらない。エディタ UI（ノード編集）は別担当（G0 × G1 の接続）で、本書の範囲外。
- 検証の結果（DXIL 同値・決定論スクショ・グラフ化の等価性・デバッグレイヤ）は §11。G2c / G3 / G3b への申し送りは §12。

---

## 1. 何を作ったか

| 領域 | 内容 |
|---|---|
| シェーダー | `shaders/forward/ForwardGraph.hlsl`（VS / PS / cbuffer / リソース宣言 / パラメータプール読み出しを固定で持つ枠。生成 HLSL は `UnoMatEval` だけを書く） |
| ランタイム | `src/renderer/GraphMaterialSystem.{h,cpp}`（インスタンス管理・パラメータプール・非同期 DXC + PSO ワーカー・旧版維持・ホットリロード・状態の保持） |
| パラメータプール | 構造化バッファ（`StructuredBuffer<float4>`）32,768 float4 × 3 フレーム。CPU の影が正、変更があったときだけフレームの UPLOAD バッファへ複写 |
| データ | `resource/MaterialAssetIO`（`graph` / `params` キー。旧ファイルはバイト一致）・`MaterialAssetManager`（`loadSerial`・代理ブロック・`SetGraphParam`） |
| コンパイル基盤 | `renderer/matgraph/ShaderCache.{h,cpp}`（DXIL のディスクキャッシュ・include ハッシュ・キー）・`ShaderRuntimeCompiler`（メモリ上のソース / 追加引数 / DXC 版） |
| グラフ化 | `renderer/matgraph/PbrTemplate.{h,cpp}`（標準 PBR テンプレート 12 通り）・`resource/MaterialGraphize.{h,cpp}`（.dxmat → インスタンス。`.bak` 退避） |
| 描画統合 | `ApplicationRender.cpp` の `drawEntity`（b2 の読み替え + PSO 切替）・`Update`（毎フレーム 1 回）・描画リストの並べ替え鍵（`DrawItem::graphHash`） |
| MCP | `core/mcp/ApplicationMcpMatGraph.cpp`（`material_graph_*` 9 本。長尾。`dx12_call` / `dx12_tool_search` で使う） |
| テスト | `MatGraphRuntimeTests`（純ロジック）・`GraphMaterialTests`（実 GPU）。既存の `DxmatRoundtripTests` は無変更 |
| ツール | `tools/engine_instance.ps1`（インスタンスへ HLSL ソースを `shaders-src/` として複写。グラフのコンパイルに要る） |

## 2. 全体の流れ

```
 .dxmat（"graph" キー）                     MaterialAssetManager::Entry（data.graphPath / graphParams / loadSerial）
   │   drawEntity → GraphMaterialSystem::Resolve(dxmat, entry, cmd, frame, lequal) → DrawParams{pso, recordBase, poolSrvIndex} | false
   ▼
 Instance（.dxmat 1 個）
   ├ .dxmg を読む → CompileGraph（CPU・数 ms）→ CompileResult（HLSL・スロット表）
   ├ プールにレコード（float4 × slotCount）を確保 → 値とテクスチャの SRV 添字を書く（PackParamRecord）
   └ 同じ HLSL ハッシュの Variant を共有（ワーカーが DXC → DXIL キャッシュ → PSO を作る）
        Build.active   = 描画に使っている版          （旧版。新版が Ready になるまで使い続ける）
        Build.pending  = 作っている最中の新版         （Ready になったら active へ。Failed なら捨ててエラーだけ保持）
```

- **`Resolve` が false のとき**（準備中・失敗・非対応 GPU・スキンド）は従来の代理材質で描く（下記 §5.3）。描画は止まらない。
- 値の編集（`.dxmat` の params・グラフの定数 / パラメータの既定値・`TexCoord` の tiling など）は **再コンパイルしない**（レコードを書き直すだけ）。HLSL が変わる編集（ノード / ワイヤ / リテラル / `Code` プロパティ / `inline`）だけワーカーで DXC + PSO を作る。
- 同じ HLSL（ハッシュ）の材質は Variant（PSO）を共有する。標準テンプレート由来の材質は構造ごとに 1 個（最大 12 個）の PSO に収まる。

## 3. `.dxmat` の `graph` キー

仕様の全文は `docs/MATGRAPH_FORMAT.md` §3。G2b で実装した内容:

- `graph` キーが無い .dxmat は **従来と 1 バイトも変わらない**（`SerializeMaterialAsset` の旧経路は無改造。`tests/matgraph_runtime_test.cpp` が「G2b 着手前の実コードが出した 3 種の出力」をリテラルで持ち、`Parse → Serialize` のバイト一致を機械判定。`DxmatRoundtripTests` は無変更で全通過）。
- `graph` あり（version 2）:
  ```json
  { "graph": "materials/_graphs/pbr_std_n1m1e0.dxmg", "name": "rock", "params": { "Albedo": "textures/rock/diff.png", "Roughness": 1.0, "EmissiveColor": [1.0, 0.6, 0.3] },
    "uvTiling": [2.0, 3.0], "version": 2 }
  ```
  `params` の値: 数値 = Scalar / 長さ 2〜4 の配列 = Vector（3 のとき w = 1）/ 文字列 = テクスチャ（assets 相対）。型が合わない値は読み捨て（ロードは止めない）。グラフに無い名前は**警告**（`W_PARAM_UNKNOWN`。黙って捨てない）。
  グラフ材質は `albedo` 等の 4 枚と `metallic` / `roughness` / `emissive*` を無視し、書き出しもしない。`metallic` / `roughness` は「影・深度・パストレの近似に使う代理値」で既定 0 / 0.5（従来の 1 / 1 ではない）。
  検証（「テクスチャを 1 枚も参照しないと無効」）は `graph` があれば免除。`uvTiling` は 1×1 でないときだけ書く。書き出しは正準形（キーは辞書順・float は最短表記。`0.8` は `0.8` のまま）でバイト一致に往復する。
- `MaterialAssetManager::Entry` に `loadSerial`（`LoadInto` のたびに +1）を追加。グラフ材質の `srvBlockStart` は**代理ブロック**（白 / 既定法線 / 既定 MR / 黒）。`SetGraphParam(rel, name, value, remove)` はメモリ上の params を書き換えて `loadSerial` を進める（ファイルは書かない）。

## 4. パラメータプール

### 4.1 レコードとオフセット規則

- プール = `StructuredBuffer<float4>` 32,768 要素（512 KB）。フレームごとの UPLOAD バッファ 3 本 + それぞれの SRV（シェーダー可視ヒープに 3 個）。
- **レコード** = グラフ 1 インスタンスの連続した float4（長さ = `CompileResult::slotCount`。0 のときも 1 個確保）。先頭の float4 番号が `recordBase`。
- 割り当て（`matgraph/ParamPool.h` の `ParamPoolAllocator`。決定論）: first-fit・解放時に隣接する空きをマージ。最初の割り当ては 0 から詰める。長さ 0 は拒否、容量超過は false（そのインスタンスは失敗扱い = 代理材質）。
- レコードの中身は G1 の `PackParamRecord`（`docs/MATGRAPH_FORMAT.md` §8.2）: スカラー / ベクトルは先頭成分から、テクスチャは `x = asuint(SRV 添字)`・`y = 1/幅`・`z = 1/高さ`。sRGB のスロットは格納時にリニア化。テクスチャは `ResourceManager::GetOrLoadTexture`（パスの表記は `AssetsDir + 相対パス` に固定）の永続 SRV 添字。読み込めないパスは既定テクスチャ（Color = 白 / Normal = 既定法線 / それ以外 = 白）＋警告 1 回。
- **同期**: CPU の影（`vector<float>`）が正。書き込みで版数を進め、`SyncPool(frameIndex)` が「そのフレームのバッファの版が古いときだけ」`[0, HighWater)` を複写する。`Update` の最後と `Resolve` の成功時に呼ぶ。フレーム多重化は既存の流儀（3 フレーム分の別バッファ + フレームフェンス）なので、GPU が読んでいるバッファへは書かない。
- 解放したレコード / 変種は 4 フレームの墓場を経てから返す。

### 4.2 b2 の読み替え（ルート DWORD +0）

`ForwardGraph.hlsl` の `cbuffer GraphMaterial : register(b2)`（9 DWORD。従来の `PBRMaterial` と同じ 36 バイト）:

| DWORD | 従来（Forward） | グラフ材質 |
|---:|---|---|
| 0 | `defaultMetallic` | `recordBase`（プール内のレコード先頭。float4 単位） |
| 1 | `defaultRoughness` | `poolSrvIndex`（このフレームのプールの SRV 添字） |
| 2 | `pbrFlags` | `graphFlags`（予約。0） |
| 3 | `packedTint` | 同じ（エンティティの色ティント RGB888 + 上位 8bit の opacity） |
| 4–7 | `uvScaleOffset` | 同じ（UV スクロール / 連番アニメ。`UnoMatInput.uv` に掛かる） |
| 8 | `packedEmissive` | 同じ（エンティティ側の発光上書き。テクスチャ無し。グラフの `Emissive` に加算） |

ルートシグネチャは 62/64 DWORD のまま（G2a の `HEAP_DIRECTLY_INDEXED` フラグ + 静的サンプラ s6–s8 を使う）。b0 の自由枠（`ObjectParameter`）は G3 のノード。

### 4.3 生成コードとの契約

`UnoMatPool`（`ForwardGraph.hlsl`）は `F1/F2/F3/F4/Tex(slot)` を持ち、`ResourceDescriptorHeap[poolSrv]` から `recordBase + slot` を引く。`UnoSample` マクロは `tex.Sample`。サンプラのマクロ: `UNO_SAMP_ANISO_WRAP = g_sampler(s0)`・`LINEAR_CLAMP = g_iblSampler(s2)`・`POINT_CLAMP = g_ssaoSampler(s4)`・`LINEAR_WRAP = s6`・`ANISO_CLAMP = s7`・`POINT_WRAP = s8`（G2a §3 のとおり）。`PSMain` は `UnoMatEval` → エンティティ tint を `baseColor` に掛ける → `emissive += UnoUnpackEntityEmissive(packedEmissive)` → 法線（`hasNormal` なら `UnoNormalFromTangentSpace`）→ `UnoShadeForward`。

## 5. PSO・描画順・他パスとの関係

### 5.1 PSO とバッチ

- PSO は Variant（HLSL ハッシュ）ごとに less / lequal（深度プリパス併用時）の 2 本。メイン RS（フラグ付き）+ `Mesh::GetInputLayout` + 両面描画 + `kSceneColorFormat` / D32。サブメッシュごとに `drawEntity` が `Resolve` して PSO を切り替える（`lastPso` の比較で同じ PSO の連続は張り替えない）。
- **バッチ（自動インスタンシング）**: グラフ材質は `HasMaterialAsset` なので従来どおりインスタンシング対象外（1 ドロー 1 体）。
- **描画リストの並べ替え**: `DrawItem::graphHash`（サブメッシュ 0 のグラフ材質の HLSL ハッシュ。未解決 / 非グラフは 0）を `sortKey` → カスタム `shaderPath` の次・`meshKey` の前に入れた。同じシェーダーの物が連続し、PSO 切替が最小になる。ハッシュは HLSL 本文の内容ハッシュ（起動をまたいで同じ値）なので決定論。グラフを使わないシーンは全部 0 で並びは従来どおり。
- エンティティの色ティント / UV スクロール / 連番アニメ / 発光上書きは効く。`set_pbr` の metallic / roughness 上書き・テクスチャ個別上書きは**グラフ材質では無視**される（設計書 §4.5）。

### 5.2 v1 の対応範囲（明記）

| 項目 | G2b の扱い |
|---|---|
| blendMode | **Opaque のみ**。Masked / Translucent / Additive は診断 `W_G2B_BLEND`（警告）を出して不透明として描く（`Opacity` / `OpacityMask` は無視）。G3b |
| shadingModel | DefaultLit のみ。他は `W_G2B_SHADING`（警告）で DefaultLit として描く |
| twoSided | 無視（エンジンのフォワードは常に両面描画） |
| スキンド | 対象外（従来どおりスキンド用シェーダー + 代理材質）。G7 |
| 頂点変位 / 特殊シェーディング | 未対応（予約ピンは接続不可） |

### 5.3 影・深度・速度・DXR・パストレ・VG でのフォールバック規則

グラフ材質は、これらのパスでは **G2b では従来の 4 枚組の代理材質（単色プロキシ）として扱う**。`MaterialAssetManager` が返す `Entry` の `srvBlockStart` は代理ブロック（白 / 既定法線 / 既定 MR / 黒）、`data.metallic / roughness` は代理値（既定 0 / 0.5）で、これらのパスの既存コード（`matAsset->srvBlockStart` / `data.metallic` など）が無改造でそのまま読める。

| パス | 扱い |
|---|---|
| 主ビューのフォワード | グラフの PSO（本書） |
| 影（CSM / スポット / ポイント）・深度プリパス・速度バッファ・Perception ID | 従来の不透明 / MASK 用 PSO。材質は代理（アルベド = 白のプロキシ）。**グラフで Opacity / OpacityMask を使っても影は板になる**（G3b で `PSMask` エントリを足すまで） |
| G-Buffer 用の roughness / metallic（SSR の保守的な入力） | 代理値（0.5 / 0）。グラフの Roughness / Metallic は反映されない（G6 で厳密化） |
| DXR（RT アルベド・RT 影 / AO・DDGI）・パストレーサー | BaseColor = 白のプロキシ（`GeometryInfo.baseColorSrvIndex` は代理ブロックの白）。グラフの色は見えない（G6a で BaseColor の定数 / UV 空間ベイクのプロキシ） |
| 仮想ジオメトリ（VG） | **対象外**（設計書 §7.4。従来のカスタムシェーダー / 材質上書きと同じ扱い）。G6b |
| 半透明のソート | 対象外（v1 は不透明のみ） |

### 5.4 グラフ材質が使えないとき（縮退）

`GraphMaterialSystem::Initialize` が false（バインドレス非対応の GPU / `DX12_DISABLE_MAIN_BINDLESS=1` / プール確保失敗）、または `Resolve` が false（初回のビルド中・失敗）のときは、上の代理材質（白・法線なし・粗さ 0.5・金属 0 の単色）で描く。黒 / 紫にはならない。

## 6. コンパイル基盤

### 6.1 非同期ワーカー・旧版維持

- ワーカー 1 本（`std::thread`）が専用の `ShaderRuntimeCompiler`（DXC インスタンス）を持つ。ジョブ = 生成 HLSL の全文 + ハッシュ。結果 = DXIL（VS / PS）+ PSO 2 本（less / lequal。D3D12 デバイスはフリースレッドなのでワーカーで作る）+ エラー + 時間。メインスレッドは `Update` で結果を取り込む。
- DXC: `ps_6_6` / `vs_6_6`・`-HV 2021`・`-I <プロジェクト assets/shaders> -I <エンジン shaders-src> -I <…/forward>`。VS は共通スタブ（`UnoMatEval` を空にした固定ソース）から 1 回だけ作る（グラフごとに作り直さない）。
- **旧版維持**: HLSL が変わる編集は新しい `Build` を `pending` に置く（レコードは新しい場所に別確保）。`pending` の Variant が Ready になるまで `active`（旧レコード + 旧 PSO）で描き続け、Ready になった瞬間に切り替える。失敗（DXC / PSO）したら `pending` を捨て、旧版のまま描き続ける。初回で失敗したら代理材質。値の編集は `active` / `pending` の**両方**のレコードを書き直す。
- エラーの保持: `InstanceStatus::errorLog`（DXC / PSO の全文）・`diagnostics`（グラフの診断 + DXC エラーを `MapDxcLog` で **nodeId + 行**へ逆引きしたもの + エンジン側の警告）。MCP `material_graph_compile` / `status` とエディタが読める。同じ HLSL の Variant が失敗済みなら再試行しない（編集して別ハッシュになるまで）。
- `Rebuild(dxmat, force)` / `RebuildAll` / `NotifyGraphFileChanged(graph)` で再ビルドを求める。`.dxmg` の更新時刻は 0.5 秒ごとにポーリング（エディタのみ）。

### 6.2 ディスクキャッシュ（DXIL）

- 置き場所: `%LOCALAPPDATA%\UnoEngine\shadercache\<エンジン版>\`（環境変数 `UNO_SHADERCACHE_DIR` で差し替え可）。ファイル名 `<キー 16 桁 hex>.<vs|ps>.dxil`。
- **キー** = FNV-1a 64（種別 / エンジン版 / DXC 版 / プロファイルと DXC 引数 / include 群のハッシュ / 生成 HLSL 全文）。include 群 = `ForwardGraph.hlsl` から `#include "…"` を再帰で辿った全ファイルの（include 名 + 内容）の合成（`ScanIncludes`。1 つでも変われば別キー）。VS のキーは共通スタブ + include 群だけ。
- **ファイル形式**: ヘッダ（マジック `UGSC` / 形式版 1 / キー / 本体の FNV / サイズ）+ 本体。読み込み時にキー・サイズ・FNV を検査し、壊れていたら捨てて作り直す（`corrupt` カウント）。書き込みは一時ファイル + rename（別プロセスのエンジンが同じ場所を使っても壊れない）。容量の上限管理はしない。
- カウンタ（`Counters`）: `dxcCompiles`（DXC を実際に呼んだ回数）・`cacheHits / cacheMisses / cacheStores`。テストが「値の編集で DXC が呼ばれない」「別インスタンスがキャッシュから DXC 0 回で立ち上がる」を見張る。

### 6.3 ゲームモード

`GameRuntime`（pak 起動）は `GraphMaterialSystem` を `allowCompile = false` で作る（DXC を使わない）。

- **焼き込み**: `Application::BuildGame` が `GraphMaterialSystem::BakeForBuild` を呼ぶ。assets 内の全 `.dxmat` を走査して graph キーのある材質のグラフをコンパイルし、HLSL ハッシュごとに 1 回だけ DXC で DXIL にして pak へ入れる（`shaders/graph/<HLSL ハッシュ 16 桁>_PS.cso` と共通の `shaders/graph/vs.cso`。ディスクキャッシュを使うので 2 回目以降は速い）。1 つでもコンパイルできなければ**ビルドを中止**する（カスタムシェーダーと同じ方針。壊れたグラフを出荷しない）。
- **実行時**: ワーカーは DXC の代わりに `vfs::ReadAsset("shaders/graph/…")` で DXIL を読んで PSO を作る（DXC 呼び出し 0 回）。pak に無い HLSL（ビルド後にグラフを差し替えた等）は失敗して「配布ビルドにこのグラフのシェーダーが入っていません」を保持し、旧版があればそのまま、無ければ代理材質で描く。
- 検証: `GraphMaterialTests` の「game mode (pak)」（`BakeForBuild` → `PakWriter` → `MountPak` → `allowCompile=false` のシステムが DXC 0 回で PSO を作る / 焼かれていない HLSL は失敗しても描画が止まらない）。**`GameRuntime.exe` の実機起動（Game.exe の前面起動になる）は行っていない**（§13）。

## 7. グラフ化（従来の PBR 材質 → 等価グラフ）

- **手動のみ**（MCP `material_graph_graphize`）。エンジンが自動で変換することは絶対にない。
- 標準テンプレート（`matgraph/PbrTemplate`。構造 = 法線マップ有無 × MR 有無 × 発光 {無し / 色だけ / テクスチャ + 色} の 12 通り。ファイル名 `materials/_graphs/pbr_std_n{0|1}m{0|1}e{0|1|2}.dxmg`）:

  | 出力 | 式（従来の Forward.hlsl と同じ） |
  |---|---|
  | BaseColor | `Albedo(sRGB).rgb * VertexColor.rgb`（エンティティ tint は `ForwardGraph.hlsl` 側で掛かる） |
  | Normal | `NormalMap(Normal, Strength=1)`（xy*2−1・z 再構成。0.35 ガードは `UnoNormalFromTangentSpace`）。無しなら未接続 |
  | Roughness / Metallic | `MetalRoughness.G * Roughness` / `MetalRoughness.B * Metallic`。MR 無しは係数だけ |
  | Emissive | `Emissive(sRGB).rgb * EmissiveColor * EmissiveIntensity`（テクスチャ無しなら 色 × 強度）。発光無しは未接続 |
  | AO | 従来どおり使わない（MR の R は未使用） |

- ★`uvTiling` は `TexCoord` に掛けない（従来の描画は頂点バッファへ焼き込み済みの UV をそのまま使い、描画時に掛けていない。二重に掛けると絵が変わる）。`.dxmat` の `uvTiling` は「これから割り当てるときの初期値」として引き継ぐだけ。設計書 §4.4 の「TexCoord × uvTiling」からの意図的な変更。
- 変換: テンプレートを書く（無い / 内容が違うときだけ）→ 元の .dxmat を `<name>.dxmat.bak` へ退避（**既にあれば上書きしない** = 最初の原本を守る）→ .dxmat を graph + params のインスタンスへ書き換え。構造が同じ材質は同じテンプレートを共有する（= 同じ HLSL = PSO 共有）。dryRun は何も書かず変換後の中身だけ返す。書く前に journal（`journal_restore` で戻せる）。
- 等価性の検証は §11.3（決定論スクショの画素差）と `MatGraphRuntimeTests`（旧経路の式との CPU 評価一致）。

## 8. MCP（`material_graph_*`。長尾）

`McpMeta` 直渡し（`describe_mcp_manifest` に載り、再起動なしで `dx12_call {name:"material_graph_get"}`）。別名 `dx12_material_graph_*`。core 面（40 本）には入れない。

| method | effect | 内容 |
|---|---|---|
| `material_graph_nodes` | read | ノード型の一覧（`query` / `detail:"full"` でピン・プロパティ）。`edit` の `add` で使う型名を引く |
| `material_graph_get` | read | グラフ（`.dxmg`。`.dxmat` を渡すと親グラフ + params）の取得。ノード・接続・設定・パラメータ一覧・検証（診断 / スロット / 統計）。`hlsl:true` で生成 HLSL、`text:true` で正準形の全文 |
| `material_graph_validate` | read | 検証 + コンパイル（何も書かず、エンジンの状態も変えない）。診断（nodeId / pin / code / 日本語）・スロット表・統計 |
| `material_graph_edit` | write_file / **dryRun** / journal | `ops` 配列で差分編集して `.dxmg` に保存。`add {type,id?,pos?,props?,in?}` / `remove {id}` / `connect {from:"id.pin",to:"id.pin"}` / `disconnect {to}` / `set {id, prop|pin, value}` / `move {id,pos}` / `settings {values}`。複製に全部当てて検証し、**1 個でも失敗したら 1 バイトも書かない**（`E_INVALID_OP` + `ops[i]`）。返り値に `structureChanged` / `recompileRequired`（HLSL のハッシュが変わるか）と検証結果。`create:true` で空のグラフから。`dryRun` は書かず同じ結果 |
| `material_graph_compile` | runtime | 再ビルドを要求（`force:true` でキャッシュ無し）して、インスタンスの状況を返す（描画に使っている / 作っている最中 / 失敗 + エラー + nodeId 逆引き・DXC / PSO の時間・キャッシュヒット）。**非ブロッキング**（数フレーム進めてから `status` で結果を読む）。未描画の材質は `known:false` |
| `material_graph_status` | read | インスタンスごとの状況とカウンタ（DXC を呼んだ回数・キャッシュヒット・PSO 数・プール使用量・値更新回数・代理材質で描いた回数） |
| `material_graph_graphize` | write_file / **dryRun** / journal | 従来の `.dxmat` → 標準テンプレートのインスタンス（§7）。dryRun は変換後の中身だけ |
| `material_graph_set_param` | write_file / **dryRun** / journal | `.dxmat` の params を書き換える。**再コンパイルしない**。`value:null` で上書きを消す。`save:false` でメモリ上だけ（保存されない）。グラフに無い名前は警告 |
| `material_graph_apply` | write_scene | エンティティのサブメッシュへ `.dxmat`（グラフ材質でも従来材質でも）を割り当てる。`path:""` で解除。Undo できる |

## 9. 罠と注意（実装で踏んだもの）

- 生成 HLSL のヘッダ行に `source=` を入れない（`CompileOptions::sourceName` を空にする）。入れると同じ構造でも HLSL 全文が違い、Variant / キャッシュが共有されない。
- ワーカーは `EngineShaderSrcDir()`（`PathResolver::ShaderSourceDirW()` → 環境変数 `UNO_SHADER_SRC_DIR`）でエンジンの HLSL ソースを探す。**エージェント用インスタンス（exe のコピー）は `shaders-src/` を持たない**ので `tools/engine_instance.ps1` が複写するようにした（これが無いとグラフは「HLSL ソースが見つかりません」で失敗する）。
- ワーカーの PSO 作成は D3D12 デバイスをスレッドから叩く。`PipelineStateBuilder::Build` は `ThrowIfFailed` で例外を投げるので必ず try / catch（メインスレッドのように落とさない）。
- `ShaderManager` は `assets/shaders/` を再帰走査して `.hlsl` を全部 `vs_6_0` / `ps_6_0` のカスタムシェーダーとして DXC に掛ける。グラフの生成物は別の経路（メモリ上のソース）で扱うが、念のため `_graph/` 配下は走査から外した。
- 旧版維持の間は **レコードを 2 個持つ**（旧 HLSL とスロット配置が違うため）。値の編集は両方に書く。新旧で `recordBase` が違うので、b2 に載せる値は常に「いま描いている版のもの」。
- Windows 上の bash heredoc はバックスラッシュを 1 段食う（`'\0'` がリテラルの NUL になり、git がバイナリ扱いにした）。ファイル編集は Edit / Write ツールで行う。
- デバッグ用に `GraphMaterialSystem::DebugSetWorkerDelayMs(ms)`（ワーカーの処理を遅らせて「コンパイル中の旧版維持」を再現。環境変数 `UNO_MATGRAPH_WORKER_DELAY_MS` でも指定できる）と `DebugReadRecord`（レコードの中身）がある。環境変数 `DX12_DISABLE_GRAPH_MATERIALS=1` でランタイムごと作らない（A/B 検証・切り分け用）。
- `MaterialAssetManager::LoadInto` は `.dxmat` を再ロードするたびに材質ブロックの SRV を作り直す。グラフ材質の代理ブロックは中身が変わらないので、一度埋めたら作り直さない（`srvIsProxy`）。作り直すと、描画中のフレームが使っている静的ディスクリプタを上書きして GPU ベース検証が `id=1001` を出す（`.dxmat` の params を書き換えるたびにホットリロードが走るため、MCP の `set_param` で毎回出ていた）。
- BMP は行を 4 バイト境界へ詰める。テスト用に 2x2 の 24bit BMP を手で書くと壊れた BMP になり、WIC が読めず既定テクスチャへ黙って縮退する（ログにだけ出る）。

## 10. テスト

| テスト | 種類 | 内容 |
|---|---|---|
| `MatGraphRuntimeTests`（`tests/matgraph_runtime_test.cpp`） | ctest・純ロジック | ① `.dxmat` の旧ファイル 3 種（G2b 着手前の実コードが出した出力）が Parse → Serialize でバイト一致 / 検証の不変（テクスチャ無しは無効・params だけでは graph 扱いにならない）② グラフ材質の往復（バイト一致・型が合わない params は読み捨て・3 成分 Vector は w=1）③ `ParamPoolAllocator` のオフセット規則（詰め方・穴の再利用・隣接マージ・二重解放 / 範囲外の拒否・決定論）④ DXIL ディスクキャッシュ（保存 / 読み込み / 別インスタンスから読める / 壊れの検出と復旧 / キーの各要素）と include ハッシュ（辿ったファイルの変更でだけ変わる・コメントアウトは辿らない）⑤ グラフ化: 12 構造すべてがコンパイルでき（警告も 0）、旧経路の式（Forward.hlsl）と CPU 評価が一致、構造が同じなら値が違っても同じ HLSL、12 通りのハッシュは全部別、ファイル書き出し（`.bak` の原本一致・テンプレート共有・dryRun は無書き込み・二重変換の拒否）。**541 チェック** |
| `GraphMaterialTests`（`tests/graph_material_test.cpp`） | ctest・実 GPU（ラベル gpu） | RTX 5060 で `GraphMaterialSystem` を動かす: ① 初回ビルドは非同期（Resolve が false → 完了後 true・DXC は VS + PS の 2 回）② パラメータプール（レコードの値・テクスチャは SRV 添字 + 1/幅 + 1/高さ・材質ごとにオフセットが違い隙間なく詰まる・同じ HLSL は PSO 共有）③ 値の編集（`SetGraphParam`・グラフの定数の編集）は DXC / PSO 作成が 0 回・レコードだけ更新 ④ 構造の編集は**旧 PSO のまま描き続けて**（32〜43 回）から新 PSO へ切り替わる ⑤ 壊れた HLSL（Custom ノード）で旧 PSO 維持・エラー全文と Custom ノード `cu` の行への逆引きを保持・直すと復帰（同じ HLSL の変種は DXC 不要）⑥ プレビュー球の足場（`ResolveData` / `SetInstanceGraphText`）⑦ ディスクキャッシュ経由の別インスタンスは DXC 0 回 ⑧ 配布ビルド（`BakeForBuild` → `PakWriter` → `MountPak` → `allowCompile=false` で DXC 0 回のまま PSO・焼かれていない HLSL は失敗しても描画が止まらない）。デバッグレイヤ + GPU ベース検証（`DX12_D3D_DEBUG=1 DX12_D3D_GBV=1`）でメッセージ 0。**143 チェック** |
| `MatGraphDxcTests`（G1。実 GPU の段を追加） | ctest・ラベル dxc | G1 の数値一致（ゴールデン 8 + 登録表の全数値ノード 41 個の自動配線 + ランダム 40 本 × 96 点 = 89 グラフ・8,544 点）を **WARP → 実 GPU（RTX 5060）** の順に流す。両方 0 不一致（§11.4）。実 GPU が無い環境はその段だけスキップ |
| `DxmatRoundtripTests` / `ShaderIncludeDepsTests` / `ForwardShadeTests` / `MainRsBindlessTests` | ctest | 既存。無変更で通過 |

## 11. 検証結果（RTX 5060・Windows 11・ヘッドレス）

### 11.1 既存（グラフ無し）の絵が変わらない

- **DXIL 同値**（`tools/shader_dxil_equiv.ps1`。ForwardGraph.hlsl 追加前後）: 12 / 12 本が「同一」（ハッシュも一致）。追加したのは新規ファイル 1 本だけで、既存のシェーダー・include は 1 バイトも触っていない。
- **決定論スクショ**（G2a の 9 シーン。`screenshot_final {deterministic:true}`）: 同じ exe で「グラフ材質のランタイムを作らない（`DX12_DISABLE_GRAPH_MATERIALS=1`）」と「既定」を比べて **9 / 9 が全画素ビット一致**（描画リストの並べ替え鍵・`Update`・`drawEntity` の分岐がグラフ無しのシーンで何も変えない）。G2a 完了時の exe（`snap_new`）とは他エージェントのエディタ変更でビューポートが違う（1045x588 対 977x550）ため直接は比べられない（同じ手順で G2a の SHA `fb77ccbae8f00f74` は再現できた）。

### 11.2 グラフ化の等価性（従来 `.dxmat` → グラフ化。同じ球・同じ光・同じカメラ）

6 材質（Poly Haven 系の岩 / 草 / 土 / 雪 + 発光テクスチャ付きの岩 + 発光色だけの土。テンプレート 5 構造を踏む）を球に貼り、従来のまま撮った絵 A と、`material_graph_graphize` した絵 B を画素差で比べた。

| 指標 | 結果 |
|---|---|
| 決定論（同じ材質を撮り直した A と A2） | 差 0（全画素一致） |
| 全画面 537,350 画素の平均絶対差 | **0.0013 / 255**（最大 1 / 255・差が 1 を超える画素 0・PSNR 76.9 dB） |
| 球の帯（321,433 画素）の平均絶対差 | 0.0022 / 255（PSNR 74.6 dB） |
| 材質別 | 岩 / 草 / 土 / 雪（発光なし）= **ビット一致**（差 0）。発光あり 2 種 = 最大 1 / 255（平均 0.010 / 0.004。従来の b2 の 8bit 二乗量子化と、グラフの float の差） |

合否基準（設計書: 平均 ≤ 1/255・差が 2/255 を超える画素 ≤ 0.5%）に対し、**差が 2/255 を超える画素は 0 %**。本書で決めた許容: 平均絶対差 ≤ 0.5/255・> 2/255 の画素 0 %・PSNR ≥ 60 dB（実測は全部これより厳しい）。グラフ側のビルドは 5 変種（草と岩は同じ HLSL で PSO 共有）・パラメータプール使用 42 float4（6 材質）・DXC 0（ディスクキャッシュ経由の 2 回目）。

### 11.3 実機の受け入れ（`screenshot_final`・MCP）

- **値の編集は再コンパイルなしで色が変わる**: `material_graph_set_param`（発光色 / ラフネス / アルベドテクスチャ）の前後で DXC 呼び出し 0 → 0・PSO 作成 10 → 10・`generation` 不変。球の領域の差は変えた球だけ（発光色を変えた球 平均 19.5 / 255、テクスチャを差し替えた球 7.7、ラフネスの球 0.49、触っていない球は 0）。グラフの定数の既定値編集・ノードの移動も DXC 0 回。
- **HLSL を変えると旧版のまま新版へ切り替わる**（ワーカーを 3 秒遅らせて撮影）: 編集 0.3 秒後（コンパイル中）に撮った絵は旧版と**全画素一致**（差 0）→ 3.5 秒後に新版へ切り替わり、`BaseColor` を 0.3 倍にした球だけが暗くなる（平均差 14.2 / 255）。他の球への影響 0。ワーカーを遅らせない通常の所要は DXC 164〜268 ms + PSO 200〜340 ms（1 グラフ）。
- **壊れた HLSL**（Custom ノードに未定義関数）: `failed:true` で旧版を維持（`hasActive:true`・描画の絵は前と全画素一致）、エラーは `node:cu:1:12: error: use of undeclared identifier 'no_such_function'` → `E_DXC` の診断が `nodeId: "cu"`・行 1 へ逆引きされて残る。直すと復帰（`failed:false`）。
- **初回ビルド中は代理材質**（`hasActive:false`・白い単色）→ 完了後にテクスチャ付きの本物へ。
- **デバッグレイヤ**（`DX12_D3D_DEBUG=1`。および GPU ベース検証 `DX12_D3D_GBV=1`）: 上の一連（グラフ化・割り当て・パラメータ変更・構造編集・壊れた HLSL・復帰）で D3D12 メッセージは起動時の既存 16 件（`id=1328` のバッファ InitialState の警告。G2a のレポートと同じ既存）だけで、**グラフ関連は 0 件**（エラー行はテスト用に意図的に壊した HLSL のログ 1 件だけ）。最初の GBV 実行で見つかった `id=1001` は代理ブロックの SRV を作り直さないようにして解消した（§9）。

### 11.4 G1 の数値ノードの実 GPU 一致（WARP のみだった G1 への追加）

89 グラフ（ゴールデン 8 + 全数値ノード 41 + ランダム 40）× 96 点 = 8,544 点。**WARP: 不一致 0・最大相対誤差 1.6e-5 / RTX 5060: 不一致 0・最大相対誤差 1.3e-5**（基準 1e-4）。非有限値の除外 0.24%（両方同じ）。CPU 側の入力をずらすと 383 件の不一致として検出される（検出力の確認）。

### 11.5 ctest

**本体の作業ツリー**（他エージェントの作業が落ち着いた最後の時点）で全 102 件を流して **100 件通過**。失敗の 2 件は `VgResolveGpuTests`（VG の作業中）と `McpParamSpecTests`（foliage の MCP に hint 無しの `McpError` が 2 件。G2b のコードは全部 hint つき）で、G2b と無関係。本体ビルド後の実機の等価性テストも私設ツリーと同じ数値（全画面 平均絶対差 0.0013 / 255・最大 1）。以下は私設の作業ツリー（§13）で全 100 件を流した記録で、G2b 関連は全通過（`MatGraphRuntimeTests` / `GraphMaterialTests` / `MatGraphTests` / `MatGraphDxcTests` / `DxmatRoundtripTests` / `ShaderIncludeDepsTests` / `ForwardShadeTests` / `MainRsBindlessTests` / `MatGraphEditorTests` ほか）。失敗した 12 件は他エージェントの作業中の領域で、G2b と無関係: `McpEditorCommandTests` / `McpManifestTests`（`editor_*` の登録・`screenshot_final` の説明の長さ）/ `McpParamSpecTests`（hint 無しの `McpError` 1 件）/ `ComponentMetaTests`（PointLight のフィールド）/ `AssetBrowserLogicTests`（two.png）/ `ParityHarnessPyTests` / `VgeoCookTests` / `VgCullGpuTests` `VgRasterGpuTests` `VgResolveGpuTests` `PathTracerGpuTests`（VG / パストレの作業中。終了コード 0xc0000409）/ `ModelScaleTests`（私設コピーが `*.obj` を複写していないだけ）。

## 12. 申し送り

### G2c（プレビュー球 + 非同期の本実装）へ

- **足場は入れてある**: `GraphMaterialSystem::ResolveData(instanceKey, MaterialAssetData, dataSerial, …)`（`.dxmat` の Entry を介さない）と `SetInstanceGraphText(instanceKey, dxmgText)`（保存前のグラフ本文でビルド）。プレビューは「一時キー + graphPath 不在 + 本文を押し込む」で、編集のたびに本文を渡せば HLSL が変わるときだけ非同期ビルド・変わらなければ値の更新だけになる。オフスクリーンのプレビュー描画（専用の軽量 PS / 球メッシュ / 固定ライト）と、エディタの状態表示（`ListInstances` / `GetInstanceStatus` の `errorLog` / `diagnostics`（nodeId 逆引き済み）・`compiling` / `failed`）は G2c。
- 非同期は最小の形で本番に入っている（専用ワーカー・旧版維持・キャッシュ）。G2c の実測ゲートに要る計測点: `InstanceStatus::{codegenMs, dxcMs, psoMs, cacheHit}`・`Counters`。**250 ms のデバウンスは未実装**（編集の通知が来るたびにビルドを積む。同じ HLSL の連続はキャッシュ / 変種の共有で安いが、連続した構造編集ではワーカーのキューが詰まる → G2c で「最新だけ」に間引く）。`-Od` の段階コンパイル・ライブラリ分割（設計書 §3.8 #6 / #7）のスパイクも未着手。
- ワーカーは 1 本（ジョブは FIFO）。DXC は 1 ジョブ 150〜350 ms・PSO 200〜340 ms だったので p95 ≤ 3 s は今のグラフでは余裕だが、Custom / Noise の多い大きなグラフでは未計測。

### G3（ノード 60 種 + パレット + MCP）へ

- ノードを足すのは `BuiltinNodes.cpp` に登録するだけ（G1 の手順）。**`ObjectParameter`（#8）の受け口は入れた**: `ForwardGraph.hlsl` の b0 に自由枠 8 float（`effectValue` / `shaderParamsB` / `shaderParams`。UnoCustom.hlsli と同じバイトレイアウト）を宣言済み。`drawEntity` は従来どおり `renderer.CustomParamBase()` を 40 DWORD で送るので、Lua の `scene:setMeshParams` / Trigger / tween が書く値がそのまま届く。ノードの HLSL はこの 3 つの変数を直接読めばよい（生成コードに cbuffer を書かせない規則は不変。`UnoMatInput` に足す場合は `UnoSurface.hlsli` と G1 の参照実装を同時に直すこと）。
- 画面微分を使うノード（`NormalFromHeight` など）はグラフ材質が VG 対象外の間は問題ない（VG resolve に載せるのは G6b）。
- MCP: `material_graph_*` は最小の 9 本。設計書の `dx12_material_graph_create`（テンプレートから作る）と `_preview` は未実装。`edit` の ops 語彙は `get` の出力と同じ（`nodes` / `in` / `props` / `pos`）。

### G3b（マスク / 半透明）へ

- `blendMode` が Opaque 以外のグラフは警告（`W_G2B_BLEND`）を出して不透明で描く。半透明は `drawEntity` の `blendPso` 相当（深度書き込み OFF + アルファブレンド）を Variant に足し、`BuildDrawList` の `alphaClass` / `sortKey` にグラフ材質を連動させる（`PeekShaderHash` と同じく、ロードせずに blendMode を引ける `PeekSettings` が要る）。
- 影・深度プリパス・速度は今は代理材質（§5.3）。`PSMask` / `PSDepth` エントリを生成物に足すとき、既存の MASK パスは b2 の先頭 2 DWORD（metallic / roughness）を読む点に注意（グラフ用の b2 では `recordBase` / `poolSrvIndex` になっている。専用エントリはグラフ材質の b2 レイアウトで読む）。
- 深度プリパス併用（LESS_EQUAL）用の `lequal` PSO は用意済み。スキンドは対象外（G7）。

### その他

- `tools/mcp-server/manifest.snapshot.json`（エンジン未接続時の代役）は更新していない（他エージェントの新 method を含めて再生成する作業なので、まとめて担当が行う）。エンジンに繋がっていれば `describe_mcp_manifest` から自動で取り込まれる。
- グラフ化の `.bak` は `.gitignore` 推奨（`*.dxmat.bak`）。テンプレート `.dxmg`（`materials/_graphs/`）は git に入れてよい（内容が決定論）。

## 13. 未確認・制約

- **`GameRuntime.exe`（pak 起動）の実機起動は未確認**。焼き込みと pak からの PSO 生成は単体テストで確認したが、Game.exe を起動する検証は前面化（ユーザー操作を奪う）を伴うため行っていない。
- 全 UI 自動テスト（`--ui-tests-run-all`）は未実行（既知の `build_game` テストが Game.exe を前面起動するため。本作業はエディタ UI に触れていない）。
- 本体の作業ツリー（`C:\Users\ryuto\Documents\dx12`）の全ビルド + ctest は、他エージェントの作業途中（foliage / EditorUi / VG 等）で何度もビルドが通らなかったため、**変更前後の検証は私設の作業ツリー**（本体の複写 + 未完成箇所への最小パッチ 2 件: `ApplicationMcpEditorUi.cpp` のキャプチャ指定と `InspectorPanel.cpp` の前方宣言。どちらも本体には入れていない）で行った。本体でのビルドは最後に試して通った（他エージェントの作業が落ち着いた後。§11.5）。
- 性能: グラフ材質の描画コスト（フレーム時間・GPU 時間）は未計測（要求外）。パラメータプールの読み出しは UPLOAD ヒープの読み取り（PCIe 越え）。大量のグラフ材質でフレームが重くなるなら、プールを DEFAULT ヒープ + 差分コピーへ変える（`SyncPool` 1 か所の変更）。
- RTX 5060 以外の GPU（非対応 GPU は `IsAvailable() = false` で代理材質。`DX12_DISABLE_MAIN_BINDLESS=1` の実機テストは G2a 済み・本作業では未実施）。
- エンジンの HLSL ヘッダ（`ForwardShade.hlsli` など）を編集しても、動作中のグラフ材質は自動では作り直されない（ディスクキャッシュのキーは include の内容を含むので次回のビルドでは新しい内容になる）。開発中は `material_graph_compile {force:true}` で作り直す。
- 私設ツリーの CMake 再構成は `vcpkg_installed` を本体と共有した。

