# マテリアルグラフ（ノードベースのマテリアルエディタ）設計書

- 対象: 自作 DX12 エンジン「Uno Engine」（`C:\Users\ryuto\Documents\dx12` / ブランチ `feat_develop`）
- 作成: 2026-09-30 / 状態: **設計のみ（エンジンのソースは 1 行も変更していない。ビルド・エディタ起動・git 操作もしていない）**
- 基準機: RTX 5060（8 GB）/ 1080p / 60 fps（`docs/VIRTUAL_GEOMETRY_DESIGN.md` と同じ）
- 表記: **[事実]** = 実コードを読んで確認（`ファイル:行` 付き）/ **[設計]** = 本書の決定 / **[推定]** = 未計測の見積もり（G2c 等の実測で差し替える）/ **[外部]** = Web で確認した外部情報 / **[記憶]** = 記憶ベースで未確認（着手時に必ず裏取りする）
- 関連: `docs/VIRTUAL_GEOMETRY_DESIGN.md`（以下「VG 設計書」）。本書 §3.11 と §7.4 で整合を取っている。

---

## 0. 要約（1 画面）

**やること**: UE のマテリアルエディタ相当（ノード + ワイヤのグラフ、リアルタイムプレビュー、パラメータ / インスタンス、マテリアル関数、コメント、検索パレット、Undo/Redo、エラー表示）を、**既存のカスタムシェーダー機構（実行時 DXC + ホットリロード + PSO キャッシュ）の上に**載せる。グラフは「HLSL を吐く」だけで、レンダラー本体はほぼ触らない。

**推奨案（決定 6 点）**

1. **ノードエディタは自前（ImDrawList）**。imnodes は vcpkg に無くズーム未対応 [外部]、imgui-node-editor は vcpkg ポート 0.9.3 が ImGui 1.92 向けに 3 本のパッチ当て（保守が薄い）[事実]。Uno 独自の見た目（ネオングロー、型別ピン形状、テーマトークン追従）と Undo・テスト用フックを全部こちらで握るには自前が最短（約 10 日 [推定]）。理由の詳細は §2。
2. **グラフ → HLSL → 既存カスタムシェーダー契約**。生成物は `assets/shaders/_graph/<guid>.hlsl`（`VSMain`/`PSMain` を持つ 1 ファイル）。`ShaderManager::CompileCustomShader`・`EnsureCustomPso`・BuildGame の焼き込みをそのまま再利用する。ただし現状のカスタムシェーダーは **ライティングを持たない**（`UnoCustom.hlsli` は b0/b1 と数学関数のみ）ので、`Forward.hlsl:174-364` の PS 本体を **`ForwardShade.hlsli` として外出し**し、生成コードは「サーフェス（BaseColor / Roughness / Normal …）を返す関数 `UnoMatEval`」だけを書く（§3.5）。
3. **パラメータとテクスチャは全部バインドレス**。材質は SRV ブロック 4 枚固定（`Material.h:60`）なので任意枚数のテクスチャを持てない。`ResourceManager` が読み込んだ全テクスチャは既に永続 SRV 添字を持つ（`ResourceManager.cpp:243-246`）ので、**パラメータバッファ（StructuredBuffer<float4>）に SRV 添字を書いて `ResourceDescriptorHeap[]` で引く**。ルートシグネチャは **DWORD +0**（残り 2/64 を消費しない）。地形が `b2` を読み替える前例（`ApplicationRender.cpp:880-905`）に倣い、グラフ材質の `b2` はレコード位置とプール SRV 添字に読み替える。必要な変更は **メイン RS に `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` を立てるだけ**（VG 設計書 P4 と共通の作業、§7.4）。
4. **コンパイル待ち対策の核は「定数をスロット化」**。Constant ノードの値もパラメータと同じくバッファのスロットに入れる → **値をいじっても再コンパイルしない**。再コンパイルは構造を変えたときだけ。さらに HLSL テキストハッシュでのディスクキャッシュ、専用ワーカースレッドでの非同期コンパイル + PSO 生成、旧版維持（現行の挙動 `ShaderManager.cpp:484-487` と同じ）で「操作が固まらない」を担保（§3.8）。
5. **データは `.dxmg`（グラフ, JSON+安定 ID）+ 既存 `.dxmat`（インスタンス）**。`.dxmat` は「`graph` キーが無ければ今と完全に同じ」= **既存資産は 1 バイトも変わらない**。グラフ化は手動ボタン（既存 `.dxmat` → 標準 PBR グラフ + インスタンスへ自動アップグレード、絵の一致をゴールデンで判定）（§4）。
6. **UE 取り込みは 3 段**: T0 = マテリアルインスタンス + パラメータ + テクスチャ → 「標準 PBR グラフ」のインスタンスへ（cook 済みでも可能）、T1 = **未 cook の `.uasset` の式ノード写像**、T2 = 写せないノードは「UE 固有」プレースホルダー（定数 0 で代替 + 警告）。**cook 済みパッケージにはグラフが無い**ので（Dead Mall で実際に踏んだ [事実: memory `dreamcore-dead-mall` §4]）T1 は UE エディタ側のプロジェクトが前提（§5）。

**段階（詳細 §7）**: G0 ノードエディタ基盤 10 日 ∥ G1 型システム + コード生成 9 日 ∥ G2a シェーダー外出し + RS フラグ 4 日 → G2b シーン反映 8 日 → G2c プレビュー + 非同期 6 日 → G3 ノード 60 種 + パレット + MCP 12 日 → G3b マスク/半透明 6 日 → G4 インスタンス/関数 12 日 → G5 UE 取り込み 16 日 → G6 DXR / VG 整合 16 日。合計約 99 実働日、クリティカルパス（G1→G2b→G2c→G3）は約 35 日。G7（スキンド / 頂点変位 / 特殊シェーディング）は任意。

**最大のリスク**: ① グラフ編集ごとのコンパイル待ち（`ps_6_6` でライティング込みの巨大 PS を毎回コンパイルする）→ プレビューは軽量シェーダー + 本番反映は非同期、G2c の実測ゲートで判定 ② RS へのフラグ追加がドライバ挙動を変える可能性（ゴールデンのビット一致 + ベンチで判定、駄目なら別 RS 案へ撤退）③ UE 式の写像率（未 cook のみ。撤退 = T0 だけ出荷）。

**ユーザーに決めてほしい点**は §9（5 件、推奨案付き）。

---

## 1. 現行の事実（実コード確認）

設計に効く事実だけを抜き出す。「含意」が本書の設計への入力。

### 1.1 マテリアル系（データ）

| # | 事実 | 根拠 | 含意 |
|---|---|---|---|
| M1 | `Material`（焼き込み）はテクスチャ 4 枚（albedo / normal / metalRoughness / emissive）+ スカラー（metallic / roughness / emissive 色・強度 / alpha 3 種）。SRV ブロックは **連続 4 枚固定**（`kMaterialSrvBlockSize = 4`）。レジスタは t0,t1,t2,t24 で、ヒープ上は OFFSET_APPEND の 4 連続 | `renderer/Material.h:29-60`、`graphics/RootSignature.cpp:36-62` | 任意枚数のテクスチャを持つグラフ材質は、このブロックに収まらない → バインドレスが必要（§3.6） |
| M2 | `.dxmat` = `MaterialAssetData`（name / albedo / normal / metalRoughness / emissive パス、metallic、roughness、emissive 色・強度、uvTiling、source、license）。JSON `version:1`。**テクスチャを 1 枚も参照しない `.dxmat` は無効**として読み込みが失敗する | `resource/MaterialAssetIO.h:16-38`、`MaterialAssetIO.cpp:62-66` | インスタンス専用 `.dxmat`（`graph` + `params` のみ）を通すには検証条件の拡張が要る。旧ファイルは `graph` キーを持たない = 既存経路に落ちる |
| M3 | 書き出し（`SerializeMaterialAsset`）は「使っているキーだけ」書く。emissive も使っている `.dxmat` にだけ書くので、**読んで書き戻してもキーが増えない（差分が出ない）** | `MaterialAssetIO.cpp:73-91`（コメント `:82-84`）、`tests/dxmat_roundtrip_test.cpp` | 後方互換の方針と同じ流儀で `graph` / `params` を足せる。往復テストを拡張して「旧ファイルのバイト一致」を機械判定できる |
| M4 | `MaterialAssetManager::LoadInto` は 4 枚の SRV を材質ブロックへ書く。albedo=sRGB、normal / MR=linear、欠落は既定テクスチャ。エディタでは 0.5 秒 mtime ポーリングでホットリロード | `resource/MaterialAssetManager.cpp:39-109`、`:145-167` | グラフ材質は別エントリ種別（`Entry::graph`）として同じキャッシュ・ポーリングに相乗りできる |
| M5 | 描画時の優先度は `materialAsset > overrideXxxTexture > モデル焼き込み Material`。`materialAsset` はサブメッシュ単位（`std::vector<std::string>`）。シーン JSON は `"materialAssets"` | `ecs/Components.h`（`materialAsset` メンバ）、`core/ApplicationRender.cpp:844-855`、`scene/SceneSerializer.cpp:571-582,1875-1880` | 「グラフ材質 = `materialAsset` の一種」にすれば **シーン JSON は無改造** |
| M6 | b2（9 DWORD）= metallic / roughness / flags / packedTint（上位 8bit = opacity）/ uvScaleOffset(float4) / packedEmissive。地形は同じ b2 を **TerrainMaterial として読み替える**前例あり | `Material.h:86-99,119-127`、`ApplicationRender.cpp:880-905`、`Forward.hlsl:52-77` | グラフ材質も b2 を読み替えてよい（§3.6）。ただしルート定数の書き換えは 1 箇所（`:1027` の 9 DWORD 書き込み）に集約されている |
| M7 | UV タイリングは **頂点バッファへ焼き込む**（`Mesh::ApplyUVScale`）。`.dxmat` の `uvTiling` は「これから割り当てるときの初期値」で、割当済みには即時反映されない | `docs/AUTHORING.md:646-649` | グラフの `TexCoord` ノード（Tiling / Offset）は頂点 UV の上に乗る。エンティティ側 `uvScale` と二重に掛かるので、標準テンプレートのグラフ化では `uvTiling` を **1 回だけ**適用する規約にする（§4.4） |

### 1.2 マテリアルエディタ（UI）

| # | 事実 | 根拠 | 含意 |
|---|---|---|---|
| E1 | 現行 `MaterialEditorPanel` は 512 行のフローティング窓（`ImGuiWindowFlags_NoDocking`）。テクスチャ 3 スロット + metallic / roughness / uvTiling。スカラーはドラッグ中 `UpdateScalarsOnly` で即反映（SRV 再構築なし）、離したら保存 + `Invalidate` | `editor/panels/MaterialEditorPanel.cpp:263-274,338`、`.h:22-27` | 「値だけの編集は軽い経路」という思想を **グラフの Constant / Parameter にも適用**する（§3.8 の核） |
| E2 | 3D プレビュー `MaterialPreviewRenderer` は **メイン Forward.hlsl と完全に独立**した最小パイプライン。専用の小さい RS（b0 32 / b1 24 / b2 4 DWORD）、固定 2 灯のスタジオライティング、LDR(R8G8B8A8) 出力、**独自の `ShaderRuntimeCompiler` を持つ**。球 / 平面。サムネイルは 128px でディスクキャッシュ | `editor/panels/MaterialPreviewRenderer.h:26-31,90-105`、`.cpp:34-59,174-227`、`shaders/forward/MaterialPreview.hlsl:1-5` | プレビューはメインの状態（影 / IBL / クラスタ）に依存しない = **グラフ用プレビューも同じ流儀で軽量に作れる**。専用 RS なので `HEAP_DIRECTLY_INDEXED` も自由に足せる |
| E3 | エディタの窓は `ToolWindows.h` の表で登録（`{"material", "マテリアルエディタ", …, DockSlot::Floating, MenuHome::Tools, …}`）。コマンドパレット（Ctrl+K）とメニューは表から自動生成 | `editor/ToolWindows.h:73`、`editor/EditorCommandTable.h:1-16` | 新窓は表に 1 行足すだけでメニュー・パレットに出る |
| E4 | Undo は `IUndoCommand`（`Undo/Redo/GetName/IsAi`）+ `CompositeCommand` + `UndoSystem`（スタッククラス。AI（MCP）の操作を「AI: …」1 エントリに束ねる横取り機構つき）。ゲーム内シングルトンではなく**インスタンス化できる** | `editor/UndoCore.h:22-120` | グラフ文書ごとに `UndoSystem` を 1 個持たせ、フォーカス中のグラフ窓が Ctrl+Z を処理する（§6.5） |
| E5 | UI テーマは `editor/EditorTheme.h` のトークン（`Bg0..Bg4` / `Accent`(0x2F8CFF, UE5 青) / `Good/Warn/Bad` / 種別色）。寸法は論理 px で書き `ui::Px()` を通す（DPI）。ImGui は **1.92.6**（docking）、FreeType 有効 | `EditorTheme.h:33-108`、`editor/UiWidgets.h:28-30`、`vcpkg.json:11-14`、`build/release/vcpkg_installed/x64-windows/include/imgui.h`（`IMGUI_VERSION "1.92.6"`） | グラフの色は **全部 `theme::` から導出**する（§6.2）。別エージェントの 3 案は theme 側の差し替えで追従できる |
| E6 | UI 自動テストは ImGuiTestEngine（`--ui-tests-run-all`、JUnit 出力）。入力は ImGui の IO へ注入で、OS のカーソルは奪わない。GUI 駆動作業は「ユーザーの PC 操作を奪う」ので、撮影は `imgui_screenshot`（バックバッファ）で行う | `gui/UiTestHarness.h:4-14`、memory `no-gui-driving-agents` | 各段階の合否判定は **ctest / ImGuiTestEngine / `--background` + `imgui_screenshot`** のみで書く（入力注入・前面化は使わない） |
| E7 | 既存のノード / ワイヤ系 UI は無い（`AddBezierCubic` / `AddBezierCurve` の使用ゼロ）。ImGuizmo は `src/gui/ImGuizmo.{h,cpp}` に**同梱（vendored）**されている | grep（`src`）、`gui/ImGuizmo.h:1-3` | 外部 UI ライブラリの同梱には前例がある（が、今回は自前を推奨） |

### 1.3 カスタムシェーダー機構（ここに載せる）

| # | 事実 | 根拠 | 含意 |
|---|---|---|---|
| C1 | `ShaderManager::CompileCustomShader(relPath)` は `assets/shaders/**.hlsl`（Registry 外）を **`VSMain`=`vs_6_0` と `PSMain`=`ps_6_0` の 2 回**、`ShaderRuntimeCompiler`（`IDxcCompiler3`）でコンパイル。`-I` はプロジェクト側 → エンジン側の順。成功したときだけバイトコードを差し替え、失敗時は**直前の有効なバイトコードを維持** | `resource/ShaderManager.cpp:427-491`（`:439,:446` がプロファイル、`:484-487` が維持）、`:75-86`（IncludeDirsFor） | 生成 `.hlsl` をここへ置くだけで動く。**ただし `ps_6_0` 固定**なのでバインドレス（`ps_6_6`）用に「ファイル先頭の `// @sm 6_6` でプロファイルを選ぶ」拡張が要る（G2b） |
| C2 | 検出は `ScanCustomShaders`（`assets/shaders` を再帰走査して mtime 差分）。**`Poll()` はメインスレッドで同期コンパイルし、変化があれば `CommandQueue::WaitIdle()` してからハンドラを実行**。周期は 0.5 秒 | `ShaderManager.cpp:184-227`、`core/Application.cpp:1704-1717` | 編集のたびにこの経路へ通すと**フレームが止まる**。グラフは専用ワーカー + 世代管理の別経路にする（§3.8） |
| C3 | PSO は `Application::EnsureCustomPso`（`ApplicationPipeline.cpp:1108-1183`）が遅延生成。**メイン RS** + `Mesh::GetInputLayout` + 4 種（less / lequal × 不透明 / アルファブレンド）。失敗時は `shaderdiag::ExplainPsoFailure` が RS と DXIL リフレクションを突き合わせて理由を出す。ホットリロードは `m_customPsoCache.erase(key)` だけ（`:1011-1020`） | 同左 | 4 種 PSO とキャッシュ破棄の仕組みをそのまま使う。**PSO 生成は同期（メインスレッド）**なので、グラフでは非同期化が要る |
| C4 | カスタムシェーダーの割当は **`MeshRenderer::shaderPath`（エンティティ単位）**。PSO 選択は**サブメッシュのループの外**（`ApplicationRender.cpp:750`）。**スキンドはカスタムを無視して既定へ**（`:730-747` に警告）。カスタムは自動インスタンシング対象外（`sortKey` 1 / 3）。深度プリパス・影・速度は既定シェーダー | `ApplicationRender.cpp:311,328-336,730-767` | グラフ材質は **サブメッシュ単位の PSO 切替**（`:1029-1038` の `want` の仕組み）を使う必要がある。スキンドは v1 対象外（G7） |
| C5 | `UnoCustom.hlsli` が提供するのは b0（mvp / model / 自由枠 8 float）、b1（view / proj / lightDir / time / lightColor / cameraPos …）、t0..t2 + s0、頂点 I/O、ノイズ / フレネル / ゲルストナー波。**ライティング（クラスタライト・影・IBL・SSAO/SSR/SSGI・DDGI・デカール）は無い** | `shaders/UnoCustom.hlsli:32-93,111-204`、`resource/ShaderTemplates.cpp:105-157` | 現行のカスタムシェーダーは実質アンリット / 手書きライティング。**グラフ材質で PBR を出すにはライティング本体の共有化が必須**（§3.5） |
| C6 | b0 の自由枠（オフセット 128..159 の float 8 個）は `MeshRenderer::effectValue / shaderParamsB / shaderParams` へ 1 対 1 対応し、DXIL リフレクションで**名前付きパラメーター**として Inspector・Lua（`scene:setMeshParams`）・Trigger（`SetShaderParam` / `AnimShaderParam`）から動かせる | `resource/ShaderParams.h:29-32,40-99`、`ecs/Components.h`（`CustomParamBase`）、`scripting/ScriptEngine.cpp:892` | グラフに **`ObjectParameter` ノード**（b0 自由枠の名前付き変数）を用意すれば、既存の Lua / Trigger / tween 資産がそのまま使える（§3.7 ノード #8）。上限 8 float |
| C7 | 配布ビルドは `RecompileAllForBuild` が `assets/shaders` 全体を再帰コンパイルして `shaders/custom/<relPath>_VS.cso/_PS.cso` を pak へ焼く。ゲームモードは ShaderManager を作らず pak から読む（`FetchCustomShaderBytecode`） | `ShaderManager.cpp:331-408`、`ApplicationPipeline.cpp:1046-1080` | 生成 `.hlsl` を `assets/shaders/_graph/` に置けば**配布の仕組みも無改造で乗る**。ゲーム側にコンパイラは要らない（ビルド時に全グラフの HLSL を再生成してから走らせる） |

### 1.4 フォワードシェーダーのマテリアル入力（最重要の制約）

| # | 事実 | 根拠 | 含意 |
|---|---|---|---|
| F1 | `Forward.hlsl` の `PSMain`（**190 行**）は「マテリアル評価（albedo サンプル・法線マップ・MR・アルファ）」と「シェーディング（デカール → 法線フィルタ → CSM + コンタクト影 → 平行光 → クラスタライト → SSAO / SSR / SSGI / DDGI / IBL → emissive）」が**1 関数に直書き**。`ForwardSkinned.hlsl:179-348` も同構造の複製 | `shaders/forward/Forward.hlsl:174-364`、`ForwardSkinned.hlsl:179` | 「マテリアル評価」を差し替え可能にするには、**シェーディング尾部を関数へ外出し**する（DXIL 同値を機械判定できる純粋な整理）。VG 設計書 §2.2.6 も同じ手順を resolve PS に**複製**する計画 → 共有すれば重複が消える（§7.4） |
| F2 | b0 は `mvp / model` の 128B（Forward は自由枠を宣言しない）。b1 は 1536B の `PerFrameConstants`（`Lighting.hlsli:71-118`、`UnoCustom.hlsli` と**バイト単位で一致**させる約束）。b2 は上記 | `Forward.hlsl:47-50`、`Lighting.hlsli` | 生成コードは `Lighting.hlsli` を include して b1 をそのまま使う |
| F3 | **b0 衝突の罠**（memory `dx12-shading-traps`）: カスタムシェーダーの cbuffer は「オフセットで対応が決まる」ので宣言順を 1 つ間違えると**エラー無しで値だけ化ける** | `UnoCustom.hlsli:5-16`、memory `dx12-shading-traps` | 生成コードは **cbuffer を自前で書かず、エンジン側ヘッダ（`ForwardGraph.hlsl`）に固定で持たせる**。生成物が触れるのは関数本体だけ（§3.5） |
| F4 | 法線マップは `PerturbNormal` が z を 0.35 で打ち止めする（黒斑点対策）。`FilterShadingNormal` が分散→ラフネスを補正 | `PBR.hlsli:57-98`、memory `dx12-shading-traps` | グラフの Normal 出力（接空間）も**同じ `PerturbNormal` 系のガードを通す**。標準テンプレートのグラフ化が旧経路と一致する条件（§7 G2b の合否） |
| F5 | アルファは 2 系統: `ALPHA_TEST` = `-D` バリアント（clip 入りは early-Z を失うので不透明パスに混ぜない）、BLEND = 深度書き込み OFF の別 PSO。**影・深度プリパス・速度・Perception ID も MASK 用に別 PSO / `-D` バリアントを持つ** | `Forward.hlsl:187-197`、`ShadowMask.hlsl:119`、`VelocityCommon.hlsli:34-77`、`ApplicationPipeline.cpp:411-508`（`RecreateDepthMaskPsos`） | グラフの OpacityMask は「影・深度・速度用の軽量 PS（マスク式だけ）」を**同じ生成物から別エントリで出す**必要がある（G3b） |
| F6 | 深度プリパス / 影 / 速度は `RenderDepthOnlyScene` が回し、G-Buffer 用の roughness / metallic を **`materialAsset` のスカラーだけ**から取る（テクスチャは読まない） | `ApplicationRender.cpp:1417-1500`（`resolvePbr` `:1492-1508`） | グラフ材質の G-Buffer 値は「Roughness / Metallic 出力が定数（スロット）ならその値、それ以外は既定 0.5 / 0」の近似（SSR の保守的挙動）。厳密化は G6 |

### 1.5 ルートシグネチャ・バインドレス・DXR

| # | 事実 | 根拠 | 含意 |
|---|---|---|---|
| R1 | ルート定数の予算は **62 / 64 DWORD**（残り 2）。b0 40 + CBV 2 + b2 9 + テーブル 11。**テーブルはレンジを何本持っても 1 DWORD**、**静的サンプラは DWORD ゼロ** | `graphics/RootSignature.cpp:16-22,317` | 新しい機能は既存テーブルへのレンジ追加か、バインドレスで吸収する。**グラフ材質は DWORD を 1 本も足さない設計にする**（§3.6） |
| R2 | メイン RS の Flags は `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` のみ。**`HEAP_DIRECTLY_INDEXED` は立っていない**。立てているのは DDGI と RtScreenPass の専用 RS | `RootSignature.cpp:329`、`renderer/DdgiVolume.cpp:124`、`renderer/RtScreenPass.cpp:129-132` | フォワードで `ResourceDescriptorHeap[]` を使うには**メイン RS にフラグを足す**（DWORD 影響なし）。順序制約「`SetDescriptorHeaps` を RS Set の前に」は `CommandList::SetDescriptorHeap` が既に満たす（`graphics/CommandList.cpp:132-135`、再バインド箇所 `ApplicationRender.cpp:2014-2015`） |
| R3 | GPU 能力の判定 `GraphicsDevice::SupportsDynamicResources()` = SM 6.6 以上 **かつ** Resource Binding Tier 3。SM6.6 バインドレスは実機（RTX 5060）で Agility SDK 不要と確認済み | `graphics/GraphicsDevice.h:56-63`、`renderer/RtBindless.hlsli:1-24`、memory `dx12-realism-roadmap` | 非対応 GPU ではグラフ材質を出さない（縮退 = 既存 `.dxmat` 経路）。Tier 3 未満は実運用上ほぼ無い |
| R4 | `ResourceManager::GetOrLoadTexture` はキーが **パス + sRGB + usage + 最大寸法**（sRGB / 法線の取り違え事故を実際に踏んで修正済み）。読み込んだ全テクスチャに **永続 SRV 添字**を割り当てる | `resource/ResourceManager.cpp:203-215,243-246` | グラフのテクスチャパラメーターは「パス + サンプラー種別（Color / Normal / Mask）」から `Texture*` を取り、`GetSrvIndex()` をパラメータバッファへ書けばよい。**追加の SRV 確保は不要** |
| R5 | 静的サンプラは s0（異方性 8x, WRAP）/ s1（影比較）/ s2（LINEAR CLAMP mip 有）/ s3（LINEAR CLAMP mip なし）/ s4（POINT CLAMP）/ s5（DDGI） | `RootSignature.cpp:252-323` | グラフのサンプラー種別（Wrap / Clamp / Mirror × Aniso / Linear / Point）は **s6 以降を無料で足せる**（R1） |
| R6 | DXR 側（`RtBindless.hlsli`）がマテリアルとして見るのは **`GeometryInfo.baseColorSrvIndex`（アルベドテクスチャ 1 枚）+ 頂点カラーだけ**。`SampleLevel` 固定。マテリアル用の別テーブルは意図的に作っていない | `RtBindless.hlsli:29-34,111-126`、`renderer/RaytracingScene.h:49-60` | グラフ材質は RT / DDGI から **BaseColor の近似（定数 or UV 空間ベイク）**でしか見えない。G6 で「RT 用プロキシ」を出す |
| R7 | VG 設計書: カスタムシェーダー / 材質上書き（`HasMaterialAsset`）は **VG 対象外（NONVG）**、resolve PS は `Lighting.hlsli` 等を include して Forward の PS 本体と同手順、マテリアルは `MaterialRecord`（96B）から引き、`SampleGrad` + 解析 UV 勾配、`m_rootSignatureVg` は「メイン同一パラメータ + HEAP_DIRECTLY_INDEXED の別オブジェクト」 | VG 設計書 §2.2.2（`:115-125`）、§2.2.6（`:157-167`）、§2.2.9（`:182-189`）、§3.4（`:513-534`）、P6（`:822-833`） | **v1 のグラフ材質は VG 対象外**（カスタムシェーダーと同じ扱い）で矛盾しない。VG 側での対応は G6（§7.4） |

---

## 2. ノードエディタ実装の選定

### 2.1 比較（事実のみ）

| 観点 | imnodes | imgui-node-editor（thedmd） | 自前（ImDrawList） |
|---|---|---|---|
| ライセンス | MIT [外部] | MIT [外部] | 自社 |
| 導入 | vcpkg に**無い**（`/c/vcpkg/ports` に不在）[事実]。3 ファイル同梱 | vcpkg に `imgui-node-editor` 0.9.3（port-version 4）**あり**。ただしポートは `fix-vec2-math-operators.patch` / `remove-getkeyindex.patch` / **`fix-imgui-v1.92.5.patch`** を当てている [事実: `/c/vcpkg/ports/imgui-node-editor/portfile.cmake`] | 依存なし |
| ImGui 1.92 / docking | 未確認（着手時に要確認）。内部 API（`imgui_internal.h`）依存 | 1.92 向けにポート側で追従パッチ。README は「Vanilla ImGui 1.72+」[外部]。docking / マルチビューポート併用の既知不具合の噂あり [記憶] | 1.92.6 の公開 API + `ImDrawList` のみ |
| ズーム | **未対応**（メインラインに無い。PR #134 が長期未マージ）[外部] | **対応**（`ImGuiEx::Canvas`。頂点変換方式。拡大時は文字がぼやける）[記憶] | 自前（`AddText(font, size*zoom)` は 1.92 の動的フォントで鮮明）[設計] |
| ミニマップ | **あり**（標準機能）[外部] | 標準では無し [記憶] | 自前（ノード矩形 + ビューポート枠。約 150 行）[推定] |
| ワイヤ品質 | 3 次ベジェ + 設定色 | 3 次ベジェ + 流れるアニメ（`Flow`）内蔵 | 3 次ベジェ + 2〜3 パスのグロー（トークン駆動） |
| ノード見た目の自由度 | ヘッダ / ピン形状は既定（円・四角・三角）+ 色。内側は ImGui ウィジェット可。中に `Separator` を置くとはみ出す既知の制約 [外部] | 中身は**完全に自前描画**（`BeginNode`〜`EndNode`）。自由度は高いがライブラリの状態機械に従う | 完全に自由（ピン形状 = 型、グロー、エラーバッジ、ノード下部プレビュー） |
| DPI | 手動（スタイルを自分でスケール） | 手動 | `ui::Px` を全描画に通せる |
| Undo / テスト | 選択・移動の状態はライブラリ内。Undo は自前で差分取り | 同左（リンク作成イベント経由） | **コマンド単位で完全に握れる**。テスト用に全ピンの画面座標を返せる |
| 状態保存 | ノード位置はライブラリ管理（INI 文字列） | 独自 `.json` 設定ファイル（コールバックで差し替え可能）| `.dxmg` 内 `layout` に直接 |
| メンテ | 活動中（461 commits、issue 49 件）[外部] | 4.5k stars / 99 open issues [外部]。ポートが `v0.9.3` 固定で本体の追従を待つ形 [事実] | 自分 |
| 追加コスト | 小（~3 日でグラフが動く [推定]） | 小〜中（~4 日 [推定]。パッチ管理 + 設定ファイル置換） | 大（**~10 日** [推定]、下記の内訳） |

### 2.2 推奨: 自前（ImDrawList）

**理由**（優先順）:

1. **見た目が製品の顔**。Uno 独自のダーク + ネオングロー（別エージェントが 3 案作成中）を、ワイヤ・ピン・ヘッダ・選択・エラー・「コンパイル中」の全部で**テーマトークン駆動**にしたい。ライブラリの既定描画を上書きし続けるより、最初から全部自分で描く方が速く、崩れない。
2. **Undo と MCP を最初から一級市民にする**。グラフ編集は全て `IUndoCommand`（§6.5）で表現し、AI（MCP）の編集も同じコマンド列を通す。ライブラリが選択・移動・リンクの状態を内部で持つ構成だと、「ライブラリの状態 ↔ 自前のモデル」の二重管理になる。
3. **ImGui 1.92.6 + docking + DPI との相性を自分で保証できる**。vcpkg のポートは 1.92.5 用パッチ止まり [事実]、imnodes は未検証。**自前は公開 API だけ**なので ImGui 更新で壊れない。
4. **テスト可能性**。全ピン・ノードの画面座標を返すテストフック（`GraphView::TestPinScreenPos`）を持てるので、ImGuiTestEngine でドラッグ接続を自動テストできる（§7 G0）。
5. **性能**。数百ノードで 60 fps にはカリング（表示外のノード・ワイヤを描かない）と LOD（縮小時にピン文字を省く）が要る。自前なら完全に制御できる。
6. **同梱コード無し**。ライセンス表記・パッチ・アップデート追従の運用が要らない。

**退けた案**: 「imgui-node-editor のフォーク」— 4 日短縮できるが、①ポートのパッチを自分で保守 ②見た目の上書き ③ Undo の二重管理 ④テストフックが無い、の 4 点で総コストが逆転する。**撤退条件**（G0 が 14 日を超えそう / ワイヤ描画が 500 ノードで 60 fps を割る）が出たら、`ImGuiEx::Canvas` だけを同梱してズーム / パン部分を借りる案へ切り替える（§8）。

### 2.3 自前実装の構成 [設計]

```
src/matgraph/                 ← 新規静的ライブラリ（D3D12 にも ImGui にも依存しない純ロジック）
  GraphModel.{h,cpp}          ノード / ピン / リンク / コメント / 設定。安定 ID。不変条件（循環禁止・型チェック）
  GraphIO.{h,cpp}             .dxmg / .dxmf の JSON 読み書き（安定順序）
  GraphCommands.{h,cpp}       IUndoCommand 実装（追加 / 削除 / 接続 / 切断 / 値変更 / 移動 / 貼り付け）
  NodeRegistry.{h,cpp}        ノード型カタログ（ピン定義・プロパティ・HLSL 生成関数・説明・検索語）
  TypeSystem.{h,cpp}          型・自動キャスト規則・型推論
  Compiler.{h,cpp}            検証 → IR → 最適化 → HLSL 生成 → ParamLayout / SourceMap（§3）
  CpuEval.{h,cpp}             IR の CPU 参照実装（数値テスト用）
src/editor/matgraph/          ← ImGui 依存（エディタ専用）
  GraphView.{h,cpp}           キャンバス（パン / ズーム / グリッド / ミニマップ / 入力 / 描画 / カリング）
  GraphTokens.h               theme:: から導出する見た目トークン（§6.2）
  NodePalette.cpp             検索付きパレット / コンテキスト検索
  MaterialGraphPanel.{h,cpp}  窓（パレット・グラフ・詳細・プレビュー・エラーリスト）
```

- **座標系**: グラフ座標（float, 100% = 1 単位 = 論理 1px）→ 画面 = `origin + (g - pan) * zoom * dpiScale`。描画関数はすべて 1 個のヘルパ `S(g)` を通す。ズーム範囲 0.25〜1.5（1.0 が等倍で最も鮮明）。
- **入力**: ImGui アイテムはキャンバス全体の `InvisibleButton` **1 個だけ**。ノード / ピン / ワイヤのヒットテストは自前（一様グリッドの空間インデックス）。数百ノードで ImGui アイテムを大量に作らない。
- **インライン編集**: ノード内の値欄は自前描画のミニウィジェット（float ドラッグ / 色見本 / チェック / 列挙 / テクスチャサムネ）。重い編集（色ピッカー・テクスチャ選択）はポップアップで**等倍**に開く（ImGui ウィジェットはズームで拡大しないため）。
- **描画**: ワイヤは `AddBezierCubic`（水平接線）。グローは 2〜3 パスの太い半透明ストローク（`ImDrawList` は加算ブレンドを持たないので、色の明るさ + アルファで代用）。**選択 / ホバー中のノードに繋がるワイヤだけ**グロー、それ以外は 1 パス（コスト管理）。
- **内訳（10 日 [推定]）**: モデル + コマンド + IO 3 日 / 描画（ノード・ピン・ワイヤ・グリッド・コメント・ミニマップ）3 日 / 入力（選択・ドラッグ・接続・矩形選択・リルート・クリップボード）3 日 / テスト + ポリッシュ 1 日。

---

## 3. コード生成設計

### 3.1 全体パイプライン

```
 .dxmg（グラフ JSON）
   │  読み込み（GraphIO）
   ▼
 GraphModel  ──編集（GraphCommands, Undo/Redo, MCP）
   │  Compile(graph, options)         ← 純ロジック。エンジン無しで単体テスト可能
   ▼
 ① 検証        : 未接続の必須ピン / 型 / 循環 / 関数再帰 / 未対応ノード → Diagnostics（nodeId, pin, 重大度, 文言）
 ② 展開        : マテリアル関数呼び出しをインライン展開（呼び出し経路つき ID）
 ③ IR 生成     : 出力ノードから到達可能なノードだけを後順 DFS（=トポロジカル順）で SSA 風の IR へ（死んだノード除去）
 ④ 最適化      : 値番号付け（CSE）/ 定数畳み込み（Inline 指定のみ）/ スロット割当（§3.6）
 ⑤ HLSL 生成   : UnoMatEval() 本体 + `#line` によるソースマップ
   ▼
 CompileResult { hlsl, paramLayout, textureSlots, sourceMap, diagnostics, stats, graphHash }
   │
   ├─ 書き出し: assets/shaders/_graph/<guid>.hlsl（内容が変わったときだけ書く＝mtime を無駄に進めない）
   ├─ ShaderManager（または専用ワーカー）が DXC → DXIL → PSO（§3.8）
   └─ ParamLayout でインスタンスのパラメータレコードを組む（§3.6）
```

**純ロジック（①〜⑤）は D3D12 / ImGui / DXC に依存しない**（`src/matgraph/`）。これで G1 を「HLSL テキストを出す + CPU で評価して数値を検証する」だけで単体マージ可能にする。

### 3.2 型システム

| 型 | 説明 | HLSL |
|---|---|---|
| `F1` `F2` `F3` `F4` | スカラー〜4 成分（すべて 32bit float。int / uint は持たない） | `float` … `float4` |
| `Tex2D` | 2D テクスチャ（オブジェクト。値ではなく「どの SRV か」） | `Texture2D<float4>`（バインドレスで取得） |
| `Bool` | コンパイル時定数（StaticSwitch 用。G4） | プリプロセッサ / 定数 |
| `Sampler`（ノード属性） | ピンではなく TextureSample / TextureParameter のプロパティ（Wrap / Clamp / Mirror × Aniso / Linear / Point） | 静的サンプラ s0, s2, s4, s6.. |
| `Attr`（予約） | マテリアルアトリビュート束（将来のレイヤー用） | 未使用 |

**自動キャスト規則**（UE に合わせつつ、失敗を早く見せる）:

1. `F1 → F2/F3/F4`: **暗黙のスカラー拡張**（`float3(x,x,x)`）。警告なし。
2. `F(n) → F(m)` で `m < n`: **切り詰め**（先頭 m 成分）。**警告**（「float3 → float2 に切り詰め」）。
3. `F(n) → F(m)` で `1 < n < m`: **エラー**（「float2 を float3 に拡張できません。Append を使ってください」）。
4. 2 項演算（Add / Multiply など）: 両オペランドの次元が一致、または片方が `F1`。それ以外はエラー。結果の型 = 大きい方。
5. `Tex2D` はスカラー / ベクトルへ接続不可（接続操作の時点で拒否し、ツールチップで理由を出す）。`F*` を `Tex2D` へも不可。
6. ピン型が「多相」（`Any` = F1〜F4）のノード（Add / Lerp / Saturate …）は、接続された入力から**出力型を推論**する。未接続なら `F1`。

**推論は 1 パス**（トポロジカル順）で行い、結果の型をノードごとに保持する。UI は型を見てピン色・形状を変える（§6.3）。

### 3.3 IR・トポロジカルソート・共通部分式

- **到達可能性**: `MaterialOutput`（と関数の `FunctionOutput`）を根に、**入力方向へ後順 DFS**。訪問順がそのままトポロジカル順。出力に繋がらない「死んだノード」は生成されない（エディタでは薄く表示）。
- **循環**: DFS 中に「訪問中」印のノードへ戻ったらエラー。**接続操作の時点で循環になる接続を拒否**する（UI でワイヤが赤くなって落ちない）。
- **IR**: `Op { id, type, opcode, args[], props }`（SSA、各 Op は 1 回だけ定義）。ノードの複数出力ピン（例: TextureSample の RGBA / R / G / B / A）は「1 個の Op + 成分アクセス」に落とす。
- **CSE（共通部分式）**: 値番号 = `hash(opcode, props, 入力の値番号…)`。同じ値番号は 1 個の Op に統合される。ユーザーが同じ TextureSample を 2 個置いても 1 回しかサンプルされない。副作用のある Op（Custom HLSL、Time は純粋なので可）は統合しない。
- **HLSL 出力**: 1 Op = 1 ローカル変数（`float3 v12 = …;`）。DXC が最終的に最適化するが、**変数 1 個 = 1 ノード出力**にしておくとエラー行から元ノードへ戻しやすい。
- **`If` ノード**: **両枝を評価して `select`**（UE と同じ。サンプルを分岐内に置いて微分が壊れる事故を避ける）。真の分岐は `StaticSwitch`（コンパイル時）のみ。

### 3.4 出力ノード（`MaterialOutput`）

| ピン | 型 | v1 | 備考 |
|---|---|---|---|
| BaseColor | F3 | ✔ | 既定 (0.5,0.5,0.5) |
| Metallic | F1 | ✔ | 既定 0 |
| Roughness | F1 | ✔ | 既定 0.5。下限 0.04 は共通尾部で掛ける（`Forward.hlsl:225`） |
| Normal | F3（接空間） | ✔ | 接続時のみ `PerturbNormal` 系のガード（z ≥ 0.35）を通して世界空間へ（F4） |
| Emissive | F3 | ✔ | 共通尾部で影・AO を通さず加算（`Forward.hlsl:340` と同じ） |
| AmbientOcclusion | F1 | ✔ | 環境光へ乗算（`aoDiff/aoSpec` へ掛ける） |
| Opacity | F1 | Translucent 時（G2b） | アルファブレンド |
| OpacityMask | F1 | Masked 時（G3b） | `clip(mask - cutoff)`。影・深度・速度用の別エントリ（F5） |
| **予約（灰色表示・接続不可）** | | | Specular / Anisotropy / SubsurfaceColor / SubsurfaceOpacity / ClearCoat / ClearCoatRoughness / Tangent / WorldPositionOffset(頂点) / PixelDepthOffset / Refraction |

**設定**（グラフ単位）: `blendMode`（Opaque / Masked / Translucent / Additive）、`shadingModel`（`DefaultLit` / `Unlit`。**予約: Subsurface / ClearCoat / Cloth / Hair / Eye**）、`twoSided`、`maskClip`（Masked のしきい値）。

**将来の特殊シェーディングの入力枠**: `UnoSurface` 構造体（§3.5）に上記予約フィールドと `shadingModel`（uint）を**最初から持たせる**。v1 の共通尾部は無視するだけ。契約（構造体のレイアウトとピン名）を固定しておけば、後から尾部を拡張しても既存グラフ・生成物は壊れない。

### 3.5 生成 HLSL を既存のカスタムシェーダー契約に載せる

**方針**: 「サーフェス評価」と「シェーディング」を分離する。前者だけを生成する。

**新規エンジン側ファイル（手書き）**

- `shaders/forward/UnoSurface.hlsli` — `struct UnoSurface { float3 baseColor; float metallic; float roughness; float3 normalTS; bool hasNormal; float3 emissive; float ao; float opacity; float opacityMask; /* 予約 */ float3 subsurfaceColor; float subsurfaceOpacity; float clearCoat; float clearCoatRoughness; float anisotropy; uint shadingModel; };`、`struct UnoMatInput { float2 uv; float3 worldPos; float3 vertexNormalWS; float3 vertexTangentWS; float tangentW; float4 vertexColor; float4 svPos; float3 cameraPos; float time; … }`、`UnoSurfaceDefault()`。
- `shaders/forward/ForwardShade.hlsli` — **`Forward.hlsl:174-364` のうち「マテリアル評価の後」を関数化**した `float4 UnoShadeForward(UnoSurface s, PSInput input)`（デカール → 法線フィルタ → 影 → 平行光 → クラスタライト → AO / SSR / SSGI / DDGI / IBL → emissive）。`Forward.hlsl` の `PSMain` は「旧来のマテリアル評価 → `UnoShadeForward`」に縮む。**DXIL が外出し前と同値**であることを G2a の合否にする（VG 設計書 §5.5 と同じ手法）。`ForwardSkinned.hlsl` の複製も同じ関数へ寄せられる（別作業）。
- `shaders/forward/ForwardGraph.hlsl` — `VSMain`（`Forward.hlsl` と同じ）+ `PSMain`（`UnoMatEval` を呼んで `UnoShadeForward`）+ グラフ材質用の cbuffer 宣言 + 前方宣言 `void UnoMatEval(UnoMatInput mi, UnoMatPool pool, out UnoSurface s);`。**cbuffer はここに固定**し、生成物は触らない（F3 の罠の回避）。

**生成物** `assets/shaders/_graph/<guid>.hlsl`:

```hlsl
// @sm 6_6
// GENERATED by Uno MaterialGraph. DO NOT EDIT.  source=materials/rock_wet.dxmg  graph=9f3c1a…  codegen=1
#include "forward/ForwardGraph.hlsl"

void UnoMatEval(UnoMatInput mi, UnoMatPool pool, out UnoSurface s)
{
    s = UnoSurfaceDefault();
#line 12 "node:n_a1b2c3"
    float2 v0 = mi.uv * pool.F2(2) + pool.F2(3);            // TexCoord(tiling, offset)
#line 20 "node:n_071829"
    float4 v1 = UnoSample(pool.Tex(0), UNO_SAMP_ANISO_WRAP, v0);   // TextureSample
#line 31 "node:n_9a8b7c"
    float  v2 = pool.F1(4);                                  // ScalarParameter "Roughness"
    s.baseColor = v1.rgb;
    s.roughness = v2;
}
```

- `pool.F1(slot) / F2 / F3 / F4 / Tex(slot)` は **パラメータレコード**（§3.6）を読むヘルパ。`Tex` は `ResourceDescriptorHeap[asuint(record[slot].x)]`。フォワードでは添字がドロー単位で一様なので `NonUniformResourceIndex` は付けない（`RtBindless.hlsli:20-24` の方針）。VG resolve のように画素ごとに材質が変わる文脈では `UNO_NU(x)` マクロで付ける（G6）。
- `UnoSample(tex, sampler, uv)` は**マクロ経由**にする。フォワードでは `tex.Sample(sampler, uv)`、G6 の resolve では `SampleGrad`（§3.11）。**サンプルを直接 `Sample` と書かせない**のが後のコストを決める。
- `#line N "node:<id>"` により、DXC のエラー行から元ノードへ逆引きできる（§3.10）。
- ファイル先頭 `// @sm 6_6` は `ShaderManager` がプロファイル選択に使う新しい規約（G2b）。

**ShaderManager 側の必要改修（G2b）**: ① `CompileCustomShader` が `// @sm` を見て `vs_6_6/ps_6_6` を選ぶ（既定は従来の 6_0 のまま）② `_graph/` 配下を Inspector のシェーダー選択肢から隠す（`InspectorPanel` の一覧フィルタ）③ 同期 `Poll()` 経由ではなく専用ワーカー経路（§3.8）。

**PSO**: `EnsureCustomPso` は「メイン RS + `Mesh::GetInputLayout` + 4 種」（C3）。グラフ材質はそのまま使えるが、**選択箇所を「サブメッシュのループの内側」へ移す**（C4）。`EnsureGraphPso(graphKey)` を新設し、`want` PSO の選択（`ApplicationRender.cpp:1029-1038`）に「グラフ材質なら `EnsureGraphPso`」を足す。スキンド・インスタンシングは対象外（C4）。

### 3.6 パラメータ・テクスチャの渡し方（ルート DWORD +0）

**問題**: 材質ブロックは 4 枚固定（M1）、b0 の自由枠は 8 float（C6）、b2 は 9 DWORD（M6）。UE 級のグラフは数十パラメータ・十数枚のテクスチャを持つ。

**解**: **パラメータプール**（`StructuredBuffer<float4>`）+ バインドレス。

- **プール**: エンジンが 1 本（3 フレーム分の複製）持つ SRV バッファ（例 1 MB = 65,536 float4）。材質インスタンス 1 個 = **レコード**（連続 N 個の float4）。レコードの中身は `ParamLayout`（コンパイル結果）が決める:
  - スカラー / ベクトルは 1 スロット = 1 float4（F1 / F2 / F3 は先頭成分に詰める。将来詰め込み最適化可）。
  - **Constant ノードも同じスロットに入れる（既定）**。`Inline` フラグ付きの定数だけ HLSL リテラルにして畳み込む。
  - テクスチャは 1 スロット = `asuint(x) = SRV 添字`（R4 の永続添字）、`y = 1/width`, `z = 1/height`, `w = 予約`（texel サイズノード用）。
- **b2 の読み替え**（地形の前例 M6）: グラフ材質の描画時、b2（9 DWORD）を `{ recordBase(float4 index), poolSrvIndex, flags, packedTint（上位 8bit=opacity）, uvScaleOffset.xyzw, packedEmissive }` として書く（`ApplicationRender.cpp:1027` の 1 箇所を分岐）。`ForwardGraph.hlsl` の cbuffer がこの読み替えを宣言する。**サイズは 9 DWORD のまま = DWORD +0**。
- **RS 変更**: メイン RS の Flags に `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` を追加（1 行）。テーブル・定数の DWORD は変わらない。**VG 設計書 P4 も同じ変更を要求**しているので共通作業にする（§7.4）。非対応 GPU（R3）ではグラフ材質を無効化。
- **サンプラー**: 静的サンプラを s6 以降に追加（AnisoClamp / LinearWrap / PointWrap / AnisoMirror …）。無料（R1）。
- **更新**: パラメータ値の変更 = プールの該当レコードを書くだけ（再コンパイル無し、PSO 無変更）。プールは「Upload ヒープに永続マップ + 3 フレームの複製」で、書き込み中のフレーム以外の複製を GPU が読む（フレーム多重化の既存流儀）。
- **インスタンスごとの上書き / エンティティごとの上書き**: 同じレコード形式で別レコードを持つ（コピーオンライト）。Lua / MCP の `setMaterialParam` はエンティティ専用レコードを作って書く。
- **ObjectParameter**: b0 自由枠（C6）の名前付き変数として宣言。値は `MeshRenderer::shaderParams` から来る（Lua / Trigger 互換）。上限 8 float。

**参考: この方式を採らない場合の代替（却下）**:
- 材質テーブルにレンジを足して t25..t32 を追加 → 全材質のブロック長が 12 になり SRV ヒープ消費が 3 倍、上限 8 枚。却下。
- 材質ごとにルート CBV を増やす → DWORD 2 を消費して残りゼロ。却下。

### 3.7 最初の 60 ノード

段階の列: **G1** = コード生成の最小セット、**G3** = ノード 60 種の本体、**G3b** = マスク / 半透明、**G4** = 関数 / インスタンス。ピン型の `A` = 多相（F1〜F4、§3.2）。`(…)` は入力、`→` の後は出力ピン。

| # | ノード | カテゴリ | 入力 → 出力 | HLSL 生成の要点 | 段階 |
|---:|---|---|---|---|---|
| 1 | Float | 定数 | → F1 | スロット（Inline なら リテラル） | G1 |
| 2 | Float2 | 定数 | → F2 | 同上 | G1 |
| 3 | Float3 | 定数 | → F3 | 同上（色ピッカー表示は props） | G1 |
| 4 | Float4 / Color | 定数 | → F4 | 同上。`srgb` 指定ならリニア化して格納（**格納時に変換**し GPU では何もしない） | G1 |
| 5 | ScalarParameter | パラメータ | → F1 | name / group / range / default。スロット（インスタンスで上書き可） | G1 |
| 6 | VectorParameter | パラメータ | → F4, R, G, B, A | 同上（Color フラグ） | G1 |
| 7 | TextureParameter | パラメータ | → Tex2D | 既定テクスチャ + サンプラー種別（Color / LinearColor / Normal / Mask）。種別が `GetOrLoadTexture` の sRGB / usage を決める（R4） | G1 |
| 8 | ObjectParameter | パラメータ | → A | **b0 自由枠**の名前付き変数（Lua `scene:setMeshParams` / Trigger / tween 互換）。上限 8 float。Uno 独自 | G3 |
| 9 | TextureSample | テクスチャ | (Tex, UV, MipBias) → RGBA, R, G, B, A | `UnoSample`。Tex 未接続なら埋め込み TextureParameter（省略記法） | G1 |
| 10 | NormalMap | テクスチャ | (Tex, UV, Strength) → F3(接空間) | `xy*2-1` に強さを掛け z を再構成（`PBR.hlsli:57-98` と同じ規約）。OpenGL 規約（G 反転しない） | G3 |
| 11 | TriplanarSample | テクスチャ | (Tex, Scale, Sharpness, Space) → RGBA | ワールド / オブジェクト空間の 3 方向ブレンド（`Terrain.hlsl` と同じ考え方） | G3 |
| 12 | Flipbook | テクスチャ | (UV, Frames, Cols, Rows, Phase) → F2 | `SpriteAnim.h` 相当の式 | G3 |
| 13 | ParallaxOcclusionMapping | テクスチャ | (HeightTex, UV, Scale, Steps, ViewDirTS) → F2 | 高さマップの線形探索 + 二分。ステップ数は定数（ループ展開）。接線フレームは VS 出力を使う | G3 |
| 14 | TexCoord | 座標 | (Tiling, Offset) → F2 | `mi.uv * tiling + offset`。頂点に焼かれた UV スケール（M7）の上に乗る | G1 |
| 15 | WorldPosition | 座標 | → F3 | `mi.worldPos` | G1 |
| 16 | ObjectPosition | 座標 | → F3 | `model` の平行移動（VS で計算して渡す） | G3 |
| 17 | VertexNormalWS | 座標 | → F3 | `mi.vertexNormalWS`（法線出力前の幾何法線） | G3 |
| 18 | VertexTangentWS | 座標 | → F3, F1(w) | 接線 + 符号 | G3 |
| 19 | VertexColor | 座標 | → F4, R, G, B, A | `mi.vertexColor` | G3 |
| 20 | CameraPosition | 座標 | → F3 | b1 `cameraPos` | G3 |
| 21 | CameraVector | 座標 | → F3 | `normalize(cameraPos - worldPos)` | G3 |
| 22 | Time | 時間 | → F1 | b1 `time` | G1 |
| 23 | Panner | UV | (UV, Speed, Time) → F2 | `uv + speed * time`（時間は frac 折り返しで精度維持） | G3 |
| 24 | Rotator | UV | (UV, Center, Speed) → F2 | 回転行列 | G3 |
| 25 | Add | 数学 | (A, B) → A | 次元規則 §3.2 | G1 |
| 26 | Subtract | 数学 | (A, B) → A | 同上 | G1 |
| 27 | Multiply | 数学 | (A, B) → A | 同上 | G1 |
| 28 | Divide | 数学 | (A, B) → A | ゼロ除算ガード（`max(B, 1e-6)` 指定は props） | G1 |
| 29 | Lerp | 数学 | (A, B, Alpha) → A | `lerp`。Alpha は F1 または同次元 | G1 |
| 30 | Clamp | 数学 | (X, Min, Max) → A | `clamp` | G1 |
| 31 | Saturate | 数学 | (X) → A | `saturate` | G1 |
| 32 | OneMinus | 数学 | (X) → A | `1 - x` | G1 |
| 33 | Abs | 数学 | (X) → A | `abs` | G3 |
| 34 | Power | 数学 | (Base, Exp) → A | `pow(max(base, 0), exp)` | G3 |
| 35 | Min | 数学 | (A, B) → A | `min` | G3 |
| 36 | Max | 数学 | (A, B) → A | `max` | G3 |
| 37 | Frac | 数学 | (X) → A | `frac` | G3 |
| 38 | Sine | 数学 | (X, Period) → A | `sin(x * 2π / period)` | G3 |
| 39 | Dot | ベクトル | (A, B) → F1 | `dot`（同次元） | G3 |
| 40 | Cross | ベクトル | (A, B) → F3 | `cross` | G3 |
| 41 | Normalize | ベクトル | (X) → A | `normalize`（ゼロ長ガード） | G3 |
| 42 | Append | ベクトル | (A, B) → F(n+m) | `float3(a, b)` 系。合計 4 まで | G1 |
| 43 | ComponentMask | ベクトル | (X; R,G,B,A チェック) → F1〜F3 | swizzle。範囲外成分はエラー | G1 |
| 44 | Desaturation | 色 | (Color, Fraction) → F3 | 輝度（Rec.709）へ lerp | G3 |
| 45 | BlendMode | 色 | (Base, Blend, Opacity; mode) → F3 | Multiply / Screen / Overlay / SoftLight / Darken / Lighten | G3 |
| 46 | sRGB⇄Linear | 色 | (Color; mode) → F3 | 厳密式（区分関数）。**テクスチャは SRV が sRGB 形式なのでここでは要らない**（二重変換の罠を避ける注記を UI に出す） | G3 |
| 47 | Noise | ノイズ | (Pos/UV, Scale, Octaves; mode) → F1 | Value / Gradient / Fbm。`UnoCustom.hlsli:149-165` の `UnoNoise/UnoFbm` を共通化。オクターブは定数 | G3 |
| 48 | Voronoi | ノイズ | (UV, Scale, Jitter) → F1(距離), F2(セル) | 3×3 近傍探索 | G3 |
| 49 | Checker | ノイズ | (UV, Scale) → F1 | 市松 | G3 |
| 50 | Fresnel | シェーディング入力 | (Normal, Exponent, BaseReflect) → F1 | `UnoFresnel`（`UnoCustom.hlsli:167-170`）と同式。Normal 未接続は幾何法線 | G3 |
| 51 | ReflectionVector | シェーディング入力 | (Normal) → F3 | `reflect(-V, N)` | G3 |
| 52 | NormalFromHeight | シェーディング入力 | (Height, Strength, Texel) → F3(接空間) | 3 サンプル差分。勾配は `ddx/ddy` を使うので **G6 の resolve では非対応フラグ**を立てる | G3 |
| 53 | BlendNormals | シェーディング入力 | (A, B; mode) → F3 | 角度補正ブレンド（whiteout / RNM） | G3 |
| 54 | If | 制御 | (A, B, Equal, Greater, Less; Epsilon) → A | 両枝評価の `select`（§3.3） | G3 |
| 55 | Custom | 制御 | (In0..In7; 出力型, 本文) → A | **HLSL 本文を関数に包む**（`#line` でエラーを本文へ）。入力名は `In0..7` 固定、`Tex2D` 入力は `Texture2D` として渡す。UE Custom の写像先でもある | G3 |
| 56 | Reroute | 構造 | (X) → X | 型を引き継ぐ 1 点。IR では消える（ノードは UI のみ） | G0 |
| 57 | FunctionCall | 構造 | （関数の入力に従う）→（関数の出力） | `.dxmf` をインライン展開（§4.3）。再帰禁止 | G4 |
| 58 | FunctionInput | 構造 | → A（既定値付き） | 関数内のみ。プレビュー値 | G4 |
| 59 | FunctionOutput | 構造 | (X) → | 関数内のみ | G4 |
| 60 | MaterialOutput | 出力 | §3.4 のピン群 | 各ピン → `s.xxx`。未接続は `UnoSurfaceDefault()` の値 | G1 |

> コメントボックス（グループ）はノードではなくグラフの注釈（`comments`）として持つ（IR に影響しない）。`StaticSwitchParameter` は G4（インスタンスごとに別 HLSL = 別パーミュテーションになるため §4.2 で扱う）。**次の候補**（60 に入らなかったもの）: Cosine / SquareRoot / Floor / Ceil / Step / SmoothStep / Distance / Length / Fmod / DDX / DDY / SphereMask / HueShift / Ramp / Gradient / VertexInterpolator / ScreenPosition / ObjectScale / SceneDepth（深度 SRV がフォワードに無いため要エンジン側追加）。

### 3.8 コンパイル時間対策

**現状**: 生成物は `ps_6_6` でライティング込みの PS 全体（クラスタライト・影・DDGI・デカール…）を毎回コンパイルする。**同期でメインスレッド**（C2）、PSO 生成も同期（C3）。DXC のコンパイル時間・ドライバの PSO 生成時間はどちらも**未計測**（[推定] 数百 ms〜数秒。G2c の実測ゲートで判定）。

**対策（効く順）**:

1. **定数のスロット化（§3.6）**: 値の編集（Constant / Parameter / テクスチャ差し替え / インスタンス値）は**プール書き込みだけ**。再コンパイルは「ノード / ワイヤ / プロパティ（型・モード・オクターブ数）の変更」のときだけ。ユーザーの操作の大半（スライダー調整）は 0 ms。
2. **ハッシュキャッシュ**: `key = hash(生成 HLSL テキスト, ForwardShade/ForwardGraph/Lighting 等の include 群のハッシュ, define, DXC バージョン)`。`<project>/.cache/matgraph/<key>.dxil` に保存。**値がスロットなので同じ構造なら同じ HLSL** = ヒット率が高い（コピーした材質・インスタンス・再オープン）。
3. **非同期**: グラフ専用のワーカースレッド（**専用の `ShaderRuntimeCompiler` インスタンス**。`MaterialPreviewRenderer` も専用インスタンスを持つ前例 E2）。世代番号を付け、古い結果は捨てる。編集後 **250 ms のデバウンス**。
4. **PSO 生成もワーカー**（D3D12 デバイスはフリースレッド）。まず `less` の 1 種類だけ作って反映し、残り 3 種（lequal / ブレンド系）は必要になったときに遅延生成。**旧 PSO は数フレームの「墓場」に置いてから解放**（`DeferredRelease` はリソース用なので PSO 用に別途。C2 の `WaitIdle` を通さない）。
5. **旧版維持**: 新しいコンパイルが終わるまで旧 PSO で描き続ける（現行 `ShaderManager.cpp:484-487` と同じ思想）。失敗しても絵が壊れない。
6. **段階コンパイル（スパイク）**: 編集中は `-Od`（最適化なし）で先に出し、静止 1 秒後に通常最適化版へ差し替える。効果は要実測。
7. **ライブラリ分割（スパイク、G2c で go/no-go）**: ライティング尾部を事前コンパイルした `lib_6_x` にして `IDxcLinker` で材質評価だけをリンクする。材質側のコンパイル量が桁で減る見込み [推定] だが、SM6.6 のバインドレス + リンクの相性は**未検証**。駄目なら 1〜6 だけで出荷。
8. **プレビューは軽量シェーダー**（§3.9）。ライティングが小さい（`MaterialPreview.hlsl` 相当）ので DXC が速い。編集中の「見え」はプレビューが担い、シーンへの反映は 3〜5 の非同期。
9. **配布**: ゲームモードにコンパイラは無い（C7）。ビルド時に全 `.dxmg` から HLSL を再生成 → 既存の `RecompileAllForBuild` が pak へ焼く。壊れたグラフがあればビルドを失敗させる（出荷しない）。

**目標**（G2c の合否）: プレビュー再コンパイル p50 ≤ 300 ms / p95 ≤ 1 s、シーン反映（PSO 差し替え）p95 ≤ 3 s、その間のフレーム時間 p99 が通常の 1.2 倍以内（固まらない）。

### 3.9 リアルタイムプレビュー

- **既定 = 専用軽量パイプライン**（`MaterialPreviewRenderer` の流用）。`MaterialPreview.hlsl` と同じ小さい RS・固定ライト・LDR 出力に、`UnoMatEval` を差し込む形（`shaders/forward/GraphPreview.hlsl`）。専用 RS は自由にできるので `HEAP_DIRECTLY_INDEXED` を足す。メッシュは**球 / 平面 / 円柱 / 立方体 / シーンで選択中のメッシュ**（`Mesh` を直接受ける）。ライト方向のドラッグ回転、背景（暗 / 明 / チェッカー）、解像度（256 / 512）。
- **シーンビューでの本番プレビュー**: 「選択中のエンティティへ仮適用」トグル（Undo 対象外の一時状態）。実際のフォワード PSO（§3.8 の非同期）で見る。
- **ノード単位プレビュー**（UE の Preview ノード）: 右クリック「このノードを表示」で、選択出力を BaseColor に流した一時グラフをプレビュー専用にコンパイル（G3）。
- **サムネイル**: 既存の 128px ディスクキャッシュ（E2）に相乗り。グラフ材質もアセットブラウザで球サムネイルが出る。
- 不足時: グラフが未コンパイル / エラーのときは**直前の成功画像を薄く表示 + 「エラー」バッジ**（消えない）。

### 3.10 エラー表示（どのノードのどの入力が何故失敗か）

`struct Diagnostic { severity, code, nodeId, pin (任意), message, hint }`。4 段階で集める:

1. **接続時**: 型が合わない・循環になる接続は**拒否**（ワイヤが赤く落ち、ツールチップに理由。`Tex2D → F3` は「テクスチャを float に繋げません。TextureSample を通してください」）。
2. **検証 / 型推論**: 未接続の必須ピン（既定値が無いもの）、次元不一致（「Multiply: A=float3, B=float2。次元が違います」）、切り詰め警告、関数再帰、未対応ノード（UE 由来）、`ObjectParameter` の 8 float 超過。
3. **DXC エラー**: 出力に `#line N "node:<id>"` が入っているので、エラーメッセージの `file:line:col` を `sourceMap` で **nodeId** に逆引き。Custom ノードの本文エラーは本文の行に対応。
4. **PSO / RS エラー**: 既存の `shaderdiag::ExplainPsoFailure`（C3）の文面をそのままエラーリストに表示（「使えない register」を名指し）。

**表示**: ノードに赤（エラー）/ 黄（警告）の縁 + 右上バッジ、該当ピンを強調、ホバーで文面。下部の**エラーリスト**（クリックでそのノードへジャンプして選択）。ステータスバー: `✓ 生成OK・DXC 132 ms・PSO 41 ms` / `⟳ コンパイル中…（旧版で表示中）` / `✗ 3 件`。MCP からも同じ `Diagnostic[]` を JSON で返す。

### 3.11 VG 設計書との整合（コード生成に効く 3 点）

1. **サンプルはマクロ経由**（§3.5）。VG の resolve は可視性バッファ方式で隣接画素が別三角形になり `ddx/ddy` が壊れるので `SampleGrad` + 解析 UV 勾配（VG 設計書 §2.2.6 `:162-165`）。生成コードは `UnoSample` → resolve 文脈で `SampleGrad(…, mi.dUVdx, mi.dUVdy)`。UV を加工するノード（Panner / Rotator / TexCoord のスケール）を通った UV の勾配は、**UV 部分グラフを 3 点（画素 / +x / +y）で評価する有限差分**で得る案（G6 のスパイク）。`NormalFromHeight`・`DDX/DDY` のように画面微分そのものを使うノードは「resolve 非対応」フラグを持ち、該当グラフは NONVG（従来経路）へ縮退。
2. **シェーディング尾部の共有**（`ForwardShade.hlsli`）。VG の resolve PS は Forward の PS 本体を**複製**する計画（VG 設計書 `:159`）だが、外出しした関数を include すれば複製が要らなくなる。
3. **RS フラグ**は共通（§3.6）。VG 設計書は「メインと同一パラメータ + フラグの**別オブジェクト**」（`:159,:182-189`）だが、フォワードパスは 1 個の RS で通すため、**グラフ材質の PSO をメイン RS で作る以上、メイン RS 自体にフラグを立てる方が単純**（別オブジェクトだと PSO ごとに RS が違いパス途中で張り替えが要る）。G2a のゴールデン + ベンチでフラグの副作用が無いことを確認し、あれば別オブジェクト案へ撤退（§8）。

---

## 4. データ・互換・MCP

### 4.1 ファイル形式

| 拡張子 | 役割 | 置き場所 | 備考 |
|---|---|---|---|
| `.dxmg` | マテリアルグラフ（**親**。グラフ本体 + 設定 + レイアウト + コメント） | `assets/materials/**` | JSON、安定 ID、キーは辞書順 |
| `.dxmat` | 既存。**インスタンス**（親グラフへの参照 + パラメータ上書き）**または** 旧来のテクスチャ 4 枚 | `assets/materials/**` | `graph` キーの有無で分岐（下記） |
| `.dxmf` | マテリアル関数（再利用サブグラフ） | `assets/materials/functions/**` | `.dxmg` と同スキーマ + `FunctionInput/Output` |
| `_graph/*.hlsl` | 生成物（**コミットしない**。エディタが保存時に再生成、BuildGame 前にも再生成） | `assets/shaders/_graph/` | `.gitignore` 推奨 |

**`.dxmg`**（1 ノード 1 エントリ、キーは辞書順、`layout` は 1 ノード 1 行の文字列で**ドラッグしても 1 行しか差分が出ない**）:

```json
{
  "version": 1,
  "kind": "material",
  "guid": "3f9a1c2e",
  "name": "rock_wet",
  "settings": { "blendMode": "Opaque", "shadingModel": "DefaultLit", "twoSided": false },
  "nodes": {
    "n_071829": { "type": "TextureSample", "in": { "Tex": "n_d4e5f6.Out", "UV": "n_a1b2c3.UV" } },
    "n_9a8b7c": { "type": "ScalarParameter",
                  "props": { "name": "Roughness", "default": 0.6, "range": [0, 1], "group": "Surface", "priority": 1 } },
    "n_a1b2c3": { "type": "TexCoord", "props": { "tiling": [2, 2] } },
    "n_d4e5f6": { "type": "TextureParameter",
                  "props": { "name": "Albedo", "default": "textures/rock/rock_diff.jpg", "sampler": "Color", "group": "Textures" } },
    "out":      { "type": "MaterialOutput", "in": { "BaseColor": "n_071829.RGB", "Roughness": "n_9a8b7c.Out" } }
  },
  "comments": { "c_11": { "text": "岩の色", "color": "#3a6", "rect": "-560 -20 380 260" } },
  "layout": { "n_071829": "-140 30", "n_9a8b7c": "-140 260", "n_a1b2c3": "-380 60", "n_d4e5f6": "-380 -100", "out": "160 40" }
}
```

- **安定 ID**: ノード `n_` + 6 桁 hex（作成時にランダム、**再採番しない**）。ピン名は型定義に固定（`A`, `B`, `UV`, `RGB`…）。接続は**入力側に持つ**（`"in": { "A": "n_x.Out" }`）ので、接続の変更は 1 ノードの 1 行だけ。未接続の入力に**リテラル**（`"B": 0.5` / `"B": [1,0,0]`）を書けるので、Constant ノードを置かない小さなグラフが自然に書ける（MCP にも書きやすい）。
- **git 差分**: 上記 + `dump(2)`（nlohmann は辞書順）。`layout` は `"x y"` の文字列にして 1 行。**保存は内容が変わらなければ書かない**（mtime を汚さない）。
- **バージョニング**: `version` を上げたときは読み込み側がマイグレーションを持つ（旧版を読めなくしない）。

**`.dxmat`（インスタンス）**:

```json
{
  "version": 2,
  "name": "rock_wet_mossy",
  "graph": "materials/rock_wet.dxmg",
  "params": { "Roughness": 0.8, "Albedo": "textures/moss/moss_diff.jpg", "Tint": [0.6, 0.7, 0.5, 1.0] },
  "uvTiling": [1.0, 1.0]
}
```

### 4.2 マテリアルインスタンス（親 + パラメータ上書き）

- 上書きできるのは **パラメータ（Scalar / Vector / Texture）と、`Inline` でない Constant の値**。**構造は変えられない**（それが親を編集する意味）。ここで再コンパイルは起きない（§3.6 プール書き込み）。
- **インスタンスの親がインスタンス**（多段）は許す。解決は「子 → 親 → … → グラフ既定値」の順。循環はエラー。
- **パラメータ名はグラフ内で一意**（UE と同じ流儀）。グラフでの名前変更は「参照している `.dxmat` を走査して書き換える」補助を UI に付ける（名前が違うと上書きが黙って無効になる事故を避ける。無効な上書きは**インスタンスに警告表示**）。
- **StaticSwitch（G4）**: Bool パラメータは HLSL の分岐になるので、値ごとに **別パーミュテーション（別 HLSL・別 PSO）**。インスタンスが持つ組み合わせだけを生成（親の全組み合わせは作らない）。パーミュテーションの上限は 16 / グラフ（超過はエラー）。
- **エンティティ単位の上書き**: 同じレコード形式（コピーオンライト）。Lua `scene:setMaterialParam(entity, name, value)`・MCP `dx12_set_material_param`。シーン JSON は `"materialParams": { … }`（**キーが無ければ従来と同一**）。

### 4.3 マテリアル関数

- `.dxmf` = `kind: "function"` の `.dxmg`。`FunctionInput`（名前・型・既定値・プレビュー値）と `FunctionOutput` を持つ。
- **展開はインライン**（UE と同じ。呼び出し箇所ごとに複製）。ノード ID は「呼び出しノード ID + 関数内ノード ID」の経路つきにして、エラーは呼び出し側のノードにも映す。
- **再帰禁止**（関数呼び出しグラフの循環検出 + 深さ上限 16）。
- 関数内のパラメータは呼び出し元マテリアルのパラメータ一覧へ**昇格**（キー = 呼び出し経路つき ID、表示名は元のまま + グループ）。
- 関数の編集は、それを呼ぶ全マテリアルの再コンパイルを起こす（依存グラフを `ShaderManager` 側の再コンパイル要求に乗せる。ヘッダ依存の追跡と同じ仕組みを流用）。

### 4.4 後方互換（既存 `.dxmat` は壊さない）

| ケース | 挙動 |
|---|---|
| 旧 `.dxmat`（`graph` キー無し、`version:1`） | **完全に今と同じ経路**（`MaterialAssetManager::LoadInto` の 4 枚 SRV + b2）。読み → 書き戻しで 1 バイトも変わらない（M3。往復テストで機械判定） |
| `graph` キーあり | グラフ経路（§3.5・§3.6）。`version:2` |
| 検証（M2）: 「テクスチャ 1 枚も参照しないと無効」 | `graph` があれば有効とする（旧ファイルの判定は不変） |
| シーン JSON | `"materialAssets"` は今のまま（M5）。グラフ材質も `.dxmat` パスを書くだけ |
| 非対応 GPU（R3）でグラフ材質 | 警告 + 標準マテリアルへ縮退（黒 / 紫にしない） |
| VG / RT | v1 は NONVG + RT はプロキシ（§7 G6） |

**アップグレード（任意・手動）**: `.dxmat` の右クリック「グラフ化」で、**標準 PBR テンプレートグラフ**（`TexCoord × uvTiling → TextureSample(albedo) → BaseColor`、`NormalMap → Normal`、`TextureSample(MR).G × roughness → Roughness`、`.B × metallic → Metallic`、`emissive × 色 × 強度 → Emissive`）を `.dxmg` として生成し、元の `.dxmat` を **そのグラフのインスタンス**（`graph` + `params`）に書き換える。**元ファイルは `.dxmat.bak` に退避**。合否は「グラフ化前後のスクショ差 ≤ 1/255（平均）」をゴールデンで判定（G2b）。自動一括変換はしない（既存資産に触らない）。

### 4.5 MCP からの操作

**既存との関係**:
- `dx12_set_pbr`（エンティティの PBR 上書き）・`dx12_set_texture`・`dx12_material_apply`（ディレクトリから 4 点セット割当、TS 側 `tools/mcp-server/materialApply.ts`）は**そのまま**（`.dxmat` 旧経路 / エンティティ上書き）。グラフ材質のエンティティでは `set_pbr` の metallic / roughness 上書きは無視される旨を警告で返す。
- `dx12_blender_material`（Blender → ポリヘイブン等のテクスチャ → 割当）も不変。将来「Blender のノードツリー → Uno グラフ」変換（Principled BSDF の 5 入力を写す）は G5 の写像基盤の流用で可能（任意）。
- `dx12_create_shader` / `dx12_set_mesh_shader`（手書きカスタムシェーダー）は**共存**。生成物は `_graph/` 下で見えない扱い。

**新規 `dx12_material_graph_*`（G3）**:

| ツール | 内容 |
|---|---|
| `dx12_material_graph_create` | 空 / テンプレート（標準 PBR・アンリット・ディゾルブ・水…）から `.dxmg` を作る |
| `dx12_material_graph_describe` | グラフをテキスト（ノード一覧 + 接続）と JSON で返す（AI が読む用） |
| `dx12_material_graph_edit` | `ops` 配列で一括編集（`add` / `remove` / `connect` / `disconnect` / `set` / `move`）。**1 呼び出し = 1 つの AI Undo エントリ**（E4 の横取り）。`add` の `id` はクライアント指定可（後続 op が参照できる） |
| `dx12_material_graph_compile` | コンパイルして `Diagnostic[]`・統計（DXC ms / PSO ms / テクスチャ数 / パラメータ数）を返す。`wait:true` で PSO まで待つ |
| `dx12_material_graph_nodes` | ノード型カタログ（検索語つき。AI が「何が置けるか」を引く。`describe_shader_contract` の流儀） |
| `dx12_material_graph_apply` | エンティティ / サブメッシュへ割当（`materialAssets` への書き込み） |
| `dx12_material_graph_preview` | プレビュー PNG（`imgui_screenshot` ではなくプレビュー RT の直接読み出し。入力注入なし） |
| `dx12_set_material_param` | インスタンス / エンティティのパラメータ上書き（Scalar / Vector / Texture） |

- **導入の作法**（memory `dx12-mcp-traps` / `lua-api-checklist`）: `ApplicationMcpManifestData.inc` の行 + `tools/mcp-server/`（TS スキーマ・`catalog.ts` の分類・`coreSpec.ts`）+ `docs/MCP.md` + `tests/mcp_manifest_test.cpp` / `mcp_param_spec_test.cpp` を**同時に**更新する。TS スキーマの反映には Claude Code の再起動が要る。Lua API（`setMaterialParam`）を足すなら MCP 辞書 `McpLuaApi()` + リファレンス 3 点（`API_REFERENCE.md` / `SCRIPTING.md` / `index.html`）+ 予測変換の確認まで（memory `lua-api-checklist`）。
- **AI が書きやすい形**: 未接続入力へのリテラル直書き（§4.1）、`describe` の出力が `edit` の入力と同じ語彙、エラーは `nodeId` + `pin` 付きで返す。

---

## 5. UE マテリアルの取り込み

### 5.1 前提: cook 済みにはグラフが無い

- **cook 済みパッケージ**（Steam 版 Dreamcore 等）には式ノードのグラフが残らない。実際に Dead Mall の抽出で「マテリアルグラフは cook 時に落ちている。単色マテリアル（約 4 割）は色そのものが取り出せない」と確認済み [事実: memory `dreamcore-dead-mall` §「移植で必ず踏む 4 つ」#4]。取れるのは `UMaterialInstanceConstant` の**パラメータ値・親チェーン・参照テクスチャ**（`CachedExpressionData` 系）まで [記憶: 着手時に CUE4Parse のクラスで確認]。
- **未 cook**（UE エディタのプロジェクトの `.uasset`。`Documents\Unreal Projects` が VG 設計書 P6 でも言及されている）には `UMaterial.Expressions`（`MaterialExpression*` の配列）が**残っている** [記憶]。読み出しは cook 済みと違い unversioned properties でなく tagged properties なので `.usmap` 無しで読める可能性が高い [記憶]。

→ **取り込みは 3 段（T0 → T1 → T2）**で設計する。環境は VG 設計書 P6 と共通（`tools/ue-cook/`、net10 / CUE4Parse、`C:\Users\ryuto\.dotnet10`、`EGame.GAME_UE5_6`、jmap の `.usmap`）。

### 5.2 T0: インスタンス + パラメータ + テクスチャ（最初にやる。cook 済みでも可）

1. `UMaterialInstanceConstant` を読み、**親チェーンを辿って**（Dead Mall の教訓: `materialDepth = AllLayers`、`TopLayerOnly` は親を辿らない）`ScalarParameterValues` / `VectorParameterValues` / `TextureParameterValues` を最終値へ解決。
2. **標準 PBR グラフ**（§4.4 のテンプレート）のインスタンスとして `.dxmat` を出力。パラメータ名の写像は**名前の接尾辞 / 語彙で判定**（Dead Mall で `T_..._D` 等のテクスチャ名キーに入っていた教訓）:
   `BaseColor / Color / Albedo / Diffuse / _D → Albedo`、`Normal / _N → Normal`、`ORM / ARM / Packed / _ORM → MR（G=roughness, B=metallic）`、`Roughness / Metallic / Emissive*` → スカラー、`Tiling / UVScale → uvTiling`。
3. 写せなかったパラメータは**捨てずに**インスタンスの `"ueImport": { "unmapped": { … } }` に保存（後で手でグラフに繋ぐための素材）。
4. テクスチャは VG 設計書 P6 と同じく **cook 済み BC7 / BC5 を再圧縮せず DDS へパススルー**。
5. VG 設計書 P6 の `MaterialRecord`（96B）は **基本 PBR 4 スロット**のまま（VG 側は v1 で NONVG のグラフを扱わない）。グラフ材質を使うメッシュへは `.dxmat` パスを持たせる拡張（`graphPathOff`、予約 4B を使用）を G6 で足す。

### 5.3 T1: 未 cook `.uasset` の式ノード写像

**写せる（直接 / 近似）** — 最初の対象（UE クラス名は [記憶]。`MaterialExpression` 接頭辞）:

| UE 式 | Uno ノード | 判定 |
|---|---|---|
| Constant / Constant2Vector / Constant3Vector / Constant4Vector | Float / Float2 / Float3 / Float4 | ✔ 直接 |
| ScalarParameter / VectorParameter | ScalarParameter / VectorParameter（group・priority も写す） | ✔ |
| TextureSample / TextureSampleParameter2D / TextureObject(Parameter) | TextureSample / TextureParameter（サンプラー種別は `SamplerType` から） | ✔ |
| TextureCoordinate | TexCoord（`UTiling/VTiling`、`CoordinateIndex` は 0 のみ。>0 は警告） | ✔ / △ |
| Add / Subtract / Multiply / Divide / Abs / Power / Frac / Min / Max / Saturate / Clamp / OneMinus / Sine | 同名 | ✔ |
| LinearInterpolate | Lerp | ✔ |
| ComponentMask / AppendVector / DotProduct / CrossProduct / Normalize | 同名 | ✔ |
| Time / Panner / Rotator / Fresnel / Desaturation / If / Noise | Time / Panner / Rotator / Fresnel / Desaturation / If / Noise | ✔ / △（Noise は種類が多く近似） |
| WorldPosition / CameraVectorWS / ReflectionVectorWS / VertexColor / VertexNormalWS / ObjectPositionWS | 同名 | ✔（`WorldPosition` の絶対 / 相対の区別は警告） |
| NormalMap 相当（TextureSample の SamplerType=Normal → 出力 Normal） | NormalMap | △（UE は正規化済み法線をそのまま使う設定がある） |
| MaterialFunctionCall | **関数を再帰的に読み `.dxmf` に変換**して FunctionCall へ | △（写像率に従属） |
| StaticSwitchParameter / StaticBool | StaticSwitch（G4 以降） | △ |
| Custom | Custom（本文をそのまま。UE 固有マクロ / 関数は警告） | △ |
| Reroute / Comment | Reroute / コメントボックス | ✔ |
| 出力（BaseColor / Metallic / Roughness / Normal / EmissiveColor / AmbientOcclusion / Opacity / OpacityMask） | MaterialOutput の同名ピン | ✔ |
| Specular / Anisotropy / SubsurfaceColor / Opacity(Subsurface) / Refraction / WorldPositionOffset / PixelDepthOffset / ShadingModel | **予約ピンへ写して灰色表示**（値は保持、描画には効かない） | △（将来対応） |

**写せない（UE 固有）→ T2 フォールバック**: SceneTexture / SceneDepth / PixelDepth / DepthFade / DistanceFieldGradient / SkyAtmosphere* / Landscape* / VertexInterpolator / PreSkinnedPosition / VirtualTexture* / RuntimeVirtualTexture* / Nanite 固有 / Substrate 関連 / Niagara 関連など。

**T2**: 未対応式は `UnsupportedNode` プレースホルダー（**元のクラス名・ピン数・位置・入力接続を保持**、出力は定数 0 / (0,0,0) で代替）に置き換えて**グラフ全体は生かす**（lenient モード）。ノードは赤縁 + 「UE 固有: `<クラス名>`。手で置き換えてください」。**目標**: 主要ノードで見た目が近似できること。写像率は **[推定] 60〜80 %**（Megascans / 標準 PBR 系は高く、大気・深度依存・ランドスケープ系は低い）。実数は G5 のスパイクで、**手元の未 cook プロジェクト**のマテリアル N 個に対して「全式のうち写せた割合」「グラフとして生成できた割合」を測って決める。

### 5.4 撤退条件

- 未 cook の `.uasset` が手元に無い / 取り込み元を用意できない → **T0 だけ出荷**（cook 済みで動く。Dead Mall 系で有用）。
- T1 の「グラフとして最後まで生成できた割合」が 40 % 未満 → T1 は「参考グラフ生成（手で仕上げる前提）」と位置づけ、UI で「近似」バッジ。
- VG 設計書 P6 のスパイク（Nanite ページのデコード可否）と**環境を共有**するので、CUE4Parse の環境構築失敗は両方を止める → G5 は P6 のスパイク完了を待つ。

---

## 6. UI/UX 設計（見た目はトークン依存）

### 6.1 画面レイアウト

```
┌ マテリアルグラフ: rock_wet.dxmg ──────────────────────────────────────────────┐
│ [保存] [▶適用] [グラフ化▾]  コンパイル: ✓ 132ms   ⌕ 検索(Ctrl+F)   ⚙        │
├───────────┬──────────────────────────────────────────┬─────────────────────┤
│ パレット  │  グラフ（キャンバス）                      │ プレビュー          │
│ ⌕ 検索    │   ┌TexCoord┐    ┌TextureSample─┐         │  ◯ 球 ▢ 平面 …     │
│ ▾定数     │   │ UV  ●──┼──●│UV        RGB ●┼─╮       │  [3D ビュー]        │
│ ▾テクスチャ│   └────────┘    └──────────────┘  ╰─●BaseColor│                │
│ ▾数学     │                      ┌MaterialOutput─┐     ├─────────────────────┤
│ ▾…        │  [ミニマップ]        └───────────────┘     │ 詳細                │
│ (D&D/dbl) │                                            │ 選択ノードの属性 /   │
├───────────┴──────────────────────────────────────────┤ グラフ設定 /         │
│ エラー / 警告リスト（クリックでノードへジャンプ）  統計 │ パラメータ一覧       │
└──────────────────────────────────────────────────────┴─────────────────────┘
```

- **ドッキング可能**（`ToolWindows.h` の表に `DockSlot::Center` / 既定 Floating で登録。既存マテリアルエディタと違い `NoDocking` を付けない）。窓が狭いときはパレット・詳細を折りたたみ。
- 既存マテリアルエディタ（`.dxmat` 旧経路）は**残す**。`.dxmat` を開くと `graph` の有無で旧エディタ / インスタンスエディタ（パラメータ一覧のみ + 親グラフを開くボタン）へ分岐。アセットブラウザで `.dxmg` をダブルクリックでグラフ窓。

### 6.2 見た目トークン（別エージェントの 3 案へ追従できる作り）

**グラフの色・寸法は 1 個の構造体 `GraphTokens` に集約**し、`theme::` のトークンから**関数で導出**する（`GraphTokens MakeGraphTokens(const theme::…)`）。ノードエディタのコードに 16 進値を直書きしない。

| トークン | 導出元（既定） | 用途 |
|---|---|---|
| `canvasBg / gridMinor / gridMajor` | `Bg0` を基準に明度差 | キャンバス背景・グリッド |
| `nodeBody / nodeBodyHover / nodeBorder` | `Bg2 / Bg3 / Border` | ノード本体 |
| `nodeSelected / nodeHover` | `Accent / AccentHover` | 選択・ホバー枠 |
| `headerCat[10]` | 種別色（`Type*`）を再利用 + 不足分は `Accent` の色相回転 | ヘッダ（カテゴリ別: 定数 / パラメータ / テクスチャ / 座標 / 数学 / ベクトル / 色 / ノイズ / 入力 / 制御・構造・出力） |
| `pinType[]` | 型ごと（F1〜F4 / Tex2D / Bool / 関数 / 予約） | ピン・ワイヤ色 |
| `wireAlpha / wireWidth` | 0.85 / `Px(2)` | ワイヤ |
| `glowStrength / glowRadius` | **テーマの `neon` スカラー**（0 = グロー無し、UE 風テーマ）| 選択・ホバー・エラーの発光 |
| `errorGlow / warnGlow / compilingShimmer` | `Bad / Warn / Accent` | 状態表示 |
| `nodeRound / headerH / pinRadius / rowH` | `Px()` 経由の論理寸法 | 寸法 |

- **3 案への追従**: 各案が `Bg* / Accent / Good / Warn / Bad / Type*` を差し替えるだけで、グラフ全体の色が変わる。**ネオンの強さだけは新トークン**（`theme::neon`、既定 0）を 1 個足してもらう（別エージェントに要依頼、§9-5）。それが無い場合は `Accent` から `glowStrength = 0.6` を既定生成。
- 高コントラスト / 色覚配慮: 型は**色 + 形**の二重符号化（§6.3）。

### 6.3 ノード・ピン・ワイヤの見た目

- **ノード**: 角丸矩形。ヘッダ = カテゴリ色（左端に細いアクセントバー + アイコン + 名前）。本体 = 入力ピン（左）/ 出力ピン（右）を行で並べ、未接続入力はインライン値欄（float ドラッグ・色見本・チェック・列挙）。`TextureSample` / `TextureParameter` は**サムネイル付き**（プレビュー小窓、ズーム 0.6 以上で表示）。ノード幅は内容に合わせて自動、最小幅あり。
- **ピン形状 = 型**: F1 = 円、F2 = 円 + 内側線 1、F3 = 円 + 内側線 2（または三角）、F4 = ダイヤ、Tex2D = 角丸四角、Bool = 小四角、関数入出力 = 五角形（**形と色の両方で区別**）。接続済み = 塗り、未接続 = 輪郭のみ。互換でない型へドラッグ中は他ピンを暗くし、接続可能なピンだけ光らせる。
- **ワイヤ**: 出力ピン側の型色。3 次ベジェ（水平接線、長さに応じて張り）。多相型は上流から推論した型色。**選択ノードの上流・下流を強調**（それ以外を減光）。ドラッグ中の仮ワイヤは点線。**グロー**は 2〜3 パス（太い低アルファ → 細い明部）、対象は選択 / ホバー / 状態のあるワイヤだけ。
- **状態演出**: 選択 = アクセント枠 + 外側グロー / ホバー = ヘッダ明度アップ / **コンパイル中** = 出力ノードのヘッダにゆっくり流れる光（`compilingShimmer`）/ **エラー** = 赤縁 + 脈動 + バッジ / 警告 = 黄縁 / 死んだノード（出力に繋がらない）= 減光 / 予約ピン = 灰色 + 「将来」ツールチップ。演出は `time` 駆動の純関数で、テスト時は `time = 0` に固定して**決定論スクショ**（既存の決定論スクショの流儀）。
- **コメントボックス**: 半透明の矩形 + 見出し。ドラッグで中のノードごと移動、リサイズ可、色 6 種。ズームが小さいと見出しを拡大表示。
- **リルート**: ワイヤのダブルクリックで挿入（小さい円）。

### 6.4 操作

| 操作 | 内容 |
|---|---|
| 接続 | 出力ピン → 入力ピンへドラッグ。入力ピンを掴んで別へ移すと**付け替え**。空き地でドロップ → **その位置に検索パレット**（接続元の型で絞り込み、選ぶと自動接続） |
| 切断 | 入力ピンを掴んで空き地へ / **Alt + クリック**でピンのワイヤを全切断 / ワイヤ上で右クリック → 削除 / **Ctrl + ドラッグで横切った全ワイヤを切断** |
| 選択 | クリック / Ctrl+クリックで追加 / **矩形選択** / Ctrl+A / 「上流を選択」「下流を選択」 |
| 移動・整列 | ドラッグ / 矢印キー / 整列（左 / 上 / 等間隔）/ グリッドスナップ（Shift で解除）|
| パン・ズーム | **中ボタン / Space + ドラッグ / 右ドラッグ**でパン、ホイールでカーソル中心ズーム（0.25〜1.5）、`F` = 選択にフィット、`Home` = 全体 |
| 追加 | 右クリック / 空き地ダブルクリック → 検索パレット（あいまい検索 = 既存 `editor/FuzzyMatch.h`、日本語名・英名・別名で引ける）/ 左パレットからD&D / **UE 風ワンキー**（`1/2/3/4`=定数、`S`=ScalarParameter、`V`=VectorParameter、`T`=TextureSample、`U`=TexCoord、`A/M/L/D`=Add/Multiply/Lerp/Divide、`P`=Panner、`N`=Normalize、`I`=If、`O`=OneMinus、`E`=Power ＋ **クリックした位置に置く**）[UE のキー割当は記憶。設定で変更可] |
| コメント | `C` = 選択をコメントで囲む / ダブルクリックで見出し編集 |
| クリップボード | Ctrl+C / Ctrl+V / Ctrl+D。**JSON テキスト**としてコピー（別ウィンドウ / 別グラフ / チャットにも貼れる）。貼り付け時は ID を再採番し、接続はコピー範囲内だけ保持 |
| 取り消し | Ctrl+Z / Ctrl+Y（**グラフ窓にフォーカスがあるときはグラフの Undo**、それ以外はシーンの Undo。§6.5）|
| コマンドパレット連携 | `window.materialgraph`（窓を開く）は `ToolWindows.h` の表から自動。加えて Ctrl+K に「マテリアルグラフ: ノードを追加 / 選択ノードをプレビュー / グラフ化 / コンパイル」を登録（`EditorCommandTable.h` の `KeyMode::Panel` 分類）|
| ジャンプ | エラーリスト / 検索結果クリック → ノードへ移動して選択。関数ノードのダブルクリック → 関数グラフへ（パンくず表示）|

### 6.5 Undo / Redo

- **グラフ文書ごとに `UndoSystem` を 1 個持つ**（E4）。コマンド: `AddNodes` / `RemoveNodes`（接続ごと）/ `Connect` / `Disconnect` / `SetProp`（**連続ドラッグは 1 個に併合**）/ `MoveNodes`（同）/ `Paste` / `SetGraphSettings` / `AddComment` …。複合操作は `CompositeCommand`。
- **MCP の編集**は `McpUndoRouter` の横取りで「AI: <ラベル>」1 エントリに束ねる（既存機構。`IsAi()` で人の編集を勝手に戻さない規則も維持）。
- **未保存判定**は `EditSeq`（`UndoCore.h` の既存機構）。
- **不変条件テスト**: ランダムな操作列（追加 / 削除 / 接続 / 切断 / 値変更）を適用 → 全部 Undo → 初期状態と**正準 JSON が一致**、全部 Redo → 最終状態と一致（G0 のファズテスト）。

### 6.6 DPI と性能

- **DPI**: 全寸法を論理 px で書いて `ui::Px()` を通す（E5）。ズームは `dpiScale` と独立（`S(g) = origin + (g - pan) * zoom * dpi`）。線幅・角丸・ピン半径も `Px`。ビューポートごとに倍率が変わっても崩れない（既存規約）。
- **性能目標**: **500 ノード / 1,000 ワイヤで 60 fps（描画 CPU 時間 ≤ 3 ms）** [推定 → G0 の合否]。
  - **カリング**: ノードは画面矩形との AABB 判定、ワイヤはベジェのバウンディングボックス（端点 + 制御点）で判定。画面外は描かない。ヒットテストは一様グリッド（セル 256 単位）。
  - **LOD**: zoom < 0.6 でサムネイル・インライン値欄を省略、< 0.4 でピン文字を省略、< 0.25 でノードを塗り矩形 + 色帯だけ。ワイヤのグローは選択 / ホバーのみ。
  - **テキスト**: ノード名 / ピン名は `AddText(font, size*zoom)`（1.92 の動的フォントで再ラスタライズなし・鮮明）。同一フレームで同じ文字列の測定結果はキャッシュ。
  - **頂点数**: 1 ワイヤ ≈ 100〜150 頂点（適応分割）。1,000 本で 15 万頂点以下。ImGui の 16bit インデックス制限は `ImGuiBackendFlags_RendererHasVtxOffset` で自動分割（DX12 バックエンド対応）。
- **ミニマップ**: 右下（トグル）。ノードは色付き矩形、ビューポートは枠。ドラッグで移動。

---

## 7. 段階計画

### 7.1 一覧（1 行 / 段階）

各段階は**単体でマージ可能**（既定では絵も既存資産も変わらない）で、**ctest / ImGuiTestEngine / `--background` + スクショ**で合否が機械判定できる。

| 段階 | 内容 | 依存 | 工数（実働日） | 合否（要点） |
|---|---|---|---:|---|
| **G0** | ノードエディタ基盤（自前。モデル / IO / Undo / 描画 / 入力 / ミニマップ。マテリアル無関係） | なし | 10 | 500 ノード 60 fps（描画 ≤ 3 ms）+ Undo ファズ + 往復テスト + スクショ |
| **G1** | 型システム + コード生成（純ロジック。最小ノード約 15 種） | なし（G0 と並列） | 9 | HLSL ゴールデン 20 本 + DXC 通過 + CPU 参照実装との数値一致 |
| **G2a** | シェーディング尾部の外出し（`ForwardShade.hlsli`）+ メイン RS フラグ | なし（G0/G1 と並列） | 4 | **DXIL 同値 + ゴールデン 4 枚ビット一致 + ctest 全緑 + フラグ副作用なしのベンチ** |
| **G2b** | グラフ → 既存フォワードへ接続（`ForwardGraph`・パラメータプール・b2 読み替え・サブメッシュ PSO・`// @sm`・標準テンプレート・グラフ化） | G1, G2a | 8 | 旧 `.dxmat` とグラフ化後のスクショ差 ≤ 1/255 + 既存シーン不変 |
| **G2c** | プレビュー（専用軽量）+ 非同期コンパイル / PSO + キャッシュ + ホットリロード + エラー表示（DXC → ノード） | G0, G2b | 6 | 再コンパイル p50 ≤ 300 ms / 固まらない（p99 ≤ 1.2 倍）+ エラー逆引き |
| **G3** | ノード 60 種完成 + 検索パレット + MCP（`dx12_material_graph_*`）+ ノード単位プレビュー | G0, G2c | 12 | 60 種の HLSL ゴールデン + CPU/GPU 数値パリティ + MCP 往復 |
| **G3b** | マスク / 半透明（影・深度・速度の別エントリ）+ 両面 | G3 | 6 | 葉の影が板にならない + 深度プリパス整合（スクショ差） |
| **G4** | パラメータ UI / インスタンス / マテリアル関数 / StaticSwitch / エンティティ上書き（Lua / MCP） | G3 | 12 | インスタンス値変更で再コンパイル 0 回 + 関数展開ゴールデン + 旧資産不変 |
| **G5a** | UE 取り込み T0（インスタンス + パラメータ + テクスチャ） | G4, VG P6 の環境 | 6 | 代表 10 マテリアルの色・粗さ・テクスチャが一致（画素差基準） |
| **G5b** | UE 取り込み T1 / T2（未 cook 式ノード写像 + プレースホルダー） | G5a | 10 | 写像率の実測 + 生成グラフのコンパイル成功率 |
| **G6a** | DXR 整合（RT / DDGI 用の BaseColor プロキシ） | G3 | 4 | RT アルベド可視化（`rtAlbedo`）がグラフの色と一致 |
| **G6b** | VG resolve 整合（グラフ材質の VG 対応: 材質ごとの resolve + 勾配） | G3, VG P4 | 12 | LOD0 パリティ（VG 設計書 §5.4 と同基準） |
| G7（任意） | スキンド対応 / WorldPositionOffset / 特殊シェーディング（Subsurface / ClearCoat …） | G3b | 各 8 | — |

**合計 約 99 実働日**（G7 を除く）。**クリティカルパス**: G1（9）→ G2b（8）→ G2c（6）→ G3（12）= **35 日**（G0・G2a は並列）。G5 / G6 は G4 / G3 の後で並列。

### 7.2 各段階の詳細

#### G0 ノードエディタ基盤（10 日）

- **(a) 成果物**: `src/matgraph/`（`GraphModel` / `GraphIO` / `GraphCommands` / 最小 `NodeRegistry`）、`src/editor/matgraph/`（`GraphView` = パン / ズーム / グリッド / ノード / ピン / ワイヤ / 矩形選択 / 接続 / リルート / コメント / ミニマップ / クリップボード）、`GraphTokens`、**ダミーノード型 8 種**（定数 / 加算 / 乗算 / 出力 …。マテリアルとは無関係の「型付きグラフのデモ」）、デバッグ窓（`ToolWindows.h` に隠し登録）、テストフック（`TestPinScreenPos` 等）。
- **(b) 依存**: なし。
- **(c) 工数**: 10 日（モデル・コマンド・IO 3 / 描画 3 / 入力 3 / テスト 1）。
- **(d) 合否**:
  1. **ctest**: モデルの不変条件（循環拒否・型拒否）、`.dxmg` 往復（正準 JSON 一致）、**Undo/Redo ファズ 1,000 操作 × 100 シード**で全 Undo / 全 Redo の状態一致。
  2. **ImGuiTestEngine**: ピンからピンへのドラッグで接続 / 空き地ドロップでパレット / Alt クリックで切断 / 矩形選択 / Ctrl+Z の往復（**入力は ImGui IO への注入のみ**。OS のカーソルを奪わない）。
  3. **性能**: 500 ノード / 1,000 ワイヤの合成グラフを描いて描画 CPU 時間 ≤ 3 ms（`--background` のヘッドレス計測。GPU 無しで計測できる `ImDrawList` 段階まで）。
  4. **スクショ**（`imgui_screenshot`）: 固定シードのグラフ 3 種（小 / 大 / エラー状態）を `time=0` で撮り、ゴールデンと画素差 ≤ 閾値。
- **(e) 並列**: (i) モデル + IO + コマンド + ctest / (ii) 描画 + トークン + ミニマップ / (iii) 入力 + クリップボード + ImGuiTestEngine テスト。3 つは I/F（`GraphModel` の公開 API）を先に固めれば独立。

#### G1 型システム + コード生成（9 日）

- **(a) 成果物**: `TypeSystem`（自動キャスト §3.2・型推論）、`Compiler`（検証 → IR → CSE → スロット割当 → HLSL 生成 + `#line` ソースマップ + `Diagnostic`）、`CpuEval`（IR の CPU 参照実装）、`ParamLayout`、**ノード約 15 種**（#1-7, 9, 14, 15, 22, 25-32, 42, 43, 60 のうち）、コンソール / テスト用 CLI（`--matgraph-compile in.dxmg` で HLSL と診断を標準出力）、`ForwardGraph.hlsl` の**雛形なしで**単体検証できる「テスト用ラッパー HLSL」（`UnoMatEval` を呼ぶだけ）。
- **(b) 依存**: なし（`GraphModel` の最小 API のみ G0 と合意）。
- **(c) 工数**: 9 日（型・検証 2 / IR・CSE・スロット 3 / HLSL 生成・ソースマップ 2 / CpuEval・ゴールデン 2）。
- **(d) 合否**:
  1. **HLSL ゴールデン 20 本**（`tests/data/matgraph/*.dxmg` → `*.hlsl.expected`）が完全一致（決定論的な出力: 変数名は Op 番号順、ID 順序は辞書順）。
  2. **DXC 通過**: 全ゴールデンをテスト用ラッパー経由で `ps_6_6` コンパイル（`dxcompiler.dll` が要るので ctest のラベル `dxc` に分離し、無い環境ではスキップ）。
  3. **数値一致**: `CpuEval` と DXC コンパイル済み HLSL（WARP アダプターで 1x1 RT に評価）の出力が 1e-4 以内（数学ノード全種）。
  4. **診断**: 意図的に壊したグラフ 15 種で、期待どおりの `nodeId` / `pin` / コードが返る。DXC エラー行 → nodeId 逆引きテスト。
  5. **CSE / 死んだノード**: 重複ノードが 1 回しか評価されない・出力に繋がらないノードが生成されない、を HLSL 差分で確認。
- **(e) 並列**: (i) 型システム + 検証 + 診断 / (ii) IR + CSE + スロット / (iii) HLSL 生成 + ソースマップ + CpuEval。

#### G2a シェーディング尾部の外出し + メイン RS フラグ（4 日）

- **(a) 成果物**: `shaders/forward/UnoSurface.hlsli` / `ForwardShade.hlsli`（`UnoShadeForward`）、`Forward.hlsl` を「マテリアル評価 → `UnoShadeForward`」へ整理（`ForwardSkinned.hlsl` は別作業で寄せられる旨をコメント）、`ShaderRegistry.cpp` の `staticDeps` へ 2 ファイル追加（`ShaderIncludeDepsTests` が見張る）、`RootSignature::Initialize` の Flags に `HEAP_DIRECTLY_INDEXED`（非対応 GPU では立てない）、静的サンプラ s6.. の追加。
- **(b) 依存**: なし。**VG 設計書 P4 と共通作業**（先に着手した側が担当。§7.4）。
- **(c) 工数**: 4 日。
- **(d) 合否**（既定で絵が 1 ビットも変わらない）:
  1. **DXIL 同値**: 外出し前後で `Forward_PS.cso` の DXIL を比較（最適化後は同一になる見込み。異なれば差分を精査して「意味が同じ」ことをスクショで担保）。
  2. **`tests/golden/` の 4 枚がビット一致**（`golden.mjs` の `bitExact`）、`ctest` 全緑、`shader_include_deps_test` 緑。
  3. **フラグの副作用なし**: `perf_stats` のベンチ（`tools/bench`）でフレーム時間が ±1% 以内（RTX 5060）。差が出れば別 RS オブジェクト案へ撤退（§8 R2）。
  4. デバッグレイヤ警告 0（RS フラグ + `SetDescriptorHeaps` の順序制約）。
- **(e) 並列**: (i) シェーダー外出し / (ii) RS + サンプラー / (iii) 検証（ゴールデン・ベンチ）。

#### G2b グラフ → 既存フォワードへ接続（8 日）

- **(a) 成果物**: `ForwardGraph.hlsl`（`VSMain` / `PSMain` / cbuffer 固定 / 前方宣言）、`ShaderManager` の `// @sm 6_6` 対応と `_graph/` の Inspector 非表示、**パラメータプール**（3 フレーム複製、レコード確保 / 解放）、`MaterialAssetManager::Entry` のグラフ種別（`graphKey` / `recordBase` / `blendMode` / `valid`）、`MaterialAssetIO` の `graph` / `params` 対応（**旧ファイルはバイト一致**）、`Application::EnsureGraphPso` + **サブメッシュ単位の PSO 切替**（`ApplicationRender.cpp:750,1029-1038`）、b2 読み替え（`:1027` 付近）、`BuildGame` 前の HLSL 再生成、標準 PBR テンプレートグラフ、`.dxmat` → グラフ化（`.bak` 退避）、Translucent の `alphaClass` / `sortKey` 連動、最小の `MaterialGraphPanel`（G0 の `GraphView` を載せ、保存で HLSL 生成 + 同期コンパイル。非同期は G2c）。
- **(b) 依存**: G1（コード生成）、G2a（外出し + フラグ）。G0 は「最小パネル」のためだけ（無ければテキストエディタで `.dxmg` を書いても検証できる）。
- **(c) 工数**: 8 日（シェーダー + ShaderManager 2 / プール + レコード + IO 2 / PSO + 描画統合 2 / テンプレート + グラフ化 + BuildGame 1 / 最小パネル 1）。
- **(d) 合否**:
  1. **標準テンプレートでのパリティ**: 既存の `.dxmat`（Poly Haven 系 5 種）と、そのグラフ化版を同じシーンに置き、**スクショの平均絶対差 ≤ 1/255、画素差 > 2/255 の割合 ≤ 0.5%**（`golden.mjs` の許容差方式）。
  2. **既存シーンが不変**: `tests/golden/` 4 枚ビット一致（グラフ材質を使わない）、旧 `.dxmat` の往復テスト緑。
  3. 3 枚以上のテクスチャ + 4 パラメータのグラフが描画され、**パラメータ変更が再コンパイル 0 回**で反映される（`ShaderManager` のコンパイル回数カウンタ）。
  4. 配布ビルド: `--build` でグラフ材質のシェーダーが pak に入り、`GameRuntime` で同じ絵（スクショ差 ≤ 1/255）。
  5. 非対応 GPU 想定（`SupportsDynamicResources() == false` を強制するデバッグスイッチ）で標準マテリアルへ縮退し、黒 / 紫にならない。
- **(e) 並列**: (i) シェーダー + `ShaderManager` / (ii) プール + IO + `MaterialAssetManager` / (iii) PSO + 描画統合 + Translucent / (iv) テンプレート + グラフ化 + BuildGame。

#### G2c プレビュー + 非同期コンパイル + キャッシュ + エラー表示（6 日）

- **(a) 成果物**: グラフ専用ワーカー（専用 `ShaderRuntimeCompiler` + 世代管理 + 250 ms デバウンス）、PSO 非同期生成 + 墓場、ディスクキャッシュ（`.cache/matgraph/<hash>.dxil`）、**`GraphPreview.hlsl` + プレビューレンダラー**（`MaterialPreviewRenderer` の流用・拡張。球 / 平面 / 円柱 / 立方体 / 選択メッシュ）、`Diagnostic` のエラー表示（ノードバッジ・リスト・ジャンプ・DXC 行 → nodeId）、ステータスバー、`-Od` 段階コンパイルと `lib` 分割の**スパイク**（結果を本書 §3.8 へ追記して採否決定）。
- **(b) 依存**: G0（描画）、G2b。
- **(c) 工数**: 6 日（ワーカー / キャッシュ / PSO 2、プレビュー 2、エラー表示 1、スパイク 1）。
- **(d) 合否**:
  1. **性能**（実測して数値を本書へ書き戻す）: プレビュー再コンパイル p50 ≤ 300 ms / p95 ≤ 1 s、シーン反映 p95 ≤ 3 s。**超過時は §8 R1 の縮退（プレビューのみ即時、シーンは保存時反映）へ**。
  2. **固まらない**: 編集中の 60 秒連続操作（合成入力）でフレーム時間 p99 ≤ 通常の 1.2 倍、33 ms 超のスパイク 0 回。
  3. **失敗しても壊れない**: 意図的に壊した HLSL（Custom ノード）で旧絵維持 + エラーが該当ノードに出る。
  4. **キャッシュ**: 再オープンで DXC 呼び出し 0 回（ヒットカウンタ）。
  5. プレビュー画像の決定論（同一グラフで同一 PNG）+ ゴールデン。
- **(e) 並列**: (i) ワーカー + キャッシュ + PSO / (ii) プレビュー / (iii) エラー表示 + スパイク。

#### G3 ノード 60 種 + パレット + MCP（12 日）

- **(a) 成果物**: §3.7 のノード全種（各: ピン定義・プロパティ・HLSL 生成・CpuEval・説明・検索語・日英名）、検索付きパレット（カテゴリ / D&D / ダブルクリック / ワイヤドロップ / あいまい検索 / UE 風ワンキー）、ノード単位プレビュー、`dx12_material_graph_*`（§4.5）+ TS スキーマ + `docs/MCP.md` + `docs/AUTHORING.md` のグラフ節、標準テンプレート集（標準 PBR・アンリット・ディゾルブ・水・トライプラナー岩）。
- **(b) 依存**: G0, G2c。
- **(c) 工数**: 12 日（ノード 45 種を 3 群に分けて各 3 日 = 9、パレット 1.5、MCP + docs 1.5）。
- **(d) 合否**: ①60 種すべてに HLSL ゴールデン + CpuEval 数値一致（数学系）+ DXC 通過 ②GPU パリティ（`--background` の材質自己診断で数学ノードを 1x1 RT に評価 → CpuEval と 1e-4）③MCP の往復（`create → edit → compile → apply → preview`）と Undo（AI エントリ 1 個）④パレット検索の ImGuiTestEngine テスト ⑤`mcp_manifest_test` / `mcp_param_spec_test` 緑。
- **(e) 並列**（**最も並列度が高い段階**）: (i) 定数 / テクスチャ / 座標 / 時間・UV（#1-24）/ (ii) 数学 / ベクトル / 色（#25-46）/ (iii) ノイズ / シェーディング入力 / 制御（#47-55）/ (iv) パレット + ノード単位プレビュー / (v) MCP + docs。

#### G3b マスク / 半透明（6 日）

- **(a) 成果物**: グラフ設定 `blendMode`（Masked / Additive）と `twoSided`、生成物に **`PSMask` / `PSDepth` エントリ**（OpacityMask の式だけを評価する軽量 PS。影・深度プリパス・速度・Perception ID 用。F5）、既存の MASK 用 PSO 群（`RecreateDepthMaskPsos` の流儀）へグラフ材質版を追加、`BuildDrawList` の `alphaClass` / `sortKey` 連動。
- **(b) 依存**: G3。
- **(c) 工数**: 6 日。
- **(d) 合否**: ①葉のカードで**影が抜ける**（影テクスチャの差分 / スクショ）②深度プリパスで葉の隙間が裏まで黒くならない ③速度バッファ整合（TAA の残像が出ない）④Translucent のソート（既存の半透明バケツと同挙動）⑤旧 MASK 材質のゴールデン不変。
- **(e) 並列**: (i) 生成物のエントリ追加 + シェーダー / (ii) PSO・描画統合。

#### G4 パラメータ / インスタンス / 関数（12 日）

- **(a) 成果物**: パラメータ一覧 UI（グループ・優先度・範囲）、インスタンス（`.dxmat` の `graph` / `params`、多段、無効上書き警告）、**マテリアル関数**（`.dxmf`、FunctionInput/Output/Call、インライン展開、再帰禁止、呼び出し経路つき ID、依存再コンパイル）、**StaticSwitch**（パーミュテーション、上限 16）、パラメータ名変更の追従、エンティティ上書き（`materialParams`、Lua `scene:setMaterialParam`、MCP `dx12_set_material_param`）、インスタンス専用エディタ。
- **(b) 依存**: G3。
- **(c) 工数**: 12 日。
- **(d) 合否**: ①インスタンスの値変更で**再コンパイル 0 回** ②関数展開のゴールデン + 再帰 / 深さ超過のエラー ③関数編集で依存マテリアルが再コンパイル ④StaticSwitch の 2 値で別 PSO（キャッシュヒット確認）⑤**旧資産不変**（旧 `.dxmat` の往復・ゴールデン）⑥Lua API のリファレンス 3 点 + 予測変換確認（memory `lua-api-checklist`）。
- **(e) 並列**: (i) インスタンス + パラメータ UI / (ii) 関数 / (iii) StaticSwitch + エンティティ上書き + Lua / MCP。

#### G5 UE 取り込み（16 日: G5a 6 + G5b 10）

- **(a) 成果物**: G5a = `tools/ue-cook/` の材質モジュール（親チェーン解決、パラメータ写像、標準 PBR インスタンス出力、`ueImport.unmapped`）。G5b = 式ノード写像表（§5.3）+ 関数の再帰変換 + `UnsupportedNode` + 写像率レポート。
- **(b) 依存**: G4、VG 設計書 P6 の環境（CUE4Parse / .NET 10 / usmap）。
- **(c) 工数**: 16 日。
- **(d) 合否**: G5a = Dead Mall の代表 10 マテリアルで、インスタンス化した `.dxmat` の見た目（色・粗さ・タイリング・テクスチャ）が既存の抽出結果と画素差基準で同等以上。G5b = 手元の未 cook プロジェクトの N 個（着手時に確定）で**写像率とグラフのコンパイル成功率を測定して本書へ記載**。
- **(e) 並列**: G5a と G5b の写像表作り（クラス調査）は独立。

#### G6 DXR / VG との整合（16 日: G6a 4 + G6b 12）

- **(a) 成果物**: G6a = RT 用 BaseColor プロキシ（`BaseColor` が定数 / スロットのみならその値、それ以外は UV 空間 128² ベイクをテクスチャとして生成し `GeometryInfo.baseColorSrvIndex` に載せる。R6）。G6b = VG の resolve へグラフ材質を載せる（材質ごとの全画面 resolve + 材質 ID を深度に書いて `EQUAL` で分離、`UNO_NU` / `SampleGrad`、勾配の有限差分、resolve 非対応フラグ → NONVG 縮退、`MaterialRecord.graphPathOff`）。
- **(b) 依存**: G6a = G3。G6b = G3 + VG 設計書 P4（resolve）。
- **(c) 工数**: 16 日。
- **(d) 合否**: G6a = `dx12_render_debug rtAlbedo` がグラフの色と一致（画素差基準）、RT 影 / RT-AO / DDGI 無改造で動く。G6b = VG 設計書 §5.4 と同基準（LOD0 パリティ 画素差 > 2/255 が 0.5% 以内）+ resolve 非対応ノードのグラフが NONVG で正しく描かれる。
- **(e) 並列**: G6a と G6b は独立。

### 7.3 マージ可能性・順序

- **G0 / G1 / G2a はお互い独立**で、3 人（3 エージェント）が同時に進められる。G2b で初めて合流する。
- どの段階も「グラフ材質を使わないシーンの絵は不変」がマージ条件（ゴールデン 4 枚ビット一致 + ctest 全緑）。G2a 以外はグラフ材質を**明示的に割り当てない限り**新経路に入らない。
- 段階ごとにエディタ起動を伴う確認は `--background`（窓は画面外・前面化しない）+ `imgui_screenshot` + 仮想入力のみ（memory `no-gui-driving-agents` の正解レシピ）。

### 7.4 VG 設計書との整合表

| 論点 | VG 設計書の記述 | 本書の扱い | 調整の要否 |
|---|---|---|---|
| カスタムシェーダー / 材質上書きは VG 対象外 | §2.2.2（`:115-125`） | v1 のグラフ材質は NONVG（同じ扱い）。G6b で VG 対応 | 矛盾なし |
| resolve PS が Forward の PS 本体を複製 | §2.2.6（`:157-167`） | `ForwardShade.hlsli`（G2a）を共有すれば複製不要 | **VG 側の実装方針を「関数を include」へ変更を推奨**（先に着手した側が抽出を担当） |
| `m_rootSignatureVg`（メイン同一 + `HEAP_DIRECTLY_INDEXED` の別オブジェクト） | §2.2.9（`:182-189`）、§2.2.11 | フォワードの PSO は 1 個の RS で通すため、**メイン RS 自体にフラグを立てる**方が単純（§3.11） | **要調整**: G2a のベンチでフラグに副作用が無ければ VG 側の別 RS を不要にできる。あれば VG の別 RS 案に合わせ、グラフ材質のフォワード PSO も別 RS 用に再設計（撤退案 R2） |
| resolve の材質は `MaterialRecord`（PBR 4 スロット） | §3.4（`:513-534`） | グラフ材質は `graphPathOff`（予約 4B）で参照。G6b で対応 | 予約フィールドの使用を VG 設計書 P0 で確保しておく（**P0 の契約に 1 行足すだけ**） |
| 解析 UV 勾配 + `SampleGrad` | §2.2.6 | `UnoSample` マクロで吸収（§3.11） | 矛盾なし |
| RT のヒット材質は `baseColorSrvIndex` | §2.2.8、F16 | G6a のベイク / 定数プロキシ | 矛盾なし |
| 追加の SRV レジスタ（P7 VSM が `t25`,`t26` を使う予定） | §2.2.9 | 本書は**レジスタを 1 本も消費しない**（バインドレス） | 衝突なし |
| UE cook（P6） | P6（`:822-833`） | G5 は環境（CUE4Parse / .NET 10 / usmap）と P6 のスパイクを共有 | 実行順: P6 スパイク → G5 |

---

## 8. リスクと撤退条件

| # | リスク | 検知 | 撤退条件 → 縮退案 |
|---|---|---|---|
| **R1** | **編集ごとのコンパイル待ち**（DXC + PSO が数秒、`ps_6_6` でライティング込み） | G2c の実測（p50 / p95、フレーム p99） | プレビュー再コンパイル p95 > 1 s またはシーン反映 p95 > 3 s → ①プレビューのみ即時、**シーンへは「適用」ボタン / 保存時のみ反映**（既定を非ライブに）②`lib` 分割スパイクの採用 ③`-Od` の 2 段階。それでも駄目なら**ライティング簡略版のプレビュー（アンリット + 単純ライト）を既定**にする |
| **R2** | **メイン RS の `HEAP_DIRECTLY_INDEXED` がドライバ挙動 / 性能を変える** | G2a のゴールデン + ベンチ（±1%）+ デバッグレイヤ | 差が出る → **別 RS オブジェクト**（VG 設計書の案）へ。グラフ材質のフォワード PSO は別 RS 用に作り、グラフ材質を描く区間だけ RS を張り替える（バインドの再設定コスト増）。それも重ければ **バインドレスを諦め、材質テーブルへレンジ追加（t25..t32、8 枚上限）** に縮退 |
| **R3** | **ルートシグネチャ残量**（2/64） | G2a〜 | 本設計は DWORD +0。万一 b2 の読み替えが他パスと衝突（影 / 速度の MASK が b2 を読む）→ グラフ材質の b2 レイアウトを「既存 9 DWORD の意味を保つ」形に再設計（flags / tint / uvScaleOffset / emissive はそのまま、metallic / roughness の 2 DWORD を recordBase / poolIndex に転用） |
| **R4** | **G0 の工数超過**（自前は約 10 日） | 進捗が 14 日を超える | `ImGuiEx::Canvas`（imgui-node-editor 由来 MIT の単体ファイル）を同梱してズーム / パン / 座標変換だけ借りる。さらに駄目なら imgui-node-editor をフォークして描画だけ上書き |
| **R5** | **UE 式の写像率が低い** | G5b の実測（写像率・生成成功率） | 生成成功率 < 40% → T1 は「近似の参考グラフ」扱い。未 cook の資産が無い → T0 のみ出荷（§5.4） |
| **R6** | **VG / RT との不整合**（グラフ材質が VG で描けない / RT で色が違う） | G6 のパリティ | resolve 非対応ノードのグラフは NONVG へ縮退（VG の既存境界と同じ）。RT は定数プロキシ（BaseColor のみ）を最低ラインとする |
| **R7** | **`b2` 読み替えが影・深度・速度の MASK 経路と衝突** | G3b | グラフ材質は MASK 系で専用エントリ（`PSMask`）を使い、b2 のレイアウトを一致させる（R3 の再設計） |
| **R8** | **生成 HLSL のバグで全材質が壊れる**（共通ヘッダの変更） | ゴールデン + ctest（`shader_include_deps_test`） | `ForwardShade.hlsli` の変更は必ず DXIL 比較 + ゴールデンを通す。グラフ側の失敗は旧 PSO 維持で局所化（他の材質へ波及しない） |
| **R9** | **プールの同期ミス**（GPU が読んでいるレコードを CPU が書く） | デバッグレイヤ + `--background` の連続書き込みテスト | 3 フレーム複製の徹底。駄目なら `UpdateSubresource` 相当のステージング + フェンス待ち（編集時のみ）へ |
| **R10** | **パラメータ名変更 / 関数変更が既存インスタンスを黙って壊す** | 無効上書き警告 + 依存走査テスト | 名前変更は必ず走査して書き換えを提案。関数のピン削除は参照元を列挙して確認ダイアログ |
| **R11** | **`ps_6_6` 要求で古い GPU が使えない** | `SupportsDynamicResources()` | 標準マテリアルへ縮退（警告）。対象外を明記 |
| **R12** | **テーマ 3 案が確定せず見た目が二度手間** | — | 見た目は `GraphTokens` に閉じている（§6.2）。G0 は現行テーマ（UE5 青）で作り、確定後にトークンを差し替えるだけ |

---

## 9. 未決事項（ユーザーに聞くべき点・5 件、推奨案つき）

1. **ノードエディタの実装方式**: 自前（ImDrawList、約 10 日）か、imgui-node-editor のフォーク（約 4 日）か。
   **推奨: 自前**。見た目（ネオングロー・型別ピン・テーマ追従）・Undo / MCP・テスト・ImGui 1.92 追従を全部握れる。工数超過時の撤退先（Canvas だけ同梱）を用意してある（§2.2、§8 R4）。
2. **GPU 要件**: グラフ材質を「SM 6.6 + Resource Binding Tier 3（`SupportsDynamicResources()`）必須」にしてよいか（非対応 GPU は標準マテリアルへ縮退）。あわせて、**メイン RS にバインドレスのフラグを立てる**変更（VG 設計書 P4 と共通、DWORD 影響なし）を先に入れてよいか。
   **推奨: 両方 Yes**。DXR・DDGI で既に同じ要件を使っている（R3）。フラグの副作用は G2a のベンチで判定し、あれば別 RS 案へ撤退。
3. **v1 の範囲**: スキンド（キャラ）対応と頂点変位（WorldPositionOffset）、Subsurface / ClearCoat などの特殊シェーディングを v1 に**含めるか**。
   **推奨: 含めない**（G7 の任意項目）。スキンドは既定でカスタムシェーダーが無視される既存の制約（C4）と、頂点変位は影・深度・速度・TLAS の全パスに波及するため。出力ノードの予約ピンと `UnoSurface` の予約フィールドで**契約だけ先に確保**する。
4. **UE 取り込みの対象と素材**: 最初は T0（インスタンス + パラメータ + テクスチャ。cook 済みで可）だけを作り、T1（式ノード写像）は**手元の未 cook プロジェクト**（UE エディタ側の `.uasset`）が用意できる場合に限る、でよいか。用意できるプロジェクトはあるか（あれば代表マテリアル N 個を写像率の実測に使う）。
   **推奨: T0 → T1 の順**。cook 済みにはグラフが無いため T1 は素材次第（§5.1）。
5. **既存 `.dxmat` のグラフ化の方針と見た目の前提**: (a) グラフ化は**手動ボタンのみ**（自動一括変換しない）でよいか。(b) ネオングロー用のテーマトークン（`theme::neon` などの強さスカラー）を別エージェントの 3 案に**1 個足してもらう**依頼をしてよいか。(c) UE 風のワンキー（`M`=Multiply 等）を既定にしてよいか。
   **推奨: (a) 手動のみ / (b) 依頼する（無ければ `Accent` から既定生成） / (c) UE 風を既定・設定で変更可**。

---

## 付録 A. 参照した主な実コード（本書の根拠）

| 領域 | ファイル |
|---|---|
| マテリアルデータ | `src/renderer/Material.h`、`src/resource/MaterialAssetIO.{h,cpp}`、`src/resource/MaterialAssetManager.{h,cpp}`、`src/ecs/Components.h`（`MeshRenderer`）、`src/scene/SceneSerializer.cpp` |
| マテリアルエディタ | `src/editor/panels/MaterialEditorPanel.{h,cpp}`、`MaterialPreviewRenderer.{h,cpp}`、`MaterialLibraryPanel.h`、`src/editor/ToolWindows.h` |
| カスタムシェーダー | `src/resource/ShaderManager.{h,cpp}`、`ShaderRuntimeCompiler.{h,cpp}`、`ShaderRegistry.cpp`、`ShaderParams.h`、`ShaderTemplates.{h,cpp}`、`src/core/ApplicationPipeline.cpp:985-1260`、`shaders/UnoCustom.hlsli`、`shaders/templates/Water.hlsl` |
| フォワード | `shaders/forward/Forward.hlsl`、`ForwardSkinned.hlsl`、`Lighting.hlsli`、`PBR.hlsli`、`MaterialPreview.hlsl`、`src/core/ApplicationRender.cpp:295-345,700-1120,1417-1560` |
| ルートシグネチャ / バインドレス | `src/graphics/RootSignature.{h,cpp}`、`GraphicsDevice.h:56-63`、`renderer/RtScreenPass.cpp:120-330`、`renderer/DdgiVolume.cpp:124` |
| DXR | `shaders/raytracing/RtBindless.hlsli`、`src/renderer/RaytracingScene.h:49-60` |
| テクスチャ | `src/resource/ResourceManager.cpp:192-258`（キー・永続 SRV） |
| Undo / テーマ / UI テスト | `src/editor/UndoCore.h`、`EditorTheme.h`、`UiWidgets.h`、`EditorCommandTable.h`、`src/gui/UiTestHarness.h`、`ImGuizmo.h` |
| MCP | `src/core/mcp/ApplicationMcpManifestData.inc`、`ApplicationMcpEntity.cpp`、`tools/mcp-server/materialApply.ts`、`catalog.ts` |
| ライブラリ調査 | `vcpkg.json`、`C:/vcpkg/ports/imgui-node-editor/`（portfile / パッチ 3 本）、`C:/vcpkg/ports/imgui/vcpkg.json`（最新 1.92.8。本プロジェクトは baseline 固定で 1.92.6） |
| 関連設計 | `docs/VIRTUAL_GEOMETRY_DESIGN.md`（§2.2.2 / §2.2.6 / §2.2.9 / §3.4 / P4 / P6） |
| メモリ（過去の事実） | `dx12-shading-traps`（b0 衝突・法線マップ）、`dx12-mcp-traps`、`dreamcore-dead-mall`（cook 済みにグラフが無い・AllLayers）、`no-gui-driving-agents`、`lua-api-checklist`、`dx12-realism-roadmap`（SM6.6 バインドレス） |

## 付録 B. 外部情報（要再確認）

- imnodes（Nelarius）: MIT、ミニマップ標準搭載、ズームは PR #134 で議論・メインライン未対応 — https://github.com/Nelarius/imnodes / https://github.com/Nelarius/imnodes/pull/134
- imgui-node-editor（thedmd）: MIT、README は「Vanilla ImGui 1.72+」、4.5k stars — https://github.com/thedmd/imgui-node-editor
- 上記の最終コミット日・ImGui 1.92 との実互換は**未確認**。G0 で自前を選ぶ前提なので決定には影響しないが、撤退案（R4）を採る場合は着手時に再確認する。
- UE のクラス名・キー割当・cook 済みパッケージの内容（`CachedExpressionData` 等）は記憶ベース。G5 着手時に CUE4Parse の実クラスで裏取りする。
