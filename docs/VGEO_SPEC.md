# `.vgeo` / VGSRC 仕様（確定版 v1.0）

- 対象: 仮想ジオメトリ（Nanite 風）の**ファイル形式の契約**。P1（オフライン cooker）と P6（UE cook, C#）を並列に進めるために固めた。
- 位置づけ: `docs/VIRTUAL_GEOMETRY_DESIGN.md` の §3（`.vgeo`）と §3.8（VGSRC）を精査し、曖昧・矛盾・実装不能な点を潰した**確定版**。設計書自体は書き換えていない。差分は §1「設計書からの変更点」に全部ある。
- 実装: `src/renderer/vg/VgeoFormat.h`（構造体・Reader・Writer・Validator）/ `VgeoBvh.h`（BVH ビルダ）/ `VgsrcFormat.h`（VGSRC）。**食い違ったらこの文書が正**。ヘッダの `static_assert` が §4〜§11 の全オフセットを固定している。
- テスト: `tests/vgeo_format_test.cpp`（形式・破損・境界・決定論・手書き DAG）/ `tests/vgeo_stub_test.cpp`（スタブ生成器）。金型ハッシュ（`kGoldenHandDag`）が「バイト列が変わった」ことを検知する。 cooker は `tests/vgeo_cook_test.cpp`（クラックなし・誤差の単調性・カバレッジ・決定論・GPU 写像・回帰・不正入力）。
- 状態: P0 完了（形式・スタブ）/ **P1 完了（オフライン cooker `tools/vgeo_cook`。階層 LOD・クラックなしを検証済み。§16.2）**。エンジン本体（描画・ローダ）には未統合（P2）。
- 表記: 「**必須**」= 違反したら Validator がエラーにする。「**推奨**」= しなくてもエラーにはならない。オフセットは特記なき限り**構造体先頭からのバイト**。

---

## 1. 設計書からの変更点

| # | 設計書の記述 | 確定した内容 | 理由 |
|---|---|---|---|
| C1 | コード置き場 `src/vgeo/`、`docs/VGEO_FORMAT.md` | `src/renderer/vg/`（ヘッダオンリー）、`docs/VGEO_SPEC.md` | 依頼どおり。`Renderer` ライブラリに何も足さず、ヘッダを include するだけにした |
| C2 | `PageTableEntry.rawSize`（常に 131072） | **`pageCrc32`**（展開後 131072 B 全体の CRC32）に置換。`SectionEntry(PAGES).crc32` は 0 | rawSize は `pageSize` と常に同値で冗長。一方ページ単位のチェックサムが無いとストリーミング（P5）で破損を検出できない |
| C3 | `PageHeader` の 36..63 は reserved | 36 に **`levelMax`** を追加（残り reserved 24 B） | `PageTableEntry` と対称にし、ページ単体で検証できるように |
| C4 | `ClusterHeader` のフィールド順（lodSphere / lodError / parentLodError / parentLodSphere / cullSphere / …） | **並べ替え**: 前半 64 B = カリングが読む値（float4 境界に 3 本: `lodSphere`@0 / `parentLodSphere`@16 / `cullSphere`@32、続いて `lodError`@48 `parentLodError`@52 `coneS8`@56 `maxEdgeLength`@60）、後半 64 B = ラスタ/シェーディング用（§9.4） | 元の並びだと `parentLodSphere` が 16B 境界に乗らない。K2 のホットパスを 16B 整列の `Load4` 3 本 + 1 本にできる |
| C5 | `ClusterHeader` の reserved 12 B | 116 に **`childGroup`**（生んだグループの packed = `page[0:16)|firstCluster[16:24)|clusterCount[24:28)`）を追加（残り reserved 8 B）。`childPage` は GPU の高速経路用に残し、`childGroup` の下位 16bit と一致が必須 | 設計書は「自分を生んだグループ」をページ単位でしか持たず、DAG の親子リンクをクラスタ単位で検証できなかった。これで「(lodSphere, lodError) = 子グループの (parentLodSphere, parentLodError)」を機械検査できる |
| C6 | 「グループ = 葉が指す、消費されるクラスタの集合」。ルートクラスタは消費されないため所属グループが未定義 | **ルートクラスタも「ルートグループ」に属する**（`parentLodError = +INF` を共有するグループ。どのクラスタの `childGroup` からも参照されない）。ルートクラスタの `parentLodSphere` は自分の `lodSphere` | BVH の葉がルートクラスタを指せないと、最粗レベルを描けない。LOD0 のみのスタブは全クラスタがルート |
| C7 | 「同じグループに消費されたクラスタは同一の `(parentLodSphere, parentLodError)`」 | **ルートグループでは `parentLodSphere` を共有しなくてよい**（`parentLodError = +INF` だけ共通）。それ以外のグループは従来どおり両方共有（ビット一致）| ルートの球は各クラスタ自身の `lodSphere`（LOD0 スタブでは全員別）。誤差が +INF なので射影は常に無限大で球は使われない |
| C8 | グループのサイズ・ページ内個数の上限が未定義 | グループ 1〜**15** クラスタ（`clusterCount` が 4bit）、1 ページ最大 **256** クラスタ（`clusterIndexInPage` / `firstCluster` が 8bit）、最大 **65535** ページ（`pageIndex` が 16bit）、材質 ≤ 65536、level 0..254（8bit。`levelCount ≤ 255`）。グループはページを跨がず、ページ内の**連続範囲**で、ページ全体を隙間なく覆う | 設計書のビット割り当てから導かれる上限を明文化 |
| C9 | 「グループ番号」の付け方が未定義 | **group id = `(pageIndex, firstCluster)` 昇順の順位**（0 起点）。葉の `ref` にその番号を書く | 葉から group を引く表を持たないため、id をページ内の位置から決定的に決める。BVH の並びとは独立（`BvhLeaf.groupId` で指定） |
| C10 | 「ノード 0 が根。子の並びに意味は無い」 | **子ノードの番号は必ず親より大きい**（幅優先番号を推奨）。根以外の全ノードがちょうど 1 回参照される。空きスロットは `ref = kNone` + 残りゼロ埋め。全スロットが空のノードは禁止 | ループ・孤立ノードを構造的に禁止し、GPU 走査の終了性を保証 |
| C11 | スタブは「BVH 1 ノード」 | **スタブも実際の 4 分木 BVH を作る**（Morton 順の 4 個束ね）| 数千〜数百万クラスタで P2 のカリング負荷試験ができるように |
| C12 | `HierChild.lodSphere` = 「部分木の `parentLodSphere` 群を内包する球」 | 変更なし。ただし**必須**として明記: メンバーの `parentLodSphere` ⊇ `lodSphere` なので、`lodSphere` は両方を内包する。内部ノードの子は「子ノードの各子の `lodSphere`」を内包 | 設計書の §2.3.2 の枝刈り（最近点・最遠点）が成り立つ条件 |
| C13 | LOD0 の `lodSphere` は `cullSphere` と「同値」 | **ビット一致が必須**（`lodError == 0` も必須）| Validator で機械検査するため |
| C14 | `PAGE_DEPS`: 「1 つ粗いレベルのグループを含むページ」 | `deps[Q] = { P : ページ P のクラスタ d の childPage == Q かつ P ≠ Q }`（昇順・重複なし・**自己依存なし**）。**依存は必ず小さいページ添字へ向く**（粗いレベルのページが先）。`priority` = 依存の無いページを 0 とした依存の深さ。`SectionEntry.count` = 依存総数、`size = 4 * (pageCount + 1 + count)` | 循環を禁止し、ローダが添字順にロードできる。writer / validator が同じ関数（`DeriveDepsFromHeaders`）で再計算する |
| C15 | `pinnedPageCount` = 「ルート側のページ先頭から連続」 | 先頭 `pinnedPageCount` ページが pinned（`PageTableEntry.flags` / `PageHeader.flags` の bit0 と一致が必須）。**ルートクラスタを含むページは全部 pinned** | 何も描けない状態を作らないための保証 |
| C16 | `flags`: bit0..2 を定義、bit3..31 予約 | **下位 16bit = 必須機能**（未知のビットが立っていたらローダは拒否）、**上位 16bit = 任意ヒント**（無視してよい）。`bit1/bit2` は `proxySectionCount / nonVgSectionCount > 0` と一致が必須 | 拡張の互換性ルール（§2.6） |
| C17 | `flags.bit0`（ページ圧縮）を v1 で使う可能性 | v1 の writer は**圧縮を出さない**。reader は圧縮ファイルを受け付けるが、展開関数（`ReadOptions::decompress`）が無ければ `UnsupportedFeature` | header-only / 標準ライブラリ縛りのため XPRESS を内蔵できない。展開はエンジン側の `vfs/Compression` を関数で渡す |
| C18 | 位置格子: `posStep = max(extent) / 16777215`（丸めが未定義）、量子化式なし | `posStep` は f32 に丸めて `posStep * 16777215 < extent` なら **`nextafter` で 1ulp 上げる**（端の頂点が格子に収まる）。extent = 0 なら `posStep = 1.0`。量子化は `q = clamp(floor((p - origin) / step + 0.5), 0, 16777215)`（**double 演算**、格納済みの f32 origin/step を使う）。復元 `origin + (f32)q * step` | 実装間で 1 ビットもずれない量子化にする（クラック防止の前提） |
| C19 | `posOrigin = aabbMin` | **ビット一致が必須**。`aabbMin/Max` は量子化前の元頂点の境界 | 検証可能にするため |
| C20 | セクションの空表現・整列が曖昧 | **空セクション = offset/size/count 全部 0（stride は公称値）**。非空セクションは 4096 整列・昇順・非重複。ファイル末尾は 4096 に切り上げ（ゼロ詰め）。`PAGE_DEPS` は `pageCount == 0` のとき空。`PROXY/NONVG` の stride は 0 | writer/reader の一意性 |
| C21 | `PROXY` の `vertexOffset` は「セクション先頭から」だが基準が曖昧 | **PROXY/NONVG セクション先頭（= `ProxyHeader` の 0 バイト目）からのバイト**。データは正準に詰める（エントリ表の後、各セクションの「頂点(16 整列) → インデックス(16 整列)」を順に、全体 16 整列） | 一意なバイト列にするため |
| C22 | `ProxySectionEntry.error`: 「NONVG は 0」／`flags.bit0` = 「NONVG 由来の完全詳細」 | **`error` = ソース形状からの最大偏差（PROXY / NONVG とも）**。**`flags.bit0 = Exact`**（簡略化していない完全詳細 = `error == 0`）。`header.proxyError` = PROXY エントリの `error` の最大 | NONVG は「20 万 tri へ簡略化した実描画用」なので誤差は 0 とは限らない。設計書内で矛盾していた |
| C23 | `MaterialRecord` の `alphaTest` / `blend` | **VG 材質（`sectionKind == 0`）は `alphaTest` / `blend` を立てられない**（MASK・半透明は NONVG）。PROXY は `sectionKind == 0` の材質だけ、NONVG は `== 1` だけを参照する | v1 の対象外境界（未決 4）を形式で強制 |
| C24 | STRINGS の規約が簡素 | 空でなければ **先頭 1 byte は `'\0'`（offset 0 = 空文字列）**、末尾も `'\0'`、UTF-8 として正しい。テクスチャパスは**相対パスのみ**（`'/'` 区切り・絶対パス / ドライブ / `..` / 空要素 / `\` を禁止）。参照 offset は文字列の先頭を指すこと | 平置き `.vgeo`（未署名）を読むローダのパストラバーサル対策 |
| C25 | ヘッダの `sourceHash` の定義なし | **入力バイト列の FNV-1a 64**。VGSRC 由来なら VGSRC の `sourceHash`（§12）をそのまま写す。`cookParamsHash` = cook パラメータの正準文字列 `key=value;…`（キー昇順）の FNV-1a 32 | 再 cook 判定を実装間で一致させる |
| C26 | 座標系の巻き順の規約なし | **`cross(p1 - p0, p2 - p0)` が外向き（前面側）**。法線コーンの軸はこの外向き法線の平均（`meshopt_computeMeshletBounds` と同じ）。UE → エンジンの `(x,y,z)→(y,z,x)` は巡回置換なので巻き順を変えない | クラスタの背面カリングとメッシュシェーダの巻き順が食い違う事故を防ぐ |
| C27 | VGSRC: `SectionRecord.firstIndex` の単位、欠損属性の扱い、UV 原点、ハッシュ範囲が未定義 | **firstIndex は indices[] の要素番号**（バイトではない）。セクションは indices[] を先頭から隙間なく覆う。`hasNormals=0` なら cooker が位置から滑らかな法線を作る、`hasUV0=0` なら (0,0)。**UV 原点は左上（D3D。V が下）**。`sourceHash` = バイト 64..EOF の FNV-1a 64。`coordSystem` は 0/1 のみ。ヘッダは最後に書き戻してよい（ストリーム書き）（§12）| P6（C#）が実装できる粒度にするため |
| C28 | 実測サイズ 12.5〜13 B/tri（LOD0） | 合成メッシュ（岩・約 128 tri/クラスタ）の実測は **約 14.5 B / LOD0 三角形**（ページのみ）。全 DAG では約 2 倍 = 約 29 B/tri（[推定]）| §16。5000 万 tri の VRAM 見積もり（設計書 §2.4.5）は約 +15% で見直す |
| C29 | （P0 の未決）「非ルートグループは、そこから生まれた親クラスタが 1 つ以上必要」だが、簡略化でグループが丸ごと消える場合が未定義 | **確定: 最低 1 三角形は残す**。cooker は簡略化の結果が 0 三角形なら、入力のうち最大面積の三角形を 1 枚残し、誤差に「グループのジオメトリの境界球半径」を足す。Validator の規則（§10-5）は緩めない | 「消えたら親なし」を許すと、その領域を親が覆えず、粗い LOD を選んだ瞬間に穴（= クラック）になる。1 枚残せば親クラスタは必ず 1 個以上でき、誤差の加算で「そこは粗すぎる」と実行時に判定される |
| C30 | ルートグループは 1 つ（C6）| **ルートグループは複数あってよい**（各 1〜15 クラスタ）。cooker は ① VG セクション（材質）ごとに別の DAG を持つ ② 材質境界（共有頂点の永久ロック）や `lockOpenBorders` のロックが支配的でこれ以上簡略化できないとき、**そのレベルをそのままルートにする**（15 クラスタごとにルートグループを分ける）。`rootClusterCount` は 1〜2 とは限らない（複数材質で数個〜十数個）| 形式は元から複数ルートを許す（Validator は「INF の共有」だけ見る）。明文化して、cooker が「簡略化できない入力」で失敗せず有効なファイルを出せるようにする |
| C31 | ページ順は「レベルの降順」（§8.3）| **確定: ルートグループ → レベル降順 → グループ中心の Morton コード → 生成順**。group id = この順位（BVH の葉も同じ順）。`pinnedPageCount` = max(ルートを含む最後のページ + 1, min(pageCount, `pinPages`（既定 64 = 8 MiB）))。深さの違う DAG（材質ごと）が混ざっても、依存は必ず小さいページ添字へ向く（子を生んだグループのレベルは親より 1 小さい）| ルートを含むページが先頭に固まる（pinned の条件）。レベル帯 → Morton の並びで BVH の LOD 枝刈りが効く |

---

## 2. 共通規約

### 2.1 エンディアンと整列
- **リトルエンディアン固定**。構造体は `#pragma pack(1)`（`static_assert` でサイズとオフセットを固定）。
- 全セクションの先頭は **4096 B 整列**（無バッファ IO のセクタ境界）。ページは 131072 B 固定長で、PAGES セクションも 4096 整列なので**全ページが 4096 整列**。
- ファイルを構造体へ読むときは必ず `memcpy`（未整列アクセスを作らない）。

### 2.2 座標系・単位・巻き順
- エンジン空間: **Y up・左手系・メートル**。エンティティのスケールは 1 で置ける。
- 三角形の巻き順は入力メッシュのまま。ただし `cross(p1 - p0, p2 - p0)` が**外向き**であること（§1 C26）。
- UE（Z up・左手系・cm）からの変換は VGSRC の `coordSystem = 1`（§12.4）。

### 2.3 数値表現
- 位置: アセット共通の 24bit 格子（§6.1）。法線: 八面体 oct16×2（§6.2）。UV: クラスタ局所 unorm16×2（§6.3）。
- `+INF` は必ず `0x7F800000`（`parentLodError` / `HierChild.maxParentError`）。NaN・-INF・負の誤差・負の半径は不正（`BadFloat`）。

### 2.4 チェックサムとハッシュ
| 用途 | アルゴリズム | 範囲 |
|---|---|---|
| ヘッダ CRC | CRC-32/IEEE（反射・初期値 0xFFFFFFFF・最終 XOR 0xFFFFFFFF。`"123456789"` → `0xCBF43926`）| ヘッダ 0..507 バイト → `headerCrc32`（508） |
| セクション CRC | 同上 | セクションの `size` バイト（パディングを含まない）→ `SectionEntry.crc32`。**0 = 未計算**（reader は検証を省く）。writer は PAGES 以外を必ず計算 |
| ページ CRC | 同上 | 展開後 131072 B 全体 → `PageTableEntry.pageCrc32`（**常に計算**。0 は「未計算」ではない）|
| `sourceHash` | FNV-1a 64（basis `0xcbf29ce484222325`, prime `0x100000001b3`）| 入力バイト列（§1 C25）|
| `cookParamsHash` | FNV-1a 32（basis `0x811C9DC5`, prime `0x01000193`）| パラメータ文字列 |

### 2.5 予約フィールドの扱い
- writer は**必ず 0**を書く（パディングも全部ゼロ）。
- reader（ローダ）は予約フィールドの値を**無視**する（将来の minor が使うため）。
- Validator の `strictReserved = true` のときだけ非 0 を `ReservedNotZero` にする。cooker は出力を必ず `strictReserved` で検査すること（`ValidateContent` は常に厳密）。

### 2.6 バージョニングと拡張方針
- `versionMajor`（互換を壊す変更で +1）: reader は**未知の major を拒否**（`UnsupportedMajor`）。major を見る前に他のフィールド（CRC 含む）を信用しない。
- `versionMinor`（互換を保つ追加）: reader は**より新しい minor でも読める**。minor で許される変更は次の 3 つだけ:
  1. 予約フィールド / パディングの利用
  2. `flags` の**上位 16bit（任意ヒント）**の追加（読み手が知らなくても正しく描ける情報）
  3. 新しいセクション（後述）。読み手が無視しても正しく描けるもの限定
- **必須機能**（無視すると描画が壊れる変更）は `flags` の下位 16bit に新ビットを割り当てる → 古い reader は `UnsupportedFeature` で拒否する。これは minor では**なく major を上げる**のが原則（下位ビットの追加を minor で許すのは「そのビットを立てたファイルは古い reader に拒否されてよい」場合のみ）。
- 新セクションの置き場: ヘッダ予約 `reserved1[0..12)` に**拡張テーブル**の位置（`u64 offset` + `u32 count`）を置く（v1.0 は 0 = 無し、reader は無視）。拡張エントリは `{fourcc, flags(bit0 = required), offset, size, crc32}`。required な未知エントリを持つファイルは古い reader が拒否する。
- 未知の値を「黙って通す」経路を作らない（未知の `clusterMaterialMode` 等はすべて `UnsupportedFeature`）。

---

## 3. ファイル全体

```
[0x0000] VgeoHeader                      512 B
         (0 詰めで 4096 まで)
[  ...] MATERIALS   section              4096 整列
[  ...] STRINGS     section
[  ...] NODES       section
[  ...] PAGE_TABLE  section
[  ...] PAGE_DEPS   section
[  ...] PAGES       section              ← 大半。ページ i は (PAGES.offset + i * 131072)
[  ...] PROXY       section
[  ...] NONVG       section
[  ...] DEBUG_JSON  section
         (末尾を 4096 の倍数までゼロ詰め)
```
- セクションは上の順に**昇順・非重複**で置く（空セクションは飛ばす）。writer は隙間を最小（4096 整列だけ）にする。reader は隙間を許す。
- 最小のファイルは 4096 B（ヘッダだけ。VG ジオメトリも材質も無い）。

---

## 4. `VgeoHeader`（512 B, offset 0）

| offset | size | 型 | 名前 | 内容 / 規則 |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `magic` | `"VGEO"` = `0x4F454756` |
| 4 | 2 | u16 | `versionMajor` | 1 |
| 6 | 2 | u16 | `versionMinor` | 0（writer）。reader は任意の値を許す |
| 8 | 4 | u32 | `headerSize` | 512 |
| 12 | 4 | u32 | `flags` | §1 C16。bit0 圧縮 / bit1 プロキシ有 / bit2 NONVG 有（bit1/2 は Writer が導出）/ 上位 16bit 任意 |
| 16 | 4 | u32 | `maxClusterVerts` | 3..128。全クラスタの頂点数の上限（cook が使った値）|
| 20 | 4 | u32 | `maxClusterTris` | 1..128 |
| 24 | 4 | u32 | `pageSize` | 131072 |
| 28 | 4 | u32 | `pageCount` | ≤ 65535 |
| 32 | 4 | u32 | `clusterCount` | 全レベル合計 |
| 36 | 4 | u32 | `groupCount` | BVH の葉の数（= ページ表の `groupCount` の総和）|
| 40 | 4 | u32 | `nodeCount` | |
| 44 | 4 | u32 | `levelCount` | = 最大レベル + 1（0 = 最も細かい）。≤ 255 |
| 48 | 8 | u64 | `sourceTriangleCount` | LOD0 の総三角形数（VG セクションのみ）|
| 56 | 8 | u64 | `sourceVertexCount` | |
| 64 | 12 | f32[3] | `aabbMin` | 量子化前の元頂点の境界（アセット空間）|
| 76 | 12 | f32[3] | `aabbMax` | |
| 88 | 16 | f32[4] | `boundingSphere` | xyz + r。ジオメトリ全体を包む。**必須**: ノード 0 の全子の `cullSphere` を内包 |
| 104 | 12 | f32[3] | `posOrigin` | 格子原点。**`aabbMin` とビット一致** |
| 116 | 4 | f32 | `posStep` | §6.1 |
| 120 | 4 | u32 | `materialCount` | ≤ 65536 |
| 124 | 4 | u32 | `proxySectionCount` | |
| 128 | 4 | u32 | `nonVgSectionCount` | |
| 132 | 4 | f32 | `proxyError` | PROXY エントリの `error` の最大（無ければ 0）。ランタイムがシャドウバイアス下限に使う |
| 136 | 4 | u32 | `rootNode` | 0 |
| 140 | 4 | u32 | `rootClusterCount` | `parentLodError = +INF` のクラスタ数 |
| 144 | 4 | u32 | `pinnedPageCount` | 先頭から数えた常駐固定ページ数 |
| 148 | 4 | u32 | `clusterMaterialMode` | 0（1 クラスタ 1 材質）。1 は v2 予約（v1 reader は拒否）|
| 152 | 8 | u64 | `sourceHash` | §2.4 |
| 160 | 4 | u32 | `cookParamsHash` | |
| 164 | 4 | u32 | `reserved0` | 0 |
| 168 | 32 | char[32] | `cooker` | NUL 終端**必須**。例 `"vgeo_stub 1.0.0 meshopt 1.0"` |
| 200 | 288 | SectionEntry[9] | `sections` | 32 B × 9 |
| 488 | 20 | u8[20] | `reserved1` | 0（将来: 拡張テーブルの位置）|
| 508 | 4 | u32 | `headerCrc32` | 0..507 の CRC32 |

**カウントの整合（必須）**: `clusterCount == 0` ⇔ `pageCount = groupCount = nodeCount = levelCount = rootClusterCount = pinnedPageCount = 0`。`clusterCount > 0` なら全部 ≥ 1、`clusterCount ≤ pageCount * 256`、`groupCount ≤ clusterCount ≤ groupCount * 15`、`groupCount ≤ 4 * nodeCount`（葉スロットの数。悪意ある巨大カウントでの確保を防ぐ）、`rootClusterCount ≤ clusterCount`、`1 ≤ pinnedPageCount ≤ pageCount`。`clusterCount > 0` のとき `aabbMin ≤ aabbMax`、`posOrigin == aabbMin`、`posOrigin + posStep * 16777215 ≥ aabbMax`。

### 4.1 `SectionEntry`（32 B）
`u64 offset` / `u64 size` / `u32 count` / `u32 stride` / `u32 crc32` / `u32 reserved`。

| 添字 | セクション | `count` | `stride`（固定）| `size` の規則 | 空になる条件 |
|---:|---|---|---:|---|---|
| 0 | MATERIALS | `materialCount` | 96 | `count * 96` | `materialCount == 0` |
| 1 | STRINGS | = `size` | 1 | バイト数（< 4 GiB）| 任意 |
| 2 | NODES | `nodeCount` | 192 | `count * 192` | `nodeCount == 0` |
| 3 | PAGE_TABLE | `pageCount` | 32 | `count * 32` | `pageCount == 0` |
| 4 | PAGE_DEPS | **依存総数** | 4 | `4 * (pageCount + 1 + count)`（CSR: `u32 offsets[pageCount+1]` の後に `u32 deps[]`）| `pageCount == 0` |
| 5 | PAGES | `pageCount` | 131072 | `count * 131072`（無圧縮）| `pageCount == 0` |
| 6 | PROXY | `proxySectionCount` | 0 | §11 | `proxySectionCount == 0` |
| 7 | NONVG | `nonVgSectionCount` | 0 | §11 | `nonVgSectionCount == 0` |
| 8 | DEBUG_JSON | = `size` | 1 | バイト数（実行時は読まない。決定的であること: 時刻・所要時間を入れない）| 任意 |

---

## 5. 座標・属性の量子化

### 5.1 位置格子（全クラスタ共通・クラック防止の要）
```
extent   = max(aabbMax[a] - aabbMin[a])                    // 軸ごとの差の最大（double）
posStep  = (f32)(extent / 16777215.0)
if ((double)posStep * 16777215.0 < extent) posStep = nextafterf(posStep, +inf)
if (extent == 0) posStep = 1.0f
posOrigin = aabbMin                                        // ビット一致
q[a]  = clamp(floor(((double)p[a] - (double)posOrigin[a]) / (double)posStep + 0.5), 0, 16777215)   // 全部 double
p'[a] = posOrigin[a] + (float)q[a] * posStep               // f32。復元
```
- 量子化誤差は最大 `posStep / 2`（元の f32 丸めと同程度）。**同じ頂点はどのクラスタでも同じ `q` → 同じ float**（シェーダが同じコードで復元する限り）。
- クラスタは自分の `posMin`（格子単位、0..16777215）と軸ごとのビット幅 `bx, by, bz`（各 0..24）を持ち、頂点は `q - posMin` を詰める。writer は**最小ビット幅**（`bit_width(max - min)`）を使う。reader は 24 以下なら許す。
- `q` は 24bit なので `bx+by+bz ≤ 72`。

### 5.2 法線（oct16×2）
- 4 B/頂点: 下位 16bit = x（snorm16）、上位 16bit = y（snorm16）。`snorm16 = round_half_away(clamp(v, -1, 1) * 32767)`。
- 符号化: `p = n / (|nx|+|ny|+|nz|)`。`nz < 0` なら `(px, py) = ((1 - |py|) * sign(px), (1 - |px|) * sign(py))`（`sign(0) = +1`）。零ベクトルは +Z。
- 復号: `x = s16/32767`（下限 -1）、`y` 同様、`z = 1 - |x| - |y|`。`z < 0` なら `(x, y) = ((1 - |y|) * sign(x), (1 - |x|) * sign(y))`。正規化。角度誤差 ≤ 0.05°（テストで確認）。

### 5.3 UV
`uv = uvBase + (u16, v16) / 65535 * uvScale`（クラスタ局所）。`uvBase` = クラスタ内の最小、`uvScale` = 最大 - 最小（0 なら 16bit は 0）。符号化は `round(clamp((uv - base) / scale, 0, 1) * 65535)`。誤差 ≤ `uvScale / 65535 / 2`。UV は縫い目で頂点が分かれるので、クラスタ間で一致させる必要は無い。**原点は左上（D3D）**。

### 5.4 法線コーン
`coneS8` = `axis.x, axis.y, axis.z`（s8、バイト 0..2）+ `cutoff`（s8、バイト 3）。`/127` で復元。**-128 は不正**。`cutoff = 127` は「コーン無効（カリングしない）」。背面棄却（透視）:
```
cull = dot(center - camera, axis) >= cutoff * length(center - camera) + radius      // center/radius = cullSphere
```
ミラー（行列式 < 0）のインスタンスでは軸の符号を反転する。`meshopt_computeMeshletBounds` の `cone_axis_s8` / `cone_cutoff_s8` をそのまま入れる（-128 だけ -127 に丸める）。

---

## 6. MATERIALS / STRINGS

### 6.1 `MaterialRecord`（96 B）
| offset | size | 型 | 名前 | 備考 |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `nameOff` | STRINGS 内。`kNone(0xFFFFFFFF)` = 名前無し |
| 4 | 4 | u32 | `flags` | bit0 hasNormalMap / bit1 hasMetalRough / bit2 alphaTest（NONVG 専用）/ bit3 hasEmissiveTex / bit4 doubleSided / bit5 blend（NONVG 専用）。bit6.. 予約 |
| 8 | 4 | u32 | `albedoPathOff` | 相対パス（§1 C24）。`kNone` = 無し |
| 12 | 4 | u32 | `normalPathOff` | 同上 |
| 16 | 4 | u32 | `metalRoughPathOff` | G = roughness, B = metallic（`Material.h` と同じ）|
| 20 | 4 | u32 | `emissivePathOff` | |
| 24 | 4 | f32 | `metallic` | 有限 |
| 28 | 4 | f32 | `roughness` | |
| 32 | 12 | f32[3] | `emissiveColor` | |
| 44 | 4 | f32 | `emissiveIntensity` | |
| 48 | 4 | f32 | `alphaCutoff` | |
| 52 | 4 | f32 | `baseColorAlpha` | |
| 56 | 16 | f32[4] | `baseColorFactor` | 記録するが v1 のランタイムは無視（`ModelLoader` と同じ挙動）|
| 72 | 16 | f32[4] | `uvScaleOffset` | xy = scale, zw = offset（既定 1,1,0,0）|
| 88 | 4 | u32 | `sectionKind` | 0 = VG / 1 = NONVG。それ以外は不正 |
| 92 | 4 | u32 | reserved | |

- テクスチャは BC7（albedo = sRGB, MR = linear）/ BC5（normal）の `.dds`（ミップ付き）。パスは **`.vgeo` のあるフォルダ基準**の相対パス。
- 全フィールドは有限であること。VG 材質（`sectionKind == 0`）は `alphaTest` / `blend` 不可。

### 6.2 STRINGS
UTF-8 の NUL 区切りプール。空でなければ先頭は `'\0'`、末尾も `'\0'`。writer は重複を畳む（`StringPool`。挿入順に offset を振るので決定的）。

---

## 7. NODES（4 分木 BVH）

### 7.1 `HierChild`（48 B）と `HierNode`（192 B = 4 子）
| offset | size | 型 | 名前 | 内容 |
|---:|---:|---|---|---|
| 0 | 16 | f32[4] | `cullSphere` | 部分木の**全ジオメトリ**を包む球（アセット空間）。錐台 / HZB 用 |
| 16 | 16 | f32[4] | `lodSphere` | メンバーの `parentLodSphere`（⊇ 自身の `lodSphere`）を包む球。LOD 枝刈りの誤差投影用 |
| 32 | 4 | f32 | `minOwnError` | メンバー `lodError` の最小 |
| 36 | 4 | f32 | `maxParentError` | メンバー `parentLodError` の最大（`+INF` あり）|
| 40 | 4 | u32 | `ref` | `0xFFFFFFFF` = 空 / bit31 = 1: **葉**（下位 31bit = group id）/ bit31 = 0: 子ノード番号（アセット相対）|
| 44 | 4 | u32 | `groupPacked` | 葉のみ: `page[0:16) | firstCluster[16:24) | clusterCount[24:28)`（上位 4bit は 0）。内部ノードでは 0 |

**集約値（必須。Validator が全部検査する）**
- 葉（グループ G）: `cullSphere ⊇` 各メンバーの `cullSphere` / `lodSphere ⊇` 各メンバーの `parentLodSphere` / `minOwnError == min(メンバー.lodError)` / `maxParentError == max(メンバー.parentLodError)`（**ビット一致**。選んだ値そのものなので丸めが入らない）。
- 内部（子ノード N）: `cullSphere ⊇` N の各非空子の `cullSphere` / `lodSphere ⊇` 各子の `lodSphere` / `minOwnError == min`、`maxParentError == max`（子の値の min/max）。
- 球の包含判定の許容: `dist(centers) + r_inner ≤ r_outer + 1e-4 * max(r_outer, r_inner) + 4 * posStep`。
- `0 ≤ minOwnError ≤ maxParentError`、NaN 不可。

### 7.2 構造規則
1. ノード 0 が根。**子ノードの番号 > 親の番号**、根以外の全ノードがちょうど 1 回参照される、全ノードに 1 つ以上の非空の子。
2. group id は `(pageIndex, firstCluster)` 昇順の順位（0..groupCount-1 が 1 回ずつ）。各ページのグループは `firstCluster = 0, count_0, count_0 + count_1, …` と**ページ内の全クラスタを隙間なく敷き詰める**（1 グループ 1..15 クラスタ）。ページ表の `groupCount` と一致。
3. 空スロットは `ref = 0xFFFFFFFF`、他は全部 0。
4. ノード数の目安: 葉 G 個に対し約 `G / 3`。

### 7.3 葉のグループ = 「描画候補の集合」
葉が指すグループ = **そのグループに消費される（入力となる）クラスタの集合**。ルートクラスタは「ルートグループ」に入る（§1 C6）。GPU の描画条件は設計書 §2.3.2 のまま:
```
draw = (ProjectedErrorPx(parentLodError, parentLodSphere) > τ) && (ProjectedErrorPx(lodError, lodSphere) <= τ || 子ページ未常駐)
```

### 7.4 BVH の作り方（P1 向け）
- `VgeoBvh.h` の `BuildBvh4(leaves)` は、渡された葉の順に **4 個ずつ束ねて下から作り**、根から幅優先で番号を振り直す（決定的）。集約値は上の規則どおりに計算する。
- 葉の並びは「空間的にまとまった順」（Morton 順）。**LOD 枝刈り（`minOwnError` / `maxParentError`）を効かせたい実 cooker は、葉を先にレベル帯（同じレベルのグループ）で並べ、各帯を Morton 順にしてから渡す**（帯の境界を 4 の倍数に揃えると下位ノードが帯を跨がない）。 P1 の cooker はこの順（ルートグループ → レベル降順 → Morton。C31）で渡すが、帯の境界を 4 の倍数へ揃える詰め物はしていない（葉の数は groupCount で固定のため）。帯を跨ぐノードが帯ごとに高々 3 個ずつできるだけで、正しさには影響しない。
- `BvhLeaf.groupId` は §7.2-2 の順位（ページ詰め後に確定）。葉の並びの添字とは独立に指定できる。

---

## 8. ページ

**ページ = 131072 B 固定長の自己完結ブロック = ストリーミング（P5）の最小単位。** ページ内に「クラスタヘッダ + 頂点 / 三角形ブロック」が全部入り、他ページへの参照はクラスタの `childPage` / `childGroup` だけ。グループはページを跨がない。

### 8.1 `PageTableEntry`（32 B）
| offset | size | 型 | 内容 |
|---:|---:|---|---|
| 0 | 8 | u64 | `fileOffset`（無圧縮: `PAGES.offset + i * 131072`）|
| 8 | 4 | u32 | `storedSize`（ディスク上のバイト数。無圧縮 = 131072）|
| 12 | 4 | u32 | **`pageCrc32`**（展開後 131072 B の CRC32）|
| 16 | 4 | u32 | `clusterCount`（1..256）|
| 20 | 4 | u32 | `groupCount`（1..clusterCount）|
| 24 | 4 | u32 | `levelMin[0:8) | levelMax[8:16) | flags[16:32)`（flags bit0 = pinned）|
| 28 | 4 | u32 | `priority`（§1 C14）|

### 8.2 ページ本体
```
[  0 ..  63]  PageHeader (64 B)
                 0 u32 magic = "VGPG" (0x47504756)   4 u32 pageIndex     8 u32 clusterCount   12 u32 groupCount
                16 u32 clusterTableOffset = 64      20 u32 payloadOffset = 64 + 128 * clusterCount
                24 u32 usedBytes（最後のブロックの終わり。以降 131072 までゼロ）   28 u32 levelMin   32 u32 flags(bit0=pinned)
                36 u32 levelMax    40 u8[24] reserved
[ 64 .. ]     ClusterHeader[clusterCount]   (各 128 B)
[payloadOffset ..]  クラスタごとに [頂点ブロック][三角形ブロック] を連続（各ブロックは 16 B 整列）
```
- ペイロードは**隙間なく連続**（正準）: クラスタ i の `vertexOffset` = 直前の三角形ブロックの終わり（最初は `payloadOffset`）、`triangleOffset = vertexOffset + vertexBlockSize`、最後の終わり = `usedBytes`。
- ページに入る個数: 1 ページ最大 256 クラスタ。最大クラスタ（128 頂点・128 三角形・72bit 位置）は 2688 B なので 48 個入る（ヘッダ 128 B 込み）。

### 8.3 依存表（`PAGE_DEPS`）とロード順
- ページ Q（細かい側）を GPU へ載せてよいのは、`deps[Q]`（粗い側のページ群）が**すでに常駐**しているとき。こうすると「子ページが未常駐なら親を描く」フォールバックが常に成立し、穴が空かない。
- 依存は**必ず小さい添字へ**向くので、ページを添字順にロードすれば依存は自動的に満たされる（ストリーミングは優先度順でも `deps` を見て待つ）。
- ページ順の作り方（P1 向け）: グループを**レベルの降順（ルート側が先）**に並べてから、`PageBuilder::TryAddGroup` で先頭から詰める。1 ページに複数レベルが混ざってよい（同じページ内の依存は自己依存として除外）。
- **材質（セクション）ごとに別の DAG** を持つ場合（v1 は 1 クラスタ 1 材質）: `level` は各 DAG の中での深さ（0 = LOD0）。グループは全 DAG ぶんを 1 本の BVH・1 つのページ列に入れる。全 DAG をまとめて「レベル降順」に並べれば `deps` の向き（小さい添字へ）は保たれる（依存は同じ DAG の中にしか生じない）。`levelCount` = 全 DAG の最大レベル + 1、`rootClusterCount` = 全 DAG のルート数の合計。
- **pinned**: 先頭 `pinnedPageCount` ページは常駐固定。ルートクラスタを含むページは全部 pinned（LOD0 のみのアセットは全ページ pinned = 退避不可。粗い代替が無いので当然）。

---

## 9. `ClusterHeader`（128 B）と頂点 / 三角形ブロック

### 9.1 ClusterHeader（ページ先頭から `64 + 128 * i`）
**前半 64 B = カリング（K2）が読む値**（float4 境界に整列）、後半 64 B = ラスタ / シェーディング用。

| offset | size | 型 | 名前 | 内容 |
|---:|---:|---|---|---|
| 0 | 16 | f32[4] | `lodSphere` | このクラスタを**生んだ**グループの LOD 球（LOD0 は `cullSphere` とビット一致）|
| 16 | 16 | f32[4] | `parentLodSphere` | このクラスタを**消費した**グループの LOD 球（ルートは `lodSphere` とビット一致）|
| 32 | 16 | f32[4] | `cullSphere` | クラスタのジオメトリを包む球（`meshopt_computeMeshletBounds`。**量子化後の頂点**から計算すること）|
| 48 | 4 | f32 | `lodError` | LOD0 = 0。絶対値（アセット空間 m）|
| 52 | 4 | f32 | `parentLodError` | ルート = `+INF`。`≥ lodError` |
| 56 | 4 | u32 | `coneS8` | §5.4 |
| 60 | 4 | f32 | `maxEdgeLength` | 最長辺（アセット空間）。SW/HW 振り分けの画面辺長推定に使う |
| 64 | 4 | u32 | `vertexOffset` | 頂点ブロックの位置（ページ先頭から、16 整列）|
| 68 | 4 | u32 | `triangleOffset` | = `vertexOffset + vertexBlockSize` |
| 72 | 4 | u32 | `packedCounts` | `vertexCount[0:8) | triangleCount[8:16) | materialIndex[16:32)`。vertexCount ∈ [1, maxClusterVerts]、triangleCount ∈ [1, maxClusterTris]、材質は `sectionKind == 0` |
| 76 | 4 | u32 | `posBits` | `bx[0:5) | by[5:10) | bz[10:15)`、上位は予約（0）。各 ≤ 24 |
| 80 | 12 | i32[3] | `posMin` | 格子単位（0..16777215）|
| 92 | 4 | u32 | `flags` | bit0 = LOD0 / bit1 = root / `[8:16)` = level。bit2..7 と bit16.. は予約 |
| 96 | 8 | f32[2] | `uvBase` | |
| 104 | 8 | f32[2] | `uvScale` | ≥ 0 |
| 112 | 4 | u32 | `childPage` | 自分を**生んだ**グループのページ。LOD0 = `kNone` |
| 116 | 4 | u32 | `childGroup` | 同グループの packed（`page[0:16)|first[16:24)|count[24:28)`）。LOD0 = `kNone`。下位 16bit は `childPage` と一致 |
| 120 | 8 | u32[2] | reserved | 0 |

**フラグの整合（必須）**: `lod0 ⇔ level == 0 ⇔ childPage == kNone ⇔ childGroup == kNone`。`root ⇔ parentLodError == +INF`。`level < levelCount`。

### 9.2 頂点ブロック（`vc` = vertexCount, `bpp = bx + by + bz`）
```
posBytes = align16( ceil(vc * bpp / 8) )
[vertexOffset]                    位置ストリーム: ビット詰め（LSB ファースト）。頂点 i は bit 位置 i * bpp から x, y, z の順に bx, by, bz ビット
[vertexOffset + posBytes]         法線ストリーム: u32[vc]（oct16×2、§5.2）      ← 実サイズ align16(4 * vc)
[... + align16(4 * vc)]           UV ストリーム: u32[vc]（下位 16bit = u, 上位 16bit = v、§5.3） ← 実サイズ align16(4 * vc)
vertexBlockSize = posBytes + 2 * align16(4 * vc)
```
- 位置ストリームの各頂点は最大 72bit。GPU が 2〜3 dword を読んでも、直後に法線ストリーム（≥ 16 B）が続くのでページ内に収まる。

### 9.3 三角形ブロック（`tc` = triangleCount）
バイト `3t + {0, 1, 2}` = 三角形 t の 3 頂点のローカル番号（0..vc-1）。サイズ `align16(3 * tc)`。巻き順は §2.2。縮退三角形（同じ番号を 2 つ含む / 面積 0）は cooker が除くこと（**推奨**。Validator は許容する）。

### 9.4 サイズの計算例
頂点 70・`bpp = 39`・三角形 110 → 位置 352 B / 法線 288 B / UV 288 B = 頂点ブロック 928 B、三角形 336 B、ヘッダ 128 B → **1392 B / 110 tri ≈ 12.7 B/tri**。

---

## 10. DAG 不変条件（Validator が全部検査する）

記号: クラスタ c の `lodError = e(c)`、`parentLodError = E(c)`、`lodSphere = S(c)`、`parentLodSphere = P(c)`、`level = L(c)`。グループ G の 子クラスタ集合（G から生まれたクラスタ d が指す `childGroup` のメンバー）。

1. **誤差の単調性**: `E(c) ≥ e(c)`（等号可）。LOD0 は `e = 0`。
2. **球の包含**: 非ルートの c は `P(c) ⊇ S(c)`（許容 §7.1）。ルートは `P(c) == S(c)`（ビット一致）。LOD0 は `S(c) == cullSphere(c)`（ビット一致）。
3. **グループ内一致**（グループ = 葉が指す消費集合）: 全メンバーが同じ `E`（ビット一致）と同じ level と同じ root フラグを持つ。非ルートグループのメンバーは同じ `P`（ビット一致）。
4. **親子リンク**: 非 LOD0 のクラスタ d の `childGroup` は実在するグループ G を指し（`(page, firstCluster)` が一致し `clusterCount` も一致）、G の全メンバー c について `L(c) + 1 == L(d)`、`E(c) == e(d)`（ビット一致）、`P(c) == S(d)`（ビット一致）。
5. **グループの被参照**: 非ルートグループ（`E` が有限）はちょうど 1 つ以上のクラスタから `childGroup` で参照される。ルートグループは参照されない。
6. **依存表**: `PAGE_DEPS` = クラスタの `childPage` から再導出した値と一致（§1 C14）。`priority` も一致。
7. **ヘッダ集計**: `rootClusterCount` = root フラグの数、`levelCount = 最大 level + 1`、ページの `levelMin/Max` は中身と一致、ルートを含むページは pinned。
8. **BVH 集約値**（§7.1）。

画面誤差 `ProjectedErrorPx(parent) ≥ ProjectedErrorPx(child)` が全視点で成り立つのは 1 と 2 から導かれる（近距離クランプ込みでもテストで確認: `TestHandDag`）。

---

## 11. `PROXY` / `NONVG` セクション

同一レイアウト（**セクション先頭からのバイト**）:
```
[  0] ProxyHeader 32 B: u32 sectionCount, u32 totalVertices, u32 totalIndices, u8[20] reserved
[ 32] ProxySectionEntry[sectionCount] 各 64 B
[..] 各セクション: 頂点（16 整列・vertexCount * 96 B）→ インデックス（16 整列・indexCount * 4 B）… 全体 16 整列
```
`ProxySectionEntry`（64 B）: `u32 materialIndex`@0 / `u32 vertexCount`@4（≥ 3）/ `u32 indexCount`@8（> 0、3 の倍数）/ `u32 vertexOffset`@12 / `u32 indexOffset`@16 / `u32 flags`@20（bit0 = Exact）/ `f32 error`@24（≥ 0）/ `f32 aabbMin[3]`@28 / `f32 aabbMax[3]`@40 / `u32 reserved[3]`@52。

- 頂点は **`ProxyVertex`（96 B）= `renderer/Mesh.h` の `Vertex` と同一レイアウト**（position 0 / normal 12 / color 24 / texCoord 40 / tangent 48 / boneIndices 64 / boneWeights 80）。`Mesh::Initialize(device, vertices, indices, cmd)` にそのまま渡せる（エンジン側で `sizeof(Vertex) == 96` を `static_assert` すること）。
- **PROXY** = VG の代表（RT / 影 / ピッキング / 物理 / VG 無効時の描画）。上限 25 万 tri。`materialIndex` は VG 材質。
- **NONVG** = MASK / BLEND セクションを予算内（既定 20 万 tri）へ簡略化した実描画用。`materialIndex` は NONVG 材質（`alphaTest` / `blend`）。
- 位置は有限、インデックスは `vertexCount` 未満、`aabb` は有限で min ≤ max、`ProxyHeader` の合計はエントリの合計と一致（必須）。

---

## 12. VGSRC（中間形式 v1.0）

UE cook / Blender などから cooker へ渡す入力。glTF は 4 GB 制限と速度で 1 億 tri に向かないため、**単純な生配列**にする。書き手は「ヘッダを空で書く → 配列を順に流す → 先頭へ戻ってヘッダを確定」でストリーム書きできる（C# なら `MemoryMappedFile` / `FileStream.Seek`）。実装: `VgsrcFormat.h`。

### 12.1 ヘッダ（64 B）
| offset | size | 型 | 名前 | 内容 |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `magic` | `"VGSR"` = `0x52534756` |
| 4 | 2 | u16 | `versionMajor` | 1 |
| 6 | 2 | u16 | `versionMinor` | 0 |
| 8 | 4 | u32 | `flags` | bit0 hasNormals / bit1 hasUV0 / bit2 hasTangents（読み飛ばす）/ bit3 hasColors（読み飛ばす）。bit4.. は不正 |
| 12 | 4 | u32 | `materialCount` | ≤ 65536 |
| 16 | 4 | u32 | `sectionCount` | |
| 20 | 4 | u32 | reserved0 | 0 |
| 24 | 8 | u64 | `vertexCount` | ≤ 2^32 - 1（インデックスが u32）|
| 32 | 8 | u64 | `indexCount` | 3 の倍数 |
| 40 | 4 | f32 | `unitScaleToMeters` | 有限・> 0。座標に掛ける（cm なら 0.01）|
| 44 | 4 | u32 | `coordSystem` | 0 = エンジン（Y up・左手・m）/ 1 = UE（Z up・左手・cm）。それ以外は不正 |
| 48 | 8 | u64 | `sourceHash` | バイト 64..EOF の FNV-1a 64。0 = 未計算 |
| 56 | 8 | u64 | reserved1 | 0 |

### 12.2 本体（ヘッダの後、各配列の先頭は 16 B 整列。位置はカウントとフラグだけで決まる: `ComputeVgsrcLayoutUpToStrings`）
```
positions   f32[3] * vertexCount
normals     f32[3] * vertexCount      （hasNormals のとき）
uv0         f32[2] * vertexCount      （hasUV0 のとき）
indices     u32    * indexCount       （三角形リスト）
sections    VgsrcSection * sectionCount   各 16 B: u32 materialIndex, u32 firstIndex, u32 indexCount, u32 flags
materials   MaterialRecord * materialCount   各 96 B（§6.1 と同一。パス文字列は下の文字列プールの offset）
strings     u32 byteLength + UTF-8 バイト列（先頭 1 byte は '\0'、末尾も '\0'）
```
- ファイルはここで終わる（末尾のゴミは不正）。文字列プールは §6.2 と同じ規則。
- **セクション**: `firstIndex` は **indices[] の要素番号**（バイトではない）。セクションは `indices[]` を先頭から**隙間なく順に**覆い、`indexCount` は 3 の倍数で > 0、`materialIndex < materialCount`。`SectionRecord.flags` bit0 = 強制 NONVG（カスタムシェーダ等）。それ以外の VG / NONVG の振り分けは材質の `alphaTest` / `blend` フラグから cooker が決める。
- **欠損属性**: `hasNormals = 0` なら cooker が位置から**面積重みの滑らかな法線**を作る（位置で溶接して平均）。`hasUV0 = 0` なら (0,0)。tangent / color は読み飛ばす（v1 は持たない）。
- **UV の原点は左上（D3D。V が下）**。UE の UV はそのまま、glTF 由来は書き手が変換する。
- **法線・位置は有限**（NaN / Inf は不正）。法線は単位長でなくてもよい（cooker が正規化する）。

### 12.3 検証（`ReadVgsrc`）
magic / major / 未知フラグ / coordSystem / unitScale / 頂点数上限 / indexCount % 3 / カウントがファイルサイズを超えない / 文字列プールがファイル末尾ぴったり / （`verifyHash`）ハッシュ / 位置・法線・UV が有限 / インデックス < vertexCount / セクションが indices を隙間なく覆う / 材質・文字列 offset の妥当性。

### 12.4 座標変換
`coordSystem = 1`（UE: X 前, Y 右, Z 上・左手系）→ エンジン: **`(x, y, z)_UE → (y, z, x)_engine`**、そのあと位置に `unitScaleToMeters` を掛ける。法線は同じ置換のみ（スケールなし）。**巡回置換なので手系も巻き順も変わらない**（`ConvertPositionToEngine` / `ConvertNormalToEngine`。テスト済み）。`coordSystem = 0` のときも `unitScaleToMeters` は掛ける。C# 側で変換して `coordSystem = 0` で出してもよい。

### 12.5 P6（C#）への注意
- 1 億 tri: 頂点 5000 万 × 32 B（位置 + 法線 + UV）+ インデックス 3 億 × 4 B ≈ 2.8 GB。u64 で数え、配列ごとに順に流す（全体をメモリに載せない）。
- 巻き順は `cross(p1 - p0, p2 - p0)` が外向きになる向きで出す（UE の三角形をそのまま出せばよい。§2.2）。
- `sourceHash` は最後に計算して先頭へ書き戻す（またはまとめて 0 のままでもよい。cooker は 0 なら自前で入力ハッシュを取る）。

---

## 13. 検証規則とエラー処理

### 13.1 検証の段階（`ValidateVgeo`。手前の段階に問題があれば後段は飛ばす）
| 段階 | 内容 | 主なエラー |
|---|---|---|
| 1. ヘッダ | 16 B で magic → major → headerSize（**CRC より前**）→ 512 B 読み → CRC → 構造（上限・カウント・浮動小数・フラグ・セクション表の範囲 / 整列 / 順序 / 形）| `Truncated` `BadMagic` `UnsupportedMajor` `BadHeaderSize` `HeaderCrcMismatch` `BadPageSize` `LimitExceeded` `UnsupportedFeature` `CountMismatch` `BadFloat` `SectionRange` `SectionAlign` `SectionShape` |
| 2. 小セクション | 読み込み + セクション CRC → STRINGS / MATERIALS / PAGE_TABLE / PAGE_DEPS の形 / BVH のツリーとグループの敷き詰め | `SectionCrcMismatch` `StringInvalid` `MaterialInvalid` `PageInvalid` `DepsInvalid` `NodeInvalid` `GroupInvalid` |
| 3. ページ | 全ページの CRC → ページヘッダ → 全クラスタのヘッダ（範囲・フラグ・浮動小数・ブロックの正準配置）→ （`deep`）全頂点をデコードして格子・AABB・cullSphere・三角形番号を検査 → 末尾ゼロ | `PageCrcMismatch` `PageInvalid` `ClusterInvalid` `BadFloat` `MaterialInvalid` `LodInvariant` |
| 4. DAG / BVH | §10 の 4〜8 | `LodInvariant` `DepsInvalid` `CountMismatch` |
| 5. プロキシ | セクション CRC + 形 + 有限性 + `proxyError` | `SectionCrcMismatch` `ProxyInvalid` |

### 13.2 ローダの規則（P2 向け）
- **1 つでも外れたらそのアセットを拒否**（プロキシも出さず、エンティティは「読み込み失敗」と同じ扱い + ログ）。部分的にロードしない。
- 通常ロードは `LoadMeta`（段階 1〜2。1000 万 tri で約 2 ms・読むのは先頭の数 MB だけ）+ ページを `ReadPage` で読む（`verifyCrc` は開発ビルドで ON、配布では任意）。`ValidateVgeo`（全ページ走査）は cooker の出力検査・CI・診断用で、通常ロードでは使わない。
- 圧縮ページ（v1 writer は出さない）は `ReadOptions::decompress` を渡す。無ければ `UnsupportedFeature`。
- ログは `VgeoError::ToString()`（コード名 + 説明 + index / offset）。

### 13.3 writer の規則（cooker 向け）
- 出力は**必ず `ValidateContent`（strictReserved）を通してから**書く（`WriteVgeoToFile` の前に）。
- 決定論: 同じ入力・同じパラメータなら**バイト一致**。パディングはゼロ、時刻・乱数・スレッド順を入れない（並列はグループ単位の結果を**最後に決定的な順序で結合**する）。`DEBUG_JSON` にも時刻・所要時間を入れない。
- Writer は `pages`（連続バッファ）を丸ごと受け取る。1 億 tri（約 2.5 GB）を超える出力にはストリーミング書き出し（`ByteSink` は逐次書きなので、ページを逐次供給する API を足すだけ）を P1 で追加すること。

---

## 14. P2 向け: GPU へのアップロード写像

**方針: v1 は「ファイルの形をそのまま GPU へコピー」する。SoA への再配置はしない**（ページは不変のストリーミング単位。再配置するとページ単位の差分アップロードと衝突する）。カリングの実測でヘッダ読み出しが律速になったら、ページロード時に「クラスタカリング表」（48 B/クラスタの SoA）を別バッファへ派生させる（P5 でページ単位で生成 / 破棄できる）。

| ファイルの領域 | GPU での行き先 | 形 | 加工 |
|---|---|---|---|
| NODES | `VgNodes`（アセットごとに連結）| `StructuredBuffer<HierNode>`（192 B）**AoS** | なし。子ノード番号・`groupPacked.page` は**アセット相対**なので、シェーダが `assetNodeBase` / `assetPageBase` を足す |
| PAGES | `VgPagePool` のスロット | `ByteAddressBuffer`（DEFAULT）。1 ページ = 131072 B のスロット | なし（無圧縮）。全常駐モード: PAGES セクションを連続でアップロードし `VgPageTable[assetPageBase + i] = slotBase + i`。ストリーミング: 1 ページずつ `CopyBufferRegion` |
| PAGE_TABLE / PAGE_DEPS | CPU（ストリーマ）| — | GPU が読む `VgPageTable`（u32/ページ: スロット or `0xFFFFFFFF`）は CPU が作る |
| MATERIALS | `VgMaterials`（`VgMaterialGpu` 64 B）| `StructuredBuffer` | CPU で SRV ブロックを確保して詰め替え（設計書 §2.4.1）|
| ヘッダ | `VgAssets`（`VgAssetGpu` 64 B）| `StructuredBuffer` | `posOrigin` / `posStep` / `boundingSphere` / `aabb` / `nodeBase` / `pageBase` / `materialBase` / `levelCount` を詰める |
| PROXY / NONVG | 通常の `Mesh` + `Material` | — | `ProxyVertex` → `Vertex`（同一 96 B）。`Mesh::Initialize` へそのまま |
| DEBUG_JSON | — | — | 実行時は読まない |

**HLSL 構造体（参考。未コンパイル）**
```hlsl
struct HierChild { float4 cullSphere; float4 lodSphere; float minOwnError; float maxParentError; uint ref; uint groupPacked; };  // 48 B
struct HierNode  { HierChild c[4]; };                                                                                             // 192 B
struct VgCluster {                                                        // ClusterHeader 128 B。ページ先頭 + 64 + 128 * i
    float4 lodSphere, parentLodSphere, cullSphere;                        // Load4 x3（16B 整列）
    float lodError, parentLodError; uint coneS8; float maxEdgeLength;     // Load4（前半 64B はここまで = カリング用）
    uint vertexOffset, triangleOffset, packedCounts, posBits;
    int3 posMin; uint flags;
    float2 uvBase, uvScale;
    uint childPage, childGroup;
};
```
**デコード（参考）**
```hlsl
uint LoadBits(ByteAddressBuffer pool, uint base, uint bitPos, uint n) {   // n <= 24, LSB ファースト。base はブロック先頭のバイト
    uint byteAddr = base + (bitPos >> 3);
    uint sh = (byteAddr & 3u) * 8u + (bitPos & 7u);                       // 0..31
    uint2 w = pool.Load2(byteAddr & ~3u);
    uint v = (w.x >> sh) | (sh != 0u ? (w.y << (32u - sh)) : 0u);
    return v & ((1u << n) - 1u);
}
float3 DecodePos(ByteAddressBuffer pool, uint pageBase, VgCluster c, uint i, float3 origin, float step) {
    uint3 b = uint3(c.posBits & 31u, (c.posBits >> 5) & 31u, (c.posBits >> 10) & 31u);
    uint bpp = b.x + b.y + b.z, base = pageBase + c.vertexOffset, bit = i * bpp;
    uint3 q = uint3(LoadBits(pool, base, bit, b.x), LoadBits(pool, base, bit + b.x, b.y), LoadBits(pool, base, bit + b.x + b.y, b.z)) + asuint(c.posMin);
    return origin + float3(q) * step;                                     // §5.1 の復元と同じ式（CPU 参照 = DequantizeCoord）
}
float3 DecodeOct(uint p) { float2 f = float2(int(p << 16) >> 16, int(p) >> 16) / 32767.0; f = max(f, -1.0);   // 下位 16bit = x, 上位 = y
                           float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
                           if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * (n.xy >= 0 ? 1.0 : -1.0); return normalize(n); }
uint3 ReadTriangle(ByteAddressBuffer pool, uint pageBase, uint triOff, uint t) {   // 4 バイト境界をまたぐので 2 dword から取り出す
    uint b = pageBase + triOff + t * 3u; uint2 w = pool.Load2(b & ~3u); uint sh = (b & 3u) * 8u;
    uint v = (w.x >> sh) | (sh != 0u ? (w.y << (32u - sh)) : 0u); return uint3(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF); }
int SignExtend8(uint x) { return int(x << 24) >> 24; }                    // coneS8 の軸 = SignExtend8((c.coneS8 >> (8*k)) & 0xFF) / 127.0
```
- **`clusterRef`**（可視リスト等）= `page[0:16) | clusterInPage[16:24)`（アセット相対。`MakeClusterRef`）。`HierChild.groupPacked` と `childGroup` は同じ packed。
- **`ByteAddressBuffer` のバイトアドレスは 32bit**: スロット数は 4 GiB / 131072 = 32768 が上限。プールが 4 GiB を超えるなら複数バッファに分ける（5000 万 tri 全常駐は約 1.3〜1.5 GB なので v1 は 1 本でよい）。
- 位置ストリームの全頂点読みはウェーブ内でコアレスしやすい（頂点 i の bit 位置は `i * bpp`）。`bpp` は実測で平均 50〜56 bit（岩、1M〜10M tri。1 軸 17〜19 bit）。

---

## 15. 実装ガイド（P1 / P6 の担当向け）

### 15.1 P1（cooker）が使うもの
1. 入力を読む: assimp（既存 `ModelLoader` と同じ単位正規化）または VGSRC（`ReadVgsrc`）。
2. LOD0 のクラスタ化、DAG（設計書 §2.3.1 の手順）。**量子化後の座標でクラスタ / 境界 / 簡略化を行う**（`QuantizeCoord` で全頂点を先に格子へ載せ、`DequantizeCoord` した float を meshopt へ渡す）。
3. クラスタ → `ClusterSource`（`q` は格子座標、`normalOct` は `EncodeOct16`、`uv`、`tri`）。`level`/`root`/`lodSphere`/`parentLodSphere`/`lodError`/`parentLodError`/`childPage`/`childGroup`/`coneS8`/`maxEdgeLength` を設定。**`childPage` / `childGroup` はページ詰めの後にしか決まらない**ので、DAG は「グループ ID」で組んでおき、ページ詰め後に「クラスタ → 生んだグループの (page, first, count)」を引いて `ClusterSource` を作る（または 2 パス: 一度詰めて位置を確定 → 参照を書き込んで詰め直す。ページ内の個数・サイズは参照値に依らないので詰め方は変わらない）。
4. グループをレベルの降順に並べ、`PageBuilder::TryAddGroup` で詰める（`FinishAppend(pages, pinned)`）。pinned は先頭ページ群（ルートを含むページは必須 + 予算内で次に粗いページ）。
5. group id を `(page, first)` 順位で確定 → `BvhLeaf` を作って `BuildBvh4`（§7.4）→ `VgeoContent` を埋める → `ValidateContent` → `WriteVgeoToFile`。
6. `header` の入力メタ（`VgeoContent` のコメント参照）: `maxClusterVerts/Tris`、`sourceTriangleCount/VertexCount`、`aabbMin/Max`、`boundingSphere`（**ノード 0 の子の cullSphere を内包**）、`posOrigin`（= aabbMin）、`posStep`（`ComputePosStep`）、`sourceHash`、`cookParamsHash`、`cooker`、`pinnedPageCount`。カウント・セクション表・CRC・`proxyError` は Writer が再計算する。

### 15.2 P1 が守る決定論
- クラスタ / グループ / ページの順序をスレッド数に依存させない（グループごとに独立に並列化し、結果をグループ番号順に結合）。
- meshoptimizer のバージョンを `cooker` 文字列に入れる（バージョンが違えば出力が変わってよい）。
- `DEBUG_JSON` に時刻を入れない。

### 15.3 P6（C#）が守ること
- VGSRC を §12 どおりに書く（座標変換・UV 原点・巻き順・セクションの被覆）。参考の C++ 実装は `VgsrcFormat.h`（`WriteVgsrc` / `ReadVgsrc`）。
- `.vgeo` は**書かない**（P1 の cooker が VGSRC から作る）。C# 側の検証は VGSRC の範囲・サイズ・三角形数まで。
- 突き合わせ: `vgeo_stub --emit-vgsrc x.vgsrc --kind sphere --tris 2000 --ue` が UE 座標（Z up・cm）の見本を出す。C# の書き手は自分の出力を `vgeo_stub --validate-vgsrc` で検査できる（C++ の `ReadVgsrc` = 参照実装）。

---

## 16. 実測（スタブ生成器・合成メッシュ）

`vgeo_stub --kind blob`（fBm 変位球、128 頂点 / 128 三角形、グループ 4、Release、Windows / RTX 5060 機の CPU）。LOD0 のみ。

| 三角形数 | クラスタ | ページ | 平均 tri/クラスタ | 平均頂点/クラスタ | ページ充填率 | ページのみ B/tri | ファイル（プロキシ込み）| 生成 | 検証（deep+CRC）| `LoadMeta` |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,002,000 | 7,844 | 111 | 127.7 | 85.5 | 97.6% | 14.5 | 28.5 MB | 0.9 s | 30 ms | 0.3 ms |
| 10,004,568 | 78,218 | 1,095 | 127.9 | 85.6 | 95.9% | 14.4 | 152.8 MB（プロキシ無し 138.1 MB）| 12 s（プロキシ簡略化 4.8 s）| 194 ms | 1.9 ms |

- 位置は平均 50〜56 bit/頂点。設計書の見積もり（12.5〜13 B/tri）より約 +12%。原因: 128 三角形クラスタでも頂点共有率が低い（約 0.5 頂点/三角形 → 平均 85 頂点/クラスタ）ため。全 DAG では約 2 倍 = 約 29 B/tri（[推定]。P1 で実測）。
- `LoadMeta`（ページを読まない）は 1000 万 tri でも約 2 ms（BVH 1.2 MB + ページ表 35 KB ほか）。全ページの検証（CRC + 深いデコード）でも 0.2 s。

---

### 16.1 ツールとテストの使い方
```
pwsh tools/build.ps1 -Target vgeo_stub                 # スタブ生成 CLI（既定ビルドには入らない）→ build/release/tools/vgeo_stub/vgeo_stub.exe
vgeo_stub --kind blob --tris 1000000 --out x.vgeo      # sphere | torus | grid | blob。--tris は目標（球は 4r(r-1) に丸まる）
vgeo_stub --validate x.vgeo [--no-deep] [--no-crc] [--strict]   # 検証と所要時間
vgeo_stub --info x.vgeo                                # ヘッダ要約 + 位置ビット数 / ページ充填率
vgeo_stub --emit-vgsrc s.vgsrc --kind sphere --tris 2000 --ue   # VGSRC の見本（--ue = UE 座標・cm）。P6 の C# 出力の突き合わせ用
vgeo_stub --validate-vgsrc s.vgsrc                     # VGSRC の検証（ハッシュ込み）
pwsh tools/build.ps1 -Tests                            # 全部 + ctest（VgeoFormatTests / VgeoStubTests）
pwsh tools/build.ps1 -Target VgeoFormatTestsAsan       # 同じテスト（ファズ込み）を AddressSanitizer で（実行は vcvars64 済みのシェルで。引数に数字 = ファズ反復数）
```
- `build.ps1 -Target` は 1 回に 1 ターゲット（複数は呼び出しを分ける）。

### 16.2 cooker（P1: `tools/vgeo_cook`）の使い方と実測

```
pwsh tools/build.ps1 -Target vgeo_cook                       # CLI（既定ビルドには入らない）→ build/release/tools/vgeo_cook/vgeo_cook.exe
vgeo_cook IN.vgsrc|IN.obj OUT.vgeo [--threads N] [--group-size G] [--reduction R] [--levels] [--curve] [--json F]   # cook + 書いたファイルの厳密検証
vgeo_cook --gen-bench blob|knot|rock|torus|sphere|grid --tris N [--seed S] [--materials M] --out F.vgsrc            # 巨大ベンチメッシュ（VGSRC）
vgeo_cook --gen-bench KIND --tris N --cook OUT.vgeo                                                                 # 生成してそのまま cook
vgeo_cook --sweep group|lod0|weights ...                                                                            # 方式の比較表
vgeo_cook --validate F.vgeo / --info F.vgeo
pwsh tools/vgeo_cook/gen_bench.ps1                           # 設計書 §5.2 の素材: 4 種 x 1250 万 tri → build/vg-bench/（.gitignore 済み）
pwsh tools/build.ps1 -Target VgeoCookTests                   # テスト（`-Tests` の ctest にも入る）。診断: -Target VgeoCookTestsAsan（AddressSanitizer）
```
- **PC を占有しない**: 既定は BelowNormal・論理コア / 4 スレッド（`--threads` は論理コアで頭打ち）。作業メモリが `--mem-limit-gb`（既定 10）を超えたら中止（終了コード 3）。
- 入力: VGSRC（`ReadVgsrc`）/ 開発用 OBJ（四角形・負の添字・usemtl。assimp は使わない）。出力は決定的（同じ入力・同じオプション → スレッド数に依らずバイト一致。1M / 10M で 1 スレッドと 7 スレッドの `cmp` 一致を確認）。

**アルゴリズム（実装: `VgeoCook.cpp`）**: ① 量子化（24bit 格子。量子化後の座標で以降を全部行う）→ 位置 + 法線 + UV が同一の頂点を融合 → 位置で溶接（同じ格子座標 = 同じ点）→ 縮退三角形（同じ点を 2 回含む / 面積 0）を除去 ② セクション（材質）ごとに三角形を Morton 順の空間チャンク（約 32k tri。スレッド数と無関係に固定）へ分け、チャンク内を `meshopt_buildMeshlets`（128 頂点 / 128 三角形、cone_weight 0.25）でクラスタ化 ③ 1 レベルごとに ［クラスタ隣接グラフ（共有境界エッジ）→ グループ化（既定 `meshopt_partitionClusters`、目標 6 クラスタ）→ グループごとに **グループ外周の溶接済み頂点をロック**して `meshopt_simplifyWithAttributes`（頂点を動かさない・法線 0.25 / UV 0.25・三角形数を 0.45 倍）→ `meshopt_buildMeshlets` で再クラスタ］ ④ 親の誤差 = max(子の誤差) + 今回の簡略化誤差、球 = `meshopt_computeSphereBounds(子の lodSphere)`（グループ内で共有）⑤ ルートが 1 クラスタになるまで繰り返す ⑥ ページ詰め（2 パス）・BVH（`BuildBvh4`）・プロキシ（250k tri 以下の最も細かいレベル）・NONVG。材質境界の頂点（複数セクションが共有）は全 LOD で永久ロック。

**実測**（Windows / 28 論理コア機。cook は 7 スレッド・BelowNormal。入力 = `--gen-bench blob`（fBm 球）を VGSRC で書いて読み込み。cook 時間 = 読み込み後〜メモリ上の `.vgeo` 完成まで（書き出し・検証は別）。既定パラメータ）:

| LOD0 三角形数 | 1 スレッド | 7 スレッド | ピーク作業メモリ | クラスタ（LOD0）| レベル | ページ B/tri | ファイル B/tri（プロキシ 25 万 tri 込み）| 書き出し + 厳密検証 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,002,000 | 1.77 s | **0.47 s** | 0.09 GB | 15,512（7,862）| 13 | 28.9 | 41.3（39.5 MB）| 36 + 39 ms |
| 10,004,568 | 23.7 s | **4.86 s** | 0.63 GB | 154,767（78,433）| 16 | 28.0 | 29.3（279 MB）| 0.32 + 0.46 s |
| 49,999,040 | （未測定）| **29.5 s**（生成込みの 1 回目は 34.4 s）| **2.96 GB** | 772,428（391,296）| 18 | 27.4 | 27.8（1,324 MB）| 1.7 + 2.5 s |

- 50M の内訳（ms）: 取り込み 4.0 / LOD0 クラスタ化 6.1 / DAG 17.7（グラフ 2.7・グループ化 3.3・簡略化 10.9）/ ページ詰め 1.0 / BVH 0.05 / プロキシ 0.05。処理速度は 1.7〜2.1 Mtri/s（7 スレッド）。設計書の合否（1000 万 tri ≤ 5 分・20 コア）に対して 6 秒。
- 作業メモリは 5000 万で 3 GB 弱（RAM 16 GB に十分収まる）。頂点を 24 B（格子座標 + oct 法線 + UV）で 1 本だけ持ち、全レベルのクラスタが同じ頂点配列を参照する（簡略化は頂点を動かさないため）。入力配列は量子化後すぐ解放する。
- サイズ: 全 DAG は LOD0 の約 2 倍の三角形を持つ（1 レベルで三角形が 0.45 倍 → 合計 1 / (1 - 0.45) = 1.8 倍）。ページのみで 27〜29 B / LOD0 三角形（設計書の見積もり 25、P0 の外挿 29）。5000 万 tri で 1.3 GB（設計書 §5.2 の VRAM 予算 1500 MB 以内）。1 段 0.5 倍だと 31.3 B/tri で予算を超えるため 0.45 を既定にした。

**簡略化の品質**（50M blob、レベルごと。誤差の単位 = メートル。blob の境界球半径 ≈ 1.2 m）:

| レベル | クラスタ | 三角形 | グループ | ロック頂点率 | 目標到達 | 簡略化誤差 平均 / 最大 | lodError（累積）平均 / 最大 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 391,296 | 49,999,040 | 60,637 | 24.6% | 99.9% | 4.6e-6 / 3.7e-4 | 0 / 0 |
| 3 | 42,576 | 4,535,590 | 6,587 | 40.1% | 99.8% | 6.4e-5 / 6.5e-4 | 4.9e-5 / 1.1e-3 |
| 6 | 3,907 | 411,760 | 606 | 50.6% | 96.7% | 1.7e-3 / 0.027 | 9.6e-4 / 0.014 |
| 9 | 368 | 38,330 | 55 | 58.5% | 72.7% | 0.028 / 0.109 | 0.033 / 0.096 |
| 12 | 38 | 3,894 | 6 | 58.8% | 66.7% | 0.29 / 0.80 | 0.41 / 0.57 |
| 17（ルート）| 1 | 120 | 0 | — | — | — | 2.02 / 2.02 |

- 上位レベルほどロック頂点率が上がる（クラスタ数が減ると外周の比率が増える）ため、粗いレベルで目標三角形数に届かないグループが増え、誤差が急に大きくなる。ルートの誤差（2 m）は blob の大きさより大きい: 誤差は「子の最大 + 今回」の**累積上界**（三角不等式）で、実際の偏差よりずっと保守的。遠方で粗い LOD が選ばれるのが遅くなる方向の誤差（安全側）。
- 画面空間しきい値 τ = 1 px（1080p・fovY 60°）で選ばれる三角形数（アセット全体を視野内とみなした上限。距離 = 境界球半径の倍数）と、見かけの三角形数削減率:

| 距離 / 半径 | 1.5× | 3× | 8× | 20× | 80× | 160× |
|---|---:|---:|---:|---:|---:|---:|
| 描く三角形（50M）| 240,276 | 150,088 | 83,908 | 43,772 | 10,936 | 6,594 |
| 削減率 | 99.52% | 99.70% | 99.83% | 99.91% | 99.98% | 99.99% |

**設計判断の根拠となった比較**（1M blob・7 スレッド。`--sweep`）:

| グループ化 | クラスタ | レベル | ページ B/tri | 境界エッジ比* | ロック頂点率* | 最上位誤差 |
|---|---:|---:|---:|---:|---:|---:|
| Morton 順に区切る | 19,016 | 27 | 39.1 | 69.9% | 59.3% | 7.15 |
| 再帰二分割（メディアン）| 16,761 | 20 | 33.1 | 59.9% | 47.7% | 5.26 |
| 貪欲グラフ成長（共有エッジ最多）| 15,513 | 14 | 29.2 | 48.6% | 35.4% | 1.55 |
| **meshopt_partitionClusters（既定）** | **15,512** | **13** | **28.9** | **41.9%** | **29.4%** | **1.14** |

\* 境界エッジ比 = 異なるグループにまたがる共有エッジ / 全共有エッジ（全レベル合計）、ロック頂点率 = ロックされた頂点 / グループの頂点（全レベル合計）。knot（1M）でも同じ順序（Morton 52.6 → 二分割 39.4 → 貪欲 31.0 → meshopt 29.6 B/tri）。隣接を見ない方式（Morton / 二分割）は境界が長くなって簡略化が進まず、レベルが増える。METIS は入れていない。

| グループ目標サイズ（meshopt）| 3 | 4 | 5 | **6（既定）** | 8 |
|---|---:|---:|---:|---:|---:|
| レベル / ページ B/tri | 16 / 30.5 | 14 / 29.4 | 13 / 29.2 | 13 / 28.9 | 13 / 28.9 |
| 80× で描く三角形（LOD 曲線）| 18,688 | 14,314 | 7,700 | 3,566 | 1,898 |

大きいグループほど外周の比率が下がって誤差が小さくなる（LOD 曲線が急に良くなる）。8 はページ充填率が下がる（グループがページを跨げない）ので 6 にした。
LOD0 の前処理（`meshopt_optimizeVertexCache` / `+ OverdrawOptimize`）は、LOD0 の充填率・サイズ・境界エッジ比のどれも変えない（±1%）ため**採用しない**（既定 OFF）。`meshopt_buildMeshletsSpatial` は境界エッジ比 41.9% → 38.5%・ページ B/tri 28.9 → 28.7 と少し良いが、LOD0 の充填率が 97.1%（Classic 99.6%）でクラスタ数が 2.3% 増え、最上位誤差も小さくならない（1.14 → 1.49）ので、Classic（cone_weight 0.25。法線コーンの効きは同じ: 平均 cutoff 0.27）を既定にした。簡略化の属性重みは幾何誤差に効く（500k blob の最上位誤差: 法線重み 0 / 0.25 / 1.0 で 0.38 / 0.74 / 3.0。UV 重み 0.25）ので、法線 0.25・UV 0.25 にした（法線を保つほど形が動く）。

---

## 17. 変更履歴

| 日付 | 内容 |
|---|---|
| 2026-09-30 | v1.0 確定（P0）。設計書 §3 / §3.8 を精査して §1 の 28 点を確定。実装: `VgeoFormat.h` / `VgeoBvh.h` / `VgsrcFormat.h` / `tools/vgeo_stub`。金型ハッシュ（手書き 3 レベル DAG、全 421,888 B の FNV-1a 64）= `0x10295F14C2901A31`。**バイト列を意図して変えるときは、この表と `kGoldenHandDag` を同時に更新する** |
| 2026-09-30 | **P1 完了（オフライン cooker `tools/vgeo_cook`）**。§1 に C29〜C31 を追加（消えるグループは最低 1 三角形を残す / ルートグループは複数可 / ページ順 = ルート → レベル降順 → Morton と pinned の方針）。**形式のバイト列は変えていない**（金型ハッシュ不変）。`VgsrcFormat.h` の `WriteVgsrc` をストリーム書きにし（出力バイトは同一・巨大入力で本体ぶんのメモリを確保しない）、`ReadVgsrc` のハッシュ検証を 64 MiB ずつの連鎖ハッシュにした。実測と設計判断の根拠 = §16.2 |
