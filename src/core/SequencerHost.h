#pragma once
// シーケンサー S1b: ホスト(.dxseq の文書・エディタの非破壊スクラブ・Play 中の再生・描画側への出力)。
//
//   Application が 1 つ持つ。Application のフレーム更新から Update() を 1 回呼ぶ(エディタ = m_scene->Update の前 /
//   Play = Lua・Trigger・AI の後・カメラ同期の前)。GPU / ImGui / Application に依存しない(テストからそのまま使える)。
//
//   エディタ(HostMode::Editor)
//     ・EditorScrub(doc, t): 文書 doc を時刻 t で評価してシーンへ書く。書く前に PreAnimatedState が元値を退避する。
//       イベントは発火しない(スクラブ / エディタ再生では発火しない。設計書 §3.4)。
//     ・元へ戻すタイミング(すべて RestoreAll → PreAnimatedState が空になる)
//         (a) EditorEnd()  (b) 文書を閉じる(CloseDoc)  (c) Play 開始の直前(EditorEnd を EnterPlayMode が呼ぶ)
//         (d) あらゆる保存の直前 = SceneSerializer の直列化フック(Save / SaveToString / SerializeEntity / SerializeSubtree)
//       (d) は保存後にセッションが生きていれば、フレーム末(PostUpdate)に再適用する(ちらつかない)。
//   Play(HostMode::Playing)
//     ・Lua `Sequence.play` / MCP `sequence_play` / シーンの自動再生(Scene::GetSequenceAutoPlay)で RuntimePlayer が増える。
//       既定の時計は実時間(タイムスケール非適用)。エンティティの値は戻さない(Stop でシーンごと復元される)。
//     ・イベント(emit / lua / log / loadScene)は前進で 1 回だけ発火する(S0 の CollectEvents の規則)。
//
//   ポスト / DoF / カメラの揺れ / アクティブカメラの選択は「書かない」。CutCameraEntity() / ApplyPostOverrides() / ShakeFor() で
//   描画側が読み、描画時のコピーに上書きする(シーンを汚さない)。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "core/SequenceLuaApi.h"
#include "core/SequencerApply.h"
#include "engine/core/EventBus.h"
#include "sequencer/SeqEval.h"
#include "sequencer/SeqModel.h"
#include "sequencer/SeqOpJson.h"
#include "sequencer/SeqOps.h"
#include "sequencer/SeqSerialize.h"

namespace dx12e
{
class Scene;
struct PostProcessSettings;

namespace seqhost
{

enum class HostMode : std::uint8_t { Editor, Playing };

// エンジン側の窓口(Application が差し込む。テストでは差し替える)。どれも null 許容。
struct HostCallbacks
{
    std::function<void(const EngineEvent&)> emit;                                                        // events:emit と同じバス
    std::function<bool(const std::string& fn, const std::vector<EngineEvent::Value>& args, std::string& err)> callLua;   // Lua のグローバル関数
    std::function<void(const std::string& scenePath)> loadScene;                                         // イベント kind:"loadScene"
    std::function<void(float)> setTimeScale;                                                             // timeScale トラック(Play 中だけ)
    std::function<float()> getTimeScale;
    std::function<void(const std::string& eventName, std::function<void(const EngineEvent&)> fn)> subscribe;   // <name>:play / :stop の購読
};

// 開いている文書(1 つの .dxseq)。編集は必ず SeqOp(ApplyOps / ApplyTxn)を通す。
struct SeqDoc
{
    std::string name;            // 識別名(ファイルの stem。ファイルが無いなら Sequence::name)
    std::string rel;             // assets 相対パス("sequences/Intro.dxseq")。空 = ファイル無し(メモリ上だけ)
    seq::Sequence seq;
    seq::SeqHistory history;
    seq::IdAllocator ids;
    bool dirty = false;
    bool onDisk = false;
    std::uint64_t revision = 1;  // 編集 / 差し替えのたびに +1(再束縛・再評価のきっかけ)
    std::string savedText;       // 最後に保存 / 読み込んだ正準形(dirty の判定に使う)
};
using DocPtr = std::shared_ptr<SeqDoc>;

struct EditResult
{
    bool ok = false;
    std::string error;
    int applied = 0;                       // 適用した op 数
    std::string label;
    std::vector<std::string> opNames;
    bool dryRun = false;
};

// 評価のスナップショット(書き込みなし)。sequence_eval が返す。
struct EvalReport
{
    seq::Tick t = 0;
    seq::EvalResult res;
    BoundSequence bound;         // 現在のシーンへの束縛(未解決の状況)
    bool sceneAvailable = false;
};

struct EditorStatus
{
    bool active = false;
    std::string doc;
    seq::Tick t = 0;
    bool playing = false;
    std::size_t preAnimated = 0;   // 退避中の値の数
    int userDrift = 0;             // 直近の適用で「制御中の Transform をユーザーが触った」と検出した数
    std::vector<std::string> warnings;
    bool cutActive = false;
    entt::entity cutCamera = entt::null;
};

struct PlayerInfo
{
    int id = 0;
    std::string name;
    double timeSec = 0.0;
    double durationSec = 0.0;
    bool paused = false;
    bool loop = false;
    double rate = 1.0;
    bool clockGame = false;
    bool finished = false;
};

class SequencerHost final : public ISequenceLuaApi
{
public:
    SequencerHost();
    ~SequencerHost() override;
    SequencerHost(const SequencerHost&) = delete;
    SequencerHost& operator=(const SequencerHost&) = delete;

    // ---- 設定 ------------------------------------------------------------------
    void SetCallbacks(HostCallbacks cb) { m_cb = std::move(cb); }
    void SetScene(Scene* scene) { m_scene = scene; }
    Scene* GetScene() const { return m_scene; }
    // テスト・特殊用途: assets の場所を固定する(空 = PathResolver / vfs に任せる)。末尾 '/' は自動で補う。
    void SetAssetsDirOverride(std::string dir);
    // SceneSerializer の直列化フックを自分に結ぶ / 外す。デストラクタは自分が結んでいれば外す。
    void InstallSaveHook();
    void UninstallSaveHook();

    // ---- 文書 ------------------------------------------------------------------
    // 名前("Intro")または assets 相対パス("sequences/Intro.dxseq")→ 正準な相対パスと識別名
    static std::string RelPathFor(const std::string& nameOrRel);
    static std::string KeyFor(const std::string& nameOrRel);

    DocPtr FindDoc(const std::string& nameOrRel) const;
    // 読み込む(既に開いていればそれを返す。forceReload なら読み直す = 未保存の編集は失う)。失敗は nullptr + err。
    DocPtr LoadDoc(const std::string& nameOrRel, std::string& err, bool forceReload = false);
    // 空の文書を作る(まだファイルは作らない)。同名が開いていれば失敗。
    DocPtr NewDoc(const std::string& name, int fps, std::string& err);
    // テキスト(.dxseq)からメモリ上の文書を作る(ファイルを介さない。テスト / 変換ツール用)。
    DocPtr AddDocFromText(const std::string& name, const std::string& text, std::string& err);
    // 保存(アトミック)。relOverride があればそこへ(以後その名前になる)。エディタ専用(ゲームモードの pak は書けない)。
    bool SaveDoc(const DocPtr& doc, std::string& err, const std::string& relOverride = {});
    // 文書を閉じる。編集セッションがこの文書なら先に EditorEnd(元へ戻す)。再生中のプレイヤーは止める。
    bool CloseDoc(const std::string& nameOrRel, std::string& err);
    std::vector<DocPtr> OpenDocs() const;
    // assets/sequences/ 直下(再帰)の .dxseq の相対パス一覧(ディスクモード = エディタだけ)
    std::vector<std::string> ListAssets() const;

    // ---- 編集(すべて SeqOp。UI / MCP / Lua が同じ経路)-----------------------------
    // op の配列(JSON。SeqOpJson.h)を 1 つの Undo ステップとして適用する。dryRun は文書を変えずに検査だけ。
    // addBinding の binding は "entity": "<名前>" または <エンティティ ID> を書けば hint を現在のシーンから作る(guid を確定させる)。
    EditResult ApplyOps(const DocPtr& doc, std::string_view opsJson, bool dryRun = false);
    // 作り済みの txn を適用する(UI から)。Undo に載る。
    EditResult ApplyTxn(const DocPtr& doc, seq::SeqTxn txn);
    // UI がドラッグ中に ApplyTxn(seq, …) で直接動かしたあと / SeqHistory を自分で持つ場合に、変更を知らせる(revision を進めて再評価させる)。
    void NotifyDocChanged(const DocPtr& doc);
    EditResult Undo(const DocPtr& doc);
    EditResult Redo(const DocPtr& doc);

    // ---- エディタ: 非破壊のスクラブ / プレビュー再生 --------------------------------
    // 文書を時刻 t(ティック)で評価してシーンへ書く。初回は編集セッションを開く(別の文書のセッションがあれば先に戻す)。
    // Editor モード限定。events は発火しない。
    bool EditorScrub(const DocPtr& doc, seq::Tick t, std::string& err);
    // プレビュー再生(エディタの時計 = Update の dt)。位置は今のプレイヘッドから。loop = 範囲をループ。
    bool EditorPlay(const DocPtr& doc, bool loop, double rate, std::string& err);
    void EditorPause();
    // セッションを終える(元へ戻す)。(a) スクラブ終了 (b) シーケンスを閉じる (c) Play 開始の直前 が呼ぶ。
    void EditorEnd();
    bool EditorActive() const { return m_ed.active; }
    EditorStatus GetEditorStatus() const;
    // 直列化の直前フックの実体(SceneSerializer から呼ばれる)。担当外の registry なら何もしない。
    void RestoreForSave(const entt::registry& reg);

    // ---- 非破壊の評価(何も書かない・guid も確定しない)----------------------------------
    bool EvalAt(const DocPtr& doc, seq::Tick t, EvalReport& out, std::string& err) const;
    // 現在のシーンへの束縛の状況だけ(書かない)
    bool DescribeBindings(const DocPtr& doc, BoundSequence& out) const;

    // ---- Play 中の再生 --------------------------------------------------------------
    int PlayRuntime(const DocPtr& doc, const SequencePlayOptions& opt, std::string& err);
    // name = シーケンス名 / "#<id>" / "*"(全部)
    bool StopRuntime(const std::string& nameOrId);
    bool PauseRuntime(const std::string& name, bool paused);
    bool SeekRuntime(const std::string& name, double seconds);
    std::vector<PlayerInfo> Players() const;
    bool AnyPlaying() const { return !m_players.empty(); }

    // ---- フレーム更新 -----------------------------------------------------------------
    // dt = ゲームの dt(GameClock。タイムスケール非適用の実時間。固定 dt のステップならその固定値)。paused = Play の一時停止(F1)。
    // sceneToken = シーンが作り直されたら変わる値(open_scene / Stop / ランタイムのシーン切替)。
    void Update(float dt, HostMode mode, bool paused, std::uint64_t sceneToken);
    // フレーム末: 保存の直前に戻された分を再適用する(ちらつき防止)。
    void PostUpdate();
    // シーン内のエンティティが増減・改名した合図(次の Update で束縛し直す)。Update がエンティティ数の増減は自分で検出する。
    void InvalidateBindings() { m_bindingsDirty = true; }

    // ---- 描画側への出力(毎フレームの Update の結果。エディタのセッションと Play の再生を合成したもの)----------
    entt::entity CutCameraEntity() const { return m_out.cutCamera; }
    bool HasPostOverrides() const { return !m_out.post.empty(); }
    // 描画時のコピー(ppApplied)へ上書きする。何か上書きしたら true。cameraView=false(エディタの自由カメラ)は DoF を除く。
    bool ApplyPostOverrides(PostProcessSettings& pp, bool cameraView = true) const;
    // このカメラに足すシェイクのオフセット(ワールド座標)。無ければ false。
    bool ShakeFor(entt::entity camera, float& x, float& y, float& z) const;
    const ApplyOutput& LastOutput() const { return m_out; }

    // ---- ISequenceLuaApi(Lua の Sequence.*)---------------------------------------------
    int LuaPlay(const std::string& name, const SequencePlayOptions& opt, std::string& err) override;
    bool LuaStop(const std::string& nameOrId) override { return StopRuntime(nameOrId); }
    bool LuaPause(const std::string& name, bool paused) override { return PauseRuntime(name, paused); }
    bool LuaSeek(const std::string& name, double seconds) override { return SeekRuntime(name, seconds); }
    bool LuaIsPlaying(const std::string& name) const override;
    double LuaDuration(const std::string& name) override;
    double LuaTime(const std::string& name) const override;

    // 検査・統計(テスト用)
    std::size_t PreAnimatedCount() const { return m_pre.Count(); }
    std::uint64_t ApplyCount() const { return m_applyCount; }

private:
    struct EditorSession
    {
        DocPtr doc;
        bool active = false;
        seq::Tick t = 0;
        bool playing = false;
        bool needsApply = false;
        seq::SeqPlayback pb;
        BoundSequence bound;
        std::uint64_t boundRevision = 0;
        std::uint64_t boundToken = ~0ull;
        bool bound_ok = false;
        seq::EvalResult res;
        ApplyOutput out;
        int userDrift = 0;
        std::vector<std::string> warnings;
    };

    struct RuntimePlayer
    {
        int id = 0;
        std::string name;
        std::shared_ptr<const seq::Sequence> seq;    // 再生開始時の複製(再生中に文書が編集されても揺れない)
        seq::SeqPlayback pb;
        seq::LoopMode mode = seq::LoopMode::Once;
        double rate = 1.0;
        bool clockGame = false;
        bool restoreOnEnd = true;
        bool paused = false;
        bool finished = false;
        double delayLeft = 0.0;
        BoundSequence bound;
        std::uint64_t boundToken = ~0ull;
        bool boundOk = false;
        seq::EvalResult res;
        ApplyOutput out;
        bool wroteTimeScale = false;
        float timeScaleBefore = 1.0f;
        std::vector<seq::EventFire> fired;
    };

    void ApplyEditorNow();
    void UpdateEditor(float dt);
    void UpdateRuntime(float dt, bool paused);
    void FireEvents(RuntimePlayer& p);
    void FireOne(const seq::Sequence& s, const seq::EventItem& ev);
    void FinishPlayer(RuntimePlayer& p);
    void OnEnterPlaying();
    void OnLeavePlaying();
    void RebuildOutputs();
    std::string ReadAssetText(const std::string& rel, bool& found) const;
    std::string AssetsDir() const;
    DocPtr MakeDoc(const std::string& name, const std::string& rel, seq::Sequence&& s, bool onDisk);
    void TouchDoc(const DocPtr& doc);
    int PlayerIndexByName(const std::string& name) const;
    bool PreprocessOpsJson(const DocPtr& doc, std::string& jsonText, std::string& err);

    HostCallbacks m_cb;
    Scene* m_scene = nullptr;
    std::string m_assetsOverride;
    bool m_hookInstalled = false;

    std::vector<DocPtr> m_docs;
    PreAnimatedState m_pre;
    EditorSession m_ed;
    std::vector<std::unique_ptr<RuntimePlayer>> m_players;
    int m_nextPlayerId = 1;

    HostMode m_prevMode = HostMode::Editor;
    std::uint64_t m_token = ~0ull;
    bool m_bindingsDirty = false;
    std::uint64_t m_frame = 0;
    std::uint64_t m_applyCount = 0;
    bool m_inApply = false;
    std::unordered_set<std::string> m_warnedKinds;

    ApplyOutput m_out;   // 合成した出力(描画側が読む)
};

} // namespace seqhost
} // namespace dx12e
