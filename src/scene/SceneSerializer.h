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

    // リストから「先祖も含まれているエンティティ」を除いた最上位だけを返す。
    // サブツリー複製と併用しないと、親子両方を選択してコピーしたとき子が二重化する。
    static std::vector<entt::entity> TopmostRoots(const Scene& scene,
                                                  const std::vector<entt::entity>& entities);

    // サブツリー（src + 全子孫）を全コンポーネント込みで複製。root の親は元と同じ。
    // outAll に生成した全エンティティ（root 先頭）を返す（Undo 用）。
    static entt::entity DuplicateSubtree(Scene& scene, entt::entity src,
                                         const std::string& assetsDir,
                                         std::vector<entt::entity>* outAll = nullptr);

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
    // 生成した root には PrefabLink（元 .prefab への紐付け）が自動で付く＝以後 Apply/Revert 可能。
    static entt::entity InstantiatePrefab(Scene& scene, const std::string& filePath,
                                          const std::string& assetsDir,
                                          std::vector<entt::entity>* outAll = nullptr);

    // --- プレハブのリンク維持（Unity の Apply / Revert / オーバーライド表示 相当）---

    // インスタンスと元 .prefab の差分1件。
    struct PrefabOverride
    {
        int         entityIndex = 0;   // サブツリー BFS 順のインデックス（0 = ルート）
        std::string entityName;
        std::string component;         // JSON キー（例 "uiRect"）。構造差なら "(構成)"
        std::string field;             // フィールド名。コンポーネント丸ごとの増減なら "(追加)"/"(削除)"
    };

    // root（PrefabLink 必須）と元 .prefab を比べて変更点を列挙する。
    // 名前は展開時に連番が付くので比較対象から外す（毎回全インスタンスが差分扱いになるため）。
    // PrefabLink 自身も比較しない。プレハブが読めない場合は false。
    static bool ComputePrefabOverrides(const Scene& scene, entt::entity root,
                                       const std::string& assetsDir,
                                       std::vector<PrefabOverride>& out);

    // インスタンスの現在の姿を元 .prefab へ書き戻す（Apply）。PrefabLink は書き出さない。
    static bool ApplyPrefabInstance(const Scene& scene, entt::entity root,
                                    const std::string& assetsDir);

    // インスタンスを元 .prefab の状態へ戻す（Revert）。作り直しなので entity ID は変わる。
    // 外部親（サブツリーの外側にいる親）だけは維持する（戻した拍子に階層から飛び出さないため）。
    // 戻り値 = 新しい root（失敗時 entt::null。この場合は元のインスタンスが無傷で残る）。
    static entt::entity RevertPrefabInstance(Scene& scene, entt::entity root,
                                             const std::string& assetsDir);

    // sourcePath が一致する他のインスタンスを全部 Revert する（Apply 後の伝播用）。
    // except は伝播元のインスタンス自身（自分は作り直さない）。戻り値 = 更新した個数。
    static int RefreshPrefabInstances(Scene& scene, const std::string& sourcePath,
                                      const std::string& assetsDir, entt::entity except);
};

} // namespace dx12e
