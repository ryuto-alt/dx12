# tools/ue_cook — UE の cook 済み StaticMesh を VGSRC へ（P6 冒頭スパイクのプロトタイプ）

- 位置づけ: 仮想ジオメトリ計画 **P6（UE アセット cook）の冒頭スパイク**（`docs/UE_COOK_SPIKE.md`）。UE 5.x の cook 済みパッケージ（`.pak` / `.utoc`）から
  **Nanite のフル解像度三角形（葉クラスタ = LOD0 相当）**を取り出し、`docs/VGEO_SPEC.md` §12 の **VGSRC v1.0** へ書く最小プロトタイプ。
- **既定のビルド / ctest には含まれない**（CMake はこのフォルダを見ない）。C#（.NET 10）+ CUE4Parse（NuGet）。
- **ファイル読み出しだけ**。ゲームも UE エディタも起動しない。重い処理は `BelowNormal` 優先度で動く。メモリ上限 8 GB（`--mem-limit-mb`）。
- **このリポジトリは PUBLIC。UE / Dreamcore 由来の実アセット（メッシュ・テクスチャ・`.usmap`・出力 `.vgsrc`・CSV）は絶対にコミットしない。**
  `.gitignore` に `*.usmap` / `*.vgsrc` / `tools/ue_cook/out/` を入れてある。出力は ignore 済みの場所（`tools/ue_cook/out/` やスクラッチパッド）へ。

## 必要なもの

| もの | 場所 / 入手 |
|---|---|
| .NET 10 SDK | `C:\Users\ryuto\.dotnet10`（ローカル導入済み。最新 CUE4Parse は net10.0 必須）|
| CUE4Parse / CUE4Parse-Conversion | NuGet `1.2.2.202609`（`UeCook.csproj`。復元は自動）|
| `.usmap`（型マッピング）| UE5.6 の cook 済みパッケージは unversioned properties なので **必須**。下の「usmap の作り方」|
| Python 3 + `capstone`（usmap を exe から作る場合のみ）| `pip install capstone` |

## ビルド

```
$env:PATH = "C:\Users\ryuto\.dotnet10;$env:PATH"
cd tools\ue_cook
dotnet build -c Release          # → bin\Release\net10.0\ue_cook.dll
```

## 使い方

```
dotnet bin\Release\net10.0\ue_cook.dll --paks "<...>\Content\Paks" --usmap Dreamcore.usmap [--ue 5.6] <コマンド>
```

| コマンド | 内容 |
|---|---|
| `--list-nanite [--filter S] [--only-nanite] [--decode] [--top N] [--csv F]` | StaticMesh を列挙。Nanite 有無・ページ数・クラスタ数・**入力三角形数**・フォールバック LOD0 三角形数・材質数。`--decode` で葉クラスタを全デコードして三角形数 / 時間も出す（Dreamcore 全 2,854 個で 70 s）。 |
| `--info PACKAGE` | 1 メッシュの詳細。`PACKAGE` はパス（`Dreamcore/Content/.../SM_x`）か一意な部分一致。 |
| `--extract PACKAGE --out F.vgsrc` | 1 メッシュを VGSRC へ。Nanite なら葉クラスタ、そうでなければ通常 LOD0。 |
| `--extract-all DIR [--filter S] [--only-nanite]` | 該当する全 StaticMesh を `DIR` へ一括変換（所要時間・失敗一覧を出す）。 |

`--extract` のオプション:

| オプション | 既定 | 意味 |
|---|---|---|
| `--fallback` | 無効 | Nanite メッシュでも通常 LOD0（フォールバック）を出す |
| `--engine-coords` | 無効 | C# 側で `(x,y,z)_UE → (y,z,x)_engine × 0.01` に変換して `coordSystem=0` で出す。既定は UE 座標（cm）のまま `coordSystem=1`（cooker / reader が変換）|
| `--no-weld` | 溶接する | 位置・法線・UV がビット一致する頂点を畳まない |
| `--no-flip-winding` | 反転する | **既定は三角形の 2・3 番目の頂点を入れ替えて出す**（下の注意）|
| `--max-tris N` / `--force` | 6,000,000 | Nanite の入力三角形数の上限（メモリ保護）。`--force` で無視 |
| `--mem-limit-mb N` | 8192 | ワーキングセット上限。超えそうなら中止 |

### 検証（P0 の C++ リーダーで）

```
pwsh tools\build.ps1 -Target vgeo_stub                                 # build\release\tools\vgeo_stub\vgeo_stub.exe
vgeo_stub.exe --validate-vgsrc out\SM_x.vgsrc                          # ハッシュ込みの VGSRC 検証（参照実装 = ReadVgsrc）
python tools\ue_cook\scripts\check_vgsrc.py out\SM_x.vgsrc             # 巻き順・符号付き体積・法線の向き（VGSRC 検証では見ない項目）
```

### 注意（実測で分かったこと。詳細は `docs/UE_COOK_SPIKE.md`）

- **UE の cook 済み三角形は `cross(p1-p0, p2-p0)` が「内向き」**（`Engine/BasicShapes/Cube` で符号付き体積 −1、頂点法線とは全三角形で逆向き）。
  VGEO_SPEC C26 / §12.5 の「UE の三角形をそのまま出せばよい」とは食い違うため、**このツールは既定で 2・3 番目を入れ替えて外向きにして出す**。
  `vgeo_stub --emit-vgsrc --ue` の見本は raw で外向きなので、実データとは向きが違う。
- Nanite は**クラスタ境界で頂点が重複**する（Gaturro: 469,201 → 溶接後 373,704）。cooker（P1）は位置 + UV で溶接すること。
- Nanite の葉に**面積ゼロの三角形**が含まれる（全体 0.51%、`SM_Sofa08` は 7,626 / 478,010 = 1.6%）。P1 で除くこと。
- 材質は**名前だけ**（`スロット名|マテリアル名`）を出す。テクスチャ・PBR 値・MASK 判定は未実装（P6 本体）。UV は UV0 のみ。

## usmap の作り方

`.usmap` は UE5.6 の cook 済みパッケージ（unversioned properties）を読むのに必須。2 通り。

1. **ゲームを 1 回起動して `jmap` で吸う（確実・全クラス）**: `trumank/jmap` の `jmap_dumper --pid <ゲームのPID> Dreamcore.usmap`（数秒。memory `dreamcore-dead-mall`）。
   マテリアル・配置（`WorldDto`）を読むならこちらが必要。
2. **ゲームを起動せずに exe から静的に作る（StaticMesh 系のみ検証済み）**: `scripts/static_usmap.py`。

   ```
   python scripts\static_usmap.py "<...>\Binaries\Win64\Dreamcore-Win64-Shipping.exe" out\Dreamcore_static.usmap --dump out\dump.txt
   ```
   exe 内の UHT 生成リフレクション表（`Statics::PropPointers` / `FStructParams` / `FEnumParams`）を読み、`StaticMesh` / `StreamableRenderAsset` と依存 struct・enum だけを書く。
   `--class NAME:SUPER:PROP[,PROP]`（PROP = そのクラスだけが持つプロパティ名。配列の特定に使う）で対象クラスを足せる。
   **UE 5.6 の構造体配置を前提**（プロパティ params のオフセット、`FStructParams` 等）。5.8 の exe では配置が変わる可能性があるので、再検証してから使うこと。
   **usmap のバージョン情報（カスタムバージョン表）を書かない**ので、`MaterialInstance` 等でネイティブ直列化が絡む型（`MaterialOverrideNanite`）は読めない（未対応）。
