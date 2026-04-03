#include "audio/AudioSystem.h"
#include "audio/AudioClip.h"
#include "core/Logger.h"

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
        Logger::Error("XAudio2Create failed: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    // マスタリングボイス作成
    hr = m_xaudio2->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr))
    {
        Logger::Error("CreateMasteringVoice failed: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    m_masterVoice->SetVolume(m_masterVolume);

    ScanAudioFiles();

    Logger::Info("AudioSystem initialized (XAudio2)");
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
    std::string fullPath = filePath;
    if (filePath.size() < 2 || filePath[1] != ':')  // 絶対パスでなければ
    {
        fullPath = m_assetsDir + filePath;
    }

    auto clip = std::make_unique<AudioClip>();
    if (!clip->LoadFromFile(fullPath))
        return nullptr;

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
        Logger::Error("CreateSourceVoice (BGM) failed: 0x{:08X}", static_cast<u32>(hr));
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
        Logger::Error("SubmitSourceBuffer (BGM) failed: 0x{:08X}", static_cast<u32>(hr));
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
        Logger::Error("CreateSourceVoice (SFX) failed: 0x{:08X}", static_cast<u32>(hr));
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
        Logger::Error("SubmitSourceBuffer (SFX) failed: 0x{:08X}", static_cast<u32>(hr));
        return;
    }

    slot.voice->Start();
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
