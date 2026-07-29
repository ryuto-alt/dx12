// ===========================================================================
// Application: 起動 / メインループ / モード遷移の中枢
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


Application::Application() = default;

// ★この関数は必ず Application.cpp（= Application 本体と同じ TU）で定義すること。
// インライン化されるとチェックの意味が消える。詳細は main.cpp のビルド健全性チェック。
size_t Application::CompiledLayoutSize()
{
    return sizeof(Application);
}

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
    m_showWhatsNew = !gameMode && (ReadShownVersion() != std::string(kEngineVersion));
    Logger::Info("Application initializing... (mode: {})", gameMode ? "game" : "editor");

    // エディタコンテキスト初期化
    m_editorCtx = std::make_unique<EditorContext>();
    // エディタ（別ライブラリ）へ Application 側のフレームデータを読み取り専用で貸す。
    // どちらも Application の寿命いっぱい生きるメンバなのでアドレスは不変＝ここで一度だけ渡す。
    //   drawItems  … 精密ピッキングのブロードフェーズ候補（ワールド行列/球が計算済み）
    //   cpuScopeMs … picking/gizmo の計測を perf_stats の cpuScopeMs に載せるため
    m_editorCtx->drawItems  = &m_drawItems;
    m_editorCtx->cpuScopeMs = m_cpuMs;

    // ウィンドウ作成（タイトルにエンジンのバージョンを表記＝更新の確認にも使える）
    m_window = std::make_unique<Window>();
    std::wstring windowTitle = L"DX12 Engine v";
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
    SplashScreen::SetStatus("ウィンドウを作成中...");
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
    if (gameMode)
    {
        m_useVsync = PersistGet("video_vsync", m_useVsync ? 1.0 : 0.0) != 0.0;
        m_fpsLimit = static_cast<f32>(PersistGet("video_fps", m_fpsLimit));
        m_instancingEnabled = PersistGet("render_instancing", 1.0) != 0.0;
        // クラスタードライティング（Forward+）。0 にすると「先頭 64 灯を総当たり」
        // フォールバックへ倒す（A/B 検証用。旧 8 灯経路そのものは残していない）。
        m_clusteredEnabled  = PersistGet("render_clustered", 1.0) != 0.0;
        m_forceDepthPrepass = PersistGet("render_depth_prepass", 0.0) != 0.0;
        // BC7/BC5 テクスチャ圧縮（0=無圧縮 / 1=高速 / 2=高品質）。既定 1。
        TextureLoader::SetCompressionMode(static_cast<int>(PersistGet("texture_compression", 1.0)));
        const int mode = static_cast<int>(PersistGet("video_mode", 0));
        const u32 w = static_cast<u32>(PersistGet("video_w", 0));
        const u32 h = static_cast<u32>(PersistGet("video_h", 0));
        if (mode == static_cast<int>(WindowMode::Borderless))
            m_window->SetMode(WindowMode::Borderless);
        else if (mode == static_cast<int>(WindowMode::Fullscreen))
            m_window->SetMode(WindowMode::Fullscreen, w, h);
        else if (w > 0 && h > 0)
            m_window->SetClientSize(w, h);
    }

    // グラフィックスデバイス初期化
    SplashScreen::SetStatus("グラフィックスデバイスを初期化中...");
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
    SplashScreen::SetStatus("オーディオを初期化中...");
    m_audioSystem = std::make_unique<AudioSystem>();
    m_audioSystem->Initialize(PathResolver::AssetsDir());

    // Physics System
    SplashScreen::SetStatus("物理エンジンを初期化中...");
    m_physicsSystem = std::make_unique<PhysicsSystem>();
    m_physicsSystem->Initialize();
    // 接触イベント（engine.contact.enter/exit）を C++ EventBus へ配信させる。
    // m_eventBus は Application の安定メンバ。物理を Shutdown→Initialize で再構築しても
    // PhysicsSystem 側はこのポインタを保持し続ける（Shutdown では null 化しない）。
    m_physicsSystem->SetEventBus(&m_eventBus);

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
    SplashScreen::SetStatus("シェーダーとパイプラインを構築中...");
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
        SplashScreen::SetStatus("アセットを読み込み中...");
        m_resourceManager = std::make_unique<ResourceManager>();
        m_resourceManager->Initialize(m_graphicsDevice.get(), m_srvHeap.get(), cmdList);

        m_materialAssetManager = std::make_unique<MaterialAssetManager>();
        m_materialAssetManager->Initialize(m_resourceManager.get(), m_graphicsDevice.get(), m_srvHeap.get());

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
        SplashScreen::SetStatus("スクリプトエンジンを初期化中...");
        m_scriptEngine = std::make_unique<ScriptEngine>();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), PathResolver::AssetsDir());
        WireScriptCallbacks();

        // ゲームスクリプト読み込み（グローバル game.lua）
        LoadGameScript();

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
            std::filesystem::path sneakPath = PathResolver::AssetsDir() + "models/human/sneakWalk.gltf";
            if (std::filesystem::exists(sneakPath))
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
    SplashScreen::SetStatus("シャドウマップを準備中...");
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
        DirectX::XMFLOAT4    _clusterReserved[40];             // 640B (offset 608..1247)
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
    SplashScreen::SetStatus("エディタUIを初期化中...");
    m_imguiManager = std::make_unique<ImGuiManager>();
    m_imguiManager->Initialize(
        m_window->GetHwnd(), *m_graphicsDevice, m_commandQueue->GetQueue(),
        *m_srvHeap, m_swapChain->GetFormat(), FrameResources::kFrameCount);

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
    SplashScreen::SetStatus("レンダラーを初期化中...");
    {
        // sceneRT(1)+cameraPreview(2)+ブルームチェーン(6)+ゴッドレイ/レンズフレア/DoF/
        // モーションブラー+SSAO(2)+コンタクトシャドウ(1)+歪みRT で 20 個ほど使う。
        // 容量 64: 今後の深度依存パス（モーションベクター/SSGI/SSR/ボリュメトリック等）を
        // 足しても枯渇しない余裕を先に取っておく（RTV は非シェーダ可視で安価。
        // 枯渇時は DescriptorHeap::Allocate が Logger::Error + throw で fail-fast する）。
        m_offscreenRtvHeap = std::make_unique<DescriptorHeap>();
        m_offscreenRtvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64, false);

        // シーンは HDR(kSceneColorFormat) の中間RTへ描き、ポストで backbuffer へ解決する。
        // クリア色はリニア空間の値（最終段のACES+ガンマ後にコーンフラワーブルーに見える値）
        const float sceneClear[4] = {0.127f, 0.306f, 0.850f, 1.0f};
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
                                             m_srvHeap->GetCpuHandle(m_ssBlackSrvIndex));
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

    // 全モデルのサムネイルを起動時にロード/レンダリング（エディタ専用機能）。
    // ゲーム(封印ランタイム)では実行しない＝起動時に exe 隣へ assets/.thumbcache/ を作らない。
    if (!m_isGameMode)
    {
        size_t uncachedCount = m_thumbRenderer->ScanAllModels(PathResolver::AssetsDir());
        size_t cachedCount   = m_thumbRenderer->GetCachedCount();
        size_t totalModels   = uncachedCount + cachedCount;

        if (totalModels > 0)
        {
            // Phase 1: ディスクキャッシュから一括ロード（高速）
            if (cachedCount > 0)
            {
                auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
                m_thumbRenderer->LoadCachedThumbnails(cmdList);
                ThrowIfFailed(cmdList->Close());
                m_commandQueue->ExecuteCommandList(cmdList);
                m_commandQueue->WaitIdle();
                m_frameResources->EndFrame(*m_commandQueue);
                Logger::Info("[Thumbnail] Cache loaded: {} models", cachedCount);
            }

            // Phase 2: 未キャッシュのみレンダリング（進捗表示付き）
            if (uncachedCount > 0)
            {
                size_t completed = 0;

                while (m_thumbRenderer->GetPendingCount() > 0)
                {
                    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
                    m_commandList->Wrap(cmdList);

                    m_thumbRenderer->RenderNext(cmdList);
                    ++completed;

                    // ★この時点ではメインウィンドウはまだ非表示(m_deferredFirstShow)なので、
                    //   下の ImGui 進捗は画面に出ない。実際にユーザーが見るのはスプラッシュ。
                    {
                        char st[96];
                        snprintf(st, sizeof(st), "サムネイルを生成中... (%zu / %zu)",
                                 completed, uncachedCount);
                        SplashScreen::SetStatus(st);
                    }

                    // ローディング画面をバックバッファに描画
                    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
                    auto rtvHandle = m_descriptorHeap->GetCpuHandle(
                        m_swapChain->GetCurrentBackBufferIndex());

                    m_commandList->TransitionResource(backBuffer,
                        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

                    float clearColor[4] = {0.08f, 0.08f, 0.10f, 1.0f};
                    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                    D3D12_VIEWPORT vp = {0, 0,
                        static_cast<f32>(m_window->GetWidth()),
                        static_cast<f32>(m_window->GetHeight()), 0, 1};
                    D3D12_RECT scissor = {0, 0,
                        static_cast<LONG>(m_window->GetWidth()),
                        static_cast<LONG>(m_window->GetHeight())};
                    cmdList->RSSetViewports(1, &vp);
                    cmdList->RSSetScissorRects(1, &scissor);

                    float progress = static_cast<float>(completed) / static_cast<float>(uncachedCount);
                    m_imguiManager->BeginFrame();
                    // multi-viewport有効時、ImGui座標はスクリーン座標になるためメインビューポート中心へ置く
                    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
                    ImGui::Begin("##Loading", nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
                    ImGui::Text("DX12 Engine");
                    ImGui::Separator();
                    ImGui::Text("Rendering thumbnails... (%zu / %zu)", completed, uncachedCount);
                    ImGui::ProgressBar(progress, ImVec2(-1, 24));
                    ImGui::End();
                    m_imguiManager->EndFrame(cmdList);

                    m_commandList->TransitionResource(backBuffer,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                    m_commandList->Close();
                    m_commandQueue->ExecuteCommandList(cmdList);
                    m_swapChain->Present(false);
                    m_frameResources->EndFrame(*m_commandQueue);

                    m_commandQueue->WaitIdle();

                    // レンダリング結果をディスクキャッシュに保存
                    m_thumbRenderer->SavePendingCache();

                    MSG msg;
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
            }

            m_resourceManager->FinishUploads();
        }
    }

    // IBL: シーンの skybox 設定に応じて環境キューブを読み込み派生をベイク（専用 cmdList）。
    SplashScreen::SetStatus("環境マップをベイク中...");
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
        SplashScreen::SetStatus("画面を準備しています...");

    Logger::Info("Application initialized successfully");

    // AI(MCP)ブリッジ。エディタ時のみ。ゲーム(封印ランタイム)では起動しない＝外部から触れない。
    // ヘッドレス --build でも起動しない: 起動中エディタが 8787 を握っている状態で build が
    // 走ると 8788 に bind→WritePortFile が %TEMP%/dx12_mcp.port を 8788 で上書きし、build 終了で
    // その port が死ぬ＝Node 側の自動検出が死にポートを掴みライブツールが切れる原因になる。
    if (!m_isGameMode && !buildMode)
    {
        m_mcpBridge = std::make_unique<McpBridge>();
        m_mcpBridge->Start(8787);   // ponytail: ポート固定。衝突したら env/引数化する。
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
        if (m_mcpBridge)
            m_mcpBridge->Poll([this](uint64_t client, const std::string& line) {
                return HandleMcpCommand(client, line);
            });

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

        // モード切替（前フレームのImGuiボタンから遅延実行）
        if (m_modeChangeRequested)
        {
            m_modeChangeRequested = false;
            try
            {
                if (m_pendingMode == EngineMode::Playing)
                    EnterPlayMode();
                else
                    EnterEditorMode();
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
                    CompleteMcp(m_mcpBridge.get(), m_mcpModeReply,
                        nlohmann::json{{"mode", nowPlaying ? "Playing" : "Editor"},
                                       {"sceneGeneration", m_sceneGeneration},
                                       {"scriptErrors", m_scriptEngine
                                            ? m_scriptEngine->CollectScriptErrors().size() : 0u}});
                m_mcpModeReply = {};
            }
        }

        // 入力状態リセット（前フレームのdeltaクリア + prevKeys保存 + XInputポーリング）
        m_inputSystem->Update(m_gameClock.GetDeltaTime());

        // メッセージ処理（ここで WM_KEYDOWN/WM_MOUSEMOVE → InputSystem に蓄積）
        m_window->ProcessMessages();

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
                    RebuildScene();
                    m_editorCtx->hotReloadFlash = 2.0f;
                    Logger::Info("Hot-reload complete");
                }
            }

            // コンポーネント .lua（assets/components/*.lua 等）のホットリロード。
            // game.lua と違い RebuildScene は要らない: 該当エンティティの env を捨てるだけで、
            // 次の UpdateAttachedScripts が作り直す。Play を止めずにスクリプトを差し替えられる。
            if (m_scriptEngine && m_scriptEngine->ReloadChangedScripts() > 0)
                m_editorCtx->hotReloadFlash = 2.0f;
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
                    m_editorCtx->hotReloadFlash = 2.0f;
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
            { CpuScopeTimer _t(&m_cpuMs[CpuUpdate]); Update(); }
            if (gvOverride) SyncActiveCameraToGlobal();   // Update の後に上書き(編集カメラ操作に勝つ)
            Render();
        }
        catch (const std::exception& ex)
        {
            Logger::Error("フレーム処理でエラー: {}", ex.what());
            // GPU 状態をリセットして次フレームで復帰を試みる。
            // cmdList が open のまま残ると次の BeginFrame の Reset が失敗し続けて
            // 復帰不能ループになるため、必ず AbortFrame で Close しておく。
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

        // 遅延初回表示: 隠れたまま数フレーム描画して絵（ランチャー）が確定してから
        // ウィンドウを出し、スプラッシュを閉じる。表示された瞬間には既に描画済み＝
        // 「白いまま固まって見える/出るタイミングが不安定」が起きない。
        if (m_deferredFirstShow && ++m_warmupFrames >= 3 && !m_loading)
        {
            // ロード中(--project直開き等)はまだ出さない。ロード完了時に
            // UpdateProjectLoad 側が表示+スプラッシュClose を引き継ぐ。
            m_deferredFirstShow = false;
            m_window->Show();       // 最大化。直後の微小リサイズは描画継続中に処理される
            SplashScreen::Close();
        }

        // ★決定論キャプチャ（#31）: 時間依存を固定したまま N フレーム回して履歴を収束させ、
        //   0 になった時点で撮る。sceneRT はここで直接読めるが、バックバッファは Render() の
        //   中でしかコピーできないので pending を立てて次フレームに撮らせる。
        if (m_deterministicCapture && m_deterministicFramesLeft > 0 && --m_deterministicFramesLeft == 0)
        {
            if (m_mcpFinalShot.wantSceneRt)
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

        // screenshot_game_view: このフレームの描画(ゲームカメラ視点)を撮って遅延応答 → 編集カメラ復元。
        if (gvShot)
        {
            std::string serr;
            const std::string p = CaptureSceneScreenshot(serr);
            if (p.empty())
                FailMcp(m_mcpBridge.get(), m_mcpGameViewReply, McpErr::Internal,
                        serr.empty() ? "screenshot failed" : serr);
            else
                CompleteMcp(m_mcpBridge.get(), m_mcpGameViewReply,
                    nlohmann::json{{"path", p}, {"width", m_sceneRT->GetWidth()},
                                   {"height", m_sceneRT->GetHeight()},
                                   {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"}});
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
                                    : CaptureSceneScreenshot(rerr);

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
            CompleteMcp(m_mcpBridge.get(), m_mcpStepReply,
                nlohmann::json{{"stepped", true},
                               {"mode", m_engineMode == EngineMode::Playing ? "Playing" : "Editor"},
                               {"sceneGeneration", m_sceneGeneration}});
            m_mcpStepReply = {};
        }

        // フレームレートリミッター（VSync OFF時のCPU暴走を防止。上限はオプション画面から変更可能）
        if (!m_useVsync && m_fpsLimit > 0.0f)
        {
            using namespace std::chrono;
            auto targetDuration = duration_cast<high_resolution_clock::duration>(
                duration<f64>(1.0 / static_cast<f64>(m_fpsLimit)));
            auto elapsed = high_resolution_clock::now() - m_frameStart;
            auto remaining = targetDuration - elapsed;

            // 1ms以上余裕があればSleepで待つ（CPU負荷軽減）
            if (remaining > milliseconds(1))
            {
                std::this_thread::sleep_for(remaining - milliseconds(1));
            }
            // 残りはスピンウェイトで精密に待つ
            while (high_resolution_clock::now() - m_frameStart < targetDuration)
            {
                _mm_pause();
            }
        }
    }

    timeEndPeriod(1);

    Logger::Info("Main loop ended");
}

void Application::Shutdown()
{
    Logger::Info("Application shutting down...");

    // MCP ブリッジを最優先で停止(worker を join)。これより後で Logger/scene/scriptengine を
    // 破棄するので、ここで止めないと worker がそれらを破棄後に触って data race/UAF になる。
    if (m_mcpBridge) m_mcpBridge.reset();

    // ネットワーク接続を明示的に切る（ENetのソケット/ホストをデバイス解放より前に片付ける）。
    if (m_networkSystem) { m_networkSystem->Disconnect(); m_networkSystem.reset(); }

    // 非同期ロードスレッドの回収
    if (m_loadThread.joinable())
        m_loadThread.join();

    // 非同期 git スレッドの回収（join 前に破棄すると std::terminate）。
    // ログイン待ちポーリングは abort で即抜けさせ、git/gh の子プロセスは終わるまで待つ。
    m_gitAbort.store(true);
    if (m_gitThread.joinable())
        m_gitThread.join();

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
    m_materialAssetManager.reset();
    // 地形レイヤー配列とスプラットテクスチャ（GPU リソース）もデバイス解放より前に明示破棄する。
    m_terrainSrvCache.clear();
    m_terrainLayerSets.reset();
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
    m_gpuParticles.reset();
    // SSAO / コンタクトシャドウ（GPU リソース）をデバイス解放より前に明示破棄
    m_ssaoPass.reset();
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
    if (m_iblBaker)
    {
        if (m_iblReady && m_srvHeap)
            m_srvHeap->FreeBlock(m_iblBaker->GetSrvBlockStart(), m_iblBaker->GetSrvBlockCount());
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
            m_uiAnimRuntime->Update(animReg, dt, animEvents);
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
    // 非同期 git 操作の完了回収
    UpdateGitOp();

    // 「テストクライアント起動」ボタン(フェーズ⑨): フレーム境界で別プロセスを起動
    if (m_editorCtx->netTestLaunchClientRequested)
    {
        m_editorCtx->netTestLaunchClientRequested = false;
        LaunchNetTestClient();
    }

    if (m_engineMode == EngineMode::Editor)
    {
        // エディタモード: C++カメラ操作
        bool rightMouseHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        // ウィンドウが非フォーカスならカメラ操作しない
        bool isForeground = (GetForegroundWindow() == m_window->GetHwnd());

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
            if (GetAsyncKeyState('W') & 0x8000) m_camera->MoveForward(speed);
            if (GetAsyncKeyState('S') & 0x8000) m_camera->MoveForward(-speed);
            if (GetAsyncKeyState('D') & 0x8000) m_camera->MoveRight(speed);
            if (GetAsyncKeyState('A') & 0x8000) m_camera->MoveRight(-speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m_camera->MoveUp(-speed);
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
        bool kbActive = isForeground && !ImGui::GetIO().WantCaptureKeyboard;  // 非フォーカス/テキスト入力中は無効
        if (kbActive && (GetAsyncKeyState(VK_OEM_3) & 1))     // ` キーでトグル
            m_editorCtx->flyMode = !m_editorCtx->flyMode;
        if (m_editorCtx->flyMode && kbActive && (GetAsyncKeyState(VK_ESCAPE) & 1))
            m_editorCtx->flyMode = false;

        if (m_editorCtx->flyMode && kbActive && !m_inputSystem->IsMouseCaptured()
            && !m_editorCtx->view2D)   // 2D中はフライ無効（パン/ズームのみ）
        {
            f32 speed = m_camera->GetMoveSpeed() * dt;
            if (GetAsyncKeyState('W') & 0x8000) m_camera->MoveForward(speed);
            if (GetAsyncKeyState('S') & 0x8000) m_camera->MoveForward(-speed);
            if (GetAsyncKeyState('D') & 0x8000) m_camera->MoveRight(speed);
            if (GetAsyncKeyState('A') & 0x8000) m_camera->MoveRight(-speed);
            if (GetAsyncKeyState('E') & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState('Q') & 0x8000) m_camera->MoveUp(-speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m_camera->MoveUp(-speed);

            // 矢印キーで視点回転（マウス不要）
            f32 rot = 1.5f * dt;  // rad/sec
            f32 yawD = 0.0f, pitchD = 0.0f;
            if (GetAsyncKeyState(VK_LEFT)  & 0x8000) yawD   -= rot;
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) yawD   += rot;
            if (GetAsyncKeyState(VK_UP)    & 0x8000) pitchD += rot;
            if (GetAsyncKeyState(VK_DOWN)  & 0x8000) pitchD -= rot;
            if (yawD != 0.0f || pitchD != 0.0f) m_camera->Rotate(yawD, pitchD);
        }

        // --- 2D ビューモード: WASD / 矢印キーでパン（3D の WASD 移動と同じ操作感で左右上下に動かせる）---
        // 2D は回転 0 固定なので MoveRight/MoveUp はそのままワールド X/Y のパンになる。速度はズーム量に比例。
        // ※ W/E/R のギズモ切替は 2D 中は下のブロックで抑止し、ここでは移動だけにする。
        // 右クリック保持中（マウスキャプチャ中）でも A/D・矢印で動かせるよう capture ゲートは付けない。
        if (m_editorCtx->view2D && kbActive)
        {
            f32 pan = (std::max)(0.5f, m_editorCtx->view2DZoom) * 1.5f * dt;
            if ((GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000)) m_camera->MoveRight(pan);
            if ((GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT)  & 0x8000)) m_camera->MoveRight(-pan);
            if ((GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP)    & 0x8000)) m_camera->MoveUp(pan);
            if ((GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN)  & 0x8000)) m_camera->MoveUp(-pan);
        }

        // Ctrl+S でクイック保存
        if (isForeground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('S') & 1))
        {
            if (m_editorCtx->currentScenePath.empty())
            {
                // パス未設定 → 名前入力ダイアログを開く（保存モード）
                m_editorCtx->showNewSceneDialog = true;
                m_editorCtx->newSceneDialogIsCreate = false;
                std::memset(m_editorCtx->newSceneNameBuf, 0, sizeof(m_editorCtx->newSceneNameBuf));
                strncpy_s(m_editorCtx->newSceneNameBuf, "Untitled", _TRUNCATE);
            }
            else
            {
                SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir());
                ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
                m_editorCtx->hotReloadFlash = 1.5f;
                m_editorLayer->RefreshAssetBrowser();
            }
        }

        // Ctrl+N で新規シーン名入力ダイアログを開く
        if (isForeground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('N') & 1))
        {
            m_editorCtx->showNewSceneDialog = true;
            m_editorCtx->newSceneDialogIsCreate = true;
            std::memset(m_editorCtx->newSceneNameBuf, 0, sizeof(m_editorCtx->newSceneNameBuf));
            strncpy_s(m_editorCtx->newSceneNameBuf, "NewScene", _TRUNCATE);
        }

        // Undo/Redo (Ctrl+Z / Ctrl+Y) + Copy/Paste/Duplicate (Ctrl+C/V/D)
        // ImGui のテキスト入力にフォーカスがある時、ウィンドウが裏にある時はエンティティ操作を抑制
        if (isForeground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && !ImGui::GetIO().WantCaptureKeyboard)
        {
            if (GetAsyncKeyState('Z') & 1)
                m_editorCtx->pendingUndo = true;
            if (GetAsyncKeyState('Y') & 1)
                m_editorCtx->pendingRedo = true;

            // コピー (Ctrl+C) — 選択の最上位ごとにサブツリー（子孫+Lua+コライダー込み）を
            // JSON スナップショットで保持。親子両方選択時の子二重コピーは TopmostRoots が防ぐ
            if (GetAsyncKeyState('C') & 1)
            {
                m_editorCtx->clipboard.clear();
                for (auto e : SceneSerializer::TopmostRoots(*m_scene, m_editorCtx->selectedEntities))
                {
                    std::string snap = SceneSerializer::SerializeSubtree(
                        *m_scene, e, PathResolver::AssetsDir());
                    if (!snap.empty())
                        m_editorCtx->clipboard.push_back(std::move(snap));
                }
            }

            // ペースト (Ctrl+V) — フレーム境界（cmdList 有効時）で生成
            if ((GetAsyncKeyState('V') & 1) && !m_editorCtx->clipboard.empty())
                m_editorCtx->pendingPastes = m_editorCtx->clipboard;

            // 複製 (Ctrl+D) — 全コンポーネントのディープコピー
            if ((GetAsyncKeyState('D') & 1) && m_editorCtx->HasSelection())
            {
                for (auto e : m_editorCtx->selectedEntities)
                    m_editorCtx->pendingDuplications.push_back(e);
            }
        }

        // ギズモモード切替（右クリック中・ImGuiフォーカス中・非フォーカス時は無効）
        if (isForeground && !ImGui::GetIO().WantCaptureKeyboard && !m_inputSystem->IsMouseCaptured())
        {
            // フライモード中・2Dビュー中は W/E/R/T をカメラ移動(パン)に使うのでギズモ切替は抑制
            if (!m_editorCtx->flyMode && !m_editorCtx->view2D)
            {
                if (GetAsyncKeyState('W') & 1) m_editorCtx->gizmoMode = GizmoMode::Translate;
                if (GetAsyncKeyState('E') & 1) m_editorCtx->gizmoMode = GizmoMode::Rotate;
                if (GetAsyncKeyState('R') & 1) m_editorCtx->gizmoMode = GizmoMode::Scale;
                if (GetAsyncKeyState('T') & 1) m_editorCtx->gizmoLocalSpace = !m_editorCtx->gizmoLocalSpace;
            }

            // F2: 編集用の照らし込み。暗い屋内シーンは「見えないから置けない」になるので、
            //     ビューポートにだけ環境光の下限を被せる。シーンには保存しない。
            if (GetAsyncKeyState(VK_F2) & 1)
                m_editorCtx->viewportFill = (m_editorCtx->viewportFill > 0.0f) ? 0.0f : 0.35f;

            // F: 選択エンティティにフォーカス（Unity 風）
            if ((GetAsyncKeyState('F') & 1) && m_editorCtx->HasSelection())
            {
                auto& reg = m_scene->GetRegistry();
                auto sel = m_editorCtx->selectedEntity;
                if (reg.valid(sel) && reg.all_of<Transform>(sel))
                {
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
            }
        }

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

        m_scriptEngine->CallOnUpdate(dt);
        m_scriptEngine->UpdateAttachedScripts(dt);
        m_scriptEngine->UpdateTriggers(dt);   // Trigger（イベント）評価

        // アクティブカメラの Transform をグローバル Camera に同期。
        // 親階層込みのワールド変換で反映するので、親オブジェクトにアタッチした
        // カメラが親の移動・回転に追従する。
        // ★MCP が dx12_set_editor_camera で視点を固定している間は同期しない（#20-6）。
        //   Play 中の絵で look_compare / camera_path を回すための一時上書き。
        if (!m_mcpCameraOverride)
        {
            auto& reg = m_scene->GetRegistry();
            auto camSyncView = reg.view<const CameraComponent>();
            for (auto [e, cam] : camSyncView.each())
            {
                if (!cam.isActive) continue;
                ApplyCameraTransformToGlobal(e);
                break;
            }
        }
    }

    // シーン更新（Animator等）— エディタモードは時間を止める（ボーン行列は維持）
    m_scene->Update(m_engineMode == EngineMode::Playing ? dt : 0.0f);

    // 配置パーティクル放出器（ParticleEmitter）を駆動。
    // エディタでも常時プレビュー（実 dt で放出/前進）し、Play では _active に従う。
    if (m_particleSystem)
    {
        auto& peReg = m_scene->GetRegistry();
        const bool pedPlaying = (m_engineMode == EngineMode::Playing);
        auto peView = peReg.view<ParticleEmitter, Transform>();
        for (auto pe_e : peView)
        {
            auto& pe = peView.get<ParticleEmitter>(pe_e);
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

            DirectX::XMMATRIX w = ComputeWorldMatrix(peReg, pe_e);
            DirectX::XMFLOAT3 pos; DirectX::XMStoreFloat3(&pos, w.r[3]);

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
            m_particleSystem->Emit(p);
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
        m_particleSystem->Update(m_deterministicCapture ? 0.0f : dt);

    // 物理更新（プレイモードのみ）
    if (m_engineMode == EngineMode::Playing && m_physicsSystem->IsInitialized())
    {
        m_physicsSystem->Update(dt, m_scene->GetRegistry());
    }

    // ネットワーク送信処理（物理確定後の座標を使うため直後。フェーズ⑤でスナップショット送信を実装）。
    if (m_engineMode == EngineMode::Playing && m_networkSystem)
    {
        m_networkSystem->PostSimUpdate(dt, m_scene->GetRegistry());
    }

    // 3D 空間オーディオ: リスナー＝カメラ、AudioSource を駆動（Playing のみ）
    if (m_engineMode == EngineMode::Playing && m_audioSystem)
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
                        src.clipPath, wx, wy, wz, src.minDistance, src.maxDistance, src.volume, src.loop);
                else
                    m_audioSystem->PlaySFX(src.clipPath, src.loop);
                src.startedThisPlay = true;
            }
            if (src.runtimeSlot >= 0 && src.spatial)
                m_audioSystem->UpdateSpatialEmitter(src.runtimeSlot, wx, wy, wz);
        }
        m_audioSystem->Update();
    }

    // Trigger の Post や接触 Post を同フレーム内で配信（Playing のみ）。
    if (m_engineMode == EngineMode::Playing)
    {
        m_eventBus.Flush();

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
// ここが「エンティティを走査して PhysicsSystem::RaycastEx を繋ぐ」接着層。
// ---------------------------------------------------------------------------
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

    // PhysicsSystem::RaycastEx を FootIK 側のコールバック形へ包む。
    // ★従来の Raycast は法線を (0,1,0) にフェイクしているので使えない★
    const FootIKRayCast rayFn =
        [this](const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& dir, f32 maxDist,
               DirectX::XMFLOAT3& outPoint, DirectX::XMFLOAT3& outNormal) -> bool
    {
        const RaycastHit hit = m_physicsSystem->RaycastEx(origin, dir, maxDist);
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

void Application::RebuildScene()
{
    m_editorCtx->ClearSelection();
    m_scene->Clear();
    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                        m_srvHeap.get(), cmdList);

    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();

    LoadGameScript();

    ThrowIfFailed(cmdList->Close());
    m_commandQueue->ExecuteCommandList(cmdList);
    m_commandQueue->WaitIdle();
    m_resourceManager->FinishUploads();
    m_frameResources->EndFrame(*m_commandQueue);

    // ホットリロード用タイムスタンプ更新
    {
        std::string reloadPath = PathResolver::GameLuaPath();
        if (std::filesystem::exists(reloadPath))
            m_scriptLastWriteTime = std::filesystem::last_write_time(reloadPath);
    }
}



} // namespace dx12e
