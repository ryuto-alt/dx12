// VgShared.h ― 仮想ジオメトリの GPU カリングの「C++ と HLSL が同じ数字を使う」定数だけを置く。
//
// ★C++ と HLSL の両方から #include される（マクロだけ。型・関数・名前空間を書かないこと）。
//   HLSL 側は shaders/vg/VgCommon.hlsli が相対パスで取り込む。ここを変えたら shaders/vg を全部
//   再コンパイルさせるため、CMakeLists.txt の VG シェーダの DEPENDS にこのファイルが入っている。
#ifndef DX12E_VG_SHARED_H
#define DX12E_VG_SHARED_H

// ── スレッド構成 ─────────────────────────────────────────────────────────────
#define VG_THREADS            64u       // 全カーネル共通の numthreads(64,1,1)
#define VG_NODE_CHILDREN      4u        // BVH の 4 分木（1 ノード = 4 スレッド）
#define VG_CLUSTER_SLOTS      16u       // K2: 1 グループ = 16 スレッド（グループの最大クラスタ数は 15）
#define VG_NONE               0xFFFFFFFFu
#define VG_GRID_X             32768u    // 1 次元のスレッド列を 2 次元 Dispatch に畳む幅（1 次元の上限 65535 グループを超えないため）
#define VG_INF_BITS           0x7F800000u

// ── 作業バッファ（VgWork）内のカウンタ番号 ───────────────────────────────────
// 「カウンタ」は u32。キューへ積む側が InterlockedAdd し、容量を超えた分は捨てて overflow 統計を立てる。
// 読む側は min(count, cap) で頭打ちにする（カウンタ自体は容量を超えて増え続けてよい）。
#define VG_CNT_NODEQ0         0u
#define VG_CNT_NODEQ1         1u
#define VG_CNT_DEFERRED       2u        // 一相目で HZB にだけ落ちたノード（二相目の入力）
#define VG_CNT_GROUPQ0        3u        // 一相目のクラスタカリング入力（{instance, groupPacked}）
#define VG_CNT_GROUPQ1        4u        // 二相目のクラスタカリング入力（HZB でだけ落ちたグループ / クラスタ）
#define VG_CNT_VISIBLE        5u        // 可視クラスタ（一相目 + 二相目）
#define VG_CNT_RASTER_DONE    6u        // P3: ここまでラスタへ回した可視クラスタ数（二相目ラスタの開始位置）
#define VG_CNT_RASTER_START   7u        // P3: 今回のラスタ呼び出しの範囲 [start, start + count)（VgRasterArgs が書く）
#define VG_CNT_RASTER_COUNT   8u
#define VG_CNT_COUNT          64u       // カウンタ領域のサイズ（u32 個数）

// ── 統計（VgWork の stats 領域。u32 の配列）─────────────────────────────────
#define VG_STAT_INSTANCES         0u    // 入力インスタンス数
#define VG_STAT_INST_FRUSTUM      1u    // 錐台 / 極小棄却を通ったインスタンス
#define VG_STAT_INST_HZB_REJ      2u    // 一相目の HZB で落ちた（二相目へ回した）インスタンス
#define VG_STAT_NODES_VISITED     3u    // BVH ノードの訪問数（一相目 + 二相目）
#define VG_STAT_CHILDREN_TESTED   4u    // ノードの子スロットを判定した数
#define VG_STAT_GROUPS            5u    // 葉（グループ）として K2 へ渡した数（重複を含む）
#define VG_STAT_CLUSTERS_TESTED   6u    // K2 が判定したクラスタ数
#define VG_STAT_CLUSTERS_LOD      7u    // LOD 条件（親が粗すぎ && 自分が十分細かい）を通ったクラスタ数（=「選択クラスタ」）
#define VG_STAT_VISIBLE_P1        8u    // 一相目で可視と確定したクラスタ数
#define VG_STAT_VISIBLE_P2        9u    // 二相目で可視と確定したクラスタ数
#define VG_STAT_TRIS              10u   // 可視クラスタの三角形数の合計（見かけの三角形数）
#define VG_STAT_OVF_NODEQ         11u   // 各キューの溢れ（捨てた数）
#define VG_STAT_OVF_GROUPQ        12u
#define VG_STAT_OVF_VISIBLE       13u
#define VG_STAT_OVF_DEFERRED      14u
#define VG_STAT_SRC_TRIS_LO       15u   // 錐台内インスタンスの LOD0 換算の三角形数（u64 の下位 / 上位）
#define VG_STAT_SRC_TRIS_HI       16u
#define VG_STAT_CL_CULL_FRUSTUM   17u   // K2 のクラスタが錐台で落ちた数
#define VG_STAT_CL_CULL_CONE      18u   // 法線コーンで落ちた数
#define VG_STAT_CL_DEFERRED       19u   // クラスタが一相目の HZB で落ちて二相目へ回った数
#define VG_STAT_NODE_CULL_FRUSTUM 20u   // ノードの子が錐台で落ちた数
#define VG_STAT_NODE_CULL_LOD     21u   // ノードの子が LOD 枝刈りで落ちた数
#define VG_STAT_NODE_DEFERRED     22u   // ノードの子が HZB で二相目へ回った数
#define VG_STAT_CL_HZB_DROP       23u   // 二相目の HZB でも落ちたクラスタ数
#define VG_STAT_HIST_BASE         32u   // 可視クラスタのレベル別ヒストグラム（32 段。レベル >= 31 は最終枠）
#define VG_HIST_LEVELS            32u
// ── P3（ラスタ）の統計。計測用のもの（フラグ ON のときだけ数える）は末尾に「計測」と書く ──
#define VG_STAT_RASTER_CLUSTERS   64u   // ラスタへ投げたクラスタ数（一相目 + 二相目）
#define VG_STAT_AS_CULLED         65u   // 増幅シェーダの追加カリングで落としたクラスタ数（計測）
#define VG_STAT_MS_PRIM_CULLED    66u   // メッシュシェーダの小三角形カリングで落とした三角形数（計測）
#define VG_STAT_MS_TRIS_OUT       67u   // メッシュシェーダが出力した三角形数（計測）
#define VG_STAT_PS_INVOC          68u   // ピクセルシェーダの起動数 = 深度テストを通った断片数（計測）
#define VG_STAT_COVERED_PIXELS    69u   // 可視性バッファの被覆画素数（計測。VgCoverage が数える）
#define VG_STAT_EDGE_CL_BASE      72u   // 可視クラスタの最長辺（画面 px）ヒストグラム: クラスタ数 x 8 段（計測）
#define VG_STAT_EDGE_TRI_BASE     80u   //   同: 三角形数 x 8 段（計測）
#define VG_EDGE_BINS              8u    // [0,1) [1,2) [2,4) [4,8) [8,16) [16,32) [32,64) [64,inf) px
#define VG_STAT_COUNT             128u

// ── 作業バッファのレイアウト（バイト）────────────────────────────────────────
#define VG_WORK_COUNTERS_OFFSET   0u
#define VG_WORK_STATS_OFFSET      (VG_CNT_COUNT * 4u)                            // 256
#define VG_WORK_QUEUES_OFFSET     (VG_WORK_STATS_OFFSET + VG_STAT_COUNT * 4u)    // 512

// ── ExecuteIndirect の引数バッファ（uint3 を 16 B 刻みで）───────────────────
#define VG_ARGS_STRIDE            16u
#define VG_ARGS_TRAVERSE          0u
#define VG_ARGS_CLUSTER           1u
#define VG_ARGS_RASTER            2u        // P3: DispatchMesh 引数（uint3。12 B を 16 B 刻みに置く）
#define VG_ARGS_SLOTS             4u

// ── P3: ラスタ ─────────────────────────────────────────────────────────────
#define VG_AS_THREADS             32u       // 増幅シェーダ 1 グループ = 32 クラスタ
#define VG_MS_THREADS             128u      // メッシュシェーダ 1 グループ = 1 クラスタ（頂点 / 三角形 各 128 まで）
#define VG_VIS_EMPTY              0xFFFFFFFFu
#define VG_VIS_TRI_BITS           7u        // 可視性バッファ = (可視スロット << 7) | クラスタ内の三角形番号
#define VG_VIS_TRI_MASK           127u
// ラスタのフラグ（ルート定数 gPass0.x）
#define VG_RF_SMALLPRIM_CULL      1u        // メッシュシェーダで「どの画素中心も覆わない」三角形を落とす
#define VG_RF_COUNT               2u        // 計測（PS 起動数 / 小三角形カリング数 / オーバードロー画像）
#define VG_RF_EDGE_HIST           4u        // K2 が辺長ヒストグラムを数える
// デバッグ描画のモード（ルート定数 gPass0.x）
#define VG_DBG_SHADE              0u        // 暫定シェーディング（P4 の resolve まで）
#define VG_DBG_CLUSTER            1u
#define VG_DBG_LOD                2u
#define VG_DBG_TRI                3u
#define VG_DBG_DEPTH              4u
#define VG_DBG_OVERDRAW           5u
#define VG_DBG_COVERAGE           6u
// ── P4（resolve の検証用可視化。RenderDebugMode = 20 + 値）──
#define VG_DBG_MATERIAL           7u        // 材質（アセット + 材質番号のハッシュ色）
#define VG_DBG_MIP                8u        // 解析 UV 勾配から求めたアルベドのミップ段（青 = 0 … 赤 = 10+）
#define VG_DBG_TILE_MATERIALS     9u        // 8x8 タイル内の異なる材質の数（resolve の波面の分岐。1 = 緑 … 4+ = 赤）
#define VG_DBG_NORMAL             10u       // resolve が使う頂点法線（補間・ワールド）
// ── P4: テスト用の生データ出力（R32G32B32A32_FLOAT の RT へ。tests/vg_resolve_gpu_test.cpp が読む）──
#define VG_DBG_RAW_BARY           16u       // xyz = 透視補正の重心座標、w = 可視スロット
#define VG_DBG_RAW_UVGRAD         17u       // xy = dUV/dx、zw = dUV/dy（1 画素差分）
#define VG_DBG_RAW_UV             18u       // xy = UV、z = ワールド位置の dW/dx の長さ、w = 三角形番号

// ── インスタンスのフラグ ─────────────────────────────────────────────────────
#define VG_INST_MIRRORED          1u        // 行列式 < 0
#define VG_INST_HAS_PREV          2u        // prevWorld が有効
#define VG_INST_EMIS_COLOR_OV     4u        // P4: packedEmissive の RGB888 = 自己発光色の上書き（MeshRenderer::overrideEmissiveColor）
#define VG_INST_EMIS_INT_OV       8u        // P4: packedEmissive の上位 8bit = 自己発光強度の上書き（二乗曲線の 8bit。Material.h の PackEmissive と同じ）

// ── P4: 材質（VgMaterialGpu.flags。bit0/1/3 は Material.h の pbrFlags と同じ意味）──
#define VG_MAT_NORMAL_TEX         1u
#define VG_MAT_MR_TEX             2u
#define VG_MAT_EMISSIVE_TEX       8u
#define VG_MAT_DOUBLE_SIDED       16u       // 両面材質: K2 の法線コーン棄却をしない
#define VG_MAT_STRIDE             64u       // sizeof(VgMaterialGpu)

// ── P4: 安定コンパクション（決定論）──
#define VG_SORT_THREADS           256u      // ビトニックソート 1 グループのスレッド数（1 スレッド = 比較 1 組）
#define VG_SORT_MAX_LOG2          17u       // 1 フェーズでソートする可視クラスタの上限 = 2^17（超えたら並べ替えない = 決定論を諦めて統計で知らせる）
#define VG_STAT_SORT_SKIPPED      70u       // 上限を超えて安定ソートを諦めたフェーズの数
#define VG_STAT_SORTED            71u       // 安定ソートした可視クラスタ数

#endif // DX12E_VG_SHARED_H
