#pragma once
// ---------------------------------------------------------------------------
// FootIK — 接地補正（足を地面に合わせる）。
//
// 地面へレイキャストして足首の高さを合わせ、届かない側に合わせて腰を下げ、
// 面法線に足の向きを寄せる。地形とスカルプトがあるこのエンジンでは
// 「平らでない地面」が主戦場なので、見た目への寄与が最も大きい。
//
// ⚠️ ここに entt / PhysicsSystem を持ち込まない。
//    ECS が Animation に依存しているので逆向きの依存は循環になるし、
//    レイキャストを差し替え可能にしておけば合成地面で単体テストできる。
//    → 地面問い合わせは FootIKRayCast（コールバック）で受け取る。
//    エンティティの走査と PhysicsSystem::RaycastEx への接続は呼び出し側（Application）の仕事。
//
// 座標系: ボーンのグローバル行列は**モデル空間**。レイは**ワールド空間**で打つので、
//         world / worldInv を渡してもらって内部で往復する。
// ---------------------------------------------------------------------------
#include <functional>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class Skeleton;
class Animator;

// 地面問い合わせ。ヒットしたら true を返し、outPoint / outNormal（ワールド空間）を埋める。
using FootIKRayCast = std::function<bool(const DirectX::XMFLOAT3& origin,
                                         const DirectX::XMFLOAT3& dir,
                                         f32 maxDistance,
                                         DirectX::XMFLOAT3& outPoint,
                                         DirectX::XMFLOAT3& outNormal)>;

// 片足ぶんのボーン index（-1 = 未解決）。
struct FootIKLegBones
{
    i32 hip  = -1;   // 股関節（IK の a）
    i32 knee = -1;   // 膝     （IK の b）
    i32 foot = -1;   // 足首   （IK の c）
    i32 toe  = -1;   // つま先（任意。今は向きの参考にのみ使う）

    bool Valid() const { return hip >= 0 && knee >= 0 && foot >= 0; }
};

struct FootIKBones
{
    FootIKLegBones left;
    FootIKLegBones right;
    i32 pelvis = -1;   // 腰下げの対象（-1 ならルートボーン）
};

// 調整つまみ（FootIK コンポーネントの永続フィールドと 1:1）。
struct FootIKParams
{
    f32 weight         = 1.0f;   // 0..1 全体の効き
    f32 rayUpOffset    = 0.5f;   // 足首から何 m 上からレイを打つか
    f32 rayLength      = 1.0f;   // レイの全長
    f32 footHeight     = 0.10f;  // レストポーズでの足首の地面からの高さ
    f32 maxPelvisDrop  = 0.5f;   // 腰を下げられる上限(m)
    f32 maxFootPitchDeg = 45.0f; // 面法線に合わせる足の傾きの上限(度)
    f32 smoothTime     = 0.10f;  // 足首高さ/腰オフセットの指数平滑の時定数(秒)
    f32 fadeOutTime    = 0.15f;  // 非接地時に IK を切るフェード時間(秒)
    bool alignToNormal = true;   // 面法線に足を合わせるか
    DirectX::XMFLOAT3 kneeForward{0.0f, 0.0f, 1.0f};  // 膝が向く方向（モデル空間）
};

// フレームをまたぐ状態（平滑化とフェードのため）。
struct FootIKState
{
    f32  leftLift    = 0.0f;   // 平滑済みの足首の上げ下げ量(m、モデル空間)
    f32  rightLift   = 0.0f;
    f32  pelvisDrop  = 0.0f;   // 平滑済みの腰下げ量(m、0 以下)
    f32  leftWeight  = 0.0f;   // フェード済みの片足ごとの効き
    f32  rightWeight = 0.0f;
    DirectX::XMFLOAT3 leftNormal {0.0f, 1.0f, 0.0f};
    DirectX::XMFLOAT3 rightNormal{0.0f, 1.0f, 0.0f};
    bool leftContact  = false;
    bool rightContact = false;
    bool initialized  = false;
};

// ボーン名からボーン index を解決する。
// 名前が空なら一般的な命名（mixamorig:LeftFoot / foot.L / Bip01 L Foot / LeftUpLeg 等）から推定する。
// 1 本でも解決できなかった脚は Valid() が false になる。
struct FootIKBoneNames
{
    std::string leftHip, leftKnee, leftFoot, leftToe;
    std::string rightHip, rightKnee, rightFoot, rightToe;
    std::string pelvis;
};
bool ResolveFootIKBones(const Skeleton& skeleton, const FootIKBoneNames& names, FootIKBones& out);

// 1 体ぶんのフット IK を適用する。
//   animator  ポーズとグローバル行列を持つ（この関数が書き換えて再計算する）
//   world     エンティティのワールド行列（モデル空間 → ワールド空間）
//   grounded  キャラが接地しているか（false なら IK を weight 0 へフェード）
// 何もできないとき（ボーン未解決 / weight 0 / ray が null）は false を返して何も触らない。
bool ApplyFootIK(Animator& animator, const Skeleton& skeleton,
                 DirectX::FXMMATRIX world,
                 const FootIKBones& bones, const FootIKParams& params,
                 FootIKState& state, const FootIKRayCast& ray,
                 f32 dt, bool grounded);

} // namespace dx12e
