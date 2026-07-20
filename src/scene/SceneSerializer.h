#pragma once

#include <string>
#include <vector>
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

    // エンティティのモデル(modelPath)だけを差し替える。全コンポーネント・親子関係・
    // 名前を維持したまま JSON 経由で再生成するため entt::entity ID は変わる。
    // 失敗時（モデルロード失敗等）は entt::null を返し、元エンティティは無傷。
    // 注意: モデルロードを伴うため cmdList が有効なフレーム境界で呼ぶこと。
    static entt::entity SwapEntityModel(Scene& scene, entt::entity e,
                                        const std::string& newModelPath,
                                        const std::string& assetsDir);

    // --- Prefab / サブツリー（再利用テンプレート）---
    // root + 全子孫を 1 つの自己完結 JSON（parent はローカル index 参照）に直列化
    static std::string SerializeSubtree(const Scene& scene, entt::entity root,
                                        const std::string& assetsDir);
    // サブツリー JSON を既存シーンへ展開（Clear しない）。戻り値 = root。
    // outAll に生成した全エンティティ（root 先頭）を返す。
    static entt::entity InstantiateSubtree(Scene& scene, const std::string& jsonStr,
                                           const std::string& assetsDir,
                                           std::vector<entt::entity>* outAll = nullptr);
    // root + 子孫を .prefab ファイルへ保存
    static bool SavePrefab(const Scene& scene, entt::entity root,
                           const std::string& filePath, const std::string& assetsDir);
    // .prefab ファイルを既存シーンへ生成。戻り値 = root（失敗時 entt::null）。
    static entt::entity InstantiatePrefab(Scene& scene, const std::string& filePath,
                                          const std::string& assetsDir,
                                          std::vector<entt::entity>* outAll = nullptr);
};

} // namespace dx12e
