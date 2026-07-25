#include "renderer/AccelerationStructure.h"

#include "graphics/GraphicsDevice.h"
#include "core/Logger.h"

namespace dx12e
{

namespace
{
u64 AlignUp(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
}

bool RtBuffer::Initialize(GraphicsDevice& device, u64 sizeInBytes, Kind kind, const wchar_t* debugName)
{
    ReleaseResource();
    m_mapped = nullptr;
    m_size   = 0;

    if (sizeInBytes == 0) return false;

    u64 aligned = sizeInBytes;
    switch (kind)
    {
    case Kind::AccelStruct: aligned = AlignUp(sizeInBytes, kAsAlignment);           break;
    case Kind::Scratch:     aligned = AlignUp(sizeInBytes, kAsScratchAlignment);    break;
    case Kind::Upload:      aligned = AlignUp(sizeInBytes, kInstanceDescAlignment); break;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment          = 0;
    desc.Width              = aligned;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = (kind == Kind::Upload) ? D3D12_RESOURCE_FLAG_NONE
                                                     : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = (kind == Kind::Upload) ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    switch (kind)
    {
    case Kind::AccelStruct: initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE; break;
    case Kind::Scratch:     initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;                  break;
    case Kind::Upload:      initialState = D3D12_RESOURCE_STATE_GENERIC_READ;                      break;
    }

    // ★throw しない。VRAM 不足は「機能の不在」であってエラーではない（計画09 §5.3 Gate 6）。
    HRESULT hr = device.GetAllocator()->CreateResource(
        &allocDesc, &desc, initialState, nullptr, &m_allocation, IID_PPV_ARGS(&m_resource));
    if (FAILED(hr))
    {
        m_allocation = nullptr;
        m_resource.Reset();
        Logger::Warn("レイトレーシング用バッファの確保に失敗しました ({} bytes, hr=0x{:08X})",
                     aligned, static_cast<u32>(hr));
        return false;
    }

    m_currentState = initialState;
    m_size = aligned;
    if (debugName) m_resource->SetName(debugName);

    if (kind == Kind::Upload)
    {
        D3D12_RANGE readRange{0, 0};   // CPU からは読まない
        if (FAILED(m_resource->Map(0, &readRange, &m_mapped)))
        {
            m_mapped = nullptr;
            Release();
            return false;
        }
    }
    return true;
}

bool RtBuffer::EnsureCapacity(GraphicsDevice& device, u64 sizeInBytes, Kind kind, const wchar_t* debugName)
{
    if (m_resource && m_size >= sizeInBytes) return true;
    // 作り直しの頻度を下げるため 1.5 倍で伸ばす（AS のスクラッチは頂点数に比例して伸びる）。
    const u64 grow = (sizeInBytes * 3) / 2 + 256;
    return Initialize(device, grow, kind, debugName);
}

void RtUavBarrier(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;   // 全リソース対象
    cmd->ResourceBarrier(1, &b);
}

} // namespace dx12e
