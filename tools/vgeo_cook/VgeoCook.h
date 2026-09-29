#pragma once
//
// VgeoCook.h ― 仮想ジオメトリ（Nanite 風）のオフライン cooker（P1）の公開 API。
//
//   入力  : VgsrcData（VGSRC をメモリへ読んだもの。OBJ や手続きメッシュもこの形へ変換して渡す）
//   出力  : VgeoContent（階層 LOD の完全な `.vgeo`: クラスタ DAG + 4 分木 BVH + ページ + プロキシ + 材質）。
//           書き出しは WriteVgeoToFile。出力前検査は ValidateContent / ValidateVgeo（CLI は書いたファイルを検証する）。
//   仕様  : docs/VGEO_SPEC.md（★食い違ったら仕様書が正）。アルゴリズムの根拠は docs/VIRTUAL_GEOMETRY_DESIGN.md §2.3。
//   決定論: 同じ入力・同じオプションなら、スレッド数に依らず同じバイト列（並列は作業項目ごとに独立に計算し、添字順に結合する）。
//
#include "CookGroup.h"
#include "renderer/vg/VgeoBvh.h"
#include "renderer/vg/VgeoFormat.h"
#include "renderer/vg/VgsrcFormat.h"

#include <string>
#include <vector>

namespace dx12e::vg::cook
{

inline constexpr const char* kCookerVersion = "1.0.0";

enum class Lod0Builder : u32
{
    Classic = 0,   // meshopt_buildMeshlets（cone_weight 付き）
    Flex    = 1,   // meshopt_buildMeshletsFlex（min_triangles 付き）
    Spatial = 2,   // meshopt_buildMeshletsSpatial
};

struct CookOptions
{
    // ── クラスタ ──
    u32  maxVerts   = 128;                 // 1 クラスタの頂点数の上限（3..128）
    u32  maxTris    = 128;                 // 三角形数の上限（4 の倍数、4..128）
    f32  coneWeight = 0.25f;               // meshopt_buildMeshlets の cone_weight（法線コーンの効き）
    Lod0Builder lod0Builder = Lod0Builder::Classic;
    bool vertexCacheOpt = false;           // LOD0 のチャンクへ meshopt_optimizeVertexCache を前処理として掛ける（実測で効果なし → 既定 OFF。P1 レポート §3）
    bool overdrawOpt = false;              // 続けて meshopt_optimizeOverdraw も掛ける（同上。vertexCacheOpt が前提）
    u32  lod0ChunkTris = 32768;            // LOD0 クラスタ化の空間チャンク（Morton 順に切る）の目安三角形数。スレッド数と無関係に固定
    // ── DAG ──
    GroupMethod groupMethod = GroupMethod::Meshopt;
    u32  groupSize  = 6;                   // グループの目標クラスタ数（2..8）。4 → 6 でロック頂点 64% → 40%（レベル 6）・誤差が小さくなり、レベル数・クラスタ数も減る（1M blob。P1 レポート §3）
    f32  reduction  = 0.45f;               // 1 段で三角形数を何倍にするか。0.5 → 31.3 B/tri、0.45 → 28.7 B/tri（5000 万 tri の全常駐 VRAM 予算 1.5 GB に収めるため 0.45）。小さいほどファイルは小さいが LOD が粗く見える
    f32  targetFill = 0.95f;               // 簡略化の目標三角形数 = 出力クラスタ数 * maxTris * targetFill
    f32  normalWeight = 0.25f;             // 簡略化の属性重み（法線。大きいほど法線を保つが幾何誤差は増える。500k blob で 0 → root 誤差 0.85 / 0.25 → 2.0 / 1.0 → 5.7）
    f32  uvWeight     = 0.25f;             // 同 UV（グループの UV 範囲で正規化してから。効きは小さい）
    bool lockOpenBorders = false;          // 元メッシュの開いた境界（三角形 1 枚だけのエッジ）の頂点もロックする
    bool regularize = false;               // meshopt_SimplifyRegularize: 三角形の大きさ / 形を揃える（細長い三角形の折れ返しを減らす。幾何精度は少し落ちる）
    bool debugNoLocks = false;             // ★診断専用: グループ境界をロックしない（クラックが出ることをテストが確かめる。実 cook では使わない）
    u32  permissive = 2;                   // meshopt_SimplifyPermissive（属性の不連続 = UV / 法線の継ぎ目・極の扇 をまたぐ縮約を許す）: 0 = 使わない / 1 = 常に / 2 = 自動（通常の簡略化が目標に届かないグループだけ）
    // ── 入力の前処理 ──
    bool dedupe = true;                    // 位置 + 法線 + UV が同一の頂点を融合する
    // ── プロキシ / NONVG ──
    bool proxy        = true;
    u32  proxyMaxTris = 250000;
    u32  nonVgMaxTris = 200000;
    // ── ページ ──
    u32  pinPages     = 64;                // 先頭から最低これだけのページを pinned にする（ルートを含むページは常に pinned）
    // ── 実行 ──
    u32  threads      = 0;                 // 0 = 既定（論理コア / 4）
    bool lowPriority  = true;              // ワーカーを BelowNormal で走らせる
    bool collectStats = true;              // LOD 曲線など（少し余分に計算する）
    bool verbose      = false;             // レベルごとの進行を stderr に出す
    std::string label = "cook";            // 材質名が無いときの名前 / debugJson の識別子

    // 決定論・再 cook 判定用の正準文字列（固定順の key=value;…）。cookParamsHash の元。threads / lowPriority / collectStats / verbose は含めない。
    std::string CanonicalString() const;
};

struct LevelStat
{
    u32  level = 0;
    u64  clusters = 0, tris = 0;           // このレベルのクラスタ数 / 三角形数（全セクション合計）
    u64  groups = 0;                       // このレベルのクラスタを消費するグループ数
    u64  groupClusters = 0;                // 同・メンバー数の合計（平均グループサイズ = groupClusters / groups）
    u64  cutEdges = 0;                     // 異なるグループにまたがる共有境界エッジ（無向）
    u64  sharedEdges = 0;                  // クラスタ間で共有される境界エッジの総数（無向）
    u64  trisIn = 0, trisOut = 0;          // 簡略化の入力 / 出力三角形数（このレベルのグループの合計）
    u64  lockedVerts = 0, groupVerts = 0;  // ロックされた頂点 / グループの頂点の合計
    f64  meanSimplifyErr = 0, maxSimplifyErr = 0;   // このレベルの簡略化 1 回分の誤差（m）
    f64  meanLodErr = 0, maxLodErr = 0;    // このレベルのクラスタの lodError（累積。m）
    u64  reachedTarget = 0;                // 目標三角形数に到達したグループ数
    u64  permissiveGroups = 0;             // 継ぎ目をまたぐ縮約（permissive）で簡略化したグループ数
};

struct LodCurvePoint
{
    f64 distanceOverRadius = 0;            // カメラ距離 / アセットの境界球半径
    u64 drawnTris = 0;                     // その距離で LOD 選択が描く三角形数（全クラスタ）
    f64 reduction = 0;                     // 1 - drawn / source（見かけの三角形数削減率）
};

struct CookStats
{
    // 入力
    u64 inputTris = 0, inputVerts = 0, degenerateDropped = 0, verticesMerged = 0, vgTris = 0, nonVgTris = 0;
    // 出力
    u64 clusters = 0, groups = 0, pages = 0, nodes = 0, rootClusters = 0, lod0Clusters = 0;
    u32 levels = 0, pinnedPages = 0, stagnatedRoots = 0;   // stagnatedRoots: これ以上簡略化できず、そのレベルをルートにした回数
    u64 pageBytes = 0, fileBytesEstimate = 0;
    f64 avgTrisPerCluster = 0, avgVertsPerCluster = 0, avgFillLod0 = 0, avgFillAll = 0;
    f64 avgPosBits = 0, pageFill = 0;
    f64 coneValidFrac = 0, coneMeanCutoff = 0;   // LOD0 クラスタのうち法線コーンが有効（cutoff < 1）な割合 / その cutoff の平均（小さいほど背面カリングが効く）
    u32 threadsUsed = 0;
    std::vector<LevelStat> perLevel;
    std::vector<LodCurvePoint> lodCurve;
    // 時間（ms）
    f64 msIngest = 0, msLod0 = 0, msDag = 0, msGraph = 0, msGroup = 0, msSimplify = 0, msPack = 0, msBvh = 0, msProxy = 0, msTotal = 0;
    u64 peakWorkingSet = 0;                // Cook 呼び出し中に観測した最大（バイト。Windows のみ）
};

// 入力を消費する（in の配列は Cook の途中で解放してメモリを絞る）。失敗は構造化エラー（Errc + 説明）。
VgeoError Cook(VgsrcData&& in, const CookOptions& opt, VgeoContent& out, CookStats* stats = nullptr);

// 便利: 位置 / インデックスだけから VGSRC 相当を作る（1 セクション・既定材質。法線は cooker が作る）。テスト・OBJ 用。
VgsrcData MakeVgsrcFromTriangles(const std::vector<f32>& positions, const std::vector<u32>& indices, const std::vector<f32>* normals = nullptr,
                                 const std::vector<f32>* uvs = nullptr, const std::string& materialName = "mat");

} // namespace dx12e::vg::cook
