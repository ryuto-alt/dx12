#include "editor/UndoSystem.h"
#include "scene/Scene.h"
#include "scene/Entity.h"
#include "core/Logger.h"

namespace dx12e
{

// ── DeleteEntityCommand ──
void DeleteEntityCommand::Undo()
{
    // 復元: モデルパスがあればSpawn、なければ最低限の再生成
    if (!m_data.modelPath.empty() && m_data.modelPath[0] != '_')
    {
        // 注意: Spawn は新しい entity ID を返すが、
        // スタック内の他コマンドが古いIDを参照してる可能性がある。
        // ここでは最低限 Transform + NameTag だけ復元
        auto e = m_reg->create();
        m_reg->emplace<NameTag>(e, NameTag{m_data.name});
        m_reg->emplace<Transform>(e, m_data.transform);
        m_entity = e;
        Logger::Info("[Undo] Restored entity: {}", m_data.name);
    }
    else
    {
        auto e = m_reg->create();
        m_reg->emplace<NameTag>(e, NameTag{m_data.name});
        m_reg->emplace<Transform>(e, m_data.transform);
        m_entity = e;
        Logger::Info("[Undo] Restored entity (no mesh): {}", m_data.name);
    }
}

void DeleteEntityCommand::Redo()
{
    if (m_reg->valid(m_entity))
    {
        m_scene->Remove(Entity(m_entity, m_reg));
        Logger::Info("[Redo] Re-deleted entity: {}", m_data.name);
    }
}

// ── SpawnEntityCommand ──
void SpawnEntityCommand::Undo()
{
    if (m_reg->valid(m_entity))
    {
        m_scene->Remove(Entity(m_entity, m_reg));
    }
}

} // namespace dx12e
