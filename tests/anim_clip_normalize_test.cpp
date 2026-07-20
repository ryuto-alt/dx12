// AnimationClip::NormalizeToSeconds（ticks → 秒正規化）の単体テスト。
// glTF(Blender/Mixamo出力)は 1000 ticks/s = ミリ秒単位でロードされるため、
// 正規化後に「duration が正しい秒長になり、キー時刻も同じ倍率で縮む」ことを保証する。
// AnimationClip.cpp は純ロジック（GPU非依存、DirectXMath のみ）なので直接ビルドして検証する。
#include "animation/AnimationClip.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) < eps;
}

// glTF 相当: 1000 ticks/s、duration=500 ticks（= 0.5 秒の歩行アニメ）
static void TestGltfMillisecondClip()
{
    AnimationClip clip;
    clip.SetTicksPerSecond(1000.0f);
    clip.SetDuration(500.0f);

    BoneTrack track;
    track.boneIndex = 0;
    track.positionKeys = {{0.0f, {0, 0, 0}}, {250.0f, {1, 0, 0}}, {500.0f, {2, 0, 0}}};
    track.rotationKeys = {{0.0f, {0, 0, 0, 1}}, {500.0f, {0, 0, 0, 1}}};
    track.scaleKeys    = {{0.0f, {1, 1, 1}}, {500.0f, {1, 1, 1}}};
    clip.AddTrack(std::move(track));

    clip.NormalizeToSeconds();

    CHECK(Near(clip.GetDuration(), 0.5f));        // 500 ticks / 1000 = 0.5 秒
    CHECK(Near(clip.GetTicksPerSecond(), 1.0f));  // 以降は「秒」単位

    const BoneTrack* t = clip.FindTrackForBone(0);
    CHECK(t != nullptr);
    if (t)
    {
        CHECK(Near(t->positionKeys[1].time, 0.25f));
        CHECK(Near(t->positionKeys[2].time, 0.5f));
        CHECK(Near(t->rotationKeys[1].time, 0.5f));
        CHECK(Near(t->scaleKeys[1].time, 0.5f));
        // 値は不変
        CHECK(Near(t->positionKeys[1].value.x, 1.0f));
    }

    // Animator::Update と同じ進行式: currentTime += dt * tps。正規化後は tps=1 なので
    // 実時間 0.25 秒でクリップのちょうど中間（250ms キー）に到達する。
    float currentTime = 0.0f;
    for (int i = 0; i < 15; ++i) currentTime += (1.0f / 60.0f) * clip.GetTicksPerSecond();
    CHECK(Near(currentTime, 0.25f, 1e-4f));
    CHECK(currentTime < clip.GetDuration());
}

// FBX 相当: 30 ticks/s、duration=60 ticks（= 2 秒）
static void TestFbxClip()
{
    AnimationClip clip;
    clip.SetTicksPerSecond(30.0f);
    clip.SetDuration(60.0f);

    BoneTrack track;
    track.boneIndex = 3;
    track.positionKeys = {{0.0f, {0, 0, 0}}, {30.0f, {0, 1, 0}}};
    clip.AddTrack(std::move(track));

    clip.NormalizeToSeconds();

    CHECK(Near(clip.GetDuration(), 2.0f));
    CHECK(Near(clip.GetTicksPerSecond(), 1.0f));
    CHECK(Near(clip.FindTrackForBone(3)->positionKeys[1].time, 1.0f));
}

// tps 不明(<=0)は Assimp 既定の 25 ticks/s とみなす
static void TestZeroTpsFallback()
{
    AnimationClip clip;
    clip.SetTicksPerSecond(0.0f);
    clip.SetDuration(50.0f);
    clip.NormalizeToSeconds();
    CHECK(Near(clip.GetDuration(), 2.0f));        // 50 / 25 = 2 秒
    CHECK(Near(clip.GetTicksPerSecond(), 1.0f));
}

int main()
{
    TestGltfMillisecondClip();
    TestFbxClip();
    TestZeroTpsFallback();

    std::printf("AnimClipNormalizeTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
