// ボーンマスク（animation/BoneMask.h）とレイヤー合成（AnimPose のブレンド）のテスト。
//
// 「下半身は走り、上半身は構え」を成立させるための土台。一番大事な性質は
// **マスク外のボーンが下位レイヤーの値とビット単位で同一に保たれること**
// （少しでも混ざると、走りの脚に構えのポーズが滲んで足が滑って見える）。
//
// Skeleton.cpp / AnimPose.cpp をリンクする（GPU/entt 非依存の純ロジック）。
#include "animation/BoneMask.h"
#include "animation/AnimPose.h"
#include "animation/Skeleton.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e;
using namespace DirectX;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-6f)
{
    return std::fabs(a - b) < eps;
}

// 人型っぽい階層:
//   0 Hips
//     1 Spine1
//       2 Spine2
//         3 LeftArm
//           4 LeftHand
//         5 Head
//     6 LeftUpLeg
//       7 LeftLeg
//         8 LeftFoot
static void BuildHumanoid(Skeleton& sk)
{
    struct Def { const char* name; i32 parent; };
    const Def defs[] = {
        {"Hips", -1}, {"Spine1", 0}, {"Spine2", 1}, {"LeftArm", 2}, {"LeftHand", 3},
        {"Head", 2}, {"LeftUpLeg", 0}, {"LeftLeg", 6}, {"LeftFoot", 7},
    };
    for (const Def& d : defs)
    {
        BoneNode b;
        b.name = d.name;
        b.parentIndex = d.parent;
        XMStoreFloat4x4(&b.localBindPose, XMMatrixTranslation(0.0f, 1.0f, 0.0f));
        XMStoreFloat4x4(&b.inverseBindPose, XMMatrixIdentity());
        sk.AddBone(std::move(b));
    }
}

// サブツリー指定が子孫を全部含む
static void TestSubtreeMask()
{
    Skeleton sk;
    BuildHumanoid(sk);
    CHECK(sk.AreBonesCorrectlyOrdered());

    std::vector<f32> mask;
    BuildBoneMaskFromNames(sk, {"Spine1"}, /*includeChildren*/ true, 1.0f, mask);

    CHECK(mask.size() == 9);
    CHECK(Near(mask[0], 0.0f));   // Hips は含まれない
    CHECK(Near(mask[1], 1.0f));   // Spine1
    CHECK(Near(mask[2], 1.0f));   // Spine2
    CHECK(Near(mask[3], 1.0f));   // LeftArm
    CHECK(Near(mask[4], 1.0f));   // LeftHand（孫の孫まで届く）
    CHECK(Near(mask[5], 1.0f));   // Head
    CHECK(Near(mask[6], 0.0f));   // LeftUpLeg（脚は別枝）
    CHECK(Near(mask[7], 0.0f));
    CHECK(Near(mask[8], 0.0f));
}

// includeChildren=false ならそのボーンだけ
static void TestSingleBoneMask()
{
    Skeleton sk;
    BuildHumanoid(sk);

    std::vector<f32> mask;
    BuildBoneMaskFromNames(sk, {"Spine2"}, /*includeChildren*/ false, 1.0f, mask);
    CHECK(Near(mask[2], 1.0f));
    CHECK(Near(mask[3], 0.0f));
    CHECK(Near(mask[5], 0.0f));
}

// 複数指定 / 部分一致 / 見つからない名前
static void TestNamesAndMissing()
{
    Skeleton sk;
    BuildHumanoid(sk);

    std::vector<f32> mask;
    std::vector<std::string> missing;
    BuildBoneMaskFromNames(sk, {"LeftArm", "Head", "NoSuchBone"}, true, 1.0f, mask, &missing);
    CHECK(Near(mask[3], 1.0f) && Near(mask[4], 1.0f));   // LeftArm 以下
    CHECK(Near(mask[5], 1.0f));                          // Head
    CHECK(Near(mask[0], 0.0f) && Near(mask[6], 0.0f));
    CHECK(missing.size() == 1 && missing[0] == "NoSuchBone");

    // 完全一致で引けなければ部分一致（大文字小文字無視）にフォールバックする
    std::vector<f32> m2;
    std::vector<std::string> miss2;
    BuildBoneMaskFromNames(sk, {"leftfoot"}, false, 1.0f, m2, &miss2);
    CHECK(miss2.empty());
    CHECK(Near(m2[8], 1.0f));
}

// 部分マスク（0.5 など）も持てる（肩の境界を滑らかにする用途）
static void TestPartialWeight()
{
    Skeleton sk;
    BuildHumanoid(sk);

    std::vector<f32> mask;
    BuildBoneMaskFromNames(sk, {"Spine2"}, true, 0.5f, mask);
    CHECK(Near(mask[2], 0.5f));
    CHECK(Near(mask[3], 0.5f));
    CHECK(Near(mask[1], 0.0f));

    // 重なった指定は max で合成される（1.0 が 0.5 に負けない）
    std::vector<f32> m2;
    MakeEmptyBoneMask(sk, m2);
    AddSubtreeToBoneMask(sk, 1, 0.5f, m2);   // Spine1 以下 0.5
    AddSubtreeToBoneMask(sk, 3, 1.0f, m2);   // LeftArm 以下 1.0
    CHECK(Near(m2[1], 0.5f));
    CHECK(Near(m2[2], 0.5f));
    CHECK(Near(m2[3], 1.0f));
    CHECK(Near(m2[4], 1.0f));
}

// ---------------------------------------------------------------------------
// レイヤー合成: マスク外のボーンが下位レイヤーと**ビット単位で同一**に保たれること
// ---------------------------------------------------------------------------
static AnimPose MakeDistinctPose(const Skeleton& sk, float seed)
{
    AnimPose p;
    MakeBindPose(sk, p);
    for (size_t i = 0; i < p.size(); ++i)
    {
        const float f = seed + static_cast<float>(i);
        p[i].t = {f * 0.11f, f * 0.22f, f * 0.33f};
        p[i].s = {1.0f + f * 0.01f, 1.0f - f * 0.005f, 1.0f + f * 0.02f};
        XMStoreFloat4(&p[i].r, XMQuaternionRotationRollPitchYaw(f * 0.13f, f * 0.07f, f * 0.19f));
    }
    return p;
}

static bool BitEqual(const BoneTRS& a, const BoneTRS& b)
{
    return a.t.x == b.t.x && a.t.y == b.t.y && a.t.z == b.t.z
        && a.r.x == b.r.x && a.r.y == b.r.y && a.r.z == b.r.z && a.r.w == b.r.w
        && a.s.x == b.s.x && a.s.y == b.s.y && a.s.z == b.s.z;
}

static void TestUpperBodyOverrideKeepsLowerBodyExact()
{
    Skeleton sk;
    BuildHumanoid(sk);

    const AnimPose lower = MakeDistinctPose(sk, 1.0f);   // 走り（下位レイヤー）
    const AnimPose upper = MakeDistinctPose(sk, 7.0f);   // 構え（上位レイヤー）

    std::vector<f32> mask;
    BuildBoneMaskFromNames(sk, {"Spine1"}, true, 1.0f, mask);

    AnimPose result = lower;
    OverrideBlendPose(result, upper, 1.0f, mask.data());

    for (size_t i = 0; i < result.size(); ++i)
    {
        if (mask[i] >= 1.0f)
        {
            // マスク内は上位レイヤーで完全に置き換わる
            CHECK(BitEqual(result[i], upper[i]));
        }
        else
        {
            // ★マスク外は下位レイヤーとビット単位で同一★
            CHECK(BitEqual(result[i], lower[i]));
        }
    }
}

// レイヤー重み 0 なら下位と完全一致、1 かつマスク全域なら上位と完全一致
static void TestLayerWeightExtremes()
{
    Skeleton sk;
    BuildHumanoid(sk);
    const AnimPose lower = MakeDistinctPose(sk, 2.0f);
    const AnimPose upper = MakeDistinctPose(sk, 9.0f);

    std::vector<f32> full(sk.GetBoneCount(), 1.0f);

    AnimPose r0 = lower;
    OverrideBlendPose(r0, upper, 0.0f, full.data());
    for (size_t i = 0; i < r0.size(); ++i) CHECK(BitEqual(r0[i], lower[i]));

    AnimPose r1 = lower;
    OverrideBlendPose(r1, upper, 1.0f, full.data());
    for (size_t i = 0; i < r1.size(); ++i) CHECK(BitEqual(r1[i], upper[i]));
}

// 加算レイヤー: add == ref なら差分ゼロ → 下位と一致（回転は正規化ぶんの誤差のみ）
static void TestAdditiveZeroDelta()
{
    Skeleton sk;
    BuildHumanoid(sk);
    const AnimPose lower = MakeDistinctPose(sk, 3.0f);
    const AnimPose add   = MakeDistinctPose(sk, 5.0f);

    AnimPose result = lower;
    AdditiveBlendPose(result, add, add, 1.0f, nullptr);

    for (size_t i = 0; i < result.size(); ++i)
    {
        CHECK(Near(result[i].t.x, lower[i].t.x, 1e-5f));
        CHECK(Near(result[i].t.y, lower[i].t.y, 1e-5f));
        CHECK(Near(result[i].s.z, lower[i].s.z, 1e-5f));
        const float dot = std::fabs(XMVectorGetX(XMVector4Dot(
            XMLoadFloat4(&result[i].r), XMLoadFloat4(&lower[i].r))));
        CHECK(Near(dot, 1.0f, 1e-4f));
    }
}

// 加算レイヤー: マスク外は触られない
static void TestAdditiveRespectsMask()
{
    Skeleton sk;
    BuildHumanoid(sk);
    const AnimPose lower = MakeDistinctPose(sk, 4.0f);
    const AnimPose add   = MakeDistinctPose(sk, 8.0f);
    const AnimPose ref   = MakeDistinctPose(sk, 1.5f);   // add != ref なので差分は非ゼロ

    std::vector<f32> mask;
    BuildBoneMaskFromNames(sk, {"LeftUpLeg"}, true, 1.0f, mask);

    AnimPose result = lower;
    AdditiveBlendPose(result, add, ref, 1.0f, mask.data());

    for (size_t i = 0; i < result.size(); ++i)
    {
        if (mask[i] > 0.0f) CHECK(!BitEqual(result[i], lower[i]));   // 効いている
        else                CHECK(BitEqual(result[i], lower[i]));    // 触られていない
    }
}

// 空マスク（全部 0）は下位レイヤーを一切変えない
static void TestEmptyMaskIsNoOp()
{
    Skeleton sk;
    BuildHumanoid(sk);
    const AnimPose lower = MakeDistinctPose(sk, 6.0f);
    const AnimPose upper = MakeDistinctPose(sk, 11.0f);

    std::vector<f32> mask;
    MakeEmptyBoneMask(sk, mask);

    AnimPose result = lower;
    OverrideBlendPose(result, upper, 1.0f, mask.data());
    for (size_t i = 0; i < result.size(); ++i) CHECK(BitEqual(result[i], lower[i]));
}

int main()
{
    TestSubtreeMask();
    TestSingleBoneMask();
    TestNamesAndMissing();
    TestPartialWeight();
    TestUpperBodyOverrideKeepsLowerBodyExact();
    TestLayerWeightExtremes();
    TestAdditiveZeroDelta();
    TestAdditiveRespectsMask();
    TestEmptyMaskIsNoOp();

    std::printf("BoneMaskTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
