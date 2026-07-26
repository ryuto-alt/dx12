#pragma once
// ---------------------------------------------------------------------------
// AnimPose — スケルタルアニメの中間表現（ローカル空間 TRS 配列）。
//
// 従来 Animator は「クリップ → いきなり最終スキニング行列」だったため、
// クロスフェードが最終行列の要素ごと線形補間になっていた（＝回転が保存されず、
// 遷移のたびに骨が縮む/シアーする）。ここに「ローカル TRS ポーズ」という段階を
// 挟むことで、位置=lerp / 回転=slerp / スケール=lerp が正しく書ける。
//
//   [clip A] --Sample--> Pose_A --\
//   [clip B] --Sample--> Pose_B ---> Blend(w) --> Pose --> FK --> global[] --> IK --> skinning[]
//
// この中間表現は IK / ブレンドツリー / レイヤー / ラグドールの全部が必要とする。
//
// 依存は DirectXMath + core/Types.h + animation/{Skeleton,AnimationClip}.h だけ
// （GPU / ECS / Jolt に一切依存しない）。tests/ から直接ビルドして検証できる。
//
// ⚠️ 行列規約: ローカル行列は S * R * T（行ベクトル規約）、
//    グローバルは local * global[parent]、最後に XMMatrixTranspose して
//    HLSL の列優先へ渡す。**この規約は絶対に崩さないこと**（崩すと全キャラが壊れる）。
// ---------------------------------------------------------------------------
#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class Skeleton;
class AnimationClip;
template<typename T> struct Keyframe;

// ローカル空間 1 ボーンぶんの TRS。
struct BoneTRS
{
    DirectX::XMFLOAT3 t{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT4 r{0.0f, 0.0f, 0.0f, 1.0f};  // quaternion XYZW
    DirectX::XMFLOAT3 s{1.0f, 1.0f, 1.0f};
};

// ボーン index と 1:1 に並んだローカルポーズ。
using AnimPose = std::vector<BoneTRS>;

// ---------------------------------------------------------------------------
// キーフレーム補間（Animator の private メソッドから移設した純関数）
// ---------------------------------------------------------------------------

// キー列から time の値を線形補間で取り出す。keys が空なら fallback。
DirectX::XMFLOAT3 SampleVec3Track(const std::vector<Keyframe<DirectX::XMFLOAT3>>& keys,
                                  float time, DirectX::XMFLOAT3 fallback);

// キー列から time の四元数を slerp で取り出す。keys が空なら単位四元数。
DirectX::XMFLOAT4 SampleQuatTrack(const std::vector<Keyframe<DirectX::XMFLOAT4>>& keys,
                                  float time);

// ---------------------------------------------------------------------------
// ポーズの生成・合成
// ---------------------------------------------------------------------------

// スケルトンのバインドポーズ（各ボーンの localBindPose を分解した TRS）を out に書く。
void MakeBindPose(const Skeleton& skeleton, AnimPose& out);

// clip を time でサンプルして out（ローカル TRS）に書く。
// トラックの無いボーンはバインドポーズの TRS で埋める（従来の localBindPose 相当）。
void SamplePose(const AnimationClip& clip, float time, const Skeleton& skeleton, AnimPose& out);

// out = lerp(a, b, t)。T/S は lerp、R は slerp（XMQuaternionSlerp が最短弧を選ぶ）。
// a と b のサイズが違うときは小さい方に合わせる。out は a と同じサイズになる。
void BlendPose(const AnimPose& a, const AnimPose& b, float t, AnimPose& out);

// dst = lerp(dst, src, w)。N-way の逐次ブレンド用。
// 四元数は「内積が負なら src を反転してから」補間する（遠回りの弧を通らないため）。
void BlendPoseAccum(AnimPose& dst, const AnimPose& src, float w);

// 加算レイヤー: dst += (add - ref) * w。
//   回転: q_dst = q_dst * slerp(identity, q_add * inverse(q_ref), w)
//   位置/スケール: p_dst = p_dst + (p_add - p_ref) * w
// mask が非 null なら mask[i]（0..1）を w に乗じる。mask の要素数はボーン数以上であること。
void AdditiveBlendPose(AnimPose& dst, const AnimPose& add, const AnimPose& ref,
                       float w, const float* mask);

// Override レイヤー: dst = lerp(dst, src, w * mask[i])。回転は slerp。
void OverrideBlendPose(AnimPose& dst, const AnimPose& src, float w, const float* mask);

// BlendPose の in-place 版（dst = lerp(dst, src, t)、回転は slerp）。
// クロスフェードで一時バッファを 1 本減らすために使う。
inline void BlendPoseInPlace(AnimPose& dst, const AnimPose& src, float t)
{
    OverrideBlendPose(dst, src, t, nullptr);
}

// ---------------------------------------------------------------------------
// FK / スキニング
// ---------------------------------------------------------------------------

// 1 ボーンぶんの TRS → ローカル行列（S * R * T）。
DirectX::XMMATRIX BoneTRSToMatrix(const BoneTRS& b);

// ローカルポーズ → グローバル行列（global[i] = local[i] * global[parent]）。
// ⚠️ 親が子より前に並んでいることを前提とした単一ループ
//    （Skeleton::AreBonesCorrectlyOrdered() で検証できる）。
void ComputeGlobalMatrices(const Skeleton& skeleton, const AnimPose& pose,
                           std::vector<DirectX::XMFLOAT4X4>& out);

// グローバル行列 → スキニング行列（invBind * global、最後に転置して HLSL の列優先へ）。
void GlobalToSkinning(const Skeleton& skeleton,
                      const std::vector<DirectX::XMFLOAT4X4>& globals,
                      std::vector<DirectX::XMFLOAT4X4>& out);

// ---------------------------------------------------------------------------
// 小物（ヘッダ inline）
// ---------------------------------------------------------------------------

inline DirectX::XMFLOAT4 NormalizeQuat(DirectX::XMFLOAT4 q)
{
    using namespace DirectX;
    XMVECTOR v = XMLoadFloat4(&q);
    // 完全なゼロ四元数は単位へ落とす（0 除算・NaN 回避）
    if (XMVectorGetX(XMVector4LengthSq(v)) < 1e-12f)
        return XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionNormalize(v));
    return out;
}

} // namespace dx12e
