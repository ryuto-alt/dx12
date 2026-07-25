#pragma once

#include "Types.h"
#include "Window.h"
#include "GameClock.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <filesystem>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include <array>
#include <thread>
#include <atomic>
#include <functional>
#include "ecs/Components.h"
#include "project/Project.h"
#include "project/GitIntegration.h"   // GitResult（非同期 git タスクの戻り値）
#include "editor/EditorIcons.h"
#include "engine/core/EventBus.h"   // ヘッダオンリー、GPU 非依存。entt の後に置く
#include "core/mcp/McpDeferred.h"   // MCP 遅延応答の相関情報（値メンバで持つので完全型が要る）
#include "core/CpuScope.h"          // CpuScope / CpuScopeTimer（エディタとも共有するので独立ヘッダ）
#include "renderer/DrawItem.h"      // 描画リストの要素（エディタのピッキングも読むので独立ヘッダ）

// Forward declarations for graphics module
namespace dx12e
{
    class GraphicsDevice;
    class CommandQueue;
    class SwapChain;
    class FrameResources;
    class DescriptorHeap;
    class RootSignature;
    class PipelineState;
    class CommandList;
    class RenderTarget;
    class GpuTimer;
    class PostProcess;
    class BloomPass;
    class AutoExposurePass;
    class GodRaysPass;
    class LensFlarePass;
    class DofPass;
    class MotionBlurPass;
    class SSAOPass;
    class ContactShadowPass;
    class TaaPass;
    class ParticleSystem;
    class GpuParticleSystem;
    class SpriteRenderer;
    class SceneTransition;
    class IBLBaker;
    class SkyboxRenderer;
    class Texture;
    class SceneFlow;
    class ConstantBuffer;
    class Camera;
    class ResourceManager;
    class InputSystem;
    class ImGuiManager;
    class UiTestHarness;
    class Scene;
    class SkinningBuffer;
    class ScriptEngine;
    class McpBridge;
    class UISystem;
    class UiAnimRuntime;
    class AudioSystem;
    class PhysicsSystem;
    class NetworkSystem;
    class PhysicsDebugRenderer;
    class EditorIconRenderer;
    class EditorContext;
    class EditorLayer;
    class ModelThumbnailRenderer;
    class VfxEditorPanel;
    class UiEditorPanel;
    class AnimationEditorPanel;
    class SpriteSheetEditorPanel;
    class NetworkPanel;
    class ShaderManager;
    class MaterialAssetManager;
    class MaterialEditorPanel;
    class MaterialLibraryPanel;
    struct Material;
}

namespace dx12e
{

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize(HINSTANCE hInstance, int nCmdShow, bool gameMode = false,
                    const ProjectInfo* projectInfo = nullptr, bool buildMode = false);
    void Run();
    void Shutdown();

    // マルチプレイ テストクライアント起動用(フェーズ⑨、--net-client CLI 引数)。
    // Initialize() より前に呼ぶこと。"ip:port" 形式。空なら何もしない(通常起動)。
    // Initialize 内でゲームモードの自動 Play に載せて net:join 相当を実行する。
    void SetNetTestClientJoin(const std::string& ipPort) { m_pendingNetClientJoin = ipPort; }
    // --project <dir>。ランチャーを飛ばしてこのプロジェクトを直接開く。--net-client 併用時は
    // 開いた後に自動Play=Join、単独指定なら開くだけ。Initialize() より前に呼ぶこと。
    void SetNetTestProject(const std::string& dir) { m_pendingNetClientProject = dir; }

    // ImGuiTestEngine による UI 自動テスト(--ui-tests)。Initialize より前に呼ぶ。
    // runAll=true なら起動後に全テストを走らせ、完了したら終了する(終了コード=UiTestExitCode)。
    // deepOnly=true なら超詳細診断だけを走らせる(--ui-tests-deep。UI 操作をほぼ伴わない)。
    void EnableUiTests(bool runAll, int speedMode, bool deepOnly = false)
    { m_uiTestsRequested = true; m_uiTestsRunAll = runAll; m_uiTestsSpeed = speedMode;
      m_uiTestsDeepOnly = deepOnly; }
    int  UiTestExitCode() const { return m_uiTestExitCode; }

    // ヘッドレスでゲームをビルド（--build CLI 用）。開始シーンは title.json があればそれ。
    // 成否を返す（CLI の終了コード / GUI の完了表示に使う）。
    bool BuildGameStandalone();

    enum class EngineMode { Editor, Playing };

    // ===== エンジン診断(UI 自動テスト)用フック。通常のコードからは使わない =====
    // メニュー経由の操作は「シーンを開く」等が Win32 のモーダルダイアログを開き、
    // 自動テストがそこで永久に固まる。テストからはこれらで状態を直接動かす。
    EditorContext* GetEditorContext()  { return m_editorCtx.get(); }
    EngineMode     GetEngineMode() const { return m_engineMode; }
    // 次のフレーム境界で Play/Stop する。m_pendingMode を直接書くと同フレームの
    // EditorLayer::Render に Editor へ上書きされるため、Update 冒頭で消費する要求として積む。
    void           RequestMode(EngineMode m) { m_diagModeRequest = (m == EngineMode::Playing) ? 2 : 1; }
    // 検査でシーンを汚さないための退避/復元。戻り値は退避先パス(空なら失敗)。
    std::string    SaveSceneSnapshot();
    void           RequestSceneRestore(const std::string& path);

    // ===== 超詳細診断用フック =====
    // シーンが参照しているアセットを走査するために registry が要る（DeepDiag::SceneAssets）。
    Scene*         GetScene() { return m_scene.get(); }

    // 表示パイプラインのフォーマット構成。ガンマ二重適用の検出用（DeepDiag::Gamma）。
    // DXGI_FORMAT を u32 で渡すのは、この診断を将来ヘッドレス側から呼んでも
    // d3d12 ヘッダを引きずらないようにするため。
    struct DiagRenderInfo
    {
        u32   backBufferFormat = 0;
        u32   sceneColorFormat = 0;
        u32   depthFormat      = 0;
        int   tonemapper       = 0;
        bool  postEnabled      = true;
        bool  exposureOn       = false;
        float exposure         = 1.0f;
    };
    DiagRenderInfo GetDiagRenderInfo() const;

    // 直近フレームの絵そのものを数値で受け取る。「配置したのに何も映らない」
    // 「ポスト処理が実は走っていない」をピクセルで確かめるため。
    struct DiagFrameStats
    {
        bool  valid    = false;
        u32   width    = 0;
        u32   height   = 0;
        float meanLuma = 0.0f;   // トーンマップ後 0..1 の平均輝度
        float nonBlack = 0.0f;   // 真っ黒でないピクセルの割合 0..1
        u64   hash     = 0;      // 絵が変わったかの比較用（値そのものに意味は無い）
    };
    // 次のフレーム境界で 1 枚読み戻す。ImGui のフレーム内から直接キャプチャすると
    // コマンドリストが二重に開くので、要求を積んで Run ループに拾わせる。
    void           RequestDiagnosticFrameStats() { m_diagFrameStatsRequest = true; }
    // 測れていれば valid=true を返し、状態をリセットする（未完了なら valid=false）。
    DiagFrameStats TakeDiagnosticFrameStats();

    // 今フレームの描画リスト（読み取り専用）。BuildDrawList() は Render() の先頭で走り、
    // エディタ UI（EditorLayer::Render）は同じ Render() の後半で走るので、エディタから
    // 見た時点で構築済み・有効。エディタの精密ピッキングがブロードフェーズ候補として借り、
    // ワールド行列と球（center/radius）を再計算せずに済ませる。
    // Grid / メッシュ無し / park済み(scale≈0) / Pfx* の除外もリスト構築時に済んでいる。
    const std::vector<DrawItem>& GetDrawItems() const { return m_drawItems; }

private:
    void Update();
    void Render();
    // MCP ブリッジから来た 1 行(JSON リクエスト)を処理して応答 JSON 行を返す。
    // メインスレッドで呼ばれるので m_scene / m_scriptEngine を直接触ってよい。
    // 戻り値が空文字列なら「遅延応答」(フレーム境界で結果確定後に SendToClient で送る)。
    // client は遅延応答を送り返すための McpBridge クライアントトークン。
    std::string HandleMcpCommand(uint64_t client, const std::string& line);
    // 直近フレームのシーン描画(m_sceneRT)を PNG に書き出す。成功=絶対パス / 失敗=空文字列+err。
    // MCP の screenshot 用。同期 readback(WaitIdle×2)＝低頻度のエディタ操作として割り切る。
    std::string CaptureSceneScreenshot(std::string& err);
    // m_sceneRT を CPU へ読み戻し、現在のポスト設定と同じ表示変換を掛けて BGRA8 にする。
    // CaptureSceneScreenshot と超詳細診断のフレーム統計で共用する実体。
    // フレーム境界からのみ呼ぶこと(内部で BeginFrame/WaitIdle する)。
    bool ReadbackSceneBgra(std::vector<u8>& outBgra, u32& outW, u32& outH, std::string& err);
    // シーン内の全メッシュを指定 viewProj で描画（メインパスとカメラプレビューで共用）。
    // isGameView=true でエディタ用グリッドを除外。per-frame CB / シャドウSRV /
    // ルートシグネチャ / RT / ビューポートは呼び出し側で設定済みとする。
    // depthPrepassActive=true のときだけ深度プリパス併用用の LESS_EQUAL forward PSO を使う
    // （プリパスが書いた深度を再利用するため）。false の通常経路は LESS PSO（既存 z-fight 挙動を維持）。
    // contactShadowSrvIndex は t11 に張るスクリーン空間の近接遮蔽（無効時は白ダミー）。
    void RenderSceneMeshes(ID3D12GraphicsCommandList* nativeCmdList, u32 frameIndex,
                           DirectX::XMMATRIX viewProj, bool isGameView, u32 aoSrvIndex,
                           bool depthPrepassActive = false,
                           u32 contactShadowSrvIndex = 0xFFFFFFFFu);
    // Sprite2D(worldSpace=true) を指定 viewProj/RT/DSV へ描画（メインパスとカメラプレビューで共用）。
    // camRight/camUp はビルボード展開用。billboard でないものはエンティティのワールド行列で配置。
    void DrawWorldSprites(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                          DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp,
                          D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                          u32 vpX, u32 vpY, u32 vpW, u32 vpH, float time);
    // CSM: カメラ視錐台を near→far で kNumCascades 分割し、各カスケードをライト視点へタイトフィット。
    // 結果は m_cascadeViewProj[] / m_cascadeSplitsView[] に格納する。
    void ComputeCascades(const DirectX::XMVECTOR& lightDir, f32 camNear, f32 camFar);
    // 深度専用シーン描画（CSM各カスケード/スポット影/ポイント影の各面/SSAOプリパスで共用）。
    // RT/DSV・ビューポート・クリア・バリアは呼び出し側の責任。frameIndex はボーンSRV参照用。
    // updateSkinning=true の時だけ skinningBuffer->Update を呼ぶ（1フレームに1回で十分なため）。
    // lodBias: 影パスは +1（1段粗いLOD）で呼ぶ。SSAO深度プリパスは 0 =
    // メインパスと同一LOD必須（LESS_EQUAL で同一深度を通すため）。
    // instPSO を渡すと、batchKey が同じ静的メッシュ群を DrawIndexedInstanced に畳む
    // （nullptr なら従来どおり per-object 描画）。
    //
    // ★深度プリパスのモード契約（00-COORDINATION §2。3つ目のモードを作らないこと）:
    //   DepthOnly            … 従来どおり深度のみ（CSM / スポット影 / ポイント影 / SSAO+コンタクトシャドウ）
    //   DepthVelocityGBuffer … 深度 + 速度(RG16F) [+ 将来の G-Buffer(法線/ラフネス/メタリック)]。
    //                          RTV は呼び出し側（TaaPass::BeginVelocity 等）がバインドしておくこと。
    //                          このモードでのみ b0 のレイアウトが 36 DWORD の速度用に変わり、
    //                          スキンドは t12 に前フレームのボーン行列がバインドされる。
    //   G-Buffer を足す者へ: PSO を 3 本（静的/インスタンシング/スキンド）差し替えて
    //   SetRenderTargetFormats に RTV を追加し、VelocityCommon.hlsli の PS 出力を
    //   構造体化して SV_TARGET1.. を足すだけでよい。ここの分岐は増やさないこと。
    enum class PrepassMode : u32
    {
        DepthOnly,
        DepthVelocityGBuffer,
    };
    struct PrepassParams
    {
        PrepassMode         mode = PrepassMode::DepthOnly;
        DirectX::XMFLOAT4X4 prevViewProj{};          // 前フレームの「ジッタなし」viewProj（非転置）
        DirectX::XMFLOAT2   jitterNdc{0.0f, 0.0f};   // 現フレームの NDC ジッタ（速度から除去する量）
        // 半透明(sortKey==3)をプリパスから除外する。カメラのプリパス専用。
        // 影パスでは false のまま＝半透明も従来どおり影を落とす。
        bool                skipTransparent = false;
    };
    void RenderDepthOnlyScene(DirectX::XMMATRIX viewProj, PipelineState& staticPSO,
                              PipelineState& skinnedPSO, bool updateSkinning, u32 frameIndex,
                              u32 lodBias = 0, PipelineState* instPSO = nullptr,
                              const PrepassParams* prepass = nullptr);
    // フレーム描画リスト: Transform+MeshRenderer の走査・ワールド行列合成を1フレーム1回だけ行い、
    // メイン/深度プリパス/CSM各カスケード/スポット影/ポイント影の全パスで共有する
    // （従来は最悪 ~20 パス × entt 全走査 + ComputeWorldMatrix 再計算）。
    // Grid/Pfx/park(scale≈0)/メッシュ無しは従来どおり除外。sortKey 順ソート済み＝PSO 切替を最小化。
    // 各パスは自分の視錐台でこのリストを球カリングして消費する（保守的＝偽陰性なし。
    // CSM はタイトフィット正射 + DepthClipEnable=TRUE で今もクリップされる範囲しか落ちない）。
    // ※ DrawItem 本体は renderer/DrawItem.h（エディタのピッキングからも読むため独立ヘッダ）。
    std::vector<DrawItem> m_drawItems;
    void BuildDrawList();
    u32 m_statDraws  = 0;   // フレーム内の DrawIndexedInstanced 発行数（ビューポートHUD用）
    u32 m_statCulled = 0;   // フレーム内にフラスタムカリングで省いたエンティティ描画の延べ数
    u32 m_statTris   = 0;   // フレーム内に発行した三角形数（全パス・インスタンス数込み）

    // パス別内訳（perf_stats 用）。描画サイトは m_passBucket に加算し、Render() が
    // パス境界でバケツを差し替える（main=メインビュー / shadow=CSM+スポット+ポイント影 / other=それ以外）。
    struct PassStats { u32 draws = 0, tris = 0; };
    PassStats  m_passMain, m_passShadow, m_passOther;
    PassStats* m_passBucket = &m_passOther;

    // CPU パス別内訳（perf_stats 用）。GPU が暇なのに fps が出ない時、
    // どのブロックが CPU 時間を食っているかを直接指す（workMs の内訳）。
    // ※ CpuScopeTimer（RAII）は core/CpuScope.h。エディタ側からも同じ配列へ加算する。
    f32 m_cpuMs[CpuScopeCount] = {};

    // ---- パフォーマンス診断（MCP perf_stats / benchmark 用）----
    // GPU パス別タイムスタンプ。Render() 内の各パスを挟んで計測（結果は約3フレーム遅れ）。
    std::unique_ptr<GpuTimer> m_gpuTimer;
    static constexpr u32 kPerfGpuScopes = 8;   // >= GpuTimer::Scope::Count（cpp で static_assert）
    struct PerfFrame
    {
        f32 frameMs;       // フレーム間隔（リミッター/VSync 込み＝実 FPS の逆数）
        f32 workMs;        // フレーム処理時間（リミッター前。Update+Render+MCP）
        f32 fenceWaitMs;   // BeginFrame のフェンス待ち（GPU が追いつかないと増える）
        f32 presentMs;     // Present+EndFrame（VSync 待ち込み）
        u32 draws, culled, tris;
        PassStats passMain, passShadow;   // パス別内訳（other = 全体との差分で出せるので持たない）
        f32 gpuMs[kPerfGpuScopes];
        f32 cpuMs[CpuScopeCount];         // CPU ブロック別内訳（workMs の中身）
    };
    static constexpr u32 kPerfHistory = 240;   // 直近 240 フレーム（60fps で 4 秒）
    std::array<PerfFrame, kPerfHistory> m_perfHistory{};
    u64 m_perfTotalFrames = 0;   // 書き込んだ延べ数（リング位置 = % kPerfHistory）
    std::chrono::high_resolution_clock::time_point m_perfPrevFrame{};
    bool m_perfPrevFrameValid = false;
    f32 m_perfFenceWaitMs = 0.0f;   // Render() 内で計測 → RecordPerfFrame が読む
    f32 m_perfPresentMs   = 0.0f;
    void RecordPerfFrame();   // Render() 末尾（EndFrame 後）で1回呼ぶ

    // benchmark（遅延応答）: N フレームの perf を貯めて統計を返す
    McpDeferred      m_benchReply{};
    std::vector<f32> m_benchSamples;      // frameMs
    u32              m_benchFramesLeft = 0;
    f64 m_benchDraws = 0, m_benchCulled = 0, m_benchTris = 0;
    f64 m_benchWork = 0, m_benchFence = 0, m_benchPresent = 0;
    f64 m_benchGpu[kPerfGpuScopes] = {};
    f64 m_benchCpu[CpuScopeCount] = {};
    // uncap: 計測中だけ FPS 上限/VSync を外す（既定 true）。終了時に元へ戻す。
    bool m_benchRestore = false;
    f32  m_benchSavedFpsLimit = 0.0f;
    bool m_benchSavedVsync = false;
    void RebuildScene();
    // シェーダーホットリロード用 PSO 再生成。初回(Initialize)と再生成(hot-reload)の両方から呼ぶ。
    // 既存 unique_ptr が非 null ならその場で Initialize() し直す(オブジェクトの住所は変えない=
    // ModelThumbnailRenderer 等が生ポインタを保持しているケースでのダングリングを避けるため)。
    // ShaderManager::RegisterReloadHandler で csoName ごとに束ねて登録する。
    void RecreateForwardPsos();          // m_pipelineState / LEqual / Thumb (Forward_VS/PS, ForwardLdr_PS)
    void RecreateSkinnedPsos();          // m_skinnedPipelineState / LEqual (ForwardSkinned_VS, Forward_PS)
    void RecreateGridPso();              // m_gridPipelineState (ForwardGrid_VS/PS)
    void RecreateEmissivePso();          // m_emissivePipelineState (Emissive_VS/PS)
    void RecreateShadowPsos();           // m_shadowPipelineState / m_shadowSkinnedPipelineState
    void RecreateDepthPrepassPsos();     // m_depthPrepassPSO / m_depthPrepassSkinnedPSO
    void RecreateVelocityPsos();         // m_velocityPSO / Inst / Skinned（深度+速度プリパス）
    void InvalidateTemporalHistory();    // TAA 履歴 + 前フレーム行列を捨てる（シーン切替/Play遷移/リサイズ）
    void EnsureInstancePrevBuffer();     // 速度パス用 per-instance 前ワールドバッファの遅延確保
    void RegisterShaderReloadHandlers(); // 上記全部+PostProcess等を ShaderManager に束ねて登録する(Initialize末尾で1回)

    // カスタムシェーダー(MeshRenderer::shaderPath)割当用の遅延生成PSOキャッシュ。
    // 静的メッシュのみ対応(スキンド/インスタンシングは m_pipelineState 等の既定へフォールバック)。
    struct CustomForwardPsos
    {
        std::unique_ptr<PipelineState> less;       // DepthFunc=LESS、不透明
        std::unique_ptr<PipelineState> lequal;     // 深度プリパス併用(SSAO)時、不透明
        std::unique_ptr<PipelineState> lessBlend;   // DepthFunc=LESS、アルファブレンド(shaderAlphaBlend=true)
        std::unique_ptr<PipelineState> lequalBlend; // 深度プリパス併用時、アルファブレンド
        bool valid = false;
    };
    std::unordered_map<std::string, CustomForwardPsos> m_customPsoCache;  // key: shaderPath(小文字正規化)
    // キャッシュに無ければ ShaderManager からバイトコードを取り PSO を生成する。
    // コンパイル未完了/PSO生成失敗時は nullptr を返す(呼び出し側は既定 Forward へフォールバックすること)。
    CustomForwardPsos* EnsureCustomPso(const std::string& shaderRel);

    // カスタムシェーダー(Sprite2D::shaderPath)割当用の遅延生成PSOキャッシュ。world space スプライトのみ対応。
    // メッシュ版と違い深度プリパスの区別が無いため2種類(不透明/アルファブレンド)のみで足りる。
    struct CustomSpritePsos
    {
        std::unique_ptr<PipelineState> opaque;  // BlendEnable=FALSE
        std::unique_ptr<PipelineState> blend;   // SrcAlpha/InvSrcAlpha(Sprite2D::shaderAlphaBlend=true)
        bool valid = false;
    };
    std::unordered_map<std::string, CustomSpritePsos> m_customSpritePsoCache;  // key: shaderPath(小文字正規化)
    CustomSpritePsos* EnsureCustomSpritePso(const std::string& shaderRel);
    // EnsureCustomPso/EnsureCustomSpritePso共通のバイトコード取得(エディタ=ShaderManager実行時コンパイル、
    // ゲーム=ビルド焼き込みcso)。取得できなければ false(呼び出し側は既定シェーダーへフォールバック)。
    bool FetchCustomShaderBytecode(const std::string& shaderRel,
                                    std::vector<u8>& vsStorage, std::vector<u8>& psStorage,
                                    const std::vector<u8>*& vsBytesOut, const std::vector<u8>*& psBytesOut);

    // MeshRenderer::overrideAlbedoTexture 等(インスタンス単位のマテリアルテクスチャ上書き、
    // アセットブラウザからのテクスチャD&D用)の SRV ブロックキャッシュ。Mesh::GetMaterial() は
    // 同一モデルパスの全インスタンスで共有されるため、上書き分だけこの専用ブロックへ
    // albedo/normal/metalRoughness の3連続SRVを合成し、通常の mat->srvBlockIndex の代わりに使う。
    struct MaterialOverrideSrv
    {
        u32 blockStart = 0xFFFFFFFF;   // srvHeap 上の3連続ブロックの先頭(未確保なら0xFFFFFFFF)
        std::string albedoPath, normalPath, mrPath;  // 直近ビルド時の上書きパス(変化検知用)
    };
    // key = (entityID << 16) | submeshIndex。エンティティ削除時の明示破棄は行わない
    // (ParticleSystem 同様、無効エンティティは次回描画されない=実害なし。SRVヒープを僅かに消費し続ける
    // 既知の制約)。
    std::unordered_map<u64, MaterialOverrideSrv> m_materialOverrideSrvCache;
    // 上書きが無ければ 0xFFFFFFFF を返す(呼び出し側は mat->srvBlockIndex 等の既定経路へフォールバック)。
    u32 EnsureMaterialOverrideSrv(entt::entity e, u32 submeshIndex, const MeshRenderer& renderer,
                                  const Material* mat, ID3D12GraphicsCommandList* cmdList);

    // .dxmat マテリアルアセット(assets/materials/*.dxmat)のロード/SRV/ホットリロード管理。
    // MeshRenderer::materialAsset が割当てられているサブメッシュはこちらが overrideXxxTexture より優先される。
    std::unique_ptr<MaterialAssetManager> m_materialAssetManager;

    // ランチャーで選んだ/作成したプロジェクトを実行時に読み込む（パス再ポイント + シーンロード）
    void LoadProject(const ProjectInfo& info);
    // エディタUIアイコン(PNG)をSRVへ読み込む（起動時に1度。エンジン側assets基準）
    void LoadEditorIcons(ID3D12GraphicsCommandList* cmdList);
    // シーンの SkyboxSettings に応じて環境キューブを読み込み IBL をベイクする（path 差分で再ベイク）。
    // cmd は記録用。呼び出し側で Execute+WaitIdle されている前提。
    void LoadSkyboxIfNeeded(ID3D12GraphicsCommandList* cmd);
    // 非同期プロジェクトロード: 作成/読込のCPU処理をワーカーで回しローディング表示
    void BeginProjectLoad(const ProjectInfo& info, bool isNew);
    void UpdateProjectLoad(f32 dt);   // 毎フレーム状態機械を進める（!m_loading なら何もしない）
    void RenderLoadingOverlay();      // ローディングオーバーレイ描画

    // ---- 段階的シーンロード（重いシーンでウィンドウが固まらないようにする）----
    // シーンが参照するテクスチャ/モデルのロードを複数フレームへ分割し、その間ずっと
    // ローディング UI を描く。全部キャッシュに載ってから SceneSerializer::Load を
    // 走らせるので、実体化フェーズは一瞬で終わる。
    // 軽いシーン（参照アセットが kSceneLoadAsyncThreshold 未満）は従来どおり同期ロード。
    void BeginSceneLoadJob(const std::string& fullPath, const std::string& rel, bool runtime);
    void UpdateSceneLoadJob(ID3D12GraphicsCommandList* cmdList);  // 毎フレーム少しずつ進める
    void RenderSceneLoadingOverlay();  // ロード中のオーバーレイ（進捗バー付き）
    // 参照アセット1件をキャッシュへ載せる（先読み / 段階ロードの両方から使う）
    void WarmSceneAssetRef(const std::string& rel, ID3D12GraphicsCommandList* cmdList);
    void FinishSceneLoad(const std::string& fullPath, const std::string& rel, bool runtime,
                         ID3D12GraphicsCommandList* cmdList);  // 実体化（同期/非同期の共通後段）
    bool IsSceneLoadJobActive() const { return m_sceneLoadJob != nullptr; }
    void RenderWhatsNewPopup();       // 版が変わった初回起動だけ「更新内容」モーダルを出す
    // エディタの「Project」「Version Control」ウィンドウ描画（ランチャー閉後）
    void RenderProjectWindow();
    void RenderVersionControlWindow();
    // タイトルバーの X（WM_CLOSE）横取りハンドラ。true=そのまま終了、false=横取りして呑み込んだ
    // （ランチャーに戻した/Playを止めた等）。Window::SetCloseHandler に渡す。
    bool HandleWindowCloseRequest();
    void RenderBuildSettingsWindow();   // 「ビルド設定」窓（構成/開始シーン/出力先 → ビルド実行）
    // git/gh 操作をワーカースレッドで実行（メインスレッドを固めない）。
    // task はワーカー上で走り GitResult を返す。label はバナー表示名。
    // isLogin=true のときは完了時に GitHub ユーザー名を取り込む（ログインポーリング用）。
    void RunGitAsync(const std::string& label, std::function<GitResult()> task, bool isLogin = false);
    void UpdateGitOp();   // 毎フレーム: ワーカー完了を回収して結果を反映
    // 現在のプロジェクトを保存（.dx12proj + 現在シーン）
    void SaveCurrentProject();
    void EnterPlayMode();
    // 「テストクライアント起動」ボタン(フェーズ⑨): 同じ exe を --net-client 付きで別プロセス起動。
    void LaunchNetTestClient();
    void EnterEditorMode();
    // エディタで開いたシーンに GridPlane が無ければ床グリッドを足す。
    // 旧シーンや Grid 未配置のテンプレ(platformer 等)を開いてもグリッドが必ず出るようにする。
    // ゲームモードでは何もしない。Scene に有効な cmdList が設定済みの状態で呼ぶこと。
    void EnsureEditorGrid();
    bool BuildGame();  // 成否を返す（早期 return = 失敗）
    // グローバル game.lua をロード（ScriptEngine 再初期化のたびに呼ぶ）。
    // ゲームモードは pak から読むのでディスク存在チェックを迂回する。
    void LoadGameScript();

    // Lua の loadScene/nextScene/quit/ui コールバックを ScriptEngine に注入（再生成のたび呼ぶ）
    void WireScriptCallbacks();
    // アクティブな CameraComponent をグローバル Camera に同期（Play 開始 / loadScene 後）
    void SyncActiveCameraToGlobal();
    // カメラエンティティの「親階層込みワールド変換」をグローバル Camera の
    // 位置・yaw・pitch に反映する（親オブジェクトにアタッチしたカメラを追従させる）。
    void ApplyCameraTransformToGlobal(entt::entity camEntity);
    // Play 中のシーン切替（フレーム境界で安全に実行）
    void DoRuntimeSceneLoad(const std::string& assetsRelPath, ID3D12GraphicsCommandList* cmdList);
    void DoScenePreload(const std::string& assetsRelPath, ID3D12GraphicsCommandList* cmdList);

    std::unique_ptr<Window>         m_window;
    std::unique_ptr<GraphicsDevice> m_graphicsDevice;
    std::unique_ptr<CommandQueue>   m_commandQueue;
    std::unique_ptr<SwapChain>      m_swapChain;
    std::unique_ptr<FrameResources> m_frameResources;
    std::unique_ptr<DescriptorHeap>    m_descriptorHeap;
    std::unique_ptr<DescriptorHeap>    m_dsvHeap;
    std::unique_ptr<RootSignature>     m_rootSignature;
    std::unique_ptr<PipelineState>     m_pipelineState;        // 通常 forward(static, DepthFunc=LESS)
    std::unique_ptr<PipelineState>     m_pipelineStateLEqual;  // SSAO 深度プリパス併用時(static, LESS_EQUAL)
    std::unique_ptr<PipelineState>     m_pipelineStateThumb;   // サムネイル用(static, LESS, R8G8B8A8)
    // 自動インスタンシング用(slot1=MeshInstanceData)。同一メッシュ+同一マテリアルの
    // 連続ドローを 1 回の DrawIndexedInstanced に畳む。PS は Forward_PS を共用。
    std::unique_ptr<PipelineState>     m_pipelineStateInst;
    std::unique_ptr<PipelineState>     m_pipelineStateInstLEqual;
    std::unique_ptr<DescriptorHeap>    m_srvHeap;
    std::unique_ptr<ResourceManager>   m_resourceManager;
    // プロジェクト独自HLSL(上書き/自作)の実行時コンパイル+ホットリロード。エディタモードのみ生成。
    std::unique_ptr<ShaderManager>     m_shaderManager;
    f32                                m_shaderPollTimer = 0.0f;
    std::unique_ptr<ImGuiManager>      m_imguiManager;
    std::unique_ptr<UiTestHarness>     m_uiTests;              // --ui-tests のときだけ生成
    bool m_uiTestsRequested = false;
    bool m_uiTestsRunAll    = false;
    bool m_uiTestsDeepOnly  = false;   // --ui-tests-deep（超詳細診断だけ走らせる）
    int  m_uiTestsSpeed     = 0;
    int  m_uiTestExitCode   = 0;
    int  m_diagModeRequest  = 0;   // 診断からの Play/Stop 要求（0=なし 1=Editor 2=Playing）
    bool m_diagFrameStatsRequest = false;   // 超詳細診断からのフレーム読み戻し要求
    DiagFrameStats m_diagFrameStats;        // 直近の測定結果（Take で valid を落とす）
    std::unique_ptr<PipelineState>     m_skinnedPipelineState;        // 通常 forward(skinned, LESS)
    std::unique_ptr<PipelineState>     m_skinnedPipelineStateLEqual;  // SSAO 深度プリパス併用時(skinned, LESS_EQUAL)
    std::unique_ptr<PipelineState>     m_gridPipelineState;
    std::unique_ptr<PipelineState>     m_emissivePipelineState;  // 加算発光（Pfx）GPU instancing 用
    // ---- 発光メッシュ instancing 用 per-instance バッファ（リング=FrameResources::kFrameCount=3）----
    // 自動インスタンシングは 1 フレームで「メイン+深度プリパス+影4カスケード」ぶんの
    // インスタンスを同じリングへ連番で書くので、可視物量 × 6 くらいの余裕が要る。
    static constexpr u32 kMaxInstances = 262144;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceBuffer[3];
    uint8_t*                               m_instanceMapped[3] = {};
    D3D12_VERTEX_BUFFER_VIEW               m_instanceVbView[3] = {};
    u32                                    m_instanceCursor = 0;  // フレーム内 instance 連番（メイン＋プレビュー共有）
    // 速度パス用の per-instance「前フレームのワールド行列」バッファ（slot2, stride=48）。
    // TAA 有効時のみ EnsureInstancePrevBuffer() で遅延確保する（+37MB を使わない人に払わせない）。
    // インデックスは m_instanceBuffer と同じ base/count を共有する＝並びが必ず揃う。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instancePrevBuffer[3];
    uint8_t*                               m_instancePrevMapped[3] = {};
    D3D12_VERTEX_BUFFER_VIEW               m_instancePrevVbView[3] = {};
    std::unique_ptr<PipelineState>     m_shadowPipelineState;
    std::unique_ptr<PipelineState>     m_shadowPipelineStateInst;   // 影パスのインスタンシング版
    std::unique_ptr<PipelineState>     m_depthPrepassPSOInst;       // 深度プリパスのインスタンシング版
    std::unique_ptr<PipelineState>     m_shadowSkinnedPipelineState;
    // ---- CSM (Cascaded Shadow Maps, 4分割) ----
    static constexpr u32 kNumCascades = 4;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;        // Texture2DArray(ArraySize=kNumCascades)
    std::unique_ptr<DescriptorHeap>    m_shadowDsvHeap;        // 容量 kNumCascades
    D3D12_CPU_DESCRIPTOR_HANDLE        m_shadowDsvHandles[kNumCascades]{}; // スライス毎の DSV
    u32                                m_shadowSrvIndex = 0;    // 配列SRV(1個)
    u32                                m_shadowMapSize = 2048;  // 4096→2048: トップダウンで影は小さく、1/4の帯域で十分
    i32                                m_shadowQualityIndex = 2;  // 0:1024, 1:2048, 2:4096, 3:8192
    bool                               m_shadowMapDirty = false;
    // CSM パラメータ（ImGui 編集用）
    f32                                m_cascadeSplitLambda = 0.5f;  // 0=一様, 1=対数
    f32                                m_cascadeBlendBand   = 1.5f;  // 境界ブレンド幅(view深度)
    f32                                m_shadowDepthBias    = 0.0015f; // shadowParams.y: 受光面アクネ/ピーターパン調整
    bool                               m_showCascadeDebug   = false;
    // フレーム毎に計算した結果（描画パス間で共有）
    DirectX::XMFLOAT4X4                m_cascadeViewProj[kNumCascades]{};  // 行優先(world*VP用、非転置)
    f32                                m_cascadeSplitsView[kNumCascades]{}; // 各カスケード遠端 view深度(正値)

    // ---- スポット/ポイントライトの影 ----
    // castShadows=true のライトのうちカメラに近い順で固定スロットへ毎フレーム割当（多数灯があっても
    // 上限を超えた分は影なしにフォールバック＝CSMと同じ「固定配列」方針）。
    static constexpr u32 kMaxShadowSpot      = 4;   // スポット影の同時上限
    static constexpr u32 kMaxShadowPoint     = 2;   // ポイント影の同時上限（6面/灯）
    static constexpr u32 kSpotShadowMapSize  = 1024;
    static constexpr u32 kPointShadowMapSize = 512;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_spotShadowMap;   // Texture2DArray(ArraySize=kMaxShadowSpot)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pointShadowMap;  // Texture2DArray(ArraySize=kMaxShadowPoint*6)。SRVはTextureCubeArrayとして参照
    std::unique_ptr<DescriptorHeap>    m_punctualShadowDsvHeap;  // 容量 kMaxShadowSpot + kMaxShadowPoint*6
    D3D12_CPU_DESCRIPTOR_HANDLE        m_spotShadowDsvHandles[kMaxShadowSpot]{};
    D3D12_CPU_DESCRIPTOR_HANDLE        m_pointShadowDsvHandles[kMaxShadowPoint * 6]{};
    u32                                m_spotShadowSrvIndex  = 0;
    u32                                m_pointShadowSrvIndex = 0;  // 初期化時に spotIndex+1（連番）である前提でRootSig側テーブルを組む
    // フレーム毎のスロット割当結果（影パス描画とCB書き込みの両方で参照）
    DirectX::XMFLOAT4X4                m_spotShadowViewProj[kMaxShadowSpot]{};  // 行優先(world*VP用、非転置)
    entt::entity                       m_spotShadowEntity[kMaxShadowSpot]{};
    u32                                m_numSpotShadowSlots = 0;
    // ポイント影: シェーダ側は距離ベースで比較深度を再構成するので面ごとのVPはCBへ渡さない
    // （描画パス内のローカル変数で完結）。永続する必要があるのは「どの灯がどのキューブスロットか」のみ。
    entt::entity                       m_pointShadowEntity[kMaxShadowPoint]{};
    u32                                m_numPointShadowSlots = 0;
    // エディタレイアウト
    static constexpr f32 kLeftPanelWidth  = 280.0f;
    static constexpr f32 kToolbarHeight   = 68.0f;  // タイトルバー兼メニューバー(~35px) + アイコン列の2段
    // プロジェクトロード時のマテリアルサムネイル事前生成の総数(進捗表示用。UpdateProjectLoadフェーズ4)
    size_t m_matThumbPreloadTotal = 0;
    bool m_isGameMode = false;
    std::unique_ptr<EditorContext> m_editorCtx;
    std::unique_ptr<EditorLayer>   m_editorLayer;
    std::unique_ptr<ModelThumbnailRenderer> m_thumbRenderer;
    bool m_showLauncher = true;  // プロジェクトランチャー表示フラグ

    // ---- 「更新内容」ポップアップ（版が変わった初回起動だけ表示）----
    bool m_showWhatsNew   = false;  // この起動で出すべきか（Initialize で版マーカーと比較して決定）
    bool m_whatsNewOpened = false;  // OpenPopup を1回だけ呼ぶためのラッチ

    // ---- プロジェクト管理 / バージョン管理(Git) ----
    ProjectInfo m_projectInfo;            // 現在開いているプロジェクト（rootDir 空 = 組み込みパス）
    bool        m_gitChecked   = false;   // git/gh の存在チェック済みか
    bool        m_gitAvailable = false;
    bool        m_ghAvailable  = false;
    std::array<char, 512>  m_gitRemoteBuf{};    // リモート URL 入力
    std::array<char, 256>  m_gitCommitMsgBuf{}; // コミットメッセージ入力
    std::string m_gitOutput;              // 直近コマンドの出力ログ
    bool        m_gitRepoCache = false;   // IsRepo の簡易キャッシュ
    std::string m_gitBranchCache;         // ブランチ名キャッシュ
    std::string m_gitRemoteCache;         // origin URL キャッシュ
    bool        m_gitForceRefresh = false; // 操作直後に git 状態を再取得するワンショットフラグ

    // GitHub ログイン状態（gh）
    std::string m_ghUser;                 // gh ログインユーザー（空=未ログイン）
    bool        m_ghUserChecked = false;  // ログイン状態を取得済みか（重いので一度だけ）

    // GitHub に新規作成するリポジトリ名（初回はプロジェクト名で事前入力、編集可）
    std::array<char, 100> m_gitNewRepoNameBuf{};

    // ブランチ操作
    std::vector<std::string> m_gitBranches;     // ローカルブランチ一覧
    std::array<char, 128>    m_gitNewBranchBuf{}; // 新規ブランチ名入力

    // upstream に対する未送信/未取得コミット数（VS の ↑/↓ 表示用。-1=upstream無し/未取得）
    int         m_gitAhead  = -1;
    int         m_gitBehind = -1;
    std::array<char, 512> m_gitCloneBuf{};      // クローン元 URL 入力
    std::vector<GitFileChange> m_gitChanges;    // 変更ファイル一覧（VS の「変更」ツリー用）

    // マージコンフリクト（pull 等で発生）
    bool                     m_gitMergeInProgress = false; // .git/MERGE_HEAD の有無
    std::vector<std::string> m_gitConflicts;               // 未解消ファイルのパス一覧

    // ---- 非同期 git 操作（メインスレッドを固めないためのワーカー） ----
    enum class GitOpStatus { None, Running, Success, Failure };
    std::thread        m_gitThread;
    std::atomic<bool>  m_gitOpDone{false};       // ワーカー→メインの完了フラグ（happens-before バリア）
    std::atomic<bool>  m_gitAbort{false};        // シャットダウン要求（ログインポーリングを早期中断）
    bool               m_gitOpRunning = false;   // メインスレッドのみが触る UI ゲート
    bool               m_gitPendingOk = false;   // ワーカーが書く: 成否
    bool               m_gitOpIsLogin = false;   // ログインポーリング中か（完了で m_ghUser 更新）
    std::string        m_gitPendingOutput;       // ワーカーが書く: 出力（done 後にメインが読む）
    GitOpStatus        m_gitOpStatus = GitOpStatus::None; // 直近操作の状態（バナー）
    std::string        m_gitOpLabel;             // 直近操作名（"プッシュ" 等）
    float              m_gitSpin = 0.0f;         // 実行中スピナーのアニメ時間
    bool               m_gitInstallPending = false; // Git インストール中→完了後に m_gitChecked を再評価

    // ---- エディタUIアイコン（ImTextureID=ImU64。0=未読込。EditorContext::icons から参照される）----
    EditorUiIcons m_icons;

    // ---- 非同期プロジェクトロード ----
    std::thread        m_loadThread;
    std::atomic<bool>  m_loadThreadDone{false};
    bool               m_loadThreadRunning = false;
    bool               m_loading           = false;  // ローディングオーバーレイ表示中
    bool               m_loadProjectStarted = false; // LoadProject 発火済みか
    bool               m_loadIsNew         = false;
    int                m_loadSceneWaitFrames = 0;
    f32                m_loadSpinTime      = 0.0f;
    std::string        m_loadStatus;
    ProjectInfo        m_loadInfo;

    // ---- 段階的シーンロード ----
    struct SceneLoadJob
    {
        std::string              fullPath;    // シーンの絶対パス
        std::string              rel;         // assets 相対パス
        bool                     runtime = false;  // true=Play中の切替 / false=エディタで開く
        std::vector<std::string> assets;      // 先読み対象（assets 相対）
        size_t                   next  = 0;   // assets の消化位置
        f32                      spin  = 0.0f;
        int                      frames = 0;  // ジョブ開始から進めたフレーム数（0 = UI を出すだけ）
    };
    std::unique_ptr<SceneLoadJob> m_sceneLoadJob;   // null = ロード中でない
    // 参照アセットがこれ以上あるシーンは分割ロード＋ローディング UI に切り替える。
    // これ未満なら1フレームで終わるので従来どおり同期ロード（画面の一瞬のチラつきを避ける）。
    static constexpr size_t kSceneLoadAsyncThreshold = 8;
    // シーン JSON がこのサイズ以上なら、参照アセットが少なくてもローディング UI を出す
    // （エンティティ数が多いシーンは実体化そのものが重く、無言で固まって見えるため）。
    static constexpr uintmax_t kSceneLoadBigFileBytes = 512 * 1024;
    // 1フレームで先読みに使ってよい時間(ms)。超えたら残りは次フレームへ回す＝UIが動き続ける。
    static constexpr f64    kSceneLoadBudgetMs       = 6.0;
    std::unique_ptr<Camera>            m_camera;
    std::unique_ptr<ConstantBuffer>    m_perFrameCB;
    std::unique_ptr<CommandList>       m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE        m_dsvHandle{};
    u32                                m_depthSrvIndex = 0xFFFFFFFFu;  // soft particles 用シーン深度SRV
    std::unique_ptr<InputSystem>       m_inputSystem;
    std::unique_ptr<Scene>             m_scene;
    // ゲームプレイ中のエンジン汎用イベントバス。Play 開始時 Clear、Update 末尾 Flush。
    // 宣言順注意: m_scriptEngine / m_physicsSystem より前に置く。
    // ~Application() が Shutdown() を経由せず自動デストラクタを走らせる場合、
    // C++ は逆宣言順にデストラクトするため m_eventBus が後に置かれていると
    // ~ScriptEngine()/~PhysicsSystem() が m_eventBus.Clear() を踏んで UAF になる。
    EventBus                           m_eventBus;
    std::unique_ptr<ScriptEngine>      m_scriptEngine;
    std::unique_ptr<McpBridge>         m_mcpBridge;   // エディタ専用 AI ブリッジ(TCP)。ゲームでは null。
    std::unique_ptr<VfxEditorPanel>    m_vfxEditorPanel;   // パーティクルエディタ（ツール窓）。ゲームでは null。
    std::unique_ptr<UiEditorPanel>     m_uiEditorPanel;    // UIエディタ（ゲーム内UIの2Dキャンバス編集）。ゲームでは null。
    std::unique_ptr<AnimationEditorPanel>   m_animEditorPanel;        // .uianim タイムライン。ゲームでは null。
    std::unique_ptr<SpriteSheetEditorPanel> m_spriteSheetEditorPanel; // .spranim シート編集。ゲームでは null。
    std::unique_ptr<MaterialEditorPanel>  m_materialEditorPanel;   // マテリアルエディタ（ツール窓）。ゲームでは null。
    std::unique_ptr<MaterialLibraryPanel> m_materialLibraryPanel;  // Poly Havenマテリアルライブラリ(ツール窓)。ゲームでは null。
    // ---- MCP 状態（HandleMcpCommand とフレーム境界の遅延応答で共有）----
    int         m_sceneGeneration = 0;   // open_scene/new_scene のたびに +1。古い entity id 検出用。
    McpDeferred m_mcpModeReply;          // play/stop の遅延応答（モード遷移後に送る）。client=0 で無効。
    McpDeferred m_mcpLoadReply;          // open_scene の遅延応答（ロード完了後に送る）。client=0 で無効。
    McpDeferred m_mcpStepReply;          // step_frames の遅延応答（N フレーム経過後に送る）。client=0 で無効。
    int         m_mcpStepFramesLeft = 0; // step_frames で残り何フレーム回すか。0 で非アクティブ。
    McpDeferred m_mcpGameViewReply;      // screenshot_game_view の遅延応答（1フレーム描画後に送る）。client=0 で無効。
    std::unordered_map<std::string, uint32_t> m_mcpIdempotency;  // idempotency_key -> 生成済み entityId
    std::unique_ptr<AudioSystem>       m_audioSystem;
    std::unique_ptr<PhysicsSystem>     m_physicsSystem;
    std::unique_ptr<NetworkSystem>     m_networkSystem;   // マルチプレイ（GPU非依存、Play/Stopでも再構築しない）
    std::unique_ptr<NetworkPanel>      m_networkPanel;    // マルチプレイのエディタパネル（状態/設定窓）。ゲームでは null。
    std::string m_pendingNetClientJoin;     // SetNetTestClientJoin で受けた "ip:port"。Initialize 内で1回消費。
    std::string m_pendingNetClientProject;  // SetNetTestProject で受けたプロジェクトルート。同上。
    bool m_netClientAutoPlayPending = false; // --net-client: プロジェクトロード完了後にPlay(Join)する予約。
    std::unique_ptr<PhysicsDebugRenderer> m_physicsDebugRenderer;
    std::unique_ptr<EditorIconRenderer>   m_editorIconRenderer;
    bool                               m_physicsDebugDraw = false;

    // オフスクリーン描画 + ポストプロセス（WP3）
    std::unique_ptr<DescriptorHeap> m_offscreenRtvHeap;
    std::unique_ptr<RenderTarget>   m_sceneRT;
    std::unique_ptr<PostProcess>    m_postProcess;
    std::unique_ptr<BloomPass>      m_bloomPass;     // 物理ベースブルーム（ダウン/アップチェーン）
    std::unique_ptr<AutoExposurePass> m_autoExposure; // 自動露出（compute ヒストグラム）
    std::unique_ptr<GodRaysPass>    m_godRaysPass;    // スクリーンスペース ゴッドレイ
    std::unique_ptr<LensFlarePass>  m_lensFlarePass;  // 疑似レンズフレア（ブルームチェーン入力）
    std::unique_ptr<DofPass>        m_dofPass;        // 被写界深度（gather ボケ）
    std::unique_ptr<MotionBlurPass> m_motionBlurPass; // カメラモーションブラー（深度再構成）
    std::unique_ptr<RenderTarget>   m_distortRT;      // パーティクル歪みバッファ（RG16F、熱ゆらぎ/衝撃波）
    DirectX::XMFLOAT4X4             m_prevViewProj{};       // 前フレームの viewProj（モーションブラー用）
    bool                            m_prevViewProjValid = false;

    // 遅延初回表示: 隠れたまま数フレーム描画→絵が確定してから Show + スプラッシュ Close
    bool m_deferredFirstShow = false;
    int  m_warmupFrames      = 0;
    std::unique_ptr<ParticleSystem> m_particleSystem;  // 加算ビルボードパーティクル（Lua fx API）
    std::unique_ptr<GpuParticleSystem> m_gpuParticles; // GPUパーティクル（compute+indirect、大量粒子用）

    // ---- SSAO（深度プリパス + 深度再構築法線 半球カーネルAO + ブラー）----
    std::unique_ptr<SSAOPass>      m_ssaoPass;                       // AO 生成器
    std::unique_ptr<Texture>      m_ssaoWhiteTex;                   // 1x1 白 R8_UNORM（AO=1.0 ダミー）
    u32                           m_ssaoWhiteSrvIndex = 0xFFFFFFFFu; // 白AOダミー SRV index
    std::unique_ptr<PipelineState> m_depthPrepassPSO;               // 深度プリパス（static, bias なし）
    std::unique_ptr<PipelineState> m_depthPrepassSkinnedPSO;        // 深度プリパス（skinned）

    // ---- コンタクトシャドウ（同じ深度プリパスを使うスクリーン空間レイマーチ）----
    // 白ダミーは SSAO と共用（どちらも 1x1 R8_UNORM の 1.0）。
    std::unique_ptr<ContactShadowPass> m_contactShadowPass;

    // ---- TAA（速度バッファ + ジッタ + 履歴再投影）----
    // 深度プリパスを MRT 化して速度(RG16F)を書き、ポストチェーンの先頭（トーンマップ前の
    // HDR）で解決する。ジッタは Camera には一切入れない（ピッキング/MCP 投影を壊さないため）。
    std::unique_ptr<TaaPass>       m_taaPass;
    std::unique_ptr<PipelineState> m_velocityPSO;          // 深度+速度（static）
    std::unique_ptr<PipelineState> m_velocityPSOInst;      // 深度+速度（instanced）
    std::unique_ptr<PipelineState> m_velocityPSOSkinned;   // 深度+速度（skinned, t12=前ボーン）
    DirectX::XMFLOAT4X4 m_prevViewProjNoJitter{};          // 前フレームの「ジッタなし」viewProj
    bool                m_prevViewProjNJValid = false;
    // 前フレームの frameIndex（SkinningBuffer の「前フレームのスロット」を指すため）。
    // GetCurrentBackBufferIndex() の巡回順は DXGI 仕様上保証されないので明示記録する。
    u32                 m_prevFrameIndex = 0;
    bool                m_prevFrameIndexValid = false;
    DirectX::XMFLOAT2   m_taaJitterNdc{};                  // 現フレームの NDC ジッタ
    // BuildDrawList() が PrevWorldMatrix を更新するか（TAA 有効時のみ。10万体の
    // emplace_or_replace を TAA を使わない人に払わせないため）。
    bool                m_trackPrevWorld = false;

    // IBL 環境マップ（irradiance/prefiltered/BRDF LUT）+ 任意スカイボックス
    std::unique_ptr<IBLBaker>       m_iblBaker;
    std::unique_ptr<SkyboxRenderer> m_skyboxRenderer;
    std::unique_ptr<Texture>        m_envCubeTex;      // 環境キューブ本体（リソース保持）
    u32   m_envCubeSrvIndex   = 0xFFFFFFFFu;           // 環境キューブの TextureCube SRV index
    bool  m_iblReady          = false;                 // baking 完了し SRV 有効
    float m_iblIntensity      = 1.0f;
    float m_skyboxIntensity   = 1.0f;
    bool  m_drawSkybox        = true;
    std::string m_loadedSkyboxPath;                    // 二重ロード防止
    bool  m_skyboxDirty       = false;                 // エディタでパス変更時に再ベイク要求

    // カメラプレビュー（選択カメラ視点を小窓表示）。専用 RT + 専用 per-frame CB。
    std::unique_ptr<RenderTarget>   m_cameraPreviewRT;
    std::unique_ptr<RenderTarget>   m_cameraPreviewLdrRT;  // プレビュー表示用(トーンマップ済みLDR)
    std::unique_ptr<ConstantBuffer> m_previewFrameCB;

    // 2D スプライト / ゲーム内 UI 描画（WP4 / WP7）
    std::unique_ptr<SpriteRenderer> m_spriteRenderer;

    // ゲーム内 UI（Lua 即時モード）コマンドバッファ（WP7）
    struct UICommand
    {
        enum class Type { Text, Button, Image, Rect } type = Type::Text;
        float x = 0, y = 0, w = 0, h = 0, size = 24.0f;  // Rect では size=角丸半径
        float r = 1, g = 1, b = 1, a = 1;
        std::string text;  // text 内容 / button ラベル / image パス
    };
    std::vector<UICommand>          m_uiCommands;     // 今フレームの UI 描画要求
    std::unordered_set<std::string> m_pressedButtons; // 前フレームに押されたボタンのラベル

    // ゲーム内 retained-mode UI（UICanvas/UIRect/UIImage/UIText/UIButton の
    // レイアウト解決・描画・入力）。ImGui DrawList 経路で GPU リソースは持たない。
    std::unique_ptr<UISystem> m_uiSystem;

    // タイムライン製 UI アニメ(.uianim)とスプライトシート(.spranim)の再生。
    // UISystem と別なのは、SpriteAnimator が UI ではない Sprite2D にも効くため
    // （##GameUI 描画時にしか回らない UISystem に置くと 2D スプライトへ届かない）。
    std::unique_ptr<UiAnimRuntime> m_uiAnimRuntime;

    // シーントランジション（WP9）
    std::unique_ptr<SceneTransition> m_sceneTransition;
    std::string                      m_transitionTargetScene;  // 中間点でロードする assets 相対パス（空=ロード無し）
    std::vector<std::string>         m_pendingScenePreloads;   // Lua preloadScene の保留分（cmdList のあるフレーム境界で処理）

    // シーンフロー（WP6）
    std::unique_ptr<SceneFlow> m_sceneFlow;
    GameClock                          m_gameClock;
    bool                               m_isRunning = false;
    u32                                m_framesSinceStart = 0;

    // エディタ/プレイモード
    EngineMode m_engineMode = EngineMode::Editor;
    EngineMode m_pendingMode = EngineMode::Editor;
    bool m_modeChangeRequested = false;
    bool m_editorWas2D = false;  // 2Dビューモードのトグル検出（OFF復帰で透視へ戻す）
    struct CameraSnapshot {
        DirectX::XMFLOAT3 position;
        f32 yaw;
        f32 pitch;
    } m_cameraSnapshot{};

    // 2D⇆3D ビュー切替時の 3D カメラ退避。2D は正射＋正面固定で位置を強制するため、
    // 退避しておかないと 3D に戻した時に視点が壊れる（位置/向きが 2D のまま残る）。
    CameraSnapshot m_cam3DSnapshot{ { -14.7f, 9.6f, -9.0f }, 0.0f, 0.0f };
    bool m_has3DSnapshot = false;

    // OnPlayStart 直後に Lua が変更した値をエディタ配置値で打ち消すための即時上書き用スナップショット。
    // Stop 時の完全復元には使わない（そちらは m_playSceneJson 経由）
    struct EntitySnapshot {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 rotation;
        DirectX::XMFLOAT3 scale;
        DirectX::XMFLOAT4 quaternion;
        bool useQuaternion;

        bool      hasRigidBody = false;
        RigidBody rigidBodyData;

        bool             hasPointLight = false;
        PointLight       pointLightData;
        bool             hasDirectionalLight = false;
        DirectionalLight directionalLightData;
        bool             hasSpotLight = false;
        SpotLight        spotLightData;

        bool  hasMeshRenderer = false;
        float materialMetallicOverride  = -1.0f;
        float materialRoughnessOverride = -1.0f;
    };
    std::unordered_map<std::string, EntitySnapshot> m_editorSnapshots;

    // Play 開始時のシーン全体スナップショット（Stop 時の復元用）
    std::string m_playSceneJson;

    // Play 開始時のシーンパス退避。Play 中の loadScene/goToScene が currentScenePath を
    // 書き換えたまま Stop すると、以後のパス無し保存が別シーンを上書きしてしまうため、
    // Stop 時に必ずここへ戻す。
    std::string m_playScenePathSnapshot;
    std::string m_playSceneRelSnapshot;

    // 現在ロード中シーンの assets 相対パス（シーンフロー / loadScene 用）
    std::string m_currentSceneRel;

    // Luaホットリロード
    std::filesystem::file_time_type m_scriptLastWriteTime{};
    f32 m_scriptPollTimer = 0.0f;
    static constexpr f32 kScriptPollInterval = 0.5f;

    // フレームレートリミッター（VSync OFF 時のみ有効。0=無制限。オプション画面から変更可能）
    // 自動インスタンシングの ON/OFF。settings.json の "render_instancing"(0/1) で切替。
    // 最適化の効果測定(A/B)用。既定 ON。
    bool m_instancingEnabled = true;
    f32  m_fpsLimit = 144.0f;
    bool m_useVsync = false;
    std::chrono::high_resolution_clock::time_point m_frameStart{};

    // ---- ユーザー設定の永続化（settings.json）----
    // Lua の savePersist/loadPersist と、映像設定（video_* キー）の保存先。
    // エディタ: <プロジェクト>/settings.json、ゲーム: exe と同じディレクトリ。
    std::filesystem::path PersistPath() const;
    void   LoadPersistStore();
    void   SavePersistStore();
    double PersistGet(const std::string& key, double def);
    void   PersistSet(const std::string& key, double v);
    std::unordered_map<std::string, double> m_persistStore;
    bool m_persistLoaded = false;
};

} // namespace dx12e
