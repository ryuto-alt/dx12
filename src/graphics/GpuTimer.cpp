#include "graphics/GpuTimer.h"
#include "core/Logger.h"

#include <cstring>

namespace dx12e
{

namespace
{
using S = GpuTimer;

// スコープの属性表（Scope の番号順。末尾の static_assert が数を照合する）。
// parent は実コードの Begin/End の入れ子から決めた: Shadows の中に VgShadow、PrepassSSAO（プリパス一式）の中に
// DepthPrepass / HiZ / VgCull / RtScreen / ScreenSpaceGI / CloudShadow、RtScreen の中に RtShadowDenoise、
// Particles の中に Clouds、PostFX の中に Upscale（RenderPostChain は PostFX の区間内で呼ばれる）。
// 新しい 30 本は、これから計装する位置に合わせた親を付けてある（未計装の間は 0 ms を返す）。
const S::ScopeInfo kScopeInfo[] = {
    { "total",           S::Total,         nullptr },
    { "shadows",         S::Total,         nullptr },
    { "depthPrepass",    S::PrepassSSAO,   nullptr },
    { "prepassSsao",     S::Total,         nullptr },
    { "clusterCull",     S::Total,         nullptr },
    { "raytracing",      S::Total,         "asBuild" },
    { "rtScreen",        S::PrepassSSAO,   nullptr },
    { "rtReflection",    S::Total,         "rtReflect" },
    { "ddgi",            S::Total,         nullptr },
    { "screenSpaceGi",   S::PrepassSSAO,   nullptr },
    { "volFog",          S::Total,         "fogScatter" },
    { "hiZ",             S::PrepassSSAO,   nullptr },
    { "mainScene",       S::Total,         nullptr },
    { "particles",       S::Total,         nullptr },
    { "postFx",          S::Total,         nullptr },
    { "ui",              S::Total,         nullptr },
    { "vgCull",          S::PrepassSSAO,   nullptr },
    { "clouds",          S::Particles,     nullptr },
    { "cloudShadow",     S::PrepassSSAO,   nullptr },
    { "rtShadowDenoise", S::RtScreen,      nullptr },
    { "upscale",         S::PostFX,        nullptr },
    { "vgShadow",        S::Shadows,       nullptr },
    // ---- W05-26: 新しい 30 本 ----
    { "shadowCsm",       S::Shadows,       nullptr },
    { "shadowLocal",     S::Shadows,       nullptr },
    { "screenShadow",    S::PrepassSSAO,   nullptr },
    { "rtShadow",        S::RtScreen,      nullptr },
    { "rtLocalShadow",   S::RtScreen,      nullptr },
    { "ssLighting",      S::PrepassSSAO,   nullptr },
    { "skinning",        S::Total,         nullptr },
    { "characters",      S::Total,         nullptr },
    { "transparent",     S::Total,         nullptr },
    { "water",           S::Total,         nullptr },
    { "foliage",         S::Total,         nullptr },
    { "atmosphere",      S::Total,         nullptr },
    { "godRays",         S::PostFX,        nullptr },
    { "decals",          S::Total,         nullptr },
    { "vfxSim",          S::Total,         nullptr },
    { "vfxDraw",         S::Total,         nullptr },
    { "vfxLighting",     S::Total,         nullptr },
    { "vfxShadow",       S::Total,         nullptr },
    { "reactive",        S::PostFX,        nullptr },
    { "taa",             S::PostFX,        nullptr },
    { "dof",             S::PostFX,        nullptr },
    { "motionBlur",      S::PostFX,        nullptr },
    { "autoExposure",    S::PostFX,        nullptr },
    { "localExposure",   S::PostFX,        nullptr },
    { "bloom",           S::PostFX,        nullptr },
    { "lensFx",          S::PostFX,        nullptr },
    { "colorLut",        S::PostFX,        nullptr },
    { "uber",            S::PostFX,        nullptr },
    { "editorOverlay",   S::UI,            nullptr },
    { "gameHud",         S::UI,            nullptr },
};
static_assert(sizeof(kScopeInfo) / sizeof(kScopeInfo[0]) == GpuTimer::Count,
              "kScopeInfo の行数が GpuTimer::Count と合わない（スコープを足したら表にも足すこと）");

bool EqualsNoCase(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
    {
        const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) return false;
    }
    return *a == *b;
}
} // namespace

const GpuTimer::ScopeInfo& GpuTimer::Info(Scope s)
{
    return kScopeInfo[(s < Count) ? s : Total];
}

bool GpuTimer::IsLeaf(Scope s)
{
    if (s >= Count) return false;
    for (u32 c = 0; c < Count; ++c)
        if (c != s && kScopeInfo[c].parent == s) return false;   // 子が 1 つでもあれば親
    return true;
}

bool GpuTimer::FindScope(const char* nameOrAlias, Scope& out)
{
    if (!nameOrAlias) return false;
    for (u32 i = 0; i < Count; ++i)
    {
        if (EqualsNoCase(nameOrAlias, kScopeInfo[i].name) ||
            (kScopeInfo[i].alias && EqualsNoCase(nameOrAlias, kScopeInfo[i].alias)))
        {
            out = static_cast<Scope>(i);
            return true;
        }
    }
    return false;
}

u64 GpuTimer::ExpandExcludeMask(u64 excludeMask)
{
    excludeMask &= ~1ull;   // Total は除外しない
    u64 out = excludeMask;
    for (u32 i = 1; i < Count; ++i)
    {
        u32 cur = kScopeInfo[i].parent, hops = 0;
        while (cur != Total && hops++ < Count)
        {
            if (excludeMask & (1ull << cur)) { out |= (1ull << i); break; }
            cur = kScopeInfo[cur].parent;
        }
    }
    return out;
}

float GpuTimer::LeafMs(Scope s, const float* ms, u64 expandedExclude)
{
    if (s >= Count || (expandedExclude & (1ull << s))) return 0.0f;
    if (IsLeaf(s)) return ms[s];
    float childSum = 0.0f;
    for (u32 c = 0; c < Count; ++c)
        if (c != s && kScopeInfo[c].parent == s && !(expandedExclude & (1ull << c))) childSum += ms[c];
    const float self = ms[s] - childSum;
    return self > 0.0f ? self : 0.0f;   // 親の作業時間 = 親 − Σ子（下限 0）
}

float GpuTimer::LeafSumMs(const float* ms, u64 excludeMask)
{
    const u64 ex = ExpandExcludeMask(excludeMask);
    float sum = 0.0f;
    for (u32 i = 0; i < Count; ++i) sum += LeafMs(static_cast<Scope>(i), ms, ex);
    return sum;
}

u32 GpuTimer::LeafEntries(const float* ms, u64 excludeMask, LeafEntry* out)
{
    const u64 ex = ExpandExcludeMask(excludeMask);
    u32 n = 0;
    for (u32 i = 0; i < Count; ++i)
    {
        if (ex & (1ull << i)) continue;
        const Scope sc = static_cast<Scope>(i);
        out[n++] = LeafEntry{ sc, !IsLeaf(sc), LeafMs(sc, ms, ex) };
    }
    return n;
}

GpuTimer::FrameCheck GpuTimer::CheckFrame(const u64* beginTs, const u64* endTs, u64 writtenMask, u64 multiMask,
                                          double freq, float* msOut)
{
    FrameCheck r;
    for (u32 s = 0; s < Count; ++s)
    {
        if (!(writtenMask & (1ull << s))) { msOut[s] = 0.0f; continue; }
        const u64 b = beginTs[s], e = endTs[s];
        msOut[s] = (e > b && freq > 0.0) ? static_cast<float>(static_cast<double>(e - b) * 1000.0 / freq) : 0.0f;
    }
    // 入れ子の検査: 子の [b, e] が親の [b, e] の中にあること。親子とも書かれたフレームだけ、
    // 複数ビューで書かれたスコープ（区間が最後のビューのもので上書きされる）は検査しない。
    const u64 ex = ExpandExcludeMask(multiMask);
    for (u32 s = 1; s < Count; ++s)
    {
        if (!(writtenMask & (1ull << s)) || (ex & (1ull << s))) continue;
        const u32 par = kScopeInfo[s].parent;
        if (!(writtenMask & (1ull << par)) || (ex & (1ull << par))) continue;
        if (beginTs[s] < beginTs[par] || endTs[s] > endTs[par])
        {
            ++r.nestViolations;
            r.nestViolatedMask |= (1ull << s);
        }
    }
    // 葉の合計 ≤ Total + 0.1ms（葉は互いに重ならず、すべて Total の中に入るはず）。
    if (writtenMask & 1ull)
    {
        r.totalMs = msOut[Total];
        r.leafSumMs = LeafSumMs(msOut, multiMask);
        r.leafSumExceeded = r.leafSumMs > r.totalMs + kLeafSumToleranceMs;
    }
    return r;
}

const char* GpuTimer::Name(u32 s)
{
    // 旧 22 本はここに残す（tools/mcp-server/schemaDrift.test.ts が case 句の戻り値の文字列を読んで
    // dx12_perf_stats の説明と照合する）。新しい 30 本は属性表から引く。
    switch (s)
    {
    case Total:       return "total";
    case Shadows:     return "shadows";
    case DepthPrepass: return "depthPrepass";
    case PrepassSSAO: return "prepassSsao";
    case ClusterCull: return "clusterCull";
    case Raytracing:  return "raytracing";
    case RtScreen:    return "rtScreen";
    case RtReflection: return "rtReflection";
    case Ddgi:        return "ddgi";
    case ScreenSpaceGI: return "screenSpaceGi";
    case VolumetricFog: return "volFog";
    case HiZ:         return "hiZ";
    case MainScene:   return "mainScene";
    case Particles:   return "particles";
    case PostFX:      return "postFx";
    case UI:          return "ui";
    case VgCull:      return "vgCull";
    case Clouds:      return "clouds";
    case CloudShadow: return "cloudShadow";
    case RtShadowDenoise: return "rtShadowDenoise";
    case Upscale:     return "upscale";
    case VgShadow:    return "vgShadow";
    default:          return (s < Count) ? kScopeInfo[s].name : "?";
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
    const u64 mask = m_writtenMask[m_frame];
    if (mask)
    {
        const D3D12_RANGE readRange{
            static_cast<size_t>(QueryIndex(m_frame, 0, 0)) * sizeof(u64),
            static_cast<size_t>(QueryIndex(m_frame, Count - 1, 1) + 1) * sizeof(u64) };
        void* p = nullptr;
        if (SUCCEEDED(m_readback->Map(0, &readRange, &p)) && p)
        {
            const u64* ts = static_cast<const u64*>(p);
            u64 bts[Count] = {}, ets[Count] = {};
            for (u32 s = 0; s < Count; ++s)
            {
                if (!(mask & (1ull << s))) continue;
                bts[s] = ts[QueryIndex(m_frame, s, 0)];
                ets[s] = ts[QueryIndex(m_frame, s, 1)];
            }
            const D3D12_RANGE noWrite{ 0, 0 };
            m_readback->Unmap(0, &noWrite);

            const u64 multi = m_multiMask[m_frame];
            m_multiLast = multi;
            const FrameCheck chk = CheckFrame(bts, ets, mask, multi, static_cast<double>(m_freq), m_ms);

            if (chk.nestViolations)
            {
                m_nestViolations += chk.nestViolations;
                for (u32 s = 1; s < Count; ++s)
                {
                    const u64 bit = 1ull << s;
                    if ((chk.nestViolatedMask & bit) && !(m_nestWarned & bit))
                    {
                        m_nestWarned |= bit;
                        Logger::Warn("GpuTimer: スコープ {} の区間が親 {} の外に出ている", Name(s), Name(kScopeInfo[s].parent));
                    }
                }
            }
            if (chk.leafSumExceeded && m_leafViolations++ < 3)
                Logger::Warn("GpuTimer: 葉の合計 {:.2f}ms が Total {:.2f}ms + {:.1f}ms を超えた",
                             chk.leafSumMs, chk.totalMs, kLeafSumToleranceMs);
        }
    }
    m_writtenMask[m_frame] = 0;
    m_beganMask[m_frame] = 0;
    m_multiMask[m_frame] = 0;
}

void GpuTimer::Begin(ID3D12GraphicsCommandList* cmd, Scope s)
{
    if (!m_valid || s >= Count) return;
    // 同一フレームの 2 回目以降の Begin（複数ビュー）は、クエリ枠を上書きするので印を付ける。
    if (m_beganMask[m_frame] & (1ull << s)) m_multiMask[m_frame] |= (1ull << s);
    m_beganMask[m_frame] |= (1ull << s);
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_frame, s, 0));
}

void GpuTimer::End(ID3D12GraphicsCommandList* cmd, Scope s)
{
    if (!m_valid || s >= Count) return;
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_frame, s, 1));
    m_writtenMask[m_frame] |= (1ull << s);
}

void GpuTimer::RecordHistory()
{
    std::memcpy(m_hist[m_histCount % kHistory], m_ms, sizeof(m_ms));
    ++m_histCount;
}

u32 GpuTimer::AverageHistory(u32 window, float* out) const
{
    const u32 n = (window < HistoryCount()) ? window : HistoryCount();
    for (u32 s = 0; s < Count; ++s) out[s] = 0.0f;
    if (n == 0) return 0;
    for (u32 i = 0; i < n; ++i)
    {
        const float* f = m_hist[(m_histCount - 1 - i) % kHistory];
        for (u32 s = 0; s < Count; ++s) out[s] += f[s];
    }
    const float inv = 1.0f / static_cast<float>(n);
    for (u32 s = 0; s < Count; ++s) out[s] *= inv;
    return n;
}

void GpuTimer::BenchReset()
{
    for (u32 s = 0; s < Count; ++s) m_benchSum[s] = 0.0;
    m_benchN = 0;
}

void GpuTimer::BenchAdd()
{
    for (u32 s = 0; s < Count; ++s) m_benchSum[s] += m_ms[s];
    ++m_benchN;
}

u32 GpuTimer::BenchAverage(float* out) const
{
    for (u32 s = 0; s < Count; ++s)
        out[s] = m_benchN ? static_cast<float>(m_benchSum[s] / static_cast<double>(m_benchN)) : 0.0f;
    return m_benchN;
}

void GpuTimer::Resolve(ID3D12GraphicsCommandList* cmd)
{
    if (!m_valid) return;
    const u64 mask = m_writtenMask[m_frame];
    for (u32 s = 0; s < Count; ++s)
    {
        if (!(mask & (1ull << s))) continue;   // 未使用スコープの Resolve は打ってないクエリを読むので避ける
        const u32 idx = QueryIndex(m_frame, s, 0);
        cmd->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx, 2,
                              m_readback.Get(), static_cast<u64>(idx) * sizeof(u64));
    }
}

} // namespace dx12e
