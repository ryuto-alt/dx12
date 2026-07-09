#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <directx/d3d12.h>

#include "core/Types.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "resource/ShaderRuntimeCompiler.h"

namespace dx12e
{

class GraphicsDevice;
class CommandList;
class Mesh;
class PipelineState;
class ResourceManager;

enum class MaterialPreviewShape { Sphere, Plane };

// マテリアルエディタの3Dプレビュー(球体/平面)描画を担う。メインのForward.hlsl(9スロット
// RootSignature、CSM/IBL/SSAO込み)とは完全に独立した専用の小さいパイプライン
// (shaders/forward/MaterialPreview.hlsl、固定2灯スタジオライト)を持つ。
// メインシーンの状態(ライト/シャドウ/IBL等)には一切依存しない=常に一定の見た目でプレビューできる。
class MaterialPreviewRenderer
{
public:
    // パスは assets 相対(PathResolver::AssetsDir() 基準)。空 = 未使用/デフォルトへフォールバック。
    // 解決(GetOrLoadTexture)は Render() が毎フレーム行う(MaterialAssetManager::LoadInto と同じ方針)。
    struct DrawInput
    {
        std::string albedoPath;
        std::string normalPath;
        std::string metalRoughnessPath;
        f32 metallic  = 1.0f;
        f32 roughness = 1.0f;
    };

    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, ResourceManager* resourceManager);
    ~MaterialPreviewRenderer();
    bool IsValid() const { return m_valid; }

    // オフスクリーンRTへ描画。cmd/nativeCmdListは呼び出し側が用意した有効なコマンドリスト。
    void Render(CommandList& cmd, const DrawInput& input, MaterialPreviewShape shape,
               f32 camYaw, f32 camPitch, f32 camDist);

    u64 GetPreviewGpuHandle() const;
    static constexpr u32 kPreviewSize = 512;

    // ---- アセットブラウザ用の球体サムネイル(128px、.dxmat単位でキャッシュ) ----
    // キャッシュ済みならGPUハンドルを即返す。無ければレンダリングキューに積んで0を返す
    // (RenderPendingThumbnails が数フレーム後に用意する)。パースに失敗した .dxmat は
    // failed としてキャッシュし、Invalidate されるまで再試行しない。
    u64 GetOrQueueThumbnail(const std::string& dxmatAbsPath);
    // .dxmat 保存時などに呼ぶ。既存サムネイルは解放せず同じテクスチャへ再レンダリングする
    // (ImGuiが前フレームで参照している可能性があるため、リソースの即時解放はしない)。
    void InvalidateThumbnail(const std::string& dxmatAbsPath);
    // フレーム先頭・メインコマンドリストが開いている間に呼ぶ(ModelThumbnailRendererと同じ運用)。
    // 重い画像デコードはワーカースレッドが済ませており、ここではデコード完了分の
    // GPUアップロード+球描画だけを行う(=ローディング画面のスピナーが固まらない)。
    void RenderPendingThumbnails(CommandList& cmd);
    // assets配下の全 .dxmat を走査して未キャッシュ分をキューに積む(プロジェクトロード時の事前生成用)。
    // 積んだ数を返す。進捗表示は GetPendingThumbnailCount() で残数を見る。
    size_t ScanAllMaterials(const std::string& assetsDir);
    size_t GetPendingThumbnailCount() const { return m_pendingKeys.size(); }
    static constexpr u32 kThumbSize = 128;

private:
    void BuildPipeline(GraphicsDevice& device);
    void BuildDepthBuffer(GraphicsDevice& device);
    // プレビュー描画の共通部(テクスチャ解決→SRV→カメラ→クリア→ドロー)。バリアは呼び出し側の責務。
    void DrawSceneTo(CommandList& cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                     u32 size, const DrawInput& input, MaterialPreviewShape shape,
                     f32 camYaw, f32 camPitch, f32 camDist);
    // ↑のさらに内側: SRVブロック確定済みで描くだけ(サムネイルの非同期パスと共用)。
    void DrawSceneWithSrvs(CommandList& cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                           u32 size, u32 srvBlockStart, f32 metallic, f32 roughness,
                           bool hasNormal, bool hasMetalRoughness, MaterialPreviewShape shape,
                           f32 camYaw, f32 camPitch, f32 camDist);
    // サムネイル用デコードワーカー(ファイル読み+パース+WICデコード+縮小をメイン外で行う)
    void DecodeWorkerLoop();
    void EnqueueThumbnailDecode(const std::string& key);

    GraphicsDevice*   m_device = nullptr;
    DescriptorHeap*   m_srvHeap = nullptr;
    ResourceManager*  m_resourceManager = nullptr;
    bool m_valid = false;

    ShaderRuntimeCompiler m_compiler;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    std::unique_ptr<PipelineState> m_pso;

    DescriptorHeap m_previewRtvHeap;
    RenderTarget   m_previewRT;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    DescriptorHeap m_dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};

    // マテリアルテクスチャ3枚分のSRVブロック(albedo/normal/metalRoughness、連続3スロット)。
    // 毎回CreateSRVを呼び直す(エディタツールなのでコストは無視できる)。同一フレーム内で
    // 複数回描画(エディタプレビュー+サムネイル数枚)しても、GPU実行前に先のSRVを上書き
    // しないようリングで巡回して使う。
    static constexpr u32 kSrvRingCount = 8;
    u32 m_srvBlocks[kSrvRingCount] = {};
    u32 m_srvRingCursor = 0;

    std::unique_ptr<Mesh> m_sphereMesh;
    std::unique_ptr<Mesh> m_planeMesh;

    // ---- 球体サムネイルキャッシュ ----
    struct ThumbEntry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;   // 128px RGBA8、各エントリが自分のRTを持つ
        u32  srvIndex  = 0xFFFFFFFFu;
        u64  gpuHandle = 0;
        bool failed    = false;   // .dxmatパース失敗(Invalidateまで再試行しない)
    };
    std::unordered_map<std::string, ThumbEntry> m_thumbCache;   // key: 正規化した絶対パス

    // ---- 非同期デコード(ワーカー1本) ----
    // 4Kテクスチャの WIC デコードは1枚数百msかかるため、メインスレッドでやると
    // ローディング画面のスピナーやエディタが固まる。ワーカーが ScratchImage まで用意し、
    // メインは軽いGPUアップロード+球描画だけを行う。
    struct ThumbDecodeJob;                                       // cpp内定義(DirectXTex依存を隠す)
    std::unordered_set<std::string> m_pendingKeys;               // 依頼済み(未完了)キー。メインスレッド専用
    std::deque<std::string> m_decodeRequests;                    // ワーカー待ち(m_decodeMutex)
    std::vector<std::shared_ptr<ThumbDecodeJob>> m_decodeDone;   // デコード完了→描画待ち(m_decodeMutex)
    std::mutex m_decodeMutex;
    std::condition_variable m_decodeCv;
    std::thread m_decodeWorker;
    std::atomic<bool> m_decodeStop{false};

    Microsoft::WRL::ComPtr<ID3D12Resource> m_thumbDepth;        // 128px D32(全サムネイルで共用)
    DescriptorHeap m_thumbRtvHeap;                              // 1個をCreateRTVで使い回す
    DescriptorHeap m_thumbDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_thumbRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_thumbDsvHandle{};
};

} // namespace dx12e
