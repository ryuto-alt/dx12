// ===========================================================================
// 水面 W1: シーンとの接続（WaterRenderer の遅延確保 / フレーム開始 / 統計）。
// ---------------------------------------------------------------------------
// 本体は renderer/water/WaterRenderer（専用ルートシグネチャ・専用パス）。ここは Application 側の糊だけ。
// ★WaterBody が 1 つも無いシーンでは m_water を作らない（GPU リソース 0・PSO 0）。描画パスの呼び出し点は
//   WaterActive() / waterPass で先に弾くので、未使用シーンの描画コマンドは 1 命令も変わらない（決定論スクショ差分 0）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "renderer/ViewDesc.h"
#include "renderer/water/WaterRenderer.h"

namespace dx12e
{

bool Application::WaterActive() const
{
    if (!m_scene) return false;
    const entt::registry& reg = m_scene->GetRegistry();
    const auto* s = reg.storage<WaterBody>();
    return s && s->size() > 0;
}

void Application::ShutdownWater()
{
    if (m_water) m_water->Shutdown();
    m_water.reset();
}

bool Application::EnsureWaterRenderer()
{
    if (m_water) return m_water->IsReady();
    if (m_waterUnavailable || !m_graphicsDevice || !m_srvHeap) return false;
    auto r = std::make_unique<water::WaterRenderer>();
    water::WaterRenderer::Deps d;
    d.gdev = m_graphicsDevice.get();
    d.queue = m_commandQueue ? m_commandQueue->GetQueue() : nullptr;
    d.srvHeap = m_srvHeap.get();
    d.shaderDir = PathResolver::ShaderDirW();
    d.frameCount = FrameResources::kFrameCount;
    d.sceneColorFormat = appdetail::kSceneColorFormat;
    std::string err;
    m_waterPhysApplied = (m_lightingUnitsApplied == 1);
    if (!r->Initialize(d, &err) || !r->RecreatePipelines(m_waterPhysApplied))
    {
        Logger::Warn("水: 初期化に失敗したので無効にする（{}）。WaterBody は描かれない", err);
        m_waterUnavailable = true;
        return false;
    }
    m_water = std::move(r);
    return true;
}

// RenderView の主ビュー、Forward+ の前で呼ぶ。収集 + カリング + パラメータ詰め（フレームで 1 回）。
bool Application::WaterBeginFrame(ID3D12GraphicsCommandList* cmd, const ViewDesc& view, u32 frameIndex, f32 totalTime)
{
    using namespace DirectX;
    if (!EnsureWaterRenderer()) return false;

    const bool phys = (m_lightingUnitsApplied == 1);
    if (phys != m_waterPhysApplied)
    {
        m_water->RecreatePipelines(phys);   // PS だけ作り直す（UpdateLightingUnits は GPU を待った後に呼ばれるので安全）
        m_waterPhysApplied = phys;
    }
    const u32 slot = frameIndex % FrameResources::kFrameCount;
    m_water->BeginFrameTimers(slot);

    water::FrameIn fi;
    entt::registry& reg = m_scene->GetRegistry();
    for (auto [e, wb] : reg.view<WaterBody>().each())
    {
        if (!wb.enabled) continue;
        water::BodyIn b;
        b.entity = e;
        b.body = wb;
        XMStoreFloat4x4(&b.world, ComputeWorldMatrix(reg, e));
        fi.bodies.push_back(std::move(b));
    }
    fi.totalTime = totalTime;
    fi.frameSlot = slot;
    fi.frameId = ++m_waterFrameCounter;
    fi.viewProj = view.viewProj;
    fi.viewProjJ = view.viewProjJittered;
    fi.proj = view.proj;
    fi.camPos = view.position;
    fi.nearZ = view.nearZ;
    fi.farZ = view.farZ;
    fi.width = view.width;
    fi.height = view.height;
    fi.physicalLights = phys;
    fi.cmd = cmd;
    return m_water->BeginFrame(fi) > 0;
}

nlohmann::json Application::WaterStatsJson() const
{
    nlohmann::json j;
    j["active"] = WaterActive();
    if (!m_water) { j["ready"] = false; return j; }
    const water::WaterStats& s = m_water->Stats();
    j["ready"] = m_water->IsReady();
    j["bodies"] = s.bodies;
    j["rings"] = s.rings;
    j["triangles"] = s.triangles;
    j["culledBodies"] = s.culledBodies;
    j["underwater"] = s.underwater;
    j["gpuMs"] = {{"total", s.gpuMsTotal}, {"copy", s.gpuMsCopy}, {"underwater", s.gpuMsUnder}, {"surface", s.gpuMsDraw}};
    j["gpuMB"] = static_cast<double>(s.gpuBytes) / (1024.0 * 1024.0);
    return j;
}

} // namespace dx12e
