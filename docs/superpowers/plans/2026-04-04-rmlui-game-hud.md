# RmlUi ゲームHUD統合 実装計画

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RmlUi（HTML/CSSベースUIライブラリ）をDX12エンジンに統合し、Luaスクリプトから操作可能なゲームHUDを実現する。

**Architecture:** RmlUiの3つのインターフェース（RenderInterface/SystemInterface/FileInterface）をDX12バックエンドで自前実装し、Application の描画パイプラインに組み込む。Luaバインドで `ui:setText()` 等のAPI を提供し、RML/RCSSファイルでHUDをマークアップする。

**Tech Stack:** RmlUi (vcpkg), FreeType (RmlUi依存), DX12 (専用PSO/RootSignature/Shader), sol2 (Luaバインド)

---

## ファイル構成

| 操作 | パス | 役割 |
|------|------|------|
| Create | `src/gui/RmlSystem.h` | Rml::SystemInterface 実装（ヘッダー） |
| Create | `src/gui/RmlSystem.cpp` | Rml::SystemInterface 実装 |
| Create | `src/gui/RmlRenderer.h` | Rml::RenderInterface 実装（ヘッダー） |
| Create | `src/gui/RmlRenderer.cpp` | Rml::RenderInterface 実装（DX12描画バックエンド） |
| Create | `src/gui/RmlUIManager.h` | 統括クラス（初期化・入力・更新） |
| Create | `src/gui/RmlUIManager.cpp` | 統括クラス実装 |
| Create | `shaders/ui/UI.hlsl` | 2D UI用シェーダー（ortho投影 + premultiplied alpha） |
| Create | `assets/ui/hud.rml` | サンプルHUDマークアップ |
| Create | `assets/ui/hud.rcss` | サンプルHUDスタイル |
| Modify | `vcpkg.json` | rmlui + freetype 追加 |
| Modify | `CMakeLists.txt` | UIシェーダーコンパイル追加 |
| Modify | `src/gui/CMakeLists.txt` | RmlUi ソース + リンク追加 |
| Modify | `src/core/Application.h` | RmlUIManager メンバー追加 |
| Modify | `src/core/Application.cpp` | 初期化・描画・入力統合 |
| Modify | `src/core/Window.h` | RmlUi 入力転送コールバック追加 |
| Modify | `src/core/Window.cpp` | WndProc にRmlUi入力転送追加 |
| Modify | `src/scripting/ScriptEngine.h` | RmlUIManager ポインタ追加 |
| Modify | `src/scripting/ScriptEngine.cpp` | ui Luaバインド追加 |

---

### Task 1: vcpkg + CMake 依存追加

**Files:**
- Modify: `vcpkg.json`
- Modify: `CMakeLists.txt`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: vcpkg.json に rmlui 追加**

```json
{
  "name": "dx12-engine",
  "version-string": "0.1.0",
  "builtin-baseline": "cb2981c4e03d421fa03b9bb5044cd1986180e7e4",
  "dependencies": [
    "spdlog",
    "directx-headers",
    "d3d12-memory-allocator",
    "directxtex",
    "directxmath",
    {
      "name": "imgui",
      "features": ["docking-experimental", "dx12-binding", "win32-binding"]
    },
    "lua",
    "sol2",
    "entt",
    "nlohmann-json",
    "joltphysics",
    {
      "name": "rmlui",
      "features": ["freetype"]
    }
  ]
}
```

- [ ] **Step 2: src/gui/CMakeLists.txt に RmlUi ソースとリンク追加**

```cmake
find_package(imgui CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)

add_library(Gui STATIC
    ImGuiManager.cpp
    ImGuizmo.cpp
    RmlSystem.cpp
    RmlRenderer.cpp
    RmlUIManager.cpp
)

set_source_files_properties(ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")

target_include_directories(Gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(Gui PUBLIC Graphics imgui::imgui RmlUi::RmlUi)

if(MSVC)
    target_compile_options(Gui PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Gui PRIVATE ${DX12_COMPILE_DEFS})
endif()
```

- [ ] **Step 3: ルート CMakeLists.txt に UI シェーダーコンパイル追加**

`CMakeLists.txt` の DXC shader compilation セクションに以下を追加:

```cmake
set(SHADER_UI "${CMAKE_SOURCE_DIR}/shaders/ui/UI.hlsl")
```

SHADER_OUTPUTS リストに追加:
```cmake
"${SHADER_OUTPUT_DIR}/UI_VS.cso"
"${SHADER_OUTPUT_DIR}/UI_PS.cso"
```

コンパイルコマンド追加:
```cmake
add_custom_command(
    OUTPUT "${SHADER_OUTPUT_DIR}/UI_VS.cso"
    COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/UI_VS.cso" "${SHADER_UI}"
    DEPENDS "${SHADER_UI}"
    COMMENT "Compiling UI.hlsl -> UI_VS.cso (vs_6_0)"
)

add_custom_command(
    OUTPUT "${SHADER_OUTPUT_DIR}/UI_PS.cso"
    COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/UI_PS.cso" "${SHADER_UI}"
    DEPENDS "${SHADER_UI}"
    COMMENT "Compiling UI.hlsl -> UI_PS.cso (ps_6_0)"
)
```

- [ ] **Step 4: vcpkg install 実行**

Run: `vcpkg install` (VCPKG_ROOT 環境変数が設定済み前提)
Expected: rmlui と freetype がインストールされる

- [ ] **Step 5: コミット**

```bash
git add vcpkg.json CMakeLists.txt src/gui/CMakeLists.txt
git commit -m "feat: add RmlUi dependency and shader build config"
```

---

### Task 2: UI シェーダー作成

**Files:**
- Create: `shaders/ui/UI.hlsl`

- [ ] **Step 1: UI.hlsl を作成**

RmlUi の頂点は `float2 position, byte4 colour (premultiplied), float2 texcoord`。
ortho射影 + translation で画面座標系に変換する。テクスチャが無い場合は白(1,1,1,1)をサンプル。

```hlsl
// UI.hlsl - 2D UI rendering for RmlUi
// RootConstants b0: float4x4 ortho + float2 translation (18 DWORD)
// SRV t0: UI texture (optional, white fallback)
// Sampler s0: linear clamp

cbuffer UIConstants : register(b0)
{
    float4x4 gOrtho;       // orthographic projection
    float2   gTranslation; // per-draw translation offset
    float2   _pad;
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSIn
{
    float2 pos    : POSITION;
    float4 color  : COLOR;    // premultiplied alpha RGBA
    float2 uv     : TEXCOORD;
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

PSIn VSMain(VSIn v)
{
    PSIn o;
    float2 translated = v.pos + gTranslation;
    o.pos   = mul(float4(translated, 0.0f, 1.0f), gOrtho);
    o.color = v.color;
    o.uv    = v.uv;
    return o;
}

float4 PSMain(PSIn p) : SV_TARGET
{
    float4 texColor = gTexture.Sample(gSampler, p.uv);
    return texColor * p.color;
}
```

- [ ] **Step 2: コミット**

```bash
git add shaders/ui/UI.hlsl
git commit -m "feat: add 2D UI shader for RmlUi (ortho + premultiplied alpha)"
```

---

### Task 3: RmlSystem (SystemInterface) 実装

**Files:**
- Create: `src/gui/RmlSystem.h`
- Create: `src/gui/RmlSystem.cpp`

- [ ] **Step 1: RmlSystem.h を作成**

```cpp
#pragma once

#include <RmlUi/Core/SystemInterface.h>

namespace dx12e
{

class GameClock;

class RmlSystem : public Rml::SystemInterface
{
public:
    explicit RmlSystem(const GameClock& clock);

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

private:
    const GameClock& m_clock;
};

} // namespace dx12e
```

- [ ] **Step 2: RmlSystem.cpp を作成**

```cpp
#include "gui/RmlSystem.h"
#include "core/GameClock.h"
#include "core/Logger.h"

namespace dx12e
{

RmlSystem::RmlSystem(const GameClock& clock)
    : m_clock(clock)
{
}

double RmlSystem::GetElapsedTime()
{
    return static_cast<double>(m_clock.GetTotalTime());
}

bool RmlSystem::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    switch (type)
    {
    case Rml::Log::LT_ERROR:
    case Rml::Log::LT_ASSERT:
        Logger::Error("[RmlUi] {}", message);
        break;
    case Rml::Log::LT_WARNING:
        Logger::Warn("[RmlUi] {}", message);
        break;
    default:
        Logger::Info("[RmlUi] {}", message);
        break;
    }
    return true;
}

} // namespace dx12e
```

- [ ] **Step 3: コミット**

```bash
git add src/gui/RmlSystem.h src/gui/RmlSystem.cpp
git commit -m "feat: RmlSystem - SystemInterface for RmlUi (time + logging)"
```

---

### Task 4: RmlRenderer (RenderInterface) 実装

**Files:**
- Create: `src/gui/RmlRenderer.h`
- Create: `src/gui/RmlRenderer.cpp`

これが最も重要なタスク。DX12でRmlUiのジオメトリ・テクスチャを描画するバックエンド。

- [ ] **Step 1: RmlRenderer.h を作成**

```cpp
#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <unordered_map>
#include <string>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class Texture;

struct RmlCompiledGeometry
{
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW  ibView{};
    u32 indexCount = 0;
};

struct RmlTextureData
{
    std::unique_ptr<Texture> texture;
    u32 srvIndex = UINT32_MAX;
};

class RmlRenderer : public Rml::RenderInterface
{
public:
    RmlRenderer() = default;
    ~RmlRenderer() override;

    RmlRenderer(const RmlRenderer&) = delete;
    RmlRenderer& operator=(const RmlRenderer&) = delete;

    void Initialize(GraphicsDevice& device, DescriptorHeap& srvHeap,
                    DXGI_FORMAT rtvFormat, const std::wstring& shaderDir);

    // フレーム開始時にコマンドリストとビューポートサイズを設定
    void BeginFrame(ID3D12GraphicsCommandList* cmdList,
                    ID3D12DescriptorHeap* srvHeap,
                    u32 viewportWidth, u32 viewportHeight);

    // --- Rml::RenderInterface overrides ---
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    void CreateRootSignature();
    void CreatePSO(DXGI_FORMAT rtvFormat, const std::wstring& shaderDir);
    Rml::TextureHandle CreateTextureFromRGBA(const Rml::byte* data, u32 width, u32 height);

    GraphicsDevice*  m_device  = nullptr;
    DescriptorHeap*  m_srvHeap = nullptr;

    // Pipeline
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // Per-frame state
    ID3D12GraphicsCommandList* m_cmdList = nullptr;
    u32 m_viewportWidth  = 0;
    u32 m_viewportHeight = 0;
    bool m_scissorEnabled = false;

    // White fallback texture (for geometry without textures)
    u32 m_whiteSrvIndex = UINT32_MAX;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_whiteTexture;

    // Handle tracking
    uintptr_t m_nextGeometryHandle = 1;
    uintptr_t m_nextTextureHandle  = 1;
    std::unordered_map<uintptr_t, RmlCompiledGeometry> m_geometries;
    std::unordered_map<uintptr_t, RmlTextureData>      m_textures;
};

} // namespace dx12e
```

- [ ] **Step 2: RmlRenderer.cpp を作成**

```cpp
#include "gui/RmlRenderer.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/Texture.h"
#include "resource/ShaderCompiler.h"

#include <cstring>

using namespace DirectX;

namespace dx12e
{

RmlRenderer::~RmlRenderer()
{
    m_geometries.clear();
    m_textures.clear();
}

void RmlRenderer::Initialize(GraphicsDevice& device, DescriptorHeap& srvHeap,
                              DXGI_FORMAT rtvFormat, const std::wstring& shaderDir)
{
    m_device  = &device;
    m_srvHeap = &srvHeap;

    CreateRootSignature();
    CreatePSO(rtvFormat, shaderDir);

    // 1x1 白テクスチャ作成（テクスチャ無しジオメトリ用）
    const Rml::byte white[] = { 255, 255, 255, 255 };
    auto handle = CreateTextureFromRGBA(white, 1, 1);
    m_whiteSrvIndex = m_textures[handle].srvIndex;

    Logger::Info("RmlRenderer initialized");
}

// ========== Pipeline Setup ==========

void RmlRenderer::CreateRootSignature()
{
    auto* dev = m_device->GetDevice();

    // Slot 0: RootConstants b0 (20 DWORD = ortho 4x4 + translation 2 + pad 2)
    D3D12_ROOT_PARAMETER params[2]{};

    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace  = 0;
    params[0].Constants.Num32BitValues = 20; // float4x4(16) + float2(2) + pad(2)
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 1: DescriptorTable t0 (UI texture SRV)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler: linear clamp
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = _countof(params);
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
               | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &serialized, &error));
    ThrowIfFailed(dev->CreateRootSignature(0,
        serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));
}

void RmlRenderer::CreatePSO(DXGI_FORMAT rtvFormat, const std::wstring& shaderDir)
{
    auto* dev = m_device->GetDevice();

    auto vsData = ShaderCompiler::LoadFromFile(shaderDir + L"UI_VS.cso");
    auto psData = ShaderCompiler::LoadFromFile(shaderDir + L"UI_PS.cso");

    // RmlUi vertex layout: float2 pos, byte4 color, float2 uv
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,      0,  8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vsData.GetData(), vsData.GetSize() };
    psoDesc.PS = { psData.GetData(), psData.GetSize() };
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Rasterizer: no culling
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    // Blend: premultiplied alpha
    auto& rt0 = psoDesc.BlendState.RenderTarget[0];
    rt0.BlendEnable           = TRUE;
    rt0.SrcBlend              = D3D12_BLEND_ONE;           // premultiplied: src already has alpha baked
    rt0.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOp               = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth: disabled
    psoDesc.DepthStencilState.DepthEnable    = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    psoDesc.SampleMask      = UINT_MAX;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0]   = rtvFormat;
    psoDesc.DSVFormat        = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc       = { 1, 0 };

    ThrowIfFailed(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));
}

// ========== Frame Management ==========

void RmlRenderer::BeginFrame(ID3D12GraphicsCommandList* cmdList,
                              ID3D12DescriptorHeap* srvHeap,
                              u32 viewportWidth, u32 viewportHeight)
{
    m_cmdList        = cmdList;
    m_viewportWidth  = viewportWidth;
    m_viewportHeight = viewportHeight;

    // Set pipeline state
    m_cmdList->SetPipelineState(m_pso.Get());
    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Set descriptor heap (shared SRV heap)
    m_cmdList->SetDescriptorHeaps(1, &srvHeap);

    // Set ortho projection
    // RmlUi uses top-left origin, Y-down screen coordinates
    XMFLOAT4X4 ortho;
    XMStoreFloat4x4(&ortho, XMMatrixTranspose(
        XMMatrixOrthographicOffCenterLH(
            0.0f, static_cast<f32>(viewportWidth),
            static_cast<f32>(viewportHeight), 0.0f,  // bottom=height, top=0 for Y-down
            -1.0f, 1.0f)));

    // Upload ortho matrix (16 floats at offset 0)
    m_cmdList->SetGraphicsRoot32BitConstants(0, 16, &ortho, 0);

    // Set viewport
    D3D12_VIEWPORT viewport{};
    viewport.Width    = static_cast<f32>(viewportWidth);
    viewport.Height   = static_cast<f32>(viewportHeight);
    viewport.MaxDepth = 1.0f;
    m_cmdList->RSSetViewports(1, &viewport);

    // Default scissor (full viewport)
    D3D12_RECT scissor = { 0, 0,
                           static_cast<LONG>(viewportWidth),
                           static_cast<LONG>(viewportHeight) };
    m_cmdList->RSSetScissorRects(1, &scissor);

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ========== Geometry ==========

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices,
    Rml::Span<const int> indices)
{
    auto* dev = m_device->GetDevice();

    RmlCompiledGeometry geom;
    geom.indexCount = static_cast<u32>(indices.size());

    // Vertex buffer (upload heap - UI data is small and changes often)
    {
        const UINT size = static_cast<UINT>(vertices.size() * sizeof(Rml::Vertex));

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width            = size;
        resDesc.Height           = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels        = 1;
        resDesc.SampleDesc       = { 1, 0 };
        resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&geom.vertexBuffer)));

        void* mapped = nullptr;
        ThrowIfFailed(geom.vertexBuffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, vertices.data(), size);
        geom.vertexBuffer->Unmap(0, nullptr);

        geom.vbView.BufferLocation = geom.vertexBuffer->GetGPUVirtualAddress();
        geom.vbView.StrideInBytes  = sizeof(Rml::Vertex);
        geom.vbView.SizeInBytes    = size;
    }

    // Index buffer (upload heap)
    {
        const UINT size = static_cast<UINT>(indices.size() * sizeof(int));

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width            = size;
        resDesc.Height           = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels        = 1;
        resDesc.SampleDesc       = { 1, 0 };
        resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&geom.indexBuffer)));

        void* mapped = nullptr;
        ThrowIfFailed(geom.indexBuffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, indices.data(), size);
        geom.indexBuffer->Unmap(0, nullptr);

        geom.ibView.BufferLocation = geom.indexBuffer->GetGPUVirtualAddress();
        geom.ibView.Format         = DXGI_FORMAT_R32_UINT;
        geom.ibView.SizeInBytes    = size;
    }

    uintptr_t handle = m_nextGeometryHandle++;
    m_geometries[handle] = std::move(geom);
    return static_cast<Rml::CompiledGeometryHandle>(handle);
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                  Rml::Vector2f translation,
                                  Rml::TextureHandle texture)
{
    auto it = m_geometries.find(static_cast<uintptr_t>(geometry));
    if (it == m_geometries.end()) return;

    const auto& geom = it->second;

    // Translation (2 floats at offset 16)
    float trans[4] = { translation.x, translation.y, 0.0f, 0.0f };
    m_cmdList->SetGraphicsRoot32BitConstants(0, 4, trans, 16);

    // Texture SRV
    u32 srvIndex = m_whiteSrvIndex;
    if (texture)
    {
        auto texIt = m_textures.find(static_cast<uintptr_t>(texture));
        if (texIt != m_textures.end())
            srvIndex = texIt->second.srvIndex;
    }
    m_cmdList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGpuHandle(srvIndex));

    // Draw
    m_cmdList->IASetVertexBuffers(0, 1, &geom.vbView);
    m_cmdList->IASetIndexBuffer(&geom.ibView);
    m_cmdList->DrawIndexedInstanced(geom.indexCount, 1, 0, 0, 0);
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    m_geometries.erase(static_cast<uintptr_t>(geometry));
}

// ========== Textures ==========

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions,
                                             const Rml::String& source)
{
    // source はファイルパス（相対 or 絶対）
    // wstring に変換して TextureLoader で読み込み
    int wLen = MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wLen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, wpath.data(), wLen);

    // テクスチャ読み込み用の一時コマンドリスト
    // 注意: RmlUi 初期化時（フォントアトラス生成等）にも呼ばれるので
    // m_cmdList が設定されてない場合がある → GenerateTexture を使う方が安全
    // ここでは LoadTexture は画像ファイルのパスだけ記録し、
    // 実際にはGenerateTextureと同じ方法で生成する

    // DirectXTex で画像を読み込み、ピクセルデータを取得
    DirectX::ScratchImage scratchImage;
    const std::wstring ext = wpath.substr(wpath.find_last_of(L'.'));

    HRESULT hr;
    if (ext == L".dds" || ext == L".DDS")
        hr = DirectX::LoadFromDDSFile(wpath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    else
        hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, scratchImage);

    if (FAILED(hr))
    {
        Logger::Warn("[RmlUi] Failed to load texture: {}", source);
        return {};
    }

    // RGBA8に変換
    DirectX::ScratchImage converted;
    const DirectX::TexMetadata& meta = scratchImage.GetMetadata();
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*scratchImage.GetImage(0, 0, 0),
                              DXGI_FORMAT_R8G8B8A8_UNORM,
                              DirectX::TEX_FILTER_DEFAULT, 0.5f, converted);
        if (FAILED(hr))
        {
            Logger::Warn("[RmlUi] Failed to convert texture format: {}", source);
            return {};
        }
    }

    const DirectX::Image* image = (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
        ? converted.GetImage(0, 0, 0)
        : scratchImage.GetImage(0, 0, 0);

    texture_dimensions.x = static_cast<int>(image->width);
    texture_dimensions.y = static_cast<int>(image->height);

    return CreateTextureFromRGBA(
        static_cast<const Rml::byte*>(image->pixels),
        static_cast<u32>(image->width),
        static_cast<u32>(image->height));
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                  Rml::Vector2i source_dimensions)
{
    return CreateTextureFromRGBA(
        source.data(),
        static_cast<u32>(source_dimensions.x),
        static_cast<u32>(source_dimensions.y));
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle texture)
{
    m_textures.erase(static_cast<uintptr_t>(texture));
}

Rml::TextureHandle RmlRenderer::CreateTextureFromRGBA(const Rml::byte* data,
                                                       u32 width, u32 height)
{
    auto* dev = m_device->GetDevice();

    RmlTextureData texData;
    texData.srvIndex = m_srvHeap->AllocateIndex();

    // DEFAULT ヒープにテクスチャ作成
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc       = { 1, 0 };
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    Microsoft::WRL::ComPtr<ID3D12Resource> texResource;
    ThrowIfFailed(dev->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&texResource)));

    // Upload buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = totalBytes;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.SampleDesc       = { 1, 0 };
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(dev->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&uploadBuffer)));

    // Copy pixel data to upload buffer
    void* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, &mapped));
    auto* dstBase = static_cast<u8*>(mapped) + footprint.Offset;
    const u32 srcRowPitch = width * 4;
    for (UINT row = 0; row < numRows; ++row)
    {
        auto* dstRow = dstBase + static_cast<size_t>(row) * footprint.Footprint.RowPitch;
        auto* srcRow = data + static_cast<size_t>(row) * srcRowPitch;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(rowSizeInBytes));
    }
    uploadBuffer->Unmap(0, nullptr);

    // テクスチャアップロードは一時コマンドリストで行う
    // 注意: この関数は初期化時にも呼ばれるため、
    // 一時的なコマンドリストを作成して即時実行する
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> tempAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> tempCmdList;

    ThrowIfFailed(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tempAllocator)));
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        tempAllocator.Get(), nullptr, IID_PPV_ARGS(&tempCmdList)));

    // CopyTextureRegion
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = texResource.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource       = uploadBuffer.Get();
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    tempCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Barrier: COPY_DEST → PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = texResource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    tempCmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(tempCmdList->Close());

    // 即時実行 (CommandQueue を直接使う)
    // 注意: ここでは m_device から直接 Queue を取れないので、
    // Application 側で CommandQueue へのポインタを渡す必要がある
    // → Initialize に CommandQueue* を追加する
    // 一旦ここは tempCmdList の Close まで。Execute は別途。
    // ※ 実装時にこの部分は調整が必要

    // SRV 作成
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                        = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels           = 1;

    dev->CreateShaderResourceView(texResource.Get(), &srvDesc,
                                  m_srvHeap->GetCpuHandle(texData.srvIndex));

    // テクスチャリソースを保持
    texData.texture = std::make_unique<Texture>();
    // Texture クラスに直接リソースを設定する方法が無いので、
    // ComPtr<ID3D12Resource> を直接保持する
    // → RmlTextureData に resource フィールドを追加

    uintptr_t handle = m_nextTextureHandle++;
    texData.texture = nullptr; // Texture クラスは使わず ComPtr で直接管理
    m_textures[handle] = std::move(texData);

    // uploadBuffer は GPU 実行完了後に解放されるべきだが、
    // 一時コマンドリストの同期は Application 側で行う
    // → m_pendingUploads に追加する設計に変更が必要

    return static_cast<Rml::TextureHandle>(handle);
}

// ========== Scissor ==========

void RmlRenderer::EnableScissorRegion(bool enable)
{
    m_scissorEnabled = enable;
    if (!enable && m_cmdList)
    {
        D3D12_RECT fullRect = { 0, 0,
                                static_cast<LONG>(m_viewportWidth),
                                static_cast<LONG>(m_viewportHeight) };
        m_cmdList->RSSetScissorRects(1, &fullRect);
    }
}

void RmlRenderer::SetScissorRegion(Rml::Rectanglei region)
{
    if (!m_cmdList) return;

    D3D12_RECT rect;
    rect.left   = static_cast<LONG>(region.Left());
    rect.top    = static_cast<LONG>(region.Top());
    rect.right  = static_cast<LONG>(region.Right());
    rect.bottom = static_cast<LONG>(region.Bottom());
    m_cmdList->RSSetScissorRects(1, &rect);
}

} // namespace dx12e
```

**注意:** `CreateTextureFromRGBA` 内のテクスチャアップロードは、初期化時やフォントアトラス生成時に呼ばれる。一時コマンドリストの作成と即時実行が必要。Initialize に `ID3D12CommandQueue*` パラメータを追加し、GPU同期する仕組みが必要。

**実装時の調整ポイント:**
1. `Initialize()` に `ID3D12CommandQueue* queue` を追加
2. `RmlTextureData` に `ComPtr<ID3D12Resource> resource` を追加（Texture クラスの代わり）
3. `CreateTextureFromRGBA` 内で一時コマンドリストを即時実行 + Fence で同期
4. uploadBuffer を vector に溜めてフレーム終了時に解放する仕組み

これらの調整は実装時に行う。上記コードは骨格として使い、コンパイル時に修正する。

- [ ] **Step 3: ビルド確認**

Run: `cmake --build build/debug --config Debug --target Gui`
Expected: コンパイル成功（RmlUi ヘッダー解決 + DX12 パイプライン作成コード）

- [ ] **Step 4: コミット**

```bash
git add src/gui/RmlRenderer.h src/gui/RmlRenderer.cpp
git commit -m "feat: RmlRenderer - DX12 RenderInterface for RmlUi"
```

---

### Task 5: RmlUIManager (統括クラス) 実装

**Files:**
- Create: `src/gui/RmlUIManager.h`
- Create: `src/gui/RmlUIManager.cpp`

- [ ] **Step 1: RmlUIManager.h を作成**

```cpp
#pragma once

#include <RmlUi/Core.h>
#include <string>
#include <memory>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class GameClock;
class RmlRenderer;
class RmlSystem;

class RmlUIManager
{
public:
    RmlUIManager();
    ~RmlUIManager();

    RmlUIManager(const RmlUIManager&) = delete;
    RmlUIManager& operator=(const RmlUIManager&) = delete;

    void Initialize(GraphicsDevice& device,
                    ID3D12CommandQueue* commandQueue,
                    DescriptorHeap& srvHeap,
                    const GameClock& clock,
                    DXGI_FORMAT rtvFormat,
                    const std::wstring& shaderDir,
                    u32 viewportWidth, u32 viewportHeight);

    void Shutdown();

    // ドキュメント管理
    bool LoadDocument(const std::string& path);
    void CloseDocument(const std::string& id);
    void CloseAllDocuments();

    // DOM操作（Lua から呼ぶ）
    void SetText(const std::string& elementId, const std::string& text);
    void SetProperty(const std::string& elementId,
                     const std::string& property, const std::string& value);
    void ShowElement(const std::string& elementId);
    void HideElement(const std::string& elementId);

    // 入力イベント転送（Window::WndProc から呼ぶ）
    bool ProcessMouseMove(int x, int y);
    bool ProcessMouseButton(int buttonIndex, bool down);
    bool ProcessMouseWheel(float delta);
    bool ProcessKeyDown(int vkCode);
    bool ProcessKeyUp(int vkCode);
    bool ProcessTextInput(wchar_t character);

    // フレーム更新・描画
    void Update();
    void Render(ID3D12GraphicsCommandList* cmdList,
                ID3D12DescriptorHeap* srvHeap,
                u32 viewportWidth, u32 viewportHeight);

    void OnResize(u32 width, u32 height);

private:
    static int ConvertVirtualKey(int vkCode);
    static int GetKeyModifiers();

    std::unique_ptr<RmlSystem>   m_system;
    std::unique_ptr<RmlRenderer> m_renderer;
    Rml::Context*                m_context = nullptr;

    bool m_initialized = false;
};

} // namespace dx12e
```

- [ ] **Step 2: RmlUIManager.cpp を作成**

```cpp
#include "gui/RmlUIManager.h"
#include "gui/RmlSystem.h"
#include "gui/RmlRenderer.h"
#include "core/Logger.h"

#include <RmlUi/Core.h>
#include <Windows.h>

namespace dx12e
{

RmlUIManager::RmlUIManager() = default;

RmlUIManager::~RmlUIManager()
{
    if (m_initialized)
        Shutdown();
}

void RmlUIManager::Initialize(GraphicsDevice& device,
                               ID3D12CommandQueue* commandQueue,
                               DescriptorHeap& srvHeap,
                               const GameClock& clock,
                               DXGI_FORMAT rtvFormat,
                               const std::wstring& shaderDir,
                               u32 viewportWidth, u32 viewportHeight)
{
    // SystemInterface
    m_system = std::make_unique<RmlSystem>(clock);
    Rml::SetSystemInterface(m_system.get());

    // RenderInterface
    m_renderer = std::make_unique<RmlRenderer>();
    m_renderer->Initialize(device, commandQueue, srvHeap, rtvFormat, shaderDir);
    Rml::SetRenderInterface(m_renderer.get());

    // RmlUi 初期化
    Rml::Initialise();

    // フォント読み込み（日本語対応: Meiryo）
    Rml::LoadFontFace("C:/Windows/Fonts/meiryo.ttc", true);
    // フォールバック: Segoe UI
    Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf", true);

    // Context 作成
    m_context = Rml::CreateContext("game",
        Rml::Vector2i(static_cast<int>(viewportWidth),
                       static_cast<int>(viewportHeight)));

    if (!m_context)
    {
        Logger::Error("[RmlUi] Failed to create context");
        return;
    }

    m_initialized = true;
    Logger::Info("RmlUIManager initialized ({}x{})", viewportWidth, viewportHeight);
}

void RmlUIManager::Shutdown()
{
    if (m_context)
    {
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }
    Rml::Shutdown();
    m_renderer.reset();
    m_system.reset();
    m_initialized = false;

    Logger::Info("RmlUIManager shutdown");
}

// ========== Document Management ==========

bool RmlUIManager::LoadDocument(const std::string& path)
{
    if (!m_context) return false;

    auto* doc = m_context->LoadDocument(path);
    if (!doc)
    {
        Logger::Warn("[RmlUi] Failed to load document: {}", path);
        return false;
    }
    doc->Show();
    Logger::Info("[RmlUi] Document loaded: {}", path);
    return true;
}

void RmlUIManager::CloseDocument(const std::string& id)
{
    if (!m_context) return;
    auto* elem = m_context->GetDocument(id);
    if (elem) elem->Close();
}

void RmlUIManager::CloseAllDocuments()
{
    if (!m_context) return;
    // 全ドキュメントを閉じる
    while (m_context->GetNumDocuments() > 0)
    {
        auto* doc = m_context->GetDocument(0);
        if (doc) doc->Close();
        else break;
    }
}

// ========== DOM Manipulation ==========

void RmlUIManager::SetText(const std::string& elementId, const std::string& text)
{
    if (!m_context) return;
    // 全ドキュメントから要素を検索
    for (int i = 0; i < m_context->GetNumDocuments(); ++i)
    {
        auto* doc = m_context->GetDocument(i);
        if (!doc) continue;
        auto* elem = doc->GetElementById(elementId);
        if (elem)
        {
            elem->SetInnerRML(text);
            return;
        }
    }
}

void RmlUIManager::SetProperty(const std::string& elementId,
                                const std::string& property,
                                const std::string& value)
{
    if (!m_context) return;
    for (int i = 0; i < m_context->GetNumDocuments(); ++i)
    {
        auto* doc = m_context->GetDocument(i);
        if (!doc) continue;
        auto* elem = doc->GetElementById(elementId);
        if (elem)
        {
            elem->SetProperty(property, value);
            return;
        }
    }
}

void RmlUIManager::ShowElement(const std::string& elementId)
{
    SetProperty(elementId, "display", "block");
}

void RmlUIManager::HideElement(const std::string& elementId)
{
    SetProperty(elementId, "display", "none");
}

// ========== Input ==========

bool RmlUIManager::ProcessMouseMove(int x, int y)
{
    if (!m_context) return false;
    return m_context->ProcessMouseMove(x, y, GetKeyModifiers());
}

bool RmlUIManager::ProcessMouseButton(int buttonIndex, bool down)
{
    if (!m_context) return false;
    if (down)
        return m_context->ProcessMouseButtonDown(buttonIndex, GetKeyModifiers());
    else
        return m_context->ProcessMouseButtonUp(buttonIndex, GetKeyModifiers());
}

bool RmlUIManager::ProcessMouseWheel(float delta)
{
    if (!m_context) return false;
    return m_context->ProcessMouseWheel(Rml::Vector2f(0, delta), GetKeyModifiers());
}

bool RmlUIManager::ProcessKeyDown(int vkCode)
{
    if (!m_context) return false;
    auto key = static_cast<Rml::Input::KeyIdentifier>(ConvertVirtualKey(vkCode));
    return m_context->ProcessKeyDown(key, GetKeyModifiers());
}

bool RmlUIManager::ProcessKeyUp(int vkCode)
{
    if (!m_context) return false;
    auto key = static_cast<Rml::Input::KeyIdentifier>(ConvertVirtualKey(vkCode));
    return m_context->ProcessKeyUp(key, GetKeyModifiers());
}

bool RmlUIManager::ProcessTextInput(wchar_t character)
{
    if (!m_context) return false;
    return m_context->ProcessTextInput(static_cast<Rml::Character>(character));
}

// ========== Frame ==========

void RmlUIManager::Update()
{
    if (!m_context) return;
    m_context->Update();
}

void RmlUIManager::Render(ID3D12GraphicsCommandList* cmdList,
                           ID3D12DescriptorHeap* srvHeap,
                           u32 viewportWidth, u32 viewportHeight)
{
    if (!m_context || !m_renderer) return;
    m_renderer->BeginFrame(cmdList, srvHeap, viewportWidth, viewportHeight);
    m_context->Render();
}

void RmlUIManager::OnResize(u32 width, u32 height)
{
    if (!m_context) return;
    m_context->SetDimensions(Rml::Vector2i(static_cast<int>(width),
                                            static_cast<int>(height)));
}

// ========== Key Conversion ==========

int RmlUIManager::GetKeyModifiers()
{
    int modifiers = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= Rml::Input::KM_CTRL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) modifiers |= Rml::Input::KM_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) modifiers |= Rml::Input::KM_ALT;
    return modifiers;
}

int RmlUIManager::ConvertVirtualKey(int vkCode)
{
    // Win32 VK → RmlUi KeyIdentifier 変換
    // 主要キーのみマッピング（必要に応じて拡張）
    switch (vkCode)
    {
    case VK_BACK:     return Rml::Input::KI_BACK;
    case VK_TAB:      return Rml::Input::KI_TAB;
    case VK_RETURN:   return Rml::Input::KI_RETURN;
    case VK_ESCAPE:   return Rml::Input::KI_ESCAPE;
    case VK_SPACE:    return Rml::Input::KI_SPACE;
    case VK_LEFT:     return Rml::Input::KI_LEFT;
    case VK_UP:       return Rml::Input::KI_UP;
    case VK_RIGHT:    return Rml::Input::KI_RIGHT;
    case VK_DOWN:     return Rml::Input::KI_DOWN;
    case VK_DELETE:   return Rml::Input::KI_DELETE;
    case VK_HOME:     return Rml::Input::KI_HOME;
    case VK_END:      return Rml::Input::KI_END;
    default:
        // A-Z → KI_A - KI_Z
        if (vkCode >= 'A' && vkCode <= 'Z')
            return Rml::Input::KI_A + (vkCode - 'A');
        // 0-9
        if (vkCode >= '0' && vkCode <= '9')
            return Rml::Input::KI_0 + (vkCode - '0');
        // F1-F12
        if (vkCode >= VK_F1 && vkCode <= VK_F12)
            return Rml::Input::KI_F1 + (vkCode - VK_F1);
        return Rml::Input::KI_UNKNOWN;
    }
}

} // namespace dx12e
```

- [ ] **Step 3: コミット**

```bash
git add src/gui/RmlUIManager.h src/gui/RmlUIManager.cpp
git commit -m "feat: RmlUIManager - initialization, input bridge, document management"
```

---

### Task 6: Application 統合

**Files:**
- Modify: `src/core/Application.h`
- Modify: `src/core/Application.cpp`

- [ ] **Step 1: Application.h に RmlUIManager メンバー追加**

`#include` セクションに前方宣言追加:
```cpp
class RmlUIManager;
```

メンバー変数に追加（`m_imguiManager` の近くに）:
```cpp
std::unique_ptr<RmlUIManager>  m_rmlManager;
```

- [ ] **Step 2: Application.cpp に初期化コード追加**

ヘッダー追加:
```cpp
#include "gui/RmlUIManager.h"
```

`Initialize()` 内、ImGui初期化の後（`m_imguiManager->Initialize(...)` の後）に追加:
```cpp
// RmlUi 初期化
m_rmlManager = std::make_unique<RmlUIManager>();
m_rmlManager->Initialize(
    *m_graphicsDevice,
    m_commandQueue->GetQueue(),
    *m_srvHeap,
    m_gameClock,
    m_swapChain->GetFormat(),
    std::wstring(SHADER_DIR),
    m_window->GetWidth(), m_window->GetHeight());
```

- [ ] **Step 3: Render() に RmlUi 描画追加**

`Application::Render()` 内、Physics debug draw の後、ImGui BeginFrame の前に追加:
```cpp
// RmlUi game HUD (game mode only)
if (m_engineMode == EngineMode::Playing)
{
    m_rmlManager->Update();
    m_rmlManager->Render(nativeCmdList, m_srvHeap->GetHeap(),
                         m_window->GetWidth(), m_window->GetHeight());
}
```

- [ ] **Step 4: Update() で RmlUi リサイズ対応**

リサイズ処理ブロック内に追加:
```cpp
if (m_rmlManager)
    m_rmlManager->OnResize(m_window->GetWidth(), m_window->GetHeight());
```

- [ ] **Step 5: Shutdown() に RmlUi クリーンアップ追加**

ImGui shutdown の前に追加:
```cpp
if (m_rmlManager)
{
    m_rmlManager->Shutdown();
    m_rmlManager.reset();
}
```

- [ ] **Step 6: コミット**

```bash
git add src/core/Application.h src/core/Application.cpp
git commit -m "feat: integrate RmlUIManager into Application lifecycle"
```

---

### Task 7: Window 入力転送

**Files:**
- Modify: `src/core/Window.h`
- Modify: `src/core/Window.cpp`

- [ ] **Step 1: Window.h に RmlUIManager ポインタ追加**

前方宣言追加:
```cpp
class RmlUIManager;
```

メンバーに追加:
```cpp
RmlUIManager* m_rmlManager = nullptr;
```

public に追加:
```cpp
void SetRmlManager(RmlUIManager* mgr) { m_rmlManager = mgr; }
```

- [ ] **Step 2: Window.cpp の WndProc に RmlUi 入力転送追加**

既存の ImGui WndProcHandler 呼び出し（line 162付近）の後に追加:

```cpp
// RmlUi input forwarding
if (window->m_rmlManager)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        window->m_rmlManager->ProcessMouseMove(
            static_cast<int>(LOWORD(lParam)),
            static_cast<int>(HIWORD(lParam)));
        break;
    case WM_LBUTTONDOWN:
        window->m_rmlManager->ProcessMouseButton(0, true);
        break;
    case WM_LBUTTONUP:
        window->m_rmlManager->ProcessMouseButton(0, false);
        break;
    case WM_RBUTTONDOWN:
        window->m_rmlManager->ProcessMouseButton(1, true);
        break;
    case WM_RBUTTONUP:
        window->m_rmlManager->ProcessMouseButton(1, false);
        break;
    case WM_MOUSEWHEEL:
        window->m_rmlManager->ProcessMouseWheel(
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
        break;
    case WM_CHAR:
        window->m_rmlManager->ProcessTextInput(static_cast<wchar_t>(wParam));
        break;
    }
}
```

- [ ] **Step 3: Application.cpp で Window に RmlManager を設定**

Initialize() 内、RmlUIManager 初期化後に追加:
```cpp
m_window->SetRmlManager(m_rmlManager.get());
```

Shutdown() でクリア:
```cpp
m_window->SetRmlManager(nullptr);
```

- [ ] **Step 4: コミット**

```bash
git add src/core/Window.h src/core/Window.cpp src/core/Application.cpp
git commit -m "feat: forward window input events to RmlUIManager"
```

---

### Task 8: サンプル HUD 作成

**Files:**
- Create: `assets/ui/hud.rml`
- Create: `assets/ui/hud.rcss`

- [ ] **Step 1: hud.rcss を作成**

```css
body {
    font-family: "Meiryo", "Segoe UI", sans-serif;
    font-size: 16px;
    color: white;
}

#hud-container {
    position: fixed;
    left: 0;
    top: 0;
    right: 0;
    bottom: 0;
    pointer-events: none;
}

/* === HP バー === */
#health-container {
    position: fixed;
    bottom: 40px;
    left: 30px;
    width: 250px;
}

#health-label {
    font-size: 14px;
    color: #ccc;
    margin-bottom: 4px;
}

#health-bar-bg {
    width: 100%;
    height: 22px;
    background-color: #333a;
    border: 1px #555;
}

#health-fill {
    height: 100%;
    width: 100%;
    background-color: #e44;
}

/* === クロスヘア === */
#crosshair {
    position: fixed;
    top: 50%;
    left: 50%;
    margin-top: -12px;
    margin-left: -12px;
    font-size: 24px;
    color: #fffa;
    text-align: center;
    width: 24px;
    height: 24px;
}

/* === 弾数 === */
#ammo-container {
    position: fixed;
    bottom: 40px;
    right: 30px;
    text-align: right;
}

#ammo-count {
    font-size: 36px;
    font-weight: bold;
}

#ammo-label {
    font-size: 14px;
    color: #aaa;
}

/* === FPS表示 === */
#fps-display {
    position: fixed;
    top: 10px;
    right: 10px;
    font-size: 12px;
    color: #8f8;
}
```

- [ ] **Step 2: hud.rml を作成**

```xml
<rml>
<head>
    <title>Game HUD</title>
    <link type="text/rcss" href="hud.rcss"/>
</head>
<body id="hud-container">

    <!-- HP Bar -->
    <div id="health-container">
        <div id="health-label">HP</div>
        <div id="health-bar-bg">
            <div id="health-fill"/>
        </div>
    </div>

    <!-- Crosshair -->
    <div id="crosshair">+</div>

    <!-- Ammo -->
    <div id="ammo-container">
        <div id="ammo-count">30 / 90</div>
        <div id="ammo-label">AMMO</div>
    </div>

    <!-- FPS -->
    <div id="fps-display">FPS: 0</div>

</body>
</rml>
```

- [ ] **Step 3: Application 初期化で HUD ドキュメントを読み込み**

`Application::Initialize()` 内、RmlUIManager 初期化後に追加:
```cpp
// デフォルト HUD 読み込み
std::string assetsDir = ASSETS_DIR;
m_rmlManager->LoadDocument(assetsDir + "ui/hud.rml");
```

- [ ] **Step 4: コミット**

```bash
git add assets/ui/hud.rml assets/ui/hud.rcss src/core/Application.cpp
git commit -m "feat: sample game HUD (health bar, crosshair, ammo, FPS)"
```

---

### Task 9: Lua バインド追加

**Files:**
- Modify: `src/scripting/ScriptEngine.h`
- Modify: `src/scripting/ScriptEngine.cpp`

- [ ] **Step 1: ScriptEngine.h に RmlUIManager 追加**

メンバーに追加:
```cpp
RmlUIManager*  m_rmlManager = nullptr;
```

`Initialize()` のシグネチャを変更:
```cpp
void Initialize(Scene* scene, InputSystem* input, Camera* camera,
                AudioSystem* audio, PhysicsSystem* physics,
                RmlUIManager* rmlManager,
                const std::string& assetsDir);
```

前方宣言追加:
```cpp
class RmlUIManager;
```

- [ ] **Step 2: ScriptEngine.cpp に ui バインド追加**

Initialize() でメンバー設定:
```cpp
m_rmlManager = rmlManager;
```

RegisterBindings() 内、`RegisterPhysicsBindings()` の前に追加:

```cpp
// --- UI System (RmlUi) ---
lua.new_usertype<RmlUIManager>("RmlUIManager",
    "loadDocument",  &RmlUIManager::LoadDocument,
    "closeDocument", &RmlUIManager::CloseDocument,
    "setText",       &RmlUIManager::SetText,
    "setProperty",   &RmlUIManager::SetProperty,
    "show",          &RmlUIManager::ShowElement,
    "hide",          &RmlUIManager::HideElement
);

lua["ui"] = m_rmlManager;
```

- [ ] **Step 3: Application.cpp で ScriptEngine 初期化を更新**

`ScriptEngine::Initialize()` 呼び出しに `m_rmlManager.get()` を追加:
```cpp
m_scriptEngine->Initialize(
    m_scene.get(), m_inputSystem.get(), &m_camera,
    m_audioSystem.get(), m_physicsSystem.get(),
    m_rmlManager.get(),
    assetsDir);
```

- [ ] **Step 4: コミット**

```bash
git add src/scripting/ScriptEngine.h src/scripting/ScriptEngine.cpp src/core/Application.cpp
git commit -m "feat: Lua bindings for RmlUi (ui:setText, setProperty, show, hide)"
```

---

### Task 10: ビルド + 動作確認

- [ ] **Step 1: CMake reconfigure**

Run:
```bash
cmake -B build/debug -G "Visual Studio 18 2026" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows
```
Expected: RmlUi found, configure 成功

- [ ] **Step 2: フルビルド**

Run: `cmake --build build/debug --config Debug`
Expected: コンパイル + リンク成功

- [ ] **Step 3: 実行テスト**

Run: `build/debug/Debug/DX12Engine.exe`
Expected:
- ゲームモードに切り替え → HUD表示（HPバー、クロスヘア、弾数）
- エディタモード → HUD非表示、ImGui表示
- クラッシュなし

- [ ] **Step 4: Lua からの UI 操作テスト**

`scripts/game.lua` の `OnUpdate(dt)` に追加してテスト:
```lua
ui:setText("fps-display", string.format("FPS: %d", math.floor(1.0 / dt)))
```

Expected: 右上にFPSがリアルタイム更新表示される

- [ ] **Step 5: 最終コミット**

```bash
git add -A
git commit -m "feat: RmlUi game HUD integration complete"
```

---

## 実装上の注意事項

1. **テクスチャアップロード同期**: `CreateTextureFromRGBA` は一時コマンドリストで GPU にアップロードする。Fence で同期が必要。既存の `Texture::Initialize()` パターンを参考にするが、一時実行用の CommandAllocator + Fence をクラス内に保持する。

2. **RmlUi の `Rml::Vertex` レイアウト**: `sizeof(Rml::Vertex)` = 20 bytes (float2 + byte4 + float2)。InputLayout の offset が合っていることを確認。

3. **Premultiplied Alpha**: RmlUi の色は premultiplied alpha。PSO のブレンドは `SrcBlend = ONE, DestBlend = INV_SRC_ALPHA`。

4. **vcpkg の rmlui パッケージ名**: `find_package(RmlUi CONFIG REQUIRED)` でリンクターゲットは `RmlUi::RmlUi`。vcpkg のバージョンによって `rmlui` vs `RmlUi` が異なる可能性があるので、ビルド時にエラーが出たら `vcpkg search rmlui` で確認。

5. **`/W4 /WX` と RmlUi ヘッダー**: RmlUi のヘッダーで警告が出る可能性あり。`/external:anglebrackets /external:W0` が既に設定されているので、`#include <RmlUi/...>` で山括弧を使えば抑制される。

6. **DescriptorHeap の SRV 枯渇**: フォントアトラスやUIテクスチャで SRV を消費する。1024 スロット中、現状の使用量は少ないので問題ないが、大量のテクスチャを使う場合は確認。
