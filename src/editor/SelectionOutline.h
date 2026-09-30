#pragma once

// ===========================================================================
// 選択 / ホバーの輪郭（エディタ専用の後処理。フェーズ 1a）
// ---------------------------------------------------------------------------
// ★ゲーム・Play・screenshot_final には影響しない:
//     ・呼ぶのは Application::RenderViewportOverlays の「最終画の撮影（CaptureFinalBackBufferRegion）より後・
//       ImGui より前」だけ。エディタモードで、`ctx.vpPrefs.outline` が ON の時だけ。
//     ・選択も無ければ 1 命令も記録しない（＝要求が無いフレームの絵はこれまでと 1 ビットも変わらない）。
// ★ピッキングは CPU の三角形精密（ScenePick）のまま。ここは「見せる」だけ。
// ★メインのルートシグネチャ（64 DWORD 中残り 2）には触れない。専用ルートシグネチャ 38 DWORD
//   （詳細は shaders/editor/SelectionOutline.hlsl）。
//
// 方式: 1) 選択（R）/ ホバー（G）の物を R8G8 のマスクへ塗る（深度なし・両面・MAX ブレンド）
//       2) マスクの外側の各画素で最寄りのマスク画素までの距離を探索し、芯 + 光の縁を
//          アクセント色でバックバッファへ重ねる（シザーは対象の投影 AABB + 探索半径だけ）。
// ===========================================================================

#include "core/Types.h"
#include "renderer/DrawItem.h"   // DrawItem / entt

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <string>
#include <vector>

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class EditorContext;

class SelectionOutlinePass
{
public:
    // シェーダ（.cso）を読んでルートシグネチャと PSO を作る。SRV を 1 つ srvHeap から確保する
    // （マスクの読み取り用。作り直しても同じ番号を使い回す＝ディスクリプタは増えない）。失敗は例外。
    void Initialize(GraphicsDevice& device, DescriptorHeap& srvHeap, DXGI_FORMAT backBufferFormat,
                    const std::wstring& shaderDir);
    bool IsReady() const { return m_psoComposite != nullptr; }

    struct Target
    {
        const DrawItem*              item = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE  bones{};   // スキンドのボーン SRV（静的は無視される）
    };

    struct Args
    {
        ID3D12GraphicsCommandList*     cmd = nullptr;
        ID3D12Resource*                backBuffer = nullptr;      // RENDER_TARGET 状態の契約
        D3D12_CPU_DESCRIPTOR_HANDLE    backBufferRtv{};
        u32                            vpX = 0, vpY = 0, vpW = 0, vpH = 0;   // 3D ビューポート（バックバッファの px）
        DirectX::XMMATRIX              viewProj;                  // ジッタなし
        ID3D12DescriptorHeap*          srvHeap = nullptr;         // シェーダ可視ヒープ（マスクの SRV / ボーン）
        D3D12_GPU_DESCRIPTOR_HANDLE    defaultSrv{};              // 静的メッシュ用のダミー（ボーン表の空き埋め）
        const std::vector<Target>*     selected = nullptr;
        const std::vector<Target>*     hovered = nullptr;
        float                          scale = 1.0f;              // DPI 倍率（縁の太さ・探索半径）
        float                          selColor[4] = { 0.38f, 0.71f, 1.0f, 1.0f };
        float                          hoverColor[4] = { 0.85f, 0.92f, 1.0f, 0.55f };
    };
    // 記録。何も描く物が無ければ何もしない（false を返す）。true のときは呼び出し側が
    // ImGui のためにルートシグネチャ / RT / ビューポートをメインの状態へ戻すこと。
    bool Record(const Args& a);

    // ---- ワイヤ表示モード（ビューモード「ワイヤ」）----
    // ビューポート矩形を単色で塗り、全描画アイテムを FILL_MODE_WIREFRAME で重ねる（エディタ専用・最終画の撮影より後）。
    struct WireArgs
    {
        ID3D12GraphicsCommandList*     cmd = nullptr;
        ID3D12Resource*                backBuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE    backBufferRtv{};
        u32                            vpX = 0, vpY = 0, vpW = 0, vpH = 0;
        DirectX::XMMATRIX              viewProj;
        ID3D12DescriptorHeap*          srvHeap = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE    defaultSrv{};
        const std::vector<Target>*     items = nullptr;
        float                          fill[4] = { 0.04f, 0.045f, 0.06f, 1.0f };   // 下地
        float                          line[4] = { 0.62f, 0.78f, 1.0f, 0.55f };    // 線
    };
    bool RecordWire(const WireArgs& a);

    // 直近の Record の統計（診断・テスト用）
    u32 LastMaskDraws() const { return m_lastDraws; }
    u32 LastScissorPixels() const { return m_lastScissorPx; }

private:
    void EnsureMask(u32 w, u32 h);

    Microsoft::WRL::ComPtr<ID3D12Device>        m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMask, m_psoMaskSkinned, m_psoComposite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoWire, m_psoWireSkinned, m_psoFill;

    Microsoft::WRL::ComPtr<ID3D12Resource>       m_mask;        // R8G8_UNORM（使っていない間は PIXEL_SHADER_RESOURCE）
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    u32 m_maskW = 0, m_maskH = 0;
    DescriptorHeap* m_srvHeapOwner = nullptr;
    u32 m_srvIndex = 0xFFFFFFFFu;
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu{};

    u32 m_lastDraws = 0, m_lastScissorPx = 0;
};

// Application::RenderViewportOverlays から 1 回呼ぶ入口（エディタ専用・最終画の撮影より後・ImGui より前）。
// 中で SelectionOutlinePass を遅延生成し（初回だけ SRV を 1 つ確保）、選択 / ホバーの輪郭と
// ワイヤ表示モードを記録する。何も記録しなかったら false（呼び出し側は状態を戻さなくてよい）。
struct EditorViewFrame
{
    GraphicsDevice*              device = nullptr;
    DescriptorHeap*              srvHeap = nullptr;
    DXGI_FORMAT                  backBufferFormat = DXGI_FORMAT_UNKNOWN;
    std::wstring                 shaderDir;
    ID3D12GraphicsCommandList*   cmd = nullptr;
    ID3D12Resource*              backBuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE  backBufferRtv{};
    u32                          vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    DirectX::XMMATRIX            viewProj;
    u32                          frameIndex = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE  whiteSrv{};
    const entt::registry*        reg = nullptr;
    const std::vector<DrawItem>* items = nullptr;
    float                        uiScale = 1.0f;
};
bool RecordEditorViewOverlays(EditorContext& ctx, const EditorViewFrame& f);

// 選択の物（自分 + 子孫）に該当する描画アイテムを集める。ヒエラルキーで親を選ぶと子のメッシュも光る。
// reg / drawItems は Application の今フレームのもの。selected の各エンティティについて、
// DrawItem::e が selected 自身か、その祖先に selected が居るものを out へ足す。
void CollectOutlineTargets(const entt::registry& reg, const std::vector<DrawItem>& drawItems,
                           const std::vector<entt::entity>& roots, std::vector<const DrawItem*>& out,
                           size_t maxItems = 4096);

} // namespace dx12e
