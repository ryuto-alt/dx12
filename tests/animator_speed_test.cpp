// Animator の再生速度倍率(SetSpeed)テスト。
// 「歩き/走りアニメの足の速度を移動速度に同期」用途で、speed=2.0 のとき
// クリップ時間が 2 倍で進むこと・既定 1.0 で従来通りなことを保証する。
// Animator/Skeleton/AnimationClip は純ロジック（GPU非依存、DirectXMath のみ）なので直接ビルドして検証する。
#include "animation/Animator.h"
#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>

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

// ルートボーン1本のスケルトンを作る（行列は単位でよい）
static void BuildSingleBoneSkeleton(Skeleton& skeleton)
{
    BoneNode bone;
    bone.name = "root";
    bone.parentIndex = -1;
    XMStoreFloat4x4(&bone.inverseBindPose, XMMatrixIdentity());
    XMStoreFloat4x4(&bone.localBindPose, XMMatrixIdentity());
    skeleton.AddBone(std::move(bone));
}

// NormalizeToSeconds 済み相当のクリップ（tps=1、duration=10 秒）
static void BuildTestClip(AnimationClip& clip)
{
    clip.SetTicksPerSecond(1.0f);
    clip.SetDuration(10.0f);

    BoneTrack track;
    track.boneIndex = 0;
    track.positionKeys = {{0.0f, {0, 0, 0}}, {10.0f, {1, 0, 0}}};
    track.rotationKeys = {{0.0f, {0, 0, 0, 1}}, {10.0f, {0, 0, 0, 1}}};
    track.scaleKeys    = {{0.0f, {1, 1, 1}}, {10.0f, {1, 1, 1}}};
    clip.AddTrack(std::move(track));
}

// 既定 speed=1.0: dt がそのままクリップ時間になる（従来挙動の回帰確認）
static void TestDefaultSpeed(const Skeleton& skeleton, const AnimationClip& clip)
{
    Animator animator;
    animator.Initialize(&skeleton, &clip);

    CHECK(Near(animator.GetSpeed(), 1.0f));

    animator.Update(0.5f);
    CHECK(Near(animator.GetClipTime(), 0.5f));

    animator.Update(0.25f);
    CHECK(Near(animator.GetClipTime(), 0.75f));
}

// speed=2.0: 同じ dt で進行が 2 倍になる
static void TestDoubleSpeed(const Skeleton& skeleton, const AnimationClip& clip)
{
    Animator animator;
    animator.Initialize(&skeleton, &clip);
    animator.SetSpeed(2.0f);

    animator.Update(0.5f);
    CHECK(Near(animator.GetClipTime(), 1.0f));  // 0.5 * 2.0

    // 複数フレームに分割しても合計は同じ（60fps × 30 フレーム = 実時間 0.5 秒 → クリップ 1.0 秒）
    Animator animator2;
    animator2.Initialize(&skeleton, &clip);
    animator2.SetSpeed(2.0f);
    for (int i = 0; i < 30; ++i) animator2.Update(1.0f / 60.0f);
    CHECK(Near(animator2.GetClipTime(), 1.0f, 1e-4f));
}

// speed=0: 一時停止（時間が進まない）
static void TestPause(const Skeleton& skeleton, const AnimationClip& clip)
{
    Animator animator;
    animator.Initialize(&skeleton, &clip);
    animator.Update(0.5f);
    animator.SetSpeed(0.0f);
    animator.Update(1.0f);
    CHECK(Near(animator.GetClipTime(), 0.5f));
}

// speed 適用後もループ折り返し（fmod）が効く
static void TestSpeedWithLooping(const Skeleton& skeleton, const AnimationClip& clip)
{
    Animator animator;
    animator.Initialize(&skeleton, &clip);
    animator.SetSpeed(2.0f);
    animator.Update(6.0f);  // 6 * 2 = 12 → duration 10 でラップして 2
    CHECK(Near(animator.GetClipTime(), 2.0f, 1e-4f));
}

int main()
{
    Skeleton skeleton;
    BuildSingleBoneSkeleton(skeleton);

    AnimationClip clip;
    BuildTestClip(clip);

    TestDefaultSpeed(skeleton, clip);
    TestDoubleSpeed(skeleton, clip);
    TestPause(skeleton, clip);
    TestSpeedWithLooping(skeleton, clip);

    std::printf("AnimatorSpeedTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
