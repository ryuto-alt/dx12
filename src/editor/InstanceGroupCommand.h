#pragma once
// インスタンス群の「変換 / 展開」の Undo コマンド。
// 記録（GroupUndoRecord）は SceneSerializer の JSON スナップショットで、群の実体（InstanceSet）は台帳の "mem" 経由で共有される
// （配列は複製しない）。変換の Undo = 群 → 個別エンティティ、展開の Undo = 個別エンティティ → 群。
// 注意: 復元はモデルの読み込みを伴うので、他の Undo コマンドと同様 cmdList が有効なフレーム境界で実行されること。
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "editor/UndoCore.h"
#include "scene/InstanceGroupOps.h"

namespace dx12e
{

class InstanceGroupCommand : public IUndoCommand
{
public:
    // exploded=false: 変換（個別 → 群）を取り消す記録 / true: 展開（群 → 個別）を取り消す記録
    InstanceGroupCommand(Scene* scene, std::string assetsDir, std::vector<instgroup::GroupUndoRecord> records, bool exploded)
        : m_scene(scene), m_assetsDir(std::move(assetsDir)), m_records(std::move(records)), m_exploded(exploded) {}

    void Undo() override
    {
        for (const auto& r : m_records) instgroup::ApplyUndoRecord(*m_scene, m_assetsDir, r, /*toGroup=*/m_exploded);
    }
    void Redo() override
    {
        for (const auto& r : m_records) instgroup::ApplyUndoRecord(*m_scene, m_assetsDir, r, /*toGroup=*/!m_exploded);
    }
    const char* GetName() const override { return m_exploded ? "Explode Instance Group" : "Make Instance Group"; }

private:
    Scene*      m_scene;
    std::string m_assetsDir;
    std::vector<instgroup::GroupUndoRecord> m_records;
    bool        m_exploded;
};

} // namespace dx12e
