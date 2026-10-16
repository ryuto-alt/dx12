#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/RtSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class ConstantBuffer;

// RT 反射（R2）。SSR の後段で、SSR の confidence が低い画素だけ RayQuery を撃ち、結果を SSR と同じ形式
// （RGBA16F: rgb = 反射放射輝度 / a = confidence）で出す。フォワード PS は無改造で t16 の代わりにこれを読む。
// 設計は docs/REFLECTION_DESIGN.md §3 / docs/GRAPHICS_PARITY_DESIGN.md §2.5.1。
//
// 4 パス（shaders/raytracing/RtReflection.hlsl）: トレース（ハーフ）→ 時間フィルタ → 空間フィルタ → 合成（フル）。
// ヒット点の shading は DDGI と共通（RtHitShade.hlsli）。
//
// ★専用ルートシグネチャ（メインの 61/64 DWORD には触れない）: b0 = DdgiCB（root CBV。ヒット shading の定数）/
//   b1 = パラメータ（root CBV）/ t0 = TLAS / t1 = GeometryInfo（root SRV）。テクスチャは全部バインドレス（SRV index を b1 で渡す）。
// ★テクスチャは初めて使うフレームで確保する（OFF なら VRAM もディスクリプタも消費しない）。
class RtReflectionPass
{
public:
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);
    void RecreatePipelines(GraphicsDevice& device);
    void Resize(GraphicsDevice& device, u32 width, u32 height);
    // 履歴を捨てる（リサイズ / シーン切替 / 主ビューの矩形変更 / 無効だったフレームの後）
    void InvalidateHistory() { m_histValid = false; }
    bool IsReady() const { return m_psoTrace != nullptr && m_psoTemporal && m_psoSpatial && m_psoComposite; }

    struct Desc
    {
        // 入力（どれも PIXEL_SHADER_RESOURCE の状態で呼ぶこと）
        u32 depthSrv = 0xFFFFFFFFu, gbufferSrv = 0xFFFFFFFFu, velocitySrv = 0xFFFFFFFFu;
        u32 ssrSrv   = 0xFFFFFFFFu;     // SSR の出力。SSR が無いときは 1x1 の黒ダミー（rgb=0, a=0）
        D3D12_GPU_VIRTUAL_ADDRESS tlas = 0, geometryInfo = 0;
        const void* hitCb = nullptr;    // DdgiVolume::FillHitShadeConstants の結果（176 バイト）
        bool  giNew = false;            // GI モード New のヒット shading か
        u32   gridUav = 0xFFFFFFFFu, probeDataSrv = 0xFFFFFFFFu;
        float bounceScale = 1.0f;       // 1 / unitScale（DDGI の内部単位 → シーンの単位）
        float skyScale = 1.0f;          // 空の倍率（IBL なら iblIntensity、環境光スカラーなら 1）
        u32   skyPrefilteredSrv = 0xFFFFFFFFu;   // 空の prefiltered キューブ（フォワードの t6）。無ければ miss は conf 0
        u32   skyMaxMip = 4;
        DirectX::XMMATRIX view{}, proj{};   // ★ジッタなし
        DirectX::XMFLOAT3 cameraPos{0, 0, 0};
        float zNear = 0.1f;
        u32   vpLeft = 0, vpTop = 0, vpW = 0, vpH = 0;
        u32   frameIndex = 0;           // 0..2（定数バッファのリング）
        u32   seq = 0;                  // ノイズ用の通し番号（決定論キャプチャ中は 0 固定）
        bool  gbufferValid = false;
    };

    // 合成後の SRV index を返す（t16 の代わりに使う）。走れなければ kInvalidIndex（SSR をそのまま使う）。
    // 戻り時、出力は PIXEL_SHADER_RESOURCE。RT / ビューポート / RootSig は呼び出し側で再設定すること。
    u32 Run(ID3D12GraphicsCommandList* cmd, const Desc& d, const RtSettings& s);

    // 確保済みテクスチャのバイト数（統計用）
    u64 GetBytes() const;

private:
    enum Pass : u32 { PTrace = 0, PTemporal, PSpatial, PComposite, PCount };

    void CreateRootSignature(GraphicsDevice& device);
    bool EnsureTargets(GraphicsDevice& device, bool half);

    GraphicsDevice* m_device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoTrace, m_psoTemporal, m_psoSpatial, m_psoComposite;

    // 0 = トレース / 1,2 = 履歴 ping-pong / 3 = 空間フィルタ後 / 4 = 合成（フル解像度）
    std::unique_ptr<RenderTarget> m_rt[5];
    D3D12_RESOURCE_STATES m_state[5] = {};
    std::unique_ptr<ConstantBuffer> m_hitCB;     // DdgiCB（フレームのリング）
    std::unique_ptr<ConstantBuffer> m_paramCB;   // 4 パス × フレームのリング
    u32  m_histWrite = 0;
    bool m_histValid = false;
    bool m_half = true;

    DescriptorHeap* m_rtvHeap = nullptr;
    DescriptorHeap* m_srvHeap = nullptr;
    u32 m_width = 0, m_height = 0;     // フル
    u32 m_tw = 0, m_th = 0;            // トレース解像度
    std::wstring m_shaderDir;
};

} // namespace dx12e
