#include "audio/AudioSystem.h"
#include "audio/AudioClip.h"
#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <xaudio2fx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace dx12e
{

namespace
{
// master の ProcessingStage。子は深さ 1 段ごとに 16 ずつ下げる（子 → 親の順に処理させる）。
constexpr UINT32 kMasterStage = 1u << 20;
constexpr u32    kMaxBusDepth = 8;

// 1 本だけの送り先を組む（CreateSourceVoice / CreateSubmixVoice の pSendList 用）。
struct OneSend
{
    XAUDIO2_SEND_DESCRIPTOR desc{};
    XAUDIO2_VOICE_SENDS     sends{};
    explicit OneSend(IXAudio2Voice* v)
    {
        desc.Flags        = 0;
        desc.pOutputVoice = v;
        sends.SendCount   = 1;
        sends.pSends      = &desc;
    }
    XAUDIO2_VOICE_SENDS* Get() { return desc.pOutputVoice ? &sends : nullptr; }
};
} // namespace

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem()
{
    Shutdown();
}

void AudioSystem::Initialize(const std::string& assetsDir)
{
    m_assetsDir = assetsDir;

    // バスは「値だけ」先に作る。デバイスが無い環境（ヘッドレス CI / 音声デバイス無しの PC）でも
    // setBusVolume などの状態は保持し、audio_state も返せるようにするため。
    if (m_buses.empty())
    {
        auto add = [&](const char* name, i32 parent, f32 vol, f32 reverbSend) {
            Bus b;
            b.name       = name;
            b.parent     = parent;
            b.depth      = (parent < 0) ? 0u : m_buses[static_cast<size_t>(parent)].depth + 1u;
            b.volume     = vol;
            b.reverbSend = reverbSend;
            b.builtin    = true;
            m_buses.push_back(std::move(b));
        };
        // リバーブ送りの既定: 空間に鳴る音（sfx / ambience）は全量、声は少し、音楽と UI は送らない
        //（BGM や UI 音が洞窟で響くと「画面の外の音」と「世界の中の音」の区別が崩れる）。
        add("master",   -1, 1.0f, 0.0f);
        add("music",     0, 0.7f, 0.0f);   // ★旧 m_bgmVolume の既定 0.7 をそのまま引き継ぐ（音量が変わらない）
        add("sfx",       0, 1.0f, 1.0f);
        add("ambience",  0, 1.0f, 1.0f);
        add("voice",     0, 1.0f, 0.6f);
        add("ui",        0, 1.0f, 0.0f);
        add("reverb",    0, 1.0f, 0.0f);
        m_buses.back().isReverb = true;
        m_reverbBus = static_cast<i32>(m_buses.size()) - 1;
        audio::FindReverbPreset("none", m_reverbCur);
        m_reverbTarget = m_reverbApplied = m_reverbCur;
    }

    // ★テスト用の口: DX12_AUDIO_DEVICE=none で「音声デバイスが無い PC」を再現する。
    //   ヘッドレス CI やリモートデスクトップでは実際にこの状態になるので、落ちない・状態は返すを
    //   手元で確かめられるようにしておく。
    {
        char env[32] = {};
        size_t len = 0;
        if (getenv_s(&len, env, sizeof(env), "DX12_AUDIO_DEVICE") == 0 && len > 0
            && _stricmp(env, "none") == 0)
        {
            m_deviceStatus = "disabled (DX12_AUDIO_DEVICE=none)";
            Logger::Warn("音声デバイスを使わずに起動します（DX12_AUDIO_DEVICE=none）。"
                         "再生要求は状態だけ記録し、音は出ません");
            ScanAudioFiles();
            return;
        }
    }

    // COM初期化（XAudio2に必要）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comInitialized = (hr == S_OK);  // S_FALSE = 既に初期化済み

    // XAudio2エンジン作成
    hr = XAudio2Create(&m_xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        Logger::Error("XAudio2 の初期化に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        m_deviceStatus = "XAudio2Create failed";
        m_xaudio2.Reset();
        ScanAudioFiles();
        return;
    }

    // マスタリングボイス作成
    hr = m_xaudio2->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr))
    {
        // ★音声デバイスが無い PC ではここで落ちる。以前は m_xaudio2 を残したまま return していたので、
        //   以降の再生のたびに CreateSourceVoice が失敗してエラーログが 1 回ずつ出続けていた。
        Logger::Error("マスタリングボイスの作成に失敗しました（音声デバイスが無い可能性）: 0x{:08X}",
                      static_cast<u32>(hr));
        m_deviceStatus = "no audio device (CreateMasteringVoice failed)";
        m_masterVoice = nullptr;
        m_xaudio2.Reset();
        ScanAudioFiles();
        return;
    }
    m_deviceStatus = "ok";

    // X3DAudio 初期化（3D 空間オーディオ）
    {
        XAUDIO2_VOICE_DETAILS details{};
        m_masterVoice->GetVoiceDetails(&details);
        m_outChannels = (details.InputChannels > 0) ? details.InputChannels : 2;
        if (m_outChannels > 8) m_outChannels = 8;
        m_outSampleRate = (details.InputSampleRate > 0) ? details.InputSampleRate : 48000;

        // バスのサブミックスボイスを親から順に作る（m_buses は親が必ず子より前）。
        for (auto& b : m_buses) CreateBusVoice(b);

        DWORD channelMask = 0;
        m_masterVoice->GetChannelMask(&channelMask);
        if (channelMask == 0) channelMask = 0x3;  // FL | FR

        HRESULT hr3d = X3DAudioInitialize(channelMask, X3DAUDIO_SPEED_OF_SOUND, m_x3d);
        m_x3dReady = SUCCEEDED(hr3d);

        m_listener.OrientFront = {0.0f, 0.0f, 1.0f};
        m_listener.OrientTop   = {0.0f, 1.0f, 0.0f};
        if (!m_x3dReady)
            Logger::Warn("X3DAudio の初期化に失敗したため、空間オーディオを無効化します");
    }

    ScanAudioFiles();

    // ストリーミングのデコード用ワーカー（デバイスが無ければ鳴らさないので起こさない）
    m_streamWorker.Start();

    Logger::Info("AudioSystem initialized (XAudio2{})", m_x3dReady ? " + X3DAudio" : "");
}

void AudioSystem::Shutdown()
{
    // ボイス破棄（BGM 含む）。★バスより先に壊す（送り先として使われているボイスは壊せない）。
    for (auto& v : m_voices)
        if (v.active) FreeVoice(v);
    m_bgmId = -1;
    m_currentBGMPath.clear();
    // ★ボイスを壊してからワーカーを止める（止める前にストリームはボイスから外れている）
    m_streamWorker.Stop();

    // バス破棄。★子から先に壊す（送り先として使われているボイスは壊せない）。
    //   m_buses は親が子より前に並ぶので逆順に回せば子が先になる。
    for (auto it = m_buses.rbegin(); it != m_buses.rend(); ++it)
    {
        if (it->voice)
        {
            it->voice->DestroyVoice();
            it->voice = nullptr;
        }
        it->appliedGain = it->appliedFilter = -1.0f;
    }

    // マスタリングボイス破棄
    if (m_masterVoice)
    {
        m_masterVoice->DestroyVoice();
        m_masterVoice = nullptr;
    }

    // クリップキャッシュクリア
    m_clipCache.clear();
    m_clipStamp.clear();

    // XAudio2エンジン解放
    m_xaudio2.Reset();

    if (m_comInitialized)
    {
        CoUninitialize();
        m_comInitialized = false;
    }

    Logger::Info("AudioSystem shutdown");
}

void AudioSystem::ScanAudioFiles()
{
    m_bgmList.clear();
    m_sfxList.clear();

    // ゲームモードでは loose な assets/ はディスクに無く(全部 pak 内)、この一覧は
    // エディタの音声ピッカ用。ディスク走査は無駄なうえ、AssetsDir(UTF-8)を
    // std::filesystem::path(=ACP 解釈)に通すと非 ASCII フォルダ名で例外になり得る。
    if (vfs::InGameMode())
    {
        // ★配布ゲームでは一覧が必ず空になる（pak の中身は列挙しない）。
        //   Lua の audio:getBGMList() / getSFXList() でサウンドテスト画面を作ると、
        //   エディタでは埋まるのに配布物では**無言で空リスト**になるので、理由を残す。
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Logger::Warn("配布ゲームでは audio:getBGMList()/getSFXList() は常に空です"
                         "（一覧はエディタのピッカ用で、pak の中身は列挙しません）。"
                         "曲名一覧が要るならスクリプト側に配列を持ってください");
        }
        return;
    }

    auto scanDir = [&](const std::string& subDir, std::vector<std::string>& outList) {
        std::filesystem::path dir = std::filesystem::path(m_assetsDir) / subDir;
        if (!std::filesystem::exists(dir)) return;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
        {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
            {
                // assetsDir 相対パスで格納
                auto relPath = std::filesystem::relative(entry.path(), m_assetsDir).string();
                std::replace(relPath.begin(), relPath.end(), '\\', '/');
                outList.push_back(relPath);
            }
        }
        std::sort(outList.begin(), outList.end());
    };

    scanDir("audio/bgm", m_bgmList);
    scanDir("audio/sfx", m_sfxList);

    Logger::Info("Audio scan: {} BGM, {} SFX found", m_bgmList.size(), m_sfxList.size());
}


std::shared_ptr<AudioClip> AudioSystem::GetOrLoadClip(const std::string& filePath, bool mono)
{
    if (mono)
    {
        // 元の（ステレオかもしれない）クリップを先に確保する。ここで更新時刻の検査も済む
        // （焼き直されていたらモノ版も一緒に捨てられている）。
        auto base = GetOrLoadClip(filePath, false);
        if (!base) return nullptr;
        if (base->GetFormat().nChannels == 1) return base;
        const std::string key = filePath + "|mono";
        auto mit = m_clipCache.find(key);
        if (mit != m_clipCache.end()) return mit->second;
        auto m = std::make_shared<AudioClip>(*base);
        m->DownmixToMono();
        Logger::Info("空間再生のため '{}' のモノラル版を作りました（元のステレオ版はそのまま）", filePath);
        m_clipCache[key] = m;
        return m;
    }

    // フルパス構築（相対パスならassetsDir基準）
    const bool isRelative = (filePath.size() < 2 || filePath[1] != ':');
    std::string fullPath = filePath;
    if (isRelative)
    {
        fullPath = m_assetsDir + filePath;
    }

    // ★★キャッシュチェック。ファイルが焼き直されていたら捨てて読み直す。
    //   ★以前は「一度読んだら二度と読み直さない」だったので、エディタを起動したまま
    //     素材を作り直しても古い音が鳴り続けた(直したはずの音が変わらない、の原因)。
    //   ★stat は 1 回の再生につき 1 回だけ。実測でも足音(毎秒 2 回)で問題にならない。
    //   ★キャッシュから外しても、鳴っている最中のボイスは shared_ptr で握っているので
    //     バッファは解放されない（以前は unique_ptr で、焼き直した瞬間に再生中の音の
    //     PCM が解放されていた）。
    std::error_code fec;
    const auto stamp = std::filesystem::last_write_time(fullPath, fec);
    auto it = m_clipCache.find(filePath);
    if (it != m_clipCache.end())
    {
        auto st = m_clipStamp.find(filePath);
        const bool fresh = fec || st == m_clipStamp.end() || st->second == stamp;
        if (fresh)
            return it->second;
        Logger::Info("音声が更新されたので読み直します: {}", filePath);
        m_clipCache.erase(it);
        m_clipCache.erase(filePath + "|mono");
        m_clipStamp.erase(filePath);
    }
    if (!fec) m_clipStamp[filePath] = stamp;

    // 拡張子抽出（VFS 経由ロード時に LoadFromMemory へ渡す）
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto clip = std::make_shared<AudioClip>();

    // VFS 経由でロード試行（ゲームモードは pak から復号展開、ディスクモードは空を返す）
    bool loaded = false;
    {
        std::vector<uint8_t> bytes;
        if (isRelative)
            bytes = vfs::ReadAsset(filePath);
        else
            bytes = vfs::ReadAssetAbs(fullPath);

        if (!bytes.empty())
            loaded = clip->LoadFromMemory(bytes.data(), bytes.size(), ext);
    }

    // VFS が空（エディタ / ディスクモード）→ ファイルから直接ロード
    if (!loaded)
        loaded = clip->LoadFromFile(fullPath);

    if (!loaded)
    {
        // 無言で消える音を可視化（pak ミス / 形式不明 / ファイル欠落）。
        Logger::Warn("オーディオの読み込みに失敗しました: '{}'（vfs/ディスク両方失敗, gameMode={}）",
                     filePath, vfs::InGameMode());
        return nullptr;
    }

    m_clipCache[filePath] = clip;
    return clip;
}

// ===========================================================================
// ボイス（論理ボイス = 1 回の再生。実ボイス = XAudio2 のソースボイスを持っているもの）
// ===========================================================================

// ★ID は (generation<<8)|index。スロットが別の音へ使い回されていたら世代が食い違うので、
//   古い ID で新しい音を止めてしまう事故が起きない。
i32 AudioSystem::MakeId(u32 index) const
{
    return static_cast<i32>((m_voices[index].generation << 8) | index);
}

AudioSystem::Voice* AudioSystem::Resolve(i32 id)
{
    return const_cast<Voice*>(static_cast<const AudioSystem*>(this)->Resolve(id));
}

const AudioSystem::Voice* AudioSystem::Resolve(i32 id) const
{
    if (id < 0) return nullptr;
    const u32 index = static_cast<u32>(id) & 0xFFu;
    const u32 gen   = static_cast<u32>(id) >> 8;
    if (index >= kMaxLogicalVoices) return nullptr;
    const Voice& v = m_voices[index];
    if (!v.active || v.generation != gen) return nullptr;
    return &v;
}

bool AudioSystem::BusInSubtree(i32 bus, i32 root) const
{
    for (i32 b = bus; b >= 0; b = m_buses[static_cast<size_t>(b)].parent)
        if (b == root) return true;
    return false;
}

void AudioSystem::UpdateBusChainGains()
{
    // 親が子より前に並んでいるので 1 パスで master からの積が出る
    for (auto& b : m_buses)
    {
        const f32 own = BusEffectiveGain(b);
        if (b.parent < 0)
        {
            b.chainGain   = own;
            b.sendChain   = 1.0f;   // master はリバーブの戻りにも掛かるので送りには含めない
            b.sendLowpass = 0.0f;
            continue;
        }
        const Bus& par = m_buses[static_cast<size_t>(b.parent)];
        b.chainGain   = own * par.chainGain;
        b.sendChain   = own * par.sendChain;
        b.sendLowpass = audio::CombineLowpass(audio::CombineLowpass(b.lowpassHz, b.snap.lowpassHz),
                                              par.sendLowpass);
    }
}

u32 AudioSystem::CountReal(i32 busRoot) const
{
    u32 n = 0;
    for (const auto& v : m_voices)
    {
        if (!v.active || !v.src || Unmanaged(v)) continue;
        if (busRoot >= 0 && !BusInSubtree(v.bus, busRoot)) continue;
        ++n;
    }
    return n;
}

void AudioSystem::UpdateDistance(Voice& v)
{
    if (!v.spatial) { v.distance = 0.0f; v.distGain = 1.0f; return; }
    const f32 dx = v.pos[0] - m_listener.Position.x;
    const f32 dy = v.pos[1] - m_listener.Position.y;
    const f32 dz = v.pos[2] - m_listener.Position.z;
    v.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    v.distGain = audio::DistanceGain(v.distance, v.minDist, v.maxDist);
}

// 遮蔽の効き。壁越しは「高域が落ちて音量も下がる」。数値は完全に趣味の範囲。
static constexpr float kOccVolumeDrop = 0.65f;    // 全遮蔽で音量 -65%
static constexpr float kOccCutoffHz   = 420.0f;   // 全遮蔽時のローパス
static constexpr float kOccSmooth     = 8.0f;     // 1/s。時定数 ~0.12s

f32 AudioSystem::ComputeAudibility(const Voice& v) const
{
    const f32 busGain = (v.bus >= 0) ? m_buses[static_cast<size_t>(v.bus)].chainGain : 1.0f;
    const f32 occ     = v.spatial ? (1.0f - kOccVolumeDrop * std::clamp(v.occ, 0.0f, 1.0f)) : 1.0f;
    return v.clipVolume * v.fade * v.distGain * occ * busGain;
}

f64 AudioSystem::CurrentFrame(const Voice& v) const
{
    if (v.totalFrames == 0) return 0.0;
    if (!v.src) return v.virtualPos;
    if (v.stream) return static_cast<f64>(v.stream->PositionFrames());
    XAUDIO2_VOICE_STATE st{};
    v.src->GetState(&st, 0);
    const u64 p = v.startFrame + st.SamplesPlayed;
    return static_cast<f64>(audio::WrapLoopPosition(p, LoopOf(v), v.totalFrames));
}

void AudioSystem::ApplyVoiceGain(Voice& v)
{
    if (!v.src) return;
    const f32 occ = std::clamp(v.occ, 0.0f, 1.0f);
    const f32 vol = v.clipVolume * v.fade * (v.spatial ? (1.0f - kOccVolumeDrop * occ) : 1.0f);
    if (std::fabs(vol - v.appliedVolume) > 1e-5f)
    {
        v.src->SetVolume(vol);
        v.appliedVolume = vol;
    }
    // ---- リバーブへの送り ----
    // 量 = バスの reverbSend × 音ごとの倍率 × バス音量の積（master 除く）× 距離。
    // ★距離は √（乾いた音より遅く減衰させる）。遠い音ほど響きの割合が増える＝距離感が出る。
    // ★バス音量を掛けるのは、sfx をミュートしたときに響きだけ残らないようにするため
    //   （送りはバスを経由しないので、掛けないとバスの操作が響きに効かない）。
    if (v.hasReverbSend && m_reverbBus >= 0)
    {
        IXAudio2Voice* rv = BusOutputVoice(m_reverbBus);
        const Bus& vb = m_buses[static_cast<size_t>(v.bus)];
        const f32 dist = v.spatial ? std::sqrt((std::max)(v.distGain, 0.0f)) : 1.0f;
        v.sendLevel = vb.reverbSend * v.reverbMul * vb.sendChain * dist;
        const u32 sc = v.channels, dc = m_reverbInCh;
        if (rv && sc >= 1 && sc <= 8 && std::fabs(v.sendLevel - v.appliedSend) > 1e-4f)
        {
            float m[16] = {};
            for (u32 d = 0; d < dc; ++d)
                for (u32 s = 0; s < sc; ++s)
                {
                    float lv = 0.0f;
                    if (dc == 1)      lv = v.sendLevel / static_cast<f32>(sc);   // モノの戻りへは平均
                    else if (sc == 1) lv = v.sendLevel;                          // モノ音源は左右両方へ
                    else              lv = (s % dc == d) ? v.sendLevel : 0.0f;   // ステレオはそのまま
                    m[sc * d + s] = lv;
                }
            v.src->SetOutputMatrix(rv, sc, dc, m);
            v.appliedSend = v.sendLevel;
        }
        const f32 sf = audio::CutoffToFilterFrequency(vb.sendLowpass, static_cast<f32>(m_reverbRate));
        if (rv && std::fabs(sf - v.appliedSendFilter) > 1e-5f)
        {
            XAUDIO2_FILTER_PARAMETERS fp{};
            fp.Type      = LowPassFilter;
            fp.Frequency = sf;
            fp.OneOverQ  = 1.0f;
            v.src->SetOutputFilterParameters(rv, &fp);
            v.appliedSendFilter = sf;
        }
    }

    if (!v.spatial) return;   // USEFILTER を付けていないボイスにフィルタは書けない
    // XAudio2 のフィルタ Frequency は 2*sin(pi*fc/fs)。1.0（＝上限）が素通し。
    const f32 fs   = static_cast<f32>(v.sampleRate > 0 ? v.sampleRate : 44100);
    const f32 shut = audio::CutoffToFilterFrequency(kOccCutoffHz, fs);
    const f32 freq = 1.0f + (shut - 1.0f) * occ;
    if (std::fabs(freq - v.appliedFilter) > 1e-5f)
    {
        XAUDIO2_FILTER_PARAMETERS fp{};
        fp.Type      = LowPassFilter;
        fp.Frequency = freq;
        fp.OneOverQ  = 1.0f;
        v.src->SetFilterParameters(&fp);
        v.appliedFilter = freq;
    }
}

void AudioSystem::ComputeAndApply(Voice& v)
{
    if (!m_x3dReady || !v.src || !v.spatial) return;
    IXAudio2Voice* dest = BusOutputVoice(v.bus);
    if (!dest) return;

    X3DAUDIO_EMITTER emitter{};
    emitter.Position            = {v.pos[0], v.pos[1], v.pos[2]};
    emitter.OrientFront         = {0.0f, 0.0f, 1.0f};
    emitter.OrientTop           = {0.0f, 1.0f, 0.0f};
    emitter.ChannelCount        = 1;
    emitter.CurveDistanceScaler = (v.maxDist > 0.01f) ? v.maxDist : 1.0f;

    // minDist までフル音量、maxDist で 0 になる線形カーブ（audio::DistanceGain と同じ）
    float minR = (v.maxDist > 0.01f) ? (v.minDist / v.maxDist) : 0.0f;
    minR = std::clamp(minR, 0.0f, 0.99f);
    X3DAUDIO_DISTANCE_CURVE_POINT pts[3] = { {0.0f, 1.0f}, {minR, 1.0f}, {1.0f, 0.0f} };
    X3DAUDIO_DISTANCE_CURVE curve{};
    curve.pPoints    = pts;
    curve.PointCount = 3;
    emitter.pVolumeCurve = &curve;

    float matrix[8] = {};
    X3DAUDIO_DSP_SETTINGS dsp{};
    dsp.SrcChannelCount     = 1;
    dsp.DstChannelCount     = m_outChannels;
    dsp.pMatrixCoefficients = matrix;

    X3DAudioCalculate(m_x3d, &m_listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
    // ★送り先を明示する（null は「送り先が 1 本だけ」のときしか使えない）
    v.src->SetOutputMatrix(dest, 1, m_outChannels, matrix);
}

bool AudioSystem::MakeReal(Voice& v, f64 startFrame)
{
    if (v.src) return true;
    if (!m_xaudio2 || !m_masterVoice || (!v.clip && !v.stream) || v.totalFrames == 0) return false;

    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);

    // ループ点があればその範囲へ畳む（ループ終点より後ろから鳴らし始めると XAudio2 が拒否する）
    u64 begin = (startFrame > 0.0) ? static_cast<u64>(startFrame) : 0u;
    begin = audio::WrapLoopPosition(begin, LoopOf(v), v.totalFrames);
    if (!v.stream && begin >= v.totalFrames) return false;   // ワンショットの末尾を過ぎている

    // ★ストリームはここではシークしない（呼び出し側が SeekTo 済み）。フォーマットはストリームから。
    WAVEFORMATEX fmt = v.stream ? v.stream->Format() : v.clip->GetFormat();
    // 送り先: 自分のバス + （BGM 以外は）リバーブの戻り。リバーブへの送りにはフィルタを付けて、
    // バスのローパス（スナップショットの「こもり」）を響きにも掛けられるようにする。
    XAUDIO2_SEND_DESCRIPTOR sd[2] = {};
    UINT32 sendCount = 0;
    sd[sendCount++] = {0, BusOutputVoice(v.bus)};
    IXAudio2Voice* reverbVoice = (m_reverbReady && !v.bgm) ? BusOutputVoice(m_reverbBus) : nullptr;
    if (reverbVoice) sd[sendCount++] = {XAUDIO2_SEND_USEFILTER, reverbVoice};
    XAUDIO2_VOICE_SENDS sends{sendCount, sd};
    // ★USEFILTER はボイス生成時にしか付けられない。遮蔽のローパスに要る。
    const UINT32 flags = v.spatial ? XAUDIO2_VOICE_USEFILTER : 0u;
    HRESULT hr = m_xaudio2->CreateSourceVoice(&v.src, &fmt, flags, XAUDIO2_DEFAULT_FREQ_RATIO,
                                              nullptr, sd[0].pOutputVoice ? &sends : nullptr);
    if (FAILED(hr))
    {
        Logger::Error("ソースボイス作成に失敗しました（{}）: 0x{:08X}", v.path, static_cast<u32>(hr));
        v.src = nullptr;
        return false;
    }

    if (v.stream)
    {
        // デコード済みのチャンクを渡すのは AudioStream::Pump（以降は Tick が毎フレーム呼ぶ）
        v.stream->AttachVoice(v.src);
        v.stream->Pump(v.paused);
    }
    else
    {
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = v.clip->GetSizeInBytes();
        buffer.pAudioData = v.clip->GetPCMData();
        buffer.Flags      = XAUDIO2_END_OF_STREAM;
        buffer.LoopCount  = v.loop ? XAUDIO2_LOOP_INFINITE : 0;
        // 途中から鳴らす（仮想から戻る / シーク）。ループは既定で末尾 → 先頭（LoopBegin=0, 全長）。
        buffer.PlayBegin  = static_cast<UINT32>(begin);
        if (v.loop && (v.loopStartFrame > 0 || v.loopEndFrame > 0))
        {
            // ループ点（イントロ付きの曲）: [start, end) を繰り返す
            const audio::StreamLoop lp = LoopOf(v);
            const u64 ls = audio::LoopStartOf(lp, v.totalFrames);
            const u64 le = audio::LoopEndOf(lp, v.totalFrames);
            buffer.LoopBegin  = static_cast<UINT32>(ls);
            buffer.LoopLength = static_cast<UINT32>(le - ls);
        }
        hr = v.src->SubmitSourceBuffer(&buffer);
        if (FAILED(hr))
        {
            Logger::Error("バッファ送信に失敗しました（{}）: 0x{:08X}", v.path, static_cast<u32>(hr));
            v.src->DestroyVoice();
            v.src = nullptr;
            return false;
        }
    }

    v.startFrame    = begin;
    v.virtualPos    = static_cast<f64>(begin);
    v.appliedVolume = -1.0f;
    v.appliedFilter = -1.0f;
    v.appliedSend   = -1.0f;
    v.appliedSendFilter = -1.0f;
    v.hasReverbSend = (reverbVoice != nullptr);
    v.virtualReason = "";
    v.src->SetFrequencyRatio(v.pitch);
    ApplyVoiceGain(v);
    ComputeAndApply(v);
    if (!v.paused) v.src->Start();

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    m_voiceChurnMs += 1000.0 * static_cast<f64>(t1.QuadPart - t0.QuadPart) / static_cast<f64>(freq.QuadPart);
    ++m_voiceChurnCount;
    return true;
}

void AudioSystem::MakeVirtual(Voice& v, const char* reason)
{
    if (v.stream) return;   // ストリームは仮想化しない（続きから鳴らすにはデコードし直しが要る）
    v.virtualReason = reason;
    if (!v.src) return;
    v.virtualPos = CurrentFrame(v);   // ★壊す前に位置を控える（戻ったときに続きから鳴らす）
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);
    v.src->DestroyVoice();
    v.src = nullptr;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    m_voiceChurnMs += 1000.0 * static_cast<f64>(t1.QuadPart - t0.QuadPart) / static_cast<f64>(freq.QuadPart);
    ++m_voiceChurnCount;
}

void AudioSystem::FreeVoice(Voice& v)
{
    // ★ストリームはボイスを壊す前に外す（ワーカーの代わり渡しが壊れたボイスへ触らないように）
    if (v.stream) v.stream->DetachVoice();
    if (v.src)
    {
        v.src->DestroyVoice();
        v.src = nullptr;
    }
    if (v.stream)
    {
        m_streamWorker.Remove(v.stream.get());
        v.stream.reset();
    }
    if (v.bgm)
    {
        const Voice* cur = Resolve(m_bgmId);
        if (cur == &v)
        {
            m_bgmId = -1;
            m_currentBGMPath.clear();
        }
    }
    v.active = false;
    v.clip.reset();
    v.path.clear();
    v.bgm = false;
    v.paused = false;
    v.stopAtFadeEnd = false;
}

i32 AudioSystem::AllocateSlot(i32 newPriority, f32 newAudibility)
{
    for (u32 i = 0; i < kMaxLogicalVoices; ++i)
        if (!m_voices[i].active) return static_cast<i32>(i);

    // 論理ボイスも満杯: 仮想ボイスから先に奪う（鳴っていない分だけ失うものが少ない）。
    for (int pass = 0; pass < 2; ++pass)
    {
        std::vector<audio::VoiceCandidate> c;
        c.reserve(kMaxLogicalVoices);
        for (u32 i = 0; i < kMaxLogicalVoices; ++i)
        {
            const Voice& v = m_voices[i];
            if (Unmanaged(v)) continue;
            if (pass == 0 && v.src) continue;
            c.push_back({static_cast<int>(i), v.priority, v.audibility, v.order});
        }
        const int k = audio::PickVictim(c.data(), c.size(), newPriority, newAudibility);
        if (k >= 0)
        {
            Voice& w = m_voices[static_cast<size_t>(c[static_cast<size_t>(k)].slot)];
            FreeVoice(w);
            return c[static_cast<size_t>(k)].slot;
        }
    }
    return -1;
}

bool AudioSystem::HasRoomFor(const Voice& v, f32 margin, i32* victim)
{
    *victim = -1;
    // 一番狭い（近い）上限を探す。バスの上限を奪って空ければ全体の枠も 1 つ空く。
    i32 scope = -2;   // -2 = どこも溢れていない
    for (i32 b = v.bus; b >= 0; b = m_buses[static_cast<size_t>(b)].parent)
    {
        const u32 lim = m_buses[static_cast<size_t>(b)].voiceLimit;
        if (lim > 0 && CountReal(b) >= lim) { scope = b; break; }
    }
    if (scope == -2 && CountReal(-1) >= m_maxRealVoices) scope = -1;
    if (scope == -2) return true;

    std::vector<audio::VoiceCandidate> c;
    c.reserve(64);
    for (u32 i = 0; i < kMaxLogicalVoices; ++i)
    {
        const Voice& w = m_voices[i];
        if (!w.active || !w.src || Unmanaged(w) || &w == &v) continue;
        if (scope >= 0 && !BusInSubtree(w.bus, scope)) continue;
        c.push_back({static_cast<int>(i), w.priority, w.audibility, w.order});
    }
    const int k = audio::PickVictim(c.data(), c.size(), v.priority, v.audibility, margin);
    if (k >= 0) *victim = c[static_cast<size_t>(k)].slot;
    return false;
}

i32 AudioSystem::Play(const PlayParams& p)
{
    return PlayInternal(p, false);
}

namespace
{
bool IsOggPath(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".ogg";
}
} // namespace

std::shared_ptr<AudioStream> AudioSystem::OpenStream(const std::string& path)
{
    // ★配布ゲームでは assets は pak の中にしか無い。std::filesystem で存在確認すると必ず false に
    //   なるので、vfs で読む（ディスクモードでも同じ関数が assets のファイルを返す）。
    const bool isRelative = (path.size() < 2 || path[1] != ':');
    std::vector<uint8_t> bytes = isRelative ? vfs::ReadAsset(path) : vfs::ReadAssetAbs(path);
    if (bytes.empty())
    {
        Logger::Warn("ストリームを開けません: '{}'（vfs で読めない。gameMode={}）", path, vfs::InGameMode());
        return nullptr;
    }
    std::string err;
    auto s = AudioStream::Open(std::move(bytes), path, err);
    if (!s) Logger::Warn("ストリームを開けません: '{}'（{}）", path, err);
    return s;
}

i32 AudioSystem::PlayInternal(const PlayParams& p, bool bgm)
{
    if (p.path.empty()) return -1;
    // ストリーミングは OGG の 2D だけ（空間音はモノ化が要るのでメモリへ読む）。
    const bool wantStream = p.stream && !p.spatial && IsOggPath(p.path);
    if (p.stream && !wantStream)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Logger::Info("ストリーミングは .ogg の 2D 再生だけです。'{}' はメモリへ読んで鳴らします", p.path);
        }
    }
    std::shared_ptr<AudioStream> stream = wantStream ? OpenStream(p.path) : nullptr;
    std::shared_ptr<AudioClip> clip;
    if (!stream)
    {
        clip = GetOrLoadClip(p.path, p.spatial);
        if (!clip) return -1;
    }
    const WAVEFORMATEX fmt = stream ? stream->Format() : clip->GetFormat();
    if (fmt.nBlockAlign == 0 || fmt.nSamplesPerSec == 0) return -1;

    // 聞こえ具合を先に見積もる（論理ボイスが満杯のときの奪い合いに要る）
    Voice probe;
    probe.bus        = bgm ? FindBus("music") : ResolveBusOrSfx(p.bus);
    probe.clipVolume = std::clamp(p.volume, 0.0f, 1.0f);
    probe.spatial    = p.spatial;
    probe.minDist    = p.minDistance;
    probe.maxDist    = p.maxDistance;
    probe.pos[0] = p.pos[0]; probe.pos[1] = p.pos[1]; probe.pos[2] = p.pos[2];
    UpdateBusChainGains();
    UpdateDistance(probe);
    const f32 aud = ComputeAudibility(probe);
    const i32 prio = bgm ? 256 : std::clamp(p.priority, 0, 255);

    const i32 slot = AllocateSlot(prio, aud);
    if (slot < 0)
    {
        if (!m_limitWarned)
        {
            m_limitWarned = true;
            Logger::Warn("論理ボイス（{} 本）が全部こちらより大事な音で埋まっているため '{}' を鳴らしません"
                         "（priority を上げるか、ループ音を stopVoice してください）", kMaxLogicalVoices, p.path);
        }
        return -1;
    }

    Voice& v = m_voices[static_cast<size_t>(slot)];
    const u32 gen = (v.generation + 1u) & 0x7FFFFFu;
    v = Voice{};
    v.generation  = (gen == 0) ? 1u : gen;
    v.active      = true;
    v.order       = ++m_voiceOrder;
    v.path        = p.path;
    v.clip        = std::move(clip);
    v.stream      = std::move(stream);
    v.sampleRate  = fmt.nSamplesPerSec;
    v.channels    = fmt.nChannels;
    v.totalFrames = v.stream ? v.stream->TotalFrames() : v.clip->GetSizeInBytes() / fmt.nBlockAlign;
    // ループ点（秒 → フレーム）。指定が無く OGG にタグがあればタグを使う
    if (p.loopStart > 0.0f || p.loopEnd > 0.0f)
    {
        v.loopStartFrame = static_cast<u64>((std::max)(p.loopStart, 0.0f) * static_cast<f32>(fmt.nSamplesPerSec));
        v.loopEndFrame   = static_cast<u64>((std::max)(p.loopEnd, 0.0f) * static_cast<f32>(fmt.nSamplesPerSec));
    }
    else if (v.stream && v.stream->HasTaggedLoop())
    {
        const audio::StreamLoop tl = v.stream->GetLoop();
        v.loopStartFrame = tl.start;
        v.loopEndFrame   = tl.end;
    }
    if (p.fadeIn > 0.0f)
    {
        v.fade       = 0.0f;
        v.fadeTarget = 1.0f;
        v.fadeSpeed  = 1.0f / p.fadeIn;
    }
    v.bus         = probe.bus;
    v.priority    = prio;
    v.clipVolume  = probe.clipVolume;
    v.pitch       = std::clamp(p.pitch, 0.05f, 2.0f);
    v.loop        = p.loop;
    v.spatial     = p.spatial;
    v.bgm         = bgm;
    v.minDist     = p.minDistance;
    v.maxDist     = p.maxDistance;
    v.pos[0] = p.pos[0]; v.pos[1] = p.pos[1]; v.pos[2] = p.pos[2];
    v.reverbMul   = std::clamp(p.reverb, 0.0f, 4.0f);
    v.distance    = probe.distance;
    v.distGain    = probe.distGain;
    v.audibility  = aud;
    const i32 id = MakeId(static_cast<u32>(slot));

    if (!IsDeviceReady())
    {
        v.virtualReason = "noDevice";   // 音は出ないが位置は進める＝状態は普段どおり追える
        return id;
    }
    if (v.stream)
    {
        // 鳴り出しが空かないよう 2 チャンク（約 0.7 秒）だけここでデコードし、残りはワーカーへ
        v.stream->SetLoop(v.loop, v.loopStartFrame, v.loopEndFrame);
        v.stream->DecodeAvailable(2);
        m_streamWorker.Add(v.stream);
    }
    if (bgm || v.stream)
    {
        if (!MakeReal(v, 0.0)) { FreeVoice(v); return -1; }
        return id;
    }
    if (audio::NextVirtualState(false, aud))
    {
        v.virtualReason = "inaudible";  // 遠い / バスがミュート。近づいたら続きから鳴る
        return id;
    }
    i32 victim = -1;
    if (!HasRoomFor(v, 1.0f, &victim))
    {
        if (victim < 0)
        {
            if (v.loop) { v.virtualReason = "limit"; return id; }   // 枠が空いたら鳴り始める
            if (!m_limitWarned)
            {
                m_limitWarned = true;
                Logger::Warn("ボイス上限（全体 {} 本 / バスごとの上限）に達していて、鳴っている音が全部 "
                             "'{}' より大事なので鳴らしません（以降このログは出しません）",
                             m_maxRealVoices, p.path);
            }
            FreeVoice(v);
            return -1;
        }
        Voice& w = m_voices[static_cast<size_t>(victim)];
        if (w.loop) MakeVirtual(w, "stolen");   // ループ音は止めずに仮想へ（枠が空いたら戻る）
        else        FreeVoice(w);
    }
    if (!MakeReal(v, 0.0)) { FreeVoice(v); return -1; }
    return id;
}

// ===== BGM =====

void AudioSystem::PlayBGM(const std::string& filePath, bool loop, f32 fadeSec)
{
    if (Voice* cur = Resolve(m_bgmId))
    {
        if (fadeSec > 0.0f)
        {
            // クロスフェード: 今の曲はフェードアウトしてから止まる（BGM 扱いのまま＝奪われない）
            cur->fadeTarget    = 0.0f;
            cur->fadeSpeed     = (std::max)(cur->fade, 0.001f) / fadeSec;
            cur->stopAtFadeEnd = true;
        }
        else
        {
            FreeVoice(*cur);
        }
    }
    m_bgmId = -1;
    m_currentBGMPath.clear();

    PlayParams p;
    p.path   = filePath;
    p.loop   = loop;
    p.stream = IsOggPath(filePath);   // ★OGG の BGM は丸ごとデコードせずストリーミング
    p.fadeIn = fadeSec;
    const i32 id = PlayInternal(p, true);
    if (id < 0) return;
    m_bgmId = id;
    m_currentBGMPath = filePath;
    const Voice* v = Resolve(id);
    Logger::Info("BGM playing: {} (loop={}{}{})", filePath, loop,
                 (v && v->stream) ? ", stream" : "", IsDeviceReady() ? "" : ", no device");
}

void AudioSystem::SetBGMLoopPoints(f32 startSec, f32 endSec)
{
    Voice* v = Resolve(m_bgmId);
    if (!v) return;
    const f32 sr = static_cast<f32>(v->sampleRate);
    v->loopStartFrame = static_cast<u64>((std::max)(startSec, 0.0f) * sr);
    v->loopEndFrame   = static_cast<u64>((std::max)(endSec, 0.0f) * sr);
    if (v->stream)
    {
        v->stream->SetLoop(v->loop, v->loopStartFrame, v->loopEndFrame);   // 先読みの後から効く
    }
    else if (v->src)
    {
        // メモリ上のクリップはループ範囲をバッファに書くので、今の位置から出し直す
        RestartVoiceAt(*v, CurrentFrame(*v));
    }
}

void AudioSystem::FadeVoice(i32 slotId, f32 target, f32 sec)
{
    Voice* v = Resolve(slotId);
    if (!v) return;
    v->fadeTarget    = std::clamp(target, 0.0f, 1.0f);
    v->stopAtFadeEnd = false;
    if (sec <= 0.0f) { v->fade = v->fadeTarget; v->fadeSpeed = 0.0f; ApplyVoiceGain(*v); return; }
    v->fadeSpeed = (std::max)(std::fabs(v->fadeTarget - v->fade), 0.001f) / sec;
}

AudioSystem::StreamStats AudioSystem::GetStreamStats() const
{
    StreamStats st;
    for (const auto& v : m_voices)
    {
        if (!v.active || !v.stream) continue;
        ++st.count;
        st.memoryBytes   += v.stream->MemoryBytes();
        st.underruns     += v.stream->Underruns();
        st.decodeMs      += v.stream->DecodeMs();
        st.chunksDecoded += v.stream->ChunksDecoded();
        st.watchdogPumps += v.stream->WatchdogPumps();
        st.audioSecondsDecoded += static_cast<f64>(v.stream->ChunksDecoded()) * AudioStream::kChunkFrames
                                  / static_cast<f64>((std::max)(v.sampleRate, 1u));
    }
    return st;
}

bool AudioSystem::IsBGMPlaying() const
{
    const Voice* v = Resolve(m_bgmId);
    return v && !v->paused;
}

void AudioSystem::RestartVoiceAt(Voice& v, f64 frame)
{
    if (v.stream)
    {
        // ★ボイスを外して壊してから巻き戻す（渡し済みのチャンクを捨てるため）
        v.stream->DetachVoice();
        if (v.src) { v.src->DestroyVoice(); v.src = nullptr; }
        v.stream->SeekTo(static_cast<u64>((std::max)(frame, 0.0)));
        v.virtualPos = frame;
        if (IsDeviceReady())
        {
            v.stream->DecodeAvailable(2);
            if (!MakeReal(v, frame)) v.virtualReason = "noDevice";
        }
        return;
    }
    if (v.src)
    {
        v.src->DestroyVoice();
        v.src = nullptr;
        if (!MakeReal(v, frame)) v.virtualPos = frame;
    }
    else
    {
        v.virtualPos = frame;
    }
}

void AudioSystem::SeekBGM(f32 seconds)
{
    Voice* v = Resolve(m_bgmId);
    if (!v || v->totalFrames == 0) return;
    u64 frame = static_cast<u64>((std::max)(0.0f, seconds) * static_cast<f32>(v->sampleRate));
    frame %= v->totalFrames;   // ループ範囲内に丸める(負は呼び出し側で扱わない)
    RestartVoiceAt(*v, static_cast<f64>(frame));
}

void AudioSystem::SetBGMRate(f32 ratio)
{
    Voice* v = Resolve(m_bgmId);
    if (!v) return;
    // XAudio2 の既定 MaxFrequencyRatio は 2.0。下限は無音同然になる前に打ち切る
    v->pitch = std::clamp(ratio, 0.05f, 2.0f);
    if (v->src) v->src->SetFrequencyRatio(v->pitch);
}

void AudioSystem::StopBGM()
{
    if (Voice* v = Resolve(m_bgmId)) FreeVoice(*v);
    m_bgmId = -1;
    m_currentBGMPath.clear();
}

void AudioSystem::PauseBGM()
{
    Voice* v = Resolve(m_bgmId);
    if (!v) return;
    v->paused = true;
    if (v->src) v->src->Stop();
}

void AudioSystem::ResumeBGM()
{
    Voice* v = Resolve(m_bgmId);
    if (!v) return;
    v->paused = false;
    if (v->src) v->src->Start();
}

// ===== SFX =====

void AudioSystem::PlaySFX(const std::string& filePath, bool loop, float volume, const std::string& bus)
{
    PlaySFXTracked(filePath, loop, volume, bus);
}

i32 AudioSystem::PlaySFXTracked(const std::string& filePath, bool loop, float volume,
                                const std::string& bus, i32 priority)
{
    PlayParams p;
    p.path     = filePath;
    p.loop     = loop;
    p.volume   = volume;
    p.bus      = bus;
    p.priority = priority;
    return Play(p);
}

i32 AudioSystem::PlaySFXSpatial(const std::string& filePath, float x, float y, float z,
                                float minDistance, float maxDistance, float volume, bool loop,
                                const std::string& bus, i32 priority)
{
    PlayParams p;
    p.path        = filePath;
    p.loop        = loop;
    p.volume      = volume;
    p.bus         = bus;
    p.priority    = priority;
    p.spatial     = true;
    p.pos[0] = x; p.pos[1] = y; p.pos[2] = z;
    p.minDistance = minDistance;
    p.maxDistance = maxDistance;
    return Play(p);
}

void AudioSystem::StopVoice(i32 slotId, f32 fadeSec)
{
    Voice* v = Resolve(slotId);
    if (!v) return;
    if (fadeSec > 0.0f)
    {
        v->fadeTarget    = 0.0f;
        v->fadeSpeed     = (std::max)(v->fade, 0.001f) / fadeSec;
        v->stopAtFadeEnd = true;
        return;
    }
    FreeVoice(*v);
}

void AudioSystem::SetVoiceVolume(i32 slotId, float volume)
{
    Voice* v = Resolve(slotId);
    if (!v) return;
    v->clipVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyVoiceGain(*v);
}

void AudioSystem::SetVoicePitch(i32 slotId, float ratio)
{
    Voice* v = Resolve(slotId);
    if (!v) return;
    v->pitch = std::clamp(ratio, 0.1f, 2.0f);
    if (v->src) v->src->SetFrequencyRatio(v->pitch);
}

void AudioSystem::SetVoicePriority(i32 slotId, i32 priority)
{
    Voice* v = Resolve(slotId);
    if (!v || v->bgm) return;
    v->priority = std::clamp(priority, 0, 255);
}

bool AudioSystem::IsVoicePlaying(i32 slotId) const
{
    return Resolve(slotId) != nullptr;
}

void AudioSystem::StopAllSFX()
{
    for (auto& v : m_voices)
        if (v.active && !v.bgm) FreeVoice(v);
}

// ===== 3D 空間オーディオ =====

void AudioSystem::SetListener(float px, float py, float pz,
                              float fx, float fy, float fz,
                              float ux, float uy, float uz)
{
    // Lua がプレイヤー位置を指定している間は、カメラ由来の位置より優先する(向きはカメラのまま)
    if (m_listenerPosOverride)
    {
        px = m_lopX; py = m_lopY; pz = m_lopZ;
    }
    m_listener.Position    = {px, py, pz};
    m_listener.OrientFront = {fx, fy, fz};
    m_listener.OrientTop   = {ux, uy, uz};
}

void AudioSystem::SetListenerPos(float x, float y, float z)
{
    m_listenerPosOverride = true;
    m_lopX = x; m_lopY = y; m_lopZ = z;
}

void AudioSystem::SetOcclusion(i32 slotId, float amount)
{
    Voice* v = Resolve(slotId);
    if (!v || !v->spatial) return;   // 世代違い（使い回された後のスロット）は触らない
    v->occTarget = std::clamp(amount, 0.0f, 1.0f);
}

void AudioSystem::GetListenerPos(float& x, float& y, float& z) const
{
    x = m_listener.Position.x;
    y = m_listener.Position.y;
    z = m_listener.Position.z;
}

void AudioSystem::UpdateSpatialEmitter(i32 slotId, float x, float y, float z)
{
    Voice* v = Resolve(slotId);
    // ★世代が違う＝このスロットは既に別の音へ使い回されている。触らない。
    if (!v || !v->spatial) return;
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = z;
}

void AudioSystem::Update(f32 dt)
{
    const float k = std::clamp(dt * kOccSmooth, 0.0f, 1.0f);
    for (auto& v : m_voices)
    {
        if (!v.active || !v.spatial) continue;
        v.occ += (v.occTarget - v.occ) * k;
        UpdateDistance(v);
        ComputeAndApply(v);
    }
}

void AudioSystem::Tick(f32 dt)
{
    if (dt < 0.0f) dt = 0.0f;
    UpdateSnapshot(dt);        // バスの補正が変わるので、バスの積より先に
    UpdateReverb(dt);          // 響きの量が変わるので、バスの積より先に
    UpdateBusChainGains();

    // 1) 終了検出・仮想ボイスの位置送り・フェード
    for (auto& v : m_voices)
    {
        if (!v.active) continue;
        if (v.src && v.stream)
        {
            // ストリーム: デコード済みを渡すだけ（デコードはワーカー）。曲が終わって鳴らし切ったら解放
            if (v.stream->Pump(v.paused)) { FreeVoice(v); continue; }
        }
        else if (v.src)
        {
            XAUDIO2_VOICE_STATE st{};
            v.src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued == 0 && !v.paused) { FreeVoice(v); continue; }
        }
        else if (!v.paused && v.totalFrames > 0)
        {
            v.virtualPos += static_cast<f64>(dt) * v.sampleRate * v.pitch;
            if (v.virtualPos >= static_cast<f64>(v.totalFrames) && !v.loop) { FreeVoice(v); continue; }
            // ループ点つきはその範囲で回す（仮想から戻ったとき正しい位置から鳴る）
            const f64 frac = v.virtualPos - std::floor(v.virtualPos);
            v.virtualPos = static_cast<f64>(audio::WrapLoopPosition(static_cast<u64>(v.virtualPos),
                                                                    LoopOf(v), v.totalFrames)) + frac;
        }
        if (v.fadeSpeed > 0.0f)
        {
            const f32 step = v.fadeSpeed * dt;
            if (v.fade < v.fadeTarget) v.fade = (std::min)(v.fade + step, v.fadeTarget);
            else                       v.fade = (std::max)(v.fade - step, v.fadeTarget);
            if (v.fade == v.fadeTarget)
            {
                v.fadeSpeed = 0.0f;
                if (v.stopAtFadeEnd && v.fade <= 0.0f) { FreeVoice(v); continue; }
            }
        }
        v.audibility = ComputeAudibility(v);
    }

    if (!IsDeviceReady()) return;

    // 2) 実 → 仮想: 聞こえなくなった音はボイスを手放す（BGM は対象外）
    for (auto& v : m_voices)
        if (v.active && v.src && !Unmanaged(v) && audio::NextVirtualState(false, v.audibility))
            MakeVirtual(v, "inaudible");

    // 3) 仮想 → 実: 聞こえるようになった音を、大事な順に 1 フレーム最大 4 本まで戻す
    //   （一度に大量に作ると 1 フレームだけ重くなる。4 本/フレームでも 60fps なら 0.25 秒で 60 本）
    std::vector<u32> wake;
    for (u32 i = 0; i < kMaxLogicalVoices; ++i)
    {
        const Voice& v = m_voices[i];
        if (v.active && !v.src && !v.paused && !v.stream && !audio::NextVirtualState(true, v.audibility))
            wake.push_back(i);
    }
    std::sort(wake.begin(), wake.end(), [&](u32 a, u32 b) {
        const Voice& va = m_voices[a];
        const Voice& vb = m_voices[b];
        if (va.priority != vb.priority) return va.priority > vb.priority;
        return va.audibility > vb.audibility;
    });
    int budget = 4;
    for (u32 i : wake)
    {
        if (budget <= 0) break;
        Voice& v = m_voices[i];
        if (!v.active || v.src) continue;   // 上で奪われた等
        i32 victim = -1;
        // ★同じ優先度で奪い返すには 2 倍大きく聞こえている必要がある（奪い合いのちらつき防止）
        if (!Unmanaged(v) && !HasRoomFor(v, 2.0f, &victim))
        {
            if (victim < 0) { v.virtualReason = "limit"; continue; }
            Voice& w = m_voices[static_cast<size_t>(victim)];
            if (w.loop) MakeVirtual(w, "stolen");
            else        FreeVoice(w);
        }
        MakeReal(v, v.virtualPos);
        --budget;
    }

    // 4) 実ボイスへ音量・遮蔽を反映（値が変わったときだけ XAudio2 を呼ぶ）
    for (auto& v : m_voices)
        if (v.active && v.src) ApplyVoiceGain(v);

    // 5) メーター
    UpdateMeters(dt);
}

void AudioSystem::UpdateMeters(f32 dt)
{
    for (auto& b : m_buses)
    {
        f32 peak = 0.0f, rms = 0.0f;
        if (b.voice && b.hasMeter && b.meterChannels > 0 && b.meterChannels <= 8)
        {
            float peaks[8] = {}, rmss[8] = {};
            XAUDIO2FX_VOLUMEMETER_LEVELS lv{};
            lv.pPeakLevels  = peaks;
            lv.pRMSLevels   = rmss;
            lv.ChannelCount = b.meterChannels;
            if (SUCCEEDED(b.voice->GetEffectParameters(b.meterIndex, &lv, sizeof(lv))))
            {
                f32 ss = 0.0f;
                for (u32 c = 0; c < b.meterChannels; ++c)
                {
                    peak = (std::max)(peak, peaks[c]);
                    ss  += rmss[c] * rmss[c];
                }
                rms = std::sqrt(ss / static_cast<f32>(b.meterChannels));
            }
        }
        audio::MeterStep(b.meter, peak, rms, dt);
    }
}

bool AudioSystem::GetBusLevel(const std::string& name, f32& peakDb, f32& rmsDb) const
{
    const i32 i = FindBus(name);
    if (i < 0) { peakDb = rmsDb = audio::kSilenceDb; return false; }
    const auto& m = m_buses[static_cast<size_t>(i)].meter;
    peakDb = audio::MeterPeakDb(m);
    rmsDb  = audio::MeterRmsDb(m);
    return true;
}

// ===== ボイス上限 / 読み出し =====

void AudioSystem::SetMaxVoices(u32 n)
{
    m_maxRealVoices = std::clamp<u32>(n, 1u, kMaxLogicalVoices);
    // 減らした分は次の Tick で順に仮想へ落ちるのではなく、今ここで弱い順に落とす
    while (CountReal(-1) > m_maxRealVoices)
    {
        std::vector<audio::VoiceCandidate> c;
        for (u32 i = 0; i < kMaxLogicalVoices; ++i)
        {
            const Voice& w = m_voices[i];
            if (w.active && w.src && !Unmanaged(w))
                c.push_back({static_cast<int>(i), w.priority, w.audibility, w.order});
        }
        const int k = audio::PickVictim(c.data(), c.size(), 1 << 20, 1e9f);
        if (k < 0) break;
        Voice& w = m_voices[static_cast<size_t>(c[static_cast<size_t>(k)].slot)];
        if (w.loop) MakeVirtual(w, "limit");
        else        FreeVoice(w);
    }
}

void AudioSystem::SetBusVoiceLimit(const std::string& bus, u32 n)
{
    const i32 i = FindBus(bus);
    if (i < 0) { Logger::Warn("setBusVoiceLimit: バス '{}' がありません", bus); return; }
    m_buses[static_cast<size_t>(i)].voiceLimit = (std::min)(n, kMaxLogicalVoices);
}

u32 AudioSystem::GetBusVoiceLimit(const std::string& bus) const
{
    const i32 i = FindBus(bus);
    return (i < 0) ? 0u : m_buses[static_cast<size_t>(i)].voiceLimit;
}

void AudioSystem::GetVoiceCounts(u32& real, u32& virt) const
{
    real = virt = 0;
    for (const auto& v : m_voices)
    {
        if (!v.active) continue;
        if (v.src) ++real; else ++virt;
    }
}

std::vector<AudioSystem::VoiceInfo> AudioSystem::GetVoices() const
{
    std::vector<VoiceInfo> out;
    for (u32 i = 0; i < kMaxLogicalVoices; ++i)
    {
        const Voice& v = m_voices[i];
        if (!v.active) continue;
        VoiceInfo vi;
        vi.id           = MakeId(i);
        vi.path         = v.path;
        vi.bus          = (v.bus >= 0) ? m_buses[static_cast<size_t>(v.bus)].name : std::string();
        vi.priority     = v.priority;
        vi.volume       = v.clipVolume;
        vi.fade         = v.fade;
        vi.pitch        = v.pitch;
        vi.audibility   = v.audibility;
        vi.spatial      = v.spatial;
        vi.distance     = v.distance;
        vi.distanceGain = v.distGain;
        vi.occlusion    = v.occ;
        vi.loop         = v.loop;
        vi.isVirtual    = (v.src == nullptr);
        vi.virtualReason = vi.isVirtual ? v.virtualReason : "";
        vi.bgm          = v.bgm;
        vi.paused       = v.paused;
        const f32 sr    = static_cast<f32>(v.sampleRate > 0 ? v.sampleRate : 44100);
        vi.positionSec  = static_cast<f32>(CurrentFrame(v)) / sr;
        vi.lengthSec    = static_cast<f32>(v.totalFrames) / sr;
        vi.reverbSend   = v.hasReverbSend ? v.sendLevel : 0.0f;
        vi.stream       = (v.stream != nullptr);
        if (v.stream)
        {
            vi.bufferedChunks = v.stream->BufferedChunks();
            vi.underruns      = v.stream->Underruns();
        }
        vi.loopStartSec = static_cast<f32>(v.loopStartFrame) / sr;
        vi.loopEndSec   = static_cast<f32>(v.loopEndFrame) / sr;
        out.push_back(std::move(vi));
    }
    return out;
}

// ===== バス（サブミックス）=====
// ★以前の setSFXVolume は「鳴っている全ボイスへ音量を掛け直す」実装で、クリップ個別音量や
//   遮蔽の掛け忘れで何度も事故っていた（小さい音が最大音量へ跳ねる / こもった音が一瞬素に戻る）。
//   今はバスのサブミックスボイス 1 本の音量を変えるだけなので、ボイス側の値には一切触らない。

i32 AudioSystem::FindBus(const std::string& name) const
{
    for (size_t i = 0; i < m_buses.size(); ++i)
        if (m_buses[i].name == name) return static_cast<i32>(i);
    return -1;
}

i32 AudioSystem::ResolveBusOrSfx(const std::string& name)
{
    if (name.empty()) return FindBus("sfx");
    const i32 i = FindBus(name);
    if (i >= 0 && m_buses[static_cast<size_t>(i)].isReverb)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Logger::Warn("reverb はリバーブの戻りバスなので音を直接流せません。sfx で鳴らします"
                         "（響かせたいなら play{{reverb=1}} や setBusReverbSend で送り量を決めてください）");
        }
        return FindBus("sfx");
    }
    if (i >= 0) return i;
    // 未知のバス名で無音にすると「鳴らない」の原因が分からなくなるので、sfx へ流して 1 度だけ警告する。
    if (std::find(m_warnedUnknownBus.begin(), m_warnedUnknownBus.end(), name) == m_warnedUnknownBus.end())
    {
        m_warnedUnknownBus.push_back(name);
        Logger::Warn("バス '{}' がありません。sfx バスで鳴らします（audio:createBus('{}') で作るか、"
                     "既定の master/music/sfx/ambience/voice/ui のどれかを指定してください）", name, name);
    }
    return FindBus("sfx");
}

IXAudio2Voice* AudioSystem::BusOutputVoice(i32 busIndex) const
{
    if (busIndex < 0 || busIndex >= static_cast<i32>(m_buses.size())) return nullptr;
    return m_buses[static_cast<size_t>(busIndex)].voice;
}

f32 AudioSystem::BusEffectiveGain(const Bus& bus) const
{
    if (bus.muted) return 0.0f;
    const f32 g = bus.volume * bus.snap.gain;
    // リバーブの戻りは「今の響きの量（ゾーンから補間した wet）」も掛ける
    return bus.isReverb ? g * m_reverbWetCur : g;
}

bool AudioSystem::CreateBusVoice(Bus& bus)
{
    if (bus.voice) return true;
    if (!m_xaudio2 || !m_masterVoice) return false;
    if (bus.isReverb) return CreateReverbVoice(bus);

    IXAudio2Voice* parentVoice = nullptr;
    if (bus.parent >= 0)
    {
        parentVoice = m_buses[static_cast<size_t>(bus.parent)].voice;
        if (!parentVoice) return false;   // 親が作れていない（デバイス無し等）
    }
    OneSend send(parentVoice);   // master は null → 既定の mastering voice へ
    // メーター（VolumeMeter APO）。★サブミックスの SetVolume はエフェクトより前に掛かるので、
    //   これはフェーダー後の値＝ミュートすれば下がる（公式: 「submix の volume は filter と
    //   effect chain の直前に掛かる」）。作れなくても鳴らすことはできるので失敗は警告だけ。
    IUnknown* meterApo = nullptr;
    XAUDIO2_EFFECT_DESCRIPTOR md{};
    XAUDIO2_EFFECT_CHAIN mchain{0, &md};
    if (SUCCEEDED(XAudio2CreateVolumeMeter(&meterApo)) && meterApo)
    {
        md.pEffect        = meterApo;
        md.InitialState   = TRUE;
        md.OutputChannels = m_outChannels;
        mchain.EffectCount = 1;
    }
    // ★入力チャンネル数は出力デバイスと同じにする。空間音の X3DAudio の行列が
    //   「ボイス → バス」をそのままスピーカー配置で書けるようにするため。
    const UINT32 stage = kMasterStage - bus.depth * 16u;
    HRESULT hr = m_xaudio2->CreateSubmixVoice(&bus.voice, m_outChannels, m_outSampleRate,
                                              XAUDIO2_VOICE_USEFILTER, stage, send.Get(),
                                              mchain.EffectCount ? &mchain : nullptr);
    if (meterApo) meterApo->Release();   // ボイスが参照を持つ
    bus.hasMeter      = SUCCEEDED(hr) && mchain.EffectCount > 0;
    bus.meterIndex    = 0;
    bus.meterChannels = m_outChannels;
    if (FAILED(hr))
    {
        Logger::Error("バス '{}' のサブミックスボイス作成に失敗しました: 0x{:08X}",
                      bus.name, static_cast<u32>(hr));
        bus.voice = nullptr;
        return false;
    }
    ApplyBus(bus, true);
    return true;
}

void AudioSystem::ApplyBus(Bus& bus, bool force)
{
    const f32 gain   = BusEffectiveGain(bus);
    const f32 hz     = audio::CombineLowpass(bus.lowpassHz, bus.snap.lowpassHz);
    const f32 filter = audio::CutoffToFilterFrequency(hz, static_cast<f32>(m_outSampleRate));
    if (!bus.voice)
    {
        bus.appliedGain = gain;
        bus.appliedFilter = filter;
        return;
    }
    if (force || std::fabs(gain - bus.appliedGain) > 1e-5f)
    {
        bus.voice->SetVolume(gain);
        bus.appliedGain = gain;
    }
    if (force || std::fabs(filter - bus.appliedFilter) > 1e-5f)
    {
        XAUDIO2_FILTER_PARAMETERS fp{};
        fp.Type      = LowPassFilter;
        fp.Frequency = filter;    // 1.0 = 素通し（Frequency=1, OneOverQ=1 はバイパスと等価）
        fp.OneOverQ  = 1.0f;
        bus.voice->SetFilterParameters(&fp);
        bus.appliedFilter = filter;
    }
}

bool AudioSystem::CreateBus(const std::string& name, const std::string& parent)
{
    if (name.empty())
    {
        Logger::Warn("audio:createBus: バス名が空です");
        return false;
    }
    if (FindBus(name) >= 0) return true;   // 既にある（Play のたびに OnStart で呼んでよい）

    const i32 p = FindBus(parent.empty() ? std::string("master") : parent);
    if (p < 0)
    {
        Logger::Warn("audio:createBus('{}'): 親バス '{}' がありません", name, parent);
        return false;
    }
    if (m_buses[static_cast<size_t>(p)].isReverb)
    {
        Logger::Warn("audio:createBus('{}'): reverb（リバーブの戻り）の下にはバスを作れません", name);
        return false;
    }
    const u32 depth = m_buses[static_cast<size_t>(p)].depth + 1u;
    if (depth > kMaxBusDepth)
    {
        Logger::Warn("audio:createBus('{}'): 入れ子が深すぎます（最大 {} 段）", name, kMaxBusDepth);
        return false;
    }
    Bus b;
    b.name       = name;
    b.parent     = p;
    b.depth      = depth;
    b.reverbSend = m_buses[static_cast<size_t>(p)].reverbSend;   // 親の送り量を引き継ぐ
    m_buses.push_back(std::move(b));
    // ★push_back の後で参照を取り直す（再確保で前の参照は無効）
    CreateBusVoice(m_buses.back());
    Logger::Info("バスを作成: {} (親 {})", name, m_buses[static_cast<size_t>(p)].name);
    return true;
}

void AudioSystem::SetBusVolume(const std::string& name, f32 volume)
{
    const i32 i = FindBus(name);
    if (i < 0) { Logger::Warn("setBusVolume: バス '{}' がありません", name); return; }
    auto& b = m_buses[static_cast<size_t>(i)];
    b.volume = std::clamp(volume, 0.0f, 4.0f);
    ApplyBus(b);
}

f32 AudioSystem::GetBusVolume(const std::string& name) const
{
    const i32 i = FindBus(name);
    return (i < 0) ? 0.0f : m_buses[static_cast<size_t>(i)].volume;
}

void AudioSystem::SetBusMute(const std::string& name, bool muted)
{
    const i32 i = FindBus(name);
    if (i < 0) { Logger::Warn("setBusMute: バス '{}' がありません", name); return; }
    auto& b = m_buses[static_cast<size_t>(i)];
    b.muted = muted;
    ApplyBus(b);
}

bool AudioSystem::IsBusMuted(const std::string& name) const
{
    const i32 i = FindBus(name);
    return (i >= 0) && m_buses[static_cast<size_t>(i)].muted;
}

void AudioSystem::SetBusLowpass(const std::string& name, f32 hz)
{
    const i32 i = FindBus(name);
    if (i < 0) { Logger::Warn("setBusLowpass: バス '{}' がありません", name); return; }
    auto& b = m_buses[static_cast<size_t>(i)];
    b.lowpassHz = (hz > 0.0f) ? std::clamp(hz, 20.0f, 24000.0f) : 0.0f;
    ApplyBus(b);
}

f32 AudioSystem::GetBusLowpass(const std::string& name) const
{
    const i32 i = FindBus(name);
    return (i < 0) ? 0.0f : m_buses[static_cast<size_t>(i)].lowpassHz;
}

void AudioSystem::SetBusReverbSend(const std::string& name, f32 send)
{
    const i32 i = FindBus(name);
    if (i < 0) { Logger::Warn("setBusReverbSend: バス '{}' がありません", name); return; }
    m_buses[static_cast<size_t>(i)].reverbSend = std::clamp(send, 0.0f, 1.0f);
}

f32 AudioSystem::GetBusReverbSend(const std::string& name) const
{
    const i32 i = FindBus(name);
    return (i < 0) ? 0.0f : m_buses[static_cast<size_t>(i)].reverbSend;
}

// ===== リバーブ =====
// XAudio2 の組み込みリバーブ（XAudio2CreateReverb）を 1 本のサブミックスに載せ、各ボイスから送る。
// ★入出力のチャンネル構成は「モノ→モノ / モノ→5.1 / ステレオ→ステレオ / ステレオ→5.1」しか
//   受け付けず、サンプルレートも 20k〜48kHz に限られる（xaudio2fx.h）。出力デバイスに合わせて選ぶ。

bool AudioSystem::CreateReverbVoice(Bus& bus)
{
    m_reverbReady = false;
    m_reverbInCh  = (m_outChannels == 1) ? 1u : 2u;
    m_reverbOutCh = (m_outChannels == 1) ? 1u : (m_outChannels >= 6 ? 6u : 2u);
    m_reverbRate  = std::clamp<u32>(m_outSampleRate, XAUDIO2FX_REVERB_MIN_FRAMERATE,
                                    XAUDIO2FX_REVERB_MAX_FRAMERATE);
    if (bus.parent < 0) return false;
    IXAudio2Voice* parentVoice = m_buses[static_cast<size_t>(bus.parent)].voice;
    if (!parentVoice) return false;

    IUnknown* apo = nullptr;
    HRESULT hr = XAudio2CreateReverb(&apo);
    if (FAILED(hr) || !apo)
    {
        Logger::Warn("リバーブの作成に失敗しました（響き無しで続けます）: 0x{:08X}", static_cast<u32>(hr));
        return false;
    }
    // チェーン: [リバーブ, メーター]。メーターは響きの出力（戻り）を測る
    XAUDIO2_EFFECT_DESCRIPTOR desc[2] = {};
    desc[0].pEffect        = apo;
    desc[0].InitialState   = TRUE;
    desc[0].OutputChannels = m_reverbOutCh;
    UINT32 effectCount = 1;
    IUnknown* meterApo = nullptr;
    if (SUCCEEDED(XAudio2CreateVolumeMeter(&meterApo)) && meterApo)
    {
        desc[1].pEffect        = meterApo;
        desc[1].InitialState   = TRUE;
        desc[1].OutputChannels = m_reverbOutCh;
        effectCount = 2;
    }
    XAUDIO2_EFFECT_CHAIN chain{effectCount, desc};
    OneSend send(parentVoice);
    const UINT32 stage = kMasterStage - 8u;   // master より前、どのバスより後（ソースからしか受けない）
    hr = m_xaudio2->CreateSubmixVoice(&bus.voice, m_reverbInCh, m_reverbRate, 0, stage,
                                      send.Get(), &chain);
    apo->Release();   // ボイスが参照を持つ
    if (meterApo) meterApo->Release();
    bus.hasMeter      = SUCCEEDED(hr) && effectCount == 2;
    bus.meterIndex    = 1;
    bus.meterChannels = m_reverbOutCh;
    if (FAILED(hr))
    {
        Logger::Warn("リバーブのサブミックス作成に失敗しました（響き無しで続けます）: 0x{:08X}",
                     static_cast<u32>(hr));
        bus.voice = nullptr;
        return false;
    }
    m_reverbReady = true;
    ApplyReverbParams(true);
    ApplyBus(bus, true);
    return true;
}

void AudioSystem::ApplyReverbParams(bool force)
{
    if (!m_reverbReady || m_reverbBus < 0) return;
    IXAudio2SubmixVoice* rv = m_buses[static_cast<size_t>(m_reverbBus)].voice;
    if (!rv) return;
    // ★毎フレーム書かない。聞き分けられない差（0.25 dB / 25ms 相当未満）は捨てる。
    if (!force && audio::ReverbDistance(m_reverbCur, m_reverbApplied) < 0.25f) return;

    const audio::ReverbParams& p = m_reverbCur;
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3{};
    i3.WetDryMix         = 100.0f;   // 送りバスなので 100% ウェット。量はバスの音量で決める
    i3.Room              = static_cast<INT32>(std::lround(p.room));
    i3.RoomHF            = static_cast<INT32>(std::lround(p.roomHF));
    i3.RoomRolloffFactor = p.roomRolloff;
    i3.DecayTime         = std::clamp(p.decayTime, 0.1f, 20.0f);
    i3.DecayHFRatio      = std::clamp(p.decayHFRatio, 0.1f, 2.0f);
    i3.Reflections       = static_cast<INT32>(std::lround(p.reflections));
    i3.ReflectionsDelay  = std::clamp(p.reflectionsDelay, 0.0f, 0.3f);
    i3.Reverb            = static_cast<INT32>(std::lround(p.reverb));
    i3.ReverbDelay       = std::clamp(p.reverbDelay, 0.0f, 0.1f);
    i3.Diffusion         = std::clamp(p.diffusion, 0.0f, 100.0f);
    i3.Density           = std::clamp(p.density, 0.0f, 100.0f);
    i3.HFReference       = std::clamp(p.hfReference, 20.0f, 20000.0f);
    XAUDIO2FX_REVERB_PARAMETERS native{};
    // ★7.1 用の後方ディレイ（既定 TRUE）は 5.1 以下の出力だと範囲外になるので FALSE で変換する
    ReverbConvertI3DL2ToNative(&i3, &native, FALSE);
    const HRESULT hr = rv->SetEffectParameters(0, &native, sizeof(native));
    if (SUCCEEDED(hr)) m_reverbApplied = m_reverbCur;
}

void AudioSystem::UpdateReverb(f32 dt)
{
    // 目標 = 既定の響き（ゾーンの外）を土台に、優先度の低い順にゾーンを重みで重ねたもの
    audio::ReverbParams base;
    if (!audio::FindReverbPreset(m_defaultReverbPreset.c_str(), base))
        audio::FindReverbPreset("none", base);

    std::vector<ReverbZoneInput> zones = m_reverbZones;
    std::stable_sort(zones.begin(), zones.end(),
                     [](const ReverbZoneInput& a, const ReverbZoneInput& b) { return a.priority < b.priority; });
    std::vector<audio::ZoneSample> samples;
    samples.reserve(zones.size());
    m_reverbDominant.clear();
    for (const auto& z : zones)
    {
        audio::ZoneSample s;
        s.priority = z.priority;
        s.weight   = z.weight;
        s.wet      = z.wet;
        if (!audio::FindReverbPreset(z.preset.c_str(), s.params))
            audio::FindReverbPreset("generic", s.params);   // 未知のプリセットは汎用で鳴らす（無音にしない）
        samples.push_back(s);
        if (z.weight > 0.0f) m_reverbDominant = z.name;     // 並びの最後 = 一番優先度が高い
    }
    const audio::ReverbMix mix = audio::BlendZones(samples.data(), samples.size(), base, m_defaultReverbWet);
    m_reverbTarget    = mix.params;
    m_reverbWetTarget = std::clamp(mix.wet, 0.0f, 1.0f);

    // 時間方向にもならす（ゾーンの境界を跨いだ瞬間に響きが切り替わらない）。
    // ★響きがほぼ無いところからは性格を即座に目標へ合わせる（量だけ時間で上げる）。
    const f32 k = audio::SmoothFactor(dt, 0.35f);
    if (m_reverbWetCur <= 1e-4f) m_reverbCur = m_reverbTarget;
    else                         m_reverbCur = audio::LerpReverb(m_reverbCur, m_reverbTarget, k);
    m_reverbWetCur = audio::Lerp(m_reverbWetCur, m_reverbWetTarget, k);
    if (std::fabs(m_reverbWetCur - m_reverbWetTarget) < 1e-4f) m_reverbWetCur = m_reverbWetTarget;

    ApplyReverbParams(false);
    if (m_reverbBus >= 0) ApplyBus(m_buses[static_cast<size_t>(m_reverbBus)]);
}

bool AudioSystem::SetDefaultReverb(const std::string& preset, f32 wet)
{
    audio::ReverbParams tmp;
    if (!audio::FindReverbPreset(preset.c_str(), tmp))
    {
        Logger::Warn("audio:setReverb: プリセット '{}' がありません（audio:getReverbPresets() で一覧）", preset);
        return false;
    }
    m_defaultReverbPreset = preset;
    m_defaultReverbWet    = std::clamp(wet, 0.0f, 1.0f);
    return true;
}

AudioSystem::ReverbState AudioSystem::GetReverbState() const
{
    ReverbState st;
    st.available     = m_reverbReady;
    st.defaultPreset = m_defaultReverbPreset;
    st.defaultWet    = m_defaultReverbWet;
    for (const auto& z : m_reverbZones)
        if (z.weight > 0.0f) st.zones.push_back(z);
    st.dominant   = m_reverbDominant;
    st.targetWet  = m_reverbWetTarget;
    st.currentWet = m_reverbWetCur;
    st.current    = m_reverbCur;
    return st;
}

void AudioSystem::ResetMixForStop()
{
    m_reverbZones.clear();
    m_defaultReverbPreset = "none";
    m_defaultReverbWet    = 0.0f;
    // スナップショットは即座に default（遷移させない。エディタへ戻った瞬間から素の音で聞く）
    SetSnapshot("default", 0.0f);
    UpdateSnapshot(0.0f);
}

// ===== スナップショット =====

bool AudioSystem::DefineSnapshot(const std::string& name, std::vector<SnapshotBus> buses)
{
    if (name.empty() || name == "default")
    {
        Logger::Warn("audio:defineSnapshot: '{}' は定義できません（default は全バス補正なしで固定）", name);
        return false;
    }
    for (auto& b : buses)
    {
        b.mod.gain      = std::clamp(b.mod.gain, 0.0f, 4.0f);
        b.mod.lowpassHz = (b.mod.lowpassHz > 0.0f) ? std::clamp(b.mod.lowpassHz, 20.0f, 24000.0f) : 0.0f;
    }
    m_snapshots[name] = std::move(buses);
    return true;
}

bool AudioSystem::SetSnapshot(const std::string& name, f32 seconds)
{
    const std::vector<SnapshotBus>* def = nullptr;
    if (name != "default")
    {
        auto it = m_snapshots.find(name);
        if (it == m_snapshots.end())
        {
            Logger::Warn("audio:setSnapshot: スナップショット '{}' がありません（先に audio:defineSnapshot）", name);
            return false;
        }
        def = &it->second;
        for (const auto& sb : *def)
            if (FindBus(sb.bus) < 0)
                Logger::Warn("audio:setSnapshot('{}'): バス '{}' がありません（無視します）", name, sb.bus);
    }
    // 遷移の起点は「今の補正値」（遷移の途中で切り替えても跳ねない）
    for (auto& b : m_buses)
    {
        b.snapFrom = b.snap;
        b.snapTo   = audio::BusMod{};
        if (def)
            for (const auto& sb : *def)
                if (sb.bus == b.name) { b.snapTo = sb.mod; break; }
    }
    m_snapCurrent  = m_snapMoving ? m_snapCurrent : m_snapTarget;
    m_snapTarget   = name;
    m_snapElapsed  = 0.0f;
    m_snapDuration = (std::max)(seconds, 0.0f);
    m_snapMoving   = true;
    return true;
}

void AudioSystem::UpdateSnapshot(f32 dt)
{
    if (!m_snapMoving) return;
    m_snapElapsed += dt;
    const f32 t = audio::TransitionProgress(m_snapElapsed, m_snapDuration);
    for (auto& b : m_buses)
    {
        b.snap = audio::LerpBusMod(b.snapFrom, b.snapTo, t);
        ApplyBus(b);
    }
    if (t >= 1.0f)
    {
        m_snapMoving  = false;
        m_snapCurrent = m_snapTarget;
    }
}

std::vector<std::string> AudioSystem::GetSnapshotNames() const
{
    std::vector<std::string> names{"default"};
    for (const auto& kv : m_snapshots) names.push_back(kv.first);
    std::sort(names.begin() + 1, names.end());
    return names;
}

AudioSystem::SnapshotState AudioSystem::GetSnapshotState() const
{
    SnapshotState st;
    st.current  = m_snapCurrent;
    st.target   = m_snapTarget;
    st.progress = m_snapMoving ? audio::TransitionProgress(m_snapElapsed, m_snapDuration) : 1.0f;
    st.duration = m_snapDuration;
    st.defined  = GetSnapshotNames();
    return st;
}

std::vector<AudioSystem::BusInfo> AudioSystem::GetBuses() const
{
    std::vector<BusInfo> out;
    out.reserve(m_buses.size());
    for (const auto& b : m_buses)
    {
        BusInfo bi;
        bi.name            = b.name;
        bi.parent          = (b.parent >= 0) ? m_buses[static_cast<size_t>(b.parent)].name : std::string();
        bi.volume          = b.volume;
        bi.muted           = b.muted;
        bi.lowpassHz       = b.lowpassHz;
        bi.snapshotGain    = b.snap.gain;
        bi.snapshotLowpass = b.snap.lowpassHz;
        bi.effectiveGain   = BusEffectiveGain(b);
        bi.chainGain       = b.chainGain;
        bi.voiceLimit      = b.voiceLimit;
        bi.reverbSend      = b.reverbSend;
        bi.reverbReturn    = b.isReverb;
        bi.peakDb          = audio::MeterPeakDb(b.meter);
        bi.rmsDb           = audio::MeterRmsDb(b.meter);
        bi.peakNowDb       = audio::LinearToDb(b.meter.peakNow);
        bi.builtin         = b.builtin;
        const i32 self = static_cast<i32>(&b - m_buses.data());
        for (const auto& v : m_voices)
        {
            if (!v.active || !BusInSubtree(v.bus, self)) continue;
            if (v.src) ++bi.realVoices; else ++bi.virtualVoices;
        }
        out.push_back(std::move(bi));
    }
    return out;
}

} // namespace dx12e
