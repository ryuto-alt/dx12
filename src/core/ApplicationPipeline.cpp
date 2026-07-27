// ===========================================================================
// Application: PSO 再生成・レンダー解像度・PSO/SRV キャッシュ
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


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

    // 自動インスタンシング版（VS だけ差し替え、slot1 に per-instance world を足す）。
    // LESS / LESS_EQUAL の 2 種を通常版と対にして作る。
    {
        auto vsInst = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardInstanced_VS.cso");
        if (vsInst.GetSize() > 0)
        {
            PipelineStateBuilder ib;
            ib.SetRootSignature(m_rootSignature->Get())
              .SetVertexShader(vsInst.GetData(), vsInst.GetSize())
              .SetPixelShader(ps.GetData(), ps.GetSize())
              .SetInputLayout(Mesh::GetInstancedInputLayout(), Mesh::GetInstancedInputLayoutCount())
              .SetRenderTargetFormat(kSceneColorFormat)
              .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
              .SetDepthEnabled(true)
              .SetCullMode(D3D12_CULL_MODE_NONE);
            if (!m_pipelineStateInst) m_pipelineStateInst = std::make_unique<PipelineState>();
            m_pipelineStateInst->Initialize(*m_graphicsDevice, ib);

            ib.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
            if (!m_pipelineStateInstLEqual) m_pipelineStateInstLEqual = std::make_unique<PipelineState>();
            m_pipelineStateInstLEqual->Initialize(*m_graphicsDevice, ib);
        }
    }

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

void Application::RecreateTerrainPsos()
{
    // 地形マテリアル（4 レイヤースプラット）。ルートシグネチャ・入力レイアウト・RT 形式は
    // 通常 forward とまったく同じで、VS/PS だけ差し替える。
    // ★t0/t1 に Texture2DArray を張るのはここではなく EnsureTerrainSrv（PSO には関係しない）。
    auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Terrain_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Terrain_PS.cso");
    if (vs.GetSize() == 0 || ps.GetSize() == 0)
    {
        // .cso が無い＝ビルドし忘れ。地形は layerSetPath 空なら従来経路で描けるので致命ではない。
        Logger::Warn("Terrain_VS/PS.cso が読めません（地形スプラットは無効。従来経路で描画します）");
        return;
    }

    PipelineStateBuilder builder;
    builder.SetRootSignature(m_rootSignature->Get())
           .SetVertexShader(vs.GetData(), vs.GetSize())
           .SetPixelShader(ps.GetData(), ps.GetSize())
           .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
           .SetRenderTargetFormat(kSceneColorFormat)
           .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
           .SetDepthEnabled(true)
           .SetCullMode(D3D12_CULL_MODE_NONE);

    if (!m_terrainPipelineState) m_terrainPipelineState = std::make_unique<PipelineState>();
    m_terrainPipelineState->Initialize(*m_graphicsDevice, builder);

    // 深度プリパス併用時の LESS_EQUAL バリアント（地形は不透明なのでプリパスに乗る）。
    builder.SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
    if (!m_terrainPipelineStateLEqual) m_terrainPipelineStateLEqual = std::make_unique<PipelineState>();
    m_terrainPipelineStateLEqual->Initialize(*m_graphicsDevice, builder);
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

    // Shadow インスタンシング版（slot1=per-instance world）。b0 には transpose(lightVP) を入れる。
    {
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ShadowPassInstanced_VS.cso");
        if (vs.GetSize() > 0)
        {
            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetInputLayout(Mesh::GetInstancedInputLayout(), Mesh::GetInstancedInputLayoutCount())
                   .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetDepthBias(8000, 2.0f);
            if (!m_shadowPipelineStateInst) m_shadowPipelineStateInst = std::make_unique<PipelineState>();
            m_shadowPipelineStateInst->Initialize(*m_graphicsDevice, builder);
        }
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
        // 深度プリパスのインスタンシング版（bias なし・その他は影版と同条件）
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ShadowPassInstanced_VS.cso");
        if (vs.GetSize() > 0)
        {
            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetInputLayout(Mesh::GetInstancedInputLayout(), Mesh::GetInstancedInputLayoutCount())
                   .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetCullMode(D3D12_CULL_MODE_NONE);
            if (!m_depthPrepassPSOInst) m_depthPrepassPSOInst = std::make_unique<PipelineState>();
            m_depthPrepassPSOInst->Initialize(*m_graphicsDevice, builder);
        }
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

// 深度+速度プリパス（PrepassMode::DepthVelocityGBuffer）の PSO 3 本。
// 深度プリパス PSO との唯一の違いは「RTV に速度バッファ(RG16F)を持つ」ことと VS/PS。
// 深度をフォワードとビット一致させるため、VS の演算順は ShadowPass / DepthPrepassSkinned と
// 完全に揃えてある（shaders/velocity/*.hlsl のヘッダコメント参照）。
// TAA の履歴と、速度の再投影に使う前フレーム情報をまとめて捨てる。
// シーンロード / Play↔Editor 遷移 / リサイズで呼ぶこと。忘れると「切替直後に
// 前のシーンの絵が半透明で残る」という気味の悪いバグになる。
void Application::InvalidateTemporalHistory()
{
    if (m_taaPass) m_taaPass->InvalidateHistory();
    // SSR/SSGI も「前フレームカラー」と時間蓄積の履歴を持っている。捨てないと
    // シーン切替直後の 1 フレームだけ前のシーンの色が反射/間接光として写り込む。
    if (m_screenSpaceGi) m_screenSpaceGi->InvalidateHistory();
    // ボリュメトリックフォグの froxel ボリュームも時間再投影の履歴を持っている。
    if (m_volumetricFogPass) m_volumetricFogPass->InvalidateHistory();
    m_prevViewProjNJValid = false;
    m_prevFrameIndexValid = false;
    m_prevViewProjValid   = false;   // モーションブラーの速度スパイクも同時に防ぐ
}

// ============================================================================
//  レンダー解像度と表示解像度の分離（#16）
// ============================================================================
// 表示側 = バックバッファ上の矩形。エディタは ImGui のシーンビュー矩形、
// 単体ゲーム / ゲームモードはウィンドウ全面。**ここだけが「画面上の位置」を知っている。**
void Application::GetDisplayViewport(u32& x, u32& y, u32& w, u32& h) const
{
    // 全画面は「単体ゲーム（エディタ UI なし）」のみ。エディタは編集中も Play 中も
    // 中央の 16:9 ビューポート矩形に描く（パネル下に潜り込ませない）。
    if (m_isGameMode || !m_editorLayer)
    {
        x = 0; y = 0;
        w = m_window ? m_window->GetWidth()  : 1u;
        h = m_window ? m_window->GetHeight() : 1u;
    }
    else
    {
        const auto vp = m_editorLayer->GetViewportPos();
        const auto vs = m_editorLayer->GetViewportSize();
        // 負座標(パネルのドラッグ中/画面外)を u32 へキャストすると巨大値にラップし、
        // ビューポートが RT 外へ飛んで描画が全滅する(クリアだけ残り真っ青)ため 0 で下限。
        x = static_cast<u32>((std::max)(0.0f, vp.x));
        y = static_cast<u32>((std::max)(0.0f, vp.y));
        w = static_cast<u32>((std::max)(0.0f, vs.x));
        h = static_cast<u32>((std::max)(0.0f, vs.y));
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}

// シーン系 RT を丸ごと (w,h) へ作り直す。**呼び出し側はフレーム外（Run ループ）であること。**
// スワップチェイン（表示解像度）はここでは触らない。
void Application::ApplyRenderResolution(u32 w, u32 h)
{
    if (w == 0 || h == 0) return;
    if (w == m_renderW && h == m_renderH) return;
    if (!m_graphicsDevice || !m_commandQueue) return;

    m_commandQueue->WaitIdle();

    // 深度バッファ（DSV + SRV は既存ハンドルへ張り直す＝ヒープは消費しない）
    if (m_dsvHandle.ptr != 0)
    {
        m_depthBuffer.Reset();
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width            = w;
        depthDesc.Height           = h;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels        = 1;
        depthDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.SampleDesc       = {1, 0};
        depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format       = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue, IID_PPV_ARGS(&m_depthBuffer)));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
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
    }

    // シーン系 RT（★ここに載っていないものは表示解像度のまま = スワップチェインだけ）
    if (m_sceneRT)          m_sceneRT->Resize(*m_graphicsDevice, w, h);
    if (m_ssaoPass)         m_ssaoPass->Resize(*m_graphicsDevice, w, h);
    if (m_contactShadowPass)m_contactShadowPass->Resize(*m_graphicsDevice, w, h);
    if (m_bloomPass)        m_bloomPass->Resize(*m_graphicsDevice, w, h);
    if (m_godRaysPass)      m_godRaysPass->Resize(*m_graphicsDevice, w, h);
    if (m_lensFlarePass)    m_lensFlarePass->Resize(*m_graphicsDevice, w, h);
    if (m_dofPass)          m_dofPass->Resize(*m_graphicsDevice, w, h);
    if (m_motionBlurPass)   m_motionBlurPass->Resize(*m_graphicsDevice, w, h);
    if (m_distortRT)        m_distortRT->Resize(*m_graphicsDevice, w, h);
    if (m_taaPass)          m_taaPass->Resize(*m_graphicsDevice, w, h);
    if (m_gbufferRT)        m_gbufferRT->Resize(*m_graphicsDevice, w, h);
    if (m_screenSpaceGi)    m_screenSpaceGi->Resize(*m_graphicsDevice, w, h);
    if (m_rtScreenPass)     m_rtScreenPass->Resize(*m_graphicsDevice, w, h);

    // ★時間履歴は必ず捨てる。座標系が変わった履歴を持ち越すと TAA / SSR / SSGI /
    //   ボリュメトリックフォグが揃ってゴーストする（引き伸ばされた前フレームが尾を引く）。
    InvalidateTemporalHistory();

    m_renderW = w;
    m_renderH = h;
    m_pendingRenderW = w;
    m_pendingRenderH = h;
    m_renderResizeSettle = 0;
    Logger::Info("レンダー解像度: {}x{}（scale {:.2f}）", w, h, m_renderScale);
}

// 毎フレーム Run ループの先頭で呼ぶ。表示矩形 × renderScale へ追従する。
void Application::UpdateRenderResolution()
{
    u32 dx = 0, dy = 0, dw = 0, dh = 0;
    GetDisplayViewport(dx, dy, dw, dh);
    const f32 s = std::clamp(m_renderScale, 0.25f, 1.0f);
    // ★アスペクトは表示側の値を使い続ける（Render() の renderAspect）。ここは丸めるだけ。
    const u32 want = (std::max)(16u, static_cast<u32>(std::lround(static_cast<double>(dw) * s)));
    const u32 wantH = (std::max)(16u, static_cast<u32>(std::lround(static_cast<double>(dh) * s)));

    if (want == m_renderW && wantH == m_renderH)
    {
        m_pendingRenderW = want; m_pendingRenderH = wantH;
        m_renderResizeSettle = 0;
        m_renderResFlush = false;
        return;
    }

    if (m_renderResFlush || m_renderW == 0)
    {
        m_renderResFlush = false;
        ApplyRenderResolution(want, wantH);
        return;
    }

    // ドッキング分割のドラッグ中は毎フレーム矩形が変わる。そのたびに RT を作り直すと
    // WaitIdle でカクつくので、同じサイズが数フレーム続いてから確定させる。
    // 待っている間は「1 つ前のレンダー解像度の絵を表示矩形へ拡大」して出るだけ＝破綻しない。
    if (want == m_pendingRenderW && wantH == m_pendingRenderH)
    {
        if (++m_renderResizeSettle >= kRenderResizeSettleFrames)
            ApplyRenderResolution(want, wantH);
    }
    else
    {
        m_pendingRenderW = want;
        m_pendingRenderH = wantH;
        m_renderResizeSettle = 0;
    }
}

void Application::SetRenderScale(f32 s)
{
    const f32 v = std::clamp(s, 0.25f, 1.0f);
    if (v == m_renderScale) return;
    m_renderScale    = v;
    m_renderResFlush = true;          // 次の Run ループ先頭で即反映（デバウンスしない）
    PersistSet("render_scale", static_cast<double>(v));
}

void Application::RecreateVelocityPsos()
{
    // MRT=2: RTV0=速度(RG16F) / RTV1=G-Buffer(RGBA16F: xy=oct(worldN) z=rough w=metal)。
    // ★ 本数を条件で変えないこと。G-Buffer が要らないフレームでも同じ PSO で走らせて
    //   RTV を張らない、をやると PSO 不一致になる（00-COORDINATION §5.5）。
    const DXGI_FORMAT rtFormats[] = { TaaPass::kVelocityFormat, kGBufferFormat };

    auto build = [&](const wchar_t* vsName, const D3D12_INPUT_ELEMENT_DESC* layout, u32 layoutCount,
                     std::unique_ptr<PipelineState>& out)
    {
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + vsName);
        auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"VelocityPrepass_PS.cso");
        if (vs.GetSize() == 0 || ps.GetSize() == 0) return;
        PipelineStateBuilder builder;
        builder.SetRootSignature(m_rootSignature->Get())
               .SetVertexShader(vs.GetData(), vs.GetSize())
               .SetPixelShader(ps.GetData(), ps.GetSize())
               .SetInputLayout(layout, layoutCount)
               .SetRenderTargetFormats(static_cast<u32>(std::size(rtFormats)), rtFormats)
               .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
               .SetDepthEnabled(true)
               .SetCullMode(D3D12_CULL_MODE_NONE);  // 深度バイアスなし（深度プリパスと同条件）
        if (!out) out = std::make_unique<PipelineState>();
        out->Initialize(*m_graphicsDevice, builder);
    };

    build(L"VelocityPrepass_VS.cso", Mesh::GetInputLayout(), Mesh::GetInputLayoutCount(),
          m_velocityPSO);
    build(L"VelocityPrepassInstanced_VS.cso",
          Mesh::GetVelocityInstancedInputLayout(), Mesh::GetVelocityInstancedInputLayoutCount(),
          m_velocityPSOInst);
    build(L"VelocityPrepassSkinned_VS.cso", Mesh::GetInputLayout(), Mesh::GetInputLayoutCount(),
          m_velocityPSOSkinned);
}

// 速度パス用の per-instance「前フレームのワールド行列」バッファ（slot2）を必要になった時だけ確保する。
// 既存の m_instanceBuffer と同じリング構成・同じ base/count で使う（stride だけ 48B）。
void Application::EnsureInstancePrevBuffer()
{
    if (m_instancePrevBuffer[0]) return;   // 確保済み

    for (u32 fi = 0; fi < FrameResources::kFrameCount; ++fi)
    {
        const UINT bytes = kMaxInstances * sizeof(MeshInstancePrevData);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_instancePrevBuffer[fi])));
        void* mapped = nullptr; D3D12_RANGE rr{0, 0};
        ThrowIfFailed(m_instancePrevBuffer[fi]->Map(0, &rr, &mapped));
        m_instancePrevMapped[fi] = static_cast<uint8_t*>(mapped);
        m_instancePrevVbView[fi].BufferLocation = m_instancePrevBuffer[fi]->GetGPUVirtualAddress();
        m_instancePrevVbView[fi].StrideInBytes  = sizeof(MeshInstancePrevData);
        m_instancePrevVbView[fi].SizeInBytes    = bytes;
    }
    Logger::Info("速度パス用の per-instance 前ワールドバッファを確保しました ({} MB)",
                 kMaxInstances * sizeof(MeshInstancePrevData) * FrameResources::kFrameCount / (1024 * 1024));
}

void Application::RegisterShaderReloadHandlers()
{
    if (!m_shaderManager)
        return;

    m_shaderManager->RegisterReloadHandler(
        { L"Forward_VS.cso", L"Forward_PS.cso", L"ForwardLdr_PS.cso", L"ForwardInstanced_VS.cso" },
        [this]() { RecreateForwardPsos(); });
    m_shaderManager->RegisterReloadHandler(
        { L"ForwardSkinned_VS.cso", L"Forward_PS.cso" },
        [this]() { RecreateSkinnedPsos(); });
    m_shaderManager->RegisterReloadHandler(
        { L"ForwardGrid_VS.cso", L"ForwardGrid_PS.cso" },
        [this]() { RecreateGridPso(); });
    m_shaderManager->RegisterReloadHandler(
        { L"Terrain_VS.cso", L"Terrain_PS.cso" },
        [this]() { RecreateTerrainPsos(); });
    m_shaderManager->RegisterReloadHandler(
        { L"Emissive_VS.cso", L"Emissive_PS.cso" },
        [this]() { RecreateEmissivePso(); });
    m_shaderManager->RegisterReloadHandler(
        { L"ShadowPass_VS.cso", L"ShadowPassSkinned_VS.cso", L"ShadowPassInstanced_VS.cso" },
        [this]() { RecreateShadowPsos(); RecreateDepthPrepassPsos(); });  // ShadowPass_VS は深度プリパスにも流用
    m_shaderManager->RegisterReloadHandler(
        { L"DepthPrepassSkinned_VS.cso" },
        [this]() { RecreateDepthPrepassPsos(); });
    m_shaderManager->RegisterReloadHandler(
        { L"VelocityPrepass_VS.cso", L"VelocityPrepass_PS.cso",
          L"VelocityPrepassInstanced_VS.cso", L"VelocityPrepassSkinned_VS.cso" },
        [this]() { RecreateVelocityPsos(); });
    if (m_taaPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"TaaResolve_PS.cso", L"VelocityDebug_PS.cso" },
            [this]() { m_taaPass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_renderDebugPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"RenderDebug_PS.cso" },
            [this]() { m_renderDebugPass->RecreatePipelines(*m_graphicsDevice); });
    }
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
    if (m_contactShadowPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"ContactShadow_VS.cso", L"ContactShadow_PS.cso" },
            [this]() { m_contactShadowPass->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_screenSpaceGi)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"ScreenSpace_VS.cso", L"SsrTrace_PS.cso", L"SsrUpsample_PS.cso",
              L"SsgiTrace_PS.cso", L"SsgiTemporal_PS.cso", L"SsgiUpsample_PS.cso",
              L"ColorDownsample_PS.cso" },
            [this]() { m_screenSpaceGi->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_rtScreenPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"Rt_VS.cso", L"RtShadow_PS.cso", L"RtAo_PS.cso", L"RtDebug_PS.cso" },
            [this]() { m_rtScreenPass->RecreatePipelines(*m_graphicsDevice); });
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
    if (m_clusteredLighting)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"ClusterBuild_CS.cso", L"ClusterCull_CS.cso" },
            [this]() { m_clusteredLighting->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_decalSystem)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"ClusterCullDecals_CS.cso" },
            [this]() { m_decalSystem->RecreatePipelines(*m_graphicsDevice); });
    }
    if (m_volumetricFogPass)
    {
        m_shaderManager->RegisterReloadHandler(
            { L"FogInject_CS.cso", L"FogScatter_CS.cso", L"FogIntegrate_CS.cso",
              L"FogComposite_VS.cso", L"FogComposite_PS.cso" },
            [this]() { m_volumetricFogPass->RecreatePipelines(*m_graphicsDevice); });
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
// シーン JSON には assets 相対の "shaders/foo.hlsl" 表記が紛れ込みがち(正しくは
// assets/shaders 相対の "foo.hlsl")。従来は黙って既定 Forward にフォールバックして
// いたので、先頭の "shaders/" は剥がして同一キーに正規化する。
static std::string NormalizeCustomShaderKey(const std::string& shaderRel)
{
    std::string key = shaderRel;
    for (char& c : key)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (key.rfind("shaders/", 0) == 0)
        key.erase(0, 8);
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
        // key を渡す("shaders/" 接頭辞剥がし済み。ShaderManager 側は assets/shaders 相対で管理)
        if (!m_shaderManager->HasValidCustomShader(key))
            m_shaderManager->CompileCustomShader(key);  // 未スキャン分の遅延コンパイル救済
        vsBytesOut = m_shaderManager->GetCustomVsBytecode(key);
        psBytesOut = m_shaderManager->GetCustomPsBytecode(key);
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
    auto resolve = [&](const std::string& overridePath, Texture* fallback, bool srgb,
                       TextureUsage usage) -> Texture* {
        if (overridePath.empty())
            return fallback;
        if (!dx12e::vfs::Exists(overridePath))  // pak/ディスク両対応(std::filesystem::exists は pak モードで常に false)
        {
            Logger::Warn("マテリアル上書きテクスチャが見つかりません: {}", overridePath);
            return fallback;
        }
        std::string fullPath = PathResolver::AssetsDir() + overridePath;
        return m_resourceManager->GetOrLoadTexture(PathResolver::Utf8ToWide(fullPath), cmdList, srgb, usage);
    };

    Texture* albedoFallback = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
    Texture* normalFallback = (mat && mat->normalMapTexture) ? mat->normalMapTexture : m_resourceManager->GetDefaultNormalTexture();
    Texture* mrFallback     = (mat && mat->metalRoughnessTexture) ? mat->metalRoughnessTexture : m_resourceManager->GetDefaultMetalRoughnessTexture();

    Texture* albedo = resolve(albedoPath, albedoFallback, /*srgb=*/true,  TextureUsage::BaseColor);
    Texture* normal = resolve(normalPath, normalFallback, /*srgb=*/false, TextureUsage::Normal);
    Texture* mr     = resolve(mrPath,     mrFallback,     /*srgb=*/false, TextureUsage::NonColor);
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

u32 Application::EnsureTerrainSrv(entt::entity e, const Terrain& terrain,
                                  ID3D12GraphicsCommandList* cmdList)
{
    // レイヤーセット未設定 = 従来経路（この関数は呼ばれない想定だが二重に守る）
    if (terrain.layerSetPath.empty() || !m_terrainLayerSets || !terrain._splat
        || !terrain._splat->IsValid())
        return 0xFFFFFFFF;

    const auto* layers = m_terrainLayerSets->GetOrLoad(terrain.layerSetPath, cmdList);
    if (!layers || !layers->valid || !layers->albedoArray || !layers->surfaceArray)
        return 0xFFFFFFFF;

    // ★シーンが作り直されたら（Play→Stop / シーン切替）キャッシュを丸ごと捨てる。
    //   entt は entity id を再利用するので、放置すると別の地形へ前のスプラットが張られる。
    const u32 sceneGen = static_cast<u32>(m_sceneGeneration);
    if (m_terrainSrvGeneration != sceneGen)
    {
        for (auto& kv : m_terrainSrvCache)
            if (kv.second.blockStart != 0xFFFFFFFF) m_srvHeap->FreeBlock(kv.second.blockStart, 3);
        m_terrainSrvCache.clear();
        m_terrainSrvGeneration = sceneGen;
    }

    TerrainSrvEntry& entry = m_terrainSrvCache[static_cast<u32>(e)];

    // ---- スプラットの GPU テクスチャ（CPU 側でミップまで焼いてから 1 回でアップロード）----
    // 解像度 512 で全ミップ込み 1.3MB。ペイント中に毎フレーム作り直しても実測で無視できる。
    const u32 splatVersion = terrain._splat->Version();
    const u32 splatSize    = terrain._splat->Size();
    const bool splatStale  = (!entry.splatTex) || entry.splatVersion != splatVersion
                          || entry.splatSize != splatSize;
    if (splatStale)
    {
        const std::vector<std::vector<u8>> mips = terrain._splat->BuildMipChain();
        if (mips.empty()) return 0xFFFFFFFF;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = splatSize;
        desc.Height           = splatSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = static_cast<UINT16>(mips.size());
        desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;   // 重みは linear（sRGB にしない）
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        std::vector<D3D12_SUBRESOURCE_DATA> subs(mips.size());
        u32 w = splatSize;
        for (size_t m = 0; m < mips.size(); ++m)
        {
            subs[m].pData      = mips[m].data();
            subs[m].RowPitch   = static_cast<LONG_PTR>(w) * 4;
            subs[m].SlicePitch = subs[m].RowPitch * static_cast<LONG_PTR>(w);
            w = (w > 1) ? (w >> 1) : 1;
        }

        auto tex = std::make_unique<Texture>();
        tex->Initialize(*m_graphicsDevice, cmdList, desc, subs.data(), static_cast<u32>(subs.size()));
        entry.splatTex     = std::move(tex);
        entry.splatVersion = splatVersion;
        entry.splatSize    = splatSize;
    }

    // ---- t0,t1,t2 の連続 3 ディスクリプタ ----
    if (entry.blockStart == 0xFFFFFFFF)
        entry.blockStart = m_srvHeap->AllocateBlock(3);

    // レイヤーセットが差し替わった / ホットリロードされた / スプラットを作り直した時だけ張り直す。
    if (splatStale || entry.layerGeneration != layers->generation
        || entry.layerSetPath != terrain.layerSetPath)
    {
        // ★同じテーブルへ Texture2DArray（t0/t1）と Texture2D（t2）を混ぜて張る。
        //   次元はルートシグネチャに書かれていないので合法。Terrain.hlsl の宣言と一致している。
        layers->albedoArray ->CreateArraySRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(entry.blockStart));
        layers->surfaceArray->CreateArraySRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(entry.blockStart + 1));
        entry.splatTex      ->CreateSRV     (*m_graphicsDevice, m_srvHeap->GetCpuHandle(entry.blockStart + 2));
        entry.layerGeneration = layers->generation;
        entry.layerSetPath    = terrain.layerSetPath;
    }

    return entry.blockStart;
}



} // namespace dx12e
