// UIアニメクリップ(animation/UiAnimAsset.h)の評価テスト。
// 評価は純関数(GPU/ECS/JSON非依存)なのでヘッダだけで動く = 依存ゼロ CI で回る。
//
// 実行: ctest --output-on-failure （失敗があれば終了コード 1）

#include "animation/UiAnimAsset.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b, float tol = 1e-4f)
{
    return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b));
}

void check(bool cond, const char* what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

void checkf(float got, float want, const char* what)
{
    ++g_checks;
    if (!feq(got, want))
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %.6f, want %.6f)\n", what, got, want);
    }
}

UiAnimTrack MakeTrack(int prop, std::vector<UiAnimKey> keys)
{
    UiAnimTrack tr;
    tr.prop = prop;
    tr.keys = std::move(keys);
    return tr;
}

// --- 評価: 端のホールドと線形補間 -------------------------------------------
void TestEvaluateBasics()
{
    std::printf("TestEvaluateBasics\n");

    // キー無し → プロパティの中立値
    UiAnimTrack empty = MakeTrack(kUiPropScaleX, {});
    checkf(EvaluateUiAnimTrack(empty, 0.5f), 1.0f, "空トラックのスケールは 1");
    UiAnimTrack emptyPos = MakeTrack(kUiPropOffsetMinX, {});
    checkf(EvaluateUiAnimTrack(emptyPos, 0.5f), 0.0f, "空トラックの位置は 0");

    // easing=0(リニア)で 0→100
    UiAnimTrack tr = MakeTrack(kUiPropOffsetMinX, {{0.0f, 0.0f, 0}, {1.0f, 100.0f, 0}});
    checkf(EvaluateUiAnimTrack(tr, -5.0f),  0.0f,   "最初のキーより前は先頭値でホールド");
    checkf(EvaluateUiAnimTrack(tr,  0.0f),  0.0f,   "t=0");
    checkf(EvaluateUiAnimTrack(tr,  0.25f), 25.0f,  "リニア補間 t=0.25");
    checkf(EvaluateUiAnimTrack(tr,  0.5f),  50.0f,  "リニア補間 t=0.5");
    checkf(EvaluateUiAnimTrack(tr,  1.0f),  100.0f, "t=1");
    checkf(EvaluateUiAnimTrack(tr,  9.0f),  100.0f, "最後のキーより後は末尾値でホールド");

    // 単一キーはどの時刻でもその値
    UiAnimTrack one = MakeTrack(kUiPropRotation, {{0.5f, 42.0f, 0}});
    checkf(EvaluateUiAnimTrack(one, 0.0f), 42.0f, "単一キー t=0");
    checkf(EvaluateUiAnimTrack(one, 9.9f), 42.0f, "単一キー t=末尾以降");
}

// --- 評価: 3キー以上の区間選択（二分探索の境界） ----------------------------
void TestEvaluateMultiSegment()
{
    std::printf("TestEvaluateMultiSegment\n");

    UiAnimTrack tr = MakeTrack(kUiPropOffsetMinY,
                               {{0.0f, 0.0f, 0}, {1.0f, 10.0f, 0},
                                {2.0f, 30.0f, 0}, {3.0f, 30.0f, 0}});
    checkf(EvaluateUiAnimTrack(tr, 0.5f), 5.0f,  "第1区間の中点");
    checkf(EvaluateUiAnimTrack(tr, 1.0f), 10.0f, "キー境界はそのキーの値");
    checkf(EvaluateUiAnimTrack(tr, 1.5f), 20.0f, "第2区間の中点");
    checkf(EvaluateUiAnimTrack(tr, 2.5f), 30.0f, "値が同じ区間はフラット");

    // 同時刻キーの重なりは「後ろ勝ち」（区間長 0 の除算を踏まない）
    UiAnimTrack dup = MakeTrack(kUiPropOffsetMinX, {{0.0f, 0.0f, 0}, {1.0f, 5.0f, 0}, {1.0f, 99.0f, 0}});
    checkf(EvaluateUiAnimTrack(dup, 1.0f), 99.0f, "同時刻キーは後ろ勝ち");
}

// --- 評価: step プロパティは補間しない --------------------------------------
void TestStepProps()
{
    std::printf("TestStepProps\n");

    check(UiAnimPropIsStep(kUiPropSpriteFrame), "スプライトフレームは step");
    check(UiAnimPropIsStep(kUiPropVisible),     "表示フラグは step");
    check(!UiAnimPropIsStep(kUiPropOffsetMinX), "位置は step ではない");

    UiAnimTrack tr = MakeTrack(kUiPropSpriteFrame, {{0.0f, 0.0f, 0}, {1.0f, 4.0f, 0}});
    checkf(EvaluateUiAnimTrack(tr, 0.0f),  0.0f, "step t=0");
    checkf(EvaluateUiAnimTrack(tr, 0.99f), 0.0f, "step は次のキー直前まで前の値を保持");
    checkf(EvaluateUiAnimTrack(tr, 1.0f),  4.0f, "step はキー時刻で切り替わる");

    UiAnimTrack vis = MakeTrack(kUiPropVisible, {{0.0f, 1.0f, 0}, {0.5f, 0.0f, 0}});
    checkf(EvaluateUiAnimTrack(vis, 0.49f), 1.0f, "表示は 0.5 手前まで 1");
    checkf(EvaluateUiAnimTrack(vis, 0.5f),  0.0f, "表示は 0.5 で 0");
}

// --- 評価: イージングは区間の左キーのものが効く -----------------------------
void TestEasingUsesLeftKey()
{
    std::printf("TestEasingUsesLeftKey\n");

    // 区間1 = リニア、区間2 = easeInCubic(型1)
    UiAnimTrack tr = MakeTrack(kUiPropOffsetMinX,
                               {{0.0f, 0.0f, 0}, {1.0f, 100.0f, 1}, {2.0f, 200.0f, 0}});
    checkf(EvaluateUiAnimTrack(tr, 0.5f), 50.0f, "区間1はリニア");
    // 区間2 の中点: p=0.5 → easeInCubic(0.5)=0.125 → 100 + 100*0.125
    checkf(EvaluateUiAnimTrack(tr, 1.5f), 112.5f, "区間2は左キーの easeInCubic");

    // 端点はイージングによらず一致する（UiEase(x,0)=0 / UiEase(x,1)=1 の性質）
    for (int e = 0; e < kUiEaseCount; ++e)
    {
        checkf(UiEase(e, 0.0f), 0.0f, "UiEase(t=0) は 0");
        checkf(UiEase(e, 1.0f), 1.0f, "UiEase(t=1) は 1");
    }
}

// --- 整列 & duration 自動決定 ------------------------------------------------
void TestSortAndDuration()
{
    std::printf("TestSortAndDuration\n");

    UiAnimClip clip;
    clip.duration = 0.0f;   // 未設定 → 最終キー時刻を採用させる
    clip.tracks.push_back(MakeTrack(kUiPropOffsetMinX,
                                    {{2.0f, 20.0f, 0}, {0.0f, 0.0f, 0}, {1.0f, 10.0f, 0}}));
    clip.events.push_back({1.5f, "mid"});
    clip.events.push_back({0.2f, "early"});

    SortUiAnimClip(clip);

    const auto& keys = clip.tracks[0].keys;
    check(keys[0].time == 0.0f && keys[1].time == 1.0f && keys[2].time == 2.0f,
          "キーが時刻昇順に整列される");
    check(clip.events[0].event == "early" && clip.events[1].event == "mid",
          "イベントも時刻昇順に整列される");
    checkf(clip.duration, 2.0f, "duration=0 は最終キー時刻で埋まる");
    checkf(UiAnimClipEndTime(clip), 2.0f, "EndTime は最終キー時刻");

    // 整列後は評価が正しく効く（未整列だと二分探索が壊れる）
    checkf(EvaluateUiAnimTrack(clip.tracks[0], 0.5f), 5.0f, "整列後は補間が正しい");

    // 完全に空のクリップは duration=1 に落ちる（0除算・無限ループ回避）
    UiAnimClip empty;
    empty.duration = 0.0f;
    SortUiAnimClip(empty);
    checkf(empty.duration, 1.0f, "空クリップの duration は 1 に落ちる");
}

// --- 時刻の畳み込み ----------------------------------------------------------
void TestWrapTime()
{
    std::printf("TestWrapTime\n");

    float t = 1.5f;
    check(WrapUiAnimTime(t, 2.0f, false) == false, "duration 未満なら未完了");
    checkf(t, 1.5f, "未完了時は時刻そのまま");

    t = 2.5f;
    check(WrapUiAnimTime(t, 2.0f, false) == true, "duration 超過で完了");
    checkf(t, 2.0f, "完了時は duration にクランプ");

    t = 2.5f;
    check(WrapUiAnimTime(t, 2.0f, true) == false, "ループは完了しない");
    checkf(t, 0.5f, "ループは剰余で巻き戻る");

    t = 4.0f;
    WrapUiAnimTime(t, 2.0f, true);
    checkf(t, 0.0f, "ちょうど周期の倍数は 0 へ");

    // duration<=0 の病的データ: 0 に固定して非ループなら完了扱い（無限ループ回避）
    t = 5.0f;
    check(WrapUiAnimTime(t, 0.0f, false) == true, "duration<=0 は即完了");
    checkf(t, 0.0f, "duration<=0 は時刻 0");
    t = 5.0f;
    check(WrapUiAnimTime(t, 0.0f, true) == false, "duration<=0 かつループは完了しない");
}

// --- イベント収集 ------------------------------------------------------------
void TestCollectEvents()
{
    std::printf("TestCollectEvents\n");

    UiAnimClip clip;
    clip.duration = 1.0f;
    clip.events = {{0.0f, "zero"}, {0.3f, "a"}, {0.6f, "b"}, {1.0f, "end"}};

    std::vector<const UiAnimEvent*> out;
    CollectUiAnimEvents(clip, 0.2f, 0.7f, out);
    check(out.size() == 2, "区間 (0.2, 0.7] に 2 件");
    if (out.size() == 2)
        check(out[0]->event == "a" && out[1]->event == "b", "拾ったのは a と b");

    // 左は開区間なので、境界にぴったり乗ったイベントを2回拾わない
    out.clear();
    CollectUiAnimEvents(clip, 0.3f, 0.6f, out);
    check(out.size() == 1 && out[0]->event == "b", "左境界のイベントは再発火しない");

    // ループでラップしたフレームは (prev, 末尾] + [0, cur] の両側を見る
    out.clear();
    CollectUiAnimEvents(clip, 0.9f, 0.1f, out);
    check(out.size() == 2, "ラップ時は末尾側と先頭側の両方を拾う");

    // t=0 のイベントはラップ時のみ拾える（開いた左境界の帰結）
    out.clear();
    CollectUiAnimEvents(clip, 0.0f, 0.1f, out);
    check(out.empty(), "t=0 のイベントは 0 から進んだフレームでは拾わない");
}
} // namespace

int main()
{
    std::printf("=== UiAnim tests ===\n");
    TestEvaluateBasics();
    TestEvaluateMultiSegment();
    TestStepProps();
    TestEasingUsesLeftKey();
    TestSortAndDuration();
    TestWrapTime();
    TestCollectEvents();

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
