# マテリアルグラフ G2a: フォワードのシェーディング尾部の外出し + メイン RS のバインドレスフラグ

- 対象: Uno Engine（ブランチ `feat_develop`）
- 上位の設計: `docs/MATERIAL_GRAPH_DESIGN.md`（§1 F1 / §3.4 / §3.5 / §3.6 / §7 G2a / §7.4）、G1 の契約: `docs/MATGRAPH_FORMAT.md`
- 性格: **見た目を 1 ピクセルも変えないリファクタ + G2b（グラフ → フォワード接続）の基盤作り**。グラフ材質はまだ描画に出てこない。
- 検証の結果（DXIL 12/12 同一・決定論スクショ 9/9 ビット一致・A/B・デバッグレイヤ）は §6。G2b への申し送りは §7。

---

## 1. 何を作ったか

| 領域 | 変更 |
|---|---|
| シェーダー | `shaders/forward/UnoSurface.hlsli`（契約構造体 `UnoSurface` / `UnoMatInput` / `UnoSurfaceDefault()` / `UnoNormalFromTangentSpace()`）・`ForwardShade.hlsli`（シェーディング尾部）を新設。`Forward.hlsl` / `ForwardSkinned.hlsl` / `Terrain.hlsl` の PS 尾部（3 か所の複製）を `ForwardShade.hlsli` へ集約 |
| ルートシグネチャ | メイン RS に `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`（SM 6.6 のバインドレス）を立てる（`SupportsDynamicResources()` = SM 6.6 以上 + Resource Binding Tier 3 のとき。非対応 GPU は従来どおり）。**ルート DWORD は 62/64 のまま**。静的サンプラ s6〜s8 を追加（DWORD 0）。`RootSignature::IsBindless()` |
| ResourceManager | 永続 SRV 添字の取得 API（`GetOrLoadTextureSrvIndex` / `FindTextureSrvIndex`）と点検（`AuditTextureSrvIndices` / `SnapshotTextureSrvIndices`） |
| エディタ（範囲外の 1 件） | `ModelThumbnailRenderer::RenderOne` の「RS を Set → SRV ヒープを Set」を逆順にした（フラグ付き RS はヒープを先に Set する規約。§4）。他のエージェントが同じファイルを編集中なので、マージ時にこの 2 か所の入れ替えが残っているか確認すること |
| 登録表 / CMake | `ShaderRegistry.cpp` の `staticDeps` と `CMakeLists.txt` の `SHADER_FWD_INC` に `ForwardShade.hlsli` / `UnoSurface.hlsli` を追加 |
| ツール | `tools/shader_dxil_equiv.ps1`（DXIL 同値の検証）、`tools/engine_instance.ps1` に `-BuildDir`（ビルド出力のスナップショットから起動。A/B 検証用。既定は従来どおり） |
| テスト | `ForwardShadeTests`（ラベル `dxc`）・`MainRsBindlessTests`（ラベル `gpu`）・`tests/data/forward_shade_probe.hlsl` |

---

## 2. `ForwardShade.hlsli` の API

```hlsl
struct UnoShadeInput { float3 worldPos; float3 worldNormal; float2 svPos; float viewDepth; };
UnoShadeInput UnoMakeShadeInput(float3 worldPos, float3 worldNormal, float4 positionSV, float viewDepth);

float3 UnoShadeLighting(UnoSurface s, float3 normalWS, UnoShadeInput si);   // デカール → 法線フィルタ → 影 → 直接光 → 環境光(+デカールの自己発光)
float3 UnoShadeFinish (float3 color, UnoShadeInput si);                       // デバッグ可視化 →(LDR)
float4 UnoShadeForward(UnoSurface s, float3 normalWS, UnoShadeInput si);      // = Lighting + s.emissive → Finish。alpha = s.opacity。★グラフ材質はこれ
```

- `UnoSurface` は G1 の参照実装（`src/renderer/matgraph/hlsl/UnoMatContractRef.hlsli`）と**同名・同型・同順**。`tests/forward_shade_test.cpp` が構造体本体を機械的に突き合わせる。
- 尾部が使うのは `baseColor / metallic / roughness / ao`（`UnoShadeForward` はさらに `emissive / opacity`）。`normalTS / hasNormal` と予約欄（Subsurface / ClearCoat / …）は**読まない**＝法線は呼び出し側が解決して `normalWS` で渡す。グラフ材質は `UnoNormalFromTangentSpace(worldNormal, worldTangent, tangentW, s.normalTS)`（`PerturbNormal` と同じ z ≥ 0.35 ガード。GPU で `PerturbNormal` と数値一致を確認済み）で作る。旧経路の `PerturbNormal` は無改造。
- ラフネスの下限 0.04 は尾部の中（`UNO_SHADE_ROUGHNESS`）。`ao` は SSAO に**掛ける**（`ao *= s.ao`。既定 1.0 = 恒等）。
- **リソース宣言は各シェーダーの責務**（t/s の番号は `RootSignature.cpp` が正。宣言順を変えると DXIL のリソース ID が動く）。include の前に `Lighting.hlsli` / `g_shadowMap(t4)` + `ShadowPcss.hlsli` / IBL(t5-7, s2, s3) / `g_ssao(t8)` / `g_contactShadow(t11)` / `g_ssr(t16)` / `g_ssgi(t17)` / `DecalApply.hlsli` があること。`SelectCascade` / `SampleCascade` / `CalcShadow` も `ForwardShade.hlsli` に移した。動く実例は `tests/data/forward_shade_probe.hlsl`（ForwardGraph.hlsl の予行）。
- 差し替え点（include の前に `#define`）: `UNO_SHADE_SANITIZE(x)`（SSR / SSGI / DDGI の Inf 対策。地形が `TerrSanitize` を指す）、`UNO_SHADE_ROUGHNESS(r)`（地形は `clamp(r, 0.04, 1)`）、`UNO_SHADE_ROUGHNESS_BEFORE_NORMAL`（局所変数の宣言順を地形の旧 PSMain に合わせる。DXIL 同値のためだけ）。
- 3 段に分けた理由: 旧 PSMain は自己発光を**ライティングの後ろで**サンプルしていた。`UnoShadeForward`（emissive を先に評価して渡す）にすると命令順が動く。旧経路は `UnoShadeLighting` → 自分で emissive を足す → `UnoShadeFinish` を直接呼んで命令列を 1 命令も動かさない。alpha を `Finish` の引数にしないのも同じ理由（評価位置が上へ動く）。
- `ForwardInstanced.hlsl`（VS のみ・PS は `Forward_PS` 共用）、`ForwardGrid.hlsl`（エディタのグリッド。尾部を持たない）は無変更。

---

## 3. メイン RS のバインドレスフラグ

- `RootSignature::Initialize` が `device.SupportsDynamicResources()` の真のとき `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` を立てる。フラグ付きで直列化に失敗したら従来の RS へ縮退する（`RtScreenPass` と同じ流儀）。ログ: `RootSignature created (PBR: 14 slots, 62/64 DWORD, bindless=on|off)`。非対応 GPU では警告を 1 行出す。
- 環境変数 `DX12_DISABLE_MAIN_BINDLESS=1` でフラグを立てない（A/B 計測・縮退経路のテスト用）。
- 静的サンプラ s6〜s8（LINEAR WRAP / ANISO(8) CLAMP / POINT WRAP）を追加。G1 の `UNO_SAMP_*` との対応: `ANISO_WRAP = s0`・`LINEAR_CLAMP = s2`・`POINT_CLAMP = s4`（既存。`Forward.hlsl` は s2 を `g_iblSampler`、s4 を `g_ssaoSampler` として宣言済みなので、ForwardGraph のマクロはそれらの変数を指す）・`LINEAR_WRAP = s6`・`ANISO_CLAMP = s7`・`POINT_WRAP = s8`。
- 非対応 GPU（SM 6.6 未満 / Tier 3 未満）: フラグを立てない。G2b 以降の「バインドレスでテクスチャを引く機能」だけが無効になる（`RootSignature::IsBindless()` で判定）。既存の描画は従来どおり。RTX 5060（SM 6.8 / Tier 3）は対応。

---

## 4. フラグ付き RS を使うときの規約

**フラグ付きのメイン RS を Set する前に、必ず `SetDescriptorHeaps` を呼ぶ**（`CommandList::SetDescriptorHeap` → `SetRootSignature` の順）。逆だとデバッグレイヤが `id=1419`（`SetGraphicsRootSignature: SetDescriptorHeaps must be called ... before setting a root signature with ...HEAP_DIRECTLY_INDEXED`）を出す。

メイン RS を Set している箇所は 6 か所: `ApplicationRender.cpp` の 2 か所と `ViewPasses.cpp` の 3 か所は満たしていた。`ModelThumbnailRenderer::RenderOne` だけが逆順で、フラグを立てるとサムネイル 1 枚ごとに `id=1419` が出ていた（デバッグレイヤ + エディタ `--background` で確認）ので入れ替えた。以後メイン RS を新しく Set する所は同じ順にすること。

---

## 5. 永続 SRV 添字（G2b の前提）

`ResourceManager::GetOrLoadTextureSrvIndex(path, cmdList, srgb, usage, maxDimension)` / `FindTextureSrvIndex(...)` が返すのは、シェーダー可視 CBV/SRV/UAV ヒープの先頭からの番号 = HLSL の `ResourceDescriptorHeap[番号]`（`Texture::GetSrvIndex()` と同値）。2D 単枚だけが対象（cube / 配列 / 失敗 = `kInvalidSrvIndex`）。

保証（コードを読んで確認 + `MainRsBindlessTests` が実 GPU で機械的に見張る）:

1. 読み込んだテクスチャは `AllocateIndex` した添字を**プロセス終了まで**持つ（キャッシュから追い出さない・`Free` しない）。
2. ホットリロード（`ReloadChangedAssets`）は同じ `Texture` の中身を差し替えるだけで添字を変えない（`AdoptFrom` + 同じスロットへ `CreateSRV`）。失敗キャッシュ（nullptr）だった枠に初めて実体が入るときだけ新しい添字を払い出す（以後は不変）。
3. ヒープは起動時に 1 度作るだけ（65,536 個。拡張も再作成もしない）。

キャッシュキーは `GetOrLoadTexture` と同じ（パス文字列 + sRGB + usage + 最大寸法）。**パスの区切り文字・大小は正規化されない**ので、グラフ側のテクスチャパラメータは一貫した表記で渡すこと。

---

## 6. 検証

### 6.1 DXIL 同値（`tools/shader_dxil_equiv.ps1`）

```powershell
# 変更前の shaders フォルダ（git stash / 別ワークツリー / コピー）を -ShaderRoot に渡して撮る
pwsh tools/shader_dxil_equiv.ps1 -Snapshot $env:TEMP\dxil_before -ShaderRoot C:\path\to\shaders_before
pwsh tools/shader_dxil_equiv.ps1 -Snapshot $env:TEMP\dxil_after
pwsh tools/shader_dxil_equiv.ps1 -Compare $env:TEMP\dxil_before,$env:TEMP\dxil_after   # 0 = 全部同一 / 3 = 順序のみ / 1 = 差あり
```

対象 12 本: `Forward` の VS・PS・LDR_OUTPUT・ALPHA_TEST、`ForwardSkinned` の VS・PS・ALPHA_TEST、`Terrain` の VS・PS、`ForwardInstanced_VS`、`ForwardGrid` の VS・PS。DXC で `-Fc` の逆アセンブリを取り、`shader hash` の行を落とす / インライン展開で付く内部定数名の接尾辞（`@tint.i.0.hca` → `@tint.0.hca`）を落とす / 末尾の `declare` 群を並べ替える、の正規化のあとで行単位に比べる。ゴールデンの DXIL は生成物なので git には入れない。

**結果: 12 本すべて「同一」**（正規化後の命令列・cbuffer レイアウト・リソース束縛が 1 行も違わない。命令の多重集合の比較も併用）。PS のコンテナハッシュだけは変わる。原因は、関数へ外出ししたことで `tint` 配列（カスケード可視化の色定数）の内部名が `@tint.0.hca` → `@tint.i.0.hca` になることと、末尾の `declare` の並びのみ（命令列ではない）。VS 4 本と `ForwardInstanced` / `ForwardGrid` はハッシュも一致。なお `Forward_PS` と `ForwardSkinned_PS` は外出し前から**同一の DXIL**だった（＝複製は完全に同じ内容だった）。

同値にするために効いた 3 点（外すと DXIL の並びが動く。値は変わらない）: ① 自己発光のサンプルをライティングの後ろに置く ② alpha（`albedo.a * opacity`）を最後に付ける ③ 局所変数の宣言順（デカールのループの phi の並びが決まる）。

### 6.2 絵の同値（決定論スクショ）

手順（同一マシン・同一設定）:

1. 変更前（HEAD）と変更後（HEAD + G2a）を**同じ場所から**ビルドする。他のエージェントの作業ツリーは壊れていることがあるので、使い捨ての `git worktree`（HEAD）へ変更を写して別ビルドにするのが確実（`src/core/generated/` は gitignore なので本体からコピーする。vcpkg は `-DVCPKG_INSTALLED_DIR` で本体のものを共有）。
2. それぞれのビルド出力（exe + DLL + `shaders\*.cso`）を別フォルダへコピーしてスナップショットにする。
3. `pwsh tools/engine_instance.ps1 -Name <名前> -Port <ポート> -Project <使い捨てプロジェクト> -Mode headless -Refresh -BuildDir <スナップショット>` で起動。**シーンごとにエンジンを起動し直し、起動の 9 秒後から繋ぐ**（起動直後の最初の接続は応答されないことがある）。
4. `open_scene` → `set_editor_camera` → `step_frames`（90 フレーム）→ `screenshot_final {deterministic:true, gizmos:false, settleFrames:8, path:…}`。エディタ UI は写らない。
5. 基準を**同じ exe で 2 回**撮って一致することを先に確かめ（決定論）、変更後の PNG とピクセル（RGBA 全チャンネル）を比べる。

使い捨てプロジェクトは手元のテスト用プロジェクトの資産（公開サンプルモデル・HDRI・地形レイヤー等）をコピーして作る（実プロジェクトは読み取りだけ）。

| シーン | 何を踏むか | ピクセルの SHA-256（先頭 16 桁）| 結果 |
|---|---|---|---|
| `indoor_skinned` | 屋内の箱 + スキンド（CesiumMan）+ 手続きスカイの IBL | `fb77ccbae8f00f74` | ビット一致 |
| `indoor_skinned_allfx` | 同上 + SSAO / SSGI / SSR / コンタクトシャドウ / DDGI を ON | `1f9020dc2d8b93fc` | ビット一致 |
| `sponza_ibl` | Sponza（ALPHA_TEST の葉）+ 実 HDRI の IBL（`hasIBL=1`） | `7b9885b31edb23ab` | ビット一致 |
| `sponza_noibl` | 同上の IBL なし（`hasIBL=0` = 従来 ambient 経路） | `0969d446d0bc8a53` | ビット一致 |
| `terrain` | 地形 4 レイヤー + トライプラナー + マクロ + IBL（`Terrain_PS`） | `2212299164701fc1` | ビット一致 |
| `outdoor_fox` | 屋外 + スキンド（Fox）+ スクリーン空間効果 | `dde7e5ee3b678a15` | ビット一致 |
| `quality_pbr_ibl` | PBR ヘルメット・チェス駒・ハイポリ + 実 HDRI | `6d029dc58d9a9d91` | ビット一致 |
| `studio_lumen` | プロダクトショット（スポット / ポイントライト + IBL） | `e7995a92098bf7a1` | ビット一致 |
| `synth_translucent_custom` | 合成: 金属 × 粗さの球 / 自己発光 / BLEND / MASK / カスタムシェーダー（水テンプレート） | `d1916fcd4bcc9c36` | ビット一致 |
| `vfx_showcase`（参考） | ポイント / スポットライト + パーティクル | — | **決定論ではない**（同じ exe で撮り直しても 77〜79% の画素が変わる。平均絶対差 2.8〜3.5 / 255）。証拠には使わない |

HEAD ビルド 2 回・G2a（フラグ ON）・G2a（フラグ OFF = `DX12_DISABLE_MAIN_BINDLESS=1`）の 4 通りで、上の 9 シーンは**全部同じ画素**（`vfx_showcase` 以外）。エンジンログのエラー / 警告の数と内容も HEAD と同じ（既存の「マテリアルプレビューシェーダーのコンパイルに失敗」等のみ）。

### 6.3 メイン RS フラグの副作用の A/B（`dx12_benchmark`）

- 同じ exe（G2a）を `DX12_DISABLE_MAIN_BINDLESS=1`（OFF）と既定（ON）で**交互に**起動して比べる。`benchmark {frames:600, uncap:true}`、ヘッドレス（既定の表示矩形）、RTX 5060。シーンごとに起動し直し、90 フレームのウォームアップ後に計測。
- 使ったシーン: `lowpoly50k`（50,000 体・インスタンシング OFF = **23,655 ドロー** / ドローコール律速）、`hipoly1000`（1,000 体・GPU 律速）、`sponza`（516 ドロー）、`cityblocks`（1,616 ドロー）、`ddgiprobe`（31 ドロー・軽い）。
- ★このマシンは他のエージェントがビルド等で常時使っているため、**CPU 時間は実行ごとに ±10〜60% 揺れる**（標準偏差が大きい）。判定は GPU パス時間（タイマ分解能 0.01 ms）と、CPU は中央値で見る。

GPU パス時間の平均（ms。フォワード = `mainScene`、全体 = `total`）:

| シーン | n(OFF/ON) | mainScene OFF → ON | total OFF → ON |
|---|---:|---|---|
| ddgiprobe（31 ドロー） | 11/11 | 0.100 → 0.100（±0%） | 0.250 → 0.256（+2.6%。`clusterCull` compute の +0.002 ms と丸め。フォワードは同値） |
| sponza（516） | 9/9 | 0.408 → 0.406（−0.5%） | 0.602 → 0.601（−0.2%） |
| hipoly1000（GPU 律速） | 9/9 | 3.054 → 3.061（+0.2%） | 3.918 → 3.928（+0.3%） |
| lowpoly50k（23,655 ドロー） | 9/9 | 1.039 → 1.037（−0.2%） | 1.643 → 1.639（−0.3%） |

CPU（フレーム時間の中央値。ms。ON が小さいほど速い）: `lowpoly50k` 17.22 → 16.39（−4.8%）/ `hipoly1000` 4.14 → 3.80（−8.2%）/ `sponza` 1.95 → 1.61（−17%。揺れ）/ `cityblocks` 2.24 → 2.23（−0.4%）/ `ddgiprobe` 1.74 → 1.86（+6.9%。1.7 ms の軽いシーンで、エディタ UI が 1.6 ms を占める。OFF の 11 回は 1.55〜4.66、ON の 11 回は 1.59〜4.61 で分布が重なる＝揺れの範囲）。

**判定**: フラグを立てても、フォワード（`mainScene`）の GPU 時間は 4 シーンとも ±0.5% 以内で、ドロー数の最も多い `lowpoly50k`（ドローごとのドライバ負荷が出るならここ）でも CPU は劣化しない。**> 2% の劣化は検出されなかった**ので、別 RS 案（設計書 R2 の撤退案）へは切り替えない。メイン RS 自体にフラグを立てる（設計書 §3.6 の方針）で確定。

### 6.4 デバッグレイヤ（`DX12_D3D_DEBUG=1`）

- ヘッドレスで 10 シーン（フラグ ON / OFF）: D3D12 メッセージは**同数・同内容**（既存の `id=1328`「バッファの InitialState を無視」の警告 981 件のみ。エラー 0）。
- エディタ（`--background`）で 5 シーンを開いて Play → Stop、アセットブラウザでサムネイル生成: 修正前は ON でだけ `id=1419` が 10 件（サムネイル 1 枚ごと。§4）。`ModelThumbnailRenderer` を直したあとは ON / OFF とも `id=1023`（サムネイル描画で t10 に 2D ダミーを張っている）58 件・`id=1001`（静的ディスクリプタの更新）6〜8 件・`id=1328` の警告のみ。**この 3 種は既存**（フラグ OFF でも同数出る。今回は触っていない）。
- フラグ無しの RS でバインドレスのシェーダーの PSO を作ると `id=690`（`Shader uses resource descriptor heap indexing, but root signature is missing the ... flag`）で作成時に弾かれる（`MainRsBindlessTests` の対照）。

### 6.5 テスト

- `ctest -R ForwardShadeTests`（GPU 不要・約 2 秒）: 契約構造体の一致 / 尾部の複製が復活していない / `ShaderRegistry` の全バリアント（97 本）+ ALPHA_TEST 派生 + G2b 予行 probe（`tests/data/forward_shade_probe.hlsl` = ps_6_6 + `ResourceDescriptorHeap[]` + `UnoShadeForward`）が DXC を通る。`dxcompiler.dll` が無い環境では DXC の検査だけ SKIP。
- `ctest -R MainRsBindlessTests`（実 GPU・約 2 秒）: フラグ / DWORD 62 / 静的サンプラ 9 個、既存のフォワード系 PSO（Forward・半透明ブレンド・LDR・ALPHA_TEST・Skinned・Terrain）・カスタムシェーダー（水テンプレート）・probe の PSO がフラグ付きメイン RS で作れる（縮退 RS でも Forward が作れる）、SRV 添字の一意性 / ホットリロード前後の不変 / 実 GPU で `ResourceDescriptorHeap[添字]` を引くとそのテクスチャの色（書き換え後は新しい色）、`UnoNormalFromTangentSpace` = `PerturbNormal`（512 点の最大差 0）。GPU が無い / SM 6.6 + Tier 3 が無い環境は SKIP（0 で終わる）。`DX12_D3D_DEBUG=1` で実行するとデバッグレイヤの指摘がコンソールに出る。
- `ctest -R ShaderIncludeDepsTests`: `staticDeps` の漏れ（`ForwardShade.hlsli` / `UnoSurface.hlsli`）を見張る。
- DXIL 同値は変更前ツリーが要るので ctest 外（上のスクリプト）。

---

## 7. G2b への申し送り

1. **`ForwardGraph.hlsl`** は `tests/data/forward_shade_probe.hlsl` を手本にする（宣言・include の順・b2 の読み替え・`ResourceDescriptorHeap[]`・`UnoNormalFromTangentSpace` → `UnoShadeForward`）。cbuffer は ForwardGraph 側に固定し、生成物には書かせない。
2. **b2 の読み替え**（9 DWORD）: probe は `{recordBase, poolSrvIndex, graphFlags, packedTint, uvScaleOffset(float4), packedEmissive}`。影 / 速度の MASK パスが b2 の先頭 2 DWORD（metallic / roughness）を読む点に注意（設計書 R3）。
3. **サンプラのマクロ**: `UNO_SAMP_ANISO_WRAP` → `g_sampler`(s0)、`UNO_SAMP_LINEAR_CLAMP` → `g_iblSampler`(s2)、`UNO_SAMP_POINT_CLAMP` → `g_ssaoSampler`(s4)、新規 `UNO_SAMP_LINEAR_WRAP` = s6 / `UNO_SAMP_ANISO_CLAMP` = s7 / `UNO_SAMP_POINT_WRAP` = s8（同じ register を別名で二重宣言できない）。
4. **`EnsureGraphPso` は `m_rootSignature->IsBindless()` を確認する**。偽ならグラフ材質は使えない（既定材質へ縮退 + 警告）。`// @sm 6_6` の `ShaderManager` 対応は G2b。
5. **テクスチャの添字**: `ResourceManager::GetOrLoadTextureSrvIndex`（§5）。パラメータプール自体（`StructuredBuffer<float4>`）も同じシェーダー可視ヒープに SRV を 1 つ確保して、その添字を b2 で渡す（3 フレーム複製なら 3 つ）。
6. 生成 HLSL が `include` する `ForwardGraph.hlsl` を `ShaderRegistry` の `staticDeps` に入れる（`ShaderIncludeDepsTests` が漏れを見張る）。
7. `Forward.hlsl` 系の PS を直すときは `tools/shader_dxil_equiv.ps1` で「同一」のままか確認する（上の 3 点を崩さない）。

---

## 8. 踏んだ罠

- **DXC は関数へ外出しすると命令列が動く**: 自己発光のサンプル位置 / alpha の評価位置 / 局所変数の宣言順（phi の並び）。値は変わらないが DXIL のテキストが変わるので、`UnoShadeLighting` / `UnoShadeFinish` の 2 段 API にして旧経路の順序を保った。関数化で内部定数名にも `.i` が付く（ツールが正規化する）。
- **フラグ付き RS は `SetDescriptorHeaps` が先**。1 か所（サムネイル）だけ逆順で、エディタ + デバッグレイヤで初めて見つかった（ヘッドレスでは通らない経路）。
- `build\release` は他のエージェントの作業途中のファイルで頻繁に壊れる。変更前後の exe / DLL / `.cso` を丸ごとコピーしたスナップショット + `tools/engine_instance.ps1 -BuildDir` で撮ると影響を受けない。
- 単独のテスト exe を `Window` 込みで作るには `Input` ライブラリも要る（`Core` の `Window.cpp` が `InputSystem` を参照する）。WIC（DirectXTex）で読むテストは `CoInitializeEx` が要る。`WIN32_LEAN_AND_MEAN` の下で `dxcapi.h` を使うには先に `d3d12.h` を include する。
- 起動した直後のエンジンへ**最初に繋ぐタイミング**で ping が返らないことがある（起動から 9 秒待つと安定）。1 接続で複数シーンを渡り歩くと `new_scene` 後に TDR になった例もあるので、決定論スクショはシーンごとに起動し直す。

---

## 9. 未確認

- RTX 5060（SM 6.8 / Tier 3）以外の GPU。非対応 GPU の縮退は `DX12_DISABLE_MAIN_BINDLESS=1` の実機テスト（ヘッドレス 10 シーン + エディタ）と `MainRsBindlessTests`（縮退 RS で Forward の PSO が作れる）で代用した。
- ゲームモード（`GameRuntime.exe` / pak 起動）でのフラグ付き RS。エディタの Play → Stop まで。
- A/B の CPU 側は、マシンの負荷が大きく揺れて有意差の検出力が低い（GPU 時間は安定）。静かな環境での再計測は未実施。
- `dx12_perf_stats` の per-pass GPU 時間は 0.01 ms 刻み。軽いシーンの 1% 未満の差は分解できない。
