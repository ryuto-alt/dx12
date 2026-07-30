#pragma once

#include <string>
#include <memory>
#include <array>
#include <unordered_map>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>
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

    // プロジェクト切替時に assets ベースを再設定して音声ファイルを再スキャン
    void SetAssetsDir(const std::string& assetsDir) { m_assetsDir = assetsDir; ScanAudioFiles(); }

    // BGM
    void PlayBGM(const std::string& filePath, bool loop = true);
    void StopBGM();
    void PauseBGM();
    void ResumeBGM();
    void SeekBGM(f32 seconds);   // 再生中BGMの位置を秒指定でジャンプ(ループ設定は維持)
    void SetBGMRate(f32 ratio);  // 再生速度倍率(ピッチ連動)。1=通常、0.5=半速+1oct下。スローモ演出用

    // SFX
    // volume は 0..1 のクリップ個別音量（マスター m_sfxVolume に乗算される）。
    // ★以前は引数が無く、AudioSource::volume は spatial=true の経路でしか効かなかった
    //   （Inspector のスライダに注記も無いので「動かしたのに変わらない」になっていた）。
    void PlaySFX(const std::string& filePath, bool loop = false, float volume = 1.0f);
    void StopAllSFX();

    // 3D 空間オーディオ（SFX のみ。ステレオ素材は自動モノ化。リスナーは通常カメラ）
    void SetListener(float px, float py, float pz,
                     float fx, float fy, float fz,
                     float ux, float uy, float uz);
    // Lua用: リスナー位置をプレイヤー等に上書き(向きはカメラ由来のまま)。毎フレーム呼ばれる想定
    void SetListenerPos(float x, float y, float z);
    // 空間 SFX をワンショット再生。戻り値スロット ID（追従用）、失敗/非対応は -1。
    i32  PlaySFXSpatial(const std::string& filePath, float x, float y, float z,
                        float minDistance, float maxDistance,
                        float volume = 1.0f, bool loop = false);
    void UpdateSpatialEmitter(i32 slotId, float x, float y, float z);
    void Update();  // 毎フレーム: 空間ボイスの定位を再計算

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
    bool                 m_bgmLoop = true;

    // SFX pool
    static constexpr u32 kMaxSFXVoices = 16;
    struct SFXSlot {
        IXAudio2SourceVoice* voice = nullptr;
        bool  spatial = false;
        float minDist = 1.0f;
        float maxDist = 30.0f;
        float emitterPos[3] = {0, 0, 0};
    };
    std::array<SFXSlot, kMaxSFXVoices> m_sfxSlots{};

    // X3DAudio
    X3DAUDIO_HANDLE   m_x3d{};
    X3DAUDIO_LISTENER m_listener{};
    bool  m_listenerPosOverride = false;   // Lua が SetListenerPos した後はカメラ位置より優先
    float m_lopX = 0, m_lopY = 0, m_lopZ = 0;
    u32  m_outChannels = 2;
    bool m_x3dReady = false;
    void ComputeAndApply(SFXSlot& slot);

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
