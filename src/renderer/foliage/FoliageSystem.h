#pragma once
// ===========================================================================
// 植生 F1: ランタイム（レイヤー収集 → GPU カリング → ExecuteIndirect 描画）。
// ---------------------------------------------------------------------------
//   FoliageLayer コンポーネントを毎フレーム走査し、レイヤーごとに GPU リソース（FoliageCuller::Layer）と描画グループ
//   （種別 × LOD × サブメッシュ）を持つ。描画は既存のメイン RS を使う（DWORD 増分ゼロ）:
//     ・頂点ストリーム slot0 = モデルの頂点 / slot1 = compact されたインスタンス（64B）。ForwardInstanced と同じ流儀。
//     ・PS は Forward.hlsl の PSMain（ディザで包んだもの）＝既存のライティング / 影 / フォグ / IBL / DDGI をそのまま受ける。
//     ・風のフレーム共通値: 本体は PerFrame(b1) の予約領域 [0..1]、深度 / 影 / 速度は専用 CBV を b1 に貼る。レイヤー別は b0 の末尾。
//   ★FoliageLayer が 1 つも無いシーンでは Initialize すらされない（GPU リソース 0・PSO 0・描画パスに 1 命令も足されない）。
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <nlohmann/json_fwd.hpp>

#include "core/Types.h"
#include "renderer/foliage/FoliageCuller.h"
#include "renderer/foliage/FoliageMath.h"
#include "renderer/foliage/SceneWind.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class ResourceManager;
class RootSignature;
class PipelineState;
struct Material;
class Mesh;

namespace foliage
{

// FoliageSystem の GPU 時間（ms。3 フレーム遅れの読み戻し）
struct FoliageGpuMs
{
    f32 cull = 0.0f;        // 全ビューのカリング一式（compute）
    f32 shadowDraw = 0.0f;  // 影カスケードへの描画
    f32 depthDraw = 0.0f;   // カメラの深度 / 速度プリパスへの描画
    f32 mainDraw = 0.0f;    // 本体（Forward+）への描画
    f32 Total() const { return cull + shadowDraw + depthDraw + mainDraw; }
};

struct FoliageStatsLayer
{
    u32 entity = 0;
    std::string name;
    u32 instances = 0, chunks = 0;
    u64 gpuBytes = 0;
    u32 visible[kMaxVariants * kMaxLods] = {};   // 主ビューの list ごとの個数（variant * lodStride + lod。容量超過分を含む生の数）
    u32 shadowVisible = 0;                       // 影カスケードの合計
    u32 overflow = 0;                            // 容量超過で描けなかった数（全ビュー合計）
    u32 lodStride = 0, variants = 0;
    u32 visibleTotal = 0;
    u64 trianglesMain = 0;                       // 主ビューで描いた三角形の見積もり（個数 × モデルの三角形数）
    bool ready = false;
    std::string note;                            // 描けない理由（モデルが読めない等）
};

class FoliageSystem
{
public:
    struct Deps
    {
        GraphicsDevice* gdev = nullptr;
        ID3D12CommandQueue* queue = nullptr;     // タイムスタンプ周波数（null なら GPU 時間 0）
        RootSignature* rootSig = nullptr;        // メインのルートシグネチャ（描画はこれで行う。DWORD 増分ゼロ）
        DescriptorHeap* srvHeap = nullptr;
        ResourceManager* resources = nullptr;
        std::wstring shaderDir;
        u32 frameCount = 3;
        DXGI_FORMAT sceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        DXGI_FORMAT velocityFormat = DXGI_FORMAT_R16G16_FLOAT;
        DXGI_FORMAT gbufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        std::string assetsDir;                   // モデルの解決に使う
    };

    FoliageSystem();
    ~FoliageSystem();
    FoliageSystem(const FoliageSystem&) = delete;
    FoliageSystem& operator=(const FoliageSystem&) = delete;

    bool Initialize(const Deps& d, std::string* err = nullptr);
    void Shutdown();
    bool IsReady() const { return m_ready; }

    // ---- フレームごと ----
    struct FrameIn
    {
        entt::registry* reg = nullptr;
        const SceneWind* wind = nullptr;
        f32 time = 0.0f;            // ゲームクロックの総時間（決定論キャプチャでは固定値）
        f32 prevTime = 0.0f;        // 前フレームの総時間
        bool physicalLightUnits = false;   // Q2: 物理ライティング単位（PS の差し替え）
        u32 frameSlot = 0;          // 0..frameCount-1
        u64 frameId = 0;
        ID3D12GraphicsCommandList* cmd = nullptr;   // レイヤー作成のアップロードコピーの記録先
        ID3D12DescriptorHeap* appHeap = nullptr;    // カリング後に張り直すヒープ
        std::function<DirectX::XMFLOAT4X4(entt::entity)> worldOf;   // レイヤーの世界行列（行ベクトル規約。親子込み）
    };
    // レイヤーの収集 / GPU リソースの作成・更新 / 風のフレーム値の計算 / 前回の統計の読み戻し。★フレームで 1 回（同じ frameId の 2 回目は何もしない）。
    // 戻り値: このフレーム描くレイヤーがあるか。
    bool BeginFrame(const FrameIn& in);
    bool HasDrawableLayers() const { return m_drawable > 0; }

    // 風のフレーム共通値（PerFrame の _clusterReserved[0..1] へそのまま書ける）。BeginFrame の後に有効。
    void WindConstants(DirectX::XMFLOAT4& a, DirectX::XMFLOAT4& b) const { a = m_windA; b = m_windB; }

    // ---- ビュー ----
    struct HzbIn
    {
        ID3D12Resource* resource = nullptr;   // 前フレームの HZB（NON_PIXEL_SHADER_RESOURCE で読める状態）
        u32 mips = 0;
        f32 width = 0, height = 0;
        DirectX::XMFLOAT4X4 prevVP{};         // HZB を作った時のジッタ付き viewProj
    };
    struct CascadeIn { DirectX::XMFLOAT4X4 viewProj; bool valid = false; };
    struct ViewIn
    {
        DirectX::XMFLOAT3 camPos{0, 0, 0};
        DirectX::XMFLOAT4X4 viewProj{};       // 主ビューの視錐台（ジッタなしでも可）
        HzbIn hzb;                            // resource == null なら HZB カリング無し
        bool shadows = false;                 // 影カスケードもカリングする
        CascadeIn cascade[2];                 // 影カスケード 0, 1（描くのは近距離の 2 枚まで）
    };
    // カリング一式（主ビュー + 影カスケード）を記録する。RenderView の先頭（影パスより前）で呼ぶ。
    void Cull(ID3D12GraphicsCommandList* cmd, const ViewIn& v, const FrameIn& f);

    // ---- 描画 ----
    // 本体（Forward+ の不透明パス内）。depthLEqual = 深度プリパスが走ったか（PSO の深度比較を LESS_EQUAL にする）。
    void DrawMain(ID3D12GraphicsCommandList* cmd, const DirectX::XMFLOAT4X4& viewProj, bool depthLEqual);
    // カメラの深度プリパス。velocity = 速度 + G-Buffer も書くモード（prevVP = 前フレームのジッタなし VP、jitterNdc = 今フレームの NDC ジッタ）。
    void DrawDepth(ID3D12GraphicsCommandList* cmd, const DirectX::XMFLOAT4X4& viewProjJittered, bool velocity,
                   const DirectX::XMFLOAT4X4& prevVP, const DirectX::XMFLOAT2& jitterNdc);
    // 影カスケード 0 / 1 のスライス（vp = そのスライスの viewProj）。
    void DrawShadow(ID3D12GraphicsCommandList* cmd, u32 cascade, const DirectX::XMFLOAT4X4& viewProj);

    // ---- 統計 / 診断 ----
    FoliageGpuMs GpuMs() const { return m_gpuMs; }
    std::vector<FoliageStatsLayer> Stats() const { return m_stats; }
    nlohmann::json StatsJson() const;
    u64 TotalGpuBytes() const;
    u32 LayerCount() const { return static_cast<u32>(m_layers.size()); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    u32 m_drawable = 0;
    DirectX::XMFLOAT4 m_windA{0, 0, 0, 0}, m_windB{0, 0, 0, 0};
    FoliageGpuMs m_gpuMs;
    std::vector<FoliageStatsLayer> m_stats;
    struct LayerRt;
    std::unordered_map<u32, std::unique_ptr<LayerRt>> m_layers;
};

} // namespace foliage
} // namespace dx12e
