#pragma once
//
// VirtualGeometrySystem ― 仮想ジオメトリ（Nanite 風）のランタイム: `.vgeo` の読込 + GPU カリング（P2）+ メッシュシェーダ描画（P3）。
//
//   ・.vgeo（loose ファイル）を開き、ページを GPU バッファへ全常駐でアップロードする（ストリーミングは P5）。
//     アップロードは専用の COPY キューで分割して非同期に行う（ワーカースレッド。PC を固めない）。
//   ・GPU カリング: インスタンス → BVH 走査（段ごとの ExecuteIndirect）→ クラスタ（LOD 選択 / 錐台 / 法線コーン / HZB 二段）。
//     出力 = 可視クラスタ配列 + 統計（設計書 docs/VIRTUAL_GEOMETRY_DESIGN.md §2.4）。
//   ・ラスタ（P3）: 可視クラスタ配列から ExecuteIndirect(DispatchMesh) で、可視性バッファ（R32_UINT: 可視スロット 25bit + 三角形番号 7bit）と
//     呼び出し側の深度（D32_FLOAT）へ描く。二相: 一相目のカリング → ラスタ → HZB 再構築 → 二相目のカリング → ラスタ → HZB 再構築
//     （最後の HZB は次フレームの一相目の入力）。増幅シェーダ（AS）経由 / MS のみを切り替えられる。
//     非対応 GPU（MeshShaderTier なし）は RasterSupported() が false（カリングだけ動く）。
//   ・**専用ルートシグネチャ**（CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED）。メインのルートシグネチャ（62/64 DWORD）は消費しない。
//   ・ディスクリプタ: 既定は**専用ヒープ**（テスト用）。P4 以降のアプリは SystemDesc::externalHeap で**アプリの SRV ヒープ**を渡し、
//     VG のディスクリプタを全部そこへ確保する（VG 設計書 §2.4.8(1)。resolve が材質テクスチャと VG のバッファを 1 本のヒープで引くため）。
//   ・P4: 材質表（VgMaterialGpu。SetAssetMaterials）/ G-Buffer + 速度パス（DrawGBuffer）/ 材質 resolve（DrawResolve。メイン RS で動く）/
//     安定コンパクション（CullSettings::stableOrder。可視リストをフェーズごとにソートして決定論にする）。
//   ・D3D12 デバイス（ID3D12Device）だけで動く＝窓なしのヘッドレステスト（tests/vg_cull_gpu_test.cpp）で回せる。
//
// 使い方（アプリ）:
//   起動時  : Initialize(desc)
//   毎フレーム: CollectStats(frameSlot)   … このフレームスロットの前回の Execute の結果を読む（GPU 完了後）
//              Pump()                    … 非同期読込が終わったアセットを有効化
//              Execute(execDesc)         … コマンドリストへカリングを記録
//   シーン  : RequestAsset(path) → 準備ができたら（IsAssetReady）インスタンスの assetId に使う
//
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/IndexAllocator.h"
#include "renderer/vg/VgGpuTypes.h"

namespace dx12e::vg
{

class ByteSource;   // VgeoFormat.h

// ── 読込エラー（構造化）───────────────────────────────────────────────────────
enum class LoadCode : uint32_t
{
    Ok = 0,
    OpenFailed,        // ファイルが開けない
    Invalid,           // .vgeo の検証に失敗（VgeoError の文字列が message）
    Unsupported,       // 圧縮ページなど、この段階では読めない形式
    OverBudget,        // VRAM 予算（vramBudgetMB）を超える
    TooManyAssets,     // 同時に持てるアセット数の上限
    DeviceFailure,     // D3D12 のリソース確保 / アップロードの失敗
    NotInitialized,
};
const char* LoadCodeName(LoadCode c);

struct LoadError
{
    LoadCode    code = LoadCode::Ok;
    std::string message;
    bool ok() const { return code == LoadCode::Ok; }
    std::string ToString() const { return std::string(LoadCodeName(code)) + (message.empty() ? "" : ": " + message); }
};

// ── アセット情報（読み取り専用）──────────────────────────────────────────────
struct AssetInfo
{
    uint32_t    id = 0xFFFFFFFFu;
    std::string path;
    bool        ready = false;          // GPU 上で使える
    bool        failed = false;
    LoadError   error;                  // failed のとき
    uint64_t    vramBytes = 0;          // ページ + BVH
    uint32_t    pageCount = 0, clusterCount = 0, nodeCount = 0, levelCount = 0, chunkCount = 0, bvhDepth = 0;
    uint64_t    sourceTriangles = 0;
    float       boundingSphere[4] = {0, 0, 0, 0};
    float       aabbMin[3] = {0, 0, 0};
    float       aabbMax[3] = {0, 0, 0};
    float       proxyError = 0.0f;
    uint32_t    materialCount = 0;      // P4: .vgeo の材質数
    bool        materialsBound = false; // P4: SetAssetMaterials 済み
};

// P4: .vgeo の材質（MATERIALS セクションの MaterialRecord の写し。このヘッダは VgeoFormat.h を include しないので必要な欄だけ）。
//   テクスチャパスは .vgeo のフォルダ基準の相対パス（空 = 無し）。
struct AssetMaterial
{
    std::string name, albedo, normal, metalRough, emissive;
    uint32_t flags = 0;                 // MaterialRecord.flags（kMatHasNormalMap / kMatHasMetalRough / kMatHasEmissiveTex / kMatDoubleSided …）
    float    metallic = 0.0f, roughness = 0.5f;
    float    emissiveColor[3] = {0, 0, 0};
    float    emissiveIntensity = 0.0f;
    float    uvScaleOffset[4] = {1, 1, 0, 0};
    uint32_t sectionKind = 0;           // 0 = VG / 1 = NONVG
};

// ── 設定 ─────────────────────────────────────────────────────────────────────
struct CullSettings
{
    float tauPx         = 1.0f;     // LOD の画面空間誤差しきい値 τ（レンダー px）
    bool  hzbCulling    = true;     // 二段 HZB オクルージョン
    bool  coneCulling   = true;     // 法線コーンの背面棄却（両面材質は P4 で対応）
    float instanceMinPx = 0.5f;     // 画面上の半径がこれ未満のインスタンスを棄却（0 で無効）
    bool  autoTau       = true;     // キュー溢れを検出したら次フレームの τ を自動で 1.25 倍する
    bool  forceLod0     = false;    // 検証 / 計測用: LOD0 の葉クラスタだけを選ぶ（τ = 1e-6。autoTau も無効）。全 LOD0 を素朴に描いた参照になる
    bool  stableOrder   = false;    // P4: 決定論。各フェーズの可視リストを {instance, clusterRef} 順にソートしてからラスタする（AS は使わない）
};

// P4: アプリのディスクリプタヒープを使う（VG 設計書 §2.4.8(1)）。heap が null なら VG の専用ヒープ（テスト用）。
struct ExternalHeap
{
    ID3D12DescriptorHeap* heap = nullptr;                   // シェーダ可視の CBV_SRV_UAV ヒープ
    std::function<uint32_t(uint32_t)> allocBlock;          // count 個の連続添字（失敗は 0xFFFFFFFF）
    std::function<void(uint32_t, uint32_t)> freeBlock;     // (先頭, 個数)
};

struct SystemDesc
{
    ID3D12Device*       device = nullptr;
    ID3D12CommandQueue* directQueue = nullptr;   // タイムスタンプ周波数の取得用（null なら GPU 時間は 0）
    std::wstring        shaderDir;               // .cso のあるフォルダ（末尾に区切りを含む）
    uint32_t            frameCount = 3;          // FrameResources::kFrameCount と一致させる
    uint32_t            maxInstances = 65536;
    uint32_t            maxAssets = 256;
    uint32_t            nodeQueueCap = 1u << 20; // 各キューの容量（要素数。1 要素 8 B）
    uint32_t            groupQueueCap = 1u << 20;
    uint32_t            visibleCap = 1u << 20;
    uint32_t            deferredCap = 1u << 20;
    uint32_t            vramBudgetMB = 3072;     // 全アセット（ページ + BVH）+ 作業バッファの合計上限
    bool                verifyPageCrc = false;   // 読込時にページ CRC32 を検証（遅い。開発 / CI 用）
    uint32_t            uploadSliceMB = 32;      // アップロードの 1 回あたり（ステージング 2 面）
    bool                asyncLoad = true;        // RequestAsset のワーカースレッド
    bool                enableRaster = true;     // P3: メッシュシェーダのラスタを用意する（MeshShaderTier が無ければ自動で無効）
    uint32_t            idleUnloadFrames = 1800; // この Execute 回数だけ使われなかったアセットを解放する（0 で無効）。GPU の使用中は解放しない（3 フレームより十分長い）
    ExternalHeap        externalHeap;            // P4: アプリの SRV ヒープへ一本化（null なら専用ヒープ）
    uint32_t            maxMaterials = 16384;    // P4: 材質表の容量（全アセット合計。1 個 64 B）
};

// ── 1 フレームの入力 ─────────────────────────────────────────────────────────
struct HzbInput
{
    ID3D12Resource* prev = nullptr;      // 一相目が読む HZB（前フレームの深度から作った物）。NON_PIXEL_SHADER_RESOURCE 状態で渡す
    bool            prevValid = false;   // false（初回 / リサイズ直後 / カメラカット）なら一相目は HZB を使わない
    ID3D12Resource* cur = nullptr;       // 二相目が読む HZB（今フレームの深度から作った物）。prev と同じリソースでもよい
    bool            curValid = false;    // buildBetweenPhases が cur を作る、または既に作ってあるとき true
    uint32_t        width = 0, height = 0, mips = 0;   // mip0 の解像度とミップ数（prev / cur 共通）
    // 一相目の後・二相目の前に呼ぶ（HZB を今フレームの深度から作り直す。呼んだ後、リソースは読み取り状態へ戻すこと）。
    // ★中でルートシグネチャ / ヒープを変えてよい（戻ってきたら VG が張り直す）。
    std::function<void(ID3D12GraphicsCommandList*)> buildBetweenPhases;
    // P3（raster.enabled のときだけ）: 二相目のラスタの後、最終深度から HZB を作り直す（次フレームの一相目の入力になる）。
    std::function<void(ID3D12GraphicsCommandList*)> buildFinal;
};

// ── P3: ラスタ ───────────────────────────────────────────────────────────────
struct RasterDesc
{
    bool enabled = false;
    uint32_t width = 0, height = 0;               // 可視性バッファ / ビューポート（= レンダー解像度。ExecuteDesc.vpW / vpH と一致させる）
    ID3D12Resource* depth = nullptr;              // D32_FLOAT（R32_TYPELESS 可）。SRV は R32_FLOAT で張る（デバッグ表示用）
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};            // depth の DSV（D32_FLOAT）
    // 深度の状態遷移（VG は呼び出し側の状態追跡を知らない）。Execute の入口と出口では深度は「読み取り（NON_PIXEL_SHADER_RESOURCE 以上）」であること。
    std::function<void(ID3D12GraphicsCommandList*)> depthToRaster;   // → DEPTH_WRITE
    std::function<void(ID3D12GraphicsCommandList*)> depthToSample;   // → NON_PIXEL_SHADER_RESOURCE（| PIXEL_SHADER_RESOURCE）
    bool useAs = false;            // 増幅シェーダ（32 クラスタ / グループ + 追加カリング）経由。false = MS のみ
    bool smallPrimCull = false;    // 画素中心を 1 つも覆わない三角形 / クラスタを落とす
    bool measure = false;          // 計測: 断片数 / 小三角形カリング数 / 被覆画素 / オーバードロー画像（少し遅い。既定 OFF）
    bool overdrawImage = false;    // measure のとき、断片数の画像も作る（デバッグ表示 VG_DBG_OVERDRAW 用）
    bool edgeHist = false;         // 計測: 可視クラスタの最長辺（画面 px）ヒストグラム（SW ラスタの判断用）
};

// デバッグ / 暫定シェーディングの全画面描画（可視性バッファ → カラー RT）。P4 の resolve までの代用品。
struct DebugDrawDesc
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    uint32_t frameSlot = 0;                       // Execute と同じ値
    uint32_t mode = 0;                            // VG_DBG_*
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    DXGI_FORMAT rtFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t width = 0, height = 0;
    float depthRange = 100.0f;                    // VG_DBG_DEPTH: この距離(m)で黒
    float overdrawMax = 8.0f;                     // VG_DBG_OVERDRAW: この断片数で赤
    float zFar = 1000.0f;
    ID3D12DescriptorHeap* restoreHeap = nullptr;
};

// P4（H2）: G-Buffer + 速度（可視性バッファ → VG 画素だけ MRT2 へ。非 VG 画素は discard）。
//   RT は呼び出し側が RENDER_TARGET 状態にしておく（速度 R16G16_FLOAT / G-Buffer R16G16B16A16_FLOAT = 深度プリパスと同じ形式）。
//   ビュー行列は Execute（ExecuteDesc::viewProjNJ / prevViewProjNJ）で渡した物を使う。
struct GBufferDesc
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    uint32_t frameSlot = 0;                       // Execute と同じ値
    D3D12_CPU_DESCRIPTOR_HANDLE velocityRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtv{};
    DXGI_FORMAT velocityFormat = DXGI_FORMAT_R16G16_FLOAT;
    DXGI_FORMAT gbufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12DescriptorHeap* restoreHeap = nullptr;  // 専用ヒープのとき、描いた後に張り直すヒープ
};

// P4（H3）: 材質 resolve（全画面 PS。メイン RS）。★呼び出し側が先にヒープ / メイン RS / b1 / 影・IBL・AO・クラスタ・SSR・SSGI の
//   テーブルを全部張っておく（ForwardScenePass と RenderSceneMeshes と同じ物）。VG は PSO・b0（ルート定数）・RT・ビューポートだけ張る。
//   externalHeap のとき（VG のディスクリプタがアプリのヒープにあるとき）だけ使える。
struct ResolveDesc
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    uint32_t frameSlot = 0;
    ID3D12RootSignature* rootSig = nullptr;       // メイン RS（HEAP_DIRECTLY_INDEXED 付き）
    uint32_t rootConstSlot = 0;                   // b0（40 DWORD のルート定数）のルートパラメータ番号
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    DXGI_FORMAT rtFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t width = 0, height = 0;
    float viewProjJ[16]{};                        // ラスタと同じジッタ付き VP（行優先 4x4）
};

struct ExecuteDesc
{
    ID3D12GraphicsCommandList* cmd = nullptr;   // DIRECT（または COMPUTE）のコマンドリスト
    uint32_t                   frameSlot = 0;   // 0..frameCount-1（毎フレーム別のスロット。CollectStats と同じ値）
    float viewProj[16]{};                       // 今フレームの（ジッタ付き）VP。行優先 4x4 = XMFLOAT4X4（行ベクトル規約）
    float prevViewProj[16]{};                   // 前フレームの（ジッタ付き）VP（HZB 一相目の投影用）
    float viewProjNJ[16]{};                     // P4: 今フレームのジッタ無し VP（G-Buffer の速度）。hasVelocityVp が偽なら viewProj を使う
    float prevViewProjNJ[16]{};                 // P4: 前フレームのジッタ無し VP（同上。履歴が無いときは viewProjNJ と同値を渡す = 速度 0）
    bool  hasVelocityVp = false;
    float camPos[3]{};
    float zNear = 0.1f;
    float projScale = 1000.0f;                  // 0.5 * viewportH / tan(fovY / 2)
    float vpX = 0, vpY = 0, vpW = 1920, vpH = 1080;
    const VgInstanceInput* instances = nullptr;
    uint32_t               instanceCount = 0;
    HzbInput               hzb;
    CullSettings           settings;
    RasterDesc             raster;                 // P3
    ID3D12DescriptorHeap*  restoreHeap = nullptr;  // Execute の最後に SetDescriptorHeaps し直す（null なら何もしない）
    bool                   dumpVisible = false;    // 可視クラスタ表を CPU へ読み戻す（テスト / 小シーン専用。GetDump で読む）
};

// ── 統計（非同期の読み戻し。1〜2 フレーム遅れ）─────────────────────────────────
struct FrameStats
{
    bool     valid = false;
    uint32_t instances = 0;            // 入力インスタンス数
    uint32_t instancesFrustum = 0;     // 錐台 / 極小棄却を通ったインスタンス
    uint32_t instancesHzbRejected = 0;
    uint32_t nodesVisited = 0;         // DAG（BVH）ノード訪問数
    uint32_t childrenTested = 0;
    uint32_t groups = 0;
    uint32_t clustersTested = 0;
    uint32_t clustersSelected = 0;     // LOD 選択で選ばれたクラスタ（錐台 / コーン / HZB の前）
    uint32_t visibleP1 = 0, visibleP2 = 0;
    uint32_t visibleClusters = 0;      // 可視クラスタ（一相目 + 二相目）
    uint32_t phase2Clusters = 0;       // うち二相目で救われたもの
    uint64_t trianglesDrawn = 0;       // 見かけの三角形数（可視クラスタの三角形数の合計）
    uint64_t sourceTrianglesInFrustum = 0;   // 錐台内インスタンスの LOD0 換算三角形数
    uint32_t clustersCulledFrustum = 0, clustersCulledCone = 0, clustersDeferred = 0, clustersHzbDropped = 0;
    uint32_t nodesCulledFrustum = 0, nodesCulledLod = 0, nodesDeferred = 0;
    uint32_t overflowNode = 0, overflowGroup = 0, overflowVisible = 0, overflowDeferred = 0;
    bool     overflow = false;
    uint32_t levelHistogram[VG_HIST_LEVELS] = {};
    float    cullGpuMs = 0.0f;         // Execute 全体から「ラスタ」を除いた GPU 時間（カリング + HZB 再構築）。ラスタが無ければ Execute 全体
    float    executeGpuMs = 0.0f;      // Execute 全体（カリング + ラスタ + HZB 再構築）の GPU 時間
    float    rasterGpuMs = 0.0f;       // P3: ラスタ（引数生成 + DispatchMesh。一相目 + 二相目）の GPU 時間
    float    rasterPhaseGpuMs[2] = {0, 0};
    // P3（ラスタ）
    bool     rasterActive = false;
    bool     rasterUsedAs = false;
    uint32_t rasterClusters = 0;       // ラスタへ投げたクラスタ数（= visibleClusters）
    uint32_t asCulled = 0;             // 増幅シェーダの追加カリングで落としたクラスタ（measure のとき）
    uint64_t msTrisOut = 0;            // メッシュシェーダが出力した三角形数（measure のとき）
    uint64_t msPrimCulled = 0;         // 小三角形カリングで落とした三角形数（measure のとき）
    uint64_t psInvocations = 0;        // 深度テストを通った断片数（measure のとき）
    uint64_t coveredPixels = 0;        // 可視性バッファの被覆画素数（measure のとき）
    uint32_t edgeClusters[VG_EDGE_BINS] = {};   // 最長辺（px）ヒストグラム: クラスタ数（edgeHist のとき）
    uint32_t edgeTris[VG_EDGE_BINS] = {};       //                       三角形数
    // P4
    bool     stableOrder = false;      // このフレームは安定ソートした
    uint32_t sortedClusters = 0;       // 安定ソートした可視クラスタ数（一相目 + 二相目）
    uint32_t sortSkipped = 0;          // 上限（2^VG_SORT_MAX_LOG2）を超えてソートを諦めたフェーズ数
    float    gbufferGpuMs = 0.0f;      // G-Buffer + 速度パス（H2）の GPU 時間（描かなかったフレームは 0）
    float    resolveGpuMs = 0.0f;      // 材質 resolve（H3）の GPU 時間
    bool     gbufferActive = false, resolveActive = false;
    uint64_t vramBytes = 0;            // アセット（ページ + BVH）+ 作業バッファの合計
    uint32_t assetsLoaded = 0;
    float    tauUsed = 0.0f;           // 実際に使った τ（autoTau で上がることがある）
    uint32_t instancesDropped = 0;     // maxInstances を超えて捨てたインスタンス数
};

class VirtualGeometrySystem
{
public:
    VirtualGeometrySystem();   // cpp で定義（unique_ptr<Uploader> の不完全型のため）
    ~VirtualGeometrySystem();
    VirtualGeometrySystem(const VirtualGeometrySystem&) = delete;
    VirtualGeometrySystem& operator=(const VirtualGeometrySystem&) = delete;

    // 失敗（SM 6.6 / リソースバインディング Tier 3 が無い、.cso が無い等）は false と err に理由。
    bool Initialize(const SystemDesc& desc, std::string* err = nullptr);
    void Shutdown();
    bool IsReady() const { return m_ready; }

    // ── アセット ─────────────────────────────────────────────────────────────
    // 同期読込（呼び出しスレッドで最後まで実行）。既に読み込み済みのパスならその ID を返す。
    bool LoadAssetSync(const std::string& path, uint32_t* outId, LoadError* err = nullptr);
    bool LoadAssetFromSource(const ByteSource& src, const std::string& name, uint32_t* outId, LoadError* err = nullptr);
    // 非同期読込の要求。戻り値は ID（読込中でも有効）。準備完了は IsAssetReady / GetAssetInfo で見る。失敗は 0xFFFFFFFF。
    uint32_t RequestAsset(const std::string& path);
    // ワーカーが終えたアセットをメインスレッドで有効化（ディスクリプタ作成）。毎フレーム呼ぶ。
    void Pump();
    bool IsAssetReady(uint32_t id) const;
    bool GetAssetInfo(uint32_t id, AssetInfo& out) const;
    uint32_t FindAsset(const std::string& path) const;    // 未登録は 0xFFFFFFFF
    uint32_t AssetCount() const;
    uint32_t PendingLoads() const { return m_pending.load(); }
    // アセットを解放する（GPU がまだ使っている可能性があるので、呼び出し側はフェンス完了後に呼ぶこと）。
    void UnloadAsset(uint32_t id);

    void SetVramBudgetMB(uint32_t mb) { m_vramBudgetMB = mb; }
    uint64_t VramUsedBytes() const { return m_vramUsed.load(); }
    uint32_t MaxBvhDepth() const { return m_maxDepth; }

    // ── 毎フレーム ───────────────────────────────────────────────────────────
    bool Execute(const ExecuteDesc& d);
    // frameSlot の前回の Execute の統計を読む（そのスロットの GPU 完了後に呼ぶ）。
    void CollectStats(uint32_t frameSlot);
    const FrameStats& GetStats() const { return m_stats; }
    // 直近の CollectStats で得た可視クラスタ表（dumpVisible で Execute したとき）。{instance, clusterRef}
    const std::vector<std::pair<uint32_t, uint32_t>>& GetDump() const { return m_dump; }

    // ── P3 ──────────────────────────────────────────────────────────────────
    // メッシュシェーダのラスタが使えるか（MeshShaderTier >= 1 + SM 6.5 + PSO 作成成功。DX12_DISABLE_MESHSHADER=1 で偽）
    bool RasterSupported() const { return m_rasterOk; }
    // 直近の Execute（raster.enabled）で描いた可視性バッファ。状態は PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE。無ければ null
    ID3D12Resource* VisibilityBuffer() const { return m_vis.Get(); }
    ID3D12Resource* OverdrawImage() const { return m_overdraw.Get(); }   // measure + overdrawImage のときだけ（UAV 状態）
    bool VisibilityValid(uint32_t frameSlot) const { return frameSlot < 8 && m_visValid[frameSlot]; }
    // 可視性バッファを全画面で表示する（VG_DBG_*）。Execute（raster）と同じフレームスロットで呼ぶ。アプリのヒープは restoreHeap で戻す。
    //   rtFormat = R32G32B32A32_FLOAT のときはブレンド無し（テスト用の生データ VG_DBG_RAW_*）。
    bool DrawDebug(const DebugDrawDesc& d);

    // ── P4 ──────────────────────────────────────────────────────────────────
    // .vgeo の材質（テクスチャパス付き）。アセットが準備済みのときだけ true。
    bool GetAssetMaterials(uint32_t id, std::vector<AssetMaterial>& out) const;
    // 材質表へ登録する（count = アセットの材質数）。以後 G-Buffer / resolve / K2（両面材質）がこの表を読む。
    bool SetAssetMaterials(uint32_t id, const VgMaterialGpu* mats, uint32_t count);
    bool AssetMaterialsBound(uint32_t id) const;
    bool DrawGBuffer(const GBufferDesc& d);
    bool DrawResolve(const ResolveDesc& d);
    bool ResolveSupported() const { return m_rasterOk && m_extHeap && !m_bcResolveVs.empty() && !m_bcResolvePs.empty(); }
    bool UsesExternalHeap() const { return m_extHeap; }
    uint32_t DescriptorsInUse() const;             // VG が確保しているディスクリプタ数（固定領域 + アセット）

    // デバッグ / テスト用
    const VgWorkLayout& WorkLayout() const { return m_layout; }
    ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_heapRaw; }

private:
    struct Asset
    {
        std::string path;
        std::atomic<int> state{0};          // 0 = 読込中 / 1 = 完了（未有効化）/ 2 = 有効 / 3 = 失敗 / 4 = 解放済み
        LoadError error;
        AssetInfo info;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> chunks;
        Microsoft::WRL::ComPtr<ID3D12Resource> nodes;
        uint32_t descBase = 0xFFFFFFFFu;    // nodes SRV。続いてチャンク SRV
        uint32_t descCount = 0;
        VgAssetGpu gpu{};
        uint64_t reservedBytes = 0;
        uint64_t lastUse = 0;               // 最後に Execute で使われた時の m_frameCounter（GC 用）
        std::vector<AssetMaterial> materials;   // P4: .vgeo の材質（読込時にワーカーが詰める）
        uint32_t matBase = 0xFFFFFFFFu;     // P4: 材質表の区間
        uint32_t matCount = 0;
    };

    struct Uploader;

    bool CreatePipelines(std::string* err);
    bool CreateRasterPipelines();
    ID3D12PipelineState* GetRasterPso(bool useAs, bool count);
    bool EnsureVisBuffers(uint32_t w, uint32_t h, bool overdraw);
    bool CreateBuffers(std::string* err);
    LoadError LoadImpl(const ByteSource& src, Asset& a);
    void WorkerMain();
    void FinalizeAsset(Asset& a);
    uint32_t AllocAssetSlot(const std::string& path, LoadError* err);
    void BindCompute(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS cb);
    void UpdateHzbSrv(uint32_t slot, uint32_t which, ID3D12Resource* res);

    bool m_ready = false;
    SystemDesc m_desc;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    uint32_t m_vramBudgetMB = 3072;

    // パイプライン
    Microsoft::WRL::ComPtr<ID3D12RootSignature>    m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_cmdSigDispatch;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>    m_psoReset, m_psoArgs, m_psoInstance, m_psoTraverse, m_psoCluster;
    // P3（ラスタ）
    bool m_rasterOk = false;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_cmdSigMesh;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>    m_psoRasterArgs, m_psoCoverage;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>    m_psoRaster[2][2];          // [useAs][count]
    std::vector<uint8_t> m_bcMsPlain, m_bcMsAs, m_bcAs, m_bcPs, m_bcPsCount, m_bcDbgVs, m_bcDbgPs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>    m_psoDebug;
    DXGI_FORMAT m_psoDebugFormat = DXGI_FORMAT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vis, m_overdraw;
    uint32_t m_visW = 0, m_visH = 0;
    D3D12_RESOURCE_STATES m_visState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_clearHeap;     // 非シェーダ可視: vis の RTV / UAV クリア用の CPU 側 UAV
    struct Retired { Microsoft::WRL::ComPtr<ID3D12Resource> a, b; uint64_t frame = 0; };
    std::vector<Retired> m_retired;                                           // リサイズで捨てた可視性バッファ（GPU の使用が終わるまで持つ）
    bool m_visValid[8]{};
    bool m_rasterUsedAs[8]{};
    bool m_rasterOn[8]{};
    bool m_tsRaster[8]{};

    // ディスクリプタ（専用ヒープ、または P4 の externalHeap = アプリのヒープ）
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;   // 専用ヒープのときだけ
    ID3D12DescriptorHeap* m_heapRaw = nullptr;              // 実際に使うヒープ（専用 or アプリ）
    bool     m_extHeap = false;
    uint32_t m_fixedBase = 0;                               // 固定領域の先頭（専用ヒープでは 0）
    uint32_t m_descSize = 0;
    IndexAllocator m_descAlloc;
    std::mutex m_descMutex;
    uint32_t m_descInUse = 0;
    static constexpr uint32_t kHeapCapacity = 16384;
    static constexpr uint32_t kFixedDescs   = 128;     // 固定領域（先頭 128 個。フレームスロット 8 面ぶん = 4 + 8 * 8）
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t i) const;
    uint32_t D(uint32_t k) const { return m_fixedBase + k; }   // 固定領域の k 番目の絶対添字
    uint32_t AllocDescBlock(uint32_t count);
    void FreeDescBlock(uint32_t base, uint32_t count);

    // P4: 材質表（UPLOAD。書き込みは登録時だけ）/ G-Buffer / resolve / 安定ソート
    Microsoft::WRL::ComPtr<ID3D12Resource> m_matBuf;
    uint8_t* m_matMapped = nullptr;
    IndexAllocator m_matAlloc;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSort, m_psoGBuffer, m_psoResolve;
    std::vector<uint8_t> m_bcGbVs, m_bcGbPs, m_bcResolveVs, m_bcResolvePs;
    ID3D12RootSignature* m_psoResolveRs = nullptr;
    DXGI_FORMAT m_psoResolveFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_psoGbFormats[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    bool m_psoDebugRaw = false;
    uint32_t m_prevVisible[2] = {0, 0};                    // 前回の統計の一相目 / 二相目の可視数（安定ソートの長さを決める）
    bool m_stableOn[8]{};
    bool m_tsGb[8]{}, m_tsRv[8]{};                         // このフレームスロットで G-Buffer / resolve のタイムスタンプを打った
    void StampPair(ID3D12GraphicsCommandList* cmd, uint32_t f, uint32_t q, bool end);

    // バッファ
    VgWorkLayout m_layout;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_work, m_extra, m_args;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instBuf[8], m_assetBuf[8], m_cbBuf[8];
    uint8_t* m_instMapped[8]{}; uint8_t* m_assetMapped[8]{}; uint8_t* m_cbMapped[8]{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statsReadback[8];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dumpReadback;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_queryReadback;
    uint64_t m_tsFreq = 0;
    bool m_statsPending[8]{};
    bool m_dumpPending[8]{};
    uint32_t m_dumpCount[8]{};
    float m_tauUsed[8]{};
    uint32_t m_instDropped[8]{};
    uint32_t m_instUploaded[8]{};
    D3D12_RESOURCE_STATES m_argsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* m_hzbRes[8][2]{};

    // アセット
    std::deque<std::unique_ptr<Asset>> m_assets;
    mutable std::mutex m_assetsMutex;
    std::unordered_map<std::string, uint32_t> m_pathToId;
    std::atomic<uint64_t> m_vramUsed{0};
    uint64_t m_fixedVram = 0;
    uint32_t m_maxDepth = 0;
    uint32_t m_gpuAssetCount = 0;
    uint64_t m_frameCounter = 0;       // Execute の回数

    // 非同期読込
    std::unique_ptr<Uploader> m_up;
    std::thread m_worker;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<uint32_t> m_jobs;
    bool m_quit = false;
    std::atomic<uint32_t> m_pending{0};

    // 統計
    FrameStats m_stats;
    std::vector<std::pair<uint32_t, uint32_t>> m_dump;
    float m_tauScale = 1.0f;
};

} // namespace dx12e::vg
