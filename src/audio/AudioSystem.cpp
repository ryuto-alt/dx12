#include "audio/AudioSystem.h"
#include "audio/AudioClip.h"
#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <algorithm>
#include <filesystem>

namespace dx12e
{

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem()
{
    Shutdown();
}

void AudioSystem::Initialize(const std::string& assetsDir)
{
    m_assetsDir = assetsDir;

    // COM初期化（XAudio2に必要）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comInitialized = (hr == S_OK);  // S_FALSE = 既に初期化済み

    // XAudio2エンジン作成
    hr = XAudio2Create(&m_xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        Logger::Error("XAudio2 の初期化に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    // マスタリングボイス作成
    hr = m_xaudio2->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr))
    {
        Logger::Error("マスタリングボイスの作成に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    m_masterVoice->SetVolume(m_masterVolume);

    // X3DAudio 初期化（3D 空間オーディオ）
    {
        XAUDIO2_VOICE_DETAILS details{};
        m_masterVoice->GetVoiceDetails(&details);
        m_outChannels = (details.InputChannels > 0) ? details.InputChannels : 2;
        if (m_outChannels > 8) m_outChannels = 8;

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

    Logger::Info("AudioSystem initialized (XAudio2{})", m_x3dReady ? " + X3DAudio" : "");
}

void AudioSystem::Shutdown()
{
    StopBGM();
    StopAllSFX();

    // SFXボイス破棄
    for (auto& slot : m_sfxSlots)
    {
        if (slot.voice)
        {
            slot.voice->DestroyVoice();
            slot.voice = nullptr;
        }
    }

    // BGMボイス破棄
    if (m_bgmVoice)
    {
        m_bgmVoice->DestroyVoice();
        m_bgmVoice = nullptr;
    }

    // マスタリングボイス破棄
    if (m_masterVoice)
    {
        m_masterVoice->DestroyVoice();
        m_masterVoice = nullptr;
    }

    // クリップキャッシュクリア
    m_clipCache.clear();

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
        return;

    auto scanDir = [&](const std::string& subDir, std::vector<std::string>& outList) {
        std::filesystem::path dir = std::filesystem::path(m_assetsDir) / subDir;
        if (!std::filesystem::exists(dir)) return;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
        {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".mp3")
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

AudioClip* AudioSystem::GetOrLoadClip(const std::string& filePath)
{
    // キャッシュチェック
    auto it = m_clipCache.find(filePath);
    if (it != m_clipCache.end())
        return it->second.get();

    // フルパス構築（相対パスならassetsDir基準）
    const bool isRelative = (filePath.size() < 2 || filePath[1] != ':');
    std::string fullPath = filePath;
    if (isRelative)
    {
        fullPath = m_assetsDir + filePath;
    }

    // 拡張子抽出（VFS 経由ロード時に LoadFromMemory へ渡す）
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto clip = std::make_unique<AudioClip>();

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

    AudioClip* rawPtr = clip.get();
    m_clipCache[filePath] = std::move(clip);
    return rawPtr;
}

// ===== BGM =====

void AudioSystem::PlayBGM(const std::string& filePath, bool loop)
{
    if (!m_xaudio2) return;

    AudioClip* clip = GetOrLoadClip(filePath);
    if (!clip) return;

    // 既存BGMボイスを停止・破棄
    if (m_bgmVoice)
    {
        m_bgmVoice->Stop();
        m_bgmVoice->DestroyVoice();
        m_bgmVoice = nullptr;
    }

    // 新しいソースボイス作成
    WAVEFORMATEX fmt = clip->GetFormat();
    HRESULT hr = m_xaudio2->CreateSourceVoice(&m_bgmVoice, &fmt);
    if (FAILED(hr))
    {
        Logger::Error("ソースボイス作成（BGM）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    m_bgmVoice->SetVolume(m_bgmVolume);

    // バッファ送信
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = clip->GetSizeInBytes();
    buffer.pAudioData = clip->GetPCMData();
    buffer.Flags      = XAUDIO2_END_OF_STREAM;
    buffer.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0;

    hr = m_bgmVoice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr))
    {
        Logger::Error("バッファ送信（BGM）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    m_bgmVoice->Start();
    m_currentBGMPath = filePath;

    Logger::Info("BGM playing: {} (loop={})", filePath, loop);
}

void AudioSystem::StopBGM()
{
    if (m_bgmVoice)
    {
        m_bgmVoice->Stop();
        m_bgmVoice->FlushSourceBuffers();
    }
    m_currentBGMPath.clear();
}

void AudioSystem::PauseBGM()
{
    if (m_bgmVoice)
        m_bgmVoice->Stop();
}

void AudioSystem::ResumeBGM()
{
    if (m_bgmVoice)
        m_bgmVoice->Start();
}

// ===== SFX =====

void AudioSystem::PlaySFX(const std::string& filePath, bool loop)
{
    if (!m_xaudio2) return;

    AudioClip* clip = GetOrLoadClip(filePath);
    if (!clip) return;

    // 空きスロットを探す
    i32 freeSlot = -1;
    for (u32 i = 0; i < kMaxSFXVoices; ++i)
    {
        if (!m_sfxSlots[i].voice)
        {
            freeSlot = static_cast<i32>(i);
            break;
        }

        // 再生終了チェック
        XAUDIO2_VOICE_STATE state{};
        m_sfxSlots[i].voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0)
        {
            freeSlot = static_cast<i32>(i);
            break;
        }
    }

    // 空きがなければ最初のスロットを強制停止
    if (freeSlot < 0)
    {
        freeSlot = 0;
    }

    auto& slot = m_sfxSlots[freeSlot];

    // 既存ボイスを破棄して再作成（フォーマットが違う可能性）
    if (slot.voice)
    {
        slot.voice->Stop();
        slot.voice->DestroyVoice();
        slot.voice = nullptr;
    }

    WAVEFORMATEX fmt = clip->GetFormat();
    HRESULT hr = m_xaudio2->CreateSourceVoice(&slot.voice, &fmt);
    if (FAILED(hr))
    {
        Logger::Error("ソースボイス作成（SFX）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    slot.voice->SetVolume(m_sfxVolume);

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = clip->GetSizeInBytes();
    buffer.pAudioData = clip->GetPCMData();
    buffer.Flags      = XAUDIO2_END_OF_STREAM;
    buffer.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0;

    hr = slot.voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr))
    {
        Logger::Error("バッファ送信（SFX）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    slot.spatial = false;  // 非空間
    slot.voice->Start();
}

// ===== 3D 空間オーディオ =====

void AudioSystem::SetListener(float px, float py, float pz,
                              float fx, float fy, float fz,
                              float ux, float uy, float uz)
{
    m_listener.Position    = {px, py, pz};
    m_listener.OrientFront = {fx, fy, fz};
    m_listener.OrientTop   = {ux, uy, uz};
}

void AudioSystem::ComputeAndApply(SFXSlot& slot)
{
    if (!m_x3dReady || !slot.voice) return;

    X3DAUDIO_EMITTER emitter{};
    emitter.Position            = {slot.emitterPos[0], slot.emitterPos[1], slot.emitterPos[2]};
    emitter.OrientFront         = {0.0f, 0.0f, 1.0f};
    emitter.OrientTop           = {0.0f, 1.0f, 0.0f};
    emitter.ChannelCount        = 1;
    emitter.CurveDistanceScaler = (slot.maxDist > 0.01f) ? slot.maxDist : 1.0f;

    // minDist までフル音量、maxDist で 0 になる線形カーブ
    float minR = (slot.maxDist > 0.01f) ? (slot.minDist / slot.maxDist) : 0.0f;
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
    slot.voice->SetOutputMatrix(nullptr, 1, m_outChannels, matrix);
}

i32 AudioSystem::PlaySFXSpatial(const std::string& filePath, float x, float y, float z,
                                float minDistance, float maxDistance, float volume, bool loop)
{
    if (!m_xaudio2) return -1;

    AudioClip* clip = GetOrLoadClip(filePath);
    if (!clip) return -1;

    // モノ以外は空間化できないので通常 SFX にフォールバック
    if (clip->GetFormat().nChannels != 1)
    {
        Logger::Warn("PlaySFXSpatial: '{}' はモノラルではないため 2D SFX として再生します", filePath);
        PlaySFX(filePath, loop);
        return -1;
    }

    // 空きスロット探索（PlaySFX と同じ方針）
    i32 freeSlot = -1;
    for (u32 i = 0; i < kMaxSFXVoices; ++i)
    {
        if (!m_sfxSlots[i].voice) { freeSlot = static_cast<i32>(i); break; }
        XAUDIO2_VOICE_STATE state{};
        m_sfxSlots[i].voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) { freeSlot = static_cast<i32>(i); break; }
    }
    if (freeSlot < 0) freeSlot = 0;

    auto& slot = m_sfxSlots[freeSlot];
    if (slot.voice)
    {
        slot.voice->Stop();
        slot.voice->DestroyVoice();
        slot.voice = nullptr;
    }

    WAVEFORMATEX fmt = clip->GetFormat();
    HRESULT hr = m_xaudio2->CreateSourceVoice(&slot.voice, &fmt);
    if (FAILED(hr))
    {
        Logger::Error("ソースボイス作成（空間SFX）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return -1;
    }

    slot.spatial        = true;
    slot.minDist        = minDistance;
    slot.maxDist        = maxDistance;
    slot.emitterPos[0]  = x;
    slot.emitterPos[1]  = y;
    slot.emitterPos[2]  = z;
    slot.voice->SetVolume(std::clamp(volume, 0.0f, 1.0f) * m_sfxVolume);

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = clip->GetSizeInBytes();
    buffer.pAudioData = clip->GetPCMData();
    buffer.Flags      = XAUDIO2_END_OF_STREAM;
    buffer.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0;
    hr = slot.voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr))
    {
        Logger::Error("バッファ送信（空間SFX）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return -1;
    }

    ComputeAndApply(slot);
    slot.voice->Start();
    return freeSlot;
}

void AudioSystem::UpdateSpatialEmitter(i32 slotId, float x, float y, float z)
{
    if (slotId < 0 || slotId >= static_cast<i32>(kMaxSFXVoices)) return;
    auto& slot = m_sfxSlots[static_cast<u32>(slotId)];
    if (!slot.voice || !slot.spatial) return;
    slot.emitterPos[0] = x;
    slot.emitterPos[1] = y;
    slot.emitterPos[2] = z;
}

void AudioSystem::Update()
{
    if (!m_x3dReady) return;
    for (auto& slot : m_sfxSlots)
    {
        if (!slot.voice || !slot.spatial) continue;
        XAUDIO2_VOICE_STATE state{};
        slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) { slot.spatial = false; continue; }
        ComputeAndApply(slot);
    }
}

void AudioSystem::StopAllSFX()
{
    for (auto& slot : m_sfxSlots)
    {
        if (slot.voice)
        {
            slot.voice->Stop();
            slot.voice->FlushSourceBuffers();
        }
    }
}

// ===== Volume =====

void AudioSystem::SetMasterVolume(f32 volume)
{
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    if (m_masterVoice)
        m_masterVoice->SetVolume(m_masterVolume);
}

void AudioSystem::SetBGMVolume(f32 volume)
{
    m_bgmVolume = std::clamp(volume, 0.0f, 1.0f);
    if (m_bgmVoice)
        m_bgmVoice->SetVolume(m_bgmVolume);
}

void AudioSystem::SetSFXVolume(f32 volume)
{
    m_sfxVolume = std::clamp(volume, 0.0f, 1.0f);
    for (auto& slot : m_sfxSlots)
    {
        if (slot.voice)
            slot.voice->SetVolume(m_sfxVolume);
    }
}

} // namespace dx12e
