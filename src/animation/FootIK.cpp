#include "animation/FootIK.h"
#include "animation/Animator.h"
#include "animation/Skeleton.h"
#include "animation/TwoBoneIK.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

namespace
{

// 名前候補を順に部分一致で試す。最初に見つかった index を返す。
i32 FindByCandidates(const Skeleton& sk, std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates)
    {
        const i32 idx = sk.FindBoneIndexContaining(c);
        if (idx >= 0) return idx;
    }
    return -1;
}

// 明示指定があればそれを優先し、無ければ候補から推定する。
i32 ResolveOne(const Skeleton& sk, const std::string& explicitName,
               std::initializer_list<const char*> candidates)
{
    if (!explicitName.empty())
    {
        const i32 idx = sk.FindBoneIndex(explicitName);
        if (idx >= 0) return idx;
        const i32 loose = sk.FindBoneIndexContaining(explicitName);
        if (loose >= 0) return loose;
        return -1;   // 明示指定が外れたら推定へ落とさない（ミスに気づけるように）
    }
    return FindByCandidates(sk, candidates);
}

// グローバル行列から平行移動を取り出す（行ベクトル規約なので 4 行目）。
XMVECTOR TranslationOf(const XMFLOAT4X4& m)
{
    return XMVectorSet(m._41, m._42, m._43, 1.0f);
}

// グローバル行列から回転だけを取り出す。
XMVECTOR RotationOf(const XMFLOAT4X4& m)
{
    XMVECTOR s, r, t;
    if (!XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4(&m)))
        return XMQuaternionIdentity();
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
bool ResolveFootIKBones(const Skeleton& skeleton, const FootIKBoneNames& names, FootIKBones& out)
{
    // よくある命名: mixamorig:LeftFoot / foot.L / Bip01 L Foot / LeftUpLeg / thigh.L ...
    out.left.hip  = ResolveOne(skeleton, names.leftHip,
                               {"leftupleg", "upleg.l", "thigh.l", "l thigh", "leftthigh",
                                "l_thigh", "leftleg01", "leftupperleg"});
    out.left.knee = ResolveOne(skeleton, names.leftKnee,
                               {"leftleg02", "leftleg", "leg.l", "l calf", "leftcalf",
                                "l_calf", "leftlowerleg", "shin.l"});
    out.left.foot = ResolveOne(skeleton, names.leftFoot,
                               {"leftfoot", "foot.l", "l foot", "l_foot", "leftankle"});
    out.left.toe  = ResolveOne(skeleton, names.leftToe,
                               {"lefttoe", "toe.l", "l toe", "l_toe", "leftfoot02"});

    out.right.hip  = ResolveOne(skeleton, names.rightHip,
                                {"rightupleg", "upleg.r", "thigh.r", "r thigh", "rightthigh",
                                 "r_thigh", "rightleg01", "rightupperleg"});
    out.right.knee = ResolveOne(skeleton, names.rightKnee,
                                {"rightleg02", "rightleg", "leg.r", "r calf", "rightcalf",
                                 "r_calf", "rightlowerleg", "shin.r"});
    out.right.foot = ResolveOne(skeleton, names.rightFoot,
                                {"rightfoot", "foot.r", "r foot", "r_foot", "rightankle"});
    out.right.toe  = ResolveOne(skeleton, names.rightToe,
                                {"righttoe", "toe.r", "r toe", "r_toe", "rightfoot02"});

    out.pelvis = ResolveOne(skeleton, names.pelvis, {"pelvis", "hips", "hip_", "hip", "spine01"});
    if (out.pelvis < 0 && skeleton.GetBoneCount() > 0) out.pelvis = 0;   // ルートボーン

    // 「膝が股関節と同じ」等の壊れた推定は無効にする
    auto sane = [](const FootIKLegBones& l) {
        return l.Valid() && l.hip != l.knee && l.knee != l.foot && l.hip != l.foot;
    };
    if (!sane(out.left))  out.left  = FootIKLegBones{};
    if (!sane(out.right)) out.right = FootIKLegBones{};

    return out.left.Valid() || out.right.Valid();
}

// ---------------------------------------------------------------------------
bool ApplyFootIK(Animator& animator, const Skeleton& skeleton, FXMMATRIX world,
                 const FootIKBones& bones, const FootIKParams& params,
                 FootIKState& state, const FootIKRayCast& ray,
                 f32 dt, bool grounded)
{
    if (!ray) return false;
    if (params.weight <= 0.0f) return false;
    if (!bones.left.Valid() && !bones.right.Valid()) return false;

    auto& globals = animator.GetMutableGlobalMatrices();
    AnimPose&  pose = animator.GetMutablePose();
    const u32 boneCount = skeleton.GetBoneCount();
    if (globals.size() < boneCount || pose.size() < boneCount) return false;

    const XMMATRIX worldInv = XMMatrixInverse(nullptr, world);
    const XMVECTOR worldUp  = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR worldDown = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);

    // ---- 1) 各足の接地を調べる -------------------------------------------
    struct FootProbe
    {
        bool  valid   = false;
        bool  hit     = false;
        f32   liftRaw = 0.0f;      // ワールド Y でどれだけ上げ/下げが要るか
        XMFLOAT3 normal{0.0f, 1.0f, 0.0f};
    };
    FootProbe probes[2];

    const FootIKLegBones* legs[2] = { &bones.left, &bones.right };
    for (int i = 0; i < 2; ++i)
    {
        if (!legs[i]->Valid()) continue;
        probes[i].valid = true;

        const XMVECTOR ankleModel = TranslationOf(globals[static_cast<size_t>(legs[i]->foot)]);
        const XMVECTOR ankleWorld = XMVector3TransformCoord(ankleModel, world);

        XMFLOAT3 origin;
        XMStoreFloat3(&origin, XMVectorAdd(ankleWorld, XMVectorScale(worldUp, params.rayUpOffset)));
        XMFLOAT3 dir; XMStoreFloat3(&dir, worldDown);

        XMFLOAT3 hitPoint, hitNormal;
        if (grounded && ray(origin, dir, params.rayLength, hitPoint, hitNormal))
        {
            probes[i].hit = true;
            probes[i].normal = hitNormal;
            const f32 targetY = hitPoint.y + params.footHeight;
            probes[i].liftRaw = targetY - XMVectorGetY(ankleWorld);
        }
    }

    // ---- 2) 平滑化とフェード ---------------------------------------------
    // 初回フレームは平滑を効かせず即座に合わせる（スポーン直後に足がスッと動くのを防ぐ）
    const f32 tau = state.initialized ? params.smoothTime : 0.0f;
    const f32 fadeTau = state.initialized ? params.fadeOutTime : 0.0f;
    state.initialized = true;

    f32* lifts[2]   = { &state.leftLift,   &state.rightLift };
    f32* weights[2] = { &state.leftWeight, &state.rightWeight };
    bool* contacts[2] = { &state.leftContact, &state.rightContact };
    XMFLOAT3* normals[2] = { &state.leftNormal, &state.rightNormal };

    for (int i = 0; i < 2; ++i)
    {
        const bool on = probes[i].valid && probes[i].hit;
        *contacts[i] = on;
        *lifts[i]   = ExpSmooth(*lifts[i], on ? probes[i].liftRaw : 0.0f, tau, dt);
        *weights[i] = ExpSmooth(*weights[i], on ? params.weight : 0.0f, on ? tau : fadeTau, dt);
        if (on)
        {
            // 法線も平滑化する（段差の縁でカクつかないように）
            XMVECTOR cur = XMLoadFloat3(normals[i]);
            XMVECTOR tgt = XMLoadFloat3(&probes[i].normal);
            const f32 k = (tau <= 1e-5f) ? 1.0f : (1.0f - std::exp(-dt / tau));
            XMStoreFloat3(normals[i], XMVector3Normalize(XMVectorLerp(cur, tgt, k)));
        }
    }

    // ---- 3) 腰を下げる ----------------------------------------------------
    // 「より深く下げる必要がある方」に合わせる。上げてはいけない（浮くため）。
    f32 dropTarget = 0.0f;
    for (int i = 0; i < 2; ++i)
        if (probes[i].valid && probes[i].hit)
            dropTarget = (std::min)(dropTarget, *lifts[i]);
    dropTarget = std::clamp(dropTarget, -params.maxPelvisDrop, 0.0f);
    state.pelvisDrop = ExpSmooth(state.pelvisDrop, dropTarget, tau, dt);

    bool poseDirty = false;
    if (bones.pelvis >= 0 && std::fabs(state.pelvisDrop) > 1e-5f)
    {
        // ワールドの下方向オフセットを、腰ボーンの「親のローカル空間」へ持ち込む。
        const XMVECTOR offsetWorld = XMVectorScale(worldUp, state.pelvisDrop);
        XMVECTOR offsetLocal = XMVector3TransformNormal(offsetWorld, worldInv);   // モデル空間へ

        const i32 parent = skeleton.GetBone(static_cast<u32>(bones.pelvis)).parentIndex;
        if (parent >= 0)
        {
            const XMMATRIX parentInv =
                XMMatrixInverse(nullptr, XMLoadFloat4x4(&globals[static_cast<size_t>(parent)]));
            offsetLocal = XMVector3TransformNormal(offsetLocal, parentInv);
        }

        BoneTRS& p = pose[static_cast<size_t>(bones.pelvis)];
        XMStoreFloat3(&p.t, XMVectorAdd(XMLoadFloat3(&p.t), offsetLocal));
        poseDirty = true;
    }

    if (poseDirty)
    {
        // 腰を動かしたので、足首の現在位置を取り直す必要がある
        animator.RecomputeFromPose();
    }

    // ---- 4) 2 ボーン IK で足首を目標へ ------------------------------------
    const XMVECTOR hintModel = XMLoadFloat3(&params.kneeForward);

    for (int i = 0; i < 2; ++i)
    {
        if (!probes[i].valid) continue;
        const f32 w = std::clamp(*weights[i], 0.0f, 1.0f);
        if (w <= 1e-4f) continue;

        const FootIKLegBones& leg = *legs[i];
        const size_t hipI  = static_cast<size_t>(leg.hip);
        const size_t kneeI = static_cast<size_t>(leg.knee);
        const size_t footI = static_cast<size_t>(leg.foot);

        const XMVECTOR aM = TranslationOf(globals[hipI]);
        const XMVECTOR bM = TranslationOf(globals[kneeI]);
        const XMVECTOR cM = TranslationOf(globals[footI]);

        // 目標: 現在の足首位置から、ワールド Y 方向に lift ぶん動かした点。
        // 重み w で元の位置とブレンドする（フェード中の急なスナップを防ぐ）。
        const XMVECTOR liftWorld = XMVectorScale(worldUp, *lifts[i] * w);
        const XMVECTOR liftModel = XMVector3TransformNormal(liftWorld, worldInv);
        const XMVECTOR targetM   = XMVectorAdd(cM, liftModel);

        XMVECTOR aLocal = XMLoadFloat4(&pose[hipI].r);
        XMVECTOR bLocal = XMLoadFloat4(&pose[kneeI].r);
        const XMVECTOR aGlobal = RotationOf(globals[hipI]);
        const XMVECTOR bGlobal = RotationOf(globals[kneeI]);

        SolveTwoBoneIK(aM, bM, cM, targetM, hintModel, aGlobal, bGlobal, aLocal, bLocal);

        XMStoreFloat4(&pose[hipI].r,  aLocal);
        XMStoreFloat4(&pose[kneeI].r, bLocal);
        poseDirty = true;
    }

    if (poseDirty) animator.RecomputeFromPose();

    // ---- 5) 足の向きを面法線に合わせる ------------------------------------
    if (params.alignToNormal)
    {
        const f32 maxPitch = XMConvertToRadians(params.maxFootPitchDeg);
        bool rotDirty = false;

        for (int i = 0; i < 2; ++i)
        {
            if (!probes[i].valid || !*contacts[i]) continue;
            const f32 w = std::clamp(*weights[i], 0.0f, 1.0f);
            if (w <= 1e-4f) continue;

            const size_t footI = static_cast<size_t>(legs[i]->foot);

            // ワールドの上方向 → 面法線 への回転を作り、角度を制限してから
            // 足首の**グローバル回転**へ左から掛ける。
            XMVECTOR nWorld = XMLoadFloat3(normals[i]);
            if (XMVectorGetX(XMVector3LengthSq(nWorld)) < 1e-6f) continue;
            nWorld = XMVector3Normalize(nWorld);

            XMVECTOR align = QuaternionFromTo(worldUp, nWorld);
            align = ClampQuaternionAngle(align, maxPitch);
            align = XMQuaternionSlerp(XMQuaternionIdentity(), align, w);

            // ワールド空間の回転をモデル空間へ持ち替える（共役変換）。
            // wr はモデル→ワールドなので、適用順は「wr → align → inv(wr)」。
            // DirectXMath の XMQuaternionMultiply(Q1,Q2) は「Q1 を先に適用」なので下の並びになる。
            XMVECTOR ws, wr, wt;
            if (!XMMatrixDecompose(&ws, &wr, &wt, world)) continue;
            const XMVECTOR alignModel = XMQuaternionMultiply(
                XMQuaternionMultiply(wr, align), XMQuaternionInverse(wr));

            const size_t parent = static_cast<size_t>(
                (std::max)(skeleton.GetBone(static_cast<u32>(footI)).parentIndex, 0));
            const XMVECTOR parentRot = RotationOf(globals[parent]);
            const XMVECTOR footGlobal = RotationOf(globals[footI]);

            // 新しいグローバル回転 = alignModel * footGlobal（ワールド軸まわりの補正）
            const XMVECTOR newGlobal = XMQuaternionMultiply(footGlobal, alignModel);
            // ローカル = 親のグローバルの逆 * 新グローバル
            const XMVECTOR newLocal =
                XMQuaternionMultiply(newGlobal, XMQuaternionInverse(parentRot));

            XMStoreFloat4(&pose[footI].r, XMQuaternionNormalize(newLocal));
            rotDirty = true;
        }
        if (rotDirty) animator.RecomputeFromPose();
    }

    return true;
}

} // namespace dx12e
