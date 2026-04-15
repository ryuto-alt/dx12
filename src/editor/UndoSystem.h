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

// ── エンティティ削除コマンド（Undo で復元） ──
struct DeletedEntityData
{
    std::string name;
    Transform   transform;
    std::string modelPath;
    float       overrideMetallic  = -1.0f;
    float       overrideRoughness = -1.0f;
};

class DeleteEntityCommand : public IUndoCommand
{
public:
    DeleteEntityCommand(Scene* scene, entt::registry* reg,
                        entt::entity entity, const DeletedEntityData& data)
        : m_scene(scene), m_reg(reg), m_entity(entity), m_data(data) {}

    void Undo() override;   // Scene::Spawn で復元
    void Redo() override;   // Scene::Remove で再削除

    const char* GetName() const override { return "Delete"; }

    entt::entity GetRestoredEntity() const { return m_entity; }

private:
    Scene*            m_scene;
    entt::registry*   m_reg;
    entt::entity      m_entity;
    DeletedEntityData m_data;
};

// ── エンティティ生成コマンド（Undo で削除） ──
class SpawnEntityCommand : public IUndoCommand
{
public:
    SpawnEntityCommand(Scene* scene, entt::registry* reg, entt::entity entity)
        : m_scene(scene), m_reg(reg), m_entity(entity) {}

    void Undo() override;   // 削除
    void Redo() override {} // 再生成は困難なので no-op（生成時点で確定）

    const char* GetName() const override { return "Spawn"; }

private:
    Scene*          m_scene;
    entt::registry* m_reg;
    entt::entity    m_entity;
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
