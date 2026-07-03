#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;

// 環境キューブから IBL 派生（irradiance / prefiltered / BRDF LUT）を compute で生成し、
// shader-visible SRV ヒープ上に連続3枚(t5,t6,t7)を確保して保持する。
// 環境キューブが無い場合でも 1x1 ダミーを生成し、テーブルは常に bind 可能（hasIBL で分岐）。
class IBLBaker
{
public:
    // 初期化（compute RootSig / 3 PSO 生成）。1回だけ呼ぶ。
    void Initialize(GraphicsDevice& device, const std::wstring& shaderDirW);

    // シェーダーホットリロード用。3 compute PSO のみ作り直す(ルートシグネチャ/リソースは不変のため触らない)。
    void RecreatePipelines(GraphicsDevice& device);

    // 環境キューブ(envCube, SRV は不要・リソースから内部で SRV を作る)から派生を生成。
    // cmdList は記録のみ（Close/Execute/WaitIdle は呼び出し側）。
    // envCube が null の場合は黒ダミーを生成（hasIBL=false 運用）。
    // SRV 連続3枚は srvHeap.AllocateBlock(3) で確保し、先頭=irradiance。
    void Bake(GraphicsDevice& device, ID3D12GraphicsCommandList* cmdList,
              DescriptorHeap& srvHeap, ID3D12Resource* envCube);

    bool  IsValid()            const { return m_valid; }
    bool  HasEnvironment()     const { return m_hasEnv; }
    u32   GetIrradianceSrv()   const { return m_blockStart; }  // ブロック先頭(=t5)
    float GetMaxPrefilterMip() const { return static_cast<float>(kPrefilterMips - 1); }

    // SRV ブロックの先頭/枚数（Application が Free する用）
    u32   GetSrvBlockStart()   const { return m_blockStart; }
    u32   GetSrvBlockCount()   const { return 3; }

    void  Reset();   // Shutdown で device 解放前に呼ぶ

private:
    static constexpr u32 kIrradianceSize = 32;
    static constexpr u32 kPrefilterSize  = 128;
    static constexpr u32 kPrefilterMips  = 5;
    static constexpr u32 kBrdfSize       = 512;

    void CreateDerivedResources(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoIrradiance;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPrefilter;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBrdf;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_irradianceCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_prefilteredCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_brdfLut;
    // constants 用 upload バッファ（Dispatch ごとに別オフセット）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cbUpload;

    // RecreatePipelines 用に保持
    std::wstring m_shaderDir;

    u32  m_blockStart = 0xFFFFFFFFu;  // srvHeap 上の連続3枚先頭(=irradiance)
    bool m_valid  = false;
    bool m_hasEnv = false;
    bool m_initialized = false;
};

} // namespace dx12e
