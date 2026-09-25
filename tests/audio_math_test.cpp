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
#include <cstring>
#include <vector>

#if defined(_WIN32)
// プリセット表が SDK の XAUDIO2FX_I3DL2_PRESET_* を正しく写しているかを突き合わせる
#include <windows.h>
#include <xaudio2fx.h>
#endif

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

void TestSnapshot()
{
    std::printf("[スナップショットの補間]\n");
    const BusMod idn{};
    const BusMod hidden{0.5f, 800.0f};
    {
        const BusMod a = LerpBusMod(idn, hidden, 0.0f);
        const BusMod b = LerpBusMod(idn, hidden, 1.0f);
        Check(a.gain == 1.0f && a.lowpassHz == 0.0f, "t=0 は遷移元（補正なし）");
        Check(b.gain == 0.5f && Near(b.lowpassHz, 800.0f, 0.01f), "t=1 は遷移先");
    }
    {
        const BusMod m = LerpBusMod(idn, hidden, 0.5f);
        Check(Near(m.gain, 0.75f), "音量は線形");
        Check(Near(m.lowpassHz, std::sqrt(kLowpassOpenHz * 800.0f), 1.0f),
              "ローパスは対数で補間（無し=24k と 800Hz の幾何平均 ≒ 4.4kHz。線形なら 12.4kHz）");
    }
    {
        const BusMod m = LerpBusMod(hidden, idn, 0.999f);
        Check(m.lowpassHz == 0.0f || m.lowpassHz > 15000.0f, "戻りの終わり際はほぼ開いている");
        const BusMod e = LerpBusMod(hidden, idn, 1.0f);
        Check(e.lowpassHz == 0.0f, "戻り切ったら『無し』（0）に戻る（24kHz のフィルタを残さない）");
    }
    {
        const BusMod m = LerpBusMod(idn, BusMod{0.2f, 0.0f}, 0.5f);
        Check(m.lowpassHz == 0.0f, "両端とも無しなら途中も無し");
    }
    Check(TransitionProgress(0.0f, 0.0f) == 1.0f, "duration 0 は即座に完了");
    Check(TransitionProgress(0.0f, 2.0f) == 0.0f, "始まりは 0");
    Check(TransitionProgress(2.0f, 2.0f) == 1.0f && TransitionProgress(5.0f, 2.0f) == 1.0f, "終わりは 1 で止まる");
    Check(Near(TransitionProgress(1.0f, 2.0f), 0.5f), "中点は 0.5（smoothstep）");
    Check(TransitionProgress(0.2f, 2.0f) < 0.1f, "出だしはゆっくり（smoothstep）");
    float prev = 0.0f;
    bool mono = true;
    for (int i = 1; i <= 50; ++i)
    {
        const float p = TransitionProgress(i * 0.04f, 2.0f);
        if (p < prev) mono = false;
        prev = p;
    }
    Check(mono, "単調に増える（戻ったりしない）");
}

void TestMeter()
{
    std::printf("[メーターの針]\n");
    MeterBallistics m;
    for (int i = 0; i < 60; ++i) MeterStep(m, 0.0f, 0.0f, 1.0f / 60.0f);
    Check(MeterPeakDb(m) == kSilenceDb && MeterRmsDb(m) == kSilenceDb, "無音なら -120（-inf ではない）");

    MeterStep(m, 0.5f, 0.35f, 1.0f / 60.0f);
    Check(Near(MeterPeakDb(m), -6.02f, 0.05f), "ピークは上がるとき即座");
    Check(MeterRmsDb(m) < -20.0f, "RMS はならすので 1 フレームでは上がり切らない");
    for (int i = 0; i < 120; ++i) MeterStep(m, 0.5f, 0.35f, 1.0f / 60.0f);
    Check(Near(MeterRmsDb(m), LinearToDb(0.35f), 0.2f), "2 秒続けば RMS は実際の値に落ち着く");

    // 音が止まってもピークは 0.5 秒保持してから落ちる
    for (int i = 0; i < 20; ++i) MeterStep(m, 0.0f, 0.0f, 1.0f / 60.0f);   // 0.33 秒
    Check(Near(MeterPeakDb(m), -6.02f, 0.05f), "止まって 0.33 秒はピークを保持");
    for (int i = 0; i < 60; ++i) MeterStep(m, 0.0f, 0.0f, 1.0f / 60.0f);   // さらに 1 秒
    const float after = MeterPeakDb(m);
    Check(after < -20.0f && after > -40.0f, "保持の後は 24dB/秒で落ちる（約 0.83 秒で -20dB）");
    for (int i = 0; i < 600; ++i) MeterStep(m, 0.0f, 0.0f, 1.0f / 60.0f);
    Check(MeterPeakDb(m) == kSilenceDb && MeterRmsDb(m) == kSilenceDb, "十分経てば無音へ戻る");

    MeterBallistics bad;
    MeterStep(bad, std::nanf(""), -1.0f, 0.016f);
    Check(MeterPeakDb(bad) == kSilenceDb && MeterRmsDb(bad) == kSilenceDb, "NaN / 負の読みは 0 扱い");
}

void TestZoneShape()
{
    std::printf("[リバーブ域の形と重み]\n");
    const float half[3] = {4.0f, 2.0f, 4.0f};
    const float in[3]   = {3.9f, -1.9f, 0.0f};
    const float side[3] = {5.0f, 0.0f, 0.0f};
    const float corner[3] = {7.0f, 6.0f, 0.0f};
    Check(BoxOutsideDistance(in, half) == 0.0f, "箱の内側は距離 0");
    Check(Near(BoxOutsideDistance(side, half), 1.0f), "面の外側は面までの距離");
    Check(Near(BoxOutsideDistance(corner, half), 5.0f), "角の外側は角までの距離（3-4-5）");
    const float s[3] = {0.0f, 6.0f, 0.0f};
    Check(Near(SphereOutsideDistance(s, 5.0f), 1.0f), "球の外側は表面までの距離");
    Check(SphereOutsideDistance(in, 5.0f) == 0.0f, "球の内側は 0");

    Check(ZoneWeight(0.0f, 2.0f) == 1.0f, "内側は重み 1");
    Check(Near(ZoneWeight(1.0f, 2.0f), 0.5f), "fade の中ほどで 0.5");
    Check(ZoneWeight(2.0f, 2.0f) == 0.0f, "fade の外端で 0");
    Check(ZoneWeight(0.01f, 0.0f) == 0.0f, "fade 0 はくっきり切り替え（外に出たら 0）");
}

void TestReverbPresets()
{
    std::printf("[リバーブのプリセット]\n");
    ReverbParams r;
    Check(FindReverbPreset("room", r) && Near(r.decayTime, 0.40f), "room が引ける");
    Check(FindReverbPreset("CAVE", r) && Near(r.decayTime, 2.91f), "大文字小文字は区別しない");
    ReverbParams keep;
    keep.decayTime = 9.9f;
    Check(!FindReverbPreset("dungeon", keep) && keep.decayTime == 9.9f, "未知は false で書き換えない");
    Check(!FindReverbPreset("roo", r) && !FindReverbPreset("rooms", r), "前方一致では引かない");

#if defined(_WIN32)
    struct { const char* name; XAUDIO2FX_REVERB_I3DL2_PARAMETERS sdk; } kSdk[] = {
        {"none",          XAUDIO2FX_I3DL2_PRESET_DEFAULT},
        {"generic",       XAUDIO2FX_I3DL2_PRESET_GENERIC},
        {"closet",        XAUDIO2FX_I3DL2_PRESET_PADDEDCELL},
        {"room",          XAUDIO2FX_I3DL2_PRESET_ROOM},
        {"smallroom",     XAUDIO2FX_I3DL2_PRESET_SMALLROOM},
        {"largeroom",     XAUDIO2FX_I3DL2_PRESET_LARGEROOM},
        {"bathroom",      XAUDIO2FX_I3DL2_PRESET_BATHROOM},
        {"stoneroom",     XAUDIO2FX_I3DL2_PRESET_STONEROOM},
        {"hallway",       XAUDIO2FX_I3DL2_PRESET_HALLWAY},
        {"stonecorridor", XAUDIO2FX_I3DL2_PRESET_STONECORRIDOR},
        {"hall",          XAUDIO2FX_I3DL2_PRESET_CONCERTHALL},
        {"cave",          XAUDIO2FX_I3DL2_PRESET_CAVE},
        {"sewer",         XAUDIO2FX_I3DL2_PRESET_SEWERPIPE},
        {"hangar",        XAUDIO2FX_I3DL2_PRESET_HANGAR},
        {"forest",        XAUDIO2FX_I3DL2_PRESET_FOREST},
        {"city",          XAUDIO2FX_I3DL2_PRESET_CITY},
        {"outdoor",       XAUDIO2FX_I3DL2_PRESET_PLAIN},
        {"underwater",    XAUDIO2FX_I3DL2_PRESET_UNDERWATER},
    };
    std::size_t n = 0;
    ReverbPresetTable(n);
    Check(n == sizeof(kSdk) / sizeof(kSdk[0]), "プリセットの数が SDK 突き合わせ表と同じ");
    for (const auto& e : kSdk)
    {
        ReverbParams p;
        const bool found = FindReverbPreset(e.name, p);
        const bool same = found
            && p.room == static_cast<float>(e.sdk.Room) && p.roomHF == static_cast<float>(e.sdk.RoomHF)
            && Near(p.decayTime, e.sdk.DecayTime) && Near(p.decayHFRatio, e.sdk.DecayHFRatio)
            && p.reflections == static_cast<float>(e.sdk.Reflections)
            && Near(p.reflectionsDelay, e.sdk.ReflectionsDelay)
            && p.reverb == static_cast<float>(e.sdk.Reverb) && Near(p.reverbDelay, e.sdk.ReverbDelay)
            && Near(p.diffusion, e.sdk.Diffusion) && Near(p.density, e.sdk.Density)
            && Near(p.hfReference, e.sdk.HFReference);
        char label[96];
        std::snprintf(label, sizeof(label), "プリセット '%s' が SDK の値と一致", e.name);
        Check(same, label);
    }
#endif
}

void TestReverbBlend()
{
    std::printf("[リバーブ域の混ぜ方]\n");
    ReverbParams none, room, cave;
    FindReverbPreset("none", none);
    FindReverbPreset("room", room);
    FindReverbPreset("cave", cave);

    {
        const ReverbMix m = BlendZones(nullptr, 0, none, 0.0f);
        Check(m.wet == 0.0f && m.params.room == none.room, "ゾーンが無ければ既定のまま");
    }
    {
        ZoneSample z;
        z.weight = 1.0f; z.wet = 0.6f; z.params = room;
        const ReverbMix m = BlendZones(&z, 1, none, 0.0f);
        Check(Near(m.wet, 0.6f) && Near(m.params.decayTime, room.decayTime), "内側ではゾーンそのもの");
    }
    {
        ZoneSample z;
        z.weight = 0.5f; z.wet = 0.6f; z.params = room;
        const ReverbMix m = BlendZones(&z, 1, none, 0.0f);
        Check(Near(m.wet, 0.3f), "fade の中ほどでは量が半分");
        Check(Near(m.params.room, room.room),
              "響きが無いところから入るときは性格は即座にゾーンのもの（none と混ぜて二重に絞らない）");
    }
    {
        // 洞窟（外側, 優先度 0）の中の小部屋（内側, 優先度 1）
        ZoneSample zs[2];
        zs[0].priority = 0; zs[0].weight = 1.0f; zs[0].wet = 0.8f; zs[0].params = cave;
        zs[1].priority = 1; zs[1].weight = 1.0f; zs[1].wet = 0.4f; zs[1].params = room;
        const ReverbMix m = BlendZones(zs, 2, none, 0.0f);
        Check(Near(m.params.decayTime, room.decayTime) && Near(m.wet, 0.4f), "入れ子の内側（優先度が高い方）が勝つ");
        zs[1].weight = 0.5f;
        const ReverbMix h = BlendZones(zs, 2, none, 0.0f);
        Check(Near(h.params.decayTime, (cave.decayTime + room.decayTime) * 0.5f) && Near(h.wet, 0.6f),
              "内側の境界では外側と内側の中間");
    }
    {
        ReverbParams a = none, b = room;
        const ReverbParams mid = LerpReverb(a, b, 0.5f);
        Check(Near(mid.room, (a.room + b.room) * 0.5f) && Near(mid.decayTime, (a.decayTime + b.decayTime) * 0.5f),
              "LerpReverb の中点");
        Check(ReverbDistance(a, a) == 0.0f && ReverbDistance(a, b) > 1.0f, "同じなら差 0、違えば大きい");
    }
    Check(SmoothFactor(0.0f, 0.35f) == 0.0f, "dt=0 なら動かない");
    Check(SmoothFactor(1.0f, 0.0f) == 1.0f, "tau=0 は即座に追従");
    Check(Near(SmoothFactor(0.35f, 0.35f), 1.0f - std::exp(-1.0f)), "時定数ぶんで 63%");
    Check(SmoothFactor(100.0f, 0.35f) <= 1.0f, "dt が大きくても行き過ぎない");
}
} // namespace

int main()
{
    TestDb();
    TestFilter();
    TestDistance();
    TestVirtual();
    TestPickVictim();
    TestSnapshot();
    TestMeter();
    TestZoneShape();
    TestReverbPresets();
    TestReverbBlend();

    std::printf("\naudio_math: %d チェック / %d 件 NG\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
