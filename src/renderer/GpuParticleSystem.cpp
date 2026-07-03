#include "renderer/GpuParticleSystem.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

using namespace DirectX;

namespace dx12e
{
// HLSL の GPCB と一致（9 x float4 = 36 DWORD、ルート定数）
struct GPCB
{
    float emitPos[4];
    float emitDir[4];
    float emitCol0[4];
    float emitCol1[4];
    float emitP0[4];
    float emitP1[4];
    float emitP2[4];
    float simP[4];
    float simP3[4];
};
static_assert(sizeof(GPCB) == 36 * sizeof(float), "GPCB must be 36 DWORDs");
static constexpr UINT kGPCBNum32 = 36;

static constexpr u32 kGPartSize = 96;   // HLSL GPart と一致

void GpuParticleSystem::Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat,
                                   const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // --- Compute Root Signature: b0(36 DWORD) + u0..u6(root UAV ×7) = 50 DWORD ---
    {
        D3D12_ROOT_PARAMETER params[8]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = kGPCBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        for (u32 i = 0; i < 7; ++i)
        {
            params[1 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[1 + i].Descriptor.ShaderRegister = i;   // u0..u6
            params[1 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 8;
        desc.pParameters   = params;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_computeRS)));
    }

    // --- Draw Root Signature: b0(32定数,ALL) + t0(深度table,PS) + t1/t2(root SRV,VS) + t2(粒子アルベドtable,PS) + s0 ---
    // t2 は VS の gAlive(root SRV) と PS の gAlbedo(table) で register 番号が重複するが、
    // シェーダ可視性(VERTEX/PIXEL)が別なので D3D12 上は正当な別バインディングとして共存できる。
    // Particle_PS.cso は ParticleSystem と共用しており gAlbedo(t2) を宣言済みのため、GPU パーティクル
    // 側もこのテーブルを用意しないと PSO 作成が REGISTER 不足で失敗する（GpuParticleDraw.hlsl の
    // VS 側は texIdx=kNoTexture 固定なので実際にはサンプルされない＝ダミー束縛で足りる）。
    {
        D3D12_DESCRIPTOR_RANGE depthRange{};
        depthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        depthRange.NumDescriptors     = 1;
        depthRange.BaseShaderRegister = 0;   // t0

        D3D12_DESCRIPTOR_RANGE albedoRange{};
        albedoRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        albedoRange.NumDescriptors     = 1;
        albedoRange.BaseShaderRegister = 2;   // t2（PIXEL可視。VSのgAlive t2とは可視性が別なので衝突しない）

        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &depthRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t1 = particles
        params[2].Descriptor.ShaderRegister = 1;
        params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t2 = alive
        params[3].Descriptor.ShaderRegister = 2;
        params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        params[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable.NumDescriptorRanges = 1;
        params[4].DescriptorTable.pDescriptorRanges   = &albedoRange;
        params[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderRegister   = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 5;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_drawRS)));
    }

    // --- PSO 生成（ホットリロード用に RecreatePipelines へ切り出し）---
    m_shaderDir = shaderDir;
    m_rtvFormat = rtvFormat;
    RecreatePipelines(device);

    // --- コマンドシグネチャ（間接 Dispatch / Draw）---
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride       = sizeof(D3D12_DISPATCH_ARGUMENTS);
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs   = &arg;
        ThrowIfFailed(dev->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_dispatchSig)));

        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        ThrowIfFailed(dev->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_drawSig)));
    }

    // --- バッファ（DEFAULT ヒープ・UAV）---
    auto makeBuf = [&](u64 size, Microsoft::WRL::ComPtr<ID3D12Resource>& out)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = size;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };
    makeBuf(static_cast<u64>(kMaxParticles) * kGPartSize, m_particleBuf);
    makeBuf(static_cast<u64>(kMaxParticles) * sizeof(u32), m_aliveBuf[0]);
    makeBuf(static_cast<u64>(kMaxParticles) * sizeof(u32), m_aliveBuf[1]);
    makeBuf(static_cast<u64>(kMaxParticles) * sizeof(u32), m_deadBuf);
    makeBuf(16, m_counterBuf);
    makeBuf(16, m_dispatchArgs);
    makeBuf(16, m_drawArgs);

    m_requests.reserve(kMaxEmitsPerFrame);
    m_initialized = true;
    Logger::Info("GpuParticleSystem initialized (max {} particles, compute + indirect draw)", kMaxParticles);
}

void GpuParticleSystem::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    // --- Compute PSO ×5 ---
    auto makeCS = [&](const wchar_t* cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_computeRS.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"GpuParticleInit_CS.cso",     m_psoInit);
    makeCS(L"GpuParticlePrepare_CS.cso",  m_psoPrepare);
    makeCS(L"GpuParticleEmit_CS.cso",     m_psoEmit);
    makeCS(L"GpuParticleKickoff_CS.cso",  m_psoKickoff);
    makeCS(L"GpuParticleSimulate_CS.cso", m_psoSim);

    // --- Draw PSO（VS=GpuParticleDraw / PS=Particle_PS 流用・加算・深度なし）---
    {
        auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"GpuParticleDraw_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"Particle_PS.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature        = m_drawRS.Get();
        pso.VS                    = { vs.GetData(), vs.GetSize() };
        pso.PS                    = { ps.GetData(), ps.GetSize() };
        pso.InputLayout           = { nullptr, 0 };   // 頂点バッファ無し
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable    = TRUE;
        rt.SrcBlend       = D3D12_BLEND_ONE;
        rt.DestBlend      = D3D12_BLEND_ONE;
        rt.BlendOp        = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha  = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask       = UINT_MAX;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0]    = m_rtvFormat;
        pso.DSVFormat        = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc       = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoDraw)));
    }
}

void GpuParticleSystem::Emit(const EmitRequest& r)
{
    if (m_requests.size() >= kMaxEmitsPerFrame) return;
    m_requests.push_back(r);
}

void GpuParticleSystem::Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

void GpuParticleSystem::UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cmd->ResourceBarrier(1, &b);
}

void GpuParticleSystem::SimulateAndRender(ID3D12GraphicsCommandList* cmd, float dt, float time,
                                          XMMATRIX viewProj,
                                          XMFLOAT3 camRight, XMFLOAT3 camUp)
{
    if (!m_initialized) return;
    // 何も居らず何も出さないフレームは丸ごとスキップ（維持コストゼロ）
    if (m_requests.empty() && m_needsInit) return;

    const u32 cur = m_flip, next = 1 - m_flip;

    // 前フレームで VS 読みにした particles/aliveNext(=今フレームの cur) を UAV へ戻す
    Barrier(cmd, m_particleBuf.Get(), m_particleState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_particleState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for (u32 i = 0; i < 2; ++i)
    {
        Barrier(cmd, m_aliveBuf[i].Get(), m_aliveState[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_aliveState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    Barrier(cmd, m_dispatchArgs.Get(), m_dispatchState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_dispatchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmd, m_drawArgs.Get(), m_drawArgState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_drawArgState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    cmd->SetComputeRootSignature(m_computeRS.Get());
    cmd->SetComputeRootUnorderedAccessView(1, m_particleBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, m_aliveBuf[cur]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_aliveBuf[next]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, m_deadBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(5, m_counterBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(6, m_dispatchArgs->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(7, m_drawArgs->GetGPUVirtualAddress());

    GPCB cb{};
    cb.simP[0] = dt;
    cb.simP[1] = time;
    cb.simP[2] = static_cast<float>(kMaxParticles);
    cb.simP[3] = 0.05f;   // 床バウンド高さ（CPU 版と同じ）

    // ---- 初回/Clear 後: dead リスト・カウンタを初期化 ----
    if (m_needsInit)
    {
        cmd->SetComputeRoot32BitConstants(0, kGPCBNum32, &cb, 0);
        cmd->SetPipelineState(m_psoInit.Get());
        cmd->Dispatch((kMaxParticles + 255) / 256, 1, 1);
        UavBarrier(cmd, m_deadBuf.Get());
        UavBarrier(cmd, m_counterBuf.Get());
        m_needsInit = false;
    }

    // ---- ① 前フレームの生存数を入力側へ ----
    cmd->SetComputeRoot32BitConstants(0, kGPCBNum32, &cb, 0);
    cmd->SetPipelineState(m_psoPrepare.Get());
    cmd->Dispatch(1, 1, 1);
    UavBarrier(cmd, m_counterBuf.Get());

    // ---- ② 放出（リクエスト毎に Dispatch。atomic 確保なのでリクエスト間バリア不要）----
    if (!m_requests.empty())
    {
        cmd->SetPipelineState(m_psoEmit.Get());
        std::uniform_real_distribution<float> dist(0.0f, 1000.0f);
        for (const auto& r : m_requests)
        {
            GPCB ecb = cb;
            ecb.emitPos[0] = r.pos.x; ecb.emitPos[1] = r.pos.y; ecb.emitPos[2] = r.pos.z;
            ecb.emitPos[3] = static_cast<float>((std::min)(r.count, kMaxParticles));
            ecb.emitDir[0] = r.dir.x; ecb.emitDir[1] = r.dir.y; ecb.emitDir[2] = r.dir.z;
            ecb.emitDir[3] = r.spread;
            ecb.emitCol0[0] = r.col0.x; ecb.emitCol0[1] = r.col0.y; ecb.emitCol0[2] = r.col0.z;
            ecb.emitCol0[3] = r.speed;
            ecb.emitCol1[0] = r.col1.x; ecb.emitCol1[1] = r.col1.y; ecb.emitCol1[2] = r.col1.z;
            ecb.emitCol1[3] = r.speedVar;
            ecb.emitP0[0] = r.size0; ecb.emitP0[1] = r.size1;
            ecb.emitP0[2] = r.life;  ecb.emitP0[3] = r.lifeVar;
            ecb.emitP1[0] = r.gravity; ecb.emitP1[1] = r.drag;
            ecb.emitP1[2] = r.up;      ecb.emitP1[3] = r.turb;
            ecb.emitP2[0] = static_cast<float>(r.kind);
            ecb.emitP2[1] = r.stretch;
            ecb.emitP2[2] = dist(m_rng);   // シード
            cmd->SetComputeRoot32BitConstants(0, kGPCBNum32, &ecb, 0);
            cmd->Dispatch((r.count + 255) / 256, 1, 1);
        }
        m_requests.clear();
        UavBarrier(cmd, m_counterBuf.Get());
        UavBarrier(cmd, m_particleBuf.Get());
        UavBarrier(cmd, m_aliveBuf[cur].Get());
    }

    // ---- ③ 間接 Dispatch / Draw 引数を書く ----
    cmd->SetComputeRoot32BitConstants(0, kGPCBNum32, &cb, 0);
    cmd->SetPipelineState(m_psoKickoff.Get());
    cmd->Dispatch(1, 1, 1);
    UavBarrier(cmd, m_dispatchArgs.Get());
    UavBarrier(cmd, m_drawArgs.Get());

    // ---- ④ シミュレーション（間接 Dispatch）----
    Barrier(cmd, m_dispatchArgs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_dispatchState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    cmd->SetPipelineState(m_psoSim.Get());
    cmd->ExecuteIndirect(m_dispatchSig.Get(), 1, m_dispatchArgs.Get(), 0, nullptr, 0);
    UavBarrier(cmd, m_particleBuf.Get());
    UavBarrier(cmd, m_aliveBuf[next].Get());
    UavBarrier(cmd, m_drawArgs.Get());
    UavBarrier(cmd, m_counterBuf.Get());

    // ---- ⑤ 描画（ExecuteIndirect。生存粒子は alive[next]）----
    Barrier(cmd, m_drawArgs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_drawArgState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    Barrier(cmd, m_particleBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_particleState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barrier(cmd, m_aliveBuf[next].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_aliveState[next] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    struct CamCB
    {
        XMFLOAT4X4 viewProj;
        XMFLOAT4   camRight;
        XMFLOAT4   camUp;
        XMFLOAT4   params;
        XMFLOAT4   params2;
    } ccb;
    XMStoreFloat4x4(&ccb.viewProj, XMMatrixTranspose(viewProj));
    ccb.camRight = { camRight.x, camRight.y, camRight.z, 0.0f };
    ccb.camUp    = { camUp.x,    camUp.y,    camUp.z,    0.0f };
    ccb.params   = { 1.0f, 2.2f, time, 0.5f };
    ccb.params2  = { m_projA, m_projB, m_hasDepth ? m_invRTW : 0.0f, m_invRTH };

    cmd->SetGraphicsRootSignature(m_drawRS.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 32, &ccb, 0);
    if (m_hasDepth) cmd->SetGraphicsRootDescriptorTable(1, m_depthSrv);
    // gAlbedo(t2,PIXEL) は Particle_PS.cso 共用のためのダミー束縛（GpuParticleDraw.hlsl は
    // texIdx=kNoTexture 固定で実際にはサンプルしない。深度SRVを使い回して有効な記述子にしておく）。
    if (m_hasDepth) cmd->SetGraphicsRootDescriptorTable(4, m_depthSrv);
    cmd->SetGraphicsRootShaderResourceView(2, m_particleBuf->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(3, m_aliveBuf[next]->GetGPUVirtualAddress());
    cmd->SetPipelineState(m_psoDraw.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->ExecuteIndirect(m_drawSig.Get(), 1, m_drawArgs.Get(), 0, nullptr, 0);

    m_flip = next;
}

} // namespace dx12e
