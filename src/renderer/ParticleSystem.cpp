#include "renderer/ParticleSystem.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

void ParticleSystem::Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat,
                                DXGI_FORMAT dsvFormat, const std::wstring& shaderDir)
{
    static_assert(sizeof(GpuParticle) == 56, "GpuParticle stride must match shader instance layout (56)");
    auto* dev = device.GetDevice();

    // --- Root Signature: b0(28 DWORD constants, ALL可視) のみ。SRV/サンプラー無し ---
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;   // b0
        param.Constants.Num32BitValues = 28;  // float4x4 + float4*3
        param.ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 1;
        desc.pParameters   = &param;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- PSO: インスタンスストリーム / 加算ブレンド / 深度テストON・書込OFF ---
    {
        auto vs = ShaderCompiler::LoadFromFile(shaderDir + L"Particle_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(shaderDir + L"Particle_PS.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,          0, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,          0, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, 36, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // stretch
            {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 40, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // vel
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature        = m_rootSig.Get();
        pso.VS                    = { vs.GetData(), vs.GetSize() };
        pso.PS                    = { ps.GetData(), ps.GetSize() };
        pso.InputLayout           = { layout, _countof(layout) };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;

        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable           = TRUE;
        rt.SrcBlend              = D3D12_BLEND_ONE;   // 加算合成
        rt.DestBlend             = D3D12_BLEND_ONE;
        rt.BlendOp               = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rt.DestBlendAlpha        = D3D12_BLEND_ONE;
        rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState.DepthEnable    = TRUE;                          // 壁/床に遮蔽される
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;   // 粒子同士は重なる
        pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.DepthStencilState.StencilEnable  = FALSE;

        pso.SampleMask            = UINT_MAX;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = rtvFormat;
        pso.DSVFormat             = dsvFormat;
        pso.SampleDesc            = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
    }

    // --- インスタンスバッファ（UPLOADヒープ。SpriteRenderer と同じ運用）---
    {
        const UINT bufferSize = kMaxParticles * sizeof(GpuParticle);
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC res{};
        res.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        res.Width            = bufferSize;
        res.Height           = 1;
        res.DepthOrArraySize = 1;
        res.MipLevels        = 1;
        res.SampleDesc       = {1, 0};
        res.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &res,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceBuffer)));

        m_vbView.BufferLocation = m_instanceBuffer->GetGPUVirtualAddress();
        m_vbView.StrideInBytes  = sizeof(GpuParticle);
        m_vbView.SizeInBytes    = bufferSize;
    }

    m_particles.assign(kMaxParticles, Particle{});
    m_gpu.reserve(kMaxParticles);
    m_initialized = true;
    Logger::Info("ParticleSystem initialized (max {} particles)", kMaxParticles);
}

float ParticleSystem::Rand(float a, float b)
{
    std::uniform_real_distribution<float> d(a, b);
    return d(m_rng);
}

void ParticleSystem::Emit(const EmitParams& p)
{
    if (!m_initialized) return;
    int count = (std::min)(p.count, static_cast<int>(kMaxParticles));

    // dir 正規化
    XMVECTOR dirV = XMLoadFloat3(&p.dir);
    if (XMVectorGetX(XMVector3LengthSq(dirV)) < 1e-6f) dirV = XMVectorSet(0, 1, 0, 0);
    dirV = XMVector3Normalize(dirV);
    XMFLOAT3 dir; XMStoreFloat3(&dir, dirV);

    for (int n = 0; n < count; ++n)
    {
        // 空きスロット探索（カーソルから一巡）
        u32 idx = kMaxParticles;
        for (u32 s = 0; s < kMaxParticles; ++s)
        {
            u32 c = (m_cursor + s) % kMaxParticles;
            if (!m_particles[c].alive) { idx = c; m_cursor = (c + 1) % kMaxParticles; break; }
        }
        if (idx == kMaxParticles) break; // 満杯
        Particle& pt = m_particles[idx];

        // 方向：ring=XZ等間隔, それ以外は球/円錐ランダム
        XMFLOAT3 vdir;
        if (p.ring)
        {
            float a = (static_cast<float>(n) / static_cast<float>((std::max)(count, 1))) * 6.2831853f;
            vdir = { std::cos(a), 0.0f, std::sin(a) };
        }
        else
        {
            // 単位球上の一様サンプル
            float z = Rand(-1.0f, 1.0f);
            float t = Rand(0.0f, 6.2831853f);
            float r = std::sqrt((std::max)(0.0f, 1.0f - z * z));
            XMFLOAT3 sph{ r * std::cos(t), z, r * std::sin(t) };
            // dir 方向へ寄せる（spread=1 で完全ランダム、0 で dir）
            vdir.x = sph.x * p.spread + dir.x * (1.0f - p.spread);
            vdir.y = sph.y * p.spread + dir.y * (1.0f - p.spread);
            vdir.z = sph.z * p.spread + dir.z * (1.0f - p.spread);
        }

        float spd = p.speed * (1.0f - Rand(0.0f, p.speedVar));
        pt.pos = p.pos;
        pt.vel = { vdir.x * spd, vdir.y * spd + p.up * p.speed * 0.5f, vdir.z * spd };
        pt.col0 = { p.color.x * p.intensity, p.color.y * p.intensity, p.color.z * p.intensity };
        if (p.hasColorEnd)
            pt.col1 = { p.colorEnd.x * p.intensity, p.colorEnd.y * p.intensity, p.colorEnd.z * p.intensity };
        else
            pt.col1 = pt.col0;
        pt.size0 = p.size  * Rand(0.7f, 1.0f);
        pt.size1 = p.sizeEnd;
        pt.life  = (std::max)(0.05f, p.life * (1.0f - Rand(0.0f, p.lifeVar)));
        pt.age   = 0.0f;
        pt.rot   = Rand(0.0f, 6.2831853f);
        pt.rotVel = Rand(-4.0f, 4.0f);
        pt.gravity = p.gravity;
        pt.drag  = p.drag;
        pt.alpha = 1.0f;
        pt.stretch = p.stretch;
        pt.turbStrength = p.turbStrength;
        pt.turbFreq = p.turbFreq;
        pt.alive = true;
    }
}

void ParticleSystem::Update(f32 dt)
{
    // 画面パルス減衰
    if (m_pulse > 0.0f) m_pulse = (std::max)(0.0f, m_pulse - dt * 3.0f);
    if (dt <= 0.0f) return;

    for (auto& pt : m_particles)
    {
        if (!pt.alive) continue;
        pt.age += dt;
        if (pt.age >= pt.life) { pt.alive = false; continue; }

        // 乱流（value-noise 風ハッシュ）：有機的な揺らぎ。turbStrength=0 で無効。
        if (pt.turbStrength > 0.0f)
        {
            float f = pt.turbFreq;
            auto frac = [](float v) { return v - std::floor(v); };
            float nx = frac(std::sin(pt.pos.x * 127.1f * f + pt.pos.y * 311.7f + pt.age * 0.7f) * 43758.5453f);
            float ny = frac(std::sin(pt.pos.y * 269.5f * f + pt.pos.z * 183.3f) * 43758.5453f);
            float nz = frac(std::sin(pt.pos.z * 419.2f * f + pt.pos.x * 371.9f + pt.age * 0.3f) * 43758.5453f);
            pt.vel.x += (nx * 2.0f - 1.0f) * pt.turbStrength * dt;
            pt.vel.y += (ny * 2.0f - 1.0f) * pt.turbStrength * dt;
            pt.vel.z += (nz * 2.0f - 1.0f) * pt.turbStrength * dt;
        }

        pt.vel.y += pt.gravity * dt;
        float damp = (std::max)(0.0f, 1.0f - pt.drag * dt);
        pt.vel.x *= damp; pt.vel.y *= damp; pt.vel.z *= damp;
        pt.pos.x += pt.vel.x * dt;
        pt.pos.y += pt.vel.y * dt;
        pt.pos.z += pt.vel.z * dt;
        pt.rot += pt.rotVel * dt;
        // 床で軽くバウンド
        if (pt.pos.y < 0.05f && pt.vel.y < 0.0f)
        {
            pt.pos.y = 0.05f;
            pt.vel.y = -pt.vel.y * 0.35f;
            pt.vel.x *= 0.7f; pt.vel.z *= 0.7f;
        }
    }
}

void ParticleSystem::Clear()
{
    for (auto& pt : m_particles) pt.alive = false;
    m_aliveCount = 0;
    m_pulse = 0.0f;
}

void ParticleSystem::Render(ID3D12GraphicsCommandList* cmd, XMMATRIX viewProj,
                            XMFLOAT3 camRight, XMFLOAT3 camUp)
{
    if (!m_initialized) return;

    // 生きてる粒子を GPU バッファへ詰める（寿命でフェード）
    m_gpu.clear();
    for (const auto& pt : m_particles)
    {
        if (!pt.alive) continue;
        float t = pt.age / pt.life;           // 0..1
        float fade = 1.0f - t;                // 末尾でフェードアウト
        fade = fade * fade;
        GpuParticle g;
        g.center = pt.pos;
        g.size   = pt.size0 + (pt.size1 - pt.size0) * t;
        g.color  = {
            pt.col0.x + (pt.col1.x - pt.col0.x) * t,
            pt.col0.y + (pt.col1.y - pt.col0.y) * t,
            pt.col0.z + (pt.col1.z - pt.col0.z) * t,
            pt.alpha * fade
        };
        g.rot     = pt.rot;
        g.stretch = pt.stretch;
        g.vel     = pt.vel;
        g._pad    = 0.0f;
        m_gpu.push_back(g);
        if (m_gpu.size() >= kMaxParticles) break;
    }
    m_aliveCount = static_cast<int>(m_gpu.size());
    if (m_gpu.empty()) return;

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    ThrowIfFailed(m_instanceBuffer->Map(0, &readRange, &mapped));
    memcpy(mapped, m_gpu.data(), m_gpu.size() * sizeof(GpuParticle));
    m_instanceBuffer->Unmap(0, nullptr);

    struct CamCB
    {
        XMFLOAT4X4 viewProj;
        XMFLOAT4   camRight;
        XMFLOAT4   camUp;
        XMFLOAT4   params;
    } cb;
    XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(viewProj));
    cb.camRight = { camRight.x, camRight.y, camRight.z, 0.0f };
    cb.camUp    = { camUp.x,    camUp.y,    camUp.z,    0.0f };
    cb.params   = { 1.0f, 2.2f, 0.0f, 0.0f }; // x=intensity, y=softness

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 28, &cb, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->DrawInstanced(6, static_cast<UINT>(m_gpu.size()), 0, 0);
}

} // namespace dx12e
