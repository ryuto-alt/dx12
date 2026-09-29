#pragma once

// ===========================================================================
// 起動画面の「演出監督」（ヘッダオンリー。Win32 / GPU に依存しない）
// ---------------------------------------------------------------------------
// タイムライン（SplashMotion.h）に、文言のクロスフェード・Tips のローテーション・ready 時の文言を合わせて、
// 描画器（SplashRenderer）へ渡す SplashFrame + SplashContent を 1 フレームぶん組み立てる。
// 実窓（SplashScreen.cpp）とプレビュー（SplashPreview.cpp）が同じ Director を通るので、
// プレビューの PNG は実窓と同じ演出になる。
// ===========================================================================

#include <string>
#include <utility>
#include <vector>

#include "core/SplashMotion.h"
#include "core/SplashRenderer.h"

namespace dx12e::splash
{

// ready（起動完了）の一言。毎回少しだけ違う（種で選ぶ）。標準語・上品に。エンジンが温まった、のようなささやかな洒落まで。
inline std::vector<std::wstring> DefaultReadyLabels()
{
    return { L"準備が整いました", L"エンジンが温まりました", L"お待たせしました。始めましょう",
             L"準備完了。ロゴも元気です", L"今日もよい作品を作りましょう" };
}

struct DirectorConfig
{
    bool projectMode = false;
    uint32_t tipSeed = 1;                 // Tips の開始位置（乱数の種）
    std::vector<std::wstring> tips;       // 標準語の Tips（SplashTips.h を変換したもの）
    SplashContent base;                   // title / version / buildInfo / projectName / sceneName / recents
    std::wstring readyLabel = L"準備が整いました";
    std::vector<std::wstring> readyLabels;   // 空でなければ、種（tipSeed）で 1 つ選んで readyLabel にする
    TimingParams timing;
};

class Director
{
public:
    explicit Director(DirectorConfig cfg)
        : cfg_(std::move(cfg)), tl_(cfg_.timing), swap_(0.25),
          tips_(static_cast<int>(cfg_.tips.size()), cfg_.tipSeed), content_(cfg_.base)
    {
        content_.projectMode = cfg_.projectMode;
        if (!cfg_.readyLabels.empty())
            cfg_.readyLabel = cfg_.readyLabels[Hash32(cfg_.tipSeed * 31u + 7u) % cfg_.readyLabels.size()];
    }

    // 1 フレーム進める。status = いまの状態文言 / stepIndex,stepTotal = 手順 / target = 実進捗。
    SplashFrame Step(double t, double target, const std::wstring& status, int stepIndex, int stepTotal,
                     double finishAt, double mainShownAt)
    {
        SplashFrame f = tl_.Step(t, target, finishAt, mainShownAt);
        // 状態文言: ready に入ったら「準備が整いました」へ切り替える
        const bool ready = f.sinceReady >= 0.0;
        swap_.Set(ready ? cfg_.readyLabel : status, t);
        const TextSwapState q = swap_.Query(t);
        content_.stepCur = q.cur;
        content_.stepPrev = q.prev;
        content_.stepU = q.u;
        content_.stepIndex = ready ? stepTotal : stepIndex;
        content_.stepTotal = stepTotal;
        // Tips（3.5 秒ごとにフェード切替）。登場の演出（0.7s）に合わせて時計を遅らせる
        if (!cfg_.tips.empty())
        {
            const TipState s = tips_.Query(std::max(0.0, t - 0.7));
            content_.tip = cfg_.tips[static_cast<size_t>(s.index)];
            content_.tipAlpha = s.alpha;
            content_.tipDy = s.dy;
        }
        return f;
    }

    const SplashContent& content() const { return content_; }
    const Timeline& timeline() const { return tl_; }
    Timeline& timeline() { return tl_; }
    const TipRotator& tips() const { return tips_; }

private:
    DirectorConfig cfg_;
    Timeline tl_;
    TextSwapper swap_;
    TipRotator tips_;
    SplashContent content_;
};

} // namespace dx12e::splash
