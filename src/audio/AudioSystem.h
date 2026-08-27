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
    // 今鳴っている BGM の assets 相対パス（鳴っていなければ空）。
    // ★playBGM は同じパスでも必ず頭出しするので、「シーンをまたいで同じ曲を鳴らし続ける」は
    //   これで判定して呼ばない、という形でしか書けない（曲の途中で切り替わると
    //   イントロへ戻ってしまうため、engine 側で勝手に早期 return はしない）。
    const std::string& GetCurrentBGM() const { return m_currentBGMPath; }
    // ★「ボイスが存在するか」ではなく「実際に鳴っているか」。
    //   StopBGM はボイスを使い回すために破棄しないので、以前の実装だと
    //   **一度 playBGM したら永久に true** だった。結果、一番自然な書き方
    //   `if not audio:isBGMPlaying() then audio:playBGM(...) end` が
    //   stopBGM 後も曲の終了後も二度と鳴らない。Play/Stop もまたぐ
    //   （Stop で StopBGM されるので、次の Play の OnStart では「鳴っている」判定になる）。
    bool IsBGMPlaying() const { return m_bgmPlaying; }

    void StopAllSFX();
    // Lua の setListener による上書きを解除してカメラ位置へ戻す。
    // シーン切替で呼ばないと、次シーンで setListener を呼ばない限り
    // 前ステージの座標にリスナーが固定されたままになる。
    void ClearListenerOverride() { m_listenerPosOverride = false; }

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
    // ★鳴っている 1 本を掴んで操作する 3 つ。PlaySFXSpatial / PlaySFXTracked が返す ID を使う。
    //   ループ再生した環境音を止める・フェードさせる・回転数が落ちるように鳴らす、が
    //   これが無いと書けなかった（StopAllSFX しか無く、他の音まで巻き添えになる）。
    //   世代が食い違う ID（スロットが使い回された後）は黙って無視する。
    void StopVoice(i32 slotId);
    void SetVoiceVolume(i32 slotId, float volume);        // 0..1（クリップ個別音量）
    void SetVoicePitch(i32 slotId, float ratio);          // 再生速度＝ピッチ。0.1..2.0
    bool IsVoicePlaying(i32 slotId) const;
    // 非空間の SFX を ID 付きで鳴らす（上の 3 つで操作できる）。失敗時 -1。
    i32  PlaySFXTracked(const std::string& filePath, bool loop = false, float volume = 1.0f);
    // 遮蔽量 0..1（1=リスナーとの間に壁がある）。ローパスで「こもった」音にし、音量も落とす。
    // 値は Update() 内で時定数 ~0.1s で追従するので、毎フレーム 0/1 を投げてよい。
    void SetOcclusion(i32 slotId, float amount);
    // 実際に使われているリスナー位置（Lua の setListener 上書き込み）。遮蔽レイの終点用。
    void GetListenerPos(float& x, float& y, float& z) const;
    void Update(f32 dt);  // 毎フレーム: 空間ボイスの定位と遮蔽を再計算

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
    // 実際に鳴っているか。ボイスは使い回すので存在では判定できない
    // （StopBGM / PauseBGM で false、PlayBGM / ResumeBGM で true）。
    bool                 m_bgmPlaying = false;
    // 空きスロットが無いときに奪う位置（ラウンドロビン）。0 番固定だと全部が 0 番を
    // 奪い合って 1 音しか聞こえなくなる。
    u32                  m_sfxStealCursor = 0;
    bool                 m_sfxStealWarned = false;
    bool                 m_bgmLoop = true;

    // SFX pool
    static constexpr u32 kMaxSFXVoices = 16;
    struct SFXSlot {
        IXAudio2SourceVoice* voice = nullptr;
        bool  spatial = false;
        float minDist = 1.0f;
        float maxDist = 30.0f;
        float emitterPos[3] = {0, 0, 0};
        // ★スロットの世代。PlaySFXSpatial のたびに +1 する。
        //   AudioSource::runtimeSlot は鳴り終わっても -1 に戻らないので、スロットが
        //   使い回されると**古いエンティティが新しい音の定位を毎フレーム上書き**していた
        //   （遠くの敵の足音が自分の足元から鳴る）。返す ID に世代を混ぜて弾く。
        u32   generation = 0;
        // ★このクリップ個別の音量（0..1）。SetSFXVolume がマスターを掛け直すのに要る。
        //   持っていなかったので、オプション画面の SE スライダーを触った瞬間に
        //   小さく鳴らしていた環境音や遠くの空間音が**マスター音量の大きさに跳ね上がって**いた。
        float clipVolume = 1.0f;
        // 遮蔽（壁越し）。target が呼び出し側の指定、cur が時間平滑した実効値。
        // 直接入れるとドア枠を通るたびにブツッと切り替わる。
        float occTarget  = 0.0f;
        float occ        = 0.0f;
        u32   sampleRate = 44100;   // ローパスのカットオフ計算に要る
    };
    std::array<SFXSlot, kMaxSFXVoices> m_sfxSlots{};

    // X3DAudio
    X3DAUDIO_HANDLE   m_x3d{};
    X3DAUDIO_LISTENER m_listener{};
    bool  m_listenerPosOverride = false;   // Lua が SetListenerPos した後はカメラ位置より優先
    float m_lopX = 0, m_lopY = 0, m_lopZ = 0;
    u32  m_outChannels = 2;
    bool m_x3dReady = false;
    SFXSlot* ResolveVoice(i32 slotId);    // ID → スロット（世代が食い違えば nullptr）
    void ComputeAndApply(SFXSlot& slot);
    void ApplyOcclusion(SFXSlot& slot);   // slot.occ を音量とローパスへ反映

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
