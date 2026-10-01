#pragma once
//
// GiMigration — GI モード「新」の既定構成を作る / 旧との切り替え（GI_FOUNDATION_DESIGN §2 S5）。
//
// 使う場所は 4 つで、どれも同じ関数を通る（構成がずれないように 1 か所へ集約）:
//   ・新規シーン（エディタの「新しいシーン」/ MCP new_scene / ランチャーの新規プロジェクト）
//   ・ライティング窓の「新しい GI に切り替える」「旧に戻す」ボタン（1 操作 = Undo 1 回）
//   ・MCP migrate_gi
//   ・テンプレートの JSON に焼く DDGI 格子の値の検算（tests/gi_migration_test.cpp）
//
// ★既存シーンは勝手に移行しない。JSON に gi キーが無いシーンは Legacy のまま読み、保存しても gi キーを書かない
//   （SceneSerializer の既存動作）。ここの関数を呼ぶのは「人が押した / 新規作成した」ときだけ。
// ★DdgiVolume.* の中身には触らない。DdgiSettings の値を計算して入れるだけ（S4 のカメラ追従ボリュームが入るまでの簡易版）。
//
#include <string>
#include <utility>
#include <vector>

#include <DirectXMath.h>
#include <entt/entt.hpp>

#include "core/Types.h"
#include "renderer/DdgiVolume.h"
#include "renderer/RtSettings.h"
#include "renderer/SsgiSettings.h"

namespace dx12e
{

class Scene;

namespace gi
{

// ワールド AABB（箱 1 個）。
struct Box
{
    DirectX::XMFLOAT3 mn{0, 0, 0};
    DirectX::XMFLOAT3 mx{0, 0, 0};
    bool isDynamic = false;   // 動く物（Dynamic な剛体）。格子の範囲には数えない
};

struct Bounds
{
    DirectX::XMFLOAT3 mn{0, 0, 0};
    DirectX::XMFLOAT3 mx{0, 0, 0};
    bool valid = false;
};

// 自動フィットの定数。
constexpr float kFitSpacing      = 1.0f;    // 既定のプローブ間隔(m)。4096 個に収まらなければ 0.25m 刻みで広げる
constexpr float kFitSpacingStep  = 0.25f;
constexpr float kFitMinHeight    = 4.0f;    // 平らな床だけのシーンでも、人の背丈より上までプローブを置く高さ(m)
constexpr float kFitMinWidth     = 8.0f;    // 水平の最小の広さ(m)。小さな物 1 個だけでもこの幅は覆う
constexpr float kFitHugeExtent  = 100.0f;  // 水平にこれを超える箱は「背景（巨大な床・地形）」とみなす(m)
constexpr float kFitHugeClamp    = 100.0f;   // 巨大な箱しか無いとき、中心のまわりに切り取る一辺(m)
constexpr f32   kUndoAmbientBack = 0.25f;   // 旧へ戻すとき、ambient が 0 の太陽へ入れる値（DirectionalLight の既定）

// 箱の集合から格子の対象範囲を決める（純関数）。
//   動かない物だけを数える。水平に kFitHugeExtent を超える箱（エディタ用の巨大な床・広い地形）は、他に箱があれば無視する。
//   動かない物が無ければ動く物も数える。箱が 1 個も無ければ valid=false。
Bounds ChooseBounds(const std::vector<Box>& boxes);

// 範囲を覆う DDGI の格子を計算して返す（純関数）。base の格子以外の項目（intensity / hysteresis など）は引き継ぐ。
//   間隔は kFitSpacing から始め、軸ごとの個数 <= 32 かつ総数 <= DdgiVolume::kMaxProbes になるまで広げる。
//   範囲の外へ 0.5 間隔ぶん張り出し、縦は kFitMinHeight 以上を確保する。格子は範囲の中心に揃える。
//   enabled は true にして返す。bounds が無効なら部屋 1 つぶんの既定の範囲（±8m・高さ 4m）を使う。
DdgiSettings FitDdgiToBounds(const DdgiSettings& base, const Bounds& bounds);

// シーンの動かない MeshRenderer の AABB を集める（エディタ用グリッド・動く剛体は除く）。
std::vector<Box> CollectStaticBoxes(const entt::registry& reg);

// 切り替えで触る値の全部（Undo 用の前後スナップショット）。
struct State
{
    GiSettings   gi;
    DdgiSettings ddgi;
    SsgiSettings ssgi;
    RtSettings   rt;
    std::vector<std::pair<entt::entity, f32>> ambient;   // 平行光源の ambient
};

State Capture(const Scene& scene);
void  Restore(Scene& scene, const State& state);   // gi / ddgi / ssgi / rt / 各太陽の ambient を書き戻す（DDGI の履歴を捨てるのは呼び出し側）

struct Options
{
    bool dxrSupported = true;   // false なら「新」へは切り替えない（理由を返す）
    const char* whyNot = nullptr;   // dxrSupported=false のときの理由（標準語。nullptr なら DXR 非対応の定型文）
    bool refitGrid    = true;   // true: 格子を毎回フィットし直す / false: DDGI が既に ON なら今の格子（手置き）を残す
};

struct Result
{
    bool         applied = false;   // 何か書き換えたか（false = 非対応 / 既にその状態）
    bool         fitted  = false;   // 格子を計算し直したか
    bool         fromGeometry = false;   // 格子の範囲がシーンのジオメトリ由来か（false = 既定の範囲）
    int          ambientChanged = 0;   // 値を変えた太陽の数（新: 0 にした数 / 旧: 戻した数）
    DdgiSettings ddgi;
    std::string  reason;            // applied=false の理由 / 補足（標準語）
};

// 新規シーン・移行ボタン共通: 「新」の既定構成を当てる。
//   gi.mode = New / DDGI ON（格子は自動フィット・bounce 1.0）/ 平行光源の ambient = 0 / SSGI ON / RT 影 ON。
//   dxrSupported=false のときは何も書き換えず reason に理由を入れる（= 旧のまま）。
Result ApplyNew(Scene& scene, const Options& opt);

// 「旧」へ戻す: gi.mode = Legacy / DDGI OFF / ambient が 0 の太陽は kUndoAmbientBack へ。SSGI・RT 影は触らない。
//   直前の状態へ正確に戻したいときは Undo を使う（こちらは「旧の既定へ戻す」操作）。
Result ApplyLegacy(Scene& scene);

// DDGI の格子だけをシーンのジオメトリへ合わせ直す（ジオメトリを足した後の「範囲を合わせ直す」）。
//   モード・ambient・SSGI・RT 影は触らない。DDGI は ON にして返す。
Result RefitGrid(Scene& scene);

// 画面表示用の 1 行（"旧" / "新" + DDGI の状態）。
const char* ModeLabel(GiMode m);

} // namespace gi
} // namespace dx12e
