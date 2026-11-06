#pragma once
// ===========================================================================
// ReflectionProbeSystem — 反射プローブ（UE の Box Reflection Capture 相当）の GPU 側。
// ---------------------------------------------------------------------------
// ・撮影: Application が ReflectionProbe の位置から 6 面を RenderView で描く（HDR・ポストなし）。
//   1 面描くごとに CopyFace() でキューブ（ミップ列つき）へ書き、6 面そろったら EndCapture() が
//   ミップ列を作り → 既存の PrefilterEnv_CS（GGX importance sampling）で粗さ別ミップへ畳み込み、
//   プローブ配列の自分のスロット（6 スライス）へ書く。
// ・実行時: フォワードは配列（t33 TextureCubeArray）とプローブ情報（t34 StructuredBuffer）を
//   ClusteredLightCulling のテーブル（slot 11）の +12/+13 から読む（ルート定数は増えない）。
// ・毎フレームは描かない。シーン読み込み後と明示操作（MCP reflection_capture / Inspector）だけ。
// ・スロット台帳（どのエンティティが何番か・焼き済みか・いつ焼いたか）もここが持つ。
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include <memory>
#include <string>

#include "core/Types.h"
#include "ecs/Components.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class ConstantBuffer;

class ReflectionProbeSystem
{
public:
    static constexpr u32 kMaxProbes    = 16;
    static constexpr u32 kCaptureSize  = 256;   // 撮影解像度（1 面）
    static constexpr u32 kSourceMips   = 9;     // 256 → 1
    static constexpr u32 kFilteredSize = 128;   // 畳み込み後の 1 面
    static constexpr u32 kFilteredMips = 7;     // 128 → 2。mip m の粗さ = m / (kFilteredMips - 1)
    static constexpr u32 kFrameCount   = 3;

    // shaders/forward/ReflectionProbe.hlsli の ReflProbeGpu と一致（64B）
    struct GpuProbe
    {
        DirectX::XMFLOAT4 posBlend;
        DirectX::XMFLOAT4 extIntensity;
        DirectX::XMFLOAT4 misc;
        DirectX::XMFLOAT4 pad;
    };
    static_assert(sizeof(GpuProbe) == 64, "ReflProbeGpu must be 64 bytes");

    // 台帳 1 行（CPU のみ）
    struct Slot
    {
        entt::entity entity   = entt::null;
        bool         used     = false;
        bool         captured = false;
        bool         pending  = false;     // 焼き待ち
        u32          wait     = 0;         // pending になってから焼くまでの残りフレーム（DDGI などが落ち着くのを待つ）
        double       capturedAtSec = 0.0;  // 焼いた時刻（NowSec）
        u32          captureCount  = 0;
        DirectX::XMFLOAT3 pos{};
        DirectX::XMFLOAT3 capturedPos{};   // 焼いた時の位置（動いたら焼き直しの印）
        ReflectionProbe   cfg{};
        bool         disabled = false;
    };

    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, DescriptorHeap* rtvHeap,
                    DescriptorHeap* dsvHeap, const std::wstring& shaderDirW, u32 frameConstantsBytes);
    void RecreatePipelines(GraphicsDevice& device);
    void Shutdown();
    bool IsReady() const { return m_ready; }

    // ---- 台帳 ----
    Slot*       Slots()       { return m_slots; }
    const Slot* Slots() const { return m_slots; }
    int  FindSlot(entt::entity e) const;
    int  AcquireSlot(entt::entity e);          // 無ければ空きを割り当てる。満杯なら -1
    void ResetAll();                            // シーン切り替え時
    static double NowSec();

    // ---- 撮影用の資源（Application::RenderView の ViewDesc へ渡す）----
    RenderTarget*               CaptureColor() const { return m_captureRT.get(); }
    ID3D12Resource*             CaptureDepth() const { return m_depth.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE CaptureDsv()   const { return m_dsv; }
    ConstantBuffer*             FaceCB(u32 face) const { return m_faceCB[face % 6].get(); }

    // ---- コマンド記録 ----
    void BeginCapture(ID3D12GraphicsCommandList* cmd);                  // ソースキューブを書き込み可能にする
    void CopyFace(ID3D12GraphicsCommandList* cmd, u32 face);            // CaptureColor（PIXEL_SHADER_RESOURCE）→ ソース mip0 の face
    void EndCapture(ID3D12GraphicsCommandList* cmd, u32 slot);          // ミップ列 → GGX 畳み込み → 配列の slot

    // ---- フォワードへ渡す ----
    // 今のフレームのプローブ情報（count 個）を書き、テーブルの +12/+13 を張る。
    void WriteFrame(GraphicsDevice& device, DescriptorHeap& srvHeap, u32 tableBase, u32 frameIndex,
                    const GpuProbe* probes, u32 count);
    // 初期化直後など、プローブが 0 個でもテーブルを有効なディスクリプタで埋める
    void WriteTableDummy(GraphicsDevice& device, DescriptorHeap& srvHeap, u32 tableBase, u32 frameIndex);

    u32 GpuBytes() const;

private:
    void CreateResources(GraphicsDevice& device);

    bool m_ready = false;
    DescriptorHeap* m_srvHeap = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCopy, m_psoDown, m_psoPrefilter;
    std::wstring m_shaderDir;

    std::unique_ptr<RenderTarget>   m_captureRT;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;
    D3D12_CPU_DESCRIPTOR_HANDLE     m_dsv{};
    std::unique_ptr<ConstantBuffer> m_faceCB[6];

    Microsoft::WRL::ComPtr<ID3D12Resource> m_srcCube;    // 256, 6 slices, 9 mips（撮影のミップ列）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_filtered;   // 128, 6*kMaxProbes slices, 7 mips（t33）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_info;       // UPLOAD。kFrameCount × kMaxProbes × 64B
    u8* m_infoMapped = nullptr;

    // 永続ディスクリプタ（shader-visible。compute 用）
    u32 m_block = 0xFFFFFFFFu;
    static constexpr u32 kDescCubeSrv    = 0;                         // ソースの TextureCube
    static constexpr u32 kDescMipSrv     = 1;                         // + m（Texture2DArray）
    static constexpr u32 kDescMipUav     = 1 + kSourceMips;           // + m
    static constexpr u32 kDescFilteredUav = 1 + 2 * kSourceMips;      // + slot * kFilteredMips + m
    static constexpr u32 kDescCount      = kDescFilteredUav + kMaxProbes * kFilteredMips;

    Slot m_slots[kMaxProbes];
};

} // namespace dx12e
