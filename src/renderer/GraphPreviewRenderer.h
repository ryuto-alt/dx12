#pragma once
// ============================================================================
// GraphPreviewRenderer — マテリアルグラフ G2c: エディタのライブプレビューとノード内サムネイルの描画（GPU 側）
//
//   ★エディタ専用（GameRuntime では作らない）。メインシーンの描画・状態には触らない（専用の RT / 定数 / 環境。決定論スクショに影響しない）。
//   ★実レンダラで描く: メイン RS + GraphMaterialSystem の PSO（PreviewLite = UNO_SHADE_LITE。既存のライティング関数そのまま・影 / クラスタ / GI を外しただけ）。
//     プレビュー専用インスタンス（GraphMaterialSystem::SetInstanceKind / SetInstanceCompiled）で描く。
//
//   1. メインプレビュー: 球 / 平面 / 円柱 / 立方体 + 環境 3 種（スタジオ / 屋外 / 暗所。手続き生成のキューブ → IBLBaker でベイク）+ 平行光。
//        RGBA16F の RT（リニア HDR。固定 512x512 の左上 pixels x pixels だけ使う）→ ACES + ガンマの表示テクスチャ（RGBA8。ImGui に貼る）。
//        背景は勾配 + ビネット（表示パスが深度から物体の有無を見て描く）。
//   2. ノード内サムネイル: 64px のタイルを並べたアトラス（RGBA8 512x512 = 8x8 タイル）。NodeLdr インスタンス（部分グラフ出力）を全画面の四角で評価。
//        テスト用に RGBA32F の 1 タイル（NodeRaw。値そのまま）も描ける。
//
//   スレッド: メイン（描画）スレッド専用。コマンドリストは呼び出し側が開いている物を渡す。
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"
#include "renderer/GraphMaterialSystem.h"
#include "renderer/matgraph/PreviewLogic.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class ResourceManager;
class RootSignature;
class Mesh;
class IBLBaker;

class GraphPreviewRenderer
{
public:
    static constexpr u32 kMainSize   = 512;    // メインプレビューの RT の 1 辺（実際に描く範囲は pixels x pixels）
    static constexpr u32 kTilePixels = 64;     // ノード内サムネイル 1 枚の 1 辺
    static constexpr u32 kAtlasCells = 8;      // アトラスの 1 辺のタイル数（8 x 8 = 64 枚）
    static constexpr u32 kAtlasSize  = kTilePixels * kAtlasCells;

    struct InitDesc
    {
        GraphicsDevice*       device = nullptr;
        RootSignature*        rootSignature = nullptr;   // メイン RS（バインドレス）
        ResourceManager*      resources = nullptr;
        DescriptorHeap*       srvHeap = nullptr;
        GraphMaterialSystem*  graphs = nullptr;
        std::wstring          shaderDirW;                // IBLBaker の .cso の場所
    };

    GraphPreviewRenderer();
    ~GraphPreviewRenderer();
    GraphPreviewRenderer(const GraphPreviewRenderer&) = delete;
    GraphPreviewRenderer& operator=(const GraphPreviewRenderer&) = delete;

    bool Initialize(const InitDesc& d);
    void Shutdown();
    bool IsValid() const { return m_valid; }

    // ---- メインプレビュー --------------------------------------------------------------------
    // instanceKey: GraphMaterialSystem に SetInstanceKind(PreviewLite) + SetInstanceCompiled した一時キー。
    // pixels: 描く 1 辺（128..512）。戻り値 = 描けたか（false = 材質がまだ使えない → 背景だけ描く）。表示テクスチャは常に更新する。
    bool RenderMain(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const std::string& instanceKey,
                    const matgraph::PreviewSettings& settings, u32 pixels, float timeSec);
    // ImGui へ渡すテクスチャ（GPU ディスクリプタハンドルの ptr）と、使う範囲の uv（0..pixels/kMainSize）
    u64   MainDisplayHandle() const;
    float MainUv1() const { return static_cast<float>(m_lastPixels) / static_cast<float>(kMainSize); }
    u32   LastPixels() const { return m_lastPixels; }

    // ---- ノード内サムネイル ------------------------------------------------------------------
    struct TileRequest
    {
        int         tile = 0;          // 0 .. 63
        std::string instanceKey;       // NodeLdr インスタンス
    };
    // 材質が使える（Resolve 成功）タイルだけ描く。描けた枚数を返す
    //   drawn（任意）= 描けたタイル番号を返す（描けなかった分は呼び出し側が次のフレームにもう一度頼む）
    int  RenderTiles(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const std::vector<TileRequest>& reqs, std::vector<int>* drawn = nullptr);
    // タイルを暗い市松で埋める（未使用 / 失敗のタイルの初期値）
    void ClearTile(ID3D12GraphicsCommandList* cmd, int tile);
    u64  AtlasHandle() const;
    ID3D12Resource* AtlasResource() const;   // テスト用（状態は PIXEL_SHADER_RESOURCE）
    static void TileUv(int tile, float& u0, float& v0, float& u1, float& v1);

    // 数値テスト用: NodeRaw インスタンスを RGBA32F の 64x64（RGBA = 値そのまま）へ描く。描けたら true。
    // 結果のリソースは PIXEL_SHADER_RESOURCE 状態（読み戻しは呼び出し側が COPY_SOURCE へ遷移して行う）。
    bool RenderRawTile(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const std::string& instanceKey);
    ID3D12Resource* RawTileResource() const { return m_rawTile.Get(); }

    // 数値テスト用: メインプレビューのリニア HDR（RGBA16F）と表示（RGBA8）。状態は PIXEL_SHADER_RESOURCE
    ID3D12Resource* MainColorResource() const { return m_color.Get(); }
    ID3D12Resource* MainDisplayResource() const { return m_display.Get(); }

    // 環境をベイクする（初回の RenderMain が自動で呼ぶ。テストが先に呼んでもよい）。専用の即時実行で完了まで待つ
    bool EnsureEnvironments();
    // テスト / 比較用: 環境の指定（"flat" = IBL なし・環境光の定数 + 平行光 = 実シーンの最小構成と同じ条件）
    struct FlatLighting
    {
        DirectX::XMFLOAT3 lightDir  = {-0.4f, -0.8f, 0.45f};   // ライトが進む向き（シェーダーの lightDir）
        DirectX::XMFLOAT3 lightColor = {1.0f, 0.96f, 0.9f};
        float             ambient = 0.35f;
    };
    void SetFlatLightingOverride(const FlatLighting* f);   // null で解除（既定の 3 環境に戻る）

    // キャプチャ（MCP / 自動テスト）: 直近の RenderMain の結果（pixels x pixels）を CPU へ。linear = true でリニア HDR（RGBA16F）/ false で表示（RGBA8）。
    //   RequestCapture は RenderMain の直後に同じコマンドリストへ積む。GPU が終わるまで（AdvanceFrame を数回呼ぶまで）PollCapture は false。
    bool RequestCapture(ID3D12GraphicsCommandList* cmd, bool linear);
    bool PollCapture(std::vector<uint8_t>& data, u32& w, u32& h, u32& bytesPerPixel, bool& linear);
    void AdvanceFrame() { ++m_frameCounter; }   // 毎フレーム 1 回（RenderGpu の頭）

    // 統計
    u32 MainRenders() const { return m_mainRenders; }
    u32 TileRenders() const { return m_tileRenders; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    bool m_valid = false;
    u32  m_lastPixels = 256;
    u64  m_frameCounter = 0;
    struct Capture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> rb;
        u32 w = 0, h = 0, bpp = 0, rowPitch = 0;
        u64 issuedAt = 0;
        bool linear = false, active = false;
    } m_capture;
    u32  m_mainRenders = 0, m_tileRenders = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_color, m_display, m_rawTile;
};

} // namespace dx12e
