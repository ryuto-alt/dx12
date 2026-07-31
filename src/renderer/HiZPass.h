#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;

// 深度プリパスで完成したカメラ深度(R32_FLOAT SRV)から、階層深度(Hi-Z)ピラミッドを作る。
//
// 各テクセルは「その範囲で最も遠い面の深度」(max 縮約)。オクルージョンカリングは
// これを引いて「箱の最近点がタイルの最遠面よりさらに遠いか」を見る。
// 縮約が max なのはこのエンジンが標準 Z だから(根拠は renderer/HiZMath.h の先頭)。
//
// ★深度バッファ自体はミップを持てない(MipLevels=1 / UNORDERED_ACCESS フラグ無しで
//   作られている)ので、専用のテクスチャを別に持つ。mip0 は深度からの単純コピー。
//
// ★このエンジンの深度プリパスは前方パスとビット厳密に一致する(同じ m_drawItems /
//   同じジッタ付き camVPJ / 同じ LOD)。したがってプリパス完了後にここを走らせれば、
//   「今フレーム・今のカメラ」の完全な遮蔽情報が得られる。前フレームの深度を
//   再投影する必要も、2 フェーズ方式も要らない。
class HiZPass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);
    void Shutdown();

    // シェーダーホットリロード用。PSO のみ作り直す。
    void RecreatePipelines(GraphicsDevice& device);

    // レンダー解像度の変更に追従してピラミッドを作り直す。
    // ★これを忘れると古い寸法のまま引いて画面全体が壊れる。
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // 深度 SRV を読んでピラミッドを構築する。
    // 呼び出し側は事前に深度を SHADER_RESOURCE 系へ遷移しておくこと。
    // 戻り時、ピラミッドは NON_PIXEL|PIXEL_SHADER_RESOURCE 状態(=カリング compute から読める)。
    // ★compute は PSO/RootSignature を奪うので、呼び出し側でグラフィクス状態を戻すこと。
    void Build(ID3D12GraphicsCommandList* cmd, D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu);

    bool IsReady() const { return m_psoCopy != nullptr && m_psoReduce != nullptr && m_hzb != nullptr; }

    // ピラミッド全ミップを見る SRV のヒープインデックス。未準備なら 0xFFFFFFFFu。
    u32  GetSrvIndex()  const { return m_srvIndex; }
    u32  GetMipCount()  const { return static_cast<u32>(m_mipSizes.size()); }
    u32  GetWidth()     const { return m_width; }
    u32  GetHeight()    const { return m_height; }
    ID3D12Resource* GetResource() const { return m_hzb.Get(); }

private:
    void CreatePyramid(GraphicsDevice& device);
    void ReleaseDescriptors();

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCopy;     // 深度 → mip0
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoReduce;   // mip N-1 → mip N
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_hzb;

    struct MipSize { u32 w, h; };
    std::vector<MipSize> m_mipSizes;

    DescriptorHeap* m_srvHeap   = nullptr;
    // 連続ブロック: [0]=mip0 の UAV(複製) / [1+m]=mip m の UAV / 末尾=全ミップ SRV。
    // ディスパッチ m はテーブル基点を block+m に置くと u0=mip(m-1) / u1=mip m になる。
    u32 m_block     = 0xFFFFFFFFu;
    u32 m_blockSize = 0;
    u32 m_srvIndex  = 0xFFFFFFFFu;

    // ピラミッドの状態は自前で追跡する(全ミップ一括)。
    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    u32 m_width  = 0;
    u32 m_height = 0;
    std::wstring m_shaderDir;
};

} // namespace dx12e
