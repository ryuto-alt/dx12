# Scene Management + Multi-Object Rendering Design

## Overview

DX12エンジンにシーン管理機能を追加し、複数オブジェクトを配置・描画できるようにする。
現状はApplication.cppに1モデルがハードコードされている状態を、Scene/Entity構造に移行する。

## 設計方針

- **アプローチ**: シンプルEntityリスト（ECSではない）
- **コード駆動**: `Scene::Spawn()` でC++コードから直接配置（エディター/ファイル不要）
- **AI駆動コンセプトに適合**: AIがC++コードを生成してシーンを構築する
- **段階的進化**: 物理エンジン追加時にコンポーネント化へ移行可能

## コンポーネント設計

### Transform (src/renderer/Transform.h/.cpp)

位置・回転・スケールを保持し、ワールド行列を合成する。

```cpp
struct Transform {
    XMFLOAT3 position = {0, 0, 0};
    XMFLOAT3 rotation = {0, 0, 0};  // Euler (degrees, YXZ order)
    XMFLOAT3 scale    = {1, 1, 1};

    XMMATRIX GetWorldMatrix() const;  // Scale * RotationYXZ * Translation
};
```

回転順序はYXZ（FPS標準: Yaw→Pitch→Roll）。

### Entity (src/scene/Entity.h)

シーン内の1オブジェクトを表す。メッシュ/マテリアルはResourceManagerからの共有参照。

```cpp
struct Entity {
    std::string name;
    Transform transform;

    // 描画データ（ResourceManagerから借用、Entity側でdeleteしない）
    std::vector<Mesh*> meshes;
    std::vector<Material*> materials;
    bool hasSkeleton = false;

    // アニメーション（スケルタルメッシュのみ、Entity固有）
    std::unique_ptr<Skeleton> skeleton;
    std::vector<std::unique_ptr<AnimationClip>> animClips;
    std::unique_ptr<Animator> animator;
    std::unique_ptr<SkinningBuffer> skinningBuffer;
};
```

### Scene (src/scene/Scene.h/.cpp)

Entity群のコンテナ。生成・削除・更新・列挙を担う。

```cpp
class Scene {
public:
    Entity* Spawn(const std::string& name,
                  const std::string& modelPath,
                  XMFLOAT3 position,
                  XMFLOAT3 rotation = {0, 0, 0},
                  XMFLOAT3 scale = {1, 1, 1});

    void Remove(Entity* entity);
    void Clear();

    void Update(f32 dt);  // 全Entityのアニメータ更新

    template<typename Fn>
    void ForEachEntity(Fn&& fn) const;  // 描画ループ用

    size_t GetEntityCount() const;

private:
    std::vector<std::unique_ptr<Entity>> m_entities;
    ResourceManager* m_resourceManager;  // 外部所有
    GraphicsDevice* m_graphicsDevice;    // SkinningBuffer作成用
    DescriptorHeap* m_srvHeap;           // SRVインデックス割当用
};
```

### ResourceManager の拡張

モデル全体のキャッシュ機能を追加。同一モデルを複数Entityで共有する。

```cpp
// 既存: テクスチャキャッシュ
// 追加: モデルデータキャッシュ
struct CachedModel {
    std::vector<std::unique_ptr<Mesh>> meshes;
    std::vector<std::unique_ptr<Material>> materials;
    std::unique_ptr<Skeleton> skeleton;           // nullならスタティック
    std::vector<std::unique_ptr<AnimationClip>> clips;
};

const CachedModel* LoadModel(const std::string& path);  // キャッシュ付き
```

メッシュ/マテリアルはCachedModelが所有し、Entityは生ポインタで参照する。
Skeleton/AnimationClip/Animatorはクローンが必要（各Entity独立で再生するため）。

## 描画フローの変更

### Before (Application.cpp に1モデルハードコード)
```
Render():
  set pipeline
  set per-frame CB
  update skinning buffer
  for each mesh: draw
```

### After (Sceneベース)
```
Update():
  scene->Update(dt)  // 全EntityのAnimator更新

Render():
  set per-frame CB (view/proj/light)
  scene->ForEachEntity([&](const Entity& entity) {
      XMMATRIX world = entity.transform.GetWorldMatrix();
      if (entity.hasSkeleton) {
          set skinned pipeline
          update & bind skinning buffer
      } else {
          set static pipeline
      }
      for each mesh in entity:
          set MVP + Model constants
          set texture SRV
          draw
  });
```

## ディレクトリ構造の変更

```
src/
├── scene/           # 新規
│   ├── CMakeLists.txt
│   ├── Entity.h
│   ├── Scene.h
│   ├── Scene.cpp
│   └── Transform.h
│   └── Transform.cpp
```

## モジュール依存関係の変更

```
Scene → Renderer, Animation, Resource, Graphics, Core
DX12Engine → Core, Graphics, Renderer, Animation, Resource, Input, Gui, Scene
```

## Application.cpp での使用例

```cpp
// Initialize
m_scene = std::make_unique<Scene>(m_resourceManager.get(), ...);
m_scene->Spawn("human1", "assets/models/human/walk.gltf", {0, 0, 0});
m_scene->Spawn("human2", "assets/models/human/walk.gltf", {3, 0, 0});
m_scene->Spawn("floor", "assets/models/floor.gltf", {0, -1, 0}, {}, {10, 1, 10});

// Update
m_scene->Update(dt);

// Render
m_scene->ForEachEntity([&](const Entity& e) { /* draw */ });
```

## ImGui連携

- EntityリストをImGuiで表示（名前、位置）
- 選択したEntityのTransformを編集可能に

## スコープ外（将来）

- フラスタムカリング
- インスタンス描画
- ECSへの移行
- 物理コンポーネント
- 親子Transform階層
