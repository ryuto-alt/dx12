#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"
#include "ecs/Components.h"

namespace dx12e
{

class Scene;

// ── Undo/Redo コマンド基底 ──
class IUndoCommand
{
public:
    virtual ~IUndoCommand() = default;
    virtual void Undo() = 0;
    virtual void Redo() = 0;
    virtual const char* GetName() const = 0;
};

// ── Transform 変更コマンド ──
class TransformCommand : public IUndoCommand
{
public:
    TransformCommand(entt::registry* reg, entt::entity entity,
                     const Transform& before, const Transform& after)
        : m_reg(reg), m_entity(entity), m_before(before), m_after(after) {}

    void Undo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<Transform>(m_entity))
            m_reg->get<Transform>(m_entity) = m_before;
    }

    void Redo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<Transform>(m_entity))
            m_reg->get<Transform>(m_entity) = m_after;
    }

    const char* GetName() const override { return "Transform"; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    Transform       m_before;
    Transform       m_after;
};

// ── PBR パラメータ変更コマンド ──
class PBRCommand : public IUndoCommand
{
public:
    PBRCommand(entt::registry* reg, entt::entity entity,
               float metalBefore, float roughBefore,
               float metalAfter, float roughAfter)
        : m_reg(reg), m_entity(entity),
          m_metalBefore(metalBefore), m_roughBefore(roughBefore),
          m_metalAfter(metalAfter), m_roughAfter(roughAfter) {}

    void Undo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<MeshRenderer>(m_entity))
        {
            auto& mr = m_reg->get<MeshRenderer>(m_entity);
            mr.overrideMetallic = m_metalBefore;
            mr.overrideRoughness = m_roughBefore;
        }
    }

    void Redo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<MeshRenderer>(m_entity))
        {
            auto& mr = m_reg->get<MeshRenderer>(m_entity);
            mr.overrideMetallic = m_metalAfter;
            mr.overrideRoughness = m_roughAfter;
        }
    }

    const char* GetName() const override { return "PBR"; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    float m_metalBefore, m_roughBefore;
    float m_metalAfter, m_roughAfter;
};

// ── エンティティ削除コマンド（サブツリー対応。Undo で全コンポーネント復元） ──
// 削除前に SceneSerializer::SerializeEntity で取った JSON スナップショットを
// 親→子の順で保持し、Undo で InstantiateEntity により親子関係ごと復元する。
// 注意: 復元処理はモデル再ロードを伴うため、cmdList が有効なフレーム境界で実行すること
// （Application は pendingUndo/pendingRedo 経由で遅延実行する）。
struct DeletedEntityRecord
{
    std::string snapshot;          // SerializeEntity の JSON
    int         parentLocalIndex = -1;  // records 配列内の親 (-1 = サブツリー外)
};

class DeleteEntityCommand : public IUndoCommand
{
public:
    DeleteEntityCommand(Scene* scene, std::string assetsDir,
                        std::vector<DeletedEntityRecord> records,
                        std::vector<entt::entity> entities,
                        entt::entity externalParent)
        : m_scene(scene),
          m_assetsDir(std::move(assetsDir)),
          m_records(std::move(records)),
          m_entities(std::move(entities)),
          m_externalParent(externalParent) {}

    void Undo() override;   // スナップショットからサブツリー復元
    void Redo() override;   // 再削除

    const char* GetName() const override { return "Delete"; }

private:
    Scene*                           m_scene;
    std::string                      m_assetsDir;
    std::vector<DeletedEntityRecord> m_records;
    std::vector<entt::entity>        m_entities;       // 現在の entity ID (Redo 用)
    entt::entity                     m_externalParent; // ルートの親（サブツリー外）
};

// ── エンティティ生成コマンド（Undo で削除 / Redo で再生成） ──
// Undo 時に JSON スナップショットを取って削除し、Redo でそのスナップショットから
// 再生成する。これで生成 → Undo → Redo が往復できる。
class SpawnEntityCommand : public IUndoCommand
{
public:
    SpawnEntityCommand(Scene* scene, std::string assetsDir, entt::entity entity)
        : m_scene(scene), m_assetsDir(std::move(assetsDir)), m_entity(entity) {}

    void Undo() override;   // スナップショットを取って削除
    void Redo() override;   // スナップショットから再生成

    const char* GetName() const override { return "Spawn"; }

private:
    Scene*       m_scene;
    std::string  m_assetsDir;
    entt::entity m_entity;
    std::string  m_snapshot;                          // Undo 時に確定（Redo 用の JSON）
    entt::entity m_externalParent = entt::null;       // ルートの親（サブツリー外）。Undo 時に捕捉
};

// ── プレハブ生成コマンド（サブツリー対応。Undo で全エンティティ削除 / Redo で再生成） ──
// プレハブは複数エンティティ（親 + 子）を生成しうるため SpawnEntityCommand では
// 子が復元されない。Undo 時にサブツリー全体を JSON 化して保持し、root + 子孫を
// まとめて削除する。Redo はその JSON から再生成する。
class SpawnPrefabCommand : public IUndoCommand
{
public:
    SpawnPrefabCommand(Scene* scene, std::string assetsDir,
                       std::vector<entt::entity> entities)
        : m_scene(scene), m_assetsDir(std::move(assetsDir)),
          m_entities(std::move(entities)) {}

    void Undo() override;   // サブツリーを JSON 化して全削除
    void Redo() override;   // JSON から再生成

    const char* GetName() const override { return "Spawn Prefab"; }

private:
    Scene*                    m_scene;
    std::string               m_assetsDir;
    std::vector<entt::entity> m_entities;   // 生成した全エンティティ（root 先頭）
    std::string               m_snapshot;   // Undo 時に確定（Redo 用のサブツリー JSON）
    entt::entity              m_externalParent = entt::null;  // ルートの親（サブツリー外）。Undo 時に捕捉
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

// ── 汎用コンポーネント編集コマンド（値のコピーで before/after を保持） ──
template<typename T>
class ComponentEditCommand : public IUndoCommand
{
public:
    ComponentEditCommand(entt::registry* reg, entt::entity entity,
                         const T& before, const T& after, const char* name)
        : m_reg(reg), m_entity(entity), m_before(before), m_after(after), m_name(name) {}

    void Undo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<T>(m_entity))
            m_reg->get<T>(m_entity) = m_before;
    }

    void Redo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<T>(m_entity))
            m_reg->get<T>(m_entity) = m_after;
    }

    const char* GetName() const override { return m_name; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    T               m_before;
    T               m_after;
    const char*     m_name;
};

// ── 汎用コンポーネント追加コマンド ──
template<typename T>
class AddComponentCommand : public IUndoCommand
{
public:
    AddComponentCommand(entt::registry* reg, entt::entity entity,
                        const T& value, const char* name)
        : m_reg(reg), m_entity(entity), m_value(value), m_name(name) {}

    void Undo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<T>(m_entity))
            m_reg->remove<T>(m_entity);
    }

    void Redo() override
    {
        if (m_reg->valid(m_entity))
            m_reg->emplace_or_replace<T>(m_entity, m_value);
    }

    const char* GetName() const override { return m_name; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    T               m_value;
    const char*     m_name;
};

// ── 汎用コンポーネント削除コマンド ──
template<typename T>
class RemoveComponentCommand : public IUndoCommand
{
public:
    RemoveComponentCommand(entt::registry* reg, entt::entity entity,
                           const T& removedValue, const char* name)
        : m_reg(reg), m_entity(entity), m_value(removedValue), m_name(name) {}

    void Undo() override
    {
        if (m_reg->valid(m_entity))
            m_reg->emplace_or_replace<T>(m_entity, m_value);
    }

    void Redo() override
    {
        if (m_reg->valid(m_entity) && m_reg->all_of<T>(m_entity))
            m_reg->remove<T>(m_entity);
    }

    const char* GetName() const override { return m_name; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    T               m_value;
    const char*     m_name;
};

// ── LuaScript Attach コマンド ──
class AttachScriptCommand : public IUndoCommand
{
public:
    AttachScriptCommand(entt::registry* reg, entt::entity entity,
                        bool hadBefore, std::string oldPath, bool oldEnabled,
                        std::string newPath)
        : m_reg(reg), m_entity(entity),
          m_hadBefore(hadBefore), m_oldPath(std::move(oldPath)),
          m_oldEnabled(oldEnabled), m_newPath(std::move(newPath)) {}

    void Undo() override
    {
        if (!m_reg->valid(m_entity)) return;
        if (m_hadBefore)
        {
            LuaScript ls;
            ls.scriptPath = m_oldPath;
            ls.enabled    = m_oldEnabled;
            m_reg->emplace_or_replace<LuaScript>(m_entity, std::move(ls));
        }
        else
        {
            if (m_reg->all_of<LuaScript>(m_entity))
                m_reg->remove<LuaScript>(m_entity);
        }
    }

    void Redo() override
    {
        if (!m_reg->valid(m_entity)) return;
        LuaScript ls;
        ls.scriptPath = m_newPath;
        ls.enabled    = true;
        m_reg->emplace_or_replace<LuaScript>(m_entity, std::move(ls));
    }

    const char* GetName() const override { return "AttachScript"; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    bool            m_hadBefore;
    std::string     m_oldPath;
    bool            m_oldEnabled;
    std::string     m_newPath;
};

// ── LuaScript Detach コマンド ──
class DetachScriptCommand : public IUndoCommand
{
public:
    DetachScriptCommand(entt::registry* reg, entt::entity entity,
                        std::string oldPath, bool oldEnabled)
        : m_reg(reg), m_entity(entity),
          m_oldPath(std::move(oldPath)), m_oldEnabled(oldEnabled) {}

    void Undo() override
    {
        if (!m_reg->valid(m_entity)) return;
        LuaScript ls;
        ls.scriptPath = m_oldPath;
        ls.enabled    = m_oldEnabled;
        m_reg->emplace_or_replace<LuaScript>(m_entity, std::move(ls));
    }

    void Redo() override
    {
        if (!m_reg->valid(m_entity)) return;
        if (m_reg->all_of<LuaScript>(m_entity))
            m_reg->remove<LuaScript>(m_entity);
    }

    const char* GetName() const override { return "DetachScript"; }

private:
    entt::registry* m_reg;
    entt::entity    m_entity;
    std::string     m_oldPath;
    bool            m_oldEnabled;
};

// ── Undo/Redo スタック ──
class UndoSystem
{
public:
    void PushCommand(std::unique_ptr<IUndoCommand> cmd)
    {
        m_undoStack.push_back(std::move(cmd));
        m_redoStack.clear();  // 新しい操作が入ったら redo は破棄
        // スタック上限
        if (m_undoStack.size() > kMaxHistory)
            m_undoStack.erase(m_undoStack.begin());
    }

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
    std::vector<std::unique_ptr<IUndoCommand>> m_undoStack;
    std::vector<std::unique_ptr<IUndoCommand>> m_redoStack;
};

} // namespace dx12e
