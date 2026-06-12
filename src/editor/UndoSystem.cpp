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
    if (m_snapshot.empty()) return;

    // スナップショットから全コンポーネントを復元
    entt::entity e = SceneSerializer::InstantiateEntity(*m_scene, m_snapshot, m_assetsDir);
    if (e == entt::null)
    {
        Logger::Warn("[Undo] Failed to restore entity from snapshot");
        return;
    }

    auto& reg = m_scene->GetRegistry();

    // 親子関係の復元（親がまだ生きていれば）
    if (m_parent != entt::null && reg.valid(m_parent) && reg.all_of<Transform>(e))
        reg.get<Transform>(e).parent = m_parent;

    // 注意: 新しい entity ID になるため、スタック内の他コマンドが持つ
    // 旧 ID は無効化される（各コマンドは valid() ガードで no-op になる）
    m_entity = e;
    Logger::Info("[Undo] Restored entity: {}", reg.get<NameTag>(e).name);
}

void DeleteEntityCommand::Redo()
{
    auto& reg = m_scene->GetRegistry();
    if (reg.valid(m_entity))
    {
        m_scene->Remove(Entity(m_entity, &reg));
        Logger::Info("[Redo] Re-deleted entity");
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
