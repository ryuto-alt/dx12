// ===========================================================================
// Application: 反射プローブ（ReflectionProbe。UE の Box Reflection Capture 相当）
//   ・台帳（ReflectionProbeSystem::Slot）をシーンの ReflectionProbe に追従させる
//   ・焼き待ちのプローブを、主ビューの後に 6 面撮影 → GGX 畳み込み
//   ・フォワードへ渡す b1 の予約（FillSceneFrameConstants から呼ぶ）
// ★ReflectionProbe が無いシーンでは台帳が空のまま ＝ 描画コマンドは 1 命令も増えない。
// ★毎フレームは描かない: シーンの読み込み後（DDGI が落ち着くのを待つ）/ プローブが動いた後 / 明示操作だけ。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "renderer/ViewDesc.h"

#include "ecs/EditorFlags.h"

#include <algorithm>

namespace dx12e
{
using namespace appdetail;

namespace
{
constexpr u32 kAutoCaptureDelayFrames = 90;    // 新顔 / シーン読み込み後に焼くまでの待ち（DDGI の収束待ち）
constexpr u32 kMovedCaptureDelayFrames = 45;   // 動かした後に焼き直すまでの待ち（動かしている間は待ち続ける）

}

void Application::InitReflectionProbes()
{
    if (m_reflProbes || !m_graphicsDevice || !m_clusteredLighting || !m_clusteredLighting->IsReady()) return;
    try
    {
        auto sys = std::make_unique<ReflectionProbeSystem>();
        // 面ごとの b1 は FrameConstants（1600B）と同じ大きさ（ApplicationRender.cpp の static_assert と一致）
        sys->Initialize(*m_graphicsDevice, m_srvHeap.get(), m_offscreenRtvHeap.get(), m_dsvHeap.get(),
                        PathResolver::ShaderDirW(), 1600);
        for (u32 f = 0; f < ClusteredLightCulling::kFrameCount; ++f)
            sys->WriteTableDummy(*m_graphicsDevice, *m_srvHeap, m_clusteredLighting->GetSrvTableIndex(f), f);
        m_reflProbes = std::move(sys);
    }
    catch (const std::exception& e)
    {
        Logger::Error("ReflectionProbeSystem の初期化に失敗しました（反射プローブは使えません）: {}", e.what());
    }
}

void Application::ShutdownReflectionProbes()
{
    if (m_reflProbes) m_reflProbes->Shutdown();
    m_reflProbes.reset();
}

// ---------------------------------------------------------------------------
// 台帳の同期（フレームに 1 回。FillReflectionProbeFrame から）
// ---------------------------------------------------------------------------
void Application::SyncReflectionProbes()
{
    using namespace DirectX;
    if (!m_reflProbes || !m_scene) return;
    auto& reg = m_scene->GetRegistry();
    auto* slots = m_reflProbes->Slots();
    bool seen[ReflectionProbeSystem::kMaxProbes] = {};

    for (auto [e, rp, tf] : reg.view<ReflectionProbe, Transform>().each())
    {
        int s = m_reflProbes->FindSlot(e);
        if (s < 0)
        {
            s = m_reflProbes->AcquireSlot(e);
            if (s < 0)
            {
                static bool warned = false;
                if (!warned) { warned = true; Logger::Warn("反射プローブが上限（{} 個）を超えました。超えた分は無視します", ReflectionProbeSystem::kMaxProbes); }
                continue;
            }
            slots[s].pending = true;
            slots[s].wait = kAutoCaptureDelayFrames;
        }
        seen[s] = true;
        auto& sl = slots[s];
        const XMMATRIX w = (tf.parent != entt::null) ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
        XMStoreFloat3(&sl.pos, w.r[3]);
        sl.cfg = rp;
        sl.disabled = eflags::IsDisabled(reg, e);

        // 焼いた位置から動いたら、動きが止まってから焼き直す（動かしている間は待ち続ける）
        if (sl.captured)
        {
            const f32 dx = sl.pos.x - sl.capturedPos.x, dy = sl.pos.y - sl.capturedPos.y, dz = sl.pos.z - sl.capturedPos.z;
            if (dx * dx + dy * dy + dz * dz > 0.25f * 0.25f)
            {
                sl.pending = true;
                sl.wait = kMovedCaptureDelayFrames;
                sl.capturedPos = sl.pos;   // 次の判定は新しい位置から（連続で動かしても 1 回の待ちに畳む）
                sl.captured = true;        // 焼き直しが済むまでは古い絵を使い続ける
            }
        }
        if (sl.pending && sl.wait > 0) --sl.wait;
    }
    for (u32 i = 0; i < ReflectionProbeSystem::kMaxProbes; ++i)
        if (slots[i].used && !seen[i]) slots[i] = ReflectionProbeSystem::Slot{};
}

// ---------------------------------------------------------------------------
// b1 の予約 16B と、フォワードが読むプローブ情報（t33 / t34）
// ---------------------------------------------------------------------------
void Application::FillReflectionProbeFrame(DirectX::XMFLOAT4* out, u32 frameIndex)
{
    using namespace DirectX;
    const bool mb = m_scene && m_scene->GetSSAOSettings().multiBounce;
    *out = XMFLOAT4(0.0f, 0.0f, mb ? 1.0f : 0.0f, 0.0f);
    if (!m_reflProbes || !m_reflProbes->IsReady()) return;
    SyncReflectionProbes();

    const auto* slots = m_reflProbes->Slots();
    struct Entry { ReflectionProbeSystem::GpuProbe g; int priority; f32 volume; };
    std::vector<Entry> list;
    list.reserve(ReflectionProbeSystem::kMaxProbes);
    for (u32 i = 0; i < ReflectionProbeSystem::kMaxProbes; ++i)
    {
        const auto& s = slots[i];
        if (!s.used || !s.captured || s.disabled) continue;
        Entry e{};
        e.g.posBlend     = XMFLOAT4(s.pos.x, s.pos.y, s.pos.z, (std::max)(s.cfg.blendDistance, 0.0f));
        e.g.extIntensity = XMFLOAT4((std::max)(s.cfg.halfExtents.x, 0.05f), (std::max)(s.cfg.halfExtents.y, 0.05f),
                                    (std::max)(s.cfg.halfExtents.z, 0.05f), (std::max)(s.cfg.intensity, 0.0f));
        e.g.misc         = XMFLOAT4(s.cfg.boxProjection ? 1.0f : 0.0f, static_cast<f32>(i), 0.0f, 0.0f);
        e.priority = s.cfg.priority;
        e.volume = e.g.extIntensity.x * e.g.extIntensity.y * e.g.extIntensity.z;
        list.push_back(e);
    }
    // 優先度の高い順 → 体積の小さい順（小さい部屋のプローブが先に重みを取る）
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.volume < b.volume;
    });
    std::vector<ReflectionProbeSystem::GpuProbe> gpu;
    gpu.reserve(list.size());
    for (const auto& e : list) gpu.push_back(e.g);

    if (m_clusteredLighting && m_clusteredLighting->IsReady())
        m_reflProbes->WriteFrame(*m_graphicsDevice, *m_srvHeap, m_clusteredLighting->GetSrvTableIndex(frameIndex),
                                 frameIndex, gpu.data(), static_cast<u32>(gpu.size()));
    out->x = static_cast<f32>(gpu.size());
    out->y = static_cast<f32>(ReflectionProbeSystem::kFilteredMips - 1);
}

// ---------------------------------------------------------------------------
// 要求（MCP / Inspector）。e = null で全部。戻り値 = 焼き待ちにした数
// ---------------------------------------------------------------------------
int Application::RequestReflectionProbeCapture(entt::entity e)
{
    if (!m_reflProbes || !m_scene) return 0;
    auto& reg = m_scene->GetRegistry();
    auto* slots = m_reflProbes->Slots();
    int n = 0;
    auto mark = [&](entt::entity ent)
    {
        if (!reg.valid(ent) || !reg.all_of<ReflectionProbe, Transform>(ent)) return;
        int s = m_reflProbes->AcquireSlot(ent);
        if (s < 0) return;
        slots[s].pending = true;
        slots[s].wait = 0;
        ++n;
    };
    if (e != entt::null) mark(e);
    else for (auto ent : reg.view<ReflectionProbe>()) mark(ent);
    return n;
}

nlohmann::json Application::ReflectionProbeStatsJson() const
{
    using nlohmann::json;
    json j;
    j["ready"] = m_reflProbes && m_reflProbes->IsReady();
    j["maxProbes"] = ReflectionProbeSystem::kMaxProbes;
    j["captureSize"] = ReflectionProbeSystem::kCaptureSize;
    j["filteredSize"] = ReflectionProbeSystem::kFilteredSize;
    j["filteredMips"] = ReflectionProbeSystem::kFilteredMips;
    j["multiBounce"] = m_scene ? m_scene->GetSSAOSettings().multiBounce : false;
    json arr = json::array();
    u32 captured = 0;
    if (m_reflProbes)
    {
        j["gpuMB"] = static_cast<double>(m_reflProbes->GpuBytes()) / (1024.0 * 1024.0);
        const double now = ReflectionProbeSystem::NowSec();
        for (u32 i = 0; i < ReflectionProbeSystem::kMaxProbes; ++i)
        {
            const auto& s = m_reflProbes->Slots()[i];
            if (!s.used) continue;
            if (s.captured) ++captured;
            json p;
            p["slot"] = i;
            p["entity"] = static_cast<u32>(s.entity);
            if (m_scene && m_scene->GetRegistry().valid(s.entity) && m_scene->GetRegistry().all_of<NameTag>(s.entity))
                p["name"] = m_scene->GetRegistry().get<NameTag>(s.entity).name;
            p["captured"] = s.captured;
            p["pending"] = s.pending;
            p["waitFrames"] = s.wait;
            p["captureCount"] = s.captureCount;
            p["ageSec"] = s.captured ? now - s.capturedAtSec : -1.0;
            p["position"] = {s.pos.x, s.pos.y, s.pos.z};
            p["halfExtents"] = {s.cfg.halfExtents.x, s.cfg.halfExtents.y, s.cfg.halfExtents.z};
            p["blendDistance"] = s.cfg.blendDistance;
            p["boxProjection"] = s.cfg.boxProjection;
            p["intensity"] = s.cfg.intensity;
            p["priority"] = s.cfg.priority;
            p["disabled"] = s.disabled;
            arr.push_back(p);
        }
    }
    j["count"] = arr.size();
    j["captured"] = captured;
    j["probes"] = arr;
    return j;
}

} // namespace dx12e
