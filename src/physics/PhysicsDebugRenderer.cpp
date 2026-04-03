#include "physics/PhysicsDebugRenderer.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "ecs/Components.h"
#include "core/Logger.h"
#include "core/Assert.h"

#include <entt/entt.hpp>

using namespace DirectX;

namespace dx12e
{

// ========== Initialize ==========

void PhysicsDebugRenderer::Initialize(GraphicsDevice& device,
                                      DXGI_FORMAT rtvFormat,
                                      DXGI_FORMAT dsvFormat,
                                      const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // --- Root Signature (RootConstants b0: 16 DWORD = float4x4 viewProj) ---
    {
        D3D12_ROOT_PARAMETER rootParam{};
        rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParam.Constants.ShaderRegister  = 0;
        rootParam.Constants.RegisterSpace   = 0;
        rootParam.Constants.Num32BitValues  = 16; // float4x4
        rootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 1;
        desc.pParameters       = &rootParam;
        desc.NumStaticSamplers = 0;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0,
            serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)));
    }

    // --- PSO (LineList, DepthTest OFF for always-visible) ---
    {
        auto vsData = ShaderCompiler::LoadFromFile(shaderDir + L"DebugLine_VS.cso");
        auto psData = ShaderCompiler::LoadFromFile(shaderDir + L"DebugLine_PS.cso");

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = { vsData.GetData(), vsData.GetSize() };
        psoDesc.PS = { psData.GetData(), psData.GetSize() };
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;

        // Rasterizer state (manual, no CD3DX12 helper)
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthBias = 0;
        psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.RasterizerState.MultisampleEnable = FALSE;
        psoDesc.RasterizerState.AntialiasedLineEnable = TRUE;

        // Blend state (default opaque)
        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable    = FALSE; // Always visible
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.StencilEnable  = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat;
        psoDesc.DSVFormat = dsvFormat;
        psoDesc.SampleDesc = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));
    }

    // --- Dynamic Vertex Buffer (Upload Heap) ---
    {
        const UINT bufferSize = kMaxVertices * sizeof(DebugLineVertex);

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width              = bufferSize;
        resDesc.Height             = 1;
        resDesc.DepthOrArraySize   = 1;
        resDesc.MipLevels          = 1;
        resDesc.SampleDesc         = { 1, 0 };
        resDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_vertexBuffer)));

        m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vbView.StrideInBytes  = sizeof(DebugLineVertex);
        m_vbView.SizeInBytes    = bufferSize;
    }

    m_vertices.reserve(4096);
    m_initialized = true;
    Logger::Info("PhysicsDebugRenderer initialized");
}

// ========== Frame management ==========

void PhysicsDebugRenderer::BeginFrame()
{
    m_vertices.clear();
}

// ========== Primitive builders ==========

void PhysicsDebugRenderer::AddLine(XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 color)
{
    if (m_vertices.size() + 2 > kMaxVertices) return;
    m_vertices.push_back({ a, color });
    m_vertices.push_back({ b, color });
}

void PhysicsDebugRenderer::AddBox(XMFLOAT3 center, XMFLOAT3 he,
                                  XMFLOAT4 quat, XMFLOAT3 color)
{
    // 8 corners of an OBB
    XMVECTOR c = XMLoadFloat3(&center);
    XMVECTOR q = XMLoadFloat4(&quat);
    XMMATRIX rot = XMMatrixRotationQuaternion(q);

    XMFLOAT3 corners[8];
    const float signs[8][3] = {
        {-1,-1,-1}, {+1,-1,-1}, {+1,+1,-1}, {-1,+1,-1},
        {-1,-1,+1}, {+1,-1,+1}, {+1,+1,+1}, {-1,+1,+1},
    };

    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR local = XMVectorSet(
            signs[i][0] * he.x, signs[i][1] * he.y, signs[i][2] * he.z, 0.0f);
        XMVECTOR world = XMVector3TransformNormal(local, rot) + c;
        XMStoreFloat3(&corners[i], world);
    }

    // 12 edges
    const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},  // bottom
        {4,5},{5,6},{6,7},{7,4},  // top
        {0,4},{1,5},{2,6},{3,7},  // sides
    };

    for (auto& e : edges)
        AddLine(corners[e[0]], corners[e[1]], color);
}

void PhysicsDebugRenderer::AddSphere(XMFLOAT3 center, f32 radius, u32 segments, XMFLOAT3 color)
{
    const float step = XM_2PI / static_cast<float>(segments);

    // 3 circles (XY, XZ, YZ)
    for (u32 i = 0; i < segments; ++i)
    {
        float a0 = step * static_cast<float>(i);
        float a1 = step * static_cast<float>(i + 1);
        float c0 = cosf(a0), s0 = sinf(a0);
        float c1 = cosf(a1), s1 = sinf(a1);

        // XZ circle (horizontal)
        AddLine({ center.x + c0 * radius, center.y, center.z + s0 * radius },
                { center.x + c1 * radius, center.y, center.z + s1 * radius }, color);

        // XY circle
        AddLine({ center.x + c0 * radius, center.y + s0 * radius, center.z },
                { center.x + c1 * radius, center.y + s1 * radius, center.z }, color);

        // YZ circle
        AddLine({ center.x, center.y + c0 * radius, center.z + s0 * radius },
                { center.x, center.y + c1 * radius, center.z + s1 * radius }, color);
    }
}

void PhysicsDebugRenderer::AddCapsule(XMFLOAT3 center, f32 radius, f32 halfHeight,
                                      XMFLOAT4 quat, XMFLOAT3 color)
{
    XMVECTOR c = XMLoadFloat3(&center);
    XMVECTOR q = XMLoadFloat4(&quat);
    XMMATRIX rot = XMMatrixRotationQuaternion(q);

    // Top and bottom centers
    XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0, halfHeight, 0, 0), rot);
    XMFLOAT3 topCenter, botCenter;
    XMStoreFloat3(&topCenter, c + up);
    XMStoreFloat3(&botCenter, c - up);

    // Draw sphere halves at top and bottom + connecting lines
    AddSphere(topCenter, radius, 12, color);
    AddSphere(botCenter, radius, 12, color);

    // 4 vertical lines connecting the hemispheres
    XMVECTOR right   = XMVector3TransformNormal(XMVectorSet(radius, 0, 0, 0), rot);
    XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, radius, 0), rot);

    XMFLOAT3 tr, br, tf, bf, tl, bl, tb, bb;
    XMStoreFloat3(&tr, c + up + right);
    XMStoreFloat3(&br, c - up + right);
    XMStoreFloat3(&tf, c + up + forward);
    XMStoreFloat3(&bf, c - up + forward);
    XMStoreFloat3(&tl, c + up - right);
    XMStoreFloat3(&bl, c - up - right);
    XMStoreFloat3(&tb, c + up - forward);
    XMStoreFloat3(&bb, c - up - forward);

    AddLine(tr, br, color);
    AddLine(tf, bf, color);
    AddLine(tl, bl, color);
    AddLine(tb, bb, color);
}

// ========== Collect from ECS ==========

void PhysicsDebugRenderer::CollectFromRegistry(entt::registry& registry)
{
    const XMFLOAT3 dynamicColor   = { 0.0f, 1.0f, 0.0f };  // green
    const XMFLOAT3 staticColor    = { 0.5f, 0.5f, 1.0f };  // blue
    const XMFLOAT3 kinematicColor = { 1.0f, 1.0f, 0.0f };  // yellow

    auto view = registry.view<Transform, RigidBody>();
    for (auto [entity, transform, rb] : view.each())
    {
        if (rb.bodyId == kInvalidBodyId) continue;

        XMFLOAT3 color = dynamicColor;
        if (rb.motionType == MotionType::Static)    color = staticColor;
        if (rb.motionType == MotionType::Kinematic) color = kinematicColor;

        XMFLOAT4 quat = transform.useQuaternion
            ? transform.quaternion
            : XMFLOAT4(0, 0, 0, 1);

        // Non-quaternion: compute quat from euler
        if (!transform.useQuaternion)
        {
            XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(transform.rotation.x),
                XMConvertToRadians(transform.rotation.y),
                XMConvertToRadians(transform.rotation.z));
            XMStoreFloat4(&quat, q);
        }

        auto* convex  = registry.try_get<ConvexHullCollider>(entity);
        auto* box     = registry.try_get<BoxCollider>(entity);
        auto* sphere  = registry.try_get<SphereCollider>(entity);
        auto* capsule = registry.try_get<CapsuleCollider>(entity);

        if (convex && !convex->points.empty())
        {
            // Convex Hull: 頂点同士を結ぶラインで描画
            XMVECTOR c = XMLoadFloat3(&transform.position);
            XMVECTOR off = XMVectorSet(convex->offset.x, convex->offset.y, convex->offset.z, 0);
            XMVECTOR center = c + off;
            XMMATRIX rot = XMMatrixRotationQuaternion(XMLoadFloat4(&quat));

            // 凸包の頂点をワールド座標に変換して隣接点と結ぶ
            std::vector<XMFLOAT3> worldPts;
            worldPts.reserve(convex->points.size());
            for (const auto& p : convex->points)
            {
                XMVECTOR local = XMVectorSet(p.x, p.y, p.z, 0);
                XMVECTOR world = XMVector3TransformNormal(local, rot) + center;
                XMFLOAT3 wp;
                XMStoreFloat3(&wp, world);
                worldPts.push_back(wp);
            }

            // 各頂点から最近傍数点にラインを引く（凸包の輪郭を近似描画）
            size_t count = worldPts.size();
            size_t step = (std::max)(count / 32, (size_t)1);
            for (size_t i = 0; i < count; i += step)
            {
                // 次の頂点と結ぶ
                size_t j = (i + step) % count;
                AddLine(worldPts[i], worldPts[j], color);
            }
        }
        else if (box)
        {
            AddBox(transform.position, box->halfExtents, quat, color);
        }
        else if (sphere)
        {
            AddSphere(transform.position, sphere->radius, 16, color);
        }
        else if (capsule)
        {
            AddCapsule(transform.position, capsule->radius, capsule->halfHeight, quat, color);
        }
        else
        {
            // Fallback: box from scale
            XMFLOAT3 he = { transform.scale.x * 0.5f,
                            transform.scale.y * 0.5f,
                            transform.scale.z * 0.5f };
            AddBox(transform.position, he, quat, color);
        }
    }
}

// ========== Render ==========

void PhysicsDebugRenderer::Render(ID3D12GraphicsCommandList* cmdList,
                                  const XMFLOAT4X4& viewProj)
{
    if (!m_initialized || m_vertices.empty()) return;

    u32 vertexCount = static_cast<u32>(m_vertices.size());
    if (vertexCount > kMaxVertices)
        vertexCount = kMaxVertices;

    // Upload vertices
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, &mapped));
    memcpy(mapped, m_vertices.data(), vertexCount * sizeof(DebugLineVertex));
    m_vertexBuffer->Unmap(0, nullptr);

    // Set pipeline
    cmdList->SetPipelineState(m_pso.Get());
    cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Set ViewProj as root constants (16 floats)
    cmdList->SetGraphicsRoot32BitConstants(0, 16, &viewProj, 0);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmdList->IASetVertexBuffers(0, 1, &m_vbView);
    cmdList->DrawInstanced(vertexCount, 1, 0, 0);
}

} // namespace dx12e
