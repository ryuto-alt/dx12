#include "editor/UndoSystem.h"
#include "scene/Scene.h"
#include "scene/Entity.h"
#include "scene/SceneSerializer.h"
#include "core/Logger.h"

namespace dx12e
{

// ── DeleteEntityCommand ──
void DeleteEntityCommand::Undo()
{
    if (m_records.empty()) return;

    auto& reg = m_scene->GetRegistry();

    // 親→子の順でスナップショットから復元し、親子関係を張り直す
    std::vector<entt::entity> restored(m_records.size(),
                                       static_cast<entt::entity>(entt::null));
    for (size_t i = 0; i < m_records.size(); ++i)
    {
        entt::entity e = SceneSerializer::InstantiateEntity(
            *m_scene, m_records[i].snapshot, m_assetsDir);
        restored[i] = e;
        if (e == entt::null)
        {
            Logger::Warn("[Undo] Failed to restore entity from snapshot");
            continue;
        }

        int pIdx = m_records[i].parentLocalIndex;
        entt::entity parent = entt::null;
        if (pIdx >= 0 && pIdx < static_cast<int>(restored.size()))
            parent = restored[static_cast<size_t>(pIdx)];
        else if (m_externalParent != entt::null && reg.valid(m_externalParent))
            parent = m_externalParent;

        if (parent != entt::null && reg.all_of<Transform>(e))
            reg.get<Transform>(e).parent = parent;
    }

    // 注意: 新しい entity ID になるため、スタック内の他コマンドが持つ
    // 旧 ID は無効化される（各コマンドは valid() ガードで no-op になる）
    m_entities = std::move(restored);
    Logger::Info("[Undo] Restored {} entity(ies)", m_entities.size());
}

void DeleteEntityCommand::Redo()
{
    auto& reg = m_scene->GetRegistry();
    for (auto e : m_entities)
    {
        if (reg.valid(e))
            m_scene->Remove(Entity(e, &reg));
    }
    Logger::Info("[Redo] Re-deleted {} entity(ies)", m_entities.size());
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
