#pragma once

#include <string>

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
};

} // namespace dx12e
