# グラフィックス・パリティ設計書（追加表現 + UE 並べ比較ハーネス + 統合ロードマップ）

- 対象: 自作 DX12 エンジン「Uno Engine」（`C:\Users\ryuto\Documents\dx12`）
- 作成: 2026-09-30 / 状態: **設計のみ（エンジンのソースは 1 行も変更していない。ビルド・エンジン起動・UE 起動・git 操作もしていない）**
- 基準機: RTX 5060（8 GB）/ RAM 16 GB / 1080p / 60 fps。ビルドは `tools/build.ps1` 経由（並列数制限 + BelowNormal + 名前付き Mutex で全セッション直列化）
- 姉妹文書: `docs/VIRTUAL_GEOMETRY_DESIGN.md`（VG。P0〜P10。以下「VG 書」）/ `docs/MCP_ENHANCEMENT_DESIGN.md`（M0〜M14。以下「MCP 書」）
- 表記: **[事実]** = 実コードを読んで確認（`ファイル:行`）/ **[出典]** = 公式文書・論文・リポジトリを読んで確認（URL 付き）/ **[設計]** = 本書の決定 / **[推定]** = 未計測の見積もり（各段階の実測で差し替える）/ **UNVERIFIED** = 確認できていない（判断材料にしない）

---

## 0. 要約（1 画面）

**目的**: 「UE に匹敵するグラフィック」を、**UE 5.8 と同じシーン・同じカメラの並べ比較**で合否判定できる形にし、VG 書の段階計画に「大気・雲 / 水 / 植生・風 / 特殊シェーディング / HW RT 反射とアップスケーラ」を足して 1 本のロードマップにする。

**先に伝えるべき発見（設計を左右する 4 点）**

1. **`C:\Program Files\Epic Games\UE_5.8` は今、空っぽ**（ファイル 0 個、`Engine\Binaries` も `Build.version` も無い。実測: `ls -la` と `du` で 0 バイト、最終更新 2026-08-12）。Epic Launcher の manifest（`C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests\63CE813D…item`）には 5.8.0 が「要検証」で残っているだけで、実体は消えている。**UE 側の基準画像は、UE の再インストールをユーザーが行うまで 1 枚も作れない**（→ Q0、未決 §6-1）。過去のログ（`Documents\Unreal Projects\AIMCPTEST\Saved\Logs\AIMCPTEST.log`）では 5.8.0-55116800、Launcher は現在 5.8.2 を提示。共有 DDC も 0 MiB（7 月は 318 MiB）なので、初回はシェーダ全コンパイルが走る。
2. **VG 書と本書の現行レンダラ認識は一致している**が、**今回の機能のほとんどは「フォワード PS を触らずに済む」形にできる**（§2.0）。ルートシグネチャは **0 DWORD 消費**（現状 62/64、残り 2 を温存）で全機能が入る: ①専用ルートシグネチャの別パス（先例: `SkyboxRenderer.h:13-14` / DDGI / RtScreen） ②既存テーブルへのレンジ追加（slot 11 に `t27`,`t28`。VG 書は `t25`,`t26` を VSM 用に予約済み） ③PerFrame CBV の**予約領域**（`_clusterReserved[40]`＝640 B が丸ごと空いている、`Lighting.hlsli:80`） ④RT 反射は**既存 SSR 出力テクスチャ（t16）へ書き込む**だけで Forward 無改造。
3. **現行の「光」と「露出」は UE と物理的に違う**（点光源の減衰が `saturate(1-d/range)^2`＝逆二乗ではない `Lighting.hlsli:221-235`、太陽強度 3.0 は任意単位、トーンマップは Narkowicz ACES `PostProcess.hlsl:101-145` で UE の Filmic と別物、スクリーンショットは 8bit）。**そのまま並べても数値が合うわけがない**ので、比較の前に「校正」段階（Q2）が要る。ここを飛ばすと、以後の全部の差分が「機能差」なのか「単位差」なのか分からなくなる。
4. **UE を「窓を出さずに」走らせる方法は、公式文書で確認できたのは `-RenderOffScreen` の記述まで**（"The application will not display any windows" は Pixel Streaming 文書、パッケージ版向け。`UnrealEditor(-Cmd).exe` / `-game` で本当に窓が出ないかは **UNVERIFIED**）。→ 設計は「UE の起動回数を最小化（基準画像をキャッシュ）+ ユーザー承認ファイル + 隠し窓/専用デスクトップ + CPU/メモリの Job 制限 + 立会い付きスパイク」で組む（§3.3）。**AI が UE を勝手に起動できる経路は作らない**（MCP に UE 起動 op を置かない）。

**推奨案の要点**

| 領域 | 推奨 | 却下 |
|---|---|---|
| 大気 | Hillaire 2020 の 4 LUT（Transmittance / MultiScattering / SkyView / AerialPerspective）を compute で毎フレーム更新（論文実測 合計 0.31 ms @GTX1080 720p）。太陽円盤・星・月は空パスに内蔵、**時刻→太陽方向→太陽色（LUT 由来）→IBL 再ベイク→DDGI の空項**を一本の「時刻系」でつなぐ。AP は FogComposite と同じ合成パス（Forward 無改造） | Bruneton 事前計算（多重散乱 LUT が 250 ms で毎フレーム更新できない、論文表） |
| 雲 | HZD/Nubis 系のレイマーチ（1/4 解像度 + 4×4 時間再投影 + 起動時 compute 生成のノイズ）。目標 ≤ 1.2 ms | ボクセル雲 Nubis3（NVDF 制作パイプラインが要る）、Shadertoy 流用（許諾不可） |
| 水 | 専用パス（不透明の後・半透明の前）。v1 = Gerstner 12 波 + 屈折（不透明色コピー）+ Beer 吸収 + SSR→空 LUT のフォールバック反射。FFT は条件付き（W3） | カスタムシェーダ水（b0/b1/t0 しか読めず屈折不可 `Water.hlsl:16-19`）、平面反射（シーン再描画が要る） |
| 植生 | **エンティティにしない**: `FoliageLayer`（1 コンポーネント = N インスタンス）+ GPU カリング + `ExecuteIndirect(DrawIndexedInstanced)`。既存 `ForwardInstanced` の slot 1 per-instance 頂点ストリームと `Forward_PS` を再利用（RS 変更ゼロ）。風は共通風場（PerFrame 予約領域）+ 頂点アニメ、草は GPU ブレード生成 | Nanite Foliage 相当（UE 5.8 でも Experimental [出典]）、エンティティ 10 万個（`dx12_scatter` は 1 回 ≤ 200 個・1 個ずつ create_entity `composite.ts:17-98`） |
| 特殊シェーディング | `pbrFlags` の**未使用ビット 4..7 = シェーディングモデル ID（16 種）**、bit16..31 = 拡張パラメータ配列の添字（StructuredBuffer を slot 11 の `t27` に）。クリアコート / 異方性 / 布（Charlie）/ 髪（Kajiya-Kay 2 ローブ）/ 肌（**プレインテグレート + 影厚み透過**。分離型 SSS は条件付き） | フォワード PS の MRT 化（禁止方針）、Marschner ストランド（メモリ・描画方式が別物） |
| RT 反射 / アップスケーラ | RT 反射 = SSR 優先→外れたらインライン RayQuery（既存 DXR 基盤）→**t16 へ書く**。ヒット shading は DDGI のものを共通化。アップスケーラ = **FSR 3.1 を直接統合**（ソースは MIT でリンク可）。**FSR 4 は RDNA3/4 専用で RTX 5060 では動かない** [出典]。DLSS は技術的には可だが**エンジンが MIT 公開であること・NVIDIA 事前通知・NVIDIA 限定**の条件が絡むので後回し | DirectSR（確定済みで死亡）、VG 書 P9 の「TSR 自前 8 日」→ **FSR 統合に置換して 8 日節約**（要承認 §6-3） |
| 合否ハーネス | UE 基準画像は**キャッシュ**（キー = シーン仕様ハッシュ + UE ビルド + UE 設定ハッシュ）。UE 起動は人間承認の CLI のみ。比較は **HDR-FLIP（線形）+ LDR-FLIP + SSIM + ΔE2000 + 輝度ヒストグラム**、しきい値は「ノイズ床（UE 同士・Uno 同士の再撮影差）の 1.5 倍以上」。M9（視覚回帰）・M10（性能ゲート）に相乗り | LPIPS をゲートに使う（重み配布の許諾が UNVERIFIED。参考表示のみ）、Dead_Mall の cook 済みを UE に戻す（不可能） |

**合計工数（実働日）**: 本書の新規 137 日（ハーネス 25 / 大気・雲 24 / 水 14 / 植生 25 / 特殊シェーディング 19 / RT 20 / アップスケーラ 10）+ VG 書 144 日 − P9 の TSR 8 日 = **約 273 日**（条件付き +17: 海 FFT 6 / 分離型 SSS 5 / DLSS 6）。**4 レーン並列で暦 約 16 週**。最初の 30 実働日で「パリティ数値が出る + 空・雲・草木・FSR・特殊シェーディング（一部）+ VG P4 の手前」まで到達（§4.5）。ビルド直列化（`Global\dx12-build`）と GPU 共有のため、実効レーンは 3〜3.5 を見込む（その場合 +25%）。

**ユーザーに決めてほしい点**は §6（5 件、推奨案付き）。

---

## 1. 現行レンダラの事実（実コード確認）

設計に効く事実だけを抜き出す。**VG 書 §1 と重複するもの（F8〜F18）は再掲せず、VG 書の ID で参照する**。

### 1.1 シェーダ群とパス（全体像）

| # | 事実（[事実]） | 含意 |
|---|---|---|
| G1 | `shaders/` の構成: `forward/`（Forward / ForwardSkinned / ForwardInstanced / Terrain / ForwardGrid / Emissive / Lighting.hlsli / PBR.hlsli / ShadowPcss / Cluster* / Decal*）、`ibl/`（Irradiance / Prefilter / IntegrateBRDF / Skybox）、`fog/`（Inject / Scatter / Integrate / Composite）、`post/`（PostProcess uber / TAA / AutoExposure / Bloom / DoF / MotionBlur / GodRays / LensFlare / Transition*）、`screenspace/`（SSR / SSGI）、`ssao/`、`shadow/`（ShadowPass* / ShadowMask / DepthPrepassSkinned / ContactShadow）、`velocity/`、`hiz/`、`raytracing/`（RtShadow / RtAo / RtAoDenoise / RtBindless.hlsli / RtCommon.hlsli）、`ddgi/`、`particle/`（CPU 8000 + GPU 131072）、`skinning/SkinCompute.hlsl`（DXR 専用）、`templates/`（Water / Ocean / ParticleEmber = **ユーザー用カスタムシェーダの雛形**でエンジンのパスではない） | 大気・水・植生・シェーディングモデル・RT 反射は**どれも既存ディレクトリに無い＝ゼロからの新設**。ただし部品（froxel fog の合成方式、IBL ベイカ、RayQuery 基盤、インスタンス描画）は再利用できる |
| G2 | **フレーム順序**（`ApplicationRender.cpp`、`RenderView` `:4418`、`RenderPostChain` `:5627`）: 影 `:4482-4672` → 深度/速度/G-Buffer プリパス `:4768-4823` → HiZ `:4831` → SSAO `:4855` → コンタクト `:4875` → RT 影/AO `:4896` → SSR/SSGI `:4950` → sceneRT クリア `:4996` → クラスタ `:5106` → DDGI `:5144` → デカール `:5176` → froxel フォグ構築 `:5235` → **スカイボックス `:5300`** → **Forward+ `:5324`** → デバッグ線 → **フォグ合成 `:5417`** → パーティクル `:5428` → スプライト → SSR/SSGI 色退避 `:5544` → ポスト（TAA `:5755` / DoF `:5768` / MB `:5793` / AE `:5816` / Bloom `:5828` / GodRays `:5836` / LensFlare `:5879` / LUT `:5892` / uber `:5941`）。sceneRT = `R16G16B16A16_FLOAT`、深度 = `D32_FLOAT`（標準 Z）、バックバッファ `R8G8B8A8_UNORM`（HDR 出力経路なし。`DXGI_COLOR_SPACE` を grep して 0 件） | 挿入点は**スカイ**（`:5300`）・**フォグ合成の隣**（`:5417`）・**不透明と半透明の境**（`:1983` の `isTransparent` ラムダ付近）・**ポスト先頭**（`:5755` の TAA を FSR に置換）の 4 か所 |
| G3 | ライティング: 平行光は **最初の `DirectionalLight` 1 個だけ**（`ApplicationRender.cpp:4050-4059`）。点/スポットはクラスタ（16×9×24、128 灯/クラスタ、シーン 1024 灯、`ClusterCommon.hlsli:14-19`）。減衰 `saturate(1-d/range)^2`、スポットコーンも 2 乗（`Lighting.hlsli:221-235`）。**エリアライト・IES・光の物理単位なし** | UE（逆二乗 + 減衰半径の窓関数、lux/cd 単位）との**単位差が最大の非機能差**。パリティ前に校正が要る（Q2） |
| G4 | BRDF: GGX NDF / Smith-Schlick-GGX / Fresnel-Schlick、拡散は Lambert（`PBR.hlsli:9-44`、`Lighting.hlsli:146-161`）。roughness 下限 0.04（`Forward.hlsl:225`）。法線マップ分散→roughness 補正あり（`PBR.hlsli:103-122`）。**多重散乱補償・クリアコート・異方性・シーン・SSS・髪・透過は全て無し**（`subsurface\|sss\|anisotropic\|clearcoat\|sheen\|kajiya\|hair\|charlie\|transmission` を grep して該当コードなし） | 特殊シェーディングは PBR.hlsli への追加（SH1〜SH3）。多重散乱エネルギー補償は UE との差として 1 日で入れる価値あり（SH1 に含める） |
| G5 | IBL（`Forward.hlsl:280-335`）: envMap があれば irradiance(32²) + prefilter(128², 5 mip) + BRDF LUT(512²)（`IBLBaker.h:46-49`）。**envMap があると `ambientStrength` を無視**、無いときだけ定数 ambient フォールバック（`:316-335`）。DDGI / SSGI / SSR は IBL を「置き換える」形で重なる | 空 → IBL の駆動は「envMap を差し替えて再ベイク」が既存の流れに素直に乗る |
| G6 | **空**: `Skybox.hlsl:38-45` はフルスクリーン三角形で環境キューブを view ray サンプル。既定 `envMapPath = "__procedural_sky__"`（`Scene.h:41,47`）は **CPU 生成の静的グラデーション**（`ApplicationScene.cpp:85-171`、64²×6 面 RGBA16F 単一 mip、太陽は意図的に焼かない `:83`）。ベイクは設定変更時のみ（`m_skyboxDirty` `ApplicationRender.cpp:2277-2283`）。`.hdr` は読めない（6 面 DDS のみ、`TextureLoader.cpp:841-845`）。**太陽円盤・大気散乱・雲・星は一切無い**（`sun disk\|atmospher\|rayleigh\|mie\|cloud\|starfield` を grep） | 大気は「新設 + 既存スカイ/IBL 経路の差し替え」。既定 OFF ならグラデーションのまま（ビット一致） |
| G7 | **時刻**: `Lighting.setTimeOfDay(hour)`（`ScriptEngine.cpp:3802-3812`）と MCP `set_sun timeOfDay`（`ApplicationMcpLighting.cpp:146-176`）は、**最初の `DirectionalLight` の方向・色・強度・ambient だけ**を曲線（`LightMath.h:108-146`。昼 (1,0.97,0.92)・夕 (1,0.46,0.18)・夜 (0.4,0.52,0.85)、強度 3.0/0.35）で動かす。**空・IBL・スカイボックス・フォグには触らない**。夜は太陽ベクトルを反転して「月」にしている | IBL が固定の昼グラデーションなので、`hasIBL=1` では**夜でも環境光が昼のまま**（コードからの読み取り。未実行）。時刻系の統合（A1）が要る |
| G8 | **フォグ**: froxel 160×90×64（`FogCommon.hlsli:20-22`）、inject → scatter（太陽 + CSM + クラスタ光 + 時間再投影）→ integrate → **`FogComposite`（`dst = src.rgb + dst*src.a`）** を Forward+ の後・パーティクルの前に描く（`ApplicationRender.cpp:5417-5427`）。既定 OFF、約 28 MB。冒頭コメントに Hillaire/Wroński の引用（`FogCommon.hlsli:10`） | **エアリアルパースペクティブ（AP）を同じ合成方式で足せる**（`FogComposite` と同形式のブレンド）。froxel の再利用ではなく、AP は別 LUT（§2.1） |
| G9 | SSR = RGBA16F **フル解像度**（rgb=反射放射輝度、a=confidence）、Forward PS が `Load(SV_Position.xy)` で直読み（`ScreenSpaceGiPass.h:37-39`）。ルート上は slot 12 の `t16`。トレースはハーフ解像度 DDA + bilateral upsample、**前フレーム色を読む**。SSGI は RGBA16F、slot 13 の `t17`（irradiance を「置き換える」） | **RT 反射は同じ形式のテクスチャを t16 へ書けば Forward 無改造**（§2.5） |
| G10 | **DXR**: インライン RayQuery（`RtSettings.h:6`）、BLAS は `Mesh` 単位・LOD0 固定・全ジオメトリ OPAQUE、TLAS 上限 32768（`RaytracingScene.h:44`）、スキンドは compute スキニング経由で入る（予算 20 万 tri/フレーム）。除外は**半透明とアルファマスク**（`DrawItem.h:117-121`。ヘッダコメント `RaytracingScene.h:27-30` は古い）。バインドレス `GeometryInfo`（16 B）+ 96 B 頂点 VB。**RT 反射は無い**（`reflect\|specular\|RtRefl` を `shaders/raytracing`・`Rt*.h`・`RtScreenPass.cpp` で grep）。RT 影は MASK を不透明扱い（any-hit 未実装。memory `dx12-shading-traps`） | RT 反射 / any-hit は新設。ヒット shading の実体は次行 |
| G11 | **DDGI ヒット shading**（`DdgiProbeUpdate.hlsl:184-240`）: `RtHitAlbedo`（頂点色 × アルベドテクスチャ、`RtBindless.hlsli:115-124`）の Lambert のみ。`albedo × (太陽 × N·L × 影レイ + 空 irradiance + 全点光源和) + min(albedo,0.9) × bounce`。**法線マップ・metallic/roughness・emissive・tint は無視**。スキンドの法線は bind pose（`RtBindless.hlsli:94-96`）。格子は 1 段（最大 4096 プローブ）、64 レイ/プローブ、八面体アトラス + Chebyshev（段階 2）、多重バウンスは既定 OFF | Dead_Mall の emissive 看板は GI に寄与しない（Lumen は寄与する）。RT3 で emissive・tint を足す。**DDGI と RT 反射のヒット shading を共通化**する（§2.5） |
| G12 | **TAA / 速度 / 解像度分離**: TAA は既定 OFF、Halton(2,3) 8 サンプル、YCoCg + 分散クリップ + Catmull-Rom 履歴 + Karis 重み（`TAA.hlsl:105-208`、`TaaSettings.h:15-18`）。ジッタは NDC でプロジェクションへ後乗せ（`TaaJitter.h:41-48`）。速度 = `R16G16_FLOAT`、`cur-prev`、ビューポート局所 UV、ジッタ除去（`VelocityCommon.hlsli:59-94`）、深度プリパスの MRT（G-Buffer `RGBA16F` = oct 法線 xy / roughness z / metallic w と同時）。スキンド・インスタンス・MASK は書く。**半透明・パーティクル・スカイは書かない** `ApplicationRender.cpp:1602`。`render_scale`（0.25〜1.0）は実装済み（`ApplicationPipeline.cpp:574-653`）だが**アップスケールはバイリニア**（uber パスが sceneRT を表示矩形へ引き伸ばす `ApplicationRender.cpp:5939-5942`）。**リアクティブマスク / 透過合成マスク / 露出連動は無い** | FSR 3.1 に要る入力のうち、**色 / 深度 / 速度 / ジッタ / カメラ値は既にある**。足りないのは ①リアクティブ系マスク（→ 水パスの「不透明色コピー」から FSR の生成ヘルパで作れる §2.5）②ジッタ位相数（`8×(表示/描画)²` が要る。現状 8 固定）③露出（GPU `StructuredBuffer<float>`、uber パスでのみ適用 `PostProcess.hlsl:69,379-380`。FSR には 1.0 と `preExposure=1` を渡し、露出は upscale 後の uber で掛かる現行順序を保つ）④**ポストが全部レンダー解像度**（`ApplyRenderResolution` が DoF/Bloom/MB 等の RT も縮める `ApplicationPipeline.cpp:574-653`）→ 表示解像度側へ分離する作業が UP1 の主要部 |
| G13 | ポスト: ACES（Narkowicz、既定）/ AgX / なし（`PostProcess.hlsl:101-145`）、自動露出（256 ビンのヒストグラム、既定 OFF）、Bloom / DoF（ギャザ、物理 F 値 CoC）/ カメラ+オブジェクトモーションブラー / LUT / グレイン / ビネット / 色収差 / FXAA / シャープ / デバンド（既定 ON）。スクリーンショット: `screenshot`（ポスト前 sceneRT、`ApplicationMcpEditor.cpp:189-233`）と `screenshot_final`（バックバッファ = 目視と同じ絵、`:238-272`）。どちらも 8bit PNG。**float 出力なし** | パリティは 2 段（線形 HDR の Tier A、表示参照の Tier B §3.5）。Tier A には**線形 float スクショ**が要る（Q2） |
| G14 | **CLI**（`main.cpp:449-534`）: `--headless`（窓を出さず MCP だけ）/ `--background[=offscreen\|minimized\|noactivate\|hidden]`（`BackgroundMode.h:8-15`）/ `--virtual-input` / `--mcp-port N` / `--project` / `--scene`。**解像度指定は無い**（`--screenshot`/`--render-size`/`--resolution` を grep して 0 件）。ヘッドレスの表示矩形は 1043×587（`tests/golden/golden.json:5`）。決定論撮影 `deterministic:true, settleFrames`（`ApplicationRender.cpp:3809-3820`: totalTime を 8.0 固定、ジッタ / フォグ位相 / SSGI 位相リセット、履歴破棄、RT デノイズ・DDGI の frameIndex を 0 固定、GPU パーティクル dt=0）。ゴールデン 4 枚が**起動をまたいで 1 ビットも変わらない**（`golden.json:9-16`） | パリティ撮影は既存の決定論・ヘッドレス基盤に乗る。足りないのは**解像度指定（1920×1080 固定）**（Q2）。Uno 側の再現性は既に確保済み＝ノイズ床が Uno 側でほぼ 0 なので、しきい値は UE 側のノイズ床で決まる |
| G15 | **地形**: `TerrainMeshBuilder`（`src/terrain/`）はハイトフィールドを**通常の `Mesh`** にする（専用 LOD なし）。既定 128 分割 / 200 m / `splatResolution` 512。スプラット 4 レイヤ（RGBA8、`TerrainSplatMap.h:3,50`。8 レイヤは v2 保留）、トライプラナー / POM / マクロ / 距離タイリング（`Terrain.hlsl:92-95`、既定フラグ 0x0D）。自動ペイント（傾斜・高さ）と MCP `terrain_paint/autopaint/set_layers`。独自 PS を持つがルートシグネチャは Forward と共用（`Terrain.hlsl:1-10`）、**インスタンシング対象外**（`ApplicationRender.cpp:322-328`） | 植生の配置マスク = **スプラットのレイヤ重み**（草レイヤ）を使える。地形は VG の対象外（スプラット地形は `VG 書 §2.2.2` で対象外）。屋外シーンの地面は従来経路 |
| G16 | **`dx12_scatter`** は**エンジン内ではなく MCP サーバ（TS）側**（`tools/mcp-server/toolset/composite.ts:17-98`）。矩形内に 1 回最大 200 個、ランダム or グリッド、`snapToGround` は 1 個ずつ `snap_to_ground` 呼び出し（`:91`）、`mulberry32(seed)`。**地形密度・傾斜・スプラット非対応**、各配置は独立エンティティ | 草木 10 万〜100 万は**桁が違う**ので別機構（`FoliageLayer`）。`dx12_scatter` は「手置き用の小規模ツール」として残す |
| G17 | **インスタンス / カリング**（`BuildDrawList` `ApplicationRender.cpp:84-471`）: `batchKey` の自動インスタンシング（適格: sortKey 0・非スキン・単一サブメッシュ・材質上書きなし等、`:332-336`）、`kMaxInstances = 262144`（`Application.h:1119`）、slot 1 の per-instance 頂点ストリーム `MeshInstanceData`（64 B: r0..r2 + color、`Mesh.h:28-34`）と速度用 prev-world（48 B、slot 2）。CPU 錐台カリングは**エンティティごとの境界球**、HiZ 遮蔽は既定 OFF・オブジェクト単位・`SetPredication`（GPU 駆動でも `ExecuteIndirect` でもない）。メッシュ LOD は meshopt の**インデックスのみ簡略化**（≥3000 index、30/10/3/1%、`Mesh.cpp:145-154`）。**インポスター・距離カリング設定・風・葉・草は無い**。ドロー数の実測: city_blocks 1068 → 1543（プリパス強制 ON）、Nocturne 4656、**影が約 90%**（`CLAUDE.md:340-343`） | 植生 LOD にインデックス簡略化は使えない（カード形状が崩れる）。インスタンス経路は**そのまま GPU 生成ストリームの受け皿になる**（§2.3） |
| G18 | **マテリアル**（`renderer/Material.h`）: テクスチャ = albedo(t0) / normal(t1) / metalRoughness(t2: G=rough, B=metal) / emissive(t24)。**AO マップは読まない**（ORM の R は無視）。スカラ = defaultMetallic/Roughness / emissive 色+強度 / alphaMode(Opaque/Mask/Blend) / cutoff / opacity。b2 = **9 DWORD**（metallic, roughness, `pbrFlags`, `packedTint`, `uvScaleOffset`×4, `packedEmissive`、`Forward.hlsl:55-79`）。`pbrFlags` = bit0 法線 / bit1 MR / bit2 アルファテスト / bit3 emissive テクスチャ / **bit8..15 cutoff**（`Forward.hlsl:62-63`、`Material.h:90-99`）＝ **bit4..7 と bit16..31 は未使用**。`packedTint` 上位 8bit = opacity。Forward PS の戻り値の α = `albedo.a * gOpacity`（`Forward.hlsl:363`）＝不透明では 1。**エンジンは glTF の `baseColorFactor` を読まない**（memory `dreamcore-dead-mall`）。ルート定数は満杯で float を足せない | シェーディングモデル ID を bit4..7 に置ける（**RS 変更ゼロ**）。拡張パラメータは bit16..31 の添字で StructuredBuffer を引く。パリティ資産は「factor を焼き込んだテクスチャ」規約が要る（§3.4） |
| G19 | **ルートシグネチャ**（`RootSignature.cpp:11-23,353`）: 14 パラメータ、slot0 = 32bit 定数 b0 が 40 DWORD、slot1 = CBV b1、slot2 = 材質テーブル（t0-t2 + **t24 emissive を `OFFSET_APPEND`**、ヒープ上 4 連続）、slot3 bones t3、slot4 CSM t4、slot5 = b2 9 DWORD、slot6 IBL t5-7、slot7 SSAO t8、slot8 スポット/ポイント影 t9-10、slot9 コンタクト t11、slot10 前フレーム bones t12、slot11 = クラスタ t13-15 + デカール t18-21 + DDGI t22-23、slot12 SSR t16、slot13 SSGI t17。**合計 62/64、残り 2 DWORD**（ログ文言も `:353`。`Material.h:87` などのコメントの「61/64」は古い）。静的サンプラ s0-s5（s0 = 異方性 8x） | **既存テーブルへのレンジ追加は DWORD 0**。今回の新規 SRV は slot 11 に `t27`（材質拡張）・`t28`（雲影）を足す。t25/t26 は VG 書 P7（VSM）予約、t29 以降は空き。**t 番号の一元管理表を §2.0 に置く** |
| G20 | **PerFrame CBV（b1、1536 B）**（`Lighting.hlsli:51` 以降）: `_clusterReserved[40]`（**640 B = float4×40、offset 608..1247**）が丸ごと空き（DDGI が 3 本使った跡地の残り）。CBV は 2 DWORD のルート記述子なので**サイズを使っても DWORD は増えない**。C++ 側 `FrameConstants(1536B)` とバイト一致が必須 | 風場・大気の全体値・水パラメータ・材質拡張の個数は全部ここへ入る（**サイズ不変**、offset も既存を 1 バイトも動かさない）。ただし VS が b1 を読めることの確認が 1 か所要る（`ForwardInstanced.hlsl` の CBV 可視性。UP/F1 の冒頭 0.5 日） |
| G21 | **ビルド・実行の制約**: `tools/build.ps1` は並列数 = min(12, max(2, 論理コア/2))、`-l (cores-2)`、BelowNormal、`Global\dx12-build` Mutex（最大 40 分待ち）で**全セッション直列化**（`tools/build.ps1`）。`CLAUDE.md:43-48` = **実入力でエディタを操作しない、`--background` + 仮想入力 MCP を使う**。エンジンの TCP ブリッジ（8787）は単一クライアント。`src/renderer/vg/` は **2026-09-30 04:25 に空ディレクトリとして作られている**（VG 実装の着手兆候。他セッションと衝突しうる） | 並列レーンは「コーディングは並列、ビルド/GPU 計測は直列」。**パリティ実行と perf 計測は GPU を専有する**ので、`Global\dx12-gpu-bench`（新設、build と同じ流儀）で直列化する（Q1）。VG 側の作業状況を着手前に確認する |
| G22 | **既存の比較・回帰資産**: `tools/bench/golden.mjs`（4 シーン、2 段判定 = 画素 SHA-256 `bitExact` + 面積平均縮小の許容差 `pixelThreshold=12` / 違い画素 0.1% / 平均差 0.25）、`tools/bench/lib/engine.mjs`（`--headless --project --scene --mcp-port` での起動 `:53-55`）、TS の `dx12_look_compare`（参照 PNG との **log 輝度ヒストグラム EMD / 平均・中央値輝度 / CCT / 彩度 / 黒潰れ・白飛び率**、`lookCompare.ts:1-60`）、`dx12_camera_path`（コンタクトシート）。MCP 書の M9 `dx12_visual`（baseline capture/compare/approve）、M10 `dx12_perf_gate`（budgets.json + 履歴） | パリティはこの**延長**（baseline の種類に「UE 参照画像」を足す）。新規は指標（FLIP/SSIM/ΔE）と UE 側の生成器だけ |

### 1.2 VG 書の統合案との整合が要る箇所

VG 書の統合方針（プロキシ + 可視性バッファ + 全画面 resolve、フォワード PS の MRT 化なし、専用 RS）と本書の各機能の関係。

| 箇所 | VG 書の決定 | 本書の機能との関係（[設計]） |
|---|---|---|
| **VG の対象外** | MASK / 半透明 / スキンド / カスタムシェーダ / 材質上書き / スプラット地形 は従来経路（VG 書 §2.2.2） | **植生（MASK）・水（半透明）・キャラ（スキンド）・地形は VG に入らない**。屋外・キャラのシーンは従来経路が主役。VG は屋内・岩・建築・重量シーン。**「Nanite Foliage 相当」は作らない**（UE 5.8 でも Experimental [出典 dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage]）。VG 書 R13 の「植生が支配的なシーンで見劣り」は本書の F1〜F3 が答え |
| **resolve PS のコピー** | `VgResolve.hlsl` が `Forward.hlsl` の PS 本体をコピーで持つ（VG 書 F12）。ドリフト検知は §5.4 のパリティのみ | **シェーディングモデル分岐は `PBR.hlsli` の関数として書き、両方が同じ関数を呼ぶ**（コピーしない）。`MaterialExtra`（`t27`）は VG 側ではバインドレスで同じバッファを引く。VG の材質表 `VgMaterialGpu` に `shadingModel` と `extraIndex` を足す（VG 書 P4 の作業に +1 日、SH1 の依存に明記） |
| **G-Buffer / 速度パス（H2）** | VG 画素の法線/roughness/metallic は頂点法線 + 材質係数（法線マップ非参照） | 特殊シェーディングで G-Buffer に足すものは無い（SSR/SSGI/RT 反射が読む roughness/metallic/法線だけで足りる）。クリアコート等の追加 roughness は**反射系には使わない**（ベース層のみ）。SSS は G-Buffer に足さない（§2.4） |
| **DDGI（VG 書 P8）** | カスケード / リロケーション / VG 併用検証（18 日） | 本書の RT3（ヒット shading 強化）と RT1（RT 反射のヒット shading 共通化）は `DdgiProbeUpdate.hlsl` を触る。**P8 のカスケード作業と同一ファイル**なので直列化（P8-1 → RT3） |
| **TSR（VG 書 P9）** | 自前 TSR 8 日（`TaaPass` 拡張） | **FSR 3.1 直接統合（UP1、10 日）で置換**を推奨（未決 §6-3）。DoF/Bloom（6 日）は残す。TAA 自体は非アップスケール時の AA として温存 |
| **VSM（VG 書 P7）** | 仮想シャドウマップ 30 日、slot 4 に `t25`,`t26` | 植生の影（MASK インスタンス）は VSM ではなく CSM の**マスク付きインスタンス影**（新規、F1）で v1 を出し、VSM 化は P7 に乗る。**`t27`/`t28` は本書、`t25`/`t26` は VG 書と分けて衝突を避ける** |
| **UE 資産の取り込み（VG 書 P6）** | CUE4Parse で Nanite 復元を試す、駄目なら UE エディタ側エクスポートへ縮退 | **cook 済みアセットは UE エディタに戻せない**（`.uasset` は cook 済みで編集不可）。パリティシーンは「両エンジンに同じソースアセットを渡す」方式にする（§3.4）。P6 の成果（VGSRC/Dead_Mall 抽出）は**アセットの供給源**として使う |

---

## 2. 機能別設計

### 2.0 共通規約（全機能に適用。ここが「既存を壊さない」担保）

**2.0.1 既定 OFF で絵が 1 ビットも変わらない（VG 書 §2.2.10 と同じ契約）**

1. 各機能は `XxxSettings`（`RtSettings` / `DdgiSettings` と同じ流儀でシーン JSON へ保存、既定 `enabled=false`）を持ち、OFF のときは GPU リソースを**一切確保しない**（遅延確保）。
2. 既存シェーダ（`Forward*.hlsl` / `Terrain.hlsl` / `ForwardInstanced.hlsl` / `PostProcess.hlsl`）は**ビット同値**を保つ。新機能は原則として**新ファイル**（`shaders/sky/` `shaders/water/` `shaders/foliage/`）と、既存 `.hlsli` への**関数追加**で入れる。`Forward.hlsl` の PS 本体に分岐を足すのは SH1 の 1 回だけ（ID = 0 のとき従来経路と DXIL 同値であることを比較テストで担保）。
3. マージ条件は VG 書 §4.1 と共通: ① `tests/golden/` 4 枚が新機能未使用でビット一致 ② `ctest` 全緑 ③ MCP スキーマ変更は TS↔C++ ドリフトテストと `docs/MCP.md` を同時更新 ④ D3D12 デバッグレイヤ警告 0。**さらに本書で追加**: ⑤ 該当機能に**パリティ・ゲート**（§3.7）があるなら `tools/parity` の判定が緑（UE 基準画像が未生成の間は `skipped` を明示。合格扱いにしない）。
4. **Lua/MCP/文書の 3 点セット**（memory `lua-api-checklist`）: 時刻・風・水・大気を Lua から触れるようにするなら、MCP 辞書 + リファレンス（`docs/API_REFERENCE.md` / `docs/index.html` 系）+ 予測変換の確認まで同じ段階で終える。

**2.0.2 ルートシグネチャ予算（62/64 → 62/64。本書の全機能で DWORD 増分 0）**

| 手段 | 使う機能 | 備考 |
|---|---|---|
| A. 専用ルートシグネチャの別パス（`HEAP_DIRECTLY_INDEXED` 可） | 大気（空/雲/AP/LUT）、水、RT 反射、FSR、草の生成 compute、インポスターのベイク | 先例: `SkyboxRenderer.h:13-14`「自前 RootSig / PSO を持つ（メイン RS を汚さない）」、`DdgiVolume.cpp:124`、`RtScreenPass.cpp:131`。`SetDescriptorHeaps` を `SetRootSignature` の前に呼ぶ規約（`RtScreenPass.cpp:318-326`） |
| B. 既存テーブルへのレンジ追加（DWORD 0） | 材質拡張 `t27`、雲影 `t28`、（布 DFG LUT `t29` = slot 6 IBL テーブルの 4 本目） | テーブルは何本レンジがあっても 1 DWORD。**slot 11（クラスタ t13-15 / デカール t18-21 / DDGI t22-23）に追記**。**t 番号の一元管理**: 使用済み t0-t24、VG 書 P7 が `t25`,`t26`（slot 4）、本書が `t27`（MaterialExtra）/`t28`（CloudShadow）/`t29`（ClothDFG）、以降空き。新規に t を取る人は本表を更新する |
| C. PerFrame CBV の予約領域 | 風場、雲影行列、材質拡張の個数、時間（前フレーム時刻） | `_clusterReserved[40]`（offset 608..1247、`Lighting.hlsli:80`）。**サイズも既存 offset も不変**。割当: `[0..1]` 風場（方向×速度 / 突風周波数・振幅・乱流・有効）、`[2..5]` 雲影の world→cloudUV 行列、`[6]` 雲影パラメータ、`[7]` 材質拡張フラグ + 前フレーム time、`[8..39]` 未割当（24 本を残す） |
| D. per-instance 頂点ストリーム（RS 無関係） | 植生・草（culling 後の compact ストリームを slot 1 に貼る） | `MeshInstanceData`（64 B、`Mesh.h:28-34`）と同じレイアウトで吐けば `ForwardInstanced` 系 VS と `Forward_PS` を共用できる |
| E. `pbrFlags` の未使用ビット | シェーディングモデル ID = bit4..7、拡張添字 = bit16..31 | `Forward.hlsl:62-63` / `Material.h:90-99`。**b2 のバイト配置は 1 バイトも変わらない**（opacity を `packedTint` 上位に詰めた前例と同じ作法） |

**2.0.3 フレーム内の順序（VG 書 §2.2.3 に本書の挿入点を足した全体像）**

```
 (1) 影                : CSM/スポット/ポイント。+ [F1] 植生のマスク付きインスタンス影（カスケード 0-1 のみ）
 (2) 深度プリパス      : 非 VG 不透明。植生（MASK）は VelocityPrepassInstanced の ALPHA_TEST 版 + 風
 (3) [VG H1/H2]        : VG 書のとおり
 (4) HiZ/SSAO/コンタクト/RT 影・AO → [RT1] RT 反射（SSR の後段で t16 へ合成）→ SSR/SSGI
 (5) sceneRT クリア → [A1] 大気の空（SkyView + 太陽円盤 + 星）→ [A2] 雲（1/4 解像度を空画素へ合成）
 (6) クラスタ/DDGI/デカール/froxel フォグ構築
 (7) [VG H3 resolve] → Forward+（不透明のみ。[SH] シェーディングモデルはここ）→ [F2] 草パス
 (8) [W1] 不透明色コピー → 水パス（深度も書く）→ 半透明（Forward+ の半透明部）
 (9) FogComposite → [A1] AP 合成 → パーティクル → スプライト
 (10) SSR/SSGI 色退避 → [UP1] FSR（TAA を置換）→ 表示解像度のポスト（DoF/MB/AE/Bloom/…/uber）
```

- (8) は **`RenderSceneMeshes` が不透明と半透明を 1 パスで描いている**（`ApplicationRender.cpp:1983` の `isTransparent`）ため、半透明の描画を分離して間に水を挟む変更が要る（W1 の作業）。
- (5) 雲が不透明ジオメトリより先に描かれるので、山など近景による遮蔽は自然に成立する（遠景の山が雲の手前に来る誤りは v1 の既知の割り切り）。

**2.0.4 GPU 予算（1080p / RTX 5060 の [推定]。各段階の実測で差し替える。VG 書の VG 合計 8〜10 ms とは別枠）**

| 機能 | 追加 GPU ms | 追加 VRAM | 根拠 |
|---|---:|---:|---|
| A1 大気（LUT + 空 + AP 合成 + IBL 再ベイクの償却） | 0.35〜0.6 | 約 3 MB | 論文実測 合計 0.31 ms（GTX 1080・720p）[出典] |
| A2 雲 | ≤ 1.2 | 約 12 MB（ノイズ 9.3 + 履歴） | HZD 約 2 ms（PS4）[出典]、Nubis Evolved ≤ 2–3 ms（PS5 1080p）[出典] |
| W1/W2 水 | ≤ 1.5 | 約 24 MB（色コピー 16 + 深度 8） | 未計測 |
| F1/F2 植生 | 描画 1.5〜2.5 + 影 +0.8 | 約 50 MB（100 万インスタンス 32 B + 可視バッファ） | 未計測。影が支配的（G17） |
| SH シェーディングモデル | 0.2〜0.6（非 default 材質のみ） | < 1 MB | ALU 追加のみ |
| RT1 RT 反射 | 1.5〜2.5 | 約 20 MB | 未計測 |
| UP1 FSR 3.1 | ≈ 1（RTX 上の公式値なし。UNVERIFIED） | 61〜75 MB（Performance/Quality）[出典 super-resolution-upscaler.md] | DLSS の 5070 実測 0.64–0.75 ms（1080p Performance）[出典] から外挿 |
| **合計（屋外フル）** | **約 8〜9** | **約 200 MB** | **描画解像度 0.67 + FSR が前提**。ネイティブ 1080p で全部載せると 60 fps は苦しい |

---

### 2.1 大気・空・ボリュメトリッククラウド

#### 2.1.1 推奨手法: Hillaire 2020 の 4 LUT

- **出典**: Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020（https://sebh.github.io/publications/egsr2020.pdf ）。参照実装 `sebh/UnrealEngineSkyAtmosphere`（**MIT**、DX11、2022-09 が最終更新、https://github.com/sebh/UnrealEngineSkyAtmosphere ）。IBL への流し込みは Bevy の `AtmosphereEnvironmentMapLight`（MIT/Apache-2.0、https://github.com/bevyengine/bevy/tree/main/crates/bevy_pbr/src/atmosphere ）が「LUT から 2 のべき乗キューブへ compute で描き、プリフィルタ」で UE と同じ考え方。
- **LUT**（RGBA16F）と論文実測（GTX 1080・720p、Table 2）: Transmittance 256×64（0.01 ms）/ MultiScattering 32×32（0.07 ms）/ SkyView 200×100（0.05 ms、論文値。UE の 192×108 は UNVERIFIED）/ AerialPerspective 32×32×32（0.04 ms）/ 最終合成 0.14 ms、**合計 0.31 ms**。メモリは合計約 0.55 MiB（本書の試算）。
- **物理パラメータ**（参照実装の値 [出典]）: 惑星半径 6360 km、大気上端 6460 km、レイリー (0.005802, 0.013558, 0.0331)/km・スケール高 8 km、ミー 散乱 0.003996・消散 0.00444・g=0.8・スケール高 1.2 km、オゾン吸収 (0.00065, 0.001881, 0.000085)/km（25 km 中心の 2 層テント）、太陽視半径 0.004675 rad。**地面アルベドは論文 0.3 / UE ドキュメントの推奨 0.4**（値が割れているので**パリティでは両エンジンへ明示的に同じ値を書く**。既定値に頼らない、§3.4）。
- **多重散乱**: 論文の 2 次成分 + 伝達関数 `1/(1−fms)` の近似（式 10）。極端に高い散乱係数で色相がずれる限界が論文に明記（Mie g=0.8 の RMSE 0.039）。

#### 2.1.2 Uno への統合 [設計]

- **`AtmosphereSettings`**（既定 OFF、シーン JSON 保存）: 上記物理パラメータ、`seaLevelY`（ワールド y=0 を標高 0 とする基準）、`groundAlbedo`、`sunIlluminance`、`driveSun` / `driveIBL` / `drawStars`、`aerialPerspectiveStartDepth`（既定 100 m。UE の既定と同じ [出典 UE Sky Atmosphere ドキュメント]。**froxel フォグ（既定 150 m まで）との二重計上を避ける**）。
- **描画**: `shaders/sky/SkyAtmosphere.hlsl`（新規）が、有効時のみ `SkyboxRenderer`（`ApplicationRender.cpp:5298-5317`）の代わりに走る。空画素 = SkyView LUT 参照 + 太陽円盤（参照実装の `GetSunLuminance` の考え方: 太陽視半径の内側を透過率 LUT で減衰）+ 星（ハッシュによる手続き星、夜間フェード。ノイズ 1 枚も外部アセットを使わない）+ 月（円盤 + 縁暗化、A3）。
- **AP**: `AtmosphereApComposite.hlsl`（新規）。深度から froxel スライスを引き、`(inscatter.rgb, transmittance)` を **`FogComposite` と同形式（`dst = src.rgb + dst*src.a`）**で合成する。空画素は除外（既に大気を含む）。**Forward は無改造**（G8）。
- **太陽の色と強度**: `AtmosphereMath`（C++、純関数）が「カメラ高度から太陽方向への透過率」を数値積分（32 ステップ）して、**最初の `DirectionalLight` の色×強度を駆動**する（`driveSun`）。GPU の Transmittance LUT との一致をテストで固定（CPU↔GPU 参照、`hiz_math_test` と同じ流儀）。既存の平行光エンティティ・PCSS・CSM・RT 影はそのまま使える。
- **時刻系の統合（G7 の穴を塞ぐ）**: `Lighting.setTimeOfDay(hour)` / MCP `set_sun timeOfDay` の API と意味は保つ。`AtmosphereSettings.enabled=false` なら従来曲線（`LightMath.h:108-146`）でビット一致。有効なら **hour → 太陽高度（同じ日の出 6 時・日の入 18 時の経路を既定）→ 太陽色（LUT 由来）→ IBL 再ベイク → DDGI の空項**が 1 本でつながる。夜は月（照度 0.1〜0.3 lux 相当、太陽ベクトル反転の現行方式を踏襲）と星。
- **IBL の駆動**: 空キューブ（64²〜128²×6、RGBA16F）へ SkyView 相当を描き、既存 `IBLBaker::Bake`（irradiance 32² / prefilter 128² 5mip、`ApplicationScene.cpp:223,266`）へ流す。**更新方針**: 太陽高度が 0.25° 変わるか雲量が変わったときのみ、最大 2 Hz、prefilter は 1 フレーム 1〜2 面へ分割（償却）、二重バッファで切替（VRAM +約 2.5 MB）。下半球は地面色で埋める（UE の "Lower Hemisphere is Solid Color" と同じ発想 [出典 UE Sky Lights ドキュメント]）。UE の実時間キャプチャも「Sky Atmosphere・Volumetric Clouds・Exponential Height Fog を取り込む」[出典]。
- **DDGI**: プローブのミス/空項は IBL の irradiance キューブを引くはず（`DdgiProbeUpdate.hlsl:184-240` の「空 irradiance」の参照先は着手時の 0.5 日で確認）。引いているなら IBL 再ベイクだけで GI の環境光も時刻に追従する（追加作業なし）。RT ミスレイは SkyView LUT を引く（RT1）。
- **コスト**: A1 = 0.35〜0.6 ms [推定]（LUT は Transmittance/MultiScattering をパラメータ変更時のみ、SkyView と AP は毎フレーム）。

**却下した案（理由つき）**

| 案 | 理由 |
|---|---|
| Bruneton 2017 の事前計算散乱（`ebruneton/precomputed_atmospheric_scattering`、BSD-3） | 全 LUT 更新が 250 ms（99% が多重散乱の反復、論文表）＝時刻・パラメータを動的に変えられない。Scattering LUT が 256×128×32（約 8 MiB）と大きい |
| 解析式の空（Preetham / Hosek-Wilkie 系） | AP と黄昏・宇宙視点が出せず、UE の SkyAtmosphere と並べる目的に合わない（本書の設計判断。数値の出典なし） |
| 現行のキューブ空 + 色調整 | 太陽円盤・AP・時刻連動が出せない。IBL が昼固定のまま（G7） |

#### 2.1.3 ボリュメトリッククラウド

- **手法**: Schneider, "The Real-time Volumetric Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015（全体 約 2 ms・RAM 20 MB＠PS4、素の実装は約 20 ms）[出典 https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf ]。要点: 3D ノイズ 128³（Perlin-Worley + Worley 3ch）と 32³（Worley 3ch）、curl 128²、天気マップ（coverage / precipitation / cloud type）、低層 1500〜4000 m の球殻、視線 **64〜128 サンプル**（安価なサンプルで空を進み、雲に当たったら詳細サンプルへ）、光サンプルは円錐 6 点、**1/4 解像度バッファの 4×4 ブロック中 1 画素を毎フレーム更新して前フレームを再投影（1/16 更新、10 倍以上高速）**。
- **ライティング**: Beer-Lambert + **二重ローブ HG**（`max(HG(cosθ, ecc), silver_intensity·HG(cosθ, 0.99−silver_spread))`、Nubis 2017 [出典 https://d3d3g8mu99pzk9.cloudfront.net/AndrewSchneider/Nubis-Authoring-Realtime-Volumetric-Cloudscapes-with-the-Decima-Engine-Final.pdf ]）+ 環境光は SkyView の天頂/地平色から。**powder 項の式は出典が確認できていない（UNVERIFIED）ので自前の近似**を置く。
- **ノイズは外部アセットなし**: 起動時 compute で生成（128³ RGBA8 = 約 8 MiB、mip 付き 約 9.3 MiB / 32³ = 128 KiB / curl 128²）。Perlin-Worley は `remap(perlin_fbm, worley_fbm−1, 1, 0, 1)` 形（HZD 資料に図あり）。天気マップは v1 では手続き 2D fbm + パラメータ（coverage / type / precip / 風向・流速）。
- **描画**: 1/4 解像度（1080p で 480×270）+ 4×4 時間再投影 + 空画素と遠景（深度 ≥ far）へバイラテラル合成。**カメラのみの再投影で足りる**（雲は遠い）。空パスの直後（§2.0.3 の (5)）。
- **雲影（A3）**: 太陽方向の 2D 透過率マップ（512²、カメラ追従、1/4 フレーム更新）→ Forward が `t28` を引いて太陽項へ乗算（雲の下が暗くなる）。UE も Beer Shadow Map [出典 UE Volumetric Cloud ドキュメント]。
- **コスト**: A2 ≤ 1.2 ms [推定]（Nubis Evolved の PS5・1080p・時間アップスケール無しで ≤ 2–3 ms [出典 https://www.guerrilla-games.com/read/nubis-evolved ] を目安に、1/4 + 時間再投影で下げる）。
- **参考にしてよい MIT 実装**（動作未確認、読むだけ）: `evroon/bevy-volumetric-clouds`（HZD ベース）、`clayjohn/godot-volumetric-cloud-demo`。Shadertoy（iq）は許諾面で使えない。

**却下した案**: Nubis3 のボクセル雲（NVDF 512×512×64・16.8 MB・圧縮 SDF の適応レイマーチ 2.2〜4.0 ms [出典 https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf ]。制作パイプラインが要る）/ 外部ノイズ画像の同梱 / 2D スカイドームのみ（視差・時間帯の光が出ない）。

#### 2.1.4 UE との比較上の注意（雲は形が一致しない）

雲の形はノイズ実装で変わるので、**画素比較は原理的に意味がない**。雲のゲートは「空領域の低周波統計（32 px ぼかし後の FLIP、被覆率、平均輝度/色度、太陽グロウの放射プロファイル）+ 目視」に落とす（§3.7）。空だけ・雲なしの画は 1:1 で比較できる（大気の合否はここで取る）。

---

### 2.2 水・海

#### 2.2.1 推奨: 専用パス（不透明の後・半透明の前）

**なぜカスタムシェーダ水では駄目か（[事実]）**: `shaders/templates/Water.hlsl` / `Ocean.hlsl` は `UnoGerstner`（`UnoCustom.hlsli:193`）で頂点を動かす**アルファブレンドのカスタムシェーダ**で、使える入力は b0 / b1 / t0+s0 だけ（memory `dx12-shading-traps`）＝**シーン色も深度も読めない**。`Water.hlsl:16-19` 自身が「屈折・岸の泡・スクリーンコピー/深度は非対応」と書いている。

**設計 [設計]**

- **`WaterBody` コンポーネント**（Transform の y = 水面高）: `waves`（Gerstner 12 波。スペクトルは `windSpeed` と `choppiness` から生成）/ `absorption`（m あたり RGB）/ `scatterColor` / `refractionStrength` / `foam`（波頭 + 岸）/ `reflectionRoughness`。
- **メッシュ**: カメラ中心のクリップマップ（5 リング × 64×64 クアッド、外側ほど倍、水平線まで）を 1 ドローで。頂点で Gerstner 変位、ピクセルで解析法線 + 手続き生成の 2 層ディテール法線（起動時 compute で 256² タイル、外部アセットなし）。
- **順序**: (1) sceneRT の不透明部を `opaqueColorCopy`（RGBA16F、1080p で 16 MB）へコピー、深度は既存の HiZ ミップ 0（R32F の全解像度深度）を流用できるか W1 冒頭で確認（無ければ 8 MB のコピー）(2) 水パス（RS は専用・`HEAP_DIRECTLY_INDEXED`）(3) 半透明。**水は深度を書く**（後段の FogComposite / AP が水面までの距離で計算するため）。速度は書かない。
- **シェーディング**: Fresnel（Schlick、F0≈0.02）で反射と屈折を混ぜる。屈折 = 法線で UV をずらした不透明色コピー（ずらし先が水より手前なら不採用）× **Beer 吸収** `exp(−σ·厚み)`（厚み = 不透明深度 − 水面深度）、散乱色を加算。反射 = ① 既存 SSR の DDA 関数（`ScreenSpaceCommon.hlsli`）を水法線で再利用 ② 外れたら空（A1 の SkyView LUT、無効なら IBL prefilter）③ RT1 が ON なら RayQuery（水パスの RS がバインドレスなので PS から直接撃てる）。太陽の反射は GGX（roughness は波勾配の分散から）。泡 = Gerstner ヤコビアンの波頭 + 深度差による岸（手続きテクスチャ）。
- **W2**: 泡・岸の仕上げ、**カウスティクス**（深度から世界座標を復元して太陽方向へ投影する**加算パス**＝デカールと同じ発想。Forward 無改造）、水中ビュー（カメラが水面下: 全画面 Beer 減衰 + フォグ）。
- **W3（条件付き）FFT 海洋**: 3 カスケード 256²（RGBA16F の変位 + 傾き + ヤコビアン泡、約 2.3 MiB）。Tessendorf（Choppy Waves 節あり [出典 https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2004.pdf ]、泡の「ヤコビアン」の語は当該資料で未確認＝UNVERIFIED）。実装の参考は MIT の `2Retr0/GodotOceanWaves`（TMA スペクトル・ヤコビアン泡）、`gasgiant/FFT-Ocean`。**Crest は 4 が MIT、5 は有償 Asset Store 版**なので参照範囲に注意 [出典 agent 調査、https://github.com/wave-harmonic/crest ]。UE の Water も波の合成が基本で、**W1 の Gerstner で UE の見た目には届く公算**があるため、FFT はパリティで不足が出たとき・外洋が要るときだけ。
- **コスト** [推定]: リングメッシュ頂点 0.3 ms、PS 0.6〜1.0 ms（画面を占める場合）、コピー 0.05 ms。**≤ 1.5 ms**。
- **UE 側**: Water プラグイン / Single Layer Water（専用パスで、下のシーン深度・色を読む。入力は Scattering / Absorption / PhaseG / Color Scale Behind Water [出典 https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine ]）。パリティ仕様の水パラメータ（吸収・散乱）は両エンジンへ同値を書く。**ゲート用は波振幅 0 の平水面（画素比較可）、動く水は統計 + 目視**。

**却下した案**: カスタムシェーダの水（上記）/ 平面反射（水面ごとにシーンを鏡像で再描画。ドローの 90% が影の現状 G17 では +50〜100%）/ SSR のみで空フォールバック無し（画面端・水平線で破綻）。

**VG・DXR との関係**: 水は半透明なので VG 対象外・TLAS 対象外（G10）。水面下の地形・岩は通常どおり描かれる。

---

### 2.3 植生・風・大量インスタンス

#### 2.3.1 問題の整理 [事実]

- 既存の描画は**エンティティ単位**（`BuildDrawList` が ECS を 1 回走査、G17）。`dx12_scatter` は 1 回 ≤ 200 個の個別エンティティ（G16）。10 万〜100 万の草木は**エンティティで持てない**（`buildList` の CPU、ヒエラルキー UI、シーン JSON、物理の都合）。
- 風・葉・草・インポスター・距離カリング設定は**全て無い**（G17）。
- MASK（アルファテスト）は深度/影/速度プリパスの 3 経路対応済み（`ShadowMask.hlsl` / `VelocityCommon.hlsli` の `-D ALPHA_TEST`）。RT 影は MASK を不透明扱い（G10）。

#### 2.3.2 推奨: `FoliageLayer` コンポーネント + GPU カリング + ExecuteIndirect

- **1 エンティティ = 1 レイヤ = N インスタンス**（1 モデル、LOD 配列 `lodModels[≤4]`、または LOD3 = インポスター）。インスタンス供給は 2 種: ①`scatter`（地形参照 + **スプラットのレイヤ重み**（G15）+ 密度 /m² + 傾斜・高さ範囲 + 間隔 + 乱数シード → 起動時に**決定論**で生成）②`list`（`.foliage` バイナリ）。生成結果は 32 m グリッドの**チャンク**（AABB + 個数 + オフセット）に詰める。1 インスタンス 32 B（位置 3f + yaw/scale の half + シード）、100 万で 32 MB。
- **毎フレーム**: (1) CPU でチャンク単位の錐台 + 距離カリング（数千チャンク）(2) compute でインスタンス単位（錐台球 + 距離フェード + 前フレーム HZB による遮蔽は任意）→ **LOD ごとの compact バッファへ Append**（`MeshInstanceData` 64 B レイアウトで吐く）+ `DrawIndexedInstanced` 引数を書く (3) `ExecuteIndirect`（コマンドシグネチャは `DrawIndexedInstanced` 引数のみ）で描画。
- **なぜ RS を触らずに済むか**: 間接描画でルート定数（b0 = 40 DWORD）を差し替えられないのが既存の制約（`CLAUDE.md:336-337`）。だが per-instance データを**頂点ストリーム（slot 1）**に置けば b0（転置 viewProj）は全インスタンス共通のまま。`ForwardInstanced` と同じ入力レイアウトなので **`Forward_PS` を共用**でき、PSO も既存の入力レイアウトで足りる。
- **影**: カスケード 0〜1 のみ（草は影を落とさない）。`ShadowMask` の**インスタンス版**（新規 `ShadowMaskInstanced.hlsl`）+ カスケード別カリング。速度プリパスも `VelocityPrepassInstanced` の ALPHA_TEST 版 + 風（今フレーム/前フレームの 2 回評価。`prevTime` は PerFrame 予約 `[7]`）。
- **風**（`shaders/foliage/FoliageWind.hlsli`）: GPU Gems 3 第 6 章（幹の主曲げ・枝・細部の 3 階層、風場を 2D テクスチャで VS から引く、GeForce 8800 GTX で 30 インスタンス 1.46 ms [出典 https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees ]）+ Crysis の頂点カラー規約（R = 葉の縁の硬さ、G = 葉ごとの位相、B = 全体の硬さ、A = 焼き込み AO。細部曲げは 4 本の三角波 周波数 1.975 / 0.793 / 0.375 / 0.193 [出典 https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis ]）。**共通風場** = PerFrame 予約 `[0..1]`（方向×速度・突風周波数/振幅・乱流・有効）+ 128² の突風ノイズ（起動時生成）。`Mesh` の頂点カラー（`Mesh.h:15-24` の offset 24）を規約に使う。新 VS は **`ForwardFoliage.hlsl`（新規）**にして `ForwardInstanced.hlsl` はビット不変。
- **アルファテストと AA**: MSAA が無い（TAA/FSR で解く）ので alpha-to-coverage は使えない。アセット側で **ミップ別のカバレッジ保存**（ミップが下がっても葉が痩せない）を cook 時に行う規約とする。
- **草（F2）**: 保存せず**毎フレーム生成**。地形タイル（チャンク）× 密度（草レイヤ重み + ノイズ）から compute が可視ブレードを compact ストリームへ（距離リング: 0〜15 m フル / 〜40 m 半分 / 〜60 m 1/4）。ブレード形状は VS が `SV_VertexID` から生成（3 セグメント・ベジェ曲げ・7 頂点）。Ghost of Tsushima（GDC 2021、Eric Wohllaib）の方式に相当するが、**ブレード数・制御点などの数値は動画を確認できておらず UNVERIFIED**（講演ページ https://gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass ）。参考実装 `2Retr0/GodotGrass`（MIT）。PS は `Forward_PS` 共用、法線は曲げ + 両面 wrap。草は影を落とさず**受ける**。
- **インポスター（F3）**: 八面体インポスターを**エンジン自身でベイクする**ツール（`dx12_bake_impostor`）+ ビルボード VS（3 ビュー補間）+ ディザ・クロスフェード。アトラス 2048² BC7 ×（albedo+alpha / 法線+深度）＝約 8 MB/種。Ryan Brucks の実装記事は存在するが本文を確認できず詳細 UNVERIFIED（https://shaderbits.com/blog/octahedral-impostors ）ので**方式は自前設計**。LOD にはインデックス簡略化を**使わない**（G17: カード形状が崩れる）。
- **コスト** [推定]: パリティ屋外シーン（樹木 200 本 × 2 LOD + 草 30 万ブレード）で描画 1.5〜2.5 ms + 影 0.8 ms。ドロー数は数十本。

**VG との住み分け**: 植生（MASK）は**VG の対象外**（VG 書 §2.2.2・R13）。幹だけの高ポリ opaque を将来 VG に載せる余地は残すが v1 では扱わない。**UE 側は Nanite Foliage（Experimental）を使わず**通常の Foliage/ISM でパリティを取る（§3.4）。

**RT との関係（RT2）**: 100 万インスタンスは TLAS（32768 上限、G10）に入らない。**近景の樹木（≤ 2000 インスタンス）のみ**any-hit 付きで TLAS へ、草は入れない（RT 影は CSM の草なし影 + コンタクトで代替）。

**却下した案**: エンティティ 10 万個 / Nanite Foliage 相当（ボクセル・アセンブリ。UE 5.8 でも Experimental で、ドキュメント自体が使用注意 [出典]）/ 毎フレーム CPU カリング / meshlet 化（既存の却下方針）/ ジオメトリシェーダによる草（GS の性能）。

---

### 2.4 特殊シェーディング（肌・髪・布・クリアコート・異方性）

#### 2.4.1 G-Buffer / resolve への組み込み方 [設計]

現状のフォワードは G-Buffer に材質情報を持たない（法線/roughness/metallic だけ、G12）ので、**シェーディングモデルは材質側（b2）に置くのが自然**。

- **ID の置き場**: `pbrFlags` **bit4..7 = `shadingModel`（16 種）**（G18: 未使用）。0 = 現行の Default（**従来経路とビット一致**）。
- **拡張パラメータ**: `pbrFlags` **bit16..31 = `extraIndex`**（65536 材質）。実体は CPU が管理する `StructuredBuffer<MaterialExtra>`（48 B）を **slot 11 の `t27`** に貼る（フレーム共通のバインド。VG resolve はバインドレスで同じバッファを引く）。**RS 変更ゼロ**。
- **分岐**: 材質単位で一様（b2 由来）なので同一ドロー内は分岐が揃う。`PBR.hlsli` に `ShadePunctualEx` / `ShadeIblEx` を足し、`Forward.hlsl`・`ForwardSkinned.hlsl`・`Terrain.hlsl`・`VgResolve.hlsl` が**同じ関数**を呼ぶ（VG 書 R9「PS コピーのドリフト」対策と同じ考え方）。
- **G-Buffer / 反射系**: SSR / SSGI / RT 反射が読む roughness / metallic / 法線は**ベース層のまま**（クリアコートの第 2 ローブは反射系に出さない）。SSS も G-Buffer には足さない。
- **VG との関係**: VG は頂点に接線を持たない（cotangent frame 代用、VG 書 §2.2.6）ので、**異方性・髪は VG 対象外**（cook が `NONVG` セクションへ隔離、VG 書 §2.2.2 の表に 1 行足す）。クリアコート・布・肌は resolve で対応可能（接線不要）。

| ID | モデル | 実装（出典） | 主な追加 |
|---|---|---|---|
| 0 | Default | 現行 | — |
| 1 | ClearCoat | GGX + Kelemen 可視項 + Schlick（IOR 1.5 固定 → F0=0.04）、ベース層は減衰（Filament [出典 https://google.github.io/filament/Filament.html 、Apache-2.0。式を読んで独自実装]） | `clearcoat`, `clearcoatRoughness`。IBL は第 2 プリフィルタサンプル |
| 2 | Anisotropic | 異方性 GGX（Burley 2012 [出典 https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf ]、Kulla & Conty 2017 [出典 https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_slides_v2.pdf ]） | `anisotropy`, `anisotropyRotation`。**接線が要る**（`PSInput` に `worldTangent/tangentW` あり、G12） |
| 3 | Cloth（Sheen） | Charlie NDF（Estevez & Kulla 2017 [出典 https://fpsunflower.github.io/ckulla/data/s2017_pbs_imageworks_sheen.pdf ]）+ Neubelt 可視項。IBL は **Charlie DFG LUT（64²、起動時 compute）を `t29`（slot 6 IBL テーブルの 4 本目）** | `sheenColor`, `sheenRoughness` |
| 4 | Skin | **プレインテグレート（曲率 LUT）+ 影厚みによる透過**。LUT は起動時 compute で生成（外部アセットなし）。曲率は `length(fwidth(N))/length(fwidth(P))`（Forward は画素微分が使える。VG resolve は解析微分版） | `sssColor`, `sssWrap`, `thickness` |
| 5 | Hair | **Kajiya-Kay 2 ローブ**（主ハイライト + 接線シフトした 2 次着色ハイライト）+ 環境の散乱近似。カードは MASK（既存）。Karis 2016 の Marschner 分解 R/TT/TRT [出典 https://blog.selfshadow.com/publications/s2016-shading-course/karis/s2016_pbs_epic_hair.pdf ] を 2 ローブへ縮約 | `hairShift1/2`, `hairExp1/2`, `hairColor2` |
| 6 | Foliage（薄い透過） | 両面 wrap + 透過項（Crysis 流 [出典 GPU Gems 3 第 16 章]） | `transmissionColor` |
| 7〜15 | 予約 | Eye / Glass / Transmission 等（本書では作らない） | |

- **glTF 拡張の入口**: `KHR_materials_clearcoat` / `_sheen` / `_anisotropy` / `_transmission` / `_volume` / `_ior` / `_specular` / `_emissive_strength` は**批准済み**、`KHR_materials_diffuse_transmission` は Release Candidate、**`KHR_materials_subsurface` は Initial Draft（未批准）**（https://github.com/KhronosGroup/glTF/blob/main/extensions/README.md ）。＝**肌と髪は glTF から来ない**（Uno のマテリアルアセット / MCP `dx12_set_pbr {shadingModel,…}` で指定）。assimp がこれらの拡張キーをどこまで読むかは **UNVERIFIED**（SH1 の初日に `aiMaterial` のキーを確認し、足りなければ `ModelLoader` が glTF JSON を直接読む）。
- **多重散乱エネルギー補償**（UE との差の一つ、G4）: SH1 に含める（IBL の split-sum LUT から `1 + F0·(1/DFG.y − 1)` の形。Filament 記載、1 日）。
- **マテリアルグラフとの接続**: `MaterialExtra` のフィールド名を **「出力ピン」の仕様**として `docs/MATERIAL_OUTPUTS.md`（SH1 で新規）に固定する（`shadingModel` / `clearcoat` / `clearcoatRoughness` / `anisotropy` / `anisotropyAngle` / `sheenColor` / `sheenRoughness` / `subsurfaceColor` / `thickness` / `opacityMask`）。別設計書のマテリアルグラフは、これを出力ノードの枠として接続する。

#### 2.4.2 肌の SSS を「分離型スクリーンスペース」にするか

- 分離型 SSS（Jimenez 2015、**0.5 ms 未満**・2 回の 1D 畳み込み [出典 https://www.iryoku.com/separable-sss/ ]）は「拡散と鏡面の分離」が前提。フォワードは**単一出力・MRT 化禁止**（確定済み）なので、そのままでは入れられない。
- **案**: 不透明の α は常に 1（`Forward.hlsl:363`）＝**α が空いている**。SSS 画素だけ `a = 2 + 鏡面輝度`（皮膚の鏡面はほぼ無彩色、F0=0.028）と符号化すれば、後段で「印」と「分離」が取れる。**他パスが sceneRT の α を読んでいないか**（FogComposite のブレンド、SSR/SSGI の色退避、TAA）を SH3b の冒頭スパイク（0.5 日）で確認してから採否を決める。
- **ライセンス注意**: `iryoku/separable-sss` は**MIT ではない**（表記必須の BSD 系。GitHub API の SPDX は NOASSERTION [出典 https://github.com/iryoku/separable-sss/blob/master/LICENSE.txt ]）。実装するなら論文から独自に書き、クレジットを入れる。
- **推奨**: まず **プレインテグレート版（SH3、コピーなし・既存パス無変更）**で UE の Subsurface Profile との差を FLIP で測り、足りなければ SH3b（条件付き、5 日）。

**UE 側**: Substrate は 5.7 以降で production-ready、新規プロジェクトは既定有効、Adaptive GBuffer 既定 80 B/px [出典 https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine ]。**パリティでは UE の現行既定（Substrate ON）を基準にする**が、Uno はレイヤ合成を模倣しない（固定モデル ID のみ）。肌/髪のパリティは緩いしきい値（§3.7）+ 目視。

**却下した案**: フォワード PS の MRT 化（確定済みの禁止）/ Marschner ストランド髪（UE の Hair はグルーム描画方式で別物、8GB のメモリ・描画方式が違う）/ Substrate 相当のレイヤ合成 / スクリーンスペース SSS を最初から（分離の前提が満たせない）。

---

### 2.5 ハードウェア RT 反射 / GI の位置づけ / アップスケーラ

#### 2.5.1 RT 反射（RT1）

- **入力**: 深度 + G-Buffer（oct 法線・roughness・metallic、G12）。**G-Buffer に albedo が無い**ので、RT が返すのは「反射放射輝度」だけにし、**F0 との掛け算は Forward 側**（SSR と同じ契約、G9）。
- **出力**: **SSR と同じ形式のテクスチャ（RGBA16F: rgb = 反射放射輝度、a = confidence）を `t16` へ書く**。Forward は無改造。SSR を先に回し、confidence が低い画素（画面外・遮蔽・roughness が範囲）だけ RT を撃つ「スクリーントレース優先 → 外したら RT」（Lumen と同じ分担）。
- **トレース**: `RtReflectionPass`（`RtScreenPass` の系列、専用 RS + バインドレス）。ハーフ解像度・1 レイ/画素、roughness < 0.6 のみ、GGX の VNDF サンプリング（roughness < 0.05 はミラー）。**ヒット shading は DDGI のもの（G11）を `RtHitShade.hlsli` へ切り出して共通化**: albedo Lambert + 太陽（影レイ）+ 全点光源 + **DDGI プローブの間接光** + emissive。ミスは A1 の SkyView LUT（無効なら IBL prefilter）。
- **デノイズ**: 速度による時間再投影蓄積 + roughness/深度/法線重みの à-trous（3×3 を 2 回）。NRD は使わない（**MIT ではなく NVIDIA RTX SDKs LICENSE**、REBLUR_DIFFUSE_SPECULAR は 2.55 ms＠RTX 4080・1440p [出典 https://github.com/NVIDIA-RTX/NRD ]）。AMD の旧 FidelityFX Denoiser / SSSR（MIT）は **v1.1.4 以前のツリーにのみ存在**（v2.x に無い）[出典 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK ]ので参考実装止まり。
- **限界（v1）**: 半透明はレイが当たらない・ガラス面自体の反射は IBL + SSR まで（G10）。TLAS はプロキシ・LOD0（VG 書 F16）。TLAS の再構築は静的シーンなら再利用（Dead_Mall で 53 → 115 fps の実績）、動的 2 万インスタンスで +11.5 ms（memory）。
- **コスト** [推定]: 1.5〜2.5 ms。

**RT2（any-hit / MASK）**: MASK の BLAS ジオメトリを `NON_OPAQUE` にし、RayQuery の候補ループで `CandidateType()==NON_OPAQUE_TRIANGLE_CANDIDATE` のときバインドレスで UV とアルファを引いて `CommitNonOpaqueTriangleHit()`（DXR 1.1 の標準手順。仕様書の該当節の URL 取得は着手時に確認＝本書では未取得）。RT 影・RT-AO・DDGI・RT 反射の全部に効く（現状 MASK は不透明扱いで葉が板の影になる、memory）。

**RT3（DDGI ヒット強化）**: emissive・tint・（可能なら）ORM の roughness を反映、ミスを大気へ。**`DdgiProbeUpdate.hlsl` は VG 書 P8 のカスケード作業と同一ファイル**なので P8-1 の後に直列。

#### 2.5.2 GI の位置づけ（蒸し返さない）

- **GI は DDGI に確定**（memory `dx12-ddgi`: SHaRC / Radiance Cascades / RTXGI SDK / ReSTIR 系は却下済み）。本書は**新しい GI 手法を足さない**。RT GI の強化 = ① P8（カスケード・リロケーション、VG 書）② RT3（ヒット shading の質）③ RT1 のヒット shading が DDGI を引く（＝反射の中の間接光が出る）。
- 追い風の事実: UE 5.8 は Lumen Lite（medium quality GI、Beta）を Irradiance Fields で出した [出典 UE 5.8 リリースノート https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes ]。プローブ系が Epic 自身の中品質の答えでもある。
- **8 GB の壁**: DDGI（最大 4096 プローブ）+ TLAS/BLAS + VG の DAG（約 1.25 GB）+ RT 反射 RT。§5 の VRAM 表で管理する。

#### 2.5.3 アップスケーラ: FSR 3.1 を直接統合（UP1）

**事実 [出典]**

- 最新は AMD FSR SDK **v2.3.0「Redstone」（2026-06-24）**: FSR Upscaling 4.1.1 / FSR3 Upscaler 3.1.5 / FSR2 2.3.4 / Frame Generation 4.0.1 / Ray Regeneration 1.2.0（https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases ）。
- **ライセンスは 2 層**（https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/docs/license.md ）: API ヘッダ・サンプル・**FSR2/3.1 のソースは MIT**。**FSR4 / Frame Generation / Ray Regeneration の署名済み DLL は「バイナリ再配布のみ可・リバース禁止」の別ライセンス**（「SDK 全体が MIT」ではない）。
- **FSR4 は RX 7000/9000 のみ**（SM6.6 必須）。それ以外の GPU では API が自動で **FSR 3.1.5** を選ぶ（https://gpuopen.com/amd-fsr-sdk/ ）。**RTX 5060 では FSR 3.1 は動くが FSR4 は動かない**。FSR4 の INT8 版は非公式（コミュニティ）で対象外。
- v2.x の DLL 構成: `Kits/FidelityFX/signedbin/` に `amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll` 等（旧 `amd_fidelityfx_dx12.dll` は廃止）。ABI は `ffxCreateContext / ffxDestroyContext / ffxConfigure / ffxQuery / ffxDispatch`、`LoadLibrary + GetProcAddress` 推奨（https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/getting-started/ffx-api.md ）。
- 入力（`ffx_upscale.h` を実読）: `color` / `depth` / `motionVectors` / `exposure`（1×1、任意）/ `reactive` / `transparencyAndComposition`（どちらも R8_UNORM、任意）/ `output` / `jitterOffset` / `motionVectorScale` / `renderSize` / `upscaleSize` / `frameTimeDelta`（**ms**）/ `preExposure` / `reset` / `cameraNear/Far/FovAngleVertical` / `viewSpaceToMetersFactor`。生成フラグ: `HIGH_DYNAMIC_RANGE` / `DEPTH_INVERTED` / `DEPTH_INFINITE` / `AUTO_EXPOSURE` / `DYNAMIC_RESOLUTION` / `MOTION_VECTORS_JITTER_CANCELLATION` 等。**深度は reverse-Z の無限遠を強く推奨**。補助: `GetJitterPhaseCount` / `GetJitterOffset` / `GetRenderResolutionFromQualityMode`。品質モード: Native AA 1.0× / Quality 1.5× / Balanced 1.7× / Performance 2.0× / Ultra Perf 3.0×。
- メモリ: FSR 3.1 は 1080p Quality で 75 MB、Performance で 61 MB。**RTX 上の ms は公式値なし（UNVERIFIED）**（FSR4 の 352 µs は RX 9070XT・Performance の値で無関係）。

**Uno の入力の揃い具合（G12）と作業**

| FSR の入力 | Uno の現状 | UP1 での作業 |
|---|---|---|
| color | sceneRT `RGBA16F` リニア HDR・トーンマップ前・露出前 | `HIGH_DYNAMIC_RANGE` フラグ。**現行の順序（露出は uber パス）を保つ**ため `preExposure=1`。太陽円盤（≫1）でのチラつきが出るなら AE バッファの値を 1×1 R32F の `exposure` へコピー（0.01 ms） |
| depth | D32_FLOAT・**標準 Z**（near=0、`HiZMath.h:11-20` に根拠あり） | 標準 Z のまま FSR に渡せるが reverse-Z 推奨のため、**反転コピー（R32F、約 0.03 ms）**を作り `DEPTH_INVERTED` で渡す。エンジンの標準 Z 方針は変えない |
| motionVectors | R16G16F、`cur−prev`、ビューポート局所 UV、ジッタ除去 | `motionVectorScale` は ±(w, h)。**符号は初日に「動く箱」で実測**（FSR の規約の該当箇所は未確認＝UNVERIFIED） |
| jitter | Halton(2,3) 8 サンプル固定、NDC | 位相数 = `GetJitterPhaseCount(renderW, displayW)`（値は API が返す。FSR 系の目安は 8×(表示/描画)² だが本書は未確認）。ピクセル単位のオフセットを NDC へ変換（`TaaJitter.h:41-48` の逆） |
| reactive / T&C | **無い**（G12） | **不透明のみの色（水パスの `opaqueColorCopy`）と最終色の差から自前 compute で R8 マスクを作る**（水・パーティクル・半透明を 1 枚で覆える、0.05 ms）。FSR 側の生成ヘルパの有無は未確認（UNVERIFIED）なので当てにしない |
| camera | `Camera::GetNearZ/GetFarZ/GetFovY`（`Camera.h:37-40`） | そのまま |
| **ポストの解像度** | **全ポスト RT がレンダー解像度**（`ApplyRenderResolution` `ApplicationPipeline.cpp:574-653`） | **UP1 の主要作業（約 5 日）**: FSR 出力（表示解像度 HDR）を起点に DoF / MB / AE / Bloom / GodRays / LensFlare / uber を表示解像度へ。シーン空間のパス（SSAO/SSR/SSGI/RT/フォグ/DDGI）はレンダー解像度のまま。深度・速度はレンダー解像度を UV 直サンプルで読む |
| **テクスチャ LOD バイアス** | s0 は静的サンプラ（`MipLODBias` を動的に変えられない） | アップスケール時は `−log2(表示/描画)` 相当のバイアスが要る。Forward/Terrain/Foliage の `Sample` を `SampleBias` 化（`#ifdef UPSCALE_MIPBIAS`）、VG resolve は `SampleGrad` の勾配スケールで対応 |
| TAA との関係 | TAA は既定 OFF・独立 | FSR ON のとき TAA resolve を置換（ジッタ・履歴の責任を FSR へ）。SSR/SSGI の色退避（レンダー解像度）は FSR の前に取る（現行どおり） |

**統合方式**: **`LoadLibrary` + `ffx*` の薄いラッパ（`FsrUpscaler.{h,cpp}`）**。理由: FSR3.1 のソース静的リンクは MIT で可能だが、シェーダ順列のビルドが重く（`tools/build.ps1` で CPU を絞る方針と相性が悪い）、ABI が小さい DLL 動的ロードのほうが軽い。DLL は署名済みバイナリで再配布条件が別なので、UP1 の初日に `docs/license.md` を再読して**同梱可否を確定**（可なら `third_party/` 外の配布ビルドにのみ同梱、git には入れない）。DLL が無い環境ではアップスケール項目を無効化（縮退）。将来 AMD の RDNA3/4 で使う人向けには **DLL 差し替えだけで FSR4 が選ばれる**ので、FSR4 固有の実装はしない（**開発機で検証できない機能は作らない**）。

**DLSS（UP2、保留）**: 技術的には可能（NGX 直叩き `NVSDK_NGX_D3D12_Init_with_ProjectID`、エンジン種別 CUSTOM、ProjectID は GUID 形式の任意文字列で可、NVIDIA 発行 ID 不要）。最新 v310.9.1（2026-09-08）、`nvngx_dlss.dll` 約 59 MB、RTX 5070 の 1080p Performance で 0.64〜0.75 ms（K/M プリセット）・追加 VRAM 86〜116 MB [出典 https://github.com/NVIDIA/DLSS ]。RTX 5060 で 1.0〜1.2 ms 程度と推定（UNVERIFIED）。**ライセンス**（NVIDIA RTX SDKs LICENSE 2024-03-14 + DLSS 補足 [出典 https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt ]）: バイナリの無償再配布は可。ただし ① **SDK がオープンソースライセンスの対象になる使い方は禁止（4.e）** ② **NVIDIA GPU 専用** ③ **商用リリース前に NVIDIA へ通知** ④ スプラッシュ/クレジットに NVIDIA の表記 ⑤ 品質問題を放置すると NVIDIA が統合を無効化できる ⑥ OTA でプリセットが更新され得る。**Uno Engine のリポジトリは MIT（`LICENSE`）**なので、DLL を git に入れると ① に抵触しうる → **配布ビルドにのみ同梱、要法務確認**（質問先 `nvidia-rtx-license-questions@nvidia.com`）。DLSS Ray Reconstruction は Streamline 不要だがドライバ 580+・RTX 5070 で 1.59 ms と重く、入力（specular albedo 等）が現行に無いので**対象外**。XeSS（SDK 3.0.2、SM6.4 DP4a で全 GPU 動作、Intel 独自ライセンス・バイナリのみ）は 3 番手。

**TSR 自前 vs FSR 3.1（推奨は FSR）**: 品質の一般評価（DLSS 4.5 > FSR 3.1 ≒ XeSS DP4a > 自前 TAAU）は**一次資料で確認できていない（UNVERIFIED）**。工数は「表示解像度側ポストの分離」が**どちらでも必要**で同等（TSR 8 日 vs FSR 10 日）。決め手は、FSR は**実績のある実装をそのまま得られる**ことと、UE の TSR に対する差を測る責任を自前 TSR のチューニングに負わせずに済むこと。VG 書 P9 は TSR 部分を UP1 に置換（DoF / Bloom は残す）。

---

## 3. パリティ・ハーネス設計（UE 5.8 との並べ比較）

### 3.1 設計方針（6 つ）

1. **UE を起動する回数を最小にする**。UE の基準画像は「シーン仕様のハッシュ + UE ビルド + UE 設定ハッシュ」をキーに**キャッシュ**し、仕様を変えたときだけ作り直す。日常の反復（Uno 側の改修 → 比較）は**UE を一切起動せず**、Uno だけをヘッドレスで回して既存の基準画像と比べる。
2. **UE の起動は fail-closed**。人間の承認（§3.3 の承認ファイル）が無ければ**起動しない**。AI が UE を起動できる MCP op は**作らない**（`dx12_parity` に `ref_make` を置かない）。過去にユーザーが AI にカーソルを奪われて怒った経緯（`CLAUDE.md:43-48`「実入力でエディタを操作しない、`--background` + 仮想入力を使う」）を、設計の不変条件にする。
3. **両エンジンに「同じソース」を渡す**。Dead_Mall の cook 済みアセットは UE エディタへ戻せない（`.uasset` は cook 済みで編集不可。VG 書 R3 も同認識）ので、**「パリティ・シーン仕様（JSON）+ 共通ソースアセット（glTF/FBX/PNG）」から両方を組み立てる**。
4. **2 層で比べる**。Tier A = **線形 HDR（露出を揃えたシーン参照）**、Tier B = **表示参照（UE Filmic 相当のトーンマップ後 PNG）**。トーンマップ差とレンダリング差を分けて見る。
5. **しきい値は「ノイズ床」から決める**。UE 同士（再撮影）・Uno 同士（Uno は決定論でほぼ 0、G14）の差を測り、合格線をその 1.5 倍以上に置く。ノイズ以下の差を追わない。
6. **「1:1 で画素比較する項目」と「統計で比べる項目」を最初に分ける**。雲・動く水・風・乱数配置は画素が原理的に一致しないので統計 + 目視（§3.7）。

### 3.2 UE 側で確認できた事実（[出典]。UNVERIFIED は明記）

**ローカル事実（`ls`・ログの読み取りのみ。UE は一切起動していない）**

- **`C:\Program Files\Epic Games\UE_5.8` は空**（0 ファイル・0 フォルダ、最終更新 2026-08-12）。Launcher の manifest `C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests\63CE813DCA5A17E7486030FACA366A42.item` は "5.8.0-55116800+++UE5+Release-5.8-Windows" を `bNeedsValidation=True` で持つだけ。`EpicGamesLauncher.log`（2026-09-12）は「manifest が見つからない」を出し、最新提示は **5.8.2-56702186**。**再インストールが必要**。
- 過去ログ `C:\Users\ryuto\Documents\Unreal Projects\AIMCPTEST\Saved\Logs\AIMCPTEST.log`（2026-07-07）: `5.8.0-55116800+++UE5+Release-5.8`、`Installed Engine Build: 1`（Launcher 版）、Win11 25H2 / i7-14700F / RTX 5060 / D3D12 SM6.6 / atomic64 対応、**シェーダコンパイル並列 14 ワーカー**、`Compressed.ddp`（同梱 DDC pak 1667 MiB）。`PythonScriptPlugin` は **`Engine/Plugins/Experimental/`**（実験扱い）、`EditorScriptingUtilities` / `Interchange*` / `GLTFExporter` / `DatasmithContent` / `SequencerScripting` / `ModelContextProtocol` / `AllToolsets` がマウントされていた。**MovieRenderPipeline は当該プロジェクトでマウントされておらず、有効化が要る（UNVERIFIED）**。UE のプロジェクトは `AIMCPTEST`（C++ サードパーソン）のみ。共有 DDC（`%LOCALAPPDATA%\UnrealEngine\Common\DerivedDataCache`）は現在 0 MiB（7 月は 318 MiB）＝初回は全シェーダコンパイル。
- Python 3.12.10 導入済み。numpy 2.4.6 / opencv-python-headless 5.0.0.93 / pillow / scipy / matplotlib / torch 2.11 / torchmetrics 1.9.0 あり。**scikit-image・flip-evaluator・imageio・colour-science・lpips・OpenEXR は未導入**。

**公式ドキュメント**

| 項目 | 事実 | 出典 |
|---|---|---|
| `-RenderOffScreen` | コマンドライン一覧に "Render off screen" として記載。Pixel Streaming 文書は「**アプリはウィンドウを一切表示せず、フルスクリーンでも描画しない**」と説明（**パッケージ版向けの記述**。`UnrealEditor(-Cmd).exe` / `-game` で同じ挙動かは **UNVERIFIED**） | https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-command-line-arguments-reference 、https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-pixel-streaming-reference |
| 併用フラグ | `-NullRHI`（描画なし＝スクリーンショットには不適）/ `-nosplash` / `-unattended`（入力不可・ダイアログ抑止）/ `-ExecCmds="a,b"` / `-ForceRes -ResX -ResY` / `-windowed` / `-stdout` / `-log` / `-NoLoadingScreen` / `-Deterministic`（= `-UseFixedTimeStep -FixedSeed`）/ `-FPS=`。`-hidden` `-ThreadCount` `-NumCores` `-NoShaderCompile` `-run=` `-ExecutePythonScript` は**一覧に無い**（別文書にある `-run=pythonscript` は下記） | 同上 |
| 既知の落とし穴（低信頼度: フォーラム） | Windows 11・UE 5.0.3 パッケージ版で `-RenderOffScreen` の出力がデスクトップ解像度で切れた（3 台中 2 台）→ `-ForceRes` を付け、まず解像度をデスクトップ以下で検証。RTX 環境で DLSS Frame Generation 有効のパッケージ版が `-RenderOffScreen` で Streamline プラグインからクラッシュした報告（回避 `r.Streamline.InitializePlugin=False`） | https://forums.unrealengine.com/t/renderoffscreen-large-resolutions-not-working/671085 、https://forums.developer.nvidia.com/t/streamline-plugin-crashes-in-unreal-5-3-packaged-game-with-renderoffscreen-command/268872 |
| Movie Render Queue の CLI | `UnrealEditor-Cmd.exe X.uproject Map -game -LevelSequence="/Game/…" -MoviePipelineConfig="/Game/…" -windowed -resx=… -resy=… -log -notexturestreaming`。サポート引数は絞られている。Python エグゼキュータ（`MoviePipelinePythonHostExecutor`、`-ExecutorPythonClass`）もある。**`-RenderOffScreen` との併用は文書に無い（UNVERIFIED）** | https://dev.epicgames.com/documentation/en-us/unreal-engine/using-command-line-rendering-with-move-render-queue-in-unreal-engine |
| MRQ の出力・AA | PNG は 8bit sRGB、**EXR は HDR 値を保持**。空間/時間サンプル数、**Render Warm Up Count**、AA 上書き（None/FXAA/TAA）、レンダ開始時に適用され後で戻る Console Variables、テクスチャストリーミング上書き（FullyLoad）。**UE 5.8 で Movie Render Graph が Production Ready** | https://dev.epicgames.com/documentation/en-us/unreal-engine/cinematic-render-settings-and-formats-in-unreal-engine 、…/cinematic-rendering-image-quality-settings-in-unreal-engine 、UE 5.8 リリースノート |
| Python | 経路 2 本: ① `UnrealEditor-Cmd.exe X.uproject -ExecutePythonScript="s.py"`（**フルエディタを起動**して既定マップを読む）② `-run=pythonscript -script=…` コマンドレット（「エディタ UI を開かずヘッドレスで動く・高速」だが「レベルを自動で読まない」） | https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python |
| コマンドレットの描画 | Epic スタッフの回答（フォーラム・低信頼度）: `-nullrhi` は描画を無効化、描画するなら `-AllowCommandletRendering`（GPU が無ければ `-AllowSoftwareRendering`）、NullRHI を避けて headless にするのが `-RenderOffscreen`。Nanite がコマンドレットで間引かれ `CaptureScene` を回すまで出ない報告あり | https://forums.unrealengine.com/t/static-mesh-merging-requires-rhi/2731294 |
| スクショ API | `unreal.AutomationLibrary.take_high_res_screenshot(res_x, res_y, filename, camera, mask_enabled, capture_hdr, …, delay, force_game_view)`（FunctionalTesting、**ライブのエディタビューポートが要る**。コマンドレットでは不可と推定＝UNVERIFIED）。`HighResShot filename=… (XxY) bCaptureHDR` は EXR を出す（文書は PIE/スタンドアロン向けで CLI の記載なし） | https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/AutomationLibrary 、…/taking-screenshots-in-unreal-engine |
| 決定論 | 露出: 既定は Histogram、**Manual は `Exposure = 1/(2^(EV100+Compensation))`**、min=max 明るさで自動露出無効（`r.EyeAdaptationQuality` / `r.DefaultFeature.AutoExposure` あり）。**TSR は非決定論**（収束は fps と screen percentage 依存、`r.TSR.History.SampleCount` 既定 16）。**Lumen は更新が複数フレームに償却**（収束速度は PostProcess の Final Gather Lighting Update Speed / Lumen Scene Quality）。テクスチャ: `r.Streaming.FullyLoadUsedTextures=1`。トーンマップ既定は **ACES 準拠の Filmic**（Slope 0.88 / Toe 0.55 / Shoulder 0.26 / Black Clip 0 / White Clip 0.04）。`r.RandomSeed` `r.Nanite.Streaming.*` `r.Streaming.PoolSize` は **UNVERIFIED** | https://dev.epicgames.com/documentation/en-us/unreal-engine/auto-exposure-in-unreal-engine 、…/temporal-super-resolution-in-unreal-engine 、…/lumen-technical-details-in-unreal-engine 、…/texture-streaming-configuration-in-unreal-engine 、…/color-grading-and-the-filmic-tonemapper-in-unreal-engine |
| 5.8 の描画機能 | MegaLights が Production Ready、Lumen Lite（中品質 GI、Beta）、Fog Screen Space Scattering（Experimental）、Substrate NPR（Experimental）、Movie Render Graph Production Ready。5.7: Substrate Production Ready、Nanite Foliage Experimental。**Nanite Foliage は 5.8 ドキュメントでも Experimental** | UE 5.8 リリースノート、https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage |
| Windows のデスクトップ | ウィンドウはデスクトップに属し、**入力デスクトップだけが表示され入力を受ける**。`CreateDesktop` で別デスクトップを作れ、`STARTUPINFO.lpDesktop` で新プロセスの所属を選べ、`wShowWindow`（`STARTF_USESHOWWINDOW`）で最初のウィンドウの表示状態を指定できる | https://learn.microsoft.com/en-us/windows/win32/winstation/desktops 、…/nf-winuser-createdesktopw 、…/ns-processthreadsapi-startupinfow |

**UE 側で分かっていない・試さないと分からないこと（Q0 スパイクの対象）**: ①`-RenderOffScreen` 付きの `UnrealEditor-Cmd.exe` / `-game` が Windows・D3D12 で**本当に窓・タスクバー項目・フォーカス奪取を出さないか** ② MRQ が `-game -RenderOffScreen` で動き出力するか ③ コマンドレット + `-AllowCommandletRendering` の描画が正しいか（Nanite/Lumen/VSM）④ 専用デスクトップ上で D3D12 + NVIDIA ドライバが動くか ⑤ 初回のシェーダ/DDC コールドスタート時間と CPU 負荷 ⑥ Lumen/TSR の収束に要るフレーム数 ⑦ 5.8.2 への更新で挙動が変わるか。

### 3.3 UE を「ユーザーの邪魔をせずに」走らせる設計

**不変条件（設計の憲法）**: ① AI は UE を承認なしに起動しない ② UE は**キーボード・マウス・フォアグラウンドに一切触らない**（入力注入・`SetForegroundWindow`・`SetCursorPos` を使わない。読み取りのみ） ③ ユーザーの PC を重くしない（CPU/メモリ上限を OS レベルで掛ける） ④ 失敗時は**黙って窓を出し続けるより、殺して報告**する。

**A. 起動前ゲート（fail-closed）**

- `tools/parity/run_ue.ps1` は最初に**承認ファイル** `%LOCALAPPDATA%\uno-parity\UE_RUN_APPROVED` を確認する。**内容 = 承認時刻とシーン仕様ハッシュ、有効期限 60 分**。ファイルは**ユーザーが `approve_ue_run.bat`（ダブルクリック）で作る**。AI がこのファイルを作ることは運用ルールで禁止（`tools/parity/README` と `CLAUDE.md` に明記）。無ければ「実行計画（コマンドライン・予想時間・予想負荷・窓の扱い）」を表示して**終了コード 2 で止まる**。
- 実行計画には「**どの UE のどのモード**で、**窓が出る可能性が何%あると設計上見ているか**」を必ず含める（下記のレベル）。

**B. 窓の抑止（4 段。上から順に試し、Q0 スパイクで採用レベルを確定）**

| レベル | 手段 | 根拠 | 状態 |
|---|---|---|---|
| L1 | `-RenderOffScreen -unattended -nosplash -NoLoadingScreen -ForceRes -ResX/-ResY -nosound`（+ `-stdout -log` でログのみ） | 公式のコマンドライン一覧 | エディタ/`-game` での効果は UNVERIFIED |
| L2 | ランチャが `CreateProcess` で `STARTF_USESHOWWINDOW` + `SW_HIDE`、`CREATE_NO_WINDOW`、**Job オブジェクト**に入れる（下記 D） | Windows 公式（STARTUPINFO） | 実装容易。最初の窓のみ有効で、後から作られる窓は防げない |
| L3 | **専用デスクトップ**（`CreateDesktop` した非入力デスクトップへ `lpDesktop` で起動）。入力デスクトップだけが表示・入力を受けるので、**UE が窓を出してもユーザーの画面に出ず、カーソルにも触れない** | Windows 公式（Desktops） | **最強の隔離**。ただし「非入力デスクトップで D3D12/NVIDIA が動くか」は UNVERIFIED（Q0 で検証） |
| L4 | 監視（常時）: 20 Hz で `EnumWindows` して**そのプロセスの可視トップレベル窓**（タイトル・矩形・可視）と `GetForegroundWindow` の変化を JSON ログへ。可視窓を検知したら `ShowWindow(SW_HIDE)`（UE の窓だけ。カーソルは触らない）→ 3 回続いたら**プロセスを Job ごと終了**して「窓が出るモードでした」と報告 | 読み取り API のみ | 客観的な証拠を残す（ユーザーの画面を見られない AI の代わりに、ログで「窓は出なかった」を示す） |

- **クラッシュダイアログ**: `CrashReportClient.exe` が UI を出しうる（UE 標準）。ランチャが同名プロセスを見つけたら即 kill。
- **UE のエディタ GUI 窓を出す経路（`-ExecutePythonScript` のフルエディタ起動）は、L3 が確認できるまで使わない**。

**C. CPU / メモリ / GPU の負荷制御**

- **Job オブジェクト**（Windows 公式機能）: `JOBOBJECT_CPU_RATE_CONTROL_INFORMATION`（`HARD_CAP` 40%）、`JOB_OBJECT_LIMIT_PROCESS_MEMORY`（**RAM 16 GB のうち UE に 10 GB まで**。超えたら PC ごと固まらず UE が落ちる）、`BelowNormal` 優先度、コアのアフィニティ（論理 28 コア中 12 に制限）。
- **シェーダコンパイル**: `ShaderCompileWorker` は子プロセスなので Job で一括制限される。ワーカー数は `[DevOptions.Shaders]` の `WorkerProcessPriority` 等で下げる（Epic Answers 由来・UE4 時代の記述で **5.8 での有効性は UNVERIFIED**）。**初回のコールドスタートは長い（DDC が空）**ので、実行は**ユーザー不在の時間帯**（承認ファイルにその旨を書かせる）。
- **GPU**: OS 側で上限を掛けられない（UNVERIFIED な回避策は書かない）。UE のレンダ時に画面が重くなりうるので、**承認時に「GPU を使う・数分〜数十分」を明示**する。`t.MaxFPS` などで負荷を下げる案は Q0 で試す。

**D. Q0 スパイク（立会い付き・段階的。ユーザーの明示許可がある間だけ）**

1. **UE 5.8 の再インストール**（ユーザー操作。5.8.0 か 5.8.2 かを選んでもらう。以後**ビルド番号を固定**し、`Build.version` を基準画像のメタデータに保存）。
2. 起動せずに `ls` で存在確認（`Engine\Binaries\Win64\UnrealEditor-Cmd.exe` / `UnrealEditor.exe` / `Engine\Plugins\MovieRenderPipeline` / `Interchange` / `PythonScriptPlugin`）。
3. **空プロジェクト**（既存 `AIMCPTEST` は C++ プロジェクトでビルドが要るので使わず、Blueprint のみの新規 `ParityBase.uproject` を Python なしで作る手順をユーザーに依頼するか、AIMCPTEST のマップを読むだけで済ませる）で、**L1 → L2 → L3 の順**に 1 回ずつ起動し、監視ログ（窓・フォアグラウンド）を取る。**1 回の起動 = 1 回の承認**。
4. 画が出ることの確認（空の部屋 + 球 1 個を EXR/PNG で保存）→ 決定論（同じ画を 2 回撮って差を測る）→ 収束フレーム数。
5. 結果を `docs/GRAPHICS_PARITY_DESIGN.md` §3.2 の UNVERIFIED 欄へ反映。**窓が出るモードしか無い場合の撤退条件は §5**。

### 3.4 パリティ・シーンの作り方

**A. 仕様 → 2 つのアダプタ**

```
tools/parity/spec/ps2_outdoor_day.json  ──┬─ build_uno.mjs  ─→ Uno のシーン JSON（既存 scene_write / M11 の apply_scene_spec を使う）
 (エンジン中立・SI 単位・glTF 座標系)      └─ build_ue.py    ─→ UE のレベル（-run=pythonscript か -ExecutePythonScript。承認後にだけ実行）
```

仕様（抜粋）: `assets[]`（`sha256` 付き glTF/FBX/PNG、単位 m）/ `materials[]`（下記の**可搬材質サブセット**）/ `instances[]`（モデル・変換・材質。**乱数配置は仕様側で明示リスト化**して両エンジンに同じ値を渡す）/ `lights[]`（種別・位置・向き・**色 = リニア sRGB**・**強度 = 物理単位**: 太陽 lux、点/スポット cd（または lm + 立体角）、半径 0 の点光源）/ `sky`（大気パラメータ **全項目を明示**、太陽の高度・方位、雲量）/ `water`（吸収・散乱・波振幅）/ `camera[]`（位置・注視点・**垂直 FOV**・アスペクト・解像度 1920×1080）/ `exposure`（**手動 EV100** 固定）/ `post`（全て OFF が既定）/ `freeze`（時間固定・風 0・波 0 の「凍結モード」と、動く「アニメ」版の 2 種）。**仕様のハッシュ**（正規化 JSON + アセット sha256）が基準画像キャッシュのキー。

**B. 可搬材質サブセット（両エンジンで作れる範囲に限定）**: `baseColorTex`（sRGB）/ `normalTex` / `metalRoughTex`（G=roughness, B=metallic。**R（AO）は使わない**: Uno は読まない G18ので、UE 側も配線しない）/ `emissiveTex` + 強度 / `alphaMode`（opaque/mask + cutoff）/ `shadingModel`（default / clearcoat / aniso / cloth / skin / hair / foliage）+ 各パラメータ。UE アダプタは Python（`unreal.MaterialEditingLibrary` によるノード生成、または Interchange の glTF 材質）で組み、**肌 = Subsurface Profile、髪 = Hair カード用設定、布 = Sheen、クリアコート = Clear Coat** へ写像する。

**C. 資産規約（パリティ資産は必ず守る。守らないと差が「機能差」に見えない）**

1. **`baseColorFactor` を焼き込んだテクスチャにする**（Uno は glTF の `baseColorFactor` を読まない、memory `dreamcore-dead-mall`。単色は 1×1 PNG でよい）。factor は常に 1。
2. AO（ORM の R）は使わない。3. 単位は m、glTF の右手 Y-up。Uno は左手 Y-up（`ModelLoader` が変換）、UE は左手 Z-up・cm（Interchange が変換）。**座標変換は校正シーン PS-0 の格子・色付き球で最初に目視 + 画像差で検証**。4. UV は 0..1 内・タイル無し（UV タイリングは仕様側のマテリアルで）。5. 法線マップは OpenGL 規約（glTF 準拠）、BC5 相当。6. 頂点色は使わない（Uno は白が既定）。7. マスク素材はミップ別カバレッジ保存済み。8. メッシュ LOD は仕様に明示（自動 LOD に頼らない。UE 側は Nanite OFF・LOD 固定）。9. スケーラビリティ: UE は Epic（`sg.*=3`）、Uno は全機能を「最高品質」設定。10. テクスチャは両者で同じ解像度・同じ圧縮（BC7/BC5）。

**D. UE 側のシーン構築（Python）**: `unreal.InterchangeManager` の `create_source_data` + `import_asset`（`is_automated` でダイアログ抑止）で glTF を取り込み、`EditorActorSubsystem` / `EditorLevelLibrary` でアクタ生成（DirectionalLight / SkyAtmosphere / SkyLight / VolumetricCloud / ExponentialHeightFog / PostProcessVolume / CineCameraActor）、レベル保存。**これらの Python API の commandlet/オフスクリーンでの動作は UNVERIFIED**（公式で確認できたのは Interchange の Python クラスの存在まで。glTF の完全な例は未確認）。撮影は §3.3 の経路（MRQ 優先、次点 = Python の tick で待ってから `HighResShot`、第 3 = `SceneCaptureComponent2D` の `FinalColorHDR` を RGBA16F レンダターゲットへ撮って `export_render_target`。第 3 は履歴を持たない 1 フレーム描画なので Lumen 収束の tick 待ちが要る）。3 経路はいずれも Q0 で 1 つに絞る。

**E. Dead_Mall の扱い**: ①**原作のスクリーンショット（Steam 版 UE5.6）は「目視の参考」**（カメラ位置が厳密に合わせられず、マテリアルグラフが cook で落ちている `dreamcore-dead-mall` ので数値比較はしない）②**パリティ用は Dead_Mall の「スライス」**（コンコースの一区画・配置 約 400・ライト 約 80・エミッシブ看板）を、移植済みの `.glb`/`.png`/ライト JSON から**仕様 JSON へ変換**して両エンジンに渡す（791 変種の全取り込みは UE 側の Interchange が重いので、スライスで必要な約 60 メッシュに限る）。UE 側のライトは逆二乗・半径 0 に**明示設定**（Uno の減衰 `saturate(1-d/range)^2` との違いは Q2 の「光の単位の校正」で吸収）。

### 3.5 揃える条件（ここを揃えないと比較が無意味）

| 項目 | UE 側 | Uno 側 | 揃え方 |
|---|---|---|---|
| **露出** | PostProcessVolume Unbound、AutoExposure = Manual、EV100 固定（`Exposure = 1/(2^(EV100+Comp))` [出典]） | 自動露出 OFF、`exposure` 固定（uber パスで適用） | **グレーカード校正**（PS-0）: 18% グレーの拡散板を既知の照度で置き、両エンジンの画素値比から定数 K を導出して Uno の単位変換に固定。UE の内部係数（1.2 など）の有無を**推測せず実測で決める** |
| **光の単位** | 太陽 lux、点/スポット cd、**逆二乗 + 減衰半径**、半径 0 | 強度は任意単位（太陽 3.0）、減衰 `saturate(1-d/range)^2`（G3） | **Q2**: Uno に「物理単位モード」（逆二乗 + 半径による窓関数、lux/cd 入力）を**既定 OFF**で追加。仕様は物理単位で書き、アダプタが変換 |
| **トーンマップ** | 既定 Filmic（ACES 準拠、係数は上表） | Narkowicz ACES / AgX / なし（G13） | Tier A は**トーンマップ前の線形**同士（UE は EXR / `bCaptureHDR`、Uno は**線形 float スクショ**を新設 Q2）。Tier B 用に Uno へ **「UE Filmic」トーンマップ（tonemapper=3）**を追加。**UE のシェーダ（`Engine/Shaders`）の式は公開文書の係数から独自実装し、コードをコピーしない**（EULA 上の注意。法的判断はしていない） |
| **ポスト** | Bloom 0 / Vignette 0 / Grain 0 / Lens Flare OFF / Motion Blur 0 / DoF OFF / AA = TSR・`r.ScreenPercentage=100` | 全 OFF（deband・FXAA も OFF）、TAA 決定論 | 仕様の `post` を両アダプタが同時に適用。UE の TSR は非決定論 [出典] → 温め運転 + EXR 平均（複数枚の平均で床を測る） |
| **カメラ** | `CameraComponent.field_of_view`（**水平 FOV が既定の制約かは UNVERIFIED**）、シネカメラは焦点距離 + センサー | 垂直 FOV（エディタ既定 45°） | 仕様は**垂直 FOV + アスペクト**。アダプタが換算 `tan(hfov/2) = aspect·tan(vfov/2)`。**PS-0 の格子・チェッカーで画像差を出して検証** |
| **解像度** | `-ResX -ResY -ForceRes`、出力 1920×1080 | ヘッドレスの表示矩形は 1043×587 固定（G14） | **Q2**: `--headless` に解像度指定（`--size WxH`）を追加（現状 CLI に無い）。1920×1080 で撮る |
| **時間・乱数** | `-Deterministic`（固定タイムステップ + 固定シード）、Lumen/TSR の温め運転 N フレーム（Q0 で決定） | `deterministic:true, settleFrames`（G14） | 「凍結モード」= 時間固定・風 0・波 0。アニメ版は統計比較 |
| **ストリーミング** | `r.Streaming.FullyLoadUsedTextures=1`、Nanite ストリーミングは待つ | 全テクスチャ常駐 | 撮影前に**十分な待機 + 常駐確認**（UE 側の確認方法は Q0） |
| **GI / 影の設定** | Lumen GI + Lumen Reflections（PS-5 は HWRT）、VSM 既定、MegaLights **OFF**（Uno に対応物なし） | DDGI + SSGI + SSR + RT 影 | **UE の設定を仕様に記録**（`ueSettingsHash`）。機能が違う項目は「比較対象外」として仕様に明記 |
| **材質差** | Substrate（既定 ON）だが可搬サブセットのみ使う | 固定モデル ID | §3.4-B |

### 3.6 比較指標

| 指標 | 何を見る | 実装 / 導入コスト / ライセンス |
|---|---|---|
| **HDR-FLIP**（線形 EXR/float） | 露出を揃えた**シーン参照の知覚差**（Tier A の主指標） | NVIDIA FLIP（**BSD-3-Clause**、v1.7、`pip install flip-evaluator`、CPython 3.8〜3.13 の Windows wheel、CPU で動く、LDR-FLIP と HDR-FLIP の両方・PNG/EXR 入力）[出典 https://github.com/NVlabs/flip 、https://pypi.org/project/flip-evaluator/ ] |
| **LDR-FLIP**（トーンマップ後 PNG） | 表示参照の知覚差（Tier B の主指標） | 同上 |
| **SSIM**（輝度、11×11 ガウス窓） | 構造の一致（形・エッジ・ボケ） | scikit-image（BSD-3、`structural_similarity`）[出典 https://scikit-image.org/docs/stable/api/skimage.metrics.html ]。numpy + opencv は導入済みなので自前 30 行でも可 |
| **PSNR** | 参考（ゲートにしない） | 同上 |
| **ΔE2000（CIELAB）** | 色差（色被り・白バランス） | scikit-image（`deltaE_ciede2000`、`rgb2lab`）[出典 https://scikit-image.org/docs/stable/api/skimage.color.html ] |
| **輝度ヒストグラム EMD / 平均・中央値輝度（EV 差）/ 黒潰れ・白飛び率 / CCT / 彩度** | 全体の露出・色調・コントラスト | **既存 `dx12_look_compare`（`lookCompare.ts:1-60`）をそのまま利用**（UE の PNG を参照画像として渡せる＝Q1 前でも手動で使える） |
| **局所ヒートマップ**（16×16 タイルの FLIP 平均、悪い上位 10 タイル + 座標） | どこが違うか | FLIP 出力を集計する自前 20 行。レポートに重ねる |
| **LPIPS** | 参考のみ（**ゲートにしない**） | コードは BSD-2、PyTorch/torchvision は導入済み、重み（AlexNet/VGG）は torchvision 経由で取得。**重みの利用条件は UNVERIFIED** [出典 https://github.com/richzhang/PerceptualSimilarity ] |
| **性能: GPU ms / VRAM / ロード時間 / CPU ms** | 効率比較 | **Uno 側は既存 `perf_stats`（`gpuPassMs` / `cpuScopeMs`）**。UE 側は `stat unit` / CSV Profiler / `-benchmark` 系で取れる可能性があるが**取得方法は未検証**なので**参考値**扱い（ゲートにしない） |

**依存の導入**: `tools/parity/.venv`（専用 venv）に `flip-evaluator` `scikit-image` `imageio`（EXR は OpenCV の `OPENCV_IO_ENABLE_OPENEXR=1` でも可）を入れる。**グローバル Python を汚さない**。CPU のみ・GPU 不要。

### 3.7 合格しきい値の考え方（「UE と同じ」を数値にする）

**原則**: ①しきい値は**段階ゲート**（G0 健全 / G1 近い / G2 同等）②各しきい値は **max(固定値, ノイズ床 × 1.5)** ③**初期値は [推定]。Q1 の実測（ノイズ床と初回ギャップ）で固定**（VG 書 §5.4 の「実測して固定」と同じ流儀）④しきい値を緩めて通すのは禁止（緩める変更は理由必須で履歴に残す、M9 の `approve` と同じ）。

| ゲート | 意味 | 初期値 [推定] |
|---|---|---|
| **G0** 健全 | 全黒/全白/NaN が無い、平均輝度が ±1 EV | 機械判定のみ |
| **G1** 近い | 「同じシーンだと分かる・雰囲気が同等」 | LDR-FLIP 平均 ≤ 0.15、輝度 ±0.5 EV、SSIM ≥ 0.75、ΔE2000 中央値 ≤ 6 |
| **G2** 同等 | 「並べて差を指摘しにくい」 | LDR-FLIP 平均 ≤ 0.08 かつ p95 ≤ 0.30、輝度 ±0.25 EV、SSIM ≥ 0.90、ΔE2000 中央値 ≤ 3。**Tier A の HDR-FLIP 平均 ≤ 0.10** |

**画素比較しない項目（統計 + 目視）**: 雲（32 px ぼかし後の FLIP・被覆率・平均輝度/色度・太陽グロウの放射プロファイル）/ 動く水（波振幅 0 の凍結版は画素比較、動く版は色分布・泡の被覆率・水平線の反射色）/ 風で動く植生（風 0 の凍結版のみ画素比較）/ 乱数配置（仕様の明示リストにして解消）/ 肌・髪（緩いゲート G1 まで + 目視）。

**重要な断り**: 指標は**必要条件**であって十分条件ではない（GI・肌・雲の「見た目の良さ」は指標で保証できない）。各節目で**コンタクトシート（UE | Uno | ヒートマップ）をユーザーが目視で承認**する運用を併用し、AI が単独で「合格」を宣言しない（MCP 書 §4.5.3 の `approve` と同じ方針）。

### 3.8 パリティ・シーン集（5 本 + 校正 1 本）

| ID | シーン | 内容 | 主に検証する機能 | 画素比較 / 統計 | 合否対象の段階 |
|---|---|---|---|---|---|
| **PS-0** | 校正 | グレーカード + 24 色パッチ + チェッカー床 + ロー/メタル球格子（roughness × metallic 5×5）、太陽 + 一様空 | 光の単位・露出・トーンマップ・カメラ・座標・BRDF | 画素 | **Q2 完了時に G2**（以降の全比較の土台） |
| **PS-1** | 屋内（Dead_Mall スライス） | 暗い廊下 + 動的点光源 約 80 + エミッシブ看板 + 濡れ床（SSR/RT 反射） | 影・GI（DDGI/SSGI）・エミッシブ・光の単位・VG（岩/柱の高ポリ）・RT 反射 | 画素 | Q2 後 **G1**、VG P4 と RT1 で **G1 維持**、P8 + RT3 で **G2 を狙う** |
| **PS-2** | 屋外昼 | 地形 200 m + 大気（朝 10° / 昼 60° の 2 時刻）+ 雲 + 湖（平水面）+ 樹木 200 + 草 30 万ブレード + 遠景の山 | 大気・AP・雲・水・植生・風・時刻・影距離 | 空/地面/凍結水は画素、雲・動く水・風は統計 | A1 で**空のみ G2**、A1+F1+W1 で **G1**、A2/W2/F2 で G1 維持 |
| **PS-3** | キャラ | 1 体（肌・髪カード・布）をスタジオ（HDRI + キー/フィル）で | クリアコート/異方性/布/肌/髪、SSS | 画素（G1 まで）+ 目視 | SH1〜SH3 で **G1**。**調達**: Blender MCP で頭部 + 髪カード + 布を自作（生成 3D は肌/髪の品質が出ないため）または CC0 の人体。Khronos の glTF サンプル資産（ClearCoat / Sheen / Anisotropy の各サンプル）を**拡張機能の単体テスト**に使える（名前は候補。取得と許諾は着手時に確認） |
| **PS-4** | 重量（5000 万 tri） | VG 書 §5.2 の `vg_bench_50m`（12.5M tri × 4 種）。UE 側は **同一ソースメッシュ 1 種を Nanite で取り込み 5 インスタンス**（16 GB RAM で 4 × 12.5M tri の Nanite ビルドは危険。**RAM 上限の Job 制限に当たる想定**） | VG の見た目の一致 + 性能 | 画素（G1）+ **Uno の性能ゲート（VG 書 §5.2）**。UE 性能は参考値 | VG P4 で G1 |
| **PS-5** | ガラス・反射 | 鏡床・クロム球・水面・ガラス窓・光沢床。UE は Lumen HW RT 反射 | RT 反射・SSR・水の反射・ガラス（IBL+SSR まで） | 画素 | RT1 で **G1**（**ガラス面自体の反射は v1 で対象外**と明記） |

### 3.9 自動化（MCP / CLI）— MCP 書 M9・M10 への相乗り

**ディレクトリ（新規）**: `tools/parity/`（`spec/*.json` / `build_uno.mjs` / `build_ue.py` / `run_uno.mjs` / `run_ue.ps1`（承認ゲート付き・人間専用）/ `approve_ue_run.bat`（人間専用）/ `metrics.py` / `report.mjs` / `gate.json` / `README.md`）。**基準画像とレポートは `.dx12/parity/`（git 管理外）**: `refs/<sceneId>/<specHash>__<ueBuild>__<ueSettingsHash>/{final.png, linear.exr, meta.json}`、`runs/<runId>/{uno.png, uno.pfm, diff_flip.png, heat.png, metrics.json}`、`history.jsonl`、`report.html`。

**1 コマンドの流れ（UE を起動しない日常の反復）**

```
node tools/parity/run_uno.mjs --scene ps2 --stage A1
  1. build_uno.mjs   : spec → Uno のシーン JSON（既存 apply_scene_spec / scene_write）
  2. Uno をヘッドレス起動（--headless --project --scene --mcp-port、--size 1920x1080。tools/bench/lib/engine.mjs を再利用）
  3. 決定論撮影: screenshot_final（Tier B）+ screenshot_linear（Tier A、Q2 で新設）
  4. 基準画像の解決: refs/ps2/<hash>... が無ければ 「基準なし: UE 承認待ち」で skipped（合格扱いにしない）
  5. metrics.py      : HDR/LDR-FLIP・SSIM・ΔE2000・ヒストグラム・タイルヒートマップ
  6. gate.json 判定  : ステージ別の G0/G1/G2 とノイズ床。結果 {pass, gate, metrics, worstTiles}
  7. perf: dx12_perf_gate（M10）で GPU ms / VRAM / ロード時間 を budgets と比較
  8. report.html     : シーン × ステージの表、UE | Uno | ヒートマップの並べ、スライダ比較、履歴グラフ
```

**MCP 表面（M9/M10 に統合）**: `dx12_visual` の baseline に **`kind:"ue_reference"`** を追加（capture=Uno のみ。UE 基準は外部で生成）、`op:"compare"` の指標に FLIP/SSIM/ΔE を追加（`metrics:["flip","ssim","de2000","hist"]`）。新規ツールは **`dx12_parity {op:"scenes"|"build"|"capture"|"compare"|"report"|"ref_status"}`** のみ。**`ref_make`（UE 起動）は MCP に置かない**。`quality_gate` に `parity` チェック（基準なしは `skipped`、ステージのゲート未達は blocking + 悪い上位タイル）。`dx12_perf_gate` の `budgets.json` にシーン別予算（§2.0.4 の表を初期値に）。`ciClient` の suite に `parity` を追加（**GPU または基準が無ければ `skipped`**、MCP 書 M10 の「GPU 無し環境では visual/perf を skipped」と同じ）。**CI はユーザー PC のローカル実行前提**（GitHub Actions の `windows-latest` は GPU なし、`ci.yml:27-46`）。

**GPU の排他**: perf とパリティ撮影は GPU を専有するので、`build.ps1` の `Global\dx12-build` と同じ流儀で **`Global\dx12-gpu-bench`** の名前付き Mutex を新設（Q1）。並列レーンの計測が互いの ms を汚さない。

**レポートの見せ方**: `.dx12/parity/report.html`（ローカル HTML。必要なら Artifact として公開）。各節目でユーザーに見せる 1 枚 = 「UE | Uno | FLIP ヒートマップ」の 3 連 + ステージ別スコアの推移 + 性能表。

### 3.10 ユーザー操作量（UE 基準画像を作るときだけ）

1. 初回のみ: UE 5.8 の再インストール（Launcher で 5.8.0/5.8.2 のどちらか）。
2. UE を走らせたい日: **ユーザーが不在にする時間を決め**、`approve_ue_run.bat` をダブルクリック（60 分有効）。AI は承認後に `run_ue.ps1` を実行し、監視ログ（窓・フォアグラウンドの有無）を報告する。
3. 基準画像はキャッシュされるので、**仕様を変えない限り 2 回目以降は不要**。目安: 屋内 1 + 屋外 2 時刻 + キャラ + 重量 + 反射 + 校正 ≒ 8〜10 枚 × 1 回。

---

## 4. 統合ロードマップ（VG 書 P0〜P10 + 本書）

工数は「1 人（または 1 エージェント）が集中した実働日」。**全段階共通のマージ条件は §2.0.1**。「見て嬉しい」は画面で変化が分かる度合い（★ 1〜5）。**ハーネス（Q）のゲートは各段階の「合否」欄の最後にある `PS-x Gy` を指す**。

### 4.1 段階の一覧（1 行 / 段階）

| ID | 段階 | 依存 | 工数 | ★ | 合否（機械判定の要点） | 主なファイル領域 | レーン |
|---|---|---|---:|:-:|---|---|:-:|
| **Q0** | UE 5.8 再導入確認 + 安全起動スパイク（立会い） | ユーザーの再インストール | 1 | — | 窓/フォアグラウンドの監視ログで L1〜L3 の採否確定、UNVERIFIED 欄の更新 | なし（UE 側のみ） | H |
| **X0** | 調整の先行: t 番号表 / PerFrame 予約割当 / 各レーンの空フック（IRenderPass 差込点） | なし | 1 | — | 既定 OFF で golden 4 枚ビット一致、ctest 緑 | `ApplicationRender.cpp`（空パス）、`Lighting.hlsli`（予約名のみ）、本書 §2.0 | H |
| **Q1** | パリティ基盤（仕様スキーマ・Uno/UE アダプタ・承認ゲート付き UE ランナー・metrics・report・`Global\dx12-gpu-bench`） | X0 | 8 | ★ | PS-0 仕様が Uno でビルドされ UE アダプタが Python を生成（実行せず構文検査）/ `run_ue.ps1` が承認なしで exit 2 / 窓を出すダミー exe を L4 が検知して hide→kill / FLIP が公式サンプルと一致 / ノイズ床測定機構 | `tools/parity/*`、`tools/bench/lib` | H |
| **Q2** | 校正: 物理単位モード（逆二乗・lux/cd）+ UE Filmic + 線形 float スクショ + `--size` + グレーカード | Q1（UE 基準は Q0 後） | 5 | ★★ | 既定 OFF ビット一致 / 単位変換テスト / **PS-0 G2**（UE 基準生成後） | `Lighting.hlsli`（減衰モード）、`PostProcess.hlsl`（tonemapper=3）、`ApplicationMcpEditor.cpp`、`main.cpp` | H |
| **Q3** | MCP 統合（`dx12_parity`・`dx12_visual` の UE 基準・`quality_gate` parity・`ciClient` suite） | Q1、**MCP 書 M9・M10** | 3 | ★ | 基準なし = `skipped` の明示 / 意図的に色を変えると fail + 悪タイル座標 | `tools/mcp-server/*` | H |
| **Q4** | パリティ資産: PS-1〜PS-5 の仕様・素材（Dead_Mall スライス変換、キャラ自作、屋外、反射） | Q1 | 7 | ★ | 全資産が資産規約 §3.4-C を満たす（自動検査: factor≠1・AO 配線・単位） | `tools/parity/spec`、資産置き場 | 空きレーン |
| **A1** | 大気・空: 4 LUT + 空パス + AP 合成 + 時刻系 + IBL 再ベイク | X0 | 9 | ★★★★★ | 既定 OFF ビット一致 / CPU 透過率 = GPU LUT（相対 1e-3）/ ≤ 0.6 ms / 24h コンタクトシート / **PS-2 空のみ G2** | `shaders/sky/*`、`src/renderer/Atmosphere*`、`ApplicationScene.cpp`、`ApplicationRender.cpp`（空・AP） | A |
| **A2** | ボリュメトリッククラウド | A1 | 10 | ★★★★ | ≤ 1.2 ms / 決定論 bitExact / 統計ゲート（被覆率・輝度） | `shaders/sky/Cloud*`、`src/renderer/Cloud*` | A |
| **A3** | 雲影（t28）・月・天気プリセット・Lua/MCP/文書 | A2 | 5 | ★★★ | 雲影で地面輝度が変わる（機械測定）/ API 3 点セット | `Lighting.hlsli`（t28 レンジ）、`ScriptEngine.cpp`、docs | A |
| **W1** | 水面（半透明分離・クリップマップ・Gerstner・屈折・吸収・反射） | A1（空反射）、X0 | 9 | ★★★★★ | ≤ 1.5 ms / **PS-2 平水面 G1** / デバッグレイヤ 0 | `src/renderer/Water*`、`shaders/water/*`、`ApplicationRender.cpp`（`:1983` 周辺） | A |
| **W2** | 泡・岸・カウスティクス・水中ビュー | W1 | 5 | ★★★ | 岸の泡被覆率が統計内 / 水中で Beer 減衰が機械測定と一致 | 同上 | A |
| **W3** | FFT 海洋（**条件付き**） | W2 + パリティで不足 | 6 | ★★ | 3 カスケード ≤ 0.6 ms / 統計 | 同上 | A |
| **F1** | `FoliageLayer` + GPU カリング + ExecuteIndirect + 風 + マスク影 + スプラット由来スキャッタ | X0、（地形は既存） | 12 | ★★★★★ | 可視 20 万・総 100 万で描画 ≤ 2.5 ms / **風 0 で個別エンティティ配置と画素一致** / `buildList` の CPU 不変 / **PS-2 植生 G1** | `src/renderer/Foliage*`、`shaders/foliage/*`、`Lighting.hlsli`（風の予約）、`terrain` | F |
| **F2** | 草（GPU 生成ブレード） | F1 | 6 | ★★★★ | 50 万ブレード ≤ 1 ms / 決定論 / 統計 | `shaders/foliage/Grass*` | F |
| **F3** | オクタヘドラル・インポスター + ベイクツール | F1 | 7 | ★★★ | 遠景 LOD 切替で PSNR ≥ 35 dB（切替前後）/ 8 MB/種 | `shaders/foliage/Impostor*`、MCP `dx12_bake_impostor` | F |
| **SH1** | シェーディングモデル基盤（ID・`t27`・クリアコート・異方性・布・多重散乱補償・glTF 拡張・`MATERIAL_OUTPUTS.md`） | X0、Q2（`Lighting.hlsli` の先行変更後） | 8 | ★★★★ | **ID=0 が DXIL 同値** / Khronos の各拡張サンプルで UE 比 G1 / VG resolve が同関数を呼ぶ | `PBR.hlsli`、`Lighting.hlsli`、`Material.h`、`ModelLoader.cpp`、`Forward*.hlsl` | F |
| **SH2** | 髪（Kajiya-Kay 2 ローブ） | SH1 | 5 | ★★★ | PS-3 髪領域 G1 + 目視 | `PBR.hlsli` | F |
| **SH3** | 肌（プレインテグレート + 影厚み透過） | SH1 | 6 | ★★★★ | PS-3 肌領域 G1 + 目視 | 同上 | F |
| **SH3b** | 分離型 SSS（**条件付き**、α 再利用のスパイク後） | SH3 + パリティで不足 | 5 | ★★ | 肌領域 FLIP 改善 ≥ 20% / ≤ 0.5 ms | 新規ポスト | F |
| **UP1** | FSR 3.1 直接統合（表示解像度ポスト分離・ジッタ位相・深度反転・露出・自前反応マスク・MIP バイアス） | X0、（水の色コピーがあると反応マスクが楽） | 10 | ★★★★（性能） | 描画 0.67 + FSR が PS-2 で TAA バイリニア比 **HDR-FLIP 改善**、GPU ≤ 1.3 ms、静止 8 フレーム差 ≤ 0.3/255、ゴースト 0（動く箱テスト） | `TaaPass.*`、`ApplicationPipeline.cpp`（`ApplyRenderResolution`）、`ApplicationRender.cpp`（ポスト）、`FsrUpscaler.*` | H |
| **UP2** | DLSS（**条件付き・保留**、法務確認後） | UP1 + 未決 §6-3 | 6 | ★ | NVIDIA 事前通知・OSS 条項の確認済み | `DlssUpscaler.*` | — |
| **RT1** | RT 反射（t16 へ合成・ハイブリッド・ヒット shading 共通化・デノイズ） | A1（空ミス）、既存 DXR | 12 | ★★★★ | 既定 OFF ビット一致 / 1〜2.5 ms / **PS-5 G1**、**PS-1 の濡れ床 G1 維持** | `shaders/raytracing/*`、`RtScreenPass.*`、`DdgiProbeUpdate.hlsl`（切り出しのみ） | H |
| **RT2** | RT any-hit（MASK を影・AO・反射・DDGI に） | RT1 | 4 | ★★★ | 葉の RT 影が板でなくなる（切り抜き形状の一致）/ 近景樹木 ≤ 2000 インスタンス | `RaytracingScene.*`、`RtBindless.hlsli` | H |
| **RT3** | DDGI ヒット強化（emissive・tint・空ミス） | **P8-1（カスケード）の後** | 4 | ★★★ | Dead_Mall の emissive 看板が GI に寄与（機械測定）/ **PS-1 G2 を狙う** | `DdgiProbeUpdate.hlsl` | H |
| **P0** | VG: 仕様確定 + スタブ | なし | 3 | — | VG 書 §4.2 のとおり | `src/vgeo/*` | V |
| **P1** | VG: オフライン cooker | P0 | 10 | — | 同上 | `tools/vgeo-cook` | V（並列可） |
| **P2** | VG: ランタイム + GPU カリング | P0 | 10 | — | 同上 | `src/renderer/vg/*`、`shaders/vg/*` | V |
| **P3** | VG: メッシュシェーダ + 可視性バッファ | P2 | 9 | ★★ | 同上（実測ゲート R1） | 同上 | V |
| **P3b** | VG: SW ラスタ（条件付き。VG 書の 144 日に含まれる） | P3 + ゲート | 7 | ★ | 同上 | 同上 | V |
| **P4** | VG: resolve + 既存統合（**5000 万 tri / 60 fps**） | P3、（SH1 の関数を resolve が呼ぶ） | 15 | ★★★★（重量シーン） | 同上 + **PS-4 G1** | `ApplicationRender.cpp`（H1〜H3）、`RootSignature.*`、`DrawItem.h` | V |
| **P5** | VG: ストリーミング | P4 | 12 | ★ | 同上 | `src/renderer/vg/*` | V |
| **P6** | VG: UE アセット cook（C#）（Q4 の素材供給にも使う） | P0 | 10 | ★ | 同上 | `tools/ue-cook` | 空き |
| **P7** | VG: 仮想シャドウマップ | P4（P5 推奨） | 30 | ★★★ | 同上（`t25`,`t26` は VG 書） | 影パス全般 | V（4 分割可） |
| **P8** | VG: GI（DDGI 延長: カスケード・リロケーション） | P4 | 18 | ★★★★ | 同上 + **PS-1 G1→G2 方向** | `DdgiVolume.*`、`DdgiProbeUpdate.hlsl` | V/H |
| **P9'** | VG: DoF / Bloom（**TSR は UP1 へ置換**） | なし | 6 | ★★ | VG 書 P9 の DoF/Bloom 部分のみ | `DofPass.cpp`、`BloomPass.cpp` | 空き |
| **P10** | VG: エディタ / MCP / 配布統合 | P4（P5 後） | 6 | ★ | 同上 | 配布・UI | 空き |

**合計**: 本書の新規 **137 日**（Q 25 / A 24 / W 14 / F 25 / SH 19 / RT 20 / UP 10）+ VG 書 144 日 − P9 の TSR 8 日 = **273 実働日**。条件付きの追加 +17 日（W3 6 / SH3b 5 / UP2 6）。VG の 144 日は P3b（条件付き 7 日）を含む。MCP 書の M9・M10（13 日）は別文書の作業（Q3 が依存）。

### 4.2 依存図

```
 Q0 (ユーザー) ──────────────────────────────────────────────┐(UE基準画像が出る)
 X0 ─┬─→ Q1 ─┬─→ Q2 ─→ Q3(MCP書 M9,M10 待ち)                 ▼
     │       └─→ Q4 ───────────────────────────────→ [各段階の PS-x ゲート判定]
     ├─→ A1 ─┬─→ A2 ─→ A3
     │       ├─→ W1 ─→ W2 ─→ (W3)
     │       └─→ RT1 ─→ RT2
     ├─→ F1 ─┬─→ F2
     │       └─→ F3
     ├─→ SH1 ─┬─→ SH2
     │        └─→ SH3 ─→ (SH3b)
     ├─→ UP1 ─→ (UP2)
     └─ VG: P0 ─┬─→ P1
                ├─→ P2 ─→ P3 ─→ (P3b) ─→ P4 ─┬─→ P5 ─→ P7
                └─→ P6                        ├─→ P8 ─→ RT3
                P9' ─(独立)                    └─→ P10
```

### 4.3 クリティカルパスと「計測のクリティカルパス」

- **実装のクリティカルパス**: VG の P0(3) → P2(10) → P3(9) → P4(15) = **37 日**（最初の合格 = 5000 万 tri / 60 fps）→ P5(12) → P7(30) = **79 日**。**本書の機能は VG のクリティカルパスを延ばさない**（別レーン、ファイル領域が主に別）。
- **計測のクリティカルパス**: **Q0(ユーザー立会い 1 日) → Q1(8) → Q2(5) = 約 14 日**。ここが終わるまで**どの段階も UE 比のゲート判定ができない**（判定は `skipped`）。これが最初の 2 週間で最優先の並列作業。
- **ユーザーの待ち時間が入る箇所**: UE 再インストール（Q0 の前提）、UE を走らせる不在時間（基準画像 1 回）、節目ごとの目視承認。

### 4.4 並列に走らせられる組 / 直列にすべき組

**並列可（ファイル領域が重ならない。レーン割当は §4.5）**

| 組 | 理由 |
|---|---|
| Q1 ∥ A1 ∥ F1 ∥ P0→P2 | ツール類 / `shaders/sky` / `shaders/foliage` / `src/vgeo`・`vg` で領域が別 |
| A2 ∥ F2 ∥ SH1 ∥ P3 | 雲 / 草 / `PBR.hlsli` / VG ラスタ |
| W1 ∥ UP1 ∥ P4 の前半 | 水 / ポスト解像度 / VG 統合。**ただし `ApplicationRender.cpp` の別領域を 3 者が触る**（下記の衝突対策） |
| Q4（資産作り）は空きレーンでいつでも | ゲームロジックに触らない |

**直列にすべき組（同一ファイル・同一契約）**

| 組 | 理由 | 対策 |
|---|---|---|
| **Q2 → SH1** | どちらも `Lighting.hlsli` / `PBR.hlsli`（Q2 = 光の減衰モード、SH1 = シェーディングモデル） | Q2 の `Lighting.hlsli` 変更（1 日）を先に入れ、SH1 が後 |
| **P8-1 → RT3**、**RT1 の切り出し ⇄ P8** | どちらも `DdgiProbeUpdate.hlsl` | RT1 の「ヒット shading の `.hlsli` 切り出し」は挙動不変で先に入れ、P8 はその後の版に対して作業 |
| **W1（半透明分離）⇄ P4（H3 差込）⇄ UP1（ポスト先頭）** | `ApplicationRender.cpp` の隣接領域 | **X0 で空の IRenderPass 差込点を先に全部入れる**（各レーンは自分の差込点の中身だけ書く）。3 者のマージは 1 日ずつ順番に |
| **PerFrame 予約 `_clusterReserved[0..7]` の割当** | A3（雲影）・F1（風）・SH1（拡張個数）が同じ CBV | X0 で割当と C++/HLSL の構造体を先に確定（§2.0.2-C） |
| **`t` 番号（t27〜t29）** | RS 全体で一意 | 本書 §2.0.2 の表を唯一の正とする。VG 書 P7 の `t25`,`t26` と併記 |
| **ビルドと GPU 計測** | `Global\dx12-build`（既存）と `Global\dx12-gpu-bench`（新設） | 実効レーンは 3〜3.5（+25% の暦） |

### 4.5 4 レーン割当と最初の 30 実働日

**レーン**（各レーンの担当と主な領域）

| レーン | 担当（順） | 主な領域 |
|:-:|---|---|
| **V**（VG） | P0 → P2 → P3 → P4 → P5 → P7 → P8 → P10（P1・P6 は空きが出たら） | `src/vgeo` `src/renderer/vg` `shaders/vg` `ApplicationRender.cpp`(H1–H3) |
| **H**（計測・アップスケーラ・RT） | Q0(立会い) ∥ X0 → Q1 → Q2 → Q3 → UP1 → RT1 → RT2 → RT3 | `tools/parity` `tools/mcp-server` `PostProcess` `TaaPass` `shaders/raytracing` |
| **A**（空・水） | A1 → A2 → A3 → W1 → W2 | `shaders/sky` `shaders/water` `ApplicationScene.cpp` |
| **F**（植生・材質） | F1 → F2 → SH1 → SH2 → SH3 → F3 | `src/renderer/Foliage*` `PBR.hlsli` `Material.h` |
| 空き | Q4（資産）、P6、P9'、P10 | — |

**最初の 30 実働日（4 レーン）**

| 日 | V | H | A | F |
|---|---|---|---|---|
| 1 | P0 | Q0（ユーザー立会い）∥ X0 | A1 | F1 |
| 2〜3 | P0 | Q1 | A1 | F1 |
| 4〜9 | P2 | Q1（〜9） | A1（〜9） | F1 |
| 10〜12 | P2 | Q2 | A2 | F1（〜12） |
| 13〜14 | P2（〜13）→ P3（14〜） | Q2（〜14） | A2 | F2 |
| 15〜18 | P3 | Q3（〜17）→ UP1 | A2 | F2（〜18） |
| 19〜22 | P3（〜22） | UP1 | A2（〜19）→ A3 | SH1 |
| 23〜27 | P4 | UP1（〜27） | A3（〜24）→ W1 | SH1（〜26）→ SH2 |
| 28〜30 | P4 | RT1（着手） | W1 | SH2（〜31） |

**30 日目の到達状態**

- 到達: **パリティ・ハーネス稼働**（14 日目から）: PS-0 / PS-1 の**初回ベースライン数値**（現状 Uno と UE のギャップ B0 とノイズ床）が出る。基準画像が未生成なら `skipped` を明示。
- 到達: **空が時刻で変わる**（9 日目 = A1）、**雲が浮く**（19 日目 = A2）。PS-2 の空のみで G2 判定。
- 到達: **植生 + 風 + 草**（F1 12 日目・F2 18 日目）: 100 万インスタンス・風で揺れる草木。
- 到達: **FSR 3.1**（UP1 27 日目）: 描画 0.67 で 60 fps 側へ。
- 到達: **特殊シェーディング**（SH1 26 日目: クリアコート/異方性/布。髪は 31 日目）。
- 途中: **VG は P4 の半ば**（P4 合格 = 37 日目）。**水は 30 日目時点で W1 の途中**（33 日目に完了）。
- 未着手: 肌（SH3）・RT 反射（RT1 は 40 日前後に完了）・インポスター・VSM。

**節目のマイルストーン（ユーザーが見られるスクショ / 比較）**

| M | 日 | 見せるもの | 数値 |
|---|---|---|---|
| **M1** | 9 | 空の 24h コンタクトシート（0/6/7.5/12/17.5/18/20 時）+ AP の効いた遠景 | PS-2 空のみ **G2**（UE 基準がある場合） |
| **M2** | 14 | **初の並べ比較レポート**（PS-0 / PS-1: UE ｜ Uno ｜ ヒートマップ）。「今どれだけ違うか」の基準線 | ギャップ B0・ノイズ床 |
| **M3** | 19 | 雲 + 風で揺れる草木の 10 秒動画（コンタクトシート） | 雲の統計ゲート、F1 の性能表 |
| **M4** | 27 | FSR: ネイティブ vs 0.67+FSR の並べ + 性能 | HDR-FLIP、GPU ms |
| **M5** | 33 | 水（PS-2 の湖）: 屈折・吸収・空の反射 | 平水面 G1 |
| **M6** | 37 | **5000 万 tri / 60 fps**（VG P4）+ PS-4 | VG 書 §5.2 の合否表 + G1 |
| **M7** | 約 45 | RT 反射（PS-5 / PS-1 の濡れ床）、肌・髪（PS-3） | G1 |
| **M8** | 約 75〜80 | VSM + GI 強化後の PS-1 / PS-2 | G2 を狙う |

**暦の見積もり**: 4 レーン完全並列なら 273 ÷ 4 ≒ 68 日 ≒ **約 14 週**、VG レーンの P0〜P5・P7 が 79 日あるので律速は VG（約 16 週）。P7 は 4 分割できる（VG 書）ので他レーンの空きを投入すれば短縮可。ビルド/GPU 直列化を織り込んだ**実効 3〜3.5 レーン → 約 19 週**。

### 4.6 「見て嬉しい順」の根拠（上位 10 段階）

現状の画で UE と最も違って見えるのは「**平坦な空（グラデーション）**」「**何も生えていない地面**」「**フォグと AP の無い遠景**」「**水が無い / 屈折しない**」「**動かない世界**」。そこで順番は ①A1（空・時刻）②F1（草木・風）③A2（雲）④W1（水）⑤UP1（性能余裕を作る）⑥SH1/SH2（材質の見え）⑦RT1（反射）⑧VG P4（重量シーン。**ユーザー確定の最優先なので V レーンは止めない**）⑨P8（GI）⑩P7（影の精度）。V レーンは常に動かし、他の 3 レーンをこの順で埋める。

---

## 5. リスクと撤退条件

### 5.1 リスク表

| ID | リスク | 検知 | 対策 | 撤退条件 |
|---|---|---|---|---|
| **G-R1** | **UE 5.8 が空（再インストール未実施）／UE のビルド差で基準画像が変わる**（5.8.0 と 5.8.2 が混在） | Q0 の `ls`、`Build.version` を基準画像のメタデータに保存 | 再インストール後に**ビルド番号を固定**、キャッシュキーに含める。更新時は基準を全再生成し「再ベースライン」を履歴に記録 | UE を再導入できない間は**UE 基準なしで運用**: Uno 側の絶対指標（健全性 G0・性能ゲート）のみ判定し、UE 比は `skipped`。ユーザーが手動で撮った UE 画像を `dx12_look_compare`（既存）へ渡す簡易比較に縮退 |
| **G-R2** | **UE の窓が出る／専用デスクトップで動かない**（§3.2 の UNVERIFIED ①④）→ ユーザーの操作を妨げる | Q0 の監視ログ（窓・フォアグラウンド）、L4 の自動 kill | L1→L2→L3 の順に採用レベルを確定。承認ファイル + 不在時間帯 + Job 制限 | **全レベルで窓が出る／動かない**なら、UE の自動起動を**やめる**。「ユーザー在席・明示許可のもと窓が出てよい時間に 1 回だけ走らせる」運用、または**UE 側の撮影はユーザー自身がエディタの MRQ で実行**し、AI は設定ファイルと手順書（`tools/parity/UE_MANUAL.md`）を渡すだけにする |
| **G-R3** | **UE 基準の非決定性**（TSR 非決定論・Lumen 収束・ストリーミング）でノイズ床が目標より大きい | Q1 の「UE 同士の再撮影差」 | 温め運転フレーム増、EXR を N 枚平均、`-Deterministic`、`r.Streaming.FullyLoadUsedTextures=1`。しきい値は max(固定, 床×1.5) | 床が G1 の値を超える場合は G1 を床に合わせて緩め**理由を履歴に残す**。G2 は「統計 + 目視のみ」に降格 |
| **G-R4** | **指標が「見た目の良さ」を表さない**（GI・肌・雲）／FLIP が局所差に鈍い | 目視との乖離 | 目視承認の併用（コンタクトシート）、局所ヒートマップ、LPIPS を参考表示 | 指標と目視が継続的に食い違う項目は**ゲートから外し目視のみ**にする |
| **G-R5** | **8 GB VRAM の壁**（VG の DAG 約 1.25 GB + テクスチャ + AS + 本書の追加 約 0.2〜0.3 GB + FSR 61〜75 MB + RT/G-Buffer 等） | `perf_stats` の VRAM、DXGI budget、`dx12_diagnose` | 下の VRAM 予算表。VG は P5 のストリーミング、雲ノイズは 64³ へ、インポスター解像度を下げる | 屋外フル + 重量 VG の同時運用で 7 GB を超える場合は、**VG 予算 1 GB へ絞る**か、重量シーンと屋外シーンを分離運用（PS-2 と PS-4 は別シーン） |
| **G-R6** | **ルートシグネチャ残量 2 DWORD / `t` 番号の衝突** | RS 直列化失敗（全描画が死ぬ）、`t` 二重宣言 | 本書は全機能 **0 DWORD**（§2.0.2）。`t` 番号は §2.0.2 の表に一元化。slot 11 へのレンジ追加時は 1×1 ダミーを常時貼る（未初期化ディスクリプタでデバッグレイヤが落ちる、`Lighting.hlsli` の DDGI 注記と同じ事情） | slot 11 へレンジを足せない場合は、SH1 を「ID のみ・係数は定数」の固定モデルへ縮小（拡張パラメータ `t27` を持たない）。**残り 2 DWORD は他機能のため触らない** |
| **G-R7** | **FSR の RTX 上の実コスト・品質が不明**、DLL 同梱可否 | UP1 初日の実測、`license.md` の再読 | 初日に「フルスクリーンで FSR だけ回す」最小プログラムで ms を測る。同梱不可なら**ソース（MIT）静的リンク**へ切替 | FSR 3.1 が 1.5 ms を超える or 品質不足なら**自前 TAAU（VG 書 P9 の TSR 元案）へ戻す**（+8 日） |
| **G-R8** | **DLSS のライセンス**（エンジンが MIT 公開 = 4.e、NVIDIA 限定、事前通知） | 法務確認 | UP2 を保留。同梱は配布ビルドのみ | 確認が取れなければ実装しない |
| **G-R9** | **半透明の分離（水）が `RenderSceneMeshes` を触る**＝VG P4 の H3 差込と衝突 | マージ時のコンフリクト、`golden` の差 | X0 で空フック先行、W1 の最初の 2 日で分離だけ先にマージ | 分離が困難なら水を「sortKey 3 のカスタム描画パス」として別ドローで描く（屈折は不透明色コピーを別途取得） |
| **G-R10** | **雲が 1.2 ms を超える** | A2 の GPU 計測 | 1/16 → さらに 1/2 解像度、サンプル数削減、`Conservative Density` 相当の早期打ち切り | 超過が解消しなければ 2D 層雲 + 低コストの体積 1 層に縮退 |
| **G-R11** | **`ExecuteIndirect` + 既存の入力レイアウト/PSO の互換性が未検証**（植生の RS ゼロ案の前提） | F1 初日に最小プロトタイプ（1 ドロー）を実測 | 互換しなければ compact バッファを SRV で読む VS（slot 3 の bones テーブルを流用）へ | 間接描画が不可ならチャンク単位の `DrawIndexedInstanced`（CPU 発行、チャンク 数千 → 数百ドロー）へ縮退 |
| **G-R12** | **PerFrame の VS からの可視性未確認**（風を VS で読む前提） | X0 で `ForwardInstanced.hlsl` の CBV 可視性を確認 | 不可視なら風の値は per-instance ストリームの空き（`color.w` など）に量子化して運ぶ | — |
| **G-R13** | **分離型 SSS の α 再利用が他パスと衝突** | SH3b 冒頭スパイク | 衝突なら不透明の α を SSS 専用に予約 | プレインテグレートのみで出荷（SH3b を実施しない） |
| **G-R14** | **UE 側シーン構築（Python）が commandlet/オフスクリーンで動かない**（§3.4-D は UNVERIFIED） | Q0 | 撮影経路 3 本（MRQ / Python tick + HighResShot / SceneCapture）から採用 | 全て不可なら UE 側の組み立てをユーザーが UE エディタ上で 1 回だけ手動で行い、レベルとカメラを固定して以後の撮影だけ自動化 |
| **G-R15** | **見積もりの不確実性**（Q1・UP1・RT1・F1 が大） | 各段階の実績 | 段階ごとの合否ゲート、Q0/Q1 の実測で以後を再見積もり | — |
| **G-R16** | **VG 実装との衝突**（`src/renderer/vg/` が 2026-09-30 04:25 に空で作られている＝他セッションが着手済みの可能性） | 着手前に `ls`・ブランチ確認 | 着手前にユーザー確認。領域の分担を X0 で明文化 | — |
| **G-R17** | **権利**: UE の出力画像・Dead_Mall 由来素材は再配布不可（`Dreamcore/README.md`）／DLSS DLL | — | `.dx12/parity/` は **git 管理外**、公開リポジトリ（MIT）に基準画像・UE 由来素材を入れない | — |
| **G-R18** | **開発機が RTX 5060 のみ**（AMD/Intel 未検証。FSR4・XeSS・DLSS の実機確認不可） | — | 検証できない機能は作らない（FSR4 は DLL 差替えで自動、固有実装なし） | — |

### 5.2 VRAM 予算（1080p / RTX 5060 8 GB、[推定]。各段階の実測で差し替え）

| 項目 | 見積もり |
|---|---:|
| VG の DAG（5000 万 tri 全常駐、VG 書 §0） | 約 1.25 GB |
| テクスチャ・メッシュ（BC7/BC5、シーン依存。Dead_Mall 級で 1〜3 GB） | 約 2〜3 GB |
| DXR の TLAS/BLAS + DDGI + RT の RT | 約 0.4〜0.6 GB |
| G-Buffer / 深度 / HiZ / SSAO / SSR / SSGI / TAA 履歴 / ポスト RT（1080p） | 約 0.4 GB |
| 本書の追加（§2.0.4: 大気 3 MB・雲 12 MB・水 24 MB・植生 約 50 MB・RT1 20 MB・インポスター 8 MB/種） | 約 0.2〜0.3 GB |
| FSR 3.1（Performance 61 MB / Quality 75 MB [出典]） | 約 0.08 GB |
| **合計** | **約 4.5〜6 GB**（8 GB に対し余裕 約 2 GB） |

---

## 6. 未決事項（ユーザーに聞くべき点・5 件）

1. **UE 5.8 の再インストールと、UE を走らせる運用ルール**
   `C:\Program Files\Epic Games\UE_5.8` は空で、Launcher の manifest だけが残っている（§0-1）。①再インストールしてよいか、5.8.0（以前の版）か 5.8.2（Launcher の最新）か ②UE を走らせるのは**ユーザーが不在にする時間帯だけ**、起動前に**ユーザーが `approve_ue_run.bat` をダブルクリック**する承認ファイル方式でよいか（AI は承認なしに UE を起動しない。Q0 の最初の数回は立会い）。
   → **推奨: 5.8.2（最新）を入れて以後ビルド固定。承認ファイル方式 + 不在時間帯のみ。Q0 のスパイクはユーザー立会いで L1→L2→L3 を 1 回ずつ**。窓が出るモードしか無いと分かった場合は、UE の撮影をユーザーの手動実行に切り替える（G-R2）。

2. **合格ラインと UE 側の基準設定**
   UE の基準は「現行既定（Lumen GI + Lumen 反射 + VSM + Nanite + Substrate ON、MegaLights OFF）」でよいか。合否は G2（同等）を全シーンに課すか。
   → **推奨: PS-0 と PS-1 は G2、PS-2〜PS-5 は G1**（雲・動く水・肌・髪は統計 + 目視。GI・肌の「良さ」は指標では保証できないので、節目ごとにユーザーがコンタクトシートで承認する）。しきい値は Q1 のノイズ床実測で固定する。

3. **VG 書 P9 の TSR を FSR 3.1 統合へ置換してよいか / DLSS は保留でよいか**
   TSR 自前 8 日 → FSR 3.1 直接統合 10 日（どちらでも「表示解像度側ポストの分離」が要る）。FSR 4 は RTX 5060 で動かない [出典]。DLSS は技術的に可能だがエンジンが MIT 公開であることが NVIDIA のライセンス 4.e と衝突しうる・NVIDIA 限定・事前通知が要る。
   → **推奨: FSR 3.1 へ置換（P9 は DoF/Bloom のみ残す）。DLSS は保留**（配布ビルドにだけ同梱する形を法務確認してから）。

4. **PS-3（キャラ: 肌・髪・布）の資産の調達方針**
   生成 3D は肌・髪の品質が出にくく、権利関係の懸念もある。
   → **推奨: Blender MCP で頭部・髪カード・布を自作**（変換規約 §3.4-C 準拠）。拡張機能の単体テストは Khronos の glTF サンプル資産（ClearCoat / Sheen / Anisotropy の各サンプル）を使う（取得・許諾は着手時に確認）。CC0 の人体を使う案もあるが、髪・肌のマテリアルは結局自作が要る。

5. **着手順序とレーン数、条件付き 3 件の扱い**
   最初の 30 日は 4 レーン（V: VG / H: 計測 + FSR + RT / A: 空・雲・水 / F: 植生 + 材質）でよいか。ビルド直列化と GPU 共有で実効 3〜3.5 レーン（+25%）になる。条件付きの W3（FFT 海洋 6 日）・SH3b（分離型 SSS 5 日）・UP2（DLSS 6 日）は**パリティで不足が出たときだけ着手**でよいか。
   → **推奨: 4 レーンで開始し、VG レーンは止めない。条件付き 3 件は「PS-x のゲートが G1 に届かないと計測で分かったら」着手**。

---

## 付録 A. 参照した主な実コード（本書の根拠）

`src/graphics/RootSignature.cpp`（`:11-23,353`）/ `shaders/forward/{Forward.hlsl（`:55-100,225,280-335,363`）,Lighting.hlsli（`:51-93,146-235`）,PBR.hlsli（`:9-122`）,ForwardInstanced.hlsl,Terrain.hlsl}` / `src/core/ApplicationRender.cpp`（`:84-471,1602,1983,3809-3820,4050-4059,4418,4482-5627,5939-5942`）/ `src/core/ApplicationScene.cpp`（`:85-171,223,266`）/ `ApplicationPipeline.cpp`（`:574-653`）/ `src/renderer/{Skybox*,IBLBaker.h,Mesh.h,Material.h,DrawItem.h,RaytracingScene.h,RtScreenPass.*,DdgiVolume.*,ScreenSpaceGiPass.h,TaaPass.*,TaaJitter.h,VolumetricFog*}` / `shaders/{fog/*,ddgi/DdgiProbeUpdate.hlsl（`:184-240`）,raytracing/RtBindless.hlsli,post/{PostProcess.hlsl,TAA.hlsl},velocity/VelocityCommon.hlsli,templates/{Water,Ocean}.hlsl,UnoCustom.hlsli}` / `src/scripting/ScriptEngine.cpp`（`:3802-3812`）/ `src/editor/LightMath.h`（`:108-146`）/ `src/mcp/{ApplicationMcpEditor.cpp,ApplicationMcpLighting.cpp}` / `src/main.cpp`（`:338-534`）/ `src/core/BackgroundMode.h` / `tools/mcp-server/toolset/{composite.ts,quality.ts}` / `lookCompare.ts` / `tools/bench/{golden.mjs,lib/engine.mjs}` / `tests/golden/golden.json` / `tools/build.ps1` / `CLAUDE.md`（`:43-48,326-345`）/ `LICENSE`（MIT）。姉妹文書: `docs/VIRTUAL_GEOMETRY_DESIGN.md` / `docs/MCP_ENHANCEMENT_DESIGN.md`（§4.5.3・§4.5.4・M9/M10）。memory: `dx12-realism-roadmap` / `dx12-ddgi` / `dx12-shading-traps` / `dreamcore-dead-mall` / `dx12-perf-tools` / `dx12-engine-build`。UE ローカル: `C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item`、`C:\Users\ryuto\Documents\Unreal Projects\AIMCPTEST\Saved\Logs\AIMCPTEST.log`、`…\EpicGamesLauncher\Saved\Logs\EpicGamesLauncher.log`。

## 付録 B. 出典（本書が [出典] とした URL）

- **大気**: Hillaire EGSR 2020 https://sebh.github.io/publications/egsr2020.pdf / 参照実装 https://github.com/sebh/UnrealEngineSkyAtmosphere（MIT）/ Bruneton https://github.com/ebruneton/precomputed_atmospheric_scattering（BSD-3）/ Bevy 大気 https://github.com/bevyengine/bevy/tree/main/crates/bevy_pbr/src/atmosphere / UE Sky Atmosphere https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine / UE Sky Lights https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-lights-in-unreal-engine
- **雲**: Schneider 2015 https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf / Nubis 2017 https://d3d3g8mu99pzk9.cloudfront.net/AndrewSchneider/Nubis-Authoring-Realtime-Volumetric-Cloudscapes-with-the-Decima-Engine-Final.pdf / Nubis Evolved https://www.guerrilla-games.com/read/nubis-evolved / Nubis3 https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf / UE Volumetric Cloud https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine
- **水**: Tessendorf https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2004.pdf / UE Single Layer Water https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine / UE Water System https://dev.epicgames.com/documentation/en-us/unreal-engine/water-system-in-unreal-engine / https://github.com/2Retr0/GodotOceanWaves（MIT）/ https://github.com/gasgiant/FFT-Ocean / https://github.com/wave-harmonic/crest
- **植生**: GPU Gems 3 第 6 章 https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees / 第 16 章 https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis / UE Nanite Foliage https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage / GoT 草 https://gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass / ExecuteIndirect https://learn.microsoft.com/windows/win32/direct3d12/indirect-drawing / https://github.com/2Retr0/GodotGrass
- **シェーディング**: Filament https://google.github.io/filament/Filament.html / Jimenez SSS https://www.iryoku.com/separable-sss/ 、ライセンス https://github.com/iryoku/separable-sss/blob/master/LICENSE.txt / Burley SSS https://graphics.pixar.com/library/ApproxBSSRDF/paper.pdf / UE Subsurface Profile https://dev.epicgames.com/documentation/en-us/unreal-engine/subsurface-profile-shading-model-in-unreal-engine / Karis 髪 https://blog.selfshadow.com/publications/s2016-shading-course/karis/s2016_pbs_epic_hair.pdf / Marschner https://graphics.stanford.edu/papers/hair/hair-sg03final.pdf / Estevez & Kulla https://fpsunflower.github.io/ckulla/data/s2017_pbs_imageworks_sheen.pdf / Burley 2012 https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf / Kulla & Conty https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_slides_v2.pdf / glTF 拡張一覧 https://github.com/KhronosGroup/glTF/blob/main/extensions/README.md / UE Substrate https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine
- **アップスケーラ / RT**: AMD FSR SDK https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK（リリース /releases、ライセンス docs/license.md、ffx-api https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/getting-started/ffx-api.md）/ https://gpuopen.com/amd-fsr-sdk/ / NVIDIA DLSS https://github.com/NVIDIA/DLSS（LICENSE.txt）/ Streamline https://github.com/NVIDIA-RTX/Streamline / NRD https://github.com/NVIDIA-RTX/NRD / Intel XeSS https://github.com/intel/xess / UE 5.8 リリースノート https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes
- **UE の起動・撮影・決定論**: コマンドライン一覧 https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-command-line-arguments-reference / Pixel Streaming 参照 https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-pixel-streaming-reference / MRQ CLI https://dev.epicgames.com/documentation/en-us/unreal-engine/using-command-line-rendering-with-move-render-queue-in-unreal-engine / MRQ 出力 https://dev.epicgames.com/documentation/en-us/unreal-engine/cinematic-render-settings-and-formats-in-unreal-engine / MRQ 画質 https://dev.epicgames.com/documentation/en-us/unreal-engine/cinematic-rendering-image-quality-settings-in-unreal-engine / Python https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python / スクショ https://dev.epicgames.com/documentation/en-us/unreal-engine/taking-screenshots-in-unreal-engine / 露出 …/auto-exposure-in-unreal-engine / TSR …/temporal-super-resolution-in-unreal-engine / Lumen …/lumen-technical-details-in-unreal-engine / トーンマップ …/color-grading-and-the-filmic-tonemapper-in-unreal-engine / Windows Desktops https://learn.microsoft.com/en-us/windows/win32/winstation/desktops
- **指標**: FLIP https://github.com/NVlabs/flip 、https://pypi.org/project/flip-evaluator/ / scikit-image https://scikit-image.org/docs/stable/api/skimage.metrics.html / LPIPS https://github.com/richzhang/PerceptualSimilarity

**低信頼度（フォーラム。設計の根拠にせず注意喚起のみ）**: https://forums.unrealengine.com/t/renderoffscreen-large-resolutions-not-working/671085 / https://forums.developer.nvidia.com/t/streamline-plugin-crashes-in-unreal-5-3-packaged-game-with-renderoffscreen-command/268872 / https://forums.unrealengine.com/t/static-mesh-merging-requires-rhi/2731294 / https://unrealcontainers.com/blog/offscreen-rendering-in-windows-containers/

**本書が更新した過去の記述**: ① VG 書 P9 の「TSR 自前」を FSR 3.1 統合へ置換する提案（未決 §6-3、承認前は VG 書のまま）② memory `dx12-realism-roadmap` / 依頼文の「ルートシグネチャ残り 3」は古く **62/64・残り 2**（VG 書 F13 と同じ）③ `RaytracingScene.h:27-30` のヘッダコメント（スキンド/半透明を除外）は古く、実際の除外は半透明とアルファマスクのみ（`DrawItem.h:117-121`）。**覆した決定はない**（DirectSR 却下・フォワード PS の MRT 化禁止・DDGI 確定・フルフレームグラフ却下を全て踏襲）。
