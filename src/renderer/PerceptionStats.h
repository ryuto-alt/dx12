#pragma once
// ===========================================================================
// 知覚層（dx12_perceive）の集計ロジック。GPU から切り離した純関数。
// ---------------------------------------------------------------------------
// ★なぜ要るか
//   Jev（判断モデル）は画像を見られない。エンジンが「プレイヤーの目から見た事実」を
//   数値で出し、TS 側（tools/mcp-server/perceive.ts）が言葉に直してから渡す。
//   ここはその数値を作る所。JUNCTION で「机上検査は全部通るのに焦点に立つと分かる欠陥」
//   （破片が真っ黒な板 / 深さが見えない / 仕掛けが画面を埋める）を数字で拾うのが最初の狙い。
//
// ★入力は「1 フレームぶんの画素の配列」だけ:
//     ids      … エンティティ ID バッファ（R32_UINT）。0 = 何も描かれていない（空 / クリア色）
//     rgba     … 同じカメラの最終画（ポスト・トーンマップ後の表示色 = sRGB ガンマ済み RGBA8）
//     posDist  … ワールド座標 xyz + カメラからの距離（任意）
//     normal   … フォワードと同じシェーディング法線（ワールド。カメラ側へは裏返さない。任意）
//   GPU（src/core/mcp/ApplicationMcpPerceive.cpp の ID パス）はこれを作って読み戻すだけで、
//   指標の定義はすべてこのファイルにある。単体テストは tests/perception_stats_test.cpp。
//
// ★指標の定義（応答 JSON のキー名と同じ。perceive.ts の言葉の境界もこれを前提にしている）
//   pixels      … そのエンティティ（グループなら構成員の和集合）の ID を持つ画素数
//   share       … pixels / 全画素
//   bbox        … 可視画素の外接矩形 [x0,y0,x1,y1]。0..1 に正規化（x 右向き・y 下向き）
//   center      … 可視画素の重心 [x,y]（同上）
//   luma        … 可視画素の Rec.709 輝度 Y' = 0.2126R'+0.7152G'+0.0722B'（ガンマ済みの値、0..1）の平均
//   lumaStd     … 同じ Y' の標準偏差（面の中に模様・陰影がどれだけあるか。小さい＝のっぺり）
//   lumaRing    … 外接矩形を margin 画素だけ広げた枠のうち、元の矩形の外側で、かつ自分以外の
//                  画素の Y' 平均。margin = max(2, round(ringMarginFrac * max(矩形の幅, 高さ)))。
//                  枠が画面外に出た分は数えない。数えた画素が kMinRingPixels 未満なら「無し」
//   contrast    … (luma + 0.05) / (lumaRing + 0.05)。1 より大きい＝周囲より明るい、小さい＝暗い
//   saturation  … 可視画素の HSV 彩度 (max-min)/max の平均（0..1）
//   distance    … 可視画素のカメラからの距離（ユークリッド, m）の平均。distanceMin は最小
//   fullyInView … ワールド AABB の 8 隅をカメラの viewProj で投影し、全部が w>0 かつ
//                  NDC の x,y が [-1,1] に入るか。projectedExtent は投影矩形の幅・高さを
//                  画面の幅・高さで割った値（1 を超えたら画面からはみ出す大きさ）。
//                  隅がカメラの後ろにあると投影できないので null
//   occlusion   … 「遮蔽物が無ければ映っていたはずの画素（isolated）」のうち、実際には見えていない割合。
//                  1 - pixels / isolatedPixels。isolated は対象だけを深度なしで描いた被覆マスク。
//                  画面の外にはみ出した分は含まない（そちらは fullyInView）。マスクが無い対象は null
//   litFacing   … 光源（平行光・点光源・スポット）の【影を無視した】直接光がその面に届く量のうち、
//                  見えている画素のシェーディング法線 N の側に当たっている割合（N は下の backFacing の注を参照）。
//                  画素ごとに E+ = Σ r·max(0, N·L)、E- = Σ r·max(0, -N·L) を足し、
//                  litFacing = ΣE+ / (ΣE+ + ΣE-)。r はシェーダと同じ減衰（(1-d/range)^2・コーン^2）の輝度。
//                  1 に近い＝灯りはプレイヤー側から当たっている / 0 に近い＝灯りが裏側（見えている面は影）。
//                  どの灯りも届いていなければ unlit=true・litFacing=null。★影（遮蔽）と環境光・IBL・自己発光は見ない
//   mainLight   … その対象へ届く直接光（|N·L| 重み）が最大の光源の名前と、その光源について
//                  見えている側に当たっている割合 facing（litFacing と同じ式を 1 灯だけで取ったもの）
//   backFacing  … 見えている画素のうち、法線がカメラと反対を向いている（＝裏面が見えている）割合。
//                  ★N はフォワードと同じシェーディング法線（頂点法線。法線マップは見ない）で、カメラ側へ
//                  裏返さない。フォワードは両面描画で裏面も表の法線のまま照らすので、裏面が見えていると
//                  灯りが手前にあっても暗くなる。litFacing もこの N で数える＝画面の明るさと同じ理屈になる
//
//   シーン全体（scene）:
//     empty       … id==0（何も描かれていない＝空・クリア色）の割合。全体と上下左右の半分ごと
//     regions     … 上下左右の半分ごとに {empty, luma, lumaStd, distance(描かれた画素の平均距離)}
//     luma        … 全画素の Y' の {mean, p5, p50, p95, crushed(Y'<=kCrushedLuma の割合), clipped(Y'>=kClippedLuma の割合)}
//     farthest    … 描かれた画素の距離の最大（m）。描かれた画素が無ければ null
//     visibleEntities … 1 画素以上見えているエンティティの数
//
// ★半透明（ガラス・JUNCTION の「未完成の破片」の幽霊表示など）は既定で ID パスに【手前の面】として
//   描く（不透明の後・深度テストあり・深度を書く）。その画素の ID は半透明の物になり、奥の物は
//   「隠れた」扱いになる。luma は最終画（半透明が混ざった色）なので「そこに見えている色」のまま。
//   要求の includeTransparent:false で従来の深度プリパスと同じく除外もできる。
// ===========================================================================

#include "core/Types.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace dx12e::perception
{

constexpr float kCrushedLuma   = 0.02f;   // これ以下は黒潰れ（8bit で 5 前後）
constexpr float kClippedLuma   = 0.98f;   // これ以上は白飛び（8bit で 250 前後）
constexpr u32   kMinRingPixels = 8;       // 周囲リングがこれ未満なら lumaRing は無し
constexpr float kContrastEps   = 0.05f;   // contrast の分母・分子に足す量（真っ黒どうしで発散させない）

struct Vec3 { float x = 0, y = 0, z = 0; };

// 1 フレームぶんの画素。ポインタは呼び出し側が所有する（Analyze の間だけ有効であればよい）。
struct PixelFrame
{
    u32          width  = 0;
    u32          height = 0;
    const u32*   ids     = nullptr;   // w*h。0 = 何も描かれていない
    const u8*    rgba    = nullptr;   // w*h*4。表示色（sRGB ガンマ済み）
    const float* posDist = nullptr;   // w*h*4（任意）。world xyz + カメラからの距離
    const float* normal  = nullptr;   // w*h*4（任意）。シェーディング法線（ワールド, xyz。w は未使用）
};

// ID ごとのメタ情報。添字 = ID（[0] は「何も無い」なので未使用）。
struct EntityMeta
{
    std::string name;
    std::string parent;          // 直近の親の名前（無ければ空）
    bool        hasAabb = false;
    Vec3        aabbMin, aabbMax;
    bool        transparent = false;   // 半透明（BLEND / shaderAlphaBlend）として ID パスに描いた
};

// 光源。radiance はシェーダと同じ「色 × 強度」の輝度（Rec.709 の重みで畳んだ 1 値）。
struct Light
{
    enum Type { Directional = 0, Point = 1, Spot = 2 };
    int         type = Point;
    std::string name;
    Vec3        position;          // point / spot
    Vec3        direction;         // directional / spot（光が進む向き。正規化済み）
    float       radiance = 0.0f;
    float       range    = 10.0f;
    float       cosInner = 1.0f;   // spot
    float       cosOuter = -1.0f;  // spot
};

// 名前で指定された対象。ids は構成員（自分 + 子孫）の ID。
struct Group
{
    std::string      name;
    std::vector<u32> ids;
    bool             hasAabb = false;
    Vec3             aabbMin, aabbMax;
    const u8*        isolatedMask = nullptr;   // w*h（任意）。非 0 = 遮蔽物が無ければ映る画素
};

struct CameraInfo
{
    float viewProj[16] = {};   // DirectXMath の行ベクトル規約（clip = [x y z 1] * M）
    Vec3  position;
};

struct Options
{
    u32   top            = 8;       // 画面占有の上位何件を返すか
    float ringMarginFrac = 0.15f;   // 周囲リングの幅（外接矩形の長辺に対する比）
};

struct Stats
{
    std::string name;
    std::string parent;
    u32   id       = 0;       // 単体なら ID、グループなら 0
    u32   members  = 1;       // グループの構成員数（見えていない物も含む）
    u32   transparentMembers = 0;   // そのうち半透明の数（単体なら 0 か 1）
    u32   pixels   = 0;
    float share    = 0.0f;
    std::array<float, 4> bbox{};     // x0,y0,x1,y1（正規化。pixels==0 なら 0）
    std::array<float, 2> center{};
    float luma     = 0.0f;
    float lumaStd  = 0.0f;
    std::optional<float> lumaRing;
    std::optional<float> contrast;
    u32   ringPixels = 0;
    float saturation = 0.0f;
    std::optional<float> distance;
    std::optional<float> distanceMin;
    std::optional<bool>  fullyInView;
    std::optional<std::array<float, 2>> projectedExtent;
    std::optional<u32>   isolatedPixels;
    std::optional<float> occlusion;
    std::optional<float> litFacing;
    bool  unlit = false;           // 位置と法線があり、どの灯りも届いていない
    std::string mainLight;         // 空 = 無し
    std::optional<float> mainLightFacing;
    std::optional<float> backFacing;
};

struct RegionStats
{
    float empty   = 0.0f;
    float luma    = 0.0f;
    float lumaStd = 0.0f;
    std::optional<float> distance;   // 描かれた画素の平均距離
};

struct SceneStats
{
    u32   width = 0, height = 0;
    float empty = 0.0f;
    RegionStats top, bottom, left, right;
    float lumaMean = 0.0f, lumaP5 = 0.0f, lumaP50 = 0.0f, lumaP95 = 0.0f;
    float crushed = 0.0f, clipped = 0.0f;
    std::optional<float> farthest;
    u32   visibleEntities = 0;
};

struct Result
{
    SceneStats         scene;
    std::vector<Stats> top;       // 画面占有の多い順
    std::vector<Stats> targets;   // Group の順
};

// ---- 部品（テストから個別に叩けるよう公開してある）----
// Y' = 0.2126R' + 0.7152G' + 0.0722B'（入力は 0..255 のガンマ済み値。返り値 0..1）
float LumaOf(u8 r, u8 g, u8 b);
// HSV の彩度 (max-min)/max（0..1。真っ黒は 0）
float SaturationOf(u8 r, u8 g, u8 b);
// AABB の 8 隅を投影して「全部画面に入るか」と投影矩形の大きさ（画面比）。
// 隅がカメラの後ろ（w<=0）なら extent は無し・fullyInView=false。
void ProjectAabb(const float viewProj[16], const Vec3& mn, const Vec3& mx,
                 bool& fullyInView, std::optional<std::array<float, 2>>& extent);
// 1 画素ぶんの直接光（影なし）。N はシェーディング法線（front = N 側 / back = 反対側）へ加算する。
// 返り値はこの光源の寄与（r·|N·L|）。
float AccumulateLight(const Light& L, const Vec3& worldPos, const Vec3& N, float& front, float& back);

// 本体。groups の isolatedMask は任意。
Result Analyze(const PixelFrame& frame, const std::vector<EntityMeta>& meta,
               const std::vector<Group>& groups, const std::vector<Light>& lights,
               const CameraInfo& camera, const Options& opt);

} // namespace dx12e::perception
