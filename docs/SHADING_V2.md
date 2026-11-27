# SHADING_V2 — シェーディング V2 の軸（W1-C3 / MH-0a）

シェーディングの新しい世代（マイクロシャドウ、GTSO、horizon occlusion、直接光 BRDF v2 など）を、
**既存のシーンの絵を 1 ビットも変えずに**入れていくための「軸」。このファイルは軸そのものの仕様で、
V2 の中身（C4 以降）は項目ごとにここへ追記する。

## 1. 用語

| 用語 | 意味 |
|---|---|
| `shadingRevision` | シーンごとの設定（`ShadingSettings::shadingRevision`）。1 = V1（従来。既定）、2 = V2。 |
| `UNO_SHADING_V2` | V2 のシェーダをコンパイルするときの `-D`。V1 の .cso は定義しない。 |
| V2 の .cso | `*V2_PS.cso`（名前の規則は下の表）。V1 の .cso とは別のファイル。 |

## 2. 約束事（変えない）

1. **V1 の .cso の DXC コマンドラインと DXIL は 1 文字も変えない。** V2 は別の .cso として並べる。
2. 既存シーンは `shadingRevision=1` のまま。V2 を選ぶのは新規シーンのルック（C8）と品質プリセット Epic（C7）だけ。
3. `shadingRevision=2` かつ全機能 OFF（`microShadow` / `horizonOcclusion` / `gtso` = false）は V1 と同じ絵になる。
   今は V2 の .cso に差分となるコードが無いので、機能のフラグに関係なく V1 と同じ命令列になる。
4. V2 の PSO は、`shadingRevision=2` のシーンを開く（または MCP で切り替える）まで作らない。起動時間を延ばさない。
5. V2 の .cso のプロファイルは V1 と同じ。SM 6.9 への引き上げは F 段で、別の .cso として行う。
6. V2 の .cso は `-HV 2021 -WX`（警告をエラーにする）。`uno_shader()` の `NEW` が付ける。

## 3. 宣言された V2 の .cso（`shaders/manifest/lane_c.cmake`）

| V2 の .cso | ソース / エントリ | 追加の `-D`（V2 以外） | 選ぶ側 | 状態 |
|---|---|---|---|---|
| `ForwardV2_PS` / `ForwardPhysV2_PS` | `forward/Forward.hlsl` `PSMain` | `UNO_PHYSICAL_LIGHTS=1`（Phys） | `ApplicationPipeline.cpp`（静的メッシュ・スキンド共用） | 結線済み |
| `ForwardMaskV2_PS` / `ForwardMaskPhysV2_PS` | 同上 | `ALPHA_TEST=1` | 同上 | 結線済み |
| `TerrainV2_PS` / `TerrainPhysV2_PS` | `forward/Terrain.hlsl` | | `ApplicationPipeline.cpp` | 結線済み |
| `VgResolveV2_PS` | `vg/VgResolve.hlsl`（ps_6_6） | | `VirtualGeometrySystem.cpp`（`LoadResolvePs`） | 結線済み（FU-E-25） |
| `ForwardFoliage{,Phys,Mask,MaskPhys}V2_PS` | `foliage/ForwardFoliage_PS.hlsl` | `FOLIAGE_MASK=1`、`UNO_PHYSICAL_LIGHTS=1` | `FoliageSystem.cpp`（`BuildForwardPsos`） | 結線済み（FU-E-25） |
| `WaterSurface{,Phys}V2_PS` / `WaterUnder{,Phys}V2_PS` | `water/WaterSurface.hlsl` / `water/WaterUnder.hlsl` | `UNO_PHYSICAL_LIGHTS=1` | `WaterRenderer.cpp`（`BuildPsos`） | 結線済み（FU-E-25） |

仮想ジオメトリ・植生・水の 3 系統は C3 では軸の宣言だけだったが、FU-E-25 で結線した（`renderer/ShadingRevisionPick.h` の `PickPs` が `FwdPsNameRev` と同じ流儀で `*V2_PS.cso` を選び、無ければ警告して V1 へ縮退する。`UpdateShadingRevision` が GPU を待った後に `SetShadingV2` で追従させる）。
結線は、選ぶ側のファイルの所有が決まった項目で行う（`FwdPsNameRev` と同じ形: V2 が無ければ警告して V1 へ縮退）。

## 4. 実行時の経路

```
MCP set_shading {shadingRevision:2}  /  シーンを開く
        │  m_scene->GetShadingSettings().shadingRevision
        ▼
Run ループの先頭（フレーム外）→ EnsureSplitResources → AppAccess::UpdateShadingRevision
        ├─ GraphMaterialSystem::SetShadingRevision(rev)     ← 毎フレーム。値が変わったときだけ作り直す
        └─ 反映済みの値と違えば WaitIdle → RecreateForwardPsos / RecreateSkinnedPsos / RecreateTerrainPsos
                                     └─ FwdPsNameRev が V2 の .cso を選ぶ（無ければ警告して V1 へ縮退）
```

- 反映済みの値と V2 の PSO の数は `dx12_get_shading` の `appliedRevision` / `v2PsoCount`（`v2Pso.{forward,skinned,terrain,water,foliage,vg}`。water = 水面+水中の PSO（0..2）、foliage = フォワード PSO（0..4）、vg = resolve の PSO（作った後だけ 1。VG を描く前は 0）。いずれも実体ができた V2 PSO だけを数える）と、ログの
  `shadingRevision=2: フォワードの V2 PSO N 本` で確認できる。
- `Application.h` は W1 では凍結なので、反映済みの値は `ApplicationPipeline.cpp` の中に持つ（`s_shadingRevApplied`）。
- `ForwardPsoProvider`（ステージフックの読み取り口）は、PSO のポインタを毎フレーム `m_pipelineState*` から詰める。
  PSO を差し替えれば V2 の PSO が自動で見える（`Select` の形は変えない）。

## 5. ForwardGraph（マテリアルグラフ）

実行時に DXC で ps_6_6 をコンパイルするので、同じ軸を実行時の引数で表す。

- `GraphMaterialSystem::SetShadingRevision(2)` のとき、**Forward 種別だけ**がコンパイル引数に `-D UNO_SHADING_V2=1` を足す。
  プレビュー（PreviewLite）とノード内サムネイル（NodeLdr / NodeRaw）は常に V1。
- キャッシュのキー（ディスクキャッシュ）: rev=2 のときだけ `extra` に `|sv2` を足す。**rev=1 のキーは 1 文字も変わらない**（既存のキャッシュが生きる）。
- 変種（`m_variants`）のキー: `VKey(hash, kind, rev)`。rev=1 は従来と同じ値。
- インスタンスの作り直し: 値が変わると、作り済みの Forward 種別のインスタンスを `graphDirty` にする。HLSL が同じでも rev が違えば別の Build
  になり、新しい版が描ける（Promote）まで旧版で描き続ける。
- 配布ビルド（`BakeForBuild`）は V1 だけを焼く。配布先で `shadingRevision=2` のグラフ材質は V1 のシェーダで描かれる（V2 の焼き込みは、V2 に中身が入る C5 以降で追加する）。

## 6. 検証

- V1 の DXIL が変わっていないこと: `pwsh tools/shader_dxil_equiv.ps1`（前後を `-Snapshot` で撮って `-Compare`）。
- V2 の .cso の命令列が V1 と同じであること（今は差分コードが無い）: 同じ `-D` で V1 / V2 を DXC に通して逆アセンブリを比較。
- `shadingRevision=2` の golden_core が V1 と画素一致すること、`appliedRevision=2` と `v2PsoCount>0` が出ること: MCP の
  `set_shading` → `get_shading`、`screenshot_final`。

## 7. 今後入るもの（このファイルの表に追記する）

| 項目 | 内容 | 入る場所 |
|---|---|---|
| C4 | ルックデブのハーネス（V1 / V2 の比較の土台） | `tests/golden/scenes/lookdev_*` |
| C5 | マイクロシャドウ、GTSO、horizon occlusion（`PfSlot::ShadingV2` のフラグで個別に切る） | **入った（W1-C5）。§9**。置き場は `Lighting.hlsli` / `ForwardShade.hlsli`（`BrdfV2.hlsli` は未使用の枠のまま。理由は §9.4） |
| MH-2 | 直接光の BRDF v2 | `BrdfV2.hlsli` |

`BrdfV2.hlsli` を `#include` するときは、同じ変更で `src/resource/ShaderRegistry.cpp` の依存表（`staticDeps`）に
`forward/BrdfV2.hlsli` を足す（`ShaderIncludeDeps` テストが漏れを検出する）。

## 8. 未解決（メインの判断待ち。C3 の所有外）

1. **水 / 植生 / VG の V2 の選択（FU-E-25 で解決済み。下は経緯）**: V2 の .cso（WaterSurface/Under{,Phys}V2、ForwardFoliage*V2、VgResolveV2）は宣言・ビルド済みだが、
   選ぶ側（`WaterRenderer`・`ApplicationWater.cpp`・`FoliageSystem.cpp`・`VirtualGeometrySystem.cpp`）は W1 の所有表に所有者がいない。
   C6（影のバイアスを「シーン全体が V2 のときだけ」下げる）の前提なので、所有を決めて `FwdPsNameRev` と同じ流儀で結線すること。
2. **shadingRevision の保存（FU-E-27 で解決済み）**: `scene/SceneShadingSerializerLaneC.cpp`（lane-C のサイドカー。`SceneSerializer.cpp` は保存 1 行・読み込み 1 行の呼び出しだけ）が
   `shading`（`shadingRevision` と V2 の欄）と `shadowPcss`（LS-02a の欄）を、既定と違う値だけ書く。`set_shading` / `set_shadow_pcss` は Undo 対象。
3. **ホットリロード**: Forward / Skinned / Terrain の V2 .cso は `RegisterShaderReloadHandlers` に登録済み。
   再コンパイル表（`ShaderRegistry`）には FU-E-26 で ForwardV2 / ForwardPhysV2 / TerrainV2 を足した。ForwardMaskV2 / ForwardMaskPhysV2 / TerrainPhysV2 / 水・植生・VG の V2 は未登録（rev=2 のとき警告を出す）。
   `BrdfV2.hlsli` を include するシェーダが現れたら `staticDeps` に足すこと。
4. **Application.cpp への直接の呼び出し**: `UpdateShadingRevision` は `EnsureSplitResources` の先頭から呼ばれている（相乗り）。
   マージ時に lane A が Run ループの `UpdateLightingUnits` の隣へ明示的に呼び、相乗りを外すこと。
5. 切替時に `InvalidateTemporalHistory(true)` を呼ぶ（TAA / DDGI / フォグの履歴を捨てる）。

### 8.x 追補（レビュー指摘の検証結果）
- 水・植生・VG の V2 結線、`shadingRevision` の保存、ShaderRegistry の V2 エントリ: 指摘は正しい。C3 では直さず、保存は FU-E-27（lane-C のサイドカー）、ShaderRegistry は FU-E-26 で対応した。
- ホットリロード: rev=2 のとき警告を出す。FU-E-26 で ForwardV2 / ForwardPhysV2 / TerrainV2 は再コンパイル表に入った。警告は残りの ForwardMaskV2 / ForwardMaskPhysV2 / TerrainPhysV2 / 水・植生・VG の V2 を名指しする。
- `s_v2PsoCount[2]` は地形の .cso が無い早期 return でも 0 に戻すようにした。
- BrdfV2.hlsli を include するシェーダが現れると `ShadingV2AxisTests` が失敗する（BakeForBuild の V2 対応が先）。
- C3 の残り確認（FU-E-28、2026-11）: `shadingRevision=2` で V2 の 3 機能（microShadow / horizonOcclusion / gtso）を全部 OFF にすると、golden_core の core_pbr_spheres / core_csm_shadows / core_transparency / core_post_stack は V1 とネイティブ解像度で画素 SHA-256 が一致（appliedRevision=2・v2PsoCount=16）。機能を ON にすると差が出る（意図どおり）。GPU の `GraphMaterialTests`（§9 の shadingRevision の節を含む）は合格。

## 9. C5: マイクロシャドウ・GTSO・地平線オクルージョン（W1-C5）

`shadingRevision=2` かつ各機能が ON のときだけ効く。V1 の .cso は 1 命令も変わらない（`#ifdef UNO_SHADING_V2` の中だけ）。
CPU の写しは `src/renderer/ShadingV2Math.h`、性質の検査は `ShadingV2MathTests`。

### 9.1 式
| 機能 | 式 | 掛ける先 |
|---|---|---|
| マイクロシャドウ（Chan 2018） | `saturate(1 + k (saturate(abs(N.L) + 2 ao^2 - 1) - 1))`。ao = 材質 AO のみ（SSAO は画面依存で間接光向けなので入れない） | 太陽の `shadow`（CSM とコンタクト影の後）と、クラスタライトの radiance。IBL・間接光には掛けない |
| GTSO（Jimenez 2016） | 可視円錐（cos^2 Av = 1 - ao）と反射ローブ円錐（cos As = 2^(-3.32193 a^2)、a = roughness^2）の球面キャップの重なり（Oat and Sander の smoothstep 近似）÷「同じローブと半球の重なり」。軸は N と R の中間（近似ベントノーマル。N のままだと掠める角度で R が円錐の外へ出て黒い縁になる。真のベントノーマルは LS-07） | V1 の Lagarde の `so` を置き換え、`aoSpec`（IBL の鏡面。SSR が当たった所は SSR 優先）へ |
| 地平線（Filament の horizon fade） | `saturate(1 + k R.Ng)^2`。Ng = 視線側へ向けた幾何法線 | IBL・プローブ・SSR が入った後の `specularIBL`、IBL 無しの屋内では `probeSpec` |

★GTSO を半球の重なりで割るのは意図した差。割らないと ao = 1 でも掠める角度の反射が半分に落ち（フレネルが強い所）、地平線で切れる分を horizon とスプリットサムの LUT が持つので二重に暗くなる。

### 9.2 受け渡し
`PfSlot::ShadingV2`（`_clusterReserved[2]`）: x = shadingFlags（bit0 マイクロシャドウ / bit1 地平線 / bit2 GTSO。asuint）、y = microShadowK、z = horizonK（0..2）、w は C6 / MH-2 の欄。
`ApplicationRender.cpp` が毎フレーム書く。`shadingRevision != 2` のときは x = 0。分岐はすべて定数バッファの値による一様分岐。

### 9.3 MCP
`set_shading` / `get_shading` に `microShadow` / `horizonOcclusion` / `gtso`（bool、既定 true）と `microShadowK` / `horizonK`（0..2、既定 1。範囲外は `E_OUT_OF_RANGE`）。シーン JSON の `shading` に保存する（FU-E-27。既定と違う値だけ書く）。
TS 側（`tools/mcp-server/toolset/shading.ts` の shape）は NB の担当。それまでは `dx12_call` か生の MCP で使う。

### 9.4 BrdfV2.hlsli を使わない理由
`BrdfV2.hlsli` を include するには `ShaderRegistry.cpp` の `staticDeps`（所有者なし）と `BakeForBuild` の V2 対応が先に要る（`ShadingV2AxisTests` が止める）。
C5 は所有範囲に収めるため、既に全消費者の依存表に載っている `Lighting.hlsli` / `ForwardShade.hlsli` の `#ifdef UNO_SHADING_V2` の中に置いた。MH-2 で `BrdfV2.hlsli` へ移すときに一緒に整理する。

### 9.5 配布ビルド
`BakeForBuild` は V1 だけ焼く。V2 のシーンのグラフ材質は配布先で V1 の絵になる（`shadingRevision` はシーンに保存される（FU-E-27）が、配布ビルドの焼き込みは V1 だけなので配布先の絵は V1）。

### 9.6 適用範囲（W1 時点）
地平線オクルージョンはベース層の `specularIBL` / `probeSpec` だけ。クリアコート層の IBL・プローブ・SSR（`UNO_EXT_COAT` の中）には掛けていないので、法線マップ付きの金属にコートを載せると地平線の下を映すことがある。
地形（Terrain / TerrainPhys）・植生（ForwardFoliage）・VG の resolve は `UnoShadeLighting` を通るので、V2 の PSO ではマイクロシャドウ・GTSO・地平線オクルージョンが効く（dxc で V1/V2 の DXIL が異なることを確認済み）。地形の材質 AO はレイヤ AO、植生は材質 AO。水面は通らないので効かない。**この 3 経路（地形・植生・VG）に対する FLIP / 見た目の測定は未実施**（測れているのは lookdev 3 シーンと LS-B のみ）。
`hasIBL==0`（屋内フォールバック）の例外: 反射プローブの鏡面は GTSO ではなく従来の Lagarde so2 のまま（GTSO は IBL 経路の `so` だけを置き換える）。地平線オクルージョンは `probeSpec` にだけ掛かり、`ambientSpecular` の SSR 部分（`lerp(F0, ssrRgb, ssrConf)`）には掛けていない。
マイクロシャドウは太陽の `shadow` に掛けるため、植生の逆光透過（`UNO_SHADE_FOLIAGE_TRANSMISSION`）にも同じ |N·L| の減衰が入る（意図はしていないが、許容範囲として残している）。
V1 の絵が変わらないことは、`shading_v2_math_test` が V2 の記述（`UnoV2*` / `v2Ao` / `v2MicroK`）を `#ifdef UNO_SHADING_V2` の内側だけに限る字句検査で守る（DXIL の hash 比較までは自動化していない。変更時は dxc で V1 の .cso が不変なことを手で確認する）。
GTSO は解析式（`UnoV2CapsIntersection` を 2 回 = acos 6 回 / V2 画素）。V2 のフォワード GPU 時間は `lookdev_check` の perf（mainScene）で V1 と並べて記録する。予約済みの t40 GTSO LUT に替える余地がある。
**未達の受け入れ（C5 は完了にしない。C7/C8 は V2 を既定にしない）**: 金属球の地平線漏れ −50%（法線マップ付き金属球のシーンが無い）、GR-1 の凹部 EV ±0.1（`gi_eval` に shadingRevision=2 の条件が無い。NB の依頼）。Epic（DDGI あり）の測定も未実施。
`shadingFlags`（PfSlot::ShadingV2 の x）を書くのは `shadingv2::PackFlags` だけ。後続項目のビットも必ずそこへ足す。

## 10. C6: Epic の太陽影の構成（W1-C6 / LS-02a + AUD-01）

Epic = **RT の太陽影が静的な物を描き、動的 CSM は動的な物（スキンド・半透明）だけを描く**（既存の `skipRtCovered` の分担）。
`ShadowPcssSettings::epicDynamicCsm`（既定 false）が立ったときだけ働く。**既定（false）の経路は 1 ビットも変わらない**（V1 の DXIL 同値、3D ゴールデンのビット一致）。
CPU の写しと性質は `src/renderer/EpicCsm.h` / `tests/shadow_bias_math_test.cpp`。

### 10.1 構成
| 項目 | 旧経路（既定） | Epic（epicDynamicCsm） |
|---|---|---|
| カスケード | 4 本、遠端 400m | 3 本、遠端 200m（4 本目は「射影すると UV が範囲外 = 影なし」の番兵行列） |
| 影マップ | 2048 D32 × 4（64 MiB） | 2048 D16 × 3（24 MiB。`m_shadowMapDirty` で作り直す。SRV は R16_UNORM） |
| 影 PSO | DepthClip=TRUE、bias 8000 / 2.0 | **DepthClip=FALSE（pancaking）**、bias は D16 の単位へ換算（31 / 2.0）。静的・インスタンシング・スキンド・MASK 3 種・植生・VG |
| カリング | 6 面 | **near 面を外す**: `RenderDepthOnlyScene`（CPU）、VG の影の錐台（CPU の `PrepareVirtualGeometryShadow` と GPU の `ExecuteShadow`）、植生の影カスケードのカリング |
| PCSS | 3×3 PCF / Vogel の Load + SampleCmp | V2 の中だけ **Gather 版**: ブロッカー探索は `GatherRed` 8 回 × 4 テクセル、フィルタは `GatherCmpRed` × `gatherFilterTaps`（8..32） |
| カスケード境界 | 2 回サンプルして lerp | `cascadeDitherBlend` のとき IGN + 時間位相で片方だけサンプル（TAA / DLSS が平均する） |

切り替えは `AppAccess::UpdateEpicCsm`（`UpdateShadingRevision` の先頭から毎フレーム。GPU を待ってから PSO と影マップを差し替える）。PSO を 1 本でも作れなければ旧経路のまま（`epicCsmApplied=false`）。
状態と PSO は `RenderBackendExt::epicCsm`（Application.h は凍結なのでここへ持つ）。

### 10.2 バイアスの規則（V1 の受光面にアクネを出さない）
1. **影を描く側（ラスタ DepthBias）を `casterBiasScaleV2`（既定 0.25）倍にするのは「Epic かつシーン全体が `shadingRevision=2`」のときだけ**。V1 のシーンでは倍率 1（D16 換算のみ）。revision や倍率が変わると PSO を作り直す。
2. **受光側の法線オフセットは V2 の .cso の中だけ**（`UNO_V2_FLAG_CSM_RECEIVER`）: `worldPos += Ng * texelWorld[c] * (base + slope * (1 - |N.L|))`。`Ng` は光の側へ向けた幾何法線、
   `texelWorld` は `PfSlot::CsmTexel`、`base` / `slope` / タップ数は `PfSlot::ShadingV2.w`（各 8 ビット。`EpicCsm.h` の `PackReceiver`）。受光側の深度バイアス（`shadowParams.y`）は従来のまま。
3. フラグ（`PackFlags(settings, pcss)`）: bit3 受光側オフセット、bit4 Gather PCSS（PCSS 有効時のみ）、bit5 カスケードのディザ。**epicDynamicCsm かつ shadingRevision=2 のときだけ立つ**。

### 10.3 MCP
`set_shadow_pcss` / `get_shadow_pcss` に `epicDynamicCsm` / `pancaking` / `gatherPcss` / `cascadeDitherBlend` / `receiverNormalOffsetBase` / `receiverNormalOffsetSlope`（0..4）/ `casterBiasScaleV2`（0.05..1）/ `gatherFilterTaps`（8..32）。
`get` は `epicCsmApplied`（D16 × 3 へ切り替え済みか）を返す。シーン JSON の `shadowPcss` に保存する（FU-E-27。既定と違う値だけ書く。C7 / C8 が品質プリセット / プロファイルから立てる）。
TS 側（`tools/mcp-server`）は NB の担当。

### 10.4 既知の範囲外
- GPU 駆動のインスタンス群（`GpuInstDrawDepth` → `GpuInstCull`、`ApplicationInstanceGpu.cpp`）のカリングは near 面を外していない（所有者のいないファイル）。旧経路と同じく、光に近い側で箱の外の群は落ちる。
- 夜間に測る項目（ShadowCsm ≤ 0.25ms、静止 8 フレームの差、LS-A のスキンドの影の縁の幅、LS-F の夕方の cascade0）は GPU を占有する窓で行う。

### 10.5 Epic の見た目の変化（意図した挙動）
- カスケード境界のブレンドで cascade2 の遠端が番兵の cascade3（影なし）と混ざるため、V1 でも V2 でも 200m の手前で影がフェードして消える。
- RT 無しの Epic では、静的な物の影が 200〜400m で消える（旧経路は 400m まで）。RT ON では静的な物の影は RT が描く。
- Epic のとき `m_shadowDsvHandles[3]` は、解放済みの旧リソースを指したまま残る（今は誰も使わない）。`kNumCascades` で回すコードを足すときは、`epicCsm.active` のとき 3 本までにすること。
- バイアスを下げてよい判定は「適用済み revision==2、フォワード（MASK 含む）・スキンド・地形・水・植生のどれも V1 へ縮退していない」こと。VG は V2 PSO が初回描画まで 0 のため判定に入らない。
- 未確認（実機）: 植生と VG の Epic 経路。検証シーンに植生と VG を置き、`DX12_D3D_DEBUG=1` でエラー 0 件を夜間に確認すること。
