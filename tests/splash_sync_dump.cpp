// 起動音ミキサーのオフライン書き出し（音デバイスは開かない）。
//   SplashSyncDump <出力ディレクトリ> [splash_d.wav のパス]
//   SplashSyncDump --calibrate <repo-root> <出力ディレクトリ>   … Graphite Gauge の起動音 Calibrate（W1-BD5）。下を参照
// 頂点の予測を T=3.0s として、実際の初期化が「早い(1.6s) / 定刻(3.0s) / 遅い(5.0s)」の 3 シナリオを
// ミキサーで計算し、sync_early.wav / sync_ontime.wav / sync_late.wav と、区間境界の一覧 sync_*.json を書く。
// つなぎ目のクリック検査は tools/check_splash_sync.py（WAV を独立に読んで境界の不連続量を測る）。
// 素材: 引数の WAV（+同名の .json メタ）があればそれ。無ければ埋め込みの案 A（riser 無しの単発音）。
#include "core/SplashMixer.h"
#include "core/SplashSequencer.h"
#include "core/SplashSoundData.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace sp = dx12e::splash;

namespace
{
bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

std::shared_ptr<sp::SoundAsset> LoadAsset(const std::string& wavPath, std::string& what)
{
    auto a = std::make_shared<sp::SoundAsset>();
    std::vector<uint8_t> bytes, meta;
    if (!wavPath.empty() && ReadFile(wavPath, bytes) && ReadFile(wavPath.substr(0, wavPath.rfind('.')) + ".json", meta))
    {
        sp::WavData w;
        int64_t onset = 0, pop = 0;
        if (sp::ParseWavPcm16(bytes.data(), bytes.size(), w) && w.sampleRate == 44100
            && sp::ParseJsonInt(reinterpret_cast<const char*>(meta.data()), meta.size(), "onsetFrame", onset))
        {
            sp::ParseJsonInt(reinterpret_cast<const char*>(meta.data()), meta.size(), "popOffsetFrames", pop);
            if (sp::MakeAssetFromWav(std::move(w), onset, pop, *a)) { what = "splash_d.wav"; return a; }
        }
    }
    sp::WavData w;
    if (!sp::ParseWavPcm16(kSplashSoundWav, sizeof(kSplashSoundWav), w)) return nullptr;
    a->sampleRate = w.sampleRate;
    a->src = std::move(w.frames);
    a->onsetFrame = 0;
    a->popOffsetFrames = 441;
    what = "embedded (案A)";
    return a;
}

void WriteWav16(const std::string& path, const std::vector<float>& st, int sr)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(st.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(static_cast<uint32_t>(sr)); u32(static_cast<uint32_t>(sr * 4)); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : st)
    {
        long q = std::lround(v * 32767.0f);
        if (q > 32767) q = 32767;
        if (q < -32768) q = -32768;
        u16(static_cast<uint16_t>(static_cast<int16_t>(q)));
    }
}

struct Boundary { std::string name; int64_t frame; };

// ---------------------------------------------------------------- Calibrate（W1-BD5）のオフライン書き出し
// 速い経路（full / rm）・遅い経路 6 シナリオ・スキップ 5 時刻を、実機と同じシーケンサ + ドライバでメモリ上に描いて WAV と JSON に書く。
// 音デバイスは開かない。JSON の tailFrame（lock_tail の頭）・handoverFrame・skipFrame は、A/V 検証（BD8）が音と絵の時刻を突き合わせるのに使う。
std::vector<uint8_t> ReadBytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void WriteWavExact(const std::string& path, const std::vector<float>& st, int sr)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(st.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(static_cast<uint32_t>(sr)); u32(static_cast<uint32_t>(sr * 4)); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : st) u16(static_cast<uint16_t>(sp::seq::FloatToPcm16(v)));   // ParseWavPcm16 の厳密な逆（graphite_1.wav とバイト一致できる）
}

struct CalRun
{
    std::vector<float> out;
    int64_t handoverFrame = -1, skipFrame = -1, tailFrame = -1;
    double lockMs = std::nan("");
};

CalRun RunCalibrate(const std::shared_ptr<const sp::CalibrateBank>& bank, bool reduce, double finishMs, double skipMs, double endMs)
{
    sp::CalibrateSequencer seq(bank, reduce);
    sp::GaugeFastDriver drv;
    CalRun r;
    constexpr int64_t kAhead = 882;   // 音は 20ms 先まで描き置く
    std::vector<float> tmp(441 * 2);
    auto fill = [&](int64_t target) {
        while (static_cast<int64_t>(r.out.size() / 2) < target)
        {
            seq.Render(tmp.data(), 441);
            r.out.insert(r.out.end(), tmp.begin(), tmp.end());
        }
    };
    seq.Begin();
    fill(kAhead);
    bool handed = false, skipped = false, skipDone = false;
    for (int k = 0;; ++k)
    {
        const double t = k * 1000.0 / 60.0;
        if (t > endMs) break;
        if (!reduce)
        {
            drv.Update(t, std::isnan(finishMs) ? 0.2f : static_cast<float>(std::min(0.99, t / finishMs)), !std::isnan(finishMs) && t >= finishMs);
            if (drv.HandedOver() && !handed) { handed = true; r.handoverFrame = std::max(seq.OutFrame(), sp::seq::MsToFrame(t)); seq.HandOver(t); }
        }
        if (!std::isnan(skipMs) && !skipDone && t >= skipMs)
        {
            skipDone = true;
            const double ts = std::max(t, sp::seq::FrameToMs(static_cast<double>(seq.OutFrame())));
            if (reduce) { seq.Skip(ts); skipped = true; }
            else { drv.Skip(ts); if (drv.Skipped()) { seq.Skip(drv.SkipWallMs()); skipped = true; } }
        }
        if (!reduce)
        {
            for (uint32_t id : drv.TakeCancelledCueIds()) seq.CancelCue(id);
            for (const sp::GaugeCue& c : drv.TakeCues()) seq.AddCue(c);
        }
        fill(sp::seq::MsToFrame(t) + kAhead);
    }
    while (!seq.Idle() && r.out.size() / 2 < 20u * 44100u) fill(static_cast<int64_t>(r.out.size() / 2) + 441);
    r.tailFrame = seq.TailStartFrame();
    r.skipFrame = skipped ? r.tailFrame : -1;
    r.lockMs = reduce ? std::nan("") : drv.LockWallMs();
    return r;
}

int DumpCalibrate(const std::string& root, const std::string& outDir)
{
    const std::string dir = root + "/assets/editor/sounds/calibrate/";
    sp::CalibrateBlobs b;
    std::vector<uint8_t> full = ReadBytes(dir + "graphite_1.wav"), rm = ReadBytes(dir + "graphite_1_rm.wav"), bed = ReadBytes(dir + "stems/bed.wav"),
                         tail = ReadBytes(dir + "stems/lock_tail.wav"), clicks[24];
    auto v = [](const std::vector<uint8_t>& x) { return sp::BlobView{ x.data(), x.size() }; };
    b.full = v(full); b.rm = v(rm); b.bed = v(bed); b.lockTail = v(tail);
    for (int i = 0; i < 24; ++i)
    {
        char n[40];
        std::snprintf(n, sizeof(n), "stems/click_%02d.wav", i);
        clicks[i] = ReadBytes(dir + n);
        b.click[i] = v(clicks[i]);
    }
    auto bank = std::make_shared<sp::CalibrateBank>();
    if (!sp::BuildCalibrateBank(b, *bank)) { std::printf("calibrate bank load failed (%s)\n", dir.c_str()); return 1; }

    struct Scn { std::string name; bool reduce; double finish, skip, end; };
    const double nan = std::nan("");
    std::vector<Scn> scns = { { "cal_fast", false, 1200, nan, 5000 }, { "cal_fast_rm", true, nan, nan, 5000 } };
    for (double f : { 1500.0, 1800.0, 2300.0, 3200.0, 5000.0, 9000.0 }) scns.push_back({ "cal_slow_" + std::to_string(static_cast<int>(f)), false, f, nan, f + 3000 });
    for (double s : { 300.0, 800.0, 1300.0, 1700.0, 2500.0 }) scns.push_back({ "cal_skip_" + std::to_string(static_cast<int>(s)), false, nan, s, s + 4500 });
    scns.push_back({ "cal_skip_rm_800", true, nan, 800, 5000 });

    const float srcMax = sp::MaxStepDiff(bank->full, 0, static_cast<size_t>(bank->FullFrames()));
    int rc = 0;
    for (const Scn& sc : scns)
    {
        const CalRun r = RunCalibrate(bank, sc.reduce, sc.finish, sc.skip, sc.end);
        WriteWavExact(outDir + "/" + sc.name + ".wav", r.out, 44100);
        const float m = sp::MaxStepDiff(r.out, 0, r.out.size() / 2);
        std::ofstream j(outDir + "/" + sc.name + ".json");
        j << "{\n  \"scenario\": \"" << sc.name << "\",\n  \"sampleRate\": 44100,\n  \"reduceMotion\": " << (sc.reduce ? "true" : "false")
          << ",\n  \"frames\": " << r.out.size() / 2 << ",\n  \"handoverFrame\": " << r.handoverFrame << ",\n  \"skipFrame\": " << r.skipFrame
          << ",\n  \"tailFrame\": " << r.tailFrame << ",\n  \"maxStep\": " << m << ",\n  \"masterMaxStep\": " << srcMax << "\n}\n";
        const bool ok = m <= srcMax * 1.6f;
        std::printf("%-16s frames=%-7zu handover=%-6lld tail=%-7lld maxStep=%.5f (master %.5f) %s\n", sc.name.c_str(), r.out.size() / 2,
                    static_cast<long long>(r.handoverFrame), static_cast<long long>(r.tailFrame), m, srcMax, ok ? "OK" : "CLICK?");
        if (!ok) rc = 1;
    }
    return rc;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 4 && std::string(argv[1]) == "--calibrate") return DumpCalibrate(argv[2], argv[3]);
    if (argc < 2) { std::printf("usage: SplashSyncDump <outDir> [splash_d.wav] | --calibrate <repo-root> <outDir>\n"); return 2; }
    const std::string outDir = argv[1];
    std::string what;
    auto asset = LoadAsset(argc >= 3 ? argv[2] : "", what);
    if (!asset) { std::printf("asset load failed\n"); return 1; }
    const int sr = asset->sampleRate;
    std::printf("asset: %s  frames=%lld (%.3fs)  onset=%lld (%.3fs)\n", what.c_str(),
                static_cast<long long>(asset->frameCount()), static_cast<double>(asset->frameCount()) / sr,
                static_cast<long long>(asset->onsetFrame), static_cast<double>(asset->onsetFrame) / sr);

    struct Scn { const char* file; const char* label; double triggerSec; };
    const Scn scns[] = { { "sync_early", "early", 1.6 }, { "sync_ontime", "ontime", 3.0 }, { "sync_late", "late", 5.0 } };
    const double predicted = 3.0;   // 予測した「TriggerHit が来る時刻」

    int rc = 0;
    for (const Scn& s : scns)
    {
        sp::Mixer m(asset);
        m.Start(predicted);
        const int64_t trig = static_cast<int64_t>(std::llround(s.triggerSec * sr));
        m.RequestHitAtFrame(trig);
        std::vector<float> out;
        std::vector<float> chunk(2 * 441);
        while (!m.Finished() && out.size() < static_cast<size_t>(sr) * 2 * 30)
        {
            m.Render(chunk.data(), 441);
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        // 境界（クリックが出うる場所）
        std::vector<Boundary> bs;
        bs.push_back({ "start", 0 });
        const int64_t H = m.HoldEnd();
        const int64_t p0 = H - static_cast<int64_t>(std::llround(predicted * sr));
        if (p0 < 0) bs.push_back({ "riser_start_after_delay", -p0 });
        if (p0 > 0) bs.push_back({ "start_fade_end", static_cast<int64_t>(std::llround(0.04 * sr)) });
        const int64_t wrap = static_cast<int64_t>(std::llround(predicted * sr));    // riser が H に届く出力フレーム
        if (m.HoldLoop() > 0 && trig > wrap)
            for (int64_t w = wrap; w < trig; w += m.HoldLoop())
            {
                bs.push_back({ "hold_wrap", w });
                bs.push_back({ "hold_seam_end", w + m.Seam() });
            }
        bs.push_back({ "trigger", trig });
        bs.push_back({ "crossfade_end", trig + m.Xfade() });
        bs.push_back({ "end", static_cast<int64_t>(out.size() / 2) - 1 });

        const std::string base = outDir + "/" + s.file;
        WriteWav16(base + ".wav", out, sr);
        std::ofstream j(base + ".json");
        j << "{\n  \"scenario\": \"" << s.label << "\",\n  \"sampleRate\": " << sr
          << ",\n  \"triggerFrame\": " << trig << ",\n  \"hitOnsetFrame\": " << m.HitOnsetOutputFrame()
          << ",\n  \"popFrame\": " << m.PopOutputFrame() << ",\n  \"boundaries\": [\n";
        for (size_t i = 0; i < bs.size(); ++i)
            j << "    {\"name\": \"" << bs[i].name << "\", \"frame\": " << bs[i].frame << "}" << (i + 1 < bs.size() ? "," : "") << "\n";
        j << "  ]\n}\n";

        const float srcMax = sp::MaxStepDiff(asset->src, 0, static_cast<size_t>(asset->frameCount()));
        const float outMax = sp::MaxStepDiff(out, 0, out.size() / 2);
        std::printf("%-12s trigger=%.3fs onset@%.3fs (out %.3fs)  maxStep out=%.5f src=%.5f  %s\n", s.file, s.triggerSec,
                    static_cast<double>(m.HitOnsetOutputFrame()) / sr, static_cast<double>(out.size() / 2) / sr, outMax, srcMax,
                    outMax <= srcMax * 1.6f ? "OK" : "CLICK?");
        if (outMax > srcMax * 1.6f) rc = 1;
    }
    return rc;
}
