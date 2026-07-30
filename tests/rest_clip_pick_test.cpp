// PickRestClip（レストポーズに使うノードアニメクリップの選択）の単体テスト。
//
// ★何を守っているか: この選択は 2 箇所で使われる。
//   - ModelLoader … 選んだクリップの t=0 の姿勢で頂点を焼き込む
//   - Scene       … 同じクリップを inverseRest として NodeAnimator へ渡す
//   描画は inv(rest) * current なので、**両者が違うクリップを選ぶと残差がそのまま
//   余分な変換として絵に出る**（モデルが余計に回る / ずれる）。
//   以前は ModelLoader が clips[0] 決め打ち、Scene が "static" 優先で食い違っていた。
//   関数を 1 本に集約したので、ここでその関数の規則を固定する。
// NodeAnimationClip.cpp は純ロジック（GPU非依存、DirectXMath のみ）なので直接ビルドする。
#include "animation/NodeAnimationClip.h"

#include <cstdio>
#include <memory>
#include <vector>

using namespace dx12e;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static std::unique_ptr<NodeAnimationClip> MakeClip(const char* name)
{
    auto c = std::make_unique<NodeAnimationClip>();
    c->SetName(name);
    return c;
}

int main()
{
    // 空 → nullptr（呼び出し側が [0] を触って落ちないこと）
    {
        std::vector<std::unique_ptr<NodeAnimationClip>> clips;
        CHECK(PickRestClip(clips) == nullptr);
    }

    // "static" が無ければ先頭
    {
        std::vector<std::unique_ptr<NodeAnimationClip>> clips;
        clips.push_back(MakeClip("Walk"));
        clips.push_back(MakeClip("Run"));
        CHECK(PickRestClip(clips) == clips[0].get());
    }

    // ★本題: "static" が先頭でなくても "static" が選ばれる
    {
        std::vector<std::unique_ptr<NodeAnimationClip>> clips;
        clips.push_back(MakeClip("Walk"));
        clips.push_back(MakeClip("static"));
        clips.push_back(MakeClip("Run"));
        CHECK(PickRestClip(clips) == clips[1].get());
    }

    // "static" が先頭にあるとき（従来から一致していたケース）
    {
        std::vector<std::unique_ptr<NodeAnimationClip>> clips;
        clips.push_back(MakeClip("static"));
        clips.push_back(MakeClip("Walk"));
        CHECK(PickRestClip(clips) == clips[0].get());
    }

    // null 要素が混ざっていても落ちない（クリップ構築に失敗した場合の保険）
    {
        std::vector<std::unique_ptr<NodeAnimationClip>> clips;
        clips.push_back(nullptr);
        clips.push_back(MakeClip("static"));
        CHECK(PickRestClip(clips) == clips[1].get());
    }

    if (g_failures != 0)
    {
        std::printf("rest_clip_pick: %d checks, %d failure(s)\n", g_checks, g_failures);
        return 1;
    }
    std::printf("rest_clip_pick: %d checks, all passed\n", g_checks);
    return 0;
}
