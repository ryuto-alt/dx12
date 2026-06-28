#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;

// スクリーン空間の 2D スプライト描画（テクスチャ付きクアッド）。
// Submit でリクエストを溜め、Render で 1 パスにまとめて描く（テクスチャ単位でバッチ）。
// HUD / タイトル / ゲーム内 2D の土台。
struct SpriteDesc
{
    DirectX::XMFLOAT2 pos{0, 0};          // 左上ピクセル座標
    DirectX::XMFLOAT2 size{100, 100};     // ピクセルサイズ
    DirectX::XMFLOAT2 uvMin{0, 0};
    DirectX::XMFLOAT2 uvMax{1, 1};
    DirectX::XMFLOAT4 color{1, 1, 1, 1};  // 乗算カラー
    u32   srvIndex = 0;                    // m_srvHeap 上のテクスチャ SRV インデックス
    float layer    = 0.0f;                 // 小さいほど奥（描画順）
};

// ワールド空間 2D スプライト（カメラ連動）。エンティティのワールド行列で 4 隅を変換するため
// 3D 空間の任意位置・向き・スケールに置ける（純2D＝正射カメラ正対 / 3D ゲーム内の板やマーカー）。
// billboard=true なら向きを無視し常にカメラへ正対。前後は layer（小さいほど奥）で CPU 安定ソート。
struct WorldSpriteDesc
{
    DirectX::XMFLOAT4X4 world;            // エンティティのワールド行列（位置/回転/スケール）
    DirectX::XMFLOAT2   size{1, 1};       // ローカルのクアッド寸法（world のスケールが乗る）
    DirectX::XMFLOAT2   uvMin{0, 0};
    DirectX::XMFLOAT2   uvMax{1, 1};
    DirectX::XMFLOAT4   color{1, 1, 1, 1};
    u32   srvIndex  = 0;
    float layer     = 0.0f;
    bool  billboard = false;             // true: 常にカメラへ正対（向きを無視）
};

class SpriteRenderer
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                    DXGI_FORMAT rtvFormat, const std::wstring& shaderDir);

    void BeginFrame();
    void Submit(const SpriteDesc& s);
    bool HasAny() const { return !m_sprites.empty(); }

    // 呼び出し側で対象 RTV を先にバインドしておくこと。
    void Render(ID3D12GraphicsCommandList* cmd, u32 screenW, u32 screenH);

    // --- ワールド空間 2D（カメラ連動・layer ソート・アルファブレンド・深度テスト）---
    // HUD 経路（上）とは別の PSO/頂点バッファ/リストで隔離。Initialize の後に呼ぶ。
    // sceneRtvFormat はシーン RT のフォーマット（HDR の kSceneColorFormat）、
    // depthFormat はシーン深度バッファのフォーマット（D32_FLOAT 等）。
    void InitializeWorld(GraphicsDevice& device, DXGI_FORMAT sceneRtvFormat,
                         DXGI_FORMAT depthFormat, const std::wstring& shaderDir);
    // フレーム先頭で 1 回呼ぶ。同一フレーム内で RenderWorld を複数回（メイン＋カメラプレビュー等）
    // 呼んでも頂点バッファが衝突しないよう、書き込みカーソルをリセットする。
    void BeginWorldVertexFrame();
    void BeginWorldFrame();
    void SubmitWorld(const WorldSpriteDesc& s);
    bool HasAnyWorld() const { return !m_worldSprites.empty(); }
    // viewProj=アクティブカメラの ViewProj。camRight/camUp=ビルボード展開用のカメラ右/上ベクトル。
    // 呼び出し側で対象 RTV(sceneRT/preview)＋深度 DSV をバインド済みのこと（PSO は深度テスト有効）。
    void RenderWorld(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                     DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp);

private:
    struct Vertex { DirectX::XMFLOAT3 pos; DirectX::XMFLOAT2 uv; DirectX::XMFLOAT4 col; };

    static constexpr u32 kMaxSprites  = 4096;
    static constexpr u32 kMaxVertices = kMaxSprites * 6;
    // in-flight 多重度（SwapChain::kFrameCount と一致）。頂点バッファをこの数の区画に
    // 分割して巡回し、GPU が前フレームで読んでいる区画を CPU が上書きしない（チラつき防止）。
    static constexpr u32 kFrameCount  = 3;
    // world 頂点バッファは 1 フレームに複数パス（メイン＋プレビュー）書き込むため余裕を持たせる。
    static constexpr u32 kWorldMaxVerts = kMaxVertices * 2;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};

    DescriptorHeap* m_srvHeap = nullptr;
    std::vector<SpriteDesc> m_sprites;
    u32  m_frameIdx = 0;   // HUD 頂点バッファの巡回区画インデックス
    bool m_initialized = false;

    // ワールド空間経路（HUD と隔離）。RootSig は m_rootSig を共有。
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_worldPso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_worldVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_worldVbView{};
    std::vector<WorldSpriteDesc>                m_worldSprites;
    u32  m_worldVbCursor   = 0;        // フレーム内の頂点書き込み位置（BeginWorldVertexFrame で 0）
    u32  m_worldFrameIdx   = 0;        // world 頂点バッファの巡回区画インデックス（フレーム間）
    bool m_worldInitialized = false;
};

} // namespace dx12e
