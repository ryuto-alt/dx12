#pragma once

#include <unordered_map>
#include <memory>
#include <string>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Texture;
class GraphicsDevice;
class DescriptorHeap;

class ResourceManager
{
public:
    void Initialize(GraphicsDevice* device, DescriptorHeap* srvHeap,
                    ID3D12GraphicsCommandList* cmdList);

    Texture* GetOrLoadTexture(
        const std::wstring& filePath,
        ID3D12GraphicsCommandList* cmdList);

    Texture* GetDefaultWhiteTexture() const { return m_defaultWhite.get(); }

    void FinishUploads();

private:
    GraphicsDevice*  m_device  = nullptr;
    DescriptorHeap*  m_srvHeap = nullptr;
    std::unordered_map<std::wstring, std::unique_ptr<Texture>> m_textureCache;
    std::unique_ptr<Texture> m_defaultWhite;
};

} // namespace dx12e
