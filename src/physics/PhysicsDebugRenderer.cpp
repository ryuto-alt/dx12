#include "physics/PhysicsDebugRenderer.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "ecs/Components.h"
#include "physics/ColliderShape.h"   // 当たり判定の実効サイズの唯一の規約
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
    m_shaderDir = shaderDir;
    m_rtvFormat = rtvFormat;
    m_dsvFormat = dsvFormat;
    RecreatePipelines(device);

    // --- Dynamic Vertex Buffer (Upload Heap, kFrames 区画リング) ---
    // 前フレームが in-flight で読んでいる区画を上書きしないようフレームごとに書き分ける
    {
        const UINT bufferSize = kFrames * kMaxVertices * sizeof(DebugLineVertex);

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

void PhysicsDebugRenderer::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    auto vsData = ShaderCompiler::LoadFromFile(m_shaderDir + L"DebugLine_VS.cso");
    auto psData = ShaderCompiler::LoadFromFile(m_shaderDir + L"DebugLine_PS.cso");

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
    psoDesc.RTVFormats[0] = m_rtvFormat;
    psoDesc.DSVFormat = m_dsvFormat;
    psoDesc.SampleDesc = { 1, 0 };

    ThrowIfFailed(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));
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

void PhysicsDebugRenderer::CollectFromRegistry(entt::registry& registry,
                                               entt::entity selected,
                                               const XMFLOAT3& cameraPos)
{
    // ★ふるい。全部出すと壁と床だけで画面が線で埋まって読めない。
    //   「このエンティティが当たらない」を調べているときに、無関係な 200 個の線は邪魔でしかない。
    const DebugDrawFilter& flt = m_filter;

    // 選択中のもの本人か、その子孫か（親を辿って selected に届くか）。
    auto isSelfOrDescendant = [&registry](entt::entity e, entt::entity root)
    {
        if (root == entt::null) return false;
        // 親子が壊れて輪になっていても止まるように上限を切る。
        for (int guard = 0; guard < 64 && e != entt::null && registry.valid(e); ++guard)
        {
            if (e == root) return true;
            const auto* t = registry.try_get<Transform>(e);
            if (!t) break;
            e = t->parent;
        }
        return false;
    };

    // スコープの判定。位置はワールドで見る（親付きでも正しく絞れる）。
    auto inScope = [&](entt::entity e, const XMFLOAT3& worldPos)
    {
        switch (flt.scope)
        {
        case DebugDrawFilter::Scope::Selected:
            return isSelfOrDescendant(e, selected);
        case DebugDrawFilter::Scope::NearCamera:
        {
            const f32 dx = worldPos.x - cameraPos.x;
            const f32 dy = worldPos.y - cameraPos.y;
            const f32 dz = worldPos.z - cameraPos.z;
            const f32 r  = (std::max)(flt.maxDistance, 0.01f);
            return (dx * dx + dy * dy + dz * dz) <= (r * r);
        }
        case DebugDrawFilter::Scope::All:
        default:
            return true;
        }
    };

    // 色は「なぜ当たる / なぜ当たらない」が色だけで分かるように割り当てる。
    const XMFLOAT3 dynamicColor   = { 0.0f, 1.0f, 0.0f };  // 緑   : 動く
    const XMFLOAT3 staticColor    = { 0.5f, 0.5f, 1.0f };  // 青   : 動かない
    const XMFLOAT3 kinematicColor = { 1.0f, 1.0f, 0.0f };  // 黄   : スクリプトが動かす
    const XMFLOAT3 orphanColor    = { 1.0f, 0.25f, 0.25f };// 赤   : ★当たらない（下記）
    const XMFLOAT3 triggerColor   = { 1.0f, 0.35f, 1.0f }; // 紫   : Trigger（物理ではない）
    const XMFLOAT3 triggerHitColor= { 1.0f, 1.0f, 1.0f };  // 白   : Trigger に入っている最中

    // ★ワールド変換で描く。ローカルの Transform で描くと、親に付けたコライダーが
    //   まったく別の場所に線だけ出る（物理は ResolveWorldTRS でワールドを見ている）。
    auto worldTRS = [&registry](entt::entity e, const Transform& t,
                                XMFLOAT3& pos, XMFLOAT4& quat, XMFLOAT3& scale)
    {
        const bool parented = (t.parent != entt::null) && registry.valid(t.parent);
        if (!parented)
        {
            pos   = t.position;
            scale = t.scale;
            if (t.useQuaternion) quat = t.quaternion;
            else
                XMStoreFloat4(&quat, XMQuaternionRotationRollPitchYaw(
                    XMConvertToRadians(t.rotation.x),
                    XMConvertToRadians(t.rotation.y),
                    XMConvertToRadians(t.rotation.z)));
            return;
        }
        XMVECTOR s, q, p;
        if (!XMMatrixDecompose(&s, &q, &p, ComputeWorldMatrix(registry, e)))
        {
            pos = t.position; scale = t.scale; quat = XMFLOAT4(0, 0, 0, 1);
            return;
        }
        XMStoreFloat3(&pos, p);
        XMStoreFloat3(&scale, s);
        XMStoreFloat4(&quat, q);
    };

    // コライダーのオフセットはローカル。回転を掛けてからワールド位置へ足す
    // （物理側も「回した後の位置」に置いている）。
    auto applyOffset = [](const XMFLOAT3& pos, const XMFLOAT3& offset, const XMFLOAT4& quat)
    {
        XMVECTOR o = XMVector3Rotate(XMVectorSet(offset.x, offset.y, offset.z, 0.0f),
                                     XMLoadFloat4(&quat));
        XMFLOAT3 out;
        XMStoreFloat3(&out, XMLoadFloat3(&pos) + o);
        return out;
    };

    // 1 エンティティぶんのコライダーを描く。RigidBody の有無で色を変えるだけで形は同じ。
    auto drawCollider = [&](entt::entity entity, const Transform& transform, const XMFLOAT3& color)
    {
        XMFLOAT3 wpos, wscale;
        XMFLOAT4 wquat;
        worldTRS(entity, transform, wpos, wquat, wscale);

        const auto* convex  = registry.try_get<ConvexHullCollider>(entity);
        const auto* box     = registry.try_get<BoxCollider>(entity);
        const auto* sphere  = registry.try_get<SphereCollider>(entity);
        const auto* capsule = registry.try_get<CapsuleCollider>(entity);

        if (convex && !convex->points.empty())
        {
            const XMFLOAT3 center = applyOffset(wpos, convex->offset, wquat);
            XMMATRIX rot = XMMatrixRotationQuaternion(XMLoadFloat4(&wquat));
            XMVECTOR c   = XMLoadFloat3(&center);

            std::vector<XMFLOAT3> worldPts;
            worldPts.reserve(convex->points.size());
            for (const auto& p : convex->points)
            {
                // 凸包の頂点もスケールが乗る（Jolt へ渡す点群と同じ扱い）。
                XMVECTOR local = XMVectorSet(p.x * wscale.x, p.y * wscale.y, p.z * wscale.z, 0.0f);
                XMFLOAT3 wp;
                XMStoreFloat3(&wp, XMVector3TransformNormal(local, rot) + c);
                worldPts.push_back(wp);
            }
            const size_t count = worldPts.size();
            const size_t step  = (std::max)(count / 32, static_cast<size_t>(1));
            for (size_t i = 0; i < count; i += step)
                AddLine(worldPts[i], worldPts[(i + step) % count], color);
        }
        else if (box)
        {
            AddBox(applyOffset(wpos, box->offset, wquat),
                   collider::BoxHalfExtents(box->halfExtents, wscale), wquat, color);
        }
        else if (sphere)
        {
            AddSphere(applyOffset(wpos, sphere->offset, wquat),
                      collider::SphereRadius(sphere->radius, wscale), 16, color);
        }
        else if (capsule)
        {
            AddCapsule(applyOffset(wpos, capsule->offset, wquat),
                       collider::CapsuleRadius(capsule->radius, wscale),
                       collider::CapsuleHalfHeight(capsule->halfHeight, wscale), wquat, color);
        }
        else
        {
            AddBox(wpos, collider::FallbackHalfExtents(wscale), wquat, color);
        }
    };

    // ---- RigidBody 付き（＝実際に物理で当たるもの）----
    auto view = registry.view<Transform, RigidBody>();
    for (auto [entity, transform, rb] : view.each())
    {
        if (rb.bodyId == kInvalidBodyId) continue;

        // 種類のふるい。★Static を切れるのが一番効く（壁と床が線の大半）。
        XMFLOAT3 color = dynamicColor;
        if (rb.motionType == MotionType::Static)
        {
            if (!flt.showStatic) continue;
            color = staticColor;
        }
        else if (rb.motionType == MotionType::Kinematic)
        {
            if (!flt.showKinematic) continue;
            color = kinematicColor;
        }
        else if (!flt.showDynamic) continue;

        if (!inScope(entity, transform.position)) continue;
        drawCollider(entity, transform, color);
    }

    // ---- ★コライダーはあるのに RigidBody が無いもの（＝何にも当たらない）----
    // 「コライダーを付けたのにすり抜ける」で一番多い原因がこれ。線が出ないと
    // 「そもそも判定が無い」のか「判定はあるが位置がずれている」のか区別できないので、
    // 赤で出して「これは当たらない」と分かるようにする。
    auto orphan = [&](auto&& colliderView)
    {
        for (auto entity : colliderView)
        {
            if (registry.all_of<RigidBody>(entity)) continue;
            if (registry.all_of<CharacterController>(entity)) continue;   // CC は下で描く
            const Transform& t = colliderView.template get<Transform>(entity);
            if (!inScope(entity, t.position)) continue;
            drawCollider(entity, t, orphanColor);
        }
    };
    if (flt.showOrphans)
    {
        orphan(registry.view<Transform, BoxCollider>());
        orphan(registry.view<Transform, SphereCollider>());
        orphan(registry.view<Transform, CapsuleCollider>());
    }

    // ---- CharacterController（RigidBody とは排他）----
    // 接地中=緑、空中=オレンジ。回転は物理に渡していないので軸はそのまま。
    const XMFLOAT4 ccQuat(0, 0, 0, 1);
    auto ccView = registry.view<Transform, CharacterController>();
    for (auto [entity, transform, cc] : ccView.each())
    {
        if (!flt.showCharacter) continue;
        if (!inScope(entity, transform.position)) continue;
        XMFLOAT3 wpos, wscale;
        XMFLOAT4 wquat;
        worldTRS(entity, transform, wpos, wquat, wscale);
        const XMFLOAT3 center = { wpos.x + cc.offset.x, wpos.y + cc.offset.y, wpos.z + cc.offset.z };
        const XMFLOAT3 color  = cc._grounded ? XMFLOAT3{ 0.0f, 1.0f, 0.0f }
                                             : XMFLOAT3{ 1.0f, 0.6f, 0.0f };
        AddCapsule(center, cc.radius, cc.halfHeight, ccQuat, color);
    }

    // ---- Trigger（物理ではなく ScriptEngine が内外判定するゲーム用の領域）----
    // ★物理コライダーと同じ窓で見えないと、「入ったのに発火しない」を調べるときに
    //   プレイヤーのカプセルとトリガー範囲の位置関係が分からない。
    //   判定中は白く光らせる＝Play 中に「今入っている」が目で追える。
    if (flt.showTriggers)
    {
        auto trView = registry.view<Transform, Trigger>();
        for (auto [entity, transform, tr] : trView.each())
        {
            if (!inScope(entity, transform.position)) continue;
            XMFLOAT3 wpos, wscale;
            XMFLOAT4 wquat;
            worldTRS(entity, transform, wpos, wquat, wscale);
            const XMFLOAT3 center = { wpos.x + tr.offset.x,
                                      wpos.y + tr.offset.y,
                                      wpos.z + tr.offset.z };
            const XMFLOAT3 color = tr._wasInside ? triggerHitColor : triggerColor;

            if (tr.shape == static_cast<int>(TriggerShape::Sphere))
                AddSphere(center, collider::TriggerSphereRadius(tr.radius, wscale), 16, color);
            else
                // ★内外判定は軸平行（回転を見ない）ので、線も回さない。
                //   回して描くと「線の中に居るのに発火しない」になる。
                AddBox(center, collider::TriggerBoxHalfExtents(tr.halfExtents, wscale),
                       XMFLOAT4(0, 0, 0, 1), color);
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

    // Upload vertices（フレーム多重化: 区画リングへ書き込み）
    m_frameIdx = (m_frameIdx + 1) % kFrames;
    const UINT regionBytes  = kMaxVertices * sizeof(DebugLineVertex);
    const UINT regionOffset = m_frameIdx * regionBytes;

    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, &mapped));
    memcpy(static_cast<u8*>(mapped) + regionOffset, m_vertices.data(),
           vertexCount * sizeof(DebugLineVertex));
    m_vertexBuffer->Unmap(0, nullptr);

    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress() + regionOffset;
    m_vbView.SizeInBytes    = regionBytes;

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
