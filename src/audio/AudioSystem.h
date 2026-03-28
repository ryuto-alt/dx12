#pragma once

#include <string>
#include <memory>
#include <array>
#include <unordered_map>
#include <wrl/client.h>
#include <xaudio2.h>
#include "core/Types.h"

namespace dx12e
{

class AudioClip;

class AudioSystem
{
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void Initialize(const std::string& assetsDir);
    void Shutdown();

    // BGM
    void PlayBGM(const std::string& filePath, bool loop = true);
    void StopBGM();
    void PauseBGM();
    void ResumeBGM();

    // SFX
    void PlaySFX(const std::string& filePath, bool loop = false);
    void StopAllSFX();

    // Volume (0.0 - 1.0)
    void SetMasterVolume(f32 volume);
    void SetBGMVolume(f32 volume);
    void SetSFXVolume(f32 volume);
    f32  GetMasterVolume() const { return m_masterVolume; }
    f32  GetBGMVolume() const { return m_bgmVolume; }
    f32  GetSFXVolume() const { return m_sfxVolume; }

    // assets/audio/ 以下の音声ファイルを自動検出
    const std::vector<std::string>& GetBGMList() const { return m_bgmList; }
    const std::vector<std::string>& GetSFXList() const { return m_sfxList; }
    void ScanAudioFiles();

private:
    AudioClip* GetOrLoadClip(const std::string& filePath);

    Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2;
    IXAudio2MasteringVoice*          m_masterVoice = nullptr;

    // BGM
    IXAudio2SourceVoice* m_bgmVoice = nullptr;
    std::string          m_currentBGMPath;

    // SFX pool
    static constexpr u32 kMaxSFXVoices = 16;
    struct SFXSlot {
        IXAudio2SourceVoice* voice = nullptr;
    };
    std::array<SFXSlot, kMaxSFXVoices> m_sfxSlots{};

    // Clip cache
    std::unordered_map<std::string, std::unique_ptr<AudioClip>> m_clipCache;

    f32 m_masterVolume = 1.0f;
    f32 m_bgmVolume    = 0.7f;
    f32 m_sfxVolume    = 1.0f;

    std::string m_assetsDir;
    bool m_comInitialized = false;

    // 自動検出されたファイルリスト（assetsDir相対パス）
    std::vector<std::string> m_bgmList;
    std::vector<std::string> m_sfxList;
};

} // namespace dx12e
