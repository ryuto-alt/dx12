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
    class PostProcess;
    class BloomPass;
    class AutoExposurePass;
    class GodRaysPass;
    class LensFlarePass;
    class DofPass;
    class MotionBlurPass;
    class SSAOPass;
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
    class Scene;
    class ScriptEngine;
    class McpBridge;
    class UISystem;
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

    // ヘッドレスでゲームをビルド（--build CLI 用）。開始シーンは title.json があればそれ。
    // 成否を返す（CLI の終了コード / GUI の完了表示に使う）。
    bool BuildGameStandalone();

    enum class EngineMode { Editor, Playing };

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
    // シーン内の全メッシュを指定 viewProj で描画（メインパスとカメラプレビューで共用）。
    // isGameView=true でエディタ用グリッドを除外。per-frame CB / シャドウSRV /
    // ルートシグネチャ / RT / ビューポートは呼び出し側で設定済みとする。
    // depthPrepassActive=true のときだけ深度プリパス併用用の LESS_EQUAL forward PSO を使う
    // （プリパスが書いた深度を再利用するため）。false の通常経路は LESS PSO（既存 z-fight 挙動を維持）。
    void RenderSceneMeshes(ID3D12GraphicsCommandList* nativeCmdList, u32 frameIndex,
                           DirectX::XMMATRIX viewProj, bool isGameView, u32 aoSrvIndex,
                           bool depthPrepassActive = false);
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
    void RenderDepthOnlyScene(DirectX::XMMATRIX viewProj, PipelineState& staticPSO,
                              PipelineState& skinnedPSO, bool updateSkinning, u32 frameIndex);
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
    std::unique_ptr<DescriptorHeap>    m_srvHeap;
    std::unique_ptr<ResourceManager>   m_resourceManager;
    // プロジェクト独自HLSL(上書き/自作)の実行時コンパイル+ホットリロード。エディタモードのみ生成。
    std::unique_ptr<ShaderManager>     m_shaderManager;
    f32                                m_shaderPollTimer = 0.0f;
    std::unique_ptr<ImGuiManager>      m_imguiManager;
    std::unique_ptr<PipelineState>     m_skinnedPipelineState;        // 通常 forward(skinned, LESS)
    std::unique_ptr<PipelineState>     m_skinnedPipelineStateLEqual;  // SSAO 深度プリパス併用時(skinned, LESS_EQUAL)
    std::unique_ptr<PipelineState>     m_gridPipelineState;
    std::unique_ptr<PipelineState>     m_emissivePipelineState;  // 加算発光（Pfx）GPU instancing 用
    // ---- 発光メッシュ instancing 用 per-instance バッファ（リング=FrameResources::kFrameCount=3）----
    static constexpr u32 kMaxInstances = 4096;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceBuffer[3];
    uint8_t*                               m_instanceMapped[3] = {};
    D3D12_VERTEX_BUFFER_VIEW               m_instanceVbView[3] = {};
    u32                                    m_instanceCursor = 0;  // フレーム内 instance 連番（メイン＋プレビュー共有）
    std::unique_ptr<PipelineState>     m_shadowPipelineState;
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
    std::string m_restoreProjectRoot;       // 通常起動の復元用: lastOpenedScene の上位で見つけた .dx12proj のルート
    std::string m_restoreSceneRel;          // 同・前回シーンの assets 相対パス(BeginProjectLoad で開き直す)
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

    // シーントランジション（WP9）
    std::unique_ptr<SceneTransition> m_sceneTransition;
    std::string                      m_transitionTargetScene;  // 中間点でロードする assets 相対パス（空=ロード無し）

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

    // フレームレートリミッター
    static constexpr f32 kTargetFps = 144.0f;
    bool m_useVsync = false;
    std::chrono::high_resolution_clock::time_point m_frameStart{};
};

} // namespace dx12e
