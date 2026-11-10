#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <functional>
#include <string>

#include "core/Types.h"
#include "renderer/DlssBackend.h"
#include "renderer/UpscalerMath.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class CommandList;

// 時間方向アップスケーラ（off / taa / dlss）の共通部分。今の実装は DLSS だけを持つが、
// FSR を足すときもここに 1 本足せば済むように「入力の準備・出力 RT・ジッタ位相・履歴リセット」を
// バックエンドから切り離してある。
//
// 入力の約束（DLSS へ渡すもの。ここが品質の全て）:
//   color  … m_sceneRT（内部解像度・RGBA16F・トーンマップ前のリニア HDR・露出前）
//   depth  … 主ビューの深度（内部解像度・D32・標準 Z = 0 near / 1 far。DepthInverted は立てない）
//   motion … 速度プリパスの RG16F（現UV - 前UV・ジッタなし）を compute で写し、空（深度 = 1）の画素へ
//            カメラ運動の速度を埋めたもの（UpscalePrep.hlsl）。DLSS には MVScale = (-renderW, -renderH)。
//   jitter … 内部解像度のピクセル。位相数は 8 ×（表示/内部）²（UpscalerMath.h）。
//   露出   … DLSS の自動露出（AutoExposure フラグ）。エンジンの露出は uber パス（DLSS の後）で掛けるので
//            DLSS の入力は「露出前」の値。露出の値は GPU バッファ（AutoExposurePass）にしか無く、手動露出 /
//            EV100 の 3 系統があるため、DLSS 自身に推定させるのが一番素直（露出は DLSS 内部のトーンマップ
//            = 履歴の重み付けにしか使われず、出力の明るさは変わらない）。
class UpscalerPass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir,
                    const std::wstring& ngxDataDir);
    void RecreatePipelines(GraphicsDevice& device);
    void Shutdown();   // GPU アイドルで呼ぶ

    // ApplyRenderResolution から（WaitIdle 済み）。DLSS の機能・履歴も捨てる。
    void Resize(u32 renderW, u32 renderH, u32 outW, u32 outH);

    bool DlssAvailable() const { return m_dlss.Available(); }
    const std::string& DlssReason() const { return m_dlss.Reason(); }
    DlssBackend& Dlss() { return m_dlss; }
    const DlssBackend& Dlss() const { return m_dlss; }

    // 品質モードから内部解像度（NGX の推奨値。取れなければ公称倍率）。
    void DlssRenderSize(u32 outW, u32 outH, upscale::DlssQuality q, u32& rw, u32& rh) { m_dlss.RenderSize(outW, outH, q, rw, rh); }

    // ハルトン(2,3) を位相数 PhaseCount(renderW, outW) で回し、NDC のジッタを返す（TaaJitter.h と同じ規約）。
    DirectX::XMFLOAT2 NextJitterNdc(u32 renderW, u32 renderH, u32 outW);
    u32  LastPhaseCount() const { return m_phaseCount; }

    // 履歴を捨てる（カメラカット / シーン読込 / Play 遷移 / 解像度・モード変更）。次の評価で reset=1。
    void ResetHistory() { m_reset = true; }
    // カメラカットの検出（1 フレームで 10 m 超の移動か 40 度超の回転なら履歴を捨てる）。
    // シーケンサーのカット・MCP の set_editor_camera・テレポートを 1 箇所で拾う。DLSS のフレームだけ呼ぶ。
    void ObserveCamera(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& fwd);

    // 検証用（保存しない）: DLSS へ渡すジッタの符号を反転する。正しい符号は静止画の連続差分で確かめる。
    void SetDebugJitterSign(float s) { m_jitterSign = (s < 0.0f) ? -1.0f : 1.0f; }
    float DebugJitterSign() const { return m_jitterSign; }

    struct DlssFrame
    {
        RenderTarget*   color    = nullptr;   // PIXEL|NON_PIXEL の読取状態にしておくこと
        ID3D12Resource* depth    = nullptr;   // 同上（呼び出し側の状態追跡で）
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};
        RenderTarget*   velocity = nullptr;   // 速度 RT（ここで読取状態へ遷移する）
        DirectX::XMFLOAT4X4 invViewProjT{};   // 現（ジッタなし）VP の逆・転置
        DirectX::XMFLOAT4X4 prevViewProjT{};  // 前（ジッタなし）VP の転置
        DirectX::XMFLOAT2   jitterNdc{};
        float               frameMs = 16.6f;
        upscale::DlssQuality quality = upscale::DlssQuality::Quality;
        std::function<void()> waitIdle;       // 機能の作り直しが要るときだけ呼ぶ（キューのアイドル待ち）
    };
    // DLSS を評価して出力（表示解像度・RGBA16F・PIXEL|NON_PIXEL）の SRV index を返す。失敗は kInvalidIndex。
    // ★戻った後はディスクリプタヒープ / ルートシグネチャが別物になっているので、呼び出し側で張り直すこと。
    u32 EvaluateDlss(CommandList& cmd, const DlssFrame& f);

    u32  RenderW() const { return m_renderW; }
    u32  RenderH() const { return m_renderH; }
    u32  OutW() const { return m_outW; }
    u32  OutH() const { return m_outH; }
    u64  EvalCount() const { return m_evalCount; }
    u64  CutCount() const { return m_cutCount; }
    bool LastEvalOk() const { return m_lastEvalOk; }

private:
    void CreateRootSignature(GraphicsDevice& device);
    bool EnsureResources();

    GraphicsDevice* m_device  = nullptr;
    DescriptorHeap* m_srvHeap = nullptr;
    std::wstring    m_shaderDir;
    DlssBackend     m_dlss;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_prepRs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_prepPso;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_mv;    // 内部解像度 RG16F（UAV）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_out;   // 表示解像度 RGBA16F（UAV + SRV）
    D3D12_RESOURCE_STATES m_mvState  = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_outState = D3D12_RESOURCE_STATE_COMMON;
    u32 m_mvUav  = 0xFFFFFFFFu;   // シェーダ可視ヒープの UAV（作り直しでも同じ番号を使う）
    u32 m_outSrv = 0xFFFFFFFFu;
    u32 m_resW = 0, m_resH = 0, m_resOutW = 0, m_resOutH = 0;   // 今ある m_mv / m_out の寸法

    u32  m_renderW = 0, m_renderH = 0, m_outW = 0, m_outH = 0;
    u32  m_jitterIndex = 0;
    u32  m_phaseCount  = 8;
    bool m_reset       = true;
    float m_jitterSign = 1.0f;
    DirectX::XMFLOAT3 m_prevCamPos{}, m_prevCamFwd{0.0f, 0.0f, 1.0f};
    bool m_prevCamValid = false;
    u64  m_cutCount    = 0;
    u64  m_evalCount   = 0;
    bool m_lastEvalOk  = false;
};

} // namespace dx12e
