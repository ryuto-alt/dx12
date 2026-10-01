#include "core/SplashAudio.h"

#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>

#include "core/SplashDiag.h"
#include "core/SplashMixer.h"
#include "core/SplashSoundData.h"   // 埋め込みの案 A（kSplashSoundWav）。約 500KB。この TU だけが取り込む

#pragma comment(lib, "winmm.lib")

namespace dx12e
{
namespace
{
namespace sp = splash;

constexpr int kSampleRate = 44100;
constexpr int kChunkFrames = 512;      // 11.6ms。小さいほど TriggerHit の反映が細かい
constexpr int kBufferCount = 5;        // 5 本 = 約 58ms 先まで供給（遅延と取りこぼしの折衷）

struct State
{
    std::mutex mu;
    HANDLE thread = nullptr;
    std::shared_ptr<sp::SoundAsset> asset;
    std::atomic<bool> hitFlag{false};
    std::atomic<bool> abortFlag{false};
    std::atomic<bool> running{false};
    double triggerOffset = 0.0;
    std::string source = "none";
    double predicted = 3.0;
};

State& S()
{
    static State* s = new State();     // 意図的にリーク（プロセス終了時にスレッドと競合しない）
    return *s;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// splash_d は出どころ未確認で配布物に入れない（リポジトリは PUBLIC）。配布版・自動更新後でもこの PC では
// 同じ起動音が鳴るよう、利用者ごとのフォルダ %LOCALAPPDATA%\DX12Engine\sounds\ にも置ける（更新では触らない）。
std::wstring UserSoundDir()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(buf) + L"\\DX12Engine\\sounds\\";
}

// 音源を探して読む。<assets>/editor/sounds/splash_d.wav（+json）→ 利用者フォルダの splash_d → 埋め込みの案 A の順。
std::shared_ptr<sp::SoundAsset> LoadAsset(const std::wstring& assetsDir, std::string& what)
{
    auto a = std::make_shared<sp::SoundAsset>();
    for (const std::wstring& dir : { assetsDir, UserSoundDir() })
    {
        if (dir.empty()) continue;
        std::vector<uint8_t> wav, meta;
        if (ReadFileBytes(dir + L"splash_d.wav", wav) && ReadFileBytes(dir + L"splash_d.json", meta))
        {
            sp::WavData w;
            int64_t onset = 0, pop = 0;
            if (sp::ParseWavPcm16(wav.data(), wav.size(), w) && w.sampleRate == kSampleRate
                && sp::ParseJsonInt(reinterpret_cast<const char*>(meta.data()), meta.size(), "onsetFrame", onset))
            {
                sp::ParseJsonInt(reinterpret_cast<const char*>(meta.data()), meta.size(), "popOffsetFrames", pop);
                if (sp::MakeAssetFromWav(std::move(w), onset, pop, *a))
                {
                    what = "splash_d.wav";
                    return a;
                }
            }
            SplashDiag("splash_d.wav を読めませんでした（形式が想定外）。次の候補を探します");
        }
    }
    sp::WavData w;
    if (!sp::ParseWavPcm16(kSplashSoundWav, sizeof(kSplashSoundWav), w) || w.sampleRate != kSampleRate) return nullptr;
    a->sampleRate = w.sampleRate;
    a->src = std::move(w.frames);
    a->onsetFrame = 0;                  // riser 無しの単発音: TriggerHit から先頭が流れる
    a->popOffsetFrames = 441;           // 案 A の「ぽん」は先頭から約 10ms
    what = "embedded";
    return a;
}

DWORD WINAPI AudioThread(LPVOID)
{
    State& st = S();
    auto asset = st.asset;
    sp::Mixer mixer(asset);
    mixer.Start(st.predicted);

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 2;
    fmt.nSamplesPerSec  = kSampleRate;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT hwo = nullptr;
    if (!ev || waveOutOpen(&hwo, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
    {
        SplashDiag("起動音: 出力デバイスを開けませんでした（無音で続行）");
        if (ev) CloseHandle(ev);
        st.running = false;
        return 0;
    }

    std::vector<std::vector<int16_t>> pcm(kBufferCount, std::vector<int16_t>(static_cast<size_t>(kChunkFrames) * 2));
    std::vector<WAVEHDR> hdr(kBufferCount);
    std::vector<float> tmp(static_cast<size_t>(kChunkFrames) * 2);
    for (int i = 0; i < kBufferCount; ++i)
    {
        hdr[static_cast<size_t>(i)] = WAVEHDR{};
        hdr[static_cast<size_t>(i)].lpData = reinterpret_cast<LPSTR>(pcm[static_cast<size_t>(i)].data());
        hdr[static_cast<size_t>(i)].dwBufferLength = static_cast<DWORD>(pcm[static_cast<size_t>(i)].size() * sizeof(int16_t));
        waveOutPrepareHeader(hwo, &hdr[static_cast<size_t>(i)], sizeof(WAVEHDR));
    }

    bool draining = false;   // 全部出し切った（以降は終了待ち）
    int  inFlight = 0;
    auto fill = [&](int i) -> bool {
        if (draining) return false;
        if (st.hitFlag.exchange(false)) mixer.RequestHit();
        if (st.abortFlag.exchange(false)) mixer.Abort();
        mixer.Render(tmp.data(), kChunkFrames);
        auto& b = pcm[static_cast<size_t>(i)];
        for (size_t k = 0; k < tmp.size(); ++k)
        {
            long q = std::lround(tmp[k] * 32767.0f);
            b[k] = static_cast<int16_t>(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
        }
        if (waveOutWrite(hwo, &hdr[static_cast<size_t>(i)], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
        ++inFlight;
        if (mixer.Finished()) draining = true;     // このチャンクが最後
        return true;
    };

    for (int i = 0; i < kBufferCount; ++i) fill(i);

    const ULONGLONG t0 = GetTickCount64();
    while (inFlight > 0)
    {
        WaitForSingleObject(ev, 40);
        for (int i = 0; i < kBufferCount; ++i)
        {
            WAVEHDR& h = hdr[static_cast<size_t>(i)];
            if ((h.dwFlags & WHDR_DONE) && (h.dwFlags & WHDR_PREPARED))
            {
                h.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
                --inFlight;
                fill(i);
            }
        }
        if (GetTickCount64() - t0 > 60000) break;   // 何があっても 1 分で止める
    }

    waveOutReset(hwo);
    for (int i = 0; i < kBufferCount; ++i) waveOutUnprepareHeader(hwo, &hdr[static_cast<size_t>(i)], sizeof(WAVEHDR));
    waveOutClose(hwo);
    CloseHandle(ev);
    st.running = false;
    return 0;
}
} // namespace

bool SplashSound::Start(const std::wstring& soundDir, double predictedFinishSec, double popAfterReadySec)
{
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.thread) return st.running.load();   // 二重起動しない

    std::string what;
    auto asset = LoadAsset(soundDir, what);
    if (!asset)
    {
        SplashDiag("起動音: 音源を読めませんでした（無音で続行）");
        return false;
    }
    st.asset = asset;
    st.source = what;
    st.hitFlag = false;
    st.abortFlag = false;
    {
        // 視覚の「ポン」= TriggerHit + (クロスフェード + popOffset)。デバイスのバッファ遅延ぶんも見込んで逆算する。
        sp::Mixer probe(asset);
        constexpr double kDeviceLatencySec = 0.06;
        st.triggerOffset = std::max(0.0, popAfterReadySec - (probe.TriggerToPopSec() + kDeviceLatencySec));
        st.predicted = std::max(0.0, predictedFinishSec) + st.triggerOffset;    // riser が終端へ届く予測時刻
    }
    st.running = true;
    st.thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
    if (!st.thread)
    {
        st.running = false;
        SplashDiag("起動音: 再生スレッドを作れませんでした");
        return false;
    }
    SetThreadPriority(st.thread, THREAD_PRIORITY_HIGHEST);
    SplashDiag("起動音: " + what + "  予測(終了要求) " + std::to_string(static_cast<int>(predictedFinishSec * 1000.0)) + "ms");
    return true;
}

void SplashSound::TriggerHit() { S().hitFlag = true; }
void SplashSound::Abort()      { S().abortFlag = true; }
double SplashSound::TriggerOffsetSec() { return S().triggerOffset; }
bool SplashSound::Active() { return S().running.load(); }
const char* SplashSound::SourceName() { return S().source.c_str(); }

} // namespace dx12e
