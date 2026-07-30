// Animator::CrossFadeTo を「毎フレーム同じ行き先で呼ぶ」使い方のテスト。
//
// ★何を守っているか
//   docs/SCRIPTING.md と docs/ANIMATION.md が勧めている書き方は
//
//       function OnUpdate(self, dt)
//         if moving then e:playAnimByName("Walk", 0.25)
//         else            e:playAnimByName("Idle", 0.25) end
//       end
//
//   ＝**毎フレーム CrossFadeTo が呼ばれる**。ところが CrossFadeTo は毎回
//   m_blendFactor を 0 に戻していたので、ブレンドが永久に完了しなかった。
//   完了しない間は m_clip が旧クリップのままなので冒頭の
//   `nextClip == m_clip` ガードも素通りする＝アニメが一生切り替わらない
//   （見た目は「Walk が 7% だけ混ざった Idle」で固まる）。
//   blendDuration を 0 にすると即時切替パスに落ちて突然直るので、
//   原因にたどり着きにくい形だった。
//
// Animator/Skeleton/AnimationClip は純ロジック（GPU 非依存）なので直接ビルドして検証する。
#include "animation/Animator.h"
#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

namespace
{
int g_checks = 0;
int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

void BuildSingleBoneSkeleton(Skeleton& skeleton)
{
    BoneNode bone;
    bone.name = "root";
    bone.parentIndex = -1;
    XMStoreFloat4x4(&bone.inverseBindPose, XMMatrixIdentity());
    XMStoreFloat4x4(&bone.localBindPose, XMMatrixIdentity());
    skeleton.AddBone(std::move(bone));
}

void BuildClip(AnimationClip& clip, float x)
{
    clip.SetTicksPerSecond(1.0f);
    clip.SetDuration(10.0f);
    BoneTrack track;
    track.boneIndex = 0;
    track.positionKeys = {{0.0f, {x, 0, 0}}, {10.0f, {x, 0, 0}}};
    track.rotationKeys = {{0.0f, {0, 0, 0, 1}}, {10.0f, {0, 0, 0, 1}}};
    track.scaleKeys    = {{0.0f, {1, 1, 1}}, {10.0f, {1, 1, 1}}};
    clip.AddTrack(std::move(track));
}
}

int main()
{
    Skeleton skeleton;
    BuildSingleBoneSkeleton(skeleton);
    AnimationClip idle, walk, run;
    BuildClip(idle, 0.0f);
    BuildClip(walk, 1.0f);
    BuildClip(run,  2.0f);

    const float kDt    = 1.0f / 60.0f;
    const float kBlend = 0.25f;   // 60fps なら 15 フレームで完了する想定

    // ---- 1) 毎フレーム同じ行き先で呼んでも、ブレンドは完了して切り替わる ----
    {
        Animator a;
        a.Initialize(&skeleton, &idle);
        CHECK(a.GetClip() == &idle);

        for (int i = 0; i < 60; ++i)   // 1 秒ぶん。blend 0.25 秒なら余裕で終わる
        {
            a.CrossFadeTo(&walk, kBlend);   // ★ゲーム側の書き方どおり毎フレーム呼ぶ
            a.Update(kDt);
        }
        CHECK(a.GetClip() == &walk);        // 修正前はここが idle のまま
        CHECK(!a.IsBlending());
    }

    // ---- 2) ブレンド率が単調に進む（毎フレーム 0 へ巻き戻らない）----
    {
        Animator a;
        a.Initialize(&skeleton, &idle);
        float prev = -1.0f;
        for (int i = 0; i < 10; ++i)        // まだ完了しない範囲で見る
        {
            a.CrossFadeTo(&walk, kBlend);
            a.Update(kDt);
            if (!a.IsBlending()) break;
            CHECK(a.GetBlendFactor() > prev);   // 修正前は毎回 ~0.067 で頭打ち
            prev = a.GetBlendFactor();
        }
        CHECK(prev > 0.3f);   // 10 フレームで 10/15 ≒ 0.66 まで進んでいるはず
    }

    // ---- 3) ブレンド中に**別の**行き先へ変えたときは、ちゃんと乗り換える ----
    //      （上のガードで「何も切り替わらない」まで潰してしまっていないか）
    {
        Animator a;
        a.Initialize(&skeleton, &idle);
        a.CrossFadeTo(&walk, kBlend);
        a.Update(kDt);
        CHECK(a.GetNextClip() == &walk);

        a.CrossFadeTo(&run, kBlend);       // 行き先を変更
        CHECK(a.GetNextClip() == &run);
        CHECK(a.GetBlendFactor() == 0.0f); // 新しい行き先なので巻き戻るのが正しい

        for (int i = 0; i < 60; ++i) { a.CrossFadeTo(&run, kBlend); a.Update(kDt); }
        CHECK(a.GetClip() == &run);
    }

    // ---- 4) 既に再生中のクリップを指定しても何も起きない（従来どおり）----
    {
        Animator a;
        a.Initialize(&skeleton, &idle);
        a.CrossFadeTo(&idle, kBlend);
        CHECK(!a.IsBlending());
        CHECK(a.GetClip() == &idle);
    }

    if (g_failures != 0)
    {
        std::printf("animator_crossfade: %d checks, %d failure(s)\n", g_checks, g_failures);
        return 1;
    }
    std::printf("animator_crossfade: %d checks, all passed\n", g_checks);
    return 0;
}
