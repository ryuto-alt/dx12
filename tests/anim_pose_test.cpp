// ポーズ空間ブレンド（animation/AnimPose.h）と Animator のクロスフェードのテスト。
//
// 本命は B1 の回帰テスト:
//   「最終スキニング行列を要素ごとに線形補間すると回転が保存されず骨が縮む」
// を数値で固定する。TestBoneLengthPreserved が旧実装（行列 lerp）と
// 新実装（ローカル TRS の slerp）の両方を計算し、
//   - 旧: 90 度差の 2 姿勢を t=0.5 で混ぜると骨が約 29% 縮む
//   - 新: 骨長が 1e-5 以内で保存される
// ことを確認する。
//
// AnimPose/Animator/Skeleton/AnimationClip は純ロジック（GPU非依存、DirectXMath のみ）。
#include "animation/AnimPose.h"
#include "animation/Animator.h"
#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dx12e;
using namespace DirectX;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) < eps;
}

static bool Finite(float a)
{
    return std::isfinite(a);
}

// ---------------------------------------------------------------------------
// 3 ボーン（hip → knee → ankle、各段 Y+1m）の合成スケルトン
// ---------------------------------------------------------------------------
static void BuildLegSkeleton(Skeleton& skeleton)
{
    const char* names[3] = {"Hips", "LeftLeg", "LeftFoot"};
    for (int i = 0; i < 3; ++i)
    {
        BoneNode bone;
        bone.name        = names[i];
        bone.parentIndex = (i == 0) ? -1 : (i - 1);

        // ローカルバインド: ルートは原点、以降は親から Y+1
        const XMMATRIX local = (i == 0) ? XMMatrixIdentity() : XMMatrixTranslation(0.0f, 1.0f, 0.0f);
        XMStoreFloat4x4(&bone.localBindPose, local);

        // グローバルバインド = (0, i, 0) の平行移動。その逆行列が inverseBindPose。
        const XMMATRIX globalBind = XMMatrixTranslation(0.0f, static_cast<float>(i), 0.0f);
        XMStoreFloat4x4(&bone.inverseBindPose, XMMatrixInverse(nullptr, globalBind));

        skeleton.AddBone(std::move(bone));
    }
}

// ボーン index の「回転だけ」が入ったクリップ（duration 1 秒、キー 1 本）
static void BuildRotationClip(AnimationClip& clip, u32 boneIndex, XMVECTOR quat)
{
    clip.SetTicksPerSecond(1.0f);
    clip.SetDuration(1.0f);

    // 全ボーンにトラックを置く（回転指定のボーンだけ quat、他は単位）
    for (u32 i = 0; i < 3; ++i)
    {
        BoneTrack track;
        track.boneIndex = i;
        const float y = (i == 0) ? 0.0f : 1.0f;
        track.positionKeys = {{0.0f, {0.0f, y, 0.0f}}, {1.0f, {0.0f, y, 0.0f}}};
        XMFLOAT4 q(0.0f, 0.0f, 0.0f, 1.0f);
        if (i == boneIndex) XMStoreFloat4(&q, quat);
        track.rotationKeys = {{0.0f, q}, {1.0f, q}};
        track.scaleKeys    = {{0.0f, {1, 1, 1}}, {1.0f, {1, 1, 1}}};
        clip.AddTrack(std::move(track));
    }
}

// スキニング行列（転置済み）を、そのボーンのバインド原点に適用した結果のワールド位置。
// = FK 後のボーン原点。ボーン間の距離を測ることで「骨が縮んだか」を判定できる。
static XMVECTOR SkinnedBoneOrigin(const Skeleton& sk, const std::vector<XMFLOAT4X4>& skinT, u32 i)
{
    const XMMATRIX skin = XMMatrixTranspose(XMLoadFloat4x4(&skinT[i]));  // HLSL 列優先 → 行ベクトル規約へ戻す
    const XMMATRIX bindGlobal = XMMatrixInverse(nullptr, XMLoadFloat4x4(&sk.GetBone(i).inverseBindPose));
    return XMVector3TransformCoord(bindGlobal.r[3], skin);
}

static float BoneLength(const Skeleton& sk, const std::vector<XMFLOAT4X4>& skinT, u32 a, u32 b)
{
    const XMVECTOR pa = SkinnedBoneOrigin(sk, skinT, a);
    const XMVECTOR pb = SkinnedBoneOrigin(sk, skinT, b);
    return XMVectorGetX(XMVector3Length(XMVectorSubtract(pb, pa)));
}

// ---------------------------------------------------------------------------
// ★ 本命: 骨長の保存（B1 の回帰テスト）
// ---------------------------------------------------------------------------
static void TestBoneLengthPreserved()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);

    // ルートを 0 度 / 90 度に回す 2 本のクリップ
    AnimationClip clipA, clipB;
    BuildRotationClip(clipA, 0, XMQuaternionIdentity());
    BuildRotationClip(clipB, 0, XMQuaternionRotationRollPitchYaw(0.0f, 0.0f, XM_PIDIV2));

    // --- 新実装: ローカル TRS ポーズを slerp してから FK ---
    AnimPose poseA, poseB, blended;
    SamplePose(clipA, 0.0f, skeleton, poseA);
    SamplePose(clipB, 0.0f, skeleton, poseB);
    BlendPose(poseA, poseB, 0.5f, blended);

    std::vector<XMFLOAT4X4> globals, skinNew;
    ComputeGlobalMatrices(skeleton, blended, globals);
    GlobalToSkinning(skeleton, globals, skinNew);

    // --- 旧実装（B1）: 最終スキニング行列を要素ごとに線形補間 ---
    std::vector<XMFLOAT4X4> skinA, skinB, skinOld;
    ComputeGlobalMatrices(skeleton, poseA, globals);
    GlobalToSkinning(skeleton, globals, skinA);
    ComputeGlobalMatrices(skeleton, poseB, globals);
    GlobalToSkinning(skeleton, globals, skinB);
    skinOld.resize(skinA.size());
    for (size_t i = 0; i < skinA.size(); ++i)
    {
        const XMMATRIX ma = XMLoadFloat4x4(&skinA[i]);
        const XMMATRIX mb = XMLoadFloat4x4(&skinB[i]);
        XMMATRIX blend;
        for (int r = 0; r < 4; ++r) blend.r[r] = XMVectorLerp(ma.r[r], mb.r[r], 0.5f);
        XMStoreFloat4x4(&skinOld[i], blend);
    }

    const float lenNew0 = BoneLength(skeleton, skinNew, 0, 1);
    const float lenNew1 = BoneLength(skeleton, skinNew, 1, 2);
    const float lenOld0 = BoneLength(skeleton, skinOld, 0, 1);
    const float lenOld1 = BoneLength(skeleton, skinOld, 1, 2);

    std::printf("  [B1] bone length @ t=0.5 (90 deg apart): new = %.6f / %.6f, old(matrix lerp) = %.6f / %.6f (expected 1.0)\n",
                lenNew0, lenNew1, lenOld0, lenOld1);

    // 新実装は骨長を保存する
    CHECK(Near(lenNew0, 1.0f, 1e-5f));
    CHECK(Near(lenNew1, 1.0f, 1e-5f));

    // 旧実装は実際に縮む（縮まないなら、このテストが検証したいバグを再現できていない）
    CHECK(lenOld1 < 0.95f);

    // 端点は一致する
    AnimPose at0, at1;
    BlendPose(poseA, poseB, 0.0f, at0);
    BlendPose(poseA, poseB, 1.0f, at1);
    for (size_t i = 0; i < poseA.size(); ++i)
    {
        CHECK(Near(at0[i].t.y, poseA[i].t.y));
        CHECK(Near(at1[i].t.y, poseB[i].t.y));
        // 四元数は符号の自由度があるので |dot| で比較する
        const float d0 = std::fabs(XMVectorGetX(XMVector4Dot(XMLoadFloat4(&at0[i].r), XMLoadFloat4(&poseA[i].r))));
        const float d1 = std::fabs(XMVectorGetX(XMVector4Dot(XMLoadFloat4(&at1[i].r), XMLoadFloat4(&poseB[i].r))));
        CHECK(Near(d0, 1.0f, 1e-5f));
        CHECK(Near(d1, 1.0f, 1e-5f));
    }
}

// ブレンド結果の四元数は必ず単位
static void TestBlendedQuaternionIsUnit()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);

    AnimationClip a, b;
    BuildRotationClip(a, 1, XMQuaternionRotationRollPitchYaw(0.3f, -1.1f, 2.0f));
    BuildRotationClip(b, 1, XMQuaternionRotationRollPitchYaw(-2.4f, 0.7f, -0.9f));

    AnimPose pa, pb, out;
    SamplePose(a, 0.0f, skeleton, pa);
    SamplePose(b, 0.0f, skeleton, pb);

    for (int step = 0; step <= 10; ++step)
    {
        const float t = static_cast<float>(step) / 10.0f;
        BlendPose(pa, pb, t, out);
        for (const BoneTRS& bone : out)
        {
            const float len = XMVectorGetX(XMVector4Length(XMLoadFloat4(&bone.r)));
            CHECK(Near(len, 1.0f, 1e-5f));
        }
    }
}

// 内積が負の 2 つの四元数は「近い方の弧」を通る（遠回りしない）
static void TestQuaternionNeighborhood()
{
    AnimPose a(1), b(1);
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(0.0f, 0.4f, 0.0f);
    XMStoreFloat4(&a[0].r, XMQuaternionIdentity());
    XMStoreFloat4(&b[0].r, XMVectorNegate(q));  // q と同じ回転だが符号が逆＝内積が負

    AnimPose out;
    BlendPose(a, b, 0.5f, out);

    // 近い方の弧を通っていれば、結果は「0.2 rad 回転」に近い（遠回りなら約 2.94 rad）
    const XMVECTOR half = XMQuaternionRotationRollPitchYaw(0.0f, 0.2f, 0.0f);
    const float dot = std::fabs(XMVectorGetX(XMVector4Dot(XMLoadFloat4(&out[0].r), half)));
    CHECK(Near(dot, 1.0f, 1e-4f));

    // BlendPoseAccum（nlerp 版）も同じ性質を持つ
    AnimPose acc = a;
    BlendPoseAccum(acc, b, 0.5f);
    const float dotAcc = std::fabs(XMVectorGetX(XMVector4Dot(XMLoadFloat4(&acc[0].r), half)));
    CHECK(Near(dotAcc, 1.0f, 1e-4f));
}

// 重み和 1 の 3 クリップを逐次ブレンドしても単位四元数のまま
static void TestAccumThreeWay()
{
    AnimPose p0(1), p1(1), p2(1);
    XMStoreFloat4(&p0[0].r, XMQuaternionRotationRollPitchYaw(0.5f, 0.0f, 0.0f));
    XMStoreFloat4(&p1[0].r, XMQuaternionRotationRollPitchYaw(0.0f, 0.9f, 0.0f));
    XMStoreFloat4(&p2[0].r, XMQuaternionRotationRollPitchYaw(0.0f, 0.0f, -1.3f));
    p0[0].t = {1.0f, 0.0f, 0.0f};
    p1[0].t = {0.0f, 2.0f, 0.0f};
    p2[0].t = {0.0f, 0.0f, 3.0f};

    const float w[3] = {0.2f, 0.3f, 0.5f};

    // 逐次ブレンド: acc = lerp(acc, p_i, w_i / Σ_{k<=i} w_k)
    AnimPose acc = p0;
    float sum = w[0];
    sum += w[1]; BlendPoseAccum(acc, p1, w[1] / sum);
    sum += w[2]; BlendPoseAccum(acc, p2, w[2] / sum);

    CHECK(Near(XMVectorGetX(XMVector4Length(XMLoadFloat4(&acc[0].r))), 1.0f, 1e-5f));
    // 位置は厳密に重み付き平均になる
    CHECK(Near(acc[0].t.x, w[0] * 1.0f, 1e-5f));
    CHECK(Near(acc[0].t.y, w[1] * 2.0f, 1e-5f));
    CHECK(Near(acc[0].t.z, w[2] * 3.0f, 1e-5f));
}

// バインドポーズの往復: MakeBindPose → FK が localBindPose の連鎖と一致する
static void TestBindPoseRoundTrip()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);

    for (u32 i = 0; i < skeleton.GetBoneCount(); ++i)
        CHECK(skeleton.GetBone(i).bindDecomposed);
    CHECK(!skeleton.HasUndecomposableBind());

    AnimPose bind;
    MakeBindPose(skeleton, bind);
    std::vector<XMFLOAT4X4> globals;
    ComputeGlobalMatrices(skeleton, bind, globals);

    for (u32 i = 0; i < skeleton.GetBoneCount(); ++i)
    {
        const XMMATRIX expect = XMMatrixInverse(nullptr, XMLoadFloat4x4(&skeleton.GetBone(i).inverseBindPose));
        const XMMATRIX got    = XMLoadFloat4x4(&globals[i]);
        for (int r = 0; r < 4; ++r)
        {
            XMFLOAT4 e, g;
            XMStoreFloat4(&e, expect.r[r]);
            XMStoreFloat4(&g, got.r[r]);
            CHECK(Near(e.x, g.x, 1e-5f) && Near(e.y, g.y, 1e-5f) &&
                  Near(e.z, g.z, 1e-5f) && Near(e.w, g.w, 1e-5f));
        }
    }

    // バインドポーズのスキニング行列は単位行列になる
    std::vector<XMFLOAT4X4> skin;
    GlobalToSkinning(skeleton, globals, skin);
    for (const auto& m : skin)
    {
        CHECK(Near(m._11, 1.0f, 1e-5f) && Near(m._22, 1.0f, 1e-5f) &&
              Near(m._33, 1.0f, 1e-5f) && Near(m._44, 1.0f, 1e-5f));
        CHECK(Near(m._14, 0.0f, 1e-5f) && Near(m._24, 0.0f, 1e-5f) && Near(m._34, 0.0f, 1e-5f));
    }
}

// B2: blendDuration=0 かつ dt=0 で NaN が出ない（Scene がスポーン直後に Update(0) を呼ぶ経路）
static void TestZeroBlendDurationNoNaN()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);
    AnimationClip a, b;
    BuildRotationClip(a, 0, XMQuaternionIdentity());
    BuildRotationClip(b, 0, XMQuaternionRotationRollPitchYaw(0.0f, 0.0f, XM_PIDIV2));

    Animator animator;
    animator.Initialize(&skeleton, &a);
    animator.CrossFadeTo(&b, 0.0f);
    animator.Update(0.0f);
    animator.Update(0.0f);
    animator.Update(1.0f / 60.0f);

    CHECK(Finite(animator.GetClipTime()));
    CHECK(Finite(animator.GetBlendFactor()));
    CHECK(animator.GetClip() == &b);          // 即座に切り替わる
    CHECK(!animator.IsBlending());
    for (const auto& m : animator.GetSkinningMatrices())
    {
        CHECK(Finite(m._11) && Finite(m._22) && Finite(m._33));
        CHECK(Finite(m._14) && Finite(m._24) && Finite(m._34));
    }
}

// B5: duration <= 0 のポーズクリップも 1 回は評価される（以前は早期 return で無視）
static void TestZeroDurationClipEvaluated()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);

    AnimationClip pose;
    pose.SetTicksPerSecond(1.0f);
    pose.SetDuration(0.0f);
    BoneTrack track;
    track.boneIndex = 0;
    track.positionKeys = {{0.0f, {0.0f, 5.0f, 0.0f}}};
    track.rotationKeys = {{0.0f, {0.0f, 0.0f, 0.0f, 1.0f}}};
    track.scaleKeys    = {{0.0f, {1, 1, 1}}};
    pose.AddTrack(std::move(track));

    Animator animator;
    animator.Initialize(&skeleton, &pose);
    animator.Update(1.0f / 60.0f);

    // ルートが Y+5 に飛んでいる＝ポーズが評価された
    const XMVECTOR p = SkinnedBoneOrigin(skeleton, animator.GetSkinningMatrices(), 0);
    CHECK(Near(XMVectorGetY(p), 5.0f, 1e-5f));
    CHECK(Near(animator.GetClipTime(), 0.0f));
}

// クロスフェード中も骨が縮まないこと（Animator 経由の統合確認）
static void TestAnimatorCrossFadeKeepsBoneLength()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);
    AnimationClip a, b;
    BuildRotationClip(a, 0, XMQuaternionIdentity());
    BuildRotationClip(b, 0, XMQuaternionRotationRollPitchYaw(0.0f, 0.0f, XM_PIDIV2));

    Animator animator;
    animator.Initialize(&skeleton, &a);
    animator.Update(0.0f);
    animator.CrossFadeTo(&b, 0.5f);

    float worst = 0.0f;
    for (int i = 0; i < 40; ++i)
    {
        animator.Update(1.0f / 60.0f);
        const float len = BoneLength(skeleton, animator.GetSkinningMatrices(), 1, 2);
        worst = (std::max)(worst, std::fabs(len - 1.0f));
    }
    std::printf("  [B1] max bone-length error during a 0.5s crossfade: %.7f\n", worst);
    CHECK(worst < 1e-4f);
}

// ポーズ注入（SetPoseOverride）は即座に反映され、次の Update で上書きされない
static void TestPoseOverride()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);
    AnimationClip a;
    BuildRotationClip(a, 0, XMQuaternionIdentity());

    Animator animator;
    animator.Initialize(&skeleton, &a);

    AnimPose custom;
    MakeBindPose(skeleton, custom);
    custom[0].t = {3.0f, 0.0f, 0.0f};

    animator.SetPoseOverride(custom);
    CHECK(animator.HasPoseOverride());
    CHECK(Near(XMVectorGetX(SkinnedBoneOrigin(skeleton, animator.GetSkinningMatrices(), 0)), 3.0f, 1e-5f));

    animator.Update(1.0f / 60.0f);   // 注入されたフレームはサンプリングをスキップ
    CHECK(Near(XMVectorGetX(SkinnedBoneOrigin(skeleton, animator.GetSkinningMatrices(), 0)), 3.0f, 1e-5f));
    CHECK(Near(animator.GetClipTime(), 0.0f));
    CHECK(!animator.HasPoseOverride());

    animator.Update(1.0f / 60.0f);   // 次のフレームからは通常再生に戻る
    CHECK(Near(XMVectorGetX(SkinnedBoneOrigin(skeleton, animator.GetSkinningMatrices(), 0)), 0.0f, 1e-5f));
}

// レイヤー合成の基本性質
static void TestLayerBlendIdentities()
{
    AnimPose lower(2), upper(2);
    lower[0].t = {1.0f, 0.0f, 0.0f};
    lower[1].t = {0.0f, 1.0f, 0.0f};
    upper[0].t = {9.0f, 0.0f, 0.0f};
    upper[1].t = {0.0f, 9.0f, 0.0f};
    XMStoreFloat4(&upper[0].r, XMQuaternionRotationRollPitchYaw(0.7f, 0.0f, 0.0f));
    XMStoreFloat4(&upper[1].r, XMQuaternionRotationRollPitchYaw(0.0f, -0.4f, 0.0f));

    // weight=0 の Override は下位と完全一致
    AnimPose dst = lower;
    OverrideBlendPose(dst, upper, 0.0f, nullptr);
    CHECK(Near(dst[0].t.x, 1.0f) && Near(dst[1].t.y, 1.0f));

    // weight=1 の Override は上位と完全一致
    dst = lower;
    OverrideBlendPose(dst, upper, 1.0f, nullptr);
    CHECK(Near(dst[0].t.x, 9.0f) && Near(dst[1].t.y, 9.0f));

    // マスクで片方のボーンだけ差し替える
    const float mask[2] = {1.0f, 0.0f};
    dst = lower;
    OverrideBlendPose(dst, upper, 1.0f, mask);
    CHECK(Near(dst[0].t.x, 9.0f));
    CHECK(Near(dst[1].t.y, 1.0f));   // マスク外は下位のまま（ビット単位で同一）
    CHECK(Near(dst[1].r.x, lower[1].r.x) && Near(dst[1].r.w, lower[1].r.w));

    // Additive で add == ref のときは下位と完全一致（差分ゼロ）
    dst = lower;
    AdditiveBlendPose(dst, upper, upper, 1.0f, nullptr);
    CHECK(Near(dst[0].t.x, 1.0f) && Near(dst[1].t.y, 1.0f));
    CHECK(Near(XMVectorGetX(XMVector4Length(XMLoadFloat4(&dst[0].r))), 1.0f, 1e-5f));
}

// Skeleton の補助 API
static void TestSkeletonHelpers()
{
    Skeleton skeleton;
    BuildLegSkeleton(skeleton);

    CHECK(skeleton.AreBonesCorrectlyOrdered());
    CHECK(skeleton.FindBoneIndexContaining("leftfoot") == 2);
    CHECK(skeleton.FindBoneIndexContaining("LEG") == 1);
    CHECK(skeleton.FindBoneIndexContaining("nosuchbone") == -1);
    CHECK(skeleton.FindBoneIndexContaining("") == -1);
    CHECK(skeleton.FindBoneIndex("LeftFoot") == 2);

    // 親が子より後ろに来ている（＝FK の単一ループが破れる）スケルトンを検出できる
    Skeleton bad;
    BoneNode child;
    child.name = "child";
    child.parentIndex = 1;   // 自分より後ろのボーンを親にしている
    XMStoreFloat4x4(&child.inverseBindPose, XMMatrixIdentity());
    XMStoreFloat4x4(&child.localBindPose, XMMatrixIdentity());
    bad.AddBone(child);
    BoneNode parent;
    parent.name = "parent";
    parent.parentIndex = -1;
    XMStoreFloat4x4(&parent.inverseBindPose, XMMatrixIdentity());
    XMStoreFloat4x4(&parent.localBindPose, XMMatrixIdentity());
    bad.AddBone(parent);
    CHECK(!bad.AreBonesCorrectlyOrdered());
}

// 非一様スケールを含むバインドポーズも分解 → 再構成できる
static void TestNonUniformScaleBind()
{
    Skeleton skeleton;
    BoneNode bone;
    bone.name = "scaled";
    bone.parentIndex = -1;
    const XMMATRIX local = XMMatrixScaling(2.0f, 0.5f, 3.0f) *
                           XMMatrixRotationRollPitchYaw(0.3f, 0.8f, -0.2f) *
                           XMMatrixTranslation(1.0f, -2.0f, 0.5f);
    XMStoreFloat4x4(&bone.localBindPose, local);
    XMStoreFloat4x4(&bone.inverseBindPose, XMMatrixInverse(nullptr, local));
    skeleton.AddBone(std::move(bone));

    CHECK(skeleton.GetBone(0).bindDecomposed);

    AnimPose bind;
    MakeBindPose(skeleton, bind);
    const XMMATRIX rebuilt = BoneTRSToMatrix(bind[0]);
    for (int r = 0; r < 4; ++r)
    {
        XMFLOAT4 e, g;
        XMStoreFloat4(&e, local.r[r]);
        XMStoreFloat4(&g, rebuilt.r[r]);
        CHECK(Near(e.x, g.x, 1e-4f) && Near(e.y, g.y, 1e-4f) &&
              Near(e.z, g.z, 1e-4f) && Near(e.w, g.w, 1e-4f));
    }
}

int main()
{
    TestBoneLengthPreserved();
    TestBlendedQuaternionIsUnit();
    TestQuaternionNeighborhood();
    TestAccumThreeWay();
    TestBindPoseRoundTrip();
    TestZeroBlendDurationNoNaN();
    TestZeroDurationClipEvaluated();
    TestAnimatorCrossFadeKeepsBoneLength();
    TestPoseOverride();
    TestLayerBlendIdentities();
    TestSkeletonHelpers();
    TestNonUniformScaleBind();

    std::printf("AnimPoseTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
