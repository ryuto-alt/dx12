#pragma once
// ---------------------------------------------------------------------------
// TwoBoneIK — 2 ボーン解析 IK（股関節→膝→足首 / 肩→肘→手首）。
//
// 出典: Daniel Holden "Simple Two Joint IK"
//       https://theorangeduck.com/page/simple-two-joint
// 原文のコードリストを DirectXMath へ移植したもの。加えて、ozz-animation の
// IKTwoBoneJob から 2 点だけ改良を取り込んでいる（下記 ★）。
//
// ⚠️ DirectXMath の四元数積は**引数順が逆**:
//      XMQuaternionMultiply(Q1, Q2) は「Q2 * Q1」を返す（行列の連結順に合わせた仕様）。
//    原文の quat_mul(a, b)（= 数学記法の a * b）は XMQuaternionMultiply(b, a) と書く。
//    ここを取り違えると腕が裏返る。
//
// ⚠️ 原文の `quat_mul(quat_inv(q), v)` は**四元数×ベクトル**（＝軸をローカル空間へ回す）。
//    四元数同士の積ではない。DirectXMath では XMVector3InverseRotate(v, q)。
//
// 依存は DirectXMath のみ。tests/ から直接ビルドして検証できる。
// ---------------------------------------------------------------------------
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include "core/Types.h"

namespace dx12e
{

// 2 ボーン IK を解く。
//   a, b, c   股関節 / 膝 / 足首 の**ワールド（モデル）空間位置**
//   target    足首を持っていきたいワールド位置
//   hint      膝の向きのヒント（極ベクトル）。ワールド空間。
//             長さ 0 なら cross(c-a, b-a) を使う（原文の既定）。
//             ★脚が伸び切っていると cross(c-a, b-a) が退化して膝がパタつくので、
//               フット IK では「キャラの前方向」を渡すのを推奨（原文 Bonus 節）。
//   aGlobalRot / bGlobalRot   a / b の**グローバル回転**（補正前の元ポーズのもの）
//   aLocalRot / bLocalRot     a / b の**ローカル回転**（入出力。右から補正を掛ける）
//   eps       到達可能距離のクランプ余裕（既定 1e-2）
//
// すべて元ポーズ 1 回ぶんの値から計算する（途中で FK をやり直さない）。
// 呼び出し後に FK を回すと足首が target に乗る。
inline void SolveTwoBoneIK(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b, DirectX::FXMVECTOR c,
                           DirectX::FXMVECTOR target, DirectX::GXMVECTOR hint,
                           DirectX::HXMVECTOR aGlobalRot, DirectX::HXMVECTOR bGlobalRot,
                           DirectX::XMVECTOR& aLocalRot, DirectX::XMVECTOR& bLocalRot,
                           float eps = 1.0e-2f)
{
    using namespace DirectX;

    const XMVECTOR ab = XMVectorSubtract(b, a);
    const XMVECTOR cb = XMVectorSubtract(b, c);
    const XMVECTOR ca = XMVectorSubtract(c, a);
    const XMVECTOR at = XMVectorSubtract(target, a);

    const float lab = XMVectorGetX(XMVector3Length(ab));
    const float lcb = XMVectorGetX(XMVector3Length(cb));

    // ボーン長が 0 なら何もできない（NaN を出さずに素通し）
    if (lab < 1.0e-6f || lcb < 1.0e-6f) return;

    // ★ozz 由来の改良その 1★
    //   原文は下限を eps にしているが、正しい下限は |lab - lcb|
    //   （2 本の長さが違うと、それ以上は折り畳めない）。等長なら従来どおり eps。
    const float minReach = (std::max)(std::fabs(lab - lcb), eps);
    const float maxReach = lab + lcb - eps;
    const float latRaw   = XMVectorGetX(XMVector3Length(at));
    const float lat      = std::clamp(latRaw, minReach, (std::max)(maxReach, minReach));

    auto SafeAcosDot = [](FXMVECTOR u, FXMVECTOR v) -> float {
        const XMVECTOR nu = XMVector3Normalize(u);
        const XMVECTOR nv = XMVector3Normalize(v);
        const float d = std::clamp(XMVectorGetX(XMVector3Dot(nu, nv)), -1.0f, 1.0f);
        return std::acos(d);
    };

    // 現在の角度
    const float ac_ab_0 = SafeAcosDot(ca, ab);
    const float ba_bc_0 = SafeAcosDot(XMVectorNegate(ab), XMVectorSubtract(c, b));
    const float ac_at_0 = SafeAcosDot(ca, at);

    // 目標の角度（余弦定理）
    const float ac_ab_1 = std::acos(std::clamp(
        (lcb * lcb - lab * lab - lat * lat) / (-2.0f * lab * lat), -1.0f, 1.0f));
    const float ba_bc_1 = std::acos(std::clamp(
        (lat * lat - lab * lab - lcb * lcb) / (-2.0f * lab * lcb), -1.0f, 1.0f));

    // 曲げ平面の法線。
    // 原文の既定は cross(c-a, b-a) だが、脚が伸び切っているとこれが退化して
    // 膝がパタつく（原文 Bonus 節）。ヒントがあれば cross(c-a, hint) を使う。
    //
    // ⚠️ ただし**符号を現在の曲がり方に合わせる**こと。
    //    ヒントの向きだけで符号を決めると、アニメが既に曲げている側と逆向きの
    //    平面法線になり、膝が逆に曲がって目標に届かなくなる（実装時に踏んだ）。
    //    伸び切っていて現在の曲げ平面が求まらないときだけ、ヒントの符号がそのまま
    //    「膝がどちらへ出るか」を決める＝これが本来ヒントに期待する役割。
    // ⚠️ ヒント由来の軸は**近似**でしかない（ヒント平面が実際の曲げ平面と一致しないと
    //    三角形の作り直しが厳密でなくなる）。なので:
    //      ・十分に曲がっている  → 実際の曲げ平面 cross(c-a, b-a) を使う（厳密）
    //      ・ほぼ伸び切っている  → ヒントを使う（近似だが安定。ここが原文 Bonus 節の狙い）
    //    しきい値は sin(角) = |cross| / (|ca||ab|) で見る。
    const XMVECTOR bendAxis = XMVector3Cross(ca, ab);
    const float bendSin = XMVectorGetX(XMVector3Length(bendAxis))
                        / (XMVectorGetX(XMVector3Length(ca)) * lab + 1.0e-9f);
    const bool bendWellConditioned = bendSin > 0.05f;   // 約 3 度以上曲がっている

    const XMVECTOR hintAxis = XMVector3Cross(ca, hint);
    const bool hintValid = XMVectorGetX(XMVector3LengthSq(hint)) > 1.0e-8f
                        && XMVectorGetX(XMVector3LengthSq(hintAxis)) > 1.0e-10f;

    XMVECTOR axis0;
    if (bendWellConditioned)
    {
        axis0 = XMVector3Normalize(bendAxis);
    }
    else if (hintValid)
    {
        axis0 = XMVector3Normalize(hintAxis);
        // 現在わずかに曲がっているなら、その側を保つ（膝が突然裏返らないように）
        if (XMVectorGetX(XMVector3LengthSq(bendAxis)) > 1.0e-12f &&
            XMVectorGetX(XMVector3Dot(axis0, XMVector3Normalize(bendAxis))) < 0.0f)
        {
            axis0 = XMVectorNegate(axis0);
        }
    }
    else if (XMVectorGetX(XMVector3LengthSq(bendAxis)) > 1.0e-12f)
    {
        axis0 = XMVector3Normalize(bendAxis);
    }
    else
    {
        return;   // 平面が決められない（完全に退化）
    }

    XMVECTOR axis1 = XMVector3Cross(ca, at);
    const bool haveAxis1 = XMVectorGetX(XMVector3LengthSq(axis1)) > 1.0e-10f;
    if (haveAxis1) axis1 = XMVector3Normalize(axis1);

    // 軸を各関節のローカル空間へ持ち込む（原文 quat_mul(quat_inv(gr), axis)）
    const XMVECTOR axis0InA = XMVector3InverseRotate(axis0, aGlobalRot);
    const XMVECTOR axis0InB = XMVector3InverseRotate(axis0, bGlobalRot);

    const XMVECTOR r0 = XMQuaternionRotationAxis(axis0InA, ac_ab_1 - ac_ab_0);
    const XMVECTOR r1 = XMQuaternionRotationAxis(axis0InB, ba_bc_1 - ba_bc_0);

    // ★合成順★ ワールド空間では「まず R0 で三角形を作り直し（a→c の向きは変わらない）、
    //   そのあと R2 で a→c を目標方向へ向ける」でなければならない。
    //   逆順にすると、R0 の軸（元の曲げ平面の法線）が R2 で動いた後の姿勢と食い違って
    //   目標に届かない（実装時に踏んだ。誤差 0.28 で収束しなかった）。
    //
    //   欲しいのは a_gr_new = R2 * R0 * a_gr（ハミルトン積・右から順に適用）。
    //   r_i = inv(a_gr) R_i a_gr なので a_lr_new = a_lr * r2 * r0。
    //   DirectXMath の XMQuaternionMultiply(Q1,Q2) は Q2*Q1 を返すので、
    //   ハミルトン積 a_lr * r2 * r0 は下のように内側から書く。
    if (haveAxis1)
    {
        const XMVECTOR axis1InA = XMVector3InverseRotate(axis1, aGlobalRot);
        const XMVECTOR r2 = XMQuaternionRotationAxis(axis1InA, ac_at_0);
        aLocalRot = XMQuaternionNormalize(
            XMQuaternionMultiply(r0, XMQuaternionMultiply(r2, aLocalRot)));
    }
    else
    {
        aLocalRot = XMQuaternionNormalize(XMQuaternionMultiply(r0, aLocalRot));
    }

    // b_lr = b_lr * r1
    bLocalRot = XMQuaternionNormalize(XMQuaternionMultiply(r1, bLocalRot));
}

// from を to へ向ける最短回転。どちらかが 0 長なら単位四元数。
// 真逆（dot ≈ -1）のときは任意の直交軸まわりに 180 度回す。
inline DirectX::XMVECTOR QuaternionFromTo(DirectX::FXMVECTOR from, DirectX::FXMVECTOR to)
{
    using namespace DirectX;
    const XMVECTOR f = XMVector3Normalize(from);
    const XMVECTOR t = XMVector3Normalize(to);
    if (XMVectorGetX(XMVector3LengthSq(f)) < 0.5f || XMVectorGetX(XMVector3LengthSq(t)) < 0.5f)
        return XMQuaternionIdentity();

    const float d = std::clamp(XMVectorGetX(XMVector3Dot(f, t)), -1.0f, 1.0f);
    if (d > 0.999999f) return XMQuaternionIdentity();
    if (d < -0.999999f)
    {
        // 真逆: f に直交な軸を 1 本作って 180 度
        XMVECTOR axis = XMVector3Cross(f, XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f));
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1.0e-6f)
            axis = XMVector3Cross(f, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        return XMQuaternionRotationAxis(XMVector3Normalize(axis), DirectX::XM_PI);
    }
    const XMVECTOR axis = XMVector3Normalize(XMVector3Cross(f, t));
    return XMQuaternionRotationAxis(axis, std::acos(d));
}

// 回転 q の角度を maxAngleRad 以内に制限する（軸はそのまま）。
inline DirectX::XMVECTOR ClampQuaternionAngle(DirectX::FXMVECTOR q, float maxAngleRad)
{
    using namespace DirectX;
    XMVECTOR axis;
    float angle = 0.0f;
    XMQuaternionToAxisAngle(&axis, &angle, q);
    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1.0e-10f) return XMQuaternionIdentity();
    if (std::fabs(angle) <= maxAngleRad) return q;
    const float clamped = (angle > 0.0f) ? maxAngleRad : -maxAngleRad;
    return XMQuaternionRotationAxis(XMVector3Normalize(axis), clamped);
}

// フレームレート非依存の指数平滑。tau（時定数、秒）が 0 以下なら即座に target。
inline float ExpSmooth(float current, float target, float tau, float dt)
{
    if (tau <= 1.0e-5f || dt <= 0.0f) return target;
    const float k = 1.0f - std::exp(-dt / tau);
    return current + (target - current) * k;
}

} // namespace dx12e
