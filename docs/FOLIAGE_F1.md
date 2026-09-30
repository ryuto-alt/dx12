# 植生 F1（FoliageLayer + GPU カリング + ExecuteIndirect + 風）

設計は `docs/GRAPHICS_PARITY_DESIGN.md` §2.3。実装の場所は `src/renderer/foliage/`・`shaders/foliage/`。
実測値・やらなかったこと・申し送りは `C:\Users\ryuto\Documents\dx12-ui-audit\FOLIAGE_F1_REPORT.md`。

## 1. 構成

```
FoliageLayer（コンポーネント。パラメータだけがシーン JSON へ）
   └─ .dxfoliage（インスタンス表 32B/個 + チャンク表。シーン JSON に直書きしない）
FoliageSystem（renderer/foliage/FoliageSystem.*）… 毎フレーム レイヤーを走査
   ├─ FoliageCuller（compute。専用ルートシグネチャ）… Reset → Cull(主ビュー / 影カスケード 0,1) → Finalize
   └─ 描画（メイン RS・DWORD 増分 0）… 本体 / 深度 / 速度+G-Buffer / CSM を ExecuteIndirect
```

- **未使用シーンは完全に無影響**: `FoliageLayer` が 1 つも無いと `FoliageSystem` は作られず（GPU リソース 0・PSO 0）、
  描画パスの呼び出し点は `FoliageActive()` で弾かれる。
- 頂点ストリーム: slot0 = モデル（`Vertex`）/ slot1 = compact されたインスタンス 64B（PER_INSTANCE）。
  PS は `Forward.hlsl` の `PSMain` をディザで包んだ `ForwardFoliage_PS`（ライティング / 影 / フォグ / IBL / DDGI は既存のまま）。
  Q2 の物理ライティング単位（`*Phys_PS`）にも追従する。
- ルートシグネチャ: メインは 62/64 のまま。カリングは専用 RS（バッファはルート記述子、HZB だけ専用ヒープのテーブル）。
- 風のフレーム共通値: 本体は PerFrame(b1) の予約領域 `_clusterReserved[0..1]`、深度 / 影 / 速度は専用 CBV を b1 に貼る。
  レイヤー別（曲げ・はばたき・高さ）は b0 の末尾（本体 24 / 深度・影 20 / 速度 40 DWORD）。

## 2. データ形式

### インスタンス（32B）
`float3 position`（レイヤーのローカル）/ `u32 rotScale`（yaw u16 + 一様スケール half）/ `u32 tilt`（snorm16×2、±45°）/
`u32 color`（RGBA8）/ `u32 seedType`（seed 24bit + 種別 8bit）/ `u32 params`（風の強さ倍率 ×1/128）。
`world = layer * [Rz(tz) Rx(tx) Ry(yaw) * s | p]`（列ベクトル）。

### `.dxfoliage`
64B ヘッダ（`DXFL` / version 1 / instanceCount / chunkCount / bounds / CRC32）+ チャンク表（32B × M）+ インスタンス（32B × N）。
チャンクは kd 木で作る ≤128 個のまとまり（AABB + 先頭添字 + 個数）。**読み込みは memcpy 2 回**（100 万 = 32 MB）。
破損（CRC / 途中切れ / 範囲外チャンク / NaN / 件数の暴走）は読み込みで拒否する。保存は一時ファイル経由。

## 3. GPU カリング（`FoliageCull.hlsl`。式は `FoliageMath.h` と一対一）

1 グループ = 1 チャンク。チャンクを AABB（レイヤー Transform 後）で棄却 → 個々のインスタンスを
距離 / 視錐台（球）/ 前フレーム HZB / 間引き の順に判定し、LOD ごとの compact ストリームへ書く。

- **決定論的な compact（3 段）**: `CSCount`（チャンク × list ごとの個数）→ `CSScan`（list ごとに排他的プレフィックス和）→ `CSEmit`
  （同じ判定をもう一度して「チャンクの先頭位置 + グループ内の順位」へ書く。順位は groupshared ビットマスクの popcount）。
  出力の並びは (チャンク順, チャンク内の添字順) で**実行順に依存しない**。原子カウンタで Append していた初版は並びが実行ごとに変わり、
  葉カードの交線など深度が同点の断片で勝者が変わって同じ絵が 1 ピクセル揺れた（決定論スクショが破れた）。コストは判定 2 回ぶん
  （1M 個・同視点で 0.12 → 0.23 ms）。
- **クロスフェード**: 切替距離 ±`lodFade` の帯で隣り合う 2 LOD に出し、PS のスクリーンドア（IGN）で補完的に捨てる
  （`discard iff lo <= n <= hi`。LOD k は `n >= (1-t)b`、LOD k+1 は `n <= 1-tb`）。
- **間引き**: `thinStart × cullDistance` から密度が 1 → 0 へ落ちる。インスタンスのハッシュで決まる＝同じ木が毎フレーム同じ運命。
- **影**: 影カスケード 0,1 を別ビューとして切る（`shadowDistance` / `shadowMaxLod` 以下の LOD だけ・クロスフェード無し）。
- **HZB**: 前フレームの Hi-Z を前フレーム VP で引く（disocclusion の 1 フレーム遅れは矩形を 6px 膨らませて緩和）。
  Hi-Z が無い（オクルージョンカリング OFF / リサイズ直後）フレームは自動で無効。記述子は**フレームスロットごと・毎フレーム**書く
  （共有 1 個を「ポインタが変わったときだけ」書き換える方式は、Hi-Z 再作成で同じアドレスに別リソースが入ると破棄済みを指して GPU ハングした）。
  Hi-Z の作り直し（`ApplicationPipeline` のリサイズ）で `m_foliageHzbValid` を落とす（未構築の UAV 状態のテクスチャを読まない）。
- 容量: `maxVisible`（既定 131072）/ LOD list。超えた分は描かれず `foliage_stats.overflow` に出る。

## 4. 深度プリパス・影・速度

- 深度 / 影 / 速度は**本体と同じ頂点関数・同じディザ式**で描く（ずれると本体の LESS_EQUAL が面を落として穴が開く）。
- MASK（アルファテスト）は `ShadowMask` と同じ t0 / b2 / cutoff で抜く。BLEND 材質は MASK 扱い。
- 速度: **現在時刻と前フレーム時刻の 2 回**同じ風関数で位置を評価する（インスタンスは静的なので前ワールドは不要）。

## 5. 風

シーン全体（`SceneWind` = `Scene::GetWind()`。シーン JSON `wind`。MCP `set_wind`）+ レイヤー別（`windBend` / `windFlutter` / `windBendExp`）。

- 突風が風下へ流れる値ノイズ場 × 速度 m/s。幹は高さ h の `h^exp` に比例して風下へ曲がる（**長さを保つ**ので伸びない）。
- 頂点色の規約（Crysis 流）: **R = はばたきの重み / G = 葉ごとの位相 / B = 曲げの重み / A = 焼き込み AO**。
  PS へ渡す色にはこの頂点色を使わない（風データがアルベドに掛かるため）。AO だけを `aoStrength` で乗せる。
- 葉のはばたきは 4 本の三角波（1.975 / 0.793 / 0.375 / 0.193）を法線方向へ。
- 時間 = ゲームクロック総時間 + `phaseOffset`。**決定論キャプチャでは総時間が固定値**なので、風の位相を撮り分けるときは `phaseOffset` を変える。

## 6. 散布とブラシ

MCP `foliage_scatter` / `foliage_paint` / `foliage_clear` / `foliage_save` / `foliage_stats` / `get_wind` / `set_wind`。エディタは「ツール → 植生ツール」。

- 散布 = ポアソンディスク（Bridson。`minSpacing`）または一様（ジッタ格子）。傾斜 `maxSlopeDeg` / 高度 / スプラット層の重み / 法線への傾き /
  スケール・色・風のばらつき / 種別の出現比。**seed 指定で決定論**（プラットフォーム非依存の自前 RNG）。
- 表面: 地形（高さ配列 + スプラット）/ 平面 / シーンのメッシュ（真下レイ）。
- ブラシ = 円で追加 / 削除。ストローク単位で Undo（`FoliageLayer` の `_set` はコピーオンライトなので古いポインタへ戻すだけ）。

## 7. 制約

- スキンドモデルは不可。モデルの材質は glTF の材質のみ（`.dxmat` / テクスチャ上書きは未対応）。
- レイヤーの Transform は平行移動 + Y 回転 + 一様スケールを想定（散布時の XZ 往復のため）。
- スポット / ポイントライトの影は植生を落とさない（CSM のみ）。RT（TLAS）にも入らない。
- 巨大レイヤー（100 万）でのブラシは、スタンプごとにチャンクを作り直すので 0.1〜0.3 秒かかる（90ms 間隔でスロットル）。
