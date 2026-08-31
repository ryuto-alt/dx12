#pragma once

#include "graphics/GpuResource.h"

#include <wrl/client.h>

namespace dx12e
{

class GraphicsDevice;

class Texture : public GpuResource
{
public:
    void Initialize(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const D3D12_RESOURCE_DESC& desc,
        const D3D12_SUBRESOURCE_DATA* subresources,
        u32 subresourceCount);

    void CreateSRV(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
    // TextureCube SRV を張る（IBL 環境キューブ用）。mipLevels はキューブの mip 数。
    void CreateCubeSRV(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, u32 mipLevels);
    // Texture2DArray SRV を張る（地形レイヤー配列用）。スライス数は Initialize 時の
    // DepthOrArraySize をそのまま使う。★同じディスクリプタテーブルへ Texture2D と
    // Texture2DArray のどちらを張るかは自由（次元はルートシグネチャに書かれていない）。
    // ただしシェーダの宣言と必ず一致させること（地形は Terrain.hlsl が Texture2DArray）。
    void CreateArraySRV(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
    void FinishUpload();

    // 別インスタンスとして読み直したテクスチャの中身をこのオブジェクトへ引き取る。
    // ★SRV インデックスだけは自分のものを保つ ── Material の 3 連続 SRV ブロックや
    //   スプライト/UI が握っているのは「この Texture のアドレス」と「srvIndex」なので、
    //   実体を作り直しても両方が変わらなければ参照側は一切書き換えずに済む
    //   （ホットリロード = dx12_reload_assets の土台）。呼び出し側は引き取り後に
    //   CreateSRV() で自分の srvIndex へ SRV を張り直すこと（フォーマットが変わり得る）。
    //   旧リソースは GpuResource::ReleaseResource 経由で DeferredRelease に積まれる。
    void AdoptFrom(Texture& src);

    u32         GetWidth()     const { return m_width; }
    u32         GetHeight()    const { return m_height; }
    DXGI_FORMAT GetFormat()    const { return m_format; }
    u32         GetMipLevels() const { return m_mipLevels; }
    u32         GetArraySize() const { return m_arraySize; }
    u32         GetSrvIndex()  const { return m_srvIndex; }
    void        SetSrvIndex(u32 index) { m_srvIndex = index; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    u32         m_width    = 0;
    u32         m_height   = 0;
    u32         m_mipLevels = 1;
    u32         m_arraySize = 1;
    DXGI_FORMAT m_format   = DXGI_FORMAT_UNKNOWN;
    u32         m_srvIndex = UINT32_MAX;
};

} // namespace dx12e
