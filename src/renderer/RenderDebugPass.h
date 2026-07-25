#pragma once

// RenderDebugPass — 中間バッファをシーン RT へ後掛けで可視化するフルスクリーンパス。
//
// ★なぜ独立パスなのか（00-COORDINATION §6 N24）
//   フォワード PS に `[branch]` でデバッグ表示を足すと、実行を飛ばしても
//   「コードがそこに在る」だけでレジスタ割当（= occupancy）が悪化して常時遅くなる。
//   デカールで実測 0.49 → 0.55 ms を踏んでいる。よってここは
//   専用ルートシグネチャ + 専用 PSO の完全に独立したパスにしてある。
//   デバッグ OFF のときは Draw を 1 回も呼ばないので、コストは厳密に 0。
//
// ★描く先はポスト前の m_sceneRT。理由は 00-COORDINATION §6 B5:
//   dx12_screenshot / render_debug の readback はポスト前の m_sceneRT を読むので、
//   バックバッファへ描くと「MCP が返す絵に可視化が写らない」。
//   （TaaPass::DrawVelocityDebug はバックバッファへ描いており、実際に写らない）
//
// クラスタライトの複雑度 / カスケード境界 / フォグの各種表示は既にフォワード PS と
// フォグ合成パスに実装されているので、ここでは重複実装せず Application 側が
// 既存トグルへ振り分ける（dx12_render_debug が唯一の入口になる、という統合）。

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class CommandList;

// dx12_render_debug の mode 文字列と 1:1（0 = 無効）。
// ★HLSL の DBG_* 定義（shaders/renderdebug/RenderDebug.hlsl）と数値を一致させること。
enum class RenderDebugMode : u32
{
    Off           = 0,
    Normal        = 1,
    Roughness     = 2,
    Metallic      = 3,
    Depth         = 4,
    Ao            = 5,
    ContactShadow = 6,
    Velocity      = 7,
    Ssr           = 8,
    Ssgi          = 9,
    // DXR（計画09 Step 1）: TLAS が正しく建っているかの目視確認。
    //   RtHit  … プライマリレイのヒット距離のヒートマップ（シルエットがラスタと一致すること）
    //   RtDiff … |RT のヒット距離 - ラスタの距離| のヒートマップ（黒＝完全一致）
    // ★rtDiff が本命。行列の転置ミス / ノード変換の付け忘れ / LOD 違いを一発で炙り出す。
    //   スキンドと半透明は TLAS に入っていないので必ず不一致になる（仕様）。
    RtHit         = 10,
    RtDiff        = 11,
};

class RenderDebugPass
{
public:
    void Initialize(GraphicsDevice& device, DXGI_FORMAT sceneRtFormat,
                    const std::wstring& shaderDir);
    // シェーダーホットリロード用（PSO だけ作り直す）
    void RecreatePipelines(GraphicsDevice& device);

    bool IsReady() const { return m_pso != nullptr; }

    struct DrawDesc
    {
        RenderDebugMode mode = RenderDebugMode::Off;
        D3D12_GPU_DESCRIPTOR_HANDLE sourceSrv{};   // t0（モードごとに差し替わる Texture2D）
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};    // t1（常に有効なものを渡すこと）
        f32 uvOfsX = 0.0f, uvOfsY = 0.0f;          // シーン RT 内のビューポート矩形（UV）
        f32 uvSclX = 1.0f, uvSclY = 1.0f;
        u32 vpLeft = 0, vpTop = 0, vpW = 1, vpH = 1;
        f32 gain      = 1.0f;    // 表示倍率（velocity / roughness / metallic）
        f32 projA     = 0.0f;    // proj._33
        f32 projB     = 0.0f;    // proj._43
        f32 depthRange = 100.0f; // 深度表示のレンジ(m)
        f32 exposure   = 1.0f;   // SSR/SSGI 表示の露出
    };

    // 呼び出し側の責任: RTV（シーン RT）をバインド済み・SRV ヒープ設定済み・
    // 渡す SRV が PIXEL_SHADER_RESOURCE であること。
    void Draw(CommandList& cmd, const DrawDesc& d);

private:
    void CreateRootSignature(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    DXGI_FORMAT  m_rtFormat = DXGI_FORMAT_UNKNOWN;
    std::wstring m_shaderDir;
};

} // namespace dx12e
