#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <random>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;

// CPUシミュレーション + GPUインスタンシングの加算ビルボードパーティクル。
// Lua の fx:burst / fx:ring から放出され、scene の HDR RT へ加算で描かれる。
// グローはシェーダ解析（テクスチャ無し）。粒子は全部「光る点」になる。
class ParticleSystem
{
public:
    // 放出パラメータ（Lua テーブルから組み立てる）。
    struct EmitParams
    {
        DirectX::XMFLOAT3 pos{0, 0, 0};
        int   count      = 16;
        DirectX::XMFLOAT3 dir{0, 1, 0}; // 基準方向（burst の偏り / ring は無視）
        float spread     = 1.0f;        // 0=dir方向に集中, 1=全球
        float speed      = 6.0f;
        float speedVar   = 0.5f;        // 0..1（速度のばらつき率）
        float size       = 0.4f;
        float sizeEnd    = 0.0f;
        float life       = 0.6f;
        float lifeVar    = 0.3f;
        DirectX::XMFLOAT3 color{1, 1, 1};
        DirectX::XMFLOAT3 colorEnd{1, 1, 1};
        bool  hasColorEnd = false;
        float intensity  = 3.0f;        // HDR増幅（>1 で白熱→ブルーム）
        float gravity    = 0.0f;        // y方向加速（負で落下）
        float drag       = 1.0f;        // 速度減衰/秒
        float up         = 0.0f;        // 初速の上向きバイアス
        bool  ring       = false;       // true=XZ平面に等間隔リング（衝撃波）
    };

    void Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat,
                    DXGI_FORMAT dsvFormat, const std::wstring& shaderDir);

    void Emit(const EmitParams& p);
    void Update(f32 dt);
    void Clear();

    // scene パス内（HDR RT + 深度バインド済み）で呼ぶ。
    void Render(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp);

    // 画面インパクト（ヒット時にクロマ/放射ブラーを瞬間的に上げる用）。
    void  AddPulse(float amount) { if (amount > m_pulse) m_pulse = amount; }
    float GetPulse() const { return m_pulse; }

    int AliveCount() const { return m_aliveCount; }

private:
    struct Particle
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 vel;
        DirectX::XMFLOAT3 col0;
        DirectX::XMFLOAT3 col1;
        float size0, size1;
        float age, life;
        float rot, rotVel;
        float gravity, drag, alpha;
        bool  alive = false;
    };

    // シェーダのインスタンス入力レイアウトと一致（stride 48）。
    struct GpuParticle
    {
        DirectX::XMFLOAT3 center;
        float             size;
        DirectX::XMFLOAT4 color;
        float             rot;
        float             _pad[3];
    };

    float Rand(float a, float b);

    static constexpr u32 kMaxParticles = 6000;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_instanceBuffer; // UPLOAD
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};

    std::vector<Particle>   m_particles;
    std::vector<GpuParticle> m_gpu;     // 毎フレーム再構築
    u32   m_cursor     = 0;             // 空きスロット探索の起点
    int   m_aliveCount = 0;
    float m_pulse      = 0.0f;
    std::mt19937 m_rng{1337u};
    bool  m_initialized = false;
};

} // namespace dx12e
