// ===========================================================================
// Application: 起動 / メインループ / モード遷移の中枢
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AtomicFileJson.h"
#include "core/SceneBackup.h"   // 世代つきバックアップ（.dx12/backups/）
#include "core/ReleaseNotes.h"   // 「更新内容」の前回→今の版の範囲（--show-whats-new の既定の前回版）
#include "resource/AssetPrewarmer.h"   // unique_ptr のデストラクタに完全型が要る
#include "resource/TextureLoader.h"   // TEXBAKE: 使ったキャッシュ一覧の書き出し
#include <fstream>
#include <algorithm>
#include "core/Profiler.h"   // Tracy ゾーン（無効時は完全に消える）
#include "core/mcp/FleetGuard.h"   // --owner-pid / --idle-exit の自己終了
#include "core/SequencerHost.h"       // シーケンサー S1b（unique_ptr<SequencerHost> のデストラクタ / Update / カメラ選択）
#include "editor/panels/MaterialGraphPanel.h"   // マテリアルグラフ G2c: ライブプレビュー / サムネイルの GPU 側（InitializeGpu / ShutdownGpu）

namespace dx12e
{
using namespace appdetail;


Application::Application() = default;

// ── TU 間のレイアウト整合（仕掛けの説明は Application.h の appdetail のところ）──
// 各 TU の静的初期化から呼ばれる。初期化順は不定なので、関数ローカル static で受ける。
namespace appdetail
{
namespace
{
std::size_t& FirstSeenSlot() noexcept { static std::size_t s = 0; return s; }
std::size_t& MismatchSlot()  noexcept { static std::size_t s = 0; return s; }
}

void RegisterApplicationLayout(std::size_t size) noexcept
{
    std::size_t& first = FirstSeenSlot();
    if (first == 0) { first = size; return; }
    if (first != size) MismatchSlot() = size;
}

std::size_t ApplicationLayoutMismatch() noexcept { return MismatchSlot(); }
std::size_t ApplicationLayoutFirstSeen() noexcept { return FirstSeenSlot(); }
} // namespace appdetail

Application::~Application()
{
    if (m_isRunning)
    {
        Shutdown();
    }
}

void Application::Initialize(HINSTANCE hInstance, int nCmdShow, bool gameMode,
                             const ProjectInfo* /*projectInfo*/, bool buildMode)
{
    // ロガー初期化
    Logger::Init();
    m_isGameMode = gameMode;
    m_showLauncher = !gameMode;  // ゲームモードではランチャーを表示しない
    // エディタで、前回表示した版と違う＝更新された/初回 のときだけ「更新内容」を出す。
    // ★自動化（--background / --headless / --virtual-input）では出さない: モーダルがエンジンの操作を塞ぐうえ、
    //   DX12E_DATA_DIR で分離した fleet のエンジンは毎回「初回」扱いになるため。--show-whats-new で検証用に出せる。
    {
        const std::string shown = ReadShownVersion();
        const bool automation = m_headless || m_bgOptions.Active() || m_virtualInputRequested;
        m_whatsNewFrom = shown;
        m_whatsNewForced = false;
        if (m_whatsNewForceReq && !gameMode)
        {
            // 前回の版が無指定なら「今の版の 1 つ前のリリース」から（直前の版からの更新を再現する）。
            std::string from = m_whatsNewForceFrom;
            if (from.empty())
            {
                const auto& all = relnotes::All();
                for (const auto& r : all)
                    if (relnotes::CompareVersions(r.version, kEngineVersion) < 0) { from = r.version; break; }
            }
            m_whatsNewFrom = from;
            m_whatsNewForced = true;
            m_showWhatsNew = true;
        }
        else
        {
            m_showWhatsNew = !gameMode && !automation && (shown != std::string(kEngineVersion));
        }
    }
    Logger::Info("Application initializing... (mode: {})", gameMode ? "game" : "editor");

    // エディタコンテキスト初期化
    m_editorCtx = std::make_unique<EditorContext>();
    // 大きいシーンを分割しないまま保存したときの通知（シーンごとに 1 回。docs/SCENE_FORMAT_DESIGN.md §4.3）
    if (!gameMode)
        SceneSerializer::SetSplitAdviceHook([](void* ctx, const std::string& msg) {
            auto* app = static_cast<Application*>(ctx);
            if (app->m_editorCtx) app->m_editorCtx->Notify(ui::ToastKind::Info, msg, 10.0f);
        }, this);
    // エディタ（別ライブラリ）へ Application 側のフレームデータを読み取り専用で貸す。
    // どちらも Application の寿命いっぱい生きるメンバなのでアドレスは不変＝ここで一度だけ渡す。
    //   drawItems  … 精密ピッキングのブロードフェーズ候補（ワールド行列/球が計算済み）
    //   cpuScopeMs … picking/gizmo の計測を perf_stats の cpuScopeMs に載せるため
    m_editorCtx->drawItems  = &m_drawItems;
    m_editorCtx->cpuScopeMs = m_cpuMs;

    // ★仮想入力モード / --background は窓を作る【前】に立てる。窓の生成中に届く実入力メッセージも
    //   最初から遮断でき、起動の最初の 1 フレームからフォアグラウンドを取らない。
    // ★ゲーム(GameRuntime)も --background を受け付ける（UI 自動テストの build_game が配布ゲームを
    //   起動して落ちないか見る時に、人の画面へ窓を出さないため）。引数なしの配布物は従来どおり。
    if ((!gameMode || m_bgOptions.Active()) && m_virtualInputRequested)
    {
        vinput::SetEnabled(true);
        Logger::Info("仮想入力モード: ON（実マウス/実キーボードを ImGui に渡さず、OS のカーソルに触れない）");
    }
    if (m_bgOptions.Active())
    {
        SplashScreen::SetSuppressed(true);          // プロジェクト読込のスプラッシュも出さない
        Window::DisablePowerThrottling();             // 裏の窓でも EcoQoS / タイマ粗化で間引かれない
        Logger::Info("--background={}{}: 手前に出さない起動",
                     BackgroundModeName(m_bgOptions.mode), m_bgOptions.toolWindow ? ",tool" : "");
    }

    // ウィンドウ作成（タイトルにエンジンのバージョンを表記＝更新の確認にも使える）
    m_window = std::make_unique<Window>();
    if (!gameMode || m_bgOptions.Active()) m_window->SetBackground(m_bgOptions);
    std::wstring windowTitle = std::wstring(kEngineNameW) + L" v";
    for (const char* vp = kEngineVersion; *vp; ++vp)
        windowTitle += static_cast<wchar_t>(*vp);  // kEngineVersion は ASCII

    u32 winW = 1280, winH = 720;
    // ゲームモード: pak の __manifest__（ビルド設定で書き出した値）からタイトル/解像度を反映。
    // main.cpp で pak は app.Initialize より前にマウント済みなので、ここで読める。
    if (gameMode)
    {
        vfs::BootConfig bc;
        if (vfs::ReadBootConfig(bc))
        {
            if (bc.windowWidth  > 0) winW = static_cast<u32>(bc.windowWidth);
            if (bc.windowHeight > 0) winH = static_cast<u32>(bc.windowHeight);
            if (!bc.title.empty())
            {
                int n = MultiByteToWideChar(CP_UTF8, 0, bc.title.c_str(), -1, nullptr, 0);
                if (n > 0)
                {
                    std::wstring wt(static_cast<size_t>(n), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, bc.title.c_str(), -1, wt.data(), n);
                    wt.pop_back();
                    windowTitle = wt;   // 配布ゲームはエンジン名ではなく製品タイトルを表示
                }
            }
        }
    }
    // エディタ起動時はメインウィンドウの表示を初期化完了まで遅延する
    // （スプラッシュが進行状況を見せるので、白い未応答ウィンドウを出さない）。
    // ゲーム/ヘッドレスビルドはスプラッシュを出さないので従来どおり即表示。
    const bool deferMainWindow = !gameMode && !buildMode;
    SplashScreen::SetStage(splash::Stage::Window);
    // ゲーム(GameRuntime)は最大化せずビルド設定の解像度のまま表示する。最大化すると
    // クライアント領域が 16:9 より横長になり、UI の ScaleToFit が左右に余白を作るため。
    m_window->Initialize(hInstance, nCmdShow, winW, winH, windowTitle.c_str(),
                         deferMainWindow, /*startMaximized=*/!gameMode);
    // タイトルバーの X を横取り。ゲーム(GameRuntime)は従来通り即終了、エディタでプロジェクトを
    // 開いている時はいきなり終了せずランチャー（プロジェクト作成前の画面）に戻す。
    m_window->SetCloseHandler([this]{ return HandleWindowCloseRequest(); });
    // エディタはOS標準タイトルバーを外し、ImGuiのメニューバー行をタイトルバーとして使う
    // (Unreal/Unity風。ドラッグ/最小化/最大化/閉じるは ToolbarPanel が描く)。ゲームは標準のまま。
    if (!gameMode)
        m_window->EnableCustomTitleBar();

    // ゲームモード: 保存済みの映像設定（オプション画面の settings.json）をスワップチェイン
    // 生成前に適用する。エディタではエディタ自身の窓を勝手に変えないため適用しない
    // （Play 中に display.* を呼んだときだけライブで反映される）。
    // ★VSync だけは窓の形を変えないので、エディタでも読み書きする。
    //   ここが gameMode 限定だったせいで「エディタで VSync を切っても次の起動で戻る」
    //   （読まないので既定 false のまま、書かないので settings.json にも残らない）。
    //   video_vsync は Lua の display:setVSync と共有する 1 個の設定なので、
    //   エディタで切った状態はビルドしたゲームにも引き継がれる。
    m_useVsync       = PersistGet("video_vsync", m_useVsync ? 1.0 : 0.0) != 0.0;
    m_persistedVsync = m_useVsync;   // 起動直後に同じ値を書き戻さないための基準

    // 既定トランジション（「トランジション」窓のプリセット）。エディタ・ゲームとも同じキーを見る。
    // エディタはこの後 LoadProject でプロジェクトの settings.json から読み直す。
    LoadTransitionPrefs();

    if (gameMode)
    {
        m_fpsLimit = static_cast<f32>(PersistGet("video_fps", m_fpsLimit));
        m_instancingEnabled = PersistGet("render_instancing", 1.0) != 0.0;
        m_gpuInst.threshold = static_cast<u32>(std::max(0.0, PersistGet("instance_gpu_threshold", 1024.0)));   // GPU 駆動のインスタンス群（4-3）
        // クラスタードライティング（Forward+）。0 にすると「先頭 64 灯を総当たり」
        // フォールバックへ倒す（A/B 検証用。旧 8 灯経路そのものは残していない）。
        m_clusteredEnabled  = PersistGet("render_clustered", 1.0) != 0.0;
        m_forceDepthPrepass = PersistGet("render_depth_prepass", 0.0) != 0.0;
        m_occlusionCulling  = PersistGet("render_occlusion_culling", 0.0) != 0.0;
        // 影の解像度 / CSM の調整値（エディタで詰めた値を配布物でも使う）
        {
            static const u32 kSizes[4] = {1024, 2048, 4096, 8192};
            m_shadowQualityIndex = std::clamp(
                static_cast<int>(PersistGet("shadow_quality",
                                            static_cast<double>(m_shadowQualityIndex))), 0, 3);
            m_shadowMapSize      = kSizes[m_shadowQualityIndex];
            m_cascadeSplitLambda = std::clamp(
                static_cast<f32>(PersistGet("shadow_csm_lambda", m_cascadeSplitLambda)), 0.0f, 1.0f);
            m_cascadeBlendBand   = std::clamp(
                static_cast<f32>(PersistGet("shadow_csm_band", m_cascadeBlendBand)), 0.0f, 5.0f);
            m_shadowDepthBias    = std::clamp(
                static_cast<f32>(PersistGet("shadow_depth_bias", m_shadowDepthBias)), 0.0f, 0.05f);
        }
        // BC7/BC5 テクスチャ圧縮（0=無圧縮 / 1=高速 / 2=高品質）。既定 1。
        TextureLoader::SetCompressionMode(static_cast<int>(PersistGet("texture_compression", 1.0)));
        const int mode = static_cast<int>(PersistGet("video_mode", 0));
        const u32 w = static_cast<u32>(PersistGet("video_w", 0));
        const u32 h = static_cast<u32>(PersistGet("video_h", 0));
        if (m_bgOptions.Active())
        {
            // --background: 窓の形は変えない（全画面化・サイズ変更は人の画面に触れる）
        }
        else if (mode == static_cast<int>(WindowMode::Borderless))
            m_window->SetMode(WindowMode::Borderless);
        else if (mode == static_cast<int>(WindowMode::Fullscreen))
            m_window->SetMode(WindowMode::Fullscreen, w, h);
        else if (w > 0 && h > 0)
            m_window->SetClientSize(w, h);
    }

    // グラフィックスデバイス初期化
    SplashScreen::SetStage(splash::Stage::Graphics);
    m_graphicsDevice = std::make_unique<GraphicsDevice>();
    m_graphicsDevice->Initialize(*m_window);

    // コマンドキュー作成
    m_commandQueue = std::make_unique<CommandQueue>();
    m_commandQueue->Initialize(*m_graphicsDevice, D3D12_COMMAND_LIST_TYPE_DIRECT);

    // ディスクリプタヒープ作成（RTV用）
    m_descriptorHeap = std::make_unique<DescriptorHeap>();
    m_descriptorHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, false);

    // スワップチェイン初期化
    m_swapChain = std::make_unique<SwapChain>();
    m_swapChain->Initialize(*m_window, *m_graphicsDevice, *m_commandQueue, *m_descriptorHeap);

    // フレームリソース初期化
    m_frameResources = std::make_unique<FrameResources>();
    m_frameResources->Initialize(*m_graphicsDevice, *m_commandQueue);

    // GPU パス別タイムスタンプ（MCP perf_stats / benchmark 用。失敗しても no-op で害なし）
    m_gpuTimer = std::make_unique<GpuTimer>();
    m_gpuTimer->Initialize(m_graphicsDevice->GetDevice(), m_commandQueue->GetQueue());

    // ゲームクロックリセット
    m_gameClock.Reset();

    // Input System
    m_inputSystem = std::make_unique<InputSystem>();
    m_inputSystem->Initialize(m_window->GetHwnd());
    m_window->SetInputSystem(m_inputSystem.get());

    // Audio System
    SplashScreen::SetStage(splash::Stage::Audio);
    m_audioSystem = std::make_unique<AudioSystem>();
    m_audioSystem->Initialize(PathResolver::AssetsDir());

    // Physics System
    SplashScreen::SetStage(splash::Stage::Physics);
    m_physicsSystem = std::make_unique<PhysicsSystem>();
    m_physicsSystem->Initialize();
    // 接触イベント（engine.contact.enter/exit）を C++ EventBus へ配信させる。
    // m_eventBus は Application の安定メンバ。物理を Shutdown→Initialize で再構築しても
    // PhysicsSystem 側はこのポインタを保持し続ける（Shutdown では null 化しない）。
    m_physicsSystem->SetEventBus(&m_eventBus);

    // ゲーム AI（群衆 / Brain）。GPU 非依存。Play 開始・停止で中身だけ捨てる（ScriptEngine が Clear を呼ぶ）
    m_aiSystem = std::make_unique<ai::AiSystem>();
    if (m_editorCtx) m_editorCtx->aiSystem = m_aiSystem.get();   // 選択中の Brain のデバッグ表示

    // Network System（GPU非依存。Play/Stopで再構築しない＝m_eventBusはここで一度だけ注入）。
    // assets/network.json が無い(初回起動等)場合は既定値のまま続行する。
    m_networkSystem = std::make_unique<NetworkSystem>();
    m_networkSystem->SetEventBus(&m_eventBus);
    m_networkSystem->SetPhysicsSystem(m_physicsSystem.get());   // 予測リコンシリエーションのリプレイ用(フェーズ⑦b)
    {
        NetworkConfig cfg;
        cfg.Load(PathResolver::AssetsDir() + "network.json");
        m_networkSystem->SetConfig(cfg);
    }
    {
        NetworkSystem::Hooks hooks;
        hooks.currentScenePath = [this]() { return m_currentSceneRel; };
        hooks.requestSceneLoad = [this](const std::string& rel) { m_editorCtx->pendingGameLoadPath = rel; };
        m_networkSystem->SetHooks(std::move(hooks));
    }

    // Shader Visible SRV ヒープ。1024 → 4096 → 65536。
    // このヒープの容量が事実上「同時に扱えるアセット総数」の上限になっている:
    // テクスチャ1枚=1、マテリアル1個=3(連続)、メッシュ1個=2(DXR有効時)、
    // スケルタル1体=3。しかもテクスチャ/モデル/サムネイルの経路は解放しないので、
    // 4096 だと DXR 有効で約800メッシュ、スケルタル約1300体で枯渇していた。
    // 枯渇 = AllocateIndex が例外 → Render を貫通 → AbortFrame → 以後ずっと真っ暗。
    // D3D12 の shader-visible CBV/SRV/UAV ヒープ上限は Tier1 で 1,000,000 なので
    // 65536 でもまだ十分低い。記述子1個32B想定でも 2MB。
    // ★これは天井を上げただけで根治ではない。根治はエビクション(LRU)と bindless 化。
    m_srvHeap = std::make_unique<DescriptorHeap>();
    m_srvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true);

    // ResourceManager
    m_resourceManager = std::make_unique<ResourceManager>();
    // ResourceManager は暫定コマンドリストで初期化（デフォルトテクスチャ作成のため）
    // → モデルロード用の BeginFrame の後に初期化する

    // DSV ヒープ
    m_dsvHeap = std::make_unique<DescriptorHeap>();
    // [0] = メイン深度（レンダー解像度に追従して縮む）/ [1] = カメラプレビュー専用（固定 480x270）
    m_dsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4, false);

    // デプスバッファ作成
    {
        // R32_TYPELESS で確保し、DSV(D32_FLOAT) と SRV(R32_FLOAT) の両ビューを張る。
        // SRV はパーティクルの soft particles（接地フェード＋手動オクルージョン）で読む。
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = m_window->GetWidth();
        depthDesc.Height = m_window->GetHeight();
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.SampleDesc = {1, 0};
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue, IID_PPV_ARGS(&m_depthBuffer)));

        m_dsvHandle = m_dsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_graphicsDevice->GetDevice()->CreateDepthStencilView(
            m_depthBuffer.Get(), &dsvDesc, m_dsvHandle);

        m_depthSrvIndex = m_srvHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
        depthSrvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels     = 1;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_depthBuffer.Get(), &depthSrvDesc, m_srvHeap->GetCpuHandle(m_depthSrvIndex));
    }

    // RootSignature
    m_rootSignature = std::make_unique<RootSignature>();
    m_rootSignature->Initialize(*m_graphicsDevice);

    // プロジェクト独自シェーダー(上書き/自作)の実行時コンパイル基盤。エディタモードのみ。
    // 最初の ShaderCompiler::LoadFromFile(直後のブロック)より前に用意しておく必要がある
    // (ShaderCompiler::LoadFromFile が ShaderManager::Instance() のオーバーライドを先に見るため)。
    if (!m_isGameMode)
    {
        m_shaderManager = std::make_unique<ShaderManager>();
        ShaderManager::SetInstance(m_shaderManager.get());
        m_shaderManager->Initialize();
        if (!m_shaderManager->IsRuntimeCompileAvailable())
            Logger::Warn("シェーダーの実行時コンパイルが利用できません(ホットリロード無効、通常の.csoは読み込めます)");
    }

    // シェーダー読み込み & PipelineState
    SplashScreen::SetStage(splash::Stage::Shaders);
    RecreateForwardPsos();

    // Camera
    m_camera = std::make_unique<Camera>();
    {
        f32 viewW = static_cast<f32>(m_window->GetWidth());
        f32 viewH = static_cast<f32>(m_window->GetHeight());
        m_camera->SetPerspective(DirectX::XM_PIDIV4, viewW / viewH, 0.1f, 1000.0f);
    }
    m_camera->LookAt({-14.7f, 9.6f, -9.0f}, {0.0f, 0.0f, 0.0f});

    // シーン + モデル読み込み
    {
        // 暫定コマンドリストで GPU アップロード
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        // ResourceManager 初期化（デフォルト白テクスチャ作成にcmdListが必要）
        SplashScreen::SetStage(splash::Stage::Assets);
        m_resourceManager = std::make_unique<ResourceManager>();
        m_resourceManager->Initialize(m_graphicsDevice.get(), m_srvHeap.get(), cmdList);

        m_materialAssetManager = std::make_unique<MaterialAssetManager>();
        m_materialAssetManager->Initialize(m_resourceManager.get(), m_graphicsDevice.get(), m_srvHeap.get());

        // マテリアルグラフ（G2b）: .dxmat の graph キーを持つ材質のランタイム。メイン RS がバインドレス（G2a のフラグ）のときだけ有効。
        //   無効（非対応 GPU / DX12_DISABLE_MAIN_BINDLESS=1）でも Resolve が false を返すだけ＝従来の代理材質で描く（壊れない）。
        //   環境変数 DX12_DISABLE_GRAPH_MATERIALS=1 でランタイムごと作らない（グラフ材質は代理材質で描く。A/B 検証・切り分け用）。
        bool disableGraphMaterials = false;
        {
            char* dis = nullptr;
            size_t disLen = 0;
            if (_dupenv_s(&dis, &disLen, "DX12_DISABLE_GRAPH_MATERIALS") == 0 && dis)
            {
                disableGraphMaterials = dis[0] == '1';
                std::free(dis);
            }
        }
        if (!disableGraphMaterials)
        {
            m_graphMaterials = std::make_unique<GraphMaterialSystem>();
            GraphMaterialSystem::InitDesc gd;
            gd.device        = m_graphicsDevice.get();
            gd.rootSignature = m_rootSignature.get();
            gd.resources     = m_resourceManager.get();
            gd.srvHeap       = m_srvHeap.get();
            gd.colorFormat   = kSceneColorFormat;
            gd.depthFormat   = DXGI_FORMAT_D32_FLOAT;
            gd.allowCompile  = !m_isGameMode;      // ゲームモードは DXC を使わない（ディスクキャッシュ / pak だけ）
            gd.pollFiles     = !m_isGameMode;
            gd.stagedCompile = !m_isGameMode;      // G2c: 構造の編集は -Od の高速版を先に出し、最適化版は裏で差し替える（エディタのみ）
            gd.engineVersion = kEngineVersion;
            m_graphMaterials->Initialize(gd);
        }

        // 地形レイヤーセット（.terrainlayers → Texture2DArray ×2）。
        // SRV ディスクリプタは地形ごとに違う（t2 = スプラット）ので、ここでは確保しない。
        m_terrainLayerSets = std::make_unique<TerrainLayerSetManager>();
        m_terrainLayerSets->Initialize(m_graphicsDevice.get());

        // SSAO 無効/編集ビュー用の 1x1 白 R8_UNORM ダミー（forward の g_ssao が常に 1.0 を返す）。
        {
            u8 white = 0xFF;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 1; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8_UNORM;
            desc.SampleDesc = {1, 0};

            D3D12_SUBRESOURCE_DATA subData{};
            subData.pData = &white; subData.RowPitch = 1; subData.SlicePitch = 1;

            m_ssaoWhiteTex = std::make_unique<Texture>();
            m_ssaoWhiteTex->Initialize(*m_graphicsDevice, cmdList, desc, &subData, 1);
            m_ssaoWhiteSrvIndex = m_srvHeap->AllocateIndex();
            m_ssaoWhiteTex->SetSrvIndex(m_ssaoWhiteSrvIndex);
            m_ssaoWhiteTex->CreateSRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(m_ssaoWhiteSrvIndex));
        }

        // SSR/SSGI 無効時の 1x1 黒 RGBA16F ダミー（forward の g_ssr / g_ssgi 用）。
        // ★白ダミー(R8_UNORM)の流用は不可。Texture2D<float4> に R8_UNORM を貼ると
        //   デバッグレイヤがフォーマット不一致で警告する。1x1 なので Load(画面座標) は
        //   範囲外＝0 を返し、SSR の confidence 0 / SSGI 寄与 0 に自動的に落ちる。
        {
            const u16 black[4] = {0, 0, 0, 0};   // half の 0 はビットパターンも 0
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 1; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc = {1, 0};

            D3D12_SUBRESOURCE_DATA subData{};
            subData.pData = black; subData.RowPitch = 8; subData.SlicePitch = 8;

            m_ssBlackTex = std::make_unique<Texture>();
            m_ssBlackTex->Initialize(*m_graphicsDevice, cmdList, desc, &subData, 1);
            m_ssBlackSrvIndex = m_srvHeap->AllocateIndex();
            m_ssBlackTex->SetSrvIndex(m_ssBlackSrvIndex);
            m_ssBlackTex->CreateSRV(*m_graphicsDevice, m_srvHeap->GetCpuHandle(m_ssBlackSrvIndex));
        }

        // エディタUIアイコンを読み込み（エンジン側assets基準。プロジェクト切替前に1度）
        if (!m_isGameMode)
            LoadEditorIcons(cmdList);

        // Scene 初期化
        m_scene = std::make_unique<Scene>();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        // ScriptEngine 初期化 + ゲームスクリプト実行
        SplashScreen::SetStage(splash::Stage::Scripts);
        m_scriptEngine = std::make_unique<ScriptEngine>();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), PathResolver::AssetsDir());
        WireScriptCallbacks();
        InitSequencer();   // シーケンサー(.dxseq)のホスト。保存フック・Lua の Sequence API もここで結ぶ

        // ゲームスクリプト読み込み（グローバル game.lua）
        // 配布ゲームは起動＝ゲームプレイ開始なので OnStart を呼ぶ。
        // エディタは「プロジェクトを開いただけ」なので呼ばない（Play を押したときに呼ばれる）。
        LoadGameScript(/*callOnStart=*/m_isGameMode);

        // ★開始シーンを読む前に遅延解放を有効にする。以前は Initialize の末尾で有効にしていたため、
        //   シーン読込中（地形の頂点バッファの作り直しなど、同じコマンドリストが GPU 参照を持ったまま
        //   旧バッファを差し替える処理）の解放が「即時」になり、**Terrain を含むシーンで配布ゲームが
        //   起動直後に DEVICE_HUNG で落ちた**（「Close: リソースが解放済み」。エディタは読込の合間に
        //   フラッシュされるので出なかった）。有効中は Stamp/Collect がフェンス完了まで解放を遅らせる。
        DeferredRelease::Enable();

        // 初期シーン: (配布) game.json の startScene → (エディタ) 最後に開いたシーン → default.json → クリーン状態
        {
            bool loaded = false;

            // 配布モード: pak __manifest__ または game.json で開始シーンを指定（最優先）
            if (m_isGameMode)
            {
                if (vfs::InGameMode())
                {
                    // ゲームモード: pak 内 __manifest__ からブート設定を読む（game.json 不要）
                    vfs::BootConfig bc;
                    if (vfs::ReadBootConfig(bc) && !bc.startScene.empty())
                    {
                        std::string startScene = PathResolver::AssetsDir() + bc.startScene;
                        loaded = SceneSerializer::Load(*m_scene, startScene, PathResolver::AssetsDir());
                        if (loaded)
                        {
                            m_editorCtx->currentScenePath = startScene;
                            m_currentSceneRel = bc.startScene;
                            Logger::Info("Loaded start scene from manifest: {}", bc.startScene);
                        }
                    }
                }
                else
                {
                    // ディスクモード（--game フラグ + game.json 配置の旧形式）
                    ProjectInfo gi;
                    if (Project::Load(PathResolver::BaseDir() + "game.json", gi) && !gi.defaultScene.empty())
                    {
                        std::string startScene = PathResolver::AssetsDir() + gi.defaultScene;
                        if (std::filesystem::exists(startScene))
                        {
                            loaded = SceneSerializer::Load(*m_scene, startScene, PathResolver::AssetsDir());
                            if (loaded)
                            {
                                m_editorCtx->currentScenePath = startScene;
                                m_currentSceneRel = gi.defaultScene;
                                Logger::Info("Loaded start scene from game.json: {}", gi.defaultScene);
                            }
                        }
                    }
                }
            }

            std::string lastScene = ProjectManager::LoadLastOpenedScene();
            std::string defaultScene = PathResolver::AssetsDir() + "scenes/default.json";

            // 通常起動では前回プロジェクトを復元しない(ランチャーを表示する)。
            // lastOpenedScene の直読みは --net-client でプロジェクト未指定のとき専用
            // (「最後のシーンのまま参加」の挙動を維持するため)。
            if (!loaded && !m_isGameMode &&
                !m_pendingNetClientJoin.empty() && m_pendingNetClientProject.empty() &&
                !lastScene.empty() && std::filesystem::exists(lastScene))
            {
                loaded = SceneSerializer::Load(*m_scene, lastScene, PathResolver::AssetsDir());
                if (loaded)
                {
                    m_editorCtx->currentScenePath = lastScene;
                    // マルチプレイの Welcome はシーンを assets 相対で送る(クライアントは自分の
                    // assets 配下から読む)ため、絶対パスから "assets/" 以降を相対として控える。
                    std::string norm = lastScene;
                    std::replace(norm.begin(), norm.end(), '\\', '/');
                    if (size_t p = norm.rfind("/assets/"); p != std::string::npos)
                        m_currentSceneRel = norm.substr(p + 8);
                }
            }
            if (!loaded && std::filesystem::exists(defaultScene))
            {
                loaded = SceneSerializer::Load(*m_scene, defaultScene, PathResolver::AssetsDir());
                if (loaded)
                    m_editorCtx->currentScenePath = defaultScene;
                else if (!m_isGameMode)
                    OnSceneLoadFailed(defaultScene);   // 壊れた default.json を黙って空にしない（以前の版からの復元を案内し、保存を止める）
            }
            if (!loaded)
            {
                // クリーン初期状態: Grid + DirectionalLight + MainCamera（再生に必要な最低限）
                m_scene->SpawnPlane("Grid", {0, 0, 0}, kEditorGridSize, true);
                auto& reg = m_scene->GetRegistry();
                auto lightE = reg.create();
                reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
                reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
                reg.emplace<DirectionalLight>(lightE);

                auto camE = reg.create();
                reg.emplace<NameTag>(camE, NameTag{"MainCamera"});
                reg.emplace<Transform>(camE, Transform{{0.0f, 6.0f, -12.0f}, {22.0f, 0.0f, 0.0f}, {1,1,1}});
                CameraComponent cam;
                cam.isActive = true;
                reg.emplace<CameraComponent>(camE, cam);
            }

            // ロードしたシーン(最後に開いた/ default.json 等)に Grid が無ければ補う。
            // 旧シーンや Grid 未配置データを開いてもエディタにグリッドが必ず出る。
            EnsureEditorGrid();

            // シーンフロー / loadScene 用に現在シーンの相対パスを記録
            if (m_currentSceneRel.empty() && !m_editorCtx->currentScenePath.empty())
                m_currentSceneRel = ToAssetRel(m_editorCtx->currentScenePath);
        }

        // ホットリロード用タイムスタンプ初期化（初回の誤発火を防止）
        {
            std::string scriptPath = PathResolver::GameLuaPath();
            if (std::filesystem::exists(scriptPath))
                m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);
        }

        // エディタモード初期化時はキャプチャ解除（Luaが OnStart で capture する場合があるため）
        if (!m_isGameMode)
            m_inputSystem->SetMouseCapture(false);

        // コマンド実行 + GPU待ち
        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();

        // アップロードバッファ解放
        m_resourceManager->FinishUploads();
        if (m_ssaoWhiteTex) m_ssaoWhiteTex->FinishUpload();
        if (m_ssBlackTex)   m_ssBlackTex->FinishUpload();

        // スキニング PSO 作成
        RecreateSkinnedPsos();

        // グリッド PSO 作成（アルファブレンド + 両面描画）
        RecreateGridPso();

        // 地形マテリアル PSO（4 レイヤースプラット。layerSetPath が空でない Terrain だけが使う）
        RecreateTerrainPsos();

        // 加算発光 PSO（パーティクル用）：ライティング無視・加算合成・深度書き込みOFF
        {
            RecreateEmissivePso();

            // per-instance バッファ（kFrameCount でリング化＝インフライト安全）。永続Map。
            for (u32 fi = 0; fi < FrameResources::kFrameCount; ++fi)
            {
                const UINT bytes = kMaxInstances * sizeof(MeshInstanceData);
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&m_instanceBuffer[fi])));
                void* mapped = nullptr; D3D12_RANGE rr{0, 0};
                ThrowIfFailed(m_instanceBuffer[fi]->Map(0, &rr, &mapped));
                m_instanceMapped[fi] = static_cast<uint8_t*>(mapped);
                m_instanceVbView[fi].BufferLocation = m_instanceBuffer[fi]->GetGPUVirtualAddress();
                m_instanceVbView[fi].StrideInBytes  = sizeof(MeshInstanceData);
                m_instanceVbView[fi].SizeInBytes    = bytes;
            }
        }

        // sneakWalk アニメーションを全スケルタルEntityに追加
        {
            // ★std::filesystem::exists は pak モード（配布ゲーム）で常に false になるため、
            //   ここだけエディタとゲームで挙動が食い違っていた（他所は vfs へ直してある）。
            const std::string sneakRel = "models/human/sneakWalk.gltf";
            std::filesystem::path sneakPath = PathResolver::AssetsDir() + sneakRel;
            if (vfs::Exists(sneakRel))
            {
                auto& reg = m_scene->GetRegistry();
                auto skelView = reg.view<SkeletalAnimation>();
                for (auto [e, skelAnim] : skelView.each())
                {
                    auto extraAnims = ModelLoader::LoadAnimationsFromFile(
                        sneakPath, *skelAnim.skeleton);
                    for (auto& a : extraAnims)
                    {
                        a->SetName("sneakWalk");
                        skelAnim.clips.push_back(std::move(a));
                    }
                }
            }
        }
    }

    // シャドウマップ作成（CSM: Texture2DArray, ArraySize=kNumCascades）
    SplashScreen::SetStage(splash::Stage::ShadowMap);
    {
        m_shadowDsvHeap = std::make_unique<DescriptorHeap>();
        m_shadowDsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kNumCascades, false);

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = m_shadowMapSize;
        shadowDesc.Height = m_shadowMapSize;
        shadowDesc.DepthOrArraySize = static_cast<u16>(kNumCascades);
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc = {1, 0};
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &shadowDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&m_shadowMap)));

        // DSV: 配列スライス毎に kNumCascades 個
        for (u32 i = 0; i < kNumCascades; ++i)
        {
            m_shadowDsvHandles[i] = m_shadowDsvHeap->Allocate();
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.FirstArraySlice = i;
            dsvDesc.Texture2DArray.ArraySize = 1;
            dsvDesc.Texture2DArray.MipSlice = 0;
            m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                m_shadowMap.Get(), &dsvDesc, m_shadowDsvHandles[i]);
        }

        // SRV: 配列SRV(1個)
        m_shadowSrvIndex = m_srvHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = kNumCascades;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_shadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_shadowSrvIndex));

        // Shadow PSO (depth-only, no pixel shader, with depth bias) + Skinned版
        RecreateShadowPsos();

        // 深度プリパス PSO（SSAO 用カメラ深度）+ Skinned版
        RecreateDepthPrepassPsos();

        // 深度+速度プリパス PSO（TAA 用モーションベクター）。static / instanced / skinned の3本。
        RecreateVelocityPsos();

        Logger::Info("Shadow map initialized ({}x{})", m_shadowMapSize, m_shadowMapSize);
    }

    // スポット/ポイントライトの影マップ作成（CSMと同レシピ: R32_TYPELESS 配列 + スライス毎DSV + 1個のSRV）。
    // PSO/サンプラーはCSM用を流用（ShadowPass_VS は b0.mvp で変換するだけ＝面/灯ごとの VP を渡せば足りる）。
    {
        const u32 kNumPointFaces = kMaxShadowPoint * 6;
        m_punctualShadowDsvHeap = std::make_unique<DescriptorHeap>();
        m_punctualShadowDsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                            kMaxShadowSpot + kNumPointFaces, false);

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        // --- スポット: Texture2DArray(ArraySize=kMaxShadowSpot) ---
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = kSpotShadowMapSize;
            desc.Height = kSpotShadowMapSize;
            desc.DepthOrArraySize = static_cast<u16>(kMaxShadowSpot);
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.SampleDesc = {1, 0};
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clearValue, IID_PPV_ARGS(&m_spotShadowMap)));

            for (u32 i = 0; i < kMaxShadowSpot; ++i)
            {
                m_spotShadowDsvHandles[i] = m_punctualShadowDsvHeap->Allocate();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.FirstArraySlice = i;
                dsvDesc.Texture2DArray.ArraySize = 1;
                m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                    m_spotShadowMap.Get(), &dsvDesc, m_spotShadowDsvHandles[i]);
            }

            m_spotShadowSrvIndex = m_srvHeap->AllocateIndex();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.ArraySize = kMaxShadowSpot;
            m_graphicsDevice->GetDevice()->CreateShaderResourceView(
                m_spotShadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_spotShadowSrvIndex));
        }

        // --- ポイント: Texture2DArray(ArraySize=kMaxShadowPoint*6)。SRVはTextureCubeArrayとして参照 ---
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = kPointShadowMapSize;
            desc.Height = kPointShadowMapSize;
            desc.DepthOrArraySize = static_cast<u16>(kNumPointFaces);
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.SampleDesc = {1, 0};
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clearValue, IID_PPV_ARGS(&m_pointShadowMap)));

            for (u32 i = 0; i < kNumPointFaces; ++i)
            {
                m_pointShadowDsvHandles[i] = m_punctualShadowDsvHeap->Allocate();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.FirstArraySlice = i;
                dsvDesc.Texture2DArray.ArraySize = 1;
                m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                    m_pointShadowMap.Get(), &dsvDesc, m_pointShadowDsvHandles[i]);
            }

            // t9(スポット)の直後の連番であることをシェーダ側テーブル(t9,t10連続レンジ)が前提にしている。
            m_pointShadowSrvIndex = m_srvHeap->AllocateIndex();
            DX_ASSERT(m_pointShadowSrvIndex == m_spotShadowSrvIndex + 1,
                     "スポット/ポイント影SRVが連番でない（RootSigのt9-t10テーブル前提が崩れる）");
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.TextureCubeArray.MipLevels = 1;
            srvDesc.TextureCubeArray.First2DArrayFace = 0;
            srvDesc.TextureCubeArray.NumCubes = kMaxShadowPoint;
            m_graphicsDevice->GetDevice()->CreateShaderResourceView(
                m_pointShadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_pointShadowSrvIndex));
        }

        Logger::Info("Punctual shadow maps initialized (spot {}x{}x{}, point {}x{}x{})",
                    kSpotShadowMapSize, kSpotShadowMapSize, kMaxShadowSpot,
                    kPointShadowMapSize, kPointShadowMapSize, kMaxShadowPoint);
    }

    // PerFrame Constant Buffer（ライトはクラスタードライティングの StructuredBuffer 側へ移動済み）
    // レイアウトは shaders/forward/Lighting.hlsli の PerFrameConstants と完全一致させること。
    struct FrameConstants {
        DirectX::XMFLOAT4X4 view;            // 64B  (offset   0)
        DirectX::XMFLOAT4X4 proj;            // 64B  (offset  64)
        DirectX::XMFLOAT3   lightDir;        // 12B
        float                time;            // 4B  → 16B (offset 128)
        DirectX::XMFLOAT3   lightColor;      // 12B
        float                ambientStrength; // 4B  → 16B (offset 144)
        DirectX::XMFLOAT4X4 cascadeViewProj[kNumCascades]; // 256B (offset 160)
        DirectX::XMFLOAT4   cascadeSplitsView; // 16B (offset 416)
        DirectX::XMFLOAT4   shadowParams;      // 16B (offset 432)
        DirectX::XMFLOAT3   cameraPos;       // 12B
        float                _pad;            // 4B  → 16B (offset 448)
        u32                  numPointLights;  // 4B  ← 統計/デバッグ用（シェーダは読まない）
        u32                  numSpotLights;   // 4B
        float                spotShadowTexel; // 4B
        float                pointShadowNear; // 4B  → 16B (offset 464)
        // ▼ クラスタードライティング 64B (offset 480)。旧 pointLights[8]/spotLights[8] の跡地。
        DirectX::XMFLOAT4    clusterParams;   // (offset 480)
        DirectX::XMFLOAT4    clusterGrid;     // (offset 496)
        DirectX::XMFLOAT4    clusterViewport; // (offset 512)
        DirectX::XMFLOAT4    clusterExtra;    // (offset 528)
        DirectX::XMFLOAT4    pcssParams;                       // 16B  (offset 544) PCSS（計画03）
        // ▼ DDGI 48B (offset 560)。ddgiOrigin.w=0 なら PS は t22 を一切読まない
        DirectX::XMFLOAT4    ddgiOrigin;                       // 16B  (offset 560) .xyz=原点 .w=強さ
        DirectX::XMFLOAT4    ddgiSpacing;                      // 16B  (offset 576) .xyz=間隔 .w=法線バイアス
        DirectX::XMFLOAT4    ddgiCounts;                       // 16B  (offset 592) .xyz=プローブ数
        DirectX::XMFLOAT4    _clusterReserved[35];             // 560B (offset 608..1167)
        DirectX::XMFLOAT4    ddgiC1;                           // 16B  (offset 1168) GI S4: カスケード 1（.xyz=原点 .w=間隔）
        DirectX::XMFLOAT4    ddgiScroll0;                      // 16B  (offset 1184) カスケード 0 の記憶領域のずらし
        DirectX::XMFLOAT4    ddgiScroll1;                      // 16B  (offset 1200)
        DirectX::XMFLOAT4    giParams;                         // 16B  (offset 1216) GI モード（GI_FOUNDATION_DESIGN）
        DirectX::XMFLOAT4    giParams2;                        // 16B  (offset 1232)
        DirectX::XMFLOAT4X4  spotShadowMatrix[kMaxShadowSpot]; // 256B (offset 1248)
        // ▼ IBL 制御 16B (offset 1504)
        float                iblIntensity;
        float                maxPrefilterMip;
        u32                  hasIBL;
        float                skyboxIntensity;
        // ▼ コンタクトシャドウ制御 16B (offset 1520)
        float                contactShadowEnabled;
        DirectX::XMFLOAT3    _csPad;
    };  // total = 1536B
    // ここは CB の確保サイズを決めるためだけの定義。Render() 内の同名構造体・
    // ModelThumbnailRenderer・Lighting.hlsli の PerFrameConstants と必ず揃えること。
    static_assert(sizeof(FrameConstants) == 1536, "FrameConstants must be 1536 bytes");
    m_perFrameCB = std::make_unique<ConstantBuffer>();
    m_perFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // カメラプレビュー用の per-frame CB（メインパスと別バッファ。同一フレーム内で
    // 別視点を描くため m_perFrameCB を上書きできない）
    m_previewFrameCB = std::make_unique<ConstantBuffer>();
    m_previewFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // CommandList ラッパー
    m_commandList = std::make_unique<CommandList>();

    // ImGui 初期化
    SplashScreen::SetStage(splash::Stage::EditorUi);
    m_imguiManager = std::make_unique<ImGuiManager>();
    m_imguiManager->Initialize(
        m_window->GetHwnd(), *m_graphicsDevice, m_commandQueue->GetQueue(),
        *m_srvHeap, m_swapChain->GetFormat(), FrameResources::kFrameCount);
    // 仮想キー（ImGui へ流したキー）は VK ベースの入力（F1 一時停止などの InputSystem 経由）へも届ける。
    m_imguiManager->SetVirtualKeySink([this](int vk, bool down)
    {
        if (down && m_inputSystem) m_inputSystem->InjectKeyPress(vk);
    });
    if (!gameMode)   // フェーズ 1b: imgui.ini の場所をユーザーデータ領域へ（カレントディレクトリ依存をやめる。DX12E_DATA_DIR で分離される）
        m_imguiManager->SetIniPath(ProjectManager::DataDir() + "/imgui.ini");
    if (m_bgOptions.Active() && !gameMode)
        m_imguiManager->SetIniSavingDisabled(true);   // 画面外/最小化の位置を普段のレイアウトへ焼き付けない
    if (vinput::Enabled())
        m_imguiManager->SetVirtualInput(true);

    // UI 自動テストエンジン。ImGui コンテキスト生成直後・初回 NewFrame より前に開始する。
    // エディタでは常時初期化して「ツール > エンジン診断」からいつでも検査を回せるようにする
    // (テストを走らせない限り実行時コストはほぼゼロ)。ゲームモードでは載せない。
    if (!m_isGameMode)
    {
        m_uiTests = std::make_unique<UiTestHarness>();
        m_uiTests->Initialize(this, m_uiTestsRunAll, m_uiTestsSpeed, m_uiTestsDeepOnly);
    }

    // EditorLayer 初期化
    m_editorLayer = std::make_unique<EditorLayer>();
    m_editorLayer->Initialize(m_editorCtx.get(), PathResolver::AssetsDir(),
                              PathResolver::ScriptsDir(),
                              m_resourceManager.get(), m_srvHeap.get());

    // ModelThumbnailRenderer 初期化
    m_thumbRenderer = std::make_unique<ModelThumbnailRenderer>();
    m_thumbRenderer->Initialize(m_graphicsDevice.get(), m_srvHeap.get(),
                                m_resourceManager.get(), m_rootSignature.get(),
                                m_pipelineStateThumb.get());
    m_thumbRenderer->SetAOWhiteSrv(m_ssaoWhiteSrvIndex);  // forward PS の t8 を白ダミーで満たす
    m_thumbRenderer->SetScreenSpaceBlackSrv(m_ssBlackSrvIndex);  // t16/t17 を黒ダミーで満たす
    m_editorLayer->SetThumbnailRenderer(m_thumbRenderer.get());

    // Physics Debug Renderer
    m_physicsDebugRenderer = std::make_unique<PhysicsDebugRenderer>();
    m_physicsDebugRenderer->Initialize(*m_graphicsDevice,
        kSceneColorFormat, DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

    m_editorIconRenderer = std::make_unique<EditorIconRenderer>();
    m_editorIconRenderer->Initialize(*m_graphicsDevice,
        m_swapChain->GetFormat(), DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

    // オフスクリーン描画用 RT + ポストプロセス（WP3）
    SplashScreen::SetStage(splash::Stage::Renderer);
    {
        // sceneRT(1)+cameraPreview(2)+ブルームチェーン(6)+ゴッドレイ/レンズフレア/DoF/
        // モーションブラー+SSAO(2)+コンタクトシャドウ(1)+歪みRT で 20 個ほど使う。
        // 容量 64: 今後の深度依存パス（モーションベクター/SSGI/SSR/ボリュメトリック等）を
        // 足しても枯渇しない余裕を先に取っておく（RTV は非シェーダ可視で安価。
        // 枯渇時は DescriptorHeap::Allocate が Logger::Error + throw で fail-fast する）。
        m_offscreenRtvHeap = std::make_unique<DescriptorHeap>();
        m_offscreenRtvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64, false);

        // シーンは HDR(kSceneColorFormat) の中間RTへ描き、ポストで backbuffer へ解決する。
        // クリア色 = skybox を描かないシーンの背景色。★ClearRenderTarget 側と必ず同じ値に
        // すること（最適化クリア値が食い違うと D3D12 が遅いパスへ落ちる）。
        const float sceneClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_sceneRT = std::make_unique<RenderTarget>();
        m_sceneRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                              m_window->GetWidth(), m_window->GetHeight(),
                              kSceneColorFormat, sceneClear);

        // カメラプレビュー RT（固定 16:9・小サイズ。選択カメラ視点をここへ描いて小窓表示）
        m_cameraPreviewRT = std::make_unique<RenderTarget>();
        m_cameraPreviewRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                      480, 270, kSceneColorFormat, sceneClear);

        // プレビュー専用の深度（固定 480x270）。#16 でメイン深度がレンダー解像度に追従して
        // 縮むようになったため、480x270 のプレビューには足りなくなり得る（RTV > DSV は違反）。
        {
            D3D12_RESOURCE_DESC pd{};
            pd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            pd.Width            = 480;
            pd.Height           = 270;
            pd.DepthOrArraySize = 1;
            pd.MipLevels        = 1;
            pd.Format           = DXGI_FORMAT_D32_FLOAT;
            pd.SampleDesc       = {1, 0};
            pd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE pcv{};
            pcv.Format = DXGI_FORMAT_D32_FLOAT;
            pcv.DepthStencil = {1.0f, 0};

            D3D12_HEAP_PROPERTIES ph{};
            ph.Type = D3D12_HEAP_TYPE_DEFAULT;

            ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                &ph, D3D12_HEAP_FLAG_NONE, &pd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &pcv, IID_PPV_ARGS(&m_previewDepthBuffer)));

            m_previewDsvHandle = m_dsvHeap->Allocate();
            D3D12_DEPTH_STENCIL_VIEW_DESC pdv{};
            pdv.Format        = DXGI_FORMAT_D32_FLOAT;
            pdv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                m_previewDepthBuffer.Get(), &pdv, m_previewDsvHandle);
        }

        // プレビュー表示用 LDR RT。プレビューRT(リニアHDR)をトーンマップして解決し、
        // ImGui にはこちらの SRV を渡す（FP16 を直接表示すると暗く見えるため）
        m_cameraPreviewLdrRT = std::make_unique<RenderTarget>();
        m_cameraPreviewLdrRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                         480, 270, DXGI_FORMAT_R8G8B8A8_UNORM, sceneClear);

        m_postProcess = std::make_unique<PostProcess>();
        m_postProcess->Initialize(*m_graphicsDevice, m_swapChain->GetFormat(), PathResolver::ShaderDirW(),
                                  FrameResources::kFrameCount);

        // 物理ベースブルーム（シーンHDR → 半解像度 6 段のダウン/アップサンプルチェーン）
        m_bloomPass = std::make_unique<BloomPass>();
        m_bloomPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());

        // 自動露出（compute ヒストグラム。露出値は GPU 内バッファで uber パスへ直結）
        m_autoExposure = std::make_unique<AutoExposurePass>();
        m_autoExposure->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());

        // ゴッドレイ / レンズフレア / DoF / モーションブラー（全て設定でOFF時はゼロコスト）
        m_godRaysPass = std::make_unique<GodRaysPass>();
        m_godRaysPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                  m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());
        m_lensFlarePass = std::make_unique<LensFlarePass>();
        m_lensFlarePass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                    m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());
        m_dofPass = std::make_unique<DofPass>();
        m_dofPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                              m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());
        m_motionBlurPass = std::make_unique<MotionBlurPass>();
        m_motionBlurPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                     m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());

        // SSAO（深度プリパス → 半球カーネル AO → ブラー）。AO/Blur RT は offscreenRtvHeap から確保。
        m_ssaoPass = std::make_unique<SSAOPass>();
        m_ssaoPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                               m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());

        // Hi-Z オクルージョンカリング（深度プリパスの深度 → 階層 Z ピラミッド）。
        // レンダー解像度ぶんの R32_FLOAT ミップ連鎖なので RTV ヒープは要らない。
        m_hiZPass = std::make_unique<HiZPass>();
        m_hiZPass->Initialize(*m_graphicsDevice, m_srvHeap.get(),
                              m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());
        m_occlusionCull = std::make_unique<OcclusionCullPass>();
        m_occlusionCull->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());

        // コンタクトシャドウ（SSAO と同じ深度プリパスの結果を使う 1 パス。RT も同じヒープから）。
        m_contactShadowPass = std::make_unique<ContactShadowPass>();
        m_contactShadowPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                        m_window->GetWidth(), m_window->GetHeight(), PathResolver::ShaderDirW());

        // TAA（速度RT 1 + 履歴RT 2 = RTV 3 枚。履歴はシーンと同じ HDR フォーマット、
        // デバッグ可視化だけバックバッファ形式へ直接描く）。
        m_taaPass = std::make_unique<TaaPass>();
        m_taaPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                              m_window->GetWidth(), m_window->GetHeight(),
                              kSceneColorFormat, m_swapChain->GetFormat(), PathResolver::ShaderDirW());

        // G-Buffer（速度プリパスの RTV1）。速度 PSO は常に MRT=2 なので、速度プリパスが
        // 走るときは必ずここへも書かれる＝常に確保しておく（分岐を作らない）。
        const float gbufClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_gbufferRT = std::make_unique<RenderTarget>();
        m_gbufferRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                m_window->GetWidth(), m_window->GetHeight(),
                                kGBufferFormat, gbufClear);

        // 中間バッファ可視化（dx12_render_debug）。RTV / SRV / ディスクリプタを 1 枚も消費しない
        // （既存バッファを読んでシーン RT へ描くだけ）。既定 OFF ＝ Draw が 1 回も呼ばれない。
        m_renderDebugPass = std::make_unique<RenderDebugPass>();
        m_renderDebugPass->Initialize(*m_graphicsDevice, kSceneColorFormat, PathResolver::ShaderDirW());

        // SSR / SSGI（前フレームカラー退避 + ハーフトレース + 時間蓄積 + アップサンプル）。
        // RTV は 8 枚使う（フル 3 + ハーフ 5）。既定 OFF なのでパスは走らない。
        m_screenSpaceGi = std::make_unique<ScreenSpaceGiPass>();
        m_screenSpaceGi->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                    m_window->GetWidth(), m_window->GetHeight(),
                                    PathResolver::ShaderDirW());

        // DXR レイトレーシング（計画09 Step 1〜3）。6 段ゲートを全部通ったときだけ作る。
        //   Gate 1/2: Tier >= 1.1 かつ SM >= 6.5（GraphicsDevice::SupportsInlineRaytracing）
        //   Gate 3/6: Device5 と加速構造バッファの確保（RaytracingScene::Initialize）
        //   Gate 5  : .cso の読み込み（RtScreenPass::Initialize → ShaderCompiler が throw）
        //   Gate 4  : ID3D12GraphicsCommandList4（初回 Build 時）
        // どれかが落ちたら m_dxrEnabled=false のままで、以後 DXR のパスは 1 つも走らない。
        // 既存の白 1x1 ダミーがそのまま t8/t11 に貼られる＝フォワード PS は 1 行も変わらない。
        if (m_graphicsDevice->SupportsInlineRaytracing())
        {
            m_rtScene = std::make_unique<RaytracingScene>();
            if (m_rtScene->Initialize(*m_graphicsDevice))
            {
                try
                {
                    m_rtScreenPass = std::make_unique<RtScreenPass>();
                    m_rtScreenPass->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                               m_window->GetWidth(), m_window->GetHeight(),
                                               PathResolver::ShaderDirW());
                    m_dxrEnabled = m_rtScreenPass->IsReady();
                }
                catch (const std::exception& e)
                {
                    // .cso が無い / 壊れている（Gate 5）。エラーではなく機能の不在として扱う。
                    Logger::Warn("レイトレーシングのシェーダを読めませんでした: {}", e.what());
                    m_rtScreenPass.reset();
                    m_dxrEnabled = false;
                }
            }
            if (!m_dxrEnabled)
            {
                m_rtScene.reset();
                m_rtScreenPass.reset();
            }
            else
            {
                // compute スキニング（計画09 Step 4）。DXR が生きているときだけ意味があるので
                // ここで作る。失敗しても DXR 自体は静的メッシュで動き続ける（スキンドが
                // TLAS に入らなくなるだけ＝Step 3 までと同じ挙動）。
                try
                {
                    m_skinningCompute = std::make_unique<SkinningCompute>();
                    m_skinningCompute->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());
                    if (!m_skinningCompute->IsReady())
                        m_skinningCompute.reset();
                }
                catch (const std::exception& e)
                {
                    Logger::Warn("compute スキニングを初期化できませんでした: {}", e.what());
                    m_skinningCompute.reset();
                }

                // DDGI（計画09 Step 6）。バインドレスのヒット読み取りを使うので
                // Dynamic Resources が要る。作れなくても DXR 自体は動き続ける。
                try
                {
                    m_ddgi = std::make_unique<DdgiVolume>();
                    if (!m_ddgi->Initialize(*m_graphicsDevice, m_srvHeap.get(),
                                            PathResolver::ShaderDirW()))
                        m_ddgi.reset();
                }
                catch (const std::exception& e)
                {
                    Logger::Warn("DDGI を初期化できませんでした: {}", e.what());
                    m_ddgi.reset();
                }
            }
        }

        // GI モード「新」が使えるか（S5）。新規シーンの既定は「新」だが、使えない環境では従来（legacy）で作る。
        // 判定はここで 1 回。新規シーン作成（ApplicationRender）も同じ条件で見る。
        if (!m_dxrEnabled)
            Logger::Info("GI: この GPU はレイトレーシング（DXR 1.1 / SM 6.5）を使えないため、新規シーンは従来の GI（旧）で作ります");
        else if (!m_ddgi || !m_ddgi->SupportsGiNew())
            Logger::Info("GI: DDGI（新モード）を初期化できなかったため、新規シーンは従来の GI（旧）で作ります");
        else
            Logger::Info("GI: 新規シーンは GI モード「新」（DDGI 自動配置・環境光 0・SSGI・RT 影）で作ります");

        // クラスタードライティング（Forward+）。ライトカリング compute 2 パス + SRV テーブル。
        // 旧「点光源 8 / スポット 8」の cbuffer 固定配列を置き換える本体。
        m_clusteredLighting = std::make_unique<ClusteredLightCulling>();
        m_clusteredLighting->Initialize(*m_graphicsDevice, m_srvHeap.get(), PathResolver::ShaderDirW());
        // サムネイルレンダラもメインの RootSig / Forward PSO を流用するのでテーブルが要る
        // （灯数 0 なので中身は読まれない。frameIndex 0 のブロックで十分）。
        if (m_thumbRenderer)
            m_thumbRenderer->SetClusterSrv(m_clusteredLighting->GetSrvTableIndex(0));

        // ボリュメトリックフォグ（froxel）。3D テクスチャ 28MB は「初めて有効になったフレーム」まで
        // 確保しない（既定 OFF）。ディスクリプタブロックだけは断片化前のここで押さえておく。
        m_volumetricFogPass = std::make_unique<VolumetricFogPass>();
        m_volumetricFogPass->Initialize(*m_graphicsDevice, m_srvHeap.get(), PathResolver::ShaderDirW());

        // デカール（クラスタードフォワードデカール）。SRV は ClusteredLightCulling の
        // テーブル（7 本）の +3..+6 を借りるので、自前のディスクリプタは 1 本も取らない。
        m_decalSystem = std::make_unique<DecalSystem>();
        m_decalSystem->Initialize(*m_graphicsDevice, m_srvHeap.get(), PathResolver::ShaderDirW());
        // ★ここで即座に t18..t21 を「正しい型の」ディスクリプタで埋める。
        //   ClusteredLightCulling が置いた仮埋め（カウントバッファの SRV の複製）のままだと、
        //   フォワード PS が t21 を Texture2D として宣言している以上、最初のサムネイル描画などで
        //   型不一致になる（GPU ベース検証が騒ぐ）。アトラスは 1x1 黒ダミー＝アルファ 0 で不可視。
        if (m_clusteredLighting && m_clusteredLighting->IsReady()
            && m_ssBlackSrvIndex != DescriptorHeap::kInvalidIndex)
        {
            for (u32 f = 0; f < DecalSystem::kFrameCount; ++f)
                m_decalSystem->WriteSrvsInto(*m_graphicsDevice, *m_srvHeap,
                                             m_clusteredLighting->GetSrvTableIndex(f), f,
                                             m_ssBlackTex.get());
        }
        m_decalSrvDirty = true;

        // 2D スプライト（バックバッファ＝スワップチェイン形式へ描く）
        m_spriteRenderer = std::make_unique<SpriteRenderer>();
        m_spriteRenderer->Initialize(*m_graphicsDevice, m_srvHeap.get(),
                                     m_swapChain->GetFormat(), PathResolver::ShaderDirW());

        // ゲーム内 retained-mode UI（UICanvas ツリー。##GameUI の ImGui DrawList へ描く）
        m_uiSystem = std::make_unique<UISystem>();
        // .uianim / .spranim の再生（エディタ中もプレビュー再生するので封印ランタイム判定は不要）
        m_uiAnimRuntime = std::make_unique<UiAnimRuntime>();
        m_uiAnimRuntime->SetAssetsDir(PathResolver::AssetsDir());
        // ワールド空間 2D（Sprite2D, worldSpace=true）: HDR scene RT へ描く別経路（HUD と隔離）
        // 深度バッファ(D32_FLOAT)に対して深度テスト＝3D形状に正しく遮蔽される。
        m_spriteRenderer->InitializeWorld(*m_graphicsDevice, kSceneColorFormat,
                                          DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

        // 画面全体のカスタムシェーダー（CameraComponent::screenShaderPath）の 1 パス。
        // ★入力 RT はレンダー解像度ではなく【表示解像度】。uber パスが
        //   「レンダー解像度 → 表示矩形」の拡大を担うので、その後段はもう表示解像度で回る。
        m_screenShaderPass = std::make_unique<ScreenShaderPass>();
        m_screenShaderPass->Initialize(*m_graphicsDevice, m_swapChain->GetFormat());
        const float screenClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_screenShaderRT = std::make_unique<RenderTarget>();
        m_screenShaderRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                     m_window->GetWidth(), m_window->GetHeight(),
                                     m_swapChain->GetFormat(), screenClear);

        // パーティクル歪みバッファ（熱ゆらぎ/衝撃波が画面を歪ませる。RG=UVオフセット）
        const float distortClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_distortRT = std::make_unique<RenderTarget>();
        m_distortRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                m_window->GetWidth(), m_window->GetHeight(),
                                DXGI_FORMAT_R16G16_FLOAT, distortClear);

        // ここまでで確保したシーン系 RT のサイズ＝現在のレンダー解像度（#16）。
        // 実際の値は次フレーム先頭の UpdateRenderResolution() が
        // 「表示矩形 × render_scale」へ合わせ直す。
        m_renderW = m_window->GetWidth();
        m_renderH = m_window->GetHeight();
        m_renderScale = static_cast<f32>(PersistGet("render_scale", 1.0));
        m_renderScale = std::clamp(m_renderScale, 0.25f, 1.0f);

        // 加算ビルボードパーティクル（HDR scene RT + 深度へ描く / Lua fx API）
        m_particleSystem = std::make_unique<ParticleSystem>();
        m_particleSystem->Initialize(*m_graphicsDevice, kSceneColorFormat,
                                     DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                     PathResolver::ShaderDirW(),
                                     m_srvHeap.get(), m_resourceManager.get());
        if (m_scriptEngine) m_scriptEngine->SetParticleSystem(m_particleSystem.get());

        // GPUパーティクル（compute シム + ExecuteIndirect。最大 131072 粒子・加算専用）
        m_gpuParticles = std::make_unique<GpuParticleSystem>();
        m_gpuParticles->Initialize(*m_graphicsDevice, kSceneColorFormat, PathResolver::ShaderDirW());
        if (m_scriptEngine) m_scriptEngine->SetGpuParticleSystem(m_gpuParticles.get());

        // パーティクルエディタ（ツール窓）。専用のオフスクリーンプレビュー(独立した ParticleSystem
        // インスタンス + RenderTarget)を持つ。ゲーム(封印ランタイム)では作らない。
        if (!m_isGameMode)
        {
            m_vfxEditorPanel = std::make_unique<VfxEditorPanel>();
            m_vfxEditorPanel->Initialize(*m_graphicsDevice, m_srvHeap.get(), m_resourceManager.get(),
                                        PathResolver::ShaderDirW());
            // トランジション窓。専用のオフスクリーン(架空のゲーム画面 2 枚を描く大プレビュー +
            // プリセット全件のサムネイルアトラス)を持つ。実機と同じ暗幕シェーダーを走らせるので、
            // ここで見た絵がそのまま配布ゲームで出る。
            m_transitionPreviewPanel = std::make_unique<TransitionPreviewPanel>();
            m_transitionPreviewPanel->Initialize(*m_graphicsDevice, m_srvHeap.get(),
                                                 PathResolver::ShaderDirW());
            // UIエディタ（ゲーム内UIの2Dキャンバス編集）。GPU リソースは持たない
            // （描画は UISystem::RenderPreview 経由で共有 SRV ヒープを借りるだけ）。
            m_uiEditorPanel = std::make_unique<UiEditorPanel>();
            // アニメ系オーサリング（.uianim タイムライン / .spranim シート）。どちらも
            // GPU リソースは持たず、テクスチャは共有 SRV ヒープから借りるだけ。
            m_animEditorPanel = std::make_unique<AnimationEditorPanel>();
            m_spriteSheetEditorPanel = std::make_unique<SpriteSheetEditorPanel>();
            m_networkPanel = std::make_unique<NetworkPanel>();

            m_materialEditorPanel = std::make_unique<MaterialEditorPanel>();
            m_materialEditorPanel->Initialize(m_materialAssetManager.get(), m_editorLayer->GetAssetBrowser(),
                                              *m_graphicsDevice, m_srvHeap.get(), m_resourceManager.get());
            // アセットブラウザの .dxmat 球体サムネイルはこのパネルのプレビューレンダラーを共用する
            m_editorLayer->SetMaterialPreviewRenderer(&m_materialEditorPanel->GetPreviewRenderer());

            // マテリアルグラフ窓（G2c）: ライブプレビュー / ノード内サムネイルの GPU 側（専用の RT / 定数 / 環境。メインシーンには触らない）。
            //   グラフ材質のランタイムが使えない環境（非対応 GPU / DX12_DISABLE_GRAPH_MATERIALS）では作らない = 窓は従来の空き枠のまま。
            if (m_graphMaterials && m_graphMaterials->IsAvailable())
            {
                MaterialGraphPanel::GpuInit gi;
                gi.device = m_graphicsDevice.get();
                gi.rootSignature = m_rootSignature.get();
                gi.resources = m_resourceManager.get();
                gi.srvHeap = m_srvHeap.get();
                gi.graphs = m_graphMaterials.get();
                gi.shaderDirW = PathResolver::ShaderDirW();
                MaterialGraphPanel::InitializeGpu(gi);
            }

            m_materialLibraryPanel = std::make_unique<MaterialLibraryPanel>();
            m_materialLibraryPanel->Initialize(m_resourceManager.get(), m_srvHeap.get(), m_materialAssetManager.get());
        }

        // シーントランジション
        m_sceneTransition = std::make_unique<SceneTransition>();
        m_sceneTransition->Initialize(*m_graphicsDevice, m_swapChain->GetFormat(), PathResolver::ShaderDirW());

        // IBL 環境マップ（irradiance/prefiltered/BRDF LUT）+ 任意スカイボックス
        m_iblBaker = std::make_unique<IBLBaker>();
        m_iblBaker->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());
        m_skyboxRenderer = std::make_unique<SkyboxRenderer>();
        m_skyboxRenderer->Initialize(*m_graphicsDevice, kSceneColorFormat, PathResolver::ShaderDirW());

        // シェーダーホットリロードの再生成コールバックを束ねる。ここまでで全パスの初回 PSO が
        // 揃っているので、この時点(Initialize 末尾付近)で一括登録する。
        RegisterShaderReloadHandlers();
    }

    // シーンフロー（assets/sceneflow.json があれば）
    m_sceneFlow = std::make_unique<SceneFlow>();
    m_sceneFlow->Load(PathResolver::AssetsDir() + "sceneflow.json");

    m_isRunning = true;

    // ゲームモードの場合、即座にPlayモードに入る
    if (m_isGameMode)
    {
        // ★キー割り当ての読み込みはプロジェクトロード完了時(ApplicationProject.cpp)にしか
        //   無かったが、その経路（BeginProjectLoad）はエディタ専用で、ゲームモードでは
        //   一度も通らない。結果「ゲーム内のオプションでキーを変えて保存はされるのに、
        //   次の起動で必ず既定へ戻る」状態だった。BaseDir はここで確定しているので読む。
        LoadActionBindings();

        m_pendingMode = EngineMode::Playing;
        m_modeChangeRequested = true;
    }

    // マルチプレイ テストクライアント起動(フェーズ⑨、--net-client "ip[:port]")。
    // ランチャーを飛ばして --project のプロジェクトを直接開き、ロード完了後(Update内)に
    // クライアントとして自動Play=Joinする。ip/port は EnterPlayMode の自動接続が参照する。
    if (!m_isGameMode && !m_pendingNetClientJoin.empty())
    {
        std::string ip = m_pendingNetClientJoin;
        if (size_t c = ip.rfind(':'); c != std::string::npos)
        {
            m_editorCtx->netTestJoinPort = static_cast<u16>(std::atoi(ip.c_str() + c + 1));
            ip.resize(c);
        }
        if (!ip.empty()) m_editorCtx->netTestJoinAddress = ip;
        m_editorCtx->netTestRole = NetTestRole::Client;

        if (!m_pendingNetClientProject.empty())
        {
            ProjectInfo info;
            if (ProjectManager::ProjectFromFolder(m_pendingNetClientProject, info))
                BeginProjectLoad(info, /*isNew=*/false);   // m_showLauncher=false もここで立つ
            else
                Logger::Warn("--project のプロジェクトが開けません: {}", m_pendingNetClientProject);
        }
        else
        {
            m_showLauncher = false;   // プロジェクト指定なし=既に読み込んだ最後のシーンのまま参加
        }
        m_netClientAutoPlayPending = true;
        m_pendingNetClientJoin.clear();
    }
    // --project 単独指定(--net-client 無し): ランチャーを飛ばして指定プロジェクトを開くだけ。
    // 自動 Play / 接続はしない(テストクライアント専用の挙動は --net-client 併用時のみ)。
    else if (!m_isGameMode && !m_pendingNetClientProject.empty())
    {
        ProjectInfo info;
        if (ProjectManager::ProjectFromFolder(m_pendingNetClientProject, info))
            BeginProjectLoad(info, /*isNew=*/false);
        else
            Logger::Warn("--project のプロジェクトが開けません: {}", m_pendingNetClientProject);
        m_pendingNetClientProject.clear();
    }
    // 通常起動(引数なし): 前回プロジェクトは復元せず、ランチャー(m_showLauncher=true のまま)を表示する。

    // 3D モデルのサムネイル: 起動時の一括生成（1 枚ごとに WaitIdle する同期ループ）はフェーズ 1b で廃止した。
    // アセットブラウザが「見えているセルだけ」を 1 フレーム数枚の予算で作り（ModelThumbnailRenderer::RenderPending）、
    // 結果はプロジェクトの assets/.thumbcache/ にヘッダ付きで保存する（次回はアップロードだけ）。
    // ゲーム(封印ランタイム)は元から何もしない＝exe 隣へ .thumbcache を作らない。

    // IBL: シーンの skybox 設定に応じて環境キューブを読み込み派生をベイク（専用 cmdList）。
    SplashScreen::SetStage(splash::Stage::EnvMap);
    {
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
        LoadSkyboxIfNeeded(cmdList);
        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();
        m_frameResources->EndFrame(*m_commandQueue);
        if (m_envCubeTex) m_envCubeTex->FinishUpload();
        m_resourceManager->FinishUploads();
    }

    // ここから先(メインループ)は WaitIdle 無しでフレームを多重化するため、
    // GPUリソースの解放をフェンス連動の遅延解放に切り替える
    DeferredRelease::Enable();

    // メインウィンドウはここではまだ表示しない。Run() の先頭数フレームを隠れたまま描画し、
    // ランチャーの初回描画・ImGuiフォント・ドライバのPSOウォームアップを済ませてから表示する
    // （「白いウィンドウが出てから絵が出るまで」のフリーズ見えを根絶。表示タイミングが決定的になる）。
    // deferMainWindow でない経路（ゲーム/ヘッドレスビルド）はウィンドウが既に表示済み。
    m_deferredFirstShow = deferMainWindow;
    if (deferMainWindow)
        SplashScreen::SetStage(splash::Stage::Finalize);

    Logger::Info("Application initialized successfully");

    // AI(MCP)ブリッジ。エディタ時のみ。ゲーム(封印ランタイム)では起動しない＝外部から触れない。
    // ヘッドレス --build でも起動しない: 起動中エディタが 8787 を握っている状態で build が
    // 走ると 8788 に bind→WritePortFile が %TEMP%/dx12_mcp.port を 8788 で上書きし、build 終了で
    // その port が死ぬ＝Node 側の自動検出が死にポートを掴みライブツールが切れる原因になる。
    if (!m_isGameMode && !buildMode)
    {
        m_mcpBridge = std::make_unique<McpBridge>();
        // --mcp-port で明示指定されたらそこに固定し、dx12_mcp.port は書かない
        //（複数インスタンスを並べたときに互いの検出ファイルを奪い合わないため）。
        const uint16_t base = (m_mcpPortRequest > 0)
            ? static_cast<uint16_t>(m_mcpPortRequest) : static_cast<uint16_t>(8787);
        m_mcpBridge->Start(base, m_mcpPortRequest <= 0);
        if (m_mcpPortRequest > 0)
            Logger::Info("MCP ポートを固定しました: {}（dx12_mcp.port は更新しない）",
                         m_mcpBridge->Port());
    }
}


void Application::Run()
{
    Logger::Info("Application running...");

    // Windowsタイマー精度を1msに設定
    timeBeginPeriod(1);

    while (!m_window->ShouldClose())
    {
        m_frameStart = std::chrono::high_resolution_clock::now();

        // AI(MCP)から溜まったコマンドをメインスレッドで処理(scene/scriptengine を安全に触れる)。
        // ★波括弧が無く、if が効いていたのは CpuScopeTimer の宣言 1 行だけだった。
        //   Poll は**無条件**に呼ばれるため、MCP ブリッジを起動しないゲームモード
        //   （m_mcpBridge は null）では 1 フレーム目で null 参照して落ちる。
        //   ＝**ビルドして配布したゲームが起動直後に必ずクラッシュする**状態だった
        //   （a1c73e7 の TU 分割で混入。GameRuntime を起動するテストが無く 3 日気づけなかった）。
        if (m_mcpBridge)
        {
            CpuScopeTimer _tMcp(&m_cpuMs[CpuMcp]);
            DX12_PROFILE_ZONE_N("MCP/Poll");
            m_mcpBridge->Poll([this](uint64_t client, const std::string& line) {
                return HandleMcpCommand(client, line);
            });
        }

        // ---- フリート運用の自己終了（--owner-pid / --idle-exit。core/mcp/FleetGuard.h）----
        //   MCP サーバが落ちた・エージェントが閉じ忘れた・無操作が続いた、のいずれでもエンジンが残らないようにする。
        //   表示モードで人が触っている間(実入力)は活動に数える。UI 自動テスト中は idle で終わらない。
        {
            auto& fg = fleet::Instance();
            if (ImGui::GetCurrentContext())
            {
                const ImGuiIO& fio = ImGui::GetIO();
                if (fio.MouseDelta.x != 0.0f || fio.MouseDelta.y != 0.0f || fio.MouseWheel != 0.0f
                    || fio.MouseClicked[0] || fio.MouseClicked[1] || fio.MouseClicked[2]
                    || !fio.InputQueueCharacters.empty())
                    fg.TouchInput();
            }
            if (m_uiTestsRequested) fg.TouchInput();   // --ui-tests 系（m_uiTests は通常起動でも常に在るので使えない）
            static bool fleetQuitting = false;
            const fleet::ExitReason why = fleetQuitting ? fleet::ExitReason::None : fg.PollExit();
            if (why != fleet::ExitReason::None)
            {
                fleetQuitting = true;
                Logger::Info("Fleet: exiting ({})", fleet::ExitReasonName(why));
                if (m_editorCtx && m_editorCtx->aiSessionEver && m_editorCtx->IsSceneDirty()
                    && !(m_headless && !m_headlessAllowSave))
                {
                    try { SaveSceneForMcp(); } catch (...) {}   // best-effort（使い捨てプロジェクトが前提）
                }
                PostQuitMessage(0);
            }
        }

        // ---- 仮想入力モードの「人の脱出口」----
        //   ① Ctrl+Alt+Shift+F12（実キーボード。WndProc が要求を立てる）で OFF。
        //   ② 実行中に MCP が ON にしたモードは、MCP が繋がっていない状態が 5 秒続いたら OFF に戻す
        //      （AI のセッションが落ちて、人の実入力が効かないまま取り残されるのを防ぐ）。
        //      起動引数（--virtual-input / --background）で ON にしたモードは意図的なので戻さない。
        if (vinput::Enabled())
        {
            if (vinput::ConsumeEscape())
            {
                Logger::Warn("仮想入力モード: 人の脱出操作（Ctrl+Alt+Shift+F12）で OFF にしました");
                ApplyVirtualInputMode(false);
                m_virtualInputRuntime = false;
            }
            else if (m_virtualInputRuntime)
            {
                if (m_mcpBridge && m_mcpBridge->IsConnected()) m_virtualInputOrphanSec = 0.0f;
                else m_virtualInputOrphanSec += m_gameClock.GetDeltaTime();
                if (m_virtualInputOrphanSec > 5.0f)
                {
                    Logger::Warn("仮想入力モード: MCP が 5 秒以上切れているので OFF に戻しました（人の入力を復帰）");
                    ApplyVirtualInputMode(false);
                    m_virtualInputRuntime = false;
                    m_virtualInputOrphanSec = 0.0f;
                }
            }
        }
        else
        {
            vinput::ConsumeEscape();   // OFF のときに立った要求は捨てる
            m_virtualInputRuntime = false;
        }

        // 超詳細診断からのフレーム読み戻し要求。ReadbackSceneBgra は内部でコマンドリストを
        // 開くのでフレーム境界のここでしか呼べない（ImGui のテスト本体から直接は呼べない）。
        if (m_diagFrameStatsRequest)
        {
            m_diagFrameStatsRequest = false;
            std::vector<u8> bgra;
            u32 w = 0, h = 0;
            std::string err;
            DiagFrameStats st;
            if (ReadbackSceneBgra(bgra, w, h, err))
            {
                const size_t px = static_cast<size_t>(w) * h;
                u64    lumaSum  = 0;
                size_t nonBlack = 0;
                u64    hash     = 1469598103934665603ull;   // FNV-1a
                for (size_t i = 0; i < px; ++i)
                {
                    const uint8_t b = bgra[i * 4 + 0], g = bgra[i * 4 + 1], r = bgra[i * 4 + 2];
                    const uint32_t l = (r * 54u + g * 183u + b * 19u) >> 8;   // 近似輝度
                    lumaSum += l;
                    if (l > 8) ++nonBlack;
                    // 全画素を混ぜると遅いので 64 画素おき。絵の変化検出にはこれで十分。
                    if ((i & 63) == 0) { hash ^= (r | (g << 8) | (b << 16)); hash *= 1099511628211ull; }
                }
                st.valid    = true;
                st.width    = w;
                st.height   = h;
                st.meanLuma = px ? static_cast<float>(lumaSum) / static_cast<float>(px) / 255.0f : 0.0f;
                st.nonBlack = px ? static_cast<float>(nonBlack) / static_cast<float>(px) : 0.0f;
                st.hash     = hash;
            }
            else
            {
                Logger::Warn("超詳細診断: フレーム読み戻しに失敗: {}", err);
            }
            m_diagFrameStats = st;
        }

        // --net-client: プロジェクトロード完了後にクライアントとして自動Play=Join(フェーズ⑨)。
        // ※ Update 内で立てると同フレームの EditorLayer::Render 後の
        //    「m_pendingMode = pendingPlayMode ? ...」に Editor へ上書きされるので、
        //    消費直前のここで立てて即座に消費させる。
        if (m_netClientAutoPlayPending && !m_loading && !m_modeChangeRequested
            && m_engineMode == EngineMode::Editor)
        {
            m_netClientAutoPlayPending = false;
            m_pendingMode = EngineMode::Playing;
            m_modeChangeRequested = true;
        }

        // エンジン診断(UI自動テスト)からの Play/Stop 要求。ImGui パスより前のここで消費する
        // （Render 側の「m_modeChangeRequested なら pendingPlayMode で上書き」に潰されないため）。
        if (m_diagModeRequest != 0 && !m_loading)
        {
            m_pendingMode = (m_diagModeRequest == 2) ? EngineMode::Playing : EngineMode::Editor;
            m_diagModeRequest = 0;
            m_modeChangeRequested = true;
        }

        // ★プロジェクト/シーンの読み込み中は Play に入らない（要求は持ち越し、読み終えた次のフレームで入る）。
        //   open_project は即応答するので、直後の play が「前のプロジェクトのシーンがまだ載っている」間に
        //   通ってしまい、前のシーンを新しい assets 基準でスナップショットしていた。そのまま新しいシーンが
        //   Play 中に読み込まれ、Stop で前のシーン（モデルの解決に失敗した欠けた状態）と前の保存先が戻り、
        //   自動保存が前のプロジェクトのシーンを上書きしていた（2026-10-02 に実際に 8→6 体）。
        const bool sceneLoadBusy = m_loading || m_sceneLoadJob
            || (m_editorCtx && !m_editorCtx->pendingLoadPath.empty());
        const bool holdPlay = m_modeChangeRequested && m_pendingMode == EngineMode::Playing
            && m_engineMode != EngineMode::Playing && sceneLoadBusy;

        // モード切替（前フレームのImGuiボタンから遅延実行）
        if (m_modeChangeRequested && !holdPlay)
        {
            m_modeChangeRequested = false;
            try
            {
                if (m_pendingMode == EngineMode::Playing)
                    EnterPlayMode();
                else if (m_engineMode == EngineMode::Playing || !m_playSceneJson.empty())
                    EnterEditorMode();
                // else: 既に Editor（プロジェクト切替前の停止で消化済み）。何もしない。
            }
            catch (const std::exception& ex)
            {
                Logger::Error("モード切替に失敗: {}", ex.what());
                if (m_engineMode == EngineMode::Playing)
                    m_scriptEngine->OnPlayStop();
                m_engineMode = EngineMode::Editor;
                m_inputSystem->SetMouseCapture(false);
            }

            // MCP play/stop の遅延応答（モード遷移が確定した直後に本物のモードを返す）。
            if (m_mcpModeReply.client != 0)
            {
                const bool wantPlaying = (m_pendingMode == EngineMode::Playing);
                const bool nowPlaying  = (m_engineMode == EngineMode::Playing);
                if (wantPlaying && !nowPlaying)
                    FailMcp(m_mcpBridge.get(), m_mcpModeReply, McpErr::ModeConflict,
                            "play failed (no active camera? check dx12_get_log)");
                else
                    // scriptErrors: Play は Lua が全滅していても ok を返してしまうので、
                    // OnPlayStart 時点で死んでいるコンポーネント数をここで一緒に返す
                    // (中身は dx12_get_script_errors)。0 でないなら絵を見る前にそっちを疑う。
                {
                    nlohmann::json r{{"mode", nowPlaying ? "Playing" : "Editor"},
                                     {"sceneGeneration", m_sceneGeneration},
                                     {"scriptErrors", m_scriptEngine
                                          ? m_scriptEngine->CollectScriptErrors().size() : 0u}};
                    // 配置検査の要約（Play を撃った瞬間＝Editor の状態で測ったもの）。
                    // ★AI は返り値に出ていないことは見ない。だから「検査ツールを用意する」だけでは
                    //   効かず、Play の返り値へ混ぜて初めて埋まり/ちらつきが直るようになる。
                    if (!m_mcpPlayLayout.is_null()) r["layout"] = m_mcpPlayLayout;
                    m_mcpPlayLayout = nlohmann::json();
                    CompleteMcp(m_mcpBridge.get(), m_mcpModeReply, std::move(r));
                }
                m_mcpModeReply = {};
            }
        }

        // 入力状態リセット（前フレームのdeltaクリア + prevKeys保存 + XInputポーリング）
        m_inputSystem->Update(m_gameClock.GetDeltaTime());

        // メッセージ処理（ここで WM_KEYDOWN/WM_MOUSEMOVE → InputSystem に蓄積）
        m_window->ProcessMessages();

        // DPI: 倍率オーバーライドの要求（UI テスト）をメインスレッドで反映する
        if (const float req = m_uiScaleRequest.exchange(-1.0f); req >= 0.0f && !m_isGameMode)
        {
            m_window->SetUiScaleOverride(req);               // 背景窓なら物理サイズも作り直す（WM_SIZE → 下のリサイズ処理）
            if (m_imguiManager) m_imguiManager->SetUiScaleOverride(req);   // 次の BeginFrame で反映
        }

        // --background=minimized: 窓が最小化されていてもクライアント矩形が 0 になるだけで、
        // 論理解像度（スワップチェインの大きさ）は保たれる。ImGui へはこの論理解像度を伝える。
        if (m_bgOptions.Active() && m_imguiManager)
            m_imguiManager->SetLogicalDisplaySize(m_window->GetWidth(), m_window->GetHeight());

        if (m_window->ShouldClose())
            break;

        // リサイズ処理（★ここで作り直すのはスワップチェイン＝表示解像度だけ。
        //   シーン系 RT は下の UpdateRenderResolution() が renderScale 込みで面倒を見る）
        if (m_window->WasResized())
        {
            m_window->ResetResizedFlag();
            u32 w = m_window->GetWidth();
            u32 h = m_window->GetHeight();
            if (w > 0 && h > 0)
            {
                m_commandQueue->WaitIdle();
                m_swapChain->Resize(w, h, *m_descriptorHeap);
                // スクリーンシェーダーの入力 RT は【表示解像度】なのでここで追従させる
                // （renderScale で動くシーン系 RT の UpdateRenderResolution とは別扱い）。
                if (m_screenShaderRT) m_screenShaderRT->Resize(*m_graphicsDevice, w, h);
                m_renderResFlush = true;   // レンダー解像度はデバウンスせず即時追従させる

                // カメラアスペクト比更新（エディタモードではサイドバー分引く）
                m_camera->SetPerspective(DirectX::XM_PIDIV4,
                    static_cast<f32>(w) / static_cast<f32>(h), 0.1f, 1000.0f);

                Logger::Info("Resized to {}x{}", w, h);
            }
        }

        // 表示矩形 × renderScale へシーン系 RT を追従させる（#16）。
        // ★必ず Render() より前・フレーム外で呼ぶこと（内部で WaitIdle する）。
        UpdateRenderResolution();
        // Q2（校正）: ライティング単位の PSO 差し替え / オフスクリーン撮影の進行。どちらも既定では何もしない。
        UpdateLightingUnits();
        ServiceOffscreenShot();

        m_gameClock.Tick();

        // シーントランジション更新（WP9）
        // 段階ロード中は中間点（画面が隠れきった状態）で止める＝ロードが終わる前に
        // 開き始めて、まだ構築されていないシーンが見えてしまうのを防ぐ。
        if (m_sceneTransition)
        {
            m_sceneTransition->SetHold(m_sceneLoadJob != nullptr);
            m_sceneTransition->Update(m_gameClock.GetDeltaTime());
        }

        // Luaホットリロード（0.5秒ごとにファイル変更チェック）
        m_scriptPollTimer += m_gameClock.GetDeltaTime();
        if (m_scriptPollTimer >= kScriptPollInterval)
        {
            m_scriptPollTimer = 0.0f;
            std::string scriptPath = PathResolver::GameLuaPath();
            if (std::filesystem::exists(scriptPath))
            {
                auto currentTime = std::filesystem::last_write_time(scriptPath);
                if (currentTime != m_scriptLastWriteTime)
                {
                    Logger::Info("Hot-reload: game.lua changed, reloading...");
                    m_commandQueue->WaitIdle();
                    ReloadGameScript();
                    m_scriptLastWriteTime = currentTime;
                    m_editorCtx->Notify(ui::ToastKind::Success, "スクリプトを再読み込みしました: game.lua");
                    Logger::Info("Hot-reload complete");
                }
            }

            UpdateAutosave(kScriptPollInterval);   // 中で 60 秒ぶん貯めてから書く

            // MCP（AI）セッションの状態を更新して、溜まった編集をディスクへ落とす。
            // ★aiSessionEver は一度立てたら落とさない。落とすと「Claude のセッションが
            //   終わった直後に人が窓を閉じる」で未保存モーダルが復活する。
            if (m_editorCtx)
            {
                const bool connected = m_mcpBridge && m_mcpBridge->IsConnected();
                m_editorCtx->aiSessionActive = connected;
                if (connected) m_editorCtx->aiSessionEver = true;
            }
            UpdateMcpAutoSave(kScriptPollInterval);

            // シーン設定（ポスト/SSAO/空/フォグ等）の変更検知。これらを触る窓は 20 箇所以上
            // あってどれも Undo を積まないので、1 箇所ずつフックする代わりに設定そのものを
            // 定期比較する。Playing 中は Lua が触っても Stop で巻き戻るので見ない。
            if (m_editorCtx && m_scene && !m_showLauncher && !m_isGameMode
                && m_engineMode == EngineMode::Editor)
            {
                // 現在値を書くだけ。未保存かどうかの判定は EditorContext::IsSceneDirty が
                // savedSettingsHash と比べて行う。ここで MarkEdited すると、保存直前の
                // 変更を保存後に二重計上してしまう。
                m_editorCtx->settingsHash = SceneSettingsFingerprint(*m_scene);
            }

            // コンポーネント .lua（assets/components/*.lua 等）のホットリロード。
            // game.lua と違い RebuildScene は要らない: 該当エンティティの env を捨てるだけで、
            // 次の UpdateAttachedScripts が作り直す。Play を止めずにスクリプトを差し替えられる。
            if (m_scriptEngine)
            {
                const auto reloaded = m_scriptEngine->ReloadChangedScripts();
                if (reloaded > 0)
                    m_editorCtx->Notify(ui::ToastKind::Success,
                        "スクリプトを再読み込みしました（" + std::to_string(reloaded) + " 件）");
            }
        }

        // シェーダーホットリロード（0.5秒ごとに .hlsl/.hlsli 変更チェック）
        if (m_shaderManager && m_shaderManager->IsRuntimeCompileAvailable())
        {
            m_shaderPollTimer += m_gameClock.GetDeltaTime();
            if (m_shaderPollTimer >= kScriptPollInterval)
            {
                m_shaderPollTimer = 0.0f;
                std::vector<std::wstring> changed = m_shaderManager->Poll();
                if (!changed.empty())
                {
                    m_commandQueue->WaitIdle();
                    m_shaderManager->DispatchReloadHandlers(changed);
                    m_editorCtx->Notify(ui::ToastKind::Success,
                        "シェーダーを再読み込みしました（" + std::to_string(changed.size()) + " 件）");
                }
            }
        }

        // MCP screenshot_game_view: Editor 中は一時的にアクティブなゲームカメラへ切り替えて1フレーム描く。
        // Playing 中は m_camera が既にゲームカメラなので上書き不要(通常 screenshot と同じ絵)。
        const bool gvShot     = (m_mcpGameViewReply.client != 0);
        const bool gvOverride = gvShot && (m_engineMode != EngineMode::Playing);
        DirectX::XMFLOAT3 gvPos{}; f32 gvYaw=0, gvPitch=0, gvFov=0, gvAsp=0, gvNear=0, gvFar=0, gvOrthoH=0;
        bool gvOrtho=false;
        if (gvOverride)
        {
            gvPos=m_camera->GetPosition(); gvYaw=m_camera->GetYaw(); gvPitch=m_camera->GetPitch();
            gvFov=m_camera->GetFovY(); gvAsp=m_camera->GetAspect();
            gvNear=m_camera->GetNearZ(); gvFar=m_camera->GetFarZ();
            gvOrtho=m_camera->IsOrthographic(); gvOrthoH=m_camera->GetOrthoHeight();
        }

        try
        {
            { CpuScopeTimer _t(&m_cpuMs[CpuUpdate]); DX12_PROFILE_ZONE_N("Update"); Update(); }
            if (gvOverride) SyncActiveCameraToGlobal();   // Update の後に上書き(編集カメラ操作に勝つ)
            ApplyPerceptionCamera();                      // dx12_perceive の視点（要求中だけ。同じ理由で Update の後）
            Render();
            // 検証用フック: 環境変数 DX12E_GAME_SHOT=<png> を付けて起動した配布ゲームが、
            //   DX12E_GAME_SHOT_SEC（既定 20）秒後に最初のシーン画像を書いて終了する。
            //   MCP を持たない配布ゲームを「前面に出さず・OS の入力に触れず」にエディタと見比べるための入口
            //   （pak 経由で全アセットが描けているかの検証用。未設定なら何もしない）。
            if (m_isGameMode)
            {
                static const std::string shotPath = [] {
                    char buf[1024] = {};
                    return GetEnvironmentVariableA("DX12E_GAME_SHOT", buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
                }();
                if (!shotPath.empty())
                {
                    static const auto t0 = std::chrono::steady_clock::now();
                    static const double waitSec = [] {
                        char buf[32] = {};
                        return GetEnvironmentVariableA("DX12E_GAME_SHOT_SEC", buf, sizeof(buf)) > 0 ? std::atof(buf) : 20.0;
                    }();
                    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= waitSec)
                    {
                        std::string serr;
                        const std::string wrote = CaptureSceneScreenshot(serr, shotPath);
                        Logger::Info("GAME_SHOT: {} {}", wrote.empty() ? "FAILED" : wrote, serr);
                        m_window->RequestClose();
                    }
                }
            }
            // ビルド時のテクスチャ事前生成（BuildGame が隠し窓で 1 回だけ走らせる）。
            //   DX12E_TEXBAKE_LIST=<出力リスト> が付いているときだけ動く。開始シーンは起動時に読み込み済みなので、
            //   DX12E_TEXBAKE_FILES=<相対パスを 1 行 1 件で並べたファイル> のシーン/プレハブ/マテリアルを
            //   順に先読みして（= 配布ゲームが後から使うテクスチャを全部 BC 圧縮させて）、
            //   使われたキャッシュの一覧を書いて終了する。キャッシュの置き場は DX12E_TEXBAKE_DIR（TextureLoader 側）。
            if (m_isGameMode)
            {
                static const std::string bakeList = [] {
                    char buf[1024] = {};
                    return GetEnvironmentVariableA("DX12E_TEXBAKE_LIST", buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
                }();
                if (!bakeList.empty())
                {
                    static std::vector<std::string> queue = [] {
                        std::vector<std::string> q;
                        char buf[1024] = {};
                        if (GetEnvironmentVariableA("DX12E_TEXBAKE_FILES", buf, sizeof(buf)) > 0)
                        {
                            std::ifstream f(buf);
                            std::string line;
                            while (std::getline(f, line))
                            {
                                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                                if (!line.empty()) q.push_back(line);
                            }
                            std::reverse(q.begin(), q.end());   // 後ろから取り出す
                        }
                        return q;
                    }();
                    static int bakeFrames = 0;
                    ++bakeFrames;
                    const bool idle = !m_scenePreloadJob && m_pendingScenePreloadAsync.empty() && !m_sceneLoadJob;
                    if (idle && !queue.empty() && bakeFrames >= 3)
                    {
                        m_pendingScenePreloadAsync = queue.back();
                        queue.pop_back();
                    }
                    else if (idle && queue.empty() && bakeFrames >= 120)   // 開始シーンの Lua が後から読むものも拾う
                    {
                        const bool ok = TextureLoader::WriteBakeList(bakeList);
                        Logger::Info("TEXBAKE: {} ({} フレーム)", ok ? "done" : "FAILED", bakeFrames);
                        m_window->RequestClose();
                    }
                }
            }
            m_consecFrameErrors = 0;   // 1 枚描けたら「復帰した」＝連続失敗を数え直す
        }
        catch (const std::exception& ex)
        {
            if (ReportFrameError(ex.what())) break;

            // ★まずデバイスが生きているかを見る。
            //   デバイスロスト(TDR / ドライバのハング / GPU リセット)は「次フレームで
            //   復帰を試みる」種類のエラーではない。以後どの GPU 呼び出しも同じ HRESULT で
            //   失敗し続け、失敗を握り潰した先で null になった Map ポインタへ書き込んで落ちる。
            //   実際 SkinningBuffer::Update がその形でアクセス違反を起こしていた
            //   （デバイスロスト → 例外 → 続行 → 次フレームで 0x20 番地へ書き込み）。
            //   ここで止めないと、落ちるまでの数フレームで作業内容ごと失われる。
            if (HandleDeviceLoss()) break;

            // 復帰を試みる。cmdList が open のまま残ると次の BeginFrame の Reset が
            // 失敗し続けて復帰不能ループになるため、必ず AbortFrame で Close しておく。
            m_commandQueue->WaitIdle();
            if (m_frameResources)
                m_frameResources->AbortFrame();
            if (m_engineMode == EngineMode::Playing)
            {
                m_scriptEngine->OnPlayStop();
                m_engineMode = EngineMode::Editor;
                m_inputSystem->SetMouseCapture(false);
                Logger::Error("エディタモードへ強制復帰しました");
            }
        }

        // 仮想入力（dx12_imgui_pointer / key）の遅延応答。キューが流れ切って ImGui が反応した後に返す。
        ServiceMcpVirtualInput();
        ServiceMcpEditorUi();   // M7: editor_command_run の遅延応答

        // 遅延初回表示: 隠れたまま数フレーム描画して絵（ランチャー）が確定してから
        // ウィンドウを出し、スプラッシュを閉じる。表示された瞬間には既に描画済み＝
        // 「白いまま固まって見える/出るタイミングが不安定」が起きない。
        if (m_headless) m_deferredFirstShow = false;   // 窓は出さない（隠したまま回す）
        if (m_deferredFirstShow && ++m_warmupFrames >= 3 && !m_loading)
        {
            // ロード中(--project直開き等)はまだ出さない。ロード完了時に
            // UpdateProjectLoad 側が Finish（メイン窓の表示 + スプラッシュの退場）を引き継ぐ。
            m_deferredFirstShow = false;
            // ready の演出（リング完成 → ハイライト → ポン）のあと、演出が PumpMainThread 経由でこのラムダを呼び、
            // メイン窓を出して、その上でスプラッシュが拡大しながらフェードアウトする。スプラッシュが無ければ即出す。
            SplashScreen::Finish([this] { if (m_window) m_window->Show(); });   // 最大化。直後の微小リサイズは描画継続中に処理される
        }
        SplashScreen::PumpMainThread();

        // ★決定論キャプチャ（#31）: 時間依存を固定したまま N フレーム回して履歴を収束させ、
        //   0 になった時点で撮る。sceneRT はここで直接読めるが、バックバッファは Render() の
        //   中でしかコピーできないので pending を立てて次フレームに撮らせる。
        if (m_deterministicCapture && m_deterministicFramesLeft > 0 && --m_deterministicFramesLeft == 0)
        {
            if (m_mcpPerceive && m_mcpPerceive->phase == McpPerceiveJob::Phase::Settling)
            {
                // dx12_perceive: 次フレームの Render が最終画のコピーと ID パスを同じコマンドリストへ積む
                m_mcpPerceive->phase = McpPerceiveJob::Phase::Pending;
            }
            else if (m_mcpFinalShot.wantSceneRt)
            {
                std::string derr;
                const std::string dpath = CaptureSceneScreenshot(derr, m_mcpFinalShot.path);
                if (dpath.empty())
                    FailMcp(m_mcpBridge.get(), m_mcpFinalShot.reply, McpErr::Internal,
                            derr.empty() ? "screenshot failed" : derr);
                else
                    CompleteMcp(m_mcpBridge.get(), m_mcpFinalShot.reply,
                        nlohmann::json{{"path", dpath},
                                       {"width",  m_sceneRT ? m_sceneRT->GetWidth()  : 0u},
                                       {"height", m_sceneRT ? m_sceneRT->GetHeight() : 0u},
                                       {"source", "sceneRT(pre-post)"},
                                       {"deterministic", true},
                                       {"note", "決定論モード: time / TAA ジッタ / フォグ・SSGI の位相を固定し、"
                                                "履歴を収束させてから撮った。ゲームのシミュレーションは止まらない"}});
                m_mcpFinalShot = {};
                m_deterministicCapture = false;
            }
            else
            {
                m_mcpFinalShot.pending = true;   // 次フレームの Render がバックバッファをコピーする
            }
        }

        // screenshot_final: Render() 内でバックバッファをコピー済みなら PNG 化して遅延応答を返す。
        FinishFinalScreenshot();
        // dx12_perceive: Render() が ID パスを記録済みなら読み戻して集計し、遅延応答を返す。
        FinishPerception();

        // screenshot_game_view: このフレームの描画(ゲームカメラ視点)を撮って遅延応答 → 編集カメラ復元。
        if (gvShot)
        {
            std::string serr;
            const std::string p = CaptureSceneScreenshot(serr, m_mcpGameViewPath);
            if (p.empty())
                FailMcp(m_mcpBridge.get(), m_mcpGameViewReply, McpErr::Internal,
                        serr.empty() ? "screenshot failed" : serr);
            else
                CompleteMcp(m_mcpBridge.get(), m_mcpGameViewReply,
                    nlohmann::json{{"path", p}, {"width", m_sceneRT->GetWidth()},
                                   {"height", m_sceneRT->GetHeight()},
                                   {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                                   // 撮った投影（編集カメラを戻す前の値）。ゲームカメラの fovDegrees と
                                   // 一致するはず。正射なら orthoHeight を返す
                                   {"orthographic", m_camera->IsOrthographic()},
                                   {"fovDeg", DirectX::XMConvertToDegrees(m_camera->GetFovY())},
                                   {"orthoHeight", m_camera->GetOrthoHeight()}});
            m_mcpGameViewReply = {};
            if (gvOverride)   // 編集カメラを完全に復元(位置/向き/投影)
            {
                m_camera->SetPosition(gvPos); m_camera->SetYaw(gvYaw); m_camera->SetPitch(gvPitch);
                if (gvOrtho) m_camera->SetOrthographic(gvOrthoH, gvAsp, gvNear, gvFar);
                else         m_camera->SetPerspective(gvFov, gvAsp, gvNear, gvFar);
            }
        }

        // MCP render_debug: N フレーム描いたらスクショを撮って返し、必ず元の設定へ戻す。
        // 安全網: 「可視化モードは立っているのに後始末の予約が無い」= 誰かが途中で
        // 抜けた状態。放置するとシーンビューが真っ暗のまま永久に戻らないので、
        // 気付いた時点で素の描画へ復帰させる（原因はログに出して分かるようにする）。
        if (m_renderDebugMode != 0 && m_mcpRenderDebugFramesLeft == 0)
        {
            Logger::Warn("render_debug が後始末されないまま残っていたので解除しました（mode={}）",
                         m_renderDebugModeName);
            m_renderDebugMode        = 0;
            m_renderDebugModeName.clear();
            m_renderDebugRawReadback = false;
            RestoreRenderDebugSettings();
        }

        if (m_mcpRenderDebugFramesLeft > 0 && --m_mcpRenderDebugFramesLeft == 0
            && m_mcpRenderDebugReply.client != 0)
        {
            std::string rerr;
            const std::string rpath = (m_renderDebugModeName == "off")
                                    ? std::string("(no capture)")
                                    : CaptureSceneScreenshot(rerr, m_mcpRenderDebugPath);

            nlohmann::json warnJson = nlohmann::json::array();
            if (!m_renderDebugWarnings.empty())
            {
                try { warnJson = nlohmann::json::parse(m_renderDebugWarnings); }
                catch (...) { warnJson = nlohmann::json::array(); }
            }

            if (rpath.empty())
            {
                FailMcp(m_mcpBridge.get(), m_mcpRenderDebugReply, McpErr::Internal,
                        rerr.empty() ? "render_debug screenshot failed" : rerr);
            }
            else
            {
                CompleteMcp(m_mcpBridge.get(), m_mcpRenderDebugReply,
                    nlohmann::json{{"path", rpath},
                                   {"mode", m_renderDebugModeName},
                                   {"width",  m_sceneRT ? m_sceneRT->GetWidth()  : 0u},
                                   {"height", m_sceneRT ? m_sceneRT->GetHeight() : 0u},
                                   {"toneMapped", !m_renderDebugRawReadback},
                                   {"warnings", warnJson},
                                   {"mode_engine", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"}});
            }
            m_mcpRenderDebugReply = {};

            // ---- 退避した設定を必ず戻す（デバッグ表示を残さない）----
            RestoreRenderDebugSettings();
            m_renderDebugMode        = 0;
            m_renderDebugRawReadback = false;
            m_renderDebugWarnings.clear();
        }

        // MCP step_frames: 1フレーム回り切ったらカウントダウン。0 になったら遅延応答を返す。
        if (m_mcpStepFramesLeft > 0 && --m_mcpStepFramesLeft == 0 && m_mcpStepReply.client != 0)
        {
            // ★固定 dt は必ずここで戻す。戻し忘れるとエディタが以後ずっと
            //   「実時間と無関係な速さ」で回り続ける（人が触ったときに一番分かりにくい壊れ方）。
            const bool wasFixed = m_mcpStepFixedDt;
            const f32  usedDt   = m_gameClock.FixedDelta();
            if (m_mcpStepFixedDt) { m_gameClock.ClearFixedDelta(); m_mcpStepFixedDt = false; }
            // 決定論ステップの既定は「進めたら止める」。次の step_frames が解除する。
            // ★止めっぱなしで MCP が切れても、Play/Stop の遷移で必ず false へ戻る
            //   （EnterPlayMode / ExitPlayMode が paused をリセットする既存の仕組み）。
            if (wasFixed && m_mcpStepHold && m_editorCtx) m_editorCtx->paused = true;
            nlohmann::json r{{"stepped", true},
                             {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                             {"sceneGeneration", m_sceneGeneration}};
            if (wasFixed)
            {
                r["deterministic"]  = true;
                r["dt"]             = usedDt;
                r["simulatedSec"]   = usedDt * static_cast<f32>(m_mcpStepFramesRequested);
                r["held"]           = m_mcpStepHold;
                if (m_mcpStepHold)
                    r["note"] = "時間を止めた（次の step_frames まで進まない）。"
                                "hold:false で止めずに走らせ続けられる";
            }
            // 相乗りしているツール（ui_click 等）の結果を混ぜる。
            if (m_mcpStepExtra.is_object())
                for (auto it = m_mcpStepExtra.begin(); it != m_mcpStepExtra.end(); ++it)
                    r[it.key()] = it.value();
            m_mcpStepExtra = nlohmann::json();
            CompleteMcp(m_mcpBridge.get(), m_mcpStepReply, std::move(r));
            m_mcpStepReply = {};
        }

        // フレームレートリミッター（VSync OFF時のCPU暴走を防止。上限はオプション画面から変更可能）
        // ★--background は VSync を使わない。見えていない窓（画面外/最小化/最背面）への Present は
        //   DWM がフレームを消費せず即座に返る＝リミッターが無いと CPU/GPU を回し続ける。
        //   なので裏の窓は常に「VSync 無し + 60fps 上限」（設定は書き換えない。既存の上限がもっと低ければそちら）。
        // ★例外: dx12_benchmark {uncap:true} の計測中だけは裏の窓でも歩調を取らない（VG P4）。
        //   uncap は「FPS 上限 / VSync を外して真のスループットを測る」ための引数なのに、裏の窓の 60fps 上限が
        //   残っていて、--background のエンジンでは何を測っても fps ≈ 59 になっていた（GPU 2 ms のシーンでも）。
        //   計測は最大 3600 フレームで終わり、終了時に m_benchRestore が戻る＝回しっぱなしにはならない。
        const bool  benchUncapped = m_benchRestore && m_benchFramesLeft > 0;
        const bool  bgPace     = m_bgOptions.Active() && !benchUncapped;
        const f32   paceLimit  = bgPace ? (m_fpsLimit > 0.0f ? (std::min)(m_fpsLimit, 60.0f) : 60.0f)
                                        : m_fpsLimit;
        if ((!m_useVsync || bgPace) && paceLimit > 0.0f)
        {
            using namespace std::chrono;
            auto targetDuration = duration_cast<high_resolution_clock::duration>(
                duration<f64>(1.0 / static_cast<f64>(paceLimit)));
            auto elapsed = high_resolution_clock::now() - m_frameStart;
            auto remaining = targetDuration - elapsed;

            // ★高分解能の待機可能タイマーで寝る。
            //   旧実装は「1ms 残るまで Sleep → 残りは _mm_pause() でスピン」だった。
            //   スピンは 1 コアを 100% 回し続けるだけで何も進めない。ノートでは
            //   その発熱がクロック低下になって返ってくるので、待つなら本当に寝る。
            //   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION は Win10 1803+ で ~0.5ms 精度。
            //   古い OS では作成に失敗するので、その時だけ従来の Sleep+スピンへ落ちる。
            static HANDLE s_frameTimer = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (s_frameTimer && remaining > microseconds(0))
            {
                // 100ns 単位・負値で「相対時間」を表す（Win32 の作法）。
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(
                    duration_cast<nanoseconds>(remaining).count() / 100);
                if (due.QuadPart < 0
                    && SetWaitableTimerEx(s_frameTimer, &due, 0, nullptr, nullptr, nullptr, 0))
                {
                    WaitForSingleObject(s_frameTimer, INFINITE);
                }
            }
            else
            {
                if (remaining > milliseconds(1))
                    std::this_thread::sleep_for(remaining - milliseconds(1));
                while (high_resolution_clock::now() - m_frameStart < targetDuration)
                    _mm_pause();
            }
        }

        // フレーム境界。FPS リミッター/VSync 待ちの「後」に打つので、Tracy が出す
        // フレーム時間が実際に画面へ出ているレートと一致する。
        DX12_PROFILE_FRAME();
    }

    timeEndPeriod(1);

    Logger::Info("Main loop ended");
}

float Application::GetUiScale() const
{
    return (m_imguiManager && !m_isGameMode) ? m_imguiManager->GetUiScale() : 1.0f;
}

void Application::SetUiScaleOverride(float scale)
{
    if (m_isGameMode) return;   // 配布ゲームは倍率を持たない
    // ★UI テストのコルーチンは別スレッドから呼ぶ。SetWindowPos はメインスレッドへの同期メッセージになり、
    //   メインスレッドはコルーチンの完了待ちでブロックしているのでデッドロックする。要求を置くだけにして、
    //   実際の窓/ImGui の切替はメインスレッドのフレーム先頭（Run の ProcessMessages 直後）で行う。
    m_uiScaleRequest.store(scale > 0.0f ? scale : 0.0f);
}

u32 Application::GetClientWidthPx() const  { return m_window ? m_window->GetWidth()  : 0; }
u32 Application::GetClientHeightPx() const { return m_window ? m_window->GetHeight() : 0; }

void Application::Shutdown()
{
    Logger::Info("Application shutting down...");

    // MCP ブリッジを最優先で停止(worker を join)。これより後で Logger/scene/scriptengine を
    // 破棄するので、ここで止めないと worker がそれらを破棄後に触って data race/UAF になる。
    if (m_mcpBridge) m_mcpBridge.reset();

    // ネットワーク接続を明示的に切る（ENetのソケット/ホストをデバイス解放より前に片付ける）。
    if (m_networkSystem) { m_networkSystem->Disconnect(); m_networkSystem.reset(); }

    // ゲームビルドの裏ジョブ（走っていれば止めて回収。join 前に破棄すると std::terminate）。
    JoinBuildJob();

    // バックグラウンドの BC 圧縮先読みを止める（ResourceManager/Logger より先に）。
    if (m_assetPrewarmer) m_assetPrewarmer.reset();

    // 非同期ロードスレッドの回収
    if (m_loadThread.joinable())
        m_loadThread.join();

    // 非同期 git スレッドの回収（join 前に破棄すると std::terminate）。
    // ログイン待ちポーリングは abort で即抜けさせ、git/gh の子プロセスは終わるまで待つ。
    m_gitAbort.store(true);
    if (m_gitThread.joinable())
        m_gitThread.join();
    StopGitWatcher();   // 変更一覧の自動追従スレッド（起動していなければ何もしない）

    // GPU の処理完了を待機
    if (m_commandQueue)
    {
        m_commandQueue->WaitIdle();
    }

    // 遅延解放を止めて溜まっている分を全解放（GPU完全停止済みなので安全）。
    // 以後の reset() 群は即時解放に戻る（デバイス解放前に確実に消えるように）
    DeferredRelease::Disable();
    DeferredRelease::FlushAll();

    // ImGui 解放
    // UI テストエンジンは ImGui コンテキスト破棄より先に止める
    if (m_uiTests)
    {
        m_uiTests->Shutdown();
        m_uiTests.reset();
    }

    if (m_imguiManager)
    {
        m_imguiManager->Shutdown();
        m_imguiManager.reset();
    }

    // リソース解放（逆順）
    m_editorLayer.reset();   // MaterialPreviewRenderer への生ポインタ保持者 → パネルより先に破棄
    // マテリアルエディタ/ライブラリ（プレビュー用 Mesh/RT 等の GPU リソース保持）を
    // デバイス解放より前に明示破棄。Shutdown で消さないと ~Application のメンバー破棄まで
    // 生き残り、破棄済み D3D12MA アロケータへ Release してクラッシュする（dx12_crash.log の
    // MaterialPreviewRenderer → DeferredRelease::Defer 落ち）。
    m_materialEditorPanel.reset();
    m_materialLibraryPanel.reset();
    MaterialGraphPanel::ShutdownGpu();   // マテリアルグラフ窓のプレビュー / サムネイル（GraphMaterialSystem のインスタンスを返してから）
    m_graphMaterials.reset();   // ワーカーを止めて PSO / プールを解放（デバイス解放より前）
    m_materialAssetManager.reset();
    // 地形レイヤー配列とスプラットテクスチャ（GPU リソース）もデバイス解放より前に明示破棄する。
    m_terrainSrvCache.clear();
    m_terrainLayerSets.reset();
    SceneSerializer::SetSplitAdviceHook(nullptr, nullptr);   // m_editorCtx を指しているので先に外す
    m_editorCtx.reset();
    m_physicsDebugRenderer.reset();
    // 新規レンダラ群（GPU リソース）をデバイス解放より前に明示破棄
    m_editorIconRenderer.reset();
    m_sceneTransition.reset();
    m_uiSystem.reset();       // retained UI（GPU リソース非保持だが解放順を明確化）
    m_uiAnimRuntime.reset();
    m_spriteRenderer.reset();
    m_postProcess.reset();
    m_bloomPass.reset();      // GPU リソース（チェーンRT/PSO）をデバイス解放より前に明示破棄
    m_autoExposure.reset();   // 同上（UAV バッファ/compute PSO）
    m_godRaysPass.reset();
    m_lensFlarePass.reset();
    m_dofPass.reset();
    m_motionBlurPass.reset();
    m_distortRT.reset();
    m_screenShaderRT.reset();
    m_screenShaderPass.reset();
    m_gpuParticles.reset();
    // SSAO / コンタクトシャドウ（GPU リソース）をデバイス解放より前に明示破棄
    m_ssaoPass.reset();
    // Hi-Z はディスクリプタブロックを持っているので、ヒープより先に明示的に返す。
    if (m_hiZPass) m_hiZPass->Shutdown();
    m_hiZPass.reset();
    if (m_occlusionCull) m_occlusionCull->Shutdown();
    m_occlusionCull.reset();
    PathTracerShutdown();        // DXR パストレーサー（累積バッファ / 専用 TLAS。実行中のジョブがあればここで GPU 完了を待って落とす）
    ShutdownVirtualGeometry();   // 仮想ジオメトリ P2（専用ヒープ / COPY キュー / m_vgHiZ のディスクリプタ）。ヒープより先に返す
    ShutdownFoliage();           // 植生 F1（compute / 間接描画バッファ。デバイス解放より前に）
    ShutdownGpuInst();           // GPU 駆動のインスタンス群（4-3。compute / 永続バッファ。デバイス解放より前に）
    ShutdownWater();             // 水面 W1（コピー用テクスチャ / ディスクリプタ。デバイス解放より前に）
    m_perceptionPass.reset();   // 知覚層（RT / 読み戻し / 専用ヒープ）。要求が無ければ最初から null
    m_contactShadowPass.reset();
    m_taaPass.reset();
    m_gbufferRT.reset();
    m_screenSpaceGi.reset();
    // DXR（加速構造 + RT パス）。デバイス解放より前に確実に落とす。
    m_rtScreenPass.reset();
    if (m_rtScene) m_rtScene->Shutdown();
    m_rtScene.reset();
    m_ssaoWhiteTex.reset();
    m_ssBlackTex.reset();
    m_depthPrepassPSO.reset();
    m_depthPrepassSkinnedPSO.reset();
    m_velocityPSO.reset();
    m_velocityPSOInst.reset();
    m_velocityPSOSkinned.reset();
    // IBL / Skybox（GPU リソース）をデバイス解放より前に明示破棄。SRV index も srvHeap 生存中に返却。
    m_skyboxRenderer.reset();
    AtmosphereShutdown();   // 物理ベース大気 A1（LUT・ディスクリプタを srvHeap 生存中に返す。確保していなければ何もしない）
    if (m_iblBaker)
    {
        if (m_iblReady && m_srvHeap)
            m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
        if (m_srvHeap) m_iblBaker->FreeRebakeDescriptors(*m_srvHeap);
        m_iblBaker->Reset();
        m_iblBaker.reset();
    }
    if (m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex && m_srvHeap)
    {
        m_srvHeap->Free(m_envCubeSrvIndex);
        m_envCubeSrvIndex = DescriptorHeap::kInvalidIndex;
    }
    m_envCubeTex.reset();
    m_cameraPreviewLdrRT.reset();
    m_cameraPreviewRT.reset();
    m_sceneRT.reset();
    m_offscreenRtvHeap.reset();
    // クラスタードライティング: SRV ブロックを返してから（m_srvHeap の破棄より前に）解放する。
    if (m_clusteredLighting)
    {
        m_clusteredLighting->Shutdown();
        m_clusteredLighting.reset();
    }
    // ボリュメトリックフォグも SRV/UAV ブロックを持っている（同上）。
    if (m_volumetricFogPass)
    {
        m_volumetricFogPass->Shutdown();
        m_volumetricFogPass.reset();
    }
    // デカールは自前のディスクリプタを持たない（クラスタのブロックを借りるだけ）が、
    // UPLOAD バッファの Unmap があるので明示的に落とす。
    if (m_decalSystem)
    {
        m_decalSystem->Shutdown();
        m_decalSystem.reset();
    }
    m_sceneFlow.reset();
    if (m_physicsSystem)
    {
        m_physicsSystem->SetEventBus(nullptr);  // EventBus 破棄より前に参照を切る
        m_physicsSystem->Shutdown();
        m_physicsSystem.reset();
    }
    // Window は InputSystem を生ポインタで持つ。m_window.reset()（= DestroyWindow）は
    // WM_KILLFOCUS を投げ、WndProc がそのポインタ経由で OnFocusLost() を呼ぶため、
    // 参照を切らずに InputSystem を解放すると解放済みメモリへの書き込みで落ちる。
    if (m_window)
        m_window->SetInputSystem(nullptr);
    m_inputSystem.reset();
    // UITweenState には Lua の onComplete クロージャが入っていることがある（tweenUi）。
    // sol::function の unref が生きた lua_State を要求するため、Lua ステート破棄
    // （m_scriptEngine.reset）より先に必ず破棄する（m_scene.reset は後ろのため）
    // ★同じ理由の後始末は ScriptEngine::Shutdown 側にもある（そちらは LuaScript の
    //   env/self も落とす＝Play 停止やシーン切替の経路もまとめて守る本体）。
    //   ここは「破棄順の危険がこの並びにある」という印として残してある。
    if (m_scene)
    {
        auto& reg = m_scene->GetRegistry();
        auto twView = reg.view<UITweenState>();
        reg.remove<UITweenState>(twView.begin(), twView.end());
    }
    m_scriptEngine.reset();
    m_audioSystem.reset();
    m_shadowSkinnedPipelineState.reset();
    m_shadowPipelineState.reset();
    m_shadowMap.Reset();
    m_shadowDsvHeap.reset();
    m_gridPipelineState.reset();
    m_terrainPipelineStateLEqual.reset();
    m_terrainPipelineState.reset();
    m_skinnedPipelineStateLEqual.reset();
    m_skinnedPipelineState.reset();
    m_scene.reset();
    m_commandList.reset();
    m_perFrameCB.reset();
    m_previewFrameCB.reset();
    m_resourceManager.reset();
    ShaderManager::SetInstance(nullptr);
    m_shaderManager.reset();
    m_srvHeap.reset();
    m_camera.reset();
    m_pipelineStateThumb.reset();
    m_pipelineStateLEqual.reset();
    m_pipelineState.reset();
    m_rootSignature.reset();
    m_depthBuffer.Reset();
    m_dsvHeap.reset();
    m_frameResources.reset();
    m_swapChain.reset();
    m_descriptorHeap.reset();
    m_commandQueue.reset();
    m_graphicsDevice.reset();
    m_window.reset();

    m_isRunning = false;

    Logger::Info("Application shut down complete");
    Logger::Shutdown();
}

void Application::Update()
{
    using namespace DirectX;
    f32 dt = m_gameClock.GetDeltaTime();

    m_framesSinceStart++;

    // 連番アニメの再生位置を進める（Sprite2D / UIImage / MeshRenderer 共通。ここ1箇所だけで
    // 加算する = 描画側が複数回走っても二重に進まない。エディタ中もプレビュー再生する）。
    if (m_scene)
    {
        auto& animReg = m_scene->GetRegistry();
        for (auto [e, sp] : animReg.view<Sprite2D>().each())
            if (sp.animFrames > 0) sp._animT += dt;
        for (auto [e, img] : animReg.view<UIImage>().each())
            if (img.animFrames > 0) img._animT += dt;
        for (auto [e, mr] : animReg.view<MeshRenderer>().each())
            if (mr.animFrames > 0 || mr.uvScrollU != 0.0f || mr.uvScrollV != 0.0f)
                mr._animT += dt;

        // タイムライン製 UI アニメ / スプライトシートも同じ場所で進める。
        // 発火イベントは EventBus へ即時 Emit する（Lua の OnUpdate より前 = クリック配信と同じ規律）。
        if (m_uiAnimRuntime)
        {
            // イベントは滅多に出ないので、空 vector はヒープを触らない = ローカルで十分
            std::vector<UiAnimRuntime::PendingEvent> animEvents;
            { DX12_PROFILE_ZONE_N("UiAnim");
              m_uiAnimRuntime->Update(animReg, dt, animEvents); }
            for (const auto& pe : animEvents)
            {
                EngineEvent ev;
                ev.name   = pe.name;
                ev.source = pe.source;
                m_eventBus.Emit(ev);
            }
        }
    }

    // スケルタルアニメのクリップイベント（.animfsm の clipEvents。足音等）を EventBus へ。
    // Scene::Update が積んだものをここで流し切る（Lua は events:on("footstep", fn) で受ける）。
    if (m_scene)
    {
        auto& animEvents = m_scene->GetPendingAnimEvents();
        for (const auto& ae : animEvents)
        {
            EngineEvent ev;
            ev.name   = ae.name;
            ev.source = ae.entity;
            if (!ae.stringParam.empty()) ev.set("string", ae.stringParam);
            ev.set("float", static_cast<double>(ae.floatParam));
            ev.set("clip",  ae.clip);
            ev.set("layer", static_cast<double>(ae.layer));
            ev.set("time",  static_cast<double>(ae.time));
            m_eventBus.Emit(ev);
        }
        animEvents.clear();
    }

    // 非同期プロジェクトロードの状態機械を進める
    UpdateProjectLoad(dt);
    // ランチャー用サムネイル（保存 / 閉じる / 初回）
    UpdateProjectThumbnail(dt);
    // 非同期 git 操作の完了回収
    UpdateGitOp();

    // 「テストクライアント起動」ボタン(フェーズ⑨): フレーム境界で別プロセスを起動
    if (m_editorCtx->netTestLaunchClientRequested)
    {
        m_editorCtx->netTestLaunchClientRequested = false;
        LaunchNetTestClient();
    }

    // ===== Play 中の一時停止（F1）=====
    // ★キーが本命でツールバーのボタンはおまけ。Play 中はゲームがマウスを
    //   キャプチャしていてボタンを押せないことが多いため。
    // ★配布ランタイム(m_isGameMode)では効かせない。デバッグ専用の機能。
    if (!m_isGameMode && m_engineMode == EngineMode::Playing
        && m_inputSystem && m_inputSystem->IsKeyPressed(VK_F1))
    {
        m_editorCtx->paused = !m_editorCtx->paused;
        // ★Logger::Info は consteval な書式文字列を取るので三項演算子は渡せない。
        if (m_editorCtx->paused) Logger::Info("一時停止（シーンビューを操作できます。F1 で再開）");
        else                     Logger::Info("再開");
    }
    // 一時停止中は「Playing だが時間は進めない」。ゲーム側の更新を全部止め、
    // カメラ操作はエディタ側（下の Editor 分岐）へ返す。
    const bool paused     = (!m_isGameMode && m_engineMode == EngineMode::Playing
                             && m_editorCtx->paused);
    const bool simRunning = (m_engineMode == EngineMode::Playing) && !paused;

    // 一時停止の切り替わりでマウスキャプチャを退避／復元する（下の Editor 分岐が
    // 「右ドラッグしていない＝解除」を毎フレームやるので、覚えておかないと戻せない）。
    if (paused != m_prevPaused)
    {
        if (paused)
            m_pausedMouseCapture = (m_inputSystem && m_inputSystem->IsMouseCaptured());
        else if (m_inputSystem && m_pausedMouseCapture)
            m_inputSystem->SetMouseCapture(true);
        m_prevPaused = paused;
    }

    // ===== エディタのコマンド（ショートカット / メニュー / パレット）が立てた要求の消化 =====
    // 実処理を持つのは Application だけのもの（保存・カメラのフォーカス・全画面）。
    // ショートカットの判定は EditorLayer::Render の cmd::ProcessShortcuts（editor/EditorCommandTable.h）。
    if (m_editorCtx && !m_isGameMode)
    {
        // Application の前面判定を共有する（ショートカットを別アプリ作業中に効かせない）。
        // 仮想入力モードでは「前面にいるか」を見ない（入力は全部 AI の仮想入力で、人の実入力は遮断済み）。
        m_editorCtx->appForeground = vinput::Enabled() || (GetForegroundWindow() == m_window->GetHwnd());
        m_editorCtx->mouseCaptured = m_inputSystem && m_inputSystem->IsMouseCaptured();

        if (m_editorCtx->pendingSaveScene)
        {
            m_editorCtx->pendingSaveScene = false;
            EditorSaveScene();
        }

        if (m_editorCtx->pendingToggleFullscreen)
        {
            m_editorCtx->pendingToggleFullscreen = false;
            m_window->ToggleFullscreen();
        }

        // 選択へカメラを寄せる（F / パレットのジャンプ）。Editor モード（と一時停止中）でだけ動かす。
        if (m_editorCtx->pendingFocusSelection)
        {
            m_editorCtx->pendingFocusSelection = false;
            if ((m_engineMode == EngineMode::Editor || m_editorCtx->paused) && m_editorCtx->HasSelection())
                FocusEditorCameraOnSelection();
        }
    }

    // ★paused のときは Editor 分岐へ入れる。これだけで
    //   「Lua を回さない・ゲームカメラの同期をしない・エディタのフライカメラが効く」が
    //   まとめて成立する（Lua もカメラ同期も下の else 側にあるため）。
    if (m_engineMode == EngineMode::Editor || paused)
    {
        // エディタモード: C++カメラ操作
        bool rightMouseHeld = m_inputSystem->IsAsyncKeyDown(VK_RBUTTON);

        // ウィンドウが非フォーカスならカメラ操作しない
        // ★仮想入力モードでは「前面にいるか」を見ない。入力は全部 AI の仮想入力で、人の実入力は
        //   遮断済み。裏（画面外/最小化）の窓でも右ドラッグのフライが効くようにする。
        bool isForeground = vinput::Enabled() || (GetForegroundWindow() == m_window->GetHwnd());

        // 仮想入力: 仮想ポインタの移動量を「マウスの移動量」として次フレームの視点回転へ渡す
        // （実マウスの Raw Input は遮断しているので、他に移動量の入口が無い）。
        // ★毎フレーム読んで捨てる（キャプチャしていない間に溜まった量が後で一気に効かないように）。
        if (vinput::Enabled())
        {
            float vdx = 0.0f, vdy = 0.0f;
            vinput::Global().TakePointerDelta(vdx, vdy);
            if (m_inputSystem->IsMouseCaptured() && (vdx != 0.0f || vdy != 0.0f))
                m_inputSystem->InjectMouseDelta(vdx, vdy);
        }

        // カーソルが 3D ビューポート上にあるか（ImGui の PassthruCentralNode の
        // WantCaptureMouse 挙動に依存せず、中央ノード矩形で直接判定）。
        // ※ パネル上で右ドラッグしてもフライが暴発しないようにするためのゲート。
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool cursorInViewport = m_editorCtx->IsCursorInViewport(mousePos.x, mousePos.y);

        if (m_framesSinceStart > 5 && rightMouseHeld && !m_inputSystem->IsMouseCaptured()
            && isForeground && cursorInViewport)
        {
            m_inputSystem->SetMouseCapture(true);
        }
        else if (!rightMouseHeld && m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(false);
        }
        // ウィンドウが裏に行ったら強制解除
        if (!isForeground && m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(false);
        }
        if (m_inputSystem->IsMouseCaptured() && !m_editorCtx->view2D)   // 2D中は視点回転/フライ無効
        {
            f32 sensitivity = m_camera->GetMouseSensitivity();
            m_camera->Rotate(
                m_inputSystem->GetMouseDeltaX() * sensitivity,
                -m_inputSystem->GetMouseDeltaY() * sensitivity);

            f32 speed = m_camera->GetMoveSpeed() * dt;
            if (m_inputSystem->IsAsyncKeyDown('W')) m_camera->MoveForward(speed);
            if (m_inputSystem->IsAsyncKeyDown('S')) m_camera->MoveForward(-speed);
            if (m_inputSystem->IsAsyncKeyDown('D')) m_camera->MoveRight(speed);
            if (m_inputSystem->IsAsyncKeyDown('A')) m_camera->MoveRight(-speed);
            if (m_inputSystem->IsAsyncKeyDown(VK_SPACE)) m_camera->MoveUp(speed);
            if (m_inputSystem->IsAsyncKeyDown(VK_SHIFT)) m_camera->MoveUp(-speed);
        }

        // 2D中の右ドラッグ: 回転せずパン（マウスユーザー向け。中ドラッグと同じ操作感）。
        // 回転は 0 固定なので MoveRight/MoveUp はワールド X/Y 平行移動になる。
        if (m_inputSystem->IsMouseCaptured() && m_editorCtx->view2D)
        {
            f32 worldPerPixel = (2.0f * m_editorCtx->view2DZoom)
                              / (std::max)(1.0f, m_editorCtx->viewportH);
            m_camera->MoveRight(-m_inputSystem->GetMouseDeltaX() * worldPerPixel);
            m_camera->MoveUp(m_inputSystem->GetMouseDeltaY() * worldPerPixel);
        }

        // --- タッチパッド向け: キーボードフライモード（マウス/ボタン長押し不要）---
        // GetAsyncKeyState はフォーカスに関係なく物理キー状態を読むため、ウィンドウが前面に
        // いる時だけ有効化する（別アプリ作業中の ` / Ctrl+Z / WASD などがエディタに効くのを防ぐ）。
        // （` キーでのトグルと Esc での解除は cmd::ProcessShortcuts の view.flyMode / edit.selectNone へ移した）
        bool kbActive = isForeground && !ImGui::GetIO().WantCaptureKeyboard;  // 非フォーカス/テキスト入力中は無効

        if (m_editorCtx->flyMode && kbActive && !m_inputSystem->IsMouseCaptured()
            && !m_editorCtx->view2D)   // 2D中はフライ無効（パン/ズームのみ）
        {
            f32 speed = m_camera->GetMoveSpeed() * dt;
            if (m_inputSystem->IsAsyncKeyDown('W')) m_camera->MoveForward(speed);
            if (m_inputSystem->IsAsyncKeyDown('S')) m_camera->MoveForward(-speed);
            if (m_inputSystem->IsAsyncKeyDown('D')) m_camera->MoveRight(speed);
            if (m_inputSystem->IsAsyncKeyDown('A')) m_camera->MoveRight(-speed);
            if (m_inputSystem->IsAsyncKeyDown('E')) m_camera->MoveUp(speed);
            if (m_inputSystem->IsAsyncKeyDown('Q')) m_camera->MoveUp(-speed);
            if (m_inputSystem->IsAsyncKeyDown(VK_SPACE)) m_camera->MoveUp(speed);
            if (m_inputSystem->IsAsyncKeyDown(VK_SHIFT)) m_camera->MoveUp(-speed);

            // 矢印キーで視点回転（マウス不要）
            f32 rot = 1.5f * dt;  // rad/sec
            f32 yawD = 0.0f, pitchD = 0.0f;
            if (m_inputSystem->IsAsyncKeyDown(VK_LEFT)) yawD   -= rot;
            if (m_inputSystem->IsAsyncKeyDown(VK_RIGHT)) yawD   += rot;
            if (m_inputSystem->IsAsyncKeyDown(VK_UP)) pitchD += rot;
            if (m_inputSystem->IsAsyncKeyDown(VK_DOWN)) pitchD -= rot;
            if (yawD != 0.0f || pitchD != 0.0f) m_camera->Rotate(yawD, pitchD);
        }

        // --- 2D ビューモード: WASD / 矢印キーでパン（3D の WASD 移動と同じ操作感で左右上下に動かせる）---
        // 2D は回転 0 固定なので MoveRight/MoveUp はそのままワールド X/Y のパンになる。速度はズーム量に比例。
        // ※ W/E/R のギズモ切替は 2D 中は下のブロックで抑止し、ここでは移動だけにする。
        // 右クリック保持中（マウスキャプチャ中）でも A/D・矢印で動かせるよう capture ゲートは付けない。
        if (m_editorCtx->view2D && kbActive)
        {
            f32 pan = (std::max)(0.5f, m_editorCtx->view2DZoom) * 1.5f * dt;
            if ((m_inputSystem->IsAsyncKeyDown('D')) || (m_inputSystem->IsAsyncKeyDown(VK_RIGHT))) m_camera->MoveRight(pan);
            if ((m_inputSystem->IsAsyncKeyDown('A')) || (m_inputSystem->IsAsyncKeyDown(VK_LEFT))) m_camera->MoveRight(-pan);
            if ((m_inputSystem->IsAsyncKeyDown('W')) || (m_inputSystem->IsAsyncKeyDown(VK_UP))) m_camera->MoveUp(pan);
            if ((m_inputSystem->IsAsyncKeyDown('S')) || (m_inputSystem->IsAsyncKeyDown(VK_DOWN))) m_camera->MoveUp(-pan);
        }

        // ★エディタのショートカット（Ctrl+S/N/O/Z/Y/C/V/D、W/E/R/T、F、F2、Del、Esc、F5 …）は
        //   EditorLayer::Render → cmd::ProcessShortcuts（editor/EditorCommandTable.h が唯一の表）へ移した。
        //   ここに手書きで散っていた処理は、メニュー・ヘルプの表記と食い違って
        //   「Ctrl+O / Ctrl+L と書いてあるのに効かない」を生んでいた。
        //   実行結果は EditorContext の pending* フラグで受け取り、上の共通ブロックが消化する。

        // シーケンサー: エディタのスクラブ / プレビュー再生（Play 中の一時停止も此処を通る）。m_scene->Update の前
        UpdateSequencers(dt, paused);
    }
    else
    {
        // ネットワーク受信/接続処理（スクリプト実行より前 — このフレームのシムに反映するため）。
        if (m_networkSystem) m_networkSystem->PreSimUpdate(dt, m_scene->GetRegistry());

        // プレイモード: Luaがカメラ+ゲームロジックを制御
        // HUD は実際のゲームビューポート基準でレイアウトさせる。
        // 単体ゲーム=全画面、エディタ Play=中央 16:9 矩形（前フレームの値で1フレーム遅延だが無視できる）。
        if (m_isGameMode)
        {
            m_scriptEngine->SetScreenSize(static_cast<int>(m_window->GetWidth()),
                                          static_cast<int>(m_window->GetHeight()));
        }
        else
        {
            auto vs = m_editorLayer->GetViewportSize();
            int sw = (vs.x >= 1.0f) ? static_cast<int>(vs.x) : static_cast<int>(m_window->GetWidth());
            int sh = (vs.y >= 1.0f) ? static_cast<int>(vs.y) : static_cast<int>(m_window->GetHeight());
            m_scriptEngine->SetScreenSize(sw, sh);
        }
        // 前フレームの Render で確定した retained UI ボタンのクリックを Lua OnUpdate より
        // 前に配信する（events:on ハンドラで書いた状態を同フレームの OnUpdate が参照できる）。
        if (m_uiSystem)
            m_uiSystem->DispatchPendingClicks(m_scene->GetRegistry(), m_eventBus);

        { DX12_PROFILE_ZONE_N("Lua/OnUpdate");   m_scriptEngine->CallOnUpdate(dt); }
        { DX12_PROFILE_ZONE_N("Lua/Components"); m_scriptEngine->UpdateAttachedScripts(dt); }
        { DX12_PROFILE_ZONE_N("Lua/Triggers");   m_scriptEngine->UpdateTriggers(dt); }   // Trigger（イベント）評価
        // ゲーム AI（知覚 → 行動の選択 → Lua の行動 → 群衆の移動）。Lua の OnUpdate の後に回すので、
        // スクリプトがこのフレームに書いた黒板の値を同じフレームの判断で使える。
        // dt はスクリプトと同じ（タイムスケール適用済み）。内部は 1/60 秒の固定ステップ。
        if (m_aiSystem)
        {
            DX12_PROFILE_ZONE_N("AI");
            m_aiSystem->Update(*m_scene, m_physicsSystem.get(), &m_eventBus, dt * m_scriptEngine->GetTimeScale());
        }

        // シーケンサー: Play 中の再生（Sequence.play / sequence_play / シーンの自動再生）。
        // ★Lua / Trigger / AI の【後】・カメラ同期の【前】: スクリプトが同じフレームに書いた値をシーケンサーが上書きし、
        //   その結果（カットのカメラ・シェイク）を下のカメラ同期が拾う。Scene::Update（Animator）はその後。
        UpdateSequencers(dt, /*paused=*/false);

        // アクティブカメラの Transform をグローバル Camera に同期。
        // 親階層込みのワールド変換で反映するので、親オブジェクトにアタッチした
        // カメラが親の移動・回転に追従する。
        // ★MCP が dx12_set_editor_camera で視点を固定している間は同期しない（#20-6）。
        //   Play 中の絵で look_compare / camera_path を回すための一時上書き。
        // ★シーケンサーのカット（FindActiveCameraEntity）があれば isActive よりカットのカメラを優先する。
        if (!m_mcpCameraOverride)
        {
            const entt::entity activeCam = FindActiveCameraEntity();
            if (activeCam != entt::null) ApplyCameraTransformToGlobal(activeCam);
        }
    }

    // シーン更新（Animator等）— エディタモードは時間を止める（ボーン行列は維持）
    { DX12_PROFILE_ZONE_N("Scene/Animators");
      m_scene->Update(simRunning ? dt : 0.0f); }

    // 物理ベース大気 A1: 時刻 → 太陽の向き・色・強度（enabled=false なら遷移の後始末だけで何も起きない）。
    // 時間経過は Play 中だけ・決定論撮影では止める。平行光の書き込みは描画（PrepareFrame）より前に済ませる。
    UpdateAtmosphereTime((simRunning && !m_deterministicCapture) ? dt : 0.0f);

    // 配置パーティクル放出器（ParticleEmitter）を駆動。
    // エディタでも常時プレビュー（実 dt で放出/前進）し、Play では _active に従う。
    if (m_particleSystem && !paused)   // 一時停止中は放出も止める（止めないと同じ位置に溜まる）
    {
        auto& peReg = m_scene->GetRegistry();
        const bool pedPlaying = (m_engineMode == EngineMode::Playing);
        auto peView = peReg.view<ParticleEmitter, Transform>();
        for (auto pe_e : peView)
        {
            auto& emitter = peView.get<ParticleEmitter>(pe_e);
            // ワールド行列はエンティティにつき 1 回だけ。レイヤーごとに取り直すと
            // 親子を辿る walk がレイヤー数ぶん走る（松明を 100 本置くと効いてくる）。
            const DirectX::XMMATRIX w = ComputeWorldMatrix(peReg, pe_e);

            // ★レイヤーを 1 枚ずつ回す。1 エンティティに炎+煙+火の粉、が成立するのはここ。
            for (ParticleLayer& pe : emitter.layers)
            {
            const bool live = pedPlaying ? pe._active : true;  // エディタは常時プレビュー
            if (!live) continue;
            if (!pe.looping && pedPlaying)
            {
                pe._age += dt;
                if (pe._age >= pe.duration) pe._active = false;
            }
            if (pe.rate <= 0.0f) continue;
            pe._emitAccum += pe.rate * dt;
            int n = static_cast<int>(pe._emitAccum);
            if (n <= 0) continue;
            pe._emitAccum -= static_cast<f32>(n);
            const int nCap = pe.gpu ? 8192 : 64;   // GPU は大量放出を許容
            if (n > nCap) n = nCap;

            // レイヤーのローカルオフセットをワールドへ乗せる（松明の炎は先端、煙はその上）。
            DirectX::XMFLOAT3 pos;
            DirectX::XMStoreFloat3(&pos,
                DirectX::XMVector3Transform(
                    DirectX::XMVectorSet(pe.offset.x, pe.offset.y, pe.offset.z, 1.0f), w));

            // GPU パーティクル経路（compute シム。distort/light 等の CPU 専用機能は無視）
            if (pe.gpu && m_gpuParticles)
            {
                GpuParticleSystem::EmitRequest r;
                r.pos = pos;
                r.count = static_cast<u32>(n);
                r.dir = pe.dir;       r.spread = pe.spread;
                r.col0 = { pe.color.x * pe.intensity, pe.color.y * pe.intensity, pe.color.z * pe.intensity };
                r.speed = pe.speed;
                r.col1 = { pe.colorEnd.x * pe.intensity, pe.colorEnd.y * pe.intensity, pe.colorEnd.z * pe.intensity };
                r.speedVar = pe.speedVar;
                r.size0 = pe.size;    r.size1 = pe.sizeEnd;
                r.life = pe.life;     r.lifeVar = pe.lifeVar;
                r.gravity = pe.gravity; r.drag = pe.drag; r.up = pe.up;
                // ★turb を渡し忘れていた。フィールドもアップロードもシェーダも揃っているのに
                //   ここだけ抜けていたので、GPU パーティクルの乱流が常に 0 だった。
                r.turb = pe.turbStrength;
                r.kind = pe.kind;     r.stretch = pe.stretch;
                m_gpuParticles->Emit(r);
                continue;
            }

            ParticleSystem::EmitParams p;
            p.pos = pos;            p.count = n;
            p.dir = pe.dir;         p.spread = pe.spread;
            p.speed = pe.speed;     p.speedVar = pe.speedVar;
            p.size = pe.size;       p.sizeEnd = pe.sizeEnd;
            p.life = pe.life;       p.lifeVar = pe.lifeVar;
            p.color = pe.color;     p.colorEnd = pe.colorEnd; p.hasColorEnd = true;
            p.colorMid = pe.colorMid; p.hasColorMid = pe.hasColorMid;
            p.intensity = pe.intensity;
            p.gravity = pe.gravity; p.drag = pe.drag; p.up = pe.up;
            p.stretch = pe.stretch; p.kind = pe.kind; p.blend = pe.blend;
            p.orient = pe.orient;
            p.turbStrength = pe.turbStrength; p.turbFreq = pe.turbFreq;
            p.sizeMid = pe.sizeMid; p.distort = pe.distort;
            p.light = pe.light;     p.lightRange = pe.lightRange;
            p.flicker = pe.flicker; p.flickerFreq = pe.flickerFreq;
            p.texturePath = pe.texturePath;
            // 自作パーティクルシェーダー。未コンパイル/生成失敗なら nullptr が返り、
            // 既定の見た目で描かれる（黙って消えない）。
            if (!pe.shaderPath.empty())
            {
                if (CustomParticlePsos* cps = EnsureCustomParticlePso(pe.shaderPath))
                    p.customPso = (pe.blend != 0) ? cps->alpha.Get() : cps->additive.Get();
            }
            m_particleSystem->Emit(p);
            }   // レイヤーループ
        }

        // トレイル（軌跡リボン）: エンティティのワールド位置を毎フレーム記録
        auto trView = peReg.view<TrailRenderer, Transform>();
        for (auto tr_e : trView)
        {
            const auto& tr = trView.get<TrailRenderer>(tr_e);
            if (!tr.emitting) continue;
            DirectX::XMMATRIX w = ComputeWorldMatrix(peReg, tr_e);
            DirectX::XMFLOAT3 pos; DirectX::XMStoreFloat3(&pos, w.r[3]);

            ParticleSystem::TrailParams tp;
            tp.width     = tr.width;
            tp.color     = tr.color;
            tp.colorEnd  = tr.colorEnd;
            tp.intensity = tr.intensity;
            tp.life      = tr.life;
            tp.blend     = tr.blend;
            tp.minDist   = tr.minDist;
            m_particleSystem->TrailPoint(static_cast<u64>(tr_e), pos, tp);
        }
    }

    // パーティクル更新（配置エミッタのプレビューのため、エディタでも実 dt で前進）
    // ★決定論キャプチャ中は前進させない（#31。粒子が動くと 2 枚が一致しない）。
    if (m_particleSystem)
        { DX12_PROFILE_ZONE_N("Particles/Sim");
          m_particleSystem->Update((m_deterministicCapture || paused) ? 0.0f : dt); }

    // 物理更新（プレイモードのみ。一時停止中は止める）
    if (simRunning && m_physicsSystem->IsInitialized())
    {
        { DX12_PROFILE_ZONE_N("Physics");
          m_physicsSystem->Update(dt, m_scene->GetRegistry()); }
    }

    // ネットワーク送信処理（物理確定後の座標を使うため直後。フェーズ⑤でスナップショット送信を実装）。
    if (simRunning && m_networkSystem)
    {
        m_networkSystem->PostSimUpdate(dt, m_scene->GetRegistry());
    }

    // 3D 空間オーディオ: リスナー＝カメラ、AudioSource を駆動（Playing のみ）
    // ★一時停止中は動かさない。動かすとリスナーがエディタのフライカメラに付いて
    //   音場が飛び回る（BGM は XAudio2 側で鳴り続けるので途切れない）。
    if (simRunning && m_audioSystem)
    {
        auto pos = m_camera->GetPosition();
        auto fwd = m_camera->GetForward();
        m_audioSystem->SetListener(pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z, 0.0f, 1.0f, 0.0f);

        auto& reg = m_scene->GetRegistry();
        for (auto [e, src] : reg.view<AudioSource>().each())
        {
            DirectX::XMFLOAT4X4 wf;
            DirectX::XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, e));
            const float wx = wf._41, wy = wf._42, wz = wf._43;

            if (src.playOnStart && !src.startedThisPlay && !src.clipPath.empty())
            {
                if (src.spatial)
                    src.runtimeSlot = m_audioSystem->PlaySFXSpatial(
                        src.clipPath, wx, wy, wz, src.minDistance, src.maxDistance, src.volume, src.loop,
                        src.bus, src.priority);
                else
                    src.runtimeSlot = m_audioSystem->PlaySFXTracked(src.clipPath, src.loop, src.volume,
                                                                    src.bus, src.priority);
                src.startedThisPlay = true;
            }
            if (src.runtimeSlot >= 0 && src.spatial)
            {
                m_audioSystem->UpdateSpatialEmitter(src.runtimeSlot, wx, wy, wz);

                // 壁越しはこもらせる。エミッタ→リスナーへレイを 1 本飛ばし、
                // 途中で何かに当たったら遮蔽 1.0（平滑は AudioSystem 側）。
                // ponytail: レイ 1 本だけ。半遮蔽も回折も見ない。要るなら本数を増やす
                float occ = 0.0f;
                if (m_physicsSystem->IsInitialized())
                {
                    float lx, ly, lz;
                    m_audioSystem->GetListenerPos(lx, ly, lz);
                    const DirectX::XMFLOAT3 dir{lx - wx, ly - wy, lz - wz};
                    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                    // ★終端を少し削る。削らないとリスナーを包んでいる
                    //   プレイヤーのコライダーに当たって常時こもる
                    if (len > 1.0f &&
                        m_physicsSystem->Raycast({wx, wy, wz}, dir, len - 0.6f, 0xFFFFFFFFu, entt::null, /*includeCharacters=*/false).hit)
                        occ = 1.0f;
                }
                m_audioSystem->SetOcclusion(src.runtimeSlot, occ);
            }
        }
        // リバーブ域: リスナー位置をゾーンのローカル空間へ写して重みを出し、AudioSystem へ渡す。
        // 混ぜ方（優先度・時間方向の平滑）は AudioSystem 側（audio::BlendZones）。
        {
            float lx, ly, lz;
            m_audioSystem->GetListenerPos(lx, ly, lz);
            std::vector<AudioSystem::ReverbZoneInput> zones;
            for (auto [e, z] : reg.view<AudioReverbZone>().each())
            {
                if (!z.enabled) continue;
                DirectX::XMVECTOR det;
                const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, ComputeWorldMatrix(reg, e));
                DirectX::XMFLOAT3 lp;
                DirectX::XMStoreFloat3(&lp, DirectX::XMVector3TransformCoord(
                                                DirectX::XMVectorSet(lx, ly, lz, 1.0f), inv));
                const float local[3] = {lp.x, lp.y, lp.z};
                const float half[3]  = {z.halfExtents.x, z.halfExtents.y, z.halfExtents.z};
                const float outside  = (z.shape == 1) ? audio::SphereOutsideDistance(local, z.radius)
                                                      : audio::BoxOutsideDistance(local, half);
                const float w = audio::ZoneWeight(outside, z.fadeDistance);
                if (w <= 0.0f) continue;
                const auto* nt = reg.try_get<NameTag>(e);
                zones.push_back({nt ? nt->name : std::string(), z.preset, w, z.wet, z.priority});
            }
            m_audioSystem->SetReverbZones(std::move(zones));
        }
        { DX12_PROFILE_ZONE_N("Audio"); m_audioSystem->Update(dt); }
    }
    // ★ボイスの終了検出・仮想⇔実の入れ替え・フェード・バスの反映はモードに関係なく毎フレーム回す
    //   （エディタのプレビュー再生・一時停止中の BGM も対象。上の Update は Play 中の定位だけ）。
    // ★dt は実時間（クランプ前）。XAudio2 の実ボイスはフレームが詰まっても実時間で鳴り進むので、
    //   0.1 秒で切った dt で仮想ボイスの位置を進めると、重いフレームの後で実ボイスへ戻したとき
    //   位置がずれる（音声デバイスが無いときの位置も実時間と合わなくなる）。
    if (m_audioSystem)
        { DX12_PROFILE_ZONE_N("Audio/Tick"); m_audioSystem->Tick(m_gameClock.GetRawDeltaTime()); }

    // Trigger の Post や接触 Post を同フレーム内で配信（Playing のみ）。
    if (simRunning)
    {
        { DX12_PROFILE_ZONE_N("Events/Flush"); m_eventBus.Flush(); }

        // プレイセッション記録。物理・スクリプト・アニメが全部確定した後に取る。
        // 記録するだけで何も操作しない（遊ぶのは人間、読むのは AI）。
        if (m_inputSystem)
            m_playSession.Update(m_gameClock.GetTotalTime(), *m_inputSystem,
                                 m_camera.get(), m_gameClock.GetFPS());
    }
}

// ---------------------------------------------------------------------------
// フット IK パス。FootIK + SkeletalAnimation を持つエンティティの足を地面に合わせる。
//
// FootIK.cpp（Animation ライブラリ）は entt も PhysicsSystem も知らない。
// ここが「エンティティを走査して PhysicsSystem::Raycast を繋ぐ」接着層。
// ---------------------------------------------------------------------------
std::string Application::ActionBindingsPath() const
{
    // プロジェクト直下。settings.json の隣に置く（あちらは double しか持てないので
    // キー割り当ては表現できず、別ファイルにするしかない）。
    return PathResolver::BaseDir() + "input_bindings.json";
}

void Application::SaveActionBindings()
{
    nlohmann::json root = nlohmann::json::object();
    for (const auto& [action, list] : m_actionMap.Bindings())
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& b : list)
            arr.push_back({{"key", b.key},
                           {"c", nlohmann::json::array({b.c.x, b.c.y, b.c.z})}});
        root[action] = std::move(arr);
    }
    const std::string path = ActionBindingsPath();
    // 原子的に書く（途中で落ちても元のキー割り当てを壊さない）
    const atomicfile::Result wr = atomicfile::WriteJson(atomicfile::PathFromUtf8(path), root, 2);
    if (!wr.ok) { Logger::Warn("キー割り当てを保存できませんでした: {} ({})", path, wr.error); return; }
    Logger::Info("キー割り当てを保存しました: {}", path);
}

void Application::LoadActionBindings()
{
    const std::string path = ActionBindingsPath();

    // ★Clear は「ファイルがあるか」より前。以前は exists の早期 return が先だったので、
    //   input_bindings.json を持たないプロジェクトを次に開くと、**前のプロジェクトの
    //   キー割り当てがそのまま生き残っていた**。
    m_actionMap.Clear();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;   // 無いのは正常（既定のまま）

    nlohmann::json root;
    try { std::ifstream ifs(path, std::ios::binary); if (!ifs) return; ifs >> root; }
    catch (const std::exception& e)
    {
        // 壊れた設定でゲームが起動しなくなる方が困る。既定のまま進む。
        Logger::Warn("キー割り当ての読み込みに失敗しました（既定のまま進みます）: {}", e.what());
        return;
    }
    if (!root.is_object()) return;

    for (auto it = root.begin(); it != root.end(); ++it)
    {
        if (!it.value().is_array()) continue;
        for (const auto& bj : it.value())
        {
            if (!bj.is_object()) continue;
            const int key = bj.value("key", -1);
            if (key < 0 || key > 255)
            {
                // ★黙って捨てない。PAD_A(4096) 等を bind して save すると、書き出しは
                //   成功するのに読み戻しでここに落ちて割り当てが消える＝「再起動したら
                //   キー設定が戻った」になる。原因が分かる 1 行を残す。
                Logger::Warn("キー割り当てを 1 件捨てました（VK の範囲外 key={}, action={}）。"
                             "actions.bind に渡せるのは KEY_* だけです（PAD_* はゲームパッド用で "
                             "actions では扱えません）", key, it.key());
                continue;
            }
            DirectX::XMFLOAT3 c{1.0f, 0.0f, 0.0f};
            if (bj.contains("c") && bj["c"].is_array() && bj["c"].size() == 3)
                c = { bj["c"][0].get<f32>(), bj["c"][1].get<f32>(), bj["c"][2].get<f32>() };
            m_actionMap.Bind(it.key(), key, c);
        }
    }
    Logger::Info("キー割り当てを読み込みました: {}", path);
}

std::string Application::AutosaveDir()
{
    // assets 配下に置く。シーン JSON がアセットを assets 相対で参照するので、
    // %TEMP% 等の外へ出すと復元時にパスが解決できない（SaveSceneSnapshot と同じ理由）。
    return PathResolver::AssetsDir() + "scenes/.autosave/";
}

void Application::UpdateAutosave(f32 dt)
{
    // 触ってはいけない状況を全部弾く。
    // Playing 中は書かない: Stop でスナップショットへ巻き戻るので、Play 中の状態を
    // オートセーブすると「Stop したら消えるはずの変更」を復旧候補にしてしまう。
    if (m_isGameMode || m_showLauncher || m_loading || m_sceneLoadJob) return;
    if (!m_editorCtx || !m_scene) return;
    if (m_engineMode != EngineMode::Editor) return;
    if (m_editorCtx->currentScenePath.empty()) return;   // 保存先未定のシーンは対象外
    if (m_editorCtx->showAutosaveRecovery) return;       // 復旧を聞いている最中に上書きしない

    m_autosaveTimer += dt;
    if (m_autosaveTimer < kAutosaveInterval) return;
    m_autosaveTimer = 0.0f;
    if (!m_editorCtx->IsSceneDirty()) return;            // 汚れていなければ書く意味が無い

    WriteAutosave();
}

// フレーム例外を記録し、「もう回しても無駄」なら畳む合図を返す。
//
// なぜ要るか:
//   例外ハンドラは「次フレームで復帰を試みる」前提で書かれている。それが正しいのは
//   一時的な失敗のときだけで、SRV ヒープ枯渇や AbortFrame 後の状態食い違いのように
//   毎フレーム同じ理由で失敗し続ける種類もある。その場合ユーザーから見えるのは
//   「画面が真っ暗のまま何も起きない」だけで、原因はログの奥（毎フレーム同じ行が
//   秒 60 本積まれた山の底）に埋もれる。最初の 1 本が一番役に立つのに、
//   それを自分で埋めてしまっていた。
//
//   なので (1) 同じ内容の連投は間引く (2) 一定回数続いたら復帰不能と認めて
//   作業を退避してから畳む。「静かに壊れたまま動き続ける」より
//   「何が起きたか言って止まる」方が失うものが少ない。
bool Application::ReportFrameError(const std::string& what)
{
    // 直前と同じ内容なら間引く（最初の 1 本と、以後 1 秒ぶんごとに 1 本だけ残す）
    const bool same = (what == m_lastFrameError);
    ++m_consecFrameErrors;
    m_lastFrameError = what;

    if (!same)
        Logger::Error("フレーム処理でエラー: {}", what);
    else if (m_consecFrameErrors % 60 == 0)
        Logger::Error("同じエラーが {} フレーム続いています: {}", m_consecFrameErrors, what);

    if (m_consecFrameErrors < kMaxConsecFrameErrors) return false;

    Logger::Error("フレーム処理が {} 回連続で失敗しました。復帰できないので終了します: {}",
                  m_consecFrameErrors, what);
    if (m_editorCtx && m_scene && m_editorCtx->IsSceneDirty() && WriteAutosave())
        Logger::Error("未保存の変更を退避しました。次回起動時に復旧するか聞きます");
    return true;
}

// GPU デバイスが失われていたら、作業内容を退避してから畳む合図を返す。
// 戻り値 true = もうフレームを回してはいけない（呼び出し側は break すること）。
//
// なぜ「復帰を試みない」のか:
//   デバイスロストは D3D12 では回復手段が無い。復帰するにはデバイス・スワップチェーン・
//   全リソースを作り直す必要があり、それは実質アプリの再起動と同じ。
//   中途半端に続けると、GPU 呼び出しが全部失敗する中で null になったポインタを踏んで
//   アクセス違反で落ちる＝未保存の作業が確実に消える。
//   落ちるのが避けられないなら、せめて落ちる前に書いてから畳む。
bool Application::HandleDeviceLoss()
{
    if (m_deviceLost) return true;   // 二度目以降は黙って畳む（同じログを積まない）
    if (!m_graphicsDevice || !m_graphicsDevice->GetDevice()) return false;
    const HRESULT reason = m_graphicsDevice->GetDevice()->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) return false;   // デバイスは生きている＝普通の例外。復帰を試してよい

    const char* why = "不明";
    switch (reason)
    {
    case DXGI_ERROR_DEVICE_HUNG:      why = "GPU がハングした（描画が重すぎて TDR に達した可能性）"; break;
    case DXGI_ERROR_DEVICE_REMOVED:   why = "GPU が取り外された（ドライバ更新・スリープ復帰など）"; break;
    case DXGI_ERROR_DEVICE_RESET:     why = "GPU がリセットされた"; break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: why = "ドライバ内部エラー"; break;
    case DXGI_ERROR_INVALID_CALL:     why = "不正な API 呼び出し（エンジン側のバグ）"; break;
    default: break;
    }
    Logger::Error("GPU デバイスが失われました（0x{:08X}: {}）。復帰できないので終了します",
                  static_cast<unsigned>(reason), why);

    if (m_editorCtx && m_scene && m_editorCtx->IsSceneDirty() && WriteAutosave())
        Logger::Error("未保存の変更を退避しました。次回起動時に復旧するか聞きます");
    else
        Logger::Error("未保存の変更はありません（退避不要）");

    m_deviceLost = true;
    return true;
}

// オートセーブの「実際に書く」部分。UpdateAutosave の間隔判定・未保存判定を通さずに
// 呼べるようにしてある。デバイスロスト時の緊急退避がこれを直接叩くため。
bool Application::WriteAutosave()
{
    if (!m_editorCtx || !m_scene || m_editorCtx->currentScenePath.empty()) return false;
    // 開けなかったシーンの後ろの「空のシーン」を退避しない（復旧候補が空のもので上書きされるのを防ぐ）
    if (!m_editorCtx->sceneLoadFailedPath.empty()) return false;

    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string dir = AutosaveDir();
    fs::create_directories(dir, ec);

    // ★通常の保存とは別物なので MarkSceneClean は呼ばない。
    //   ここで未保存フラグを落とすと「保存した」と誤認させ、確認ダイアログが出なくなる。
    if (!SceneSerializer::Save(*m_scene, autosave::ScenePath(dir), PathResolver::AssetsDir()))
    {
        Logger::Warn("オートセーブに失敗しました: {}", autosave::ScenePath(dir));
        return false;
    }

    // どのシーンの退避かを残す。復旧側はこれを見て本体のパスへ書き戻す。
    nlohmann::json meta{
        {"originPath", m_editorCtx->currentScenePath},
        {"engineVersion", std::string(kEngineVersion)},
        // AI セッション中の退避には印を付ける。復旧の確認モーダルも AI は押せないので、
        // 次回起動時はこれを見て黙って捨てる（CheckAutosaveRecovery）。
        {"aiSession", m_editorCtx->aiSessionEver},
        {"savedAtUnix", static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())},
    };
    if (!atomicfile::WriteJson(atomicfile::PathFromUtf8(autosave::MetaPath(dir)), meta, 2).ok)
        Logger::Warn("オートセーブの記録（meta.json）を書けませんでした: {}", autosave::MetaPath(dir));
    Logger::Info("オートセーブしました: {}", autosave::ScenePath(dir));
    return true;
}

// ============================================================================
//  MCP（AI）セッション中の自動保存
//
//  なぜ要るか:
//    MCP で編集すると HandleMcpCommand が書き込み系メソッドのたびに MarkEdited する。
//    AI は保存を明示的に頼まない限り save_scene を撃たないので、未保存フラグが立ちっぱなしになり、
//    ウィンドウを閉じた時・プロジェクトを閉じた時に「保存していない変更があります」が出る。
//    そのモーダルは AI には押せないし、人間にとっても「AI が置いた物を保存するか」を
//    毎回聞かれるだけの雑音でしかない。
//
//  方針（ユーザー合意 2026-09-10）:
//    最後の MCP 書き込みから kMcpAutoSaveDelay 秒アイドルしたらディスクへ**本保存**する。
//    ＝未保存フラグが立ちっぱなしにならないので、モーダルを出す理由そのものが消える。
//    代償は「保存しなければ無かったことにできる」が使えなくなること。その代わりに
//    セッション最初の上書きの前に .dx12/backups/ へ 1 本残す（WriteMcpBackup）。
// ============================================================================

std::string Application::McpBackupDir()
{
    // プロジェクトルート配下。assets/ の外に置くのは、これが「ゲームの素材」ではなく
    // 「編集履歴の退避」だから（game.pak にも入らないし、assets 相対の参照解決も要らない）。
    return PathResolver::BaseDir() + ".dx12/backups/";
}

bool Application::WriteMcpBackup()
{
    if (!m_editorCtx) return false;
    const std::string scenePath = m_editorCtx->currentScenePath;
    if (scenePath.empty()) return true;                            // ディスクに実体が無い＝壊す元が無い
    if (m_editorCtx->mcpBackupTakenFor == scenePath) return true;  // このセッションでは取得済み

    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(scenePath, ec))
    {
        m_editorCtx->mcpBackupTakenFor = scenePath;   // 新規シーンの初回保存。退避する中身が無い
        return true;
    }

    // 世代つきバックアップ（core/SceneBackup）と同じ置き場・同じ仕組み。分割シーンの .parts / .inst / .nav も一緒に残る。
    // 世代は保存（SceneSerializer::Save）が作る（置き換わる元のファイルを移動するのでコピーが要らない）。ここは「この保存では
    // 間隔の制限を無視して必ず世代を残す」と予約するだけ＝セッション最初の 1 本が確実に残り、世代が二重にもならない。
    scenebackup::ForceNext(scenePath);
    m_editorCtx->mcpBackupTakenFor = scenePath;
    Logger::Info("AI が編集する前のシーンを、次の保存で世代として退避します: {}", scenePath);
    return true;
}

void Application::ApplyBackupPolicyFromSettings()
{
    scenebackup::Policy p;   // 既定: 有効・10 世代・1024MB・60 秒
    p.enabled        = PersistGet("backup_enabled", 1.0) != 0.0;
    p.generations    = std::clamp(static_cast<int>(PersistGet("backup_generations", static_cast<double>(p.generations))), 1, 200);
    p.maxTotalBytes  = static_cast<uint64_t>(std::clamp(PersistGet("backup_max_mb", 1024.0), 16.0, 1048576.0)) * 1024ull * 1024ull;
    p.minIntervalSec = std::clamp(static_cast<int>(PersistGet("backup_interval_sec", static_cast<double>(p.minIntervalSec))), 0, 86400);
    scenebackup::GlobalPolicy() = p;
}

bool Application::RestoreSceneBackup(const std::string& id, const std::string& scenePath, std::string& err)
{
    namespace fs = std::filesystem;
    if (!m_editorCtx) { err = "エディタが使えません"; return false; }
    if (m_engineMode != EngineMode::Editor) { err = "Play 中は戻せません。停止してから戻してください"; return false; }
    const std::string target = !scenePath.empty() ? scenePath
                             : !m_editorCtx->currentScenePath.empty() ? m_editorCtx->currentScenePath
                             : m_editorCtx->sceneBackupTarget;
    if (target.empty()) { err = "対象のシーンがありません"; return false; }
    std::string pre;
    const atomicfile::Result r = scenebackup::Restore(PathResolver::BaseDir(), fs::path(target), id, &pre);
    if (!r.ok) { err = r.error; return false; }
    Logger::Info("シーンを以前の版へ戻しました: {} <- {}（戻す前の版: {}）", target, id, pre.empty() ? "同じ内容のため作らず" : pre);
    // いま開いているシーン（または開けなかったシーン）を戻したなら開き直す（メモリ上の版を捨てる。本人が選んだ操作なので確認しない）。
    // 別のシーンのファイルだけを戻したときは、開いているシーンを切り替えない。
    const bool isCurrent = m_editorCtx->currentScenePath.empty()
                        || autosave::SamePath(target, m_editorCtx->currentScenePath)
                        || autosave::SamePath(target, m_editorCtx->sceneLoadFailedPath);
    if (isCurrent)
    {
        m_editorCtx->pendingLoadPath        = target;
        m_editorCtx->pendingLoadSkipConfirm = true;
    }
    m_editorCtx->Notify(ui::ToastKind::Success, "以前の版へ戻しました: " + id);
    return true;
}

void Application::OnSceneLoadFailed(const std::string& fullPath)
{
    if (!m_editorCtx) return;
    const std::string reason = SceneSerializer::LastLoadError();
    const std::string name = std::filesystem::path(fullPath).filename().string();
    Logger::Error("シーンを開けませんでした: {}（{}）。空のシーンで上書きしないよう、保存と自動保存を止めます", fullPath, reason);
    m_editorCtx->sceneLoadFailedPath = fullPath;
    // 空のシーンの後ろに古いパスが残っていると、Ctrl+S が「空のシーン」で本体を上書きする。保存先を外す。
    m_editorCtx->currentScenePath.clear();
    m_currentSceneRel.clear();
    m_editorCtx->sceneBackupTarget = fullPath;
    m_editorCtx->sceneBackupNotice = "「" + name + "」を開けませんでした。" + (reason.empty() ? std::string() : reason)
        + "\n空のシーンで上書きしてしまわないよう、保存と自動保存を止めています。下の一覧から以前の版を選ぶと元に戻せます。";
    // AI（MCP）が繋がっているときはモーダルを出さない（AI は押せずに固まる）。AI には open_scene の失敗に
    // scene_backups の案内が付いて返る。窓はファイル メニュー「以前の版に戻す…」から開ける。
    m_editorCtx->showSceneBackups = !m_editorCtx->aiSessionEver;
    m_editorCtx->Notify(ui::ToastKind::Error, "シーンを開けませんでした: " + name + "（以前の版から戻せます）", 12.0f);
    MarkSceneClean(/*dropAutosave=*/false);   // 空のシーンの「未保存」で閉じる時に聞かれないように
}

bool Application::SaveSceneForMcp()
{
    if (!m_editorCtx || !m_scene) return false;
    // 開けなかったシーンの後ろの「空のシーン」を自動で保存しない（本体や仮の名前のファイルを作らない）
    if (!m_editorCtx->sceneLoadFailedPath.empty()) return false;

    if (m_editorCtx->currentScenePath.empty())
    {
        // 未保存の新規シーン。AI は「名前を付けて保存」ダイアログを押せないので、
        // ここで保存先を決めてやらないと永久に保存できない＝必ず未保存のまま残る。
        // 人が後から本来の名前で保存し直せるよう、時刻入りの明らかに仮の名前にする。
        namespace fs = std::filesystem;
        std::error_code ec;
        char stamp[32] = {};
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tmv{};
        if (localtime_s(&tmv, &t) == 0) std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
        const std::string full = PathResolver::AssetsDir() + "scenes/_mcp_untitled_" + stamp + ".json";
        fs::create_directories(fs::path(full).parent_path(), ec);
        m_editorCtx->currentScenePath = full;
        Logger::Info("未保存の新規シーンに保存先を割り当てました: {}", full);
    }

    // 退避に失敗しても保存は続ける。ここで止めると「保存されないまま編集が積み上がる」＝
    // 未保存モーダルが復活する側に倒れるので、失敗はログに残すだけにする。
    WriteMcpBackup();

    if (!SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir()))
    {
        Logger::Warn("MCP 自動保存に失敗しました: {}", m_editorCtx->currentScenePath);
        return false;
    }
    MarkSceneClean();
    return true;
}

void Application::UpdateMcpAutoSave(f32 dt)
{
    if (!m_editorCtx || !m_scene) return;
    // ★ヘッドレスは既定で読み取り専用。CI が「検証しただけ」でプロジェクトを書き換えないため。
    //   （navmesh_build のような検証手順自体が編集扱いになるので、ここで止めないと必ず書かれる）
    if (m_headless && !m_headlessAllowSave) return;
    if (m_isGameMode || m_showLauncher || m_loading || m_sceneLoadJob) return;
    // Playing 中は書かない。Stop でスナップショットへ巻き戻る＝Play 中の状態は
    // 「保存されるべき編集」ではない（オートセーブ側と同じ判断）。
    if (m_engineMode != EngineMode::Editor) return;

    // MCP の書き込み以外の経路（シーン設定の窓・Play/Stop でのフィンガープリント変化・
    // 人が手でいじった分）で汚れることもある。AI が繋がっている間はそれも拾って書く。
    // ここが無いと「MCP は撃っていないのに未保存」が残り、窓を閉じる時まで気づけない
    // ＝「必ず出ない」の保証が閉じる瞬間の 1 経路だけに頼ることになる。
    if (m_editorCtx->mcpSaveCountdown < 0.0f && m_editorCtx->aiSessionActive
        && m_editorCtx->IsSceneDirty())
        m_editorCtx->mcpSaveCountdown = kMcpAutoSaveDelay;

    if (m_editorCtx->mcpSaveCountdown < 0.0f) return;

    // M5: AI のトランザクションが開いている間は本保存しない（rollback で戻す途中の状態をディスクへ書かない）。
    //     カウントダウンは進めず、閉じた後の最初の Update で通常どおり書く。
    if (m_editorCtx->mcpUndo.TxOpen()) return;

    m_editorCtx->mcpSaveCountdown -= dt;
    if (m_editorCtx->mcpSaveCountdown > 0.0f) return;
    m_editorCtx->mcpSaveCountdown = -1.0f;

    if (!m_editorCtx->IsSceneDirty()) return;
    SaveSceneForMcp();
}

void Application::CheckAutosaveRecovery(const std::string& sceneFullPath)
{
    if (!m_editorCtx || sceneFullPath.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;

    const std::string dir   = AutosaveDir();
    const std::string auto_ = autosave::ScenePath(dir);
    const std::string metaP = autosave::MetaPath(dir);
    if (!fs::exists(auto_, ec) || !fs::exists(metaP, ec)) return;

    nlohmann::json meta;
    try { std::ifstream mf(metaP, std::ios::binary); if (!mf) return; mf >> meta; }
    catch (...) { return; }   // 壊れた meta は黙って無視（復旧を促せないだけ）

    // 別のシーンの退避なら関係ない
    if (!autosave::SamePath(meta.value("originPath", std::string()), sceneFullPath)) return;

    // ★AI セッション中に書かれた退避、または今まさに MCP が繋がっている状態では聞かない。
    //   「保存されていない自動保存が見つかりました」も AI には押せないモーダルで、
    //   ここで止まると open_scene が永久に完了しない。AI の編集は自動保存で本体に
    //   落ちているので、復旧候補として残す意味も無い。
    if (meta.value("aiSession", false) || m_editorCtx->aiSessionActive)
    {
        DiscardAutosaveFor(sceneFullPath);
        return;
    }

    // 本体の方が新しければ復旧するものは無い（＝正常に保存して終えている）。
    const auto autoT   = fs::last_write_time(auto_, ec);
    if (ec) return;
    const auto sceneT  = fs::last_write_time(sceneFullPath, ec);
    if (!ec && sceneT >= autoT) { DiscardAutosaveFor(sceneFullPath); return; }

    // ★更新時刻だけを信じない。中身が同じなら復旧するものは無い。
    //   時刻の比較はコピー・展開・同期・時計のずれで簡単に逆転するので、これが最後の砦。
    //   ここが無いと「保存して終えたのに毎回聞かれる」が時刻のいたずらだけで再発する。
    // 分割保存のシーンは scene.json（目次）が同じでもセルファイルの中身が違いうるので、この近道は使わない。
    std::error_code partsEc;
    if (!fs::exists(autosave::PartsPath(dir), partsEc) && autosave::SameBytes(auto_, sceneFullPath))
    {
        DiscardAutosaveFor(sceneFullPath);
        return;
    }

    const long long at = meta.value("savedAtUnix", 0LL);
    std::string when = "(時刻不明)";
    if (at > 0)
    {
        const std::time_t t = static_cast<std::time_t>(at);
        std::tm tmv{};
        if (localtime_s(&tmv, &t) == 0)
        {
            char buf[64];
            if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) > 0) when = buf;
        }
    }
    m_editorCtx->autosaveInfo         = when;
    m_editorCtx->autosaveChoice       = EditorContext::AutosaveChoice::None;
    m_editorCtx->showAutosaveRecovery = true;
    Logger::Warn("保存されていない自動保存が見つかりました（{}）。復旧するか確認します", when);
}

bool Application::ConfirmDiscardScene(bool& outCancelled)
{
    outCancelled = false;
    if (!m_editorCtx) return true;
    if (!m_editorCtx->IsSceneDirty()) return true;   // 汚れていないなら黙って進む

    // ★AI（MCP）が絡んだセッションでは未保存モーダルを絶対に出さない。
    //   AI はボタンを押せないので、出た瞬間にツール呼び出しがタイムアウトし、
    //   人間からは「エディタがハングした」ようにしか見えない。
    //   代わりに黙って本保存する。保存できなかった時だけ退避（オートセーブ）へ逃がす
    //   ＝どちらに転んでも作業は残り、モーダルは出ない。
    if (m_editorCtx->aiSessionEver)
    {
        // ヘッドレスの読み取り専用モードでは何も書かずに進む（CI は捨てるだけ）
        if (m_headless && !m_headlessAllowSave) return true;
        if (SaveSceneForMcp()) return true;
        if (WriteAutosave())
            Logger::Warn("保存に失敗したので退避しました。次回起動時に復旧できます");
        else
            Logger::Error("保存にも退避にも失敗しました。この変更は失われます");
        MarkSceneClean();   // 二度と聞かない（聞いても AI は答えられない）
        return true;
    }

    using Choice = EditorContext::UnsavedChoice;
    const Choice choice = m_editorCtx->unsavedChoice;
    if (choice != Choice::None)
    {
        m_editorCtx->unsavedChoice      = Choice::None;
        m_editorCtx->showUnsavedConfirm = false;
    }

    switch (choice)
    {
    case Choice::Save:
        if (m_scene && !m_editorCtx->currentScenePath.empty() &&
            SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir()))
        {
            MarkSceneClean();
            return true;
        }
        // 保存できなかった（保存先が未設定 / 書き込み失敗）。ここで進むと変更が消えるので、
        // 進まずに操作ごと取り消す。黙って捨てるくらいなら操作を無かったことにする方がまし。
        Logger::Warn("未保存の変更を保存できませんでした（保存先が未設定か書き込みに失敗）。操作を取り消します");
        outCancelled = true;
        return false;

    case Choice::Discard:
        // 破棄＝この変更は無かったことにする。以後もう聞かない。
        MarkSceneClean();
        return true;

    case Choice::Cancel:
        outCancelled = true;
        return false;

    case Choice::None:
    default:
        m_editorCtx->showUnsavedConfirm = true;   // 毎フレーム true でよい（Toolbar 側でラッチする）
        return false;
    }
}

void Application::MarkSceneClean(bool dropAutosave)
{
    // 「いまの状態＝保存済み」に揃える。保存直後・シーンを開いた直後・新規作成直後。
    // 指紋はここで取り直す（ポーリングで拾った古い値を使うと、保存直前の設定変更が
    // 保存後に「新しい変更」として再検出される）。
    if (!m_editorCtx || !m_scene) return;
    m_editorCtx->MarkSceneSaved(SceneSettingsFingerprint(*m_scene));

    // ★ここに来た＝「今の内容が正」とユーザーが決めた（保存した / 破棄を選んだ）。
    //   退避しておいたオートセーブはもう復旧候補ではないので消す。
    //
    //   消さないと何が起きるか（実際に起きていた）:
    //   復旧するかの判定はファイルの更新時刻だけ（オートセーブの方が新しいか）で見ている。
    //   保存や破棄でオートセーブを消していなかったので、一度書かれたオートセーブが
    //   シーン本体より新しいまま残り続け、**Ctrl+S で保存して終えても次に開くたびに**
    //   「保存されていない自動保存が見つかりました」を聞かれた。
    //   特に「破棄」は 「以後もう聞かない」 つもりの操作なのに、ディスク側は何も変わって
    //   いなかったので必ず再発した。
    if (dropAutosave) DiscardAutosaveFor(m_editorCtx->currentScenePath);
}

void Application::EditorSaveScene()
{
    if (!m_editorCtx || !m_scene) return;
    if (m_engineMode != EngineMode::Editor)
    {
        m_editorCtx->Notify(ui::ToastKind::Warn, "Play 中は保存できません。停止してから保存してください");
        return;
    }
    if (m_editorCtx->currentScenePath.empty())
    {
        // 保存先が未設定 → 名前入力ダイアログ（保存モード）。OS のダイアログは仮想入力中に出せないので使わない。
        m_editorCtx->showNewSceneDialog = true;
        m_editorCtx->newSceneDialogIsCreate = false;
        std::memset(m_editorCtx->newSceneNameBuf, 0, sizeof(m_editorCtx->newSceneNameBuf));
        strncpy_s(m_editorCtx->newSceneNameBuf, "Untitled", _TRUNCATE);
        return;
    }

    const std::string name = std::filesystem::path(m_editorCtx->currentScenePath).filename().string();
    // ★成否を見てから通知する（以前は Save の戻り値を見ずに緑の「✓ Saved」を出していた＝
    //   書けていないのに保存できたように見え、プロジェクトは「そのシーンを開いていた」と記録してしまう）。
    if (SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir()))
    {
        MarkSceneClean();
        ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
        RequestProjectThumbnail(/*force*/ false);   // ランチャーのカード用（45 秒に 1 回まで）
        m_editorCtx->Notify(ui::ToastKind::Success, "保存しました: " + name);
    }
    else
    {
        Logger::Error("シーンを保存できませんでした: {}", m_editorCtx->currentScenePath);
        m_editorCtx->Notify(ui::ToastKind::Error, "保存に失敗しました: " + name + "（詳細は dx12_engine.log）");
    }
    if (m_editorLayer) m_editorLayer->RefreshAssetBrowser();
}

void Application::FocusEditorCameraOnSelection()
{
    if (!m_editorCtx || !m_scene || !m_camera || !m_editorCtx->HasSelection()) return;
    auto& reg = m_scene->GetRegistry();
    auto sel = m_editorCtx->selectedEntity;
    if (!reg.valid(sel) || !reg.all_of<Transform>(sel)) return;
    const auto& t = reg.get<Transform>(sel);

    // 対象サイズからフォーカス距離を決める
    f32 dist = 5.0f;
    if (reg.all_of<MeshRenderer>(sel))
    {
        const auto& mr = reg.get<MeshRenderer>(sel);
        f32 maxExtent = 0.0f;
        for (const auto* mesh : mr.meshes)
        {
            if (!mesh) continue;
            auto mn = mesh->GetAABBMin();
            auto mx = mesh->GetAABBMax();
            maxExtent = std::max({maxExtent,
                (mx.x - mn.x) * t.scale.x,
                (mx.y - mn.y) * t.scale.y,
                (mx.z - mn.z) * t.scale.z});
        }
        if (maxExtent > 0.0f)
            dist = std::clamp(maxExtent * 2.0f, 2.0f, 100.0f);
    }

    // 親階層込みのワールド位置にフォーカス
    DirectX::XMFLOAT3 wpos = t.position;
    if (t.parent != entt::null && reg.valid(t.parent))
    {
        DirectX::XMFLOAT4X4 wf;
        XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, sel));
        wpos = {wf._41, wf._42, wf._43};
    }

    auto fwd = m_camera->GetForward();
    m_camera->SetPosition({wpos.x - fwd.x * dist,
                           wpos.y - fwd.y * dist,
                           wpos.z - fwd.z * dist});
}

// 指定シーンの退避（オートセーブ）を捨てる。別シーンの退避なら触らない
// （そこにしか無い未保存作業を消してしまうため）。判定と削除は autosave::DiscardIfFor。
void Application::DiscardAutosaveFor(const std::string& scenePath)
{
    if (autosave::DiscardIfFor(AutosaveDir(), scenePath))
        m_autosaveTimer = 0.0f;   // 直後にもう一度書かれないよう間隔を測り直す
}

void Application::RestoreRenderDebugSettings()
{
    if (!m_renderDebugRestore.valid || !m_scene) return;
    m_scene->GetTaaSettings().enabled           = m_renderDebugRestore.taa;
    m_scene->GetSSAOSettings().enabled          = m_renderDebugRestore.ssao;
    m_scene->GetContactShadowSettings().enabled = m_renderDebugRestore.contactShadow;
    m_scene->GetSsrSettings().enabled           = m_renderDebugRestore.ssr;
    m_scene->GetSsgiSettings().enabled          = m_renderDebugRestore.ssgi;
    m_scene->GetVolumetricFogSettings().debugMode = m_renderDebugRestore.fogDebug;
    m_scene->GetRtSettings().forceBuildTlas       = m_renderDebugRestore.rtForceTlas;
    if (m_editorCtx) m_editorCtx->clusterDebugMode = m_renderDebugRestore.clusterDebug;
    m_showCascadeDebug = m_renderDebugRestore.cascadeDebug;
    m_renderDebugRestore.valid = false;
}

void Application::ApplyFootIkPass()
{
    if (!m_scene) return;
    // 物理ボディは Play 中しか存在しない＝エディタでは接地判定ができない（仕様）
    if (m_engineMode != EngineMode::Playing) return;
    if (!m_physicsSystem) return;

    auto& reg = m_scene->GetRegistry();
    auto view = reg.view<FootIK, SkeletalAnimation>();
    if (view.begin() == view.end()) return;

    const f32 dt = m_gameClock.GetDeltaTime();

    // PhysicsSystem::Raycast を FootIK 側のコールバック形へ包む。
    // ★従来の Raycast は法線を (0,1,0) にフェイクしているので使えない★
    const FootIKRayCast rayFn =
        [this](const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& dir, f32 maxDist,
               DirectX::XMFLOAT3& outPoint, DirectX::XMFLOAT3& outNormal) -> bool
    {
        const RaycastHit hit = m_physicsSystem->Raycast(origin, dir, maxDist, 0xFFFFFFFFu, entt::null, /*includeCharacters=*/false);
        if (!hit.hit) return false;
        outPoint  = hit.point;
        outNormal = hit.normal;
        return true;
    };

    for (auto [e, ik, skelAnim] : view.each())
    {
        if (!ik.enabled || ik.weight <= 0.0f) continue;
        if (!skelAnim.animator || !skelAnim.skeleton) continue;

        // ---- ボーン解決（1 回だけ）----
        if (!ik._resolved && !ik._resolveFailed)
        {
            FootIKBoneNames names;
            names.leftHip   = ik.leftHipBone;   names.leftKnee  = ik.leftKneeBone;
            names.leftFoot  = ik.leftFootBone;  names.leftToe   = ik.leftToeBone;
            names.rightHip  = ik.rightHipBone;  names.rightKnee = ik.rightKneeBone;
            names.rightFoot = ik.rightFootBone; names.rightToe  = ik.rightToeBone;
            names.pelvis    = ik.pelvisBone;

            FootIKBones bones;
            if (ResolveFootIKBones(*skelAnim.skeleton, names, bones))
            {
                ik._lHip = bones.left.hip;   ik._lKnee = bones.left.knee;
                ik._lFoot = bones.left.foot; ik._lToe  = bones.left.toe;
                ik._rHip = bones.right.hip;  ik._rKnee = bones.right.knee;
                ik._rFoot = bones.right.foot; ik._rToe = bones.right.toe;
                ik._pelvis = bones.pelvis;
                ik._resolved = true;
            }
            else
            {
                ik._resolveFailed = true;
                std::string all;
                for (u32 b = 0; b < skelAnim.skeleton->GetBoneCount(); ++b)
                {
                    if (!all.empty()) all += ", ";
                    all += skelAnim.skeleton->GetBone(b).name;
                }
                Logger::Warn("FootIK: 足ボーンを特定できませんでした。FootIK の "
                             "leftHipBone/leftKneeBone/leftFootBone(+right) で明示指定してください。"
                             "このスケルトンのボーン一覧: [{}]", all);
            }
        }
        if (!ik._resolved) continue;

        FootIKBones bones;
        bones.left  = { ik._lHip, ik._lKnee, ik._lFoot, ik._lToe };
        bones.right = { ik._rHip, ik._rKnee, ik._rFoot, ik._rToe };
        bones.pelvis = ik._pelvis;

        FootIKParams params;
        params.weight          = ik.weight;
        params.rayUpOffset     = ik.rayUpOffset;
        params.rayLength       = ik.rayLength;
        params.footHeight      = ik.footHeight;
        params.maxPelvisDrop   = ik.maxPelvisDrop;
        params.maxFootPitchDeg = ik.maxFootPitchDeg;
        params.smoothTime      = ik.smoothTime;
        params.fadeOutTime     = ik.fadeOutTime;
        params.alignToNormal   = ik.alignToNormal;
        params.kneeForward     = ik.kneeForward;

        FootIKState st;
        st.leftLift    = ik._lLift;    st.rightLift   = ik._rLift;
        st.pelvisDrop  = ik._pelvisDrop;
        st.leftWeight  = ik._lWeight;  st.rightWeight = ik._rWeight;
        st.leftNormal  = ik._lNormal;  st.rightNormal = ik._rNormal;
        st.leftContact = ik._lContact; st.rightContact = ik._rContact;
        st.initialized = ik._smoothInit;

        // 接地判定は CharacterController があればそれを使う（ジャンプ中は IK を切る）
        bool grounded = true;
        if (reg.all_of<CharacterController>(e))
            grounded = reg.get<CharacterController>(e)._grounded;

        const DirectX::XMMATRIX world = ComputeWorldMatrix(reg, e);
        ApplyFootIK(*skelAnim.animator, *skelAnim.skeleton, world,
                    bones, params, st, rayFn, dt, grounded);

        ik._lLift   = st.leftLift;    ik._rLift   = st.rightLift;
        ik._pelvisDrop = st.pelvisDrop;
        ik._lWeight = st.leftWeight;  ik._rWeight = st.rightWeight;
        ik._lNormal = st.leftNormal;  ik._rNormal = st.rightNormal;
        ik._lContact = st.leftContact; ik._rContact = st.rightContact;
        ik._smoothInit = st.initialized;
    }
}

// グローバル game.lua を読み直す。**シーンには一切触らない**。
//
// ★以前ここは RebuildScene() で、`m_scene->Clear()` から作り直していた。
//   game.lua がシーンを手続き的に組んでいた時代の名残で、シーンをエディタで作る今は
//   **エディタを開いたまま game.lua を保存しただけでシーンが全部消える**（しかも
//   ポスト / SSAO / 空 / フォグ / 影 ON-OFF 等のシーン設定も既定へ戻る）。
//   RebuildScene は Load をしないので何も復元されず、そのまま Ctrl+S すると
//   空のシーンが本物のファイルを上書きする。自動保存も空の内容を書きに行く。
//   ホットリロードでやりたいのは「Lua を読み直す」ことだけなので、それだけをする。
void Application::ReloadGameScript()
{
    // Lua state を捨てる前にネットワークの RPC ハンドラを外す（DoRuntimeSceneLoad と同じ理由）
    if (m_networkSystem) m_networkSystem->ClearRpcHandlers();
    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();

    // Play 中（またはゲーム）に書き換えたなら、読み直しはゲームプレイの再開始なので OnStart を呼ぶ。
    LoadGameScript(/*callOnStart=*/m_isGameMode || m_engineMode == EngineMode::Playing);
}


} // namespace dx12e
