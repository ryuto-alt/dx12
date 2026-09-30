# 仮想ジオメトリ（Nanite 風）設計書

- 対象: 自作 DX12 エンジン（`C:\Users\ryuto\Documents\dx12` / ブランチ `feat_develop`）
- 作成: 2026-09-29 / 状態: **設計のみ（エンジンのソースは 1 行も変更していない）**
- 基準機: RTX 5060（8 GB）/ 1080p / 60 fps / 可視 5000 万 tri 級シーン
- 表記: **[事実]** = 実コードを読んで確認（`ファイル:行`付き）/ **[設計]** = 本書の決定 / **[推定]** = 未計測の見積もり（P2〜P4 の実測で差し替える）

---

## 0. 要約（1 画面）

**やること**: 静的メッシュ専用の「クラスタ + 階層 LOD（DAG）+ GPU カリング + 可視性バッファ」を、既存のフォワード+ パイプラインへ**既定 OFF で絵が 1 ビットも変わらない形**で相乗りさせる。

**統合の鍵（最小侵襲にする 5 つの決定）**

1. **プロキシ方式**: `.vgeo` を読むと、通常の `Mesh`（数万〜25 万 tri のプロキシ）と `Material` を持つ通常の `MeshRenderer` エンティティになる。VG が ON のときだけ描画リスト側で「VG 本体が描く」印（`DrawItem::vg`）を付け、深度プリパス / フォワードから外す。**TLAS・影・ピッキング・物理・ナビ・知覚 ID は無改造でプロキシを見る**。VG OFF / 非対応 GPU ではプロキシがそのまま描かれる（縮退が自動で成立）。
2. **ルートシグネチャは 0 DWORD 消費**（現状 62/64、残り 2）。VG のカリング / ラスタは専用ルートシグネチャ（DDGI / RtScreen と同じ先例）、色の resolve だけは**メインと同一パラメータ + `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` を足した別オブジェクト**で走らせ、バインドレスでクラスタ・材質を引く。
3. **可視性バッファ = R32_UINT（可視クラスタ番号 25bit + 三角形番号 7bit）+ 既存 D32 深度**。64bit は使わない（HW ラスタは 64bit アトミックを持たない。SW ラスタ導入時だけ別バッファ + 合流パス）。
4. **フォワード PS の MRT 化はしない**（確定済み方針を守る）。速度 / G-Buffer は「VG 専用の全画面パス」が可視性バッファから書く。色は「VG resolve（全画面 PS、既存ライティングを全部そのまま使う）」→ 既存フォワード（非 VG のみ、LESS_EQUAL）の順。
5. **PSO は新規 `MeshPipelineStateBuilder`（PSO ストリーム）を別クラスで追加**。既存 `PipelineStateBuilder` は 1 行も触らない。

**cook（オフライン）**: meshoptimizer 1.0（vcpkg 導入済み）で「meshlet 化 → `partitionClusters` で 4 個ずつグループ化 → グループ境界を**位置溶接ベースで**ロックして 1/2 に簡略化 → 再クラスタ」を根まで繰り返し DAG を作る。誤差は**絶対値の累積**（親 = 自分の簡略化誤差 + 子の最大誤差）、LOD 球は子を内包（→画面誤差が親子で単調）。座標は**アセット全体で共通の 24bit 格子**へ量子化（クラック防止の要）。形式は 128 KiB ページ単位の `.vgeo`（§3、バイトオフセット付き）。

**GPU**: インスタンスカリング → BVH（4 分木、葉 = グループ）を multi-dispatch でウェーブフロント走査 → クラスタカリング（錐台 / 法線コーン / **HZB 二段**: 前フレーム HZB → 今フレームの中間 HZB で再判定）→ `ExecuteIndirect(DispatchMesh)` でメッシュシェーダ HW ラスタ（1 グループ = 1 クラスタ、AS 不使用）。微小三角形用 SW ラスタ（compute + 64bit アトミック）は**インターフェース（HW/SW の 2 リスト）だけ最初から用意し、実装は P3b（条件付き）**。

**段階**（詳細 §4）: P0 仕様確定 3 日 → P1 cooker 10 日 ∥ P2 ランタイム + GPU カリング 10 日 → P3 メッシュシェーダ + 可視性バッファ 9 日（P3b SW ラスタ 7 日・条件付き）→ **P4 マテリアル resolve + 既存統合 15 日（ここで「5000 万 tri / 60 fps」を全ページ常駐モードで機械判定）** → P5 ストリーミング 12 日。P6（UE cook, C#）10 日 は P0 だけに依存して並列。P7 VSM 30 日 / P8 GI 18 日 / P8 と並列で P9 TSR・DoF・Bloom 14 日。合計 約 144 実働日、クリティカルパス（P0→P2→P3→P4）は約 37 日。

**5060 の見積もり（[推定]）**: 5000 万 tri のシーンは全 DAG で約 12.5 B/tri × 2（DAG は各段で三角形が半分）≈ **1.25 GB**。8 GB VRAM に**全常駐で載る**ので、最初の合格判定はストリーミング無しで出せる（P5 は 1 億 tri 超 / UE 級の複数 GB のため）。1 フレームの VG 予算は 8〜10 ms（カリング ~1 / HW ラスタ 2〜4 / G-Buffer 0.5 / 色 resolve 3〜4）。

**最大のリスク**: ① 5060 実機でのメッシュシェーダ小三角形スループット（P3 の実測ゲートで SW ラスタ主体へ切替判断）、② UE の Nanite cook 済みアセットは「フォールバック低ポリ + Nanite ストリーミングページ」しか無く、ページのデコードが要る（P6 冒頭 3 日のスパイクで可否判定、駄目なら非 Nanite メッシュ / UE エディタ側エクスポートへ縮退）。

**ユーザーに決めてほしい点**は §7（5 件、推奨案付き）。

**覆した過去の決定**: 「meshlet / メッシュシェーダは却下」（`dx12-realism-roadmap`）。却下理由は (a) `PipelineStateBuilder` が固定 desc で PSO ストリーム基盤が無い、(b) 既に draw 14 本で効果が薄い、の 2 つだった。本件は目的が変わった（数千万〜億 tri）ので (b) は当たらず、(a) は別クラスの追加で解決する（既存を触らない）。

---

## 1. 現行パイプラインの事実（実コード確認）

設計に効く事実だけを抜き出す。「含意」列が本書の設計への入力。

### 1.1 メッシュの読み込みと表現

| # | 事実（[事実]） | 含意 |
|---|---|---|
| F1 | モデルは **assimp** で読む（`src/resource/ModelLoader.cpp:15` include、`ReadFile` は `:662-678`。フラグ = Triangulate / SortByPType / GenNormals / CalcTangentSpace / JoinIdenticalVertices / LimitBoneWeights / FlipUVs / GlobalScale）。1 つの `aiMesh` → 1 つの `Mesh` + 1 つの `Material`（`ModelData` の `meshes` / `materials` は並列配列、`ModelLoader.h:22-32`、`mesh->Initialize` は `ModelLoader.cpp:1060`） | `.vgeo` も「サブメッシュ = マテリアル 1 個」の並列配列でプロキシを返せば、下流（TLAS / ピッキング / 物理）は無改造 |
| F2 | 頂点は **96 B インターリーブ**（`Mesh.h:15-24`: position 0 / normal 12 / color 24 / uv 40 / tangent 48 / boneIndices 64 / boneWeights 80。入力レイアウト `Mesh.cpp:57-65`）、インデックスは u32 | 5000 万 tri ≈ 2500 万頂点 × 96 B = 2.4 GB（VB のみ）。**従来形式では VRAM も CPU も持たない** |
| F3 | `Mesh` は CPU 側に `m_verticesCache`（96 B/頂点）・`m_indicesCache`・`m_positions` を**全コピー保持**（`Mesh.h:204-206`。ピッキング / 物理 / 凸包用）。DEFAULT ヒープ VB/IB（`Mesh.h:53`）、頂点は `meshopt_generateVertexRemap` + `optimizeVertexCache` + `optimizeVertexFetch` で並べ替え（`Mesh.cpp:67-104`） | 1 億 tri を `Mesh` に入れると CPU コピーだけで 10 GB 超。**大規模メッシュは `Mesh` に入れない**（プロキシだけが `Mesh`） |
| F4 | 既存の自動 LOD は `Mesh::GenerateLods`（`Mesh.cpp:138-195`）。インデックス 3000 未満は LOD なし（`:145`）、LOD0 + 簡略 4 段（30/10/3/1%）、`meshopt_simplify`（Permissive | Prune、`:154`）、**頂点バッファ共有**。LOD 選択は**エンティティ単位**（`DrawItem::lod`、`DrawItem.h:42`） | クラスタも DAG も無い。VG は既存 LOD と独立に持つ（プロキシ側は従来どおり自動 LOD が効く） |
| F5 | マテリアルは `Material`（`Material.h`）。albedo / normal / metalRoughness / emissive の **4 連続 SRV ブロック**（`kMaterialSrvBlockSize = 4`、`ModelLoader.cpp:1287` の `AllocateBlock`）。レジスタは t0,t1,t2,t24 に飛ぶがヒープ上は連続 | バインドレスで `ResourceDescriptorHeap[srvBlockIndex + k]` と引ける。VG も同じブロック確保を使えば MCP のマテリアル編集と整合 |
| F6 | テクスチャは `TextureLoader` が BC7 / BC5 圧縮 + `.dds` ディスクキャッシュ（`TextureLoader.cpp:162-460`）、`.dds` 直読み可（`:110`）。BC7 の CPU 圧縮は実用上遅い（memory `dx12-load-optimization`: DirectXTex は bc7enc16 の 264 倍遅い） | cook 側で BC 化済み `.dds`（ミップ付き）を作り、既存ローダへそのまま渡す（GPU BC7 圧縮は却下済み・オフラインなら別） |
| F7 | `PakArchive::Read` は**ファイル全体を `vector` へ**読む（`PakArchive.h:31`）。`readAt` は単一 HANDLE をロック無しで共有（`PakArchive.h:38`、memory 既知バグ⑥） | 巨大 `.vgeo` は pak に入れず**ループ配置 + 専用 IO ハンドル**（§2.4.4、未決 §7-2） |

### 1.2 フレームの構造（`Application::RenderView`）

| # | 事実 | 含意 |
|---|---|---|
| F8 | 描画リスト `BuildDrawList`（`ApplicationRender.cpp:80-470`）が ECS を 1 回走査して `DrawItem`（`DrawItem.h:21-65`）を作り、`sortKey`（`:306-312`: 0 既定 / 1 カスタム不透明 / 2 スキンド / 3 半透明）、`batchKey`（自動インスタンシング、`:328-`）でソート（`:400-420`）。インスタンス上限 `kMaxInstances = 262144`（`Application.h:1039`） | `DrawItem` に `bool vg` を足し、**VG 適格判定を 1 関数に一本化**（`IsRaytracedItem` `DrawItem.h:117-121` と同じ流儀。判定が 2 箇所に割れると事故る、と同ファイルに明記） |
| F9 | フレーム順序（`RenderView` `ApplicationRender.cpp:4411~`）: 影（CSM / スポット / ポイント）→ 深度プリパス（`:4766-4976`。`DepthPrepassPass` `:4815`、Hi-Z `:4844`、SSAO / コンタクト / RT / SSR-SSGI）→ シーン RT クリア → スカイボックス（`:5291-5308`、深度テスト OFF で全面塗り）→ フォワード+（`ForwardScenePass` `:5318-5349`、`RenderSceneMeshes` `:612`）→ 透明 / パーティクル → `RenderPostChain`（`:5620`） | 挿入点は 3 つ（§2.2.3）: H1 = `DepthPrepassPass` の直後（`:4815`）、H2 = 同ブロック内の G-Buffer 書き込み、H3 = スカイボックスの後・`ForwardScenePass` の前（`:5308` 付近）。パスは IRenderPass（`RenderPass.h`）の流儀で「入口で状態を全部自分で張る」 |
| F10 | 深度は **D32_FLOAT**（`R32_TYPELESS` + SRV `R32_FLOAT`、`ApplicationPipeline.cpp:585-622`）、**標準 Z**（near = 0 / far = 1、`HiZMath.h:11-20` に根拠 3 点）。`DepthPrepassPass::Execute` は冒頭で `ClearDepthStencil`（`ViewPasses.cpp:116`） | VG のラスタは**プリパスの後**に同じ深度へ描く（クリアされないので、非 VG の深度が自然にオクルーダになる） |
| F11 | プリパスは 2 モード（`DepthOnly` / `DepthVelocityGBuffer`。3 つ目は作らない、と確定済み）。後者は MRT = RTV0 速度 `R16G16_FLOAT`（`TaaPass.h:32`）+ RTV1 G-Buffer `R16G16B16A16_FLOAT`（`ApplicationInternal.h:204`。xy = 法線 oct、z = roughness、w = metallic。`VelocityCommon.hlsli`）。**G-Buffer は法線マップ / MR テクスチャを読まない**（同ファイル先頭コメント） | VG の速度 / G-Buffer 書き込みも同水準（頂点法線 + 材質係数）でよい。TAA jitter は SV_Position 側にだけ入れる規約（`VelocityCommon.hlsli`）を踏襲 |
| F12 | フォワード PS 本体は `Forward.hlsl:174-364`。入力 `PSInput`（worldPos / worldNormal / worldTangent / tangentW / color / texCoord / viewDepth / SV_Position、`:105-115`）。ライティング束縛は b1（`Lighting.hlsli:44-90`、1536 B）+ t4 CSM + t5-7 IBL + t8 AO + t9,t10 スポット/ポイント影 + t11 コンタクト + t13-15 クラスタ + t16 SSR + t17 SSGI + t18-21 デカール + t22,t23 DDGI + s0-s5。**Forward / ForwardSkinned / Terrain / Grid は PS 本体をコピーで持ち、重い部品だけ `.hlsli` で共有**する流儀（`Forward.hlsl:20-21`, `ShadowPcss.hlsli`）。画面空間微分は `PBR.hlsli:106-107`（`FilterShadingNormal` の `ddx(N)`）と `DecalApply.hlsli:42-43`（`ddx(worldPos)`）の 2 か所 | VG resolve も同じ流儀で `VgResolve.hlsl` を新設（Forward.hlsl は触らない＝既定 OFF 一致が自明）。微分 2 か所だけは「導出関数に外出し + 既存は薄いラッパ」で解析微分版を追加（§2.2.6） |
| F13 | **ルートシグネチャ**: 14 パラメータ（`RootSignature.cpp:23`）、予算コメント（`:13-22`）= 40 + 2 + 9 + 11 = **62/64、残り 2 DWORD**（依頼文の「約 3」は古い。ログ文言も `:353` で 62/64）。フラグは `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` のみ（`:329`）＝ `HEAP_DIRECTLY_INDEXED` **無し** | メインの RS のままでは `ResourceDescriptorHeap[]` を使う PS を作れない。**専用 RS を作る先例がある**: `DdgiVolume.cpp:124`、`RtScreenPass.cpp:131`。規約: `SetDescriptorHeaps` を `SetRootSignature` の前に呼ぶ（`RtScreenPass.cpp:318-326`） |
| F14 | `PipelineStateBuilder` は `D3D12_GRAPHICS_PIPELINE_STATE_DESC` 固定（`PipelineState.h:16-43`）。VS / PS / IA のみ。MRT 版 `SetRenderTargetFormats` あり（`PipelineState.h:29`）。`GraphicsDevice` は `ID3D12Device5`（`GraphicsDevice.h:28`）＝ `ID3D12Device2::CreatePipelineState`（ストリーム）も呼べる。**メッシュシェーダ Tier は未問い合わせ**（grep 0 件。問い合わせは DXR / SM / BindingTier のみ `GraphicsDevice.cpp:172-250`） | `GraphicsDevice::SupportsMeshShaders()`（`D3D12_OPTIONS7.MeshShaderTier`）と `MeshPipelineStateBuilder` を新設。Agility SDK は不要（メッシュシェーダは Win10 2004+ の in-box ランタイム） |
| F15 | **Hi-Z**: `HiZPass`（`HiZPass.h`）は**今フレームのプリパス深度**から max 縮約（`:20-33` のコメント）。`OcclusionCullPass` は 1 スレッド = 1 `DrawItem` の AABB を判定して `SetPredication` に渡す（`OcclusionCullPass.h`）。判定関数は `HiZMath.h` の `ProjectAabbToScreen` / `SelectHiZMip` / `IsOccludedByHiZ` と `shaders/hiz/HiZCull.hlsl` が一対一（片方だけ直すと誤カリング、と明記） | 既存の遮蔽はオブジェクト単位・単相。VG は**クラスタ単位・二相**が要る（前フレーム HZB を使うので `HiZCull.hlsl` の投影 / ミップ選択関数を `HiZCommon.hlsli` へ外出しして共有する） |
| F16 | **DXR**: BLAS は `Mesh` 単位・**LOD0 固定**・全ジオメトリ `OPAQUE`（`RaytracingScene.h:21-32`, `.cpp:164,268`）、TLAS 上限 `kMaxRtInstances = 32768`（`.h:44`）。TLAS に入る条件は `IsRaytracedItem`（`DrawItem.h:117-121`）に一本化。ヒット点は `GeometryInfo`（16 B: VB raw SRV / IB raw SRV / baseColor SRV / flags、`RtBindless.hlsli:36-42`）で **96 B 頂点の VB** から引く | プロキシを標準 `Mesh` にすれば RT 影 / RT-AO / DDGI は**無改造**。1 億 tri の LOD0 を BLAS にするのは不可能なので、BLAS はプロキシ（VG のクラスタ LOD は TLAS から見えない） |
| F17 | **DDGI**: 段階 0 / 1 / 1.5 / 2 は実装済み、3-a（多重バウンス配線）は既定 OFF で入り、3-b は実測で却下（`DdgiVolume.h` 冒頭、commit `bca218e` / `135469c`）。memory の「次は段階 1」は古い | P8 は「既存 DDGI の延長」＝カスケード化 / リロケーション / VG 対応の検証（§4） |
| F18 | 計測: `GpuTimer` は固定 enum（`GpuTimer.h:19-36`）。MCP `dx12_perf_stats` / `dx12_benchmark`（`tools/mcp-server/index.ts:1305-1335`）、ヘッドレス起動ハーネス `tools/bench/lib/engine.mjs`、`golden.mjs`（`tests/golden/` に 4 枚）、`scale_ladder.mjs`。決定論スクショ（`m_deterministicCapture`、`Application.h:472`） | VG 用スコープと `virtualGeometry` ブロックを足す（TS↔C++ ドリフト検出テストがあるので schema も同時更新）。VG の可視リスト順序が非決定的だと golden が揺れる（§2.4.7） |
| F19 | 既存資産: SRV ヒープは 65536 個 1 枚（`Application.cpp:241`）。UE 抽出の実績: Dreamcore Dead Mall（CUE4Parse、net10、jmap の `.usmap`、`StaticMeshExporter` Gltf2。memory `dreamcore-dead-mall`）。ベンチ用ハイポリ: PerfTest の `dragon_7m.glb`（7.2M tri / 173 MB、memory `dx12-perf-tools`）、`hp_rock` 等 | glTF 経由は 1 億 tri に向かない（4 GB 制限・遅い）→ 自前の中間形式 VGSRC（§3.8）を P6 が出す |
| F20 | **meshoptimizer 1.0**（`vcpkg.json:20`、`build/release/vcpkg_installed/x64-windows/include/meshoptimizer.h:15`）。`meshopt_buildMeshlets` / `Flex` / `Spatial`（`max_vertices ≤ 256`, `max_triangles ≤ 512`）、`meshopt_optimizeMeshlet`、`meshopt_computeMeshletBounds`（球 + 法線コーン s8 表現）、`meshopt_computeSphereBounds`（子の球を内包する球）、`meshopt_partitionClusters`、`meshopt_simplifyWithAttributes` / `WithUpdate`（`vertex_lock`、`SimplifyLockBorder` / `Sparse` / `ErrorAbsolute` / `Prune` / `Regularize` / `Permissive`）、`meshopt_generatePositionRemap` | cook に必要な部品は全部そろっている（バージョンは baseline 固定なので API 変動リスクは低い）。meshoptimizer リポジトリ側の参考実装（`demo/clusterlod.h`）は vcpkg には入らないので**自前で書く** |

### 1.3 現状の限界（なぜ今の経路では目的を満たせないか）

- 従来経路の実測上限: 252k tri × 201 体で 1.26 ms（VB を DEFAULT ヒープにして）、7.2M tri のドラゴン 1 体がやっと。**エンティティ単位 LOD** なので、1 体が 1000 万 tri あると LOD0 か粗い LOD かの二択（memory: 7.2M tri が 722k で描かれていた不具合の温床）。
- `Mesh` は CPU に全頂点を保持（F3）。`ModelLoader` は assimp 経由で全メッシュをメインスレッドで展開。1 億 tri は読み込めない。
- 三角形が画素より小さくなる領域（近距離のハイポリ）で、HW ラスタは 2×2 クアッド単位の無駄で頭打ち。

---
## 2. 設計

### 2.1 全体像

```
[オフライン]                                            [実行時]
 ソース(glb/fbx/obj: assimp)  ─┐                        .vgeo ─(VgeoLoader)→ 常駐: ヘッダ/BVH/材質/ページ表
 UE(CUE4Parse, C#)→ VGSRC    ─┴→ vgeocook.exe ─→ .vgeo   ├─→ プロキシ Mesh + Material（通常の MeshRenderer）
   meshlet化→DAG(グループ/簡略化)→量子化→ページ詰め        └─→ ページ群（全常駐 or ストリーミング）→ GPU ページプール

 フレーム（主ビュー・透視のみ）
  影 ──→ 深度プリパス(非VG) ──→ [H1] VG: 二相カリング→HWラスタ(+SW)→可視性バッファ+深度
                                    └→ [H2] VG G-Buffer/速度パス → HiZ/SSAO/コンタクト/RT/SSR…（既存、無改造）
       ──→ sceneRT クリア → スカイ → [H3] VG resolve(色) → 既存フォワード(非VG, LESS_EQUAL) → 透明 → ポスト
```

新規コードの置き場（案）:

| 場所 | 中身 | D3D 依存 |
|---|---|---|
| `src/vgeo/` | `VgeoFormat.h`（POD + `static_assert`）、`VgeoReader`（CPU 参照デコーダ）、`VgeoWriter`、cook ライブラリ（DAG 構築、ページ詰め）、`VgLodMath.h`（LOD / 誤差の純関数） | なし（テスト可能） |
| `tools/vgeo-cook/` | CLI `vgeocook.exe`（assimp / VGSRC 入力、ベンチメッシュ生成 `--gen-bench`） | なし |
| `tools/ue-cook/` | C# / CUE4Parse（P6）。VGSRC + BC 済み `.dds` を出す | なし |
| `src/renderer/vg/` | `VirtualGeometrySystem`（バッファ / キュー / ページプール / ストリーマ）、`MeshPipelineStateBuilder`、IRenderPass ラッパ（`ViewPasses.h` の流儀） | あり |
| `shaders/vg/` | `VgInstanceCull` / `VgNodeTraverse` / `VgClusterCull`（cs_6_6）、`VgRaster_MS/PS`（ms_6_5）、`VgGBuffer_PS`、`VgResolve_PS`、`VgVisibility.hlsli`、`VgeoDecode.hlsli`、`hiz/HiZCommon.hlsli` | — |
| `tests/` | `vgeo_format_test` / `vgeo_cook_test` / `vg_lod_math_test`（`hiz_math_test` と同じ流儀） | なし |

### 2.2 既存パイプラインへの統合（既定 OFF・最小侵襲）

#### 2.2.1 資産とエンティティの扱い（プロキシ方式）

1. `ResourceManager::GetOrLoadModel`（`ResourceManager.cpp:259-300`）が拡張子 `.vgeo` を見て `VgeoLoader` へ分岐する。返す `CachedModel`（`ResourceManager.h:28-38`）は、**プロキシ `Mesh` 群 + `Material` 群**（通常のモデルと同形）に `std::shared_ptr<VgAsset> vg` を 1 個足したもの。`Scene::Spawn`（`Scene.cpp:76`）と `SceneSerializer`（`:1569` の `meshRenderer.modelPath`）は拡張子非依存なので**シーン JSON は無改造**（`"modelPath": "vg/rock.vgeo"` と書くだけ）。
2. プロキシは cook 時に「グループ誤差 ≤ `rtProxyMaxError`（既定: アセット外接球半径の 1/2000）かつ ≤ 25 万 tri」でカットした DAG の断面から作る。`Mesh::Initialize` は中で meshopt の再最適化と自動 LOD を行うが、25 万 tri 以下なら現行でも問題ない量（F3, F4）。
3. `MeshRenderer` に `const VgAsset* vgAsset`（非シリアライズ）を足す。VG の ON/OFF は**シーン設定** `VirtualGeometrySettings::enabled`（既定 false、`RtSettings` / `DdgiSettings` と同じ流儀でシーン JSON へ保存）と GPU 能力（`SupportsMeshShaders()`）の AND。
4. `DrawItem` に `bool vg` を追加（`DrawItem item{}` で 0 初期化されるので既存経路は不変）。立てる条件は **1 関数 `IsVirtualGeometryItem(const DrawItem&, bool vgActive)`** に一本化する（`IsRaytracedItem` と同じ理由）。

#### 2.2.2 VG 対象外の境界（v1）

| 種類 | 扱い | 理由 / 将来 |
|---|---|---|
| スキンド（`item.skin`）/ ノードアニメ（`hasNodeAnim`） | **VG 対象外**。プロキシを従来経路で描く | 変形後頂点が要る。UE でも Nanite の対象外だった時期が長い |
| アルファテスト（MASK, `alphaClass==1`）/ 半透明（BLEND, `alphaClass==2` / `sortKey==3`） | **VG 対象外**。cook がそのセクションを `NONVG` セクション（§3.7）に分け、**予算内へ簡略化した通常メッシュ**として従来経路（アルファテスト PSO）で描く | HW ラスタの PS でアルファテストは可能（P3 後の拡張候補）だが、SW ラスタとクラスタ LOD の誤差指標が崩れる。v1 では植生・柵は従来経路 |
| カスタムシェーダ（`renderer.shaderPath` 非空 → `sortKey` 1 / 3） | **VG 対象外**（そのエンティティはプロキシを従来経路で描く） | ユーザー PS を resolve に混ぜられない |
| 材質上書き（`HasMaterialAsset` / `HasAnyTextureOverride`）、UV スクロール / 連番、`opacity < 1`、地形スプラット | **VG 対象外**（`batchKey` 適格条件 `ApplicationRender.cpp:328-` と同じ列挙を流用） | 「per-object 定数を要する物」は VG インスタンス表に載せない |
| `colorTint`（RGB888）/ emissive 上書き / metallic・roughness 上書き | **VG 対応**（インスタンス表に `packedTint` / `packedEmissive` を持たせる、§2.4.1） | 実装コストが小さく、`Material.h` の詰め方をそのまま使える |
| ミラー（行列式 < 0）/ 非一様スケール | 対応（フラグ + 誤差スケールは最大軸スケールで保守的に） | |
| 正射ビュー / 副ビュー（カメラプレビュー等）| **VG しない**（プロキシを従来経路で描く） | HZB 履歴が主ビューにしか無い（TAA と同じ制約、`RenderView` の `primary` ゲート） |

#### 2.2.3 フレーム内の挿入点と順序

```
 (1) 影パス                 : VG エンティティはプロキシを既存 PSO で描く（CSM/スポット/ポイント）。RT 影 ON 時は IsRaytracedItem が CSM から除外（既存の排他ルールのまま）
 (2) 深度プリパス(既存)     : DepthPrepassPass  ← 深度を Clear して非VGを描く。item.vg は skip（RenderDepthOnlyScene に 1 行）
 (3) [H1] VG ラスタ         : 二相カリング + ラスタ → 可視性バッファ + 同じ深度へ追記
 (4) [H2] VG G-Buffer/速度  : 全画面 PS が可視性バッファから RTV0(速度)/RTV1(G-Buffer) を VG 画素だけ書く
 (5) Hi-Z(既存)/SSAO/コンタクト/RT/SSR/SSGI : 深度・G-Buffer が完成しているので無改造
 (6) sceneRT クリア → スカイ(既存)
 (7) [H3] VG resolve(色)    : 全画面 PS。VG 画素だけ sceneRT へ書く（深度テスト無し・深度書き込み無し）
 (8) フォワード(既存)       : LESS_EQUAL。VG 画素は深度が手前なので落ちる。item.vg は skip
```

- **`useDepthPrepass` に `|| useVg` を足す**（`ApplicationRender.cpp:4728` 付近）。VG はプリパスの深度が前提で、フォワードが深度を Clear する経路（`:4992`）では成立しない。`velocityPrepass` でないフレームは (4) を丸ごと飛ばす。
- (3)(4) は `DepthPrepassPass` の直後、(7) はスカイボックスの直後に `IRenderPass` として差し込む。各パスは入口で RS / ヒープ / RT / ビューポートを全部自分で張る（`RenderPass.h` の契約）。既存の `ForwardScenePass` も入口で全部張り直すので、(7) の後に RS が変わっていても問題ない（[事実] `ViewPasses.cpp:383-414`）。
- (5) の Hi-Z（既存 `HiZOcclusionPass`）は最終深度（VG 込み）から作られる＝非 VG のオブジェクト単位オクルージョンが VG を遮蔽物として使える。

#### 2.2.4 可視性バッファ

- 形式 **R32_UINT**（レンダー解像度、SRV + RTV）。値 = `(visibleSlot << 7) | triangleIndex`。`triangleIndex` は 0..127（1 クラスタ最大 128 三角形）、`visibleSlot` は「可視クラスタ表（§2.4.1）の添字」で 25bit（最大 3355 万クラスタ、設計上限は 100 万）。空 = `0xFFFFFFFF`。
- 深度は**既存の `m_depthBuffer` をそのまま使う**（32bit float 深度を HW が書く）。64bit（深度 + ID）にしないのは、HW ラスタが 64bit アトミックを持たないため。SW ラスタ（P3b）は別の 64bit UAV へ書き、合流パス（全画面 PS が `SV_Depth` を書く）で `深度テスト付きに` 可視性バッファへ統合する。
- 罠: `ClearRenderTargetView` は UINT フォーマットでは float→uint 変換になる。`0xFFFFFFFF` を確実に入れるなら `ClearUnorderedAccessViewUint`（UAV フラグ付きで作る）を使う。
- どの三角形か → `visibleList[visibleSlot] = {instanceIndex, clusterRef}` → クラスタヘッダ（ページ内）→ 3 頂点をデコード、で全属性が復元できる。

#### 2.2.5 G-Buffer / 速度パス（H2）

- 全画面 PS（`VgGBuffer_PS`）が可視性バッファを読み、VG 画素だけ MRT2（速度 `RG16F`、G-Buffer `RGBA16F`）へ書く。PSO の RT 形式は既存 `RecreateVelocityPsos` と同じ `{kVelocityFormat, kGBufferFormat}`（`ApplicationPipeline.cpp:717-728`）。非 VG 画素は `discard` して既存プリパスの値を残す（`DepthPrepassPass` が出口で SRV 状態にした RT を RTV へ遷移し、また SRV へ戻す）。
- 中身: 三角形 3 頂点の位置・法線をデコード → ピクセルレイと三角形の交点から重心座標 → 法線補間 + 材質係数（metallic / roughness）→ `SS_PackGBuffer`。速度は「現在ワールド位置」を `prevWorld`（インスタンス表）で前フレームへ戻し、非ジッタの前フレーム VP で投影、現フレームからジッタを除去した NDC との差（`VelocityCommon.hlsli` と同一の式・同一の符号規約）。
- 背面: 三角形の向きとピクセルレイの内積で `isFront` を決め、背面なら法線反転（`VelocityPS` と同じ扱い）。

#### 2.2.6 色 resolve（H3）— 既存ライティングを全部そのまま使う

- **全画面 PS `VgResolve_PS`**（`VgResolve.hlsl`）。`Lighting.hlsli` / `ShadowPcss.hlsli` / `DecalApply.hlsli` / `PBR.hlsli` を include し、`Forward.hlsl:174-364` の PS 本体と同じ手順（albedo → 法線 → MR → デカール → 法線フィルタ → CSM + コンタクト → 平行光 → クラスタライト → AO / SSR / SSGI / DDGI / IBL → emissive → クラスタ / デカールのデバッグ）を踏む。**b1 / t4-t23 / s0-s5 は既存のバインドがそのまま使える**（同一パラメータの RS のため）。
- **RS**: `RootSignature::Initialize` に「追加フラグ」引数を足し（既定 0 = 従来と同一）、`m_rootSignatureVg`（`HEAP_DIRECTLY_INDEXED` 付き、パラメータ・静的サンプラは同一）を 2 個目として作る。resolve PSO だけがこちらを使う。resolve の入口で `SetDescriptorHeaps` → `SetRootSignature(m_rootSignatureVg)` → b1 / 各テーブルを `ForwardScenePass::Execute`（`ViewPasses.cpp:383-414`）と同じ手順で張る。**ルートスロット追加 0、DWORD 増分 0**。b0（40 DWORD のオブジェクト定数）は resolve では使わないので、バインドレスの添字群（可視性バッファ / 可視リスト / インスタンス表 / ページプール / 材質表 の SRV ヒープ添字、ジッタ付き VP、ビューポート）を詰める。b2（材質 9 DWORD）は使わず材質表から引く。
- **材質**: `VgMaterialGpu`（インスタンスではなくアセットの `MaterialRecord` から作る）に `srvBlockIndex`（F5 の 4 連続 SRV）・`pbrFlags`（`Material.h` と同じビット）・`defaultMetallic/Roughness`・`uvScaleOffset` を持つ。PS は `ResourceDescriptorHeap[srvBlockIndex + 0..3]` を `NonUniformResourceIndex` 付きで引き、`s0`（異方性 8x の静的サンプラ）で **`SampleGrad`**。
- **画面空間微分**: 可視性バッファ方式では隣接画素が別三角形なので `ddx/ddy` が壊れる。UV の勾配は三角形のスクリーン空間平面から**解析的に**求める（重心座標の x/y 偏微分、標準手法）。既存の 2 か所は「導出関数の外出し」で対応する:
  - `PBR.hlsli:106-107` の `FilterShadingNormal` → 導出を `FilterShadingNormalFromDerivs(N, roughness, Ng, params, dNx, dNy)` に分け、従来の関数は `ddx/ddy` を取ってそれを呼ぶだけの薄いラッパにする。
  - `DecalApply.hlsli:42-43` の `ddx(worldPos)` → 同様に `ApplyDecalsGrad(…, dWdx, dWdy)` を分ける。
  どちらもコンパイル後の DXIL 同値を `shader_include_deps_test` 系の CI で確認する（既定 OFF 一致の担保）。
- **接線**: 頂点に tangent を持たせない（4 B/頂点の節約）。法線マップは三角形の UV 勾配から作る cotangent frame（Schüler）で代用。assimp の MikkTSpace 接線と厳密には一致しないので、これが**フォワードとの既知の差分**（P4 の一致テストで許容差に入れる、§5.4）。
- **半透明 / MASK は resolve に来ない**（対象外境界）。resolve は完全不透明前提でブレンド無効、深度テスト無効。非 VG 画素は最初の分岐（可視性 == 空）で即 return。

#### 2.2.7 影

- **P4〜P6 の暫定**: VG の影は**プロキシ**が落とす（CSM / スポット / ポイントは既存 PSO、RT 影 ON なら TLAS のプロキシ）。プロキシ誤差 `proxyError`（ヘッダ、§3.2）をランタイムが `shadowNormalBias` の下限に使う（VG の細かい面と粗いプロキシのセルフシャドウ / 漏れ対策）。
- **P7（VSM）**: 仮想シャドウマップは VG クラスタを直接ページへ描く（§4 P7）。既存 CSM とは並存し、シーン設定で切り替え（既定 OFF）。

#### 2.2.8 DXR との関係

- **TLAS にはプロキシ**（`IsRaytracedItem` 無改造）。ヒット点のアルベドは `GeometryInfo` → プロキシの VB（96 B）と `baseColorSrvIndex`（プロキシの `Material`）から取るので `RtBindless.hlsli` も無改造（F16）。
- RT 影 / RT-AO は「深度 + G-Buffer からワールドを復元してレイを撃つ」ので、VG 画素の由来を問わない（H2 が G-Buffer を書くのが前提）。
- 差: 光線は粗いプロキシに当たる。VG の細かい表面より数 mm〜数 cm ずれうる → `shadowNormalBias` 下限（上記）と、RT-AO の半径下限を `proxyError` の 2 倍にする。
- 1 億 tri のアセットを 1 個持っても TLAS / BLAS は「プロキシの 25 万 tri × 1 BLAS」で済む。`kMaxRtInstances = 32768`（F16）は従来のまま。
- DDGI（F17）は TLAS のプロキシをトレースするので、VG を入れても GI は変わらない（P8 で検証項目にする）。

#### 2.2.9 ルートシグネチャ残量への影響

| 項目 | 増分 |
|---|---|
| メイン RS のルート DWORD | **0**（62/64 のまま。残り 2 は他機能のために温存） |
| 新規ルートスロット | **0**（既存 14 スロットのまま） |
| 新規 RS オブジェクト | +3（VG カリング用 compute、VG ラスタ用 mesh、resolve 用メイン同型 + `HEAP_DIRECTLY_INDEXED`） |
| 新規 SRV レジスタ | resolve は**バインドレスなので t/u レジスタを消費しない**。将来 VSM（P7）だけ `slot 4` テーブルへレンジ追加（`t25`, `t26`。t3-t24 は使用済み、t25 以降は空き）で、DWORD は増えない |

#### 2.2.10 「既定 OFF で絵が変わらない」の担保

1. `VirtualGeometrySettings::enabled = false`（既定）のとき、VG のバッファ / PSO / ヒープは**一切確保しない**（`RtScreenPass` の遅延確保と同じ）。`.vgeo` を読むエンティティはプロキシを従来経路で描く。
2. 既存シェーダは**バイナリ同値**であること: `Forward.hlsl` 等は無改造、`PBR.hlsli` / `DecalApply.hlsli` は外出しのみ（DXIL 比較）。
3. `RenderDepthOnlyScene` / `RenderSceneMeshes` の変更は `if (item.vg) continue;` の各 1 行だけ。`item.vg` は VG 有効時しか立たない。
4. 機械判定: `tests/golden/`（既存 4 枚）が VG 未使用シーンで**ビット一致**（`golden.mjs` の `bitExact`）、`ctest` 全緑（P2 以降の全マージ条件）。

#### 2.2.11 触る既存ファイル（総覧）

| ファイル | 変更 |
|---|---|
| `src/renderer/DrawItem.h` | `bool vg` 追加、`IsVirtualGeometryItem()` |
| `src/core/ApplicationRender.cpp` | `BuildDrawList` で `vg` を立てる / `RenderView` に H1-H3 を差す / `useDepthPrepass \|\|= useVg`（`:4728`）/ `RenderDepthOnlyScene`（`:1417`）で prepass・velocity モードのみ `vg` を skip / `RenderSceneMeshes`（`:612`）で skip |
| `src/renderer/ViewPasses.{h,cpp}` | `VgRasterPass` / `VgGBufferPass` / `VgResolvePass`（IRenderPass 3 個） |
| `src/graphics/RootSignature.{h,cpp}` | `Initialize(device, extraFlags = 0)`（既定 0 で従来と同一）、VG 用 2 個目 |
| `src/graphics/GraphicsDevice.{h,cpp}` | `SupportsMeshShaders()`（`OPTIONS7`）/ `SupportsInt64Atomics()`（`OPTIONS9`、P3b） |
| `src/graphics/GpuTimer.{h,cpp}` | スコープ追加（`VgCull` / `VgRaster` / `VgGBuffer` / `VgShade`） |
| `src/resource/ResourceManager.{h,cpp}` / `ModelLoader` | `.vgeo` 分岐、`CachedModel::vg` |
| `shaders/forward/PBR.hlsli` / `DecalApply.hlsli` | 微分の外出し（挙動不変） |
| `shaders/hiz/HiZCull.hlsl` | 投影 / ミップ選択 / 遮蔽判定を `HiZCommon.hlsli` へ外出し（`HiZMath.h` との一対一対応は維持） |
| `src/scene/Scene*.{h,cpp}` / `SceneSerializer.cpp` | `VirtualGeometrySettings`（既定 OFF）の保存 / 読込 |
| `CMakeLists.txt`（`:377` 付近の dxc 呼び出し群） | `ms_6_5` / `cs_6_6` / `ps_6_6` の VG シェーダ追加 |
| `tools/mcp-server/*` / `docs/MCP.md` | `dx12_set_virtual_geometry` / `dx12_get_virtual_geometry`、`perf_stats` の `virtualGeometry` ブロック、`render_debug` の VG モード（TS スキーマ・ドリフトテスト・docs を同時更新） |

---

### 2.3 オフライン cook（`vgeocook`）

#### 2.3.1 パイプライン

入力: assimp（glb / fbx / obj、既存の `kUnitScaleFlag` と同じ単位正規化）または VGSRC（§3.8）。出力: `.vgeo` + `textures/*.dds`。

```
0. 前処理   : 位置の NaN/退化三角形の除去、meshopt_generateVertexRemap で重複頂点融合（属性 = pos+normal+uv）、
             マテリアル別セクションに分割（MASK/BLEND のセクションは NONVG へ隔離）。座標は エンジン空間（Y up・LH・m）へ。
1. LOD0     : セクションごとに meshopt_optimizeVertexCache → meshopt_buildMeshletsFlex(max_v=128, min_t=96? max_t=128, cone_weight=0.25)
             → meshopt_optimizeMeshlet。（P1 で buildMeshletsSpatial と A/B。基準は「クラスタ平均充填率」と「後段グループ簡略化の到達率」）
2. DAG ループ（レベル L = 0,1,2,… ; 現レベルのクラスタが 1 個になるまで）
   a. グループ化   : meshopt_partitionClusters(target_partition_size = 4)。各クラスタの頂点インデックス列 + 位置を渡す。結果 2..5 クラスタ/グループ。
   b. ロック集合   : 「位置で溶接した頂点 ID」（meshopt_generatePositionRemap）でエッジ表を作り、
                    グループ境界エッジ（両側の三角形が別グループ）の両端点を vertex_lock=1。溶接 ID が同じ全頂点（UV/法線シームの複製）も同様に 1。
                    ※ 元メッシュの開いた境界（三角形が 1 枚だけのエッジ）は「グローバル境界」なのでロックしない（境界形状は簡略化器の境界ペナルティに任せる）。
                    ※ 材質セクション境界は各セクションが別 DAG のため「グローバル境界」に見えるが、セクション間で共有される頂点は
                       溶接 ID の一致でロック対象にする（＝マテリアル境界は全 LOD で不変。§2.3.4 の代償）。
   c. 簡略化       : meshopt_simplifyWithAttributes(vertex_lock, attributes = {normal xyz, uv xy}, weights = {1.0×3, 0.5×2}（初期値。P1 で調整）,
                    target_index_count = 入力の 1/2, target_error = 無制限, options = Sparse | ErrorAbsolute, &result_error)。
                    ★頂点位置を動かさない版（WithUpdate ではない）を v1 で採用: 出力は元の頂点集合の部分集合になり、量子化格子上の点が保たれる。
   d. 誤差         : groupError = result_error + max(子クラスタの lodError)  （絶対値・アセット空間。三角不等式で累積の上界）
   e. LOD 球       : groupLodSphere = meshopt_computeSphereBounds( 子の lodSphere 群（半径付き）, 簡略化後ジオメトリの点群 )。子の球を内包することを検証。
   f. 再クラスタ   : 簡略化後の三角形を meshopt_buildMeshlets(128/128) で切り直し → optimizeMeshlet → meshopt_computeMeshletBounds（cullSphere + 法線コーン）。
                    出力クラスタは全員 (lodSphere, lodError) = 上記 groupLodSphere / groupError を共有（＝「生んだグループ」の値）。
   g. 親子記録     : 各入力クラスタに parentLodSphere / parentLodError = (groupLodSphere, groupError) を記録（＝「消費したグループ」の値）。
   h. 停滞ガード   : レベル全体の三角形数が前レベルの 85% より減らない場合 → 当該グループだけ LockBorder を外して再試行、
                    それでも減らなければ受け入れて次へ（グループ分割が次レベルで変わるので通常は解消する）。最終グループ（クラスタ ≤ 5）は
                    ロックなしで 128 三角形以下になるまで繰り返し簡略化（ルート）。ルートは lodError = 計算値、parentLodError = +INF。
3. 階層      : 全グループを BVH 化（4 分木）。レベルを無視して空間で分割すると minOwnError 枝刈りが効かないので、
              まず「レベル帯」で分け、帯の内側を空間（メディアン分割）で分割する。ノード = 4 子（cullSphere / lodSphere / minOwnError / maxParentError）。
4. ページ    : 128 KiB ページへ「グループ単位」で詰める（グループを跨がない）。順序 = ルート側（粗いレベル）から。
              ルートページ群は pinned（常駐固定）。各クラスタに childPage（自分を生んだグループのページ）を記録。ページ依存表（CSR）を作る。
5. 量子化    : 座標 = アセット共通の 24bit 格子（§3.3）。法線 = oct16×2。UV = クラスタ局所 16bit。
6. プロキシ  : DAG のカット（グループ誤差 ≤ rtProxyMaxError かつ 25 万 tri 以下の最も細かいカット）を標準頂点（96 B）+ u32 インデックスで書く。
7. テクスチャ: 参照テクスチャを BC7(albedo, sRGB)/BC5(normal)/BC7(MR, linear) の DDS（ミップ付き）に変換（bc7enc 系のオフライン圧縮）。UE 由来は BC 済みをパススルー（P6）。
8. 出力      : ヘッダ・BVH・ページ・材質・プロキシ・NONVG・デバッグ JSON。ページ表の CRC と統計（レベル別クラスタ数 / 誤差 / バイト数）。
```

パラメータ既定値: クラスタ 128 三角形・128 頂点 / グループ目標 4 / 各段 1/2 / 法線重み 1.0・UV 重み 0.5 / cone_weight 0.25 / プロキシ 25 万 tri。クラスタ上限は**ヘッダに記録**（v1 ローダは 128/128 超を拒否）。P3 の実測で 64 頂点 / 124 三角形に下げる可能性を残す（NVIDIA 推奨値。§6 R1）。

#### 2.3.2 誤差指標・単調性・LOD 選択条件

- **誤差の定義**: アセット空間の絶対距離（メートル）。`lodError` = そのクラスタを「生んだグループ」の累積簡略化誤差（LOD0 は 0）。`parentLodError` = そのクラスタを「消費したグループ」の誤差（ルートは +INF）。
- **不変条件（cook が assert、テストで検証）**:
  1. `parentLodError ≥ lodError`（等号可。累積加算なので通常は狭義）
  2. 親の `lodSphere` ⊇ 子の `lodSphere`（半径の許容差 1e-4 相対）
  3. 同じグループから生まれたクラスタは同一の `(lodSphere, lodError)` を持つ / 同じグループに消費されたクラスタは同一の `(parentLodSphere, parentLodError)` を持つ
- **画面空間誤差**（ランタイム。HLSL と C++ `VgLodMath.h` で一対一、`hiz_math_test` と同じ流儀でテスト）:
  ```
  // 透視: projScale = 0.5 * 画面高(px) / tan(fovY / 2)。s = インスタンスの最大軸スケール
  float ProjectedErrorPx(float err, float3 sphereCenterWorld, float sphereRadiusWorld, float3 camPos, float s, float projScale, float zNear)
  {
      float d = max(length(sphereCenterWorld - camPos) - sphereRadiusWorld, zNear);   // 球の最近点までの距離
      return err * s * projScale / d;
  }
  // 描くクラスタの条件（τ = lodPixelError、既定 1.0 レンダー px）
  bool fine        = ProjectedErrorPx(lodError,       lodSphere,       ...) <= tau;
  bool parentCoarse = ProjectedErrorPx(parentLodError, parentLodSphere, ...) >  tau;
  bool childrenResident = (childPage == NONE) || pageTable[assetPageBase + childPage] != NOT_RESIDENT;
  bool draw = parentCoarse && (fine || !childrenResident);     // 最後の項が「子が未ロードなら親を描く」フォールバック（§2.4.4）
  ```
  親の球が子の球を内包し、誤差が単調なので、`ProjectedErrorPx(parent) ≥ ProjectedErrorPx(child)` が常に成り立つ＝どの視点でもカットが DAG の「切断」になる。正射は `err * s * 画面高 / 正射高`。TSR（P9）でアップスケールする場合は τ を**出力解像度の px**で評価する（レンダー px に換算して渡す）。
- **BVH ノードの枝刈り**（保守的）: 部分木に描くクラスタが**存在しうるか**を見る。
  - 親側（粗すぎないか）: `ProjectedErrorPx(maxParentError, node.lodSphere の最近点) > τ` でなければ部分木ごと捨てる。
  - 自分側（細かすぎないか）: `ProjectedErrorPx(minOwnError, node.lodSphere の最遠点) ≤ τ` でなければ捨てる（球の最遠点を使うのは、メンバーの球が `lodSphere` に内包され、距離が最大でも `dist_far` 以下だから。射影誤差の下界になる）。
- **クラック防止（全 5 点。どれか 1 つ欠けても割れる）**:
  1. 座標は**アセット全体で共通の 24bit 格子**（クラスタごとではない）。同じ頂点はどのクラスタでも同じ float になる。
  2. グループ境界の頂点は**溶接ベースでロック**（UV / 法線シームの複製頂点も含む）。
  3. 簡略化は**頂点を動かさない**（`simplifyWithAttributes`）。
  4. 同じグループを出入りするクラスタは同じ `(lodSphere, lodError)` / `(parentLodSphere, parentLodError)` を共有 → 描く / 描かないの判定がグループ内で一致。
  5. 単調性（上記不変条件）→ LOD の切り替わりがグループ境界で食い違わない。
  **機械テスト**（§5.3）: 距離をスイープして「描くクラスタ集合」を CPU 参照実装で選び、三角形を量子化座標の溶接 ID に直して**全エッジが 2 回ちょうど出現する（元メッシュの境界エッジを除く）**ことを確認する。

#### 2.3.3 圧縮とサイズ（[推定]、P1 で実測）

- 1 クラスタ ≈ 1.4 KB（ヘッダ 128 B + 頂点 ~70 個 × ~13 B + 三角形 3 B × ~110）→ **約 12〜13 B/tri**。DAG 全体は各段で三角形が半分になるので LOD0 の **約 2 倍**の三角形を持つ ⇒ 約 **25 B / LOD0 三角形**。
- ページ単位圧縮（既存 `vfs/Compression` の XPRESS_HUFF）は**ヘッダフラグで任意**（v1 は無圧縮 = ディスクから GPU へ直接コピーできる。ロード時間が支配的になったら有効化）。

#### 2.3.4 既知の代償と割り切り

- **マテリアル境界は全レベルで頂点が減らない**（別セクション = 別 DAG、共有頂点は永久ロック）。マテリアル数 100 超のアセットは粗いレベルの三角形数が下がりにくい → cook レポートに「ロック頂点率」を出す。マルチマテリアルクラスタは v2（ヘッダに `clusterMaterialMode` を予約）。
- 頂点位置を動かさないので同じ三角形数での品質は `WithUpdate` 版より少し劣る。P1 のチューニング項目（v1.1 で `WithUpdate` + 再量子化を評価）。
- 頂点カラーは v1 では持たない（ModelLoader は白が既定で、実質使われていない）。頂点カラー付きの入力は cook が警告し無視する。
- tangent は持たない（§2.2.6）。

#### 2.3.5 cook の性能・メモリ（[推定]、P1 で実測して合否に使う）

- LOD0 の構築は線形。簡略化は各レベルで総三角形が半分になるので総作業量は LOD0 の約 2 倍。グループ単位で独立なので**スレッド並列**（`std::thread` のワーカープール）。目標: **1000 万 tri を 20 コアで 5 分以内**、1 億 tri で 1 時間以内。
- 作業メモリ: 位置 + 法線 + UV = 32 B/頂点 + インデックス 12 B/tri ≈ 1 億 tri で 6〜8 GB。これを超えるなら空間タイル分割（v1 では実装しない。境界が固まるため）。

---

### 2.4 GPU 側

#### 2.4.1 バッファ設計（すべてバインドレス。専用 RS の root CBV は 1 本 + ルート定数のみ）

| バッファ | 型 / 構造 | 寿命 | サイズの目安 |
|---|---|---|---|
| `VgPagePool` | `ByteAddressBuffer`（DEFAULT）。128 KiB スロットの配列 | 常駐 | 全常駐モード: アセット総量 / ストリーミング: 予算 512 MB〜2 GB |
| `VgPageTable` | `StructuredBuffer<uint>`。`assetPageBase + pageIndex → スロット or 0xFFFFFFFF` | 常駐（差分更新） | 4 B × 総ページ数（1 億 tri で約 1.9 万 = 76 KB） |
| `VgNodes` | `StructuredBuffer<HierNode>`（192 B、§3.5）。アセットごとに連結 | 常駐 | 5000 万 tri で ≈ 12 MB |
| `VgAssets` | `StructuredBuffer<VgAssetGpu>`（64 B） | 常駐 | 数 KB |
| `VgMaterials` | `StructuredBuffer<VgMaterialGpu>`（64 B: `srvBlockIndex` / `pbrFlags` / 係数 / `uvScaleOffset`） | 常駐 | 数 KB |
| `VgInstances` | `StructuredBuffer<VgInstance>`（128 B）。UPLOAD リング（3 フレーム分、`m_instanceMapped` と同じ流儀） | 毎フレーム（静的シーンは前フレーム流用） | 上限 65536 インスタンス = 8 MB × 3 |
| `VgNodeQueue[2]` | `RWStructuredBuffer<uint2>`（{instance, node}）ping-pong + カウンタ | フレーム内 | 各 2^21 × 8 B = 16 MB |
| `VgGroupQueue` | `RWStructuredBuffer<uint2>`（{instance, groupPacked}） | フレーム内 | 2^20 × 8 B |
| `VgDeferredQueue` | 二相目へ回す（HZB でだけ落ちた）要素 | フレーム内 | 2^20 × 8 B |
| `VgVisibleList` | `RWStructuredBuffer<uint2>`（{instance, clusterRef}）HW 用 / SW 用の 2 本 | フレーム内 | 2^20 × 8 B × 2 |
| `VgArgs` | `ExecuteIndirect` 引数（DispatchMesh / Dispatch、ノード走査用 Dispatch） | フレーム内 | 数百 B |
| `VgStats` / `VgRequests` | 統計カウンタ / ページ要求ビットマップ + 優先度（READBACK 3 面） | フレーム内 | 数 KB〜数百 KB |
| 可視性バッファ | `R32_UINT` 2D（レンダー解像度） | 常駐 | 1080p で 8.3 MB |
| VG 用 HZB | `HiZPass` のインスタンスをもう 1 個（`m_vgHiZ`）。前フレーム最終深度から作った物が次フレームの一相目の入力 | 常駐 | 1080p で 約 11 MB |

`clusterRef` = `pageIndex(16) | clusterIndexInPage(8) | reserved(8)`（アセット相対）。ノード `ref` / `groupPacked` は §3.5。`VgInstance`（128 B）:

```
off  size  field
0    48    float3x4 world           (行 3 本 × float4。4 行目 (0,0,0,1) は省略)
48   48    float3x4 prevWorld       (TAA 無効時は world と同値)
96   4     u32 assetIndex
100  4     u32 flags                bit0 mirrored(det<0) / bit1 hasPrev / bit2 castShadow(将来) / bit3 selected(将来)
104  4     u32 packedTint           RGB888 + opacity8（Material.h と同じ詰め方、opacity は 255 固定）
108  4     u32 packedEmissive       PackEmissive() と同じ
112  4     f32 maxScale             ワールド行列の最大軸スケール（誤差 / 球半径の上界）
116  4     u32 entityId             デバッグ / ピッキング
120  8     u32 reserved[2]
```

#### 2.4.2 カリングの構成（二相 HZB）

すべて compute（`cs_6_6`、wave = 32 の Blackwell 前提で `numthreads(64,1,1)`）。1 フレームの流れ:

```
K0 VgInstanceCull  (1 スレッド = 1 インスタンス)
   アセット全体の外接球を world へ → 錐台（6 平面）/ 画面占有 < 0.5px なら破棄 / 一相目は前フレーム HZB で球の AABB を判定
   生き残り → NodeQueue[0] に {instance, rootNode}。一相目で HZB により落ちたものは DeferredQueue へ {instance, rootNode}
K1 VgNodeTraverse  (1 スレッド = キュー要素 1 個 × 子 4 個。ExecuteIndirect でウェーブフロント、深さ D ≈ 10〜12 回)
   子ごとに: 錐台（cullSphere→world）/ LOD 枝刈り（§2.3.2 の親側・自分側）/ HZB（一相目 = 前フレーム、二相目 = 中間 HZB）
   子がノード → 次の NodeQueue へ。子がグループ（葉）→ GroupQueue へ。HZB でのみ落ちた子 → DeferredQueue
K2 VgClusterCull   (1 スレッド = グループ × メンバークラスタ 1 個)
   ページ表でスロット解決 → ページ未常駐なら「要求ビット」を立ててスキップ（フォールバックで親が描かれる）
   クラスタヘッダから: 錐台 / 法線コーン（背面のみクラスタの棄却。ミラー時は符号反転）/ HZB / draw 条件（§2.3.2）/
   画面上の辺長の推定（maxEdgeLength）→ HW リスト or SW リスト（P3 までは全部 HW）へ追記
   → VgArgs の DispatchMesh カウント
R1 ラスタ（HW: ExecuteIndirect(DispatchMesh)、SW: compute）→ 可視性バッファ + 深度
H  VgHiZ Build（今フレーム深度 → 中間 HZB）
二相目: DeferredQueue を入力に K1 → K2 を再実行（今度は中間 HZB で判定）→ R1 → 可視性バッファ完成
H  最終深度から HZB を作り直し（次フレームの一相目の入力）
```

- **一相目の HZB は前フレームの物**なので、カメラ / オブジェクトが動くと誤って「隠れている」と判定しうる。そのぶんは**二相目が救う**（今フレームの深度で再判定するので偽陰性は残らない）。`prevWorld` があれば一相目の判定にも使う（無いときは現在の行列で判定するだけで、二相目が救うので正しさは保たれる）。カメラカット / リサイズ直後は HZB 履歴を無効化し、一相目は「隠れていない」扱い。
- **HZB の判定式は既存を共有**: `HiZMath.h` の `ProjectAabbToScreen` / `SelectHiZMip` / `IsOccludedByHiZ`（標準 Z・max 縮約、F15）を `HiZCommon.hlsli` へ外出しして VG カリングから呼ぶ。球は「球を包む軸平行箱」に直して同じ関数へ渡す。
- 走査を multi-dispatch にする理由: persistent threads（1 ディスパッチで自己完結）は D3D12 に前進保証が無い。まず安全な形で作り、バリア間の隙間が計測で 0.5 ms を超えたら P2b で persistent 化を試す。Work Graphs は Agility SDK が要る（このエンジンは意図的に使っていない、F13 系の方針）ので**却下**。
- 容量超過: 各キューの上限を超えたら `overflow` フラグを統計へ出し、次フレームの τ を自動で 1.25 倍に（クラスタ上限に達したときの安全弁）。

#### 2.4.3 ラスタライズ

**HW（メッシュシェーダ、P3）**

- `ms_6_5`（`as` は使わない）。`[numthreads(128,1,1)] [outputtopology("triangle")]`、出力 ≤ 128 頂点 / ≤ 128 プリミティブ。`SV_GroupID.x` = HW リストの添字。グループ内 128 スレッドが「頂点 i」と「三角形 i」を分担。
- 頂点: クラスタヘッダ（ページ内）から `posMin` / `posBits` を読み、ビット詰めの位置をデコード → アセット空間 → インスタンス行列 → ジッタ付き VP（`camVPJ`、深度プリパスとビット一致させる）。プリミティブ: 三角形ブロックから 3 頂点のローカルインデックス。**プリミティブ属性**として `nointerpolation uint vis = (slot << 7) | triangleIndex`。
- PS: `SV_Target0 = vis`（R32_UINT）だけ。深度は HW が書く（`DEPTH = LESS`、非 VG 深度がオクルーダ）。カリングは NONE（フォワードと同じ両面、`ApplicationPipeline.cpp:98-104`）。MASK は無い前提なので PS にテクスチャは無い。
- **PSO**: `MeshPipelineStateBuilder`（新設。`D3DX12_MESH_SHADER_PIPELINE_STATE_DESC` 相当のストリームを `ID3D12Device2::CreatePipelineState`。`d3dx12_pipeline_state_stream.h` は directx-headers に同梱のはず — P3 冒頭で確認）。`ExecuteIndirect` のコマンドシグネチャは `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH` 1 本（RS 変更なしなので `pRootSignature = nullptr`）。
- GPU 能力: `MeshShaderTier ≥ 1`（`OPTIONS7`）が必須。無ければ VG は静かに無効（プロキシ描画）。

**SW（compute、P3b・条件付き）**

- 1 ワープ = 1 クラスタ（またはスレッド = 1 三角形）。境界矩形内を走査し、`InterlockedMin(RWTexture2D<uint64_t>, (depthBits << 32) | vis)`（`OPTIONS9.AtomicInt64OnTypedResourceSupported`）。合流パス（全画面 PS が `SV_Depth` を書く）で HW 結果と深度テストして可視性バッファ + 深度へ統合。
- **切り替え基準**: K2 がクラスタの画面上の最大辺長（推定）`edgePx = maxEdgeLength * s * projScale / d` を出し、`edgePx < T_sw`（初期値 18 px、Nanite の既定と同水準。P3b で 5060 実測してチューニング）なら SW リスト、以上なら HW リスト。**P3 では `T_sw = 0`（全部 HW）で開始**し、リスト分岐の配線と統計だけ先に入れておく。
- **実装の要否は P3 の実測ゲートで決める**（§6 R1）。

#### 2.4.4 ストリーミング（P5）

- **ページ = 128 KiB 固定**（§3.6）。GPU 側は 1 本の巨大 `VgPagePool`（スロット配列）。ページ表 `VgPageTable` が「アセットのページ → スロット」を引く。
- **要求**: K1 / K2 が「必要だが未常駐」のページの `VgRequests` ビット + 優先度（`InterlockedMax` で最大の画面誤差 × 256）を立てる。3 フレーム遅れの READBACK（`OcclusionCullPass::CollectStats` と同じ流儀）で CPU が読む。
- **選択**: 優先度上位の N ページ / フレーム（既定 32 ページ = 4 MB）を、**依存ページが常駐していること**（ページ依存表、§3.6）を条件にロード。ルートページ群（`pinned`）は起動時に全部ロード。
- **IO**: ワーカースレッド + `ReadFile`（OVERLAPPED、セクタ境界 4096 整列で無バッファ IO 可）。DirectStorage は却下済み（memory）。読み出し先は UPLOAD リング、`CopyBufferRegion` でプールへ（フェンス管理は `DeferredRelease` の流儀）。
- **退避**: GPU が毎フレーム立てる `VgUsed` ビットから LRU。ピン留め・子の常駐ページが依存するページは退避しない。予算は `min(設定値, DXGI の VideoMemoryInfo.Budget × 0.5)`（`IDXGIAdapter3::QueryVideoMemoryInfo` は現状エンジンに無い。P5 で追加）。
- **穴が空かない理由**: 「子ページが未常駐なら親を描く」フォールバック（§2.3.2 の `childrenResident`）と、「親ページが常駐している時にしか子ページをロードしない」不変条件（依存表）。ルートは常に常駐。
- 全常駐モード（P2〜P4）は「全ページを起動時にロードして `VgPageTable` を全部埋める」だけ。同じシェーダが動く（`childrenResident` が常に真）。

#### 2.4.5 VRAM 見積もり（5000 万 tri 級、[推定]）

| 項目 | 見積もり |
|---|---|
| ページ（全 DAG） | 5000 万 tri × 約 25 B ≈ **1.25 GB**（全常駐）。5000 万 tri が 4 アセット × 1250 万 tri の場合。同一アセットを多数配置する場合はデータは共有なので 1 アセット分だけ |
| BVH + 表 | 約 12 MB / 5000 万 tri |
| キュー / 可視リスト / 統計 | 約 60 MB |
| 可視性バッファ + HZB | 約 20 MB（1080p） |
| **VG 合計** | **約 1.35 GB**（全常駐）。8 GB の 17% |
| 1 億 tri / UE 級（複数 GB） | ストリーミング（P5）。作業セット ≈ 画面が要求するクラスタ（可視 30k〜100k クラスタ × 1.4 KB ≈ 40〜140 MB）+ 先読み ×2〜3 ⇒ 予算 512 MB〜1 GB |
| 描画される三角形（1080p） | 約 2〜4 M（画素数 × 0.5〜1.5 tri/px × 二相後の重なり）。**入力 5000 万 tri でも描くのは 1 桁少ない**のが VG の要点 |

#### 2.4.6 GPU 時間の予算（1080p・5060・[推定]。P3 / P4 の実測で差し替える）

| パス | 目標 |
|---|---|
| K0 + K1 + K2（二相合計）| 1.0 ms |
| HW ラスタ（2〜4 M tri / 6 万クラスタ）| 2.0〜4.0 ms |
| VgHiZ Build ×2 | 0.2 ms |
| VG G-Buffer / 速度 | 0.5 ms |
| VG resolve（色。既存フォワード 1 パス相当のライティング）| 3.0〜4.0 ms |
| **VG 合計** | **6.7〜9.7 ms**（60 fps = 16.6 ms の枠で、影・ポスト・GI に 7 ms 前後を残す） |

#### 2.4.7 決定論

- 可視クラスタ表への追記が `InterlockedAdd` だと順序が起動ごとに変わり、同一深度の面（z-fight）が揺れて golden が不安定になる（`DrawItem.h` の安定キーのコメントと同じ事故）。`m_deterministicCapture` 中だけ、K2 の出力を「ビットマップ → プレフィックスサム」の**安定コンパクション**（インスタンス番号 × クラスタ番号順）に切り替える（コストは決定論キャプチャ時のみ）。
- 二相目の再判定順も同様に安定化する。

#### 2.4.8 P4 で確定した設計判断（2026-09-30。§2.2.5 / §2.2.6 / §2.2.9 / §2.4.7 を上書きする）

**(1) ヒープ問題 = 「VG のディスクリプタをアプリの SRV ヒープへ一本化」で解く（採用）**

- 事実: P2/P3 の VG は専用ヒープ（16384 個）に全ディスクリプタを持ち、アプリのヒープ（65536 個、材質テクスチャ・影・IBL・クラスタ等）とは同時にバインドできない（CBV_SRV_UAV ヒープは 1 本だけ）。resolve は「材質テクスチャ（アプリ側）」と「可視性バッファ / 可視リスト / インスタンス / ページ（VG 側）」の両方を 1 回の描画で引く必要がある。
- 候補: (a) 材質 SRV を VG ヒープへ複製（影・IBL・クラスタ等のテーブルもアプリ側なので結局足りない＝**不可**）/ (b) VG のバッファ SRV をアプリのヒープにも**二重に**作る / (c) **VG のディスクリプタを最初からアプリのヒープへ確保する**（VG 専用ヒープを使わない）。
- **採用 = (c)**。`vg::SystemDesc::externalHeap`（ヒープと AllocateBlock / FreeBlock のコールバック）を渡すと、VG は固定領域（先頭 128 個）とアセットごとのブロックをアプリのヒープから取る。VG のルートシグネチャは元から `HEAP_DIRECTLY_INDEXED` なので、添字の数え方が変わるだけでシェーダは無改造。**ヘッドレステスト（窓なし D3D12）は従来どおり専用ヒープ**（externalHeap 無し）。
  - 利点: カリング / ラスタ / HZB 再構築 / G-Buffer / resolve が**全部同じヒープ**で動く＝`SetDescriptorHeaps` の往復（ヒープ切り替えは GPU によってはパイプラインのフラッシュを伴う）が消える。resolve は VG の添字をそのまま使える（二重化の同期・寿命管理が要らない）。
  - 代償: アプリのヒープを VG が使う（固定 128 + アセットごとに 1 + チャンク数 = 5000 万 tri 級で約 150 個。65536 個に対して 0.3%）。VG OFF では 1 個も確保しない（遅延初期化）ので「VG OFF で SRV ヒープ使用量が不変」（§5.5）は保たれる。
- resolve のルートシグネチャ = **メイン RS そのもの**（G2a でメイン RS に `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` が立った＝設計書の `m_rootSignatureVg`（別 RS）は不要になった。§7.4 の整合表どおり）。b0（40 DWORD のルート定数）を resolve 用の添字 + ジッタ付き VP（28 DWORD）に読み替え、b1 / t4〜t23 / s0〜s5 はフォワードと同じテーブルをそのまま張る。**メイン RS の DWORD 増分 0・スロット増分 0**。メイン RS がバインドレスでない環境（`DX12_DISABLE_MAIN_BINDLESS=1` / SM 6.6 未満）は VG のラスタごと無効（プロキシ描画に縮退）。

**(2) resolve = 全画面 PS（CS ではない）**

- sceneRT（R16G16B16A16F）は RTV のみで UAV を持たない / フォワードの尾部（`ForwardShade.hlsli`）が PS 前提（`SV_Position` の画面座標で SSAO・SSR・SSGI・クラスタ・デカールを引く）/ 全画面 1 三角形 + 可視性が空の画素は即 `discard`。材質のタイル分類（波面の一貫性）は実測で要らなければ入れない（§5.2 の結果を P4 レポートに記録）。
- シェーディングは `ForwardShade.hlsli` の `UnoShadeLighting` → 自己発光 → `UnoShadeFinish` を**そのまま呼ぶ**（Forward.hlsl の PS 本体の複製はしない。§7.4）。
- **画面空間微分**: 可視性バッファから三角形の 3 頂点を復元し、2D 同次座標の逆行列（Olano-Greer）で透視補正の重心座標と「隣の画素（x+1 / y+1）での重心座標」を求め、UV・位置・法線の 1 画素差分を解析的に作る（`w ≤ 0` の頂点があっても破綻しない形）。テクスチャは `SampleGrad`。既存の `ddx/ddy` 2 か所（`PBR.hlsli` の `FilterShadingNormal`、`DecalApply.hlsli` の `ApplyDecals`）は**マクロのフック**（`UNO_SHADE_DDX_N` / `UNO_DECAL_DDX_W` 等。既定は `ddx(x)`）へ置き換え、resolve だけが解析値を差し込む。既存シェーダはプリプロセス後のテキストが同一＝DXIL 同値（`tools/shader_dxil_equiv.ps1` で確認）。
- 接線は頂点に持たないので、三角形ごとの UV 勾配から作る（assimp の CalcTangentSpace と同じ式・同じ符号規約）。

**(3) G-Buffer / 速度 = VG の RS の全画面 PS（H2）**

- 可視性バッファから同じ復元をして、速度（ジッタ無しの今 / 前フレーム VP、インスタンスの `prevWorld`）と G-Buffer（頂点法線の補間 + 背面なら反転、材質係数の roughness / metallic を 8bit 量子化 = 深度プリパスと同じ）を MRT で書く。VG でない画素は `discard`（プリパスの値を残す）。速度プリパスが走るフレーム（TAA / SSR / SSGI / RT-AO）だけ。

**(4) 決定論 = 可視リストのフェーズごとの安定ソート**

- 仕様上、増幅シェーダ無しの `DispatchMesh` は**グループ順にラスタ結果が確定する**（D3D12 Mesh Shader 仕様 "Rasterization Order"）。そこで各フェーズのラスタ直前に可視リスト `[start, start+count)` を `(instance, clusterRef)` の 64bit キーで**ビトニックソート**（全比較が昇順の「反転 + 半クリーナー」版。末尾の仮想パディングへは書かないので範囲外書き込みが無い）してから描く。可視スロット（= 可視性バッファの値）も並びも起動ごとに同一になる。`stableOrder`（MCP）または決定論キャプチャ中に自動で ON。AS 経由は子グループの順序が不定なので、安定モードでは MS のみに切り替える。
---

## 3. `.vgeo` 仕様（v1.0）

### 3.1 規約

- リトルエンディアン。構造体は `#pragma pack(1)` 相当（`VgeoFormat.h` に POD + `static_assert(sizeof / offsetof)`、`PakFormat.h` と同じ流儀）。
- 各セクションの先頭は **4096 B 整列**（無バッファ IO のセクタ境界）。ページは 131072 B 固定長（無圧縮時）。
- 「オフセット」列は**構造体先頭からのバイト**。「ページ先頭」= そのページの 0 バイト目。
- 座標系: **エンジン空間**（Y up・左手系・メートル）で書く。UE の Z up・cm → エンジンへの変換は cooker / VGSRC の `coordSystem` が担当（§3.8）。エンティティのスケールは 1 で置ける。
- v1 はスキン / 頂点カラー / tangent / 2 セット目 UV を持たない（予約のみ）。

### 3.2 ファイル全体とヘッダ（512 B、offset 0）

```
[0x0000] VgeoHeader                      512 B
[      ] MATERIALS   section             (4096 整列)
[      ] STRINGS     section
[      ] NODES       section
[      ] PAGE_TABLE  section
[      ] PAGE_DEPS   section
[      ] PAGES       section             ← 大半はここ。ページ i は (pagesBase + i * 131072)（無圧縮時）
[      ] PROXY       section
[      ] NONVG       section
[      ] DEBUG_JSON  section
```

`VgeoHeader`（512 B）:

| offset | size | 型 | 名前 | 内容 |
|---:|---:|---|---|---|
| 0 | 4 | char[4] | `magic` | `"VGEO"`（u32 LE = `0x4F454756`） |
| 4 | 2 | u16 | `versionMajor` | 1。互換を壊す変更で +1。ローダは未知の major を拒否 |
| 6 | 2 | u16 | `versionMinor` | 0。予約領域の追加だけ +1（古いローダも読める） |
| 8 | 4 | u32 | `headerSize` | 512 |
| 12 | 4 | u32 | `flags` | bit0 = ページ圧縮（XPRESS_HUFF）/ bit1 = プロキシあり / bit2 = NONVG あり / bit3..31 予約 |
| 16 | 4 | u32 | `maxClusterVerts` | 128（ローダは 128 超を拒否） |
| 20 | 4 | u32 | `maxClusterTris` | 128（同上） |
| 24 | 4 | u32 | `pageSize` | 131072 |
| 28 | 4 | u32 | `pageCount` | |
| 32 | 4 | u32 | `clusterCount` | 全レベル合計 |
| 36 | 4 | u32 | `groupCount` | |
| 40 | 4 | u32 | `nodeCount` | |
| 44 | 4 | u32 | `levelCount` | DAG のレベル数（0 = 最も細かい） |
| 48 | 8 | u64 | `sourceTriangleCount` | LOD0 の総三角形数（VG セクションのみ。統計 `sourceTrisInFrustum` の基準） |
| 56 | 8 | u64 | `sourceVertexCount` | |
| 64 | 12 | f32[3] | `aabbMin` | アセット空間 |
| 76 | 12 | f32[3] | `aabbMax` | |
| 88 | 16 | f32[4] | `boundingSphere` | xyz + r（インスタンスカリング用） |
| 104 | 12 | f32[3] | `posOrigin` | 量子化格子の原点（= `aabbMin`） |
| 116 | 4 | f32 | `posStep` | 格子間隔（= `max(extent) / 16777215`） |
| 120 | 4 | u32 | `materialCount` | |
| 124 | 4 | u32 | `proxySectionCount` | |
| 128 | 4 | u32 | `nonVgSectionCount` | |
| 132 | 4 | f32 | `proxyError` | プロキシの LOD0 からの最大偏差（アセット空間 m）。ランタイムがバイアス下限に使う |
| 136 | 4 | u32 | `rootNode` | 0（BVH 根） |
| 140 | 4 | u32 | `rootClusterCount` | `parentLodError = +INF` のクラスタ数 |
| 144 | 4 | u32 | `pinnedPageCount` | 常駐固定ページ数（ルート側のページ先頭から連続） |
| 148 | 4 | u32 | `clusterMaterialMode` | 0 = 1 クラスタ 1 マテリアル（v1 固定）。1 は v2 予約 |
| 152 | 8 | u64 | `sourceHash` | 入力内容の FNV-1a 64（再 cook 判定） |
| 160 | 4 | u32 | `cookParamsHash` | cook パラメータのハッシュ |
| 164 | 4 | u32 | reserved | 0 |
| 168 | 32 | char[32] | `cooker` | 例 `"vgeocook 1.0.0 meshopt 1.0"`（NUL 終端） |
| 200 | 288 | SectionEntry[9] | `sections` | 下表。32 B × 9 |
| 488 | 20 | u8[20] | reserved | 0 |
| 508 | 4 | u32 | `headerCrc32` | 0..507 バイトの CRC32 |

`SectionEntry`（32 B）: `u64 offset` / `u64 size` / `u32 count` / `u32 stride` / `u32 crc32`（0 = 未計算）/ `u32 reserved`。

| 添字 | セクション | count | stride | 内容 |
|---:|---|---|---|---|
| 0 | MATERIALS | `materialCount` | 96 | `MaterialRecord`（§3.4） |
| 1 | STRINGS | 総バイト数 | 1 | UTF-8、NUL 区切り。テクスチャの相対パス（`.vgeo` のディレクトリ基準）、名前 |
| 2 | NODES | `nodeCount` | 192 | `HierNode`（§3.5）。**そのまま `VgNodes` へコピー** |
| 3 | PAGE_TABLE | `pageCount` | 32 | `PageTableEntry`（§3.6） |
| 4 | PAGE_DEPS | 依存総数 | 4 | CSR: `u32 offsets[pageCount+1]` の後に `u32 deps[]` |
| 5 | PAGES | `pageCount` | 131072 | ページ本体（§3.6）。**そのまま `VgPagePool` のスロットへコピー** |
| 6 | PROXY | ProxySection 数 | 可変 | §3.7 |
| 7 | NONVG | 同上 | 可変 | §3.7 |
| 8 | DEBUG_JSON | 総バイト数 | 1 | cook パラメータ / レベル別統計 / ロック頂点率（実行時は読まない） |

### 3.3 座標の量子化（クラック防止の要）

- **全クラスタ共通の 24bit 格子**: `q = clamp(round((p - posOrigin) / posStep), 0, 16777215)`（各軸）。復元 `p = posOrigin + q * posStep`。量子化誤差 ≤ `posStep / 2` ≈ 最大辺の 3e-8 倍（元の float32 の丸めと同程度）。
- クラスタは `posMin`（i32×3、格子単位）と軸ごとのビット幅 `bx, by, bz`（各 0..24）を持ち、頂点は `q - posMin` を `bx+by+bz` ビットで詰める。同じ頂点はクラスタが違っても**同じ `q`** → 同じ float。
- 法線 = 八面体 oct16×2（snorm）。UV = クラスタ局所: `uv = uvBase + (u16, v16) / 65535 * uvScale`（UV は縫い目で頂点が分かれるので、クラスタ間で一致させる必要は無い）。

### 3.4 `MaterialRecord`（96 B）

| offset | size | 型 | 名前 | 備考 |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `nameOff` | STRINGS 内 |
| 4 | 4 | u32 | `flags` | bit0 hasNormalMap / bit1 hasMetalRough / bit2 alphaTest（NONVG） / bit3 hasEmissiveTex / bit4 doubleSided / bit5 blend（NONVG） |
| 8 | 4 | u32 | `albedoPathOff` | STRINGS 内。`0xFFFFFFFF` = 無し |
| 12 | 4 | u32 | `normalPathOff` | 同上 |
| 16 | 4 | u32 | `metalRoughPathOff` | 同上（G = roughness, B = metallic、`Material.h` と同じ） |
| 20 | 4 | u32 | `emissivePathOff` | 同上 |
| 24 | 4 | f32 | `metallic` | `Material::defaultMetallic` 相当 |
| 28 | 4 | f32 | `roughness` | 同 `defaultRoughness` |
| 32 | 12 | f32[3] | `emissiveColor` | |
| 44 | 4 | f32 | `emissiveIntensity` | |
| 48 | 4 | f32 | `alphaCutoff` | |
| 52 | 4 | f32 | `baseColorAlpha` | |
| 56 | 16 | f32[4] | `baseColorFactor` | 記録するが v1 のランタイムは無視（`ModelLoader` と同じ挙動。memory: baseColorFactor 無視の既知事項） |
| 72 | 16 | f32[4] | `uvScaleOffset` | xy = scale, zw = offset（既定 1,1,0,0） |
| 88 | 4 | u32 | `sectionKind` | 0 = VG / 1 = NONVG |
| 92 | 4 | u32 | reserved | |

テクスチャは BC7（albedo = sRGB、MR = linear）/ BC5（normal）の `.dds`（ミップ付き）。ランタイムは既存 `ResourceManager::GetOrLoadTexture` へパスを渡し（`TextureLoader.cpp:110` の DDS 直読み）、`AllocateBlock(kMaterialSrvBlockSize)` で 4 連続 SRV を作る。

### 3.5 階層（`NODES`、`HierNode` = 192 B = `HierChild` 48 B × 4）

`HierChild`（48 B）:

| offset | size | 型 | 名前 | 内容 |
|---:|---:|---|---|---|
| 0 | 16 | f32[4] | `cullSphere` | 部分木の全ジオメトリを包む球（アセット空間）。錐台 / HZB 用 |
| 16 | 16 | f32[4] | `lodSphere` | 部分木の `parentLodSphere` 群を内包する球。LOD 枝刈りの誤差投影用 |
| 32 | 4 | f32 | `minOwnError` | 部分木のメンバークラスタの `lodError` の最小 |
| 36 | 4 | f32 | `maxParentError` | 部分木の `parentLodError` の最大（`+INF = 0x7F800000`） |
| 40 | 4 | u32 | `ref` | `0xFFFFFFFF` = 空スロット / bit31 = 1: 葉（下位 31bit = グループ番号）/ bit31 = 0: 子ノード番号（アセット相対） |
| 44 | 4 | u32 | `groupPacked` | 葉のみ: `pageIndex[0:16)` \| `firstCluster[16:24)` \| `clusterCount[24:28)` \| reserved。葉でなければ 0 |

- ノード 0 が根。子の並びに意味は無い（空きは `ref = 0xFFFFFFFF`）。
- 葉が指す「グループ」= **そのグループに消費される（入力となる）クラスタの集合**（= 描画候補）。クラスタは同じページ内で連続（`firstCluster` から `clusterCount` 個、≤ 5）。

### 3.6 ページ

`PageTableEntry`（32 B、`PAGE_TABLE`）:

| offset | size | 型 | 内容 |
|---:|---:|---|---|
| 0 | 8 | u64 | `fileOffset`（ファイル先頭から。無圧縮なら `pagesBase + i * 131072`） |
| 8 | 4 | u32 | `storedSize`（ディスク上のバイト数。無圧縮なら 131072） |
| 12 | 4 | u32 | `rawSize`（131072） |
| 16 | 4 | u32 | `clusterCount` |
| 20 | 4 | u32 | `groupCount` |
| 24 | 4 | u32 | `levelMin[0:8)` \| `levelMax[8:16)` \| `flags[16:32)`（bit0 = pinned） |
| 28 | 4 | u32 | `priority`（cook 時の初期優先度。粗いほど小さい） |

`PAGE_DEPS`（CSR）: ページ `p` を常駐させる前に**常駐していなければならない**ページ = `deps[offsets[p] .. offsets[p+1])`（＝ p のグループのクラスタを消費する「1 つ粗いレベル」のグループを含むページ）。ルートページは依存なし。

ページ本体（131072 B、無圧縮時）:

```
[ 0 .. 63]    PageHeader (64 B)
                0  u32 magic = "VGPG" (0x47504756)
                4  u32 pageIndex
                8  u32 clusterCount
                12 u32 groupCount
                16 u32 clusterTableOffset = 64
                20 u32 payloadOffset       = 64 + 128 * clusterCount   (16 整列)
                24 u32 usedBytes           (以降 131072 まではゼロ詰め)
                28 u32 levelMin
                32 u32 flags
                36 u8[28] reserved
[64 .. ]      ClusterHeader[clusterCount]   (各 128 B, 16 整列)
[payloadOffset ..] クラスタごとに [頂点ブロック][三角形ブロック] を連続（各ブロックは 16 B 整列）
```

`ClusterHeader`（128 B、ページ先頭からのバイトオフセットで `vertexOffset` / `triangleOffset` を持つ）:

| offset | size | 型 | 名前 | 内容 |
|---:|---:|---|---|---|
| 0 | 16 | f32[4] | `lodSphere` | このクラスタを**生んだ**グループの LOD 球（LOD0 は `cullSphere` と同値） |
| 16 | 4 | f32 | `lodError` | 同グループの誤差（LOD0 = 0） |
| 20 | 4 | f32 | `parentLodError` | このクラスタを**消費した**グループの誤差（ルート = `+INF`） |
| 24 | 16 | f32[4] | `parentLodSphere` | 同グループの LOD 球（ルートは自分の `lodSphere`） |
| 40 | 16 | f32[4] | `cullSphere` | クラスタのジオメトリを包む球（`meshopt_computeMeshletBounds`） |
| 56 | 4 | u32 | `coneS8` | `cone_axis_s8[0..2]`（バイト 0..2）+ `cone_cutoff_s8`（バイト 3）。`/127` で復元 |
| 60 | 4 | u32 | `vertexOffset` | 頂点ブロックの位置（ページ先頭から、16 整列） |
| 64 | 4 | u32 | `triangleOffset` | 三角形ブロックの位置（同 16 整列、`= vertexOffset + vertexBlockSize`） |
| 68 | 4 | u32 | `packedCounts` | `vertexCount[0:8)` \| `triangleCount[8:16)` \| `materialIndex[16:32)` |
| 72 | 12 | i32[3] | `posMin` | 格子単位 |
| 84 | 4 | u32 | `posBits` | `bx[0:5)` \| `by[5:10)` \| `bz[10:15)`、上位 reserved |
| 88 | 8 | f32[2] | `uvBase` | |
| 96 | 8 | f32[2] | `uvScale` | |
| 104 | 4 | u32 | `childPage` | このクラスタを**生んだ**グループのページ（= 自分の子が居るページ）。LOD0 は `0xFFFFFFFF` |
| 108 | 4 | u32 | `flags` | bit0 = LOD0（葉）/ bit1 = ルート / `[8:16)` = レベル |
| 112 | 4 | f32 | `maxEdgeLength` | 最長辺（アセット空間）。SW/HW 振り分けの画面辺長推定に使う |
| 116 | 12 | u32[3] | reserved | 0 |

頂点ブロック（`vertexOffset` から。`vc = vertexCount`、`bpp = bx+by+bz`）:

```
posBytes  = align16( ceil(vc * bpp / 8) )
[vertexOffset]                          位置ストリーム : ビット詰め。頂点 i は bit オフセット i*bpp から x,y,z の順（各軸 bx,by,bz ビット、LSB ファースト）
[vertexOffset + posBytes]               法線ストリーム : u32[vc]   oct16: 下位 16bit = x(snorm16), 上位 16bit = y(snorm16)
[... + align16(4*vc)]                   UV ストリーム  : u32[vc]   下位 16bit = u(unorm16), 上位 16bit = v(unorm16)
vertexBlockSize = posBytes + 2 * align16(4*vc)
```

三角形ブロック（`triangleOffset` から。`tc = triangleCount`）: バイト `3*t + {0,1,2}` = 三角形 `t` の 3 頂点のローカル番号（0..127）。サイズ `align16(3*tc)`。巻き順は入力メッシュのまま。

GPU での読み出し（HLSL 擬似コード。`VgeoDecode.hlsli`、CPU 参照は `VgeoReader` と一致テスト）:

```
uint3 ReadTriangle(ByteAddressBuffer pool, uint pageBase, uint triOff, uint t) {
    uint b = pageBase + triOff + t * 3;             // 4 バイト境界をまたぐので 2 dword から取り出す
    uint2 w = pool.Load2(b & ~3u);
    uint  sh = (b & 3u) * 8;                        // 0 / 8 / 16 / 24
    uint  v  = (w.x >> sh) | ((sh != 0) ? (w.y << (32 - sh)) : 0);   // 下位 24bit が 3 バイト分
    return uint3(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
}
uint3 ReadQuantPos(ByteAddressBuffer pool, uint pageBase, uint vtxOff, uint i, uint3 bits) {
    uint bpp = bits.x + bits.y + bits.z;
    uint bit = i * bpp;                              // 最大 72bit → 3 dword 読み。軸ごとにシフト + マスク
    // ... (dword 境界をまたぐ抽出を 3 回)
}
```

### 3.7 `PROXY` / `NONVG` セクション

同一レイアウト。先頭 32 B の `ProxyHeader`（`u32 sectionCount, totalVertices, totalIndices`、残り reserved）の後に `ProxySectionEntry`（64 B）× `sectionCount`、その後にデータ:

| offset | size | 型 | 内容（`ProxySectionEntry`） |
|---:|---:|---|---|
| 0 | 4 | u32 | `materialIndex` |
| 4 | 4 | u32 | `vertexCount` |
| 8 | 4 | u32 | `indexCount` |
| 12 | 4 | u32 | `vertexOffset`（セクション先頭から、16 整列）。**`Mesh.h` の `Vertex`（96 B インターリーブ）そのまま** |
| 16 | 4 | u32 | `indexOffset`（u32 インデックス） |
| 20 | 4 | u32 | `flags`（bit0 = NONVG 由来の完全詳細） |
| 24 | 4 | f32 | `error`（LOD0 からの最大偏差。NONVG は 0） |
| 28 | 12 | f32[3] | `aabbMin` |
| 40 | 12 | f32[3] | `aabbMax` |
| 52 | 12 | u32[3] | reserved |

`Mesh::Initialize(device, vertices, indices, cmd)` にそのまま渡せる（F3 のとおり中で再最適化 + 自動 LOD が走る）。PROXY = VG の**代表**（RT / 影 / ピッキング / 物理 / VG 無効時の描画）、NONVG = MASK / BLEND セクションを予算内（既定 20 万 tri）へ簡略化した**実描画用**。

### 3.8 中間形式 `VGSRC`（P6 → cooker の契約）

UE 由来の 1 億 tri を glTF に通すと 4 GB 制限と速度で詰むので、単純な生配列を直接渡す。`u64` を使い、C# から `MemoryMappedFile` でストリーム書きできる形。

```
[ 0..63]  VgsrcHeader (64 B)
            0  char[4] "VGSR"      4 u16 major=1   6 u16 minor=0
            8  u32 flags           bit0 hasNormals / bit1 hasUV0 / bit2 hasTangents(無視) / bit3 hasColors(無視)
            12 u32 materialCount   16 u32 sectionCount   20 u32 reserved
            24 u64 vertexCount     32 u64 indexCount
            40 f32 unitScaleToMeters   44 u32 coordSystem (0 = エンジン: Y up LH m / 1 = UE: Z up LH cm)
            48 u64 sourceHash      56 u64 reserved
[16 整列] positions  f32[3] × vertexCount
[16 整列] normals    f32[3] × vertexCount        (hasNormals)
[16 整列] uv0        f32[2] × vertexCount        (hasUV0)
[16 整列] indices    u32 × indexCount            (三角形リスト)
[16 整列] SectionRecord × sectionCount           (16 B: u32 materialIndex, u32 firstIndex, u32 indexCount, u32 flags)
[16 整列] MaterialRecord × materialCount         (§3.4 と同じ 96 B。パスは同梱の文字列プール参照)
[16 整列] strings: u32 byteLength + UTF-8 bytes
```

UE → エンジン座標: UE の (X 前, Y 右, Z 上, 左手系, cm) から `(x, y, z)_UE → (y, z, x)_engine × 0.01`（**巡回置換なので手系・巻き順は変わらない**）。`coordSystem = 1` のとき cooker が変換する（C# 側で変換しても可、`coordSystem = 0` で出す）。

### 3.9 サイズの計算例

クラスタ 1 個（頂点 70、`bpp = 13+13+13 = 39`、三角形 110）:
`posBytes = align16(ceil(70*39/8) = 342) = 352` / 法線 `align16(280) = 288` / UV `288` → 頂点ブロック 928 B、三角形 `align16(330) = 336` B、ヘッダ 128 B → **1392 B / 110 tri ≈ 12.7 B/tri**。128 KiB ページ ≈ 90 クラスタ ≈ 9900 tri。

### 3.10 検証（ローダが必ずやる）

`magic` / `versionMajor` / `headerSize == 512` / `headerCrc32` / `pageSize == 131072` / `maxClusterVerts ≤ 128` / `maxClusterTris ≤ 128` / 各セクションのファイル範囲内 / `PAGE_TABLE` の `fileOffset + storedSize ≤ ファイルサイズ` / ノードの `ref` が範囲内 / ページ先頭の `magic`（読み込み時）/ 各クラスタの `vertexOffset + vertexBlockSize ≤ 131072`。1 つでも外れたらそのアセットを拒否（プロキシも出さず、エンティティは「読み込み失敗」と同じ扱い + ログ）。

### 3.11 GPU への直結（コピーだけで上がる対応）

| ファイルの領域 | 行き先 | 加工 |
|---|---|---|
| NODES | `VgNodes`（`StructuredBuffer<HierNode>`）の当該アセット区画 | なし（`memcpy` → UPLOAD → `CopyBufferRegion`） |
| PAGES（各ページ 131072 B） | `VgPagePool` のスロット | なし（圧縮時のみ CPU 展開） |
| PAGE_TABLE / PAGE_DEPS | CPU（ストリーマ） | — |
| MATERIALS | `VgMaterials`（`VgMaterialGpu`） | CPU で SRV ブロックを確保して詰め替え |
| PROXY / NONVG | 通常の `Mesh`（`Mesh::Initialize`） | なし |

---

## 4. 段階計画

### 4.1 一覧（1 行 / 段階）

工数は「1 人（または 1 エージェント）が集中した実働日」。**全段階共通のマージ条件**: ① VG 未使用シーンで `tests/golden/`（4 枚）が**ビット一致** ② `ctest` 全緑 ③ MCP のスキーマ変更は TS↔C++ ドリフトテスト（`tools/mcp-server/schemaDrift.test.ts`）と `docs/MCP.md` を同時更新 ④ D3D12 デバッグレイヤ警告 0。

| ID | 段階 | 依存 | 工数 | 合否基準（要約。機械判定） | 並列に振れる単位 |
|---|---|---|---:|---|---|
| **P0** | 仕様確定 + スタブ | なし | 3 | `VgeoFormatTests`（sizeof/offset・往復・破損 10 種拒否）緑。スタブ `.vgeo` を `VgeoReader` が読める | 形式 + reader / スタブ writer + テスト / HLSL デコード |
| **P1** | オフライン cooker | P0 | 10 | DAG 不変条件 0 違反・クラック 0・往復誤差・決定論（同入力 = 同バイト）・1000 万 tri ≤ 5 分・12.5M tri ×4 のベンチ生成 | DAG 構築 / ページ詰め + writer + BVH / ベンチ生成 + テクスチャ + プロキシ |
| **P2** | ランタイム読込 + GPU カリング（統計のみ） | P0（スタブで開始）、実アセットは P1 | 10 | GPU 選択集合 = CPU 参照、5000 万 tri で K0–K2 ≤ 1.5 ms、overflow 0、既定 OFF ビット一致 | ローダ + 設定/MCP / カリング HLSL + HiZ 外出し / CPU 参照 + テスト |
| **P3** | メッシュシェーダ描画 + 可視性バッファ | P2 | 9 | 深度が従来経路と一致（99.9% 画素 ≤ 2e-6）・穴 ≤ 0.01%・HW ラスタ ≤ 4 ms（5000 万 tri）・**実測ゲート（§6 R1）** | PSO/能力 / MS・PS + デバッグビュー / マイクロベンチ + 二相配線 |
| **P3b** | SW ラスタ（**条件付き**） | P3 + ゲート判定 | 7 | 微小三角形シーンで VG ラスタ −25% 以上かつ画像一致。届かなければ撤退 | SW カーネル / 合流 + 閾値調整 |
| **P4** | マテリアル resolve + 既存統合 | P3（+ P1 実アセット） | 15 | **5000 万 tri / 60 fps 合格（§5.2）**・LOD0 パリティ（§5.4）・既定 OFF ビット一致・機能組合せ耐性 | G-Buffer・速度 / resolve + 材質 + 微分 / 統合（DrawItem・影・RT・UI・MCP）/ ベンチ + golden |
| **P5** | ストリーミング | P4 | 12 | 予算 300 MB で 5000 万 tri が穴 0・スパイク 0・1 億 tri が 8 GB で 60 fps・依存違反 0 | CPU ストリーマ / GPU 要求 + フォールバック / 統計 + ベンチ |
| **P6** | UE アセット cook（C#） | P0（E2E 検証は P1 の CLI） | 10 | VGSRC 検証通過・Dead Mall 代表 10 アセットが `.vgeo` 経由で一致・Nanite 可否の結論 | スパイク / メッシュ + テクスチャ出力 / 材質 + 配置 |
| **P7** | 仮想シャドウマップ（VSM） | P4（P5 推奨） | 30 | 既定 OFF ビット一致・影テクセル密度 ≈ 1/px・追加コスト ≤ 2.5 ms（静止 ≤ 1.0 ms）・アクネ / ペーターパン 0 | 基盤 / サンプリング差替え / 非 VG 描画 / VG 描画 |
| **P8** | GI（DDGI 延長） | P4（DDGI 側は先行可） | 18 | 既定 OFF ビット一致・閉じた部屋の漏れ ≤ 現行の 50%・200 m 四方でカスケード追従（フリッカ ≤ 1%）・≤ 1.5 ms | カスケード / リロケーション + 分類 / 検証 |
| **P9** | TSR / DoF / Bloom | なし（VG 併用検証のみ P4） | 14 | TSR: PSNR +2 dB・≤ 1.2 ms・静止安定 / DoF・Bloom: golden + エネルギー保存 | TSR / DoF / Bloom |
| **P10** | エディタ / MCP / 配布統合 | P4（P5 で完成） | 6 | ctest + 診断緑、配布ビルドで `.vgeo` を読んで起動 | 配布 / エディタ UI / 診断 |

合計 約 144 実働日。クリティカルパスは P0(3) → P2(10) → P3(9) → P4(15) = **37 日**（ここで最初の合格判定）、続けて P5(12) = 49 日、VSM まで行くと P7(30) で 79 日。P1・P6・P9・P10 と P8 は別レーンで並行できるので、3〜4 レーンなら暦で **約 8〜9 週で P4 合格**、約 4 か月で P7 まで。

依存図:

```
P0 ─┬─→ P1 ────────────┐
    ├─→ P2 ──→ P3 ──┬→ P3b(条件付き)
    │        (スタブ→実) │
    │                 └→ P4 ──┬→ P5 ─→ (P7 の VG 側)
    └─→ P6 (E2E は P1)        ├→ P7 (VSM)
                              ├→ P8 (GI)
   P9 (TSR/DoF/Bloom) ── 独立 ┴→ P10
```

### 4.2 各段階の詳細

#### P0 仕様確定（3 日）— P1 と P6 を並列化するための契約

- **(a) 成果物**: `src/vgeo/VgeoFormat.h`（§3 の POD + `static_assert`）/ `VgsrcFormat.h` / `VgeoReader`（ヘッダ検証・ページ / クラスタのデコード → 三角形リスト）/ 最小 `VgeoWriter` / スタブ cooker（`vgeocook --stub`: **LOD0 のみ**・BVH 1 ノード・全クラスタが `parentLodError = +INF` の正しい `.vgeo` を球 / トーラス / glb から出す。これで P2 が実 cooker を待たずに始められる）/ `shaders/vg/VgeoDecode.hlsli`（dxc コンパイルが通るところまで）/ `docs/VGEO_FORMAT.md`（§3 を規範文書化）/ ゴールデン `tests/data/vgeo/`（単一レベル 1 個 + 手書き 3 レベルの最小 DAG 1 個）。
- **(b) 依存**: なし。
- **(c) 工数**: 3 日。
- **(d) 合否**: `VgeoFormatTests`（`tests/CMakeLists.txt` の既存パターン）: 全構造体のサイズ / オフセット、書き出し→読み出しで三角形集合が入力と一致（位置誤差 ≤ `posStep/2`、UV ≤ `uvScale/65535`、法線角 ≤ 0.05°）、破損ファイル 10 種（CRC 不一致 / 未知 major / セクション範囲外 / ノード `ref` 範囲外 / `maxClusterVerts=129` …）を全部拒否。
- **(e) 並列**: 3 分割（形式 + reader / スタブ writer + テスト / HLSL デコード）。ただし契約なので**決め切るのは 1 人**、他はレビュー役。

#### P1 オフライン cooker（10 日）

- **(a) 成果物**: cook ライブラリ（§2.3.1 の全ステップ）、`vgeocook.exe`（`in.glb out.vgeo [--proxy-tris N] [--threads N]` / `--gen-bench rock|plane --tris N --seed S`）、ページ詰め・量子化・BVH・プロキシ・NONVG・BC7/BC5 の `.dds` 生成（オフライン圧縮）、`VgeoCookTests`、**巨大ベンチメッシュ生成**（fBm 変位した細分割球 1250 万 tri × 4 種、UV・PBR テクスチャ付き。`vg_bench_50m` の素材）。
- **(b) 依存**: P0。
- **(c) 工数**: 10 日（DAG 4 / ページ・量子化・writer 2 / プロキシ・NONVG・テクスチャ 2 / ベンチ生成・テスト・チューニング 2）。
- **(d) 合否**（`VgeoCookTests` + `tools/bench/vg_cook_bench.mjs`）:
  1. §2.3.2 の不変条件 3 点が全メッシュ・全レベルで 0 違反。
  2. **クラック検査**: 球 / トーラス / 箱 / 開いた平面 / UV シーム付きキューブの 5 種 × 距離 64 点で、選んだカットの全エッジが「2 回ちょうど（元の境界を除く）」。
  3. 構造: ルートクラスタ ≤ 2、平均充填率 ≥ 85%（≥ 109 tri/クラスタ）、レベル数 ≈ log2(クラスタ数) ± 2。
  4. サイズ: ファイル / LOD0 三角形数 ≤ 30 B（[推定] 25 B）。
  5. 決定論: 同一入力・同一パラメータで出力が**バイト一致**（スレッド数 1 と 20 でも一致するよう、並列は結果の順序に影響させない）。
  6. 速度: 1000 万 tri ≤ 5 分（20 コア）、実測値をレポートに保存（1 億 tri の外挿を §6 R6 に反映）。
  7. 1250 万 tri × 4 のベンチ生成が成功し `VgeoReader` の検証を通る。
- **(e) 並列**: (i) DAG（グループ化・ロック・簡略化・誤差・LOD 球）/ (ii) 量子化・ページ詰め・BVH・writer / (iii) ベンチ生成・テクスチャ変換・プロキシ。(i) と (ii) は P0 の構造体で結合。

#### P2 ランタイム読み込み + GPU カリング（統計のみ、10 日）

- **(a) 成果物**: `VgeoLoader`（検証 → **全ページ常駐**アップロード → プロキシ `Mesh` / `Material` 生成、`GetOrLoadModel` の `.vgeo` 分岐、`CachedModel::vg`）、`VirtualGeometrySystem`（§2.4.1 のバッファ、インスタンス表）、`VirtualGeometrySettings`（シーン JSON 保存、既定 OFF）+ MCP `dx12_set_virtual_geometry` / `dx12_get_virtual_geometry` + `perf_stats` の `virtualGeometry` ブロック（`enabled` / `instances` / `sourceTrisInFrustum` / `visibleClusters` / `nodesVisited` / `levelHistogram[]` / `overflow` / `vramMB` / `phase2Clusters`）+ `GpuTimer` スコープ + `HiZCommon.hlsli` 外出し + K0/K1/K2（cs_6_6）+ `m_vgHiZ`（二相の HZB）+ `dx12_vg_dump`（小シーン用に選択クラスタ ID 一覧を返す）+ CPU 参照 `VgCullReference` / `VgLodMath.h`。**描かない**。
- **(b) 依存**: P0（スタブ `.vgeo`）。実アセットでの検証は P1 の出力が出来次第。
- **(c) 工数**: 10 日。
- **(d) 合否**:
  1. 既定 OFF でゴールデン 4 枚ビット一致、`HiZCull.hlsl` の外出し後の DXIL がバイナリ同値（または既存の Hi-Z 単体テスト緑）。
  2. **GPU の選択クラスタ集合 = CPU 参照**（小シーン 3 視点 × τ 3 種で完全一致。浮動小数の境界で ≤ 0.1% の差は許容し記録）。
  3. 5000 万 tri ベンチが全常駐でロードでき、K0–K2 の GPU 時間 ≤ 1.5 ms（1080p・5060）、`overflow = 0`。
  4. 遮蔽物を置くと `visibleClusters` が単調に減る（偽陰性の検証は P3 の画像一致で行う）。
  5. `VgLodMath` のテスト（単調性: 親の射影誤差 ≥ 子）緑。
- **(e) 並列**: (i) ローダ・バッファ・設定 / MCP / 統計 / (ii) K0–K2 HLSL + `HiZCommon` 外出し / (iii) CPU 参照 + `VgLodMath` + テスト。

#### P3 メッシュシェーダ描画 + 可視性バッファ（9 日）

- **(a) 成果物**: `GraphicsDevice::SupportsMeshShaders()` + `DX12_DISABLE_MESHSHADER=1`（`DX12_DISABLE_DXR` と同じ流儀の縮退検証スイッチ）、`MeshPipelineStateBuilder`（PSO ストリーム）、`VgRaster_MS/PS`（§2.4.3）、`ExecuteIndirect(DispatchMesh)`、可視性バッファ + 既存深度への描画（H1、`useDepthPrepass |= useVg`）、二相目のラスタ、HW/SW の 2 リスト配線（SW = 0）、`dx12_render_debug` の VG モード（`vgCluster` / `vgLod` / `vgTri` / `vgOverdraw` / `vgCoverage`）、安定コンパクション（決定論モード）、**マイクロベンチ**（三角形サイズ 1 / 4 / 16 / 64 px 別のスループットを MS 128/128 と 64/124 で測る `tools/bench/vg_raster_micro.mjs`）。
- **(b) 依存**: P2。
- **(c) 工数**: 9 日。
- **(d) 合否**:
  1. **深度一致**: τ = 0（LOD0 固定）で、従来経路で描いた同一ジオメトリ（`dragon_7m.glb` を cook）と深度が 99.9% の画素で NDC 差 ≤ 2e-6。
  2. **穴**: τ = 0.25 / 1 / 4 × 3 距離で `vgCoverage`（プロキシのシルエット内で可視性が空の画素率）≤ 0.01%。
  3. **決定論**: 安定コンパクション ON で同一フレームを 2 回撮って bitExact。
  4. **性能**: 5000 万 tri ベンチの HW ラスタ GPU ≤ 4 ms（超えたら §6 R1 のゲートへ）。
  5. 三角形サイズ別スループット表を保存（P3b の要否判断に使う）。
  6. `DX12_DISABLE_MESHSHADER=1` で起動しても VG エンティティがプロキシで正しく描かれる（絵が崩れない）。
- **(e) 並列**: (i) PSO ビルダ + 能力判定 + シグネチャ / (ii) MS・PS HLSL + デバッグビュー / (iii) マイクロベンチ + 二相目配線。

#### P3b SW ラスタ（7 日・条件付き）

- **(a) 成果物**: `VgRasterSW.hlsl`（64bit アトミック UAV）、合流パス（`SV_Depth` 書き込みの全画面 PS）、`T_sw` の調整、`OPTIONS9` 能力ゲート。
- **(b) 依存**: P3 と**ゲート判定**（§6 R1: 近距離ベンチで HW ラスタが 4 ms を超える、または小三角形域のスループットが 64/124 でも不足）。
- **(c) 工数**: 7 日。
- **(d) 合否**: 小三角形シーン（τ を上げず近距離 close-up）で VG ラスタ時間が HW のみ比 −25% 以上、HW のみとの画像一致（境界の穴 ≤ 0.01%、深度差 ≤ 1e-5）。**−25% に届かなければコードを残さず撤退**。
- **(e) 並列**: (i) SW カーネル / (ii) 合流パス + 統合 + 閾値調整。

#### P4 マテリアル resolve + 既存パイプライン統合（15 日）— 最初の合格判定

- **(a) 成果物**: `VgGBuffer_PS`（H2）/ `VgResolve_PS`（H3）/ `m_rootSignatureVg` + `RootSignature::Initialize(extraFlags)` / `VgMaterials` / 解析微分（`PBR.hlsli` / `DecalApply.hlsli` の外出し）/ `DrawItem::vg` + `IsVirtualGeometryItem` + prepass・フォワードの skip / 影（CSM・RT）のプロキシ確認 / インスタンス表の tint・emissive / `VirtualGeometrySettings` の UI（Inspector + ライティング窓）/ MCP / `docs/AUTHORING.md` の VG 節 / `tests/golden/` に VG 用エントリ追加 / **`tools/bench/vg_bench.mjs`**（§5.2）。
- **(b) 依存**: P3（+ P1 の実アセット）。
- **(c) 工数**: 15 日（G-Buffer 2 / resolve + 材質 + 微分 5 / 統合 5 / ベンチ + golden + 組合せ耐性 3）。
- **(d) 合否**:
  1. **5000 万 tri / 60 fps の機械判定合格**（§5.2 の表）。
  2. **LOD0 パリティ**（§5.4）: 画素差 > 2/255 の割合 ≤ 0.5%、平均絶対差 ≤ 0.5/255（初期値。実測後に固定）。
  3. 既定 OFF でゴールデン 4 枚ビット一致、`ctest` 全緑。
  4. TAA / SSAO / コンタクト / SSR / SSGI / RT 影 / RT-AO / DDGI / デカール / フォグの各組合せ（少なくとも 2^4 の代表 16 通り）でクラッシュ・デバッグレイヤ警告・全黒 / 全白が 0（`dx12_quality_gate` / `dx12_diagnose` の `render_health` を利用）。
  5. τ = 1 と τ = 0 の絵の PSNR ≥ 40 dB（LOD の目立つポップ無し）。
- **(e) 並列**: (i) G-Buffer・速度 / (ii) resolve PS + 材質 + 微分外出し / (iii) 統合（DrawItem・skip・影・RT・UI・MCP）/ (iv) ベンチ + golden + 組合せ検査。

#### P5 ストリーミング（12 日）

- **(a) 成果物**: ページプール + `VgPageTable` の GPU 更新、要求ビットマップ + 優先度 + READBACK、IO ワーカー（OVERLAPPED `ReadFile`）、依存順ロード、LRU 退避、`IDXGIAdapter3::QueryVideoMemoryInfo` による予算、`childrenResident` フォールバックの有効化、ルートページ pinned、統計（`residentPages` / `pendingRequests` / `streamedMB` / `holePixels`）、`.vgeo` のループ配置対応、ストリーミング ON/OFF（OFF = 全常駐）。
- **(b) 依存**: P4。
- **(c) 工数**: 12 日。
- **(d) 合否**:
  1. 予算を 300 MB に絞った 5000 万 tri ベンチで VRAM ≤ 予算 + 5%、静止 2 秒後の絵が全常駐と 0.1% 以内（穴 0）。
  2. カメラパス 600 フレーム中の穴画素率の最大 ≤ 0.05%。
  3. ストリーミング中の frameMs p99 ≤ 平均 × 1.5、33 ms 超のスパイク 0 回。
  4. 1 億 tri（cook 済み約 2.5 GB）のベンチが 8 GB VRAM・予算 1 GB で §5.2 と同じ基準（60 fps）を満たす。
  5. 依存違反（親未ロードで子をロード）= 0（統計カウンタ）。
  6. 退避 → 再ロードの往復で絵が決定論。
- **(e) 並列**: (i) CPU ストリーマ（IO / LRU / 依存）/ (ii) GPU 側（要求・フォールバック・ページ表）/ (iii) 統計・MCP・ベンチ。

#### P6 UE アセット cook（C#、10 日）

- **(a) 成果物**: `tools/ue-cook/`（net10 / CUE4Parse。**Dead Mall 抽出で使った環境をそのまま使う**: `C:\Users\ryuto\.dotnet10`、`EGame.GAME_UE5_6`、jmap の `.usmap`。memory `dreamcore-dead-mall`）。`.pak` / `.utoc` → `UStaticMesh`（LOD0 / Nanite）→ **VGSRC**（座標変換 UE→エンジン）、テクスチャは UE がcook 済みの BC7 / BC5 / BC1 を**再圧縮せず DDS へパススルー**、マテリアルは基本 PBR 4 スロットへ写像、配置は `WorldDto` → シーン JSON（Dead Mall 抽出の延長）。**冒頭 3 日のスパイク**: Nanite cook 済みメッシュのフルレゾ三角形を CUE4Parse で復元できるか（cook 済み UE パッケージには通常 LOD のフォールバック低ポリと Nanite ストリーミングページしか無い。CUE4Parse に Nanite ページのデコーダがあるか、UE5.6 のページ形式変更に追随できるかを調べる）。
  - 縮退案: (A) Nanite 化されていないメッシュ + Nanite のフォールバックメッシュだけを対象にする（低忠実度）/ (B) `C:\Users\ryuto\Documents\Unreal Projects` 等の**エディタ側**でソースメッシュを FBX / OBJ にエクスポート（Python）→ assimp 経路。
- **(b) 依存**: P0（VGSRC 契約）。E2E 検証は P1 の CLI。
- **(c) 工数**: 10 日（スパイク 3 + 実装 7）。
- **(d) 合否**:
  1. 出力 VGSRC が `VgsrcReader` の検証（範囲 / サイズ / 三角形数）を通る。
  2. Dead Mall の代表 10 アセット（高ポリ順）が `.vgeo` 経由で描画され、従来の glTF 経路と画素差（§5.4 と同基準）で一致し、法線が外向き・スケールが正しい（座標変換の巻き順 / スケール誤りが無い）。
  3. Nanite メッシュ 5 個: デコード成功、または縮退案の採用を根拠つきで文書化（スパイクの結論）。
  4. UE 側の三角形数と VGSRC の三角形数が一致（デコード欠損 0）。変換スループットを記録。
- **(e) 並列**: (i) スパイク / (ii) メッシュ + テクスチャ出力 / (iii) マテリアル + 配置 → シーン JSON。3 つは独立。

#### P7 仮想シャドウマップ（30 日）

- **(a) 成果物**: **ソフトウェア方式の仮想テクスチャ**（Tiled / Sparse Resources は却下済みなので不使用）。物理ページプール（例 8192² `R32_FLOAT` = 256 MB、ページ 128²）+ ページ表テクスチャ（ライトごとのクリップマップ、平行光は 12〜16 段）+ 画素からのページ要求（深度 + G-Buffer から必要ページを算出する compute）+ GPU 側のページ割り当て・静止ページのキャッシュ再利用 + **非 VG メッシュ**（既存の影 PSO をページ矩形ごとに）+ **VG クラスタ**（ページごとの HZB カリング → ラスタ。P2 のカリングを「ビュー = ページ」で再利用）+ サンプリング（**slot 4 のテーブルへ `t25` / `t26` をレンジ追加で相乗り**、DWORD 増分 0。Forward / ForwardSkinned / Terrain / Grid / VgResolve の影関数を差し替え）+ 既定 OFF（CSM は温存）。
  - 内部分割: **P7a**（基盤 + 非 VG + サンプリング、12 日）/ **P7b**（VG 統合 + キャッシュ・無効化、18 日）。
- **(b) 依存**: P4（VG 影の本命）。P5 推奨（ページ要求の IO・GPU 側割り当ての知見が共通）。
- **(c) 工数**: 30 日。
- **(d) 合否**: ① 既定 OFF でビット一致 ② 影テクセル密度が画面 1 px あたり 1 テクセル前後（誤差 2 倍以内）の画素が 95% 以上（デバッグビューの統計）③ 1080p・5060 で VSM 追加コスト ≤ 2.5 ms（静止時 ≤ 1.0 ms）④ CSM 比でアクネ・ペーターパン 0（ゴールデン + 目視）⑤ VG 本体の細部影がプロキシ影より正しく出る（差分画像）⑥ ページプール溢れ時の劣化が段階的（真っ黒 / 穴 0）。
- **(e) 並列**: 基盤（ページ表・プール・要求）/ サンプリング差替え / 非 VG 描画 / VG 描画。

#### P8 GI — DDGI の延長（18 日）

- **(a) 成果物**: DDGI は段階 0 / 1 / 1.5 / 2 実装済み、3-a（多重バウンス）配線済みで既定 OFF、3-b は却下（F17）。残りを延長する: ① **カスケード / カメラ追従の入れ子格子**（2〜3 段、段ごとに間隔 2 倍。`DdgiVolume` の `kMaxProbes = 4096` を段ごとに持つ）② **プローブのリロケーション + 分類**（壁内 / 空虚プローブの無効化＝ライトリーク抑制）③ **VG 併用の検証と補正**（TLAS のプロキシ vs 実描画の GI 差、ヒット点の材質 = プロキシ `Material` の baseColor）④ 粗い鏡面（任意: SSR + プローブの prefiltered）。「Lumen 風」の最小実現 = 大規模屋外まで届く DDGI カスケード + SSGI + RT-AO の役割分担（Lumen のスクリーントレース優先 → world-space プローブと同じ分担、F12 のコメント）。3-b の再設計（密閉部屋の環境項）は本計画に含めない。
- **(b) 依存**: P4（VG 込みの絵で検証）。DDGI 側は VG 非依存で先行できる。
- **(c) 工数**: 18 日。
- **(d) 合否**: ① 既定 OFF でビット一致 ② 「閉じた部屋」シーンで外部光の漏れ（室内平均輝度）≤ 現行の 50% ③ 屋外 200 m 四方で 3 段カスケードがカメラ追従し、カメラ移動中の平均輝度フリッカ（隣接フレーム差）≤ 1% ④ 1080p・5060 で ≤ 1.5 ms ⑤ VG ON / OFF（proxy）で GI の画面平均輝度差 ≤ 3%。
- **(e) 並列**: カスケード / リロケーション + 分類 / 検証。

#### P9 TSR / DoF / Bloom（14 日）

- **(a) 成果物**: **TSR** = `TaaPass`（`TaaPass.cpp` 363 行）をレンダー解像度 → 表示解像度のアップスケール版に拡張（Halton ジッタ列、履歴の Catmull-Rom 再サンプル、輝度重み、速度 / 深度によるリジェクション。既存の「レンダー解像度と表示解像度の分離」を利用。DirectSR は却下済みなので自前）/ **DoF**（`DofPass.cpp` 244 行 → ボケ形状・前景 / 背景分離・ゴースト抑制）/ **Bloom**（`BloomPass.cpp` 229 行 → 物理ベース多段ダウン / アップ、しきい値なし、レンズダート対応）。全て既定 OFF または既存 UI 互換。
- **(b) 依存**: なし（既存の TAA と速度バッファ）。VG との併用検証だけ P4。
- **(c) 工数**: 14 日（TSR 8 + DoF 3 + Bloom 3）。
- **(d) 合否**: TSR: 表示 1080p を render scale 0.67 で描いた絵の PSNR が「既存 TAA + バイリニア」比 +2 dB 以上（基準 = ネイティブ 16x SS、Sponza + dragon シーン）、静止 8 フレームの差分 ≤ 0.3/255、GPU ≤ 1.2 ms。DoF: 焦点面 ±0.1 m が鮮明（エッジ幅 ≤ 1 px）、CoC が連続（ゴールデン）。Bloom: トーンマップ前の総エネルギー ≤ 元 +5%、ちらつき無し。全て既定 OFF でビット一致。
- **(e) 並列**: TSR / DoF / Bloom の 3 並列。

#### P10 エディタ / MCP / 配布統合（6 日）

- **(a) 成果物**: `BuildGame` が `.vgeo`（ループ）を同梱、Inspector の VG バッジ + 統計、取り込み UI（`.glb` を `.vgeo` へ cook するボタン = `vgeocook.exe` 呼び出し）、`dx12_import_asset` の `.vgeo` / VGSRC 対応、`DeepDiagnostics` に `virtual_geometry` 検査（メッシュシェーダ非対応 / 設定 OFF なのに `.vgeo` がある / ページ予算 / overflow）、UI 自動テスト。
- **(b) 依存**: P4（完成は P5 後）。
- **(c) 工数**: 6 日。
- **(d) 合否**: `ctest` + 診断（現行 14 種 + 1）緑、配布ビルドで `.vgeo` を読んで起動。
- **(e) 並列**: 配布 / エディタ UI / 診断。

---

## 5. 検証（RTX 5060 で 5000 万 tri / 60 fps を機械判定する）

### 5.1 3 層構成

| 層 | 何を | どこで | GPU |
|---|---|---|---|
| 純関数 / データ | フォーマット・cook 不変条件・クラック・LOD 数式・CPU 参照カリング | `ctest`（`tests/*_test.cpp`、既存と同じ流儀） | 不要 |
| GPU 統合 | 選択集合の一致・ラスタの深度一致・穴・決定論・ベンチ | `tools/bench/*.mjs`（既存の `lib/engine.mjs` でヘッドレス起動 → MCP） | 5060 |
| 目視補助 | LOD 色・クラスタ色・オーバードロー・穴の可視化 | `dx12_render_debug` の VG モード（`vgCluster` / `vgLod` / `vgTri` / `vgOverdraw` / `vgCoverage`） | 5060 |

### 5.2 5000 万 tri / 60 fps ベンチ（`tools/bench/vg_bench.mjs`）

**素材の生成**（決定論・再現可能）:

```
node tools/bench/gen_vg_scene.mjs
  1) vgeocook --gen-bench rock --tris 12500000 --seed 1..4 --out <PerfTest>/assets/vg/bench_rock_{1..4}.vgeo
       fBm 変位した細分割球 + 球面 UV + PBR テクスチャ 1 組（PerfTest の hp_rock 系 2k を流用）。1 アセット ≈ 310 MB（25 B/LOD0 tri の見積もり）
  2) シーン JSON を <PerfTest>/assets/scenes/ へ出力（memory dx12-perf-tools: シーン JSON を直接吐くのが最速。MCP で 1 体ずつ spawn しない）
       vg_bench_50m   : アセット 4 種を 1 体ずつ + アセット 1 をもう 1 体 = 5 インスタンス（錐台内ソース三角形の合計 62.5M、ユニーク 50M）。
                        リング配置 + 地面(通常 plane) + 太陽(CSM) + IBL スカイ + クラスタライト 32 灯
       vg_bench_shared: 1 アセット × 50 インスタンス（データ共有。ストレス用・非ゲート）
  3) 固定カメラ 3 姿勢を tests/vg/bench_poses.json へ（overview = 全部視野内 / mid / close）
```

**実行**（`scale_ladder.mjs` と同じく MCP 接続は 1 本を使い回す。単一クライアントの罠あり）:

1. `open_project(PerfTest)` → `open_scene(vg_bench_50m)` → `dx12_set_virtual_geometry {enabled:true, lodPixelError:1.0, streaming:false}`。
2. **受け入れプロファイル**: レンダー解像度 1920×1080（`get_render_scale` / `perf_stats` で寸法を確認）、TAA ON、CSM 4×2048（プロキシが落とす）、SSAO ON、コンタクトシャドウ ON、クラスタライト 32 灯、IBL。RT 影 / DDGI は「フルプロファイル」として**別に測って報告のみ**（ゲートにしない）。
3. 姿勢ごとに `step_frames 180`（TAA / HZB 履歴の収束）→ `pendingRequests == 0` を待つ → `dx12_benchmark {frames:600, uncap:true}` → `dx12_perf_stats {window:240}`。**3 回繰り返して中央値**で判定（他プロセスによる揺れ対策）。
4. `build/vg-bench/result.json` に全数値、終了コード 0 / 1。

**合否表**（overview 姿勢がゲート。mid / close は穴・overflow だけゲート、時間は報告）:

| 指標 | 取得元 | 合格 |
|---|---|---|
| 平均 fps | `benchmark.fps` | ≥ 60.0 |
| 1% low fps | `benchmark.fps1PercentLow` | ≥ 50 |
| frameMs p95 | `perf_stats.frameMs` | ≤ 18.5 ms |
| GPU 合計 | `gpuPassMs.total` | ≤ 15.5 ms |
| VG の GPU 合計 | `gpuPassMs.vgCull + vgRaster + vgGBuffer + vgShade` | ≤ 10.0 ms |
| CPU 作業時間 | `cpu.workMs` | ≤ 5 ms |
| 錐台内ソース三角形（LOD0 換算） | `virtualGeometry.sourceTrisInFrustum` | ≥ 50,000,000 かつ ≤ 80,000,000（「5000 万級」を静かに 10 倍にしないため上限も見る） |
| 実際に描いた三角形 | `virtualGeometry.drawnTris` | 0.3M 〜 12M（LOD が効いているかの健全性） |
| キュー溢れ | `virtualGeometry.overflow` | 0 |
| 穴 | `vgCoverage` の穴画素率 | ≤ 0.01% |
| VRAM（全常駐時） | `virtualGeometry.vramMB` | ≤ 1500 |
| デバッグレイヤ警告 / エラー | `dx12_get_log` | 0 |

`fps1PercentLow`（p99 フレーム時間の逆数）は既存 `dx12_benchmark` が返す（`tools/mcp-server/index.ts:1322`）。**新しい MCP 引数 / ブロックは MCP セッションを再接続するまで出ない**（memory `dx12-perf-tools` の罠）ので、ハーネスはエンジンへ直接 TCP で話す `lib/engine.mjs` 経路を使う。

### 5.3 正しさの機械テスト

| テスト | 内容 | 置き場 |
|---|---|---|
| cook 不変条件 | 単調性・球の内包・グループ内一致（§2.3.2） | `VgeoCookTests` |
| **クラック検査** | 距離スイープでカットを取り、量子化座標の溶接 ID で全エッジが 2 回（境界除く） | `VgeoCookTests` |
| LOD 数式 | 親の射影誤差 ≥ 子（HLSL 版との一致は P2 の GPU テスト） | `vg_lod_math_test` |
| **GPU = CPU 参照** | 同じ視点で GPU が選んだクラスタ集合 = `VgCullReference` | `tools/bench/vg_verify.mjs`（`dx12_vg_dump`） |
| 深度一致 | VG(τ = 0) と従来経路の深度差（§4.2 P3） | `vg_verify.mjs` |
| 穴 | `vgCoverage`（プロキシシルエット内で可視性が空の画素率） | `vg_bench.mjs` |
| 決定論 | 安定コンパクションで 2 回撮影 bitExact | `golden.mjs --repeat 2` |
| ストリーミング | 依存違反 0・穴の最大値・スパイク | `vg_bench.mjs`（P5 以降） |

### 5.4 既存経路との絵の一致（パリティ）

`hp_rock`（40 万 tri・PBR 2k、法線マップあり）を `.vgeo` へ cook し、同じカメラ・同じ決定論設定（`screenshot_final {deterministic:true, settleFrames:8}`）で「従来 glb（VG OFF）」と「`.vgeo`（VG ON、τ = 0）」を撮って比較する。

- 合格（初期値、P4 で実測して固定）: 画素差 > 2/255 の割合 ≤ 0.5%、平均絶対差 ≤ 0.5/255。
- 既知の差（許容差に含める）: tangent（UV 勾配から作る cotangent frame）による法線マップの微差、頂点法線の oct16 量子化。
- バリアント: デカール ON / `colorTint` / emissive 上書き / 屋内（envMap 空）/ SSGI ON。
- `Forward.hlsl` を変更した場合は、このパリティを必ず再実行する（PS がコピー方式なのでドリフトの検知はここが唯一）。`Forward.hlsl` 冒頭に注記を追加（P4）。

### 5.5 既定 OFF の回帰

- `tests/golden/`（`perftest_ddgi_probe.png` / `perftest_stress_5000.png` / `nocturne_hall.png` / `junction_f6.png`）が VG 未使用でビット一致（`golden.mjs` の `bitExact`）。
- `.vgeo` を置いたシーンで VG OFF → プロキシが従来経路で描かれ、`golden.mjs` に「VG OFF + `.vgeo` あり」のエントリを追加（P4）。
- 新規 GPU リソースが VG OFF で 0 個であること（`dx12_diagnose` の SRV ヒープ使用量が導入前後で同一）。

---

## 6. リスクと撤退条件

| ID | リスク | 検知 | 対策 | 撤退条件 |
|---|---|---|---|---|
| **R1** | **5060 実機でのメッシュシェーダ小三角形スループット不足**（HW ラスタは 2×2 クアッド単位で小三角形に弱い。MS 128/128 が 64/126 より遅い可能性もある） | P3 のマイクロベンチ（1/4/16/64 px）と 5000 万 tri ベンチの HW ラスタ時間 | ① 64 頂点 / 124 三角形で再 cook して A/B（+1 日）② それでも 5000 万 tri で HW ラスタ > 4 ms、または close-up で > 6 ms なら **P3b（SW ラスタ）を必須化**し `T_sw` を 18 → 32〜64 px へ ③ SW 込みでも > 5 ms なら τ を 1.5〜2 に緩める、または render scale 0.67 + TSR（P9）で 60 fps を確保 | 上記全部でも VG 合計が受け入れ表（§5.2）に届かない場合は **VG を採用せず P3 で止める**（投資は P0〜P3 = 約 31 日が上限。プロキシ描画は残るので現状比で退行しない） |
| R2 | resolve（全画面バインドレス PS）が 5 ms 超 | `gpuPassMs.vgShade` | 非 VG 画素の早期 return、遠距離のミップ固定、デカール / コンタクト等の分岐削減、タイル分類 | render scale + TSR（P9）で吸収。それでも駄目ならライティング機能の一部を VG 側で簡略（設定で選択） |
| R3 | UE の Nanite cook 済みアセットをフルレゾで取れない | P6 冒頭スパイク | 縮退案 A（フォールバック + 非 Nanite）/ B（UE エディタ側でエクスポート → assimp 経路） | 縮退案 A の忠実度でも要件を満たさないなら、UE 資産は「ユーザーが所有するエディタプロジェクト経由」に限定（要ユーザー判断） |
| R4 | LOD 境界のクラック / 穴 | §5.3 のクラック検査・`vgCoverage` | §2.3.2 の 5 点 + 機械テスト。迷ったら親を描く（保守側） | 特定アセットで再現するなら `maxLevelBias`（LOD0 側へ固定）で運用 |
| R5 | VRAM（8 GB）不足 | `virtualGeometry.vramMB` / DXGI budget | P5 のストリーミング、全常駐の上限を設定、予算超過時は自動でストリーミングへ | — |
| R6 | cook の時間 / メモリ（1 億 tri で 1 時間 / 8 GB 超） | P1 の 1000 万 tri 実測の外挿 | ワーカー並列、属性の精度を落とした作業配列、v1.1 で空間タイル分割 cook | 1 億 tri が 3 時間を超えるなら、ベンチ規模の上限を 5000 万 tri に据え置き（要件は満たす） |
| R7 | 二相 HZB の偽陰性で物が消える | 画像一致テスト・CPU 参照との差 | 迷ったら可視（`HiZMath.h` の保守原則）、`hzb=off` の切替、二相目で必ず再判定 | 再現が止まらなければ VG カリングは HZB なし（フラスタム + LOD のみ）で出荷し、遮蔽は既存のオブジェクト単位 Hi-Z に任せる |
| R8 | golden の揺れ（可視リストの順序非決定） | `golden.mjs --repeat 2` | §2.4.7 の安定コンパクション | — |
| R9 | resolve PS と Forward PS のコピーのドリフト | §5.4 パリティ | パリティを CI（`golden.mjs`）に常設、`Forward.hlsl` 冒頭に注記 | — |
| R10 | RS が 2 個になることによる乖離 | ユニットテスト | 生成は 1 つの実装（`extraFlags` 引数）から。2 個のシリアライズ結果が flags 以外一致することをテスト | — |
| R11 | プロキシと本体の差（RT 影 / GI の自己遮蔽・漏れ） | `rtDiff`（プロキシ vs ラスタ深度差の可視化、既存）| `proxyError` をバイアス下限に、プロキシの品質を P1 で数値化 | RT 影が破綻するなら VG では CSM（プロキシ）+ コンタクトのみで運用し P7（VSM）を前倒し |
| R12 | `.vgeo` を pak に入れられない（保護要件、F7） | — | v1 はループ配置。保護が必要なら pak v2（範囲読み + ブロック暗号）を別計画にし、先に `PakArchive::readAt` の排他バグ（memory ⑥）を直す | — |
| R13 | 植生（MASK）が支配的なシーンで見劣り | UE 資産の棚卸し（P6） | NONVG の予算を上げる / P3 後に HW ラスタの PS アルファテストを再評価 | 仕様（対象外） |
| R14 | 他 GPU（AMD / Intel）で未検証 | — | 能力判定 + 縮退のみ。開発機は 5060 のみと明記 | — |
| R15 | ドライバ依存（MS Tier / `DispatchMesh` の ExecuteIndirect / 64bit アトミック） | 起動ログ（`DXR:` 行と同じ流儀で `VG:` 行を出す） | 最低ドライババージョンをログとドキュメントに記載 | — |
| R16 | 見積もりの不確実性（P4 が最大） | 各段階の実績 | 段階ごとに合否ゲート。P3 の実測で以後の工数を再見積もり | — |

**却下した案（再調査しない）**: ① Work Graphs（Agility SDK が要る。このエンジンは意図的に使っていない）/ ② persistent threads を最初から（D3D12 に前進保証が無い。P2b で計測してから）/ ③ 増幅シェーダでのカリング（compute + `ExecuteIndirect` の方が二相・SW 振り分けと相性がよい）/ ④ 64bit 可視性バッファを HW ラスタで（HW に 64bit アトミックは無い）/ ⑤ フルの G-Buffer デファード（フォワード PS の MRT 化禁止の方針）/ ⑥ UE の Nanite ページをそのまま流用（形式が UE 内部仕様でバージョン依存。デコード → 再 cook）/ ⑦ 頂点位置を動かす簡略化（`WithUpdate`）を v1 で（格子上の点が保たれる利点を優先。v1.1 で評価）。

---

## 7. 未決事項（ユーザーに聞くべき点・5 件）

1. **VG の対象 GPU と縮退方針**
   メッシュシェーダ Tier 1 未満の GPU は VG を使わず、プロキシ（最大 25 万 tri）を従来経路で描く縮退でよいか。
   → **推奨: それでよい**。古い GPU 向けの compute 経路（+10 日）は作らない。プロキシの忠実度は cook パラメータ（`--proxy-tris`）で調整できる。

2. **`.vgeo` の配布形態と保護**
   `.vgeo` は数百 MB〜数 GB になり、現行の pak（全体読み・`readAt` に排他無し、F7）は不向き。v1 は**ループ配置（暗号化 / 改ざん防止なし）**でよいか。配布物に UE 由来の巨大資産を含める予定はあるか。
   → **推奨: v1 はループ配置**。保護が要るなら pak v2（範囲読み + ブロック暗号）を別計画にする（先に `PakArchive` の排他バグを直す）。

3. **マテリアル境界の扱い（v1 = 1 クラスタ 1 マテリアル）**
   マテリアル境界の頂点は全 LOD で固定され、マテリアル数が多いアセット（目安: 1 メッシュ 50 超）は粗い LOD の三角形数が下がりにくい。まずこの形で始めてよいか。
   → **推奨: v1 で始める**。P6 で実資産の cook レポート（ロック頂点率）を見てから、マルチマテリアルクラスタ（v2、ヘッダに予約済み）の要否を判断する。

4. **VG 対象外（MASK / 半透明 / スキンド / カスタムシェーダ）の割り切り**
   植生・柵・ガラス・キャラは従来経路（NONVG は 20 万 tri に簡略化）になる。UE の対象シーンに植生が支配的なものがあるか。
   → **推奨: v1 はこの境界で確定**。植生が主役のシーンが出てきた時点で、HW ラスタ限定の MASK 対応（PS アルファテスト）を P3 後の拡張として再評価する。

5. **VG の影の暫定運用（プロキシ経由）と VSM の優先度**
   P4〜P6 の間、VG の影はプロキシ（CSM / RT 影）が落とす。近距離の影精度が要る用途なら、VSM（P7、30 日）を P5 の前に持ってくる手もある。
   → **推奨: 暫定運用で先に VG を使い始め、VSM は P5 の後**。RT 影が使えるシーンではプロキシ影の精度で足りる公算が高い。

---

## 付録 A. 参照した主な実コード（本書の根拠）

`src/renderer/Mesh.h`（`:15-24,53,204-206`）/ `Mesh.cpp`（`:57-65,67-104,138-195`）/ `DrawItem.h`（`:21-65,117-121`）/ `RaytracingScene.h` / `HiZPass.h` / `HiZMath.h` / `OcclusionCullPass.h` / `RenderPass.h` / `ViewPasses.cpp`（`:109-142,383-414`）/ `DdgiVolume.h` / `GpuTimer.h`、`src/core/ApplicationRender.cpp`（`:80-470,612,1417,4411,4766-4976,5291-5349,5620`）/ `ApplicationPipeline.cpp`（`:98-104,585-622,717-728`）/ `ApplicationInternal.h`（`:199,204`）/ `Application.h:1039`、`src/graphics/RootSignature.cpp`（`:13-23,329,353`）/ `PipelineState.h` / `GraphicsDevice.{h,cpp}`、`src/resource/ModelLoader.cpp`（`:662-678,1060,1287`）/ `ResourceManager.{h,cpp}` / `TextureLoader.cpp`、`src/core/vfs/PakArchive.h`、`shaders/forward/{Forward.hlsl,Lighting.hlsli,PBR.hlsli,DecalApply.hlsli}` / `shaders/velocity/VelocityCommon.hlsli` / `shaders/hiz/*` / `shaders/raytracing/RtBindless.hlsli`、`tools/bench/{golden,scale_ladder}.mjs` / `tools/mcp-server/index.ts`、`build/release/vcpkg_installed/x64-windows/include/meshoptimizer.h`（1.0 の API 確認）。memory: `dx12-realism-roadmap` / `dx12-ddgi` / `dx12-perf-tools` / `dx12-load-optimization` / `dreamcore-dead-mall`。

## 付録 B. 参考文献（記憶ベース。URL は未確認）

- Karis et al., "A Deep Dive into Nanite Virtualized Geometry", SIGGRAPH 2021（クラスタ DAG・親誤差・二相 HZB・SW / HW ラスタの切替）
- Burns & Hunter, "The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading", JCGT 2013 / Schied & Dachsbacher, "Deferred Attribute Interpolation for Memory-Efficient Deferred Shading", HPG 2015（重心座標・解析微分）
- meshoptimizer README / `meshopt_simplify` 系のドキュメント、リポジトリ `demo/clusterlod.h`（参考実装）
- Schüler, "Followup: Normal Mapping Without Precomputed Tangents"（cotangent frame）
- Majercik et al., "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields", JCGT 2019（既存 DDGI の出典、`dx12-ddgi`）
