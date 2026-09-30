#pragma once
// ===========================================================================
// AtmospherePasses — 物理大気(A1)の空パスと AP 合成を IRenderPass（renderer/RenderPass.h の状態の契約）として包む。
// ヘッダオンリー。GPU 資源は持たない（AtmosphereRenderer を借りる）。ViewPasses.h とは別ファイル（別担当の編集と衝突しないため）。
// ===========================================================================
#include "graphics/CommandList.h"
#include "graphics/DescriptorHeap.h"
#include "renderer/RenderPass.h"
#include "renderer/atmosphere/AtmosphereRenderer.h"

namespace dx12e
{

// 空。不透明描画の前・深度テスト OFF・深度は 1.0 のまま残る（後段のフォグ合成 / AP が空画素を判別できる）。
class AtmosphereSkyPass final : public IRenderPass
{
public:
    struct Inputs
    {
        AtmosphereRenderer*         atmo = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
        ID3D12Resource*             sceneColor = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};   // 空の PS は読まない（ルートの穴埋め）
        u32 width = 0, height = 0;
        u32 cbSlot = 0;                            // 主ビュー = frameIndex / 副ビュー = frameIndex + 3
    };
    explicit AtmosphereSkyPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "AtmosphereSky"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override
    {
        out.push_back({"sceneColor", m_in.sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET, PassAccess::Write});
    }
    void Execute(const RenderPassContext& ctx) override
    {
        if (!m_in.atmo) return;
        ctx.cmd->SetDescriptorHeap(ctx.srvHeap->GetHeap());
        ctx.cmd->SetRenderTarget(m_in.sceneRtv, m_in.depthDsv);
        ctx.cmd->SetViewportAndScissor(m_in.width, m_in.height);
        m_in.atmo->DrawSky(ctx.native, m_in.cbSlot, m_in.depthSrv);
    }

private:
    Inputs m_in;
};

// エアリアルパースペクティブ合成。FogCompositePass と同じ位置（フォグ合成の直後）・同じ深度の扱い。Forward は触らない。
class AtmosphereApPass final : public IRenderPass
{
public:
    struct Inputs
    {
        AtmosphereRenderer*         atmo = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv{};
        ID3D12Resource*             sceneColor = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};
        u32 width = 0, height = 0;
        u32 cbSlot = 0;
    };
    explicit AtmosphereApPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "AtmosphereAerialPerspective"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override
    {
        out.push_back({"depth",      nullptr,           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, PassAccess::Read});
        out.push_back({"sceneColor", m_in.sceneColor,   D3D12_RESOURCE_STATE_RENDER_TARGET,         PassAccess::Write});
    }
    void Execute(const RenderPassContext& ctx) override
    {
        if (!m_in.atmo) return;
        // 深度は SRV で読む（DSV は張らない）。FogCompositePass と同じ。
        if (ctx.depth) ctx.depth->Require(*ctx.cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ctx.native->OMSetRenderTargets(1, &m_in.sceneRtv, FALSE, nullptr);
        ctx.cmd->SetViewportAndScissor(m_in.width, m_in.height);
        ctx.cmd->SetDescriptorHeap(ctx.srvHeap->GetHeap());
        m_in.atmo->DrawAerialPerspective(ctx.native, m_in.cbSlot, m_in.depthSrv);
    }

private:
    Inputs m_in;
};

} // namespace dx12e
