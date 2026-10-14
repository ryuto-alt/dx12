#pragma once

#include <vector>
#include <memory>
#include <filesystem>

#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class ResourceManager;

struct ModelData
{
    std::vector<std::unique_ptr<Mesh>>     meshes;
    std::vector<std::unique_ptr<Material>> materials;
    std::unique_ptr<Skeleton>              skeleton;   // null = static mesh
    std::vector<std::unique_ptr<AnimationClip>> animClips;  // empty = no animation

    // Node animation（skeleton が null でアニメーションがある場合）
    std::unique_ptr<NodeGraph>                         nodeGraph;
    std::vector<std::unique_ptr<NodeAnimationClip>>    nodeAnimClips;
};

// MCP asset_info 用: GPU を使わずモデルのメタ情報だけ読む(Assimp のみ)。
struct ModelProbeInfo
{
    bool        ok = false;
    std::string error;
    uint32_t meshCount = 0, materialCount = 0, boneCount = 0;
    uint64_t totalVertices = 0, totalFaces = 0;
    bool     hasSkeleton = false;
    struct Anim { std::string name; double durationSec = 0.0; };
    std::vector<Anim> animations;
    // ノード変換込みのワールド空間 AABB(= スケール1で置いた時の実サイズ)
    float aabbMin[3] = {0, 0, 0}, aabbMax[3] = {0, 0, 0};
};

// glTF の occlusionTexture の読み取り結果（マテリアル 1 つ分。GPU 不要。テスト・診断用）。
struct OcclusionProbe
{
    std::string material;
    bool  hasOcclusion      = false;   // 使える occlusionTexture がある（glTF/GLB・UV0）
    bool  hasMetalRoughness = false;
    bool  sharedWithMR      = false;   // metallicRoughness と同じ画像（ORM 1 枚）。false なら読み込み時に ORM を作る
    float strength          = 1.0f;
};

// モデルごとの取り込みオプション。モデルファイルの隣の `<モデルファイル名>.import.json`
// （例 BistroExterior.fbx.import.json）から読む。キーを足すときはここへメンバーと Parse の行を足す。
struct ModelImportOptions
{
    // FBX の SpecularColor スロットを metallicRoughness（G=roughness / B=metallic、R は未使用で AO にしない）として読む（Falcor 規約）。
    // 一般の FBX では Specular は本物の鏡面反射色なので既定は false。
    bool specularIsORM = false;

    // 静的モデル（スキン無し・アニメ無し）の Mesh を「同じ aiMaterial × XZ 空間セル」ごとに 1 個へ結合する
    // （都市 FBX のようにメッシュが数万ある時のドローコール削減）。既定 false＝従来と同じ。
    bool mergeStatic = false;
    // 結合セルの一辺（モデル空間の単位）。0 以下 / 省略は自動 = モデル AABB の XZ 最大辺 / 8。
    float mergeCellSize = 0.0f;

    // JSON 文字列から読む。壊れている / キーが無い / 型違いは既定値（全部 false / 0）。
    static ModelImportOptions Parse(const std::string& jsonText);
    // モデルの隣の import.json を vfs 経由で読む（配布ゲームの pak でも読める）。無ければ既定値。
    static ModelImportOptions LoadFor(const std::filesystem::path& modelPath);
};

class ModelLoader
{
public:
    // glTF の occlusionTexture をマテリアルごとに調べる（LoadFromFile と同じ判定）。読めなければ空。
    static std::vector<OcclusionProbe> ProbeOcclusion(const std::filesystem::path& filePath);

    static ModelProbeInfo Probe(const std::filesystem::path& filePath);

    static ModelData LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::filesystem::path& filePath,
        ResourceManager& resourceManager);

    static std::vector<std::unique_ptr<AnimationClip>> LoadAnimationsFromFile(
        const std::filesystem::path& filePath,
        const Skeleton& skeleton);
};

} // namespace dx12e
