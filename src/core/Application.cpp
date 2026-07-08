#include "Application.h"
#include "Logger.h"
#include "Assert.h"
#include "PathResolver.h"
#include "Version.h"

// Graphics module headers
#include "graphics/GraphicsDevice.h"
#include "graphics/CommandQueue.h"
#include "graphics/DeferredRelease.h"
#include "graphics/SwapChain.h"
#include "graphics/FrameResources.h"
#include "core/SplashScreen.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GpuResource.h"
#include "graphics/Buffer.h"
#include "graphics/RootSignature.h"
#include "graphics/PipelineState.h"
#include "graphics/CommandList.h"
#include "graphics/Texture.h"
#include "graphics/RenderTarget.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "renderer/Camera.h"
#include "renderer/Frustum.h"
#include "renderer/PostProcess.h"
#include "renderer/BloomPass.h"
#include "renderer/AutoExposurePass.h"
#include "renderer/GodRaysPass.h"
#include "renderer/LensFlarePass.h"
#include "renderer/DofPass.h"
#include "renderer/MotionBlurPass.h"
#include "renderer/GpuParticleSystem.h"
#include "renderer/SSAOPass.h"
#include "renderer/ParticleSystem.h"
#include "renderer/SpriteRenderer.h"
#include "renderer/SceneTransition.h"
#include "renderer/IBLBaker.h"
#include "renderer/SkyboxRenderer.h"
#include "resource/ShaderCompiler.h"
#include "resource/ShaderManager.h"
#include "resource/ModelLoader.h"
#include "resource/ResourceManager.h"
#include "resource/TextureLoader.h"
#include "graphics/Texture.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "input/InputSystem.h"
#include "scene/Scene.h"
#include "scene/Entity.h"
#include "ecs/Components.h"
#include "scripting/ScriptEngine.h"
#include "core/mcp/McpBridge.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>    // MCP read_lua_component / validate_scene のレポート読み込み用
#include <cstdio>     // sscanf_s（ahead/behind 解析）
#include <cctype>     // std::isalnum（ビルド出力フォルダ名のサニタイズ）
#include <cmath>      // sin/cos/atan2/asin（カメラのワールド変換→yaw/pitch 逆算）
#include <map>        // 変更ファイルツリーの構築
#include "audio/AudioSystem.h"
#include "physics/PhysicsSystem.h"
#include "network/NetworkSystem.h"
#include "network/NetworkConfig.h"
#include "physics/PhysicsDebugRenderer.h"
#include "gui/ImGuiManager.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneFlow.h"
#include "engine/ecs/ComponentRegistry.h"   // MCP set_component の deserialize 走査用
#include "editor/EditorContext.h"
#include "editor/EditorLayer.h"
#include "editor/EditorTheme.h"        // バージョン管理パネルのステータス配色
#include "core/Version.h"              // kEngineVersion / 「更新内容」ポップアップの中身
#include "editor/EditorIconRenderer.h"
#include "editor/UndoSystem.h"
#include "editor/ModelThumbnailRenderer.h"
#include "editor/panels/McpBridgePanel.h"
#include "editor/panels/NetworkPanel.h"
#include "editor/panels/VfxEditorPanel.h"
#include "project/Project.h"
#include "project/ProjectManager.h"
#include "project/GitIntegration.h"
#include "vfs/Vfs.h"
#include "vfs/PakWriter.h"
#include <commdlg.h>
#include <shellapi.h>   // ShellExecuteA（ビルド完了後にフォルダを開く）

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"

#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <thread>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <cctype>
#include <unordered_map>
#include <immintrin.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <wincodec.h>                 // MCP screenshot: OS 標準 WIC で PNG 書き出し(外部依存なし)
#include <DirectXPackedVector.h>      // XMConvertHalfToFloat(FP16 sceneRT → 8bit)
#pragma comment(lib, "windowscodecs.lib")

namespace dx12e
{

// オフスクリーンのシーンカラーは HDR(FP16) で描く。発光が 1.0 を超えられるので
// ブルームとトーンマップで「白熱して光る」パーティクル/エフェクトが出せる。
// scene RT / cameraPreview RT、およびそこへ描く 3D PSO 群はこの形式で揃える。
// （バックバッファ＝スワップチェインは従来どおりスワップ形式のまま）
static constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// フルパスを assets ディレクトリ相対へ（シーンフロー / loadScene 用）
static std::string ToAssetRel(const std::string& full)
{
    auto norm = [](std::string s) { for (auto& c : s) if (c == '\\') c = '/'; return s; };
    std::string f = norm(full);
    std::string base = norm(PathResolver::AssetsDir());
    if (!base.empty() && f.rfind(base, 0) == 0)
        return f.substr(base.size());
    return f;
}

Application::Application() = default;

Application::~Application()
{
    if (m_isRunning)
    {
        Shutdown();
    }
}

namespace
{
// UTF-8 → wstring 変換は PathResolver::Utf8ToWide に一本化した
// （マージ前は両ブランチが同じ修正を別実装で持っていた）。

// 「更新内容」ポップアップを表示済みのバージョンを記録するファイル。
// %LOCALAPPDATA%\DX12Engine\shown_version.txt（exe の場所に依存せず必ず書ける）。
// Updater の last_update.txt と同じ場所に置く。
std::filesystem::path WhatsNewStatePath()
{
    char* base = nullptr;
    size_t len = 0;
    if (_dupenv_s(&base, &len, "LOCALAPPDATA") != 0 || !base) return {};
    std::filesystem::path dir = std::filesystem::path(base) / "DX12Engine";
    free(base);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "shown_version.txt";
}

std::string ReadShownVersion()
{
    std::filesystem::path p = WhatsNewStatePath();
    if (p.empty()) return {};
    std::ifstream f(p);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

void WriteShownVersion(const std::string& v)
{
    std::filesystem::path p = WhatsNewStatePath();
    if (p.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    if (f) f << v;
}
} // namespace

// ---------------------------------------------------------------------------
// シェーダーホットリロード用 PSO 再生成群
// 初回(Initialize)・以降の再生成(ShaderManager 経由のホットリロード)の両方から呼ばれる。
// 既存 unique_ptr が非 null ならオブジェクトを作り直さず Initialize() し直す(ComPtr 再代入のみ)。
// ModelThumbnailRenderer 等が PipelineState* の生ポインタを保持しているため、住所を変えないことが必須。
// ---------------------------------------------------------------------------
void Application::RecreateForwardPsos()
{
    auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Forward_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Forward_PS.cso");

    // 通常 forward は DepthFunc=LESS（既定）。毎フレーム深度を 1.0 へクリアして描くため、
    // 同深度フラグメントは先勝ち（従来の z-fight 解決を維持）。
    PipelineStateBuilder builder;
    builder.SetRootSignature(m_rootSignature->Get())
           .SetVertexShader(vs.GetData(), vs.GetSize())
           .SetPixelShader(ps.GetData(), ps.GetSize())
           .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
           .SetRenderTargetFormat(kSceneColorFormat)  // 描き込み先はシーンRT(HDR)。swapchain形式だとRTV不一致
           .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
           .SetDepthEnabled(true)
           .SetCullMode(D3D12_CULL_MODE_NONE);  // 両面描画（片面メッシュ対応）

    if (!m_pipelineState) m_pipelineState = std::make_unique<PipelineState>();
    m_pipelineState->Initialize(*m_graphicsDevice, builder);

    // 深度プリパス併用(SSAO)時専用の LESS_EQUAL バリアント。プリパスが書いた深度を
    // forward で再利用するため、同一深度を通す必要がある（通常経路の z-fight 解決は変えない）。
    builder.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
    if (!m_pipelineStateLEqual) m_pipelineStateLEqual = std::make_unique<PipelineState>();
    m_pipelineStateLEqual->Initialize(*m_graphicsDevice, builder);

    // サムネイル描画用バリアント。ModelThumbnailRenderer の RT は R8G8B8A8 で
    // ポストプロセス(トーンマップ)を通らないため、シェーダー内で ACES+ガンマまで
    // 済ませる LDR 直出力の PS を使う。
    auto psLdr = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardLdr_PS.cso");
    builder.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS);
    builder.SetRenderTargetFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
    builder.SetPixelShader(psLdr.GetData(), psLdr.GetSize());
    if (!m_pipelineStateThumb) m_pipelineStateThumb = std::make_unique<PipelineState>();
    m_pipelineStateThumb->Initialize(*m_graphicsDevice, builder);
}

void Application::RecreateSkinnedPsos()
{
    auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardSkinned_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Forward_PS.cso");

    // 通常 forward(skinned) は DepthFunc=LESS（既定）。SSAO 無効時の z-fight 解決を維持。
    PipelineStateBuilder builder;
    builder.SetRootSignature(m_rootSignature->Get())
           .SetVertexShader(vs.GetData(), vs.GetSize())
           .SetPixelShader(ps.GetData(), ps.GetSize())
           .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
           .SetRenderTargetFormat(kSceneColorFormat)
           .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
           .SetDepthEnabled(true)
           .SetCullMode(D3D12_CULL_MODE_NONE);

    if (!m_skinnedPipelineState) m_skinnedPipelineState = std::make_unique<PipelineState>();
    m_skinnedPipelineState->Initialize(*m_graphicsDevice, builder);

    // 深度プリパス併用(SSAO)時専用の LESS_EQUAL バリアント。
    builder.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
    if (!m_skinnedPipelineStateLEqual) m_skinnedPipelineStateLEqual = std::make_unique<PipelineState>();
    m_skinnedPipelineStateLEqual->Initialize(*m_graphicsDevice, builder);
}

void Application::RecreateGridPso()
{
    auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardGrid_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardGrid_PS.cso");

    PipelineStateBuilder builder;
    builder.SetRootSignature(m_rootSignature->Get())
           .SetVertexShader(vs.GetData(), vs.GetSize())
           .SetPixelShader(ps.GetData(), ps.GetSize())
           .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
           .SetRenderTargetFormat(kSceneColorFormat)
           .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
           .SetDepthEnabled(true)
           .SetDepthWrite(false)        // 深度を書かない＝床など同一平面の不透明物を隠さない
           .SetAlphaBlendEnabled(true)
           .SetCullMode(D3D12_CULL_MODE_NONE)
           .SetDepthBias(-100, -1.0f);  // 深度テストではカメラ側に寄せて床の上に線を乗せる

    if (!m_gridPipelineState) m_gridPipelineState = std::make_unique<PipelineState>();
    m_gridPipelineState->Initialize(*m_graphicsDevice, builder);
}

void Application::RecreateEmissivePso()
{
    auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Emissive_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Emissive_PS.cso");

    // インスタンシング版（slot1=MeshInstanceData）。Emissive.hlsl は instanced VS。
    PipelineStateBuilder builder;
    builder.SetRootSignature(m_rootSignature->Get())
           .SetVertexShader(vs.GetData(), vs.GetSize())
           .SetPixelShader(ps.GetData(), ps.GetSize())
           .SetInputLayout(Mesh::GetInstancedInputLayout(), Mesh::GetInstancedInputLayoutCount())
           .SetRenderTargetFormat(kSceneColorFormat)
           .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
           .SetDepthEnabled(true)
           .SetDepthWrite(false)
           .SetAdditiveBlendEnabled(true)
           .SetCullMode(D3D12_CULL_MODE_NONE);

    if (!m_emissivePipelineState) m_emissivePipelineState = std::make_unique<PipelineState>();
    m_emissivePipelineState->Initialize(*m_graphicsDevice, builder);
}

void Application::RecreateShadowPsos()
{
    // Shadow PSO (depth-only, no pixel shader, with depth bias)
    {
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ShadowPass_VS.cso");
        PipelineStateBuilder builder;
        builder.SetRootSignature(m_rootSignature->Get())
               .SetVertexShader(vs.GetData(), vs.GetSize())
               .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
               .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
               .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
               .SetDepthEnabled(true)
               .SetDepthBias(8000, 2.0f);

        if (!m_shadowPipelineState) m_shadowPipelineState = std::make_unique<PipelineState>();
        m_shadowPipelineState->Initialize(*m_graphicsDevice, builder);
    }

    // Shadow Skinned PSO
    {
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ShadowPassSkinned_VS.cso");
        PipelineStateBuilder builder;
        builder.SetRootSignature(m_rootSignature->Get())
               .SetVertexShader(vs.GetData(), vs.GetSize())
               .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
               .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
               .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
               .SetDepthEnabled(true)
               .SetDepthBias(8000, 2.0f);

        if (!m_shadowSkinnedPipelineState) m_shadowSkinnedPipelineState = std::make_unique<PipelineState>();
        m_shadowSkinnedPipelineState->Initialize(*m_graphicsDevice, builder);
    }
}

void Application::RecreateDepthPrepassPsos()
{
    // 深度プリパス PSO（SSAO 用カメラ深度。bias なし・カメラ視点・D32・RTVなし）。
    // ShadowPass_VS は b0 の mvp で頂点変換するだけなので、b0 に world*cameraVP を渡せば流用できる。
    {
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ShadowPass_VS.cso");
        PipelineStateBuilder builder;
        builder.SetRootSignature(m_rootSignature->Get())
               .SetVertexShader(vs.GetData(), vs.GetSize())
               .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
               .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
               .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
               .SetDepthEnabled(true)
               .SetCullMode(D3D12_CULL_MODE_NONE);  // bias なし（カメラ深度をそのまま使う）
        if (!m_depthPrepassPSO) m_depthPrepassPSO = std::make_unique<PipelineState>();
        m_depthPrepassPSO->Initialize(*m_graphicsDevice, builder);
    }
    {
        // forward(ForwardSkinned) とクリップ Z をビット一致させる専用スキンド深度プリパス。
        // ShadowPassSkinned はスキニング演算順(sum(w*mul(pos,B))) と totalWeight==0 フォールバックが
        // forward(mul(pos,sum(w*B)) / フォールバック無し) と違い、SSAO の深度再利用で z-fight/欠落を招く。
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"DepthPrepassSkinned_VS.cso");
        PipelineStateBuilder builder;
        builder.SetRootSignature(m_rootSignature->Get())
               .SetVertexShader(vs.GetData(), vs.GetSize())
               .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
               .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
               .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
               .SetDepthEnabled(true)
               .SetCullMode(D3D12_CULL_MODE_NONE);
        if (!m_depthPrepassSkinnedPSO) m_depthPrepassSkinnedPSO = std::make_unique<PipelineState>();
        m_depthPrepassSkinnedPSO->Initialize(*m_graphicsDevice, builder);
    }
}

void Application::RegisterShaderReloadHandlers()
{
    if (!m_shaderManager)
        return;

    m_shaderManager->RegisterReloadHandler(
        { L"Forward_VS.cso", L"Forward_PS.cso", L"ForwardLdr_PS.cso" },
        [this]() { RecreateForwardPsos(); });
    m_shaderManager->RegisterReloadHandler(
        { L"ForwardSkinned_VS.cso", L"Forward_PS.cso" },
        [this]() { RecreateSkinnedPsos(); });
    m_shaderManager->RegisterReloadHandler(
        { L"ForwardGrid_VS.cso", L"ForwardGrid_PS.cso" },
        [this]() { RecreateGridPso(); });
    m_shaderManager->RegisterReloadHandler(
        { L"Emissive_VS.cso", L"Emissive_PS.cso" },
        [this]() { RecreateEmissivePso(); });
    m_shaderManager->RegisterReloadHandler(
        { L"ShadowPass_VS.cso", L"ShadowPassSkinned_VS.cso" },
        [this]() { RecreateShadowPsos(); RecreateDepthPrepassPsos(); });  // ShadowPass_VS は深度プリパスにも流用
    m_shaderManager->RegisterReloadHandler(
        { L"DepthPrepassSkinned_VS.cso" },
        [this]() { RecreateDepthPrepassPsos(); });
    if (m_postProcess)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"PostProcess_VS.cso", L"PostProcess_PS.cso" },
            [this]() { m_postProcess->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_ssaoPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"SSAO_VS.cso", L"SSAO_PS.cso", L"SSAOBlur_PS.cso" },
            [this]() { m_ssaoPass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_bloomPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"Bloom_VS.cso", L"BloomDown_PS.cso", L"BloomUp_PS.cso" },
            [this]() { m_bloomPass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_dofPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"DofCoc_PS.cso", L"DofGather_PS.cso", L"DofComposite_PS.cso" },
            [this]() { m_dofPass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_motionBlurPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"MotionBlur_PS.cso" },
            [this]() { m_motionBlurPass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_lensFlarePass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"LensFlare_PS.cso" },
            [this]() { m_lensFlarePass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_godRaysPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"GodRaysMask_PS.cso", L"GodRaysBlur_PS.cso" },
            [this]() { m_godRaysPass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_autoExposure)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"ExposureHistogram_CS.cso", L"ExposureAdapt_CS.cso" },
            [this]() { m_autoExposure->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_skyboxRenderer)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"Skybox_VS.cso", L"Skybox_PS.cso" },
            [this]() { m_skyboxRenderer->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_sceneTransition)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"Transition_VS.cso", L"Transition_PS.cso" },
            [this]() { m_sceneTransition->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_iblBaker)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"IrradianceConvolution_CS.cso", L"PrefilterEnv_CS.cso", L"IntegrateBRDF_CS.cso" },
            [this]() { m_iblBaker->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_physicsDebugRenderer)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"DebugLine_VS.cso", L"DebugLine_PS.cso" },
            [this]() { m_physicsDebugRenderer->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_editorIconRenderer)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"IconBillboard_VS.cso", L"IconBillboard_PS.cso" },
            [this]() { m_editorIconRenderer->RecreatePipelines(*m_graphicsDevice); });
    }
    // Particle.hlsl は ParticleSystem(m_particleSystem/VFXプレビュー) + GpuParticleSystem の
    // 描画PSOがPS(Particle_PS.cso)を共有するクロス依存。3者まとめて再生成する。
    {
        std::vector<std::function<void()>> particleTargets;
        if (m_particleSystem)
            particleTargets.push_back([this]() { m_particleSystem->RecreatePipelines(*m_graphicsDevice); });
        if (m_gpuParticles)
            particleTargets.push_back([this]() { m_gpuParticles->RecreatePipelines(*m_graphicsDevice); });
        if (m_vfxEditorPanel)
            particleTargets.push_back([this]() { m_vfxEditorPanel->RecreatePipelines(*m_graphicsDevice); });
        if (!particleTargets.empty())
        {
            m_shaderManager->RegisterReloadHandler(
                { L"Particle_VS.cso", L"Particle_PS.cso", L"ParticleDistort_PS.cso",
                  L"Trail_VS.cso", L"Trail_PS.cso", L"Beam_VS.cso", L"Beam_PS.cso",
                  L"GpuParticleInit_CS.cso", L"GpuParticlePrepare_CS.cso", L"GpuParticleEmit_CS.cso",
                  L"GpuParticleKickoff_CS.cso", L"GpuParticleSimulate_CS.cso", L"GpuParticleDraw_VS.cso" },
                [particleTargets]() { for (auto& fn : particleTargets) fn(); });
        }
    }

    // カスタムシェーダー(MeshRenderer::shaderPath)は Registry 外なので csoName 経路ではなく
    // 専用のカスタムハンドラで扱う。キャッシュを捨てるだけで、次回描画時に EnsureCustomPso が
    // 最新バイトコードから作り直す。
    // 既知の制約: カスタムシェーダーが forward/Lighting.hlsli 等の共有 .hlsli を include していても、
    // その .hlsli の変更だけでは再コンパイルされない(依存追跡は Registry ソースのみ対象)。
    // カスタムシェーダー自身を保存し直せば最新の include 内容で再コンパイルされる。
    m_shaderManager->RegisterCustomReloadHandler(
        [this](const std::string& relKey)
        {
            m_customPsoCache.erase(relKey);
            m_customSpritePsoCache.erase(relKey);  // Sprite2D::shaderPath 用キャッシュも同じキーで破棄
        });
}

// EnsureCustomPso/EnsureCustomSpritePso 共通: shaderRel の正規化キーを返す(小文字・'/'区切り)。
static std::string NormalizeCustomShaderKey(const std::string& shaderRel)
{
    std::string key = shaderRel;
    for (char& c : key)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

// バイトコードの取得元: エディタは ShaderManager(実行時コンパイル済みのメモリ内キャッシュ)。
// ゲームモード(ShaderManager 非生成)は BuildGame が game.pak に焼いた
// "shaders/custom/<relPath>_VS.cso" 等を ShaderCompiler::LoadFromFile 経由(VFS)で読む
// (キー生成規約は Application::BuildGame と一致させること)。
// 戻り値: 取得できたら true(vsBytesOut/psBytesOutが有効)。vsStorage/psStorageはゲームモード時の実体保持用。
bool Application::FetchCustomShaderBytecode(const std::string& shaderRel,
                                             std::vector<u8>& vsStorage, std::vector<u8>& psStorage,
                                             const std::vector<u8>*& vsBytesOut, const std::vector<u8>*& psBytesOut)
{
    vsBytesOut = nullptr;
    psBytesOut = nullptr;
    const std::string key = NormalizeCustomShaderKey(shaderRel);

    if (m_shaderManager)
    {
        if (!m_shaderManager->HasValidCustomShader(shaderRel))
            m_shaderManager->CompileCustomShader(shaderRel);  // 未スキャン分の遅延コンパイル救済
        vsBytesOut = m_shaderManager->GetCustomVsBytecode(shaderRel);
        psBytesOut = m_shaderManager->GetCustomPsBytecode(shaderRel);
    }
    else
    {
        try
        {
            const std::wstring base = PathResolver::ShaderDirW() + L"custom/" + PathResolver::Utf8ToWide(key);
            vsStorage = ShaderCompiler::LoadFromFile(base + L"_VS.cso").data;
            psStorage = ShaderCompiler::LoadFromFile(base + L"_PS.cso").data;
            vsBytesOut = &vsStorage;
            psBytesOut = &psStorage;
        }
        catch (const std::exception&)
        {
            // ビルドに含まれていない(未使用 or ビルド後に割当変更)。既定シェーダーへフォールバック。
        }
    }
    return vsBytesOut && psBytesOut && !vsBytesOut->empty() && !psBytesOut->empty();
}

Application::CustomForwardPsos* Application::EnsureCustomPso(const std::string& shaderRel)
{
    const std::string key = NormalizeCustomShaderKey(shaderRel);

    auto it = m_customPsoCache.find(key);
    if (it != m_customPsoCache.end())
        return it->second.valid ? &it->second : nullptr;

    std::vector<u8> vsStorage, psStorage;
    const std::vector<u8>* vsBytes = nullptr;
    const std::vector<u8>* psBytes = nullptr;
    FetchCustomShaderBytecode(shaderRel, vsStorage, psStorage, vsBytes, psBytes);

    CustomForwardPsos entry;
    if (vsBytes && psBytes && !vsBytes->empty() && !psBytes->empty())
    {
        try
        {
            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vsBytes->data(), vsBytes->size())
                   .SetPixelShader(psBytes->data(), psBytes->size())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(kSceneColorFormat)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetCullMode(D3D12_CULL_MODE_NONE);
            entry.less = std::make_unique<PipelineState>();
            entry.less->Initialize(*m_graphicsDevice, builder);
            builder.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
            entry.lequal = std::make_unique<PipelineState>();
            entry.lequal->Initialize(*m_graphicsDevice, builder);

            // アルファブレンド版(MeshRenderer::shaderAlphaBlend=true 時に使う)。半透明の定石で
            // DepthWrite は OFF(ForwardGrid と同じ考え方、背後が透けて見えるようにする)。
            builder.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS)
                   .SetDepthWrite(false)
                   .SetAlphaBlendEnabled(true);
            entry.lessBlend = std::make_unique<PipelineState>();
            entry.lessBlend->Initialize(*m_graphicsDevice, builder);
            builder.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
            entry.lequalBlend = std::make_unique<PipelineState>();
            entry.lequalBlend->Initialize(*m_graphicsDevice, builder);

            entry.valid = true;
        }
        catch (const std::exception& ex)
        {
            Logger::Error("カスタムシェーダーのPSO生成に失敗しました: {} - {}", shaderRel, ex.what());
            entry.valid = false;
        }
    }

    auto& stored = m_customPsoCache[key];
    stored = std::move(entry);
    return stored.valid ? &stored : nullptr;
}

Application::CustomSpritePsos* Application::EnsureCustomSpritePso(const std::string& shaderRel)
{
    const std::string key = NormalizeCustomShaderKey(shaderRel);

    auto it = m_customSpritePsoCache.find(key);
    if (it != m_customSpritePsoCache.end())
        return it->second.valid ? &it->second : nullptr;

    std::vector<u8> vsStorage, psStorage;
    const std::vector<u8>* vsBytes = nullptr;
    const std::vector<u8>* psBytes = nullptr;
    FetchCustomShaderBytecode(shaderRel, vsStorage, psStorage, vsBytes, psBytes);

    CustomSpritePsos entry;
    if (vsBytes && psBytes && !vsBytes->empty() && !psBytes->empty())
    {
        try
        {
            u32 layoutCount = 0;
            const D3D12_INPUT_ELEMENT_DESC* layout = SpriteRenderer::GetInputLayout(&layoutCount);

            // Sprite2D の world PSO と同じ深度設定(LESS_EQUAL・書き込みOFF)で固定。
            // メッシュ版と違い深度プリパスの概念が無いので不透明/ブレンドの2種類のみ。
            PipelineStateBuilder builder;
            builder.SetRootSignature(m_spriteRenderer->GetRootSignature())
                   .SetVertexShader(vsBytes->data(), vsBytes->size())
                   .SetPixelShader(psBytes->data(), psBytes->size())
                   .SetInputLayout(layout, layoutCount)
                   .SetRenderTargetFormat(kSceneColorFormat)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL)
                   .SetDepthWrite(false)
                   .SetCullMode(D3D12_CULL_MODE_NONE);
            entry.opaque = std::make_unique<PipelineState>();
            entry.opaque->Initialize(*m_graphicsDevice, builder);

            builder.SetAlphaBlendEnabled(true);
            entry.blend = std::make_unique<PipelineState>();
            entry.blend->Initialize(*m_graphicsDevice, builder);

            entry.valid = true;
        }
        catch (const std::exception& ex)
        {
            Logger::Error("Sprite2D カスタムシェーダーのPSO生成に失敗しました: {} - {}", shaderRel, ex.what());
            entry.valid = false;
        }
    }

    auto& stored = m_customSpritePsoCache[key];
    stored = std::move(entry);
    return stored.valid ? &stored : nullptr;
}

u32 Application::EnsureMaterialOverrideSrv(entt::entity e, u32 submeshIndex, const MeshRenderer& renderer,
                                            const Material* mat, ID3D12GraphicsCommandList* cmdList)
{
    if (!renderer.HasAnyTextureOverride(submeshIndex))
        return 0xFFFFFFFF;

    const std::string& albedoPath = MeshRenderer::SafeGetOverride(renderer.overrideAlbedoTexture, submeshIndex);
    const std::string& normalPath = MeshRenderer::SafeGetOverride(renderer.overrideNormalTexture, submeshIndex);
    const std::string& mrPath     = MeshRenderer::SafeGetOverride(renderer.overrideMetalRoughnessTexture, submeshIndex);

    const u64 key = (static_cast<u64>(e) << 16) | submeshIndex;
    auto it = m_materialOverrideSrvCache.find(key);
    if (it != m_materialOverrideSrvCache.end() && it->second.blockStart != 0xFFFFFFFF
        && it->second.albedoPath == albedoPath && it->second.normalPath == normalPath
        && it->second.mrPath == mrPath)
    {
        return it->second.blockStart;
    }

    // 上書き指定があればロードして使い、無ければ Material 既定→ResourceManager デフォルトの順に
    // フォールバック（ModelLoader が SRV ブロックを組む時と同じ解決順）。
    auto resolve = [&](const std::string& overridePath, Texture* fallback, bool srgb) -> Texture* {
        if (overridePath.empty())
            return fallback;
        std::string fullPath = PathResolver::AssetsDir() + overridePath;
        if (!std::filesystem::exists(fullPath))
        {
            Logger::Warn("マテリアル上書きテクスチャが見つかりません: {}", fullPath);
            return fallback;
        }
        return m_resourceManager->GetOrLoadTexture(PathResolver::Utf8ToWide(fullPath), cmdList, srgb);
    };

    Texture* albedoFallback = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
    Texture* normalFallback = (mat && mat->normalMapTexture) ? mat->normalMapTexture : m_resourceManager->GetDefaultNormalTexture();
    Texture* mrFallback     = (mat && mat->metalRoughnessTexture) ? mat->metalRoughnessTexture : m_resourceManager->GetDefaultMetalRoughnessTexture();

    Texture* albedo = resolve(albedoPath, albedoFallback, /*srgb=*/true);
    Texture* normal = resolve(normalPath, normalFallback, /*srgb=*/false);
    Texture* mr     = resolve(mrPath,     mrFallback,     /*srgb=*/false);
    if (!albedo || !normal || !mr)
        return 0xFFFFFFFF;

    MaterialOverrideSrv& entry = m_materialOverrideSrvCache[key];
    if (entry.blockStart == 0xFFFFFFFF)
        entry.blockStart = m_srvHeap->AllocateBlock(3);

    albedo->CreateSRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(entry.blockStart));
    normal->CreateSRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(entry.blockStart + 1));
    mr->CreateSRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(entry.blockStart + 2));

    entry.albedoPath = albedoPath;
    entry.normalPath = normalPath;
    entry.mrPath = mrPath;
    return entry.blockStart;
}

void Application::Initialize(HINSTANCE hInstance, int nCmdShow, bool gameMode,
                             const ProjectInfo* /*projectInfo*/, bool buildMode)
{
    // ロガー初期化
    Logger::Init();
    m_isGameMode = gameMode;
    m_showLauncher = !gameMode;  // ゲームモードではランチャーを表示しない
    // エディタで、前回表示した版と違う＝更新された/初回 のときだけ「更新内容」を出す。
    m_showWhatsNew = !gameMode && (ReadShownVersion() != std::string(kEngineVersion));
    Logger::Info("Application initializing... (mode: {})", gameMode ? "game" : "editor");

    // エディタコンテキスト初期化
    m_editorCtx = std::make_unique<EditorContext>();

    // ウィンドウ作成（タイトルにエンジンのバージョンを表記＝更新の確認にも使える）
    m_window = std::make_unique<Window>();
    std::wstring windowTitle = L"DX12 Engine v";
    for (const char* vp = kEngineVersion; *vp; ++vp)
        windowTitle += static_cast<wchar_t>(*vp);  // kEngineVersion は ASCII

    u32 winW = 1280, winH = 720;
    // ゲームモード: pak の __manifest__（ビルド設定で書き出した値）からタイトル/解像度を反映。
    // main.cpp で pak は app.Initialize より前にマウント済みなので、ここで読める。
    if (gameMode)
    {
        vfs::BootConfig bc;
        if (vfs::ReadBootConfig(bc))
        {
            if (bc.windowWidth  > 0) winW = static_cast<u32>(bc.windowWidth);
            if (bc.windowHeight > 0) winH = static_cast<u32>(bc.windowHeight);
            if (!bc.title.empty())
            {
                int n = MultiByteToWideChar(CP_UTF8, 0, bc.title.c_str(), -1, nullptr, 0);
                if (n > 0)
                {
                    std::wstring wt(static_cast<size_t>(n - 1), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, bc.title.c_str(), -1, wt.data(), n);
                    windowTitle = wt;   // 配布ゲームはエンジン名ではなく製品タイトルを表示
                }
            }
        }
    }
    // エディタ起動時はメインウィンドウの表示を初期化完了まで遅延する
    // （スプラッシュが進行状況を見せるので、白い未応答ウィンドウを出さない）。
    // ゲーム/ヘッドレスビルドはスプラッシュを出さないので従来どおり即表示。
    const bool deferMainWindow = !gameMode && !buildMode;
    SplashScreen::SetStatus("ウィンドウを作成中...");
    m_window->Initialize(hInstance, nCmdShow, winW, winH, windowTitle.c_str(), deferMainWindow);
    // タイトルバーの X を横取り。ゲーム(GameRuntime)は従来通り即終了、エディタでプロジェクトを
    // 開いている時はいきなり終了せずランチャー（プロジェクト作成前の画面）に戻す。
    m_window->SetCloseHandler([this]{ return HandleWindowCloseRequest(); });

    // グラフィックスデバイス初期化
    SplashScreen::SetStatus("グラフィックスデバイスを初期化中...");
    m_graphicsDevice = std::make_unique<GraphicsDevice>();
    m_graphicsDevice->Initialize(*m_window);

    // コマンドキュー作成
    m_commandQueue = std::make_unique<CommandQueue>();
    m_commandQueue->Initialize(*m_graphicsDevice, D3D12_COMMAND_LIST_TYPE_DIRECT);

    // ディスクリプタヒープ作成（RTV用）
    m_descriptorHeap = std::make_unique<DescriptorHeap>();
    m_descriptorHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, false);

    // スワップチェイン初期化
    m_swapChain = std::make_unique<SwapChain>();
    m_swapChain->Initialize(*m_window, *m_graphicsDevice, *m_commandQueue, *m_descriptorHeap);

    // フレームリソース初期化
    m_frameResources = std::make_unique<FrameResources>();
    m_frameResources->Initialize(*m_graphicsDevice, *m_commandQueue);

    // ゲームクロックリセット
    m_gameClock.Reset();

    // Input System
    m_inputSystem = std::make_unique<InputSystem>();
    m_inputSystem->Initialize(m_window->GetHwnd());
    m_window->SetInputSystem(m_inputSystem.get());

    // Audio System
    SplashScreen::SetStatus("オーディオを初期化中...");
    m_audioSystem = std::make_unique<AudioSystem>();
    m_audioSystem->Initialize(PathResolver::AssetsDir());

    // Physics System
    SplashScreen::SetStatus("物理エンジンを初期化中...");
    m_physicsSystem = std::make_unique<PhysicsSystem>();
    m_physicsSystem->Initialize();
    // 接触イベント（engine.contact.enter/exit）を C++ EventBus へ配信させる。
    // m_eventBus は Application の安定メンバ。物理を Shutdown→Initialize で再構築しても
    // PhysicsSystem 側はこのポインタを保持し続ける（Shutdown では null 化しない）。
    m_physicsSystem->SetEventBus(&m_eventBus);

    // Network System（GPU非依存。Play/Stopで再構築しない＝m_eventBusはここで一度だけ注入）。
    // assets/network.json が無い(初回起動等)場合は既定値のまま続行する。
    m_networkSystem = std::make_unique<NetworkSystem>();
    m_networkSystem->SetEventBus(&m_eventBus);
    m_networkSystem->SetPhysicsSystem(m_physicsSystem.get());   // 予測リコンシリエーションのリプレイ用(フェーズ⑦b)
    {
        NetworkConfig cfg;
        cfg.Load(PathResolver::AssetsDir() + "network.json");
        m_networkSystem->SetConfig(cfg);
    }
    {
        NetworkSystem::Hooks hooks;
        hooks.currentScenePath = [this]() { return m_currentSceneRel; };
        hooks.requestSceneLoad = [this](const std::string& rel) { m_editorCtx->pendingGameLoadPath = rel; };
        m_networkSystem->SetHooks(std::move(hooks));
    }

    // Shader Visible SRV ヒープ
    m_srvHeap = std::make_unique<DescriptorHeap>();
    m_srvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, true);

    // ResourceManager
    m_resourceManager = std::make_unique<ResourceManager>();
    // ResourceManager は暫定コマンドリストで初期化（デフォルトテクスチャ作成のため）
    // → モデルロード用の BeginFrame の後に初期化する

    // DSV ヒープ
    m_dsvHeap = std::make_unique<DescriptorHeap>();
    m_dsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

    // デプスバッファ作成
    {
        // R32_TYPELESS で確保し、DSV(D32_FLOAT) と SRV(R32_FLOAT) の両ビューを張る。
        // SRV はパーティクルの soft particles（接地フェード＋手動オクルージョン）で読む。
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = m_window->GetWidth();
        depthDesc.Height = m_window->GetHeight();
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.SampleDesc = {1, 0};
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue, IID_PPV_ARGS(&m_depthBuffer)));

        m_dsvHandle = m_dsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_graphicsDevice->GetDevice()->CreateDepthStencilView(
            m_depthBuffer.Get(), &dsvDesc, m_dsvHandle);

        m_depthSrvIndex = m_srvHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
        depthSrvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels     = 1;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_depthBuffer.Get(), &depthSrvDesc, m_srvHeap->GetCpuHandle(m_depthSrvIndex));
    }

    // RootSignature
    m_rootSignature = std::make_unique<RootSignature>();
    m_rootSignature->Initialize(*m_graphicsDevice);

    // プロジェクト独自シェーダー(上書き/自作)の実行時コンパイル基盤。エディタモードのみ。
    // 最初の ShaderCompiler::LoadFromFile(直後のブロック)より前に用意しておく必要がある
    // (ShaderCompiler::LoadFromFile が ShaderManager::Instance() のオーバーライドを先に見るため)。
    if (!m_isGameMode)
    {
        m_shaderManager = std::make_unique<ShaderManager>();
        ShaderManager::SetInstance(m_shaderManager.get());
        m_shaderManager->Initialize();
        if (!m_shaderManager->IsRuntimeCompileAvailable())
            Logger::Warn("シェーダーの実行時コンパイルが利用できません(ホットリロード無効、通常の.csoは読み込めます)");
    }

    // シェーダー読み込み & PipelineState
    RecreateForwardPsos();

    // Camera
    m_camera = std::make_unique<Camera>();
    {
        f32 viewW = static_cast<f32>(m_window->GetWidth());
        f32 viewH = static_cast<f32>(m_window->GetHeight());
        m_camera->SetPerspective(DirectX::XM_PIDIV4, viewW / viewH, 0.1f, 1000.0f);
    }
    m_camera->LookAt({-14.7f, 9.6f, -9.0f}, {0.0f, 0.0f, 0.0f});

    // シーン + モデル読み込み
    {
        // 暫定コマンドリストで GPU アップロード
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        // ResourceManager 初期化（デフォルト白テクスチャ作成にcmdListが必要）
        SplashScreen::SetStatus("アセットを読み込み中...");
        m_resourceManager = std::make_unique<ResourceManager>();
        m_resourceManager->Initialize(m_graphicsDevice.get(), m_srvHeap.get(), cmdList);

        // SSAO 無効/編集ビュー用の 1x1 白 R8_UNORM ダミー（forward の g_ssao が常に 1.0 を返す）。
        {
            u8 white = 0xFF;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 1; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8_UNORM;
            desc.SampleDesc = {1, 0};

            D3D12_SUBRESOURCE_DATA subData{};
            subData.pData = &white; subData.RowPitch = 1; subData.SlicePitch = 1;

            m_ssaoWhiteTex = std::make_unique<Texture>();
            m_ssaoWhiteTex->Initialize(*m_graphicsDevice, cmdList, desc, &subData, 1);
            m_ssaoWhiteSrvIndex = m_srvHeap->AllocateIndex();
            m_ssaoWhiteTex->SetSrvIndex(m_ssaoWhiteSrvIndex);
            m_ssaoWhiteTex->CreateSRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(m_ssaoWhiteSrvIndex));
        }

        // エディタUIアイコンを読み込み（エンジン側assets基準。プロジェクト切替前に1度）
        if (!m_isGameMode)
            LoadEditorIcons(cmdList);

        // Scene 初期化
        m_scene = std::make_unique<Scene>();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        // ScriptEngine 初期化 + ゲームスクリプト実行
        SplashScreen::SetStatus("スクリプトエンジンを初期化中...");
        m_scriptEngine = std::make_unique<ScriptEngine>();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), PathResolver::AssetsDir());
        WireScriptCallbacks();

        // ゲームスクリプト読み込み（グローバル game.lua）
        LoadGameScript();

        // 初期シーン: (配布) game.json の startScene → (エディタ) 最後に開いたシーン → default.json → クリーン状態
        {
            bool loaded = false;

            // 配布モード: pak __manifest__ または game.json で開始シーンを指定（最優先）
            if (m_isGameMode)
            {
                if (vfs::InGameMode())
                {
                    // ゲームモード: pak 内 __manifest__ からブート設定を読む（game.json 不要）
                    vfs::BootConfig bc;
                    if (vfs::ReadBootConfig(bc) && !bc.startScene.empty())
                    {
                        std::string startScene = PathResolver::AssetsDir() + bc.startScene;
                        loaded = SceneSerializer::Load(*m_scene, startScene, PathResolver::AssetsDir());
                        if (loaded)
                        {
                            m_editorCtx->currentScenePath = startScene;
                            m_currentSceneRel = bc.startScene;
                            Logger::Info("Loaded start scene from manifest: {}", bc.startScene);
                        }
                    }
                }
                else
                {
                    // ディスクモード（--game フラグ + game.json 配置の旧形式）
                    ProjectInfo gi;
                    if (Project::Load(PathResolver::BaseDir() + "game.json", gi) && !gi.defaultScene.empty())
                    {
                        std::string startScene = PathResolver::AssetsDir() + gi.defaultScene;
                        if (std::filesystem::exists(startScene))
                        {
                            loaded = SceneSerializer::Load(*m_scene, startScene, PathResolver::AssetsDir());
                            if (loaded)
                            {
                                m_editorCtx->currentScenePath = startScene;
                                m_currentSceneRel = gi.defaultScene;
                                Logger::Info("Loaded start scene from game.json: {}", gi.defaultScene);
                            }
                        }
                    }
                }
            }

            std::string lastScene = ProjectManager::LoadLastOpenedScene();
            std::string defaultScene = PathResolver::AssetsDir() + "scenes/default.json";

            if (!loaded && !m_isGameMode && !lastScene.empty() && std::filesystem::exists(lastScene))
            {
                loaded = SceneSerializer::Load(*m_scene, lastScene, PathResolver::AssetsDir());
                if (loaded)
                {
                    m_editorCtx->currentScenePath = lastScene;
                    // マルチプレイの Welcome はシーンを assets 相対で送る(クライアントは自分の
                    // assets 配下から読む)ため、絶対パスから "assets/" 以降を相対として控える。
                    std::string norm = lastScene;
                    std::replace(norm.begin(), norm.end(), '\\', '/');
                    if (size_t p = norm.rfind("/assets/"); p != std::string::npos)
                        m_currentSceneRel = norm.substr(p + 8);
                }
            }
            if (!loaded && std::filesystem::exists(defaultScene))
            {
                loaded = SceneSerializer::Load(*m_scene, defaultScene, PathResolver::AssetsDir());
                if (loaded)
                    m_editorCtx->currentScenePath = defaultScene;
            }
            if (!loaded)
            {
                // クリーン初期状態: Grid + DirectionalLight + MainCamera（再生に必要な最低限）
                m_scene->SpawnPlane("Grid", {0, 0, 0}, kEditorGridSize, true);
                auto& reg = m_scene->GetRegistry();
                auto lightE = reg.create();
                reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
                reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
                reg.emplace<DirectionalLight>(lightE);

                auto camE = reg.create();
                reg.emplace<NameTag>(camE, NameTag{"MainCamera"});
                reg.emplace<Transform>(camE, Transform{{0.0f, 6.0f, -12.0f}, {22.0f, 0.0f, 0.0f}, {1,1,1}});
                CameraComponent cam;
                cam.isActive = true;
                reg.emplace<CameraComponent>(camE, cam);
            }

            // ロードしたシーン(最後に開いた/ default.json 等)に Grid が無ければ補う。
            // 旧シーンや Grid 未配置データを開いてもエディタにグリッドが必ず出る。
            EnsureEditorGrid();

            // シーンフロー / loadScene 用に現在シーンの相対パスを記録
            if (m_currentSceneRel.empty() && !m_editorCtx->currentScenePath.empty())
                m_currentSceneRel = ToAssetRel(m_editorCtx->currentScenePath);
        }

        // ホットリロード用タイムスタンプ初期化（初回の誤発火を防止）
        {
            std::string scriptPath = PathResolver::GameLuaPath();
            if (std::filesystem::exists(scriptPath))
                m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);
        }

        // エディタモード初期化時はキャプチャ解除（Luaが OnStart で capture する場合があるため）
        if (!m_isGameMode)
            m_inputSystem->SetMouseCapture(false);

        // コマンド実行 + GPU待ち
        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();

        // アップロードバッファ解放
        m_resourceManager->FinishUploads();
        if (m_ssaoWhiteTex) m_ssaoWhiteTex->FinishUpload();

        // スキニング PSO 作成
        RecreateSkinnedPsos();

        // グリッド PSO 作成（アルファブレンド + 両面描画）
        RecreateGridPso();

        // 加算発光 PSO（パーティクル用）：ライティング無視・加算合成・深度書き込みOFF
        {
            RecreateEmissivePso();

            // per-instance バッファ（kFrameCount でリング化＝インフライト安全）。永続Map。
            for (u32 fi = 0; fi < FrameResources::kFrameCount; ++fi)
            {
                const UINT bytes = kMaxInstances * sizeof(MeshInstanceData);
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&m_instanceBuffer[fi])));
                void* mapped = nullptr; D3D12_RANGE rr{0, 0};
                ThrowIfFailed(m_instanceBuffer[fi]->Map(0, &rr, &mapped));
                m_instanceMapped[fi] = static_cast<uint8_t*>(mapped);
                m_instanceVbView[fi].BufferLocation = m_instanceBuffer[fi]->GetGPUVirtualAddress();
                m_instanceVbView[fi].StrideInBytes  = sizeof(MeshInstanceData);
                m_instanceVbView[fi].SizeInBytes    = bytes;
            }
        }

        // sneakWalk アニメーションを全スケルタルEntityに追加
        {
            std::filesystem::path sneakPath = PathResolver::AssetsDir() + "models/human/sneakWalk.gltf";
            if (std::filesystem::exists(sneakPath))
            {
                auto& reg = m_scene->GetRegistry();
                auto skelView = reg.view<SkeletalAnimation>();
                for (auto [e, skelAnim] : skelView.each())
                {
                    auto extraAnims = ModelLoader::LoadAnimationsFromFile(
                        sneakPath, *skelAnim.skeleton);
                    for (auto& a : extraAnims)
                    {
                        a->SetName("sneakWalk");
                        skelAnim.clips.push_back(std::move(a));
                    }
                }
            }
        }
    }

    // シャドウマップ作成（CSM: Texture2DArray, ArraySize=kNumCascades）
    {
        m_shadowDsvHeap = std::make_unique<DescriptorHeap>();
        m_shadowDsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kNumCascades, false);

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = m_shadowMapSize;
        shadowDesc.Height = m_shadowMapSize;
        shadowDesc.DepthOrArraySize = static_cast<u16>(kNumCascades);
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc = {1, 0};
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &shadowDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&m_shadowMap)));

        // DSV: 配列スライス毎に kNumCascades 個
        for (u32 i = 0; i < kNumCascades; ++i)
        {
            m_shadowDsvHandles[i] = m_shadowDsvHeap->Allocate();
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.FirstArraySlice = i;
            dsvDesc.Texture2DArray.ArraySize = 1;
            dsvDesc.Texture2DArray.MipSlice = 0;
            m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                m_shadowMap.Get(), &dsvDesc, m_shadowDsvHandles[i]);
        }

        // SRV: 配列SRV(1個)
        m_shadowSrvIndex = m_srvHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = kNumCascades;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_shadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_shadowSrvIndex));

        // Shadow PSO (depth-only, no pixel shader, with depth bias) + Skinned版
        RecreateShadowPsos();

        // 深度プリパス PSO（SSAO 用カメラ深度）+ Skinned版
        RecreateDepthPrepassPsos();

        Logger::Info("Shadow map initialized ({}x{})", m_shadowMapSize, m_shadowMapSize);
    }

    // スポット/ポイントライトの影マップ作成（CSMと同レシピ: R32_TYPELESS 配列 + スライス毎DSV + 1個のSRV）。
    // PSO/サンプラーはCSM用を流用（ShadowPass_VS は b0.mvp で変換するだけ＝面/灯ごとの VP を渡せば足りる）。
    {
        const u32 kNumPointFaces = kMaxShadowPoint * 6;
        m_punctualShadowDsvHeap = std::make_unique<DescriptorHeap>();
        m_punctualShadowDsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                            kMaxShadowSpot + kNumPointFaces, false);

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        // --- スポット: Texture2DArray(ArraySize=kMaxShadowSpot) ---
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = kSpotShadowMapSize;
            desc.Height = kSpotShadowMapSize;
            desc.DepthOrArraySize = static_cast<u16>(kMaxShadowSpot);
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.SampleDesc = {1, 0};
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clearValue, IID_PPV_ARGS(&m_spotShadowMap)));

            for (u32 i = 0; i < kMaxShadowSpot; ++i)
            {
                m_spotShadowDsvHandles[i] = m_punctualShadowDsvHeap->Allocate();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.FirstArraySlice = i;
                dsvDesc.Texture2DArray.ArraySize = 1;
                m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                    m_spotShadowMap.Get(), &dsvDesc, m_spotShadowDsvHandles[i]);
            }

            m_spotShadowSrvIndex = m_srvHeap->AllocateIndex();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.ArraySize = kMaxShadowSpot;
            m_graphicsDevice->GetDevice()->CreateShaderResourceView(
                m_spotShadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_spotShadowSrvIndex));
        }

        // --- ポイント: Texture2DArray(ArraySize=kMaxShadowPoint*6)。SRVはTextureCubeArrayとして参照 ---
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = kPointShadowMapSize;
            desc.Height = kPointShadowMapSize;
            desc.DepthOrArraySize = static_cast<u16>(kNumPointFaces);
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.SampleDesc = {1, 0};
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clearValue, IID_PPV_ARGS(&m_pointShadowMap)));

            for (u32 i = 0; i < kNumPointFaces; ++i)
            {
                m_pointShadowDsvHandles[i] = m_punctualShadowDsvHeap->Allocate();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.FirstArraySlice = i;
                dsvDesc.Texture2DArray.ArraySize = 1;
                m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                    m_pointShadowMap.Get(), &dsvDesc, m_pointShadowDsvHandles[i]);
            }

            // t9(スポット)の直後の連番であることをシェーダ側テーブル(t9,t10連続レンジ)が前提にしている。
            m_pointShadowSrvIndex = m_srvHeap->AllocateIndex();
            DX_ASSERT(m_pointShadowSrvIndex == m_spotShadowSrvIndex + 1,
                     "スポット/ポイント影SRVが連番でない（RootSigのt9-t10テーブル前提が崩れる）");
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.TextureCubeArray.MipLevels = 1;
            srvDesc.TextureCubeArray.First2DArrayFace = 0;
            srvDesc.TextureCubeArray.NumCubes = kMaxShadowPoint;
            m_graphicsDevice->GetDevice()->CreateShaderResourceView(
                m_pointShadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_pointShadowSrvIndex));
        }

        Logger::Info("Punctual shadow maps initialized (spot {}x{}x{}, point {}x{}x{})",
                    kSpotShadowMapSize, kSpotShadowMapSize, kMaxShadowSpot,
                    kPointShadowMapSize, kPointShadowMapSize, kMaxShadowPoint);
    }

    // PerFrame Constant Buffer（PointLight / SpotLight 各最大8灯対応）
    // レイアウトは shaders/forward/Lighting.hlsli の PerFrameConstants と完全一致させること。
    static constexpr u32 kMaxPointLights = 8;
    static constexpr u32 kMaxSpotLights  = 8;
    struct PointLightGPU {
        DirectX::XMFLOAT3 position;
        float range;
        DirectX::XMFLOAT3 color;  // color * intensity
        float shadowIndex;        // -1=影なし、それ以外はポイント影キューブ配列のインデックス
    };
    struct SpotLightGPU {
        DirectX::XMFLOAT3 position;   float range;
        DirectX::XMFLOAT3 direction;  float cosInner;
        DirectX::XMFLOAT3 color;      float cosOuter;  // color * intensity
        float shadowIndex; DirectX::XMFLOAT3 _spad;    // -1=影なし、それ以外は spotShadowMatrix[] のインデックス
    };
    struct FrameConstants {
        DirectX::XMFLOAT4X4 view;            // 64B  (offset   0)
        DirectX::XMFLOAT4X4 proj;            // 64B  (offset  64)
        DirectX::XMFLOAT3   lightDir;        // 12B
        float                time;            // 4B  → 16B (offset 128)
        DirectX::XMFLOAT3   lightColor;      // 12B
        float                ambientStrength; // 4B  → 16B (offset 144)
        DirectX::XMFLOAT4X4 cascadeViewProj[kNumCascades]; // 256B (offset 160)
        DirectX::XMFLOAT4   cascadeSplitsView; // 16B (offset 416)
        DirectX::XMFLOAT4   shadowParams;      // 16B (offset 432)
        DirectX::XMFLOAT3   cameraPos;       // 12B
        float                _pad;            // 4B  → 16B (offset 448)
        u32                  numPointLights;  // 4B
        u32                  numSpotLights;   // 4B
        float                spotShadowTexel; // 4B
        float                pointShadowNear; // 4B  → 16B (offset 464)
        PointLightGPU        pointLights[kMaxPointLights]; // 256B (offset 480)
        SpotLightGPU         spotLights[kMaxSpotLights];   // 512B (offset 736)
        DirectX::XMFLOAT4X4  spotShadowMatrix[kMaxShadowSpot]; // 256B (offset 1248)
        // ▼ IBL 制御 16B (offset 1504)
        float                iblIntensity;
        float                maxPrefilterMip;
        u32                  hasIBL;
        float                skyboxIntensity;
    };  // total = 1520B
    static_assert(sizeof(FrameConstants) == 1520, "FrameConstants must be 1520 bytes");
    m_perFrameCB = std::make_unique<ConstantBuffer>();
    m_perFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // カメラプレビュー用の per-frame CB（メインパスと別バッファ。同一フレーム内で
    // 別視点を描くため m_perFrameCB を上書きできない）
    m_previewFrameCB = std::make_unique<ConstantBuffer>();
    m_previewFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // CommandList ラッパー
    m_commandList = std::make_unique<CommandList>();

    // ImGui 初期化
    SplashScreen::SetStatus("エディタUIを初期化中...");
    m_imguiManager = std::make_unique<ImGuiManager>();
    m_imguiManager->Initialize(
        m_window->GetHwnd(), *m_graphicsDevice, m_commandQueue->GetQueue(),
        *m_srvHeap, m_swapChain->GetFormat(), FrameResources::kFrameCount);

    // EditorLayer 初期化
    m_editorLayer = std::make_unique<EditorLayer>();
    m_editorLayer->Initialize(m_editorCtx.get(), PathResolver::AssetsDir(),
                              PathResolver::ScriptsDir(),
                              m_resourceManager.get(), m_srvHeap.get());

    // ModelThumbnailRenderer 初期化
    m_thumbRenderer = std::make_unique<ModelThumbnailRenderer>();
    m_thumbRenderer->Initialize(m_graphicsDevice.get(), m_srvHeap.get(),
                                m_resourceManager.get(), m_rootSignature.get(),
                                m_pipelineStateThumb.get());
    m_thumbRenderer->SetAOWhiteSrv(m_ssaoWhiteSrvIndex);  // forward PS の t8 を白ダミーで満たす
    m_editorLayer->SetThumbnailRenderer(m_thumbRenderer.get());

    // Physics Debug Renderer
    m_physicsDebugRenderer = std::make_unique<PhysicsDebugRenderer>();
    m_physicsDebugRenderer->Initialize(*m_graphicsDevice,
        kSceneColorFormat, DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

    m_editorIconRenderer = std::make_unique<EditorIconRenderer>();
    m_editorIconRenderer->Initialize(*m_graphicsDevice,
        m_swapChain->GetFormat(), DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

    // オフスクリーン描画用 RT + ポストプロセス（WP3）
    SplashScreen::SetStatus("レンダラーを初期化中...");
    {
        // 容量 32: sceneRT(1)+cameraPreview(2)+SSAO(2)+ブルームチェーン(6) = 11 使用。
        // 今後のポスト追加パス（ゴッドレイ/DoF 等）用に余裕を持たせる（RTV は非シェーダ可視で安価）。
        m_offscreenRtvHeap = std::make_unique<DescriptorHeap>();
        m_offscreenRtvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 32, false);

        // シーンは HDR(kSceneColorFormat) の中間RTへ描き、ポストで backbuffer へ解決する。
        // クリア色はリニア空間の値（最終段のACES+ガンマ後にコーンフラワーブルーに見える値）
        const float sceneClear[4] = {0.127f, 0.306f, 0.850f, 1.0f};
        m_sceneRT = std::make_unique<RenderTarget>();
        m_sceneRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                              m_window->GetWidth(), m_window->GetHeight(),
                              kSceneColorFormat, sceneClear);

        // カメラプレビュー RT（固定 16:9・小サイズ。選択カメラ視点をここへ描いて小窓表示）
        m_cameraPreviewRT = std::make_unique<RenderTarget>();
        m_cameraPreviewRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                      480, 270, kSceneColorFormat, sceneClear);

        // プレビュー表示用 LDR RT。プレビューRT(リニアHDR)をトーンマップして解決し、
        // ImGui にはこちらの SRV を渡す（FP16 を直接表示すると暗く見えるため）
        m_cameraPreviewLdrRT = std::make_unique<RenderTarget>();
        m_cameraPreviewLdrRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                         480, 270, DXGI_FORMAT_R8G8B8A8_UNORM, sceneClear);

        m_postProcess = std::make_unique<PostProcess>();
        m_postProcess->Initialize(*m_graphicsDevice, m_swapChain->GetFormat(), PathResolver::ShaderDirW(),
                                  FrameResources::kFrameCount);

        // 物理ベースブルーム（シーンHDR → 半解像度 6 段のダウン/アップサンプルチェーン）
        m_bloomPass = std::make_unique<BloomPass>();
        m_bloomPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());

        // 自動露出（compute ヒストグラム。露出値は GPU 内バッファで uber パスへ直結）
        m_autoExposure = std::make_unique<AutoExposurePass>();
        m_autoExposure->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());

        // ゴッドレイ / レンズフレア / DoF / モーションブラー（全て設定でOFF時はゼロコスト）
        m_godRaysPass = std::make_unique<GodRaysPass>();
        m_godRaysPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                  m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());
        m_lensFlarePass = std::make_unique<LensFlarePass>();
        m_lensFlarePass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                    m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());
        m_dofPass = std::make_unique<DofPass>();
        m_dofPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                              m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());
        m_motionBlurPass = std::make_unique<MotionBlurPass>();
        m_motionBlurPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                     m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());

        // SSAO（深度プリパス → 半球カーネル AO → ブラー）。AO/Blur RT は offscreenRtvHeap から確保。
        m_ssaoPass = std::make_unique<SSAOPass>();
        m_ssaoPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                               m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());

        // 2D スプライト（バックバッファ＝スワップチェイン形式へ描く）
        m_spriteRenderer = std::make_unique<SpriteRenderer>();
        m_spriteRenderer->Initialize(*m_graphicsDevice, m_srvHeap.get(),
                                     m_swapChain->GetFormat(), PathResolver::ShaderDirW());
        // ワールド空間 2D（Sprite2D, worldSpace=true）: HDR scene RT へ描く別経路（HUD と隔離）
        // 深度バッファ(D32_FLOAT)に対して深度テスト＝3D形状に正しく遮蔽される。
        m_spriteRenderer->InitializeWorld(*m_graphicsDevice, kSceneColorFormat,
                                          DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

        // パーティクル歪みバッファ（熱ゆらぎ/衝撃波が画面を歪ませる。RG=UVオフセット）
        const float distortClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_distortRT = std::make_unique<RenderTarget>();
        m_distortRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                m_window->GetWidth(), m_window->GetHeight(),
                                DXGI_FORMAT_R16G16_FLOAT, distortClear);

        // 加算ビルボードパーティクル（HDR scene RT + 深度へ描く / Lua fx API）
        m_particleSystem = std::make_unique<ParticleSystem>();
        m_particleSystem->Initialize(*m_graphicsDevice, kSceneColorFormat,
                                     DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                     PathResolver::ShaderDirW(),
                                     m_srvHeap.get(), m_resourceManager.get());
        if (m_scriptEngine) m_scriptEngine->SetParticleSystem(m_particleSystem.get());

        // GPUパーティクル（compute シム + ExecuteIndirect。最大 131072 粒子・加算専用）
        m_gpuParticles = std::make_unique<GpuParticleSystem>();
        m_gpuParticles->Initialize(*m_graphicsDevice, kSceneColorFormat, PathResolver::ShaderDirW());
        if (m_scriptEngine) m_scriptEngine->SetGpuParticleSystem(m_gpuParticles.get());

        // パーティクルエディタ（ツール窓）。専用のオフスクリーンプレビュー(独立した ParticleSystem
        // インスタンス + RenderTarget)を持つ。ゲーム(封印ランタイム)では作らない。
        if (!m_isGameMode)
        {
            m_vfxEditorPanel = std::make_unique<VfxEditorPanel>();
            m_vfxEditorPanel->Initialize(*m_graphicsDevice, m_srvHeap.get(), m_resourceManager.get(),
                                        PathResolver::ShaderDirW());
            m_networkPanel = std::make_unique<NetworkPanel>();
        }

        // シーントランジション
        m_sceneTransition = std::make_unique<SceneTransition>();
        m_sceneTransition->Initialize(*m_graphicsDevice, m_swapChain->GetFormat(), PathResolver::ShaderDirW());

        // IBL 環境マップ（irradiance/prefiltered/BRDF LUT）+ 任意スカイボックス
        m_iblBaker = std::make_unique<IBLBaker>();
        m_iblBaker->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());
        m_skyboxRenderer = std::make_unique<SkyboxRenderer>();
        m_skyboxRenderer->Initialize(*m_graphicsDevice, kSceneColorFormat, PathResolver::ShaderDirW());

        // シェーダーホットリロードの再生成コールバックを束ねる。ここまでで全パスの初回 PSO が
        // 揃っているので、この時点(Initialize 末尾付近)で一括登録する。
        RegisterShaderReloadHandlers();
    }

    // シーンフロー（assets/sceneflow.json があれば）
    m_sceneFlow = std::make_unique<SceneFlow>();
    m_sceneFlow->Load(PathResolver::AssetsDir() + "sceneflow.json");

    m_isRunning = true;

    // ゲームモードの場合、即座にPlayモードに入る
    if (m_isGameMode)
    {
        m_pendingMode = EngineMode::Playing;
        m_modeChangeRequested = true;
    }

    // マルチプレイ テストクライアント起動(フェーズ⑨、--net-client "ip[:port]")。
    // ランチャーを飛ばして --project のプロジェクトを直接開き、ロード完了後(Update内)に
    // クライアントとして自動Play=Joinする。ip/port は EnterPlayMode の自動接続が参照する。
    if (!m_isGameMode && !m_pendingNetClientJoin.empty())
    {
        std::string ip = m_pendingNetClientJoin;
        if (size_t c = ip.rfind(':'); c != std::string::npos)
        {
            m_editorCtx->netTestJoinPort = static_cast<u16>(std::atoi(ip.c_str() + c + 1));
            ip.resize(c);
        }
        if (!ip.empty()) m_editorCtx->netTestJoinAddress = ip;
        m_editorCtx->netTestRole = NetTestRole::Client;

        if (!m_pendingNetClientProject.empty())
        {
            ProjectInfo info;
            if (ProjectManager::ProjectFromFolder(m_pendingNetClientProject, info))
                BeginProjectLoad(info, /*isNew=*/false);   // m_showLauncher=false もここで立つ
            else
                Logger::Warn("--project のプロジェクトが開けません: {}", m_pendingNetClientProject);
        }
        else
        {
            m_showLauncher = false;   // プロジェクト指定なし=既に読み込んだ最後のシーンのまま参加
        }
        m_netClientAutoPlayPending = true;
        m_pendingNetClientJoin.clear();
    }

    // 全モデルのサムネイルを起動時にロード/レンダリング（エディタ専用機能）。
    // ゲーム(封印ランタイム)では実行しない＝起動時に exe 隣へ assets/.thumbcache/ を作らない。
    if (!m_isGameMode)
    {
        size_t uncachedCount = m_thumbRenderer->ScanAllModels(PathResolver::AssetsDir());
        size_t cachedCount   = m_thumbRenderer->GetCachedCount();
        size_t totalModels   = uncachedCount + cachedCount;

        if (totalModels > 0)
        {
            // Phase 1: ディスクキャッシュから一括ロード（高速）
            if (cachedCount > 0)
            {
                auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
                m_thumbRenderer->LoadCachedThumbnails(cmdList);
                ThrowIfFailed(cmdList->Close());
                m_commandQueue->ExecuteCommandList(cmdList);
                m_commandQueue->WaitIdle();
                m_frameResources->EndFrame(*m_commandQueue);
                Logger::Info("[Thumbnail] Cache loaded: {} models", cachedCount);
            }

            // Phase 2: 未キャッシュのみレンダリング（進捗表示付き）
            if (uncachedCount > 0)
            {
                size_t completed = 0;

                while (m_thumbRenderer->GetPendingCount() > 0)
                {
                    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
                    m_commandList->Wrap(cmdList);

                    m_thumbRenderer->RenderNext(cmdList);
                    ++completed;

                    // ローディング画面をバックバッファに描画
                    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
                    auto rtvHandle = m_descriptorHeap->GetCpuHandle(
                        m_swapChain->GetCurrentBackBufferIndex());

                    m_commandList->TransitionResource(backBuffer,
                        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

                    float clearColor[4] = {0.08f, 0.08f, 0.10f, 1.0f};
                    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                    D3D12_VIEWPORT vp = {0, 0,
                        static_cast<f32>(m_window->GetWidth()),
                        static_cast<f32>(m_window->GetHeight()), 0, 1};
                    D3D12_RECT scissor = {0, 0,
                        static_cast<LONG>(m_window->GetWidth()),
                        static_cast<LONG>(m_window->GetHeight())};
                    cmdList->RSSetViewports(1, &vp);
                    cmdList->RSSetScissorRects(1, &scissor);

                    float progress = static_cast<float>(completed) / static_cast<float>(uncachedCount);
                    m_imguiManager->BeginFrame();
                    float dispW = static_cast<float>(m_window->GetWidth());
                    float dispH = static_cast<float>(m_window->GetHeight());
                    ImGui::SetNextWindowPos(ImVec2(dispW * 0.5f, dispH * 0.5f),
                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
                    ImGui::Begin("##Loading", nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
                    ImGui::Text("DX12 Engine");
                    ImGui::Separator();
                    ImGui::Text("Rendering thumbnails... (%zu / %zu)", completed, uncachedCount);
                    ImGui::ProgressBar(progress, ImVec2(-1, 24));
                    ImGui::End();
                    m_imguiManager->EndFrame(cmdList);

                    m_commandList->TransitionResource(backBuffer,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                    m_commandList->Close();
                    m_commandQueue->ExecuteCommandList(cmdList);
                    m_swapChain->Present(false);
                    m_frameResources->EndFrame(*m_commandQueue);

                    m_commandQueue->WaitIdle();

                    // レンダリング結果をディスクキャッシュに保存
                    m_thumbRenderer->SavePendingCache();

                    MSG msg;
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
            }

            m_resourceManager->FinishUploads();
        }
    }

    // IBL: シーンの skybox 設定に応じて環境キューブを読み込み派生をベイク（専用 cmdList）。
    {
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
        LoadSkyboxIfNeeded(cmdList);
        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();
        m_frameResources->EndFrame(*m_commandQueue);
        if (m_envCubeTex) m_envCubeTex->FinishUpload();
        m_resourceManager->FinishUploads();
    }

    // ここから先(メインループ)は WaitIdle 無しでフレームを多重化するため、
    // GPUリソースの解放をフェンス連動の遅延解放に切り替える
    DeferredRelease::Enable();

    // メインウィンドウはここではまだ表示しない。Run() の先頭数フレームを隠れたまま描画し、
    // ランチャーの初回描画・ImGuiフォント・ドライバのPSOウォームアップを済ませてから表示する
    // （「白いウィンドウが出てから絵が出るまで」のフリーズ見えを根絶。表示タイミングが決定的になる）。
    // deferMainWindow でない経路（ゲーム/ヘッドレスビルド）はウィンドウが既に表示済み。
    m_deferredFirstShow = deferMainWindow;
    if (deferMainWindow)
        SplashScreen::SetStatus("画面を準備しています...");

    Logger::Info("Application initialized successfully");

    // AI(MCP)ブリッジ。エディタ時のみ。ゲーム(封印ランタイム)では起動しない＝外部から触れない。
    // ヘッドレス --build でも起動しない: 起動中エディタが 8787 を握っている状態で build が
    // 走ると 8788 に bind→WritePortFile が %TEMP%/dx12_mcp.port を 8788 で上書きし、build 終了で
    // その port が死ぬ＝Node 側の自動検出が死にポートを掴みライブツールが切れる原因になる。
    if (!m_isGameMode && !buildMode)
    {
        m_mcpBridge = std::make_unique<McpBridge>();
        m_mcpBridge->Start(8787);   // ponytail: ポート固定。衝突したら env/引数化する。
    }
}

namespace
{
// MCP set_component / remove_component 共有の小スイッチ。
// jsonKey(get_entity が返すキー = deserialize が見るキー) を対応する型へ写して reg.remove<T>(e)。
// SceneSerializer の RegisterCoreComponentSerializers 登録済みコア部品に限定。
// 未対応キーは false(呼び側が error を返す)。meshRenderer はメッシュ所有整合が要るため非対応。
bool RemoveRegisteredComponent(entt::registry& reg, entt::entity e, const std::string& key)
{
    if      (key == "pointLight")          reg.remove<PointLight>(e);
    else if (key == "directionalLight")    reg.remove<DirectionalLight>(e);
    else if (key == "spotLight")           reg.remove<SpotLight>(e);
    else if (key == "camera")              reg.remove<CameraComponent>(e);
    else if (key == "rigidBody")           reg.remove<RigidBody>(e);
    else if (key == "boxCollider")         reg.remove<BoxCollider>(e);
    else if (key == "sphereCollider")      reg.remove<SphereCollider>(e);
    else if (key == "capsuleCollider")     reg.remove<CapsuleCollider>(e);
    else if (key == "characterController") reg.remove<CharacterController>(e);
    else if (key == "tags")                reg.remove<Tag>(e);
    else if (key == "data")                reg.remove<DataComponent>(e);
    else if (key == "sprite2d")            reg.remove<Sprite2D>(e);
    else if (key == "audioSource")         reg.remove<AudioSource>(e);
    else if (key == "particleEmitter")     reg.remove<ParticleEmitter>(e);
    else if (key == "trigger")             reg.remove<Trigger>(e);
    else if (key == "gimmick")             reg.remove<Gimmick>(e);
    else if (key == "convexHullCollider")  reg.remove<ConvexHullCollider>(e);
    else if (key == "luaScript")           reg.remove<LuaScript>(e);
    else if (key == "trailRenderer")       reg.remove<TrailRenderer>(e);
    else if (key == "networkIdentity")     reg.remove<NetworkIdentity>(e);
    else if (key == "networkTransform")    reg.remove<NetworkTransform>(e);
    else return false;
    return true;
}

// float3 を JSON 配列から読む小ヘルパ(SceneSerializer の DeserializeFloat3 相当・Application 内版)。
DirectX::XMFLOAT3 McpF3(const nlohmann::json& j, DirectX::XMFLOAT3 def = {0.0f, 0.0f, 0.0f})
{
    if (j.is_array() && j.size() >= 3)
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    return def;
}

// レジストリ未登録(orphan)コンポーネントを set_component から適用する。
// SceneSerializer の instantiate 側 deserialize と同じキー/既定値で emplace_or_replace するので
// save/load 経路には一切触れない(=シリアライズ回帰リスクゼロ)。対応外キーは false。
bool ApplyOrphanComponent(entt::registry& reg, entt::entity e,
                          const std::string& comp, const nlohmann::json& d)
{
    if (comp == "gimmick")
    {
        Gimmick gm;
        gm.kind = d.value("kind", 0); gm.period = d.value("period", 4.0f);
        gm.phase = d.value("phase", 0.0f); gm.amplitude = d.value("amplitude", 1.6f);
        gm.threshold = d.value("threshold", 0.5f); gm.solid = d.value("solid", true);
        gm.deadly = d.value("deadly", false);
        reg.emplace_or_replace<Gimmick>(e, gm);
        return true;
    }
    if (comp == "audioSource")
    {
        AudioSource as;
        as.clipPath = d.value("clipPath", std::string{}); as.volume = d.value("volume", 1.0f);
        as.loop = d.value("loop", false); as.spatial = d.value("spatial", true);
        as.playOnStart = d.value("playOnStart", true);
        as.minDistance = d.value("minDistance", 1.0f); as.maxDistance = d.value("maxDistance", 30.0f);
        reg.emplace_or_replace<AudioSource>(e, std::move(as));
        return true;
    }
    if (comp == "particleEmitter")
    {
        ParticleEmitter pe;
        pe.kind = d.value("kind", 0); pe.blend = d.value("blend", 0); pe.rate = d.value("rate", 30.0f);
        pe.playOnStart = d.value("playOnStart", true); pe.looping = d.value("looping", true);
        pe.duration = d.value("duration", 1.0f);
        if (d.contains("dir")) pe.dir = McpF3(d["dir"], {0.0f, 1.0f, 0.0f});
        pe.spread = d.value("spread", 0.4f); pe.speed = d.value("speed", 3.0f);
        pe.speedVar = d.value("speedVar", 0.4f); pe.size = d.value("size", 0.3f);
        pe.sizeEnd = d.value("sizeEnd", 0.0f); pe.life = d.value("life", 0.8f);
        pe.lifeVar = d.value("lifeVar", 0.3f);
        if (d.contains("color"))    pe.color    = McpF3(d["color"], {1.0f, 0.6f, 0.2f});
        if (d.contains("colorEnd")) pe.colorEnd = McpF3(d["colorEnd"], {1.0f, 0.12f, 0.05f});
        if (d.contains("colorMid")) { pe.colorMid = McpF3(d["colorMid"], {1.0f, 0.6f, 0.2f}); pe.hasColorMid = true; }
        pe.hasColorMid = d.value("hasColorMid", pe.hasColorMid);
        pe.intensity = d.value("intensity", 3.0f); pe.gravity = d.value("gravity", 0.0f);
        pe.drag = d.value("drag", 1.0f); pe.up = d.value("up", 0.0f); pe.stretch = d.value("stretch", 0.0f);
        pe.turbStrength = d.value("turbStrength", 0.0f); pe.turbFreq = d.value("turbFreq", 1.0f);
        pe.sizeMid = d.value("sizeMid", -1.0f); pe.distort = d.value("distort", 0.0f);
        pe.light = d.value("light", false); pe.lightRange = d.value("lightRange", 3.0f);
        pe.flicker = d.value("flicker", 0.0f); pe.flickerFreq = d.value("flickerFreq", 18.0f);
        pe.gpu = d.value("gpu", false);
        pe.texturePath = d.value("texturePath", std::string{});
        reg.emplace_or_replace<ParticleEmitter>(e, pe);
        return true;
    }
    if (comp == "trigger")
    {
        Trigger tr;
        tr.shape = d.value("shape", 0);
        if (d.contains("halfExtents")) tr.halfExtents = McpF3(d["halfExtents"], {1.0f, 1.0f, 1.0f});
        tr.radius = d.value("radius", 1.0f);
        if (d.contains("offset")) tr.offset = McpF3(d["offset"]);
        tr.filter = d.value("filter", std::string{}); tr.once = d.value("once", false);
        if (d.contains("actions") && d["actions"].is_array())
        {
            for (const auto& aj : d["actions"])
            {
                TriggerAction a;
                a.when = aj.value("when", 0); a.type = aj.value("type", 0);
                a.target = aj.value("target", std::string{}); a.str = aj.value("str", std::string{});
                a.num = aj.value("num", 0.0);
                if (aj.contains("vec")) a.vec = McpF3(aj["vec"]);
                tr.actions.push_back(std::move(a));
            }
        }
        reg.emplace_or_replace<Trigger>(e, std::move(tr));
        return true;
    }
    return false;
}

// MCP エラーコード（Node 側が JSON-RPC コードへ写像し、AI が分類/回復に使う）。
namespace McpErr
{
    constexpr int InvalidParam     = 2;  // 引数不正（既定: 検証エラーはこれ）
    constexpr int ModeConflict     = 3;  // Editor/Playing が要件と合わない
    constexpr int StaleScene       = 4;  // sceneGeneration 不一致（再読込後の古い id）
    constexpr int NotFound         = 1;  // entity / scene / asset が無い
    constexpr int UnknownComponent = 6;  // 未対応コンポーネント jsonKey
    constexpr int Internal         = 7;  // エンジン内部エラー
}

// error_code を運べる例外。HandleMcpCommand の catch で resp へ写す。
struct McpError : std::runtime_error
{
    int code;
    McpError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

// MCP のエンティティ指定を解決する。params["name"](完全一致 FindEntity) を優先し、
// 無ければ params["entity"](数値 id) を検証して返す。どちらも解決できなければ NotFound を投げる。
// ※ コンポーネント有無は見ない(呼び出し側で all_of を別に確認 → エラー文を分けるため)。
entt::entity ResolveMcpEntity(Scene& scene, const nlohmann::json& params)
{
    auto it = params.find("name");
    if (it != params.end() && it->is_string())
    {
        auto ent = scene.FindEntity(it->get<std::string>());
        if (ent.IsValid()) return ent.GetHandle();
        throw McpError(McpErr::NotFound, "no entity named '" + it->get<std::string>() + "'");
    }
    auto e = static_cast<entt::entity>(params.value("entity", 0xFFFFFFFFu));
    if (scene.GetRegistry().valid(e)) return e;
    // 数値 id が無効: Stop/open_scene で世代が変わると古い id はここに来る。再取得を促す。
    throw McpError(McpErr::NotFound,
        "invalid entity id (Stop/シーン再読込で id は変わる。dx12_list_entities で取り直すか name 指定で操作してくれ)");
}

// MCP key_* 用。params["key"] を Win32 VK コードに解決する。数値(VK そのもの)か、
// 文字列(1文字 A-Z/0-9 or "SPACE"/"SHIFT"/"TAB"/"ESC"/"ENTER"/"UP"/"DOWN"/"LEFT"/"RIGHT"/"F1".."F12")。
// Lua の KEY_* と同じ割り当て(ScriptEngine.cpp)。
int ParseMcpVk(const nlohmann::json& params)
{
    auto it = params.find("key");
    if (it == params.end()) throw McpError(McpErr::InvalidParam, "missing 'key'");
    if (it->is_number_integer())
    {
        int vk = it->get<int>();
        if (vk < 0 || vk > 255) throw McpError(McpErr::InvalidParam, "key (VK) must be 0..255");
        return vk;
    }
    if (!it->is_string()) throw McpError(McpErr::InvalidParam, "key must be a VK int or a key name string");
    std::string s = it->get<std::string>();
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (s.size() == 1)
    {
        char c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<int>(c);
    }
    if (s == "SPACE")  return VK_SPACE;
    if (s == "SHIFT")  return VK_SHIFT;
    if (s == "CTRL" || s == "CONTROL") return VK_CONTROL;
    if (s == "ALT")    return VK_MENU;
    if (s == "TAB")    return VK_TAB;
    if (s == "ESC" || s == "ESCAPE")   return VK_ESCAPE;
    if (s == "ENTER" || s == "RETURN") return VK_RETURN;
    if (s == "UP")     return VK_UP;
    if (s == "DOWN")   return VK_DOWN;
    if (s == "LEFT")   return VK_LEFT;
    if (s == "RIGHT")  return VK_RIGHT;
    if (s[0] == 'F' && (s.size() == 2 || s.size() == 3))   // F1..F12
    {
        int n = (s.size() == 2) ? (s[1] - '0') : (s[1] - '0') * 10 + (s[2] - '0');
        if (n >= 1 && n <= 12) return VK_F1 + (n - 1);
    }
    throw McpError(McpErr::InvalidParam, "unknown key name: " + s);
}

// エンジン自身(自 exe)を子プロセスとして起動し、終了(または timeoutMs)まで待つ。
// dx12_validate_scene の --validate ヘッドレス実行に使う。--validate は main.cpp で
// GPU/ウィンドウ初期化より前に return するため、エディタ実行中でも安全に並行起動できる。
// 戻り値: 終了コード(起動失敗やタイムアウトは 1 = FAIL 扱い)。
int RunEngineSubprocessAndWait(const std::string& exePath, const std::string& args,
                               const std::string& workDir, DWORD timeoutMs)
{
    std::string cmd = "\"" + exePath + "\" " + args;
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok) return 1;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// 遅延応答の送信ヘルパ。client==0(=MCP 由来でない) は何もしない。
void SendMcp(McpBridge* bridge, const McpDeferred& d, nlohmann::json resp)
{
    if (!bridge || d.client == 0) return;
    resp["id"] = d.requestId;
    // 不正 UTF-8(CP932 のモデル名由来 NameTag 等)で dump が投げないよう replace。
    bridge->SendToClient(d.client,
        resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}
void CompleteMcp(McpBridge* bridge, const McpDeferred& d, nlohmann::json result)
{
    SendMcp(bridge, d, nlohmann::json{{"ok", true}, {"result", std::move(result)}});
}
void FailMcp(McpBridge* bridge, const McpDeferred& d, int code, const std::string& msg)
{
    SendMcp(bridge, d, nlohmann::json{{"ok", false}, {"error", msg}, {"error_code", code}});
}

// dx12_describe_components 用のコンポーネントスキーマ表。
// AI がフィールド名/型/既定値を推測せず set_component を正しく呼べるようにする。
// jsonKey は get_entity が返し set_component/remove_component が受けるキー。
// settable=set_component 可 / removable=remove_component 可。
// Lua コンポーネントスクリプトから entity プロパティとして直接読めるか。
// Entity usertype が公開しているデータプロパティは transform だけ(ScriptEngine.cpp の new_usertype<Entity>)。
// boxCollider/rigidBody など他は entity.<key> では nil になる(physics:getVelocity 等の別経路のみ)。
bool LuaReadableComponent(const std::string& jsonKey)
{
    return jsonKey == "transform";
}

nlohmann::json McpComponentSchema()
{
    using nlohmann::json;
    auto F = [](const char* name, const char* type, json def) {
        return json{{"name", name}, {"type", type}, {"default", std::move(def)}};
    };
    auto C = [](const char* key, bool settable, bool removable, json fields, const char* note = "") {
        json c{{"jsonKey", key}, {"settable", settable}, {"removable", removable},
               {"luaAccessible", LuaReadableComponent(key)}, {"fields", std::move(fields)}};
        if (note[0]) c["note"] = note;
        return c;
    };
    json comps = json::array();
    comps.push_back(C("transform", true, false, json::array({
        F("position", "float3", json::array({0, 0, 0})),
        F("rotation", "float3 (euler degrees)", json::array({0, 0, 0})),
        F("scale", "float3", json::array({1, 1, 1})),
        F("quaternion", "float4 (x,y,z,w)", json::array({0, 0, 0, 1})),
        F("useQuaternion", "bool", false),
    }), "core; cannot be removed. Prefer dx12_set_transform for position/rotation/scale."));
    comps.push_back(C("meshRenderer", false, false, json::array({
        F("modelPath", "string (assets-relative)", ""),
    }), "read-only via MCP; create with dx12_spawn_model/dx12_create_entity. Use dx12_set_pbr for material, "
        "dx12_set_mesh_shader for shaderPath (custom HLSL from dx12_create_shader)."));
    comps.push_back(C("pointLight", true, true, json::array({
        F("color", "float3", json::array({1, 1, 1})), F("intensity", "float", 1.0), F("range", "float", 10.0),
        F("castShadows", "bool (max 2 simultaneous, nearest-to-camera wins)", false),
    })));
    comps.push_back(C("directionalLight", true, true, json::array({
        F("direction", "float3", json::array({0, -1, 0})), F("color", "float3", json::array({1, 1, 1})),
        F("intensity", "float", 1.0), F("ambient", "float", 0.25),
    })));
    comps.push_back(C("spotLight", true, true, json::array({
        F("color", "float3", json::array({1, 1, 1})), F("intensity", "float", 3.0), F("range", "float", 15.0),
        F("direction", "float3", json::array({0, -1, 0})),
        F("innerConeDeg", "float", 18.0), F("outerConeDeg", "float", 28.0),
        F("castShadows", "bool (max 4 simultaneous, nearest-to-camera wins)", false),
    })));
    comps.push_back(C("camera", true, true, json::array({
        F("fovDegrees", "float", 60.0), F("nearClip", "float", 0.1), F("farClip", "float", 1000.0),
        F("isActive", "bool", false), F("projection", "int (0=Perspective,1=Orthographic)", 0),
        F("orthoSize", "float", 10.0),
    })));
    comps.push_back(C("rigidBody", true, true, json::array({
        F("motionType", "int (0=Static,1=Kinematic,2=Dynamic)", 2), F("mass", "float", 1.0),
        F("restitution", "float", 0.4), F("friction", "float", 0.3),
        F("linearDamping", "float", 0.02), F("angularDamping", "float", 0.01), F("useGravity", "bool", true),
    })));
    comps.push_back(C("boxCollider", true, true, json::array({
        F("halfExtents", "float3", json::array({0.5, 0.5, 0.5})), F("offset", "float3", json::array({0, 0, 0})),
    })));
    comps.push_back(C("sphereCollider", true, true, json::array({
        F("radius", "float", 0.5), F("offset", "float3", json::array({0, 0, 0})),
    })));
    comps.push_back(C("capsuleCollider", true, true, json::array({
        F("radius", "float", 0.5), F("halfHeight", "float", 1.0), F("offset", "float3", json::array({0, 0, 0})),
    })));
    comps.push_back(C("characterController", true, true, json::array({
        F("radius", "float", 0.4), F("halfHeight", "float", 0.6), F("offset", "float3", json::array({0, 0, 0})),
        F("mass", "float", 70.0), F("maxSlopeDeg", "float", 50.0), F("stepHeight", "float", 0.3),
        F("jumpSpeed", "float", 6.0), F("gravityScale", "float", 1.0),
    }), "mutually exclusive with rigidBody; do not add both."));
    comps.push_back(C("convexHullCollider", false, true, json::array({}),
        "auto-generated from mesh on load; not settable via MCP. Removable."));
    comps.push_back(C("sprite2d", true, true, json::array({
        F("texturePath", "string (assets-relative)", ""), F("layer", "int", 0),
        F("size", "float2", json::array({1, 1})), F("uvMin", "float2", json::array({0, 0})),
        F("uvMax", "float2", json::array({1, 1})), F("color", "float4 (rgba)", json::array({1, 1, 1, 1})),
        F("worldSpace", "bool", true), F("billboard", "bool", false),
        F("shaderPath", "string (assets-relative, worldSpace only)", ""), F("shaderAlphaBlend", "bool", false),
        F("effectValue", "float (generic progress/strength for custom shader)", 0.0),
    }), "Use dx12_set_sprite_shader for shaderPath (custom HLSL, world-space only; different vertex/root-"
        "signature contract than meshRenderer shaders, see docs/AUTHORING.md)."));
    comps.push_back(C("tags", true, true, json::array({}),
        "data is a STRING ARRAY, e.g. set_component(component='tags', data=[\"enemy\",\"boss\"])."));
    comps.push_back(C("data", true, true, json::array({}),
        "key->{t,v} map. t in number|bool|string|vec3 (int は number 扱い・get_entity は number で返す). e.g. data={\"hp\":{\"t\":\"number\",\"v\":100}}."));
    comps.push_back(C("audioSource", true, true, json::array({
        F("clipPath", "string (assets-relative)", ""), F("volume", "float", 1.0), F("loop", "bool", false),
        F("spatial", "bool", true), F("playOnStart", "bool", true),
        F("minDistance", "float", 1.0), F("maxDistance", "float", 30.0),
    })));
    comps.push_back(C("particleEmitter", true, true, json::array({
        F("kind", "int (0=Glow,1=Fire,2=Smoke,3=Spark,4=Magic,5=Electric,6=Ring,7=Star)", 0),
        F("blend", "int (0=Additive,1=Alpha)", 0), F("rate", "float (per sec)", 30.0),
        F("playOnStart", "bool", true), F("looping", "bool", true), F("duration", "float", 1.0),
        F("dir", "float3", json::array({0, 1, 0})), F("spread", "float", 0.4), F("speed", "float", 3.0),
        F("speedVar", "float", 0.4), F("size", "float", 0.3), F("sizeEnd", "float", 0.0),
        F("life", "float", 0.8), F("lifeVar", "float", 0.3),
        F("color", "float3", json::array({1, 0.6, 0.2})), F("colorEnd", "float3", json::array({1, 0.12, 0.05})),
        F("intensity", "float", 3.0), F("gravity", "float", 0.0), F("drag", "float", 1.0),
        F("up", "float", 0.0), F("stretch", "float", 0.0),
        F("colorMid", "float3 (set implies hasColorMid=true)", json::array({1, 0.6, 0.2})),
        F("hasColorMid", "bool (3-key color curve start→mid→end)", false),
        F("turbStrength", "float (>0 = curl-noise turbulence for smoke/fire)", 0.0),
        F("turbFreq", "float", 1.0),
        F("sizeMid", "float (>=0 = 3-key size curve)", -1.0),
        F("distort", "float (>0 = heat-haze/shockwave distortion particles)", 0.0),
        F("light", "bool (brightest N particles become real point lights)", false),
        F("lightRange", "float", 3.0),
        F("flicker", "float (0..1 emissive flicker)", 0.0), F("flickerFreq", "float", 18.0),
        F("gpu", "bool (GPU compute particles, max 131072, additive only; distort/light/sizeMid/alpha-blend unsupported)", false),
        F("texturePath", "string (assets-relative; empty = procedural look)", ""),
    })));
    comps.push_back(C("trailRenderer", true, true, json::array({
        F("emitting", "bool", true), F("width", "float (world units)", 0.25),
        F("life", "float (sec = ribbon length)", 0.5),
        F("color", "float3", json::array({0.4, 0.8, 1.0})), F("colorEnd", "float3", json::array({0.1, 0.2, 1.0})),
        F("intensity", "float (HDR, >1 blooms)", 2.0), F("blend", "int (0=Additive,1=Alpha)", 0),
        F("minDist", "float (min movement to drop a point)", 0.03),
    }), "camera-facing ribbon trail (sword slash / projectile / magic tail). Follows the entity's world position."));
    comps.push_back(C("networkIdentity", true, true, json::array({
        F("interestRadius", "float (0 = always relevant, no distance culling)", 0.0),
        F("serverAuthority", "bool", true),
    }), "marks the entity for multiplayer replication (host assigns netId). Pair with networkTransform. "
        "Use dx12_net_setup + dx12_play to test."));
    comps.push_back(C("networkTransform", true, true, json::array({
        F("syncMode", "int (0=interpolated proxy, 1=owner-predicted)", 0),
        F("sendRate", "float Hz (reserved)", 20.0),
        F("syncPosition", "bool", true), F("syncRotation", "bool", true), F("syncScale", "bool", false),
        F("interpDelayMs", "float (jitter buffer)", 100.0), F("snapDistance", "float (teleport threshold)", 5.0),
    }), "replicates Transform snapshots. Requires networkIdentity on the same entity."));
    comps.push_back(C("skeletalAnimation", false, false, json::array({}),
        "read-only via MCP (created by model load). Control playback with dx12_play_anim / dx12_get_anim_state."));
    comps.push_back(C("trigger", true, true, json::array({
        F("shape", "int (0=Box,1=Sphere)", 0), F("halfExtents", "float3", json::array({1, 1, 1})),
        F("radius", "float", 1.0), F("offset", "float3", json::array({0, 0, 0})),
        F("filter", "string (entity name; empty=Player)", ""), F("once", "bool", false),
        F("actions", "array of {when:int(0=Enter,1=Exit,2=Stay), type:int(0..10), target:string, str:string, num:number, vec:float3}", json::array()),
    })));
    comps.push_back(C("gimmick", true, true, json::array({
        F("kind", "int (0=StaticWall,1=SpikePulse,2=SlideX,3=SlideZ)", 0), F("period", "float", 4.0),
        F("phase", "float (0..1)", 0.0), F("amplitude", "float", 1.6), F("threshold", "float (0..1)", 0.5),
        F("solid", "bool", true), F("deadly", "bool", false),
    })));
    comps.push_back(C("luaScript", false, true, json::array({
        F("scriptPath", "string (assets-relative)", ""), F("enabled", "bool", true),
    }), "attach via dx12_attach_lua_component (not set_component). Removable via MCP."));
    return json{{"components", std::move(comps)}};
}

// dx12_describe_lua_api 用。Lua コンポーネントスクリプトから使えるバインディングの静的辞書。
// 実体は ScriptEngine.cpp の new_usertype/グローバル(ハンドコード)。ここは「何が呼べるか」を
// バインディングオブジェクトごとに列挙するだけ(ランタイムリフレクションはしない)。MCP 上で見える
// コンポーネントと Lua から読める API のズレ(例: entity.boxCollider は nil)を AI に明示するのが目的。
nlohmann::json McpLuaApi()
{
    using nlohmann::json;
    auto O = [](const char* name, const char* obtainedBy, json members) {
        json o{{"name", name}, {"members", std::move(members)}};
        if (obtainedBy[0]) o["obtainedBy"] = obtainedBy;
        return o;
    };
    json objects = json::array();
    objects.push_back(O("callbacks", "(各 Lua コンポーネントが任意で定義)", json::array({
        "OnStart(self)       — Play開始/アタッチ時に1回。第1引数は self(table)",
        "OnUpdate(self, dt)  — 毎フレーム。第1引数 self、第2引数 dt(秒)。コンポーネントは self が必須",
        "注: グローバル(シーン)スクリプトは OnUpdate(dt)(self 無し)。コンポーネントは OnUpdate(self, dt)",
    })));
    objects.push_back(O("entity", "scene:findEntity(name) / scene:spawn* / physics:overlap*", json::array({
        "isValid() -> bool",
        "name  (string, read-only property)",
        "transform  (Transform getter。フィールドは書込可: entity.transform.position = Vec3.new(x,y,z)。ただし entity.transform 自体の再代入は read-only) — 唯一直接読めるコンポーネントデータ",
        "hasComponent(type:string) -> bool  (type: Transform,MeshRenderer,SkeletalAnimation,NodeAnimation,GridPlane,PointLight,DirectionalLight,SpotLight,Camera,AudioSource,Gimmick,ParticleEmitter,Trigger,CharacterController)",
        "playAnim(clipIndex:int, blend:float)",
        "playAnimByName(name:string, blend:float)",
        "setLooping(loop:bool)",
        "getAnimCount() -> int",
        "getAnimName(index:int) -> string",
    })));
    objects.push_back(O("transform", "entity.transform / self.transform", json::array({
        "position  (Vec3, 読み書き)", "rotation  (Vec3, euler degrees, 読み書き)", "scale  (Vec3, 読み書き)",
        "代入: tr.position = Vec3.new(x,y,z) も tr.position.x=… も可。tr 自体(entity.transform)の再代入は不可(read-only)",
    })));
    objects.push_back(O("Vec3", "Vec3.new(x,y,z)", json::array({"x", "y", "z"})));
    objects.push_back(O("self", "(各 Lua コンポーネントに自動で渡る)", json::array({
        "entity  (u32 id NUMBER — Entity usertype ではない。callable が要るなら scene:findEntity(self.name))",
        "name  (string)", "transform  (Transform)", "enabled  (bool)",
        "<宣言した properties の各値>  (dx12_get_lua_component_state で確認)",
    })));
    objects.push_back(O("scene", "global", json::array({
        "spawn(name,modelPath,pos,rot,scale) -> entity", "spawnBox(name,pos,rot,scale) -> entity",
        "spawnSphere(name,pos,radius) -> entity", "spawnPlane(name,pos,size,grid) -> entity",
        "remove(entity)", "getEntityCount() -> int", "findEntity(name) -> entity",
        "setUVScale(entity,u,v)", "setColor(entity,r,g,b)", "gimmicks() -> table",
        "setSpriteEffect(entity,value)  (Sprite2D.effectValue、カスタムシェーダー用)",
        "setSpriteAlpha(entity,alpha)  (Sprite2D不透明度0..1、半透明演出用)",
        "queryByTag(tag) -> table(names)", "queryInBox(minX,minZ,maxX,maxZ,tag?) -> table(names)",
    })));
    objects.push_back(O("input", "global", json::array({
        "isKeyDown(vk) -> bool", "isKeyPressed(vk) -> bool", "isAsyncKeyDown(vk) -> bool",
        "isMouseCaptured() -> bool", "isRightMouseDown() -> bool",
        "getMouseDeltaX() -> float", "getMouseDeltaY() -> float", "setMouseCapture(b)",
    })));
    objects.push_back(O("camera", "global", json::array({
        "getPosition()/setPosition(v)", "getYaw()/setYaw(f)", "getPitch()/setPitch(f)",
        "moveForward/moveRight/moveUp(amt)", "rotate(dx,dy)",
        "getMoveSpeed/setMoveSpeed", "getMouseSensitivity/setMouseSensitivity",
        "project(x,y,z) -> (u,v,visible)",
    })));
    objects.push_back(O("physics", "global", json::array({
        "autoCollider(e)", "addBoxCollider(e,hx,hy,hz)", "addSphereCollider(e,radius)",
        "addCapsuleCollider(e,radius,halfHeight)", "addRigidBody(e,motionType,mass)", "removeRigidBody(e)",
        "applyForce(e,vec3)", "applyImpulse(e,vec3)", "setVelocity(e,vec3)", "getVelocity(e) -> vec3",
        "setPosition(e,vec3)  ※DYNAMIC ボディ向け。KINEMATIC/STATIC は Transform 駆動なので entity.transform.position を直接書く",
        "raycast(origin,dir,maxDist) -> RaycastHit",
        "overlapBox(center,half,maxN?) -> {entity..}", "overlapSphere(center,radius,maxN?) -> {entity..}",
        "setGravity(vec3)", "setPaused(b)", "step(dt)",
        "addCharacterController(e,radius,halfHeight)", "move(e,vx,vz)", "jump(e,amount?)", "isGrounded(e) -> bool",
    })));
    objects.push_back(O("audio", "global", json::array({
        "playBGM(path)/stopBGM()/pauseBGM()/resumeBGM()", "playSFX(path)",
        "playSpatial(path,x,y,z,minD,maxD,vol?,loop?)", "stopAllSFX()",
        "setMasterVolume/setBGMVolume/setSFXVolume(v)", "getBGMList()/getSFXList() -> table",
    })));
    objects.push_back(O("time", "global ('.' で呼ぶ)", json::array({
        "time.now() -> float  — Play開始からの経過秒(タイムスケール適用済み)",
        "time.realtime() -> float  — 実時間の経過秒(スケール非適用)",
        "time.dt() -> float / time.realDt() -> float  — 今フレームの dt(スケール済み/実時間)",
        "time.frame() -> int  — フレームカウンタ",
        "time.getScale()/time.setScale(s)  — タイムスケール。0=ポーズ, 0.5=スローモ, 2=早送り。OnUpdate の dt 自体に掛かるので既存スクリプトは無改修で追従(物理/パーティクルは対象外)",
        "time.after(sec, fn) -> id  — sec秒後に fn を1回実行(スケール済み時間で進む)",
        "time.every(sec, fn) -> id  — sec秒ごとに fn を繰り返し実行",
        "time.cancel(id)  — after/every の解除。タイマーは Play 開始でクリア",
        "time.video.start(duration, {skipCost=1.0}?)/stop()/active()  — ステージ共有の\"ビデオ時計\"開始。ギミックは t=video.localTime(self) の純関数で動きを書く(決定論タイムライン)",
        "time.video.now()/duration()/remaining()/finished()  — 動画時間・残り時間(未startなら remaining=math.huge)。skip の消費も残り時間に反映",
        "time.video.skip(entOrName, ±sec) -> offset  — 対象だけ先送り/巻き戻し(オフセット±)。残り時間を |sec|*skipCost 自動消費",
        "time.video.localTime(entOrName) -> t / setOffset/getOffset  — 動画時間+個別オフセット。キーは self テーブル/名前文字列/数値id(名前優先、同名は同一時計)",
        "time.localTime(e)/skipEntity(e,±sec)/scaleEntity(e,s)/getEntityScale(e)/resetEntity(e)  — ビデオ時計と独立したエンティティ個別時計(0=停止、負=逆再生)",
        "charge.new(key, {max=2,rate=1,realtime=false}?) -> c  — 押しっぱなしチャージ計測(弓を引く等)。OnUpdate で c:update()、c:charging()/c:ratio()/c:value()、離した瞬間 c:released() がチャージ量を返す(他は nil)",
    })));
    objects.push_back(O("RaycastHit", "physics:raycast(...)", json::array({
        "hit() -> bool", "distance() -> float", "point() -> vec3", "normal() -> vec3",
    })));
    objects.push_back(O("ui", "global (':' で呼ぶ)", json::array({
        "ui:text(x,y,text,size?,r?,g?,b?,a?)", "ui:button(x,y,w,h,label) -> bool",
        "ui:image(x,y,w,h,path)", "ui:rect(x,y,w,h,r?,g?,b?,a?,rounding?)",
    })));
    objects.push_back(O("fx", "global (':' で呼ぶ)。座標/色キーは省略可(既定値あり)", json::array({
        "fx:burst{ x,y,z, count, size,sizeEnd, life,lifeVar, r,g,b, rEnd,gEnd,bEnd, rMid,gMid,bMid, intensity, kind, speed,spread, dx,dy,dz, gravity,drag,up, stretch, turbStrength,turbFreq, flicker } — 1発放出",
        "fx:ring{ ...burst と同じキー... } — リング状放出。サイズは radius/scale ではなく size",
        "kind: glow/fire/smoke/spark/magic/electric/ring/star（文字列 or 0..7）",
        "fx:beam{ x0,y0,z0, x1,y1,z1, width, r,g,b, intensity, life, kind } — kind: energy/electric/fire。座標は ax/bz ではなく x0..z1",
        "fx:pulse(amt?)  画面全体パルス  /  fx:clear()",
        "例: fx:burst{ x=p.x, y=p.y, z=p.z, kind=\"spark\", count=18, size=0.5, r=1, g=0.78, b=0.18 }  ← scale/radius は無効キー(黙って無視される)",
    })));
    objects.push_back(O("events", "global (Play 中のみ)", json::array({
        "events:on(name,fn) -> id", "events:off(id)", "events:emit(name,data?)", "events:clear()",
    })));
    objects.push_back(O("globals", "", json::array({
        "log(msg)", "saveNum(key,val)", "loadNum(key,default?) -> double",
        "loadScene(rel)", "nextScene()", "quit()", "fadeToScene(rel,dur?)", "transitionToScene(rel,type:int,dur?)",
        "ASSETS, SCREEN_W, SCREEN_H, KEY_*(VK codes), MOTION_STATIC/KINEMATIC/DYNAMIC",
    })));
    objects.push_back(O("prelude", "global (高レベルヘルパ)", json::array({
        "keyDown(name) -> bool / keyPressed(name) -> bool  (name: \"W\",\"SPACE\",\"ESC\" 等)",
        "actor(name,opts?) -> Actor", "cameraFollow/cameraTPS/cameraLockOn(...)",
        "goToScene(path,dur?)", "win(dur?)", "clamp(v,lo,hi)", "lerp(a,b,t)", "angleDelta(from,to)",
        "FX.explosion/shockwave/spark/...", "vfx.register(name,fn) / vfx.play(name,x,y,z,scale?)",
    })));
    return json{
        {"version", 1},
        {"note", "Lua コンポーネントから使えるバインディング一覧。重要: コンポーネントは transform を除き "
                 "entity.<key> では読めない(entity.boxCollider 等は nil)。collider/rigidBody の値は "
                 "physics:getVelocity(e) など別 API 経由。self.entity は数値 id で Entity usertype ではない。"
                 " コールバックは OnStart(self) / OnUpdate(self, dt)(コンポーネントは self 必須)。"
                 " 位置更新は entity.transform.position = Vec3.new(x,y,z)（KINEMATIC も Transform 駆動）。"
                 " スクリプトエラーは dx12_get_lua_component_state の errorMessage に出る(loadError=true のとき)。"},
        {"objects", std::move(objects)},
    };
}

// entity が持つコンポーネントの jsonKey 一覧(list_entities verbose / get_entity 概況)。
nlohmann::json McpComponentTypesOf(const entt::registry& reg, entt::entity e)
{
    nlohmann::json a = nlohmann::json::array();
    if (reg.all_of<Transform>(e))           a.push_back("transform");
    if (reg.all_of<MeshRenderer>(e))        a.push_back("meshRenderer");
    if (reg.all_of<PointLight>(e))          a.push_back("pointLight");
    if (reg.all_of<DirectionalLight>(e))    a.push_back("directionalLight");
    if (reg.all_of<SpotLight>(e))           a.push_back("spotLight");
    if (reg.all_of<CameraComponent>(e))     a.push_back("camera");
    if (reg.all_of<RigidBody>(e))           a.push_back("rigidBody");
    if (reg.all_of<BoxCollider>(e))         a.push_back("boxCollider");
    if (reg.all_of<SphereCollider>(e))      a.push_back("sphereCollider");
    if (reg.all_of<CapsuleCollider>(e))     a.push_back("capsuleCollider");
    if (reg.all_of<CharacterController>(e))  a.push_back("characterController");
    if (reg.all_of<ConvexHullCollider>(e))  a.push_back("convexHullCollider");
    if (reg.all_of<Sprite2D>(e))            a.push_back("sprite2d");
    if (reg.all_of<Tag>(e))                 a.push_back("tags");
    if (reg.all_of<DataComponent>(e))       a.push_back("data");
    if (reg.all_of<AudioSource>(e))         a.push_back("audioSource");
    if (reg.all_of<ParticleEmitter>(e))     a.push_back("particleEmitter");
    if (reg.all_of<Trigger>(e))             a.push_back("trigger");
    if (reg.all_of<Gimmick>(e))             a.push_back("gimmick");
    if (reg.all_of<LuaScript>(e))           a.push_back("luaScript");
    if (reg.all_of<TrailRenderer>(e))       a.push_back("trailRenderer");
    if (reg.all_of<NetworkIdentity>(e))     a.push_back("networkIdentity");
    if (reg.all_of<NetworkTransform>(e))    a.push_back("networkTransform");
    if (reg.all_of<SkeletalAnimation>(e))   a.push_back("skeletalAnimation");
    return a;
}
} // namespace

std::string Application::HandleMcpCommand(uint64_t client, const std::string& line)
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    json req;
    try { req = json::parse(line); }
    catch (const std::exception& e)
    {
        return json{{"id", nullptr}, {"ok", false}, {"error_code", McpErr::InvalidParam},
                    {"error", std::string("parse error: ") + e.what()}}.dump();
    }

    json resp;
    resp["id"] = req.value("id", json(nullptr));
    const std::string method = req.value("method", std::string());
    const json params = req.value("params", json::object());

    // 遅延応答(create/spawn/delete/open_scene/play/stop)の相関情報。
    // 該当ブランチで deferred=true にし、保留キューへ mcp を積んで空文字列を返す。
    McpDeferred deferred{ client, req.value("id", 0LL), params.value("idempotency_key", std::string()) };
    bool isDeferred = false;

    try
    {
        if (!m_scene || !m_scriptEngine)
            throw std::runtime_error("engine not ready");

        // 生成/削除/シーン系を弾く判定。Playing 中はもちろん、同一 Poll バッチで先に play が
        // 積まれた(モード遷移保留)場合も弾く＝そのフレームで spawn ドレインが skip されて
        // 遅延応答が宙吊り(クライアント timeout)になるのを防ぐ。
        const bool busyPlaying = (m_engineMode == EngineMode::Playing) ||
                                 (m_modeChangeRequested && m_pendingMode == EngineMode::Playing);

        if (method == "list_entities")
        {
            const bool verbose = params.value("verbose", false);
            const std::string namePrefix = params.value("name_prefix", std::string());
            std::string typeFilter = params.value("component_type", std::string());
            json arr = json::array();
            auto& reg = m_scene->GetRegistry();
            auto view = reg.view<const NameTag>();
            for (auto e : view)
            {
                const std::string& nm = view.get<const NameTag>(e).name;
                if (!namePrefix.empty() && nm.rfind(namePrefix, 0) != 0) continue;
                json types;   // verbose か component_type 指定時のみ計算
                if (verbose || !typeFilter.empty()) types = McpComponentTypesOf(reg, e);
                if (!typeFilter.empty())
                {
                    bool has = false;
                    for (auto& t : types) if (t.get<std::string>() == typeFilter) { has = true; break; }
                    if (!has) continue;
                }
                json item{{"entityId", static_cast<u32>(e)}, {"id", static_cast<u32>(e)}, {"name", nm}};
                if (verbose) item["componentTypes"] = types;
                arr.push_back(std::move(item));
            }
            resp["ok"] = true;
            resp["result"] = {{"entities", arr}, {"count", arr.size()},
                              {"sceneGeneration", m_sceneGeneration}};
        }
        else if (method == "create_lua_component")
        {
            const std::string name = params.value("name", std::string());
            const std::string code = params.value("code", std::string());
            if (name.empty()) throw std::runtime_error("missing 'name'");
            // ponytail: name はファイル名へ直結。パス区切り等を弾いて traversal を防ぐ。
            if (name.find_first_of("/\\:*?\"<>|") != std::string::npos)
                throw std::runtime_error("invalid component name");
            // 構文チェック(コンパイルのみ・実行しない)。不正なら書かずに AI へエラーを返す。
            std::string serr;
            if (!m_scriptEngine->CheckLuaSyntax(code, serr))
                throw std::runtime_error("Lua syntax error: " + serr);

            const std::string rel = "components/" + name + ".lua";
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            fs::create_directories(full.parent_path());
            std::ofstream ofs(full, std::ios::binary | std::ios::trunc);
            if (!ofs) throw std::runtime_error("cannot write " + full.string());
            ofs.write(code.data(), static_cast<std::streamsize>(code.size()));
            resp["ok"] = true;
            resp["result"] = {{"path", rel}};
        }
        else if (method == "create_shader")
        {
            // カスタムシェーダー(MeshRenderer::shaderPath 割当用)を assets/shaders/ に作成/上書きする。
            // Lua と違い、書く前の静的検証ができない(DXC はファイルからしかコンパイルできない)ため、
            // 先に書いてから即コンパイルを試み、成否をそのまま返す(失敗してもファイルは残す=
            // 反復修正前提。エンジン側も無効なカスタムシェーダーは既定 Forward へ安全にフォールバックする)。
            const std::string name = params.value("name", std::string());
            const std::string code = params.value("code", std::string());
            if (name.empty()) throw McpError(McpErr::InvalidParam, "missing 'name'");
            if (name.find_first_of("/\\:*?\"<>|") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid shader name");

            const std::string rel = name + ".hlsl";
            const fs::path full = fs::path(PathResolver::ProjectShaderDir()) / rel;
            fs::create_directories(full.parent_path());
            {
                std::ofstream ofs(full, std::ios::binary | std::ios::trunc);
                if (!ofs) throw McpError(McpErr::Internal, "cannot write " + full.string());
                ofs.write(code.data(), static_cast<std::streamsize>(code.size()));
            }

            bool compiled = false;
            std::string error;
            if (m_shaderManager)
            {
                compiled = m_shaderManager->CompileCustomShader(rel);
                if (!compiled) error = m_shaderManager->GetCustomShaderError(rel);
            }
            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"compiled", compiled}};
            if (!compiled) resp["result"]["error"] = error.empty() ? "shader manager unavailable" : error;
        }
        else if (method == "read_shader")
        {
            const std::string rel = params.value("path", std::string());
            if (rel.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets/shaders 相対のみ)");

            const fs::path full = fs::path(PathResolver::ProjectShaderDir()) / rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "shader not found: " + rel);
            std::ifstream ifs(full, std::ios::binary);
            if (!ifs) throw McpError(McpErr::Internal, "cannot open " + full.string());
            std::ostringstream oss; oss << ifs.rdbuf();

            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"code", oss.str()},
                               {"compiled", m_shaderManager && m_shaderManager->HasValidCustomShader(rel)}};
        }
        else if (method == "set_mesh_shader")
        {
            // MeshRenderer::shaderPath の割当/解除。Inspector の「Shader」コンボと同じ操作を MCP から。
            // modelPath と違いメッシュ再ロードを伴わない(PSO 選択が変わるだけ)ので即時反映して安全。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e)) throw McpError(McpErr::NotFound, "entity has no MeshRenderer");
            auto& mr = reg.get<MeshRenderer>(e);

            std::string rel = params.value("shaderPath", std::string());
            if (!rel.empty())
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid shaderPath (assets/shaders 相対のみ)");
                if (!fs::exists(fs::path(PathResolver::ProjectShaderDir()) / rel))
                    throw McpError(McpErr::NotFound, "shader not found: " + rel);
            }
            mr.shaderPath = rel;
            if (params.contains("alphaBlend"))
                mr.shaderAlphaBlend = params.value("alphaBlend", false);

            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"shaderPath", mr.shaderPath},
                               {"alphaBlend", mr.shaderAlphaBlend},
                               {"skinnedFallbackWarning", reg.all_of<SkeletalAnimation>(e) && !mr.shaderPath.empty()}};
        }
        else if (method == "set_sprite_shader")
        {
            // Sprite2D::shaderPath の割当/解除。set_mesh_shader と同型だが、対象はworld-spaceスプライトのみ
            // (ルートシグネチャ/頂点フォーマットがメッシュ用と異なる別キャッシュ。docs/AUTHORING.md参照)。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Sprite2D>(e)) throw McpError(McpErr::NotFound, "entity has no Sprite2D");
            auto& sp = reg.get<Sprite2D>(e);

            std::string rel = params.value("shaderPath", std::string());
            if (!rel.empty())
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid shaderPath (assets/shaders 相対のみ)");
                if (!fs::exists(fs::path(PathResolver::ProjectShaderDir()) / rel))
                    throw McpError(McpErr::NotFound, "shader not found: " + rel);
            }
            sp.shaderPath = rel;
            if (params.contains("alphaBlend"))
                sp.shaderAlphaBlend = params.value("alphaBlend", false);

            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"shaderPath", sp.shaderPath},
                               {"alphaBlend", sp.shaderAlphaBlend},
                               {"worldSpaceWarning", !sp.worldSpace && !sp.shaderPath.empty()}};
        }
        else if (method == "attach_lua_component")
        {
            const std::string script = params.value("script", std::string());
            if (script.empty()) throw std::runtime_error("missing 'script'");
            // assets 配下限定。絶対パス/ドライブレター/バックスラッシュ/".." を弾いて
            // assets ルート外の任意ファイルを Lua として読ませない(traversal 防止)。
            if (script.front() == '/' || script.find('\\') != std::string::npos ||
                script.find(':') != std::string::npos || script.find("..") != std::string::npos)
                throw std::runtime_error("invalid script path (assets 相対のみ)");
            const auto e = ResolveMcpEntity(*m_scene, params);
            m_scriptEngine->AttachScriptToEntity(e, script);
            m_scriptEngine->ReloadScript(e);
            resp["ok"] = true;
        }
        else if (method == "create_entity")
        {
            // 生成はメッシュ構築に cmdList が要るためフレーム境界で遅延処理。本物の entityId は
            // 生成後に SendToClient で返す(遅延同期)。Play 中は spawn キューが drain されないため拒否。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create entities while Playing; call dx12_stop first");
            const std::string type = params.value("type", std::string("box"));
            std::string name = params.value("name", std::string());
            const auto pos = params.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (pos.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
            std::string marker;
            if      (type == "box")    marker = "__primitive_box__";
            else if (type == "sphere") marker = "__primitive_sphere__";
            else if (type == "plane")  marker = "__primitive_plane__";
            else if (type == "empty")  marker = "__empty__";
            else if (type == "camera")            marker = "__camera__";
            else if (type == "light_directional") marker = "__directional_light__";
            else if (type == "light_point")       marker = "__point_light__";
            else if (type == "light_spot")        marker = "__spot_light__";
            else if (type == "particle_emitter")  marker = "__particle_emitter__";
            else if (type == "trigger")           marker = "__trigger__";
            else throw McpError(McpErr::InvalidParam,
                "type must be one of: box, sphere, plane, empty, camera, light_directional, "
                "light_point, light_spot, particle_emitter, trigger");
            if (name.empty())   // 既定名: 種別名を先頭大文字に
            {
                name = type;
                if (name[0] >= 'a' && name[0] <= 'z') name[0] = static_cast<char>(name[0] - 'a' + 'A');
            }
            // idempotency: 同 key で既に生成済みかつ有効ならそれを即返す(再試行の重複生成防止)。
            if (!deferred.idempotencyKey.empty())
            {
                auto it = m_mcpIdempotency.find(deferred.idempotencyKey);
                if (it != m_mcpIdempotency.end() &&
                    m_scene->GetRegistry().valid(static_cast<entt::entity>(it->second)))
                {
                    resp["ok"] = true;
                    resp["result"] = {{"entityId", it->second}, {"name", name},
                                      {"sceneGeneration", m_sceneGeneration}, {"idempotentReplay", true}};
                }
            }
            if (!resp.contains("result"))
            {
                PendingSpawnRequest sreq;
                sreq.modelPath = marker;
                sreq.position  = { pos[0], pos[1], pos[2] };
                sreq.name      = name;
                sreq.mcp       = deferred;
                m_editorCtx->pendingSpawns.push_back(std::move(sreq));
                isDeferred = true;
            }
        }
        else if (method == "delete_entity")
        {
            // 削除ドレイン(Render)は Editor モード限定。Play 中に積むと drain されず未応答ハングするため弾く。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot delete while Playing; call dx12_stop first");
            const auto e = ResolveMcpEntity(*m_scene, params);
            // 子ごと削除+Undo はフレーム境界で処理し、deletedCount を遅延応答で返す。
            m_editorCtx->mcpDeletions.push_back(McpPendingDelete{ e, deferred });
            isDeferred = true;
        }
        else if (method == "set_transform")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);   // 無効 id は "invalid entity id" を投げる
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Transform>(e))
                throw McpError(McpErr::NotFound, "entity has no Transform");
            auto& t = reg.get<Transform>(e);
            if (params.contains("position"))
            {
                const auto p = params["position"].get<std::vector<float>>();
                if (p.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
                t.position = { p[0], p[1], p[2] };
            }
            if (params.contains("rotation"))
            {
                const auto r = params["rotation"].get<std::vector<float>>();
                if (r.size() != 3) throw McpError(McpErr::InvalidParam, "rotation must be [x,y,z]");
                t.rotation = { r[0], r[1], r[2] };
                t.useQuaternion = false;   // Euler を反映(物理同期の quaternion に上書きされないように)
            }
            if (params.contains("quaternion"))
            {
                const auto q = params["quaternion"].get<std::vector<float>>();
                if (q.size() != 4) throw McpError(McpErr::InvalidParam, "quaternion must be [x,y,z,w]");
                t.quaternion = { q[0], q[1], q[2], q[3] };
                t.useQuaternion = true;   // set_component の transform 経路と同じ挙動に揃える
            }
            if (params.contains("scale"))
            {
                const auto s = params["scale"].get<std::vector<float>>();
                if (s.size() != 3) throw McpError(McpErr::InvalidParam, "scale must be [x,y,z]");
                t.scale = { s[0], s[1], s[2] };
            }
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}};
        }
        else if (method == "get_entity")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            // 既存シリアライザを流用(リフレクション的に全コンポーネントを JSON 化)。
            std::string js = SceneSerializer::SerializeEntity(*m_scene, e, PathResolver::AssetsDir());
            json result = json::parse(js);
            result["entityId"] = static_cast<u32>(e);
            json types = McpComponentTypesOf(reg, e);
            // Lua スクリプトが entity.<key> で直接読めるコンポーネントだけを別出し(現状 transform のみ)。
            // MCP で見えても Lua では nil になる boxCollider 等との取り違えを防ぐ。
            json luaReadable = json::array();
            for (auto& k : types) if (LuaReadableComponent(k.get<std::string>())) luaReadable.push_back(k);
            result["luaReadable"] = std::move(luaReadable);
            result["componentTypes"] = std::move(types);
            result["sceneGeneration"] = m_sceneGeneration;
            resp["ok"] = true;
            resp["result"] = std::move(result);
        }
        else if (method == "save_scene")
        {
            std::string rel = params.value("path", std::string());
            std::string full;
            if (rel.empty())
            {
                if (m_editorCtx->currentScenePath.empty())
                    throw std::runtime_error("no current scene; specify 'path'");
                full = m_editorCtx->currentScenePath;          // 既存は絶対パス
            }
            else
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw std::runtime_error("invalid path (assets 相対のみ)");
                full = PathResolver::AssetsDir() + rel;        // 末尾 '/' 付き
                fs::create_directories(fs::path(full).parent_path());
            }
            if (!SceneSerializer::Save(*m_scene, full, PathResolver::AssetsDir()))
                throw std::runtime_error("save failed");
            resp["ok"] = true;
            resp["result"] = {{"path", rel.empty() ? m_editorCtx->currentScenePath : rel}};
        }
        else if (method == "open_scene")
        {
            std::string rel = params.value("path", std::string());
            if (rel.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            // pendingLoadPath は Editor モードでのみ drain される(Play 中はロードしない)。
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot open scene while Playing; call dx12_stop first");
            // 単一スロット: 既に未処理の open_scene があれば 2件目を弾く(上書きで1件目が宙吊りになるのを防ぐ)。
            if (m_mcpLoadReply.client != 0 || !m_editorCtx->pendingLoadPath.empty())
                throw McpError(McpErr::ModeConflict, "a scene load is already in progress; retry after it completes");
            const std::string full = PathResolver::AssetsDir() + rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "scene not found: " + rel);
            // 遅延ロード: フレーム境界の機構が pendingLoadPath を消費し SceneSerializer::Load を行う。
            // 完了後に m_mcpLoadReply 経由で sceneName/entityCount/sceneGeneration を返す(遅延同期)。
            m_editorCtx->pendingLoadPath = full;
            m_mcpLoadReply = deferred;
            isDeferred = true;
        }
        else if (method == "list_scenes")
        {
            json arr = json::array();
            const std::string root = PathResolver::AssetsDir();      // 末尾 '/'
            const fs::path scenesDir = fs::path(root) / "scenes";    // scenes/ 配下のみシーン扱い
            if (fs::exists(scenesDir))
            {
                for (const auto& de : fs::recursive_directory_iterator(scenesDir))
                {
                    if (!de.is_regular_file() || de.path().extension() != ".json") continue;
                    std::string relPath = fs::relative(de.path(), fs::path(root)).generic_string();
                    arr.push_back({{"path", relPath}, {"name", de.path().stem().string()}});
                }
            }
            resp["ok"] = true;
            resp["result"] = std::move(arr);
        }
        else if (method == "list_assets")
        {
            const std::string filter = params.value("type", std::string());
            json arr = json::array();
            const std::string root = PathResolver::AssetsDir();
            // ext + 相対パスで分類。.json は scenes/ 配下だけ "scene"(game.json/sceneflow.json 等を除外)。
            auto classify = [](std::string ext, const std::string& relPath) -> std::string {
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj") return "model";
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" ||
                    ext == ".tga" || ext == ".bmp") return "texture";
                if (ext == ".lua") return "script";
                if (ext == ".hlsl") return "shader";
                if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") return "audio";
                if (ext == ".json")
                    return (relPath.rfind("scenes/", 0) == 0) ? "scene" : std::string();
                if (ext == ".prefab") return "prefab";
                return std::string();
            };
            if (fs::exists(fs::path(root)))
            {
                for (const auto& de : fs::recursive_directory_iterator(fs::path(root)))
                {
                    if (!de.is_regular_file()) continue;
                    std::string relPath = fs::relative(de.path(), fs::path(root)).generic_string();
                    std::string type = classify(de.path().extension().string(), relPath);
                    if (type.empty()) continue;
                    if (!filter.empty() && type != filter) continue;
                    arr.push_back({{"path", relPath}, {"type", type}, {"name", de.path().stem().string()}});
                }
            }
            resp["ok"] = true;
            resp["result"] = std::move(arr);
        }
        else if (method == "spawn_model")
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot spawn while Playing; call dx12_stop first");
            std::string path = params.value("path", std::string());
            if (path.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (path.front() == '/' || path.find('\\') != std::string::npos ||
                path.find(':') != std::string::npos || path.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            if (!fs::exists(PathResolver::AssetsDir() + path))
                throw McpError(McpErr::NotFound, "model not found: " + path);
            const auto pos = params.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (pos.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
            std::string name = params.value("name", std::string());
            if (name.empty()) name = fs::path(path).stem().string();
            // idempotency: 同 key で生成済みかつ有効ならそれを即返す。
            if (!deferred.idempotencyKey.empty())
            {
                auto it = m_mcpIdempotency.find(deferred.idempotencyKey);
                if (it != m_mcpIdempotency.end() &&
                    m_scene->GetRegistry().valid(static_cast<entt::entity>(it->second)))
                {
                    resp["ok"] = true;
                    resp["result"] = {{"entityId", it->second}, {"name", name},
                                      {"meshPath", path}, {"sceneGeneration", m_sceneGeneration},
                                      {"idempotentReplay", true}};
                }
            }
            if (!resp.contains("result"))
            {
                // 実モデルのロードは GPU を伴うため cmdList 有効なフレーム境界で遅延処理。
                // create_entity と同じ pendingSpawns に積む(marker でなく実パスを入れる)。
                PendingSpawnRequest sreq;
                sreq.modelPath = path;
                sreq.position  = { pos[0], pos[1], pos[2] };
                sreq.name      = name;
                sreq.mcp       = deferred;
                m_editorCtx->pendingSpawns.push_back(std::move(sreq));
                isDeferred = true;
            }
        }
        else if (method == "set_component")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            const std::string comp = params.value("component", std::string());
            const json data = params.value("data", json::object());
            if (comp.empty()) throw McpError(McpErr::InvalidParam, "missing 'component'");
            if (comp == "transform")
            {
                // コア不変: 専用処理(set_transform 相当)
                auto& t = reg.get_or_emplace<Transform>(e);
                if (data.contains("position"))
                {
                    auto p = data["position"].get<std::vector<float>>();
                    if (p.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
                    t.position = { p[0], p[1], p[2] };
                }
                if (data.contains("rotation"))
                {
                    auto r = data["rotation"].get<std::vector<float>>();
                    if (r.size() != 3) throw McpError(McpErr::InvalidParam, "rotation must be [x,y,z]");
                    t.rotation = { r[0], r[1], r[2] };
                    t.useQuaternion = false;
                }
                if (data.contains("quaternion"))
                {
                    auto q = data["quaternion"].get<std::vector<float>>();
                    if (q.size() != 4) throw McpError(McpErr::InvalidParam, "quaternion must be [x,y,z,w]");
                    t.quaternion = { q[0], q[1], q[2], q[3] };
                    t.useQuaternion = true;
                }
                if (data.contains("scale"))
                {
                    auto s = data["scale"].get<std::vector<float>>();
                    if (s.size() != 3) throw McpError(McpErr::InvalidParam, "scale must be [x,y,z]");
                    t.scale = { s[0], s[1], s[2] };
                }
            }
            // orphan(レジストリ未登録)コンポーネントは専用適用(save/load 経路に触れない)。
            else if (ApplyOrphanComponent(reg, e, comp, data))
            {
                // 適用済み(emplace_or_replace)。
            }
            else
            {
                // deserialize に emplace-only の型があるため、"上書き(set)" 実現には
                // 既存を remove してから登録済みデシリアライザで再生成する。
                if (!RemoveRegisteredComponent(reg, e, comp))
                    throw McpError(McpErr::UnknownComponent,
                        "unknown/unsupported component: " + comp + " (call dx12_describe_components)");
                json ej;
                ej[comp] = data;          // deserialize は ej.contains(jsonKey) を見る形
                RuntimeComponentRegistry::Get().ForEach([&](const RuntimeComponentInfo& info) {
                    if (info.deserialize) info.deserialize(reg, e, ej);
                });
            }
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"component", comp}};
        }
        else if (method == "remove_component")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            const std::string comp = params.value("component", std::string());
            if (comp == "transform" || comp == "name")
                throw McpError(McpErr::InvalidParam, "cannot remove core component (transform/name)");
            if (!RemoveRegisteredComponent(reg, e, comp))
                throw McpError(McpErr::UnknownComponent,
                    "unknown/unsupported component: " + comp + " (call dx12_describe_components)");
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"removed", comp}};
        }
        else if (method == "describe_components")
        {
            const std::string only = params.value("component", std::string());
            json all = McpComponentSchema();
            if (only.empty())
            {
                resp["ok"] = true;
                resp["result"] = std::move(all);
            }
            else
            {
                json filtered = json::array();
                for (auto& c : all["components"])
                    if (c.value("jsonKey", std::string()) == only) filtered.push_back(c);
                if (filtered.empty())
                    throw McpError(McpErr::UnknownComponent, "unknown component: " + only);
                resp["ok"] = true;
                resp["result"] = {{"components", std::move(filtered)}};
            }
        }
        else if (method == "describe_lua_api")
        {
            // Lua スクリプトから使えるバインディング一覧(静的)。MCP のコンポーネントと
            // Lua から読める API のズレ(entity.boxCollider は nil 等)を AI に伝えるため。
            resp["ok"] = true;
            resp["result"] = McpLuaApi();
        }
        else if (method == "set_parent")
        {
            auto& reg = m_scene->GetRegistry();
            const auto child = ResolveMcpEntity(*m_scene, params);   // entity か name で子を指定
            const u32 pid = params.value("parent", 0xFFFFFFFFu);
            entt::entity parent = entt::null;
            if (pid != 0xFFFFFFFFu)
            {
                parent = static_cast<entt::entity>(pid);
                if (!reg.valid(parent)) throw std::runtime_error("invalid parent id");
                // サイクル検出: parent の祖先鎖に child が現れたら拒否(O(N))。
                for (entt::entity cur = parent; cur != entt::null; )
                {
                    if (cur == child) throw std::runtime_error("would create cycle");
                    auto* t = reg.try_get<Transform>(cur);
                    cur = t ? t->parent : entt::null;
                }
            }
            auto& t = reg.get_or_emplace<Transform>(child);
            t.parent = parent;   // 階層は Transform.parent が駆動。SerializeEntity に自動反映。
            resp["ok"] = true;
        }
        else if (method == "rename_entity")
        {
            // ここの "name" は新しい名前。エンティティ指定は entity(id) のみ(name 引きは曖昧なので不可)。
            const auto e = static_cast<entt::entity>(params.value("entity", 0xFFFFFFFFu));
            auto& reg = m_scene->GetRegistry();
            if (!reg.valid(e)) throw McpError(McpErr::NotFound, "invalid entity id");
            if (!reg.all_of<NameTag>(e)) throw McpError(McpErr::NotFound, "entity has no NameTag");
            std::string base = params.value("name", std::string());
            if (base.empty()) throw std::runtime_error("missing 'name'");
            auto taken = [&](const std::string& s) {
                for (auto [oe, tag] : reg.view<NameTag>().each())
                    if (oe != e && tag.name == s) return true;
                return false;
            };
            std::string name = base;
            int n = 2;            // 重複時は連番付与(MakeUniqueName 相当をインライン)
            while (taken(name)) name = base + "_" + std::to_string(n++);
            reg.get<NameTag>(e).name = name;
            resp["ok"] = true;
            resp["result"] = {{"name", name}};
        }
        else if (method == "ping")
        {
            int entityCount = 0;
            for (auto e : m_scene->GetRegistry().view<NameTag>()) { (void)e; ++entityCount; }
            resp["ok"] = true;
            resp["result"] = {
                {"pong", true},
                {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                {"entityCount", entityCount},
                {"sceneGeneration", m_sceneGeneration},
                {"currentScene", ToAssetRel(m_editorCtx->currentScenePath)},
                {"protocolVersion", 3}
            };
        }
        else if (method == "find_entity")
        {
            const std::string name = params.value("name", std::string());
            if (name.empty()) throw McpError(McpErr::InvalidParam, "missing 'name'");
            auto ent = m_scene->FindEntity(name);
            resp["ok"] = true;
            if (ent.IsValid())
                resp["result"] = {{"entityId", static_cast<u32>(ent.GetHandle())}, {"name", name}};
            else
                resp["result"] = nullptr;
        }
        else if (method == "query_entities")
        {
            auto& reg = m_scene->GetRegistry();
            const std::string tag = params.value("tag", std::string());
            std::vector<entt::entity> hits;
            if (params.contains("box") && params["box"].is_array() && params["box"].size() == 4)
            {
                auto b = params["box"].get<std::vector<float>>();
                hits = m_scene->QueryInBox(b[0], b[1], b[2], b[3], tag);  // minX,minZ,maxX,maxZ
            }
            else if (!tag.empty())
            {
                hits = m_scene->QueryByTag(tag);
            }
            else
            {
                throw McpError(McpErr::InvalidParam, "provide 'tag' and/or 'box':[minX,minZ,maxX,maxZ]");
            }
            json arr = json::array();
            for (auto e : hits)
            {
                if (!reg.valid(e)) continue;
                std::string nm = reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string();
                arr.push_back({{"entityId", static_cast<u32>(e)}, {"name", nm}});
            }
            resp["ok"] = true;
            resp["result"] = {{"entities", arr}, {"count", arr.size()}};
        }
        else if (method == "select_entity")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            m_editorCtx->Select(e);
            resp["ok"] = true;
            resp["result"] = {{"selected", static_cast<u32>(e)}};
        }
        else if (method == "focus_camera")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<Transform>(e))
                throw McpError(McpErr::NotFound, "entity has no Transform");
            const auto& t = reg.get<Transform>(e);
            float dist = 8.0f;
            if (reg.all_of<MeshRenderer>(e))
            {
                const auto& mr = reg.get<MeshRenderer>(e);
                float maxExtent = 0.0f;
                for (const auto* mesh : mr.meshes)
                {
                    if (!mesh) continue;
                    auto mn = mesh->GetAABBMin();
                    auto mx = mesh->GetAABBMax();
                    maxExtent = std::max({maxExtent,
                        (mx.x - mn.x) * t.scale.x, (mx.y - mn.y) * t.scale.y, (mx.z - mn.z) * t.scale.z});
                }
                if (maxExtent > 0.0f) dist = std::clamp(maxExtent * 2.0f, 2.0f, 100.0f);
            }
            DirectX::XMFLOAT3 wpos = t.position;
            if (t.parent != entt::null && reg.valid(t.parent))
            {
                DirectX::XMFLOAT4X4 wf;
                DirectX::XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, e));
                wpos = { wf._41, wf._42, wf._43 };
            }
            auto fwd = m_camera->GetForward();
            DirectX::XMFLOAT3 camPos{ wpos.x - fwd.x * dist, wpos.y - fwd.y * dist, wpos.z - fwd.z * dist };
            m_camera->SetPosition(camPos);
            resp["ok"] = true;
            resp["result"] = {{"cameraPos", {camPos.x, camPos.y, camPos.z}},
                              {"target", {wpos.x, wpos.y, wpos.z}}, {"distance", dist}};
        }
        else if (method == "set_pbr")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e))
                throw McpError(McpErr::NotFound, "entity has no MeshRenderer");
            auto& mr = reg.get<MeshRenderer>(e);
            if (params.contains("metallic"))  mr.overrideMetallic  = params["metallic"].get<float>();
            if (params.contains("roughness")) mr.overrideRoughness = params["roughness"].get<float>();
            if (params.contains("uvScaleU"))  mr.uvScaleU = params["uvScaleU"].get<float>();
            if (params.contains("uvScaleV"))  mr.uvScaleV = params["uvScaleV"].get<float>();
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)},
                              {"metallic", mr.overrideMetallic}, {"roughness", mr.overrideRoughness},
                              {"uvScaleU", mr.uvScaleU}, {"uvScaleV", mr.uvScaleV}};
        }
        else if (method == "duplicate_entity")
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot duplicate while Playing; call dx12_stop first");
            const auto e = ResolveMcpEntity(*m_scene, params);
            m_editorCtx->mcpDuplications.push_back(McpPendingDelete{ e, deferred });  // .entity=複製元
            isDeferred = true;
        }
        else if (method == "undo")
        {
            m_editorCtx->pendingUndo = true;   // フレーム境界で適用
            resp["ok"] = true;
            resp["result"] = {{"queuedUndo", true}};
        }
        else if (method == "redo")
        {
            m_editorCtx->pendingRedo = true;
            resp["ok"] = true;
            resp["result"] = {{"queuedRedo", true}};
        }
        else if (method == "new_scene")
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create a new scene while Playing");
            std::string rel = params.value("savePath", std::string());
            if (!rel.empty())
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid savePath (assets 相対のみ)");
                m_editorCtx->pendingNewScenePath = PathResolver::AssetsDir() + rel;
            }
            m_editorCtx->pendingNewScene = true;   // フレーム境界で空シーン生成 + sceneGeneration++
            resp["ok"] = true;
            resp["result"] = {{"applied", "next frame"}};
        }
        else if (method == "spawn_prefab")
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot spawn while Playing; call dx12_stop first");
            std::string path = params.value("path", std::string());
            if (path.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (path.front() == '/' || path.find('\\') != std::string::npos ||
                path.find(':') != std::string::npos || path.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            if (fs::path(path).extension() != ".prefab")
                throw McpError(McpErr::InvalidParam, "path must be a .prefab");
            if (!fs::exists(PathResolver::AssetsDir() + path))
                throw McpError(McpErr::NotFound, "prefab not found: " + path);
            const auto pos = params.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (pos.size() != 3) throw McpError(McpErr::InvalidParam, "position must be [x,y,z]");
            PendingSpawnRequest sreq;
            sreq.modelPath = path;          // 拡張子 .prefab で spawn ループが展開し root+ids を返す
            sreq.position  = { pos[0], pos[1], pos[2] };
            sreq.name      = params.value("name", std::string());
            sreq.mcp       = deferred;
            m_editorCtx->pendingSpawns.push_back(std::move(sreq));
            isDeferred = true;
        }
        else if (method == "get_scene_settings")
        {
            const auto& sky = m_scene->GetSkyboxSettings();
            resp["ok"] = true;
            resp["result"] = {{"skybox", {
                                  {"envMapPath", sky.envMapPath}, {"iblIntensity", sky.iblIntensity},
                                  {"skyboxIntensity", sky.skyboxIntensity}, {"drawSkybox", sky.drawSkybox}}},
                              {"note", "post-process は dx12_get_post_process、SSAO は dx12_get_ssao を使う"}};
        }
        else if (method == "set_scene_settings")
        {
            const json sky = params.value("skybox", json::object());
            auto& s = m_scene->GetSkyboxSettings();
            bool envChanged = false;
            if (sky.contains("envMapPath"))
            {
                std::string p = sky["envMapPath"].get<std::string>();
                if (!p.empty() && (p.front() == '/' || p.find('\\') != std::string::npos ||
                    p.find(':') != std::string::npos || p.find("..") != std::string::npos))
                    throw McpError(McpErr::InvalidParam, "invalid envMapPath (assets 相対のみ)");
                if (p != s.envMapPath) { s.envMapPath = p; envChanged = true; }
            }
            if (sky.contains("iblIntensity"))    s.iblIntensity    = sky["iblIntensity"].get<float>();
            if (sky.contains("skyboxIntensity")) s.skyboxIntensity = sky["skyboxIntensity"].get<float>();
            if (sky.contains("drawSkybox"))      s.drawSkybox      = sky["drawSkybox"].get<bool>();
            if (envChanged) { m_loadedSkyboxPath.clear(); m_skyboxDirty = true; }  // 環境マップ再ベイク要求
            resp["ok"] = true;
            resp["result"] = {{"applied", true}, {"envMapRebake", envChanged}};
        }
        else if (method == "play")
        {
            if (m_engineMode == EngineMode::Playing)
            {
                resp["ok"] = true;
                resp["result"] = {{"mode", "Playing"}, {"sceneGeneration", m_sceneGeneration}};
            }
            else
            {
                // 単一スロット: 既にモード遷移待ちなら 2件目を弾く(上書きで1件目が宙吊りになるのを防ぐ)。
                if (m_mcpModeReply.client != 0)
                    throw McpError(McpErr::ModeConflict, "a mode change is already pending; retry shortly");
                // 実切替は EnterPlayMode(snapshot/script init/GPU) を伴うためフレーム境界で遅延。
                // 遷移確定後に Run() のモード応答ブロックが本物のモード(or 失敗)を返す。
                m_pendingMode = EngineMode::Playing;
                m_modeChangeRequested = true;
                m_mcpModeReply = deferred;
                isDeferred = true;
            }
        }
        else if (method == "stop")
        {
            if (m_engineMode == EngineMode::Editor)
            {
                resp["ok"] = true;
                resp["result"] = {{"mode", "Editor"}, {"sceneGeneration", m_sceneGeneration}};
            }
            else
            {
                if (m_mcpModeReply.client != 0)
                    throw McpError(McpErr::ModeConflict, "a mode change is already pending; retry shortly");
                m_pendingMode = EngineMode::Editor;
                m_modeChangeRequested = true;     // 次フレームで EnterEditorMode()(snapshot 復元)
                m_mcpModeReply = deferred;
                isDeferred = true;
            }
        }
        else if (method == "get_mode")
        {
            resp["ok"] = true;
            resp["result"] = {{"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"}};
        }
        else if (method == "get_log")
        {
            int lines = params.value("lines", 50);
            if (lines < 1) lines = 1;
            // Logger は CWD の "dx12_engine.log" へ出力。末尾 N 行を返すだけ(リングは足さない)。
            std::ifstream ifs("dx12_engine.log", std::ios::binary);
            json arr = json::array();
            if (ifs)
            {
                std::vector<std::string> all;
                std::string ln;
                while (std::getline(ifs, ln))
                {
                    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                    all.push_back(ln);
                }
                size_t start = all.size() > static_cast<size_t>(lines) ? all.size() - static_cast<size_t>(lines) : 0;
                for (size_t i = start; i < all.size(); ++i) arr.push_back(all[i]);
            }
            resp["ok"] = true;
            resp["result"] = arr;   // ファイル無しは空配列(grace)
        }
        else if (method == "screenshot")
        {
            // 直近フレームのシーン描画を PNG にして絶対パスを返す。AI 側はそのパスを画像として読む。
            std::string serr;
            const std::string path = CaptureSceneScreenshot(serr);
            if (path.empty()) throw std::runtime_error(serr.empty() ? "screenshot failed" : serr);
            resp["ok"] = true;
            resp["result"] = {{"path", path},
                              {"width", m_sceneRT->GetWidth()},
                              {"height", m_sceneRT->GetHeight()}};
        }
        else if (method == "project_world_to_screen")
        {
            // エンティティのワールド座標を、今シーンビューを描いているカメラ(m_camera)の
            // ビュー*射影で画面ピクセルへ投影する。Playing 中は m_camera = アクティブなゲームカメラ
            // なので「ゲーム画面で player が中央/画面内か」を数値で確認できる(screenshot と整合)。
            using namespace DirectX;
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            XMVECTOR wpos = XMVectorSetW(ComputeWorldMatrix(reg, e).r[3], 1.0f);
            XMVECTOR clip = XMVector4Transform(wpos, m_camera->GetViewProjMatrix());
            const float w = XMVectorGetW(clip);
            const float ndcX = (w != 0.0f) ? XMVectorGetX(clip) / w : 0.0f;
            const float ndcY = (w != 0.0f) ? XMVectorGetY(clip) / w : 0.0f;
            const float ndcZ = (w != 0.0f) ? XMVectorGetZ(clip) / w : 0.0f;
            const float vw = static_cast<float>(m_sceneRT->GetWidth());
            const float vh = static_cast<float>(m_sceneRT->GetHeight());
            const float px = (ndcX * 0.5f + 0.5f) * vw;
            const float py = (0.5f - ndcY * 0.5f) * vh;   // NDC +Y up → ピクセル +Y down
            const bool visible = (w > 0.0f) && ndcX >= -1.0f && ndcX <= 1.0f &&
                                 ndcY >= -1.0f && ndcY <= 1.0f && ndcZ >= 0.0f && ndcZ <= 1.0f;
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"x", px}, {"y", py},
                              {"visible", visible}, {"depth", ndcZ}, {"w", w},
                              {"width", m_sceneRT->GetWidth()}, {"height", m_sceneRT->GetHeight()},
                              {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"}};
        }
        else if (method == "get_lua_component_state")
        {
            // LuaScript の現在のプロパティ値(オーバーライド+スキーマ既定)を全部返す。
            // get_entity は保存済みオーバーライドしか出さないので、スキーマを基準に既定も含めて出す。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<LuaScript>(e)) throw McpError(McpErr::NotFound, "entity has no LuaScript");
            const auto& ls = reg.get<LuaScript>(e);
            const auto& schema = m_scriptEngine->GetPropertySchema(ls.scriptPath);
            auto typeStr = [](ScriptPropType t) -> const char* {
                switch (t) {
                    case ScriptPropType::Int:    return "int";
                    case ScriptPropType::Bool:   return "bool";
                    case ScriptPropType::String: return "string";
                    case ScriptPropType::Vec3:   return "vec3";
                    case ScriptPropType::Color:  return "color";
                    case ScriptPropType::Entity: return "entity";
                    default:                     return "float";
                } };
            auto emitVal = [](json& pj, const ScriptProp& v) {
                switch (v.type) {
                    case ScriptPropType::Int:    pj["value"] = static_cast<long long>(v.num); break;
                    case ScriptPropType::Bool:   pj["value"] = v.b; break;
                    case ScriptPropType::String:
                    case ScriptPropType::Entity: pj["value"] = v.str; break;
                    case ScriptPropType::Vec3:
                    case ScriptPropType::Color:  pj["value"] = json::array({v.vec.x, v.vec.y, v.vec.z}); break;
                    default:                     pj["value"] = v.num; break;
                } };
            json props = json::array();
            for (const auto& d : schema)
            {
                const ScriptProp* ov = nullptr;
                for (const auto& p : ls.props) if (p.name == d.name) { ov = &p; break; }
                ScriptProp v = ov ? *ov : d.def;
                v.type = d.type;   // オーバーライドの型がズレてても schema を正とする
                json pj{{"name", d.name}, {"type", typeStr(d.type)}, {"isOverride", ov != nullptr}};
                emitVal(pj, v);
                props.push_back(std::move(pj));
            }
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"scriptPath", ls.scriptPath},
                              {"enabled", ls.enabled}, {"started", ls.started},
                              {"loadError", ls.loadError}, {"errorMessage", ls.errorMessage},
                              {"properties", std::move(props)}};
        }
        else if (method == "set_lua_property")
        {
            // LuaScript のプロパティを1つ書き換える。スキーマで型を確認して検証。
            // 実行中(Playing)なら ReloadScript で再注入、Editor では保存だけ(次 Play で反映)。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<LuaScript>(e)) throw McpError(McpErr::NotFound, "entity has no LuaScript");
            const std::string key = params.value("key", std::string());
            if (key.empty()) throw McpError(McpErr::InvalidParam, "missing 'key'");
            if (!params.contains("value")) throw McpError(McpErr::InvalidParam, "missing 'value'");
            const json& value = params["value"];
            auto& ls = reg.get<LuaScript>(e);
            const auto& schema = m_scriptEngine->GetPropertySchema(ls.scriptPath);
            const ScriptPropDef* def = nullptr;
            for (const auto& d : schema) if (d.name == key) { def = &d; break; }
            if (!def) throw McpError(McpErr::InvalidParam,
                "unknown property '" + key + "' (script の properties に未宣言。dx12_get_lua_component_state で確認)");
            ScriptProp* p = nullptr;
            for (auto& ex : ls.props) if (ex.name == key) { p = &ex; break; }
            if (!p) { ls.props.push_back(def->def); p = &ls.props.back(); }
            p->type = def->type;
            switch (def->type)
            {
            case ScriptPropType::Float:
            case ScriptPropType::Int:
                if (!value.is_number()) throw McpError(McpErr::InvalidParam, "value must be a number");
                p->num = value.get<double>(); break;
            case ScriptPropType::Bool:
                if (!value.is_boolean()) throw McpError(McpErr::InvalidParam, "value must be a bool");
                p->b = value.get<bool>(); break;
            case ScriptPropType::String:
            case ScriptPropType::Entity:
                if (!value.is_string()) throw McpError(McpErr::InvalidParam, "value must be a string");
                p->str = value.get<std::string>(); break;
            case ScriptPropType::Vec3:
            case ScriptPropType::Color:
            {
                if (!value.is_array() || value.size() != 3)
                    throw McpError(McpErr::InvalidParam, "value must be [x,y,z]");
                auto a = value.get<std::vector<float>>();
                p->vec = { a[0], a[1], a[2] }; break;
            }
            }
            if (ls.started) m_scriptEngine->ReloadScript(e);  // 実行中のみ再注入(Editor は保存のみ)
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"key", key}, {"reloaded", ls.started}};
        }
        else if (method == "key_down")
        {
            // 合成キー押下(押しっぱなし)。次フレーム以降の Lua input:isKeyDown / keyDown() が true になる。
            // key_up を呼ぶまで保持。Playing 中の移動などの確認用(isAsyncKeyDown 系には効かない)。
            const int vk = ParseMcpVk(params);
            m_inputSystem->OnKeyDown(vk);
            resp["ok"] = true;
            resp["result"] = {{"key", vk}, {"down", true}};
        }
        else if (method == "key_up")
        {
            const int vk = ParseMcpVk(params);
            m_inputSystem->OnKeyUp(vk);
            resp["ok"] = true;
            resp["result"] = {{"key", vk}, {"down", false}};
        }
        else if (method == "key_press")
        {
            // 1フレームだけ押す(isKeyPressed が立つ)。ジャンプ等のタップ操作の確認用。
            const int vk = ParseMcpVk(params);
            m_inputSystem->InjectKeyPress(vk);
            resp["ok"] = true;
            resp["result"] = {{"key", vk}, {"pressed", true}};
        }
        else if (method == "step_frames")
        {
            // N フレーム進めてから応答する同期バリア(遅延応答)。key_down/press の後に呼ぶと
            // 入力がシミュレーションに効いてから get_entity/project_world_to_screen で結果を見られる。
            // ※ 真の決定論ステッパではない(各フレーム dt は実時間)。エンジンは常時実時間で回る。
            int n = params.value("frames", params.value("n", 1));
            if (n < 1) n = 1;
            if (n > 600) n = 600;   // ~10s 上限(クライアント timeout 対策)
            if (m_mcpStepReply.client != 0)
                throw McpError(McpErr::ModeConflict, "a step is already pending; retry shortly");
            m_mcpStepFramesLeft = n;
            m_mcpStepReply = deferred;
            isDeferred = true;
        }
        else if (method == "set_color")
        {
            // メッシュの頂点色(基本色の乗算)を設定する。scene:setColor(Lua) と同じ。
            // 足場やコインの色付けに。色は [r,g,b](0..1)。
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e)) throw McpError(McpErr::NotFound, "entity has no MeshRenderer");
            auto c = params.value("color", std::vector<float>{1.0f, 1.0f, 1.0f});
            if (c.size() != 3) throw McpError(McpErr::InvalidParam, "color must be [r,g,b]");
            auto* device = m_scene->GetDevice();
            if (!device) throw McpError(McpErr::Internal, "no graphics device");
            auto& mr = reg.get<MeshRenderer>(e);
            for (auto* mesh : mr.meshes) if (mesh) mesh->SetVertexColor(*device, c[0], c[1], c[2], 1.0f);
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"color", {c[0], c[1], c[2]}}};
        }
        else if (method == "screenshot_game_view")
        {
            // アクティブな CameraComponent 視点でシーンを1フレーム描いて撮る(遅延応答)。
            // Editor 中でもゲームカメラの画角を確認できる。Playing 中は通常 screenshot と同じ絵。
            auto& reg = m_scene->GetRegistry();
            bool hasActiveCam = false;
            for (auto [e, cam] : reg.view<const CameraComponent>().each())
                if (cam.isActive) { hasActiveCam = true; break; }
            if (!hasActiveCam && m_engineMode != EngineMode::Playing)
                throw McpError(McpErr::NotFound,
                    "no active CameraComponent (camera.isActive=true にするか dx12_screenshot を使う)");
            if (m_mcpGameViewReply.client != 0)
                throw McpError(McpErr::ModeConflict, "a game-view screenshot is already pending; retry shortly");
            m_mcpGameViewReply = deferred;   // フレーム境界で描画→撮影→応答(Run ループ側)
            isDeferred = true;
        }
        // ════════════════════════════════════════════════════════════
        //  ランタイム物理検証(raycast/overlap/velocity) — 全て同期・読み取り系。
        //  bodies は Play 中のみ登録される(RegisterBody は Play 開始/loadScene 時)。
        //  Editor 中に呼んでもエラーにはせず hit=false / entities=[] / velocity=[0,0,0] を返す。
        // ════════════════════════════════════════════════════════════
        else if (method == "raycast")
        {
            auto originV = params.value("origin", std::vector<float>{});
            auto dirV = params.value("direction", std::vector<float>{});
            if (originV.size() != 3 || dirV.size() != 3)
                throw McpError(McpErr::InvalidParam, "origin and direction must be [x,y,z]");
            const float maxDist = params.value("maxDistance", 1000.0f);
            RaycastHit hit = m_physicsSystem->Raycast(
                {originV[0], originV[1], originV[2]}, {dirV[0], dirV[1], dirV[2]}, maxDist);
            json result{{"hit", hit.hit}};
            if (hit.hit)
            {
                result["distance"] = hit.distance;
                result["point"]    = {hit.point.x, hit.point.y, hit.point.z};
                // 法線は近似(常に up 向き。PhysicsSystem::Raycast の既知の制約)。厳密な面法線は未対応。
                result["normal"]   = {hit.normal.x, hit.normal.y, hit.normal.z};
                auto& reg = m_scene->GetRegistry();
                entt::entity ent = m_physicsSystem->EntityForBody(hit.bodyId);
                if (ent != entt::null && reg.valid(ent))
                {
                    result["entityId"] = static_cast<u32>(ent);
                    if (reg.all_of<NameTag>(ent)) result["name"] = reg.get<NameTag>(ent).name;
                }
            }
            resp["ok"] = true;
            resp["result"] = std::move(result);
        }
        else if (method == "overlap_box" || method == "overlap_sphere")
        {
            int maxResults = params.value("maxResults", 32);
            if (maxResults < 1) maxResults = 1;
            if (maxResults > 256) maxResults = 256;
            std::vector<entt::entity> buf(static_cast<size_t>(maxResults));
            size_t n = 0;
            if (method == "overlap_box")
            {
                auto centerV = params.value("center", std::vector<float>{});
                auto halfV = params.value("halfExtents", std::vector<float>{});
                if (centerV.size() != 3 || halfV.size() != 3)
                    throw McpError(McpErr::InvalidParam, "center and halfExtents must be [x,y,z]");
                n = m_physicsSystem->OverlapBox({centerV[0], centerV[1], centerV[2]},
                                                 {halfV[0], halfV[1], halfV[2]}, buf.data(), buf.size());
            }
            else
            {
                auto centerV = params.value("center", std::vector<float>{});
                if (centerV.size() != 3)
                    throw McpError(McpErr::InvalidParam, "center must be [x,y,z]");
                const float radius = params.value("radius", 1.0f);
                n = m_physicsSystem->OverlapSphere({centerV[0], centerV[1], centerV[2]}, radius,
                                                    buf.data(), buf.size());
            }
            json arr = json::array();
            auto& reg = m_scene->GetRegistry();
            for (size_t i = 0; i < n; ++i)
            {
                json item{{"entityId", static_cast<u32>(buf[i])}};
                if (reg.valid(buf[i]) && reg.all_of<NameTag>(buf[i])) item["name"] = reg.get<NameTag>(buf[i]).name;
                arr.push_back(std::move(item));
            }
            resp["ok"] = true;
            resp["result"] = {{"entities", arr}, {"count", arr.size()}};
        }
        else if (method == "get_physics_state")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            json result{{"entityId", static_cast<u32>(e)},
                        {"hasRigidBody", false}, {"velocity", {0.0f, 0.0f, 0.0f}},
                        {"hasCharacterController", false}, {"isGrounded", false}};
            if (reg.all_of<RigidBody>(e))
            {
                const auto& rb = reg.get<RigidBody>(e);
                result["hasRigidBody"] = true;
                if (rb.bodyId != kInvalidBodyId)
                {
                    auto v = m_physicsSystem->GetLinearVelocity(rb.bodyId);
                    result["velocity"] = {v.x, v.y, v.z};
                }
            }
            if (reg.all_of<CharacterController>(e))
            {
                result["hasCharacterController"] = true;
                result["isGrounded"] = reg.get<CharacterController>(e)._grounded;
            }
            resp["ok"] = true;
            resp["result"] = std::move(result);
        }
        // ════════════════════════════════════════════════════════════
        //  コンテンツ制作ヘルパー拡充
        // ════════════════════════════════════════════════════════════
        else if (method == "read_lua_component")
        {
            std::string rel = params.value("path", std::string());
            if (rel.empty()) throw McpError(McpErr::InvalidParam, "missing 'path'");
            if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
            const fs::path full = fs::path(PathResolver::AssetsDir()) / rel;
            if (!fs::exists(full)) throw McpError(McpErr::NotFound, "script not found: " + rel);
            std::ifstream ifs(full, std::ios::binary);
            if (!ifs) throw McpError(McpErr::Internal, "cannot open " + full.string());
            std::ostringstream ss; ss << ifs.rdbuf();
            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"code", ss.str()}};
        }
        else if (method == "create_prefab")
        {
            if (busyPlaying)
                throw McpError(McpErr::ModeConflict, "cannot create a prefab while Playing; call dx12_stop first");
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            std::string rel = params.value("path", std::string());
            fs::path file;
            if (rel.empty())
            {
                std::string base = reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string("Prefab");
                if (base.empty()) base = "Prefab";
                fs::path dir = fs::path(PathResolver::AssetsDir()) / "prefabs";
                fs::create_directories(dir);
                file = dir / (base + ".prefab");
                for (int n = 1; fs::exists(file); ++n)
                    file = dir / (base + " (" + std::to_string(n) + ").prefab");
                rel = "prefabs/" + file.filename().string();
            }
            else
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
                if (fs::path(rel).extension() != ".prefab")
                    throw McpError(McpErr::InvalidParam, "path must end with .prefab");
                file = fs::path(PathResolver::AssetsDir()) / rel;
                fs::create_directories(file.parent_path());
            }
            if (!SceneSerializer::SavePrefab(*m_scene, e, file.string(), PathResolver::AssetsDir()))
                throw McpError(McpErr::Internal, "failed to save prefab");
            resp["ok"] = true;
            resp["result"] = {{"path", rel}, {"entityId", static_cast<u32>(e)}};
        }
        // ════════════════════════════════════════════════════════════
        //  ビジュアル/ポスト設定の操作(ポストプロセス・SSAO)
        // ════════════════════════════════════════════════════════════
        else if (method == "get_post_process")
        {
            const auto& pp = m_scene->GetPostSettings();
            resp["ok"] = true;
            resp["result"] = {
                {"enabled", pp.enabled},
                {"exposureOn", pp.exposureOn}, {"exposure", pp.exposure},
                {"contrastOn", pp.contrastOn}, {"contrast", pp.contrast},
                {"brightnessOn", pp.brightnessOn}, {"brightness", pp.brightness},
                {"saturationOn", pp.saturationOn}, {"saturation", pp.saturation},
                {"warmthOn", pp.warmthOn}, {"warmth", pp.warmth},
                {"hueOn", pp.hueOn}, {"hueShift", pp.hueShift},
                {"tintOn", pp.tintOn}, {"tint", {pp.tint.x, pp.tint.y, pp.tint.z}},
                {"bloomOn", pp.bloomOn}, {"bloom", pp.bloom}, {"bloomThreshold", pp.bloomThreshold},
                {"vignetteOn", pp.vignetteOn}, {"vignette", pp.vignette},
                {"chromaticOn", pp.chromaticOn}, {"chromatic", pp.chromatic},
                {"pixelizeOn", pp.pixelizeOn}, {"pixelSize", pp.pixelSize},
                {"posterizeOn", pp.posterizeOn}, {"posterize", pp.posterize},
                {"ditherOn", pp.ditherOn}, {"ditherLevels", pp.ditherLevels},
                {"scanlineOn", pp.scanlineOn}, {"scanline", pp.scanline},
                {"sharpenOn", pp.sharpenOn}, {"sharpen", pp.sharpen},
                {"grainOn", pp.grainOn}, {"grain", pp.grain},
                {"invertOn", pp.invertOn}, {"invert", pp.invert},
                {"sepiaOn", pp.sepiaOn}, {"sepia", pp.sepia},
                {"grayscaleOn", pp.grayscaleOn}, {"grayscale", pp.grayscale},
                {"lensOn", pp.lensOn}, {"lens", pp.lens},
                {"waveOn", pp.waveOn}, {"waveAmp", pp.waveAmp}, {"waveFreq", pp.waveFreq}, {"waveSpeed", pp.waveSpeed},
                {"radialOn", pp.radialOn}, {"radial", pp.radial},
                {"glitchOn", pp.glitchOn}, {"glitch", pp.glitch},
                {"outlineOn", pp.outlineOn}, {"outline", pp.outline},
                {"outlineColor", {pp.outlineColor.x, pp.outlineColor.y, pp.outlineColor.z}},
                {"fxaaOn", pp.fxaaOn},
                {"tonemapper", pp.tonemapper},
                {"bloomKnee", pp.bloomKnee}, {"bloomRadius", pp.bloomRadius},
                {"autoExposureOn", pp.autoExposureOn}, {"aeSpeed", pp.aeSpeed}, {"aeEvComp", pp.aeEvComp},
                {"aeLogMin", pp.aeLogMin}, {"aeLogMax", pp.aeLogMax},
                {"lutOn", pp.lutOn}, {"lutPath", pp.lutPath}, {"lutAmount", pp.lutAmount},
                {"debandOn", pp.debandOn},
                {"godraysOn", pp.godraysOn}, {"grIntensity", pp.grIntensity},
                {"grDensity", pp.grDensity}, {"grDecay", pp.grDecay},
                {"lensflareOn", pp.lensflareOn}, {"lfIntensity", pp.lfIntensity},
                {"lfGhosts", pp.lfGhosts}, {"lfDispersal", pp.lfDispersal},
                {"lfHalo", pp.lfHalo}, {"lfChroma", pp.lfChroma},
                {"dofOn", pp.dofOn}, {"dofFocusDist", pp.dofFocusDist},
                {"dofFocusRange", pp.dofFocusRange}, {"dofBlurSize", pp.dofBlurSize},
                {"motionBlurOn", pp.motionBlurOn}, {"mbStrength", pp.mbStrength},
                {"mbSamples", pp.mbSamples},
            };
        }
        else if (method == "set_post_process")
        {
            auto& pp = m_scene->GetPostSettings();
            pp.enabled = params.value("enabled", pp.enabled);
            pp.exposureOn = params.value("exposureOn", pp.exposureOn); pp.exposure = params.value("exposure", pp.exposure);
            pp.contrastOn = params.value("contrastOn", pp.contrastOn); pp.contrast = params.value("contrast", pp.contrast);
            pp.brightnessOn = params.value("brightnessOn", pp.brightnessOn); pp.brightness = params.value("brightness", pp.brightness);
            pp.saturationOn = params.value("saturationOn", pp.saturationOn); pp.saturation = params.value("saturation", pp.saturation);
            pp.warmthOn = params.value("warmthOn", pp.warmthOn); pp.warmth = params.value("warmth", pp.warmth);
            pp.hueOn = params.value("hueOn", pp.hueOn); pp.hueShift = params.value("hueShift", pp.hueShift);
            pp.tintOn = params.value("tintOn", pp.tintOn);
            if (params.contains("tint"))
            {
                auto t = params["tint"].get<std::vector<float>>();
                if (t.size() == 3) pp.tint = {t[0], t[1], t[2]};
            }
            pp.bloomOn = params.value("bloomOn", pp.bloomOn); pp.bloom = params.value("bloom", pp.bloom);
            pp.bloomThreshold = params.value("bloomThreshold", pp.bloomThreshold);
            pp.bloomKnee = params.value("bloomKnee", pp.bloomKnee); pp.bloomRadius = params.value("bloomRadius", pp.bloomRadius);
            pp.tonemapper = params.value("tonemapper", pp.tonemapper);
            pp.autoExposureOn = params.value("autoExposureOn", pp.autoExposureOn);
            pp.aeSpeed = params.value("aeSpeed", pp.aeSpeed); pp.aeEvComp = params.value("aeEvComp", pp.aeEvComp);
            pp.aeLogMin = params.value("aeLogMin", pp.aeLogMin); pp.aeLogMax = params.value("aeLogMax", pp.aeLogMax);
            pp.lutOn = params.value("lutOn", pp.lutOn); pp.lutPath = params.value("lutPath", pp.lutPath);
            pp.lutAmount = params.value("lutAmount", pp.lutAmount);
            pp.debandOn = params.value("debandOn", pp.debandOn);
            pp.godraysOn = params.value("godraysOn", pp.godraysOn);
            pp.grIntensity = params.value("grIntensity", pp.grIntensity);
            pp.grDensity = params.value("grDensity", pp.grDensity);
            pp.grDecay = params.value("grDecay", pp.grDecay);
            pp.lensflareOn = params.value("lensflareOn", pp.lensflareOn);
            pp.lfIntensity = params.value("lfIntensity", pp.lfIntensity);
            pp.lfGhosts = params.value("lfGhosts", pp.lfGhosts);
            pp.lfDispersal = params.value("lfDispersal", pp.lfDispersal);
            pp.lfHalo = params.value("lfHalo", pp.lfHalo);
            pp.lfChroma = params.value("lfChroma", pp.lfChroma);
            pp.dofOn = params.value("dofOn", pp.dofOn);
            pp.dofFocusDist = params.value("dofFocusDist", pp.dofFocusDist);
            pp.dofFocusRange = params.value("dofFocusRange", pp.dofFocusRange);
            pp.dofBlurSize = params.value("dofBlurSize", pp.dofBlurSize);
            pp.motionBlurOn = params.value("motionBlurOn", pp.motionBlurOn);
            pp.mbStrength = params.value("mbStrength", pp.mbStrength);
            pp.mbSamples = params.value("mbSamples", pp.mbSamples);
            pp.vignetteOn = params.value("vignetteOn", pp.vignetteOn); pp.vignette = params.value("vignette", pp.vignette);
            pp.chromaticOn = params.value("chromaticOn", pp.chromaticOn); pp.chromatic = params.value("chromatic", pp.chromatic);
            pp.pixelizeOn = params.value("pixelizeOn", pp.pixelizeOn); pp.pixelSize = params.value("pixelSize", pp.pixelSize);
            pp.posterizeOn = params.value("posterizeOn", pp.posterizeOn); pp.posterize = params.value("posterize", pp.posterize);
            pp.ditherOn = params.value("ditherOn", pp.ditherOn); pp.ditherLevels = params.value("ditherLevels", pp.ditherLevels);
            pp.scanlineOn = params.value("scanlineOn", pp.scanlineOn); pp.scanline = params.value("scanline", pp.scanline);
            pp.sharpenOn = params.value("sharpenOn", pp.sharpenOn); pp.sharpen = params.value("sharpen", pp.sharpen);
            pp.grainOn = params.value("grainOn", pp.grainOn); pp.grain = params.value("grain", pp.grain);
            pp.invertOn = params.value("invertOn", pp.invertOn); pp.invert = params.value("invert", pp.invert);
            pp.sepiaOn = params.value("sepiaOn", pp.sepiaOn); pp.sepia = params.value("sepia", pp.sepia);
            pp.grayscaleOn = params.value("grayscaleOn", pp.grayscaleOn); pp.grayscale = params.value("grayscale", pp.grayscale);
            pp.lensOn = params.value("lensOn", pp.lensOn); pp.lens = params.value("lens", pp.lens);
            pp.waveOn = params.value("waveOn", pp.waveOn); pp.waveAmp = params.value("waveAmp", pp.waveAmp);
            pp.waveFreq = params.value("waveFreq", pp.waveFreq); pp.waveSpeed = params.value("waveSpeed", pp.waveSpeed);
            pp.radialOn = params.value("radialOn", pp.radialOn); pp.radial = params.value("radial", pp.radial);
            pp.glitchOn = params.value("glitchOn", pp.glitchOn); pp.glitch = params.value("glitch", pp.glitch);
            pp.outlineOn = params.value("outlineOn", pp.outlineOn); pp.outline = params.value("outline", pp.outline);
            if (params.contains("outlineColor"))
            {
                auto c = params["outlineColor"].get<std::vector<float>>();
                if (c.size() == 3) pp.outlineColor = {c[0], c[1], c[2]};
            }
            pp.fxaaOn = params.value("fxaaOn", pp.fxaaOn);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        }
        else if (method == "get_ssao")
        {
            const auto& s = m_scene->GetSSAOSettings();
            resp["ok"] = true;
            resp["result"] = {{"enabled", s.enabled}, {"radius", s.radius}, {"bias", s.bias},
                              {"intensity", s.intensity}, {"power", s.power},
                              {"sampleCount", s.sampleCount}, {"blur", s.blur}};
        }
        else if (method == "set_ssao")
        {
            auto& s = m_scene->GetSSAOSettings();
            s.enabled = params.value("enabled", s.enabled);
            s.radius = params.value("radius", s.radius);
            s.bias = params.value("bias", s.bias);
            s.intensity = params.value("intensity", s.intensity);
            s.power = params.value("power", s.power);
            s.sampleCount = params.value("sampleCount", s.sampleCount);
            s.blur = params.value("blur", s.blur);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        }
        // ════════════════════════════════════════════════════════════
        //  ビルド/検証パイプライン連携
        // ════════════════════════════════════════════════════════════
        else if (method == "validate_scene")
        {
            std::string rel = params.value("path", std::string());
            fs::path scenePath;
            if (rel.empty())
            {
                if (m_editorCtx->currentScenePath.empty())
                    throw McpError(McpErr::InvalidParam, "no scene currently open and 'path' not given");
                scenePath = m_editorCtx->currentScenePath;
            }
            else
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
                scenePath = fs::path(PathResolver::AssetsDir()) / rel;
            }
            if (!fs::exists(scenePath)) throw McpError(McpErr::NotFound, "scene not found: " + scenePath.string());

            wchar_t exeBuf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
            std::string exePath(fs::path(exeBuf).string());

            // 呼び出しごとに専用の作業ディレクトリで実行(validate_report.txt の競合/汚染回避)。
            static int s_validateSeq = 0;
            fs::path workDir = fs::temp_directory_path() /
                ("dx12_validate_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++s_validateSeq));
            std::error_code ec;
            fs::create_directories(workDir, ec);

            const std::string args = "--validate \"" + scenePath.string() + "\"";
            const int code = RunEngineSubprocessAndWait(exePath, args, workDir.string(), 30000);

            std::string report;
            std::ifstream rf(workDir / "validate_report.txt", std::ios::binary);
            if (rf) { std::ostringstream ss; ss << rf.rdbuf(); report = ss.str(); }
            fs::remove_all(workDir, ec);

            resp["ok"] = true;
            resp["result"] = {{"pass", code == 0}, {"exitCode", code}, {"report", report},
                              {"scenePath", scenePath.string()}};
        }
        else if (method == "build_game")
        {
            const bool ok = BuildGame();
            json result{{"success", ok}};
            if (m_editorCtx)
            {
                result["outputDir"] = m_editorCtx->buildConfig.outputDir.empty()
                    ? (PathResolver::BaseDir() + "build/game") : m_editorCtx->buildConfig.outputDir;
                if (!ok) result["error"] = m_editorCtx->buildErrorMsg;
            }
            resp["ok"] = true;
            resp["result"] = std::move(result);
        }
        // ════════════════════════════════════════════════════════════
        //  Lua 即時実行(eval) — デバッグ用。globals フォールバック環境で実行するため
        //  scene/physics/camera/audio 等の既存グローバルバインディングがそのまま使える。
        //  print() は捕捉されない(log() を使うと dx12_get_log で見える)。
        // ════════════════════════════════════════════════════════════
        else if (method == "eval_lua")
        {
            const std::string code = params.value("code", std::string());
            if (code.empty()) throw McpError(McpErr::InvalidParam, "missing 'code'");
            std::string resultStr, err;
            const bool ok = m_scriptEngine->EvalLua(code, resultStr, err);
            if (!ok) throw McpError(McpErr::Internal, "Lua error: " + err);
            resp["ok"] = true;
            resp["result"] = {{"result", resultStr}};
        }
        // ════════════════════════════════════════════════════════════
        //  マテリアルテクスチャ上書き(Inspector の D&D 割当と同じ経路)
        // ════════════════════════════════════════════════════════════
        else if (method == "set_texture")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<MeshRenderer>(e))
                throw McpError(McpErr::InvalidParam, "entity has no meshRenderer");
            const std::string slot = params.value("slot", std::string("albedo"));
            const u32 submesh = params.value("submesh", 0u);
            std::string rel = params.value("path", std::string());
            if (!rel.empty())
            {
                if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
                    rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
                    throw McpError(McpErr::InvalidParam, "invalid path (assets 相対のみ)");
                if (!fs::exists(fs::path(PathResolver::AssetsDir()) / rel))
                    throw McpError(McpErr::NotFound, "texture not found: " + rel);
            }
            auto& mr = reg.get<MeshRenderer>(e);
            // Material は同一モデルの全インスタンスで共有されるため直接触らず、
            // インスタンス単位の override に書く(描画側 EnsureMaterialOverrideSrv が合成)。
            if      (slot == "albedo")         MeshRenderer::SetOverride(mr.overrideAlbedoTexture, submesh, rel);
            else if (slot == "normal")         MeshRenderer::SetOverride(mr.overrideNormalTexture, submesh, rel);
            else if (slot == "metalRoughness") MeshRenderer::SetOverride(mr.overrideMetalRoughnessTexture, submesh, rel);
            else throw McpError(McpErr::InvalidParam, "slot must be albedo|normal|metalRoughness");
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"slot", slot},
                              {"submesh", submesh}, {"path", rel}};
        }
        // ════════════════════════════════════════════════════════════
        //  スケルタルアニメーション制御(Lua playAnim/playAnimByName と同じ経路)
        // ════════════════════════════════════════════════════════════
        else if (method == "play_anim")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            if (!reg.all_of<SkeletalAnimation>(e))
                throw McpError(McpErr::InvalidParam, "entity has no skeletalAnimation");
            auto& sa = reg.get<SkeletalAnimation>(e);
            if (!sa.animator) throw McpError(McpErr::Internal, "animator not initialized");
            const float blend = params.value("blend", 0.3f);
            int idx = -1;
            if (params.contains("clipName"))
            {
                const std::string want = params["clipName"].get<std::string>();
                for (int i = 0; i < static_cast<int>(sa.clips.size()); ++i)
                    if (sa.clips[i]->GetName() == want) { idx = i; break; }
                if (idx < 0) throw McpError(McpErr::NotFound, "no clip named '" + want + "' (dx12_get_anim_state で一覧を確認)");
            }
            else
            {
                idx = params.value("clip", 0);
                if (idx < 0 || idx >= static_cast<int>(sa.clips.size()))
                    throw McpError(McpErr::InvalidParam, "clip index out of range (0.." +
                        std::to_string(sa.clips.empty() ? 0 : sa.clips.size() - 1) + ")");
            }
            sa.animator->CrossFadeTo(sa.clips[idx].get(), blend);
            if (params.contains("loop")) sa.animator->SetLooping(params["loop"].get<bool>());
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"clip", idx},
                              {"clipName", sa.clips[idx]->GetName()}, {"blend", blend}};
        }
        else if (method == "get_anim_state")
        {
            const auto e = ResolveMcpEntity(*m_scene, params);
            auto& reg = m_scene->GetRegistry();
            json result;
            result["hasSkeletalAnimation"] = reg.all_of<SkeletalAnimation>(e);
            json clips = json::array();
            if (reg.all_of<SkeletalAnimation>(e))
                for (const auto& c : reg.get<SkeletalAnimation>(e).clips)
                    clips.push_back(c->GetName());
            result["clips"] = std::move(clips);
            resp["ok"] = true;
            resp["result"] = std::move(result);
        }
        // ════════════════════════════════════════════════════════════
        //  マルチプレイヤー(フェーズ⑨のローカルテストループを AI から回す)
        // ════════════════════════════════════════════════════════════
        else if (method == "net_status")
        {
            json result;
            result["available"] = (m_networkSystem != nullptr);
            if (m_networkSystem)
            {
                const char* role = m_networkSystem->IsServer() ? "Host"
                                 : m_networkSystem->IsClient() ? "Client" : "Offline";
                result["role"] = role;
                result["isConnected"] = m_networkSystem->IsConnected();
                result["localClientId"] = m_networkSystem->LocalClientId();
                result["tick"] = m_networkSystem->CurrentTick();
                result["syncedEntityCount"] = m_networkSystem->SyncedEntityCount(m_scene->GetRegistry());
                json players = json::array();
                for (const auto& p : m_networkSystem->Players())
                    players.push_back({{"id", p.id}, {"rttMs", p.rttMs},
                                       {"bytesSent", p.bytesSent}, {"bytesReceived", p.bytesReceived}});
                result["players"] = std::move(players);
                const auto& cfg = m_networkSystem->Config();
                result["config"] = {{"tickRate", cfg.tickRate}, {"snapshotRate", cfg.snapshotRate},
                                    {"maxPlayers", cfg.maxPlayers}, {"defaultPort", cfg.defaultPort}};
            }
            const char* testRole = m_editorCtx->netTestRole == NetTestRole::Host ? "host"
                                 : m_editorCtx->netTestRole == NetTestRole::Client ? "client" : "offline";
            result["testRole"] = testRole;
            result["testJoinAddress"] = m_editorCtx->netTestJoinAddress;
            resp["ok"] = true;
            resp["result"] = std::move(result);
        }
        else if (method == "net_setup")
        {
            // ツールバーの Play ロールドロップダウンと同じ状態を書く。次の play で
            // EnterPlayMode が Host/Join を自動実行する(直接 Host/Join は EnterPlayMode の
            // イベント順序保証を壊すのでやらない)。
            const std::string role = params.value("role", std::string());
            if (role == "host")         m_editorCtx->netTestRole = NetTestRole::Host;
            else if (role == "client")  m_editorCtx->netTestRole = NetTestRole::Client;
            else if (role == "offline") m_editorCtx->netTestRole = NetTestRole::Offline;
            else throw McpError(McpErr::InvalidParam, "role must be host|client|offline");
            if (params.contains("address")) m_editorCtx->netTestJoinAddress = params["address"].get<std::string>();
            const int port = params.value("port", 0);
            if (port < 0 || port > 65535) throw McpError(McpErr::InvalidParam, "port must be 0..65535");
            m_editorCtx->netTestJoinPort = static_cast<u16>(port);
            resp["ok"] = true;
            resp["result"] = {{"testRole", role}, {"address", m_editorCtx->netTestJoinAddress},
                              {"port", m_editorCtx->netTestJoinPort}};
        }
        else if (method == "net_launch_test_client")
        {
            if (!m_networkSystem || !m_networkSystem->IsServer())
                throw McpError(McpErr::ModeConflict,
                    "not hosting (dx12_net_setup role=host → dx12_play してからテストクライアントを起動する)");
            // ツールバーの「テストクライアント起動」ボタンと同じ: フレーム境界で CreateProcess。
            m_editorCtx->netTestLaunchClientRequested = true;
            resp["ok"] = true;
            resp["result"] = {{"requested", true},
                              {"note", "second engine process launches at next frame boundary and auto-joins 127.0.0.1"}};
        }
        else
        {
            resp["ok"] = false;
            resp["error"] = "unknown method: " + method;
            resp["error_code"] = McpErr::InvalidParam;
        }
        if (isDeferred) resp["ok"] = true;   // パネル表示用: dispatch 成功(本応答は遅延)
    }
    catch (const McpError& e)
    {
        resp["ok"] = false;
        resp["error"] = e.what();
        resp["error_code"] = e.code;
        isDeferred = false;
    }
    catch (const std::exception& e)
    {
        resp["ok"] = false;
        resp["error"] = e.what();
        resp["error_code"] = McpErr::InvalidParam;   // 大半は引数検証エラー
        isDeferred = false;
    }
    // パネル(MCP / AI Bridge)用にコマンド結果を記録（メインスレッドからのみ）。
    if (m_mcpBridge)
        m_mcpBridge->RecordCommand(method, resp.value("ok", false), resp.value("error", std::string()));
    // 遅延応答は今は送らない(フレーム境界で SendToClient が送る)。Poll が空文字列をスキップ。
    if (isDeferred) return std::string();
    // 不正 UTF-8(例: CP932 のモデル名由来の NameTag)で dump() が例外を投げないよう置換。
    return resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// BGRA8(tightly packed, w*4 stride)を PNG ファイルへ。WIC(OS 標準)で書くので外部依存なし。
static bool WriteBgraPng(const std::wstring& path, const uint8_t* bgra,
                         uint32_t w, uint32_t h, std::string& err)
{
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // 既初期化なら S_FALSE。Uninit はしない(常駐エディタで無害)。

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
    { err = "WIC factory failed"; return false; }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
    { err = "WIC stream open failed"; return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    { err = "WIC encoder init failed"; return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) ||
        FAILED(frame->Initialize(props.Get())))
    { err = "WIC frame init failed"; return false; }

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;   // PNG エンコーダがネイティブ対応＝変換なし
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)))
    { err = "WIC frame setup failed"; return false; }

    const UINT stride = w * 4;
    if (FAILED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgra))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    { err = "WIC write failed"; return false; }

    return true;
}

std::string Application::CaptureSceneScreenshot(std::string& err)
{
    namespace fs = std::filesystem;
    using Microsoft::WRL::ComPtr;

    if (!m_sceneRT || !m_sceneRT->GetResource()) { err = "scene RT not ready"; return {}; }
    if (!m_commandQueue || !m_frameResources)    { err = "gpu not ready";     return {}; }

    auto* dev    = m_graphicsDevice->GetDevice();
    auto* srcTex = m_sceneRT->GetResource();
    const D3D12_RESOURCE_DESC texDesc = srcTex->GetDesc();
    const UINT w = static_cast<UINT>(texDesc.Width);
    const UINT h = texDesc.Height;
    if (w == 0 || h == 0) { err = "scene size is 0"; return {}; }

    // readback バッファのレイアウト(行ピッチは 256B アライン)を取得
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rowCount   = 0;
    UINT64 rowSize    = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rowCount, &rowSize, &totalBytes);

    ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalBytes;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        { err = "readback alloc failed"; return {}; }
    }

    // 前フレームの GPU 完了を待ってから sceneRT(単一リソース＝内容確定)をコピー
    m_commandQueue->WaitIdle();
    auto* cmd = m_frameResources->BeginFrame(*m_commandQueue);

    const D3D12_RESOURCE_STATES prev = m_sceneRT->GetState();
    auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{};
        br.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource   = srcTex;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter  = b;
        br.Transition.Subresource = 0;
        cmd->ResourceBarrier(1, &br);
    };
    const bool needBarrier = (prev != D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (needBarrier) barrier(prev, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = srcTex;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = readback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (needBarrier) barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, prev);  // 元の状態へ戻す(エンジンの追跡と一致)

    if (FAILED(cmd->Close())) { m_frameResources->EndFrame(*m_commandQueue); err = "cmd close failed"; return {}; }
    m_commandQueue->ExecuteCommandList(cmd);
    m_commandQueue->WaitIdle();
    m_frameResources->EndFrame(*m_commandQueue);

    // R16G16B16A16_FLOAT(リニアHDR) → ACES → sRGB OETF → BGRA8。
    // PostProcess 最終段と同じトーンマップを CPU で適用し「ビューポートで見える絵」に寄せる。
    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(totalBytes) };
    if (FAILED(readback->Map(0, &rr, &mapped))) { err = "readback map failed"; return {}; }

    auto aces = [](float x) -> float
    {
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        if (x <= 0.0f) return 0.0f;
        const float v = (x * (a * x + b)) / (x * (c * x + d) + e);
        return v > 1.0f ? 1.0f : v;
    };
    auto toSrgb = [](float c) -> uint8_t
    {
        if (c <= 0.0f) return 0;
        if (c >= 1.0f) return 255;
        c = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        const int v = static_cast<int>(c * 255.0f + 0.5f);
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };

    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < h; ++y)
    {
        const auto* in  = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(fp.Footprint.RowPitch) * y);
        uint8_t*    out = &bgra[static_cast<size_t>(w) * 4 * y];
        for (UINT x = 0; x < w; ++x)
        {
            const float r = DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 0]);
            const float g = DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 1]);
            const float b = DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + 2]);
            out[x * 4 + 0] = toSrgb(aces(b));   // BGRA 順
            out[x * 4 + 1] = toSrgb(aces(g));
            out[x * 4 + 2] = toSrgb(aces(r));
            out[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE wr{ 0, 0 };
    readback->Unmap(0, &wr);

    const fs::path outPath = fs::absolute("mcp_screenshot.png");   // CWD(= dx12_engine.log と同じ場所)へ上書き
    if (!WriteBgraPng(outPath.wstring(), bgra.data(), w, h, err)) return {};
    return outPath.string();
}

void Application::Run()
{
    Logger::Info("Application running...");

    // Windowsタイマー精度を1msに設定
    timeBeginPeriod(1);

    while (!m_window->ShouldClose())
    {
        m_frameStart = std::chrono::high_resolution_clock::now();

        // AI(MCP)から溜まったコマンドをメインスレッドで処理(scene/scriptengine を安全に触れる)。
        if (m_mcpBridge)
            m_mcpBridge->Poll([this](uint64_t client, const std::string& line) {
                return HandleMcpCommand(client, line);
            });

        // --net-client: プロジェクトロード完了後にクライアントとして自動Play=Join(フェーズ⑨)。
        // ※ Update 内で立てると同フレームの EditorLayer::Render 後の
        //    「m_pendingMode = pendingPlayMode ? ...」に Editor へ上書きされるので、
        //    消費直前のここで立てて即座に消費させる。
        if (m_netClientAutoPlayPending && !m_loading && !m_modeChangeRequested
            && m_engineMode == EngineMode::Editor)
        {
            m_netClientAutoPlayPending = false;
            m_pendingMode = EngineMode::Playing;
            m_modeChangeRequested = true;
        }

        // モード切替（前フレームのImGuiボタンから遅延実行）
        if (m_modeChangeRequested)
        {
            m_modeChangeRequested = false;
            try
            {
                if (m_pendingMode == EngineMode::Playing)
                    EnterPlayMode();
                else
                    EnterEditorMode();
            }
            catch (const std::exception& ex)
            {
                Logger::Error("モード切替に失敗: {}", ex.what());
                if (m_engineMode == EngineMode::Playing)
                    m_scriptEngine->OnPlayStop();
                m_engineMode = EngineMode::Editor;
                m_inputSystem->SetMouseCapture(false);
            }

            // MCP play/stop の遅延応答（モード遷移が確定した直後に本物のモードを返す）。
            if (m_mcpModeReply.client != 0)
            {
                const bool wantPlaying = (m_pendingMode == EngineMode::Playing);
                const bool nowPlaying  = (m_engineMode == EngineMode::Playing);
                if (wantPlaying && !nowPlaying)
                    FailMcp(m_mcpBridge.get(), m_mcpModeReply, McpErr::ModeConflict,
                            "play failed (no active camera? check dx12_get_log)");
                else
                    CompleteMcp(m_mcpBridge.get(), m_mcpModeReply,
                        nlohmann::json{{"mode", nowPlaying ? "Playing" : "Editor"},
                                       {"sceneGeneration", m_sceneGeneration}});
                m_mcpModeReply = {};
            }
        }

        // 入力状態リセット（前フレームのdeltaクリア + prevKeys保存 + XInputポーリング）
        m_inputSystem->Update(m_gameClock.GetDeltaTime());

        // メッセージ処理（ここで WM_KEYDOWN/WM_MOUSEMOVE → InputSystem に蓄積）
        m_window->ProcessMessages();

        if (m_window->ShouldClose())
            break;

        // リサイズ処理
        if (m_window->WasResized())
        {
            m_window->ResetResizedFlag();
            u32 w = m_window->GetWidth();
            u32 h = m_window->GetHeight();
            if (w > 0 && h > 0)
            {
                m_commandQueue->WaitIdle();
                m_swapChain->Resize(w, h, *m_descriptorHeap);

                // デプスバッファ再作成
                m_depthBuffer.Reset();
                D3D12_RESOURCE_DESC depthDesc{};
                depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                depthDesc.Width = w;
                depthDesc.Height = h;
                depthDesc.DepthOrArraySize = 1;
                depthDesc.MipLevels = 1;
                depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                depthDesc.SampleDesc = {1, 0};
                depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

                D3D12_CLEAR_VALUE clearValue{};
                clearValue.Format = DXGI_FORMAT_D32_FLOAT;
                clearValue.DepthStencil = {1.0f, 0};

                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

                ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE,
                    &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    &clearValue, IID_PPV_ARGS(&m_depthBuffer)));

                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                    m_depthBuffer.Get(), &dsvDesc, m_dsvHandle);

                if (m_depthSrvIndex != 0xFFFFFFFFu)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
                    depthSrvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
                    depthSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    depthSrvDesc.Texture2D.MipLevels     = 1;
                    m_graphicsDevice->GetDevice()->CreateShaderResourceView(
                        m_depthBuffer.Get(), &depthSrvDesc, m_srvHeap->GetCpuHandle(m_depthSrvIndex));
                }

                // オフスクリーン RT もウィンドウサイズへ作り直す
                if (m_sceneRT)
                    m_sceneRT->Resize(*m_graphicsDevice, w, h);
                // SSAO の AO/Blur RT も同寸へ（深度と同じフル解像度）
                if (m_ssaoPass)
                    m_ssaoPass->Resize(*m_graphicsDevice, w, h);
                // ブルームチェーン（1/2〜1/64）も追従
                if (m_bloomPass)
                    m_bloomPass->Resize(*m_graphicsDevice, w, h);
                if (m_godRaysPass)    m_godRaysPass->Resize(*m_graphicsDevice, w, h);
                if (m_lensFlarePass)  m_lensFlarePass->Resize(*m_graphicsDevice, w, h);
                if (m_dofPass)        m_dofPass->Resize(*m_graphicsDevice, w, h);
                if (m_motionBlurPass) m_motionBlurPass->Resize(*m_graphicsDevice, w, h);
                if (m_distortRT)      m_distortRT->Resize(*m_graphicsDevice, w, h);
                m_prevViewProjValid = false;  // リサイズ直後のMB速度スパイク防止

                // カメラアスペクト比更新（エディタモードではサイドバー分引く）
                m_camera->SetPerspective(DirectX::XM_PIDIV4,
                    static_cast<f32>(w) / static_cast<f32>(h), 0.1f, 1000.0f);

                Logger::Info("Resized to {}x{}", w, h);
            }
        }

        m_gameClock.Tick();

        // シーントランジション更新（WP9）
        if (m_sceneTransition)
            m_sceneTransition->Update(m_gameClock.GetDeltaTime());

        // Luaホットリロード（0.5秒ごとにファイル変更チェック）
        m_scriptPollTimer += m_gameClock.GetDeltaTime();
        if (m_scriptPollTimer >= kScriptPollInterval)
        {
            m_scriptPollTimer = 0.0f;
            std::string scriptPath = PathResolver::GameLuaPath();
            if (std::filesystem::exists(scriptPath))
            {
                auto currentTime = std::filesystem::last_write_time(scriptPath);
                if (currentTime != m_scriptLastWriteTime)
                {
                    Logger::Info("Hot-reload: game.lua changed, reloading...");
                    m_commandQueue->WaitIdle();
                    RebuildScene();
                    m_editorCtx->hotReloadFlash = 2.0f;
                    Logger::Info("Hot-reload complete");
                }
            }
        }

        // シェーダーホットリロード（0.5秒ごとに .hlsl/.hlsli 変更チェック）
        if (m_shaderManager && m_shaderManager->IsRuntimeCompileAvailable())
        {
            m_shaderPollTimer += m_gameClock.GetDeltaTime();
            if (m_shaderPollTimer >= kScriptPollInterval)
            {
                m_shaderPollTimer = 0.0f;
                std::vector<std::wstring> changed = m_shaderManager->Poll();
                if (!changed.empty())
                {
                    m_commandQueue->WaitIdle();
                    m_shaderManager->DispatchReloadHandlers(changed);
                    m_editorCtx->hotReloadFlash = 2.0f;
                }
            }
        }

        // MCP screenshot_game_view: Editor 中は一時的にアクティブなゲームカメラへ切り替えて1フレーム描く。
        // Playing 中は m_camera が既にゲームカメラなので上書き不要(通常 screenshot と同じ絵)。
        const bool gvShot     = (m_mcpGameViewReply.client != 0);
        const bool gvOverride = gvShot && (m_engineMode != EngineMode::Playing);
        DirectX::XMFLOAT3 gvPos{}; f32 gvYaw=0, gvPitch=0, gvFov=0, gvAsp=0, gvNear=0, gvFar=0, gvOrthoH=0;
        bool gvOrtho=false;
        if (gvOverride)
        {
            gvPos=m_camera->GetPosition(); gvYaw=m_camera->GetYaw(); gvPitch=m_camera->GetPitch();
            gvFov=m_camera->GetFovY(); gvAsp=m_camera->GetAspect();
            gvNear=m_camera->GetNearZ(); gvFar=m_camera->GetFarZ();
            gvOrtho=m_camera->IsOrthographic(); gvOrthoH=m_camera->GetOrthoHeight();
        }

        try
        {
            Update();
            if (gvOverride) SyncActiveCameraToGlobal();   // Update の後に上書き(編集カメラ操作に勝つ)
            Render();
        }
        catch (const std::exception& ex)
        {
            Logger::Error("フレーム処理でエラー: {}", ex.what());
            // GPU 状態をリセットして次フレームで復帰を試みる。
            // cmdList が open のまま残ると次の BeginFrame の Reset が失敗し続けて
            // 復帰不能ループになるため、必ず AbortFrame で Close しておく。
            m_commandQueue->WaitIdle();
            if (m_frameResources)
                m_frameResources->AbortFrame();
            if (m_engineMode == EngineMode::Playing)
            {
                m_scriptEngine->OnPlayStop();
                m_engineMode = EngineMode::Editor;
                m_inputSystem->SetMouseCapture(false);
                Logger::Error("エディタモードへ強制復帰しました");
            }
        }

        // 遅延初回表示: 隠れたまま数フレーム描画して絵（ランチャー）が確定してから
        // ウィンドウを出し、スプラッシュを閉じる。表示された瞬間には既に描画済み＝
        // 「白いまま固まって見える/出るタイミングが不安定」が起きない。
        if (m_deferredFirstShow && ++m_warmupFrames >= 3)
        {
            m_deferredFirstShow = false;
            m_window->Show();       // 最大化。直後の微小リサイズは描画継続中に処理される
            SplashScreen::Close();
        }

        // screenshot_game_view: このフレームの描画(ゲームカメラ視点)を撮って遅延応答 → 編集カメラ復元。
        if (gvShot)
        {
            std::string serr;
            const std::string p = CaptureSceneScreenshot(serr);
            if (p.empty())
                FailMcp(m_mcpBridge.get(), m_mcpGameViewReply, McpErr::Internal,
                        serr.empty() ? "screenshot failed" : serr);
            else
                CompleteMcp(m_mcpBridge.get(), m_mcpGameViewReply,
                    nlohmann::json{{"path", p}, {"width", m_sceneRT->GetWidth()},
                                   {"height", m_sceneRT->GetHeight()},
                                   {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"}});
            m_mcpGameViewReply = {};
            if (gvOverride)   // 編集カメラを完全に復元(位置/向き/投影)
            {
                m_camera->SetPosition(gvPos); m_camera->SetYaw(gvYaw); m_camera->SetPitch(gvPitch);
                if (gvOrtho) m_camera->SetOrthographic(gvOrthoH, gvAsp, gvNear, gvFar);
                else         m_camera->SetPerspective(gvFov, gvAsp, gvNear, gvFar);
            }
        }

        // MCP step_frames: 1フレーム回り切ったらカウントダウン。0 になったら遅延応答を返す。
        if (m_mcpStepFramesLeft > 0 && --m_mcpStepFramesLeft == 0 && m_mcpStepReply.client != 0)
        {
            CompleteMcp(m_mcpBridge.get(), m_mcpStepReply,
                nlohmann::json{{"stepped", true},
                               {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                               {"sceneGeneration", m_sceneGeneration}});
            m_mcpStepReply = {};
        }

        // フレームレートリミッター（VSync OFF時のCPU暴走を防止）
        if (!m_useVsync)
        {
            using namespace std::chrono;
            auto targetDuration = duration_cast<high_resolution_clock::duration>(
                duration<f64>(1.0 / static_cast<f64>(kTargetFps)));
            auto elapsed = high_resolution_clock::now() - m_frameStart;
            auto remaining = targetDuration - elapsed;

            // 1ms以上余裕があればSleepで待つ（CPU負荷軽減）
            if (remaining > milliseconds(1))
            {
                std::this_thread::sleep_for(remaining - milliseconds(1));
            }
            // 残りはスピンウェイトで精密に待つ
            while (high_resolution_clock::now() - m_frameStart < targetDuration)
            {
                _mm_pause();
            }
        }
    }

    timeEndPeriod(1);

    Logger::Info("Main loop ended");
}

void Application::Shutdown()
{
    Logger::Info("Application shutting down...");

    // MCP ブリッジを最優先で停止(worker を join)。これより後で Logger/scene/scriptengine を
    // 破棄するので、ここで止めないと worker がそれらを破棄後に触って data race/UAF になる。
    if (m_mcpBridge) m_mcpBridge.reset();

    // ネットワーク接続を明示的に切る（ENetのソケット/ホストをデバイス解放より前に片付ける）。
    if (m_networkSystem) { m_networkSystem->Disconnect(); m_networkSystem.reset(); }

    // 非同期ロードスレッドの回収
    if (m_loadThread.joinable())
        m_loadThread.join();

    // 非同期 git スレッドの回収（join 前に破棄すると std::terminate）。
    // ログイン待ちポーリングは abort で即抜けさせ、git/gh の子プロセスは終わるまで待つ。
    m_gitAbort.store(true);
    if (m_gitThread.joinable())
        m_gitThread.join();

    // GPU の処理完了を待機
    if (m_commandQueue)
    {
        m_commandQueue->WaitIdle();
    }

    // 遅延解放を止めて溜まっている分を全解放（GPU完全停止済みなので安全）。
    // 以後の reset() 群は即時解放に戻る（デバイス解放前に確実に消えるように）
    DeferredRelease::Disable();
    DeferredRelease::FlushAll();

    // ImGui 解放
    if (m_imguiManager)
    {
        m_imguiManager->Shutdown();
        m_imguiManager.reset();
    }

    // リソース解放（逆順）
    m_editorLayer.reset();
    m_editorCtx.reset();
    m_physicsDebugRenderer.reset();
    // 新規レンダラ群（GPU リソース）をデバイス解放より前に明示破棄
    m_editorIconRenderer.reset();
    m_sceneTransition.reset();
    m_spriteRenderer.reset();
    m_postProcess.reset();
    m_bloomPass.reset();      // GPU リソース（チェーンRT/PSO）をデバイス解放より前に明示破棄
    m_autoExposure.reset();   // 同上（UAV バッファ/compute PSO）
    m_godRaysPass.reset();
    m_lensFlarePass.reset();
    m_dofPass.reset();
    m_motionBlurPass.reset();
    m_distortRT.reset();
    m_gpuParticles.reset();
    // SSAO（GPU リソース）をデバイス解放より前に明示破棄
    m_ssaoPass.reset();
    m_ssaoWhiteTex.reset();
    m_depthPrepassPSO.reset();
    m_depthPrepassSkinnedPSO.reset();
    // IBL / Skybox（GPU リソース）をデバイス解放より前に明示破棄。SRV index も srvHeap 生存中に返却。
    m_skyboxRenderer.reset();
    if (m_iblBaker)
    {
        if (m_iblReady && m_srvHeap)
            m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
        m_iblBaker->Reset();
        m_iblBaker.reset();
    }
    if (m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex && m_srvHeap)
    {
        m_srvHeap->Free(m_envCubeSrvIndex);
        m_envCubeSrvIndex = DescriptorHeap::kInvalidIndex;
    }
    m_envCubeTex.reset();
    m_cameraPreviewLdrRT.reset();
    m_cameraPreviewRT.reset();
    m_sceneRT.reset();
    m_offscreenRtvHeap.reset();
    m_sceneFlow.reset();
    if (m_physicsSystem)
    {
        m_physicsSystem->SetEventBus(nullptr);  // EventBus 破棄より前に参照を切る
        m_physicsSystem->Shutdown();
        m_physicsSystem.reset();
    }
    m_inputSystem.reset();
    m_scriptEngine.reset();
    m_audioSystem.reset();
    m_shadowSkinnedPipelineState.reset();
    m_shadowPipelineState.reset();
    m_shadowMap.Reset();
    m_shadowDsvHeap.reset();
    m_gridPipelineState.reset();
    m_skinnedPipelineStateLEqual.reset();
    m_skinnedPipelineState.reset();
    m_scene.reset();
    m_commandList.reset();
    m_perFrameCB.reset();
    m_previewFrameCB.reset();
    m_resourceManager.reset();
    ShaderManager::SetInstance(nullptr);
    m_shaderManager.reset();
    m_srvHeap.reset();
    m_camera.reset();
    m_pipelineStateThumb.reset();
    m_pipelineStateLEqual.reset();
    m_pipelineState.reset();
    m_rootSignature.reset();
    m_depthBuffer.Reset();
    m_dsvHeap.reset();
    m_frameResources.reset();
    m_swapChain.reset();
    m_descriptorHeap.reset();
    m_commandQueue.reset();
    m_graphicsDevice.reset();
    m_window.reset();

    m_isRunning = false;

    Logger::Info("Application shut down complete");
    Logger::Shutdown();
}

void Application::Update()
{
    using namespace DirectX;
    f32 dt = m_gameClock.GetDeltaTime();

    m_framesSinceStart++;

    // 非同期プロジェクトロードの状態機械を進める
    UpdateProjectLoad(dt);
    // 非同期 git 操作の完了回収
    UpdateGitOp();

    // 「テストクライアント起動」ボタン(フェーズ⑨): フレーム境界で別プロセスを起動
    if (m_editorCtx->netTestLaunchClientRequested)
    {
        m_editorCtx->netTestLaunchClientRequested = false;
        LaunchNetTestClient();
    }

    if (m_engineMode == EngineMode::Editor)
    {
        // エディタモード: C++カメラ操作
        bool rightMouseHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        // ウィンドウが非フォーカスならカメラ操作しない
        bool isForeground = (GetForegroundWindow() == m_window->GetHwnd());

        // カーソルが 3D ビューポート上にあるか（ImGui の PassthruCentralNode の
        // WantCaptureMouse 挙動に依存せず、中央ノード矩形で直接判定）。
        // ※ パネル上で右ドラッグしてもフライが暴発しないようにするためのゲート。
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool cursorInViewport = m_editorCtx->IsCursorInViewport(mousePos.x, mousePos.y);

        if (m_framesSinceStart > 5 && rightMouseHeld && !m_inputSystem->IsMouseCaptured()
            && isForeground && cursorInViewport)
        {
            m_inputSystem->SetMouseCapture(true);
        }
        else if (!rightMouseHeld && m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(false);
        }
        // ウィンドウが裏に行ったら強制解除
        if (!isForeground && m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(false);
        }
        if (m_inputSystem->IsMouseCaptured() && !m_editorCtx->view2D)   // 2D中は視点回転/フライ無効
        {
            f32 sensitivity = m_camera->GetMouseSensitivity();
            m_camera->Rotate(
                m_inputSystem->GetMouseDeltaX() * sensitivity,
                -m_inputSystem->GetMouseDeltaY() * sensitivity);

            f32 speed = m_camera->GetMoveSpeed() * dt;
            if (GetAsyncKeyState('W') & 0x8000) m_camera->MoveForward(speed);
            if (GetAsyncKeyState('S') & 0x8000) m_camera->MoveForward(-speed);
            if (GetAsyncKeyState('D') & 0x8000) m_camera->MoveRight(speed);
            if (GetAsyncKeyState('A') & 0x8000) m_camera->MoveRight(-speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m_camera->MoveUp(-speed);
        }

        // 2D中の右ドラッグ: 回転せずパン（マウスユーザー向け。中ドラッグと同じ操作感）。
        // 回転は 0 固定なので MoveRight/MoveUp はワールド X/Y 平行移動になる。
        if (m_inputSystem->IsMouseCaptured() && m_editorCtx->view2D)
        {
            f32 worldPerPixel = (2.0f * m_editorCtx->view2DZoom)
                              / (std::max)(1.0f, m_editorCtx->viewportH);
            m_camera->MoveRight(-m_inputSystem->GetMouseDeltaX() * worldPerPixel);
            m_camera->MoveUp(m_inputSystem->GetMouseDeltaY() * worldPerPixel);
        }

        // --- タッチパッド向け: キーボードフライモード（マウス/ボタン長押し不要）---
        // GetAsyncKeyState はフォーカスに関係なく物理キー状態を読むため、ウィンドウが前面に
        // いる時だけ有効化する（別アプリ作業中の ` / Ctrl+Z / WASD などがエディタに効くのを防ぐ）。
        bool kbActive = isForeground && !ImGui::GetIO().WantCaptureKeyboard;  // 非フォーカス/テキスト入力中は無効
        if (kbActive && (GetAsyncKeyState(VK_OEM_3) & 1))     // ` キーでトグル
            m_editorCtx->flyMode = !m_editorCtx->flyMode;
        if (m_editorCtx->flyMode && kbActive && (GetAsyncKeyState(VK_ESCAPE) & 1))
            m_editorCtx->flyMode = false;

        if (m_editorCtx->flyMode && kbActive && !m_inputSystem->IsMouseCaptured()
            && !m_editorCtx->view2D)   // 2D中はフライ無効（パン/ズームのみ）
        {
            f32 speed = m_camera->GetMoveSpeed() * dt;
            if (GetAsyncKeyState('W') & 0x8000) m_camera->MoveForward(speed);
            if (GetAsyncKeyState('S') & 0x8000) m_camera->MoveForward(-speed);
            if (GetAsyncKeyState('D') & 0x8000) m_camera->MoveRight(speed);
            if (GetAsyncKeyState('A') & 0x8000) m_camera->MoveRight(-speed);
            if (GetAsyncKeyState('E') & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState('Q') & 0x8000) m_camera->MoveUp(-speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m_camera->MoveUp(-speed);

            // 矢印キーで視点回転（マウス不要）
            f32 rot = 1.5f * dt;  // rad/sec
            f32 yawD = 0.0f, pitchD = 0.0f;
            if (GetAsyncKeyState(VK_LEFT)  & 0x8000) yawD   -= rot;
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) yawD   += rot;
            if (GetAsyncKeyState(VK_UP)    & 0x8000) pitchD += rot;
            if (GetAsyncKeyState(VK_DOWN)  & 0x8000) pitchD -= rot;
            if (yawD != 0.0f || pitchD != 0.0f) m_camera->Rotate(yawD, pitchD);
        }

        // --- 2D ビューモード: WASD / 矢印キーでパン（3D の WASD 移動と同じ操作感で左右上下に動かせる）---
        // 2D は回転 0 固定なので MoveRight/MoveUp はそのままワールド X/Y のパンになる。速度はズーム量に比例。
        // ※ W/E/R のギズモ切替は 2D 中は下のブロックで抑止し、ここでは移動だけにする。
        // 右クリック保持中（マウスキャプチャ中）でも A/D・矢印で動かせるよう capture ゲートは付けない。
        if (m_editorCtx->view2D && kbActive)
        {
            f32 pan = (std::max)(0.5f, m_editorCtx->view2DZoom) * 1.5f * dt;
            if ((GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000)) m_camera->MoveRight(pan);
            if ((GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT)  & 0x8000)) m_camera->MoveRight(-pan);
            if ((GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP)    & 0x8000)) m_camera->MoveUp(pan);
            if ((GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN)  & 0x8000)) m_camera->MoveUp(-pan);
        }

        // Ctrl+S でクイック保存
        if (isForeground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('S') & 1))
        {
            if (m_editorCtx->currentScenePath.empty())
            {
                // パス未設定 → 名前入力ダイアログを開く（保存モード）
                m_editorCtx->showNewSceneDialog = true;
                m_editorCtx->newSceneDialogIsCreate = false;
                std::memset(m_editorCtx->newSceneNameBuf, 0, sizeof(m_editorCtx->newSceneNameBuf));
                strncpy_s(m_editorCtx->newSceneNameBuf, "Untitled", _TRUNCATE);
            }
            else
            {
                SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir());
                ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
                m_editorCtx->hotReloadFlash = 1.5f;
                m_editorLayer->RefreshAssetBrowser();
            }
        }

        // Ctrl+N で新規シーン名入力ダイアログを開く
        if (isForeground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('N') & 1))
        {
            m_editorCtx->showNewSceneDialog = true;
            m_editorCtx->newSceneDialogIsCreate = true;
            std::memset(m_editorCtx->newSceneNameBuf, 0, sizeof(m_editorCtx->newSceneNameBuf));
            strncpy_s(m_editorCtx->newSceneNameBuf, "NewScene", _TRUNCATE);
        }

        // Undo/Redo (Ctrl+Z / Ctrl+Y) + Copy/Paste/Duplicate (Ctrl+C/V/D)
        // ImGui のテキスト入力にフォーカスがある時、ウィンドウが裏にある時はエンティティ操作を抑制
        if (isForeground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && !ImGui::GetIO().WantCaptureKeyboard)
        {
            if (GetAsyncKeyState('Z') & 1)
                m_editorCtx->pendingUndo = true;
            if (GetAsyncKeyState('Y') & 1)
                m_editorCtx->pendingRedo = true;

            // コピー (Ctrl+C) — 全コンポーネントを JSON スナップショットで保持
            if (GetAsyncKeyState('C') & 1)
            {
                m_editorCtx->clipboard.clear();
                auto& reg = m_scene->GetRegistry();
                for (auto e : m_editorCtx->selectedEntities)
                {
                    if (!reg.valid(e)) continue;
                    std::string snap = SceneSerializer::SerializeEntity(
                        *m_scene, e, PathResolver::AssetsDir());
                    if (!snap.empty())
                        m_editorCtx->clipboard.push_back(std::move(snap));
                }
            }

            // ペースト (Ctrl+V) — フレーム境界（cmdList 有効時）で生成
            if ((GetAsyncKeyState('V') & 1) && !m_editorCtx->clipboard.empty())
                m_editorCtx->pendingPastes = m_editorCtx->clipboard;

            // 複製 (Ctrl+D) — 全コンポーネントのディープコピー
            if ((GetAsyncKeyState('D') & 1) && m_editorCtx->HasSelection())
            {
                for (auto e : m_editorCtx->selectedEntities)
                    m_editorCtx->pendingDuplications.push_back(e);
            }
        }

        // ギズモモード切替（右クリック中・ImGuiフォーカス中・非フォーカス時は無効）
        if (isForeground && !ImGui::GetIO().WantCaptureKeyboard && !m_inputSystem->IsMouseCaptured())
        {
            // フライモード中・2Dビュー中は W/E/R/T をカメラ移動(パン)に使うのでギズモ切替は抑制
            if (!m_editorCtx->flyMode && !m_editorCtx->view2D)
            {
                if (GetAsyncKeyState('W') & 1) m_editorCtx->gizmoMode = GizmoMode::Translate;
                if (GetAsyncKeyState('E') & 1) m_editorCtx->gizmoMode = GizmoMode::Rotate;
                if (GetAsyncKeyState('R') & 1) m_editorCtx->gizmoMode = GizmoMode::Scale;
                if (GetAsyncKeyState('T') & 1) m_editorCtx->gizmoLocalSpace = !m_editorCtx->gizmoLocalSpace;
            }

            // F: 選択エンティティにフォーカス（Unity 風）
            if ((GetAsyncKeyState('F') & 1) && m_editorCtx->HasSelection())
            {
                auto& reg = m_scene->GetRegistry();
                auto sel = m_editorCtx->selectedEntity;
                if (reg.valid(sel) && reg.all_of<Transform>(sel))
                {
                    const auto& t = reg.get<Transform>(sel);

                    // 対象サイズからフォーカス距離を決める
                    f32 dist = 5.0f;
                    if (reg.all_of<MeshRenderer>(sel))
                    {
                        const auto& mr = reg.get<MeshRenderer>(sel);
                        f32 maxExtent = 0.0f;
                        for (const auto* mesh : mr.meshes)
                        {
                            if (!mesh) continue;
                            auto mn = mesh->GetAABBMin();
                            auto mx = mesh->GetAABBMax();
                            maxExtent = std::max({maxExtent,
                                (mx.x - mn.x) * t.scale.x,
                                (mx.y - mn.y) * t.scale.y,
                                (mx.z - mn.z) * t.scale.z});
                        }
                        if (maxExtent > 0.0f)
                            dist = std::clamp(maxExtent * 2.0f, 2.0f, 100.0f);
                    }

                    // 親階層込みのワールド位置にフォーカス
                    DirectX::XMFLOAT3 wpos = t.position;
                    if (t.parent != entt::null && reg.valid(t.parent))
                    {
                        DirectX::XMFLOAT4X4 wf;
                        XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, sel));
                        wpos = {wf._41, wf._42, wf._43};
                    }

                    auto fwd = m_camera->GetForward();
                    m_camera->SetPosition({wpos.x - fwd.x * dist,
                                           wpos.y - fwd.y * dist,
                                           wpos.z - fwd.z * dist});
                }
            }
        }

    }
    else
    {
        // ネットワーク受信/接続処理（スクリプト実行より前 — このフレームのシムに反映するため）。
        if (m_networkSystem) m_networkSystem->PreSimUpdate(dt, m_scene->GetRegistry());

        // プレイモード: Luaがカメラ+ゲームロジックを制御
        // HUD は実際のゲームビューポート基準でレイアウトさせる。
        // 単体ゲーム=全画面、エディタ Play=中央 16:9 矩形（前フレームの値で1フレーム遅延だが無視できる）。
        if (m_isGameMode)
        {
            m_scriptEngine->SetScreenSize(static_cast<int>(m_window->GetWidth()),
                                          static_cast<int>(m_window->GetHeight()));
        }
        else
        {
            auto vs = m_editorLayer->GetViewportSize();
            int sw = (vs.x >= 1.0f) ? static_cast<int>(vs.x) : static_cast<int>(m_window->GetWidth());
            int sh = (vs.y >= 1.0f) ? static_cast<int>(vs.y) : static_cast<int>(m_window->GetHeight());
            m_scriptEngine->SetScreenSize(sw, sh);
        }
        m_scriptEngine->CallOnUpdate(dt);
        m_scriptEngine->UpdateAttachedScripts(dt);
        m_scriptEngine->UpdateTriggers(dt);   // Trigger（イベント）評価

        // アクティブカメラの Transform をグローバル Camera に同期。
        // 親階層込みのワールド変換で反映するので、親オブジェクトにアタッチした
        // カメラが親の移動・回転に追従する。
        auto& reg = m_scene->GetRegistry();
        auto camSyncView = reg.view<const CameraComponent>();
        for (auto [e, cam] : camSyncView.each())
        {
            if (!cam.isActive) continue;
            ApplyCameraTransformToGlobal(e);
            break;
        }
    }

    // シーン更新（Animator等）— エディタモードは時間を止める（ボーン行列は維持）
    m_scene->Update(m_engineMode == EngineMode::Playing ? dt : 0.0f);

    // 配置パーティクル放出器（ParticleEmitter）を駆動。
    // エディタでも常時プレビュー（実 dt で放出/前進）し、Play では _active に従う。
    if (m_particleSystem)
    {
        auto& peReg = m_scene->GetRegistry();
        const bool pedPlaying = (m_engineMode == EngineMode::Playing);
        auto peView = peReg.view<ParticleEmitter, Transform>();
        for (auto pe_e : peView)
        {
            auto& pe = peView.get<ParticleEmitter>(pe_e);
            const bool live = pedPlaying ? pe._active : true;  // エディタは常時プレビュー
            if (!live) continue;
            if (!pe.looping && pedPlaying)
            {
                pe._age += dt;
                if (pe._age >= pe.duration) pe._active = false;
            }
            if (pe.rate <= 0.0f) continue;
            pe._emitAccum += pe.rate * dt;
            int n = static_cast<int>(pe._emitAccum);
            if (n <= 0) continue;
            pe._emitAccum -= static_cast<f32>(n);
            const int nCap = pe.gpu ? 8192 : 64;   // GPU は大量放出を許容
            if (n > nCap) n = nCap;

            DirectX::XMMATRIX w = ComputeWorldMatrix(peReg, pe_e);
            DirectX::XMFLOAT3 pos; DirectX::XMStoreFloat3(&pos, w.r[3]);

            // GPU パーティクル経路（compute シム。distort/light 等の CPU 専用機能は無視）
            if (pe.gpu && m_gpuParticles)
            {
                GpuParticleSystem::EmitRequest r;
                r.pos = pos;
                r.count = static_cast<u32>(n);
                r.dir = pe.dir;       r.spread = pe.spread;
                r.col0 = { pe.color.x * pe.intensity, pe.color.y * pe.intensity, pe.color.z * pe.intensity };
                r.speed = pe.speed;
                r.col1 = { pe.colorEnd.x * pe.intensity, pe.colorEnd.y * pe.intensity, pe.colorEnd.z * pe.intensity };
                r.speedVar = pe.speedVar;
                r.size0 = pe.size;    r.size1 = pe.sizeEnd;
                r.life = pe.life;     r.lifeVar = pe.lifeVar;
                r.gravity = pe.gravity; r.drag = pe.drag; r.up = pe.up;
                r.kind = pe.kind;     r.stretch = pe.stretch;
                m_gpuParticles->Emit(r);
                continue;
            }

            ParticleSystem::EmitParams p;
            p.pos = pos;            p.count = n;
            p.dir = pe.dir;         p.spread = pe.spread;
            p.speed = pe.speed;     p.speedVar = pe.speedVar;
            p.size = pe.size;       p.sizeEnd = pe.sizeEnd;
            p.life = pe.life;       p.lifeVar = pe.lifeVar;
            p.color = pe.color;     p.colorEnd = pe.colorEnd; p.hasColorEnd = true;
            p.colorMid = pe.colorMid; p.hasColorMid = pe.hasColorMid;
            p.intensity = pe.intensity;
            p.gravity = pe.gravity; p.drag = pe.drag; p.up = pe.up;
            p.stretch = pe.stretch; p.kind = pe.kind; p.blend = pe.blend;
            p.turbStrength = pe.turbStrength; p.turbFreq = pe.turbFreq;
            p.sizeMid = pe.sizeMid; p.distort = pe.distort;
            p.light = pe.light;     p.lightRange = pe.lightRange;
            p.flicker = pe.flicker; p.flickerFreq = pe.flickerFreq;
            p.texturePath = pe.texturePath;
            m_particleSystem->Emit(p);
        }

        // トレイル（軌跡リボン）: エンティティのワールド位置を毎フレーム記録
        auto trView = peReg.view<TrailRenderer, Transform>();
        for (auto tr_e : trView)
        {
            const auto& tr = trView.get<TrailRenderer>(tr_e);
            if (!tr.emitting) continue;
            DirectX::XMMATRIX w = ComputeWorldMatrix(peReg, tr_e);
            DirectX::XMFLOAT3 pos; DirectX::XMStoreFloat3(&pos, w.r[3]);

            ParticleSystem::TrailParams tp;
            tp.width     = tr.width;
            tp.color     = tr.color;
            tp.colorEnd  = tr.colorEnd;
            tp.intensity = tr.intensity;
            tp.life      = tr.life;
            tp.blend     = tr.blend;
            tp.minDist   = tr.minDist;
            m_particleSystem->TrailPoint(static_cast<u64>(tr_e), pos, tp);
        }
    }

    // パーティクル更新（配置エミッタのプレビューのため、エディタでも実 dt で前進）
    if (m_particleSystem)
        m_particleSystem->Update(dt);

    // 物理更新（プレイモードのみ）
    if (m_engineMode == EngineMode::Playing && m_physicsSystem->IsInitialized())
    {
        m_physicsSystem->Update(dt, m_scene->GetRegistry());
    }

    // ネットワーク送信処理（物理確定後の座標を使うため直後。フェーズ⑤でスナップショット送信を実装）。
    if (m_engineMode == EngineMode::Playing && m_networkSystem)
    {
        m_networkSystem->PostSimUpdate(dt, m_scene->GetRegistry());
    }

    // 3D 空間オーディオ: リスナー＝カメラ、AudioSource を駆動（Playing のみ）
    if (m_engineMode == EngineMode::Playing && m_audioSystem)
    {
        auto pos = m_camera->GetPosition();
        auto fwd = m_camera->GetForward();
        m_audioSystem->SetListener(pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z, 0.0f, 1.0f, 0.0f);

        auto& reg = m_scene->GetRegistry();
        for (auto [e, src] : reg.view<AudioSource>().each())
        {
            DirectX::XMFLOAT4X4 wf;
            DirectX::XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, e));
            const float wx = wf._41, wy = wf._42, wz = wf._43;

            if (src.playOnStart && !src.startedThisPlay && !src.clipPath.empty())
            {
                if (src.spatial)
                    src.runtimeSlot = m_audioSystem->PlaySFXSpatial(
                        src.clipPath, wx, wy, wz, src.minDistance, src.maxDistance, src.volume, src.loop);
                else
                    m_audioSystem->PlaySFX(src.clipPath, src.loop);
                src.startedThisPlay = true;
            }
            if (src.runtimeSlot >= 0 && src.spatial)
                m_audioSystem->UpdateSpatialEmitter(src.runtimeSlot, wx, wy, wz);
        }
        m_audioSystem->Update();
    }

    // Trigger の Post や接触 Post を同フレーム内で配信（Playing のみ）。
    if (m_engineMode == EngineMode::Playing)
    {
        m_eventBus.Flush();
    }
}

void Application::RebuildScene()
{
    m_editorCtx->ClearSelection();
    m_scene->Clear();
    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                        m_srvHeap.get(), cmdList);

    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();

    LoadGameScript();

    ThrowIfFailed(cmdList->Close());
    m_commandQueue->ExecuteCommandList(cmdList);
    m_commandQueue->WaitIdle();
    m_resourceManager->FinishUploads();
    m_frameResources->EndFrame(*m_commandQueue);

    // ホットリロード用タイムスタンプ更新
    {
        std::string reloadPath = PathResolver::GameLuaPath();
        if (std::filesystem::exists(reloadPath))
            m_scriptLastWriteTime = std::filesystem::last_write_time(reloadPath);
    }
}

void Application::LoadEditorIcons(ID3D12GraphicsCommandList* cmdList)
{
    auto load = [&](const char* name) -> u64
    {
        std::string p = PathResolver::AssetsDir() + "editor/icons/" + name + ".png";
        if (!std::filesystem::exists(p)) return 0;
        std::wstring wp = PathResolver::Utf8ToWide(p);
        Texture* t = m_resourceManager->GetOrLoadTexture(wp, cmdList, true);
        if (!t) return 0;
        return m_srvHeap->GetGpuHandle(t->GetSrvIndex()).ptr;
    };
    m_icons.logo        = load("logo");
    m_icons.newProject  = load("new_project");
    m_icons.openProject = load("open_project");
    m_icons.recent      = load("recent");
    m_icons.save        = load("save");
    m_icons.git         = load("git");
    m_icons.github      = load("github");
    m_icons.commit      = load("commit");
    m_icons.push        = load("push");

    // ツールバー
    m_icons.file        = load("file");
    m_icons.play        = load("play");
    m_icons.stop        = load("stop");
    m_icons.build       = load("build");
    m_icons.gizmoMove   = load("gizmo_move");
    m_icons.gizmoRotate = load("gizmo_rotate");
    m_icons.gizmoScale  = load("gizmo_scale");
    m_icons.spaceWorld  = load("space_world");
    m_icons.spaceLocal  = load("space_local");
    m_icons.window      = load("window");

    // エンティティ / コンポーネント種別
    m_icons.entMesh     = load("ent_mesh");
    m_icons.entLight    = load("ent_light");
    m_icons.entCamera   = load("ent_camera");
    m_icons.entAudio    = load("ent_audio");
    m_icons.entScript   = load("ent_script");
    m_icons.entPhysics  = load("ent_physics");
    m_icons.entCollider = load("ent_collider");
    m_icons.entEmpty    = load("ent_empty");

    // プロジェクトテンプレート
    m_icons.tmplFps     = load("tmpl_fps");
    m_icons.tmplTps     = load("tmpl_tps");
    m_icons.tmpl2d      = load("tmpl_2d");
    m_icons.tmplEmpty   = load("tmpl_empty");

    // 各パネルが EditorContext 経由で参照できるようにポインタを配る
    if (m_editorCtx)
        m_editorCtx->icons = &m_icons;

    Logger::Info("Editor UI icons loaded");
}

void Application::LoadSkyboxIfNeeded(ID3D12GraphicsCommandList* cmd)
{
    if (!m_scene || !m_iblBaker) return;

    const auto& sky = m_scene->GetSkyboxSettings();
    m_iblIntensity    = sky.iblIntensity;
    m_skyboxIntensity = sky.skyboxIntensity;
    m_drawSkybox      = sky.drawSkybox;

    // パス未指定: IBL 無し（ダミーを1度だけベイクしてテーブルは常に有効化）
    if (sky.envMapPath.empty())
    {
        // 「解放 + ダミー再ベイク」が要るのは、(a) まだ一度もベイクしていない
        // (=!m_iblReady)か、(b) 実 env が読み込まれている(=直前は別シーンで envMap 有り)
        // のいずれか。判定を m_loadedSkyboxPath 文字列ではなく実 env の有無で行うことで、
        // シーンロード時に m_loadedSkyboxPath をクリアしても env→empty 切替を取りこぼさない。
        const bool hasRealEnv = (m_envCubeTex != nullptr) || m_iblBaker->HasEnvironment();
        if (hasRealEnv || !m_iblReady)
        {
            // 既存 env を解放
            if (m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex && m_srvHeap)
                m_srvHeap->Free(m_envCubeSrvIndex);
            m_envCubeSrvIndex = DescriptorHeap::kInvalidIndex;
            m_envCubeTex.reset();
            if (m_iblReady && m_srvHeap)
                m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
            // ダミーベイク（env=null）
            m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, nullptr);
            m_iblReady = m_iblBaker->IsValid();
        }
        m_loadedSkyboxPath = "";
        return;
    }

    // 同一パスかつ既に有効ならスキップ
    if (sky.envMapPath == m_loadedSkyboxPath && m_iblReady && m_iblBaker->HasEnvironment())
        return;

    // 既存 IBL/env を解放してから再ロード
    if (m_iblReady && m_srvHeap)
        m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
    if (m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex && m_srvHeap)
        m_srvHeap->Free(m_envCubeSrvIndex);
    m_envCubeSrvIndex = DescriptorHeap::kInvalidIndex;
    m_envCubeTex.reset();
    m_iblReady = false;

    // VFS 経由でまず試みる（ゲームモード = pak から復号、ディスクモード = loose file）
    {
        auto envBytes = vfs::ReadAsset(sky.envMapPath);
        if (!envBytes.empty())
        {
            m_envCubeTex = TextureLoader::LoadCubeFromMemory(*m_graphicsDevice, cmd,
                               envBytes.data(), envBytes.size(), /*srgb=*/false);
        }
        else
        {
            // disk fallback（ディスクモードかつファイルが存在する場合）
            std::string fullPath = PathResolver::AssetsDir() + sky.envMapPath;
            if (!std::filesystem::exists(fullPath))
            {
                Logger::Warn("スカイボックスの環境マップが見つかりません: {}", fullPath);
                // ダミーベイクしてフォールバック
                m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, nullptr);
                m_iblReady = m_iblBaker->IsValid();
                m_loadedSkyboxPath = sky.envMapPath;
                return;
            }
            std::wstring wpath = PathResolver::Utf8ToWide(fullPath);
            m_envCubeTex = TextureLoader::LoadCubeFromFile(*m_graphicsDevice, cmd, wpath, /*srgb=*/false);
        }
    }
    if (!m_envCubeTex)
    {
        Logger::Warn("スカイボックス（キューブマップ）の読み込みに失敗: {}", sky.envMapPath);
        m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, nullptr);
        m_iblReady = m_iblBaker->IsValid();
        m_loadedSkyboxPath = sky.envMapPath;
        return;
    }

    // env cube の TextureCube SRV を shader-visible ヒープに作成
    m_envCubeSrvIndex = m_srvHeap->AllocateIndex();
    m_envCubeTex->CreateCubeSRV(*m_graphicsDevice,
        m_srvHeap->GetCpuHandle(m_envCubeSrvIndex), m_envCubeTex->GetMipLevels());

    // 派生をベイク
    m_iblBaker->Bake(*m_graphicsDevice, cmd, *m_srvHeap, m_envCubeTex->GetResource());
    m_iblReady = m_iblBaker->IsValid();
    m_loadedSkyboxPath = sky.envMapPath;

    Logger::Info("Skybox loaded: {} (ibl={}, sky={})", sky.envMapPath, m_iblIntensity, m_skyboxIntensity);
}

void Application::BeginProjectLoad(const ProjectInfo& info, bool isNew)
{
    namespace fs = std::filesystem;
    // 直前のスレッドが残っていれば回収
    if (m_loadThread.joinable())
        m_loadThread.join();

    m_loadInfo            = info;
    m_loadIsNew           = isNew;
    m_loading             = true;
    m_showLauncher        = false;
    m_loadProjectStarted  = false;
    m_loadSceneWaitFrames = 0;
    m_loadSpinTime        = 0.0f;
    m_loadThreadDone      = false;
    m_loadStatus          = isNew ? "プロジェクトを作成中..." : "プロジェクトを読み込み中...";

    if (isNew)
    {
        // ディスク作成（フォルダ生成・テンプレ書き出し）はワーカースレッドで
        m_loadThreadRunning = true;
        ProjectInfo copy = info;
        m_loadThread = std::thread([this, copy]()
        {
            Project::CreateDefaultStructure(copy);
            std::string projPath =
                (std::filesystem::path(copy.rootDir) / (copy.name + ".dx12proj")).string();
            Project::Save(copy, projPath);
            ProjectManager::AddToRecents(copy);
            m_loadThreadDone = true;
        });
    }
    else
    {
        // 既存プロジェクト: 重い CPU 処理は無い（シーンの GPU ロードは本スレッドで）
        m_loadThreadRunning = false;
        m_loadThreadDone    = true;
    }
}

void Application::UpdateProjectLoad(f32 dt)
{
    if (!m_loading) return;
    m_loadSpinTime += dt;

    // フェーズ1: 作成スレッドの完了待ち
    if (m_loadThreadRunning)
    {
        if (!m_loadThreadDone.load()) return;  // まだ作成中（スピナー回し続ける）
        if (m_loadThread.joinable()) m_loadThread.join();
        m_loadThreadRunning = false;
    }

    // フェーズ2: LoadProject を一度だけ発火（次フレームの Render で実シーンロード）
    if (!m_loadProjectStarted)
    {
        m_loadStatus = "シーンを読み込み中...";
        LoadProject(m_loadInfo);
        m_loadProjectStarted  = true;
        m_loadSceneWaitFrames = 2;  // pending* が Render で消化されるまで猶予
        return;
    }

    // フェーズ3: シーンロード（pending*）が消化されたら完了
    bool pendingScene = !m_editorCtx->pendingLoadPath.empty() || m_editorCtx->pendingNewScene;
    if (m_loadSceneWaitFrames > 0) --m_loadSceneWaitFrames;
    if (m_loadSceneWaitFrames == 0 && !pendingScene)
    {
        m_loading            = false;
        m_loadProjectStarted = false;
        m_editorCtx->buildCompleteFlash = 1.5f;
    }
}

void Application::RunGitAsync(const std::string& label, std::function<GitResult()> task, bool isLogin)
{
    if (m_gitOpRunning) return;                         // 同時実行は1本だけ（ボタンも無効化済み）
    if (m_gitThread.joinable()) m_gitThread.join();     // 前回スレッドを回収してから再利用

    m_gitOpRunning = true;
    m_gitOpIsLogin = isLogin;
    m_gitOpLabel   = label;
    m_gitOpStatus  = GitOpStatus::Running;
    m_gitSpin      = 0.0f;
    m_gitOpDone.store(false);

    // task はワーカー上で git/gh の子プロセスのみ叩く（ImGui/シーン/GPU には触れない）。
    // 結果を m_gitPending* に書いてから done を立てる＝メインは done 観測後にだけ読む。
    m_gitThread = std::thread([this, task = std::move(task)]() {
        GitResult r = task();
        m_gitPendingOutput = std::move(r.output);
        m_gitPendingOk     = r.ok();
        m_gitOpDone.store(true);                         // RELEASE: 結果を最後に公開
    });
}

void Application::UpdateGitOp()
{
    if (!m_gitOpRunning) return;
    m_gitSpin += m_gameClock.GetDeltaTime();
    if (!m_gitOpDone.load()) return;                     // ACQUIRE: まだワーカー実行中
    if (m_gitThread.joinable()) m_gitThread.join();

    if (m_gitOpIsLogin)
    {
        // ログイン/ユーザー確認: バナーは出さず GitHub 行(●/○)で表す。出力ログも汚さない。
        m_ghUser        = m_gitPendingOutput;            // 空=未ログイン
        m_ghUserChecked = true;
        m_gitOpIsLogin  = false;
        m_gitOpStatus   = GitOpStatus::None;
    }
    else
    {
        m_gitOpStatus = m_gitPendingOk ? GitOpStatus::Success : GitOpStatus::Failure;
        m_gitOutput   = m_gitPendingOutput.empty()
                      ? (m_gitPendingOk ? "完了" : "失敗（出力なし）")
                      : m_gitPendingOutput;
    }

    m_gitForceRefresh = true;                            // ブランチ/リモート/ahead-behind を取り直す
    m_gitOpRunning    = false;
}

void Application::RenderLoadingOverlay()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.09f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##LoadingOverlay", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

    // 回転スピナー（円弧をぐるぐる）
    const float r = 34.0f;
    const int   segs = 28;
    float t = m_loadSpinTime * 3.2f;
    for (int i = 0; i < segs; ++i)
    {
        float a0 = t + (float)i / segs * 6.2831853f;
        float a1 = t + (float)(i + 1) / segs * 6.2831853f;
        float alpha = (float)i / segs;  // フェードする尾
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.39f, 0.58f, 0.93f, alpha));
        dl->AddLine(ImVec2(c.x + cosf(a0) * r, c.y + sinf(a0) * r),
                    ImVec2(c.x + cosf(a1) * r, c.y + sinf(a1) * r), col, 5.0f);
    }

    // ステータステキスト（中央寄せ）
    const char* msg = m_loadStatus.c_str();
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r + 24.0f),
                IM_COL32(235, 235, 235, 255), msg);

    if (!m_loadInfo.name.empty())
    {
        ImVec2 ns = ImGui::CalcTextSize(m_loadInfo.name.c_str());
        dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y + r + 48.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), m_loadInfo.name.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Application::RenderWhatsNewPopup()
{
    if (!m_showWhatsNew) return;
    namespace th = dx12e::theme;

    const char* kId = "更新内容###whatsnew";
    if (!m_whatsNewOpened) { ImGui::OpenPopup(kId); m_whatsNewOpened = true; }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        // 見づらい要望対応: 専用フォントは追加せず、既存フォントを SetWindowFontScale で拡大
        // （ToolbarPanel.cpp のエラーモーダルと同じやり方）。
        ImGui::SetWindowFontScale(1.35f);
        ImGui::PushStyleColor(ImGuiCol_Text, th::Accent);
        ImGui::TextUnformatted(kWhatsNewTitle);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetWindowFontScale(1.18f);
        ImGui::TextWrapped("%s", kWhatsNewBody);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("閉じる", ImVec2(160.0f, 0.0f)))
        {
            // この版は表示済みとして記録 → 次回以降は版が変わるまで出さない。
            WriteShownVersion(kEngineVersion);
            m_showWhatsNew = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::LoadProject(const ProjectInfo& info)
{
    namespace fs = std::filesystem;
    m_projectInfo = info;

    // git 状態をプロジェクトごとに再評価
    m_gitChecked   = false;
    m_gitOutput.clear();

    // 「スキップ（デフォルト）」= 組み込みパスのまま。すでに既定シーンが読み込まれている。
    if (info.rootDir.empty())
    {
        Logger::Info("Using built-in default project (no project root)");
        if (m_window) m_window->SetTitle(L"DX12 Engine");
        return;
    }

    // 1) パスをプロジェクト配下へ再ポイント（shaders はエンジン側を維持）
    PathResolver::SetProjectRoot(info.rootDir);

    // 1.5) プロジェクト独自シェーダー(上書き/自作)を再走査。切替前の PSO が残っている可能性があるので
    //      WaitIdle 後に全リロードキーを差分無視で作り直す(Poll() の逐次差分検知とは別経路)。
    if (m_shaderManager)
    {
        m_shaderManager->OnProjectRootChanged();
        if (m_commandQueue)
            m_commandQueue->WaitIdle();
        m_shaderManager->DispatchReloadHandlers(m_shaderManager->AllKnownReloadKeys());
    }

    // 2) パス依存サブシステムを更新
    m_audioSystem->SetAssetsDir(PathResolver::AssetsDir());
    m_scriptEngine->SetAssetsDir(PathResolver::AssetsDir());
    if (m_networkSystem)
    {
        // 起動時はエンジン側 assets の network.json を読んでいるので、
        // プロジェクトの assets/network.json で読み直す(無ければ既定値)。
        NetworkConfig cfg;
        cfg.Load(PathResolver::AssetsDir() + "network.json");
        m_networkSystem->SetConfig(cfg);
    }
    if (m_editorLayer)
        m_editorLayer->SetAssetRoots(PathResolver::AssetsDir(), PathResolver::ScriptsDir());

    // 3) プロジェクトの game.lua を読み込み直す
    LoadGameScript();
    {
        std::string scriptPath = PathResolver::GameLuaPath();
        if (fs::exists(scriptPath))
            m_scriptLastWriteTime = fs::last_write_time(scriptPath);  // ホットリロード用（エディタ）
    }

    // 4) 開始シーンを決定してロード（フレーム境界で実行）
    std::string sceneRel = info.defaultScene.empty() ? "scenes/default.json" : info.defaultScene;
    std::string sceneFull = PathResolver::AssetsDir() + sceneRel;
    m_currentSceneRel = sceneRel;

    if (fs::exists(sceneFull))
    {
        // 既存シーンを次フレームで安全にロード
        m_editorCtx->pendingLoadPath = sceneFull;
    }
    else
    {
        // 新規プロジェクト: グリッド + 平行光源のスターターシーンを生成して保存
        fs::create_directories(fs::path(sceneFull).parent_path());
        m_editorCtx->pendingNewScenePath = sceneFull;
        m_editorCtx->pendingNewScene = true;
    }

    // 5) ウィンドウタイトルにプロジェクト名
    //    info.name は UTF-8。byte 単位の widen（std::wstring(begin,end)）だと
    //    日本語等のマルチバイトが文字化けするので CP_UTF8 で正しく変換する。
    if (m_window)
    {
        std::wstring title = L"DX12 Engine - ";
        if (!info.name.empty())
        {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, info.name.c_str(),
                                           static_cast<int>(info.name.size()), nullptr, 0);
            if (wlen > 0)
            {
                std::wstring wname(static_cast<size_t>(wlen), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, info.name.c_str(),
                                    static_cast<int>(info.name.size()), wname.data(), wlen);
                title += wname;
            }
        }
        m_window->SetTitle(title);
    }

    Logger::Info("Project loaded: {} ({})", info.name, info.rootDir);
}

void Application::SaveCurrentProject()
{
    namespace fs = std::filesystem;

    // 現在シーンを保存
    if (!m_editorCtx->currentScenePath.empty())
    {
        SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir());
        ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
    }

    // .dx12proj を保存（プロジェクトを開いている場合のみ）
    if (!m_projectInfo.rootDir.empty())
    {
        m_projectInfo.defaultScene    = m_currentSceneRel.empty() ? m_projectInfo.defaultScene : m_currentSceneRel;
        m_projectInfo.lastOpenedScene = m_currentSceneRel;
        std::string projPath = (fs::path(m_projectInfo.rootDir) / (m_projectInfo.name + ".dx12proj")).string();
        Project::Save(m_projectInfo, projPath);
    }
    m_editorCtx->buildCompleteFlash = 2.0f;
    Logger::Info("Project saved");
}

void Application::RenderProjectWindow()
{
    ImGui::Begin("Project");

    if (m_projectInfo.rootDir.empty())
    {
        ImGui::TextDisabled("(組み込みデフォルトプロジェクト)");
    }
    else
    {
        ImGui::Text("名前: %s", m_projectInfo.name.c_str());
        ImGui::TextWrapped("場所: %s", m_projectInfo.rootDir.c_str());
        ImGui::TextWrapped("シーン: %s", m_currentSceneRel.c_str());
    }
    ImGui::Separator();

    auto icon = [](u64 h, float s) { if (h) { ImGui::Image(static_cast<ImTextureID>(h), ImVec2(s, s)); ImGui::SameLine(); } };

    icon(m_icons.save, 22);
    if (ImGui::Button("プロジェクトを保存", ImVec2(-1, 30)))
        SaveCurrentProject();

    icon(m_icons.newProject, 22);
    if (ImGui::Button("新規プロジェクト...", ImVec2(-1, 0)))
    {
        ProjectInfo created;
        if (ProjectManager::NewProjectDialog(created, m_window->GetHwnd()))
            BeginProjectLoad(created, /*isNew=*/true);
    }
    icon(m_icons.openProject, 22);
    if (ImGui::Button("プロジェクトを開く...", ImVec2(-1, 0)))
    {
        ProjectInfo opened;
        if (ProjectManager::OpenProjectDialog(opened, m_window->GetHwnd()))
            BeginProjectLoad(opened, /*isNew=*/false);
    }
    icon(m_icons.recent, 22);
    if (ImGui::Button("ランチャーに戻る", ImVec2(-1, 0)))
        m_showLauncher = true;

    ImGui::End();
}

bool Application::HandleWindowCloseRequest()
{
    if (m_isGameMode) return true;              // GameRuntime.exe: 従来通りそのまま終了

    if (m_showLauncher || m_loading) return true; // ランチャー表示中/ロード中はそのまま終了して良い

    if (m_engineMode == EngineMode::Playing)
    {
        // Play 中にいきなり閉じようとした→まず停止するだけに留める（誤操作で未保存の作業が消えるのを防ぐ）。
        // もう一度 X を押せばプロジェクトを閉じてランチャーへ戻る（上の分岐に入る）。
        m_pendingMode = EngineMode::Editor;
        m_modeChangeRequested = true;
        return false;
    }

    // プロジェクトを開いた状態で X → ファイルメニュー「プロジェクトを閉じる」と同じ扱い。
    // ファイル削除等は不要＝ランチャーに戻すだけ。
    m_showLauncher = true;
    return false;
}

void Application::RenderVersionControlWindow()
{
    // 非表示タブ/折りたたみ時は中身を一切実行しない（git の外部プロセス起動を毎フレーム回さない）。
    // 表示名は "Git 変更" だが ImGui ID は従来通り（### 以降）にしてドッキング配置を維持する。
    if (!ImGui::Begin("Git 変更###Version Control (Git)"))
    {
        ImGui::End();
        return;
    }

    // インストール操作が完了していたら再チェックさせる（このパネルが表示されているフレームでのみ検出）
    if (m_gitInstallPending && !m_gitOpRunning)
    {
        m_gitInstallPending = false;
        m_gitChecked = false;
    }

    // git/gh の存在チェック（1 度だけ、インストール完了時は上で再度リセットされる）
    if (!m_gitChecked)
    {
        m_gitAvailable = GitIntegration::IsGitAvailable();
        m_ghAvailable  = GitIntegration::IsGhAvailable();
        m_gitChecked   = true;
    }

    namespace th = dx12e::theme;

    // 実行中スピナー / 成功・失敗バナー。結果が出るまで残るので「押したのに反映されたか分からん」を無くす。
    auto statusBanner = [&]()
    {
        if (m_gitOpStatus == GitOpStatus::Running)
        {
            const char frames[] = { '|', '/', '-', '\\' };
            char sp = frames[(int)(m_gitSpin * 10.0f) & 3];
            ImGui::PushStyleColor(ImGuiCol_Text, th::Accent);
            ImGui::Text("%c %s 実行中...", sp, m_gitOpLabel.c_str());
            ImGui::PopStyleColor();
        }
        else if (m_gitOpStatus == GitOpStatus::Success)
            ImGui::TextColored(th::Good, "✓ %s 成功", m_gitOpLabel.c_str());
        else if (m_gitOpStatus == GitOpStatus::Failure)
        {
            // pull 等がコンフリクトで止まっただけなら「失敗」ではなく専用の案内に倒す
            // （下のコンフリクト一覧セクションで解消操作ができる）。
            if (m_gitMergeInProgress && !m_gitConflicts.empty())
                ImGui::TextColored(th::Warn, "⚠ %s でコンフリクトが発生したで。下の一覧から解消してや", m_gitOpLabel.c_str());
            else
                ImGui::TextColored(th::Bad, "✗ %s 失敗 (下の出力ログを確認してや)", m_gitOpLabel.c_str());
        }
    };

    // 出力ログ（既定は畳む。エラー時だけ開いて確認）
    auto outputLog = [&]()
    {
        if (m_gitOutput.empty()) return;
        if (ImGui::CollapsingHeader("出力ログ"))
        {
            ImGui::BeginChild("##gitout", ImVec2(0, 120), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(m_gitOutput.c_str());
            ImGui::EndChild();
        }
    };

    if (m_projectInfo.rootDir.empty())
    {
        ImGui::TextWrapped("プロジェクトを開く/作成すると Git を使えるで。");
        ImGui::End();
        return;
    }
    if (!m_gitAvailable)
    {
        ImGui::TextColored(th::Bad, "✗ git が見つからへん。");
        ImGui::BeginDisabled(m_gitOpRunning);
        if (ImGui::Button("Git をインストール"))
        {
            m_gitOutput = "Git をインストール中...（winget があれば自動、無ければブラウザでダウンロード"
                          "ページを開くで。別ウィンドウが出たら指示に従ってや）";
            m_gitInstallPending = true;
            RunGitAsync("Git インストール", [this]{ return GitIntegration::InstallGit(m_gitAbort); });
        }
        ImGui::EndDisabled();
        statusBanner();
        outputLog();
        ImGui::End();
        return;
    }

    const std::string& root = m_projectInfo.rootDir;
    const bool busy = m_gitOpRunning;

    // 新規リポジトリ名の初期値（未入力なら一度だけプロジェクト名で埋める。以降は編集を尊重）
    if (m_gitNewRepoNameBuf[0] == '\0' && !m_projectInfo.name.empty())
        strncpy_s(m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size(),
                  m_projectInfo.name.c_str(), _TRUNCATE);

    // 状態の再取得（ローカル git のみで軽い。開いた瞬間と操作完了直後＋手動「更新」だけ＝定期ヒッチ無し）。
    if (ImGui::IsWindowAppearing() || m_gitForceRefresh)
    {
        m_gitForceRefresh = false;
        m_gitRepoCache = GitIntegration::IsRepo(root);
        m_gitAhead = m_gitBehind = -1;
        if (m_gitRepoCache)
        {
            m_gitBranchCache = GitIntegration::CurrentBranch(root);
            m_gitRemoteCache = GitIntegration::RemoteUrl(root);
            m_gitBranches    = GitIntegration::ListBranches(root);
            m_gitChanges     = GitIntegration::ChangedFiles(root);
            m_gitMergeInProgress = GitIntegration::IsMergeInProgress(root);
            m_gitConflicts       = GitIntegration::ConflictedFiles(root);
            // upstream に対する未取得/未送信コミット数（VS の ↓/↑）。upstream 無しは失敗→-1のまま。
            auto rl = GitIntegration::RunGit(root, "rev-list --left-right --count @{upstream}...HEAD");
            int behind = 0, ahead = 0;
            if (rl.ok() && sscanf_s(rl.output.c_str(), "%d %d", &behind, &ahead) == 2)
            { m_gitBehind = behind; m_gitAhead = ahead; }
        }
        else
        {
            m_gitBranchCache.clear(); m_gitRemoteCache.clear(); m_gitBranches.clear(); m_gitChanges.clear();
            m_gitMergeInProgress = false; m_gitConflicts.clear();
        }
    }

    auto icon = [](u64 h, float s) { if (h) { ImGui::Image(static_cast<ImTextureID>(h), ImVec2(s, s)); ImGui::SameLine(); } };

    // GitHub アカウント行（ログイン状態 + ログインボタン）。リポジトリの有無に関わらず使うので
    // 共通化（未初期化の空状態でも、初回からログイン導線を出すため）。
    auto renderGitHubAccountRow = [&]()
    {
        if (!m_ghUserChecked && !busy)
        {
            m_ghUserChecked = true;
            RunGitAsync("GitHub確認", []{
                GitResult r; r.output = GitIntegration::GitHubUser(); r.exitCode = 0; return r;
            }, /*isLogin*/ true);
        }
        ImGui::AlignTextToFramePadding();
        if (!m_ghUser.empty())
            ImGui::TextColored(th::Good, "● @%s", m_ghUser.c_str());
        else
        {
            ImGui::TextColored(th::Warn, "○ 未ログイン");
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("GitHub にログイン"))
            {
                m_gitOutput = "別ウィンドウでブラウザ認証してや。完了したら自動で反映されるで。";
                // gh auth login --web の子プロセス終了をそのまま待つ＝ブラウザ承認した瞬間に
                // 検知できる（ポーリングより速い。詳細は GitIntegration::LoginAndWait 参照）。
                RunGitAsync("GitHubログイン待ち",
                    [this]{ return GitIntegration::LoginAndWait(m_gitAbort); }, /*isLogin*/ true);
            }
            ImGui::EndDisabled();
        }
    };

    // GitHub に新規リポジトリ作成 → push。needInit=true ならローカル未初期化の状態から面倒見る
    // （「初期化」という別操作を挟まず、「リポジトリ作成」一発で完結させるため）。
    // repoName が空ならプロジェクト名にフォールバック。
    auto createGitHubRepo = [&](bool isPrivate, bool needInit, const std::string& commitMsg,
                                 const std::string& repoName)
    {
        SaveCurrentProject();
        std::string n = repoName.empty() ? m_projectInfo.name : repoName, m = commitMsg;
        RunGitAsync(isPrivate ? "リポジトリ作成(private)" : "リポジトリ作成(public)",
            [root, n, m, isPrivate, needInit]{
                if (needInit)
                {
                    auto i = GitIntegration::Init(root);
                    if (!i.ok()) return i;
                }
                auto c = GitIntegration::CommitAll(root, m);
                bool nothingStaged = GitIntegration::RunGit(root, "diff --cached --quiet").ok();
                if (!c.ok() && !nothingStaged) return c;   // 本当のコミット失敗 → 作成せず失敗
                auto r = GitIntegration::CreateGitHubRepo(root, n, isPrivate);
                r.output = c.output + "\n----\n" + r.output;
                return r;
            });
    };

    // ================= リポジトリ未初期化 =================
    if (!m_gitRepoCache)
    {
        ImGui::TextWrapped("このプロジェクトはまだ Git リポジトリやないで。");
        ImGui::Spacing();
        statusBanner();
        ImGui::Spacing();

        if (m_ghAvailable)
        {
            renderGitHubAccountRow();
            ImGui::Spacing();

            ImGui::TextDisabled("リポジトリ名");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##reponame", m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size());

            ImGui::BeginDisabled(busy || m_ghUser.empty() || m_gitNewRepoNameBuf[0] == '\0');
            icon(m_icons.github, 22);
            if (ImGui::Button("GitHub にリポジトリを作成 (Public)", ImVec2(-1, 32)))
                createGitHubRepo(/*isPrivate=*/false, /*needInit=*/true, "Initial commit",
                                  m_gitNewRepoNameBuf.data());
            icon(m_icons.github, 22);
            if (ImGui::Button("GitHub にリポジトリを作成 (Private)", ImVec2(-1, 32)))
                createGitHubRepo(/*isPrivate=*/true, /*needInit=*/true, "Initial commit",
                                  m_gitNewRepoNameBuf.data());
            ImGui::EndDisabled();
            if (m_ghUser.empty())
                ImGui::TextDisabled("↑ 先に GitHub にログインしてや");

            ImGui::Spacing();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("ローカルだけで管理する（GitHub には後で公開）"))
                RunGitAsync("初期化", [root]{ return GitIntegration::Init(root); });
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::BeginDisabled(busy);
            icon(m_icons.git, 22);
            if (ImGui::Button("Git リポジトリを初期化", ImVec2(-1, 32)))
                RunGitAsync("初期化", [root]{ return GitIntegration::Init(root); });
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("クローン");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##cloneurl", "https://github.com/owner/repo.git",
                                 m_gitCloneBuf.data(), m_gitCloneBuf.size());
        ImGui::BeginDisabled(busy || m_gitCloneBuf[0] == '\0');
        if (ImGui::Button("このプロジェクトの隣にクローン", ImVec2(-1, 0)))
        {
            std::string url    = m_gitCloneBuf.data();
            std::string parent = std::filesystem::path(root).parent_path().string();
            RunGitAsync("クローン", [url, parent]{
                std::string outDir;
                return GitIntegration::Clone(url, parent, outDir);
            });
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        outputLog();
        ImGui::End();
        return;
    }

    // ================= リポジトリあり（VS「Git 変更」風レイアウト）=================
    const std::string& branch = m_gitBranchCache;
    const std::string& remote = m_gitRemoteCache;
    const float fh = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemSpacing.x;

    // ---- 行1: ブランチ コンボ（全幅）。ドロップダウン内で新規ブランチも作れる ----
    {
        ImGui::BeginDisabled(busy);
        const char* curBr = branch.empty() ? "(未コミット)" : branch.c_str();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##branch", curBr))
        {
            for (const auto& b : m_gitBranches)
                if (ImGui::Selectable(b.c_str(), b == branch) && b != branch)
                {
                    std::string target = b;
                    RunGitAsync("ブランチ切替", [root, target]{ return GitIntegration::CheckoutBranch(root, target); });
                }
            ImGui::Separator();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##nb", "+ 新規ブランチ名 → Enter",
                    m_gitNewBranchBuf.data(), m_gitNewBranchBuf.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue) && m_gitNewBranchBuf[0] != '\0')
            {
                std::string nb = m_gitNewBranchBuf.data();
                RunGitAsync("ブランチ作成", [root, nb]{ return GitIntegration::CreateBranch(root, nb); });
                m_gitNewBranchBuf.fill('\0');
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    // ---- 行2: GitHub アカウント ----
    if (m_ghAvailable)
        renderGitHubAccountRow();

    // ---- リポジトリ URL（リモートがある間は常時表示。クリックでブラウザを開く）----
    if (!remote.empty())
    {
        std::string webUrl = GitIntegration::ToWebUrl(remote);
        if (!webUrl.empty() && ImGui::SmallButton(("🔗 " + webUrl).c_str()))
            ShellExecuteA(nullptr, "open", webUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ---- 行3: 同期ツールバー（更新 / ↑↓ / フェッチ / プル↓ / プッシュ↑）----
    {
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("更新")) m_gitForceRefresh = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("変更とブランチ状態を取り直す");
        if (!remote.empty())
        {
            ImGui::SameLine(0, sp * 2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("↑%d ↓%d", m_gitAhead < 0 ? 0 : m_gitAhead, m_gitBehind < 0 ? 0 : m_gitBehind);
            ImGui::SameLine();
            if (ImGui::SmallButton("フェッチ"))
                RunGitAsync("フェッチ", [root]{ return GitIntegration::Fetch(root); });
            ImGui::SameLine();
            if (ImGui::ArrowButton("##pull", ImGuiDir_Down))
                RunGitAsync("プル", [root]{ return GitIntegration::Pull(root); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("プル（受信 ↓）");
            ImGui::SameLine();
            if (ImGui::ArrowButton("##push", ImGuiDir_Up))
                RunGitAsync("プッシュ", [root]{ return GitIntegration::Push(root, true); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("プッシュ（送信 ↑）");
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    statusBanner();
    ImGui::Separator();

    // ---- コンフリクト一覧（pull/merge が競合で止まっている間だけ表示）----
    if (!m_gitConflicts.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.12f, 0.12f, 0.35f));
        ImGui::BeginChild("##conflicts", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextColored(th::Bad, "⚠ コンフリクト %zu 件（解消してからコミットしてや）", m_gitConflicts.size());
        for (const auto& path : m_gitConflicts)
        {
            ImGui::PushID(path.c_str());
            ImGui::TextUnformatted(path.c_str());
            ImGui::BeginDisabled(busy);
            ImGui::SameLine();
            if (ImGui::SmallButton("自分優先"))
                RunGitAsync("コンフリクト解消", [root, path]{ return GitIntegration::ResolveOurs(root, path); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("HEAD（自分側）の内容で解消する");
            ImGui::SameLine();
            if (ImGui::SmallButton("相手優先"))
                RunGitAsync("コンフリクト解消", [root, path]{ return GitIntegration::ResolveTheirs(root, path); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("取り込んだ側（pull元/マージ元）の内容で解消する");
            ImGui::SameLine();
            if (ImGui::SmallButton("開く"))
                GitIntegration::OpenConflictFile(root, path);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("VSCode（無ければ既定アプリ）で開いて手動編集する");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    else if (m_gitMergeInProgress)
    {
        ImGui::TextColored(th::Good, "✓ コンフリクトは全部解消したで。下でコミットしてマージを終わらせてや。");
        ImGui::Spacing();
    }

    // ---- コミットメッセージ（複数行・空ならプレースホルダを重ね描き）----
    ImVec2 msgPos = ImGui::GetCursorScreenPos();
    ImGui::InputTextMultiline("##commitmsg", m_gitCommitMsgBuf.data(), m_gitCommitMsgBuf.size(),
                              ImVec2(-FLT_MIN, fh * 2.2f));
    if (m_gitCommitMsgBuf[0] == '\0')
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(msgPos.x + 6, msgPos.y + ImGui::GetStyle().FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), "メッセージを入力してください <必須>");

    // ---- コミット スプリットボタン（既定=コミット、▼=コミット&プッシュ）----
    auto doCommit = [&](bool alsoPush)
    {
        SaveCurrentProject();                            // シーン保存はメインスレッドで
        std::string msg = m_gitCommitMsgBuf.data();
        if (alsoPush)
            RunGitAsync("コミット&プッシュ", [root, msg]{
                auto c = GitIntegration::CommitAll(root, msg);
                // コミット失敗が「変更なし」(良性)か本当の失敗(hook却下/identity未設定)かを判定。
                bool nothingStaged = GitIntegration::RunGit(root, "diff --cached --quiet").ok();
                auto p = GitIntegration::Push(root, true);
                p.output = c.output + "\n----\n" + p.output;
                if (!c.ok() && !nothingStaged)
                    p.exitCode = c.exitCode ? c.exitCode : 1;  // 本当のコミット失敗は全体失敗
                return p;
            });
        else
            RunGitAsync("コミット", [root, msg]{ return GitIntegration::CommitAll(root, msg); });
    };
    // マージ解消後、選んだ側が HEAD と同一内容だと通常の変更差分は0件になる（それでもマージコミットとして
    // 成立する = git commit は成功する）ので、mid-merge のときは変更0件でもコミットボタンを塞がない。
    const bool canCommit = (!m_gitChanges.empty() || m_gitMergeInProgress) && m_gitCommitMsgBuf[0] != '\0';
    ImGui::BeginDisabled(busy || !canCommit);
    icon(m_icons.commit, 18);
    if (ImGui::Button("すべてをコミット", ImVec2(ImGui::GetContentRegionAvail().x - fh - 1.0f, 0)))
        doCommit(false);
    ImGui::SameLine(0, 1);
    if (ImGui::ArrowButton("##commitdrop", ImGuiDir_Down))
        ImGui::OpenPopup("##commitopts");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##commitopts"))
    {
        if (ImGui::Selectable("コミット"))                 doCommit(false);
        if (!remote.empty() && ImGui::Selectable("コミット & プッシュ")) doCommit(true);
        ImGui::EndPopup();
    }

    ImGui::Spacing();

    // ---- 変更 (N) ツリー ----
    std::string changesHdr = "変更 (" + std::to_string(m_gitChanges.size()) + ")###changes";
    if (ImGui::CollapsingHeader(changesHdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (m_gitChanges.empty())
            ImGui::TextDisabled("変更なし（クリーン）");
        else
        {
            // パスを '/' で分割して階層ツリーを構築
            struct TNode { std::map<std::string, TNode> dirs; std::vector<std::pair<std::string, char>> files; };
            TNode rootNode;
            for (const auto& ch : m_gitChanges)
            {
                TNode* cur = &rootNode;
                size_t start = 0;
                for (;;)
                {
                    size_t slash = ch.path.find('/', start);
                    if (slash == std::string::npos)
                    { cur->files.emplace_back(ch.path.substr(start), ch.status); break; }
                    cur = &cur->dirs[ch.path.substr(start, slash - start)];
                    start = slash + 1;
                }
            }
            auto stColor = [&](char st) -> ImVec4 {
                switch (st) {
                    case 'A': return th::Good;
                    case 'M': return th::Warn;
                    case 'R': return th::Accent;
                    case 'D': case 'U': return th::Bad;
                    default:  return th::TextDim;
                }
            };
            std::function<void(const TNode&)> draw = [&](const TNode& n)
            {
                for (const auto& kv : n.dirs)
                    if (ImGui::TreeNodeEx(kv.first.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
                    { draw(kv.second); ImGui::TreePop(); }
                for (const auto& f : n.files)
                {
                    ImGui::TreeNodeEx(f.first.c_str(), ImGuiTreeNodeFlags_Leaf
                        | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
                    char s[2] = { f.second, 0 };
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(s).x - 2.0f);
                    ImGui::TextColored(stColor(f.second), "%s", s);
                }
            };
            ImGui::BeginChild("##changes", ImVec2(0, 220), true);
            draw(rootNode);
            ImGui::EndChild();
        }
    }

    // ---- リモート未設定: URL 手動設定 + GitHub 新規作成 ----
    if (remote.empty())
    {
        ImGui::SeparatorText("リモート未設定");
        ImGui::TextDisabled("プッシュ先がまだ無いで。URL 設定か GitHub 新規作成してや。");
        ImGui::SetNextItemWidth(-70);
        ImGui::InputTextWithHint("##remote", "https://github.com/owner/repo.git",
                                 m_gitRemoteBuf.data(), m_gitRemoteBuf.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || m_gitRemoteBuf[0] == '\0');
        if (ImGui::Button("設定", ImVec2(-FLT_MIN, 0)))
        {
            std::string url = m_gitRemoteBuf.data();
            RunGitAsync("リモート設定", [root, url]{ return GitIntegration::AddRemote(root, url); });
        }
        ImGui::EndDisabled();

        if (m_ghAvailable)
        {
            ImGui::TextDisabled("リポジトリ名");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##reponame2", m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size());

            ImGui::BeginDisabled(busy || m_gitNewRepoNameBuf[0] == '\0');
            std::string msg2 = m_gitCommitMsgBuf.data();
            if (ImGui::Button("GitHub に作成 (private) & push", ImVec2(-FLT_MIN, 0)))
                createGitHubRepo(/*isPrivate=*/true, /*needInit=*/false, msg2, m_gitNewRepoNameBuf.data());
            if (ImGui::Button("GitHub に作成 (public) & push",  ImVec2(-FLT_MIN, 0)))
                createGitHubRepo(/*isPrivate=*/false, /*needInit=*/false, msg2, m_gitNewRepoNameBuf.data());
            ImGui::EndDisabled();
        }
    }

    ImGui::Spacing();
    outputLog();

    ImGui::End();
}

void Application::WireScriptCallbacks()
{
    if (!m_scriptEngine) return;

    m_scriptEngine->SetLoadSceneCallback(
        [this](const std::string& rel) { m_editorCtx->pendingGameLoadPath = rel; });

    m_scriptEngine->SetNextSceneCallback(
        [this]() {
            if (m_sceneFlow)
            {
                std::string n = m_sceneFlow->Next(m_currentSceneRel);
                if (!n.empty()) m_editorCtx->pendingGameLoadPath = n;
            }
        });

    m_scriptEngine->SetQuitCallback(
        [this]() { if (m_window) PostMessageW(m_window->GetHwnd(), WM_CLOSE, 0, 0); });

    m_scriptEngine->SetTransitionCallback(
        [this](const std::string& rel, int type, float dur) {
            if (!m_sceneTransition) return;
            m_transitionTargetScene = rel;
            m_sceneTransition->Start(static_cast<TransitionType>(type), dur);
        });

    // ゲーム内 UI（即時モード）コールバック
    m_scriptEngine->SetUiCallbacks(
        [this](float x, float y, const std::string& text, float size, float r, float g, float b, float a) {
            UICommand c; c.type = UICommand::Type::Text;
            c.x = x; c.y = y; c.size = size; c.text = text;
            c.r = r; c.g = g; c.b = b; c.a = a;
            m_uiCommands.push_back(std::move(c));
        },
        [this](float x, float y, float w, float h, const std::string& label) -> bool {
            UICommand c; c.type = UICommand::Type::Button;
            c.x = x; c.y = y; c.w = w; c.h = h; c.text = label;
            m_uiCommands.push_back(std::move(c));
            // 前フレームに押されたか
            return m_pressedButtons.count(label) > 0;
        },
        [this](float x, float y, float w, float h, const std::string& path) {
            UICommand c; c.type = UICommand::Type::Image;
            c.x = x; c.y = y; c.w = w; c.h = h; c.text = path;
            m_uiCommands.push_back(std::move(c));
        });

    m_scriptEngine->SetUiRectCallback(
        [this](float x, float y, float w, float h, float r, float g, float b, float a, float rounding) {
            UICommand c; c.type = UICommand::Type::Rect;
            c.x = x; c.y = y; c.w = w; c.h = h; c.size = rounding;
            c.r = r; c.g = g; c.b = b; c.a = a;
            m_uiCommands.push_back(std::move(c));
        });

    // C++ EventBus を ScriptEngine へ注入（events:on/emit/clear が薄いバインドになる）。
    m_scriptEngine->SetEventBus(&m_eventBus);

    // マルチプレイシステムを Lua net API へ注入（net:host/join 等が薄いバインドになる）。
    m_scriptEngine->SetNetworkSystem(m_networkSystem.get());
}

void Application::ApplyCameraTransformToGlobal(entt::entity camEntity)
{
    using namespace DirectX;
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(camEntity) || !reg.all_of<Transform>(camEntity)) return;
    const auto& tf = reg.get<Transform>(camEntity);

    // ローカル euler からカメラ規約の forward を構築（pitch 反転は従来同様。
    // エディタのギズモ/フラスタム(Euler)と Camera(forward.y=sin(pitch)) は符号が逆）。
    // 親なしのときはこの forward がそのまま使われ、従来挙動と完全に一致する。
    const f32 yawL   = XMConvertToRadians(tf.rotation.y);
    const f32 pitchL = XMConvertToRadians(-tf.rotation.x);
    const f32 cosP   = std::cos(pitchL);
    XMVECTOR fwd = XMVectorSet(std::sin(yawL) * cosP, std::sin(pitchL), std::cos(yawL) * cosP, 0.0f);

    // 親階層のワールド回転を forward に乗せる＝親オブジェクトの回転に追従する。
    if (tf.parent != entt::null && reg.valid(tf.parent))
        fwd = XMVector3TransformNormal(fwd, ComputeWorldMatrix(reg, tf.parent));
    fwd = XMVector3Normalize(fwd);

    XMFLOAT3 f; XMStoreFloat3(&f, fwd);
    const f32 yaw   = std::atan2(f.x, f.z);
    const f32 pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));

    // 親階層込みのワールド位置を抽出（親オブジェクトの移動に追従する）。
    XMFLOAT3 worldPos; XMStoreFloat3(&worldPos, ComputeWorldMatrix(reg, camEntity).r[3]);

    m_camera->SetPosition(worldPos);
    m_camera->SetYaw(yaw);
    m_camera->SetPitch(pitch);
}

void Application::SyncActiveCameraToGlobal()
{
    auto& reg = m_scene->GetRegistry();
    auto camView = reg.view<const CameraComponent, const Transform>();
    for (auto [e, cam, tf] : camView.each())
    {
        if (!cam.isActive) continue;
        // 位置・向きは親階層込みのワールド変換で同期（親にアタッチしたカメラの追従）。
        ApplyCameraTransformToGlobal(e);
        const f32 camAspect =
            static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight());
        if (cam.projection == CameraProjection::Orthographic)
            m_camera->SetOrthographic(2.0f * cam.orthoSize, camAspect, cam.nearClip, cam.farClip);
        else
            m_camera->SetPerspective(
                DirectX::XMConvertToRadians(cam.fovDegrees), camAspect, cam.nearClip, cam.farClip);
        break;
    }
}

void Application::DoRuntimeSceneLoad(const std::string& rel, ID3D12GraphicsCommandList* cmdList)
{
    std::string full = PathResolver::AssetsDir() + rel;
    // ゲームモードはシーンが pak 内＝ディスクに無いので vfs::Exists で確認（エディタはディスク）。
    if (!dx12e::vfs::Exists(rel))
    {
        Logger::Warn("loadScene: シーンが見つかりません: {}", rel);
        return;
    }

    // 物理リセット
    m_physicsSystem->UnregisterAllBodies(m_scene->GetRegistry());
    m_physicsSystem->UnregisterAllCharacters(m_scene->GetRegistry());
    m_physicsSystem->Shutdown();
    m_physicsSystem->Initialize();

    // シーン再構築
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(), m_srvHeap.get(), cmdList);
    if (!SceneSerializer::Load(*m_scene, full, PathResolver::AssetsDir()))
    {
        Logger::Warn("loadScene: 読み込みに失敗しました: {}", full);
        return;
    }
    m_editorCtx->currentScenePath = full;
    m_currentSceneRel = rel;

    // ScriptEngine 作り直し（コールバック再注入）
    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(), m_camera.get(),
                               m_audioSystem.get(), m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();
    LoadGameScript();

    // アクティブカメラがなければ最初のものを有効化
    {
        auto& reg = m_scene->GetRegistry();
        auto camView = reg.view<CameraComponent>();
        bool hasActive = false;
        for (auto [e, c] : camView.each()) if (c.isActive) { hasActive = true; break; }
        if (!hasActive && !camView.empty())
            reg.get<CameraComponent>(*camView.begin()).isActive = true;
    }

    m_eventBus.Clear();   // 前シーンの購読を消去（ランタイムシーン切替）
    m_scriptEngine->OnPlayStart();
    if (m_particleSystem) m_particleSystem->Clear();  // シーン切替時に前シーンの粒子を消す
    if (m_gpuParticles) m_gpuParticles->Clear();
    SyncActiveCameraToGlobal();

    // 新シーンの RigidBody を物理登録
    {
        auto& reg = m_scene->GetRegistry();
        for (auto [e, rb] : reg.view<RigidBody>().each())
        {
            if (rb.bodyId != kInvalidBodyId) m_physicsSystem->UnregisterBody(reg, e);
            m_physicsSystem->RegisterBody(reg, e);
        }
    }

    // 新シーンの CharacterController を CharacterVirtual として生成
    {
        auto& reg = m_scene->GetRegistry();
        for (auto [e, cc] : reg.view<CharacterController>().each())
        {
            if (cc._registered) m_physicsSystem->UnregisterCharacter(reg, e);
            m_physicsSystem->RegisterCharacter(reg, e);
        }
    }
    m_physicsSystem->ResetAccumulator();

    // ランタイムでシーンが切り替わったので、新シーンの SkyboxSettings で IBL/skybox を
    // 次フレーム冒頭に再ベイク。m_loadedSkyboxPath はクリアして旧パス一致のスキップを防ぐ。
    m_loadedSkyboxPath.clear();
    m_skyboxDirty = true;

    Logger::Info("Runtime scene loaded: {}", rel);
}

void Application::EnsureEditorGrid()
{
    // 封印ランタイム(ゲーム)では編集用グリッドは不要。
    if (m_isGameMode)
        return;

    auto& reg = m_scene->GetRegistry();

    // 既に GridPlane を持つエンティティがあれば何もしない（二重生成を防ぐ）。
    auto gridView = reg.view<GridPlane>();
    if (gridView.begin() != gridView.end())
        return;

    // グリッド未配置のシーン（旧データ / Grid 無しテンプレ）に編集用グリッドを追加。
    // SpawnPlane はメッシュ生成に Scene の cmdList を使うので、呼び出し側で有効化済みであること。
    m_scene->SpawnPlane("Grid", {0, 0, 0}, kEditorGridSize, true);
    Logger::Info("EnsureEditorGrid: グリッド未配置のシーンへ編集用グリッドを追加");
}

void Application::LaunchNetTestClient()
{
    // ホスト(自分)の待ち受けポートへ 127.0.0.1 で自動接続する検証用の別ウィンドウを起動する。
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    const u16 port = m_networkSystem ? m_networkSystem->Config().defaultPort : u16(7777);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --editor --net-client 127.0.0.1:"
                     + std::to_wstring(port);
    if (!m_projectInfo.rootDir.empty())
    {
        // rootDir は UTF-8。日本語パスでも化けないように wide へ変換してから渡す。
        int n = MultiByteToWideChar(CP_UTF8, 0, m_projectInfo.rootDir.c_str(), -1, nullptr, 0);
        std::wstring wroot(n > 0 ? static_cast<size_t>(n - 1) : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, m_projectInfo.rootDir.c_str(), -1, wroot.data(), n);
        cmd += L" --project \"" + wroot + L"\"";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Logger::Info("テストクライアントを起動しました (127.0.0.1:{})", port);
    }
    else
    {
        Logger::Warn("テストクライアントの起動に失敗しました (GetLastError={})", GetLastError());
    }
}

void Application::EnterPlayMode()
{
    // カメラ設置チェック
    {
        auto& reg = m_scene->GetRegistry();
        auto camCheck = reg.view<const CameraComponent>();
        if (camCheck.empty())
        {
            m_editorCtx->errorMessage = "シーンに Camera が配置されていません。\nHierarchy 右クリック → Camera で追加してください。";
            m_editorCtx->errorFlash = 1.0f;
            Logger::Warn("Play を中止しました: アクティブな CameraComponent がありません");
            return;
        }
        // isActive なカメラがなければ最初のカメラを自動で有効化
        bool hasActive = false;
        for (auto [e, cam] : camCheck.each())
            if (cam.isActive) { hasActive = true; break; }
        if (!hasActive)
        {
            auto first = *camCheck.begin();
            reg.get<CameraComponent>(first).isActive = true;
            Logger::Info("Auto-activated first CameraComponent for play mode");
        }
    }

    // GPU を待機してコマンドリスト状態を安全にする
    m_commandQueue->WaitIdle();

    // カメラ状態保存
    m_cameraSnapshot.position = m_camera->GetPosition();
    m_cameraSnapshot.yaw = m_camera->GetYaw();
    m_cameraSnapshot.pitch = m_camera->GetPitch();

    // Lua が触る前のエディタ状態を Stop 時の完全復元用に保存
    m_playSceneJson = SceneSerializer::SaveToString(*m_scene, PathResolver::AssetsDir());

    // ゲーム用カメラ: アクティブな CameraComponent をグローバル Camera に同期
    SyncActiveCameraToGlobal();

    // OnPlayStart 直後に Lua が変えた値を打ち消すため、
    // Transform / RigidBody / Light / Material PBR を覚えておく
    m_editorSnapshots.clear();
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<NameTag, Transform>();
        for (auto [entity, name, transform] : view.each())
        {
            EntitySnapshot snap;
            snap.position      = transform.position;
            snap.rotation      = transform.rotation;
            snap.scale         = transform.scale;
            snap.quaternion    = transform.quaternion;
            snap.useQuaternion = transform.useQuaternion;

            snap.hasRigidBody = reg.all_of<RigidBody>(entity);
            if (snap.hasRigidBody)
                snap.rigidBodyData = reg.get<RigidBody>(entity);

            snap.hasPointLight = reg.all_of<PointLight>(entity);
            if (snap.hasPointLight)
                snap.pointLightData = reg.get<PointLight>(entity);

            snap.hasDirectionalLight = reg.all_of<DirectionalLight>(entity);
            if (snap.hasDirectionalLight)
                snap.directionalLightData = reg.get<DirectionalLight>(entity);

            snap.hasSpotLight = reg.all_of<SpotLight>(entity);
            if (snap.hasSpotLight)
                snap.spotLightData = reg.get<SpotLight>(entity);

            snap.hasMeshRenderer = reg.all_of<MeshRenderer>(entity);
            if (snap.hasMeshRenderer)
            {
                const auto& mr = reg.get<MeshRenderer>(entity);
                snap.materialMetallicOverride  = mr.overrideMetallic;
                snap.materialRoughnessOverride = mr.overrideRoughness;
            }

            m_editorSnapshots[name.name] = snap;
        }
    }

    m_inputSystem->SetMouseCapture(false);

    // スクリプトエンジン初期化（エンティティにアタッチされた LuaScript 用）
    // ※ グローバルな game.lua の OnStart は呼ばない（エディタ配置のみで Play する）
    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();

    LoadGameScript();
    m_eventBus.Clear();   // Play 開始時に前 Play の購読を完全消去
    m_scriptEngine->OnPlayStart();
    if (m_particleSystem) m_particleSystem->Clear();  // Play 開始時に粒子をリセット
    if (m_gpuParticles) m_gpuParticles->Clear();

    // マルチプレイ ローカルテストループ(フェーズ⑨): ツールバーのPlayドロップダウンで
    // 選んだロールに従い、Luaを書かずに net:host()/net:join() 相当を自動実行する。
    // m_eventBus.Clear() より後なので、net.hostStarted/net.connected イベントは
    // OnStart() 内の events:on 登録に間に合う(Post→Flushはフレーム末)。
    if (m_networkSystem && m_editorCtx->netTestRole != NetTestRole::Offline)
    {
        std::string netErr;
        if (m_editorCtx->netTestRole == NetTestRole::Host)
        {
            if (!m_networkSystem->Host(m_networkSystem->Config().defaultPort,
                                       m_networkSystem->Config().maxPlayers, netErr))
                Logger::Warn("テストロール: ホスト開始に失敗しました: {}", netErr);
        }
        else if (m_editorCtx->netTestRole == NetTestRole::Client)
        {
            const u16 port = m_editorCtx->netTestJoinPort != 0
                           ? m_editorCtx->netTestJoinPort           // --net-client ip:port の明示指定
                           : m_networkSystem->Config().defaultPort;
            if (!m_networkSystem->Join(m_editorCtx->netTestJoinAddress, port, netErr))
                Logger::Warn("テストロール: 接続開始に失敗しました: {}", netErr);
        }
    }

    // エディタのスナップショットで上書き（Luaが勝手に変えた状態をエディタの状態に戻す）
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<NameTag, Transform>();
        for (auto [entity, name, transform] : view.each())
        {
            auto it = m_editorSnapshots.find(name.name);
            if (it == m_editorSnapshots.end()) continue;
            const auto& snap = it->second;

            // Transform 復元
            transform.position      = snap.position;
            transform.rotation      = snap.rotation;
            transform.scale         = snap.scale;
            transform.quaternion    = snap.quaternion;
            transform.useQuaternion = snap.useQuaternion;

            // Physics: エディタで外してたら Lua が付けたものを削除
            if (!snap.hasRigidBody)
            {
                reg.remove<RigidBody>(entity);
                reg.remove<ConvexHullCollider>(entity);
                reg.remove<BoxCollider>(entity);
                reg.remove<SphereCollider>(entity);
                reg.remove<CapsuleCollider>(entity);
            }
            else
            {
                // エディタのパラメータで上書き（Luaのデフォルト値ではなくエディタの設定値を使う）
                auto rb = snap.rigidBodyData;
                rb.bodyId = kInvalidBodyId;
                reg.emplace_or_replace<RigidBody>(entity, rb);
            }

            // Lighting: エディタで設定したライトを Play 初期化後も維持する。
            // LuaScript の OnStart が同じエンティティへ既定ライト値を入れても、配置値を優先する。
            if (snap.hasPointLight)
                reg.emplace_or_replace<PointLight>(entity, snap.pointLightData);
            else
                reg.remove<PointLight>(entity);

            if (snap.hasDirectionalLight)
            {
                auto dl = snap.directionalLightData;
                dl._prevRot = transform.rotation;
                dl._prevRotInit = true;
                reg.emplace_or_replace<DirectionalLight>(entity, dl);
            }
            else
            {
                reg.remove<DirectionalLight>(entity);
            }

            if (snap.hasSpotLight)
            {
                auto sl = snap.spotLightData;
                sl._prevRot = transform.rotation;
                sl._prevRotInit = true;
                reg.emplace_or_replace<SpotLight>(entity, sl);
            }
            else
            {
                reg.remove<SpotLight>(entity);
            }

            // Material PBR 復元（エディタで持っていた override 状態をそのまま戻す）。
            // Material を持たないプリミティブ床へ有効 override を捏造すると metallic=1 になり、
            // 床だけ拡散光が消えて暗くなるので、-1(=Material/既定値を使う) も保持する。
            if (snap.hasMeshRenderer && reg.all_of<MeshRenderer>(entity))
            {
                auto& mr = reg.get<MeshRenderer>(entity);
                mr.overrideMetallic  = snap.materialMetallicOverride;
                mr.overrideRoughness = snap.materialRoughnessOverride;
            }
        }
    }

    // 物理のタイムステップ蓄積をリセット
    m_physicsSystem->ResetAccumulator();

    // 全 RigidBody を物理エンジンに登録（エディタで復元した状態で）
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<RigidBody>();
        for (auto [entity, rb] : view.each())
        {
            if (rb.bodyId != kInvalidBodyId)
                m_physicsSystem->UnregisterBody(reg, entity);
            m_physicsSystem->RegisterBody(reg, entity);
        }
    }

    // 全 CharacterController を CharacterVirtual として生成
    {
        auto& reg = m_scene->GetRegistry();
        for (auto [entity, cc] : reg.view<CharacterController>().each())
        {
            if (cc._registered) m_physicsSystem->UnregisterCharacter(reg, entity);
            m_physicsSystem->RegisterCharacter(reg, entity);
        }
    }

    // ホットリロード用タイムスタンプ更新（エディタ）
    {
        std::string scriptPath = PathResolver::GameLuaPath();
        if (std::filesystem::exists(scriptPath))
            m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);
    }

    m_engineMode = EngineMode::Playing;
    Logger::Info("Entered PLAY mode");
}

void Application::EnterEditorMode()
{
    DX_ASSERT(!m_playSceneJson.empty(),
              "EnterEditorMode requires a prior EnterPlayMode snapshot");

    m_commandQueue->WaitIdle();

    // Play 中に鳴っていた SE（空間含む）と BGM を停止（Stop で鳴り続けるのを防ぐ）
    if (m_audioSystem) { m_audioSystem->StopAllSFX(); m_audioSystem->StopBGM(); }

    // Play 中に張ったネットワーク接続を Stop で確実に切る（次の Play に持ち越さない）。
    if (m_networkSystem) m_networkSystem->Disconnect();

    // OnPlayStop は ScriptEngine::Shutdown より前に呼ぶ（Shutdown で Lua state が消える）
    if (m_engineMode == EngineMode::Playing)
        m_scriptEngine->OnPlayStop();

    // EventBus には Play 中に登録された Lua ハンドラのラムダが残っている。
    // 物理を Shutdown→Initialize で作り直す前に購読を消し、物理側の EventBus 参照も外す。
    // こうしておくと Initialize と ScriptEngine::Shutdown の間に万一 Flush が走っても
    // 半壊状態の古いハンドラが発火しない（呼び順が将来変わっても安全）。
    m_eventBus.Clear();
    m_physicsSystem->SetEventBus(nullptr);

    m_physicsSystem->UnregisterAllBodies(m_scene->GetRegistry());
    m_physicsSystem->UnregisterAllCharacters(m_scene->GetRegistry());
    m_physicsSystem->Shutdown();
    m_physicsSystem->Initialize();
    // 新しい物理システムに同じ EventBus を再注入（接触イベントの配信先を復帰）。
    m_physicsSystem->SetEventBus(&m_eventBus);

    m_inputSystem->SetMouseCapture(false);

    m_camera->SetPosition(m_cameraSnapshot.position);
    m_camera->SetYaw(m_cameraSnapshot.yaw);
    m_camera->SetPitch(m_cameraSnapshot.pitch);

    m_editorCtx->ClearSelection();

    // JSON スナップショットからシーン全体を完全復元
    {
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        // Scene::Spawn は内部で m_cmdList を使うので最新の cmdList で更新する
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        // スナップショットからシーンを完全復元する。失敗(空/破損 JSON、復元中の例外)すると
        // ApplySceneJson が scene.Clear() 後に中断してシーンが空になる(= Stop 後に list_entities が
        // count:0 になる原因)。失敗を握りつぶさず、ディスク上の現在シーンから読み直してフォールバックする
        // (ユーザが手動で open_scene し直して復旧していた動作を自動化)。
        bool restored = false;
        try {
            restored = SceneSerializer::LoadFromString(*m_scene, m_playSceneJson, PathResolver::AssetsDir());
        } catch (const std::exception& ex) {
            Logger::Error("EnterEditorMode: スナップショット復元で例外が発生: {}", ex.what());
        }
        if (!restored && !m_editorCtx->currentScenePath.empty()) {
            Logger::Error("EnterEditorMode: スナップショット復元に失敗。ディスクから再読込します: {}",
                          ToAssetRel(m_editorCtx->currentScenePath));
            try {
                restored = SceneSerializer::Load(*m_scene, m_editorCtx->currentScenePath,
                                                 PathResolver::AssetsDir());
            } catch (const std::exception& ex) {
                Logger::Error("EnterEditorMode: ディスクからの再読込にも失敗: {}", ex.what());
            }
        }
        if (!restored)
            Logger::Error("EnterEditorMode: Stop 後のシーンが空です（有効なスナップショットもディスクのシーンもありません）");

        m_scriptEngine->Shutdown();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), PathResolver::AssetsDir());
        WireScriptCallbacks();

        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();
        m_resourceManager->FinishUploads();
        m_frameResources->EndFrame(*m_commandQueue);
    }

    // 古い JSON で誤復元しないよう、消費後はクリアする
    m_playSceneJson.clear();
    m_playSceneJson.shrink_to_fit();

    // シーン再構築でエンティティ ID が変わり Undo スタックの参照が無効になるためクリア
    m_editorCtx->undoSystem.Clear();
    m_editorCtx->ClearSelection();

    // Stop でもシーンを丸ごと作り直すため entity id が全部変わる。open_scene/new_scene と同様に
    // 世代を進めて古い id を無効化する(これが無いと Stop 後に古い id が別 entity に化けて
    // "invalid entity id" や誤動作になる)。dx12_stop の応答に新しい sceneGeneration が乗る。
    ++m_sceneGeneration;

    // Play 中に CameraComponent の FOV を採用してた可能性があるためエディタ用に戻す
    m_camera->SetPerspective(DirectX::XM_PIDIV4,
        static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight()),
        0.1f, 1000.0f);

    m_inputSystem->SetMouseCapture(false);
    m_engineMode = EngineMode::Editor;
    Logger::Info("Entered EDITOR mode");
}

void Application::LoadGameScript()
{
    // ゲームモードでは game.lua はディスクに無く game.pak 内に在る（exists() は false）。
    // LoadScript は VFS 経由で pak から読むので、InGameMode 時は存在チェックを迂回する。
    // ※ ScriptEngine を作り直すたび（EnterPlayMode / シーン切替 / プロジェクト読込）に
    //   グローバル OnUpdate も作り直されるので、その都度ここを呼ぶ必要がある。
    std::string scriptPath = PathResolver::GameLuaPath();
    if (dx12e::vfs::InGameMode() || std::filesystem::exists(scriptPath))
        m_scriptEngine->LoadScript(scriptPath);
    else
        Logger::Warn("ゲームスクリプトが見つかりません: {}", scriptPath);
}

bool Application::BuildGameStandalone()
{
    // 開始シーンを title.json に（あれば）。無ければ現在の currentScenePath を使う。
    std::string title = PathResolver::AssetsDir() + "scenes/title.json";
    if (std::filesystem::exists(title))
        m_editorCtx->currentScenePath = title;
    return BuildGame();
}

bool Application::BuildGame()
{
    namespace fs = std::filesystem;

    // --- 出力パスの非ASCII（日本語フォルダ名等）検出ガード（最優先）---
    // 出力先に非ASCII文字が含まれると、配布した Game.exe が起動時に std::filesystem の
    // UTF-8↔ANSI 誤変換で即クラッシュする（Windows error 1113 "No mapping for the Unicode
    // character..."）。原因不明の「ビルド成功 → 実行時クラッシュ」を防ぐため、ここで明示的に
    // 失敗させる。chosen は生の std::string（UTF-8でもACPでも日本語は >=0x80 を含む）なので
    // fs::path を経由せず（=ここで例外を出さず）バイト走査で判定する。
    {
        const std::string chosen =
            (m_editorCtx && !m_editorCtx->buildConfig.outputDir.empty())
                ? m_editorCtx->buildConfig.outputDir
                : PathResolver::BaseDir();
        bool nonAscii = false;
        for (unsigned char c : chosen) if (c >= 0x80) { nonAscii = true; break; }
        if (nonAscii)
        {
            Logger::Error("ビルドを中止しました: 出力先パスに非ASCII文字（日本語フォルダ名など）が"
                          "含まれています。このままビルドすると起動時にパスエラーで落ちるため、"
                          "半角英数のみのフォルダを指定してください。パス: {}", chosen);
            if (m_editorCtx)
                m_editorCtx->buildErrorMsg =
                    "出力フォルダのパスに日本語など非ASCII文字が含まれています。\n"
                    "このまま配布すると Game.exe が起動時にクラッシュします。\n"
                    "出力先を半角英数字のみのパスにしてください。\n\n" + chosen;
            return false;
        }
    }

    // ビルド出力先。ユーザーがビルド設定で選んだフォルダの中に「製品名_build」サブフォルダを作る。
    // 選んだフォルダ自体を出力先にして remove_all するとユーザーのデータを消す恐れがあるので必ずサブフォルダ化する。
    fs::path outputDir;
    if (m_editorCtx && !m_editorCtx->buildConfig.outputDir.empty())
    {
        // フォルダ名 = タイトルをサニタイズ（英数・空白・_- のみ残す）。空なら "Game"
        std::string sub;
        for (char c : std::string(m_editorCtx->buildConfig.title))
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ')
                sub += c;
        while (!sub.empty() && sub.back()  == ' ') sub.pop_back();
        while (!sub.empty() && sub.front() == ' ') sub.erase(sub.begin());
        if (sub.empty()) sub = "Game";
        sub += "_build";
        outputDir = fs::path(m_editorCtx->buildConfig.outputDir) / sub;
    }
    else
    {
        outputDir = fs::path(PathResolver::BaseDir()) / "build" / "game";
    }

    // クリーンアップ（安全策: 既存が「前回ビルド or 空」でなければ消さずに中止＝ユーザーデータ保護）
    if (fs::exists(outputDir))
    {
        std::error_code ec;
        bool looksLikeBuild = fs::exists(outputDir / "Game.exe")
                           || fs::exists(outputDir / "game.pak")
                           || fs::is_empty(outputDir, ec);
        if (!looksLikeBuild)
        {
            Logger::Error("ビルドを中止しました: 出力先に過去のビルド以外のデータが存在します（保護のため中断）: {}",
                          outputDir.string());
            return false;
        }
        fs::remove_all(outputDir, ec);
    }
    fs::create_directories(outputDir);

    // 完了後に Explorer で開くため、最終的な出力先を控える
    if (m_editorCtx)
        m_editorCtx->lastBuildDir = outputDir.string();

    // 1. GameRuntime.exe を Game.exe としてコピー（+ exe 隣の DLL も全部コピー）
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        fs::path exeDir = fs::path(exePath).parent_path();
        fs::path runtimeSrc = exeDir / "GameRuntime.exe";

        if (!fs::exists(runtimeSrc))
        {
            Logger::Error("GameRuntime.exe が見つかりません（{}）。先にエンジンをビルドしてください", runtimeSrc.string());
            return false;
        }

        fs::copy_file(runtimeSrc, outputDir / "Game.exe", fs::copy_options::overwrite_existing);
        Logger::Info("Copied GameRuntime.exe -> Game.exe");

        // 同じフォルダの .dll をすべて配布フォルダへ。
        // dxcompiler.dll(実行時シェーダーコンパイル専用、エディタのみ必要)だけ除外する。
        // ゲームは ShaderCompiler::LoadFromFile が game.pak から .cso を読むだけで実行時コンパイルは
        // 不要なため、同梱すると無駄に容量が増えるだけ(~25MB)。GameRuntime は dxcompiler.dll を
        // delay-load にしてある(ルート CMakeLists.txt)ので、同梱しなくても exe は正常起動する。
        // ※ dxil.dll は除外しない: これは D3D12 ランタイムが CreatePipelineState 時に
        // (Developer Mode OFF の環境で)DXIL署名検証のため内部で LoadLibrary するもので、
        // 我々のコードがリンクしているわけではない delay-load できない実行時依存。
        // 除外するとユーザー環境次第で PSO 生成が失敗するため、常に同梱する。
        static const std::unordered_set<std::string> kDllExcludeList = { "dxcompiler.dll" };
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(exeDir, ec))
        {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".dll" && ext != ".DLL") continue;
            std::string lowerName = entry.path().filename().string();
            for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (kDllExcludeList.count(lowerName)) continue;
            fs::copy_file(entry.path(), outputDir / entry.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
            Logger::Info("Copied dll -> {}", entry.path().filename().string());
        }
    }

    // 2+3. assets/ と scripts/ を game.pak にパック（コピーではなく暗号化アーカイブ化）
    {
        // 開始シーンの相対パスを計算。
        // ビルド設定で明示指定があればそれを最優先。無ければ現在開いているシーンから求める。
        std::string startSceneRel = "scenes/default.json";
        if (m_editorCtx && !m_editorCtx->buildConfig.startScene.empty())
        {
            startSceneRel = m_editorCtx->buildConfig.startScene;
        }
        else if (!m_editorCtx->currentScenePath.empty())
        {
            auto norm = [](std::string s) { for (auto& c : s) if (c == '\\') c = '/'; return s; };
            std::string full = norm(m_editorCtx->currentScenePath);
            std::string base = norm(PathResolver::AssetsDir());
            if (!base.empty() && full.rfind(base, 0) == 0)
                startSceneRel = full.substr(base.size());
            else
                startSceneRel = fs::path(full).lexically_relative(fs::path(base)).generic_string();

            if (startSceneRel.empty() || startSceneRel.rfind("..", 0) == 0)
            {
                Logger::Warn("現在のシーンが assets/ の外にあるため、既定の開始シーンを使用します: {}",
                             m_editorCtx->currentScenePath);
                startSceneRel = "scenes/default.json";
            }
        }

        vfs::PakWriter pak;
        if (!pak.Open((outputDir / "game.pak").string()))
        {
            Logger::Error("game.pak を書き込み用に開けません");
            return false;
        }

        // assets/ 配下を全パック（Normalize が "assets/" プレフィックスを剥がす）
        {
            fs::path assetsDir = fs::path(PathResolver::AssetsDir());
            std::error_code ec;
            for (auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
            {
                if (!entry.is_regular_file()) continue;
                std::string relPath = entry.path().lexically_relative(assetsDir).generic_string();
                pak.AddFile(entry.path().string(), relPath);
            }
        }

        // scripts/ 配下を全パック（"scripts/" プレフィックスを付けて格納）
        {
            fs::path scriptsDir = fs::path(PathResolver::ScriptsDir());
            if (fs::exists(scriptsDir))
            {
                std::error_code ec;
                for (auto& entry : fs::recursive_directory_iterator(scriptsDir, ec))
                {
                    if (!entry.is_regular_file()) continue;
                    std::string relPath = "scripts/" +
                        entry.path().lexically_relative(scriptsDir).generic_string();
                    pak.AddFile(entry.path().string(), relPath);
                }
            }
        }

        // shaders/ 配下の .cso を全パック（"shaders/" プレフィックス付き）。
        // → 出荷フォルダにプレーンな shaders/ を置かず、暗号化して pak に封入する。
        //   実行時は ShaderCompiler::LoadFromFile が VFS 経由で pak から復号する。
        // プロジェクト独自シェーダー(上書き/自作)がある場合は実行時再コンパイルして反映する。
        // コンパイル失敗があれば古い .cso を出荷せずビルド自体を中止する。
        {
            std::vector<std::string> shaderErrors;
            if (m_shaderManager && !m_shaderManager->RecompileAllForBuild(&shaderErrors))
            {
                std::string msg = "プロジェクトのシェーダーのコンパイルに失敗しました。ビルドを中止しました:\n";
                for (const auto& e : shaderErrors) msg += "  - " + e + "\n";
                Logger::Error("{}", msg);
                if (m_editorCtx) m_editorCtx->buildErrorMsg = msg;
                return false;
            }

            fs::path shadersDir = fs::path(PathResolver::ShaderDirW());
            if (fs::exists(shadersDir))
            {
                std::error_code ec;
                for (auto& entry : fs::recursive_directory_iterator(shadersDir, ec))
                {
                    if (!entry.is_regular_file()) continue;
                    std::string relPath = "shaders/" +
                        entry.path().lexically_relative(shadersDir).generic_string();
                    // プロジェクトオーバーライドで再コンパイル済みなら baked .cso より優先する。
                    const std::vector<u8>* overrideBytes = m_shaderManager
                        ? m_shaderManager->TryGetOverride(entry.path().filename().wstring())
                        : nullptr;
                    if (overrideBytes)
                        pak.AddBlob(relPath, overrideBytes->data(), overrideBytes->size());
                    else
                        pak.AddFile(entry.path().string(), relPath);
                }
            }

            // カスタムシェーダー(Registry外、MeshRenderer::shaderPath 割当用)。
            // キー規約は Application::EnsureCustomPso のゲームモード分岐と一致させること。
            if (m_shaderManager)
            {
                for (const std::string& relPath : m_shaderManager->AllValidCustomRelPaths())
                {
                    const std::vector<u8>* vs = m_shaderManager->GetCustomVsBytecode(relPath);
                    const std::vector<u8>* ps = m_shaderManager->GetCustomPsBytecode(relPath);
                    if (!vs || !ps) continue;
                    pak.AddBlob("shaders/custom/" + relPath + "_VS.cso", vs->data(), vs->size());
                    pak.AddBlob("shaders/custom/" + relPath + "_PS.cso", ps->data(), ps->size());
                }
            }
        }

        // ブートマニフェスト（game.json の代替。GameRuntime は pak からこれを読む）。
        // ビルド設定のタイトル/解像度を反映する。
        {
            std::string title = "Game";
            int winW = 1280, winH = 720;
            if (m_editorCtx)
            {
                if (m_editorCtx->buildConfig.title[0] != '\0')
                    title = m_editorCtx->buildConfig.title;
                winW = m_editorCtx->buildConfig.width;
                winH = m_editorCtx->buildConfig.height;
            }
            // JSON 文字列エスケープ（" と \ のみ。タイトルは UTF-8 のまま格納）
            std::string titleEsc;
            for (char c : title)
            {
                if (c == '\\' || c == '"') titleEsc += '\\';
                titleEsc += c;
            }

            std::string manifest =
                std::string("{\n") +
                "  \"title\": \"" + titleEsc + "\",\n" +
                "  \"startScene\": \"" + startSceneRel + "\",\n" +
                "  \"windowWidth\": " + std::to_string(winW) + ",\n" +
                "  \"windowHeight\": " + std::to_string(winH) + "\n" +
                "}\n";
            pak.AddBlob("__manifest__",
                reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
        }

        if (!pak.Finish(/*stripStrings=*/true))
        {
            Logger::Error("game.pak の書き出しに失敗しました");
            return false;
        }
        Logger::Info("Packed game.pak (startScene = {})", startSceneRel);
    }

    // 4. （shaders は手順 2+3 の game.pak に暗号化封入済み＝プレーンな shaders/ は出力しない）

    // 5. 起動用バッチ（GameRuntime は --game 不要: 常にゲームモード）
    {
        std::ofstream bat(outputDir / "Game.bat");
        bat << "@echo off\n";
        bat << "Game.exe\n";
        bat << "pause\n";
    }

    Logger::Info("Game build complete: {}", outputDir.string());
    return true;
}

// 「ビルド設定」ウィンドウ（Unity の Build Settings / Unreal の Packaging 相当）。
// 構成・開始シーン・出力先を決めてから「ビルド」で BuildGame を実行する。
void Application::RenderBuildSettingsWindow()
{
    if (!m_editorCtx || !m_editorCtx->showBuildSettings)
        return;

    namespace fs = std::filesystem;
    auto& cfg = m_editorCtx->buildConfig;

    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ビルド設定", &m_editorCtx->showBuildSettings))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("ゲームを単体 exe + 暗号化アセット(game.pak) に書き出す");
    ImGui::Spacing();

    // ===== シーン =====
    if (ImGui::CollapsingHeader("シーン", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::vector<std::string> scenes;
        std::string scenesDir = PathResolver::AssetsDir() + "scenes";
        if (fs::exists(scenesDir))
            for (auto& e : fs::directory_iterator(scenesDir))
                if (e.is_regular_file() && e.path().extension() == ".json")
                    scenes.push_back("scenes/" + e.path().filename().string());

        ImGui::TextUnformatted("開始シーン");
        const char* curLabel = cfg.startScene.empty()
            ? "(\xe7\x8f\xbe\xe5\x9c\xa8\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3)"  // (現在開いているシーン)
            : cfg.startScene.c_str();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##startScene", curLabel))
        {
            if (ImGui::Selectable("(\xe7\x8f\xbe\xe5\x9c\xa8\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3)",
                                  cfg.startScene.empty()))
                cfg.startScene.clear();
            for (auto& s : scenes)
                if (ImGui::Selectable(s.c_str(), s == cfg.startScene))
                    cfg.startScene = s;
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("\xe2\x80\xbb \xe5\x85\xa8\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x81\x8c game.pak \xe3\x81\xab\xe5\x90\xab\xe3\x81\xbe\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\xe8\xb5\xb7\xe5\x8b\x95\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x82\x92\xe9\x81\xb8\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82");  // ※全シーンがgame.pakに含まれます。起動シーンを選びます。
    }

    // ===== 製品 =====
    if (ImGui::CollapsingHeader("製品", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("タイトル（ウィンドウ名）");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##title", cfg.title, sizeof(cfg.title));

        ImGui::TextUnformatted("解像度");
        struct Res { const char* name; int w, h; };
        static const Res presets[] = {
            {"1280 x 720 (HD)",   1280, 720},
            {"1600 x 900",        1600, 900},
            {"1920 x 1080 (FHD)", 1920, 1080},
            {"2560 x 1440 (QHD)", 2560, 1440},
        };
        std::string cur = std::to_string(cfg.width) + " x " + std::to_string(cfg.height);
        ImGui::SetNextItemWidth(210.0f);
        if (ImGui::BeginCombo("##respreset", cur.c_str()))
        {
            for (auto& p : presets)
                if (ImGui::Selectable(p.name, p.w == cfg.width && p.h == cfg.height))
                {
                    cfg.width  = p.w;
                    cfg.height = p.h;
                }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("\xe5\xb9\x85##w", &cfg.width, 0);    // 幅
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("\xe9\xab\x98\xe3\x81\x95##h", &cfg.height, 0);  // 高さ
        cfg.width  = std::clamp(cfg.width,  320, 7680);
        cfg.height = std::clamp(cfg.height, 240, 4320);
    }

    // ===== 出力先 =====
    if (ImGui::CollapsingHeader("出力先", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("配置先フォルダ");
        char pathBuf[1024];
        strncpy_s(pathBuf,
                  cfg.outputDir.empty()
                    ? "(\xe6\x9c\xaa\xe9\x81\xb8\xe6\x8a\x9e \xe2\x80\x94 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe6\x99\x82\xe3\x81\xab\xe9\x81\xb8\xe6\x8a\x9e)"  // (未選択 — ビルド時に選択)
                    : cfg.outputDir.c_str(),
                  _TRUNCATE);
        ImGui::SetNextItemWidth(-92.0f);
        ImGui::InputText("##outdir", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("\xe5\x8f\x82\xe7\x85\xa7...", ImVec2(-1.0f, 0.0f)))  // 参照...
        {
            std::string dir;
            if (ProjectManager::PickFolder(m_window->GetHwnd(), dir, L"ビルドの配置先フォルダを選択"))
                cfg.outputDir = dir;
        }
        ImGui::Checkbox("ビルド後にフォルダを開く", &cfg.openFolderAfterBuild);
        ImGui::TextDisabled("\xe2\x80\xbb \xe9\x81\xb8\xe3\x82\x93\xe3\x81\xa0\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe7\x9b\xb4\xe4\xb8\x8b\xe3\x81\xab \"<\xe8\xa3\xbd\xe5\x93\x81\xe5\x90\x8d>_build\" \xe3\x82\x92\xe4\xbd\x9c\xe3\x81\xa3\xe3\x81\xa6\xe5\x87\xba\xe5\x8a\x9b\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82");  // ※選んだフォルダ直下に "<製品名>_build" を作って出力します。
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ===== ビルド実行 =====
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.42f, 0.68f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.52f, 0.82f, 1.0f));
    const bool doBuild = ImGui::Button("ビルド", ImVec2(-1.0f, 38.0f));
    ImGui::PopStyleColor(2);
    if (doBuild)
    {
        bool proceed = true;
        if (cfg.outputDir.empty())   // 未選択なら今すぐフォルダを選ばせる
        {
            std::string dir;
            if (ProjectManager::PickFolder(m_window->GetHwnd(), dir, L"ビルドの配置先フォルダを選択"))
                cfg.outputDir = dir;
            else
                proceed = false;
        }
        if (proceed)
            m_editorCtx->pendingBuildGame = true;   // フレーム境界で BuildGame 実行
    }

    if (m_editorCtx->buildCompleteFlash > 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
        ImGui::TextUnformatted("\xe2\x9c\x93 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86");  // ✓ ビルド完了
        ImGui::PopStyleColor();
        m_editorCtx->buildCompleteFlash -= m_gameClock.GetDeltaTime();
    }
    else if (m_editorCtx->buildErrorFlash > 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextUnformatted("\xe2\x9c\x97 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xa4\xb1\xe6\x95\x97 (dx12_engine.log)");  // ✗ ビルド失敗
        ImGui::PopStyleColor();
        m_editorCtx->buildErrorFlash -= m_gameClock.GetDeltaTime();
    }

    ImGui::End();
}

void Application::RenderSceneMeshes(ID3D12GraphicsCommandList* nativeCmdList, u32 frameIndex,
                                   DirectX::XMMATRIX viewProj, bool isGameView, u32 aoSrvIndex,
                                   bool depthPrepassActive)
{
    using namespace DirectX;
    auto& reg = m_scene->GetRegistry();
    auto renderView = reg.view<const Transform, const MeshRenderer>();

    // 視錐台カリング（ゲームビューのみ）。画面外エンティティの forward ドローを省く＝
    // 敵がアリーナ全域に散るゲームで、画面外の敵を毎フレーム描かずに済む。
    // 編集シーンビュー(isGameView=false)では従来通り全描画＝編集時の見え方は不変。
    const bool cullEnabled = isGameView;
    Frustum camFrustum;
    if (cullEnabled) camFrustum = Frustum::FromViewProj(viewProj);

    // SSAO AO テーブル(t8)を1回バインド（無効/編集ビューは白=1.0 ダミー）。
    // 全 forward 系 PSO が同一 RootSig を共有するため、ここで一括バインドして hazard を防ぐ。
    if (aoSrvIndex != DescriptorHeap::kInvalidIndex)
        m_commandList->SetSRVTable(RootSignature::kSlotAOSRV,
            m_srvHeap->GetGpuHandle(aoSrvIndex));

    // パーティクル判定（名前が "Pfx" で始まる＝加算発光で描く）
    auto isPfx = [&](entt::entity e) -> bool {
        const auto* nt = reg.try_get<NameTag>(e);
        return nt && nt->name.rfind("Pfx", 0) == 0;
    };

    // 1エンティティ分の描画（パイプライン選択 + メッシュ描画）
    auto drawEntity = [&](entt::entity e, const Transform& transform, const MeshRenderer& renderer)
    {
        // park 済み（scale≈0 で画面外へ退避したプール要素）は不可視なので描画スキップ。
        // エンジンはフラスタムカリングしないため、これが無いと game1 の未使用プール(~700体)を
        // 毎フレーム全部描いてしまい、空に見える画面でもGPUが張り付く。
        const auto& sc = transform.scale;
        if (sc.x * sc.x + sc.y * sc.y + sc.z * sc.z < 1e-8f) return;

        XMMATRIX world = (transform.parent != entt::null)
            ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

        bool isGrid = reg.all_of<GridPlane>(e);
        bool isSkinned = reg.all_of<SkeletalAnimation>(e);

        // フラスタムカリング: グリッド以外で、ワールド球が視錐台の完全に外なら描画スキップ。
        // 床/壁など大きい構造物は球半径も大きく必ず交差＝カリングされない（自然に残る）。
        if (cullEnabled && !isGrid)
        {
            const float ms = (std::max)((std::max)(std::abs(sc.x), std::abs(sc.y)), std::abs(sc.z));
            // ponytail: 球は meshes[0] 基準＋1.25x バイアスで保守的（game1 の敵は単一メッシュ）。
            const float radius = (!renderer.meshes.empty() && renderer.meshes[0])
                               ? renderer.meshes[0]->GetBoundingRadius() : 1.0f;
            if (!camFrustum.SphereVisible(world.r[3], radius * ms * 1.25f)) return;
        }

        if (isPfx(e))
        {
            m_commandList->SetPipelineState(*m_emissivePipelineState);
        }
        else if (isGrid)
        {
            m_commandList->SetPipelineState(*m_gridPipelineState);
        }
        else if (isSkinned)
        {
            auto& skelAnim = reg.get<SkeletalAnimation>(e);
            // 深度プリパス併用時は LESS_EQUAL バリアントで同一深度を通す。
            m_commandList->SetPipelineState(depthPrepassActive
                ? *m_skinnedPipelineStateLEqual : *m_skinnedPipelineState);
            m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                m_srvHeap->GetGpuHandle(skelAnim.skinningBuffer->GetSrvIndex(frameIndex)));
        }
        else
        {
            // カスタムシェーダー割当(静的メッシュのみ)。未コンパイル/生成失敗時は既定 Forward へフォールバック。
            CustomForwardPsos* custom = renderer.shaderPath.empty() ? nullptr : EnsureCustomPso(renderer.shaderPath);
            if (custom)
            {
                PipelineState* pso = renderer.shaderAlphaBlend
                    ? (depthPrepassActive ? custom->lequalBlend.get() : custom->lessBlend.get())
                    : (depthPrepassActive ? custom->lequal.get() : custom->less.get());
                m_commandList->SetPipelineState(*pso);
            }
            else
                m_commandList->SetPipelineState(depthPrepassActive
                    ? *m_pipelineStateLEqual : *m_pipelineState);
        }

        bool hasNodeAnim = reg.all_of<NodeAnimationComp>(e);
        for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
        {
            const auto* mesh = renderer.meshes[mi];

            XMMATRIX meshWorld = world;
            if (hasNodeAnim && mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
            {
                XMMATRIX nodeMat = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]);
                meshWorld = nodeMat * world;
            }

            struct PerObjectData { XMMATRIX mvp; XMMATRIX mdl; } objData;
            objData.mvp = XMMatrixTranspose(meshWorld * viewProj);
            objData.mdl = XMMatrixTranspose(meshWorld);
            m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);

            const Material* mat = mesh->GetMaterial();

            // PBR テクスチャ SRV ブロックをバインド。インスタンス単位のテクスチャ上書き
            // (D&Dでのマテリアル割当、MeshRenderer::overrideAlbedoTexture 等)があれば
            // 専用ブロックを優先する(mat は同一モデルの全インスタンスで共有されるため直接は触らない)。
            u32 overrideBlock = EnsureMaterialOverrideSrv(e, mi, renderer, mat, nativeCmdList);
            if (overrideBlock != 0xFFFFFFFF)
            {
                m_commandList->SetSRVTable(RootSignature::kSlotSRVTable,
                    m_srvHeap->GetGpuHandle(overrideBlock));
            }
            else if (mat && mat->srvBlockIndex != 0xFFFFFFFF)
            {
                m_commandList->SetSRVTable(RootSignature::kSlotSRVTable,
                    m_srvHeap->GetGpuHandle(mat->srvBlockIndex));
            }
            else
            {
                Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
                m_commandList->SetSRVTable(RootSignature::kSlotSRVTable,
                    m_srvHeap->GetGpuHandle(tex->GetSrvIndex()));
            }

            // PBR Material Constants (Slot 5)
            struct { float metallic; float roughness; u32 flags; float pad; } pbrParams;
            // MeshRenderer のオーバーライド値を優先、なければ Material の値
            pbrParams.metallic  = (renderer.overrideMetallic  >= 0.0f) ? renderer.overrideMetallic
                                : (mat ? mat->defaultMetallic : 0.0f);
            pbrParams.roughness = (renderer.overrideRoughness >= 0.0f) ? renderer.overrideRoughness
                                : (mat ? mat->defaultRoughness : 0.5f);
            pbrParams.flags     = 0;
            if (mat && mat->normalMapTexture) pbrParams.flags |= 1u;
            // overrideが有効な場合、metalRoughnessテクスチャのスケーリングを無効化
            bool hasOverride = (renderer.overrideMetallic >= 0.0f || renderer.overrideRoughness >= 0.0f);
            if (!hasOverride && mat && mat->metalRoughnessTexture) pbrParams.flags |= 2u;
            pbrParams.pad = 0;
            nativeCmdList->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 4, &pbrParams, 0);

            m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
            m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
            m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
        }
    };

    // パス1: 不透明（グリッド・パーティクル以外）を描く＝深度を確定
    for (auto [e, transform, renderer] : renderView.each())
    {
        if (reg.all_of<GridPlane>(e)) continue;
        if (isPfx(e)) continue;
        drawEntity(e, transform, renderer);
    }

    // パス2: エディタ用グリッド。線だけを後描きする（ForwardGrid 側で線以外 alpha=0）。
    // 床全体へ半透明の膜を被せず、グリッド表示だけ維持する。
    if (!isGameView)
    {
        for (auto [e, transform, renderer] : renderView.each())
        {
            const auto* gp = reg.try_get<GridPlane>(e);
            if (!gp || !gp->enabled) continue;
            drawEntity(e, transform, renderer);
        }
    }

    // パス3: 発光弾(Pfx) を GPU instancing で加算合成。同一メッシュ(共有)を1ドローに集約。
    // 弾が数百発でも「メッシュ種類ぶんのドロー」だけで済む（boss3 弾幕の draw 数を一定化）。
    {
        std::unordered_map<const Mesh*, std::vector<MeshInstanceData>> byMesh;
        for (auto [e, transform, renderer] : renderView.each())
        {
            if (!isPfx(e)) continue;
            const auto& sc = transform.scale;
            if (sc.x * sc.x + sc.y * sc.y + sc.z * sc.z < 1e-8f) continue;   // park スキップ
            if (renderer.meshes.empty() || !renderer.meshes[0]) continue;

            XMMATRIX world = (transform.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();
            XMMATRIX t = XMMatrixTranspose(world);
            MeshInstanceData inst;
            XMStoreFloat4(&inst.r0, t.r[0]);
            XMStoreFloat4(&inst.r1, t.r[1]);
            XMStoreFloat4(&inst.r2, t.r[2]);
            inst.color = renderer.instanceColor;
            byMesh[renderer.meshes[0]].push_back(inst);
        }

        if (!byMesh.empty())
        {
            struct InstBucket { const Mesh* mesh; u32 base; u32 count; };
            std::vector<InstBucket> buckets;
            u32 cursor = m_instanceCursor;   // メイン/プレビューで同フレームバッファを連番共有
            uint8_t* dst = m_instanceMapped[frameIndex];
            for (auto& [mesh, vec] : byMesh)
            {
                if (cursor >= kMaxInstances) break;
                u32 n = (std::min)(static_cast<u32>(vec.size()), kMaxInstances - cursor);
                if (n == 0) continue;
                memcpy(dst + static_cast<size_t>(cursor) * sizeof(MeshInstanceData),
                       vec.data(), static_cast<size_t>(n) * sizeof(MeshInstanceData));
                buckets.push_back({mesh, cursor, n});
                cursor += n;
            }
            m_instanceCursor = cursor;   // 次の RenderSceneMeshes 呼び出し（プレビュー）へ連番を引き継ぐ

            m_commandList->SetPipelineState(*m_emissivePipelineState);
            XMMATRIX vpT = XMMatrixTranspose(viewProj);   // cbuffer 列優先再解釈で hlsl 上は VP
            m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 16, &vpT);
            m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (auto& b : buckets)
            {
                m_commandList->SetVertexBuffer(b.mesh->GetVertexBuffer().GetView());   // slot0
                m_commandList->SetIndexBuffer(b.mesh->GetIndexBuffer().GetView());
                D3D12_VERTEX_BUFFER_VIEW iv = m_instanceVbView[frameIndex];
                iv.BufferLocation += static_cast<u64>(b.base) * sizeof(MeshInstanceData);
                iv.SizeInBytes     = b.count * sizeof(MeshInstanceData);
                nativeCmdList->IASetVertexBuffers(1, 1, &iv);                          // slot1
                m_commandList->DrawIndexedInstanced(b.mesh->GetIndexCount(), b.count);
            }
        }
    }
}

void Application::DrawWorldSprites(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                                  DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp,
                                  D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                  u32 vpX, u32 vpY, u32 vpW, u32 vpH, float time)
{
    using namespace DirectX;
    if (!m_spriteRenderer || !m_scene) return;

    auto& sreg = m_scene->GetRegistry();
    m_spriteRenderer->BeginWorldFrame();
    for (auto [e, sp] : sreg.view<const Sprite2D>().each())
    {
        if (!sp.worldSpace || sp.texturePath.empty()) continue;
        if (!sreg.all_of<Transform>(e)) continue;
        const std::string absPath = PathResolver::AssetsDir() + sp.texturePath;
        std::wstring wpath = PathResolver::Utf8ToWide(absPath);
        Texture* tex = m_resourceManager->GetOrLoadTexture(wpath, cmd);
        if (!tex) continue;

        WorldSpriteDesc d;
        XMStoreFloat4x4(&d.world, ComputeWorldMatrix(sreg, e));
        d.size      = sp.size;
        d.uvMin     = sp.uvMin;
        d.uvMax     = sp.uvMax;
        d.color     = sp.color;
        d.srvIndex  = tex->GetSrvIndex();
        d.layer     = static_cast<float>(sp.layer);
        d.billboard = sp.billboard;
        d.effect    = sp.effectValue;
        if (!sp.shaderPath.empty())
        {
            if (CustomSpritePsos* custom = EnsureCustomSpritePso(sp.shaderPath))
                d.customPso = sp.shaderAlphaBlend ? custom->blend->Get() : custom->opaque->Get();
        }
        m_spriteRenderer->SubmitWorld(d);
    }

    if (m_spriteRenderer->HasAnyWorld())
    {
        cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);   // 深度テストのため DSV もバインド
        m_commandList->SetViewportAndScissor(vpX, vpY, vpW, vpH);
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        m_spriteRenderer->RenderWorld(cmd, viewProj, camRight, camUp, time);
    }
}

// CSM: カメラ視錐台を near→far で kNumCascades 分割し、各カスケードをライト視点へタイトフィット。
// 結果は m_cascadeViewProj[]（行優先 world*VP 用、非転置）と m_cascadeSplitsView[]（各遠端 view 深度）に格納。
// 深度専用シーン描画（CSM各カスケード/スポット影/ポイント影の各面/SSAOプリパスで共用）。
// RTVなし/DSVのみを前提に、Transform+MeshRenderer を全走査して viewProj で変換し描画する。
// GridPlane・park済み(scale≈0)・発光弾(Pfx*)は除外（元のシャドウパスのフィルタをそのまま踏襲）。
void Application::RenderDepthOnlyScene(DirectX::XMMATRIX viewProj, PipelineState& staticPSO,
                                       PipelineState& skinnedPSO, bool updateSkinning, u32 frameIndex)
{
    using namespace DirectX;

    auto& reg = m_scene->GetRegistry();
    auto renderView = reg.view<const Transform, const MeshRenderer>();
    for (auto [e, transform, renderer] : renderView.each())
    {
        if (reg.all_of<GridPlane>(e)) continue;
        // park 済み(scale≈0)は影/深度も不要＝該当ドローをまるごと削減。
        const auto& sc = transform.scale;
        if (sc.x * sc.x + sc.y * sc.y + sc.z * sc.z < 1e-8f) continue;
        // 発光弾(Pfx*)は光源扱い＝影/深度を落とさない（加算発光なので影が無い方が自然）。
        if (auto* nt = reg.try_get<NameTag>(e); nt && nt->name.rfind("Pfx", 0) == 0) continue;

        XMMATRIX world = (transform.parent != entt::null)
            ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

        bool isSkinned = reg.all_of<SkeletalAnimation>(e);
        if (isSkinned)
        {
            auto& skelAnim = reg.get<SkeletalAnimation>(e);
            if (updateSkinning)
                skelAnim.skinningBuffer->Update(skelAnim.animator->GetSkinningMatrices(), frameIndex);
            m_commandList->SetPipelineState(skinnedPSO);
            m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                m_srvHeap->GetGpuHandle(skelAnim.skinningBuffer->GetSrvIndex(frameIndex)));
        }
        else
        {
            m_commandList->SetPipelineState(staticPSO);
        }

        bool hasNodeAnim = reg.all_of<NodeAnimationComp>(e);
        for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
        {
            const auto* mesh = renderer.meshes[mi];

            XMMATRIX meshWorld = world;
            if (hasNodeAnim && mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
            {
                XMMATRIX nodeMat = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]);
                meshWorld = nodeMat * world;
            }

            struct PerObjectData { XMMATRIX mvp; XMMATRIX mdl; } objData;
            objData.mvp = XMMatrixTranspose(meshWorld * viewProj);
            objData.mdl = XMMatrixTranspose(meshWorld);
            m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);

            m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
            m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
            m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
        }
    }
}

void Application::ComputeCascades(const DirectX::XMVECTOR& lightDir, f32 camNear, f32 camFar)
{
    using namespace DirectX;
    const u32 N = kNumCascades;

    // CSM は透視前提（スライス錐台を XMMatrixPerspectiveFovLH で復元する）。
    // 編集2Dビュー等でカメラが正射になると錐台復元が破綻し影が崩れるため、
    // 正射時は CSM を無効化（無影フォールバック）する。
    // cascadeViewProj=identity + cascadeSplitsView=巨大正値 で、PS の SelectCascade は
    // 必ず cascade0 を返し、identity 変換で UV が [0,1] 外へ出て SampleCascade が 1.0(無影)。
    // シーンで影を無効化した場合も同じ無影センチネルを書く＝シェーダは全面ライト(黒画面にならない)。
    const bool shadowsOff = !(m_scene && m_scene->GetShadowsEnabled());
    if (m_camera->IsOrthographic() || shadowsOff)
    {
        XMMATRIX id = XMMatrixIdentity();
        for (u32 i = 0; i < N; ++i)
        {
            XMStoreFloat4x4(&m_cascadeViewProj[i], id);
            m_cascadeSplitsView[i] = 1e9f;  // 全成分を遠端 → cascade0 固定
        }
        return;
    }

    const f32 camFovY   = m_camera->GetFovY();
    const f32 camAspect = m_camera->GetAspect();

    // 1) 分割距離（対数 × 一様の混合, lambda）
    f32 splits[kNumCascades];
    f32 range = camFar - camNear;
    f32 ratio = camFar / (std::max)(camNear, 1e-4f);
    for (u32 i = 0; i < N; ++i)
    {
        f32 p    = (i + 1) / static_cast<f32>(N);
        f32 logS = camNear * std::pow(ratio, p);
        f32 uniS = camNear + range * p;
        splits[i] = m_cascadeSplitLambda * logS + (1.0f - m_cascadeSplitLambda) * uniS;
        m_cascadeSplitsView[i] = splits[i];  // PS のカスケード選択に渡す view 深度（正値）
    }

    // 2) カメラビュー（スライス錐台の隅を world へ戻すのに使用）
    XMMATRIX camView = m_camera->GetViewMatrix();

    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    // 退化回避: lightDir が up とほぼ平行なら up を Z 軸へ
    if (fabsf(XMVectorGetX(XMVector3Dot(lightDir, up))) > 0.99f)
        up = XMVectorSet(0, 0, 1, 0);

    f32 prevSplit = camNear;
    for (u32 ci = 0; ci < N; ++ci)
    {
        f32 n = prevSplit, f = splits[ci];

        // スライス専用の透視投影で NDC 隅 → world
        XMMATRIX sliceProj = XMMatrixPerspectiveFovLH(camFovY, camAspect, n, f);
        XMMATRIX invVP     = XMMatrixInverse(nullptr, camView * sliceProj);

        const XMVECTOR ndc[8] = {
            {-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, 1, 0, 1},
            {-1, -1, 1, 1}, {1, -1, 1, 1}, {-1, 1, 1, 1}, {1, 1, 1, 1}};
        XMVECTOR corners[8];
        XMVECTOR center = XMVectorZero();
        for (int k = 0; k < 8; ++k)
        {
            XMVECTOR w = XMVector4Transform(ndc[k], invVP);
            w = XMVectorScale(w, 1.0f / XMVectorGetW(w));
            corners[k] = w;
            center = XMVectorAdd(center, w);
        }
        center = XMVectorScale(center, 1.0f / 8.0f);

        // 包む球半径（回転不変＝テクセルスイム抑制）
        f32 radius = 0.0f;
        for (int k = 0; k < 8; ++k)
            radius = (std::max)(radius,
                XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[k], center))));
        radius = std::ceil(radius * 16.0f) / 16.0f;

        // ライトビュー: 球中心から -lightDir 方向へ後退
        XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(lightDir, radius));
        XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);

        // タイトフィット正射（球で対称）
        XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            -radius, radius, -radius, radius, 0.0f, radius * 2.0f);

        // テクセルスナップ（シャドウのちらつき防止）
        XMMATRIX shadowVP = lightView * lightProj;
        XMVECTOR origin   = XMVector4Transform(XMVectorSet(0, 0, 0, 1), shadowVP);
        origin = XMVectorScale(origin, static_cast<f32>(m_shadowMapSize) / 2.0f);
        XMVECTOR rounded = XMVectorRound(origin);
        XMVECTOR offset  = XMVectorScale(XMVectorSubtract(rounded, origin),
                                         2.0f / static_cast<f32>(m_shadowMapSize));
        offset = XMVectorSetZ(XMVectorSetW(offset, 0.0f), 0.0f);
        XMMATRIX snap = XMMatrixTranslationFromVector(offset);
        shadowVP = shadowVP * snap;

        XMStoreFloat4x4(&m_cascadeViewProj[ci], shadowVP);  // 行優先（world*VP 用、非転置）
        prevSplit = f;
    }
}

void Application::Render()
{
    using namespace DirectX;

    // Skybox 再ベイク要求（エディタでパス変更時）。専用 cmdList + WaitIdle で安全に処理。
    if (m_skyboxDirty && m_iblBaker)
    {
        m_skyboxDirty = false;
        m_commandQueue->WaitIdle();   // 前フレームの GPU 完了を待ってから SRV を入れ替える
        auto* bakeCmd = m_frameResources->BeginFrame(*m_commandQueue);
        LoadSkyboxIfNeeded(bakeCmd);
        ThrowIfFailed(bakeCmd->Close());
        m_commandQueue->ExecuteCommandList(bakeCmd);
        m_commandQueue->WaitIdle();
        m_frameResources->EndFrame(*m_commandQueue);
        if (m_envCubeTex) m_envCubeTex->FinishUpload();
        m_resourceManager->FinishUploads();
    }

    auto* nativeCmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_commandList->Wrap(nativeCmdList);

    // Deferred: new scene（描画前に処理しないと GPU リソース解放でクラッシュする）
    if (m_editorCtx->pendingNewScene && m_engineMode == EngineMode::Editor)
    {
        m_editorCtx->pendingNewScene = false;
        // プロジェクト新規作成の初期シーンなら保存先が指定されている
        std::string starterPath = std::move(m_editorCtx->pendingNewScenePath);
        m_editorCtx->pendingNewScenePath.clear();
        m_editorCtx->ClearSelection();
        m_editorCtx->undoSystem.Clear();
        m_editorCtx->currentScenePath = starterPath;  // 空なら未保存の新規シーン
        m_scene->Clear();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        // ---- 再生に必要な最低限のデフォルト配置 ----
        m_scene->SpawnPlane("Grid", {0, 0, 0}, kEditorGridSize, true);   // エディタ用グリッド
        m_scene->SpawnPlane("Ground", {0, 0, 0}, 20.0f, false); // 実体のある床
        m_scene->SpawnBox("Cube", {0, 0.5f, 0}, {0, 0, 0}, {1, 1, 1}); // サンプルオブジェクト
        {
            auto& reg = m_scene->GetRegistry();
            // 平行光源
            auto lightE = reg.create();
            reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
            reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
            reg.emplace<DirectionalLight>(lightE);

            // メインカメラ（CameraComponent が無いと Play で映らないため必須）
            auto camE = reg.create();
            reg.emplace<NameTag>(camE, NameTag{"MainCamera"});
            reg.emplace<Transform>(camE, Transform{{0.0f, 6.0f, -12.0f}, {22.0f, 0.0f, 0.0f}, {1,1,1}});
            CameraComponent cam;
            cam.isActive = true;
            reg.emplace<CameraComponent>(camE, cam);
        }
        if (!starterPath.empty())
            m_currentSceneRel = ToAssetRel(starterPath);
        // 作成と同時にシーンファイルを保存
        if (!m_editorCtx->currentScenePath.empty())
        {
            SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir());
            ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
            m_editorCtx->hotReloadFlash = 1.5f;
        }
        m_editorLayer->RefreshAssetBrowser();
        // 新シーンの SkyboxSettings で IBL/skybox を次フレーム冒頭に再ベイク。
        // m_loadedSkyboxPath をクリアして「偶然旧パス一致でスキップ」の取りこぼしを防ぐ。
        m_loadedSkyboxPath.clear();
        m_skyboxDirty = true;
        ++m_sceneGeneration;   // 古い entity id を無効化(MCP の STALE_SCENE 検出用)
        m_mcpIdempotency.clear();   // 別シーンの entity を idempotentReplay で誤返却しないようクリア
        Logger::Info("New scene created");
    }

    // Deferred: scene load（描画前に処理）
    if (!m_editorCtx->pendingLoadPath.empty() && m_engineMode == EngineMode::Editor)
    {
        std::string loadPath = std::move(m_editorCtx->pendingLoadPath);
        m_editorCtx->pendingLoadPath.clear();
        m_editorCtx->ClearSelection();
        m_editorCtx->undoSystem.Clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        const bool loaded = SceneSerializer::Load(*m_scene, loadPath, PathResolver::AssetsDir());
        if (loaded)
        {
            m_editorCtx->currentScenePath = loadPath;
            m_currentSceneRel = ToAssetRel(loadPath);
            ProjectManager::SaveLastOpenedScene(loadPath);
            m_editorCtx->hotReloadFlash = 1.5f;
            m_editorLayer->RefreshAssetBrowser();
            // 開いたシーン(既存ゲーム / プロジェクト / Grid 無しテンプレ)に Grid が無ければ補う。
            // Scene::Initialize 済み(有効 cmdList)なのでここでメッシュ生成して安全。
            EnsureEditorGrid();
            // ロードしたシーンの SkyboxSettings(envMapPath/iblIntensity 等) を反映するため
            // 次フレーム冒頭で再ベイクを要求。別 envMapPath のシーンを開いても自動追従する。
            // 差分判定の取りこぼし防止に m_loadedSkyboxPath をクリア。
            m_loadedSkyboxPath.clear();
            m_skyboxDirty = true;
            ++m_sceneGeneration;   // 古い entity id を無効化(MCP の STALE_SCENE 検出用)
            m_mcpIdempotency.clear();   // 別シーンの entity を idempotentReplay で誤返却しないようクリア
            Logger::Info("Scene loaded: {}", loadPath);
        }
        // MCP open_scene の遅延応答。
        if (m_mcpLoadReply.client != 0)
        {
            if (loaded)
            {
                auto& reg = m_scene->GetRegistry();
                int entityCount = 0;
                for (auto e : reg.view<NameTag>()) { (void)e; ++entityCount; }
                CompleteMcp(m_mcpBridge.get(), m_mcpLoadReply,
                    nlohmann::json{{"sceneName", std::filesystem::path(loadPath).stem().string()},
                                   {"path", ToAssetRel(loadPath)},
                                   {"entityCount", entityCount},
                                   {"sceneGeneration", m_sceneGeneration}});
            }
            else
            {
                FailMcp(m_mcpBridge.get(), m_mcpLoadReply, McpErr::Internal,
                        "scene load failed: " + loadPath);
            }
            m_mcpLoadReply = {};
        }
    }

    // Play 中のシーン切替（Lua loadScene/nextScene、またはトランジション中間点）
    {
        // トランジションが中間点に達したら、保留中のターゲットをロード対象にする
        if (m_sceneTransition && m_sceneTransition->ConsumeHalfway() && !m_transitionTargetScene.empty())
        {
            m_editorCtx->pendingGameLoadPath = m_transitionTargetScene;
            m_transitionTargetScene.clear();
        }

        if (!m_editorCtx->pendingGameLoadPath.empty() && m_engineMode == EngineMode::Playing)
        {
            std::string rel = std::move(m_editorCtx->pendingGameLoadPath);
            m_editorCtx->pendingGameLoadPath.clear();
            DoRuntimeSceneLoad(rel, nativeCmdList);
        }
        else if (!m_editorCtx->pendingGameLoadPath.empty())
        {
            // Play 中でなければ無視（誤発火防止）
            m_editorCtx->pendingGameLoadPath.clear();
        }
    }

    // ネットワーク複製: サーバーが RequestSpawn した(またはクライアントが Spawn パケットを
    // 受信した)エンティティをフレーム境界で実体化する。InstantiatePrefab はモデルロードに
    // 現在フレームの cmdList が要るため net:spawn 呼び出しの場では即時実行できない。
    if (m_networkSystem && m_engineMode == EngineMode::Playing)
    {
        auto netSpawns = m_networkSystem->ConsumePendingSpawns();
        if (!netSpawns.empty())
        {
            m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                                m_srvHeap.get(), nativeCmdList);
            auto& netReg = m_scene->GetRegistry();
            for (auto& sp : netSpawns)
            {
                entt::entity root = SceneSerializer::InstantiatePrefab(
                    *m_scene, PathResolver::AssetsDir() + sp.prefabPath, PathResolver::AssetsDir());
                if (root == entt::null)
                {
                    Logger::Warn("ネットワークスポーン失敗(プレハブ読込エラー): {}", sp.prefabPath);
                    continue;
                }
                if (netReg.all_of<Transform>(root))
                    netReg.get<Transform>(root).position = { sp.x, sp.y, sp.z };
                m_networkSystem->OnEntityInstantiated(sp.netId, sp.owner, root, netReg);
            }
        }
    }

    // エンティティ生成（前フレームのドラッグ&ドロップ等から遅延実行）
    if (!m_editorCtx->pendingSpawns.empty())
    {
        char dbgBuf[128];
        snprintf(dbgBuf, sizeof(dbgBuf), "[PendingSpawns] count=%zu mode=%s\n",
            m_editorCtx->pendingSpawns.size(),
            m_engineMode == EngineMode::Editor ? "Editor" : "Playing");
        OutputDebugStringA(dbgBuf);
    }
    if (!m_editorCtx->pendingSpawns.empty() && m_engineMode == EngineMode::Editor)
    {
        auto spawns = std::move(m_editorCtx->pendingSpawns);
        m_editorCtx->pendingSpawns.clear();

        // Scene の内部 cmdList を今フレームのものに更新
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        for (auto& req : spawns)
        {
            std::string name = std::filesystem::path(req.modelPath).stem().string();
            if (!req.name.empty()) name = req.name;   // MCP 等からの任意名で上書き
            entt::entity spawnedEntity = entt::null;
            entt::entity mcpPrefabRoot = entt::null;          // prefab 経路の MCP 応答用ルート
            std::vector<entt::entity> mcpPrefabAll;           // prefab 経路の全 entity

            if (req.modelPath == "__primitive_box__")
            {
                auto e = m_scene->SpawnBox(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__primitive_sphere__")
            {
                auto e = m_scene->SpawnSphere(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__primitive_plane__")
            {
                auto e = m_scene->SpawnPlane(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__empty__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e);
                spawnedEntity = e;
            }
            else if (req.modelPath == "__camera__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{req.name.empty() ? std::string("Camera") : req.name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                // 他にアクティブカメラがなければ自動で isActive=true
                bool hasActive = false;
                for (auto [oe, oc] : reg.view<const CameraComponent>().each())
                    if (oc.isActive) { hasActive = true; break; }
                reg.emplace<CameraComponent>(e, CameraComponent{60.0f, 0.1f, 1000.0f, !hasActive});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__directional_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{req.name.empty() ? std::string("DirectionalLight") : req.name});
                reg.emplace<Transform>(e, Transform{req.position, {-30.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<DirectionalLight>(e, DirectionalLight{{0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__point_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{req.name.empty() ? std::string("PointLight") : req.name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<PointLight>(e, PointLight{{1.0f, 1.0f, 1.0f}, 1.0f, 10.0f});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__spot_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{req.name.empty() ? std::string("SpotLight") : req.name});
                reg.emplace<Transform>(e, Transform{req.position, {-60.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<SpotLight>(e, SpotLight{});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__gimmick_spike__" || req.modelPath == "__gimmick_slide__" ||
                     req.modelPath == "__gimmick_wall__")
            {
                // ステージギミックのプリセット: 着色ボックス + Gimmick コンポーネント
                auto& reg = m_scene->GetRegistry();
                Gimmick g;
                const char* nm = "Gimmick";
                float sx = 2.0f, sy = 1.4f, sz = 1.2f, cr = 0.86f, cg = 0.16f, cb = 0.12f;
                if (req.modelPath == "__gimmick_spike__")
                {
                    nm = "Spike"; g.kind = 1; g.period = 3.6f; g.amplitude = 1.6f;
                    g.threshold = 0.5f; g.deadly = true;
                    sx = 2.0f; sy = 1.4f; sz = 1.2f; cr = 0.86f; cg = 0.16f; cb = 0.12f;
                }
                else if (req.modelPath == "__gimmick_slide__")
                {
                    nm = "SlideWall"; g.kind = 2; g.period = 5.0f; g.amplitude = 3.8f;
                    sx = 5.2f; sy = 1.5f; sz = 1.1f; cr = 0.72f; cg = 0.40f; cb = 0.14f;
                }
                else
                {
                    nm = "Wall"; g.kind = 0;
                    sx = 4.0f; sy = 1.6f; sz = 1.0f; cr = 0.30f; cg = 0.32f; cb = 0.40f;
                }
                auto e = m_scene->SpawnBox(nm, req.position);
                auto h = e.GetHandle();
                reg.get<Transform>(h).scale = {sx, sy, sz};
                if (auto* dev = m_scene->GetDevice())
                    if (auto* mr = reg.try_get<MeshRenderer>(h))
                        for (auto* mesh : mr->meshes)
                            if (mesh) mesh->SetVertexColor(*dev, cr, cg, cb, 1.0f);
                reg.emplace<Gimmick>(h, g);
                spawnedEntity = h;
            }
            else if (req.modelPath == "__particle_emitter__")
            {
                // 配置エフェクト: 空エンティティ + ParticleEmitter（エディタで即プレビュー表示）
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{req.name.empty() ? std::string("ParticleEmitter") : req.name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<ParticleEmitter>(e, ParticleEmitter{});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__trigger__")
            {
                // イベント範囲: 空エンティティ + Trigger（Inspector でアクションを組む）
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{req.name.empty() ? std::string("Trigger") : req.name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<Trigger>(e, Trigger{});
                spawnedEntity = e;
            }
            else if (std::filesystem::path(req.modelPath).extension().string() == ".prefab")
            {
                // プレハブ（再利用テンプレート）を展開。子も含めてサブツリーごと生成する。
                std::vector<entt::entity> all;
                entt::entity root = SceneSerializer::InstantiatePrefab(
                    *m_scene, req.modelPath, PathResolver::AssetsDir(), &all);
                if (root != entt::null)
                {
                    auto& reg = m_scene->GetRegistry();
                    if (reg.all_of<Transform>(root))
                        reg.get<Transform>(root).position = req.position;
                    m_editorCtx->Select(root);
                    m_editorCtx->undoSystem.PushCommand(
                        std::make_unique<SpawnPrefabCommand>(
                            m_scene.get(), PathResolver::AssetsDir(), all));
                    Logger::Info("Prefab instantiated ({} entities): {}", all.size(), req.modelPath);
                    mcpPrefabRoot = root;          // MCP 応答にルート + 全 id を返す
                    mcpPrefabAll  = std::move(all);
                }
                // spawnedEntity は null のまま（独自に Undo を積んだので下の汎用 SpawnEntityCommand はスキップ）
            }
            else
            {
                // 拡張子で振り分ける。画像を Assimp（モデルローダ）に食わせると
                // インポータ総当たりでフリーズ→クラッシュするため、モデル拡張子のみ Spawn へ。
                std::string ext = std::filesystem::path(req.modelPath).extension().string();
                for (auto& ch : ext) if (ch >= 'A' && ch <= 'Z') ch += 32;   // 小文字化
                auto extIs = [&ext](std::initializer_list<const char*> list) {
                    for (const char* s : list) if (ext == s) return true;
                    return false;
                };

                if (extIs({".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".gif"}))
                {
                    // 画像 → ワールド空間の 2D スプライトとして配置（texturePath は assets 相対の規約）。
                    // D&D は絶対パス・MCP spawn_model は assets 相対で来るので両対応。
                    std::string relStr;
                    const std::filesystem::path inPath(req.modelPath);
                    if (inPath.is_absolute())
                    {
                        std::error_code rec;
                        auto rel = std::filesystem::relative(inPath, PathResolver::AssetsDir(), rec);
                        relStr = rec ? std::string() : rel.generic_string();
                    }
                    else
                    {
                        relStr = inPath.generic_string();
                    }
                    if (relStr.empty() || relStr.rfind("..", 0) == 0)
                    {
                        Logger::Warn("ドロップされた画像が assets/ の外にあるため、スプライトとして配置できません: {}",
                                     req.modelPath);
                    }
                    else
                    {
                        auto& reg = m_scene->GetRegistry();
                        auto e = reg.create();
                        reg.emplace<NameTag>(e, NameTag{name});
                        // 床(Y=0)ドロップだと Z ファイトするので、既定サイズ(1)の半分だけ持ち上げる
                        DirectX::XMFLOAT3 pos = req.position;
                        pos.y += 0.5f;
                        reg.emplace<Transform>(e, Transform{pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                        Sprite2D sp{};
                        sp.texturePath = relStr;
                        sp.worldSpace  = true;
                        sp.billboard   = false;   // 既定はTransformの回転に従う（ビルボードはInspectorでON可）
                        reg.emplace<Sprite2D>(e, sp);
                        spawnedEntity = e;
                        Logger::Info("Placed world sprite: {}", relStr);
                    }
                }
                else if (extIs({".gltf", ".glb", ".obj", ".fbx", ".dae", ".stl", ".ply", ".3ds"}))
                {
                    auto entity = m_scene->Spawn(name, req.modelPath, req.position);

                    // D&D時のみ: AABBから自動スケーリング
                    bool valid = entity.IsValid();
                    bool hasMR = valid && entity.HasComponent<MeshRenderer>();
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "[D&D Scale] valid=%d hasMR=%d\n", valid, hasMR);
                        OutputDebugStringA(buf);
                    }

                    if (hasMR)
                    {
                        auto& mr = entity.GetComponent<MeshRenderer>();
                        f32 maxExtent = 0.0f;
                        for (const auto* mesh : mr.meshes)
                        {
                            if (!mesh) continue;
                            auto mn = mesh->GetAABBMin();
                            auto mx = mesh->GetAABBMax();
                            f32 dx = mx.x - mn.x;
                            f32 dy = mx.y - mn.y;
                            f32 dz = mx.z - mn.z;
                            if (dx > maxExtent) maxExtent = dx;
                            if (dy > maxExtent) maxExtent = dy;
                            if (dz > maxExtent) maxExtent = dz;
                        }
                        // 既存Luaモデルと同じ見た目サイズにスケーリング
                        constexpr f32 kDefaultScale = 0.01f;
                        auto& t = entity.GetComponent<Transform>();
                        t.scale = {kDefaultScale, kDefaultScale, kDefaultScale};

                        // glTF/glb はZ-upなのでX軸90度回転で立たせる
                        if (ext == ".gltf" || ext == ".glb")
                            t.rotation.x = 90.0f;
                    }
                    spawnedEntity = entity.GetHandle();
                }
                else
                {
                    Logger::Warn("未対応のファイルがシーンにドロップされたためスキップしました: {} "
                                 "（モデル: gltf/glb/obj/fbx/dae/stl/ply/3ds, 画像: png/jpg/bmp/tga/dds）",
                                 req.modelPath);
                }
            }

            // Undo に Spawn コマンドを積む
            if (spawnedEntity != entt::null)
            {
                m_editorCtx->undoSystem.PushCommand(
                    std::make_unique<SpawnEntityCommand>(
                        m_scene.get(), PathResolver::AssetsDir(), spawnedEntity));
            }
            Logger::Info("Spawned: {}", name);

            // MCP create_entity / spawn_model / spawn_prefab の遅延応答(本物の entityId を返す)。
            if (req.mcp.client != 0)
            {
                auto& reg = m_scene->GetRegistry();
                if (mcpPrefabRoot != entt::null)
                {
                    nlohmann::json ids = nlohmann::json::array();
                    for (auto a : mcpPrefabAll) ids.push_back(static_cast<u32>(a));
                    CompleteMcp(m_mcpBridge.get(), req.mcp,
                        nlohmann::json{{"entityId", static_cast<u32>(mcpPrefabRoot)},
                                       {"rootEntityId", static_cast<u32>(mcpPrefabRoot)},
                                       {"entityIds", ids}, {"name", name},
                                       {"sceneGeneration", m_sceneGeneration}});
                    if (!req.mcp.idempotencyKey.empty())
                        m_mcpIdempotency[req.mcp.idempotencyKey] = static_cast<u32>(mcpPrefabRoot);
                }
                else if (spawnedEntity != entt::null && reg.valid(spawnedEntity))
                {
                    CompleteMcp(m_mcpBridge.get(), req.mcp,
                        nlohmann::json{{"entityId", static_cast<u32>(spawnedEntity)},
                                       {"name", name}, {"sceneGeneration", m_sceneGeneration}});
                    if (!req.mcp.idempotencyKey.empty())
                        m_mcpIdempotency[req.mcp.idempotencyKey] = static_cast<u32>(spawnedEntity);
                }
                else
                {
                    FailMcp(m_mcpBridge.get(), req.mcp, McpErr::Internal,
                            "spawn failed (model load error? check dx12_get_log): " + req.modelPath);
                }
            }
        }
    }

    // プレハブ書き出し（選択エンティティ + 子孫を assets/prefabs/<name>.prefab へ保存）
    if (m_editorCtx->pendingCreatePrefab != entt::null && m_engineMode == EngineMode::Editor)
    {
        entt::entity root = m_editorCtx->pendingCreatePrefab;
        m_editorCtx->pendingCreatePrefab = entt::null;

        auto& reg = m_scene->GetRegistry();
        if (reg.valid(root) && reg.all_of<NameTag>(root))
        {
            namespace fs = std::filesystem;
            std::string base = reg.get<NameTag>(root).name;
            if (base.empty()) base = "Prefab";
            fs::path dir = fs::path(PathResolver::AssetsDir()) / "prefabs";
            std::error_code ec; fs::create_directories(dir, ec);
            fs::path file = dir / (base + ".prefab");
            for (int n = 1; fs::exists(file); ++n)
                file = dir / (base + " (" + std::to_string(n) + ").prefab");

            if (SceneSerializer::SavePrefab(*m_scene, root, file.string(), PathResolver::AssetsDir()))
            {
                m_editorCtx->hotReloadFlash = 1.0f;   // 保存通知のフラッシュを流用
                Logger::Info("Prefab created: {}", file.string());
            }
        }
    }

    // ファイルメニュー「プロジェクトを閉じる」→ ランチャーに戻す。
    // 「ランチャーに戻る」ボタン（RenderProjectWindow）と同じ遷移＝ファイル削除等は一切不要。
    if (m_editorCtx->pendingCloseProject)
    {
        m_editorCtx->pendingCloseProject = false;
        m_showLauncher = true;
    }

    // Undo/Redo（エンティティ復元がモデル再ロードを伴うため cmdList 有効時に実行）
    if ((m_editorCtx->pendingUndo || m_editorCtx->pendingRedo)
        && m_engineMode == EngineMode::Editor)
    {
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        if (m_editorCtx->pendingUndo) m_editorCtx->undoSystem.Undo();
        if (m_editorCtx->pendingRedo) m_editorCtx->undoSystem.Redo();
        m_editorCtx->pendingUndo = false;
        m_editorCtx->pendingRedo = false;
    }

    // エンティティ複製（Ctrl+D / 右クリック複製。全コンポーネントのディープコピー）
    if (!m_editorCtx->pendingDuplications.empty() && m_engineMode == EngineMode::Editor)
    {
        auto sources = std::move(m_editorCtx->pendingDuplications);
        m_editorCtx->pendingDuplications.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        m_editorCtx->ClearSelection();
        for (auto src : sources)
        {
            entt::entity copy = SceneSerializer::DuplicateEntity(
                *m_scene, src, PathResolver::AssetsDir());
            if (copy == entt::null) continue;

            m_editorCtx->AddToSelection(copy);
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<SpawnEntityCommand>(
                    m_scene.get(), PathResolver::AssetsDir(), copy));
            Logger::Info("Duplicated entity: {}",
                         m_scene->GetRegistry().get<NameTag>(copy).name);
        }
    }

    // Deferred: MCP entity deletion（サブツリー削除後に deletedCount を遅延応答で返す）。
    // ★エディタUIブランチの中ではなく Render トップレベルに置く(mcpDuplications と同格)。
    //   ランチャー表示中でも drain され、MCP の delete_entity が未応答ハングしない。
    if (!m_editorCtx->mcpDeletions.empty() && m_engineMode == EngineMode::Editor)
    {
        auto dels = std::move(m_editorCtx->mcpDeletions);
        m_editorCtx->mcpDeletions.clear();
        auto& reg = m_scene->GetRegistry();
        for (auto& d : dels)
        {
            const entt::entity root = d.entity;
            if (!reg.valid(root))
            {
                // 既に(先行サブツリー等で)削除済み。冪等に成功扱い(deletedCount=0)。
                CompleteMcp(m_mcpBridge.get(), d.mcp,
                    nlohmann::json{{"deletedEntityId", static_cast<u32>(root)},
                                   {"deletedCount", 0}, {"sceneGeneration", m_sceneGeneration}});
                continue;
            }
            // サブツリー収集（親→子の順。BFS）— 既存削除ブロックと同手順。
            std::vector<entt::entity> subtree{root};
            for (size_t i = 0; i < subtree.size(); ++i)
                for (auto [c, t] : reg.view<const Transform>().each())
                    if (t.parent == subtree[i]) subtree.push_back(c);

            std::vector<DeletedEntityRecord> records;
            records.reserve(subtree.size());
            entt::entity externalParent = reg.all_of<Transform>(root)
                ? reg.get<Transform>(root).parent : entt::null;
            for (auto e : subtree)
            {
                DeletedEntityRecord rec;
                rec.snapshot = SceneSerializer::SerializeEntity(*m_scene, e, PathResolver::AssetsDir());
                if (reg.all_of<Transform>(e))
                {
                    auto parent = reg.get<Transform>(e).parent;
                    auto it = std::find(subtree.begin(), subtree.end(), parent);
                    if (it != subtree.end())
                        rec.parentLocalIndex = static_cast<int>(it - subtree.begin());
                }
                records.push_back(std::move(rec));
            }
            const int deletedCount = static_cast<int>(subtree.size());
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<DeleteEntityCommand>(
                    m_scene.get(), PathResolver::AssetsDir(),
                    std::move(records), subtree, externalParent));
            for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
                if (reg.valid(*it)) m_scene->Remove(Entity(*it, &reg));

            CompleteMcp(m_mcpBridge.get(), d.mcp,
                nlohmann::json{{"deletedEntityId", static_cast<u32>(root)},
                               {"deletedCount", deletedCount},
                               {"sceneGeneration", m_sceneGeneration}});
        }
        // 削除で無効になった選択をクリーンアップ
        auto& sel = m_editorCtx->selectedEntities;
        sel.erase(std::remove_if(sel.begin(), sel.end(),
                  [&](entt::entity e) { return !reg.valid(e); }), sel.end());
        if (m_editorCtx->selectedEntity != entt::null && !reg.valid(m_editorCtx->selectedEntity))
            m_editorCtx->selectedEntity = sel.empty() ? entt::null : sel.back();
    }

    // Deferred: MCP entity duplication（複製先 entityId を遅延応答で返す）
    if (!m_editorCtx->mcpDuplications.empty() && m_engineMode == EngineMode::Editor)
    {
        auto dups = std::move(m_editorCtx->mcpDuplications);
        m_editorCtx->mcpDuplications.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        auto& reg = m_scene->GetRegistry();
        for (auto& d : dups)
        {
            if (!reg.valid(d.entity))
            {
                FailMcp(m_mcpBridge.get(), d.mcp, McpErr::NotFound, "source entity no longer valid");
                continue;
            }
            entt::entity copy = SceneSerializer::DuplicateEntity(*m_scene, d.entity, PathResolver::AssetsDir());
            if (copy == entt::null)
            {
                FailMcp(m_mcpBridge.get(), d.mcp, McpErr::Internal, "duplicate failed");
                continue;
            }
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<SpawnEntityCommand>(m_scene.get(), PathResolver::AssetsDir(), copy));
            std::string nm = reg.all_of<NameTag>(copy) ? reg.get<NameTag>(copy).name : std::string();
            Logger::Info("Duplicated entity (MCP): {}", nm);
            CompleteMcp(m_mcpBridge.get(), d.mcp,
                nlohmann::json{{"entityId", static_cast<u32>(copy)}, {"name", nm},
                               {"sceneGeneration", m_sceneGeneration}});
        }
    }

    // エンティティペースト（Ctrl+V。コピー時の JSON スナップショットから生成）
    if (!m_editorCtx->pendingPastes.empty() && m_engineMode == EngineMode::Editor)
    {
        auto pastes = std::move(m_editorCtx->pendingPastes);
        m_editorCtx->pendingPastes.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        m_editorCtx->ClearSelection();
        for (const auto& snap : pastes)
        {
            entt::entity e = SceneSerializer::InstantiateEntity(
                *m_scene, snap, PathResolver::AssetsDir());
            if (e == entt::null) continue;

            auto& reg = m_scene->GetRegistry();
            // 元と重ならないよう少しずらして配置
            if (reg.all_of<Transform>(e))
                reg.get<Transform>(e).position.x += 1.0f;

            m_editorCtx->AddToSelection(e);
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<SpawnEntityCommand>(
                    m_scene.get(), PathResolver::AssetsDir(), e));
            Logger::Info("Pasted entity: {}", reg.get<NameTag>(e).name);
        }
    }

    // スクリプトアタッチ遅延処理
    if (!m_editorCtx->pendingScriptAttachments.empty())
    {
        auto attachments = std::move(m_editorCtx->pendingScriptAttachments);
        m_editorCtx->pendingScriptAttachments.clear();

        char dbgBuf[128];
        snprintf(dbgBuf, sizeof(dbgBuf),
            "[PendingScriptAttachments] processing %zu\n", attachments.size());
        OutputDebugStringA(dbgBuf);

        auto& reg = m_scene->GetRegistry();
        for (const auto& req : attachments)
        {
            if (!reg.valid(req.entity))
            {
                OutputDebugStringA("[PendingScriptAttachments] SKIP invalid entity\n");
                continue;
            }

            // Undo 用に現状を保存
            bool        hadBefore  = reg.all_of<LuaScript>(req.entity);
            std::string oldPath;
            bool        oldEnabled = true;
            if (hadBefore)
            {
                const auto& cur = reg.get<LuaScript>(req.entity);
                oldPath    = cur.scriptPath;
                oldEnabled = cur.enabled;
            }

            m_scriptEngine->AttachScriptToEntity(req.entity, req.scriptPath);

            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<AttachScriptCommand>(
                    &reg, req.entity,
                    hadBefore, oldPath, oldEnabled,
                    req.scriptPath));
        }
    }

    // マテリアルテクスチャD&D割当 遅延処理（アセットブラウザ→SceneView/Inspector）
    if (!m_editorCtx->pendingMaterialTextureDrops.empty())
    {
        auto drops = std::move(m_editorCtx->pendingMaterialTextureDrops);
        m_editorCtx->pendingMaterialTextureDrops.clear();

        auto& reg = m_scene->GetRegistry();
        std::string base = std::filesystem::path(PathResolver::AssetsDir()).lexically_normal().string();
        std::replace(base.begin(), base.end(), '\\', '/');

        for (const auto& req : drops)
        {
            if (!reg.valid(req.entity) || !reg.all_of<MeshRenderer>(req.entity))
                continue;

            // 絶対パス → assets 相対パスへ正規化(HierarchyPanel のスクリプトD&Dと同じ手順)
            std::string abs = std::filesystem::path(req.texturePath).lexically_normal().string();
            std::replace(abs.begin(), abs.end(), '\\', '/');
            std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

            auto& mr = reg.get<MeshRenderer>(req.entity);
            MeshRenderer before = mr;   // Undo 用スナップショット(値コピー、生ポインタは共有でOK)
            switch (req.slot)
            {
                case MaterialTextureSlot::Albedo:
                    MeshRenderer::SetOverride(mr.overrideAlbedoTexture, req.submeshIndex, rel);
                    break;
                case MaterialTextureSlot::Normal:
                    MeshRenderer::SetOverride(mr.overrideNormalTexture, req.submeshIndex, rel);
                    break;
                case MaterialTextureSlot::MetalRoughness:
                    MeshRenderer::SetOverride(mr.overrideMetalRoughnessTexture, req.submeshIndex, rel);
                    break;
            }
            m_editorCtx->undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                &reg, req.entity, before, mr, "Material Texture"));

            // キャッシュは消さない（EnsureMaterialOverrideSrv がパス不一致を検知して同じ
            // blockStart 上へ CreateSRV し直す。erase すると AllocateBlock が再度走り
            // 前のブロックを解放しないまま SRV ヒープを浪費するので避ける）。

            Logger::Info("マテリアルテクスチャ割当: entity={} submesh={} slot={} -> {}",
                static_cast<u32>(req.entity), req.submeshIndex, static_cast<int>(req.slot), rel);
        }
    }

    // サムネイルテクスチャのロード（描画コマンドの前に実行）
    m_editorLayer->LoadPendingThumbnails(nativeCmdList);

    // モデルサムネイルのオフスクリーンレンダリング
    m_thumbRenderer->RenderPending(nativeCmdList, m_swapChain->GetCurrentBackBufferIndex());

    u32 frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    f32 totalTime = m_gameClock.GetTotalTime();

    // シャドウマップ再作成（ImGuiで解像度変更時、前フレーム完了後に実行）
    if (m_shadowMapDirty)
    {
        m_shadowMapDirty = false;
        m_shadowMap.Reset();

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = m_shadowMapSize;
        shadowDesc.Height = m_shadowMapSize;
        shadowDesc.DepthOrArraySize = static_cast<u16>(kNumCascades);
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc = {1, 0};
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &shadowDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&m_shadowMap)));

        // DSV はハンドル再利用（初回 Allocate 済み）。スライス毎に再作成。
        for (u32 i = 0; i < kNumCascades; ++i)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.FirstArraySlice = i;
            dsvDesc.Texture2DArray.ArraySize = 1;
            dsvDesc.Texture2DArray.MipSlice = 0;
            m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                m_shadowMap.Get(), &dsvDesc, m_shadowDsvHandles[i]);
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = kNumCascades;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_shadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_shadowSrvIndex));
    }

    // ===== メインパスのビューポートとカメラ投影を先に確定 =====
    // 以降の CSM / SSAO / forward はすべてこの同じカメラ状態を前提にする。
    // ここがシャドウ計算より後だと、Scene カメラの投影で影を作って GameCamera で描くなどの
    // 経路差が起き、Scene/Game で光の強さが違って見える。
    u32 vpLeft, vpTop, vpW, vpH;
    {
        auto vp = m_editorLayer->GetViewportPos();
        auto vs = m_editorLayer->GetViewportSize();
        vpLeft = static_cast<u32>(vp.x);
        vpTop  = static_cast<u32>(vp.y);
        vpW    = static_cast<u32>(vs.x);
        vpH    = static_cast<u32>(vs.y);
        if (vpW < 1) vpW = 1;
        if (vpH < 1) vpH = 1;
        // 全画面は「単体ゲーム（エディタUIなし）」のみ。エディタは編集中も Play 中も
        // 中央の 16:9 ビューポート矩形に描く（パネル下に潜り込ませない）。
        if (m_isGameMode)
        {
            vpLeft = 0; vpTop = 0;
            vpW = m_window->GetWidth();
            vpH = m_window->GetHeight();
        }
    }

    const f32 renderAspect = static_cast<f32>(vpW) / static_cast<f32>(vpH);

    // ===== 2D ビューモード: エディタカメラを正射＋XY平面正対(forward +Z)へ固定 =====
    // 回転/ドリーは入力側で無効化済み。Play 中は CameraComponent 同期が優先する。
    if (!m_isGameMode && m_engineMode == EngineMode::Editor)
    {
        if (m_editorCtx->view2D)
        {
            // 3D→2D に入った瞬間だけ、3Dカメラの位置/向きを退避（戻した時に復元する）。
            if (!m_editorWas2D)
            {
                m_cam3DSnapshot.position = m_camera->GetPosition();
                m_cam3DSnapshot.yaw      = m_camera->GetYaw();
                m_cam3DSnapshot.pitch    = m_camera->GetPitch();
                m_has3DSnapshot = true;
            }
            m_camera->SetYaw(0.0f);
            m_camera->SetPitch(0.0f);
            XMFLOAT3 p = m_camera->GetPosition();
            p.z = -100.0f;                                   // XY 平面(z=0)を十分手前から見る
            m_camera->SetPosition(p);
            m_camera->SetOrthographic(2.0f * m_editorCtx->view2DZoom,
                                      renderAspect, 0.1f, 2000.0f);
        }
        else
        {
            // 2D→3D に戻った瞬間だけ、退避してあった 3Dカメラ状態を復元（視点が壊れないように）。
            if (m_editorWas2D && m_has3DSnapshot)
            {
                m_camera->SetPosition(m_cam3DSnapshot.position);
                m_camera->SetYaw(m_cam3DSnapshot.yaw);
                m_camera->SetPitch(m_cam3DSnapshot.pitch);
            }
            m_camera->SetPerspective(DirectX::XM_PIDIV4, renderAspect, 0.1f, 1000.0f);
        }
        m_editorWas2D = m_editorCtx->view2D;
    }
    else
    {
        // Play / 単体ゲーム: アクティブな CameraComponent の投影（透視/正射・FOV・orthoSize・near/far）を
        // 実ビューポートのアスペクトで m_camera に毎フレーム反映する。
        bool applied = false;
        auto& reg = m_scene->GetRegistry();
        for (auto [e, cam] : reg.view<const CameraComponent>().each())
        {
            if (!cam.isActive) continue;
            if (cam.projection == CameraProjection::Orthographic)
                m_camera->SetOrthographic(2.0f * cam.orthoSize, renderAspect, cam.nearClip, cam.farClip);
            else
                m_camera->SetPerspective(DirectX::XMConvertToRadians(cam.fovDegrees),
                                         renderAspect, cam.nearClip, cam.farClip);
            applied = true;
            break;
        }
        if (!applied)
            m_camera->SetAspect(renderAspect);  // アクティブカメラが無ければアスペクトのみ更新
    }

    // ライトの向きを Transform 回転に追従させる（回転の変化分=デルタを direction に適用）。
    // これでインスペクターの Transform 回転でもギズモ回転でも光の向きが変わる。
    // direction を真実とし回転はデルタのみ与えるので、Direction 欄の直接編集とも共存できる。
    {
        auto& reg = m_scene->GetRegistry();
        auto eq = [](const XMFLOAT3& r) {
            return XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(r.x), XMConvertToRadians(r.y), XMConvertToRadians(r.z));
        };
        auto applyDelta = [&](XMFLOAT3& dir, XMFLOAT3& prevRot, bool& init, const XMFLOAT3& rot) {
            if (!init) { prevRot = rot; init = true; return; }
            if (rot.x == prevRot.x && rot.y == prevRot.y && rot.z == prevRot.z) return;
            XMVECTOR delta = XMQuaternionMultiply(XMQuaternionInverse(eq(prevRot)), eq(rot));
            XMVECTOR nd = XMVector3Normalize(XMVector3Rotate(XMLoadFloat3(&dir), delta));
            XMStoreFloat3(&dir, nd);
            prevRot = rot;
        };
        for (auto [e, dl, tf] : reg.view<dx12e::DirectionalLight, const Transform>().each())
            applyDelta(dl.direction, dl._prevRot, dl._prevRotInit, tf.rotation);
        for (auto [e, sl, tf] : reg.view<dx12e::SpotLight, const Transform>().each())
            applyDelta(sl.direction, sl._prevRot, sl._prevRotInit, tf.rotation);
    }

    // ライト方向/色: ECS の DirectionalLight から取得。
    // ★ DirectionalLight が無い場合は「太陽光なし」(色=黒) にする。
    //   以前はフル強度の既定太陽にフォールバックしていたため、ライトを消しても
    //   明るいまま＆既定方向の影が出る、という分かりにくい挙動だった。
    //   ambient だけ残すので真っ暗にはならない。
    XMFLOAT3 lightDirF3   = {-0.3f, -1.0f, -0.5f};  // 影の方向（色が黒なら影は出ない）
    XMFLOAT3 lightColorF3 = {0.0f, 0.0f, 0.0f};      // 既定=太陽なし
    float    lightAmbient = 0.25f;
    {
        auto& reg = m_scene->GetRegistry();
        auto dlView = reg.view<const dx12e::DirectionalLight>();
        if (!dlView.empty())
        {
            auto first = *dlView.begin();
            const auto& dl = dlView.get<const dx12e::DirectionalLight>(first);
            lightDirF3 = dl.direction;
            lightColorF3 = {dl.color.x * dl.intensity,
                            dl.color.y * dl.intensity,
                            dl.color.z * dl.intensity};
            lightAmbient = dl.ambient;
        }
    }
    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirF3));
    XMStoreFloat3(&lightDirF3, lightDir);  // 正規化した値を書き戻す
    // CSM: 確定済みの描画カメラ視錐台を 4 分割し、各カスケードをライト視点へタイトフィット。
    // 結果は m_cascadeViewProj[] / m_cascadeSplitsView[] に格納される。
    // CSM は透視前提。正射カメラでは ComputeCascades 側が無影センチネルへ切り替える。
    {
        f32 camNear = m_camera->GetNearZ();
        f32 camFar  = m_camera->GetFarZ();
        // 影が無限遠まで必要なわけではないので、現実的な距離にクランプ（タイトに保つ）。
        camFar = (std::min)(camFar, 200.0f);
        ComputeCascades(lightDir, camNear, camFar);
    }

    // SRV ヒープをバインド（シャドウパスでもボーンSRVが必要）
    m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
    m_commandList->SetRootSignature(*m_rootSignature);

    // ===== スポットライト影スロット割当（castShadows なライトをカメラに近い順で最大kMaxShadowSpot灯）=====
    // 結果は m_spotShadowViewProj[] / m_spotShadowEntity[] に格納し、直後の影パス描画と
    // 後段のライト収集(shadowIndex書き込み)の両方で使う。
    m_numSpotShadowSlots = 0;
    {
        auto& reg = m_scene->GetRegistry();
        struct SpotShadowCandidate { entt::entity e; f32 distSq; };
        std::vector<SpotShadowCandidate> candidates;
        XMFLOAT3 camPosF3 = m_camera->GetPosition();
        XMVECTOR camPos = XMLoadFloat3(&camPosF3);
        auto slView = reg.view<const dx12e::SpotLight, const Transform>();
        for (auto [e, sl, tf] : slView.each())
        {
            if (!sl.castShadows) continue;
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR d = XMVectorSubtract(world.r[3], camPos);
            candidates.push_back({e, XMVectorGetX(XMVector3LengthSq(d))});
        }
        std::sort(candidates.begin(), candidates.end(),
                 [](const auto& a, const auto& b) { return a.distSq < b.distSq; });

        const u32 n = (std::min)(static_cast<u32>(candidates.size()), kMaxShadowSpot);
        for (u32 i = 0; i < n; ++i)
        {
            entt::entity e = candidates[i].e;
            const auto& sl = reg.get<const dx12e::SpotLight>(e);
            const auto& tf = reg.get<const Transform>(e);
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR pos = world.r[3];
            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&sl.direction));
            // dir がワールドYにほぼ平行だと LookToLH の up ベクトルが縮退するので切り替える。
            XMVECTOR up = (std::fabs(XMVectorGetY(dir)) > 0.99f)
                ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f) : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

            f32 outerDeg = (std::max)(sl.outerConeDeg, sl.innerConeDeg);
            f32 fov = (std::min)(XMConvertToRadians(outerDeg) * 2.0f * 1.02f, XMConvertToRadians(170.0f));
            f32 range = (std::max)(sl.range, 0.2f);

            XMMATRIX lightView = XMMatrixLookToLH(pos, dir, up);
            XMMATRIX lightProj = XMMatrixPerspectiveFovLH(fov, 1.0f, 0.1f, range);
            XMStoreFloat4x4(&m_spotShadowViewProj[i], lightView * lightProj);
            m_spotShadowEntity[i] = e;
        }
        m_numSpotShadowSlots = n;
    }

    // ===== スポットライト影パス =====
    if (m_scene && m_scene->GetShadowsEnabled() && m_numSpotShadowSlots > 0)
    {
        m_commandList->TransitionResource(m_spotShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_VIEWPORT spotVp{};
        spotVp.Width = spotVp.Height = static_cast<f32>(kSpotShadowMapSize);
        spotVp.MinDepth = 0.0f;
        spotVp.MaxDepth = 1.0f;
        D3D12_RECT spotScissor = {0, 0, static_cast<LONG>(kSpotShadowMapSize), static_cast<LONG>(kSpotShadowMapSize)};
        nativeCmdList->RSSetViewports(1, &spotVp);
        nativeCmdList->RSSetScissorRects(1, &spotScissor);

        for (u32 i = 0; i < m_numSpotShadowSlots; ++i)
        {
            XMMATRIX lvp = XMLoadFloat4x4(&m_spotShadowViewProj[i]);
            m_commandList->ClearDepthStencil(m_spotShadowDsvHandles[i]);
            nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_spotShadowDsvHandles[i]);
            RenderDepthOnlyScene(lvp, *m_shadowPipelineState, *m_shadowSkinnedPipelineState,
                                 /*updateSkinning*/ false, frameIndex);
        }

        m_commandList->TransitionResource(m_spotShadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ===== ポイントライト影スロット割当（castShadows なライトをカメラに近い順で最大kMaxShadowPoint灯）=====
    m_numPointShadowSlots = 0;
    {
        auto& reg = m_scene->GetRegistry();
        struct PointShadowCandidate { entt::entity e; f32 distSq; };
        std::vector<PointShadowCandidate> candidates;
        XMFLOAT3 camPosF3 = m_camera->GetPosition();
        XMVECTOR camPos = XMLoadFloat3(&camPosF3);
        auto plShadowView = reg.view<const dx12e::PointLight, const Transform>();
        for (auto [e, pl, tf] : plShadowView.each())
        {
            if (!pl.castShadows) continue;
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR d = XMVectorSubtract(world.r[3], camPos);
            candidates.push_back({e, XMVectorGetX(XMVector3LengthSq(d))});
        }
        std::sort(candidates.begin(), candidates.end(),
                 [](const auto& a, const auto& b) { return a.distSq < b.distSq; });

        const u32 n = (std::min)(static_cast<u32>(candidates.size()), kMaxShadowPoint);
        for (u32 i = 0; i < n; ++i)
            m_pointShadowEntity[i] = candidates[i].e;
        m_numPointShadowSlots = n;
    }

    // ===== ポイントライト影パス（灯ごとに6面。D3Dキューブ面順: +X,-X,+Y,-Y,+Z,-Z）=====
    if (m_scene && m_scene->GetShadowsEnabled() && m_numPointShadowSlots > 0)
    {
        static const XMFLOAT3 kFaceDir[6] = {
            { 1,  0,  0}, {-1,  0,  0}, { 0,  1,  0}, { 0, -1,  0}, { 0,  0,  1}, { 0,  0, -1},
        };
        static const XMFLOAT3 kFaceUp[6] = {
            {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0},
        };

        m_commandList->TransitionResource(m_pointShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_VIEWPORT pointVp{};
        pointVp.Width = pointVp.Height = static_cast<f32>(kPointShadowMapSize);
        pointVp.MinDepth = 0.0f;
        pointVp.MaxDepth = 1.0f;
        D3D12_RECT pointScissor = {0, 0, static_cast<LONG>(kPointShadowMapSize), static_cast<LONG>(kPointShadowMapSize)};
        nativeCmdList->RSSetViewports(1, &pointVp);
        nativeCmdList->RSSetScissorRects(1, &pointScissor);

        auto& reg = m_scene->GetRegistry();
        for (u32 i = 0; i < m_numPointShadowSlots; ++i)
        {
            entt::entity e = m_pointShadowEntity[i];
            const auto& pl = reg.get<const dx12e::PointLight>(e);
            const auto& tf = reg.get<const Transform>(e);
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR pos = world.r[3];
            f32 range = (std::max)(pl.range, 0.2f);
            XMMATRIX faceProj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, range);

            for (u32 f = 0; f < 6; ++f)
            {
                XMMATRIX faceView = XMMatrixLookToLH(pos, XMLoadFloat3(&kFaceDir[f]), XMLoadFloat3(&kFaceUp[f]));
                u32 slice = i * 6 + f;
                m_commandList->ClearDepthStencil(m_pointShadowDsvHandles[slice]);
                nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_pointShadowDsvHandles[slice]);
                RenderDepthOnlyScene(faceView * faceProj, *m_shadowPipelineState, *m_shadowSkinnedPipelineState,
                                     /*updateSkinning*/ false, frameIndex);
            }
        }

        m_commandList->TransitionResource(m_pointShadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ===== シャドウパス（CSM: カスケード毎に kNumCascades 回描画）=====
    // シーンで影 OFF / 正射カメラのときは丸ごとスキップ＝(全アクティブ敵 × 4カスケード)の
    // ドローと 2048²×4 のデプスフィルを撤廃（lv35 の主因）。m_shadowMap は生成時 PSR のまま＝
    // forward の t4 バインドは有効（センチネルで読まれないので未クリアでも安全）。
    if (m_scene && m_scene->GetShadowsEnabled() && !m_camera->IsOrthographic())
    {
        // 配列リソース全体を一括で DEPTH_WRITE へ遷移（カスケードループの外で1回）
        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // シャドウマップ用ビューポート（全カスケード共通＝各スライス同サイズ正方）
        D3D12_VIEWPORT shadowVp{};
        shadowVp.Width    = static_cast<f32>(m_shadowMapSize);
        shadowVp.Height   = static_cast<f32>(m_shadowMapSize);
        shadowVp.MinDepth = 0.0f;
        shadowVp.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor = {0, 0, static_cast<LONG>(m_shadowMapSize), static_cast<LONG>(m_shadowMapSize)};
        nativeCmdList->RSSetViewports(1, &shadowVp);
        nativeCmdList->RSSetScissorRects(1, &shadowScissor);

        for (u32 ci = 0; ci < kNumCascades; ++ci)
        {
            XMMATRIX cascadeVP = XMLoadFloat4x4(&m_cascadeViewProj[ci]);

            m_commandList->ClearDepthStencil(m_shadowDsvHandles[ci]);
            // RTVなし、DSVのみ（該当カスケードのスライス）
            nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandles[ci]);

            // skinningBuffer の Update はフレーム内1回で良い（最初のカスケードのみ）。SRVバインドは各カスケードで必要。
            RenderDepthOnlyScene(cascadeVP, *m_shadowPipelineState, *m_shadowSkinnedPipelineState,
                                 /*updateSkinning*/ ci == 0, frameIndex);
        }

        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ===== メインパス（オフスクリーン RT へ描画）=====

    // ===== 深度プリパス → SSAO（透視のみ。2D 正射ビューや無効時は素通し）=====
    // SSAO 有効時はカメラ視点の深度を m_depthBuffer へ先に完成させ、深度から法線を再構築して AO を作る。
    const SSAOSettings& ssaoCfg = m_scene->GetSSAOSettings();
    // SSAO は透視前提（深度線形化が透視射影に依存）。正射カメラ（俯瞰ゲーム/2Dビュー）では
    // AO 計算が壊れて全面 AO≈0 になり、ambient を黒く潰す（ゲームだけ真っ暗の原因）。→ 正射は無効化。
    const bool useSSAO = ssaoCfg.enabled && m_ssaoPass && m_ssaoPass->IsReady()
                       && !(m_editorCtx && m_editorCtx->view2D)
                       && !m_camera->IsOrthographic();
    u32 aoSrv = m_ssaoWhiteSrvIndex;  // 既定 = 白（AO=1.0 素通し）

    if (useSSAO)
    {
        XMMATRIX camVP = m_camera->GetViewProjMatrix();

        // --- 深度プリパス（カメラ視点で m_depthBuffer/m_dsvHandle へ深度のみ書く）---
        m_commandList->ClearDepthStencil(m_dsvHandle);
        nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_dsvHandle);
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

        // skinningBuffer は毎フレーム1回どこかで Update されていれば良い（このプリパスより前に
        // シャドウパスの ci==0 で更新済み＝ここでは false）。
        RenderDepthOnlyScene(camVP, *m_depthPrepassPSO, *m_depthPrepassSkinnedPSO,
                             /*updateSkinning*/ false, frameIndex);

        // --- SSAO 生成（depth SRV を読み AO→Blur）---
        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        aoSrv = m_ssaoPass->Generate(nativeCmdList, m_srvHeap.get(),
            m_srvHeap->GetGpuHandle(m_depthSrvIndex), ssaoCfg,
            m_camera->GetProjectionMatrix(), m_camera->GetNearZ(), m_camera->GetFarZ(),
            vpLeft, vpTop, vpW, vpH, frameIndex);
        // 生成失敗（未準備）時は白ダミー(AO=1.0)へフォールバック。誤テクスチャの読み出しを防ぐ。
        if (aoSrv == DescriptorHeap::kInvalidIndex)
            aoSrv = m_ssaoWhiteSrvIndex;
        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // SSAO/プリパスで RootSig/PSO/RT/ヒープを切り替えたので forward 用に再設定
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        m_commandList->SetRootSignature(*m_rootSignature);
    }

    m_sceneRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    constexpr float clearColor[4] = {0.127f, 0.306f, 0.850f, 1.0f};  // リニア空間のコーンフラワーブルー
    m_commandList->ClearRenderTarget(m_sceneRT->GetRtv(), clearColor);
    // プリパス有効時は深度が完成済みなので forward では clear しない（再利用）。
    if (!useSSAO)
        m_commandList->ClearDepthStencil(m_dsvHandle);
    m_commandList->SetRenderTarget(m_sceneRT->GetRtv(), m_dsvHandle);
    m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

    m_commandList->SetPipelineState(*m_pipelineState);

    // PerFrame CB（PointLight / SpotLight 各最大8灯対応）
    // レイアウトは shaders/forward/Lighting.hlsli の PerFrameConstants と完全一致させること。
    static constexpr u32 kMaxPointLightsR = 8;
    static constexpr u32 kMaxSpotLightsR  = 8;
    struct PointLightGPU {
        XMFLOAT3 position;
        float range;
        XMFLOAT3 color;
        float shadowIndex;   // -1=影なし、それ以外はポイント影キューブ配列のインデックス
    };
    struct SpotLightGPU {
        XMFLOAT3 position;   float range;
        XMFLOAT3 direction;  float cosInner;
        XMFLOAT3 color;      float cosOuter;
        float shadowIndex;   XMFLOAT3 _spad;  // -1=影なし、それ以外は spotShadowMatrix[] のインデックス
    };
    struct FrameConstants {
        XMFLOAT4X4 view;
        XMFLOAT4X4 proj;
        XMFLOAT3   lightDir;
        float      time;
        XMFLOAT3   lightColor;
        float      ambientStrength;
        XMFLOAT4X4 cascadeViewProj[kNumCascades]; // 256B
        XMFLOAT4   cascadeSplitsView;             // 16B
        XMFLOAT4   shadowParams;                  // 16B
        XMFLOAT3   cameraPos;
        float      aoEnabled;   // 1=実AOを読む / 0=AO読まず ao=1（白ダミー1x1の範囲外Load=0で環境光が消えるのを防ぐ）
        u32        numPointLights;
        u32        numSpotLights;
        float      spotShadowTexel;   // 1/kSpotShadowMapSize
        float      pointShadowNear;
        PointLightGPU pointLights[kMaxPointLightsR];
        SpotLightGPU  spotLights[kMaxSpotLightsR];
        XMFLOAT4X4 spotShadowMatrix[kMaxShadowSpot]; // 256B
        // ▼ IBL 制御 16B
        float iblIntensity;
        float maxPrefilterMip;
        u32   hasIBL;
        float skyboxIntensity;
    };
    static_assert(sizeof(FrameConstants) == 1520, "FrameConstants must be 1520 bytes");

    FrameConstants fc{};
    XMStoreFloat4x4(&fc.view, XMMatrixTranspose(m_camera->GetViewMatrix()));
    XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(m_camera->GetProjectionMatrix()));
    fc.lightDir = lightDirF3;
    fc.time = totalTime;
    fc.lightColor = lightColorF3;
    fc.ambientStrength = lightAmbient;
    // CSM: カスケード行列（HLSL は列優先 mul(row,mat) なので転置して格納）
    for (u32 i = 0; i < kNumCascades; ++i)
        XMStoreFloat4x4(&fc.cascadeViewProj[i],
            XMMatrixTranspose(XMLoadFloat4x4(&m_cascadeViewProj[i])));
    fc.cascadeSplitsView = {m_cascadeSplitsView[0], m_cascadeSplitsView[1],
                            m_cascadeSplitsView[2], m_cascadeSplitsView[3]};
    fc.shadowParams = {1.0f / static_cast<f32>(m_shadowMapSize), m_shadowDepthBias,
                       m_cascadeBlendBand, m_showCascadeDebug ? 1.0f : 0.0f};
    fc.cameraPos = m_camera->GetPosition();
    fc.spotShadowTexel = 1.0f / static_cast<f32>(kSpotShadowMapSize);
    fc.pointShadowNear = 0.1f;
    // スポット影行列（HLSL は列優先 mul(row,mat) なので転置して格納。上で割り当てたスロット分だけ埋める）
    for (u32 i = 0; i < kMaxShadowSpot; ++i)
        XMStoreFloat4x4(&fc.spotShadowMatrix[i],
            i < m_numSpotShadowSlots ? XMMatrixTranspose(XMLoadFloat4x4(&m_spotShadowViewProj[i])) : XMMatrixIdentity());

    // IBL 制御
    fc.iblIntensity    = m_iblReady ? m_iblIntensity : 0.0f;
    fc.maxPrefilterMip = m_iblBaker ? m_iblBaker->GetMaxPrefilterMip() : 4.0f;
    fc.hasIBL          = (m_iblReady && m_iblBaker && m_iblBaker->HasEnvironment()) ? 1u : 0u;
    fc.skyboxIntensity = m_skyboxIntensity;

    // AO: 実 AO テクスチャがバインドされている時だけシェーダで読む。SSAO 無効/正射/フォールバック時は
    // 白ダミー(1x1)で、Load は範囲外 0 を返して環境光を潰すため、シェーダ側で読まず ao=1 にする。
    fc.aoEnabled = (aoSrv != m_ssaoWhiteSrvIndex) ? 1.0f : 0.0f;

    // PointLight を ECS から収集
    fc.numPointLights = 0;
    {
        auto& reg = m_scene->GetRegistry();
        auto plView = reg.view<const dx12e::PointLight, const Transform>();
        for (auto [e, pl, tf] : plView.each())
        {
            if (fc.numPointLights >= kMaxPointLightsR) break;
            auto& pld = fc.pointLights[fc.numPointLights];
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMStoreFloat3(&pld.position, world.r[3]);
            pld.range = pl.range;
            pld.color = {pl.color.x * pl.intensity,
                         pl.color.y * pl.intensity,
                         pl.color.z * pl.intensity};

            // 影スロット割当（上で計算済みの m_pointShadowEntity[]）と突合
            pld.shadowIndex = -1.0f;
            for (u32 si = 0; si < m_numPointShadowSlots; ++si)
            {
                if (m_pointShadowEntity[si] == e) { pld.shadowIndex = static_cast<f32>(si); break; }
            }

            fc.numPointLights++;
        }
    }

    // SpotLight を ECS から収集（位置=Transform、軸=direction、内外コーン角を cos へ）
    fc.numSpotLights = 0;
    {
        auto& reg = m_scene->GetRegistry();
        auto slView = reg.view<const dx12e::SpotLight, const Transform>();
        for (auto [e, sl, tf] : slView.each())
        {
            if (fc.numSpotLights >= kMaxSpotLightsR) break;
            auto& sld = fc.spotLights[fc.numSpotLights];
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMStoreFloat3(&sld.position, world.r[3]);
            sld.range    = sl.range;

            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&sl.direction));
            XMStoreFloat3(&sld.direction, dir);

            // outer >= inner を保証してから cos 化（cos は単調減少なので inner の cos の方が大きい）
            float outerDeg = (std::max)(sl.outerConeDeg, sl.innerConeDeg);
            sld.cosInner = std::cos(XMConvertToRadians(sl.innerConeDeg));
            sld.cosOuter = std::cos(XMConvertToRadians(outerDeg));

            sld.color = {sl.color.x * sl.intensity,
                         sl.color.y * sl.intensity,
                         sl.color.z * sl.intensity};

            // 影スロット割当（上で計算済みの m_spotShadowEntity[]）と突合
            sld.shadowIndex = -1.0f;
            for (u32 si = 0; si < m_numSpotShadowSlots; ++si)
            {
                if (m_spotShadowEntity[si] == e) { sld.shadowIndex = static_cast<f32>(si); break; }
            }

            fc.numSpotLights++;
        }
    }

    // パーティクルライト: light=true の明るい粒子上位を、ポイントライトの空き枠へ注ぐ
    // （炎や魔法が実際に周囲を照らす。シーン配置のライトが優先）
    if (m_particleSystem && fc.numPointLights < kMaxPointLightsR)
    {
        ParticleSystem::LightInfo pls[kMaxPointLightsR];
        const u32 got = m_particleSystem->CollectLights(kMaxPointLightsR - fc.numPointLights, pls);
        for (u32 li = 0; li < got; ++li)
        {
            auto& pld = fc.pointLights[fc.numPointLights];
            pld.position = pls[li].pos;
            pld.range    = pls[li].range;
            pld.color    = pls[li].color;
            pld.shadowIndex = -1.0f;
            fc.numPointLights++;
        }
    }

    m_perFrameCB->Update(&fc, sizeof(fc), frameIndex);

    // ===== Skybox（不透明描画の前に全画面塗り。深度テスト OFF なので後続不透明が上書き）=====
    // skybox は自前 RootSig/PSO を bind するため、直後にメイン RootSig/PSO を再設定してから
    // per-frame CBV / shadow / IBL を bind し直す。
    if (m_iblReady && m_drawSkybox && m_skyboxIntensity > 0.0f && m_skyboxRenderer &&
        m_iblBaker && m_iblBaker->HasEnvironment() &&
        m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex)
    {
        XMFLOAT4X4 invVP;
        XMStoreFloat4x4(&invVP, XMMatrixTranspose(
            XMMatrixInverse(nullptr, m_camera->GetViewProjMatrix())));
        m_skyboxRenderer->Render(nativeCmdList, m_srvHeap->GetGpuHandle(m_envCubeSrvIndex),
                                 invVP, m_skyboxIntensity);
        // メイン RootSig / PSO を再設定
        m_commandList->SetRootSignature(*m_rootSignature);
        m_commandList->SetPipelineState(*m_pipelineState);
    }

    m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, m_perFrameCB->GetGpuAddress(frameIndex));

    // シャドウマップSRVをバインド
    m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
        m_srvHeap->GetGpuHandle(m_shadowSrvIndex));

    // スポット/ポイント影SRV(t9,t10)をバインド（連番確保なのでスポット側の1個渡しで2枚とも有効になる）
    m_commandList->SetSRVTable(RootSignature::kSlotPunctualShadowSRV,
        m_srvHeap->GetGpuHandle(m_spotShadowSrvIndex));

    // IBL テーブル(t5,t6,t7)をバインド（常に有効＝ダミー含む。hasIBL で読むか分岐）
    if (m_iblReady && m_iblBaker)
        m_commandList->SetSRVTable(RootSignature::kSlotIBLTable,
            m_srvHeap->GetGpuHandle(m_iblBaker->GetIrradianceSrv()));

    XMMATRIX viewProj = m_camera->GetViewProjMatrix();

    m_instanceCursor = 0;   // フレーム先頭で instancing バッファのカーソルをリセット（メイン→プレビューで連番追記）

    // 全Entityを描画（メインパス: 編集カメラ視点）。AO は SSAO 有効時のみ実テクスチャ、無効時は白。
    // useSSAO=true のときだけ深度プリパスで深度が完成済み → LESS_EQUAL forward PSO で再利用する。
    RenderSceneMeshes(nativeCmdList, frameIndex, viewProj,
                      (m_isGameMode || m_engineMode == EngineMode::Playing), aoSrv, useSSAO);

    // ---- Physics Debug Draw（オフスクリーン RT へ）----
    if (m_physicsDebugDraw && m_physicsDebugRenderer->IsEnabled())
    {
        m_physicsDebugRenderer->BeginFrame();
        m_physicsDebugRenderer->CollectFromRegistry(m_scene->GetRegistry());

        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, XMMatrixTranspose(m_camera->GetViewProjMatrix()));
        m_physicsDebugRenderer->Render(nativeCmdList, vp);
    }

    // ---- パーティクル（プロシージャル質感ビルボード）: HDR scene RT へ ----
    // エディタ編集中も描画する（配置エミッタ/トレイルの常時プレビュー。従来は Play/ゲームのみ）
    bool particleDistortDrawn = false;
    if (m_particleSystem)
    {
        // 深度を読み取り可能へ遷移し soft particles 用 SRV を供給。DSV はバインドせず PS で手動オクルージョン。
        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        auto srtv = m_sceneRT->GetRtv();
        nativeCmdList->OMSetRenderTargets(1, &srtv, FALSE, nullptr);
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());

        XMMATRIX invView = XMMatrixInverse(nullptr, m_camera->GetViewMatrix());
        XMFLOAT3 camRight, camUp, camPos;
        XMStoreFloat3(&camRight, invView.r[0]);
        XMStoreFloat3(&camUp,    invView.r[1]);
        XMStoreFloat3(&camPos,   invView.r[3]);

        XMFLOAT4X4 proj; XMStoreFloat4x4(&proj, m_camera->GetProjectionMatrix());
        const float rtw = static_cast<float>(m_sceneRT->GetWidth());
        const float rth = static_cast<float>(m_sceneRT->GetHeight());
        if (m_depthSrvIndex != DescriptorHeap::kInvalidIndex)
            m_particleSystem->SetSceneDepth(m_srvHeap->GetGpuHandle(m_depthSrvIndex),
                proj._33, proj._43, 1.0f / rtw, 1.0f / rth);
        else
            m_particleSystem->DisableSceneDepth();
        m_particleSystem->SetTime(totalTime);
        m_particleSystem->Render(nativeCmdList, m_camera->GetViewProjMatrix(), camRight, camUp, camPos);

        // ---- GPUパーティクル（compute シム + ExecuteIndirect）: 同じ HDR RT へ加算 ----
        if (m_gpuParticles)
        {
            if (m_depthSrvIndex != DescriptorHeap::kInvalidIndex)
                m_gpuParticles->SetSceneDepth(m_srvHeap->GetGpuHandle(m_depthSrvIndex),
                    proj._33, proj._43, 1.0f / rtw, 1.0f / rth);
            else
                m_gpuParticles->DisableSceneDepth();
            m_gpuParticles->SimulateAndRender(nativeCmdList, m_gameClock.GetDeltaTime(), totalTime,
                                              m_camera->GetViewProjMatrix(), camRight, camUp);
        }

        // ---- 歪みパーティクル（熱ゆらぎ/衝撃波）: 歪みバッファ(RG16F)へ ----
        // Render() と同一フレームのインスタンスバッファを共有するため直後に描く。
        if (m_particleSystem->HasDistortion() && m_distortRT)
        {
            m_distortRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float distClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            m_commandList->ClearRenderTarget(m_distortRT->GetRtv(), distClear);
            auto drtv = m_distortRT->GetRtv();
            nativeCmdList->OMSetRenderTargets(1, &drtv, FALSE, nullptr);
            m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
            m_particleSystem->RenderDistortion(nativeCmdList, m_camera->GetViewProjMatrix(),
                                               camRight, camUp);
            m_distortRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            particleDistortDrawn = true;
        }

        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    // ---- ワールド空間 2D スプライト（Sprite2D, worldSpace=true）: HDR scene RT へ ----
    // 各スプライトをエンティティのワールド行列で配置（3D 空間の任意位置/向き/スケール、billboard 可）。
    // layer 昇順ソート・アルファブレンド・深度テスト(書込みOFF)。PostProcess 前なのでブルーム等の対象。
    // ゲームビュー/Play に加え、エディタのシーンビュー(編集中)でも描画して配置を可視化する。
    if (m_spriteRenderer) m_spriteRenderer->BeginWorldVertexFrame();  // 本フレームの頂点書込みを先頭へ
    {
        XMMATRIX invView = XMMatrixInverse(nullptr, m_camera->GetViewMatrix());
        XMFLOAT3 camRight, camUp;
        XMStoreFloat3(&camRight, invView.r[0]);
        XMStoreFloat3(&camUp,    invView.r[1]);
        DrawWorldSprites(nativeCmdList, m_camera->GetViewProjMatrix(), camRight, camUp,
                         m_sceneRT->GetRtv(), m_dsvHandle, vpLeft, vpTop, vpW, vpH, totalTime);
    }

    // ===== ポストプロセス: オフスクリーン RT → バックバッファ =====
    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
    auto  rtv        = m_swapChain->GetCurrentRTV();

    // シーンRT は PS（ブルーム/uber）と CS（自動露出）の両方から読むので複合読取状態へ遷移
    m_sceneRT->Transition(*m_commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_commandList->TransitionResource(backBuffer,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    {
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());

        const f32 fullW = static_cast<f32>(m_sceneRT->GetWidth());
        const f32 fullH = static_cast<f32>(m_sceneRT->GetHeight());
        const f32 uvOfsX = static_cast<f32>(vpLeft) / fullW;
        const f32 uvOfsY = static_cast<f32>(vpTop)  / fullH;
        const f32 uvSclX = static_cast<f32>(vpW)    / fullW;
        const f32 uvSclY = static_cast<f32>(vpH)    / fullH;
        const auto sceneSrvGpu = m_srvHeap->GetGpuHandle(m_sceneRT->GetSrvIndex());

        // ポストエフェクトも Scene/Game で同じ設定を適用する。
        // ここを分けると「Scene では明るいのに Play すると暗い」など、ライティング調整が破綻する。
        PostProcessSettings ppApplied = m_scene->GetPostSettings();
        const bool isGameView = (m_isGameMode || m_engineMode == EngineMode::Playing);

        // ヒット時の画面インパクト（fx:pulse）: クロマ + 放射ブラーを瞬間的に上乗せ
        if (isGameView && m_particleSystem)
        {
            float pulse = m_particleSystem->GetPulse();
            if (pulse > 0.001f)
            {
                ppApplied.chromaticOn = true;
                ppApplied.chromatic   = (std::max)(ppApplied.chromatic, pulse * 1.2f);
                ppApplied.radialOn    = true;
                ppApplied.radial      = (std::max)(ppApplied.radial, pulse * 0.8f);
            }
        }

        // ---- 深度依存パス（DoF/モーションブラー/ゴッドレイ）の準備 ----
        // 透視カメラのみ（正射は CoC/再投影/太陽投影が破綻するため無効）
        const bool persp = !m_camera->IsOrthographic();
        const bool wantDepthPost = ppApplied.enabled && persp &&
            m_depthBuffer && m_depthSrvIndex != DescriptorHeap::kInvalidIndex &&
            (ppApplied.dofOn || ppApplied.motionBlurOn || ppApplied.godraysOn);
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu{};
        if (wantDepthPost)
        {
            m_commandList->TransitionResource(m_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            depthSrvGpu = m_srvHeap->GetGpuHandle(m_depthSrvIndex);
        }

        // ---- シーン変換チェーン: DoF → モーションブラー（結果を以降の「シーン」として使う）----
        D3D12_GPU_DESCRIPTOR_HANDLE curSceneSrv = sceneSrvGpu;
        if (wantDepthPost && ppApplied.dofOn && m_dofPass)
        {
            XMFLOAT4X4 projF;
            XMStoreFloat4x4(&projF, m_camera->GetProjectionMatrix());
            const u32 o = m_dofPass->Apply(*m_commandList, m_srvHeap.get(),
                curSceneSrv, depthSrvGpu,
                uvOfsX, uvOfsY, uvSclX, uvSclY, vpLeft, vpTop, vpW, vpH,
                projF._33, projF._43, ppApplied);
            if (o != DescriptorHeap::kInvalidIndex)
                curSceneSrv = m_srvHeap->GetGpuHandle(o);
        }
        if (wantDepthPost && ppApplied.motionBlurOn && m_motionBlurPass && m_prevViewProjValid)
        {
            const XMMATRIX vp  = m_camera->GetViewProjMatrix();
            const XMMATRIX inv = XMMatrixInverse(nullptr, vp);
            XMFLOAT4X4 invT, prevT;
            XMStoreFloat4x4(&invT,  XMMatrixTranspose(inv));
            XMStoreFloat4x4(&prevT, XMMatrixTranspose(XMLoadFloat4x4(&m_prevViewProj)));
            const u32 o = m_motionBlurPass->Apply(*m_commandList, m_srvHeap.get(),
                curSceneSrv, depthSrvGpu, invT, prevT,
                uvOfsX, uvOfsY, uvSclX, uvSclY, vpLeft, vpTop, vpW, vpH, ppApplied);
            if (o != DescriptorHeap::kInvalidIndex)
                curSceneSrv = m_srvHeap->GetGpuHandle(o);
        }

        // ---- 自動露出（compute。ビューポート矩形のヒストグラム→露出値を GPU 内バッファへ）----
        if (m_autoExposure && ppApplied.enabled && ppApplied.autoExposureOn)
            m_autoExposure->Generate(nativeCmdList, curSceneSrv, vpLeft, vpTop, vpW, vpH,
                                     m_gameClock.GetDeltaTime(), ppApplied);
        D3D12_GPU_VIRTUAL_ADDRESS exposureVA = 0;
        if (m_autoExposure)
        {
            m_autoExposure->EnsureReadable(nativeCmdList);
            exposureVA = m_autoExposure->GetExposureBufferVA();
        }

        // ---- ブルーム（レンズフレアの入力も兼ねる。内部で RT/ビューポート切替）----
        u32 bloomSrv = DescriptorHeap::kInvalidIndex;
        if (m_bloomPass && ppApplied.enabled && (ppApplied.bloomOn || ppApplied.lensflareOn))
            bloomSrv = m_bloomPass->Generate(*m_commandList, m_srvHeap.get(), curSceneSrv,
                                             uvOfsX, uvOfsY, uvSclX, uvSclY,
                                             1.0f / fullW, 1.0f / fullH, ppApplied);
        const bool bloomReady = (bloomSrv != DescriptorHeap::kInvalidIndex);
        const auto bloomSrvGpu = m_srvHeap->GetGpuHandle(bloomReady ? bloomSrv : m_ssaoWhiteSrvIndex);

        // ---- ゴッドレイ（太陽=最初の平行光源をスクリーンへ投影）----
        u32 godraysSrv = DescriptorHeap::kInvalidIndex;
        if (wantDepthPost && ppApplied.godraysOn && m_godRaysPass)
        {
            XMFLOAT3 sunDir{}; XMFLOAT3 sunColI{}; bool hasSun = false;
            auto& greg = m_scene->GetRegistry();
            auto dlView = greg.view<DirectionalLight>();
            if (dlView.begin() != dlView.end())
            {
                const auto& dl = dlView.get<DirectionalLight>(*dlView.begin());
                sunDir  = dl.direction;
                sunColI = XMFLOAT3(dl.color.x * dl.intensity,
                                   dl.color.y * dl.intensity,
                                   dl.color.z * dl.intensity);
                hasSun = true;
            }
            if (hasSun)
            {
                // 太陽ワールド位置 ≒ カメラ位置 - 光方向×遠距離 → スクリーン投影
                const XMFLOAT3 camPos = m_camera->GetPosition();
                XMVECTOR d  = XMVector3Normalize(XMLoadFloat3(&sunDir));
                XMVECTOR wp = XMVectorSubtract(XMLoadFloat3(&camPos), XMVectorScale(d, 5000.0f));
                XMVECTOR clip = XMVector4Transform(XMVectorSetW(wp, 1.0f), m_camera->GetViewProjMatrix());
                const f32 cw = XMVectorGetW(clip);
                if (cw > 0.01f)
                {
                    const f32 lu = XMVectorGetX(clip) / cw * 0.5f + 0.5f;   // ローカルUV
                    const f32 lv = 0.5f - XMVectorGetY(clip) / cw * 0.5f;
                    // 画面中心からの距離でフェード（画面外に離れると消える）
                    const f32 dc = std::sqrt((lu - 0.5f) * (lu - 0.5f) + (lv - 0.5f) * (lv - 0.5f));
                    const f32 fade = (std::min)((std::max)((1.1f - dc) / 0.4f, 0.0f), 1.0f);
                    if (fade > 0.001f)
                    {
                        godraysSrv = m_godRaysPass->Generate(*m_commandList, m_srvHeap.get(),
                            depthSrvGpu,
                            uvOfsX, uvOfsY, uvSclX, uvSclY, vpLeft, vpTop, vpW, vpH,
                            lu * uvSclX + uvOfsX, lv * uvSclY + uvOfsY, fade, sunColI, ppApplied);
                    }
                }
            }
        }
        const bool grReady = (godraysSrv != DescriptorHeap::kInvalidIndex);

        // ---- レンズフレア（ブルームチェーンの縮小ミップから生成）----
        u32 flareSrv = DescriptorHeap::kInvalidIndex;
        if (m_lensFlarePass && ppApplied.enabled && ppApplied.lensflareOn && bloomReady)
        {
            const u32 mip = m_bloomPass->GetMipSrvIndex(1);
            if (mip != DescriptorHeap::kInvalidIndex)
                flareSrv = m_lensFlarePass->Generate(*m_commandList, m_srvHeap.get(),
                    m_srvHeap->GetGpuHandle(mip), ppApplied);
        }
        const bool lfReady = (flareSrv != DescriptorHeap::kInvalidIndex);

        // 深度を DSV 用途（エディタアイコン等）へ戻す
        if (wantDepthPost)
            m_commandList->TransitionResource(m_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // ---- 3D LUT（assets 相対パス。sRGB 無効=バイト列そのままロード。ストリップ形式 N*N x N）----
        auto lutSrvGpu = m_srvHeap->GetGpuHandle(m_ssaoWhiteSrvIndex);
        f32  lutSize   = 0.0f;
        if (ppApplied.enabled && ppApplied.lutOn && !ppApplied.lutPath.empty() && m_resourceManager)
        {
            const std::string lutAbs = PathResolver::AssetsDir() + ppApplied.lutPath;
            if (Texture* lut = m_resourceManager->GetOrLoadTexture(
                    PathResolver::Utf8ToWide(lutAbs), nativeCmdList, /*srgb=*/false))
            {
                if (lut->GetHeight() >= 2 &&
                    lut->GetWidth() == lut->GetHeight() * lut->GetHeight())
                {
                    lutSrvGpu = m_srvHeap->GetGpuHandle(lut->GetSrvIndex());
                    lutSize   = static_cast<f32>(lut->GetHeight());
                }
            }
        }

        // ---- 最終(uber)パス: バックバッファへ ----
        constexpr float bbClear[4] = {0.05f, 0.05f, 0.06f, 1.0f};
        m_commandList->ClearRenderTarget(rtv, bbClear);
        nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);  // 深度なし
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

        const auto whiteDummy = m_srvHeap->GetGpuHandle(m_ssaoWhiteSrvIndex);
        PostProcess::Inputs pin{};
        pin.sceneSrv     = curSceneSrv;
        pin.bloomSrv     = bloomSrvGpu;
        pin.lutSrv       = lutSrvGpu;
        pin.godraysSrv   = grReady ? m_srvHeap->GetGpuHandle(godraysSrv) : whiteDummy;
        pin.flareSrv     = lfReady ? m_srvHeap->GetGpuHandle(flareSrv)   : whiteDummy;
        pin.distortSrv   = (particleDistortDrawn && m_distortRT)
                         ? m_srvHeap->GetGpuHandle(m_distortRT->GetSrvIndex()) : whiteDummy;
        pin.lutSize      = lutSize;
        pin.exposureVA   = exposureVA;
        pin.bloomReady   = bloomReady;
        pin.godraysReady = grReady;
        pin.flareReady   = lfReady;
        pin.distortReady = particleDistortDrawn;

        m_postProcess->Apply(nativeCmdList, pin, ppApplied,
            uvOfsX, uvOfsY, uvSclX, uvSclY,
            1.0f / fullW, 1.0f / fullH, totalTime, frameIndex);

        // 次フレームのモーションブラー用に今フレームの viewProj を保存
        XMStoreFloat4x4(&m_prevViewProj, m_camera->GetViewProjMatrix());
        m_prevViewProjValid = true;
    }

    // ---- Editor Icon Draw（ポスト後のバックバッファへ, エディタモードのみ）----
    if (m_engineMode == EngineMode::Editor && !m_isGameMode)
    {
        m_commandList->SetRenderTarget(rtv, m_dsvHandle);
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

        m_editorIconRenderer->BeginFrame();
        m_editorIconRenderer->CollectFromRegistry(m_scene->GetRegistry(), *m_editorCtx);

        XMFLOAT4X4 vpIcon;
        XMStoreFloat4x4(&vpIcon, XMMatrixTranspose(m_camera->GetViewProjMatrix()));
        m_editorIconRenderer->Render(nativeCmdList, vpIcon, vpW, vpH);
    }

    // ---- 2D スプライト / ゲーム内 UI 画像（バックバッファ全面へ）----
    if (m_spriteRenderer && (m_isGameMode || m_engineMode == EngineMode::Playing))
    {
        m_spriteRenderer->BeginFrame();
        // Lua の ui:image() コマンドをテクスチャ読み込み＋サブミット
        for (const auto& c : m_uiCommands)
        {
            if (c.type != UICommand::Type::Image || c.text.empty()) continue;
            std::wstring wpath = PathResolver::Utf8ToWide(c.text);
            Texture* tex = m_resourceManager->GetOrLoadTexture(wpath, nativeCmdList);
            if (!tex) continue;
            SpriteDesc s;
            s.pos      = {c.x, c.y};
            s.size     = {c.w, c.h};
            s.color    = {c.r, c.g, c.b, c.a};
            s.srvIndex = tex->GetSrvIndex();
            m_spriteRenderer->Submit(s);
        }

        // Sprite2D(worldSpace=false): HUD として画面ピクセル座標で描く。
        // 位置は Transform の並進 x/y を「画面ピクセル中心」とみなす（ワールド側と同じく中心基準で
        // size を展開）。Transform が無ければ画面左上(0,0)中心。layer 昇順は Render 内でソート。
        if (m_scene)
        {
            auto& sreg = m_scene->GetRegistry();
            for (auto [e, sp] : sreg.view<const Sprite2D>().each())
            {
                if (sp.worldSpace || sp.texturePath.empty()) continue;
                const std::string absPath = PathResolver::AssetsDir() + sp.texturePath;
                std::wstring wpath = PathResolver::Utf8ToWide(absPath);
                Texture* tex = m_resourceManager->GetOrLoadTexture(wpath, nativeCmdList);
                if (!tex) continue;
                float cx = 0.0f, cy = 0.0f;
                if (sreg.all_of<Transform>(e))
                {
                    XMFLOAT3 wp; XMStoreFloat3(&wp, ComputeWorldMatrix(sreg, e).r[3]);
                    cx = wp.x; cy = wp.y;
                }
                SpriteDesc s;
                s.pos      = {cx - sp.size.x * 0.5f, cy - sp.size.y * 0.5f};
                s.size     = sp.size;
                s.uvMin    = sp.uvMin;
                s.uvMax    = sp.uvMax;
                s.color    = sp.color;
                s.srvIndex = tex->GetSrvIndex();
                s.layer    = static_cast<float>(sp.layer);
                m_spriteRenderer->Submit(s);
            }
        }

        if (m_spriteRenderer->HasAny())
        {
            nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            // ゲームビューポート矩形に合わせる（エディタ Play の 16:9 中央矩形でも UI 画像が
            // テキスト/矩形と同じ座標系になり、ずれない）。
            m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
            m_spriteRenderer->Render(nativeCmdList, vpW, vpH);
        }
    }

    // ===== カメラプレビュー（選択カメラ視点を小窓へ）=====
    // 選択中エンティティにカメラがあれば、その視点でシーンを専用 RT に再描画。
    // EditorLayer がシーンビュー隅に小窓表示する（Play を押さずに見える）。
    m_editorCtx->cameraPreviewTexHandle = 0;
    if (m_engineMode == EngineMode::Editor && !m_isGameMode && m_cameraPreviewRT)
    {
        auto& reg = m_scene->GetRegistry();
        entt::entity camEnt = entt::null;
        for (auto e : m_editorCtx->selectedEntities)
            if (reg.valid(e) && reg.all_of<CameraComponent, Transform>(e)) { camEnt = e; break; }

        if (camEnt != entt::null)
        {
            const auto& tf  = reg.get<Transform>(camEnt);
            const auto& cam = reg.get<CameraComponent>(camEnt);

            // 回転（quat）からビュー行列を構築（カメラアイコン/フラスタムと同じ向き）
            XMFLOAT4 q;
            if (tf.useQuaternion)
                q = tf.quaternion;
            else
                XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(
                    XMConvertToRadians(tf.rotation.x),
                    XMConvertToRadians(tf.rotation.y),
                    XMConvertToRadians(tf.rotation.z)));
            XMMATRIX rot  = XMMatrixRotationQuaternion(XMLoadFloat4(&q));
            XMVECTOR eye  = XMLoadFloat3(&tf.position);
            XMMATRIX view = XMMatrixLookToLH(eye, rot.r[2], rot.r[1]);

            const u32 pw = m_cameraPreviewRT->GetWidth();
            const u32 ph = m_cameraPreviewRT->GetHeight();
            const f32 paspect = static_cast<f32>(pw) / static_cast<f32>(ph);
            // 正射カメラはプレビューも正射投影に（ゲームの SetOrthographic と同じ: 縦=2*orthoSize）。
            XMMATRIX proj = (cam.projection == CameraProjection::Orthographic)
                ? XMMatrixOrthographicLH(2.0f * cam.orthoSize * paspect, 2.0f * cam.orthoSize,
                                         cam.nearClip, cam.farClip)
                : XMMatrixPerspectiveFovLH(XMConvertToRadians(cam.fovDegrees), paspect,
                                           cam.nearClip, cam.farClip);
            XMMATRIX camViewProj = view * proj;

            // メインパスの fc（ライト等）を流用し、視点だけ差し替えて専用 CB へ
            FrameConstants fcp = fc;
            XMStoreFloat4x4(&fcp.view, XMMatrixTranspose(view));
            XMStoreFloat4x4(&fcp.proj, XMMatrixTranspose(proj));
            fcp.cameraPos = tf.position;
            fcp.aoEnabled = 0.0f;   // プレビューは白ダミー AO（SSAO 非対応）なので AO を読まない
            m_previewFrameCB->Update(&fcp, sizeof(fcp), frameIndex);

            m_cameraPreviewRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float pvClear[4] = {0.127f, 0.306f, 0.850f, 1.0f};  // リニア空間のコーンフラワーブルー
            m_commandList->ClearRenderTarget(m_cameraPreviewRT->GetRtv(), pvClear);
            m_commandList->ClearDepthStencil(m_dsvHandle);
            m_commandList->SetRenderTarget(m_cameraPreviewRT->GetRtv(), m_dsvHandle);
            m_commandList->SetViewportAndScissor(pw, ph);

            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
            m_commandList->SetRootSignature(*m_rootSignature);
            m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame,
                m_previewFrameCB->GetGpuAddress(frameIndex));
            m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
                m_srvHeap->GetGpuHandle(m_shadowSrvIndex));
            if (m_iblReady && m_iblBaker)
                m_commandList->SetSRVTable(RootSignature::kSlotIBLTable,
                    m_srvHeap->GetGpuHandle(m_iblBaker->GetIrradianceSrv()));

            // グリッドは出さない＝isGameView=true。プレビューは SSAO 非対応＝白ダミー。
            RenderSceneMeshes(nativeCmdList, frameIndex, camViewProj, true, m_ssaoWhiteSrvIndex);

            // ワールド空間スプライトもプレビューへ（このカメラ視点で。ビルボードは行列の右/上ベクトル）。
            XMFLOAT3 pvRight, pvUp;
            XMStoreFloat3(&pvRight, rot.r[0]);
            XMStoreFloat3(&pvUp,    rot.r[1]);
            DrawWorldSprites(nativeCmdList, camViewProj, pvRight, pvUp,
                             m_cameraPreviewRT->GetRtv(), m_dsvHandle, 0u, 0u, pw, ph, totalTime);

            m_cameraPreviewRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            // プレビューRT(リニアHDR)をトーンマップして LDR RT へ解決する。
            // ImGui へ FP16 の SRV を直接渡すとトーンマップ/ガンマ無しで暗く表示されるため。
            // enabled=false → mask=0 = PostProcess はトーンマップ+ガンマのみ適用。
            if (m_cameraPreviewLdrRT && m_postProcess && m_postProcess->IsReady())
            {
                m_cameraPreviewLdrRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
                D3D12_CPU_DESCRIPTOR_HANDLE ldrRtv = m_cameraPreviewLdrRT->GetRtv();
                nativeCmdList->OMSetRenderTargets(1, &ldrRtv, FALSE, nullptr);  // 深度なし
                m_commandList->SetViewportAndScissor(pw, ph);

                PostProcessSettings pvPost{};
                pvPost.enabled = false;
                // トーンマッパはシーン設定と揃える（プレビューと本画面の見た目一致）
                pvPost.tonemapper = m_scene->GetPostSettings().tonemapper;
                const auto pvDummy = m_srvHeap->GetGpuHandle(m_ssaoWhiteSrvIndex);
                PostProcess::Inputs pvIn{};
                pvIn.sceneSrv   = m_srvHeap->GetGpuHandle(m_cameraPreviewRT->GetSrvIndex());
                pvIn.bloomSrv   = pvDummy;
                pvIn.lutSrv     = pvDummy;
                pvIn.godraysSrv = pvDummy;
                pvIn.flareSrv   = pvDummy;
                pvIn.distortSrv = pvDummy;
                pvIn.exposureVA = m_autoExposure ? m_autoExposure->GetExposureBufferVA() : 0;
                m_postProcess->Apply(nativeCmdList, pvIn, pvPost,
                    0.0f, 0.0f, 1.0f, 1.0f,
                    1.0f / static_cast<f32>(pw), 1.0f / static_cast<f32>(ph), totalTime, frameIndex);

                m_cameraPreviewLdrRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_editorCtx->cameraPreviewTexHandle =
                    m_srvHeap->GetGpuHandle(m_cameraPreviewLdrRT->GetSrvIndex()).ptr;
            }
            else
            {
                m_editorCtx->cameraPreviewTexHandle =
                    m_srvHeap->GetGpuHandle(m_cameraPreviewRT->GetSrvIndex()).ptr;
            }

            // プレビュー描画でRT/ビューポートを切り替えたので、バックバッファへ戻す。
            // これをしないと直後の ImGui がプレビューRTへ描かれ、画面に出なくなる。
            m_commandList->SetRenderTarget(rtv, m_dsvHandle);
            m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
        }
    }

    // ===== パーティクルエディタのプレビュー（専用オフスクリーンRT。UI本体は後段のImGuiパスで描く）=====
    if (m_vfxEditorPanel && m_editorCtx->showVfxEditor)
    {
        m_vfxEditorPanel->RenderPreview3D(*m_editorCtx, *m_commandList, m_gameClock.GetDeltaTime());
        // プレビュー描画でRT/ビューポートを切り替えたので、バックバッファへ戻す。
        m_commandList->SetRenderTarget(rtv, m_dsvHandle);
        m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
    }

    // ---- ImGui フレーム ----
    m_imguiManager->BeginFrame();
    ImGuizmo::BeginFrame();

    // 版が変わった初回起動だけ「更新内容」モーダルを最前面に出す（ランチャー/エディタの上）。
    RenderWhatsNewPopup();

    if (!m_isGameMode && m_loading)
    {
        // ---- ローディングオーバーレイ（プロジェクト作成/読込中）----
        RenderLoadingOverlay();
    }
    else if (!m_isGameMode && m_showLauncher)
    {
        // ---- プロジェクトランチャー（起動直後 / 「ランチャーに戻る」選択時）----
        LauncherIcons li;
        li.logo        = m_icons.logo;
        li.newProject  = m_icons.newProject;
        li.openProject = m_icons.openProject;
        li.recent      = m_icons.recent;
        li.tmplFps     = m_icons.tmplFps;
        li.tmplTps     = m_icons.tmplTps;
        li.tmpl2d      = m_icons.tmpl2d;
        li.tmplEmpty   = m_icons.tmplEmpty;

        ProjectInfo selected;
        LauncherAction action = ProjectManager::RenderLauncher(selected, m_window->GetHwnd(), li);
        if (action == LauncherAction::CreateNew)
            BeginProjectLoad(selected, /*isNew=*/true);
        else if (action == LauncherAction::OpenExisting)
            BeginProjectLoad(selected, /*isNew=*/false);
        else if (action == LauncherAction::Skip)
        {
            LoadProject(selected);
            m_showLauncher = false;
        }
    }
    else if (!m_isGameMode)
    {
        bool pendingPlayMode = false;
        m_editorLayer->Render(
            m_engineMode == EngineMode::Playing,
            m_scene.get(), m_camera.get(), m_window.get(),
            m_scriptEngine.get(), m_audioSystem.get(),
            m_physicsDebugRenderer.get(), m_physicsDebugDraw,
            m_useVsync, m_shadowQualityIndex, m_shadowMapSize,
            m_shadowMapDirty, m_cascadeSplitLambda, m_cascadeBlendBand,
            m_showCascadeDebug, &m_gameClock,
            m_modeChangeRequested, pendingPlayMode,
            PathResolver::AssetsDir(), kLeftPanelWidth, kToolbarHeight);

        if (m_modeChangeRequested)
            m_pendingMode = pendingPlayMode ? EngineMode::Playing : EngineMode::Editor;

        // ---- ポストプロセス: ON/OFF ウィンドウ と パラメータ ウィンドウ ----
        {
            auto& pp = m_scene->GetPostSettings();

            // 全エフェクトのメタ情報（トグルとパラメータ描画を一元定義）
            struct PostFx {
                const char* cat;                 // カテゴリ見出し
                const char* label;               // 表示名
                const char* help;                // 説明（null可）
                bool*       on;                  // 有効フラグ
                std::function<void()> params;    // パラメータ描画
            };
            const std::vector<PostFx> fx = {
                {"カラー", "露出 Exposure", "明るさを乗算で調整", &pp.exposureOn,
                    [&]{ ImGui::SliderFloat("値##exposure", &pp.exposure, 0.1f, 4.0f, "%.2f"); }},
                {"カラー", "自動露出 Auto Exposure", "平均輝度に合わせて露出を自動追従（目の順応）", &pp.autoExposureOn,
                    [&]{ ImGui::SliderFloat("適応速度##aespd", &pp.aeSpeed, 0.1f, 10.0f, "%.1f");
                         ImGui::SliderFloat("EV補正##aeev", &pp.aeEvComp, -4.0f, 4.0f, "%.1f"); }},
                {"カラー", "コントラスト Contrast", nullptr, &pp.contrastOn,
                    [&]{ ImGui::SliderFloat("値##contrast", &pp.contrast, 0.0f, 2.0f, "%.2f"); }},
                {"カラー", "明るさ Brightness", "加算で明暗を調整", &pp.brightnessOn,
                    [&]{ ImGui::SliderFloat("値##brightness", &pp.brightness, -0.5f, 0.5f, "%.2f"); }},
                {"カラー", "彩度 Saturation", nullptr, &pp.saturationOn,
                    [&]{ ImGui::SliderFloat("値##saturation", &pp.saturation, 0.0f, 2.0f, "%.2f"); }},
                {"カラー", "色温度 Warmth", "+で暖色、-で寒色", &pp.warmthOn,
                    [&]{ ImGui::SliderFloat("値##warmth", &pp.warmth, -1.0f, 1.0f, "%.2f"); }},
                {"カラー", "色相回転 Hue", "色相を回す（度）", &pp.hueOn,
                    [&]{ ImGui::SliderFloat("角度##hue", &pp.hueShift, 0.0f, 360.0f, "%.0f°"); }},
                {"カラー", "色味 Tint", "RGB を乗算", &pp.tintOn,
                    [&]{ ImGui::ColorEdit3("色##tint", &pp.tint.x); }},

                {"ブルーム/ビネット", "ブルーム Bloom", "明部が咲く（物理ベース・ダウンサンプルチェーン）", &pp.bloomOn,
                    [&]{ ImGui::SliderFloat("強度##bloom", &pp.bloom, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderFloat("しきい値##bloomth", &pp.bloomThreshold, 0.0f, 4.0f, "%.2f");
                         ImGui::SliderFloat("ニー(肩)##bloomknee", &pp.bloomKnee, 0.0f, 1.0f, "%.2f");
                         ImGui::SliderFloat("広がり##bloomrad", &pp.bloomRadius, 0.05f, 0.95f, "%.2f"); }},
                {"ブルーム/ビネット", "ビネット Vignette", "周辺減光", &pp.vignetteOn,
                    [&]{ ImGui::SliderFloat("強度##vig", &pp.vignette, 0.0f, 1.0f, "%.2f"); }},

                {"ライト/カメラ", "ゴッドレイ God Rays", "太陽(平行光源)からの光条。太陽が画面内/近くにある時に見える(透視カメラのみ)", &pp.godraysOn,
                    [&]{ ImGui::SliderFloat("強度##gri", &pp.grIntensity, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderFloat("長さ##grd", &pp.grDensity, 0.1f, 1.0f, "%.2f");
                         ImGui::SliderFloat("減衰##grdc", &pp.grDecay, 0.8f, 0.999f, "%.3f"); }},
                {"ライト/カメラ", "レンズフレア Lens Flare", "ゴースト+ハロー。強い光源があると出る(ブルームと入力共有)", &pp.lensflareOn,
                    [&]{ ImGui::SliderFloat("強度##lfi", &pp.lfIntensity, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderInt("ゴースト数##lfg", &pp.lfGhosts, 1, 8);
                         ImGui::SliderFloat("間隔##lfd", &pp.lfDispersal, 0.05f, 1.0f, "%.2f");
                         ImGui::SliderFloat("ハロー##lfh", &pp.lfHalo, 0.0f, 1.0f, "%.2f");
                         ImGui::SliderFloat("色収差##lfc", &pp.lfChroma, 0.0f, 0.05f, "%.3f"); }},
                {"ライト/カメラ", "被写界深度 DoF", "フォーカス距離の前後がボケる(透視カメラのみ)", &pp.dofOn,
                    [&]{ ImGui::SliderFloat("フォーカス距離##doff", &pp.dofFocusDist, 0.1f, 100.0f, "%.1f");
                         ImGui::SliderFloat("シャープ範囲##dofr", &pp.dofFocusRange, 0.1f, 50.0f, "%.1f");
                         ImGui::SliderFloat("最大ボケpx##dofb", &pp.dofBlurSize, 1.0f, 32.0f, "%.0f"); }},
                {"ライト/カメラ", "モーションブラー Motion Blur", "カメラの動きで残像(深度再構成方式・透視カメラのみ)", &pp.motionBlurOn,
                    [&]{ ImGui::SliderFloat("強度##mbs", &pp.mbStrength, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderInt("サンプル数##mbn", &pp.mbSamples, 4, 16); }},

                {"スタイライズ", "色収差 Chromatic", "画面端でRGBがズレる", &pp.chromaticOn,
                    [&]{ ImGui::SliderFloat("強度##chroma", &pp.chromatic, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "ピクセル化 Pixelize", "ブロック状にモザイク", &pp.pixelizeOn,
                    [&]{ ImGui::SliderFloat("ブロックpx##pix", &pp.pixelSize, 1.0f, 64.0f, "%.0f"); }},
                {"スタイライズ", "ポスタライズ Posterize", "色数を段階化", &pp.posterizeOn,
                    [&]{ ImGui::SliderInt("階調##post", &pp.posterize, 2, 16); }},
                {"スタイライズ", "ディザ Dither", "順序ディザで階調化", &pp.ditherOn,
                    [&]{ ImGui::SliderInt("階調##dither", &pp.ditherLevels, 2, 8); }},
                {"スタイライズ", "CRT走査線 Scanline", "走査線＋画面湾曲", &pp.scanlineOn,
                    [&]{ ImGui::SliderFloat("強度##scan", &pp.scanline, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "シャープ Sharpen", "輪郭を強調", &pp.sharpenOn,
                    [&]{ ImGui::SliderFloat("強度##sharp", &pp.sharpen, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "フィルムグレイン Grain", "ザラつきノイズ", &pp.grainOn,
                    [&]{ ImGui::SliderFloat("強度##grain", &pp.grain, 0.0f, 1.0f, "%.2f"); }},

                {"カラー操作", "色反転 Invert", nullptr, &pp.invertOn,
                    [&]{ ImGui::SliderFloat("強度##inv", &pp.invert, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "セピア Sepia", nullptr, &pp.sepiaOn,
                    [&]{ ImGui::SliderFloat("強度##sepia", &pp.sepia, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "グレースケール Grayscale", nullptr, &pp.grayscaleOn,
                    [&]{ ImGui::SliderFloat("強度##gray", &pp.grayscale, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "LUT グレーディング", "ストリップ画像(N*N x N, 例:1024x32)で色変換。Photoshop等で作った LUT を適用", &pp.lutOn,
                    [&]{ static char lutBuf[260] = "";
                         ImGui::InputTextWithHint("##lutpath", "assets からの相対パス (例: luts/warm.png)", lutBuf, sizeof(lutBuf));
                         if (ImGui::IsItemDeactivatedAfterEdit()) pp.lutPath = lutBuf;
                         if (!ImGui::IsItemActive() && pp.lutPath != lutBuf)
                         {
                             size_t n = pp.lutPath.size();
                             if (n >= sizeof(lutBuf)) n = sizeof(lutBuf) - 1;
                             std::memcpy(lutBuf, pp.lutPath.c_str(), n);
                             lutBuf[n] = '\0';
                         }
                         ImGui::SliderFloat("適用量##lutamt", &pp.lutAmount, 0.0f, 1.0f, "%.2f"); }},

                {"歪み", "レンズ歪み Lens", "バレル/魚眼", &pp.lensOn,
                    [&]{ ImGui::SliderFloat("強度##lens", &pp.lens, -1.0f, 1.0f, "%.2f"); }},
                {"歪み", "波ゆらぎ Wave", "水中/陽炎のゆれ", &pp.waveOn,
                    [&]{ ImGui::SliderFloat("振幅##wamp", &pp.waveAmp, 0.0f, 0.05f, "%.3f");
                         ImGui::SliderFloat("周波数##wfreq", &pp.waveFreq, 1.0f, 40.0f, "%.1f");
                         ImGui::SliderFloat("速度##wspd", &pp.waveSpeed, 0.0f, 8.0f, "%.1f"); }},
                {"歪み", "放射ブラー Radial", "中心へズームブラー", &pp.radialOn,
                    [&]{ ImGui::SliderFloat("強度##rad", &pp.radial, 0.0f, 1.0f, "%.2f"); }},
                {"歪み", "グリッチ Glitch", "デジタル乱れ", &pp.glitchOn,
                    [&]{ ImGui::SliderFloat("強度##glitch", &pp.glitch, 0.0f, 1.0f, "%.2f"); }},

                {"輪郭", "輪郭線 Outline", "Sobelエッジ検出", &pp.outlineOn,
                    [&]{ ImGui::SliderFloat("強度##outl", &pp.outline, 0.0f, 4.0f, "%.2f");
                         ImGui::ColorEdit3("線の色##outlc", &pp.outlineColor.x); }},

                {"アンチエイリアス", "FXAA", "簡易アンチエイリアス", &pp.fxaaOn, {}},

                {"仕上げ", "デバンディング Deband", "TPDFディザで空/ビネットの縞(バンディング)を除去", &pp.debandOn, {}},
            };

            // 有効中エフェクト数（両窓で使うので、窓の表示有無に関わらず先に数える）
            int enabledCount = 0;
            for (const auto& fEff : fx)
                if (*fEff.on) ++enabledCount;

            // ===== ウィンドウ1: ON/OFF チェックリスト（ツール窓・トグル表示）=====
            if (m_editorCtx->showPostProcess)
            {
            ImGui::Begin("Post Process");
            ImGui::Checkbox("有効（マスター）", &pp.enabled);
            // トーンマップ（表示変換）はマスターOFF でも常に適用されるのでディセーブル外
            ImGui::SetNextItemWidth(200.0f);
            ImGui::Combo("トーンマップ", &pp.tonemapper, "ACES\0AgX\0なし(ガンマのみ)\0");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted("ACES: コントラスト強めの定番\nAgX: 高輝度・高彩度光源(ネオン/発光体)の色割れがない\nなし: ガンマのみ(デバッグ/2D向け)");
                ImGui::EndTooltip();
            }
            ImGui::TextDisabled("SceneビューとGameビューへ同じ見た目を適用します / パラメータは「Post Process パラメータ」窓で");
            ImGui::Separator();

            ImGui::BeginDisabled(!pp.enabled);
            const char* curCat = nullptr;
            for (const auto& f : fx)
            {
                if (curCat == nullptr || std::strcmp(curCat, f.cat) != 0)
                {
                    curCat = f.cat;
                    ImGui::SeparatorText(curCat);
                }
                ImGui::Checkbox(f.label, f.on);
                if (f.help)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::BeginItemTooltip())
                    { ImGui::TextUnformatted(f.help); ImGui::EndTooltip(); }
                }
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::Button("すべてOFF"))
                for (const auto& f : fx) *f.on = false;
            ImGui::SameLine();
            if (ImGui::Button("初期値に戻す"))
                pp = PostProcessSettings{};
            ImGui::SameLine();
            ImGui::TextDisabled("有効中: %d", enabledCount);
            ImGui::End();
            } // if showPostProcess

            // ===== ウィンドウ2: 有効なエフェクトのパラメータ（ツール窓・トグル表示）=====
            if (m_editorCtx->showPostParams)
            {
            ImGui::Begin("Post Process パラメータ");
            if (!pp.enabled)
                ImGui::TextDisabled("マスターが OFF です（Post Process 窓で有効化）");
            else if (enabledCount == 0)
                ImGui::TextDisabled("エフェクトを有効にすると、ここに調整項目が出ます");
            else
            {
                ImGui::PushItemWidth(-120.0f);
                for (const auto& f : fx)
                {
                    if (!*f.on || !f.params) continue;
                    ImGui::SeparatorText(f.label);
                    ImGui::PushID(f.label);
                    f.params();
                    ImGui::PopID();
                }
                ImGui::PopItemWidth();
            }
            ImGui::End();
            } // if showPostParams
        }

        // ---- Skybox / IBL 設定ウィンドウ（シーン単位の環境マップ・トグル表示）----
        if (m_scene && m_editorCtx->showSkybox)
        {
            auto& sk = m_scene->GetSkyboxSettings();
            ImGui::Begin("Skybox / IBL");
            ImGui::TextWrapped("環境キューブ(.dds, TEXTURECUBE) から irradiance / prefiltered / BRDF LUT を生成し、"
                               "ambient を IBL 化する。空欄なら従来 ambient。");
            ImGui::Separator();

            // env map パス入力
            static char pathBuf[260];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", sk.envMapPath.c_str());
            if (ImGui::InputText("Env Map (.dds, assets相対)", pathBuf, sizeof(pathBuf)))
                sk.envMapPath = pathBuf;

            ImGui::SliderFloat("IBL Intensity", &sk.iblIntensity, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Skybox Intensity", &sk.skyboxIntensity, 0.0f, 3.0f, "%.2f");
            ImGui::Checkbox("Draw Skybox (背景を描く)", &sk.drawSkybox);

            // ランタイム値へ即時反映（強度/描画フラグは再ベイク不要）
            m_iblIntensity    = sk.iblIntensity;
            m_skyboxIntensity = sk.skyboxIntensity;
            m_drawSkybox      = sk.drawSkybox;

            ImGui::Separator();
            if (ImGui::Button("環境マップ適用 / 再ベイク"))
                m_skyboxDirty = true;   // 次フレーム冒頭で再ベイク（WaitIdle 込み）
            ImGui::SameLine();
            ImGui::TextDisabled(m_iblReady && m_iblBaker && m_iblBaker->HasEnvironment()
                                ? "IBL: 有効" : "IBL: フォールバック(ambient)");
            ImGui::End();
        }

        // ---- SSAO 設定ウィンドウ（シーン単位・グローバルレンダ設定・トグル表示）----
        if (m_scene && m_editorCtx->showSSAO)
        {
            auto& ss = m_scene->GetSSAOSettings();
            ImGui::Begin("SSAO");
            ImGui::TextWrapped("深度プリパス + 深度から法線再構築の半球カーネル AO。"
                               "ambient/IBL へ ao を乗算する。透視ビューのみ（2D 正射では無効）。");
            ImGui::Separator();
            ImGui::Checkbox("SSAO 有効", &ss.enabled);
            ImGui::BeginDisabled(!ss.enabled);
            ImGui::SliderFloat("半径 Radius",  &ss.radius,    0.05f, 2.0f, "%.2f");
            ImGui::SliderFloat("バイアス Bias", &ss.bias,     0.0f,  0.1f, "%.3f");
            ImGui::SliderFloat("強度 Intensity", &ss.intensity, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("べき Power",    &ss.power,     0.5f,  4.0f, "%.2f");
            {
                int s16 = (ss.sampleCount >= 16) ? 1 : 0;
                if (ImGui::Combo("サンプル数", &s16, "8\0" "16\0"))
                    ss.sampleCount = s16 ? 16 : 8;
            }
            ImGui::Checkbox("ブラー Blur", &ss.blur);
            ImGui::EndDisabled();
            ImGui::End();
        }

        // ---- Scene Flow 設定ウィンドウ（シーンの流れ・トグル表示）----
        if (m_sceneFlow && m_editorCtx->showSceneFlow)
        {
            namespace fs = std::filesystem;
            std::vector<std::string> scenes;
            std::string scenesDir = PathResolver::AssetsDir() + "scenes";
            if (fs::exists(scenesDir))
            {
                for (auto& e : fs::directory_iterator(scenesDir))
                    if (e.is_regular_file() && e.path().extension() == ".json")
                        scenes.push_back("scenes/" + e.path().filename().string());
            }

            ImGui::Begin("Scene Flow");
            ImGui::TextWrapped("ゲーム開始シーンと、各シーンの次シーンを設定する。");

            std::string start = m_sceneFlow->Start();
            if (ImGui::BeginCombo("Start Scene", start.empty() ? "(none)" : start.c_str()))
            {
                for (auto& s : scenes)
                    if (ImGui::Selectable(s.c_str(), s == start))
                        m_sceneFlow->SetStart(s);
                ImGui::EndCombo();
            }

            ImGui::SeparatorText("Next scene");
            for (auto& s : scenes)
            {
                std::string nx = m_sceneFlow->Next(s);
                ImGui::PushID(s.c_str());
                if (ImGui::BeginCombo(s.c_str(), nx.empty() ? "(none)" : nx.c_str()))
                {
                    if (ImGui::Selectable("(none)", nx.empty()))
                        m_sceneFlow->SetNext(s, "");
                    for (auto& t : scenes)
                        if (ImGui::Selectable(t.c_str(), t == nx))
                            m_sceneFlow->SetNext(s, t);
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            if (ImGui::Button("Save sceneflow.json"))
                m_sceneFlow->Save(PathResolver::AssetsDir() + "sceneflow.json");
            ImGui::End();
        }

        // ---- ビルド設定ウィンドウ（構成/開始シーン/出力先 → ビルド実行・トグル表示）----
        RenderBuildSettingsWindow();

        // Deferred: game build
        // ビルド設定パネルの「ビルド」で pendingBuildGame が立つ。配置先などは buildConfig から読む。
        // 完了したら（設定で有効なら）成果物フォルダを Explorer で開く。
        if (m_editorCtx->pendingBuildGame)
        {
            m_editorCtx->pendingBuildGame = false;
            const bool ok = BuildGame();
            if (ok)
            {
                m_editorCtx->buildCompleteFlash = 3.0f;
                if (m_editorCtx->buildConfig.openFolderAfterBuild && !m_editorCtx->lastBuildDir.empty())
                    ShellExecuteA(nullptr, "open", m_editorCtx->lastBuildDir.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
            }
            else
            {
                m_editorCtx->buildErrorFlash = 6.0f;
                m_editorCtx->errorMessage = m_editorCtx->buildErrorMsg.empty()
                    ? "ビルドに失敗しました。\n詳細は dx12_engine.log を確認してください。"
                    : m_editorCtx->buildErrorMsg;
                m_editorCtx->buildErrorMsg.clear();  // 次回ビルドへ持ち越さない
                m_editorCtx->errorFlash = 1.0f;   // 中央モーダルで通知
            }
        }

        // Deferred: entity deletion
        if (!m_editorCtx->pendingDeletions.empty())
        {
            auto deletions = std::move(m_editorCtx->pendingDeletions);
            m_editorCtx->pendingDeletions.clear();
            for (auto root : deletions)
            {
                auto& reg = m_scene->GetRegistry();
                if (!reg.valid(root)) continue;  // 先行削除のサブツリーに含まれていた場合

                // サブツリー収集（親→子の順。BFS）
                std::vector<entt::entity> subtree{root};
                for (size_t i = 0; i < subtree.size(); ++i)
                {
                    for (auto [c, t] : reg.view<const Transform>().each())
                    {
                        if (t.parent == subtree[i])
                            subtree.push_back(c);
                    }
                }

                // Undo 用スナップショット（全コンポーネント + ローカル親インデックス）
                std::vector<DeletedEntityRecord> records;
                records.reserve(subtree.size());
                entt::entity externalParent = reg.all_of<Transform>(root)
                    ? reg.get<Transform>(root).parent : entt::null;
                for (auto e : subtree)
                {
                    DeletedEntityRecord rec;
                    rec.snapshot = SceneSerializer::SerializeEntity(
                        *m_scene, e, PathResolver::AssetsDir());
                    if (reg.all_of<Transform>(e))
                    {
                        auto parent = reg.get<Transform>(e).parent;
                        auto it = std::find(subtree.begin(), subtree.end(), parent);
                        if (it != subtree.end())
                            rec.parentLocalIndex = static_cast<int>(it - subtree.begin());
                    }
                    records.push_back(std::move(rec));
                }

                m_editorCtx->undoSystem.PushCommand(
                    std::make_unique<DeleteEntityCommand>(
                        m_scene.get(), PathResolver::AssetsDir(),
                        std::move(records), subtree, externalParent));

                // 子から順に削除
                for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
                {
                    if (reg.valid(*it))
                        m_scene->Remove(Entity(*it, &reg));
                }
            }

            // 削除で無効になった選択をクリーンアップ
            {
                auto& reg = m_scene->GetRegistry();
                auto& sel = m_editorCtx->selectedEntities;
                sel.erase(std::remove_if(sel.begin(), sel.end(),
                          [&](entt::entity e) { return !reg.valid(e); }),
                          sel.end());
                if (m_editorCtx->selectedEntity != entt::null
                    && !reg.valid(m_editorCtx->selectedEntity))
                {
                    m_editorCtx->selectedEntity =
                        sel.empty() ? entt::null : sel.back();
                }
            }
        }

        // ---- プロジェクト / バージョン管理(Git) ウィンドウ（トグル表示）----
        if (m_editorCtx->showProject)        RenderProjectWindow();
        if (m_editorCtx->showVersionControl) RenderVersionControlWindow();
        if (m_editorCtx->showMcpBridge && m_mcpBridge)
            McpBridgePanel::Render(*m_mcpBridge, *m_editorCtx);
        if (m_networkPanel && m_networkSystem)
        {
            if (m_editorCtx->showNetworkStatus)
                m_networkPanel->RenderStatus(*m_networkSystem, m_scene->GetRegistry(), *m_editorCtx);
            if (m_editorCtx->showNetworkSettings)
                m_networkPanel->RenderSettings(*m_networkSystem, *m_editorCtx, PathResolver::AssetsDir());
        }
        if (m_vfxEditorPanel)
            m_vfxEditorPanel->RenderWindow(m_scene->GetRegistry(), *m_editorCtx, PathResolver::AssetsDir());
    }

    // ---- ゲーム内 UI: テキスト/ボタン（ImGui オーバーレイ・ゲーム/Play 中のみ）----
    if (m_isGameMode || m_engineMode == EngineMode::Playing)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
            | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##GameUI", nullptr, flags);

        auto* dl = ImGui::GetWindowDrawList();
        // ゲーム UI はビューポート原点へオフセットし、矩形でクリップ＝パネル下に潜らない
        const float ox = static_cast<float>(vpLeft);
        const float oy = static_cast<float>(vpTop);
        dl->PushClipRect(ImVec2(ox, oy),
                         ImVec2(ox + static_cast<float>(vpW), oy + static_cast<float>(vpH)), true);
        std::unordered_set<std::string> nowPressed;
        for (const auto& c : m_uiCommands)
        {
            if (c.type == UICommand::Type::Rect)
            {
                ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
                dl->AddRectFilled(ImVec2(ox + c.x, oy + c.y),
                                  ImVec2(ox + c.x + c.w, oy + c.y + c.h), col, c.size);
            }
            else if (c.type == UICommand::Type::Text)
            {
                ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
                dl->AddText(ImGui::GetFont(), c.size, ImVec2(ox + c.x, oy + c.y), col, c.text.c_str());
            }
            else if (c.type == UICommand::Type::Button)
            {
                ImGui::SetCursorPos(ImVec2(ox + c.x, oy + c.y));
                if (ImGui::Button(c.text.c_str(), ImVec2(c.w, c.h)))
                    nowPressed.insert(c.text);
            }
        }
        dl->PopClipRect();
        ImGui::End();
        m_pressedButtons = std::move(nowPressed);
    }
    else
    {
        m_pressedButtons.clear();
    }
    m_uiCommands.clear();

    m_imguiManager->EndFrame(nativeCmdList);

    // ---- シーントランジション オーバーレイ（ImGui の上に被せる）----
    // フェードは 3D ビューにだけ適用する:
    //   エディタ/Play 中 … 中央ビューポート矩形だけにスシザーを絞り、周りの
    //                      エディタUI（パネル/ツールバー）には掛からないようにする。
    //   ゲーム単体       … ウィンドウ全体。
    if (m_sceneTransition && m_sceneTransition->IsActive())
    {
        u32 tLeft = 0, tTop = 0;
        u32 tW = m_window->GetWidth(), tH = m_window->GetHeight();
        if (!m_isGameMode && m_editorLayer)
        {
            auto vpos  = m_editorLayer->GetViewportPos();
            auto vsize = m_editorLayer->GetViewportSize();
            tLeft = static_cast<u32>(vpos.x);
            tTop  = static_cast<u32>(vpos.y);
            tW    = static_cast<u32>(vsize.x);
            tH    = static_cast<u32>(vsize.y);
            if (tW < 1) tW = 1;
            if (tH < 1) tH = 1;
        }

        nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_commandList->SetViewportAndScissor(tLeft, tTop, tW, tH);
        float aspect = (tH > 0) ? static_cast<f32>(tW) / static_cast<f32>(tH) : 1.0f;
        m_sceneTransition->Render(nativeCmdList, aspect);
    }

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    m_swapChain->Present(m_useVsync);
    m_frameResources->EndFrame(*m_commandQueue);

    // フェンス連動の遅延解放（DeferredRelease）。リモート側の「アップロードを積んだ
    // フレームだけ WaitIdle」方式より強い保証:
    //   - アップロードのあるフレームでも GPU 全停止しない（スポーン時のヒッチ無し）
    //   - メッシュ再生成/シーンClear/RT再作成など GpuResource 系の解放も
    //     フェンス完了までキューで保護される
    //   1. 今フレームでロードされたテクスチャのアップロードステージングを解放キューへ
    //   2. キュー内の未確定分に今フレームの Signal 値を刻む
    //   3. GPU が完了したフェンス値以下の分を実際に解放
    m_resourceManager->DeferPendingUploads();
    DeferredRelease::Stamp(m_commandQueue->GetLastSignaledValue());
    DeferredRelease::Collect(m_commandQueue->GetCompletedValue());
}

} // namespace dx12e
