#pragma once

// Undo/Redo の土台（コマンド基底・複合コマンド・スタック）。
//
// ★このヘッダは entt も DirectXMath もコンポーネントも include しない。
//   具体的なコマンド（Transform / 削除 / 生成 …）は UndoSystem.h 側にあり、ここを include する。
//   分けてあるのは、スタックの規則（AI の印・横取り・トランザクション）を
//   tests/mcp_undo_test.cpp がエンジン抜きで検証できるようにするため。

#include <memory>
#include <string>
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
        m_undoStack.push_back(std::move(cmd));
        m_redoStack.clear();  // 新しい操作が入ったら redo は破棄
        // スタック上限
        if (m_undoStack.size() > kMaxHistory)
            m_undoStack.erase(m_undoStack.begin());
    }

    // 横取り先を張る / 外す（nullptr で解除）。入れ子にはしない（呼び出し側が 1 段で使う）。
    void SetCaptureSink(std::vector<std::unique_ptr<IUndoCommand>>* sink) { m_capture = sink; }
    bool Capturing() const { return m_capture != nullptr; }

    // ── 「シーンが変更されたか」の指標 ──
    // スタックの深さは指標に使えない: kMaxHistory を超えると先頭から捨てるし、
    // Clear() は Play/Stop とシーンロードで呼ばれる。なので単調増加カウンタを別に持つ。
    // ★Clear() ではリセットしない。Undo スタックが消えることと、
    //   「保存していない変更があること」は別の話なので。
    u64  EditSeq() const { return m_editSeq; }
    // Undo を積まない変更経路（レンダ設定の窓・Lua プロパティ・MCP など）から呼ぶ。
    // Undo できないこと自体は別問題だが、少なくとも「保存し忘れ」からは守る。
    void MarkEdited() { ++m_editSeq; }
    // スタックへ実際に積まれた回数（横取り分は数えない）。トランザクション中に
    // 人の編集が何件挟まったかを数えるのに使う（MarkEdited では増えない）。
    u64  PushSeq() const { return m_pushSeq; }

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

    void Undo()
    {
        if (m_undoStack.empty()) return;
        auto cmd = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        cmd->Undo();
        m_redoStack.push_back(std::move(cmd));
    }

    void Redo()
    {
        if (m_redoStack.empty()) return;
        auto cmd = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        cmd->Redo();
        m_undoStack.push_back(std::move(cmd));
    }

    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    void Clear() { m_undoStack.clear(); m_redoStack.clear(); }

private:
    static constexpr size_t kMaxHistory = 100;
    u64 m_editSeq = 0;
    u64 m_pushSeq = 0;
    std::vector<std::unique_ptr<IUndoCommand>> m_undoStack;
    std::vector<std::unique_ptr<IUndoCommand>> m_redoStack;
    std::vector<std::unique_ptr<IUndoCommand>>* m_capture = nullptr;
};

} // namespace dx12e
