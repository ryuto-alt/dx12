#pragma once

// ============================================================================
// ナビメッシュ（自作）— 型定義
// ============================================================================
// 方式は業界標準の「ボクセル化パイプライン」（Recast と同系統。2025 年時点で
// Havok もこの方式へ寄せている）。三角形スープを一度ボクセルへ焼いてから
// 歩ける面を取り出すので、AABB 近似ではなく **実際の三角形** が入力になり、
// 傾斜角 / 段差 / エージェント半径 / 頭上クリアランスを一貫して扱える。
//
//   1. Rasterize  : 三角形 → ソリッドハイトフィールド（列ごとの span 列）
//   2. Filter     : 低い障害物のまたぎ / 崖(ledge) / 頭がつかえる面 を落とす
//   3. Compact    : 歩行面だけの密な表現 + 隣接接続（段差 walkableClimb で繋ぐ）
//   4. Erode      : エージェント半径ぶん壁から削る（チャンファ距離変換）
//   5. Region     : 距離場 → 分水嶺(watershed) or monotone で領域分割
//   6. Contour    : 領域の輪郭を追跡 → Douglas-Peucker で単純化 → 穴を橋渡し
//   7. PolyMesh   : 耳刈り三角形分割 → 凸性を保ったままポリゴン併合
//   8. NavMesh    : ランタイム表現（ポリゴン + 隣接 + 高さサンプル格子）
//
// 単位の約束: 設定はメートル（利用者が触るのはこちら）。内部はボクセル単位。
//   walkableHeight = ceil(agentHeight / ch)
//   walkableClimb  = floor(agentMaxClimb / ch)
//   walkableRadius = ceil(agentRadius / cs)
// ============================================================================

#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e
{
namespace nav
{

// ---- 定数 ------------------------------------------------------------------
inline constexpr u16 kNullArea      = 0;      // 歩けない
inline constexpr u16 kWalkableArea  = 63;     // 既定の歩行可能エリア
inline constexpr u16 kMaxHeight     = 0xffff; // span の天井なし
inline constexpr u16 kNullRegion    = 0;
inline constexpr u16 kBorderRegion  = 0x8000; // 領域IDの最上位ビット = 外周ボーダー
inline constexpr u16 kRegionMask    = 0x7fff;
inline constexpr u32 kNotConnected  = 0x3f;   // 6bit の「未接続」
inline constexpr u16 kMeshNullIdx   = 0xffff;

// 輪郭頂点のフラグ（点の w に載せる）
inline constexpr u32 kBorderVertex  = 0x10000;
inline constexpr u32 kAreaBorder    = 0x20000;
inline constexpr u32 kContourRegMask = 0xffff;

// ---- 入力ジオメトリ ---------------------------------------------------------
// ワールド空間の三角形スープ。呼び出し側（シーン走査）が組む。
struct NavInputGeometry
{
    std::vector<f32> verts;   // xyz * n
    std::vector<i32> tris;    // 3 * n（verts のインデックス）
    f32 bmin[3]{ 0, 0, 0 };
    f32 bmax[3]{ 0, 0, 0 };

    void Clear() { verts.clear(); tris.clear(); }
    bool Empty() const { return tris.empty(); }
    i32  TriCount() const { return static_cast<i32>(tris.size() / 3); }
    void ComputeBounds();
};

// ---- ビルド設定 -------------------------------------------------------------
// ★ここを触るのはユーザー。全部メートル / 度。
struct NavBuildConfig
{
    f32 cellSize             = 0.20f;  // cs: XZ のボクセル辺長。細かいほど精度↑・時間↑
    f32 cellHeight           = 0.12f;  // ch: Y のボクセル高さ。段差の分解能
    f32 agentHeight          = 1.80f;  // 頭上クリアランス
    f32 agentRadius          = 0.35f;  // 壁からこのぶん削る
    f32 agentMaxClimb        = 0.45f;  // またげる段差（階段一段）
    f32 agentMaxSlope        = 45.0f;  // 歩ける最大傾斜（度）★坂道の許容角
    f32 minRegionArea        = 2.0f;   // m^2 未満の孤立領域は捨てる
    f32 mergeRegionArea      = 20.0f;  // m^2 未満の領域は隣へ併合
    f32 maxEdgeLen           = 12.0f;  // 輪郭辺の最大長（0 で無制限）
    f32 maxSimplificationErr = 1.30f;  // 輪郭単純化の許容誤差（ボクセル単位）
    i32 maxVertsPerPoly      = 6;      // 1 ポリゴンの最大頂点数（3..12）
    bool monotonePartition   = false;  // true で monotone 分割（速い/穴が出ない・形は劣る）
    bool filterLedgeSpans    = true;   // 崖ぎわを歩行不可にする
    bool filterLowHanging    = true;   // 低い障害物をまたげるようにする

    // ビルド範囲。useBounds=false なら入力ジオメトリの AABB を使う。
    bool useBounds = false;
    f32  boundsMin[3]{ 0, 0, 0 };
    f32  boundsMax[3]{ 0, 0, 0 };
};

// ---- ソリッドハイトフィールド ------------------------------------------------
struct NavSpan
{
    u16 smin = 0;             // 下端（ボクセル）
    u16 smax = 0;             // 上端（ボクセル）
    u8  area = kNullArea;     // 上面が歩けるか
    i32 next = -1;            // 同じ列の次の span（上向き）。-1 で終端
};

struct NavHeightfield
{
    i32 w = 0, h = 0;
    f32 bmin[3]{ 0, 0, 0 }, bmax[3]{ 0, 0, 0 };
    f32 cs = 0.0f, ch = 0.0f;
    std::vector<i32>     cells;   // w*h、spans への先頭インデックス（-1 で空）
    std::vector<NavSpan> spans;   // プール
};

// ---- コンパクトハイトフィールド ----------------------------------------------
struct NavCompactCell
{
    u32 index = 0;   // spans への開始位置
    u32 count = 0;   // この列の span 数
};

struct NavCompactSpan
{
    u16 y   = 0;              // 床の高さ（ボクセル）
    u16 reg = kNullRegion;    // 領域 ID
    u32 con = 0;              // 4 方向 × 6bit の隣接 span インデックス（列内オフセット）
    u16 h   = 0;              // 頭上の空き（ボクセル）
};

struct NavCompactHeightfield
{
    i32 w = 0, h = 0;
    i32 spanCount = 0;
    i32 walkableHeight = 0, walkableClimb = 0, borderSize = 0;
    u16 maxDistance = 0, maxRegions = 0;
    f32 bmin[3]{ 0, 0, 0 }, bmax[3]{ 0, 0, 0 };
    f32 cs = 0.0f, ch = 0.0f;
    std::vector<NavCompactCell> cells;
    std::vector<NavCompactSpan> spans;
    std::vector<u16>            dist;   // 距離場
    std::vector<u8>             areas;
};

// 4 方向のオフセット（0=-x, 1=+z, 2=+x, 3=-z）。全段階でこの並びを共有する。
inline constexpr i32 kDirOffsetX[4] = { -1, 0, 1, 0 };
inline constexpr i32 kDirOffsetZ[4] = { 0, 1, 0, -1 };

// con は 4 方向 × 6bit（隣接列の何番目の span か）。0x3f = 未接続。
inline u32 GetCon(const NavCompactSpan& s, i32 dir)
{
    return (s.con >> (static_cast<u32>(dir) * 6)) & 0x3fu;
}
inline void SetCon(NavCompactSpan& s, i32 dir, u32 v)
{
    const u32 shift = static_cast<u32>(dir) * 6;
    s.con = (s.con & ~(0x3fu << shift)) | ((v & 0x3fu) << shift);
}

// ---- 輪郭 -------------------------------------------------------------------
struct NavContour
{
    std::vector<i32> verts;     // 4 成分 * n : x, y, z, （領域ID | フラグ）
    std::vector<i32> rverts;    // 単純化前の生輪郭（穴の判定と面積計算に使う）
    u16 reg  = 0;
    u8  area = 0;
};

struct NavContourSet
{
    std::vector<NavContour> conts;
    f32 bmin[3]{ 0, 0, 0 }, bmax[3]{ 0, 0, 0 };
    f32 cs = 0.0f, ch = 0.0f;
    i32 w = 0, h = 0, borderSize = 0;
    f32 maxError = 0.0f;
};

// ---- ポリゴンメッシュ（ボクセル座標）------------------------------------------
struct NavPolyMeshRaw
{
    std::vector<u16> verts;   // 3 * nverts（ボクセル整数座標）
    std::vector<u16> polys;   // 2 * nvp * npolys（前半=頂点, 後半=隣接ポリ）
    std::vector<u16> regs;    // npolys
    std::vector<u8>  areas;   // npolys
    i32 nverts = 0, npolys = 0, nvp = 0;
    f32 bmin[3]{ 0, 0, 0 }, bmax[3]{ 0, 0, 0 };
    f32 cs = 0.0f, ch = 0.0f;
    i32 borderSize = 0;
};

// ---- ポリゴン参照（polyRef）--------------------------------------------------
// 0 = 無効。上位 32bit = メッシュの世代（焼き直す / 読み直す / 消すたびに変わる）、
// 下位 32bit = ポリゴン番号 + 1。
// ★Lua へは整数のまま渡す（Lua 5.4 の整数は 64bit）。世代が違う ref は DecodeRef で -1 になる
//   ＝焼き直した後に古い ref を使っても、別のポリゴンを黙って指すことがない。
using NavPolyRef = u64;
inline constexpr NavPolyRef kNullPolyRef = 0;

// ---- レイキャストの結果 --------------------------------------------------------
// ★旧 Raycast は bool 1 個を返していて、false が「通れた」「始点がポリゴンの外だった」
//   「辿る上限で打ち切った」の 3 つに潰れていた（MikuChase.lua が hit=true しか信用できなかった理由）。
//   ここでは 4 状態を区別して返す。
enum class NavRayStatus : u8
{
    Clear        = 0,   // 終点まで壁に当たらずに届いた
    Hit          = 1,   // 壁（隣のポリゴンが無い辺）に当たった
    StartOffMesh = 2,   // 始点がナビメッシュ上に無い（判定できない）
    Truncated    = 3,   // 辿るポリゴン数の上限で打ち切った（判定できない）
};
const char* NavRayStatusName(NavRayStatus s);

struct NavRaycastResult
{
    NavRayStatus status = NavRayStatus::StartOffMesh;
    f32 t = 0.0f;              // 始点→終点の割合。Clear=1 / Hit=当たった所 / 判定不能=そこまで進めた所
    f32 hitPos[3]{ 0, 0, 0 };  // Hit=当たった点 / Clear=終点 / 判定不能=進めた所
    f32 hitNormal[3]{ 0, 0, 0 };   // Hit のときの壁の法線（xz、通路の内側を向く）
    i32 lastPoly = -1;         // 最後に居たポリゴン
    i32 visitedCount = 0;      // 辿ったポリゴン数
};

// FindPolyPath の結果
enum class NavPathStatus : u8
{
    Failed   = 0,   // 始点か終点がナビメッシュに無い
    Complete = 1,   // 終点のポリゴンまで繋がった
    Partial  = 2,   // 繋がらないので「一番近いところまで」
};
const char* NavPathStatusName(NavPathStatus s);

// FindStraightPath の点ごとの印
inline constexpr u8 kStraightPathStart = 0x01;
inline constexpr u8 kStraightPathEnd   = 0x02;

// ---- ランタイム NavMesh -----------------------------------------------------
// ワールド座標のポリゴン + 隣接 + 「高さサンプル格子」。
// 格子は 2 役: (a) 位置→ポリゴンの空間索引、(b) 坂道の正確な高さ取得。
struct NavPoly
{
    u32 firstVert = 0;   // verts へのオフセット
    u32 vertCount = 0;
    u32 firstNei  = 0;   // neis へのオフセット（vertCount 個。0xffffffff で壁）
    u8  area      = kWalkableArea;
    f32 center[3]{ 0, 0, 0 };
};

struct NavHeightSample
{
    u16 y    = 0;             // bmin.y からの高さ（cellHeight 単位）
    u16 poly = kMeshNullIdx;  // 属するポリゴン
};

struct NavGridCell
{
    i32 first = -1;
    u16 count = 0;
};

class NavMesh
{
public:
    void Clear();
    bool Empty() const { return m_polys.empty(); }

    // ---- 構築（NavBuilder が呼ぶ）----
    void BuildFromPolyMesh(const NavPolyMeshRaw& pm, const NavCompactHeightfield& chf,
                           const NavBuildConfig& cfg);

    // ---- 参照 ----
    const std::vector<NavPoly>& Polys()     const { return m_polys; }
    const std::vector<f32>&     Verts()     const { return m_verts; }      // xyz * n
    const std::vector<u32>&     PolyVerts() const { return m_polyVerts; }  // 頂点インデックス列
    const std::vector<u32>&     Neis()      const { return m_neis; }
    i32  PolyCount() const { return static_cast<i32>(m_polys.size()); }
    i32  VertCount() const { return static_cast<i32>(m_verts.size() / 3); }
    void GetBounds(f32 outMin[3], f32 outMax[3]) const;
    const NavBuildConfig& Config() const { return m_cfg; }

    // ポリゴンの頂点をワールド座標で取り出す（i は 0..vertCount-1）
    void GetPolyVert(const NavPoly& p, u32 i, f32 out[3]) const;

    // ---- 世代と polyRef ----
    u32        Generation() const { return m_generation; }
    bool       IsValidPoly(i32 poly) const { return poly >= 0 && poly < static_cast<i32>(m_polys.size()); }
    NavPolyRef MakeRef(i32 poly) const;
    i32        DecodeRef(NavPolyRef ref) const;   // 無効 / 古い世代なら -1

    // ---- 幾何の小道具（xz 平面）----
    // 点がポリゴンの内側か。tol > 0 なら縁から tol 以内の外側も内側とみなす。
    bool PointInPoly(i32 poly, const f32 pos[3], f32 tol = 0.0f) const;
    // ポリゴン上の最近点（xz。高さはナビ面から引く）。戻り値 = xz 距離（内側なら 0）
    f32  ClosestPointOnPoly(i32 poly, const f32 pos[3], f32 out[3]) const;
    // 点をポリゴンの内側へ寄せ、どの辺からも eps 以上離す（辺ちょうどに載せない）
    void NudgeInside(i32 poly, f32 pos[3], f32 eps) const;
    // 辺 edge（poly の edge 番目 → 次の頂点）の、ポリゴン内側を向く xz 法線
    void EdgeInwardNormal(i32 poly, u32 edge, f32 outN[3]) const;

    // ---- クエリ（NavQuery.cpp）----
    // 位置の近くで最も近い歩行面を探す。ext は探索半径（xz, y, xz）。
    // ★点を含むポリゴンを最優先し（多層なら高さの近い方）、無ければ ext 内で最も近いポリゴン。
    //   outPos は【そのポリゴン上の最近点】（旧実装は隣のセル中心へ飛ばしていた＝ガタつきの原因）。
    i32  FindNearestPoly(const f32 pos[3], const f32 ext[3], f32 outPos[3]) const;
    // ポリゴン上での正確な高さ（高さサンプル格子から。坂道でも誤差はセル高さ以下）
    bool GetHeightAt(const f32 pos[3], i32 poly, f32& outY) const;
    // A* + ファネル。戻り値は経路点数（0 で失敗）。
    i32  FindPath(const f32 start[3], const f32 end[3], const f32 ext[3],
                  std::vector<f32>& outPath, i32 maxPoints = 256) const;
    // A* でポリゴン列（通路 corridor）だけを出す。maxNodes=0 で無制限。
    NavPathStatus FindPolyPath(i32 startPoly, i32 endPoly, const f32 startPos[3], const f32 endPos[3],
                               std::vector<i32>& outPolys, i32 maxNodes = 0) const;
    // 通路（ポリゴン列）の中の最短折れ線（ファネル）。endPos は最後のポリゴン上にあること。
    // outFlags（任意）には kStraightPath* が入る。戻り値 = 点数。
    i32  FindStraightPath(const f32 startPos[3], const f32 endPos[3], const i32* polys, i32 npolys,
                          std::vector<f32>& outPts, std::vector<u8>* outFlags, i32 maxPoints) const;
    // ナビメッシュ上のレイキャスト（4 状態を区別する版）。visited（任意）に辿ったポリゴンを積む。
    // maxVisit=0 ならポリゴン総数まで辿る（＝実質打ち切らない）。
    NavRaycastResult RaycastEx(const f32 start[3], const f32 end[3], i32 startPoly,
                               std::vector<i32>* visited = nullptr, i32 maxVisit = 0) const;
    // 旧 API（互換）。★true は Hit のときだけ。判定不能も false になるので新規コードは RaycastEx を使う。
    bool Raycast(const f32 start[3], const f32 end[3], i32 startPoly,
                 f32& outT, f32 outHitNormal[3], f32 outHitPos[3]) const;
    // 壁で滑らせながら移動先へ寄せる（Detour の moveAlongSurface と同じ方式）。
    // 始点から届くポリゴンだけを辿って「終点に一番近い点」を返すので、壁の向こうへワープしない。
    // 結果は必ず outPoly の内側で、辺から少し内側へ寄せてある（辺ちょうどに載らない）。
    // visited（任意）に始点→終点のポリゴン列を積む（通路の更新に使う）。
    bool MoveAlongSurface(const f32 start[3], const f32 end[3], i32 startPoly,
                          f32 outPos[3], i32& outPoly, std::vector<i32>* visited = nullptr) const;
    // 周囲の壁の辺を集める（群衆回避の壁判定用）。outSegs に 6 float（p,q）ずつ、近い順。
    i32  FindLocalWalls(i32 startPoly, const f32 pos[3], f32 radius, std::vector<f32>& outSegs,
                        i32 maxSegs) const;

    // ---- シリアライズ ----
    bool Save(const std::string& absPath, std::string& err) const;
    bool Load(const std::string& absPath, std::string& err);   // 実ファイル
    bool LoadFromMemory(const u8* data, size_t size, std::string& err);

    // ---- 統計 ----
    struct Stats
    {
        i32 polyCount = 0, vertCount = 0, sampleCount = 0;
        i32 gridW = 0, gridH = 0;
        f32 walkableArea = 0.0f;   // m^2
        f32 buildMs = 0.0f;
        size_t memoryBytes = 0;
    };
    const Stats& GetStats() const { return m_stats; }
    Stats&       MutableStats()   { return m_stats; }

private:
    i32  CellIndex(f32 x, f32 z, i32& cx, i32& cz) const;
    void ComputeStats();
    // ポリゴンの AABB を粗いバケット格子へ登録する（FindNearestPoly の候補集め）。
    // 保存はしない（読み込み / 構築のたびに作り直す派生データ）。
    void BuildPolyIndex();
    void BumpGeneration();
    // 着地点を壁から eps 押し出す（隣のポリゴンへ出たら poly を差し替える）。MoveAlongSurface の着地処理
    bool NudgeFromWalls(i32& poly, f32 pos[3], f32 eps) const;

    // 候補集め用の粗い格子（ポリゴンの AABB が掛かるバケットに登録）
    std::vector<u32> m_bucketStart;   // bw*bh + 1
    std::vector<u32> m_bucketPolys;
    std::vector<f32> m_polyBounds;    // ポリゴンごとに xmin,ymin,zmin,xmax,ymax,zmax
    f32 m_bucketOrigin[2]{ 0, 0 };
    f32 m_bucketSize = 1.0f;
    i32 m_bw = 0, m_bh = 0;
    u32 m_generation = 0;

    std::vector<NavPoly> m_polys;
    std::vector<f32>     m_verts;      // xyz * n（共有頂点）
    std::vector<u32>     m_polyVerts;  // 各ポリゴンの頂点インデックス列（firstVert から vertCount 個）
    std::vector<u32>     m_neis;       // m_polyVerts と同じ並びの隣接ポリゴン。0xffffffff = 壁

    // 高さサンプル格子
    std::vector<NavGridCell>     m_grid;
    std::vector<NavHeightSample> m_samples;
    i32 m_gw = 0, m_gh = 0;
    // 格子の原点/歩幅。★サンプルの復号(bmin.y + y*ch)とセル索引に使うので content 側とは別物。
    // bmax[1] はボクセル上限(0xffff)由来の巨大な値になるので、表示には使わない。
    f32 m_bmin[3]{ 0, 0, 0 }, m_bmax[3]{ 0, 0, 0 };
    // 実際にポリゴンが存在する範囲（UI / MCP が出すのはこちら）
    f32 m_contentMin[3]{ 0, 0, 0 }, m_contentMax[3]{ 0, 0, 0 };
    f32 m_cs = 0.0f, m_ch = 0.0f;

    NavBuildConfig m_cfg;
    Stats          m_stats;
};

// ---- ビルドの進捗/ログ -------------------------------------------------------
struct NavBuildReport
{
    bool        ok = false;
    std::string error;
    std::string stageLog;      // 各段階の要約（UI にそのまま出す）
    f32         totalMs = 0.0f;
    i32 triCount = 0, spanCount = 0, walkableSpans = 0;
    i32 regionCount = 0, contourCount = 0, polyCount = 0;
};

// ---- パイプライン本体（NavBuilder.cpp）---------------------------------------
bool BuildNavMesh(const NavInputGeometry& geom, const NavBuildConfig& cfg,
                  NavMesh& outMesh, NavBuildReport& report);

// 各段階（テストから個別に叩けるように公開）
bool RasterizeInput(const NavInputGeometry& geom, const NavBuildConfig& cfg,
                    NavHeightfield& hf, std::string& err);
void FilterHeightfield(const NavBuildConfig& cfg, NavHeightfield& hf);
bool BuildCompactHeightfield(const NavBuildConfig& cfg, const NavHeightfield& hf,
                             NavCompactHeightfield& chf, std::string& err);
void ErodeWalkableArea(i32 radius, NavCompactHeightfield& chf);
void BuildDistanceField(NavCompactHeightfield& chf);
bool BuildRegions(NavCompactHeightfield& chf, i32 borderSize,
                  i32 minRegionArea, i32 mergeRegionArea, std::string& err);
bool BuildRegionsMonotone(NavCompactHeightfield& chf, i32 borderSize,
                          i32 minRegionArea, i32 mergeRegionArea, std::string& err);
bool BuildContours(const NavCompactHeightfield& chf, f32 maxError, i32 maxEdgeLen,
                   NavContourSet& cset, std::string& err);
bool BuildPolyMesh(const NavContourSet& cset, i32 nvp, NavPolyMeshRaw& pm, std::string& err);

} // namespace nav
} // namespace dx12e
