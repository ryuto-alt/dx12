// 起動音 Calibrate の「音を主時計にする」純ロジック（core/SplashSequencer.h。W1-BD5）の単体テスト。
// 音デバイスは一切使わない（モックの時計と、メモリ上のオフライン描画だけ）。素材は assets/editor/sounds/calibrate/ を読む。
//   SplashSequencerTests <repo-root>
#include "core/SplashSequencer.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace sp = dx12e::splash;

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

std::string g_root;

std::vector<std::uint8_t> ReadAll(const std::string& rel)
{
    std::ifstream f(g_root + "/assets/editor/sounds/calibrate/" + rel, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Files
{
    std::vector<std::uint8_t> full, rm, bed, tail, click[24];
};

std::shared_ptr<const sp::CalibrateBank> LoadBank(Files& fl)
{
    fl.full = ReadAll("graphite_1.wav");
    fl.rm = ReadAll("graphite_1_rm.wav");
    fl.bed = ReadAll("stems/bed.wav");
    fl.tail = ReadAll("stems/lock_tail.wav");
    sp::CalibrateBlobs b;
    auto v = [](const std::vector<std::uint8_t>& x) { return sp::BlobView{ x.data(), x.size() }; };
    b.full = v(fl.full); b.rm = v(fl.rm); b.bed = v(fl.bed); b.lockTail = v(fl.tail);
    for (int i = 0; i < 24; ++i)
    {
        char n[40];
        std::snprintf(n, sizeof(n), "stems/click_%02d.wav", i);
        fl.click[i] = ReadAll(n);
        b.click[i] = v(fl.click[i]);
    }
    auto bank = std::make_shared<sp::CalibrateBank>();
    if (!sp::BuildCalibrateBank(b, *bank)) return nullptr;
    return bank;
}

std::vector<float> RenderAll(sp::CalibrateSequencer& s, std::int64_t frames, int chunk = 441)
{
    std::vector<float> out;
    std::vector<float> tmp(static_cast<size_t>(chunk) * 2);
    while (static_cast<std::int64_t>(out.size() / 2) < frames)
    {
        s.Render(tmp.data(), chunk);
        out.insert(out.end(), tmp.begin(), tmp.end());
    }
    return out;
}

bool EqualRange(const std::vector<float>& a, std::int64_t aFrom, const std::vector<float>& b, std::int64_t bFrom, std::int64_t n)
{
    if (aFrom < 0 || bFrom < 0 || (aFrom + n) * 2 > static_cast<std::int64_t>(a.size()) || (bFrom + n) * 2 > static_cast<std::int64_t>(b.size())) return false;
    for (std::int64_t i = 0; i < n * 2; ++i)
        if (a[static_cast<size_t>(aFrom * 2 + i)] != b[static_cast<size_t>(bFrom * 2 + i)]) return false;
    return true;
}

// ---- 実機の音のスレッド + スプラッシュのスレッドの模擬（60fps。音は 20ms 先まで描き置く。時計は「聞こえている時刻」）
struct Sim
{
    std::vector<float> out;
    std::int64_t tailFrame = -1;
    int lateCues = 0;
    double lockWallMs = std::nan("");
    bool handed = false;
    bool skipped = false;
    double skipWall = std::nan("");
    int cues = 0, cancelled = 0;
};

Sim Run(const std::shared_ptr<const sp::CalibrateBank>& bank, bool reduce, double finishMs, const std::vector<double>& skipsMs, double endMs)
{
    sp::CalibrateSequencer seq(bank, reduce);
    sp::GaugeFastDriver drv;
    Sim r;
    constexpr std::int64_t kAhead = 882;
    std::vector<float> tmp(441 * 2);
    auto fill = [&](std::int64_t target) {
        while (static_cast<std::int64_t>(r.out.size() / 2) < target)
        {
            seq.Render(tmp.data(), 441);
            r.out.insert(r.out.end(), tmp.begin(), tmp.end());
        }
    };
    seq.Begin();
    fill(kAhead);
    size_t nextSkip = 0;
    bool handSent = false, skipSent = false;
    for (int k = 0;; ++k)
    {
        const double t = k * 1000.0 / 60.0;
        if (t > endMs) break;
        const bool fin = !std::isnan(finishMs) && t >= finishMs;
        const float prog = std::isnan(finishMs) ? 0.2f : static_cast<float>(std::min(0.99, t / finishMs));
        if (!reduce) drv.Update(t, prog, fin);
        if (!reduce && drv.HandedOver() && !handSent) { seq.HandOver(t); handSent = true; r.handed = true; }
        while (nextSkip < skipsMs.size() && skipsMs[nextSkip] <= t)
        {
            ++nextSkip;
            // スキップ時刻は「聞こえている時刻」と「音が描き終えた時刻」の遅い方（描き置き済みの音は書き換えられない）。絵も同じ時刻でロックへ飛ぶので音と揃う
            const double tSkip = std::max(t, sp::seq::FrameToMs(static_cast<double>(seq.OutFrame())));
            drv.Skip(tSkip);
            if (drv.Skipped() && !skipSent) { skipSent = true; r.skipped = true; r.skipWall = drv.SkipWallMs(); seq.Skip(r.skipWall); }
        }
        for (std::uint32_t id : drv.TakeCancelledCueIds()) { seq.CancelCue(id); ++r.cancelled; }
        for (const sp::GaugeCue& c : drv.TakeCues()) { seq.AddCue(c); ++r.cues; }
        fill(sp::seq::MsToFrame(t) + kAhead);
    }
    while (!seq.Idle() && r.out.size() / 2 < 20u * 44100u) fill(static_cast<std::int64_t>(r.out.size() / 2) + 441);
    r.tailFrame = seq.TailStartFrame();
    r.lateCues = seq.LateCues();
    r.lockWallMs = drv.LockWallMs();
    return r;
}

float MaxStep(const std::vector<float>& v, std::int64_t from, std::int64_t to)
{
    return sp::MaxStepDiff(v, static_cast<size_t>(from), static_cast<size_t>(to));
}

// ----------------------------------------------------------------

void TestBankAndFast(const std::shared_ptr<const sp::CalibrateBank>& bank, const Files& fl)
{
    CHECK(bank && bank->Valid());
    if (!bank) return;
    // 速い経路: 1 ボイスで鳴らしたオフライン描画が graphite_1.wav とビット一致（float でも PCM16 のバイトでも）
    {
        sp::CalibrateSequencer s(bank, false);
        s.Begin();
        const std::vector<float> out = RenderAll(s, bank->FullFrames());
        CHECK(EqualRange(out, 0, bank->full, 0, bank->FullFrames()));
        std::vector<std::uint8_t> pcm;
        for (std::int64_t i = 0; i < bank->FullFrames() * 2; ++i)
        {
            const std::int16_t q = sp::seq::FloatToPcm16(out[static_cast<size_t>(i)]);
            pcm.push_back(static_cast<std::uint8_t>(q & 0xff));
            pcm.push_back(static_cast<std::uint8_t>((q >> 8) & 0xff));
        }
        CHECK(fl.full.size() == 44 + pcm.size());
        CHECK(std::equal(pcm.begin(), pcm.end(), fl.full.begin() + 44));
        RenderAll(s, 441);
        CHECK(s.Idle());                 // 鳴り終えたら空になる
    }
    // Reduce Motion: rm を 1 ボイスで
    {
        sp::CalibrateSequencer s(bank, true);
        s.Begin();
        const std::vector<float> out = RenderAll(s, bank->FullFrames());
        CHECK(EqualRange(out, 0, bank->rm, 0, bank->FullFrames()));
        CHECK(!EqualRange(out, 0, bank->full, 0, bank->FullFrames()));   // 風切りの層が無い別の版
    }
    // lock_tail は full の T_lock 以降とサンプル単位で一致
    CHECK(EqualRange(bank->full, sp::calibrate::kLockSample, bank->lockTail, 0, bank->FullFrames() - sp::calibrate::kLockSample));
    // 1 チャンクの大きさを変えても同じ（チャンク境界で継ぎ目が出ない）
    for (int chunk : { 1, 7, 128, 512, 1024 })
    {
        sp::CalibrateSequencer s(bank, false);
        s.Begin();
        const std::vector<float> out = RenderAll(s, 20000, chunk);
        CHECK(EqualRange(out, 0, bank->full, 0, 20000));
    }
}

// ステムからの再構築（ベッド + クリックのパン・Haas）が master に近い（ロック前、ベッドがループに入る手前まで）
void TestStemReconstruction(const std::shared_ptr<const sp::CalibrateBank>& bank)
{
    sp::CalibrateSequencer s(bank, false);
    s.BeginSlow();
    for (int i = 0; i < 24; ++i)
    {
        sp::GaugeCue c;
        c.kind = sp::GaugeCueKind::Click;
        c.index = i;
        c.playAtMs = sp::calibrate::kClickMs[i];
        c.pan = static_cast<float>(sp::DefaultCalibrateEvents().clickPan[i]);
        c.id = static_cast<std::uint32_t>(i + 1);
        s.AddCue(c);
    }
    const std::int64_t n = sp::seq::MsToFrame(1500.0);
    const std::vector<float> out = RenderAll(s, n);
    double se = 0, sr = 0;
    for (std::int64_t i = 0; i < n * 2; ++i)
    {
        const double d = static_cast<double>(out[static_cast<size_t>(i)]) - static_cast<double>(bank->full[static_cast<size_t>(i)]);
        se += d * d;
        sr += static_cast<double>(bank->full[static_cast<size_t>(i)]) * bank->full[static_cast<size_t>(i)];
    }
    const double rmsDb = 10.0 * std::log10(se / static_cast<double>(n * 2) + 1e-30);
    std::printf("stem reconstruction residual RMS %.3f dBFS (signal %.1f)\n", rmsDb, 10.0 * std::log10(sr / static_cast<double>(n * 2)));
    CHECK_MSG(rmsDb <= -59.5, "residual %.3f dBFS", rmsDb);
}

void TestSlowScenarios(const std::shared_ptr<const sp::CalibrateBank>& bank)
{
    const float srcMax = MaxStep(bank->full, 0, bank->FullFrames());
    // 遅い経路の 6 シナリオ: 完了要求の時刻（持ち替え 1,475 より後）
    const double finishes[] = { 1500, 1800, 2300, 3200, 5000, 9000 };
    for (double fin : finishes)
    {
        const Sim r = Run(bank, false, fin, {}, fin + 3000);
        CHECK(r.handed);
        CHECK_MSG(r.tailFrame > 0, "fin=%.0f no tail", fin);
        CHECK(r.lateCues == 0);
        // ロックの音の位置: ドライバが決めた T_lock' にサンプル単位
        CHECK_MSG(r.tailFrame == sp::seq::MsToFrame(r.lockWallMs), "fin=%.0f tail %lld vs lock %.3f", fin, static_cast<long long>(r.tailFrame), r.lockWallMs);
        // ロック以降の出力が lock_tail とサンプル単位で一致（最後まで）
        const std::int64_t tl = static_cast<std::int64_t>(bank->lockTail.size() / 2);
        CHECK_MSG(EqualRange(r.out, r.tailFrame, bank->lockTail, 0, std::min<std::int64_t>(tl, static_cast<std::int64_t>(r.out.size() / 2) - r.tailFrame)),
                  "fin=%.0f tail mismatch", fin);
        CHECK(static_cast<std::int64_t>(r.out.size() / 2) - r.tailFrame >= tl);
        // 継ぎ目が無い: 全体の最大段差が master の最大段差の 1.6 倍以内
        const float m = MaxStep(r.out, 0, static_cast<std::int64_t>(r.out.size() / 2));
        std::printf("slow fin=%.0f  lock@%.2fms  maxStep %.5f (master %.5f)\n", fin, r.lockWallMs, m, srcMax);
        CHECK_MSG(m <= srcMax * 1.6f, "fin=%.0f maxStep %.5f > %.5f", fin, m, srcMax * 1.6f);
        // 持ち替え前は master そのまま
        CHECK(EqualRange(r.out, 0, bank->full, 0, sp::seq::MsToFrame(1475.0)));
    }
    // 完了が持ち替えの前なら速い経路のまま、全体が master と一致
    {
        const Sim r = Run(bank, false, 1200, {}, 5000);
        CHECK(!r.handed);
        CHECK(EqualRange(r.out, 0, bank->full, 0, bank->FullFrames()));
    }
}

void TestSkip(const std::shared_ptr<const sp::CalibrateBank>& bank)
{
    const float srcMax = MaxStep(bank->full, 0, bank->FullFrames());
    const std::int64_t tl = static_cast<std::int64_t>(bank->lockTail.size() / 2);
    // 6 時刻（持ち替え前: 300/800/1300、境: 1480、持ち替え後でロードが続いている: 1700/2500）
    const double skips[] = { 300, 800, 1300, 1480, 1700, 2500 };
    for (double sk : skips)
    {
        const Sim r = Run(bank, false, std::nan(""), { sk }, sk + 4500);
        CHECK_MSG(r.skipped, "skip %.0f not effective", sk);
        const std::int64_t f = sp::seq::MsToFrame(r.skipWall);
        CHECK_MSG(r.tailFrame == f, "skip %.0f tail %lld vs %lld", sk, static_cast<long long>(r.tailFrame), static_cast<long long>(f));
        const std::int64_t fade = sp::seq::MsToFrame(sp::seq::kSkipFadeMs);
        // フェード（30ms）が終わったあとは lock_tail とサンプル単位で一致
        CHECK_MSG(EqualRange(r.out, f + fade, bank->lockTail, fade, tl - fade), "skip %.0f tail mismatch", sk);
        // スキップ前は master（持ち替え前）のまま
        if (sk < 1400) CHECK(EqualRange(r.out, 0, bank->full, 0, f));
        // クリックノイズ無し: 全体で最大段差が master の 1.6 倍以内。フェードの区間でも同じ
        const float m = MaxStep(r.out, 0, static_cast<std::int64_t>(r.out.size() / 2));
        const float mf = MaxStep(r.out, f - 2, f + fade + 2);
        std::printf("skip %.0f  tail@%lld  maxStep %.5f fadeRegion %.5f (master %.5f)\n", sk, static_cast<long long>(f), m, mf, srcMax);
        CHECK_MSG(m <= srcMax * 1.6f, "skip %.0f maxStep %.5f", sk, m);
    }
    // 2 回目のスキップは音に何もしない
    {
        const Sim r = Run(bank, false, std::nan(""), { 800, 900 }, 6000);
        const std::int64_t f = sp::seq::MsToFrame(r.skipWall);
        CHECK(r.tailFrame == f);
        CHECK(EqualRange(r.out, f + 1323, bank->lockTail, 1323, tl - 1323));
    }
    // ロック以降のスキップは何もしない（ロック音を重ねない）
    {
        const Sim r = Run(bank, false, 1200, { 1700 }, 5000);
        CHECK(!r.skipped);
        CHECK(EqualRange(r.out, 0, bank->full, 0, bank->FullFrames()));
    }
    // Reduce Motion のスキップ（音だけ）
    {
        sp::CalibrateSequencer s(bank, true);
        s.Begin();
        std::vector<float> out = RenderAll(s, 441 * 40);
        const std::int64_t f = static_cast<std::int64_t>(out.size() / 2);
        s.Skip(sp::seq::FrameToMs(static_cast<double>(f)));
        std::vector<float> more = RenderAll(s, tl + 100);
        out.insert(out.end(), more.begin(), more.end());
        CHECK(EqualRange(out, f + 1323, bank->lockTail, 1323, tl - 1323));
    }
}

void TestCancelAndSteal(const std::shared_ptr<const sp::CalibrateBank>& bank)
{
    sp::CalibrateSequencer s(bank, false);
    s.BeginSlow();
    sp::GaugeCue c;
    c.kind = sp::GaugeCueKind::Click; c.index = 3; c.playAtMs = 500; c.pan = 0.f; c.id = 7;
    s.AddCue(c);
    CHECK(s.CountKind(sp::VoiceKind::Click) == 1);
    s.CancelCue(7);
    CHECK(s.CountKind(sp::VoiceKind::Click) == 0);
    // 同時 8 本を超えると、いちばん古いクリックを 2ms で落とす
    for (int i = 0; i < 12; ++i)
    {
        c.index = i; c.id = static_cast<std::uint32_t>(100 + i); c.playAtMs = 600.0 + i;
        s.AddCue(c);
    }
    CHECK(s.StolenVoices() >= 4);
    const std::vector<float> out = RenderAll(s, sp::seq::MsToFrame(900));
    CHECK(MaxStep(out, 0, static_cast<std::int64_t>(out.size() / 2)) < 0.5f);
}

void TestClockMap()
{
    // モックのデバイス 2 種。実際のストリームの開始は 123.4567ms（QPC）。
    //  (a) 位置は 10ms 単位に切り捨て、QPC はその位置を測った時刻（整合した組。IAudioClock::GetPosition の想定）。QPC にジッタ +-0.3ms
    //  (b) 位置は 10ms 単位に切り捨て、QPC は「報告した時刻」（組が整合しない側）。報告周期が 9.37ms で量子化の位相が回る
    const double start = 123.4567;
    unsigned seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (static_cast<double>(seed >> 8) / 16777216.0) * 2.0 - 1.0; };
    for (int model = 0; model < 2; ++model)
    {
        sp::AudioClockMap m;
        double worst = 0;
        for (int k = 0; k < 600; ++k)
        {
            const double period = model == 0 ? 10.0 : 9.37;
            const double reportAt = 140.0 + k * period;
            const double trueMs = reportAt - start;
            const double quant = std::floor(trueMs / 10.0) * 10.0;
            const std::int64_t frames = static_cast<std::int64_t>(quant * 44.1);
            const double stamp = model == 0 ? start + quant + rnd() * 0.3 : reportAt;
            m.Report(frames, stamp);
            if (!m.Valid() || k < 100) continue;                     // 窓が埋まるまでは対象外
            for (int j = 0; j < 10; ++j)                              // 報告と報告の間の任意の時刻で換算
            {
                const double now = reportAt + j;
                worst = std::max(worst, std::fabs(m.TimelineMsAt(now) - (now - start)));
            }
        }
        std::printf("clock map model %c worst error %.3f ms\n", 'a' + model, worst);
        CHECK_MSG(worst <= 0.5, "model %d worst %.3f", model, worst);
    }
    sp::AudioClockMap m2;
    m2.Report(0, 100.0);
    CHECK(!m2.Valid());                                             // frames == 0 は無視（まだ動いていない）
    for (double ms : { 268.0045, 1628.0045, 2148.0, 3749.99 })
    {
        const double back = sp::seq::FrameToMs(static_cast<double>(sp::seq::MsToFrame(ms)));
        CHECK(std::fabs(back - ms) <= 0.5 * 1000.0 / 44100.0 + 1e-9);
    }
}

void TestTimelineClock()
{
    {
        sp::TimelineClock c;
        c.Begin(1000.0, false);
        CHECK(std::fabs(c.Now(1000.0, -1.0)) < 1e-9);
        CHECK(std::fabs(c.Now(1500.0, -1.0) - 500.0) < 1e-9);
    }
    {
        sp::TimelineClock c;
        c.Begin(1000.0, true);
        CHECK(c.Now(1100.0, -1.0) == 0.0);
        CHECK(std::fabs(c.Now(1200.0, 90.0) - 90.0) < 1e-9);
        CHECK(std::fabs(c.Now(1300.0, 190.0) - 190.0) < 1e-9);
        CHECK(c.UsingAudio());
        CHECK(c.Now(1400.0, 150.0) >= 190.0);              // 単調
    }
    {
        sp::TimelineClock c;                                // 開けなかった: 猶予のあとに QPC で完走
        c.Begin(0.0, true);
        double t = 0;
        for (int k = 1; k <= 60; ++k) t = c.Now(k * 16.7, -1.0);
        CHECK(!c.UsingAudio());
        CHECK(t > 250.0);   // 猶予（700ms）の間は 0 で待ち、そこから QPC で進む
    }
    {
        sp::TimelineClock c;                                // 止まった: 飛ばない・戻らない
        c.Begin(0.0, true);
        double last = 0;
        for (int k = 1; k <= 120; ++k)
        {
            const double wall = k * 16.7;
            const double audio = wall < 1000.0 ? wall - 20.0 : 980.0;
            const double t = c.Now(wall, audio);
            CHECK(t >= last);
            CHECK(t - last < 40.0);
            last = t;
        }
        CHECK(!c.UsingAudio());
        CHECK(last > 1500.0);
    }
}

void TestGate()
{
    int opens = 0;
    auto open = [&]() { ++opens; return true; };
    sp::SoundGate g;
    CHECK(sp::OpenIfAllowed(g, open));
    CHECK(opens == 1);
    sp::SoundGate cases[5];
    cases[0].settingOn = false;
    cases[1].argOn = false;
    cases[2].envOff = true;
    cases[3].headless = true;
    cases[4].remoteSession = true;
    for (const sp::SoundGate& c : cases) CHECK(!sp::OpenIfAllowed(c, open));
    CHECK(opens == 1);                                              // 鳴らさない条件ではデバイスを開かない
}

void TestPanLaw()
{
    const sp::seq::PanLaw c = sp::seq::MakePanLaw(0.0);
    CHECK(std::fabs(c.gainL - 0.70710678f) < 1e-6f && std::fabs(c.gainR - 0.70710678f) < 1e-6f);
    CHECK(c.delayL == 0 && c.delayR == 0);
    const sp::seq::PanLaw r = sp::seq::MakePanLaw(0.6);
    CHECK(r.gainR > r.gainL);
    CHECK(r.delayL > 0 && r.delayR == 0);             // pan > 0 は L を遅らせる
    CHECK(r.delayL == static_cast<std::int64_t>(std::llround(0.6 * 0.35e-3 * 44100.0)));
    const sp::seq::PanLaw l = sp::seq::MakePanLaw(-1.0);
    CHECK(l.delayR > 0 && l.delayL == 0);
    for (double p : { -1.0, -0.4, 0.0, 0.3, 1.0 })
    {
        const sp::seq::PanLaw x = sp::seq::MakePanLaw(p);
        CHECK(std::fabs(x.gainL * x.gainL + x.gainR * x.gainR - 1.0f) < 1e-5f);
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: SplashSequencerTests <repo-root>\n"); return 2; }
    g_root = argv[1];
    Files fl;
    auto bank = LoadBank(fl);
    CHECK(bank != nullptr);
    if (bank)
    {
        TestBankAndFast(bank, fl);
        TestStemReconstruction(bank);
        TestSlowScenarios(bank);
        TestSkip(bank);
        TestCancelAndSteal(bank);
    }
    TestClockMap();
    TestTimelineClock();
    TestGate();
    TestPanLaw();
    std::printf(g_fail ? "SplashSequencerTests: %d FAILED\n" : "SplashSequencerTests: ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
