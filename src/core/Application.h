#pragma once

#include "Types.h"
#include "Window.h"
#include "GameClock.h"

#include <memory>
#include <vector>
#include <wrl/client.h>
#include <directx/d3d12.h>

// Forward declarations for graphics module
namespace dx12e
{
    class GraphicsDevice;
    class CommandQueue;
    class SwapChain;
    class FrameResources;
    class DescriptorHeap;
    class RootSignature;
    class PipelineState;
    class CommandList;
    class ConstantBuffer;
    class Mesh;
    class Camera;
    class ResourceManager;
    struct Material;
    struct MeshData;
}

namespace dx12e
{

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize(HINSTANCE hInstance, int nCmdShow);
    void Run();
    void Shutdown();

private:
    void Update();
    void Render();

    std::unique_ptr<Window>         m_window;
    std::unique_ptr<GraphicsDevice> m_graphicsDevice;
    std::unique_ptr<CommandQueue>   m_commandQueue;
    std::unique_ptr<SwapChain>      m_swapChain;
    std::unique_ptr<FrameResources> m_frameResources;
    std::unique_ptr<DescriptorHeap>    m_descriptorHeap;
    std::unique_ptr<DescriptorHeap>    m_dsvHeap;
    std::unique_ptr<RootSignature>     m_rootSignature;
    std::unique_ptr<PipelineState>     m_pipelineState;
    std::unique_ptr<DescriptorHeap>    m_srvHeap;
    std::unique_ptr<ResourceManager>   m_resourceManager;
    std::vector<std::unique_ptr<Mesh>>     m_modelMeshes;
    std::vector<std::unique_ptr<Material>> m_modelMaterials;
    std::unique_ptr<Camera>            m_camera;
    std::unique_ptr<ConstantBuffer>    m_perFrameCB;
    std::unique_ptr<CommandList>       m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE        m_dsvHandle{};
    GameClock                          m_gameClock;
    bool                               m_isRunning = false;
};

} // namespace dx12e
