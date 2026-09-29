# UE cook スパイク（仮想ジオメトリ P6 冒頭）— UE の cook 済みアセットから、どこまで取れるか

- 日付: 2026-09-30
- 位置づけ: `docs/VIRTUAL_GEOMETRY_DESIGN.md` の **P6（UE アセット cook）冒頭スパイク**（リスク R3「UE の Nanite cook 済みアセットをフルレゾで取れない」）の結果。契約は `docs/VGEO_SPEC.md`（VGSRC v1.0）。
- 検証対象: Steam 版 **Dreamcore（UE 5.6）** の cook 済みパッケージ（`Dreamcore-Windows.utoc/.ucas` + `.pak`、uasset 19,802 個）。**UE 5.8 は再インストール待ちで未検証**。
- 方法: **ファイル読み出しだけ**（CUE4Parse）。ゲーム・UE エディタは起動していない。
- プロトタイプ: `tools/ue_cook/`（C# .NET 10。既定ビルド / ctest に含まれない。README に手順）。実アセット由来のデータ（メッシュ・`.usmap`・出力 VGSRC・CSV）は**コミットしていない**（`.gitignore` 済み）。この文書の数値は集計値と実アセット名だけ。

---

## 0. 結論（先に）

**Nanite 実体は「取れる」。フォールバックメッシュへの縮退（縮退案 A/B）は要らない。**

| 問い | 答え | 根拠（§2〜§3）|
|---|---|---|
| cook 済み StaticMesh に Nanite のクラスタ階層は入っているか | **入っている**（ストリーミングページ・クラスタ・階層ノード）。Nanite 有効メッシュにフォールバック（低ポリ通常 LOD）も**併存**するが、**フォールバックしか使えないわけではない** | Dreamcore の StaticMesh 12,115 個中 Nanite 2,854 個（23.6%）、全部で `PageStreamingStates` > 0 |
| CUE4Parse は Nanite をデコードできるか | **できる**（ページ → クラスタ → 頂点 / 三角形 / 材質範囲。UE 5.0〜5.6 実装済み、5.7 / 5.8 は**コード上の分岐あり・実データ未検証**）| `CUE4Parse/UE4/Assets/Exports/Nanite/*`（§2.1）|
| 実アセットで試した結果 | **Nanite 2,854 / 2,854 個が欠損 0 でデコード成功**。葉クラスタ（= LOD0 相当）の三角形合計 **16,846,176** が UE 側の `NumInputTriangles` の合計と**完全一致**（1 個も不一致なし）。デコード合計 43.6 s | §2.2 |
| 取れるデータ | LOD0 相当の全三角形・頂点位置（Nanite の格子量子化のまま）・法線・UV（最大 4〜8 組）・材質スロット別の範囲・AABB。**階層 LOD（全レベルのクラスタ + 誤差 / 境界）も復元できる**（全レベル合計は葉の約 2.05 倍）| §3 |
| 規模 | Dreamcore の最大は 670,530 三角形（1 メッシュ）。**取り出しは約 0.9 秒 / 67 万 tri、ピークメモリ約 290 MB**。現実装のまま扱える上限は概算 **約 2,500 万 tri / メッシュ**（8 GB 制限）。それ以上は**ページ単位のストリーム抽出**が要る | §5 |
| 最大の落とし穴 | ① **UE の cook 済み三角形は `cross(p1-p0, p2-p0)` が内向き**（VGEO_SPEC C26 / §12.5 の「UE のままでよい」と食い違う。§4.2）。② `.usmap` が無いと 1 バイトも読めない（今回は**ゲームを起動せず exe から静的に作れた**。§2.3）。③ マテリアル（MASK 判定・テクスチャ）は今回**未検証**（§6）| |

**推奨**: 縮退案は使わず、**Nanite の葉クラスタ（フル解像度）を VGSRC に出す**方針で P6 を進める。フォールバック LOD0 は、密なメッシュで元の 0.6〜11% しか三角形が無く（§2.2）、仮想ジオメトリの目的（近接の高精細）に合わないので**使わない**（非 Nanite メッシュの LOD0 だけが対象）。

---

## 1. 環境と手順

| 項目 | 内容 |
|---|---|
| ゲーム | Steam 版 Dreamcore（UE 5.6）。`C:\Program Files (x86)\Steam\steamapps\common\Dreamcore` |
| CUE4Parse | NuGet `1.2.2.202609`（コミット `b4e95441`、2026-09-01 ビルド。net10.0 必須）。ソース調査用に GitHub `HEAD 28f385e6`（2026-09-27）も読んだ |
| .NET | `C:\Users\ryuto\.dotnet10`（.NET 10.0.401）|
| バージョン指定 | `EGame.GAME_UE5_6`（5.5 以下だとパッケージ側で読み取り失敗。memory `dreamcore-dead-mall`）。AES 鍵は無し（ゼロ鍵）|
| `.usmap` | **手元に無かった**（過去のスクラッチパッドと共に消滅。`C:\Users\ryuto` と `D:\` を再帰検索して 0 件）。ゲーム起動が禁止なので、**exe から静的に作った**（§2.3）|
| 優先度 / メモリ | プロセスは `BelowNormal`。ワーキングセット上限 8 GB（実測ピークは最大 290 MB）|

---

## 2. 根拠

### 2.1 CUE4Parse のソース（Nanite のデコードは実装されている）

| ファイル（`CUE4Parse/UE4/Assets/Exports/Nanite/` 配下ほか）| 内容 |
|---|---|
| `NaniteResources.cs` | `FNaniteResources`。cook 済み `FStaticMeshRenderData` の中に入っている（`StaticMesh/FStaticMeshRenderData.cs:78`）。`RootData`（uexp 内のルートページ）+ `StreamablePages`（bulk。残りのページ）、`PageStreamingStates`、`HierarchyNodes`、`HierarchyRootOffsets`、`PageDependencies`、`NumInputTriangles` / `NumInputVertices` / `NumClusters`、`PositionPrecision` / `NormalPrecision`、5.6〜: `AssemblyTransforms` / `MeshBounds`。`LoadAllPages()` / `GetPage(i)` でページをデコード |
| `FNaniteStreamableData.cs` | 1 ページのデコード（`FFixupChunk`・`FPageDiskHeader`・`FClusterDiskHeader`・GPU ヘッダ・SoA のクラスタ → `Decode` → `ResolveVertexReferences`）|
| `FCluster.cs` | クラスタヘッダ（頂点数・三角形数・`PosStart` / `PosScale`・`LODBounds` / `LODError` / `EdgeLength` / `Flags`・材質エンコード）、三角形インデックスのデコード、材質範囲（3 分割の高速経路 / 材質テーブル）|
| `FNaniteVertex.cs` | 頂点のデコード: 位置（クラスタ局所の整数格子 × 精度）、法線（八面体 `2×NormalPrecision` bit）、接線（あれば）、頂点カラー、UV（最大 8 組、UV 範囲つきの浮動小数風エンコード）|
| `CUE4Parse-Conversion/Dto/MeshDto.cs:101-155`（`ParseNaniteResources`）| **`EdgeLength < 0` のクラスタ = 「高品質クラスタ」= 葉（フル解像度）**だけを集めて 1 本の LOD にする。`MeshLodDto.NaniteClusters.cs` が材質ごとにセクションを作る |

履歴（`git log -- CUE4Parse/UE4/Assets/Exports/Nanite`）: 2025-02-21 「updated nanite for UE 5.6」、2025-06-26 「full Nanite support for Static Meshes」、2025-11「fix nanite meshes / Nanite Constants fixes」、2026-07「Nanite boneInfluences / nanite skel mesh export」、2026-09-06「Skip nanite voxel clusters」。**ずっと保守されている**。

### 2.2 実験（Dreamcore、UE 5.6）

**全体スキャン**（`ue_cook --list-nanite`。uasset 19,802 個、24 s）

| 項目 | 値 |
|---|---|
| StaticMesh | 12,115 個（読み取りエラー 0）|
| うち Nanite | **2,854 個**（23.6%）。`ResourceFlags` は全部 `NONE`（アセンブリ / ボクセル / スキニング / カーブは無し）|
| Nanite 入力三角形数 | 合計 16,846,176。最小 2、中央値 750、p90 14,564、p99 50,160、**最大 670,530**（`Models/Gaturro/texturedMesh`、フォトグラメトリ系）|
| 非 Nanite の LOD0 | 9,261 個。合計 2,320,360 tri、中央値 32、p99 3,584、最大 138,405 |
| Nanite メッシュの通常 LOD（フォールバック）| 全部に存在（LOD0 合計 4,196,232 tri。入力の 24.9%）。**三角形 2 万以上の 179 個では中央値で入力の 11.3%**、最大級は 0.6〜1.2%（`SM_Sofa08` 478,010 → 2,958、`texturedMesh` 670,530 → 5,934）。→ **フォールバックだと、密なメッシュは形が粗くなりすぎる** |

**全デコード**（`--list-nanite --only-nanite --decode`。2,854 個、wall 70 s、デコード合計 43.6 s）

- **葉クラスタの三角形合計 16,846,176 = `NumInputTriangles` の合計 16,846,176。1 個ごとの不一致 0、読み取りエラー 0、ページ読み取り失敗 0、頂点の取りこぼし（属性が null）0**（設計書 P6 合否 4「UE 側と VGSRC の三角形数が一致（デコード欠損 0）」を Nanite 全数で満たす）。
- 面積ゼロ（同一頂点を含む / 外積が 0）の三角形は合計 85,941 個（全体の 0.51%。メッシュによっては 1〜2%）。UE 側の入力にもともと含まれるもので、欠損ではない。
- 全レベル合計のクラスタ三角形は葉の約 2.05 倍（`texturedMesh`: 葉 670,530 / 全レベル 1,372,557、クラスタ 10,914 個のうち葉 5,332）。

**代表メッシュ 11 個**（`--extract` → `vgeo_stub --validate-vgsrc`。時間は BelowNormal / UE 座標出力 / 溶接あり。ロードは `LoadPackage` + StaticMesh の読み出し）

| メッシュ | 種別 | 入力 tri | 取得 tri | フォールバック LOD0 | 頂点（溶接後）| ページ / クラスタ | セクション | ロード | デコード | 全体 | ピーク | VGSRC | 検証 |
|---|---|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---|
| `QuantumLab/SM_Server_rack_3` | Nanite | 2,676 | 2,676 | 790 | 3,304 | 2 / 46 | 1 | 86 ms | 26 ms | 129 ms | 89 MB | 0.1 MB | OK |
| `SM_ComputerMouse_A01_N1` | Nanite | 38,925 | 38,925 | 1,930 | 21,742 | 9 / 632 | 1 | 76 ms | 66 ms | 174 ms | 102 MB | 1.1 MB | OK |
| `SM_DeskChair_A01_N1` | Nanite | 95,616 | 95,616 | 2,138 | 52,558 | 19 / 1,554 | 1 | 81 ms | 119 ms | 256 ms | 115 MB | 2.7 MB | OK |
| `Fab/.../bed_version_03` | Nanite | 151,826 | 151,826 | 3,323 | 164,807 | 63 / 2,756 | 6 | 86 ms | 341 ms | 505 ms | 169 MB | 6.8 MB | OK |
| `SM_ComputerTower_A01_N1` | Nanite | 228,276 | 228,276 | 2,803 | 139,299 | 36 / 3,708 | 1 | 79 ms | 234 ms | 388 ms | 157 MB | 6.9 MB | OK |
| `Fab/Chain_Link_Fence/chain_link_fence` | Nanite | 347,680 | 347,680 | 34,713 | 210,894 | 73 / 5,663 | 1 | 115 ms | 445 ms | 635 ms | 217 MB | 10.4 MB | OK |
| `Sofa_Leather/SM_Sofa09` | Nanite | 360,014 | 360,014 | 3,786 | 189,444 | 68 / 5,840 | 2 | 93 ms | 430 ms | 590 ms | 190 MB | 9.9 MB | OK |
| `Sofa_Leather/SM_Sofa08` | Nanite | 478,010 | 478,010 | 2,958 | 253,034 | 90 / 7,765 | 2 | 86 ms | 484 ms | 722 ms | 231 MB | 13.2 MB | OK |
| `Models/Gaturro/texturedMesh`（最大）| Nanite | 670,530 | 670,530 | 5,934 | 373,704 | 101 / 10,914 | 2 | 92 ms | 593 ms | 872 ms | 288 MB | 19.1 MB | OK |
| `Dead_Mall/.../SM_MERGED_BP_SphereBall2_C_137` | 非 Nanite | — | 129,600（LOD0）| 129,600 | 78,165 | — | 1 | 200 ms | 33 ms | 259 ms | 118 MB | 3.9 MB | OK |
| `Megascans/.../Boxwood_..._Var5_lod3` | 非 Nanite | — | 138,405（LOD0）| 138,405 | 174,593 | — | 1（材質 2）| 329 ms | 34 ms | 386 ms | 242 MB | 6.9 MB | OK |

- 取得 tri = 入力 tri（Nanite 9 個すべて完全一致）。`vgeo_stub --validate-vgsrc`（`ReadVgsrc` = C++ 参照実装、ハッシュ込み）は全部 **OK**。
- Nanite の頂点は**クラスタ境界で重複**する（`texturedMesh` は溶接前 469,201 → 溶接後 373,704 = −20%。位置・法線・UV がビット一致するものだけ畳んだ値。法線がクラスタごとに量子化されるので、それ以上は畳めない）。
- 葉クラスタに**面積ゼロの三角形**が入っている（全体 0.51%、`SM_Sofa08` 7,626 / 478,010 = 1.6%、`SM_DeskChair` 1,856 / 95,616 = 1.9%）。VGEO_SPEC §9.3 のとおり **P1 が除く**（推奨事項）。
- 精度: Nanite の位置格子は `texturedMesh` で **1/32 cm（0.3 mm）**、法線は八面体 8 bit × 2（誤差は UE の元データより粗い）。VGEO の位置 24bit 格子（1 軸 1677 万分割）と法線 oct16×2 に**再量子化しても劣化は出ない**（Nanite の量子化の方が粗い）。
- **一括変換**（`ue_cook --extract-all`。Nanite は葉クラスタ、非 Nanite は LOD0、溶接あり、UE 座標）: **12,114 個成功**（うち Nanite 2,854）、失敗 1 個（`CubeGridToolOutput_6FAF75C7`: cook で通常 LOD が削除され、メッシュデータ自体が無い）。合計 **19,166,536 tri / 20,652,995 頂点（溶接後）**、VGSRC 853 MB、**114.6 s**（うちデコード 41.1 s）、ピーク 506 MB。出力 12,114 個すべてが `vgeo_stub --validate-vgsrc` を **OK**（`ReadVgsrc` の検証 + ハッシュ。全 2 分 38 秒）。

### 2.3 `.usmap` を静的に作れた（ゲーム起動が要らない道）

UE 5.6 の cook 済みパッケージは unversioned properties で、`.usmap` が無いと `MappingException: Package has unversioned properties but mapping file is missing` で 1 バイトも読めない（実測で確認）。手元の usmap は消えていて、`jmap` はゲーム起動が要る。そこで **`Dreamcore-Win64-Shipping.exe`（モノリシック）の静的リフレクション表**（UHT が生成する `Statics::PropPointers` / `FStructParams` / `FEnumParams`）を直接読む `tools/ue_cook/scripts/static_usmap.py` を作った。

- プロパティ params の配置（5.6）: `+0 NameUTF8` / `+16 PropertyFlags(u64)` / `+24 Flags(u32: EPropertyGenFlags)` / `+28 ObjectFlags` / `+48 ArrayDim(u16)` / `+50 Offset(u16)` / `+56 型別の追加(struct/enum の関数ポインタ)`。Array / Set / Optional / Enum は**直前の要素が内側**、Map は直前 2 つ（value, key）。`FStructParams` は `+24 Name / +32 PropertyArray / +40 NumProperties(u16)`。struct / enum の実体は `Z_Construct_UScriptStruct_*` / `Z_Construct_UEnum_*` を**逆アセンブル**して RIP 相対 `lea` の指す先から見つける（`capstone`）。
- 生成物は **2,010 バイト**（9 個のスキーマ = StaticMesh・StreamableRenderAsset・Object と依存 struct 6 個、enum 1 個）。**これで StaticMesh 12,115 個がエラー 0 で読めた**（= 添字空間の規則「共有ライブラリの全プロパティを宣言順に、配列は次元ぶん、派生クラス先頭」が StaticMesh 系で正しい）。
- **限界**: (a) 検証済みは StaticMesh 系のみ。(b) usmap の**バージョン情報（カスタムバージョン表）を書かない**ので、`MaterialInstanceConstant` は `NaniteOverrideMaterial`（`MaterialOverrideNanite` のネイティブ直列化）で読み取りに失敗した（CUE4Parse は失敗を握りつぶして既定値を返す。**Serilog を有効にしないと気付けない**。ツールは有効化済み）。マテリアル・配置（`WorldDto`）を読む P6 本体では、**ゲームを 1 回起動して `jmap` で usmap を取り直す**のが確実。(c) 5.6 の構造体配置が前提。5.8 の exe では再検証が要る。

---

## 3. 取れるデータの一覧

| データ | Nanite（葉クラスタ）| 通常 LOD0 / フォールバック | 備考 |
|---|---|---|---|
| 三角形（LOD0 相当、フル解像度）| **○ 完全**（`NumInputTriangles` と一致）| ○（Nanite メッシュでは 0.6〜11% に間引き済み）| 三角形リスト。材質ごとにまとまる |
| 頂点位置 | ○（cm、UE 座標。Nanite の整数格子 × 精度 = 1/32〜1/16 cm 程度）| ○（float）| クラスタ境界で重複あり |
| 法線 | ○（八面体 8 bit × 2 を float へ）| ○（UE の packed normal）| |
| タンジェント | ○（あるメッシュだけ `bHasTangents`。`ue_cook` は捨てる）| ○ | VGSRC v1 は持たない |
| 頂点カラー | ○（クラスタごとのビット幅）| ○ | VGSRC v1 は持たない |
| UV | ○（最大 4〜8 組。`ue_cook` は UV0 のみ）| ○ | 原点は UE のまま（左上）でよい（VGSRC の規則と一致）|
| 材質スロット別の範囲 | ○（クラスタごとに 3 分割 or 材質テーブル → 材質ごとの連続区間にまとめた）| ○（セクション）| 材質**名**（`スロット名|マテリアル名`）まで。中身は §6 |
| メッシュ AABB | ○（頂点から計算。`nr.MeshBounds`（5.6+）もある）| ○ | |
| 階層 LOD（親子クラスタ）| **○**（全レベルのクラスタと三角形。`LODBounds` / `LODError` / `EdgeLength` / `Flags`、ページの `FixupChunk`（グループ / 親の修正）と `HierarchyNodes`（BVH）も読める）| — | **Uno は P1 cooker で DAG を作り直す方針なので使わない**（LOD0 相当が取れれば十分）|
| ページ構造（`NumRootPages`・`PageDependencies`）| ○ | — | 使わない |
| 取れない / 未対応 | スキニング（ボーン影響）・ボクセル・カーブ・アセンブリ（5.6+ の新機能）。Dreamcore には**無い**（`ResourceFlags` 全部 `NONE`）。CUE4Parse 側は一部対応（`bVoxel` はスキップ）| — | マテリアルグラフは cook 時に落ちている（既知。memory `dreamcore-dead-mall`）|

---

## 4. 座標系と変換規則（VGSRC）

### 4.1 座標・単位

| | UE | Uno（VGSRC `coordSystem = 0`）|
|---|---|---|
| 軸 | X 前 / Y 右 / Z 上（**左手系**）| X 右 / Y 上 / Z 前（左手系）|
| 単位 | cm | m |
| 変換 | — | `(x, y, z)_UE → (y, z, x)`、位置に `0.01`（法線は置換のみ）。**巡回置換なので手系は変わらない** |

- `ue_cook` は既定で **UE 座標・cm のまま `coordSystem = 1`, `unitScaleToMeters = 0.01`** で出す（P1 の cooker / reader が変換）。`--engine-coords` で C# 側で変換して `coordSystem = 0` でも出せる。
- 検証: `texturedMesh` を両方で出し、AABB が `(y,z,x) × 0.01` で一致（UE: x −118.3〜66.2 / y −103.3〜19.3 / z −2.3〜205.2 cm ↔ Uno: x −1.03〜0.19 / y −0.02〜2.05 / z −1.18〜0.66 m）。フォールバックの AABB とも数 mm 以内で一致（Nanite 側が本来の形、フォールバックは間引き済み）。

### 4.2 巻き順（★ VGEO_SPEC との食い違い）

- **実測: UE の cook 済み三角形は `cross(p1-p0, p2-p0)` が「内向き」**（頂点法線と全三角形で逆向き）。
  - `Engine/BasicShapes/Cube`（閉じた立方体）: **符号付き体積 −1（AABB 体積比）**、法線との内積は 48 / 48 三角形で負。
  - Nanite 9 個・通常 LOD0 5 個（`Cube` / `SphereBall2` / `Boxwood` / `texturedMesh` のフォールバック / `SM_Server_rack_3` のフォールバック）でも、法線と逆向きの三角形が 98〜100%（残りは面積ゼロ・法線が縮退した三角形）。
- VGEO_SPEC §1 C26 は「`cross(p1-p0, p2-p0)` が外向き」、§12.5 は「UE の三角形をそのまま出せばよい」。**この 2 つは実データでは両立しない**。`vgeo_stub --emit-vgsrc --ue` の見本は raw のまま外向き（`check_vgsrc.py` で確認: 2,024 / 2,024 が外向き）なので、**実データとは巻きが逆**。
- **対処（実装済み）**: `ue_cook` は既定で **各三角形の 2・3 番目の頂点を入れ替えて外向きにして出す**（`--no-flip-winding` で raw）。出力後に `check_vgsrc.py` で外向き 98〜100%（残りは面積ゼロ等）、`Cube` は体積比 +1.0 を確認。
- **VGEO_SPEC / 設計書への申し送り**: §12.5 と設計書 §3.8 を「UE の cook 済み三角形は内向き。VGSRC の書き手（P6）が入れ替えて外向きで出す」に直す。`--emit-vgsrc --ue` の見本は実データと同じ向き（内向き）にするか、書き手が入れ替える前提の名前にする。または `coordSystem = 1` のとき reader / cooker が反転する規則を足す（どちらでもよいが、**どちらか 1 箇所に決める**）。

### 4.3 その他の規則

- UV: UE の UV（原点左上）をそのまま `uv0` に入れる（VGSRC の規則どおり。V 反転しない）。
- 材質添字: Nanite の材質添字は信用できない（範囲外があり得る）ので**スロット数へクランプ**（CUE4Parse も同じ）。
- 負スケール（鏡像配置）は**配置側の問題**（memory `dreamcore-dead-mall`）。`.vgeo` 経由でも、配置行列の行列式 < 0 のインスタンスは法線コーン軸の符号反転（VGEO_SPEC §5.4）で扱う。

---

## 5. 規模とスループット

- **1 メッシュ**: Dreamcore 最大 670,530 tri（Nanite）で、ロード 92 ms + デコード 593 ms + 溶接 118 ms + 書き出し 31 ms = **872 ms**、ピーク 288 MB。
- **スループット**: デコードのみ約 **0.39〜1.1 M tri/s**（メッシュごとの固定費を含む全 2,854 個で 0.39 M tri/s、大きいメッシュほど速く約 1.1 M tri/s）。全 2,854 個のデコードは 43.6 s。
- **メモリ**: 約 **300 B / 三角形**（(288 − 89 MB) / 670,530 tri。CUE4Parse が頂点ごとにオブジェクトを作る・全ページを一度に保持するため）。ワーキングセット上限 8 GB では **約 2,500 万 tri / メッシュ**が限界。設計書が想定する「1 億 tri」級（Megascans の 1 アセット 500〜2000 万 tri など）は、**ページ単位でクラスタを取り出して逐次書く**実装（P6 本体、下の §7）が要る。VGSRC の書き手は既に**配列を順に流す**作りにしてある（ヘッダは最後に書き戻し、`sourceHash` は書きながら FNV-1a 64）。
- **列挙**: StaticMesh 12,115 個の走査は 19〜24 s（`LoadPackage` + StaticMesh の読み出し）。マウントは 0.15〜0.9 s。

---

## 6. 今回わかっていないこと（憶測しない）

| 項目 | 状況 |
|---|---|
| **UE 5.8 の Nanite** | 実データ未検証（Dreamcore は 5.6）。CUE4Parse には 5.7 / 5.8 の分岐がある（下の §6.1）。5.8 の cook 済みアセットで `ue_cook --ue 5.8 --list-nanite --decode` を実行して、**`leafTris == inputTris` が全数で成り立つか**を確認するのが最初の一手 |
| **マテリアルの MASK / BLEND 判定** | 未検証。`.usmap` にバージョン情報が無く `MaterialInstanceConstant` が読めなかった（§2.3）。ゲームを起動して `jmap` で usmap を取り直すと読める（過去の Dead Mall 抽出では読めていた）。VG / NONVG の振り分け（設計書 §未決 4 / R13）に必要 |
| **テクスチャの BC パススルー** | 未着手（設計書 P6 (a)）。フォーマットは UE が cook 済みの BC7 / BC5 / BC1（`UTexture2D` の `FTexturePlatformData`）。CUE4Parse の `Decode` は RGBA へ展開してしまうので、**生の BC ブロックを取る経路**を選ぶ |
| **配置（`WorldDto` → シーン JSON）** | 既存（Dead Mall 抽出）の延長。今回は触っていない |
| **フォールバックだけのメッシュ**（Nanite ではないが密なもの）| 非 Nanite の LOD0 は最大 138,405 tri。`--extract` で取れる |
| **スキニング / ボクセル / カーブ / アセンブリ** | Dreamcore には無い。CUE4Parse は一部対応（`bVoxel` スキップ、スキン付き Nanite の書き出しは 2026-07 に追加）。P6 の対象外（静的メッシュのみ）|

### 6.1 UE 5.8 との関係（CUE4Parse のコード上の差分。実データでの確認は未）

| 版 | Nanite の変更点（`FNaniteResources` / `FCluster` / `FFixupChunk` / `FHierarchyNodeSlice`）|
|---|---|
| 5.4 | 位置精度の範囲・ページあたりクラスタ数のビット数・接線 / 頂点カラーのエンコードの変更 |
| 5.5 | 材質エンコード、`bHasTangents` / `bSkinning` / `NumUVs` / `ColorMode` の位置、ボクセル用 `ExtendedData` / `BrickData` |
| 5.6 | `NumInputMeshes` / `NumInputTexCoords` が消え、クラスタごとの `NumUVs` に。`AssemblyTransforms` / `MeshBounds` が追加。頂点数のビット幅が 9 → 14 |
| **5.7** | `PageDependencies` が `u32 → u16`、`VoxelMaterialsMask` 追加、`DecodeInfoOffset` がページヘッダからクラスタ ディスクヘッダへ、`AssemblyBoneAttachmentData` / `PageRangeLookup` 追加、`FFixupChunk` / `FHierarchyNodeSlice` の拡張 |
| **5.8** | `BoneIndices` 追加、`ImposterAtlas` 削除、`NumInputCurves` 追加、クラスタにカーブ用フィールドと `ParentLODError` / `ParentSphereBoundRadius`（half）、`FHierarchyNodeSlice` にも 5.8 分岐 |

- NuGet `1.2.2.202609`（2026-09-01）のバイナリに 5.7 / 5.8 のフィールド（`NumInputCurves` / `BoneIndices` / `VoxelMaterialsMask` / `PageRangeLookup`）が**入っている**ことを確認済み。つまり**コード上は 5.8 を読む準備がある**が、「実際に読めた」とは言えない。
- **静的 usmap 生成（`static_usmap.py`）は 5.6 の params 配置が前提**。5.8 の cook 済みプロジェクトを読むなら、(a) ゲーム / プロジェクトの exe から作り直す（配置が変わっていれば再検証）、または (b) その exe を一度起動して `jmap` を使う。エディタ側の UE 5.8 プロジェクト（`Documents\Unreal Projects\AIMCPTEST`）は未 cook。cook 済み 5.8 アセットを作れば同じ手順で確認できる。

---

## 7. 推奨する P6 の進め方

1. **縮退案（A: フォールバック / B: エディタ側エクスポート → assimp）は採らない**。Nanite 葉クラスタをそのまま VGSRC へ出す（本スパイクで実証）。エディタ側エクスポート（縮退案 B）は、cook 済みアセットが手に入らない**自作 UE プロジェクト**用の別ルートとして残す。
2. **cook 対象**: (i) **Nanite 有効の StaticMesh は葉クラスタ**、(ii) **非 Nanite の StaticMesh は LOD0**（LOD1 以降は捨てる。仮想ジオメトリが自前で LOD を作る）、(iii) **諦めるもの**: スキニング・ボクセル・カーブ・アセンブリ・Nanite 以外の特殊メッシュ（スプライン等は別途）。
3. **実装の順序**（残り約 6〜7 日。§9）:
   1. `ue_cook` の本実装化（プロトタイプは `--extract` / `--extract-all` まで動いている）: **ページ単位のストリーム抽出**（1000 万 tri 超のメッシュ用）、材質ごとに別ファイルへ出すか 1 ファイルに載せるかの決定（VGSRC は 1 メッシュ 1 ファイル、セクション = 材質）。
   2. **usmap**: ゲームを 1 回起動して `jmap` で取り直す（数秒。**ユーザー操作が要る**）。マテリアル・配置が読めるようになる。静的 usmap は「起動しないで StaticMesh だけ読める」保険として残す。
   3. **マテリアル**: `MaterialInstanceConstant` → 親をたどって `BlendMode`（MASK 判定）/ テクスチャパラメータ / スカラー（roughness・metallic）→ `MaterialRecord`。**MASK / BLEND のセクションは `sectionKind = 1`（NONVG）**。テクスチャは cook 済み BC を DDS へパススルー。
   4. **配置**: `WorldDto`（既存）→ シーン JSON。負スケールは配置側で処理（memory `dreamcore-dead-mall`）。
   5. E2E は P1 の cooker が揃ってから（`.vgeo` 経由で描画して glTF 経路と画素比較。設計書 P6 合否 2）。
4. **P1 への申し送り（cooker が面倒を見る）**: 位置 + UV での頂点溶接（Nanite はクラスタ境界で重複）、**面積ゼロ三角形の除去**（全体 0.5%、多いメッシュで 2%）、量子化は VGEO の 24bit 格子（Nanite の格子より細かいので劣化なし）。
5. **P0 / 設計書への修正**（§4.2）: 巻き順の規則を 1 箇所に決める（UE は内向き）。`--emit-vgsrc --ue` の見本を実データに合わせる。

---

## 8. リスクと撤退条件の更新

| # | リスク | 旧 | 更新 |
|---|---|---|---|
| **R3** | UE の Nanite cook 済みアセットをフルレゾで取れない | 最大リスクの 1 つ（P6 冒頭スパイクで判定）| **解消（Dreamcore 5.6 で 2,854 / 2,854 個が欠損 0）**。縮退案 A/B は撤回。**残る形**: 5.8 の Nanite 形式差分（**未検証**）|
| R3b（新）| UE 5.8 の cook 済み Nanite で欠損が出る | — | 撤退条件: 5.8 の実アセットで `leafTris != inputTris` が**全 Nanite メッシュの 1% 超**、または CUE4Parse が 5.8 のページを読めない → CUE4Parse の更新待ち（月単位で追随されている）を 1 サイクル待ち、それでも駄目なら縮退案 B（エディタ側エクスポート）|
| R3c（新）| 1000 万 tri 超のメッシュでメモリ不足 | — | 現実装は約 300 B/tri（上限 約 2,500 万 tri）。ページ単位ストリーム抽出で解決（実装済みの VGSRC ストリーム書きと組み合わせる）。撤退条件: 1 億 tri 級で 8 GB に収まらない → CUE4Parse の頂点型を struct 化する自前デコーダ（`FNaniteVertex` を使わない）|
| R3d（新）| usmap の入手 | — | ゲームを 1 回起動して `jmap`（確実）。起動できない環境では静的 usmap（StaticMesh 系のみ）。ゲーム更新で usmap は作り直し |
| R13 | 植生（MASK）が支配的なシーンで見劣り | UE 資産の棚卸し（P6）| **未検証**（マテリアル未読）。棚卸しは usmap 取り直し後 |

---

## 9. 工数見積りの更新

設計書 P6: **10 日（スパイク 3 + 実装 7）**。

| 項目 | 旧 | 新 |
|---|---:|---:|
| スパイク（Nanite 可否）| 3 日 | **実績 約 1 日相当**（可否確定 + プロトタイプ + 全数検証）|
| メッシュ + VGSRC 出力（ストリーム抽出・一括変換）| 含む | 1.5 日（プロトタイプ済みの本実装化）|
| usmap 取り直し + マテリアル（MASK 判定・PBR 値）+ テクスチャ BC パススルー | 含む | 3 日 |
| 配置 → シーン JSON | 含む | 1 日 |
| E2E 検証（P1 待ち）| 含む | 1 日 |
| **合計** | **10 日** | **約 7.5 日**（スパイク 1 + 実装 6.5）。縮退案の設計 / 実装が不要になった分が減る。5.8 検証は別途 0.5 日（実アセット待ち）|

P6 は P0 だけに依存（E2E の最後だけ P1 待ち）。**クリティカルパスには載らない**（変更なし）。

---

## 10. 再現手順

```
# ビルド
$env:PATH = "C:\Users\ryuto\.dotnet10;$env:PATH"; cd tools\ue_cook; dotnet build -c Release

# usmap（ゲーム起動なし・StaticMesh 系のみ）
python scripts\static_usmap.py "<...>\Dreamcore\Binaries\Win64\Dreamcore-Win64-Shipping.exe" out\Dreamcore_static.usmap

# 一覧 / 全数デコード
dotnet bin\Release\net10.0\ue_cook.dll --paks "<...>\Dreamcore\Content\Paks" --usmap out\Dreamcore_static.usmap --list-nanite --only-nanite --decode --csv out\nanite.csv --top 20

# 1 メッシュ → VGSRC → 検証
dotnet bin\Release\net10.0\ue_cook.dll --paks ... --usmap ... --extract Dreamcore/Content/Models/Gaturro/texturedMesh --out out\g.vgsrc
pwsh tools\build.ps1 -Target vgeo_stub
build\release\tools\vgeo_stub\vgeo_stub.exe --validate-vgsrc out\g.vgsrc
python tools\ue_cook\scripts\check_vgsrc.py out\g.vgsrc        # 巻き順・符号付き体積
```

詳細と全オプションは `tools/ue_cook/README.md`。
