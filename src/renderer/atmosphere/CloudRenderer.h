#pragma once
// ===========================================================================
// CloudRenderer — ボリュメトリッククラウド(Schneider 2015 / Hillaire 2016 系のレイマーチ。docs/GRAPHICS_PARITY_DESIGN.md §2.1.3)
// ---------------------------------------------------------------------------
// 専用ルートシグネチャ(メイン RS を汚さない。先例: AtmosphereRenderer / SkyboxRenderer)・専用ノイズ・専用履歴。
// 大気(AtmosphereRenderer)の LUT と定数バッファを借りる: 太陽光は透過率 LUT で色づけ、遠景は AP の froxel を掛ける。
//
//   起動時(最初の Record):  形状 128^3 / 詳細 32^3 / 天気 256^2 のノイズを compute で生成(約 1 回・数 ms)
//   毎フレーム:             TraceCS   … 1/4 解像度(4x4 ブロックにつき 1 画素。毎フレーム別の画素で 16 フレームで 1 周)のレイマーチ
//                           ResolveCS … フル解像度の履歴へ時間再投影(今フレームの画素は新しい値、ほかは再投影 + 近傍クランプ)
//                           Composite … sceneColor へ dst = src.rgb + dst * src.a(深度で雲より手前の不透明物は除く)
//   履歴を捨てるとき(初回 / 解像度変更 / 決定論撮影):  16 サブフレームを 1 フレームで回して全画素を作る(=履歴なしでも同じ絵)
//
// ディスクリプタ(自前ブロック): SRV 9 = [0]形状 [1]詳細 [2]天気 [3]粗い色 [4]粗い距離 [5,6]履歴 0(色,距離) [7,8]履歴 1
//                               UAV 10 = [0]粗い色 [1]粗い距離 [2,3]履歴 0 [4,5]履歴 1 [6]形状 [7]天気 [8]詳細 [9]天気(重複)
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/atmosphere/AtmosphereSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class ConstantBuffer;

class CloudRenderer
{
public:
    static constexpr u32 kShapeSize = 128, kDetailSize = 32, kWeatherSize = 256;
    static constexpr u32 kSubFrames = 16;   // 4x4 の時間再投影
    static constexpr float kMaxDistKm = 100.0f;

    // HLSL の cbuffer CloudCB(CloudMarch.hlsl)と行ごとに一致させること
    struct Cb
    {
        u32 resX, resY, coarseX, coarseY;
        float prevViewProj[16];          // 前フレームの viewProj(ジッタなし)。HLSL の mul(v, M) 用に転置済み
        float windOffsetKm[3]; float coverage;
        float cloudType, density, baseKm, thickKm;
        float detail, scale, maxDistKm, histBlend;
        u32 steps, frame, flags, pad0;
    };
    static_assert(sizeof(Cb) == 144, "CloudCB(CloudMarch.hlsl)と一致させること");
    static constexpr u32 kFlagReset = 1u, kFlagBurst = 2u;

    struct FrameInfo
    {
        u32 width = 0, height = 0;
        float windOffsetKm[3] = {0, 0, 0};
        float prevViewProjT[16] = {};    // 転置済み
        float viewProjT[16] = {};        // 今フレームの viewProj(ジッタなし・転置済み。一括生成で使う)
        bool  havePrev = false;          // 前フレームの行列が有効
        bool  forceReset = false;        // 履歴を捨てる(決定論撮影・シーン読込など)
        bool  settingsChanged = false;   // 設定が変わった(履歴の誤差を早めに戻す)
    };

    bool Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void Shutdown();
    void RecreatePipelines(GraphicsDevice& device);
    bool IsReady() const { return m_ready; }

    // この大きさに作り直す必要があるか(true なら呼び出し側が GPU を待ってから Record すること。履歴・粗い RT を作り直すため)
    bool NeedsResize(u32 w, u32 h) const { return w != m_w || h != m_h; }

    // ノイズ生成(初回のみ)+ 粗いレイマーチ + 解決。sceneColor には触れない(合成は DrawComposite)。
    // atmCb = AtmosphereRenderer の CB(今フレームの主ビュー)、atmSrv = 同 LUT の SRV テーブル(t0..t4)
    void Record(ID3D12GraphicsCommandList* cmd, u32 frameIndex, GraphicsDevice& device, const CloudSettings& s, const FrameInfo& f,
                D3D12_GPU_VIRTUAL_ADDRESS atmCb, D3D12_GPU_DESCRIPTOR_HANDLE atmSrv);
    // 合成。呼び出し側が RT・ビューポート・シザー・ディスクリプタヒープを張っておくこと。Record の後。
    void DrawComposite(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS atmCb, D3D12_GPU_DESCRIPTOR_HANDLE atmSrv,
                       D3D12_GPU_DESCRIPTOR_HANDLE depthSrv);

    u32 FrameCounter() const { return m_frameCounter; }
    bool NoiseReady() const { return m_noiseReady; }

private:
    struct Tex { Microsoft::WRL::ComPtr<ID3D12Resource> res; D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; };
    bool CreateSized(GraphicsDevice& device, u32 w, u32 h);
    void Barrier(ID3D12GraphicsCommandList* cmd, Tex* const* t, u32 n, D3D12_RESOURCE_STATES next);
    void GenerateNoise(ID3D12GraphicsCommandList* cmd, u32 frameIndex);
    void BindCommon(ID3D12GraphicsCommandList* cmd, bool compute, D3D12_GPU_VIRTUAL_ADDRESS atmCb, D3D12_GPU_DESCRIPTOR_HANDLE atmSrv,
                    D3D12_GPU_VIRTUAL_ADDRESS cloudCb, u32 histSrvPair, u32 outUav, u32 volUav, D3D12_GPU_DESCRIPTOR_HANDLE depthSrv);
    D3D12_GPU_DESCRIPTOR_HANDLE Srv(u32 i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Uav(u32 i) const;
    void WriteSizedDescriptors();

    bool m_ready = false;
    DescriptorHeap* m_srvHeap = nullptr;
    ID3D12Device* m_dev = nullptr;
    std::wstring m_shaderDir;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoShape, m_psoDetail, m_psoWeather, m_psoTrace, m_psoResolve, m_psoComposite;

    Tex m_shape, m_detail, m_weather;
    Tex m_traceC, m_traceD;
    Tex m_histC[2], m_histD[2];
    u32 m_w = 0, m_h = 0, m_cw = 0, m_ch = 0;
    u32 m_srvBlock = 0xFFFFFFFFu, m_uavBlock = 0xFFFFFFFFu;
    static constexpr u32 kSrvCount = 9, kUavCount = 10;

    std::unique_ptr<ConstantBuffer> m_cb;   // 3 フレーム × 16 サブ + ノイズ生成用 1
    bool m_noiseReady = false;
    u32  m_frameCounter = 0;   // 通常フレームの 4x4 位相
    u32  m_cur = 0;            // 直近に書いた履歴(0/1)
    bool m_histValid = false;
    u32  m_lastSlot = 0;       // 直近の Record が使った CB スロット(合成が読む)
    u32  m_fastFrames = 0;     // 設定変更後に履歴の戻しを速める残りフレーム
};

} // namespace dx12e
