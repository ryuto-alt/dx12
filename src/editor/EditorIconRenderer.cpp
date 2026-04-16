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

    m_vertices.reserve(2048);
    m_initialized = true;
    Logger::Info("EditorIconRenderer initialized");
}

// ========== Frame ==========

void EditorIconRenderer::BeginFrame()
{
    m_vertices.clear();
}

// ========== Primitives ==========

void EditorIconRenderer::AddLine(XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 color)
{
    if (m_vertices.size() + 2 > kMaxVertices) return;
    m_vertices.push_back({ a, color });
    m_vertices.push_back({ b, color });
}

// ========== 常時表示アイコン ==========

void EditorIconRenderer::AddCameraIcon(const XMFLOAT3& pos,
                                       const XMFLOAT4& quat,
                                       const XMFLOAT3& color)
{
    // カメラ形状: ボディ(箱) + レンズ(台形)
    const f32 w = 0.3f, h = 0.22f, d = 0.2f;   // ボディ半サイズ
    const f32 lw = 0.12f, lh = 0.10f, ld = 0.15f; // レンズ半サイズ

    XMMATRIX rot = XMMatrixRotationQuaternion(XMLoadFloat4(&quat));
    XMVECTOR R = rot.r[0], U = rot.r[1], F = rot.r[2];
    XMVECTOR P = XMLoadFloat3(&pos);

    // ボディ 8頂点
    XMFLOAT3 c[8];
    const float signs[8][3] = {
        {-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},
        {-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1},
    };
    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR local = R * (signs[i][0] * w) + U * (signs[i][1] * h) + F * (signs[i][2] * d);
        XMStoreFloat3(&c[i], P + local);
    }
    // ボディ 12辺
    const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };
    for (auto& e : edges) AddLine(c[e[0]], c[e[1]], color);

    // レンズ（前面にちょっと飛び出た台形）
    XMFLOAT3 lens[4];
    XMStoreFloat3(&lens[0], P + F * (d + ld) + U * lh - R * lw);
    XMStoreFloat3(&lens[1], P + F * (d + ld) + U * lh + R * lw);
    XMStoreFloat3(&lens[2], P + F * (d + ld) - U * lh + R * lw);
    XMStoreFloat3(&lens[3], P + F * (d + ld) - U * lh - R * lw);
    // レンズ面 4辺
    AddLine(lens[0], lens[1], color);
    AddLine(lens[1], lens[2], color);
    AddLine(lens[2], lens[3], color);
    AddLine(lens[3], lens[0], color);
    // ボディ前面 → レンズ 4本
    AddLine(c[4], lens[0], color);
    AddLine(c[5], lens[1], color);
    AddLine(c[6], lens[2], color);
    AddLine(c[7], lens[3], color);
}

void EditorIconRenderer::AddDirLightIcon(const XMFLOAT3& pos,
                                         const XMFLOAT3& color)
{
    // 太陽マーク: 中心の円 + 放射線8本
    const f32 r = 0.25f;      // 中心円の半径
    const f32 rayIn = 0.35f;  // 放射線の内径
    const f32 rayOut = 0.55f; // 放射線の外径

    // 水平の円（XZ平面）
    const u32 seg = 12;
    const float step = XM_2PI / static_cast<float>(seg);
    for (u32 i = 0; i < seg; ++i)
    {
        float a0 = step * static_cast<float>(i);
        float a1 = step * static_cast<float>(i + 1);
        AddLine({ pos.x + cosf(a0) * r, pos.y, pos.z + sinf(a0) * r },
                { pos.x + cosf(a1) * r, pos.y, pos.z + sinf(a1) * r }, color);
    }

    // 放射線 8本
    for (int i = 0; i < 8; ++i)
    {
        float a = XM_2PI * static_cast<float>(i) / 8.0f;
        float cx = cosf(a), sz = sinf(a);
        AddLine({ pos.x + cx * rayIn,  pos.y, pos.z + sz * rayIn },
                { pos.x + cx * rayOut, pos.y, pos.z + sz * rayOut }, color);
    }
}

void EditorIconRenderer::AddPointLightIcon(const XMFLOAT3& pos,
                                           const XMFLOAT3& color)
{
    // 電球マーク: 小さな十字 + ダイヤモンド形
    const f32 s = 0.25f;

    // 3D 十字
    AddLine({ pos.x - s, pos.y, pos.z }, { pos.x + s, pos.y, pos.z }, color);
    AddLine({ pos.x, pos.y - s, pos.z }, { pos.x, pos.y + s, pos.z }, color);
    AddLine({ pos.x, pos.y, pos.z - s }, { pos.x, pos.y, pos.z + s }, color);

    // ダイヤモンド (XY, XZ, YZ 各4辺 = 12辺)
    const f32 d = 0.35f;
    // XY 平面
    AddLine({ pos.x + d, pos.y, pos.z }, { pos.x, pos.y + d, pos.z }, color);
    AddLine({ pos.x, pos.y + d, pos.z }, { pos.x - d, pos.y, pos.z }, color);
    AddLine({ pos.x - d, pos.y, pos.z }, { pos.x, pos.y - d, pos.z }, color);
    AddLine({ pos.x, pos.y - d, pos.z }, { pos.x + d, pos.y, pos.z }, color);
    // XZ 平面
    AddLine({ pos.x + d, pos.y, pos.z }, { pos.x, pos.y, pos.z + d }, color);
    AddLine({ pos.x, pos.y, pos.z + d }, { pos.x - d, pos.y, pos.z }, color);
    AddLine({ pos.x - d, pos.y, pos.z }, { pos.x, pos.y, pos.z - d }, color);
    AddLine({ pos.x, pos.y, pos.z - d }, { pos.x + d, pos.y, pos.z }, color);
    // YZ 平面
    AddLine({ pos.x, pos.y + d, pos.z }, { pos.x, pos.y, pos.z + d }, color);
    AddLine({ pos.x, pos.y, pos.z + d }, { pos.x, pos.y - d, pos.z }, color);
    AddLine({ pos.x, pos.y - d, pos.z }, { pos.x, pos.y, pos.z - d }, color);
    AddLine({ pos.x, pos.y, pos.z - d }, { pos.x, pos.y + d, pos.z }, color);
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

            // 常時: カメラアイコン（箱+レンズ形状）
            AddCameraIcon(tf.position, quat, selected ? colorSelected : iconColor);

            // 選択時のみ: フラスタム線画
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
}

// ========== Render ==========

void EditorIconRenderer::Render(ID3D12GraphicsCommandList* cmdList,
                                const XMFLOAT4X4& viewProj)
{
    if (!m_initialized || m_vertices.empty()) return;

    u32 vertexCount = static_cast<u32>(m_vertices.size());
    if (vertexCount > kMaxVertices)
        vertexCount = kMaxVertices;

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

} // namespace dx12e
