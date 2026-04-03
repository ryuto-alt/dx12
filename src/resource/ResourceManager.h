#pragma once

#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Texture;
class Mesh;
struct Material;
class Skeleton;
class AnimationClip;
class NodeGraph;
class NodeAnimationClip;
class GraphicsDevice;
class DescriptorHeap;

// モデルキャッシュ（同一パスのモデルを共有）
struct CachedModel
{
    std::vector<std::unique_ptr<Mesh>>          meshes;
    std::vector<std::unique_ptr<Material>>      materials;
    std::unique_ptr<Skeleton>                   skeleton;   // null = static mesh
    std::vector<std::unique_ptr<AnimationClip>> animClips;

    // Node animation（skeleton が null でアニメーションがある場合）
    std::unique_ptr<NodeGraph>                         nodeGraph;
    std::vector<std::unique_ptr<NodeAnimationClip>>    nodeAnimClips;
};

class ResourceManager
{
public:
    void Initialize(GraphicsDevice* device, DescriptorHeap* srvHeap,
                    ID3D12GraphicsCommandList* cmdList);

    Texture* GetOrLoadTexture(
        const std::wstring& filePath,
        ID3D12GraphicsCommandList* cmdList,
        bool srgb = true);

    // モデル読み込み（キャッシュ付き）
    const CachedModel* GetOrLoadModel(
        const std::string& filePath,
        ID3D12GraphicsCommandList* cmdList);

    // 埋め込みテクスチャ用
    Texture* GetOrLoadEmbeddedTexture(
        const std::string& key,
        const uint8_t* data, size_t dataSize,
        const char* formatHint,
        ID3D12GraphicsCommandList* cmdList);

    Texture* GetDefaultWhiteTexture() const { return m_defaultWhite.get(); }
    Texture* GetDefaultNormalTexture() const { return m_defaultNormal.get(); }
    Texture* GetDefaultMetalRoughnessTexture() const { return m_defaultMetalRoughness.get(); }
    GraphicsDevice* GetDevice() const { return m_device; }
    DescriptorHeap* GetSrvHeap() const { return m_srvHeap; }

    void FinishUploads();

private:
    GraphicsDevice*  m_device  = nullptr;
    DescriptorHeap*  m_srvHeap = nullptr;
    std::unordered_map<std::wstring, std::unique_ptr<Texture>> m_textureCache;
    std::unique_ptr<Texture> m_defaultWhite;
    std::unique_ptr<Texture> m_defaultNormal;         // (128,128,255,255) = flat normal
    std::unique_ptr<Texture> m_defaultMetalRoughness; // (0,128,0,255) = non-metal, mid-rough
    std::unordered_map<std::string, std::unique_ptr<CachedModel>> m_modelCache;
};

} // namespace dx12e
