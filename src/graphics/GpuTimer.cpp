#include "graphics/GpuTimer.h"
#include "core/Logger.h"

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

bool GpuTimer::IsEffectiveLeaf(Scope s, const float* ms)
{
    if (s == Total || s >= Count) return false;
    if (IsLeaf(s)) return true;
    for (u32 c = 0; c < Count; ++c)
        if (c != s && kScopeInfo[c].parent == s && ms[c] > 0.0f) return false;   // 計測された子がある
    return true;   // 子が全部 0 ms（まだ計装されていない）→ 親自身を葉として数える
}

float GpuTimer::LeafSumMs(const float* ms)
{
    float sum = 0.0f;
    for (u32 i = 0; i < Count; ++i)
        if (IsEffectiveLeaf(static_cast<Scope>(i), ms)) sum += ms[i];
    return sum;
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
            for (u32 s = 0; s < Count; ++s)
            {
                if (!(mask & (1ull << s))) { m_ms[s] = 0.0f; continue; }
                const u64 b = ts[QueryIndex(m_frame, s, 0)];
                const u64 e = ts[QueryIndex(m_frame, s, 1)];
                m_ms[s] = (e > b)
                    ? static_cast<float>(static_cast<double>(e - b) * 1000.0 / static_cast<double>(m_freq))
                    : 0.0f;

                // 入れ子の検査: 子の [b, e] が親の [b, e] の中にあること（親も計測されたフレームだけ）。
                const u32 par = kScopeInfo[s].parent;
                if (s != Total && (mask & (1ull << par)))
                {
                    const u64 pb = ts[QueryIndex(m_frame, par, 0)];
                    const u64 pe = ts[QueryIndex(m_frame, par, 1)];
                    if (b < pb || e > pe)
                    {
                        ++m_nestViolations;
                        if (!(m_nestWarned & (1ull << s)))
                        {
                            m_nestWarned |= (1ull << s);
                            Logger::Warn("GpuTimer: スコープ {} の区間が親 {} の外に出ている", Name(s), Name(par));
                        }
                    }
                }
            }
            const D3D12_RANGE noWrite{ 0, 0 };
            m_readback->Unmap(0, &noWrite);

            // 葉の合計 ≤ Total + 0.1ms（葉は互いに重ならず、すべて Total の中に入るはず）。
            if (mask & 1ull)
            {
                const float leafSum = LeafSumMs(m_ms);
                if (leafSum > m_ms[Total] + kLeafSumToleranceMs)
                {
                    if (m_leafViolations++ < 3)
                        Logger::Warn("GpuTimer: 葉の合計 {:.2f}ms が Total {:.2f}ms + {:.1f}ms を超えた",
                                     leafSum, m_ms[Total], kLeafSumToleranceMs);
                }
            }
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
    m_writtenMask[m_frame] |= (1ull << s);
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
