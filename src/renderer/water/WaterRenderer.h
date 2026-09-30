#pragma once
// ===========================================================================
// WaterRenderer — 水面 W1 の専用パス（不透明の後・半透明の前）。
// ---------------------------------------------------------------------------
// ・専用ルートシグネチャ（メイン RS の DWORD 増分 0。メインの材質テーブルは PS 専用で VS が波を読めないため）
// ・不透明シーンの色と深度のコピー（RGBA16F + R32F。1080p で約 24 MB）を読み、屈折 + 吸収 + 反射 + 泡 + 岸のフェードを描く。深度も書く
// ・水面メッシュ = カメラ中心の入れ子リング（クリップマップ風。各レベル 96x96 セル、セルは 2 倍ずつ粗くなる。世界座標に吸着するので波が泳がない）
//   VS でゲルストナー変位、PS で解析法線 + 手続きディテール。外周の奇数頂点は粗いレベルの辺の中点へ寄せて亀裂を無くす
// ・水中（カメラが水面下）: 不透明シーンへ全画面 Beer-Lambert 減衰 + 内部散乱、水面は下から（スネルの窓 + 全反射）
// ★WaterBody が 1 つも無いシーンでは Initialize すらされない（GPU リソース 0・PSO 0・描画コマンド 0）。
//
// 使い方（Application 側）: BeginFrame（収集 + カリング + パラメータ詰め。フレームで 1 回）→ 不透明の Forward+ 後に Record。
// ★Record の前後で呼び出し側が RootSig / PSO / RT を張り直すこと（本クラスはメイン RS を壊す）。RenderPass.h の状態の契約と同じ。
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <entt/entt.hpp>

#include "core/Types.h"
#include "ecs/Components.h"
#include "renderer/water/WaterMath.h"
#include "renderer/water/WaterShared.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class CommandList;
struct TrackedState;

namespace water
{

// 水域 1 個ぶんの入力（収集は呼び出し側。レンダラは ECS を知らない）
struct BodyIn
{
    entt::entity entity = entt::null;
    WaterBody body;
    DirectX::XMFLOAT4X4 world{};   // 行ベクトル規約
};

struct FrameIn
{
    std::vector<BodyIn> bodies;
    f32 totalTime = 0.0f;                  // 決定論クロックの総時間
    u32 frameSlot = 0;                     // 0..2
    u64 frameId = 0;
    DirectX::XMFLOAT4X4 viewProj{};        // ジッタなし（カリング用）
    DirectX::XMFLOAT4X4 viewProjJ{};       // ジッタあり（描画用）
    DirectX::XMFLOAT4X4 proj{};            // ジッタなしの射影（深度の線形化に _33 / _43 を使う）
    DirectX::XMFLOAT3 camPos{};
    f32 nearZ = 0.1f, farZ = 1000.0f;
    u32 width = 0, height = 0;             // レンダー解像度（= シーン RT のサイズ）
    bool physicalLights = false;           // Q2: 物理ライティング単位の PS を使う
    ID3D12GraphicsCommandList* cmd = nullptr;   // 初回のアップロードコピーの記録先
};

// Record が必要とするもの（メインの Forward が張ったハンドルを専用 RS へ張り直す）
struct PassIn
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    CommandList* wrap = nullptr;
    RenderTarget* sceneRT = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    ID3D12Resource* depthRes = nullptr;
    TrackedState* depthState = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS perFrameCB = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE csm{};
    D3D12_GPU_DESCRIPTOR_HANDLE punctual{};
    bool hasIbl = false;
    D3D12_GPU_DESCRIPTOR_HANDLE ibl{};
    bool hasCluster = false;
    D3D12_GPU_DESCRIPTOR_HANDLE cluster{};
    u32 width = 0, height = 0;
};

struct WaterStats
{
    u32 bodies = 0;          // 描いた水域
    u32 rings = 0;           // 描いたリング（ドロー）の数
    u64 triangles = 0;
    bool underwater = false; // 水中表現が走った
    f32 gpuMsTotal = 0.0f;   // 水パス全体（コピー + 水中 + 描画）
    f32 gpuMsCopy = 0.0f;    // 色 + 深度のコピー
    f32 gpuMsUnder = 0.0f;   // 水中の全画面パス
    f32 gpuMsDraw = 0.0f;    // 水面メッシュ
    u64 gpuBytes = 0;
    u32 culledBodies = 0;
};

class WaterRenderer
{
public:
    struct Deps
    {
        GraphicsDevice* gdev = nullptr;
        ID3D12CommandQueue* queue = nullptr;     // タイムスタンプ周波数
        DescriptorHeap* srvHeap = nullptr;
        std::wstring shaderDir;
        u32 frameCount = 3;
        DXGI_FORMAT sceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    };

    WaterRenderer();
    ~WaterRenderer();
    WaterRenderer(const WaterRenderer&) = delete;
    WaterRenderer& operator=(const WaterRenderer&) = delete;

    bool Initialize(const Deps& d, std::string* err = nullptr);
    void Shutdown();
    bool IsReady() const { return m_ready; }
    // シェーダのホットリロード / ライティング単位の切替（PS だけ作り直す）
    bool RecreatePipelines(bool physical);

    // フレームで 1 回（同じ frameId の 2 回目は何もしない）。戻り値 = このフレーム描く水域の数。コピー先のサイズ変更・初回アップロードもここ。
    u32 BeginFrame(const FrameIn& in);
    u32 DrawableBodies() const { return m_drawCount; }
    // 主ビューの不透明の直後に呼ぶ。色 + 深度のコピー → 水中の全画面 → 水面メッシュ。
    // 終了時: RT = sceneRT + dsv（RENDER_TARGET / DEPTH_WRITE）・専用 RS が張られている。
    void Record(const PassIn& p);

    const WaterStats& Stats() const { return m_stats; }
    // フレーム先頭（フェンス待ちの後）に呼ぶ。このスロットの前回のタイマー結果を回収する。
    void BeginFrameTimers(u32 frameSlot);

    // カメラが水面下に入っている水域の添字（-1 = なし）。BeginFrame の後に有効
    int UnderwaterBody() const { return m_underBody; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    u32 m_drawCount = 0;
    int m_underBody = -1;
    WaterStats m_stats;
};

}   // namespace water
}   // namespace dx12e
