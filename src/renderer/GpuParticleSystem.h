#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <random>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;

// GPUパーティクル（compute シミュレーション + ExecuteIndirect 描画）。
// dead/alive リスト方式で最大 131072 粒子。CPU は放出リクエストを積むだけで、
// 生存管理・シム・描画数は全て GPU 内で完結する（readback なし）。
// 加算合成専用（ソート不要）= 火花・魔法・雪・デブリ等の大量粒子向け。
// 少数で挙動を細かく制御したい演出は従来の CPU ParticleSystem を使う（併存）。
class GpuParticleSystem
{
public:
    static constexpr u32 kMaxParticles = 131072;
    static constexpr u32 kMaxEmitsPerFrame = 64;

    // 放出リクエスト（フレーム内に積み、次の SimulateAndRender で GPU へ流す）
    struct EmitRequest
    {
        DirectX::XMFLOAT3 pos{0, 0, 0};
        u32   count = 256;
        DirectX::XMFLOAT3 dir{0, 1, 0};
        float spread = 1.0f;
        DirectX::XMFLOAT3 col0{1, 1, 1};   // 強度乗算済みで渡すこと
        float speed = 6.0f;
        DirectX::XMFLOAT3 col1{1, 1, 1};
        float speedVar = 0.5f;
        float size0 = 0.3f, size1 = 0.0f, life = 1.0f, lifeVar = 0.3f;
        float gravity = 0.0f, drag = 1.0f, up = 0.0f, turb = 0.0f;
        int   kind = 0;
        float stretch = 0.0f;
        bool  ring = false;   // true = XZ 平面へ等間隔（fx:ring 相当）
    };

    void Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat, const std::wstring& shaderDir);

    // シェーダーホットリロード用。compute 5本(Init/Prepare/Emit/Kickoff/Simulate) + 描画 PSO の
    // みを作り直す(ルートシグネチャ/バッファ/コマンドシグネチャは不変のため触らない)。
    // 描画 PS は Particle_PS.cso を ParticleSystem と共有しているため、そのまま再読み込みするだけでよい。
    void RecreatePipelines(GraphicsDevice& device);

    void Emit(const EmitRequest& r);   // フレーム内キューへ（上限 kMaxEmitsPerFrame）
    void Clear() { m_needsInit = true; m_requests.clear(); }   // 次フレームで全消去

    // soft particles 用シーン深度（CPU 版と同じ規約）
    void SetSceneDepth(D3D12_GPU_DESCRIPTOR_HANDLE srv, float projA, float projB,
                       float invRTW, float invRTH)
    { m_depthSrv = srv; m_projA = projA; m_projB = projB; m_invRTW = invRTW; m_invRTH = invRTH; m_hasDepth = true; }
    void DisableSceneDepth() { m_hasDepth = false; }

    // compute シム一式（prepare→emit→kickoff→simulate）+ ExecuteIndirect 描画。
    // シーン HDR RT / ビューポート / SRV ヒープをバインドし、深度 SRV を
    // PIXEL_SHADER_RESOURCE にした状態で（= CPU パーティクルパス内で）呼ぶこと。
    void SimulateAndRender(ID3D12GraphicsCommandList* cmd, float dt, float time,
                           DirectX::XMMATRIX viewProj,
                           DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp);

    bool IsReady() const { return m_psoSim != nullptr; }

private:
    void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_drawRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoInit;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPrepare;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoEmit;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoKickoff;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSim;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDraw;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchSig;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_drawSig;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleBuf;   // GPart × kMax（96B）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_aliveBuf[2];   // uint × kMax（ピンポン）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_deadBuf;       // uint × kMax
    Microsoft::WRL::ComPtr<ID3D12Resource> m_counterBuf;    // 16B raw
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dispatchArgs;  // 16B（Dispatch x,y,z）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_drawArgs;      // 16B（Draw 6,inst,0,0）

    // 状態追跡（particles/alive は UAV ↔ NON_PIXEL_SRV、args は UAV ↔ INDIRECT_ARGUMENT）
    D3D12_RESOURCE_STATES m_particleState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_aliveState[2] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    D3D12_RESOURCE_STATES m_dispatchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_drawArgState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    std::vector<EmitRequest> m_requests;
    u32  m_flip = 0;            // 0/1: alive リストの現/次
    bool m_needsInit = true;
    bool m_initialized = false;
    std::mt19937 m_rng{20260702u};

    D3D12_GPU_DESCRIPTOR_HANDLE m_depthSrv{};
    float m_projA = 0.0f, m_projB = 0.0f, m_invRTW = 0.0f, m_invRTH = 0.0f;
    bool  m_hasDepth = false;

    // RecreatePipelines 用に保持
    std::wstring m_shaderDir;
    DXGI_FORMAT  m_rtvFormat = DXGI_FORMAT_UNKNOWN;
};

} // namespace dx12e
