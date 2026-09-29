// 起動音ソフトウェアミキサー（core/SplashMixer.h）の単体テスト。音デバイスは一切使わない。
// 合成した音源（滑らかな連続信号）を「riser / hit」に分けて、早着・定刻・遅着それぞれの出力を
// 数値で検査する:  頂点(onset)の位置合わせ / クロスフェード / ループ保持 / クリック無し（区間端の連続性）。
#include "core/SplashMixer.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double aa_ = (a), bb_ = (b); if (!(std::fabs(aa_ - bb_) <= (eps))) { \
    std::printf("FAIL %s:%d  %s=%.7f vs %s=%.7f\n", __FILE__, __LINE__, #a, aa_, #b, bb_); ++g_fail; } } while (0)

namespace sp = dx12e::splash;

constexpr double kPiT = 3.14159265358979323846;
constexpr int kSr = 44100;
constexpr int64_t kO = 2 * kSr;            // onset = 2.0s
constexpr int64_t kN = 3 * kSr + 20000;    // 全長

// 滑らかな連続信号（いくつかの正弦の和）。riser も hit も「同じ 1 本の信号の別区間」なので、
// 定刻なら出力は源と完全に一致するはず。クリックは源の最大差分を超える段差として現れる。
std::shared_ptr<sp::SoundAsset> MakeAsset(int64_t onset = kO, int64_t frames = kN)
{
    auto a = std::make_shared<sp::SoundAsset>();
    a->sampleRate = kSr;
    a->onsetFrame = onset;
    a->popOffsetFrames = 4000;
    a->src.resize(static_cast<size_t>(frames) * 2);
    for (int64_t i = 0; i < frames; ++i)
    {
        const double t = static_cast<double>(i) / kSr;
        const double env = (i < onset) ? 0.15 + 0.25 * (static_cast<double>(i) / onset) : 0.5 * std::exp(-(t - static_cast<double>(onset) / kSr) * 1.2);
        const double l = env * (std::sin(2 * kPiT * 440.0 * t) + 0.6 * std::sin(2 * kPiT * 1234.5 * t + 0.4));
        const double r = env * (std::sin(2 * kPiT * 523.25 * t + 0.9) + 0.6 * std::sin(2 * kPiT * 987.7 * t));
        a->src[static_cast<size_t>(i) * 2]     = static_cast<float>(l * 0.6);
        a->src[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(r * 0.6);
    }
    return a;
}

// 出力を n フレーム分まとめて作る。triggerFrame >= 0 ならそのフレームで hit へ。
std::vector<float> Run(const std::shared_ptr<sp::SoundAsset>& a, double predictedSec, int64_t triggerFrame,
                       int64_t frames, size_t chunk = 441, sp::MixerConfig cfg = sp::MixerConfig())
{
    sp::Mixer m(a, cfg);
    m.Start(predictedSec);
    if (triggerFrame >= 0) m.RequestHitAtFrame(triggerFrame);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    for (int64_t f = 0; f < frames; f += static_cast<int64_t>(chunk))
    {
        const size_t n = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(chunk), frames - f));
        m.Render(out.data() + static_cast<size_t>(f) * 2, n);
    }
    return out;
}

float SrcAt(const sp::SoundAsset& a, int64_t f, int c) { return a.src[static_cast<size_t>(f) * 2 + static_cast<size_t>(c)]; }

// ------------------------------------------------------------ WAV パース

std::vector<uint8_t> MakeWav(int ch, int rate, const std::vector<int16_t>& pcm, bool extraChunk)
{
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); };
    auto tag = [&](const char* t) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(t[i])); };
    tag("RIFF"); u32(0); tag("WAVE");
    tag("fmt "); u32(16); u16(1); u16(static_cast<uint16_t>(ch)); u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * ch * 2)); u16(static_cast<uint16_t>(ch * 2)); u16(16);
    if (extraChunk) { tag("LIST"); u32(5); for (int i = 0; i < 5; ++i) b.push_back(0x41); b.push_back(0); }   // 奇数長 + パッド
    tag("data"); u32(static_cast<uint32_t>(pcm.size() * 2));
    for (int16_t s : pcm) u16(static_cast<uint16_t>(s));
    return b;
}

void Test_WavParse()
{
    sp::WavData w;
    auto st = MakeWav(2, 44100, { 100, -200, 32767, -32768 }, false);
    CHECK(sp::ParseWavPcm16(st.data(), st.size(), w));
    CHECK(w.sampleRate == 44100 && w.channels == 2 && w.frameCount() == 2);
    CHECK_NEAR(w.frames[0], 100.0 / 32768.0, 1e-7);
    CHECK_NEAR(w.frames[1], -200.0 / 32768.0, 1e-7);
    CHECK_NEAR(w.frames[3], -1.0, 1e-7);
    auto mo = MakeWav(1, 44100, { 1000, -1000, 5 }, true);   // モノ + 余分なチャンク(奇数長)
    CHECK(sp::ParseWavPcm16(mo.data(), mo.size(), w));
    CHECK(w.channels == 1 && w.frameCount() == 3);
    CHECK(w.frames[0] == w.frames[1]);                       // L=R に複製
    CHECK_NEAR(w.frames[2], -1000.0 / 32768.0, 1e-7);
    // 壊れた/非対応
    std::vector<uint8_t> junk(64, 0x11);
    CHECK(!sp::ParseWavPcm16(junk.data(), junk.size(), w));
    CHECK(!sp::ParseWavPcm16(st.data(), 20, w));              // 途中で切れている
    auto f32 = st; f32[20] = 3;                               // フォーマットタグ 3 = float → 非対応
    CHECK(!sp::ParseWavPcm16(f32.data(), f32.size(), w));
}

// ------------------------------------------------------------ 定刻

void Test_OnTime_ExactlyMatchesSource()
{
    auto a = MakeAsset();
    sp::Mixer probe(a);
    const int64_t H = probe.HoldEnd();
    CHECK(probe.Xfade() == static_cast<int64_t>(std::llround(0.12 * kSr)));
    CHECK(H == kO - probe.Xfade());

    // riser を先頭から再生（予測 = H/sr）し、H に届いた瞬間（出力フレーム H）に hit → 出力は源と一致
    const auto out = Run(a, static_cast<double>(H) / kSr, H, kN);
    double md = 0.0;
    for (int64_t i = 0; i < kN; ++i)
        for (int c = 0; c < 2; ++c) md = std::max(md, static_cast<double>(std::fabs(out[static_cast<size_t>(i) * 2 + static_cast<size_t>(c)] - SrcAt(*a, i, c))));
    CHECK(md < 1e-6);   // 継ぎ目なし: クロスフェード区間も含めて源そのもの

    sp::Mixer m(a);
    m.Start(static_cast<double>(H) / kSr);
    m.RequestHitAtFrame(H);
    std::vector<float> tmp(2 * 512);
    for (int64_t f = 0; f < kN + 4000; f += 512) m.Render(tmp.data(), 512);
    CHECK(m.HitOnsetOutputFrame() == kO);                      // 頂点は源と同じ位置
    CHECK(m.PopOutputFrame() == kO + 4000);
    CHECK(m.Finished());
}

void Test_OnTime_MidStart()
{
    // 予測 1.0s → riser は H - 1.0s から始まり（先頭フェードイン）、1.0s 後に H へ届く
    auto a = MakeAsset();
    sp::Mixer probe(a);
    const int64_t H = probe.HoldEnd();
    const int64_t trig = kSr;
    const auto out = Run(a, 1.0, trig, kN);
    const int64_t p0 = H - kSr;
    CHECK(p0 > 0);
    // 先頭は 0 から立ち上がる（フェードイン）
    CHECK(std::fabs(out[0]) < 1e-6f && std::fabs(out[1]) < 1e-6f);
    // フェード後は riser が源の p0 + n
    const int64_t k = static_cast<int64_t>(std::llround(0.05 * kSr));
    CHECK_NEAR(out[static_cast<size_t>(k) * 2], SrcAt(*a, p0 + k, 0), 1e-6);
    // 定刻: trig 以降は源の H から続く
    for (int64_t j = 0; j < kN - H; j += 97)
        CHECK_NEAR(out[static_cast<size_t>(trig + j) * 2], SrcAt(*a, H + j, 0), 1e-6);
}

// ------------------------------------------------------------ 早着

void Test_Early_Crossfade()
{
    auto a = MakeAsset();
    sp::Mixer probe(a);
    const int64_t H = probe.HoldEnd(), X = probe.Xfade();
    const int64_t p0 = H - static_cast<int64_t>(std::llround(1.5 * kSr));
    const int64_t trig = 20000;   // 予測（1.5s）より早く終わった
    const auto out = Run(a, 1.5, trig, kN);

    sp::Mixer m(a);
    m.Start(1.5); m.RequestHitAtFrame(trig);
    std::vector<float> tmp(2 * 441);
    for (int64_t f = 0; f < 60000; f += 441) m.Render(tmp.data(), 441);
    CHECK(m.HitOnsetOutputFrame() == trig + X);               // 頂点は trigger + クロスフェード長

    // クロスフェードの途中（中点）: 0.5 * riser + 0.5 * hit
    const int64_t mid = trig + X / 2;
    for (int c = 0; c < 2; ++c)
    {
        const double expect = 0.5 * SrcAt(*a, p0 + mid, c) + 0.5 * SrcAt(*a, H + X / 2, c);
        CHECK_NEAR(out[static_cast<size_t>(mid) * 2 + static_cast<size_t>(c)], expect, 2e-5);
    }
    // クロスフェードの後は、源の onset 以降と完全一致（頂点が揃う）
    for (int64_t j = 0; j < kN - kO; j += 89)
        for (int c = 0; c < 2; ++c)
            CHECK_NEAR(out[static_cast<size_t>(trig + X + j) * 2 + static_cast<size_t>(c)], SrcAt(*a, kO + j, c), 1e-6);
    // クロスフェード長が 80〜150ms の範囲
    CHECK(static_cast<double>(X) / kSr >= 0.08 && static_cast<double>(X) / kSr <= 0.15);
}

// ------------------------------------------------------------ 遅着

void Test_Late_HoldLoop()
{
    auto a = MakeAsset();
    sp::Mixer probe(a);
    const int64_t H = probe.HoldEnd(), X = probe.Xfade(), Lh = probe.HoldLoop(), S = probe.Seam();
    CHECK(static_cast<double>(Lh) / kSr == 0.30);
    CHECK(S > 0 && S < Lh);
    const double pred = 1.0;
    const int64_t wrapFrame = static_cast<int64_t>(std::llround(pred * kSr));    // riser が H に届く出力フレーム
    const int64_t trig = wrapFrame + 3 * Lh + 12345;                               // 3 周以上ループしてから終わった
    const int64_t total = trig + X + 20000;
    sp::MixerConfig noDuck; noDuck.holdDuckSec = 0.0;      // 周期性そのものを見るので、保持の絞りは切る（絞りは別のテスト）
    const auto out = Run(a, pred, trig, total, 441, noDuck);

    sp::Mixer m(a, noDuck);
    m.Start(pred); m.RequestHitAtFrame(trig);
    std::vector<float> tmp(2 * 441);
    // 状態: 保持中は Hold、trigger 後は Crossfade → Hit
    for (int64_t f = 0; f < wrapFrame + Lh; f += 441) m.Render(tmp.data(), 441);
    CHECK(m.State() == sp::MixState::Hold);
    for (int64_t f = m.OutputFrame(); f < trig + X / 2; f += 441) m.Render(tmp.data(), 441);
    CHECK(m.State() == sp::MixState::Crossfade);

    // ループは周期 Lh（seam 区間の後は完全に周期的）
    for (int64_t n = wrapFrame + S + 5; n + Lh < trig - 10; n += 53)
        CHECK_NEAR(out[static_cast<size_t>(n) * 2], out[static_cast<size_t>(n + Lh) * 2], 1e-6);
    // ループ位置は源の [H-Lh, H) の中（内容が源と一致）
    CHECK_NEAR(out[static_cast<size_t>(wrapFrame + S + 100) * 2], SrcAt(*a, H - Lh + S + 100, 0), 1e-6);
    // 遅着でも頂点は trigger + X、その後は源の onset 以降と一致
    for (int64_t j = 0; j < 15000; j += 71)
        CHECK_NEAR(out[static_cast<size_t>(trig + X + j) * 2 + 1], SrcAt(*a, kO + j, 1), 1e-6);

    // ---- クリック無し: 全区間の隣接サンプル差が、源の最大差分 × 係数を超えない（段差があれば源の 10 倍以上になる）
    const float srcMax = sp::MaxStepDiff(a->src, 0, static_cast<size_t>(kN));
    const float outMax = sp::MaxStepDiff(out, 0, static_cast<size_t>(total));
    CHECK(outMax <= srcMax * 1.6f);
    // 区間端の近傍を個別に: wrap / seam 終わり / trigger / crossfade 終わり / 開始
    for (int64_t b : { wrapFrame, wrapFrame + S, wrapFrame + Lh, wrapFrame + 2 * Lh, wrapFrame + 2 * Lh + S, trig, trig + X })
        CHECK(sp::MaxStepDiff(out, static_cast<size_t>(b - 8), static_cast<size_t>(b + 8)) <= srcMax * 1.6f);
}

// 保持が長引く（更新確認が遅い等）と、同じ 300ms の繰り返しを絞っていく。クリックは出さず、hit は絞った音から全音量で立ち上がる。
void Test_Late_HoldDucks()
{
    auto a = MakeAsset();
    sp::Mixer probe(a);
    const int64_t X = probe.Xfade();
    const double pred = 1.0;
    const int64_t wrapFrame = static_cast<int64_t>(std::llround(pred * kSr));
    const int64_t trig = wrapFrame + 8 * kSr;              // 8 秒も待たされた
    const int64_t total = trig + X + 20000;
    const auto out = Run(a, pred, trig, total);            // 既定の設定 = 3 秒かけて 0.45 まで絞る
    auto rms = [&](int64_t from, int64_t len) {
        double e = 0.0;
        for (int64_t i = from; i < from + len; ++i) { const double v = out[static_cast<size_t>(i) * 2]; e += v * v; }
        return std::sqrt(e / static_cast<double>(len));
    };
    const double early = rms(wrapFrame + 2000, 13230);                 // 保持の直後
    const double late  = rms(trig - 13230 - 500, 13230);               // 8 秒後（絞り切った後）
    CHECK(late < early * 0.62);                                        // 音量が下がっている（0.45 倍に近い）
    CHECK(late > early * 0.30);                                        // 消えてはいない
    // 絞りの下限（0.45）を下回らない: 保持区間の RMS は、絞る前の 0.45 倍より小さくならない
    // hit は絞った音から全音量で立ち上がる（頂点以降は源と一致）
    for (int64_t j = 0; j < 12000; j += 131)
        CHECK_NEAR(out[static_cast<size_t>(trig + X + j) * 2], SrcAt(*a, kO + j, 0), 1e-6);
    // クリック無し
    const float srcMax = sp::MaxStepDiff(a->src, 0, static_cast<size_t>(kN));
    CHECK(sp::MaxStepDiff(out, 0, static_cast<size_t>(total)) <= srcMax * 1.6f);
}

void Test_Late_WithoutTrigger_HoldsForever()
{
    auto a = MakeAsset();
    const auto out = Run(a, 1.0, -1, 20 * kSr);   // 一度も TriggerHit が来ない（初期化が終わらない）
    // 20 秒たっても鳴り続け（無音にならない）、クリックも無い
    double rms = 0.0;
    for (size_t i = out.size() - 20000; i < out.size(); ++i) rms += static_cast<double>(out[i]) * out[i];
    CHECK(rms > 1.0);
    const float srcMax = sp::MaxStepDiff(a->src, 0, static_cast<size_t>(kN));
    CHECK(sp::MaxStepDiff(out, 0, out.size() / 2) <= srcMax * 1.6f);
}

// ------------------------------------------------------------ 開始 / 中止 / 状態

void Test_Start_SilentDelay()
{
    // 予測が riser より長い（T > H/sr）: 先頭に無音を置き、H に届くのがちょうど T 後になる
    auto a = MakeAsset();
    sp::Mixer probe(a);
    const int64_t H = probe.HoldEnd();
    const double T = static_cast<double>(H) / kSr + 0.5;
    const int64_t delay = static_cast<int64_t>(std::llround(0.5 * kSr));
    const auto out = Run(a, T, static_cast<int64_t>(std::llround(T * kSr)), kN + delay);
    for (int64_t i = 0; i < delay; ++i) CHECK(out[static_cast<size_t>(i) * 2] == 0.0f);
    // riser は無音の後に先頭から始まり、定刻で H に届いて hit へ
    CHECK_NEAR(out[static_cast<size_t>(delay + 5000) * 2], SrcAt(*a, 5000, 0), 1e-6);
    const int64_t trig = static_cast<int64_t>(std::llround(T * kSr));
    CHECK_NEAR(out[static_cast<size_t>(trig + 20000) * 2], SrcAt(*a, H + 20000, 0), 1e-6);
}

void Test_Abort_FadesOut()
{
    auto a = MakeAsset();
    sp::Mixer m(a);
    m.Start(1.0);
    std::vector<float> buf(2 * 441);
    for (int i = 0; i < 20; ++i) m.Render(buf.data(), 441);
    m.Abort();
    std::vector<float> after(2 * 4410, 1.0f);
    m.Render(after.data(), 4410);
    CHECK(m.Finished());
    // フェード終了後は無音。フェード中は段差なし
    CHECK(after[after.size() - 2] == 0.0f);
    const float srcMax = sp::MaxStepDiff(a->src, 0, static_cast<size_t>(kN));
    CHECK(sp::MaxStepDiff(after, 0, 4410) <= srcMax * 1.6f);
    // 停止後の Render は 0 を書く
    std::vector<float> z(2 * 64, 1.0f);
    m.Render(z.data(), 64);
    for (float v : z) CHECK(v == 0.0f);
}

void Test_StateSequence()
{
    auto a = MakeAsset();
    sp::Mixer m(a);
    CHECK(m.State() == sp::MixState::Idle);
    CHECK(m.Finished());
    m.Start(1.0);
    CHECK(m.State() == sp::MixState::Riser);
    std::vector<float> buf(2 * 441);
    bool seen[8] = {};
    m.RequestHitAtFrame(kSr + 9000);
    for (int i = 0; i < 400 && !m.Finished(); ++i) { m.Render(buf.data(), 441); seen[static_cast<int>(m.State())] = true; }
    CHECK(seen[static_cast<int>(sp::MixState::Riser)]);
    CHECK(seen[static_cast<int>(sp::MixState::Hold)]);
    CHECK(seen[static_cast<int>(sp::MixState::Crossfade)]);
    CHECK(seen[static_cast<int>(sp::MixState::Hit)]);
    CHECK(seen[static_cast<int>(sp::MixState::Stopped)]);
    CHECK(m.Finished());
}

void Test_NoRiserAsset()
{
    // onset = 0（riser 無し。埋め込みの案 A のような単発音）: hit は trigger から源の先頭が流れる
    auto a = MakeAsset(0, 30000);
    sp::Mixer m(a);
    m.Start(2.0);
    std::vector<float> out(2 * 40000, 0.0f);
    m.RequestHitAtFrame(10000);
    m.Render(out.data(), 40000);
    for (size_t i = 0; i < 10000; ++i) CHECK(out[i * 2] == 0.0f);   // trigger まで無音
    for (int64_t j = 0; j < 30000; j += 211) CHECK_NEAR(out[static_cast<size_t>(10000 + j) * 2], SrcAt(*a, j, 0), 1e-6);
    CHECK(m.Finished());
    CHECK(m.HitOnsetOutputFrame() == 10000);
}

void Test_ChunkSizeIndependent()
{
    // デバイスのチャンク長（441 / 1024 / 1）に依らず、サンプル精度で同じ出力になる
    auto a = MakeAsset();
    const auto o1 = Run(a, 1.0, 60000, 90000, 441);
    const auto o2 = Run(a, 1.0, 60000, 90000, 1024);
    const auto o3 = Run(a, 1.0, 60000, 90000, 77);
    CHECK(o1 == o2);
    CHECK(o1 == o3);
}

void Test_RequestHit_NextSample()
{
    // RequestHit() は「次に Render するサンプル」から効く（デバイススレッド側の即時発火）
    auto a = MakeAsset();
    sp::Mixer m(a);
    m.Start(1.0);
    std::vector<float> buf(2 * 441);
    m.Render(buf.data(), 441);
    m.RequestHit();
    m.Render(buf.data(), 441);
    CHECK(m.HitOnsetOutputFrame() == 441 + m.Xfade());
}
} // namespace

int main()
{
    Test_WavParse();
    Test_OnTime_ExactlyMatchesSource();
    Test_OnTime_MidStart();
    Test_Early_Crossfade();
    Test_Late_HoldLoop();
    Test_Late_HoldDucks();
    Test_Late_WithoutTrigger_HoldsForever();
    Test_Start_SilentDelay();
    Test_Abort_FadesOut();
    Test_StateSequence();
    Test_NoRiserAsset();
    Test_ChunkSizeIndependent();
    Test_RequestHit_NextSample();
    if (g_fail == 0) std::printf("splash mixer tests: OK\n");
    else std::printf("splash mixer tests: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
