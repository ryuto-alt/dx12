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
class GraphicsDevice;
class DescriptorHeap;

// モデルキャッシュ（同一パスのモデルを共有）
struct CachedModel
{
    std::vector<std::unique_ptr<Mesh>>          meshes;
    std::vector<std::unique_ptr<Material>>      materials;
    std::unique_ptr<Skeleton>                   skeleton;   // null = static mesh
    std::vector<std::unique_ptr<AnimationClip>> animClips;
};

class ResourceManager
{
public:
    void Initialize(GraphicsDevice* device, DescriptorHeap* srvHeap,
                    ID3D12GraphicsCommandList* cmdList);

    Texture* GetOrLoadTexture(
        const std::wstring& filePath,
        ID3D12GraphicsCommandList* cmdList);

    // モデル読み込み（キャッシュ付き）
    const CachedModel* GetOrLoadModel(
        const std::string& filePath,
        ID3D12GraphicsCommandList* cmdList);

    Texture* GetDefaultWhiteTexture() const { return m_defaultWhite.get(); }
    GraphicsDevice* GetDevice() const { return m_device; }
    DescriptorHeap* GetSrvHeap() const { return m_srvHeap; }

    void FinishUploads();

private:
    GraphicsDevice*  m_device  = nullptr;
    DescriptorHeap*  m_srvHeap = nullptr;
    std::unordered_map<std::wstring, std::unique_ptr<Texture>> m_textureCache;
    std::unique_ptr<Texture> m_defaultWhite;
    std::unordered_map<std::string, std::unique_ptr<CachedModel>> m_modelCache;
};

} // namespace dx12e
