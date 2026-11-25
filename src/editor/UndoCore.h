#pragma once

// Undo/Redo の土台（コマンド基底・複合コマンド・スタック）。
//
// ★このヘッダは entt も DirectXMath もコンポーネントも include しない。
//   具体的なコマンド（Transform / 削除 / 生成 …）は UndoSystem.h 側にあり、ここを include する。
//   分けてあるのは、スタックの規則（AI の印・横取り・トランザクション）を
//   tests/mcp_undo_test.cpp がエンジン抜きで検証できるようにするため。

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "core/Types.h"

namespace dx12e
{

// ── Undo/Redo コマンド基底 ──
class IUndoCommand
{
public:
    virtual ~IUndoCommand() = default;
    virtual void Undo() = 0;
    virtual void Redo() = 0;
    virtual const char* GetName() const = 0;
    // AI（MCP）が積んだエントリか。既定は人の操作。
    // ★MCP の undo は既定でこれが true のエントリしか戻さない（McpUndoRouter）。
    //   人の編集を AI が黙って巻き戻す事故を止めるための印。
    virtual bool IsAi() const { return false; }
};

// ── 複合コマンド（複数コマンドを 1 回の Undo/Redo で実行） ──
class CompositeCommand : public IUndoCommand
{
public:
    explicit CompositeCommand(const char* name) : m_name(name) {}

    void Add(std::unique_ptr<IUndoCommand> cmd) { m_commands.push_back(std::move(cmd)); }
    bool Empty() const { return m_commands.empty(); }
    size_t Size() const { return m_commands.size(); }

    void Undo() override
    {
        for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
            (*it)->Undo();
    }

    void Redo() override
    {
        for (auto& cmd : m_commands)
            cmd->Redo();
    }

    const char* GetName() const override { return m_name; }

private:
    std::vector<std::unique_ptr<IUndoCommand>> m_commands;
    const char* m_name;
};

// ── AI（MCP）由来のエントリ ──
// 「AI: <メソッド名>」または「AI: <トランザクションのラベル>」の名前で 1 エントリになる。
// 中身は積まれた順に持ち、Undo は逆順・Redo は順に流す（CompositeCommand と同じ規則）。
// ★名前は std::string で持つ（CompositeCommand の const char* は文字列リテラル前提なので使えない）。
class AiUndoEntry : public IUndoCommand
{
public:
    explicit AiUndoEntry(std::string label) : m_label(std::move(label)) {}

    void Add(std::unique_ptr<IUndoCommand> cmd) { if (cmd) m_commands.push_back(std::move(cmd)); }
    bool   Empty() const { return m_commands.empty(); }
    size_t Size()  const { return m_commands.size(); }
    // 中身の名前（transaction_status が「何を積んだか」を返すため）
    const IUndoCommand* At(size_t i) const { return m_commands[i].get(); }

    void Undo() override
    {
        for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
            (*it)->Undo();
    }
    void Redo() override
    {
        for (auto& cmd : m_commands) cmd->Redo();
    }
    const char* GetName() const override { return m_label.c_str(); }
    bool IsAi() const override { return true; }
    void SetLabel(std::string label) { m_label = std::move(label); }

private:
    std::string m_label;
    std::vector<std::unique_ptr<IUndoCommand>> m_commands;
};

// ── 編集の起点（W05-02a）──
// 自動保存が「人の未保存の変更」を上書きしないために、変更が人由来か MCP 由来かを区別する。
// 仮想入力（imgui_* / input_*）は人の操作として Human に数える。
enum class EditOrigin : u8 { Human, Mcp };

namespace detail
{
    struct EditOriginTls
    {
        int        depth    = 0;                 // EditOriginScope の入れ子の深さ
        EditOrigin value    = EditOrigin::Human; // depth > 0 のときの値（一番内側）
        int        captures = 0;                 // UndoSystem の横取り（McpUndoRouter の呼び出し中）の数
    };
    inline EditOriginTls& EditOriginState()
    {
        static thread_local EditOriginTls s;
        return s;
    }
} // namespace detail

// この寿命の間、このスレッドの編集の起点を固定する（入れ子可。例外安全）。
struct EditOriginScope
{
    explicit EditOriginScope(EditOrigin o)
    {
        auto& s = detail::EditOriginState();
        m_prev = s.value;
        ++s.depth;
        s.value = o;
    }
    ~EditOriginScope()
    {
        auto& s = detail::EditOriginState();
        s.value = m_prev;
        --s.depth;
    }
    EditOriginScope(const EditOriginScope&) = delete;
    EditOriginScope& operator=(const EditOriginScope&) = delete;
private:
    EditOrigin m_prev = EditOrigin::Human;
};

// スコープがあればその値。無ければ横取り中（McpUndoRouter の呼び出しの間）は Mcp、それ以外は Human。
inline EditOrigin CurrentEditOrigin()
{
    const auto& s = detail::EditOriginState();
    if (s.depth > 0) return s.value;
    return s.captures > 0 ? EditOrigin::Mcp : EditOrigin::Human;
}

// ── Undo/Redo スタック ──
class UndoSystem
{
public:
    // 積む。横取り中（MCP の呼び出しを処理している間）は横取り先へ回す。
    // ★横取りは McpUndoRouter が張る。既存の積み口（グループ化 / 地形ストローク / 生成・削除の
    //   遅延処理 …）を 1 行も書き換えずに「AI の操作」として束ねるための仕組み。
    void PushCommand(std::unique_ptr<IUndoCommand> cmd)
    {
        if (!cmd) return;
        if (m_capture)
        {
            ++m_editSeq;      // 未保存判定は横取りでも進める（変更自体は起きている）
            NoteEditOrigin(false);   // 横取り中は Mcp（EditOriginScope が Human ならその値）
            m_capture->push_back(std::move(cmd));
            return;
        }
        PushUncaptured(std::move(cmd));
    }

    // 横取りを無視してスタックへ直接積む（McpUndoRouter が AI エントリを積むときの口）。
    void PushUncaptured(std::unique_ptr<IUndoCommand> cmd)
    {
        if (!cmd) return;
        ++m_editSeq;          // 未保存判定用（下記 EditSeq のコメント参照）
        ++m_pushSeq;
        NoteEditOrigin(cmd->IsAi());   // AI エントリ（McpUndoRouter が束ねたもの）は Mcp
        m_undoStack.push_back(std::move(cmd));
        m_undoTags.push_back(m_pushSeq);
        m_redoStack.clear();  // 新しい操作が入ったら redo は破棄
        m_redoTags.clear();
        // スタック上限
        if (m_undoStack.size() > kMaxHistory)
        {
            m_undoStack.erase(m_undoStack.begin());
            m_undoTags.erase(m_undoTags.begin());
        }
    }

    // 横取り先を張る / 外す（nullptr で解除）。入れ子にはしない（呼び出し側が 1 段で使う）。
    // 張ったスレッドと外すスレッドは同じでなければならない（thread_local の数を対にするため）。
    void SetCaptureSink(std::vector<std::unique_ptr<IUndoCommand>>* sink)
    {
        auto& tls = detail::EditOriginState();
        if (m_capture)
        {
            assert(m_captureThread == std::this_thread::get_id() && "SetCaptureSink: 張ったスレッドで外すこと");
            if (m_captureThread != std::this_thread::get_id())   // NDEBUG でも気付けるように（カウンタが漏れる）
                std::fprintf(stderr, "[UndoSystem] SetCaptureSink: 張ったスレッドと別のスレッドで外した（thread_local の数が漏れる）\n");
            if (tls.captures > 0) --tls.captures;
        }
        m_capture = sink;
        if (m_capture) { ++tls.captures; m_captureThread = std::this_thread::get_id(); }
    }
    // 横取り中のまま破棄されても thread_local の数が漏れないようにする。
    ~UndoSystem() { if (m_capture) SetCaptureSink(nullptr); }
    UndoSystem() = default;
    UndoSystem(const UndoSystem&) = delete;
    UndoSystem& operator=(const UndoSystem&) = delete;
    bool Capturing() const { return m_capture != nullptr; }

    // ── 「シーンが変更されたか」の指標 ──
    // スタックの深さは指標に使えない: kMaxHistory を超えると先頭から捨てるし、
    // Clear() は Play/Stop とシーンロードで呼ばれる。なので単調増加カウンタを別に持つ。
    // ★Clear() ではリセットしない。Undo スタックが消えることと、
    //   「保存していない変更があること」は別の話なので。
    u64  EditSeq() const { return m_editSeq; }
    // Undo を積まない変更経路（レンダ設定の窓・Lua プロパティ・MCP など）から呼ぶ。
    // Undo できないこと自体は別問題だが、少なくとも「保存し忘れ」からは守る。
    // 起点（人 / MCP）も記録する: Undo を積まない人の編集（Lua プロパティなど）も LastHumanEditSeq に入る。
    void MarkEdited() { ++m_editSeq; NoteEditOrigin(false); }
    // スタックへ実際に積まれた回数（横取り分は数えない）。トランザクション中に
    // 人の編集が何件挟まったかを数えるのに使う（MarkEdited では増えない）。
    u64  PushSeq() const { return m_pushSeq; }

    // 人（Human）が最後に積んだ変更の EditSeq。EditSeq() がこれより大きい分は MCP 由来だけ。
    // ★IsSceneHumanDirty() = LastHumanEditSeq() > sceneSavedSeq || humanSettingsDirty（EditorContext）。
    u64  LastHumanEditSeq() const { return m_lastHumanSeq; }

    // Play 中に積まれた分を捨てる（W05-11）。seq は Play 開始時に控えた PushSeq()。
    // それより後に積まれた Undo エントリ、および Redo 側にある seq より後のタグのエントリを Undo せずに捨てる
    // （Play のエンティティは Stop で消えるため）。Redo 側に seq より後のものがあれば Redo 全体を空にする。
    // EditSeq は変えない。
    // humanSeqBefore: Play 開始時の LastHumanEditSeq()。何か捨てたときだけ m_lastHumanSeq をそこまで戻す
    // （Play 中の人の Push が Stop の後も「人の未保存」として残るのを防ぐ）。省略時は戻さない。
    void TruncateTo(u64 seq, u64 humanSeqBefore = ~u64(0))
    {
        bool dropped = false;
        while (!m_undoStack.empty() && m_undoTags.back() > seq)
        {
            m_undoStack.pop_back();
            m_undoTags.pop_back();
            dropped = true;
        }
        for (u64 t : m_redoTags)
            if (t > seq) { dropped = true; m_redoStack.clear(); m_redoTags.clear(); break; }
        if (dropped)
        {
            // Undo 側を捨てた場合も Redo は無効（積んだ直後の状態でしか Redo できない）
            m_redoStack.clear(); m_redoTags.clear();
            if (humanSeqBefore != ~u64(0) && m_lastHumanSeq > humanSeqBefore) m_lastHumanSeq = humanSeqBefore;
        }
    }

    // 次の Undo / Redo が何に当たるか。押す前に呼び出し側へ見せるため。
    // AI のエントリは「AI: <メソッド名>」の名前で返る（人の操作と区別できる）。
    const char* PeekUndoName() const
    {
        return m_undoStack.empty() ? nullptr : m_undoStack.back()->GetName();
    }
    const char* PeekRedoName() const
    {
        return m_redoStack.empty() ? nullptr : m_redoStack.back()->GetName();
    }
    bool PeekUndoIsAi() const { return !m_undoStack.empty() && m_undoStack.back()->IsAi(); }
    bool PeekRedoIsAi() const { return !m_redoStack.empty() && m_redoStack.back()->IsAi(); }
    size_t UndoDepth() const { return m_undoStack.size(); }
    size_t RedoDepth() const { return m_redoStack.size(); }
    // 履歴の名前（新しい順・最大 maxN 件。dx12_editor_state の undo セクション用。読むだけ）。
    std::vector<std::string> RecentUndoNames(size_t maxN) const { return NamesNewestFirst(m_undoStack, maxN); }
    std::vector<std::string> RecentRedoNames(size_t maxN) const { return NamesNewestFirst(m_redoStack, maxN); }

    void Undo()
    {
        if (m_undoStack.empty()) return;
        auto cmd = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        const u64 tag = m_undoTags.back();
        m_undoTags.pop_back();
        cmd->Undo();
        m_redoStack.push_back(std::move(cmd));
        m_redoTags.push_back(tag);
        ++m_editSeq;               // 状態が変わった。保存済みの状態から外れる
        NoteEditOrigin(false);     // 人の Ctrl+Z は人の編集（MCP の undo は Mcp のスコープで Mcp になる）
    }

    void Redo()
    {
        if (m_redoStack.empty()) return;
        auto cmd = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        const u64 tag = m_redoTags.back();
        m_redoTags.pop_back();
        cmd->Redo();
        m_undoStack.push_back(std::move(cmd));
        m_undoTags.push_back(tag);
        ++m_editSeq;
        NoteEditOrigin(false);
    }

    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    void Clear() { m_undoStack.clear(); m_redoStack.clear(); m_undoTags.clear(); m_redoTags.clear(); }

private:
    // 積んだ直後（m_editSeq は進めた後）に呼ぶ。Human なら m_lastHumanSeq を追従させる。
    void NoteEditOrigin(bool isAi)
    {
        if (!isAi && CurrentEditOrigin() == EditOrigin::Human) m_lastHumanSeq = m_editSeq;
    }
    static std::vector<std::string> NamesNewestFirst(const std::vector<std::unique_ptr<IUndoCommand>>& stack, size_t maxN)
    {
        std::vector<std::string> out;
        for (size_t i = stack.size(); i > 0 && out.size() < maxN; --i)
        {
            const char* n = stack[i - 1]->GetName();
            out.emplace_back(n ? n : "");
        }
        return out;
    }
    static constexpr size_t kMaxHistory = 100;
    u64 m_editSeq = 0;
    u64 m_pushSeq = 0;
    u64 m_lastHumanSeq = 0;
    std::vector<std::unique_ptr<IUndoCommand>> m_undoStack;
    std::vector<std::unique_ptr<IUndoCommand>> m_redoStack;
    std::vector<u64> m_undoTags;   // m_undoStack と同じ並び: 積んだときの PushSeq（TruncateTo 用）
    std::vector<u64> m_redoTags;
    std::vector<std::unique_ptr<IUndoCommand>>* m_capture = nullptr;
    std::thread::id m_captureThread;
};

} // namespace dx12e
