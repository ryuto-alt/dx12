#pragma once

// ===== ヒエラルキーの Undo コマンド（フェーズ 1b）=====
//   EditorFlagCommand<T> … 非表示 / ロック / フォルダのタグの付け外し（複数対象を 1 エントリに束ねる）
// 親替え + 兄弟順は TransformCommand（Transform 丸ごと。parent と siblingOrder を含む）の CompositeCommand で積む。

#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "ecs/Components.h"
#include "editor/UndoCore.h"

namespace dx12e
{

template <class T>
class EditorFlagCommand : public IUndoCommand
{
public:
    struct Change { entt::entity e; bool before; bool after; };

    EditorFlagCommand(entt::registry* reg, std::vector<Change> changes, const char* name)
        : m_reg(reg), m_changes(std::move(changes)), m_name(name) {}

    void Undo() override { for (const Change& c : m_changes) Apply(c.e, c.before); }
    void Redo() override { for (const Change& c : m_changes) Apply(c.e, c.after); }
    const char* GetName() const override { return m_name; }
    bool Empty() const { return m_changes.empty(); }

private:
    void Apply(entt::entity e, bool on)
    {
        if (!m_reg->valid(e)) return;
        if (on) m_reg->template emplace_or_replace<T>(e);
        else    m_reg->template remove<T>(e);
    }

    entt::registry*     m_reg;
    std::vector<Change> m_changes;
    const char*         m_name;   // 文字列リテラル
};

// ── フォルダ作成（空の親 + EditorFolder を作って選択をぶら下げる / Undo でフォルダを消して元へ戻す）──
// GroupCommand と同じ作りで、違いは EditorFolder タグと siblingOrder を持つことと、メンバーが空でもよいこと。
// Redo のたびにフォルダを作り直す（entity ID は変わるので子へ配り直す）。子自体は消さない。
class FolderCommand : public IUndoCommand
{
public:
    FolderCommand(entt::registry* reg, std::string name, entt::entity folder, entt::entity folderParent,
                  int siblingOrder, std::vector<std::pair<entt::entity, entt::entity>> children)
        : m_reg(reg), m_name(std::move(name)), m_folder(folder), m_folderParent(folderParent),
          m_order(siblingOrder), m_children(std::move(children)) {}

    void Undo() override
    {
        for (const auto& [child, oldParent] : m_children)
            if (m_reg->valid(child) && m_reg->all_of<Transform>(child))
                m_reg->get<Transform>(child).parent = oldParent;
        if (m_reg->valid(m_folder)) m_reg->destroy(m_folder);
        m_folder = entt::null;
    }

    void Redo() override
    {
        if (!m_reg->valid(m_folder))
        {
            m_folder = m_reg->create();
            m_reg->emplace<NameTag>(m_folder, NameTag{m_name});
            Transform ft{};
            if (m_folderParent != entt::null && m_reg->valid(m_folderParent)) ft.parent = m_folderParent;
            ft.siblingOrder = m_order;
            m_reg->emplace<Transform>(m_folder, ft);
            m_reg->emplace<EditorFolder>(m_folder);
        }
        for (const auto& [child, oldParent] : m_children)
        {
            (void)oldParent;
            if (m_reg->valid(child) && m_reg->all_of<Transform>(child))
                m_reg->get<Transform>(child).parent = m_folder;
        }
    }

    const char* GetName() const override { return "Folder"; }
    entt::entity Folder() const { return m_folder; }

private:
    entt::registry* m_reg;
    std::string     m_name;
    entt::entity    m_folder;
    entt::entity    m_folderParent;
    int             m_order;
    std::vector<std::pair<entt::entity, entt::entity>> m_children;   // (子, 元の親)
};

// ── 空の子エンティティの作成（Undo で消す / Redo で作り直す）──
class CreateEmptyCommand : public IUndoCommand
{
public:
    CreateEmptyCommand(entt::registry* reg, std::string name, entt::entity e, entt::entity parent)
        : m_reg(reg), m_name(std::move(name)), m_e(e), m_parent(parent) {}

    void Undo() override { if (m_reg->valid(m_e)) m_reg->destroy(m_e); m_e = entt::null; }
    void Redo() override
    {
        if (m_reg->valid(m_e)) return;
        m_e = m_reg->create();
        m_reg->emplace<NameTag>(m_e, NameTag{m_name});
        Transform t{};
        if (m_parent != entt::null && m_reg->valid(m_parent)) t.parent = m_parent;
        m_reg->emplace<Transform>(m_e, t);
    }
    const char* GetName() const override { return "Create"; }

private:
    entt::registry* m_reg;
    std::string     m_name;
    entt::entity    m_e;
    entt::entity    m_parent;
};

} // namespace dx12e
