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
2. **shadingRevision の保存**: `SceneSerializer` は凍結のため保存されない。`set_shading` でシーンは dirty になるが、
   開き直すと 1 に戻る（C7 / C8 の前に、1 行の解凍か側路の判断が必要）。
3. **ホットリロード**: Forward / Skinned / Terrain の V2 .cso は `RegisterShaderReloadHandlers` に登録済み。
   `ShaderRegistry.cpp`（所有者なし）の依存表への V2 名の追加は、`BrdfV2.hlsli` を include する変更で一緒に行う。
4. **Application.cpp への直接の呼び出し**: `UpdateShadingRevision` は `EnsureSplitResources` の先頭から呼ばれている（相乗り）。
   マージ時に lane A が Run ループの `UpdateLightingUnits` の隣へ明示的に呼び、相乗りを外すこと。
5. 切替時に `InvalidateTemporalHistory(true)` を呼ぶ（TAA / DDGI / フォグの履歴を捨てる）。

### 8.x 追補（レビュー指摘の検証結果）
- 水・植生・VG の V2 結線、`shadingRevision` の保存（SceneSerializer）、ShaderRegistry の V2 エントリ: 指摘は正しい。いずれも W1 の所有表に所有者がない（または凍結）ため C3 では直していない。メインが所有を決める。
- ホットリロード: rev=2 のとき警告を出すようにした（ShaderRegistry 対応までの暫定）。§8.3 の「登録済み」は「ハンドラの対象ファイル名に登録済み。再コンパイル表は未対応」と読み替える。
- `s_v2PsoCount[2]` は地形の .cso が無い早期 return でも 0 に戻すようにした。
- BrdfV2.hlsli を include するシェーダが現れると `ShadingV2AxisTests` が失敗する（BakeForBuild の V2 対応が先）。

## 9. C5: マイクロシャドウ・GTSO・地平線オクルージョン（W1-C5）

`shadingRevision=2` かつ各機能が ON のときだけ効く。V1 の .cso は 1 命令も変わらない（`#ifdef UNO_SHADING_V2` の中だけ）。
CPU の写しは `src/renderer/ShadingV2Math.h`、性質の検査は `ShadingV2MathTests`。

### 9.1 式
| 機能 | 式 | 掛ける先 |
|---|---|---|
| マイクロシャドウ（Chan 2018） | `saturate(1 + k (saturate(abs(N.L) + 2 ao^2 - 1) - 1))`。ao = SSAO x 材質 AO | 太陽の `shadow`（CSM とコンタクト影の後）と、クラスタライトの radiance。IBL・間接光には掛けない |
| GTSO（Jimenez 2016） | 可視円錐（cos^2 Av = 1 - ao）と反射ローブ円錐（cos As = 2^(-3.32193 a^2)、a = roughness^2）の球面キャップの重なり（Oat and Sander の smoothstep 近似）÷「同じローブと半球の重なり」。N をベントノーマルの代わりに使う | V1 の Lagarde の `so` を置き換え、`aoSpec`（IBL の鏡面。SSR が当たった所は SSR 優先）へ |
| 地平線（Filament の horizon fade） | `saturate(1 + k R.Ng)^2`。Ng = 視線側へ向けた幾何法線 | IBL・プローブ・SSR が入った後の `specularIBL`、IBL 無しの屋内では `probeSpec` |

★GTSO を半球の重なりで割るのは意図した差。割らないと ao = 1 でも掠める角度の反射が半分に落ち（フレネルが強い所）、地平線で切れる分を horizon とスプリットサムの LUT が持つので二重に暗くなる。

### 9.2 受け渡し
`PfSlot::ShadingV2`（`_clusterReserved[2]`）: x = shadingFlags（bit0 マイクロシャドウ / bit1 地平線 / bit2 GTSO。asuint）、y = microShadowK、z = horizonK（0..2）、w は C6 / MH-2 の欄。
`ApplicationRender.cpp` が毎フレーム書く。`shadingRevision != 2` のときは x = 0。分岐はすべて定数バッファの値による一様分岐。

### 9.3 MCP
`set_shading` / `get_shading` に `microShadow` / `horizonOcclusion` / `gtso`（bool、既定 true）と `microShadowK` / `horizonK`（0..2、既定 1。範囲外は `E_OUT_OF_RANGE`）。シーン JSON には保存しない（`SceneSerializer` は W1 では凍結。shadingRevision と同じ）。
TS 側（`tools/mcp-server/toolset/shading.ts` の shape）は NB の担当。それまでは `dx12_call` か生の MCP で使う。

### 9.4 BrdfV2.hlsli を使わない理由
`BrdfV2.hlsli` を include するには `ShaderRegistry.cpp` の `staticDeps`（所有者なし）と `BakeForBuild` の V2 対応が先に要る（`ShadingV2AxisTests` が止める）。
C5 は所有範囲に収めるため、既に全消費者の依存表に載っている `Lighting.hlsli` / `ForwardShade.hlsli` の `#ifdef UNO_SHADING_V2` の中に置いた。MH-2 で `BrdfV2.hlsli` へ移すときに一緒に整理する。

### 9.5 配布ビルド
`BakeForBuild` は V1 だけ焼く。V2 のシーンのグラフ材質は配布先で V1 の絵になる（`shadingRevision` がシーンに保存されない W1 では、配布先は常に V1）。
