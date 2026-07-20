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
            Logger::Warn("[Undo] スナップショットからのエンティティ復元に失敗しました");
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
    auto& reg = m_scene->GetRegistry();
    if (reg.valid(m_entity))
    {
        // SerializeEntity は外部親（サブツリー外の Transform::parent）を含めないため、
        // Redo で親子関係を復元できるよう削除前にここで捕捉する
        if (const auto* t = reg.try_get<Transform>(m_entity))
            m_externalParent = t->parent;
        // Redo 用にスナップショットを取ってから削除する
        m_snapshot = SceneSerializer::SerializeEntity(*m_scene, m_entity, m_assetsDir);
        m_scene->Remove(Entity(m_entity, &reg));
    }
}

void SpawnEntityCommand::Redo()
{
    // 生成直後（まだ一度も Undo していない）はスナップショットが無いので何もしない
    if (m_snapshot.empty()) return;
    auto& reg = m_scene->GetRegistry();
    entt::entity e = SceneSerializer::InstantiateEntity(*m_scene, m_snapshot, m_assetsDir);
    if (e != entt::null)
    {
        m_entity = e;  // 新しい ID に更新
        // 外部親を張り直す（親が既に消えていれば黙って null のまま＝安全）
        if (m_externalParent != entt::null && reg.valid(m_externalParent)
            && reg.all_of<Transform>(e))
            reg.get<Transform>(e).parent = m_externalParent;
    }
}

// ── SpawnPrefabCommand ──
void SpawnPrefabCommand::Undo()
{
    auto& reg = m_scene->GetRegistry();
    // Redo 用にサブツリー全体をスナップショット（root から子孫を辿って自己完結 JSON 化）
    if (!m_entities.empty() && reg.valid(m_entities[0]))
    {
        // SerializeSubtree は root の外部親（サブツリー外の Transform::parent）を含めない
        // ため、Redo で親子関係を復元できるよう削除前にここで捕捉する
        if (const auto* t = reg.try_get<Transform>(m_entities[0]))
            m_externalParent = t->parent;
        m_snapshot = SceneSerializer::SerializeSubtree(*m_scene, m_entities[0], m_assetsDir);
    }

    // 子 → 親 の順で削除（Scene::Remove はカスケードしないので全要素を明示削除）
    for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it)
        if (reg.valid(*it))
            m_scene->Remove(Entity(*it, &reg));
    m_entities.clear();
}

void SpawnPrefabCommand::Redo()
{
    if (m_snapshot.empty()) return;
    auto& reg = m_scene->GetRegistry();
    std::vector<entt::entity> all;
    SceneSerializer::InstantiateSubtree(*m_scene, m_snapshot, m_assetsDir, &all);
    m_entities = std::move(all);  // 新しい ID 群に更新
    // root（先頭）の外部親を張り直す（親が既に消えていれば黙って null のまま＝安全）
    if (!m_entities.empty() && reg.valid(m_entities[0])
        && m_externalParent != entt::null && reg.valid(m_externalParent)
        && reg.all_of<Transform>(m_entities[0]))
        reg.get<Transform>(m_entities[0]).parent = m_externalParent;
}

// ── PrefabRevertCommand ──
namespace
{
// サブツリーを丸ごと差し替える共通処理。
// 「消す前に今の姿を JSON 化 → 子から順に削除 → 与えられた JSON を展開 → 外部親を復元」
// という Undo/Redo 対称の手順を 1 箇所にまとめたもの。
void SwapSubtree(Scene* scene, const std::string& assetsDir, const std::string& incomingJson,
                 std::string& outCapturedJson, std::vector<entt::entity>& entities,
                 entt::entity externalParent)
{
    auto& reg = scene->GetRegistry();
    if (!entities.empty() && reg.valid(entities[0]))
        outCapturedJson = SceneSerializer::SerializeSubtree(*scene, entities[0], assetsDir);

    for (auto it = entities.rbegin(); it != entities.rend(); ++it)
        if (reg.valid(*it)) scene->Remove(Entity(*it, &reg));
    entities.clear();

    if (incomingJson.empty()) return;
    std::vector<entt::entity> all;
    SceneSerializer::InstantiateSubtree(*scene, incomingJson, assetsDir, &all);
    entities = std::move(all);

    if (!entities.empty() && reg.valid(entities[0]) && externalParent != entt::null
        && reg.valid(externalParent) && reg.all_of<Transform>(entities[0]))
        reg.get<Transform>(entities[0]).parent = externalParent;
}
} // namespace

void PrefabRevertCommand::Undo()
{
    SwapSubtree(m_scene, m_assetsDir, m_before, m_after, m_entities, m_externalParent);
}

void PrefabRevertCommand::Redo()
{
    SwapSubtree(m_scene, m_assetsDir, m_after, m_before, m_entities, m_externalParent);
}

// ── ModelSwapCommand ──
void ModelSwapCommand::Swap(const std::string& path)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(m_entity)) return;
    entt::entity ne = SceneSerializer::SwapEntityModel(*m_scene, m_entity, path, m_assetsDir);
    if (ne != entt::null)
        m_entity = ne;   // 新しい ID に更新（失敗時は旧エンティティが残るので据え置き）
    else
        Logger::Warn("[Undo/Redo] モデル差し替えに失敗しました: {}", path);
}

void ModelSwapCommand::Undo() { Swap(m_oldPath); }
void ModelSwapCommand::Redo() { Swap(m_newPath); }

} // namespace dx12e
