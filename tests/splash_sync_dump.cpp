// 起動音ミキサーのオフライン書き出し（音デバイスは開かない）。
//   SplashSyncDump <出力ディレクトリ> [splash_d.wav のパス]
// 頂点の予測を T=3.0s として、実際の初期化が「早い(1.6s) / 定刻(3.0s) / 遅い(5.0s)」の 3 シナリオを
// ミキサーで計算し、sync_early.wav / sync_ontime.wav / sync_late.wav と、区間境界の一覧 sync_*.json を書く。
// つなぎ目のクリック検査は tools/check_splash_sync.py（WAV を独立に読んで境界の不連続量を測る）。
// 素材: 引数の WAV（+同名の .json メタ）があればそれ。無ければ埋め込みの案 A（riser 無しの単発音）。
#include "core/SplashMixer.h"
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
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: SplashSyncDump <outDir> [splash_d.wav]\n"); return 2; }
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
