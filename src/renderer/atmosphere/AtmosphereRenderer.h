#pragma once
// ===========================================================================
// AtmosphereRenderer — 物理ベース大気(A1 / Hillaire 2020)の LUT 群・空パス・エアリアルパースペクティブ合成
// ---------------------------------------------------------------------------
// 専用ルートシグネチャ(メイン RS を汚さない。docs/GRAPHICS_PARITY_DESIGN.md §2.0.2 の手段 A)・専用の RGBA16F LUT・専用 CB を持つ。
// 遅延確保はしない: Initialize で LUT(合計約 0.7 MiB)とディスクリプタを確保する(atmosphere.enabled=false の間は 1 度も走らない)。
//
//   Transmittance 256x64 / MultiScattering 32x32 … パラメータ(媒質・地面アルベド)が変わったときだけ作り直す
//   SkyView 192x108                                … カメラ高度・光源の向き・パラメータが変わったときだけ
//   AerialPerspective 32x32x32(L と T の 2 枚)  … 毎フレーム(カメラの向きに追従する froxel)
//   SkyCube 64x64x6                                … IBL 再ベイクのときだけ(SkyView から環境キューブを作る)
//
// 使い方(Application 側): BuildFrame → Update(LUT 更新)→ [RecordSkyCube] → DrawSky / DrawAerialPerspective。
// ディスクリプタ: SRV ブロック 7 本 [0]=T [1]=MS [2]=SkyView [3]=ApL [4]=ApT [5]=SkyCube(TextureCube) [6]=パラメータ(ByteAddressBuffer。PT が引く)
//                UAV ブロック 6 本 [0..4] は同じ順、[5]=SkyCube(Texture2DArray)
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/atmosphere/AtmosphereShared.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class ConstantBuffer;

class AtmosphereRenderer
{
public:
    static constexpr u32 kSkyCubeSize = 64;
    static constexpr u32 kSkyCubeMips = 4;   // 64 / 32 / 16 / 8（IBL の拡散・スペキュラが mip を引く）

    // GPU 計測のスコープ(自前のタイムスタンプ。GpuTimer のスコープ表には足さない)
    enum Scope : u32
    {
        ScopeTransMs = 0,   // Transmittance + MultiScattering(パラメータ変更時のみ)
        ScopeSkyView,       // Sky-View LUT
        ScopeAp,            // Aerial-Perspective LUT
        ScopeSkyCube,       // 環境キューブ生成(IBL 再ベイク時のみ)
        ScopeSkyDraw,       // 空パス
        ScopeApComposite,   // AP 合成
        ScopeIblIrr,        // IBL: 拡散(irradiance)畳み込み
        ScopeIblPre,        // IBL: スペキュラ(prefilter)畳み込み
        ScopeCount
    };
    static const char* ScopeName(u32 s);

    bool Initialize(GraphicsDevice& device, ID3D12CommandQueue* queue, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void Shutdown();
    void RecreatePipelines(GraphicsDevice& device);
    bool IsReady() const { return m_ready; }

    // LUT を更新する(compute)。g の内容で T/MS(媒質ハッシュが変わったとき)・SkyView(鍵が変わったとき)・AP(毎回)を作り直す。
    // 呼び出し前に何も張っていなくてよい(ヒープ・RS・PSO を中で張る)。frameIndex はフレームスロット(CB リング・タイマー用)。
    // forceAll = true なら全 LUT を作り直す(シーン読込・決定論撮影の最初)。
    void Update(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const atmosphere::AtmosphereGpuParams& g, bool forceAll);

    // 副ビュー(カメラプレビューなど)用: 定数(行列・カメラ位置だけが違う)を別スロットへ書く。slot = frameIndex + 3(主ビューは frameIndex)。
    void UploadParams(u32 slot, const atmosphere::AtmosphereGpuParams& g);

    // 環境キューブ(SkyView から)を作る。終了時 PIXEL_SHADER_RESOURCE。Update の後に呼ぶこと。
    void RecordSkyCube(ID3D12GraphicsCommandList* cmd, u32 frameIndex);

    // 空パス。呼び出し側が RT・ビューポート・シザー・ディスクリプタヒープを張っておくこと。深度テストは OFF の PSO(DSV 付き)。
    void DrawSky(ID3D12GraphicsCommandList* cmd, u32 frameIndex, D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu);
    // AP 合成。dst = src0 + dst*src1(デュアルソース)。RT のみ(DSV 無し)。深度は SRV で渡す。
    void DrawAerialPerspective(ID3D12GraphicsCommandList* cmd, u32 frameIndex, D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu);

    // ---- 環境キューブ(IBLBaker::Bake の入力 / スカイボックス / PT が読む)----
    ID3D12Resource* SkyCubeResource() const { return m_cube.Get(); }
    u32 SkyCubeSrvIndex() const { return m_srvBlock == 0xFFFFFFFFu ? 0xFFFFFFFFu : m_srvBlock + 5; }
    // PT が bindless で引く SRV ブロックの先頭(T / MS / SkyView / ApL / ApT / cube / params)
    u32 SrvBlockBase() const { return m_srvBlock; }

    // ---- 計測 ----
    // 直近に完了したフレームの各スコープの GPU 時間[ms]。実行しなかったスコープは最後に実行したときの値を保つ(ScopeExecuted で判別)。
    float GetMs(u32 scope) const { return scope < ScopeCount ? m_scopeMs[scope] : 0.0f; }
    bool  ScopeRanRecently(u32 scope) const { return scope < ScopeCount && m_ranLast[scope]; }
    // IBL の段(外部の IBLBaker 呼び出し)を計測に含めるためのフック
    void TimerBegin(ID3D12GraphicsCommandList* cmd, u32 frameIndex, Scope s);
    void TimerEnd(ID3D12GraphicsCommandList* cmd, u32 frameIndex, Scope s);
    // フレーム先頭(フェンス待ちの後)に呼ぶ。このスロットの前回結果を回収する。
    void BeginFrameTimers(u32 frameIndex);

    // 直近の Update で LUT を作り直した回数(テスト・診断用)
    u32 GetUpdateCount(u32 which) const { return which < 4 ? m_updateCount[which] : 0; }

    // テスト・診断用: LUT のリソース。0=Transmittance 1=MultiScattering 2=SkyView 3=ApL 4=ApT 5=SkyCube(状態は SRV(5 は PIXEL_SHADER_RESOURCE)。読み戻しは呼び出し側)
    ID3D12Resource* LutResource(u32 which) const
    {
        switch (which) { case 0: return m_trans.res.Get(); case 1: return m_ms.res.Get(); case 2: return m_sky.res.Get();
                         case 3: return m_apL.res.Get(); case 4: return m_apT.res.Get(); case 5: return m_cube.Get(); default: return nullptr; }
    }

private:
    struct Tex { Microsoft::WRL::ComPtr<ID3D12Resource> res; D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; };
    void Transition(ID3D12GraphicsCommandList* cmd, Tex* const* texes, u32 n, D3D12_RESOURCE_STATES next);
    void SetCommon(ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool compute, D3D12_GPU_DESCRIPTOR_HANDLE depthSrv);

    bool m_ready = false;
    DescriptorHeap* m_srvHeap = nullptr;
    std::wstring m_shaderDir;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoT, m_psoMs, m_psoSv, m_psoAp, m_psoCube, m_psoCubeMip;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSky, m_psoApComp;

    Tex m_trans, m_ms, m_sky, m_apL, m_apT, m_cubeTex;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cube;   // = m_cubeTex.res(公開用)
    u32 m_srvBlock = 0xFFFFFFFFu;
    u32 m_uavBlock = 0xFFFFFFFFu;
    static constexpr u32 kSrvCount = 7;
    static constexpr u32 kUavCount = 5 + kSkyCubeMips;   // [0..4] LUT / [5..8] 環境キューブの mip 0..3

    std::unique_ptr<ConstantBuffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_paramsBuf;   // PT が引く最新パラメータ(UPLOAD・永続マップ)
    u8* m_paramsMapped = nullptr;

    // 変更検出
    bool m_haveLut = false;
    u64  m_mediaHash = 0;
    struct SvKey { float camAlt = -1, lightDir[3] = {0, 0, 0}, msFactor = 0, lightE0 = 0; u64 media = 0; bool operator==(const SvKey&) const; };
    SvKey m_svKey; bool m_svValid = false;
    u32 m_updateCount[4] = {0, 0, 0, 0};

    // GPU タイマー
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_qheap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_qreadback;
    u64  m_freq = 0;
    static constexpr u32 kFrames = 3;
    u32  m_wrote[kFrames] = {0, 0, 0};   // ビットマスク(スコープ)
    float m_scopeMs[ScopeCount] = {};
    bool  m_ranLast[ScopeCount] = {};
    u32 m_timerFrame = 0;
};

} // namespace dx12e
