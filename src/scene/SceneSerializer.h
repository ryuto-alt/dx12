#pragma once

#include <string>
#include <entt/entt.hpp>

namespace dx12e
{

class Scene;

class SceneSerializer
{
public:
    static bool Save(const Scene& scene, const std::string& filePath,
                     const std::string& assetsDir);
    static bool Load(Scene& scene, const std::string& filePath,
                     const std::string& assetsDir);
    // 既存エンティティの Transform / Material を JSON から上書き（エンティティは作らない）
    static bool ApplyOverrides(Scene& scene, const std::string& filePath,
                               const std::string& assetsDir);

    // in-memory snapshot 用（Play→Stop 時のシーン復元に使用）
    static std::string SaveToString(const Scene& scene, const std::string& assetsDir);
    static bool LoadFromString(Scene& scene, const std::string& jsonStr,
                               const std::string& assetsDir);

    // 単一エンティティの JSON 化（クリップボード/複製用。parent は含まない）
    static std::string SerializeEntity(const Scene& scene, entt::entity e,
                                       const std::string& assetsDir);
    // JSON からエンティティを既存シーンに追加生成（Clear しない）
    // 失敗時は entt::null を返す
    static entt::entity InstantiateEntity(Scene& scene, const std::string& jsonStr,
                                          const std::string& assetsDir);
    // エンティティを全コンポーネント込みで複製（名前は重複しないよう連番付与）
    static entt::entity DuplicateEntity(Scene& scene, entt::entity src,
                                        const std::string& assetsDir);
};

} // namespace dx12e
