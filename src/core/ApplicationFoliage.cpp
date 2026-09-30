// ===========================================================================
// 植生 F1: シーンとの接続（FoliageSystem の遅延確保 / フレーム開始 + GPU カリング / 描画の呼び出し / 統計）。
// ---------------------------------------------------------------------------
// 本体は renderer/foliage/FoliageSystem。ここは Application 側の糊だけ。
// ★FoliageLayer が 1 つも無いシーンでは m_foliage を作らない（GPU リソース 0・PSO 0）。描画パスの呼び出し点は
//   FoliageActive() で先に弾くので、未使用シーンの描画コマンドは 1 命令も変わらない（決定論スクショ差分 0）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "renderer/ViewDesc.h"
#include "renderer/foliage/FoliageSystem.h"

namespace dx12e
{

bool Application::FoliageActive() const
{
    if (!m_scene) return false;
    const entt::registry& reg = m_scene->GetRegistry();
    const auto* s = reg.storage<FoliageLayer>();
    return s && s->size() > 0;
}

void Application::ShutdownFoliage()
{
    if (m_foliage) m_foliage->Shutdown();
    m_foliage.reset();
}

bool Application::EnsureFoliageSystem()
{
    if (m_foliage) return m_foliage->IsReady();
    if (m_foliageUnavailable || !m_graphicsDevice || !m_rootSignature || !m_srvHeap || !m_resourceManager) return false;
    auto sys = std::make_unique<foliage::FoliageSystem>();
    foliage::FoliageSystem::Deps d;
    d.gdev = m_graphicsDevice.get();
    d.queue = m_commandQueue ? m_commandQueue->GetQueue() : nullptr;
    d.rootSig = m_rootSignature.get();
    d.srvHeap = m_srvHeap.get();
    d.resources = m_resourceManager.get();
    d.shaderDir = PathResolver::ShaderDirW();
    d.frameCount = FrameResources::kFrameCount;
    d.sceneColorFormat = appdetail::kSceneColorFormat;
    d.velocityFormat = TaaPass::kVelocityFormat;
    d.gbufferFormat = appdetail::kGBufferFormat;
    d.assetsDir = PathResolver::AssetsDir();
    std::string err;
    if (!sys->Initialize(d, &err))
    {
        Logger::Warn("植生: 初期化に失敗したので無効にする（{}）。FoliageLayer は描かれない", err);
        m_foliageUnavailable = true;
        return false;
    }
    m_foliage = std::move(sys);
    return true;
}

// RenderView の先頭（影パスより前）で呼ぶ。レイヤー収集 + GPU カリング（主ビュー + 影カスケード 0/1）。
void Application::FoliageBeginAndCull(ID3D12GraphicsCommandList* cmd, const ViewDesc& view, u32 frameIndex, f32 totalTime,
                                      bool shadowsOn)
{
    using namespace DirectX;
    if (!EnsureFoliageSystem()) return;

    // 時間: 総時間と前フレームの総時間（風の速度ベクトル用）。フレームで 1 回だけ進める。
    const u64 frameId = m_perfTotalFrames;
    if (m_foliageTimeFrame != frameId)
    {
        m_foliageTimePrev = m_foliageTimeValid ? m_foliageTimeCur : totalTime;
        m_foliageTimeCur = totalTime;
        m_foliageTimeValid = true;
        m_foliageTimeFrame = frameId;
    }

    foliage::FoliageSystem::FrameIn fi;
    entt::registry* reg = &m_scene->GetRegistry();
    fi.reg = reg;
    fi.wind = &m_scene->GetWind();
    fi.time = m_foliageTimeCur;
    fi.prevTime = m_deterministicCapture ? m_foliageTimeCur : m_foliageTimePrev;
    fi.physicalLightUnits = (m_lightingUnitsApplied == 1);
    fi.frameSlot = frameIndex % FrameResources::kFrameCount;
    fi.frameId = frameId;
    fi.cmd = cmd;
    fi.appHeap = m_srvHeap->GetHeap();
    fi.worldOf = [reg](entt::entity e)
    {
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, ComputeWorldMatrix(*reg, e));
        return f;
    };
    if (!m_foliage->BeginFrame(fi)) return;

    foliage::FoliageSystem::ViewIn vi;
    vi.camPos = view.position;
    vi.viewProj = view.viewProj;   // ジッタなし（視錐台は球の余裕で吸収される）
    if (view.primary && m_foliageHzbValid && m_hiZPass && m_hiZPass->IsReady()
        && m_hiZPass->GetWidth() == view.width && m_hiZPass->GetHeight() == view.height)
    {
        vi.hzb.resource = m_hiZPass->GetResource();
        vi.hzb.mips = m_hiZPass->GetMipCount();
        vi.hzb.width = static_cast<f32>(m_hiZPass->GetWidth());
        vi.hzb.height = static_cast<f32>(m_hiZPass->GetHeight());
        vi.hzb.prevVP = m_foliageHzbVP;
    }
    vi.shadows = shadowsOn;
    for (u32 c = 0; c < 2; ++c)
    {
        vi.cascade[c].viewProj = m_cascadeViewProj[c];
        vi.cascade[c].valid = shadowsOn;
    }
    m_foliage->Cull(cmd, vi, fi);
}

void Application::FoliageDrawMain(ID3D12GraphicsCommandList* cmd, const DirectX::XMMATRIX& viewProj, bool depthLEqual)
{
    if (!m_foliage) return;
    DirectX::XMFLOAT4X4 vp;
    DirectX::XMStoreFloat4x4(&vp, viewProj);
    m_foliage->DrawMain(cmd, vp, depthLEqual);
}

void Application::FoliageDrawDepth(ID3D12GraphicsCommandList* cmd, const DirectX::XMMATRIX& viewProjJ, bool velocity,
                                   const DirectX::XMFLOAT4X4& prevVP, const DirectX::XMFLOAT2& jitterNdc)
{
    if (!m_foliage) return;
    DirectX::XMFLOAT4X4 vp;
    DirectX::XMStoreFloat4x4(&vp, viewProjJ);
    m_foliage->DrawDepth(cmd, vp, velocity, prevVP, jitterNdc);
}

void Application::FoliageDrawShadow(ID3D12GraphicsCommandList* cmd, u32 cascade, const DirectX::XMFLOAT4X4& viewProj)
{
    if (m_foliage) m_foliage->DrawShadow(cmd, cascade, viewProj);
}

void Application::FoliageFillFrameConstants(DirectX::XMFLOAT4* reserved)
{
    if (!m_foliage || !FoliageActive()) return;
    m_foliage->WindConstants(reserved[0], reserved[1]);
}

nlohmann::json Application::FoliageStatsJson() const
{
    nlohmann::json j;
    j["active"] = FoliageActive();
    j["wind"] = {{"enabled", m_scene ? m_scene->GetWind().enabled : false},
                 {"speed", m_scene ? m_scene->GetWind().speed : 0.0f}};
    if (!m_foliage) { j["ready"] = false; return j; }
    nlohmann::json s = m_foliage->StatsJson();
    for (auto it = s.begin(); it != s.end(); ++it) j[it.key()] = it.value();
    j["hzbHistory"] = m_foliageHzbValid;
    return j;
}

} // namespace dx12e
