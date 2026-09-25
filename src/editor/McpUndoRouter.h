#pragma once

// MCP（AI）の編集を Undo に積む窓口 + トランザクション。
//
// ■ なぜ要るか
//   以前の MCP の編集はほぼ Undo に積まれず（group_entities と生成/削除の一部だけ）、
//   dx12_undo（エンジンの undo）はスタックの一番上＝**人の編集**を戻してしまうことがあった。
//   batch が途中で失敗しても巻き戻らず、安全網はオートセーブとバックアップだけだった。
//
// ■ 仕組み（3 段）
//   1) 呼び出し 1 回 = 1 エントリ。BeginCall 〜 EndCall の間に UndoSystem へ積まれた物を
//      横取りし（UndoSystem::SetCaptureSink）、AddToCall で足された値スナップショットと合わせて
//      「AI: <method>」という名前の AiUndoEntry 1 個にまとめて積む。
//      ★既存の積み口（グループ化 / 地形ストローク / 生成・削除の遅延処理）は書き換えずに済み、
//        しかも二重に積まれない（横取りした物がそのまま中身になる）。
//   2) トランザクション。Begin 〜 Commit の間の呼び出しは 1 個の「AI: <label>」へ入れて
//      Commit で 1 エントリとして積む。Rollback は中身を逆順に Undo して捨てる。入れ子は禁止。
//   3) AI の undo / redo。onlyAi=true（既定）なら、スタックの一番上が人の操作のとき戻さずに断る。
//
// ■ このヘッダはエンジンに依存しない（UndoCore.h だけ）。時刻は呼び出し側が秒で渡す。
//   tests/mcp_undo_test.cpp がまとめ方と rollback の順序をここだけで検証している。

#include <memory>
#include <string>
#include <vector>
#include "editor/UndoCore.h"

namespace dx12e
{

// 閉じたトランザクションの記録（transaction_status が「なぜ閉じたか」を返すため）。
struct McpTxClosed
{
    std::string label;
    // "commit" / "rollback" / 自動で閉じた理由（"idle_timeout" / "scene_changed" / "play" /
    // "human_undo"）。自動で閉じたときは「確定扱い」＝変更は残り、1 エントリとして積まれている。
    std::string reason;
    size_t      calls = 0;
    double      at    = 0.0;
};

class McpUndoRouter
{
public:
    explicit McpUndoRouter(UndoSystem& undo) : m_undo(undo) {}
    McpUndoRouter(const McpUndoRouter&) = delete;
    McpUndoRouter& operator=(const McpUndoRouter&) = delete;

    // ================= 1 回の MCP 呼び出し =================
    // 入れ子にしない前提だが、万一入れ子になっても外側 1 回ぶんにまとめる（内側は数えるだけ）。
    void BeginCall(std::string method)
    {
        if (m_callDepth++ > 0) return;
        m_callMethod = std::move(method);
        m_captured.clear();
        m_undo.SetCaptureSink(&m_captured);
    }

    // 呼び出し中に直接足す（ハンドラの前後で取った値スナップショット由来のコマンド）。
    // 呼び出し外なら単独の AI エントリとして積む。
    void AddToCall(std::unique_ptr<IUndoCommand> cmd)
    {
        if (!cmd) return;
        if (m_callDepth > 0) { m_captured.push_back(std::move(cmd)); return; }
        std::vector<std::unique_ptr<IUndoCommand>> one;
        one.push_back(std::move(cmd));
        Route(std::move(one), "AI");
    }

    // 呼び出しを終える。1 件でも積んだら true。失敗した呼び出しでも途中まで変わった分は積む
    // （変更が起きたのに Undo できない、を作らない）。
    bool EndCall(double now)
    {
        if (m_callDepth == 0) return false;
        if (--m_callDepth > 0) return false;
        m_undo.SetCaptureSink(nullptr);
        if (m_captured.empty()) return false;
        std::vector<std::unique_ptr<IUndoCommand>> cmds;
        cmds.swap(m_captured);
        Route(std::move(cmds), m_callMethod);
        if (m_tx) m_tx->lastActivity = now;
        return true;
    }
    bool InCall() const { return m_callDepth > 0; }

    // ================= トランザクション =================
    enum class TxStatus { Ok, AlreadyOpen, NotOpen };

    struct TxResult
    {
        std::string label;
        std::string entryName;     // 積んだエントリの名前（"AI: <label>"）。空 = 何も積んでいない
        size_t      calls = 0;     // 中身の呼び出し数（= 積まれていた AI エントリ数）
        u64         humanPushes = 0;   // begin 以降に人がスタックへ積んだ件数
        bool        pushed = false;
    };

    bool TxOpen() const { return m_tx != nullptr; }

    TxStatus Begin(std::string label, double now)
    {
        if (m_tx) return TxStatus::AlreadyOpen;
        if (label.empty()) label = "transaction";
        m_tx = std::make_unique<Tx>();
        m_tx->label        = label;
        m_tx->entry        = std::make_unique<AiUndoEntry>("AI: " + label);
        m_tx->openedAt     = now;
        m_tx->lastActivity = now;
        m_tx->pushSeqAtBegin = m_undo.PushSeq();
        return TxStatus::Ok;
    }

    // 中身を 1 エントリとしてスタックへ積んで閉じる。空なら何も積まない。
    TxStatus Commit(double now, TxResult* out, const char* reason = "commit")
    {
        if (!m_tx) return TxStatus::NotOpen;
        TxResult r;
        r.label       = m_tx->label;
        r.calls       = m_tx->entry->Size();
        r.humanPushes = m_undo.PushSeq() - m_tx->pushSeqAtBegin;
        if (!m_tx->entry->Empty())
        {
            r.entryName = m_tx->entry->GetName();
            r.pushed    = true;
            m_undo.PushUncaptured(std::move(m_tx->entry));
        }
        m_lastClosed = McpTxClosed{r.label, reason, r.calls, now};
        m_tx.reset();
        if (out) *out = std::move(r);
        return TxStatus::Ok;
    }

    // 中身を逆順に Undo して捨てる（スタックにも redo にも残さない）。
    // ★エンティティの復元（削除の取り消し）はモデルの再読み込みを伴うので、エンジン側は
    //   フレーム境界（cmdList が有効な所）でこれを呼ぶこと。
    TxStatus Rollback(double now, TxResult* out)
    {
        if (!m_tx) return TxStatus::NotOpen;
        TxResult r;
        r.label       = m_tx->label;
        r.calls       = m_tx->entry->Size();
        r.humanPushes = m_undo.PushSeq() - m_tx->pushSeqAtBegin;
        m_tx->entry->Undo();
        m_lastClosed = McpTxClosed{r.label, "rollback", r.calls, now};
        m_tx.reset();
        if (out) *out = std::move(r);
        return TxStatus::Ok;
    }

    // 自動で閉じる＝確定扱い（変更は残し、1 エントリとして積む）。開いていなければ false。
    // ★巻き戻しにしない理由: 画面に見えている作業（オートセーブで既にディスクへ書かれている
    //   こともある）を、誰も指示していないのに黙って消すことになるから。確定しておけば
    //   Ctrl+Z / undo 1 回で丸ごと戻せる。
    bool AutoClose(const char* reason, double now)
    {
        if (!m_tx) return false;
        Commit(now, nullptr, reason);
        return true;
    }

    // 最後の MCP 活動から idleSec 秒たっていたら自動で閉じる。閉じたら true。
    bool CloseIfIdle(double now, double idleSec)
    {
        if (!m_tx || now - m_tx->lastActivity < idleSec) return false;
        return AutoClose("idle_timeout", now);
    }

    // MCP の活動（読み取りも含む）。放置タイマーを延ばす。
    void Touch(double now) { if (m_tx) m_tx->lastActivity = now; }

    // ---- 状態（transaction_status）----
    const std::string& TxLabel() const { static const std::string k; return m_tx ? m_tx->label : k; }
    size_t TxCalls() const { return m_tx ? m_tx->entry->Size() : 0; }
    std::vector<std::string> TxCallNames() const
    {
        std::vector<std::string> v;
        if (!m_tx) return v;
        for (size_t i = 0; i < m_tx->entry->Size(); ++i) v.emplace_back(m_tx->entry->At(i)->GetName());
        return v;
    }
    double TxOpenedAt()     const { return m_tx ? m_tx->openedAt : 0.0; }
    double TxLastActivity() const { return m_tx ? m_tx->lastActivity : 0.0; }
    u64    TxHumanPushes()  const { return m_tx ? m_undo.PushSeq() - m_tx->pushSeqAtBegin : 0; }
    const McpTxClosed* LastClosed() const { return m_lastClosed.label.empty() && m_lastClosed.reason.empty()
                                                   ? nullptr : &m_lastClosed; }

    // ================= AI の undo / redo =================
    enum class StepStatus { Ok, Empty, TopIsHuman, TxOpen };

    // 実行前の判定だけ（何も変えない）。
    StepStatus CheckUndo(bool onlyAi) const
    {
        if (m_tx) return StepStatus::TxOpen;
        if (!m_undo.CanUndo()) return StepStatus::Empty;
        if (onlyAi && !m_undo.PeekUndoIsAi()) return StepStatus::TopIsHuman;
        return StepStatus::Ok;
    }
    StepStatus CheckRedo(bool onlyAi) const
    {
        if (m_tx) return StepStatus::TxOpen;
        if (!m_undo.CanRedo()) return StepStatus::Empty;
        if (onlyAi && !m_undo.PeekRedoIsAi()) return StepStatus::TopIsHuman;
        return StepStatus::Ok;
    }

    StepStatus Undo(bool onlyAi, std::string* undoneName)
    {
        const StepStatus s = CheckUndo(onlyAi);
        if (s != StepStatus::Ok) return s;
        if (undoneName) *undoneName = m_undo.PeekUndoName();
        m_undo.Undo();
        return StepStatus::Ok;
    }
    StepStatus Redo(bool onlyAi, std::string* redoneName)
    {
        const StepStatus s = CheckRedo(onlyAi);
        if (s != StepStatus::Ok) return s;
        if (redoneName) *redoneName = m_undo.PeekRedoName();
        m_undo.Redo();
        return StepStatus::Ok;
    }

    // MCP 由来の遅延処理（生成・削除・複製をフレーム境界で実行する所）を 1 呼び出しとして囲む。
    // ★ループの途中の continue でも必ず EndCall させるための RAII。横取りが外れ忘れると、
    //   以後の人の操作が全部 AI のエントリへ吸い込まれる。
    class CallScope
    {
    public:
        CallScope(McpUndoRouter& r, bool active, const std::string& method, double now)
            : m_r(r), m_active(active), m_now(now)
        {
            if (m_active) m_r.BeginCall(method.empty() ? std::string("mcp") : method);
        }
        ~CallScope() { if (m_active) m_r.EndCall(m_now); }
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
    private:
        McpUndoRouter& m_r;
        bool   m_active;
        double m_now;
    };
private:
    struct Tx
    {
        std::string label;
        std::unique_ptr<AiUndoEntry> entry;
        double openedAt = 0.0;
        double lastActivity = 0.0;
        u64    pushSeqAtBegin = 0;
    };

    // 呼び出し 1 回ぶんを AI エントリにしてスタックかトランザクションへ。
    void Route(std::vector<std::unique_ptr<IUndoCommand>> cmds, const std::string& method)
    {
        auto entry = std::make_unique<AiUndoEntry>("AI: " + method);
        for (auto& c : cmds) entry->Add(std::move(c));
        if (entry->Empty()) return;
        if (m_tx) m_tx->entry->Add(std::move(entry));
        else      m_undo.PushUncaptured(std::move(entry));
    }

    UndoSystem& m_undo;
    int         m_callDepth = 0;
    std::string m_callMethod;
    std::vector<std::unique_ptr<IUndoCommand>> m_captured;
    std::unique_ptr<Tx> m_tx;
    McpTxClosed m_lastClosed;
};

} // namespace dx12e
