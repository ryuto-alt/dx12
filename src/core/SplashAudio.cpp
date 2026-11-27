#include "core/SplashAudio.h"

#include <Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <wrl/client.h>

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
#include "core/SplashSequencer.h"
#include "core/SplashSoundData.h"   // 埋め込みの案 A（kSplashSoundWav）。約 500KB。この TU だけが取り込む

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

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


// ===========================================================================
// Calibrate（W1-BD5）: 音が主時計。純ロジックは core/SplashSequencer.h。ここは「素材の取り出し → 出力デバイス → 再生位置の報告」だけ。
// ===========================================================================
namespace
{
namespace seqd = splash;

// エディタの exe の RCDATA（resources/editor.rc の lane-B ブロック）から素材を取り出す。無ければ（テストの exe など）false
bool LoadBlob(const wchar_t* name, seqd::BlobView& out)
{
    HRSRC r = FindResourceW(nullptr, name, RT_RCDATA);
    if (!r) return false;
    HGLOBAL h = LoadResource(nullptr, r);
    const DWORD sz = SizeofResource(nullptr, r);
    if (!h || sz == 0) return false;
    out.data = static_cast<const uint8_t*>(LockResource(h));
    out.size = sz;
    return out.data != nullptr;
}

std::shared_ptr<const seqd::CalibrateBank> LoadCalibrateBank()
{
    seqd::CalibrateBlobs b;
    if (!LoadBlob(L"UNO_CALIBRATE_FULL", b.full) || !LoadBlob(L"UNO_CALIBRATE_RM", b.rm) || !LoadBlob(L"UNO_CALIBRATE_BED", b.bed)
        || !LoadBlob(L"UNO_CALIBRATE_LOCK_TAIL", b.lockTail))
        return nullptr;
    for (int i = 0; i < splash::calibrate::kClickCount; ++i)
    {
        wchar_t n[40];
        swprintf_s(n, L"UNO_CALIBRATE_CLICK_%02d", i);
        if (!LoadBlob(n, b.click[i])) return nullptr;
    }
    auto bank = std::make_shared<seqd::CalibrateBank>();
    if (!seqd::BuildCalibrateBank(b, *bank)) return nullptr;
    return bank;
}

double QpcNowMs()
{
    static const double inv = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv;
}

struct CalState
{
    std::mutex mu;                         // シーケンサと時計の保護（コマンドは短い。Render も 1 チャンクぶんだけ）
    std::shared_ptr<const seqd::CalibrateBank> bank;
    std::unique_ptr<seqd::CalibrateSequencer> seq;
    seqd::AudioClockMap clock;
    HANDLE thread = nullptr;
    std::atomic<int> state{static_cast<int>(CalibrateSound::State::NotStarted)};
    std::atomic<bool> abortReq{false};
    std::atomic<bool> expected{false};
    bool reduce = false;
    const char* backend = "none";
};

CalState& CS()
{
    static CalState* s = new CalState();   // 意図的にリーク（プロセス終了時にスレッドと競合しない）
    return *s;
}

// 出力デバイスの共通の形
struct IOut
{
    virtual ~IOut() = default;
    virtual bool Open() = 0;
    virtual bool Prime(CalState& c) = 0;                   // 最初の 1 バッファを描いて再生を始める
    virtual bool Pump(CalState& c, int waitMs) = 0;        // イベントを待ち、空きを描き足し、再生位置を報告する。続けられなければ false
    virtual void Drain() = 0;                              // 鳴らし切ってから止める
    virtual const char* Name() const = 0;
};

constexpr GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct WasapiOut final : IOut
{
    Microsoft::WRL::ComPtr<IAudioClient> client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render;
    Microsoft::WRL::ComPtr<IAudioClock> aclock;
    HANDLE ev = nullptr;
    UINT32 bufFrames = 0;
    UINT64 clockFreq = 0;
    HANDLE mmcss = nullptr;
    bool started = false;
    ~WasapiOut() override
    {
        if (started && client) client->Stop();
        if (ev) CloseHandle(ev);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }
    const char* Name() const override { return "wasapi"; }

    bool Open() override
    {
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return false;
        Microsoft::WRL::ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())))) return false;
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = seqd::seq::kSampleRate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * 8;
        wf.Format.cbSize = 22;
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = kSubtypeFloat;
        // 共有モード・イベント駆動。形式の変換（44.1kHz → デバイスの形式）はエンジンに任せる。バッファは 20ms（エンジンの周期に丸められる）
        constexpr REFERENCE_TIME kBuf = 200000;   // 100ns 単位
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      kBuf, 0, &wf.Format, nullptr)))
            return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev || FAILED(client->SetEventHandle(ev))) return false;
        if (FAILED(client->GetBufferSize(&bufFrames)) || bufFrames == 0) return false;
        if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) return false;
        if (FAILED(client->GetService(IID_PPV_ARGS(&aclock))) || FAILED(aclock->GetFrequency(&clockFreq)) || clockFreq == 0) return false;
        DWORD idx = 0;
        mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &idx);   // 失敗しても続ける
        return true;
    }

    bool Fill(CalState& c, UINT32 frames)
    {
        if (frames == 0) return true;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data))) return false;
        {
            std::lock_guard<std::mutex> lk(c.mu);
            c.seq->Render(reinterpret_cast<float*>(data), static_cast<int>(frames));
        }
        return SUCCEEDED(render->ReleaseBuffer(frames, 0));
    }

    bool Prime(CalState& c) override
    {
        if (!Fill(c, bufFrames)) return false;     // 出力フレーム 0 から。ストリーム位置 0 = タイムライン 0
        if (FAILED(client->Start())) return false;
        started = true;
        return true;
    }

    void Report(CalState& c)
    {
        UINT64 pos = 0, qpc100 = 0;
        if (FAILED(aclock->GetPosition(&pos, &qpc100))) return;
        const std::int64_t frames = static_cast<std::int64_t>(static_cast<double>(pos) * seqd::seq::kSampleRate / static_cast<double>(clockFreq));
        std::lock_guard<std::mutex> lk(c.mu);
        c.clock.Report(frames, static_cast<double>(qpc100) / 1.0e4);   // 100ns → ms（QPC の時計。QpcNowMs と同じ基準）
    }

    bool Pump(CalState& c, int waitMs) override
    {
        const DWORD w = WaitForSingleObject(ev, static_cast<DWORD>(waitMs));
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) return false;
        if (w == WAIT_OBJECT_0 && bufFrames > pad && !Fill(c, bufFrames - pad)) return false;
        Report(c);
        return true;
    }
    void Drain() override { Sleep(static_cast<DWORD>(1000ull * bufFrames / seqd::seq::kSampleRate) + 30); }
};

struct WaveOutDev final : IOut
{
    static constexpr int kBuffers = 4, kChunk = 256;   // 4 x 5.8ms = 23ms 先まで（予約の先読み 50ms より短く）
    HWAVEOUT hwo = nullptr;
    HANDLE ev = nullptr;
    std::vector<std::vector<int16_t>> pcm;
    std::vector<WAVEHDR> hdr;
    std::vector<float> tmp;
    bool prepared = false;
    ~WaveOutDev() override
    {
        if (hwo)
        {
            waveOutReset(hwo);
            if (prepared) for (auto& h : hdr) waveOutUnprepareHeader(hwo, &h, sizeof(WAVEHDR));
            waveOutClose(hwo);
        }
        if (ev) CloseHandle(ev);
    }
    const char* Name() const override { return "waveout"; }

    bool Open() override
    {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = seqd::seq::kSampleRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev || waveOutOpen(&hwo, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { hwo = nullptr; return false; }
        pcm.assign(kBuffers, std::vector<int16_t>(static_cast<size_t>(kChunk) * 2));
        hdr.assign(kBuffers, WAVEHDR{});
        tmp.assign(static_cast<size_t>(kChunk) * 2, 0.0f);
        for (int i = 0; i < kBuffers; ++i)
        {
            hdr[static_cast<size_t>(i)].lpData = reinterpret_cast<LPSTR>(pcm[static_cast<size_t>(i)].data());
            hdr[static_cast<size_t>(i)].dwBufferLength = static_cast<DWORD>(pcm[static_cast<size_t>(i)].size() * sizeof(int16_t));
            waveOutPrepareHeader(hwo, &hdr[static_cast<size_t>(i)], sizeof(WAVEHDR));
        }
        prepared = true;
        return true;
    }
    bool Write(CalState& c, int i)
    {
        {
            std::lock_guard<std::mutex> lk(c.mu);
            c.seq->Render(tmp.data(), kChunk);
        }
        auto& b = pcm[static_cast<size_t>(i)];
        for (size_t k = 0; k < tmp.size(); ++k) b[k] = seqd::seq::FloatToPcm16(tmp[k]);
        hdr[static_cast<size_t>(i)].dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
        return waveOutWrite(hwo, &hdr[static_cast<size_t>(i)], sizeof(WAVEHDR)) == MMSYSERR_NOERROR;
    }
    bool Prime(CalState& c) override
    {
        for (int i = 0; i < kBuffers; ++i) if (!Write(c, i)) return false;
        return true;
    }
    bool Pump(CalState& c, int waitMs) override
    {
        WaitForSingleObject(ev, static_cast<DWORD>(waitMs));
        for (int i = 0; i < kBuffers; ++i)
        {
            WAVEHDR& h = hdr[static_cast<size_t>(i)];
            if ((h.dwFlags & WHDR_DONE) && !Write(c, i)) return false;
        }
        MMTIME mt{};
        mt.wType = TIME_SAMPLES;
        if (waveOutGetPosition(hwo, &mt, sizeof(mt)) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES)
        {
            const double q = QpcNowMs();
            std::lock_guard<std::mutex> lk(c.mu);
            c.clock.Report(static_cast<std::int64_t>(mt.u.sample), q);
        }
        return true;
    }
    void Drain() override { Sleep(static_cast<DWORD>(1000ull * kChunk * kBuffers / seqd::seq::kSampleRate) + 30); }
};

void ResetSequencer(CalState& c)
{
    std::lock_guard<std::mutex> lk(c.mu);
    c.seq = std::make_unique<seqd::CalibrateSequencer>(c.bank, c.reduce);
    c.seq->Begin();
}

DWORD WINAPI CalibThread(LPVOID)
{
    CalState& c = CS();
    const bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    c.bank = LoadCalibrateBank();
    std::unique_ptr<IOut> out;
    if (c.bank)
    {
        ResetSequencer(c);
        auto wa = std::make_unique<WasapiOut>();
        if (wa->Open() && wa->Prime(c)) out = std::move(wa);
        else
        {
            SplashDiag("起動音(Calibrate): WASAPI を開けませんでした。waveOut へ縮退します");
            wa.reset();
            ResetSequencer(c);                       // Prime が途中まで進めたシーケンサを作り直す
            auto wo = std::make_unique<WaveOutDev>();
            if (wo->Open() && wo->Prime(c)) out = std::move(wo);
        }
    }
    else
        SplashDiag("起動音(Calibrate): 素材を読めませんでした（無音で続行。絵は QPC の時計で進む）");
    if (!out)
    {
        c.state.store(static_cast<int>(CalibrateSound::State::Failed));
        if (comOk) CoUninitialize();
        return 0;
    }
    c.backend = out->Name();
    c.state.store(static_cast<int>(CalibrateSound::State::Running));
    SplashDiag(std::string("起動音(Calibrate): ") + c.backend);

    const double t0 = QpcNowMs();
    bool aborting = false;
    double abortAt = 0;
    while (true)
    {
        if (!out->Pump(c, 10)) break;
        const double now = QpcNowMs();
        if (!aborting && c.abortReq.load())
        {
            aborting = true;
            abortAt = now;
            std::lock_guard<std::mutex> lk(c.mu);
            c.seq->Abort();
        }
        bool idle;
        {
            std::lock_guard<std::mutex> lk(c.mu);
            idle = c.seq->Idle();
        }
        if (idle) break;
        if (aborting && now - abortAt > 400.0) break;
        if (now - t0 > 60000.0) break;               // 何があっても 1 分で止める
    }
    out->Drain();
    out.reset();
    c.state.store(static_cast<int>(CalibrateSound::State::Finished));
    if (comOk) CoUninitialize();
    return 0;
}
} // namespace

bool CalibrateSound::Start(const splash::SoundGate& gate, bool reduceMotion)
{
    CalState& c = CS();
    if (c.thread) return c.expected.load();                            // 二重起動しない
    return splash::OpenIfAllowed(gate, [&]() -> bool {                 // 鳴らさない条件ではここに来ない（デバイスも素材も触らない）
        c.reduce = reduceMotion;
        c.abortReq.store(false);
        c.state.store(static_cast<int>(State::Opening));
        c.thread = CreateThread(nullptr, 0, CalibThread, nullptr, 0, nullptr);
        if (!c.thread) { c.state.store(static_cast<int>(State::Failed)); return false; }
        SetThreadPriority(c.thread, THREAD_PRIORITY_HIGHEST);
        c.expected.store(true);
        return true;
    });
}

CalibrateSound::State CalibrateSound::GetState() { return static_cast<State>(CS().state.load()); }
bool CalibrateSound::Expected() { return CS().expected.load(); }
double CalibrateSound::QpcMs() { return QpcNowMs(); }
const char* CalibrateSound::BackendName() { return CS().backend; }

bool CalibrateSound::TimelineMs(double qpcMs, double* outMs)
{
    CalState& c = CS();
    const int st = c.state.load();
    if (st != static_cast<int>(State::Running) && st != static_cast<int>(State::Finished)) return false;
    std::lock_guard<std::mutex> lk(c.mu);
    if (!c.clock.Valid()) return false;
    *outMs = c.clock.TimelineMsAt(qpcMs);
    return true;
}

double CalibrateSound::RenderedMs()
{
    CalState& c = CS();
    std::lock_guard<std::mutex> lk(c.mu);
    return c.seq ? c.seq->RenderedMs() : 0.0;
}

void CalibrateSound::HandOver(double nowMs)
{
    CalState& c = CS();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.seq) c.seq->HandOver(nowMs);
}

void CalibrateSound::AddCues(const std::vector<splash::GaugeCue>& cues)
{
    CalState& c = CS();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.seq) for (const splash::GaugeCue& q : cues) c.seq->AddCue(q);
}

void CalibrateSound::CancelCues(const std::vector<std::uint32_t>& ids)
{
    CalState& c = CS();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.seq) for (std::uint32_t id : ids) c.seq->CancelCue(id);
}

void CalibrateSound::Skip(double tMs)
{
    CalState& c = CS();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.seq) c.seq->Skip(tMs);
}

void CalibrateSound::Abort() { CS().abortReq.store(true); }

} // namespace dx12e
