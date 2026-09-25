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
//                            ← reverb    … リバーブの戻り（XAudio2 の組み込みリバーブ。音は直接流せない）
//
// リバーブ（送り = send）
//   各ボイスは「自分のバス」と「reverb」の 2 か所へ出力する。reverb への送り量は
//   バスの reverbSend × 音ごとの reverb × 距離（遠いほど響きの割合が増える＝√距離減衰）。
//   さらにバスの音量・ミュート・ローパスを送りにも掛ける（sfx をミュートしたら響きも消える）。
//   響きの性格（プリセット）と量（wet）は、リスナーが居るリバーブ域（AudioReverbZone）から
//   補間して決める。ゾーンの外は setReverb で決めた既定（最初は none = 響き無し）。
//
// ★バスは「子 → 親」の順に処理しないと音が 1 クォンタム遅れる/消える。XAudio2 は
//   ProcessingStage の小さい方から処理し、「自分以下の stage へは送れない」ので、
//   深さに応じて stage を下げている（master が最大）。
// ★旧 API は互換のまま内部でバスへ写す: setMasterVolume = master、setBGMVolume = music、
//   setSFXVolume = sfx。ボイス側はクリップ個別の音量しか持たない（バス音量は掛けない）。
//
// ボイス（1 回の再生）
//   論理ボイスは最大 kMaxLogicalVoices 本。そのうち XAudio2 のソースボイスを実際に持つ
//   （= 実ボイス）のは全体上限 maxVoices 本とバスごとの上限まで。残りは「仮想ボイス」で、
//   音は出さず再生位置だけ進める。聞こえない（遠い / バスがミュート）音も仮想になる。
//   上限に達したら 優先度が低い → 小さく聞こえている → 古い の順に 1 本選んで奪う
//   （ループ音は仮想へ落とすだけで止めない。ワンショットは止める）。
//   BGM は数えない・奪わない・仮想化しない（音楽が消えるのは最悪の壊れ方なので）。
//   音声デバイスが無いときは全部が仮想ボイスになる＝状態は普段どおり追える。
// ===========================================================================
class AudioSystem
{
public:
    static constexpr u32 kMaxLogicalVoices = 128;   // ID の下位 8 bit が添字なので 256 未満
    static constexpr i32 kDefaultPriority  = 128;   // 0..255。大きいほど大事

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
        f32  chainGain       = 1.0f; // master までの積（このバスの音が最終的に何倍になるか）
        u32  voiceLimit = 0;         // 0 = 上限なし（全体上限だけ）
        u32  realVoices = 0;         // このバス以下（子孫含む）で鳴っている実ボイス数
        u32  virtualVoices = 0;
        f32  reverbSend = 0.0f;      // このバスの音をリバーブへどれだけ送るか 0..1
        bool reverbReturn = false;   // reverb（戻り）バス
        bool builtin    = false;     // 既定のバス（消せない・親を変えられない）
    };

    // ボイス 1 本ぶんの読み出し用の写し。
    struct VoiceInfo
    {
        i32         id = -1;
        std::string path;
        std::string bus;
        i32   priority   = kDefaultPriority;
        f32   volume     = 1.0f;     // クリップ個別音量（setVoiceVolume）
        f32   fade       = 1.0f;     // フェードの現在値
        f32   pitch      = 1.0f;
        f32   audibility = 1.0f;     // 最終的な聞こえ具合（音量×フェード×距離×遮蔽×バス）
        bool  spatial    = false;
        f32   distance   = 0.0f;     // リスナーまで（2D は 0）
        f32   distanceGain = 1.0f;
        f32   occlusion  = 0.0f;     // 平滑後の遮蔽量 0..1
        bool  loop       = false;
        bool  isVirtual  = false;
        std::string virtualReason;   // inaudible / limit / stolen / noDevice（実ボイスは空）
        bool  bgm        = false;
        bool  paused     = false;
        f32   positionSec = 0.0f;
        f32   lengthSec   = 0.0f;
        f32   reverbSend  = 0.0f;    // 今リバーブへ送っている量（バス×音ごと×距離×バス音量）
    };

    // リバーブ域 1 つぶんの入力（Application が毎フレーム、リスナー位置から重みを出して渡す）。
    struct ReverbZoneInput
    {
        std::string name;        // エンティティ名（audio_state / ミキサー窓の表示用）
        std::string preset;      // audio::ReverbPresetTable の名前
        f32 weight   = 0.0f;     // 0..1（内側 1、fadeDistance で 0 へ）
        f32 wet      = 0.5f;     // 0..1
        i32 priority = 0;
    };
    struct ReverbState
    {
        bool        available = false;     // リバーブのボイスが作れているか（デバイス無しなら false）
        std::string defaultPreset;         // ゾーンの外（setReverb）
        f32         defaultWet = 0.0f;
        std::vector<ReverbZoneInput> zones;   // 重み > 0 のゾーンだけ
        std::string dominant;              // 一番内側（優先度が高い）で効いているゾーン名。無ければ空
        f32         targetWet  = 0.0f;
        f32         currentWet = 0.0f;
        audio::ReverbParams current;       // 平滑後（I3DL2）
    };

    // 汎用の再生要求（audio:play / AudioSource / 旧 API は全部これへ写す）。
    struct PlayParams
    {
        std::string path;
        std::string bus;                    // 空 = sfx
        i32  priority    = kDefaultPriority;
        f32  volume      = 1.0f;
        f32  pitch       = 1.0f;
        bool loop        = false;
        bool spatial     = false;
        f32  pos[3]      = {0.0f, 0.0f, 0.0f};
        f32  minDistance = 1.0f;
        f32  maxDistance = 30.0f;
        f32  reverb      = 1.0f;            // リバーブへの送りの倍率（バスの reverbSend に掛かる）
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
    // 今鳴っている BGM の assets 相対パス（鳴っていなければ空。ループしない曲が終わっても空に戻る）。
    // ★playBGM は同じパスでも必ず頭出しするので、「シーンをまたいで同じ曲を鳴らし続ける」は
    //   これで判定して呼ばない、という形でしか書けない（曲の途中で切り替わると
    //   イントロへ戻ってしまうため、engine 側で勝手に早期 return はしない）。
    const std::string& GetCurrentBGM() const { return m_currentBGMPath; }
    // ★「ボイスが存在するか」ではなく「実際に鳴っているか」。
    //   以前の実装だと **一度 playBGM したら永久に true** で、一番自然な書き方
    //   `if not audio:isBGMPlaying() then audio:playBGM(...) end` が
    //   stopBGM 後も曲の終了後も二度と鳴らなかった。今は BGM ボイスが生きていて
    //   一時停止していないときだけ true（曲が終わったら false）。
    bool IsBGMPlaying() const;

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
                        const std::string& bus = {}, i32 priority = kDefaultPriority);
    void UpdateSpatialEmitter(i32 slotId, float x, float y, float z);
    // ★鳴っている 1 本を掴んで操作する。PlaySFXSpatial / PlaySFXTracked / Play が返す ID を使う。
    //   ループ再生した環境音を止める・フェードさせる・回転数が落ちるように鳴らす、が
    //   これが無いと書けなかった（StopAllSFX しか無く、他の音まで巻き添えになる）。
    //   世代が食い違う ID（スロットが使い回された後）は黙って無視する。
    // fadeSec > 0 ならその秒数で音量を 0 まで下げてから止める（ブツッと切れない）。
    void StopVoice(i32 slotId, f32 fadeSec = 0.0f);
    void SetVoiceVolume(i32 slotId, float volume);        // 0..1（クリップ個別音量）
    void SetVoicePitch(i32 slotId, float ratio);          // 再生速度＝ピッチ。0.1..2.0
    void SetVoicePriority(i32 slotId, i32 priority);      // 0..255
    // ★仮想ボイス（遠くて聞こえない・上限で押し出された）も「鳴っている」扱い。
    //   ゲームの論理としてはまだ再生中なので、isVoicePlaying で分岐しているコードが壊れない。
    bool IsVoicePlaying(i32 slotId) const;
    // 非空間の SFX を ID 付きで鳴らす（上の操作で掴める）。失敗時 -1。
    i32  PlaySFXTracked(const std::string& filePath, bool loop = false, float volume = 1.0f,
                        const std::string& bus = {}, i32 priority = kDefaultPriority);
    // 汎用の再生口。-1 = 失敗（ファイルが無い / 上限で全部こちらより大事な音が鳴っているワンショット）。
    i32  Play(const PlayParams& p);
    // 遮蔽量 0..1（1=リスナーとの間に壁がある）。ローパスで「こもった」音にし、音量も落とす。
    // 値は Update() 内で時定数 ~0.1s で追従するので、毎フレーム 0/1 を投げてよい。
    void SetOcclusion(i32 slotId, float amount);
    // 実際に使われているリスナー位置（Lua の setListener 上書き込み）。遮蔽レイの終点用。
    void GetListenerPos(float& x, float& y, float& z) const;
    // Play 中だけ: 空間ボイスの距離・定位（X3DAudio の行列）と遮蔽の平滑を再計算する。
    void Update(f32 dt);
    // ★毎フレーム（エディタでも一時停止中でも）: ボイスの終了検出・仮想ボイスの位置送り・
    //   仮想⇔実の入れ替え・フェード・バスの反映。Update（Play 中だけ）とは別に呼ぶこと。
    void Tick(f32 dt);

    // ---- ボイス上限 ----
    void SetMaxVoices(u32 n);                          // 実ボイスの全体上限（1..kMaxLogicalVoices）
    u32  GetMaxVoices() const { return m_maxRealVoices; }
    void SetBusVoiceLimit(const std::string& bus, u32 n);   // 0 = 上限なし。子孫のバスも数える
    u32  GetBusVoiceLimit(const std::string& bus) const;
    void GetVoiceCounts(u32& real, u32& virt) const;   // BGM も含めた実 / 仮想の本数
    std::vector<VoiceInfo> GetVoices() const;          // 生きている論理ボイス全部

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
    void SetBusReverbSend(const std::string& name, f32 send);  // 0..1
    f32  GetBusReverbSend(const std::string& name) const;
    std::vector<BusInfo> GetBuses() const;                      // 親 → 子の順（master が先頭）

    // ---- リバーブ ----
    // ゾーンの外で使う既定の響き。preset は "none" / "room" / "hallway" / "cave" ...（未知は false）。
    bool SetDefaultReverb(const std::string& preset, f32 wet);
    // 今フレームのリバーブ域（重み > 0 のものだけでよい）。Play 中に Application が毎フレーム渡す。
    void SetReverbZones(std::vector<ReverbZoneInput> zones) { m_reverbZones = std::move(zones); }
    ReverbState GetReverbState() const;
    // Play → Stop で呼ぶ: ゲームが設定したミックス（リバーブ域・既定の響き・スナップショット）を
    // 初期状態へ戻す。
    void ResetMixForStop();

    // ---- スナップショット（バスの音量・ローパスの組に名前を付け、時間を掛けて遷移する）----
    // 例: 「追跡中」= 音楽を上げて環境音を絞る / 「隠れている」= 効果音をこもらせる。
    // 値はバスのユーザー設定（setBusVolume / setBusLowpass）に「掛ける」補正なので、
    // オプション画面の音量とぶつからない。載っていないバスは補正なし（1.0 / ローパス無し）。
    // "default" は常にある（全バス補正なし）。
    struct SnapshotBus
    {
        std::string   bus;
        audio::BusMod mod;
    };
    bool DefineSnapshot(const std::string& name, std::vector<SnapshotBus> buses);
    bool SetSnapshot(const std::string& name, f32 seconds);   // 未定義は false
    const std::string& GetSnapshot() const { return m_snapTarget; }
    std::vector<std::string> GetSnapshotNames() const;
    struct SnapshotState
    {
        std::string current;      // 遷移元（遷移が終わっていれば target と同じ）
        std::string target;
        f32         progress = 1.0f;   // 0..1
        f32         duration = 0.0f;
        std::vector<std::string> defined;
    };
    SnapshotState GetSnapshotState() const;
    // デバイスが使えているか（ヘッドレス/音声デバイス無しでは false。状態は保持し続ける）。
    bool IsDeviceReady() const { return m_masterVoice != nullptr; }
    const std::string& GetDeviceStatus() const { return m_deviceStatus; }
    u32  GetOutputChannels() const { return m_outChannels; }
    u32  GetOutputSampleRate() const { return m_outSampleRate; }
    // XAudio2 のボイスを作る/壊すのに掛かった累計時間（仮想化の往復が高く付いていないかを測る）。
    f64  GetVoiceChurnMs() const { return m_voiceChurnMs; }
    u64  GetVoiceChurnCount() const { return m_voiceChurnCount; }

    // assets/audio/ 以下の音声ファイルを自動検出
    const std::vector<std::string>& GetBGMList() const { return m_bgmList; }
    const std::vector<std::string>& GetSFXList() const { return m_sfxList; }
    void ScanAudioFiles();

private:
    // mono=true は空間音用のモノラル版（元のクリップとは別にキャッシュする。
    // ★以前は元のクリップをその場でモノ化していたので、同じ素材を 2D で鳴らしている最中に
    //   空間で鳴らすと、再生中のバッファが解放されていた）。
    std::shared_ptr<AudioClip> GetOrLoadClip(const std::string& filePath, bool mono = false);

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
        audio::BusMod snapFrom, snapTo;         // 遷移の両端
        u32  voiceLimit = 0;
        f32  reverbSend = 0.0f;                 // このバスのボイスがリバーブへ送る量
        bool isReverb  = false;                 // リバーブの戻りバス（ソースを直接つながない）
        bool builtin   = false;
        f32  chainGain = 1.0f;                  // Tick で更新（master までの積）
        f32  sendChain = 1.0f;                  // master を除いた積（リバーブ送りに掛ける）
        f32  sendLowpass = 0.0f;                // master を除いた経路のローパス（送りに掛ける）
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
    bool BusInSubtree(i32 bus, i32 root) const;
    void UpdateBusChainGains();
    // ソースボイスの送り先（バス）。デバイスが無ければ null を返す＝呼び出し側は作らない。
    IXAudio2Voice* BusOutputVoice(i32 busIndex) const;
    std::vector<std::string> m_warnedUnknownBus;

    // ---- ボイス ----
    struct Voice
    {
        bool  active     = false;
        u32   generation = 0;
        u64   order      = 0;              // 鳴らし始めた順（奪う相手の同点決着）
        std::string path;
        std::shared_ptr<AudioClip> clip;   // ★再生中は必ず握る（キャッシュが入れ替わっても解放させない）
        u32   sampleRate = 44100;
        u32   channels   = 1;
        u64   totalFrames = 0;
        i32   bus        = -1;
        i32   priority   = kDefaultPriority;
        f32   clipVolume = 1.0f;
        f32   pitch      = 1.0f;
        bool  loop       = false;
        bool  spatial    = false;
        bool  bgm        = false;
        bool  paused     = false;
        f32   minDist    = 1.0f;
        f32   maxDist    = 30.0f;
        f32   pos[3]     = {0.0f, 0.0f, 0.0f};
        // 遮蔽（壁越し）。target が呼び出し側の指定、occ が時間平滑した実効値。
        f32   occTarget  = 0.0f;
        f32   occ        = 0.0f;
        f32   distance   = 0.0f;
        f32   distGain   = 1.0f;
        // フェード（stopVoice(id, sec)）
        f32   fade       = 1.0f;
        f32   fadeTarget = 1.0f;
        f32   fadeSpeed  = 0.0f;           // 1 秒あたりの変化量（0 = 止まっている）
        bool  stopAtFadeEnd = false;
        f32   audibility = 1.0f;
        f32   reverbMul  = 1.0f;           // PlayParams::reverb
        f32   sendLevel  = 0.0f;           // 直近のリバーブ送り量
        bool  hasReverbSend = false;       // 実ボイスが reverb へも送っているか
        // XAudio2 側
        IXAudio2SourceVoice* src = nullptr;   // null = 仮想ボイス
        const char* virtualReason = "";
        f64   virtualPos = 0.0;            // 仮想中の再生位置（フレーム）
        u64   startFrame = 0;              // src を作ったときの開始フレーム
        f32   appliedVolume = -1.0f;
        f32   appliedFilter = -1.0f;
        f32   appliedSend   = -1.0f;
        f32   appliedSendFilter = -1.0f;
    };
    std::array<Voice, kMaxLogicalVoices> m_voices{};
    u64  m_voiceOrder = 0;
    u32  m_maxRealVoices = 32;
    i32  m_bgmId = -1;
    std::string m_currentBGMPath;
    bool m_limitWarned = false;
    f64  m_voiceChurnMs = 0.0;
    u64  m_voiceChurnCount = 0;

    i32    MakeId(u32 index) const;
    Voice* Resolve(i32 id);
    const Voice* Resolve(i32 id) const;
    i32    PlayInternal(const PlayParams& p, bool bgm);
    i32    AllocateSlot(i32 newPriority, f32 newAudibility);
    void   FreeVoice(Voice& v);                        // 実ボイスなら壊して論理ボイスも空ける
    bool   MakeReal(Voice& v, f64 startFrame);         // 仮想 → 実（XAudio2 のボイスを作って鳴らす）
    void   MakeVirtual(Voice& v, const char* reason);  // 実 → 仮想（位置を控えてボイスを壊す）
    f64    CurrentFrame(const Voice& v) const;         // 再生位置（フレーム）
    f32    ComputeAudibility(const Voice& v) const;
    // v を実ボイスにしてよいか（全体上限 / バス上限）。ダメなら奪ってよい相手を *victim に返す。
    bool   HasRoomFor(const Voice& v, f32 margin, i32* victim);
    u32    CountReal(i32 busRoot) const;               // busRoot < 0 = 全体（BGM は数えない）
    void   ApplyVoiceGain(Voice& v);                   // 音量・遮蔽ローパス
    void   ComputeAndApply(Voice& v);                  // X3DAudio の定位（実ボイスのみ）
    void   UpdateDistance(Voice& v);
    void   RestartVoiceAt(Voice& v, f64 frame);        // シーク

    // ---- スナップショット ----
    std::unordered_map<std::string, std::vector<SnapshotBus>> m_snapshots;
    std::string m_snapCurrent = "default";
    std::string m_snapTarget  = "default";
    f32  m_snapElapsed  = 0.0f;
    f32  m_snapDuration = 0.0f;
    bool m_snapMoving   = false;
    void UpdateSnapshot(f32 dt);

    // ---- リバーブ ----
    i32  m_reverbBus = -1;                 // m_buses の添字
    u32  m_reverbInCh = 2, m_reverbOutCh = 2, m_reverbRate = 48000;
    bool m_reverbReady = false;
    std::string m_defaultReverbPreset = "none";
    f32  m_defaultReverbWet = 0.0f;
    std::vector<ReverbZoneInput> m_reverbZones;
    audio::ReverbParams m_reverbCur, m_reverbTarget, m_reverbApplied;
    f32  m_reverbWetCur = 0.0f, m_reverbWetTarget = 0.0f;
    std::string m_reverbDominant;
    bool CreateReverbVoice(Bus& bus);
    void UpdateReverb(f32 dt);
    void ApplyReverbParams(bool force);

    // X3DAudio
    X3DAUDIO_HANDLE   m_x3d{};
    X3DAUDIO_LISTENER m_listener{};
    bool  m_listenerPosOverride = false;   // Lua が SetListenerPos した後はカメラ位置より優先
    float m_lopX = 0, m_lopY = 0, m_lopZ = 0;
    u32  m_outChannels = 2;
    bool m_x3dReady = false;

    // Clip cache
    std::unordered_map<std::string, std::shared_ptr<AudioClip>> m_clipCache;
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
