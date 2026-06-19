#include "editor/EditorIconRenderer.h"
#include "editor/EditorContext.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "ecs/Components.h"
#include "core/Logger.h"
#include "core/Assert.h"

using namespace DirectX;

namespace dx12e
{

// ========== Initialize ==========

void EditorIconRenderer::Initialize(GraphicsDevice& device,
                                    DXGI_FORMAT rtvFormat,
                                    DXGI_FORMAT dsvFormat,
                                    const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // --- Root Signature (same as PhysicsDebugRenderer) ---
    {
        D3D12_ROOT_PARAMETER rootParam{};
        rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParam.Constants.ShaderRegister  = 0;
        rootParam.Constants.RegisterSpace   = 0;
        rootParam.Constants.Num32BitValues  = 16;
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

    // --- PSO (LineList, DepthTest OFF) ---
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

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthBias = 0;
        psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.RasterizerState.MultisampleEnable = FALSE;
        psoDesc.RasterizerState.AntialiasedLineEnable = TRUE;

        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable    = FALSE;
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
        const UINT bufferSize = kMaxVertices * sizeof(IconLineVertex);

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
        m_vbView.StrideInBytes  = sizeof(IconLineVertex);
        m_vbView.SizeInBytes    = bufferSize;
    }

    // --- Billboard Root Signature (b0: viewProj 16 + invScreen 2 + pad 2 = 20 DWORD) ---
    {
        D3D12_ROOT_PARAMETER rootParam{};
        rootParam.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParam.Constants.ShaderRegister = 0;
        rootParam.Constants.Num32BitValues = 20;
        rootParam.ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 1;
        desc.pParameters   = &rootParam;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_billboardRootSig)));
    }

    // --- Billboard PSO (LineList, Depth OFF) ---
    {
        auto vsData = ShaderCompiler::LoadFromFile(shaderDir + L"IconBillboard_VS.cso");
        auto psData = ShaderCompiler::LoadFromFile(shaderDir + L"IconBillboard_PS.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_billboardRootSig.Get();
        psoDesc.VS = { vsData.GetData(), vsData.GetSize() };
        psoDesc.PS = { psData.GetData(), psData.GetSize() };
        psoDesc.InputLayout = { layout, _countof(layout) };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        psoDesc.RasterizerState.AntialiasedLineEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.DepthStencilState.DepthEnable    = FALSE;
        psoDesc.DepthStencilState.StencilEnable  = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_billboardPSO)));
    }

    // --- Billboard Dynamic Vertex Buffer ---
    {
        const UINT bufferSize = kMaxVertices * sizeof(IconBillboardVertex);
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width            = bufferSize;
        resDesc.Height           = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels        = 1;
        resDesc.SampleDesc       = { 1, 0 };
        resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_billboardVB)));
        m_billboardVBView.BufferLocation = m_billboardVB->GetGPUVirtualAddress();
        m_billboardVBView.StrideInBytes  = sizeof(IconBillboardVertex);
        m_billboardVBView.SizeInBytes    = bufferSize;
    }

    m_vertices.reserve(2048);
    m_billboardVerts.reserve(2048);
    m_initialized = true;
    Logger::Info("EditorIconRenderer initialized");
}

// ========== Frame ==========

void EditorIconRenderer::BeginFrame()
{
    m_vertices.clear();
    m_billboardVerts.clear();
}

// ========== Primitives ==========

void EditorIconRenderer::AddLine(XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 color)
{
    if (m_vertices.size() + 2 > kMaxVertices) return;
    m_vertices.push_back({ a, color });
    m_vertices.push_back({ b, color });
}

void EditorIconRenderer::AddBillboardLine(const XMFLOAT3& center,
                                          XMFLOAT2 a, XMFLOAT2 b,
                                          const XMFLOAT3& color)
{
    if (m_billboardVerts.size() + 2 > kMaxVertices) return;
    m_billboardVerts.push_back({ center, a, color });
    m_billboardVerts.push_back({ center, b, color });
}

// ========== 常時表示アイコン ==========

void EditorIconRenderer::AddCameraIcon(const XMFLOAT3& pos,
                                       const XMFLOAT4& quat,
                                       const XMFLOAT3& color)
{
    // カメラの向き（quat）に追従する 3D カメラアイコン。
    // ローカル軸: x=Right, y=Up, z=Forward。レンズは +Forward に伸びる。
    // → カメラが斜め下を向けばアイコンも斜め下を向く。
    XMMATRIX rot = XMMatrixRotationQuaternion(XMLoadFloat4(&quat));
    XMVECTOR R = rot.r[0];
    XMVECTOR U = rot.r[1];
    XMVECTOR F = rot.r[2];
    XMVECTOR P = XMLoadFloat3(&pos);

    // ローカル座標(r,u,f)をワールド点へ
    auto W = [&](f32 r, f32 u, f32 f) {
        XMFLOAT3 o;
        XMStoreFloat3(&o, P + R * r + U * u + F * f);
        return o;
    };
    auto box = [&](const XMFLOAT3 v[8], const int e[12][2], int n) {
        for (int i = 0; i < n; ++i) AddLine(v[e[i][0]], v[e[i][1]], color);
    };

    // 本体ボックス（背面 f=-0.30 〜 前面 f=0）
    const f32 bw = 0.18f, bh = 0.13f, bBack = -0.30f, bFront = 0.0f;
    const XMFLOAT3 body[8] = {
        W(-bw, -bh, bBack), W(bw, -bh, bBack), W(bw, bh, bBack), W(-bw, bh, bBack),     // 背面 0-3
        W(-bw, -bh, bFront), W(bw, -bh, bFront), W(bw, bh, bFront), W(-bw, bh, bFront), // 前面 4-7
    };
    const int bodyEdges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7},
    };
    box(body, bodyEdges, 12);

    // レンズ（前面から +Forward へ広がる台形錐）
    const f32 ln0 = 0.08f, ln1 = 0.12f, lf1 = 0.26f;
    const XMFLOAT3 lens[8] = {
        W(-ln0, -ln0, bFront), W(ln0, -ln0, bFront), W(ln0, ln0, bFront), W(-ln0, ln0, bFront), // 根元 0-3
        W(-ln1, -ln1, lf1),    W(ln1, -ln1, lf1),    W(ln1, ln1, lf1),    W(-ln1, ln1, lf1),    // 先端 4-7
    };
    const int lensEdges[8][2] = {
        {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7}, // 先端の四角 + 側面4本（根元は本体前面と重複なので省略）
    };
    box(lens, lensEdges, 8);
}

void EditorIconRenderer::AddDirLightIcon(const XMFLOAT3& pos,
                                         const XMFLOAT3& color)
{
    // 太陽マーク（ビルボード）: 中心円 + 放射線8本。どの角度からも常に見える。
    const f32 r = 8.0f;        // 中心円(px)
    const f32 rayIn = 12.0f;
    const f32 rayOut = 18.0f;

    const u32 seg = 16;
    const float step = XM_2PI / static_cast<float>(seg);
    for (u32 i = 0; i < seg; ++i)
    {
        float a0 = step * static_cast<float>(i);
        float a1 = step * static_cast<float>(i + 1);
        AddBillboardLine(pos, { cosf(a0) * r, sinf(a0) * r },
                              { cosf(a1) * r, sinf(a1) * r }, color);
    }
    for (int i = 0; i < 8; ++i)
    {
        float a = XM_2PI * static_cast<float>(i) / 8.0f;
        float cx = cosf(a), sy = sinf(a);
        AddBillboardLine(pos, { cx * rayIn, sy * rayIn },
                              { cx * rayOut, sy * rayOut }, color);
    }
}

void EditorIconRenderer::AddPointLightIcon(const XMFLOAT3& pos,
                                           const XMFLOAT3& color)
{
    // 電球マーク（ビルボード）: 円 + 下のソケット線。
    const f32 r = 8.0f;
    const u32 seg = 14;
    const float step = XM_2PI / static_cast<float>(seg);
    for (u32 i = 0; i < seg; ++i)
    {
        float a0 = step * static_cast<float>(i);
        float a1 = step * static_cast<float>(i + 1);
        AddBillboardLine(pos, { cosf(a0) * r, sinf(a0) * r - 3.0f },
                              { cosf(a1) * r, sinf(a1) * r - 3.0f }, color);
    }
    // ソケット
    AddBillboardLine(pos, {-4.0f, -11.0f}, { 4.0f, -11.0f}, color);
    AddBillboardLine(pos, {-3.0f, -14.0f}, { 3.0f, -14.0f}, color);
}

// ========== 選択時のみ表示 ==========

void EditorIconRenderer::AddCameraFrustum(const XMFLOAT3& pos,
                                          const XMFLOAT4& quat,
                                          f32 fovDegrees, f32 /*nearClip*/, f32 farClip,
                                          const XMFLOAT3& color)
{
    // エディタ表示用の near/far（実際の値だと大きすぎるのでクランプ）
    const f32 nearDraw = 0.3f;
    const f32 farDraw  = (std::min)(farClip, 6.0f);
    const f32 aspect   = 16.0f / 9.0f;

    f32 fovRad = XMConvertToRadians(fovDegrees);
    f32 halfH_n = tanf(fovRad * 0.5f) * nearDraw;
    f32 halfW_n = halfH_n * aspect;
    f32 halfH_f = tanf(fovRad * 0.5f) * farDraw;
    f32 halfW_f = halfH_f * aspect;

    // カメラの座標系を quaternion から構築
    XMMATRIX rot = XMMatrixRotationQuaternion(XMLoadFloat4(&quat));
    XMVECTOR R = rot.r[0];  // right
    XMVECTOR U = rot.r[1];  // up
    XMVECTOR F = rot.r[2];  // forward
    XMVECTOR P = XMLoadFloat3(&pos);

    // 近面 4頂点
    XMFLOAT3 nTL, nTR, nBL, nBR;
    XMStoreFloat3(&nTL, P + F * nearDraw + U * halfH_n - R * halfW_n);
    XMStoreFloat3(&nTR, P + F * nearDraw + U * halfH_n + R * halfW_n);
    XMStoreFloat3(&nBL, P + F * nearDraw - U * halfH_n - R * halfW_n);
    XMStoreFloat3(&nBR, P + F * nearDraw - U * halfH_n + R * halfW_n);

    // 遠面 4頂点
    XMFLOAT3 fTL, fTR, fBL, fBR;
    XMStoreFloat3(&fTL, P + F * farDraw + U * halfH_f - R * halfW_f);
    XMStoreFloat3(&fTR, P + F * farDraw + U * halfH_f + R * halfW_f);
    XMStoreFloat3(&fBL, P + F * farDraw - U * halfH_f - R * halfW_f);
    XMStoreFloat3(&fBR, P + F * farDraw - U * halfH_f + R * halfW_f);

    // 近面 4辺
    AddLine(nTL, nTR, color);
    AddLine(nTR, nBR, color);
    AddLine(nBR, nBL, color);
    AddLine(nBL, nTL, color);

    // 遠面 4辺
    AddLine(fTL, fTR, color);
    AddLine(fTR, fBR, color);
    AddLine(fBR, fBL, color);
    AddLine(fBL, fTL, color);

    // 接続 4本
    AddLine(nTL, fTL, color);
    AddLine(nTR, fTR, color);
    AddLine(nBR, fBR, color);
    AddLine(nBL, fBL, color);
}

void EditorIconRenderer::AddDirectionalArrow(const XMFLOAT3& origin,
                                             const XMFLOAT3& direction,
                                             f32 length,
                                             const XMFLOAT3& color)
{
    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    XMVECTOR O = XMLoadFloat3(&origin);
    XMVECTOR tip = O + dir * length;

    // シャフト
    XMFLOAT3 tipF;
    XMStoreFloat3(&tipF, tip);
    AddLine(origin, tipF, color);

    // 矢頭の垂直ベクトル計算（Gram-Schmidt安定化）
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    if (fabsf(XMVectorGetX(XMVector3Dot(dir, worldUp))) > 0.99f)
        worldUp = XMVectorSet(1, 0, 0, 0);

    XMVECTOR perp1 = XMVector3Normalize(XMVector3Cross(dir, worldUp));
    XMVECTOR perp2 = XMVector3Cross(dir, perp1);

    const f32 headLen = 0.4f;
    const f32 headR   = 0.15f;
    XMVECTOR base = tip - dir * headLen;

    // 矢頭 4本
    XMFLOAT3 h1, h2, h3, h4;
    XMStoreFloat3(&h1, base + perp1 * headR);
    XMStoreFloat3(&h2, base - perp1 * headR);
    XMStoreFloat3(&h3, base + perp2 * headR);
    XMStoreFloat3(&h4, base - perp2 * headR);
    AddLine(tipF, h1, color);
    AddLine(tipF, h2, color);
    AddLine(tipF, h3, color);
    AddLine(tipF, h4, color);

    // 3本の追加矢印（太陽光の放射線イメージ）
    for (int i = 0; i < 3; ++i)
    {
        f32 offset = static_cast<f32>(i - 1) * 0.6f;
        XMVECTOR rayOrigin = O + perp1 * offset;
        XMFLOAT3 ro, rt;
        XMStoreFloat3(&ro, rayOrigin);
        XMStoreFloat3(&rt, rayOrigin + dir * (length * 0.6f));
        AddLine(ro, rt, color);
    }
}

void EditorIconRenderer::AddPointLightSphere(const XMFLOAT3& center,
                                             f32 radius, u32 segments,
                                             const XMFLOAT3& color)
{
    const float step = XM_2PI / static_cast<float>(segments);

    for (u32 i = 0; i < segments; ++i)
    {
        float a0 = step * static_cast<float>(i);
        float a1 = step * static_cast<float>(i + 1);
        float c0 = cosf(a0), s0 = sinf(a0);
        float c1 = cosf(a1), s1 = sinf(a1);

        // XZ circle
        AddLine({ center.x + c0 * radius, center.y, center.z + s0 * radius },
                { center.x + c1 * radius, center.y, center.z + s1 * radius }, color);
        // XY circle
        AddLine({ center.x + c0 * radius, center.y + s0 * radius, center.z },
                { center.x + c1 * radius, center.y + s1 * radius, center.z }, color);
        // YZ circle
        AddLine({ center.x, center.y + c0 * radius, center.z + s0 * radius },
                { center.x, center.y + c1 * radius, center.z + s1 * radius }, color);
    }

    // 中心から4方向の小さな十字（光源マーク）
    const f32 crossSize = 0.3f;
    AddLine({ center.x - crossSize, center.y, center.z },
            { center.x + crossSize, center.y, center.z }, color);
    AddLine({ center.x, center.y - crossSize, center.z },
            { center.x, center.y + crossSize, center.z }, color);
    AddLine({ center.x, center.y, center.z - crossSize },
            { center.x, center.y, center.z + crossSize }, color);
}

// ========== Collect ==========

void EditorIconRenderer::CollectFromRegistry(entt::registry& registry,
                                             const EditorContext& ctx)
{
    const XMFLOAT3 colorDefault   = { 0.8f, 0.8f, 0.8f };
    const XMFLOAT3 colorSelected  = { 1.0f, 0.9f, 0.0f };
    const XMFLOAT3 colorDirLight  = { 1.0f, 0.9f, 0.2f };
    const XMFLOAT3 colorPointLight = { 1.0f, 0.6f, 0.1f };
    const XMFLOAT3 colorSpotLight = { 0.5f, 0.85f, 1.0f };
    const XMFLOAT3 colorCamActive = { 0.2f, 0.8f, 1.0f };

    // --- Camera ---
    {
        auto view = registry.view<const Transform, const CameraComponent>();
        for (auto [entity, tf, cam] : view.each())
        {
            bool selected = ctx.IsSelected(entity);
            XMFLOAT3 iconColor = cam.isActive ? colorCamActive : colorDefault;

            // Euler → quaternion
            XMFLOAT4 quat;
            if (tf.useQuaternion)
            {
                quat = tf.quaternion;
            }
            else
            {
                XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                    XMConvertToRadians(tf.rotation.x),
                    XMConvertToRadians(tf.rotation.y),
                    XMConvertToRadians(tf.rotation.z));
                XMStoreFloat4(&quat, q);
            }

            // 常時: カメラの向きに追従する 3D カメラアイコン（斜め下を向けばアイコンも斜め下）
            AddCameraIcon(tf.position, quat, selected ? colorSelected : iconColor);

            // 選択時のみ: FOV フラスタム線画（3D）
            if (selected)
                AddCameraFrustum(tf.position, quat, cam.fovDegrees, cam.nearClip, cam.farClip, colorSelected);
        }
    }

    // --- DirectionalLight ---
    {
        auto view = registry.view<const Transform, const DirectionalLight>();
        for (auto [entity, tf, dl] : view.each())
        {
            bool selected = ctx.IsSelected(entity);

            // 常時: 太陽アイコン
            AddDirLightIcon(tf.position, selected ? colorSelected : colorDirLight);

            // 選択時のみ: 方向矢印 + 放射線
            if (selected)
                AddDirectionalArrow(tf.position, dl.direction, 2.0f, colorSelected);
        }
    }

    // --- PointLight ---
    {
        auto view = registry.view<const Transform, const PointLight>();
        for (auto [entity, tf, pl] : view.each())
        {
            bool selected = ctx.IsSelected(entity);

            // 常時: ダイヤモンド十字アイコン
            AddPointLightIcon(tf.position, selected ? colorSelected : colorPointLight);

            // 選択時のみ: range 球体ワイヤーフレーム
            if (selected)
            {
                f32 radius = (std::min)(pl.range, 20.0f);
                AddPointLightSphere(tf.position, radius, 16, colorSelected);
            }
        }
    }

    // --- SpotLight ---
    {
        auto view = registry.view<const Transform, const SpotLight>();
        for (auto [entity, tf, sl] : view.each())
        {
            bool selected = ctx.IsSelected(entity);

            // 常時: アイコン
            AddPointLightIcon(tf.position, selected ? colorSelected : colorSpotLight);

            // 選択時のみ: コーン軸方向の矢印（照らす向きを可視化）
            if (selected)
            {
                f32 len = (std::min)(sl.range, 20.0f);
                AddDirectionalArrow(tf.position, sl.direction, len,
                                    selected ? colorSelected : colorSpotLight);
            }
        }
    }

    // --- ParticleEmitter（配置エフェクト）---
    {
        const XMFLOAT3 colorEmitter = { 1.0f, 0.55f, 0.15f };
        auto view = registry.view<const Transform, const ParticleEmitter>();
        for (auto [entity, tf, pe] : view.each())
        {
            bool selected = ctx.IsSelected(entity);
            AddPointLightIcon(tf.position, selected ? colorSelected : colorEmitter);
        }
    }

    // --- Trigger（イベント範囲）: アイコン + 選択時に範囲ワイヤーフレーム ---
    {
        const XMFLOAT3 colorTrigger = { 0.3f, 1.0f, 0.55f };
        auto view = registry.view<const Transform, const Trigger>();
        for (auto [entity, tf, tr] : view.each())
        {
            bool selected = ctx.IsSelected(entity);
            XMFLOAT3 col = selected ? colorSelected : colorTrigger;
            XMFLOAT3 c = { tf.position.x + tr.offset.x,
                           tf.position.y + tr.offset.y,
                           tf.position.z + tr.offset.z };
            AddPointLightIcon(c, col);

            if (tr.shape == static_cast<int>(TriggerShape::Sphere))
            {
                f32 sc = (std::max)((std::max)(tf.scale.x, tf.scale.y), tf.scale.z);
                AddPointLightSphere(c, tr.radius * sc, 16, col);
            }
            else
            {
                f32 hx = tr.halfExtents.x * tf.scale.x;
                f32 hy = tr.halfExtents.y * tf.scale.y;
                f32 hz = tr.halfExtents.z * tf.scale.z;
                XMFLOAT3 p[8] = {
                    { c.x-hx, c.y-hy, c.z-hz }, { c.x+hx, c.y-hy, c.z-hz },
                    { c.x+hx, c.y-hy, c.z+hz }, { c.x-hx, c.y-hy, c.z+hz },
                    { c.x-hx, c.y+hy, c.z-hz }, { c.x+hx, c.y+hy, c.z-hz },
                    { c.x+hx, c.y+hy, c.z+hz }, { c.x-hx, c.y+hy, c.z+hz },
                };
                const int edges[12][2] = {
                    {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
                };
                for (const auto& ed : edges) AddLine(p[ed[0]], p[ed[1]], col);
            }
        }
    }
}

// ========== Render ==========

void EditorIconRenderer::Render(ID3D12GraphicsCommandList* cmdList,
                                const XMFLOAT4X4& viewProj,
                                u32 screenW, u32 screenH)
{
    if (!m_initialized) return;

    // --- 3D 選択補助線（フラスタム/範囲球/矢印）---
    if (!m_vertices.empty())
    {
        u32 vertexCount = static_cast<u32>(m_vertices.size());
        if (vertexCount > kMaxVertices) vertexCount = kMaxVertices;

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, &mapped));
        memcpy(mapped, m_vertices.data(), vertexCount * sizeof(IconLineVertex));
        m_vertexBuffer->Unmap(0, nullptr);

        cmdList->SetPipelineState(m_pso.Get());
        cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
        cmdList->SetGraphicsRoot32BitConstants(0, 16, &viewProj, 0);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cmdList->IASetVertexBuffers(0, 1, &m_vbView);
        cmdList->DrawInstanced(vertexCount, 1, 0, 0);
    }

    // --- ビルボードアイコン（常にカメラを向く・一定サイズ）---
    if (!m_billboardVerts.empty())
    {
        u32 vertexCount = static_cast<u32>(m_billboardVerts.size());
        if (vertexCount > kMaxVertices) vertexCount = kMaxVertices;

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(m_billboardVB->Map(0, &readRange, &mapped));
        memcpy(mapped, m_billboardVerts.data(), vertexCount * sizeof(IconBillboardVertex));
        m_billboardVB->Unmap(0, nullptr);

        struct IconCB {
            XMFLOAT4X4 viewProj;
            float invScreen[2];
            float pad[2];
        } cb{};
        cb.viewProj = viewProj;
        cb.invScreen[0] = (screenW > 0) ? 1.0f / static_cast<float>(screenW) : 0.0f;
        cb.invScreen[1] = (screenH > 0) ? 1.0f / static_cast<float>(screenH) : 0.0f;

        cmdList->SetPipelineState(m_billboardPSO.Get());
        cmdList->SetGraphicsRootSignature(m_billboardRootSig.Get());
        cmdList->SetGraphicsRoot32BitConstants(0, 20, &cb, 0);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cmdList->IASetVertexBuffers(0, 1, &m_billboardVBView);
        cmdList->DrawInstanced(vertexCount, 1, 0, 0);
    }
}

} // namespace dx12e
