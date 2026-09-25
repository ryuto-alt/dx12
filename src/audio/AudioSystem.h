#pragma once

#include <string>
#include <memory>
#include <array>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>
#include "core/Types.h"
#include "audio/AudioMath.h"

namespace dx12e
{

class AudioClip;

// ===========================================================================
// ミキサーの構造（バス = XAudio2 のサブミックスボイス）
//
//   mastering voice ← master ← music     … BGM（playBGM）
//                            ← sfx       … 効果音の既定（playSFX / playSpatial / AudioSource）
//                            ← ambience  … 環境音
//                            ← voice     … 声・セリフ
//                            ← ui        … UI の操作音
//                            ← (createBus で足したユーザー定義バス。親は任意のバス)
//
// ★バスは「子 → 親」の順に処理しないと音が 1 クォンタム遅れる/消える。XAudio2 は
//   ProcessingStage の小さい方から処理し、「自分以下の stage へは送れない」ので、
//   深さに応じて stage を下げている（master が最大）。
// ★旧 API は互換のまま内部でバスへ写す: setMasterVolume = master、setBGMVolume = music、
//   setSFXVolume = sfx。ボイス側はクリップ個別の音量しか持たない（バス音量は掛けない）。
// ===========================================================================
class AudioSystem
{
public:
    // バス 1 本ぶんの読み出し用の写し（エディタのミキサー窓 / MCP audio_state / Lua getBuses）。
    struct BusInfo
    {
        std::string name;
        std::string parent;          // master は空
        f32  volume     = 1.0f;      // ユーザー設定（setBusVolume）
        bool muted      = false;
        f32  lowpassHz  = 0.0f;      // ユーザー設定（0 = 無し）
        f32  snapshotGain    = 1.0f; // スナップショット補正（線形）
        f32  snapshotLowpass = 0.0f;
        f32  effectiveGain   = 1.0f; // このバスが実際に掛けている量（volume*snapshot*mute）
        bool builtin    = false;     // 既定の 6 本（消せない・親を変えられない）
    };

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
    // volume は 0..1 のクリップ個別音量（バス音量 sfx / master はバス側で掛かる）。
    // ★以前は引数が無く、AudioSource::volume は spatial=true の経路でしか効かなかった
    //   （Inspector のスライダに注記も無いので「動かしたのに変わらない」になっていた）。
    // bus は空なら "sfx"。存在しないバス名は警告を 1 度だけ出して "sfx" へ流す（無音にはしない）。
    void PlaySFX(const std::string& filePath, bool loop = false, float volume = 1.0f,
                 const std::string& bus = {});
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
                        float volume = 1.0f, bool loop = false,
                        const std::string& bus = {});
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
    i32  PlaySFXTracked(const std::string& filePath, bool loop = false, float volume = 1.0f,
                        const std::string& bus = {});
    // 遮蔽量 0..1（1=リスナーとの間に壁がある）。ローパスで「こもった」音にし、音量も落とす。
    // 値は Update() 内で時定数 ~0.1s で追従するので、毎フレーム 0/1 を投げてよい。
    void SetOcclusion(i32 slotId, float amount);
    // 実際に使われているリスナー位置（Lua の setListener 上書き込み）。遮蔽レイの終点用。
    void GetListenerPos(float& x, float& y, float& z) const;
    void Update(f32 dt);  // 毎フレーム: 空間ボイスの定位と遮蔽を再計算

    // Volume (0.0 - 1.0)。★中身はバス音量（master / music / sfx）の別名。
    void SetMasterVolume(f32 volume) { SetBusVolume("master", volume); }
    void SetBGMVolume(f32 volume)    { SetBusVolume("music", volume); }
    void SetSFXVolume(f32 volume)    { SetBusVolume("sfx", volume); }
    f32  GetMasterVolume() const { return GetBusVolume("master"); }
    f32  GetBGMVolume() const    { return GetBusVolume("music"); }
    f32  GetSFXVolume() const    { return GetBusVolume("sfx"); }

    // ---- バス（サブミックス）----
    // 新しいバスを parent（空なら master）の子として作る。既にあれば何もせず true。
    // 失敗（親が無い / 深すぎる / 名前が空）は false とログ。
    bool CreateBus(const std::string& name, const std::string& parent = {});
    bool HasBus(const std::string& name) const { return FindBus(name) >= 0; }
    void SetBusVolume(const std::string& name, f32 volume);     // 0..4（1 超は増幅）
    f32  GetBusVolume(const std::string& name) const;          // 無いバスは 0
    void SetBusMute(const std::string& name, bool muted);
    bool IsBusMuted(const std::string& name) const;
    void SetBusLowpass(const std::string& name, f32 hz);       // 0 = 無し
    f32  GetBusLowpass(const std::string& name) const;
    std::vector<BusInfo> GetBuses() const;                      // 親 → 子の順（master が先頭）
    // デバイスが使えているか（ヘッドレス/音声デバイス無しでは false。状態は保持し続ける）。
    bool IsDeviceReady() const { return m_masterVoice != nullptr; }
    const std::string& GetDeviceStatus() const { return m_deviceStatus; }
    u32  GetOutputChannels() const { return m_outChannels; }
    u32  GetOutputSampleRate() const { return m_outSampleRate; }

    // assets/audio/ 以下の音声ファイルを自動検出
    const std::vector<std::string>& GetBGMList() const { return m_bgmList; }
    const std::vector<std::string>& GetSFXList() const { return m_sfxList; }
    void ScanAudioFiles();

private:
    AudioClip* GetOrLoadClip(const std::string& filePath);

    Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2;
    IXAudio2MasteringVoice*          m_masterVoice = nullptr;
    std::string                      m_deviceStatus = "not initialized";
    u32                              m_outSampleRate = 48000;

    // ---- バス ----
    struct Bus
    {
        std::string          name;
        i32                  parent = -1;       // m_buses の添字。master は -1
        u32                  depth  = 0;        // master = 0
        IXAudio2SubmixVoice* voice  = nullptr;  // デバイスが無ければ null（値だけ保持）
        f32  volume    = 1.0f;
        bool muted     = false;
        f32  lowpassHz = 0.0f;
        audio::BusMod snap;                     // スナップショット補正の現在値
        bool builtin   = false;
        // 直近に XAudio2 へ書いた値（同じ値を毎フレーム書かない）
        f32  appliedGain   = -1.0f;
        f32  appliedFilter = -1.0f;
    };
    std::vector<Bus> m_buses;                   // [0] = master。親は必ず子より前に並ぶ
    i32  FindBus(const std::string& name) const;
    i32  ResolveBusOrSfx(const std::string& name);    // 空/未知 → sfx（未知は 1 度だけ警告）
    bool CreateBusVoice(Bus& bus);
    void ApplyBus(Bus& bus, bool force = false);      // volume/mute/snap → SetVolume / フィルタ
    f32  BusEffectiveGain(const Bus& bus) const;
    // ソースボイスの送り先（バス）。デバイスが無ければ null を返す＝呼び出し側は作らない。
    IXAudio2Voice* BusOutputVoice(i32 busIndex) const;
    std::vector<std::string> m_warnedUnknownBus;

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
        i32   bus = -1;             // 送り先バス（m_buses の添字）
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
    // ★★キャッシュした時点のファイル更新時刻。焼き直した wav を反映するために要る。
    //   これが無いと、エディタを起動したまま素材を作り直しても【古い音が鳴り続ける】。
    //   「直したのに何も変わらない」の原因になり、実際に半日ぶん溶かした(2026-08-27)。
    std::unordered_map<std::string, std::filesystem::file_time_type> m_clipStamp;

    std::string m_assetsDir;
    bool m_comInitialized = false;

    // 自動検出されたファイルリスト（assetsDir相対パス）
    std::vector<std::string> m_bgmList;
    std::vector<std::string> m_sfxList;
};

} // namespace dx12e
