#include "graphics/GpuTimer.h"
#include "core/Logger.h"

namespace dx12e
{

const char* GpuTimer::Name(u32 s)
{
    switch (s)
    {
    case Total:       return "total";
    case Shadows:     return "shadows";
    case DepthPrepass: return "depthPrepass";
    case PrepassSSAO: return "prepassSsao";
    case ClusterCull: return "clusterCull";
    case Raytracing:  return "raytracing";
    case RtScreen:    return "rtScreen";
    case Ddgi:        return "ddgi";
    case ScreenSpaceGI: return "screenSpaceGi";
    case VolumetricFog: return "volFog";
    case MainScene:   return "mainScene";
    case Particles:   return "particles";
    case PostFX:      return "postFx";
    case UI:          return "ui";
    default:          return "?";
    }
}

void GpuTimer::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    if (!device || !queue) return;
    if (FAILED(queue->GetTimestampFrequency(&m_freq)) || m_freq == 0)
    {
        Logger::Warn("GpuTimer: タイムスタンプ周波数が取得できないため GPU 計測は無効");
        return;
    }

    const u32 numQueries = kFrames * Count * 2;

    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = numQueries;
    if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_heap))))
    {
        Logger::Warn("GpuTimer: QueryHeap 作成に失敗。GPU 計測は無効");
        return;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = static_cast<u64>(numQueries) * sizeof(u64);
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback))))
    {
        Logger::Warn("GpuTimer: readback バッファ作成に失敗。GPU 計測は無効");
        m_heap.Reset();
        return;
    }
    m_valid = true;
}

void GpuTimer::NewFrame(u32 frameIndex)
{
    if (!m_valid) return;
    m_frame = frameIndex % kFrames;

    // このスロットは kFrames 周前に Resolve 済みかつフェンス完了済み → 安全に読める。
    const u32 mask = m_writtenMask[m_frame];
    if (mask)
    {
        const D3D12_RANGE readRange{
            static_cast<size_t>(QueryIndex(m_frame, 0, 0)) * sizeof(u64),
            static_cast<size_t>(QueryIndex(m_frame, Count - 1, 1) + 1) * sizeof(u64) };
        void* p = nullptr;
        if (SUCCEEDED(m_readback->Map(0, &readRange, &p)) && p)
        {
            const u64* ts = static_cast<const u64*>(p);
            for (u32 s = 0; s < Count; ++s)
            {
                if (!(mask & (1u << s))) { m_ms[s] = 0.0f; continue; }
                const u64 b = ts[QueryIndex(m_frame, s, 0)];
                const u64 e = ts[QueryIndex(m_frame, s, 1)];
                m_ms[s] = (e > b)
                    ? static_cast<float>(static_cast<double>(e - b) * 1000.0 / static_cast<double>(m_freq))
                    : 0.0f;
            }
            const D3D12_RANGE noWrite{ 0, 0 };
            m_readback->Unmap(0, &noWrite);
        }
    }
    m_writtenMask[m_frame] = 0;
}

void GpuTimer::Begin(ID3D12GraphicsCommandList* cmd, Scope s)
{
    if (!m_valid || s >= Count) return;
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_frame, s, 0));
}

void GpuTimer::End(ID3D12GraphicsCommandList* cmd, Scope s)
{
    if (!m_valid || s >= Count) return;
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_frame, s, 1));
    m_writtenMask[m_frame] |= (1u << s);
}

void GpuTimer::Resolve(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid) return;
    const u32 mask = m_writtenMask[m_frame];
    for (u32 s = 0; s < Count; ++s)
    {
        if (!(mask & (1u << s))) continue;   // 未使用スコープの Resolve は打ってないクエリを読むので避ける
        const u32 idx = QueryIndex(m_frame, s, 0);
        cmd->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx, 2,
                              m_readback.Get(), static_cast<u64>(idx) * sizeof(u64));
    }
}

} // namespace dx12e
