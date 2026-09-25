// 音まわりの純粋ロジック（src/audio/AudioMath.h）のテスト。
//
// XAudio2 も音声デバイスも要らない。ここで固めるのは「耳では気づきにくい決め事」:
//   - 上限に達したとき、どの音を奪うか（優先度 → 聞こえ具合 → 古さ）
//   - 仮想化のヒステリシス（境界の上でボイスを作っては壊すのを防ぐ）
//   - dB と線形の換算、XAudio2 のフィルタ周波数（fs/6 を超えたら素通し）
//
// 実行: ctest --output-on-failure -R AudioMath

#include "audio/AudioMath.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace dx12e::audio;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool cond, const char* label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label);
}

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void TestDb()
{
    std::printf("[dB]\n");
    Check(Near(LinearToDb(1.0f), 0.0f), "1.0 は 0 dB");
    Check(Near(LinearToDb(0.5f), -6.0206f, 1e-3f), "0.5 は -6.02 dB");
    Check(LinearToDb(0.0f) == kSilenceDb, "0 は無音の床（-inf ではなく -120）");
    Check(LinearToDb(std::nanf("")) == kSilenceDb, "NaN も床へ落とす");
    Check(Near(DbToLinear(-6.0206f), 0.5f, 1e-4f), "-6.02 dB は 0.5");
    Check(DbToLinear(kSilenceDb) == 0.0f, "床は 0");
}

void TestFilter()
{
    std::printf("[フィルタ]\n");
    Check(CutoffToFilterFrequency(0.0f, 48000.0f) == 1.0f, "0 Hz = ローパス無し = 1.0（素通し）");
    Check(CutoffToFilterFrequency(8000.0f, 48000.0f) == 1.0f, "fs/6 ちょうどは 1.0");
    Check(CutoffToFilterFrequency(15000.0f, 48000.0f) == 1.0f, "fs/6 を超える指定は素通し（表せない）");
    const float f420 = CutoffToFilterFrequency(420.0f, 44100.0f);
    Check(Near(f420, 2.0f * std::sin(3.14159265f * 420.0f / 44100.0f)), "420Hz@44.1k は 2sin(pi fc/fs)");
    Check(f420 > 0.0f && f420 < 0.1f, "420Hz は十分小さい係数");
    Check(CombineLowpass(0.0f, 0.0f) == 0.0f, "無し × 無し = 無し");
    Check(CombineLowpass(0.0f, 800.0f) == 800.0f, "無し × 800 = 800");
    Check(CombineLowpass(1200.0f, 800.0f) == 800.0f, "直列は低い方が支配的");
}

void TestDistance()
{
    std::printf("[距離減衰]\n");
    Check(DistanceGain(0.5f, 1.0f, 30.0f) == 1.0f, "minDistance の内側はフル音量");
    Check(DistanceGain(30.0f, 1.0f, 30.0f) == 0.0f, "maxDistance で 0");
    Check(DistanceGain(100.0f, 1.0f, 30.0f) == 0.0f, "外側も 0");
    Check(Near(DistanceGain(15.5f, 1.0f, 30.0f), 0.5f), "中点で 0.5（直線）");
    Check(DistanceGain(5.0f, 1.0f, 0.0f) == 1.0f, "maxDistance 0 は減衰なし（0 割りしない）");
}

void TestVirtual()
{
    std::printf("[仮想化のヒステリシス]\n");
    Check(NextVirtualState(false, 0.0005f) == true, "-66dB の実ボイスは仮想へ");
    Check(NextVirtualState(false, 0.0015f) == false, "-56dB の実ボイスはそのまま（入る閾値 -60dB より上）");
    Check(NextVirtualState(true, 0.0015f) == true, "-56dB の仮想ボイスはまだ戻らない（出る閾値 -54dB より下）");
    Check(NextVirtualState(true, 0.003f) == false, "-50dB の仮想ボイスは実へ戻る");
    // 境界の上で揺れても入れ替わらない（入って出てを繰り返さない）
    bool v = false;
    int flips = 0;
    for (int i = 0; i < 100; ++i)
    {
        const float a = (i % 2) ? 0.0012f : 0.0018f;   // 両閾値の間で揺れる
        const bool n = NextVirtualState(v, a);
        if (n != v) ++flips;
        v = n;
    }
    Check(flips == 0, "閾値の間で揺れても一度も入れ替わらない");
}

void TestPickVictim()
{
    std::printf("[奪う相手の選び方]\n");
    {
        std::vector<VoiceCandidate> c = {
            {10, 128, 0.8f, 1}, {11, 64, 0.9f, 2}, {12, 200, 0.1f, 3},
        };
        const int k = PickVictim(c.data(), c.size(), 128, 0.5f);
        Check(k == 1, "優先度が一番低いもの（64）を選ぶ（聞こえ具合より優先度が先）");
    }
    {
        std::vector<VoiceCandidate> c = {
            {10, 128, 0.8f, 1}, {11, 128, 0.2f, 2}, {12, 128, 0.5f, 3},
        };
        const int k = PickVictim(c.data(), c.size(), 128, 0.5f);
        Check(k == 1, "同じ優先度なら一番小さく聞こえているもの");
    }
    {
        std::vector<VoiceCandidate> c = {
            {10, 128, 0.5f, 7}, {11, 128, 0.5f, 3}, {12, 128, 0.5f, 9},
        };
        const int k = PickVictim(c.data(), c.size(), 128, 0.5f);
        Check(k == 1, "それも同じなら一番古いもの");
    }
    {
        std::vector<VoiceCandidate> c = { {10, 200, 0.1f, 1}, {11, 150, 0.1f, 2} };
        Check(PickVictim(c.data(), c.size(), 128, 1.0f) == -1, "全部こちらより大事なら奪わない");
    }
    {
        std::vector<VoiceCandidate> c = { {10, 128, 0.9f, 1} };
        Check(PickVictim(c.data(), c.size(), 128, 0.3f) == -1,
              "同じ優先度でこちらの方が小さいなら奪わない（聞こえない音で聞こえる音を消さない）");
        Check(PickVictim(c.data(), c.size(), 129, 0.3f) == 0, "優先度が 1 でも上なら奪う");
    }
    {
        std::vector<VoiceCandidate> c = { {10, 128, 0.4f, 1} };
        Check(PickVictim(c.data(), c.size(), 128, 0.5f, 1.0f) == 0, "margin=1 なら少しでも大きければ奪う");
        Check(PickVictim(c.data(), c.size(), 128, 0.5f, 2.0f) == -1,
              "margin=2 だと 2 倍大きくないと奪わない（仮想→実の奪い合いのちらつき防止）");
        Check(PickVictim(c.data(), c.size(), 128, 0.9f, 2.0f) == 0, "2 倍を超えれば奪う");
    }
    Check(PickVictim(nullptr, 0, 128, 1.0f) == -1, "候補が無ければ -1");
}
} // namespace

int main()
{
    TestDb();
    TestFilter();
    TestDistance();
    TestVirtual();
    TestPickVictim();

    std::printf("\naudio_math: %d チェック / %d 件 NG\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
