#pragma once

#include <memory>
#include <string>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <directx/d3d12.h>

#include "core/Types.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "resource/ShaderRuntimeCompiler.h"

namespace dx12e
{

class GraphicsDevice;
class CommandList;
class Mesh;
class PipelineState;
class ResourceManager;

enum class MaterialPreviewShape { Sphere, Plane };

// マテリアルエディタの3Dプレビュー(球体/平面)描画を担う。メインのForward.hlsl(9スロット
// RootSignature、CSM/IBL/SSAO込み)とは完全に独立した専用の小さいパイプライン
// (shaders/forward/MaterialPreview.hlsl、固定2灯スタジオライト)を持つ。
// メインシーンの状態(ライト/シャドウ/IBL等)には一切依存しない=常に一定の見た目でプレビューできる。
class MaterialPreviewRenderer
{
public:
    // パスは assets 相対(PathResolver::AssetsDir() 基準)。空 = 未使用/デフォルトへフォールバック。
    // 解決(GetOrLoadTexture)は Render() が毎フレーム行う(MaterialAssetManager::LoadInto と同じ方針)。
    struct DrawInput
    {
        std::string albedoPath;
        std::string normalPath;
        std::string metalRoughnessPath;
        f32 metallic  = 1.0f;
        f32 roughness = 1.0f;
    };

    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, ResourceManager* resourceManager);
    bool IsValid() const { return m_valid; }

    // オフスクリーンRTへ描画。cmd/nativeCmdListは呼び出し側が用意した有効なコマンドリスト。
    void Render(CommandList& cmd, const DrawInput& input, MaterialPreviewShape shape,
               f32 camYaw, f32 camPitch, f32 camDist);

    u64 GetPreviewGpuHandle() const;
    static constexpr u32 kPreviewSize = 512;

private:
    void BuildPipeline(GraphicsDevice& device);
    void BuildDepthBuffer(GraphicsDevice& device);

    GraphicsDevice*   m_device = nullptr;
    DescriptorHeap*   m_srvHeap = nullptr;
    ResourceManager*  m_resourceManager = nullptr;
    bool m_valid = false;

    ShaderRuntimeCompiler m_compiler;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    std::unique_ptr<PipelineState> m_pso;

    DescriptorHeap m_previewRtvHeap;
    RenderTarget   m_previewRT;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    DescriptorHeap m_dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};

    // マテリアルテクスチャ3枚分のSRVブロック(albedo/normal/metalRoughness、連続3スロット)。
    // 毎フレームCreateSRVを呼び直す(エディタツールなのでコストは無視できる)。
    u32 m_srvBlockStart = 0xFFFFFFFFu;

    std::unique_ptr<Mesh> m_sphereMesh;
    std::unique_ptr<Mesh> m_planeMesh;
};

} // namespace dx12e
