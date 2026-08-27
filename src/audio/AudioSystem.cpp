#include "audio/AudioSystem.h"
#include "audio/AudioClip.h"
#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <algorithm>
#include <cmath>
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

    m_bgmVoice->SetFrequencyRatio(1.0f);   // 前曲のスローモ演出を持ち越さない
    m_bgmVoice->Start();
    m_currentBGMPath = filePath;
    m_bgmLoop = loop;
    m_bgmPlaying = true;

    Logger::Info("BGM playing: {} (loop={})", filePath, loop);
}

void AudioSystem::SeekBGM(f32 seconds)
{
    if (!m_bgmVoice || m_currentBGMPath.empty()) return;

    AudioClip* clip = GetOrLoadClip(m_currentBGMPath);
    if (!clip) return;

    const WAVEFORMATEX& fmt = clip->GetFormat();
    if (fmt.nBlockAlign == 0 || fmt.nSamplesPerSec == 0) return;

    const u32 totalFrames = clip->GetSizeInBytes() / fmt.nBlockAlign;
    if (totalFrames == 0) return;
    u32 frame = static_cast<u32>(seconds * static_cast<f32>(fmt.nSamplesPerSec));
    frame %= totalFrames;   // ループ範囲内に丸める(負は呼び出し側で扱わない)

    m_bgmVoice->Stop();
    m_bgmVoice->FlushSourceBuffers();

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = clip->GetSizeInBytes();
    buffer.pAudioData = clip->GetPCMData();
    buffer.Flags      = XAUDIO2_END_OF_STREAM;
    buffer.LoopCount  = m_bgmLoop ? XAUDIO2_LOOP_INFINITE : 0;
    buffer.PlayBegin  = frame;   // ここから再生(ループ時は末尾→先頭に戻る)

    HRESULT hr = m_bgmVoice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr))
    {
        Logger::Error("バッファ送信（BGMシーク）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return;
    }
    m_bgmVoice->Start();
}

void AudioSystem::SetBGMRate(f32 ratio)
{
    if (!m_bgmVoice) return;
    // XAudio2 の既定 MaxFrequencyRatio は 2.0。下限は無音同然になる前に打ち切る
    if (ratio < 0.05f) ratio = 0.05f;
    if (ratio > 2.0f)  ratio = 2.0f;
    m_bgmVoice->SetFrequencyRatio(ratio);
}

void AudioSystem::StopBGM()
{
    if (m_bgmVoice)
    {
        m_bgmVoice->Stop();
        m_bgmVoice->FlushSourceBuffers();
    }
    m_currentBGMPath.clear();
    m_bgmPlaying = false;
}

void AudioSystem::PauseBGM()
{
    if (m_bgmVoice)
        m_bgmVoice->Stop();
    m_bgmPlaying = false;
}

void AudioSystem::ResumeBGM()
{
    if (m_bgmVoice)
    { m_bgmVoice->Start(); m_bgmPlaying = true; }
}

// ===== SFX =====

void AudioSystem::PlaySFX(const std::string& filePath, bool loop, float volume)
{
    PlaySFXTracked(filePath, loop, volume);
}

// ★中身は元の PlaySFX そのままで、最後にスロット ID を返すだけ。
//   ループ再生した環境音を後から止める・絞る・回転を落とす、が ID 無しでは書けなかった。
i32 AudioSystem::PlaySFXTracked(const std::string& filePath, bool loop, float volume)
{
    if (!m_xaudio2) return -1;

    AudioClip* clip = GetOrLoadClip(filePath);
    if (!clip) return -1;

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

    // 空きがなければ一番古いスロットを奪う。
    // ★以前は無条件に 0 番だった。ループ再生の SE（環境音など）は BuffersQueued が
    //   永久に 0 にならずスロットを占有し続けるので、混み合うと**新しい音が全部 0 番へ
    //   集中して互いを切り合い、1 つしか聞こえないうえ鳴り直し続ける**。
    //   ラウンドロビンなら少なくとも 16 個ぶんは分散する。ログも 1 度だけ出して、
    //   「音が鳴らない/途切れる」の原因に気づけるようにする。
    if (freeSlot < 0)
    {
        freeSlot = static_cast<i32>(m_sfxStealCursor % kMaxSFXVoices);
        m_sfxStealCursor = (m_sfxStealCursor + 1) % kMaxSFXVoices;
        if (!m_sfxStealWarned)
        {
            m_sfxStealWarned = true;
            Logger::Warn("SE の同時発音数が上限({})に達しました。以降は古い音を止めて鳴らします"
                         "（ループ再生の SE はスロットを占有し続けるので、"
                         "使い終わったら stopAllSFX するか loop=false にしてください）",
                         kMaxSFXVoices);
        }
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
        return -1;
    }

    slot.clipVolume = std::clamp(volume, 0.0f, 1.0f);
    slot.voice->SetVolume(m_sfxVolume * slot.clipVolume);

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = clip->GetSizeInBytes();
    buffer.pAudioData = clip->GetPCMData();
    buffer.Flags      = XAUDIO2_END_OF_STREAM;
    buffer.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0;

    hr = slot.voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr))
    {
        Logger::Error("バッファ送信（SFX）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return -1;
    }

    slot.spatial = false;  // 非空間
    slot.voice->Start();
    ++slot.generation;
    return static_cast<i32>((slot.generation << 8) | static_cast<u32>(freeSlot));
}

// ===== 鳴っている 1 本を掴んで操作する =====
// ★ID は (generation<<8)|index。スロットが別の音へ使い回されていたら世代が食い違うので、
//   古い ID で新しい音を止めてしまう事故が起きない。
AudioSystem::SFXSlot* AudioSystem::ResolveVoice(i32 slotId)
{
    if (slotId < 0) return nullptr;
    const u32 index = static_cast<u32>(slotId) & 0xFFu;
    const u32 gen   = static_cast<u32>(slotId) >> 8;
    if (index >= kMaxSFXVoices) return nullptr;
    auto& slot = m_sfxSlots[index];
    if (!slot.voice || slot.generation != gen) return nullptr;
    return &slot;
}

void AudioSystem::StopVoice(i32 slotId)
{
    SFXSlot* slot = ResolveVoice(slotId);
    if (!slot) return;
    slot->voice->Stop();
    slot->voice->FlushSourceBuffers();   // ループ中でもこれで確実に止まる
}

void AudioSystem::SetVoiceVolume(i32 slotId, float volume)
{
    SFXSlot* slot = ResolveVoice(slotId);
    if (!slot) return;
    slot->clipVolume = std::clamp(volume, 0.0f, 1.0f);
    // 空間音は距離減衰を ComputeAndApply が毎フレーム掛け直すので、そちらに任せる。
    if (!slot->spatial) slot->voice->SetVolume(slot->clipVolume * m_sfxVolume);
}

void AudioSystem::SetVoicePitch(i32 slotId, float ratio)
{
    SFXSlot* slot = ResolveVoice(slotId);
    if (!slot) return;
    slot->voice->SetFrequencyRatio(std::clamp(ratio, 0.1f, 2.0f));
}

bool AudioSystem::IsVoicePlaying(i32 slotId) const
{
    if (slotId < 0) return false;
    const u32 index = static_cast<u32>(slotId) & 0xFFu;
    const u32 gen   = static_cast<u32>(slotId) >> 8;
    if (index >= kMaxSFXVoices) return false;
    const auto& slot = m_sfxSlots[index];
    if (!slot.voice || slot.generation != gen) return false;
    XAUDIO2_VOICE_STATE state{};
    slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued > 0;
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

// 遮蔽の効き。壁越しは「高域が落ちて音量も下がる」。数値は完全に趣味の範囲。
static constexpr float kOccVolumeDrop = 0.65f;    // 全遮蔽で音量 -65%
static constexpr float kOccCutoffHz   = 420.0f;   // 全遮蔽時のローパス
static constexpr float kOccSmooth     = 8.0f;     // 1/s。時定数 ~0.12s

void AudioSystem::ApplyOcclusion(SFXSlot& slot)
{
    if (!slot.voice) return;
    const float occ = std::clamp(slot.occ, 0.0f, 1.0f);
    slot.voice->SetVolume(m_sfxVolume * slot.clipVolume * (1.0f - kOccVolumeDrop * occ));

    // XAudio2 のフィルタ Frequency は 2*sin(pi*fc/fs)。1.0（＝上限）が素通し。
    const float fs   = static_cast<float>(slot.sampleRate > 0 ? slot.sampleRate : 44100);
    const float shut = std::clamp(2.0f * std::sin(3.14159265f * kOccCutoffHz / fs),
                                  0.0f, XAUDIO2_MAX_FILTER_FREQUENCY);
    XAUDIO2_FILTER_PARAMETERS fp{};
    fp.Type      = LowPassFilter;
    fp.Frequency = XAUDIO2_MAX_FILTER_FREQUENCY + (shut - XAUDIO2_MAX_FILTER_FREQUENCY) * occ;
    fp.OneOverQ  = 1.0f;
    slot.voice->SetFilterParameters(&fp);
}

void AudioSystem::SetOcclusion(i32 slotId, float amount)
{
    if (slotId < 0) return;
    const u32 index = static_cast<u32>(slotId) & 0xFFu;
    const u32 gen   = static_cast<u32>(slotId) >> 8;
    if (index >= kMaxSFXVoices) return;
    auto& slot = m_sfxSlots[index];
    if (!slot.voice || !slot.spatial) return;
    if (slot.generation != gen) return;   // 使い回された後のスロット。触らない
    slot.occTarget = std::clamp(amount, 0.0f, 1.0f);
}

void AudioSystem::GetListenerPos(float& x, float& y, float& z) const
{
    x = m_listener.Position.x;
    y = m_listener.Position.y;
    z = m_listener.Position.z;
}

i32 AudioSystem::PlaySFXSpatial(const std::string& filePath, float x, float y, float z,
                                float minDistance, float maxDistance, float volume, bool loop)
{
    if (!m_xaudio2) return -1;

    AudioClip* clip = GetOrLoadClip(filePath);
    if (!clip) return -1;

    // ステレオ素材は自動でモノにダウンミックスして空間化(キャッシュごと変換、次回からはモノ)
    if (clip->GetFormat().nChannels != 1)
    {
        Logger::Info("PlaySFXSpatial: '{}' をモノにダウンミックスして空間再生します", filePath);
        clip->DownmixToMono();
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
    // ★USEFILTER はボイス生成時にしか付けられない。遮蔽のローパスに要る。
    HRESULT hr = m_xaudio2->CreateSourceVoice(&slot.voice, &fmt, XAUDIO2_VOICE_USEFILTER);
    if (FAILED(hr))
    {
        Logger::Error("ソースボイス作成（空間SFX）に失敗しました: 0x{:08X}", static_cast<u32>(hr));
        return -1;
    }

    slot.sampleRate     = fmt.nSamplesPerSec;
    slot.occ            = 0.0f;
    slot.occTarget      = 0.0f;
    slot.spatial        = true;
    slot.minDist        = minDistance;
    slot.maxDist        = maxDistance;
    slot.emitterPos[0]  = x;
    slot.emitterPos[1]  = y;
    slot.emitterPos[2]  = z;
    slot.clipVolume = std::clamp(volume, 0.0f, 1.0f);
    slot.voice->SetVolume(slot.clipVolume * m_sfxVolume);

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
    // 世代を進めて (generation<<8)|index を返す。呼び出し側はこれをそのまま持ち、
    // UpdateSpatialEmitter へ渡す。スロットが別の音に使い回されたら世代が食い違って弾かれる。
    ++slot.generation;
    return static_cast<i32>((slot.generation << 8) | static_cast<u32>(freeSlot));
}

void AudioSystem::UpdateSpatialEmitter(i32 slotId, float x, float y, float z)
{
    if (slotId < 0) return;
    const u32 index = static_cast<u32>(slotId) & 0xFFu;
    const u32 gen   = static_cast<u32>(slotId) >> 8;
    if (index >= kMaxSFXVoices) return;
    auto& slot = m_sfxSlots[index];
    if (!slot.voice || !slot.spatial) return;
    // ★世代が違う＝このスロットは既に別の音へ使い回されている。触らない。
    if (slot.generation != gen) return;
    slot.emitterPos[0] = x;
    slot.emitterPos[1] = y;
    slot.emitterPos[2] = z;
}

void AudioSystem::Update(f32 dt)
{
    if (!m_x3dReady) return;
    const float k = std::clamp(dt * kOccSmooth, 0.0f, 1.0f);
    for (auto& slot : m_sfxSlots)
    {
        if (!slot.voice || !slot.spatial) continue;
        XAUDIO2_VOICE_STATE state{};
        slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) { slot.spatial = false; continue; }
        ComputeAndApply(slot);
        slot.occ += (slot.occTarget - slot.occ) * k;
        ApplyOcclusion(slot);
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
        // ★クリップ個別音量を掛け直す。以前はマスター値をそのまま書いていたので、
        //   再生中の小さい音がスライダーを触った瞬間に最大音量へ跳ね上がっていた
        //   （再生時は個別音量を掛けているので、ここだけ規約が破れていた）。
        // ★ApplyOcclusion 経由で書く。直接 SetVolume すると壁越しでこもらせた音が
        //   スライダーを触った瞬間だけ素の音量へ戻る（次の Update で戻るので一瞬鳴る）。
        if (slot.voice)
            ApplyOcclusion(slot);
    }
}

} // namespace dx12e
