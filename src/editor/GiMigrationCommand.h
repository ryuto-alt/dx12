#pragma once
//
// GiMigrationCommand — ライティング窓の「新しい GI に切り替える / 旧に戻す」を Undo 1 回にまとめる。
//   GI モード・DDGI・SSGI・RT 影・各太陽の ambient を、操作の前後のスナップショットで丸ごと戻す / やり直す。
//   スナップショットの中身と適用は scene/GiMigration.h（MCP の migrate_gi・新規シーンと同じ関数）。
//
#include <string>
#include <utility>

#include "editor/UndoCore.h"
#include "scene/GiMigration.h"
#include "scene/Scene.h"

namespace dx12e
{

class GiMigrationCommand : public IUndoCommand
{
public:
    GiMigrationCommand(Scene* scene, gi::State before, gi::State after, std::string name)
        : m_scene(scene), m_before(std::move(before)), m_after(std::move(after)), m_name(std::move(name)) {}

    void Undo() override { if (m_scene) gi::Restore(*m_scene, m_before); }
    void Redo() override { if (m_scene) gi::Restore(*m_scene, m_after); }
    const char* GetName() const override { return m_name.c_str(); }

private:
    Scene*      m_scene;
    gi::State   m_before, m_after;
    std::string m_name;
};

} // namespace dx12e
