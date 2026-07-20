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

    // カスタムシェーダー用(Sprite2D::shaderPath/effectValue/shaderParams から供給。既定は無効)。
    float effect     = 0.0f;             // 頂点属性として補間される汎用進捗値(TEXCOORD1)
    DirectX::XMFLOAT4 params{0, 0, 0, 0}; // 汎用パラメーター4つ(TEXCOORD2。バッチを壊さず sprite 単位)
    void* customPso  = nullptr;          // 解決済み ID3D12PipelineState*。nullptr=既定 m_worldPso を使う
};

class SpriteRenderer
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                    DXGI_FORMAT rtvFormat, const std::wstring& shaderDir);

    void BeginFrame();
    void Submit(const SpriteDesc& s);
    bool HasAny() const { return !m_sprites.empty(); }

    // 呼び出し側で対象 RTV を先にバインドしておくこと。time はカスタムシェーダー向け(既定Sprite.hlslは無視)。
    void Render(ID3D12GraphicsCommandList* cmd, u32 screenW, u32 screenH, float time = 0.0f);

    // --- ワールド空間 2D（カメラ連動・layer ソート・アルファブレンド・深度テスト）---
    // HUD 経路（上）とは別の PSO/頂点バッファ/リストで隔離。Initialize の後に呼ぶ。
    // sceneRtvFormat はシーン RT のフォーマット（HDR の kSceneColorFormat）、
    // depthFormat はシーン深度バッファのフォーマット（D32_FLOAT 等）。
    void InitializeWorld(GraphicsDevice& device, DXGI_FORMAT sceneRtvFormat,
                         DXGI_FORMAT depthFormat, const std::wstring& shaderDir);

    // シェーダーホットリロード用。HUD/world 両 PSO を作り直す(ルートシグネチャ/頂点バッファは不変)。
    void RecreatePipelines(GraphicsDevice& device);
    // フレーム先頭で 1 回呼ぶ。同一フレーム内で RenderWorld を複数回（メイン＋カメラプレビュー等）
    // 呼んでも頂点バッファが衝突しないよう、書き込みカーソルをリセットする。
    void BeginWorldVertexFrame();
    void BeginWorldFrame();
    void SubmitWorld(const WorldSpriteDesc& s);
    bool HasAnyWorld() const { return !m_worldSprites.empty(); }
    // viewProj=アクティブカメラの ViewProj。camRight/camUp=ビルボード展開用のカメラ右/上ベクトル。
    // time はカスタムシェーダー向け(既定Sprite.hlslは無視)。
    // 呼び出し側で対象 RTV(sceneRT/preview)＋深度 DSV をバインド済みのこと（PSO は深度テスト有効）。
    void RenderWorld(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                     DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp, float time = 0.0f);

    // Sprite2D カスタムシェーダー用(Application::EnsureCustomSpritePso が使う)。
    // ルートシグネチャ・頂点入力レイアウトは HUD/world 共通でこれ 1 種類のみ。
    ID3D12RootSignature* GetRootSignature() const { return m_rootSig.Get(); }
    static const D3D12_INPUT_ELEMENT_DESC* GetInputLayout(u32* countOut);

private:
    // POSITION(12) + TEXCOORD0(8) + COLOR0(16) + TEXCOORD1(4) + TEXCOORD2(16) = 56 バイト。
    // 末尾の effect/params はカスタムシェーダー向けの汎用値(既定Sprite.hlslは未使用)。
    struct Vertex { DirectX::XMFLOAT3 pos; DirectX::XMFLOAT2 uv; DirectX::XMFLOAT4 col; float effect;
                    DirectX::XMFLOAT4 params; };

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

    // RecreatePipelines 用に保持
    std::wstring m_shaderDir;
    DXGI_FORMAT  m_rtvFormat      = DXGI_FORMAT_UNKNOWN;  // HUD PSO の出力先
    DXGI_FORMAT  m_sceneRtvFormat = DXGI_FORMAT_UNKNOWN;  // world PSO の出力先
    DXGI_FORMAT  m_depthFormat    = DXGI_FORMAT_UNKNOWN;  // world PSO の深度

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
