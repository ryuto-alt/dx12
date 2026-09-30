#pragma once
// 編集操作 SeqOp: すべての編集(UI / MCP / Lua / Undo)がこの 1 本の経路を通る。
// ・各 op は「適用」と「逆 op の生成」を持つ(ApplyOp が inverse を返す)。逆 op を適用すると元とバイト一致に戻る。
// ・op は自己完結(ID・添字・値を全部運ぶ)。ID は呼び出し側が IdAllocator で先に決める → Redo / リプレイが決定的。
// ・失敗した op はシーケンスを 1 バイトも変えない。SeqTxn(複数 op)は全部成功か全部巻き戻し。
// 一覧と意味論: docs/DXSEQ_FORMAT.md §SeqOp

#include <string>
#include <variant>
#include <vector>

#include "sequencer/SeqModel.h"

namespace dx12e::seq
{

// index の -1 は「既定位置」(配列末尾 / キーは時刻順の挿入位置)。それ以外は厳密な添字(範囲検査あり)。

// --- シーケンス自体 -----------------------------------------------------------
struct OpSetName      { std::string name; };
struct OpSetFrameRate { int fps = 30; };
struct OpSetRange     { bool has = false; Tick start = 0; Tick end = 0; };
struct OpSetRender    { std::optional<RenderSettings> render; };
struct OpSetMeta      { SeqValue meta; };

// --- バインディング -----------------------------------------------------------
struct OpAddBinding       { Binding binding; int index = -1; };
struct OpDeleteBinding    { std::string id; };
struct OpMoveBinding      { std::string id; int toIndex = 0; };            // toIndex = 移動後の添字
struct OpSetBindingHeader { std::string id; BindingHeader header; };

// --- トラック -----------------------------------------------------------------
struct OpAddTrack       { std::string bindingId; Track track; int index = -1; };
struct OpDeleteTrack    { std::string trackId; };
struct OpMoveTrack      { std::string trackId; std::string toBindingId; int toIndex = 0; };   // toIndex = 移動後の添字
struct OpSetTrackHeader { std::string trackId; TrackHeader header; };

// --- チャンネル ---------------------------------------------------------------
struct OpAddChannel       { std::string trackId; Channel channel; int index = -1; };
struct OpDeleteChannel    { std::string trackId; std::string channel; };
struct OpSetChannelExtrap { std::string trackId; std::string channel; Extrap pre = Extrap::Hold; Extrap post = Extrap::Hold; };

// --- キー ---------------------------------------------------------------------
struct OpAddKey      { std::string trackId; std::string channel; Key key; int index = -1; };
struct OpDeleteKey   { std::string trackId; std::string channel; int index = 0; };
// 時刻だけ動かす(値/補間は保つ)。newIndex = -1 なら新しい時刻の並び順の位置(同時刻の後ろ)
struct OpMoveKey     { std::string trackId; std::string channel; int index = 0; Tick newT = 0; int newIndex = -1; };
struct OpSetKeyValue { std::string trackId; std::string channel; int index = 0; double v = 0.0; };
// 補間を変える。Bezier へ切り替えるとき、既存ハンドルが無ければ Auto と同じ見た目の既定ハンドルが入る
struct OpSetInterp   { std::string trackId; std::string channel; int index = 0; Interp ip = Interp::Auto; EaseId ease = EaseId::Linear; };
// Bezier キーのハンドルを設定する(Bezier 以外のキーには使えない)
struct OpSetTangent  { std::string trackId; std::string channel; int index = 0; Tick inDt = 0; double inDv = 0.0; Tick outDt = 0; double outDv = 0.0; };
// キーの全フィールド差し替え(時刻は同じであること。時刻を動かすなら OpMoveKey)
struct OpSetKey      { std::string trackId; std::string channel; int index = 0; Key key; };

// --- クリップ / イベント ------------------------------------------------------
struct OpAddClip     { std::string trackId; Clip clip; int index = -1; };
struct OpDeleteClip  { std::string trackId; std::string clipId; };
struct OpSetClip     { std::string trackId; Clip clip; };                   // clip.id で対象を特定し丸ごと差し替え(移動/伸縮/パラメータ)
struct OpAddEvent    { std::string trackId; EventItem event; int index = -1; };
struct OpDeleteEvent { std::string trackId; std::string eventId; };
struct OpSetEvent    { std::string trackId; EventItem event; };

// --- マーカー / カット --------------------------------------------------------
struct OpAddMarker    { Marker marker; int index = -1; };
struct OpDeleteMarker { std::string id; };
struct OpSetMarker    { Marker marker; };
struct OpAddCut       { Cut cut; int index = -1; };
struct OpDeleteCut    { std::string id; };
struct OpSetCut       { Cut cut; };

using SeqOp = std::variant<
    OpSetName, OpSetFrameRate, OpSetRange, OpSetRender, OpSetMeta,
    OpAddBinding, OpDeleteBinding, OpMoveBinding, OpSetBindingHeader,
    OpAddTrack, OpDeleteTrack, OpMoveTrack, OpSetTrackHeader,
    OpAddChannel, OpDeleteChannel, OpSetChannelExtrap,
    OpAddKey, OpDeleteKey, OpMoveKey, OpSetKeyValue, OpSetInterp, OpSetTangent, OpSetKey,
    OpAddClip, OpDeleteClip, OpSetClip, OpAddEvent, OpDeleteEvent, OpSetEvent,
    OpAddMarker, OpDeleteMarker, OpSetMarker, OpAddCut, OpDeleteCut, OpSetCut>;

// op の名前(ログ・MCP・Undo の表示用)
const char* OpName(const SeqOp& op);

// 複数 op をひとまとめにした単位(= Undo の 1 ステップ)
struct SeqTxn
{
    std::string label;
    std::vector<SeqOp> ops;
};

struct ApplyResult
{
    bool ok = true;
    std::string error;   // 日本語。ok のとき空
    explicit operator bool() const { return ok; }
};

// op を適用する。inverse が非 null なら、成功時に「これを適用すると元に戻る」逆 op を入れる。
// 失敗時はシーケンスを一切変更せず、inverse も触らない。
ApplyResult ApplyOp(Sequence& seq, const SeqOp& op, SeqOp* inverse = nullptr);

// txn を先頭から適用する。途中で失敗したら、そこまでの op を逆順に巻き戻して(= 元と一致)エラーを返す。
// 成功時の inverse は「逆 op を逆順に並べた txn」。
ApplyResult ApplyTxn(Sequence& seq, const SeqTxn& txn, SeqTxn* inverse = nullptr);

// ---------------------------------------------------------------------------
// Undo / Redo
// ---------------------------------------------------------------------------
class SeqHistory
{
public:
    explicit SeqHistory(std::size_t maxEntries = 1000) : m_max(maxEntries) {}

    // 適用して履歴に積む(成功時のみ)。Redo 側は捨てる。
    ApplyResult Execute(Sequence& seq, SeqTxn txn);
    // すでに適用済みの変更を履歴に積む(ドラッグ中は ApplyTxn で直接動かして inverse を溜め、離したときに 1 ステップにまとめる用途)。
    // forward = 適用した txn / inverse = ApplyTxn が返した逆 txn。Redo 側は捨てる。
    void Record(SeqTxn forward, SeqTxn inverse);
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    ApplyResult Undo(Sequence& seq);
    ApplyResult Redo(Sequence& seq);
    void Clear() { m_undo.clear(); m_redo.clear(); }
    std::size_t UndoCount() const { return m_undo.size(); }
    std::size_t RedoCount() const { return m_redo.size(); }
    const std::string* UndoLabel() const { return m_undo.empty() ? nullptr : &m_undo.back().forward.label; }
    const std::string* RedoLabel() const { return m_redo.empty() ? nullptr : &m_redo.back().forward.label; }

private:
    struct Entry { SeqTxn forward; SeqTxn inverse; };
    std::vector<Entry> m_undo;
    std::vector<Entry> m_redo;
    std::size_t m_max;
};

} // namespace dx12e::seq
