// ===========================================================================
// Application: プロジェクト / バージョン管理 / ゲームビルド
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ===== プロジェクト固有ビルド設定の永続化(<プロジェクトルート>/build_settings.json) =====
// .dx12proj は Project::Save が既知フィールドだけで全体を書き直すため、そこへ相乗りすると
// 他の保存経路(lastOpenedScene 更新等)で消える。独立ファイルでプロジェクト単位に保存/復元する。
static void LoadProjectBuildConfig(EditorContext& ctx, const std::string& projectRoot)
{
    ctx.buildConfig = {};   // まず既定へ(前プロジェクトの設定を引き継がない)
    std::ifstream ifs(std::filesystem::path(projectRoot) / "build_settings.json");
    if (!ifs.is_open()) return;
    try
    {
        nlohmann::json j;
        ifs >> j;
        strncpy_s(ctx.buildConfig.title, j.value("title", std::string("Game")).c_str(), _TRUNCATE);
        ctx.buildConfig.width      = std::clamp(j.value("width", 1280), 320, 7680);
        ctx.buildConfig.height     = std::clamp(j.value("height", 720), 240, 4320);
        ctx.buildConfig.startScene = j.value("startScene", std::string());
        ctx.buildConfig.outputDir  = j.value("outputDir", std::string());
        ctx.buildConfig.openFolderAfterBuild = j.value("openFolderAfterBuild", true);
    }
    catch (const std::exception& ex)
    {
        Logger::Warn("build_settings.json の読み込みに失敗しました(既定値を使用): {}", ex.what());
        ctx.buildConfig = {};
    }
}

static void SaveProjectBuildConfig(const EditorContext& ctx, const std::string& projectRoot)
{
    if (projectRoot.empty()) return;
    nlohmann::json j = {
        {"title",      ctx.buildConfig.title},
        {"width",      ctx.buildConfig.width},
        {"height",     ctx.buildConfig.height},
        {"startScene", ctx.buildConfig.startScene},
        {"outputDir",  ctx.buildConfig.outputDir},
        {"openFolderAfterBuild", ctx.buildConfig.openFolderAfterBuild},
    };
    std::ofstream ofs(std::filesystem::path(projectRoot) / "build_settings.json");
    if (ofs.is_open()) ofs << j.dump(2) << '\n';
}

void Application::BeginProjectLoad(const ProjectInfo& info, bool isNew)
{
    namespace fs = std::filesystem;
    // 直前のスレッドが残っていれば回収
    if (m_loadThread.joinable())
        m_loadThread.join();

    m_loadInfo            = info;
    m_loadIsNew           = isNew;
    m_loading             = true;
    m_showLauncher        = false;
    m_loadProjectStarted  = false;
    m_loadSceneWaitFrames = 0;
    m_loadSpinTime        = 0.0f;
    m_loadThreadDone      = false;
    m_loadStatus          = isNew ? "プロジェクトを作成中..." : "プロジェクトを読み込み中...";

    // プロジェクト固有のビルド設定(ゲーム名/解像度/出力先)を復元。無ければ既定へリセット。
    if (m_editorCtx)
        LoadProjectBuildConfig(*m_editorCtx, info.rootDir);

    // ローディングのくるくるは専用スレッドのスプラッシュ窓に任せる(起動時と同じ仕組み)。
    // メインスレッドがシーンロードやマテリアルサムネイル生成(同期テクスチャデコード)で
    // ブロックしてもアニメが止まらない(ImGui側の演出はフレームが止まると固まるため)。
    SplashScreen::Show("DX12 Engine",
                       std::string("v") + kEngineVersion,
                       PathResolver::AssetsDir() + "editor/icons/logo.png");
    SplashScreen::SetStatus(m_loadStatus);

    // ロードが終わるまでメインウィンドウを隠す。ロード中に古いシーンやテンプレートが
    // 一瞬見えるのを防ぐ(表示はスプラッシュのみ。完了時に UpdateProjectLoad が再表示する)。
    if (m_window && m_window->GetHwnd() && IsWindowVisible(m_window->GetHwnd()))
        ShowWindow(m_window->GetHwnd(), SW_HIDE);

    if (isNew)
    {
        // ディスク作成（フォルダ生成・テンプレ書き出し）はワーカースレッドで
        m_loadThreadRunning = true;
        ProjectInfo copy = info;
        m_loadThread = std::thread([this, copy]()
        {
            Project::CreateDefaultStructure(copy);
            std::string projPath =
                (std::filesystem::path(copy.rootDir) / (copy.name + ".dx12proj")).string();
            Project::Save(copy, projPath);
            ProjectManager::AddToRecents(copy);
            m_loadThreadDone = true;
        });
    }
    else
    {
        // 既存プロジェクト: 重い CPU 処理は無い（シーンの GPU ロードは本スレッドで）
        m_loadThreadRunning = false;
        m_loadThreadDone    = true;
    }
}

void Application::UpdateProjectLoad(f32 dt)
{
    if (!m_loading) return;
    m_loadSpinTime += dt;

    // フェーズ1: 作成スレッドの完了待ち
    if (m_loadThreadRunning)
    {
        if (!m_loadThreadDone.load()) return;  // まだ作成中（スピナー回し続ける）
        if (m_loadThread.joinable()) m_loadThread.join();
        m_loadThreadRunning = false;
    }

    // フェーズ2: LoadProject を一度だけ発火（次フレームの Render で実シーンロード）
    if (!m_loadProjectStarted)
    {
        m_loadStatus = "シーンを読み込み中...";
        SplashScreen::SetStatus(m_loadStatus);
        LoadProject(m_loadInfo);
        m_loadProjectStarted  = true;
        m_loadSceneWaitFrames = 2;  // pending* が Render で消化されるまで猶予
        return;
    }

    // フェーズ3: シーンロード（pending* と段階ロードのジョブ）が消化されたら次へ
    // ローディング画面とスプラッシュに「今どのアセットを読んでいるか」を出す。
    // 総数と件数だけだと、1 件に数秒かかる初回の BC 圧縮で固まったように見える。
    if (m_sceneLoadJob)
    {
        const SceneLoadJob& job = *m_sceneLoadJob;
        char buf[320];
        if (!job.assets.empty())
        {
            const size_t slash = job.current.find_last_of('/');
            const char* base = (slash == std::string::npos)
                             ? job.current.c_str() : job.current.c_str() + slash + 1;
            snprintf(buf, sizeof(buf), "アセットを読み込み中... (%zu / %zu)  %s",
                     job.next, job.assets.size(), base);
        }
        else
        {
            snprintf(buf, sizeof(buf), "シーンを構築中...");
        }
        m_loadStatus = buf;
        SplashScreen::SetStatus(m_loadStatus);
    }

    bool pendingScene = !m_editorCtx->pendingLoadPath.empty() || m_editorCtx->pendingNewScene
                     || m_sceneLoadJob != nullptr;
    if (m_loadSceneWaitFrames > 0) --m_loadSceneWaitFrames;
    if (m_loadSceneWaitFrames == 0 && !pendingScene)
    {
        // フェーズ4: マテリアル球体サムネイルの事前生成が終わるまでローディング画面を維持する
        // (実際の生成は Render() 内の RenderPendingThumbnails が毎フレーム数件ずつ進める。
        //  エディタが開いてからアセットブラウザで初表示した瞬間に重くなるのを防ぐ)。
        if (m_materialEditorPanel)
        {
            const size_t remaining =
                m_materialEditorPanel->GetPreviewRenderer().GetPendingThumbnailCount();
            if (remaining > 0)
            {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "マテリアルを読み込み中... (%zu / %zu)",
                         m_matThumbPreloadTotal - remaining, m_matThumbPreloadTotal);
                m_loadStatus = buf;
                SplashScreen::SetStatus(m_loadStatus);
                return;
            }
        }

        m_loading            = false;
        m_loadProjectStarted = false;
        m_editorCtx->buildCompleteFlash = 1.5f;

        // ロード完了: 隠していたメインウィンドウを出してからスプラッシュを閉じる
        // (順序を逆にすると一瞬何も表示されない空白ができる)。
        // 起動直後の遅延表示(--project直開き等でまだ未表示)もここで役目を引き継ぐ。
        if (m_window) m_window->Show();
        m_deferredFirstShow = false;
        SplashScreen::Close();
    }
}

void Application::RunGitAsync(const std::string& label, std::function<GitResult()> task, bool isLogin)
{
    if (m_gitOpRunning) return;                         // 同時実行は1本だけ（ボタンも無効化済み）
    if (m_gitThread.joinable()) m_gitThread.join();     // 前回スレッドを回収してから再利用

    m_gitOpRunning = true;
    m_gitOpIsLogin = isLogin;
    m_gitOpLabel   = label;
    m_gitOpStatus  = GitOpStatus::Running;
    m_gitSpin      = 0.0f;
    m_gitOpDone.store(false);

    // task はワーカー上で git/gh の子プロセスのみ叩く（ImGui/シーン/GPU には触れない）。
    // 結果を m_gitPending* に書いてから done を立てる＝メインは done 観測後にだけ読む。
    m_gitThread = std::thread([this, task = std::move(task)]() {
        GitResult r = task();
        m_gitPendingOutput = std::move(r.output);
        m_gitPendingOk     = r.ok();
        m_gitOpDone.store(true);                         // RELEASE: 結果を最後に公開
    });
}

void Application::UpdateGitOp()
{
    if (!m_gitOpRunning) return;
    m_gitSpin += m_gameClock.GetDeltaTime();
    if (!m_gitOpDone.load()) return;                     // ACQUIRE: まだワーカー実行中
    if (m_gitThread.joinable()) m_gitThread.join();

    if (m_gitOpIsLogin)
    {
        // ログイン/ユーザー確認: バナーは出さず GitHub 行(●/○)で表す。出力ログも汚さない。
        m_ghUser        = m_gitPendingOutput;            // 空=未ログイン
        m_ghUserChecked = true;
        m_gitOpIsLogin  = false;
        m_gitOpStatus   = GitOpStatus::None;
    }
    else
    {
        m_gitOpStatus = m_gitPendingOk ? GitOpStatus::Success : GitOpStatus::Failure;
        m_gitOutput   = m_gitPendingOutput.empty()
                      ? (m_gitPendingOk ? "完了" : "失敗（出力なし）")
                      : m_gitPendingOutput;
    }

    m_gitForceRefresh = true;                            // ブランチ/リモート/ahead-behind を取り直す
    m_gitOpRunning    = false;
}

void Application::RenderLoadingOverlay()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.09f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##LoadingOverlay", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

    // 回転スピナー（円弧をぐるぐる）
    const float r = 34.0f;
    const int   segs = 28;
    float t = m_loadSpinTime * 3.2f;
    for (int i = 0; i < segs; ++i)
    {
        float a0 = t + (float)i / segs * 6.2831853f;
        float a1 = t + (float)(i + 1) / segs * 6.2831853f;
        float alpha = (float)i / segs;  // フェードする尾
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.39f, 0.58f, 0.93f, alpha));
        dl->AddLine(ImVec2(c.x + cosf(a0) * r, c.y + sinf(a0) * r),
                    ImVec2(c.x + cosf(a1) * r, c.y + sinf(a1) * r), col, 5.0f);
    }

    // ステータステキスト（中央寄せ）
    const char* msg = m_loadStatus.c_str();
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r + 24.0f),
                IM_COL32(235, 235, 235, 255), msg);

    if (!m_loadInfo.name.empty())
    {
        ImVec2 ns = ImGui::CalcTextSize(m_loadInfo.name.c_str());
        dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y + r + 48.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), m_loadInfo.name.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Application::RenderWhatsNewPopup()
{
    if (!m_showWhatsNew) return;
    namespace th = dx12e::theme;

    const char* kId = "更新内容###whatsnew";
    if (!m_whatsNewOpened) { ImGui::OpenPopup(kId); m_whatsNewOpened = true; }

    // マルチビューポート有効なので、明示しないとこのモーダルが独立OSウィンドウ化し、
    // 「画面に出ていないのに入力だけ塞ぐ」状態になる（ギズモ消失バグと同じ罠）。
    // 併せて AlwaysAutoResize は使わない: 本文が画面より縦に長いと「閉じる」が画面外に出て詰む。
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const float wnH = (vp->WorkSize.y * 0.8f < 720.0f) ? vp->WorkSize.y * 0.8f : 720.0f;
    ImGui::SetNextWindowSize(ImVec2(760.0f, wnH), ImGuiCond_Appearing);
    bool open = true;  // ESC で閉じられるように p_open を渡す（保険）
    if (ImGui::BeginPopupModal(kId, &open, ImGuiWindowFlags_NoSavedSettings))
    {
        // 見づらい要望対応: 専用フォントは追加せず、既存フォントを SetWindowFontScale で拡大
        // （ToolbarPanel.cpp のエラーモーダルと同じやり方）。
        ImGui::SetWindowFontScale(1.35f);
        ImGui::PushStyleColor(ImGuiCol_Text, th::Accent);
        ImGui::TextUnformatted(kWhatsNewTitle);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Separator();
        ImGui::Spacing();
        // 本文はスクロール領域に入れる。ボタン1行分を残しておけば「閉じる」は必ず見える。
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        if (ImGui::BeginChild("##whatsnew_body", ImVec2(0.0f, -footer), false))
        {
            ImGui::SetWindowFontScale(1.18f);
            ImGui::TextWrapped("%s", kWhatsNewBody);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("閉じる", ImVec2(160.0f, 0.0f)) || !open)
        {
            // この版は表示済みとして記録 → 次回以降は版が変わるまで出さない。
            WriteShownVersion(kEngineVersion);
            m_showWhatsNew = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::LoadProject(const ProjectInfo& info)
{
    namespace fs = std::filesystem;
    m_projectInfo = info;

    // settings.json はプロジェクトごとなので読み直す
    m_persistLoaded = false;

    // git 状態をプロジェクトごとに再評価
    m_gitChecked   = false;
    m_gitOutput.clear();

    // 「スキップ（デフォルト）」= 組み込みパスのまま。すでに既定シーンが読み込まれている。
    if (info.rootDir.empty())
    {
        Logger::Info("Using built-in default project (no project root)");
        if (m_window) m_window->SetTitle(L"DX12 Engine");
        return;
    }

    // 1) パスをプロジェクト配下へ再ポイント（shaders はエンジン側を維持）
    PathResolver::SetProjectRoot(info.rootDir);

    // プロジェクトの settings.json で描画オプションを上書き（A/B 計測用。既定 ON）。
    // ※ SetProjectRoot の後でないと PersistPath がエンジン側を指してしまう。
    m_instancingEnabled = PersistGet("render_instancing", 1.0) != 0.0;
    Logger::Info("自動インスタンシング: {}", m_instancingEnabled ? "ON" : "OFF");

    // クラスタードライティング（Forward+）。0 で「先頭 64 灯を総当たり」フォールバックへ倒す。
    m_clusteredEnabled = PersistGet("render_clustered", 1.0) != 0.0;
    Logger::Info("クラスタードライティング: {}", m_clusteredEnabled ? "ON" : "OFF");

    // 深度プリパスの単独強制（計画10 A2）。既定 OFF。
    m_forceDepthPrepass = PersistGet("render_depth_prepass", 0.0) != 0.0;
    if (m_forceDepthPrepass) Logger::Info("深度プリパス: 強制 ON（render_depth_prepass=1）");

    // 内部解像度スケール（#16）。1.0 で従来と完全に同じ絵。0.5 なら 3D だけ半解像度で
    // 描いて表示解像度へ引き伸ばす（UI / ImGui は表示解像度のまま鮮明）。
    {
        const f32 s = std::clamp(static_cast<f32>(PersistGet("render_scale", 1.0)), 0.25f, 1.0f);
        if (s != m_renderScale) { m_renderScale = s; m_renderResFlush = true; }
        Logger::Info("レンダー解像度スケール: {:.2f}", m_renderScale);
    }

    // BC7/BC5 テクスチャ圧縮（settings.json "texture_compression"、既定 1）。
    //   0 = 無圧縮（ハードウェア/ツール差で絵が壊れた時に従来の R8G8B8A8 へ戻す逃げ道）
    //   1 = 高速（BC7 は mode6 のみ。実測 1024² で 1.1 秒）
    //   2 = 高品質（BC7 全モード探索。実測 1024² で 37 秒。初回だけ待てるなら）
    TextureLoader::SetCompressionMode(static_cast<int>(PersistGet("texture_compression", 1.0)));
    {
        const int q = static_cast<int>(TextureLoader::GetCompressionMode());
        Logger::Info("テクスチャ BC 圧縮: {}", q == 0 ? "OFF" : (q == 1 ? "ON(高速)" : "ON(高品質)"));
    }

    // 1.5) プロジェクト独自シェーダー(上書き/自作)を再走査。切替前の PSO が残っている可能性があるので
    //      WaitIdle 後に全リロードキーを差分無視で作り直す(Poll() の逐次差分検知とは別経路)。
    if (m_shaderManager)
    {
        m_shaderManager->OnProjectRootChanged();
        if (m_commandQueue)
            m_commandQueue->WaitIdle();
        m_shaderManager->DispatchReloadHandlers(m_shaderManager->AllKnownReloadKeys());
    }

    // 2) パス依存サブシステムを更新
    m_audioSystem->SetAssetsDir(PathResolver::AssetsDir());
    m_scriptEngine->SetAssetsDir(PathResolver::AssetsDir());
    if (m_networkSystem)
    {
        // 起動時はエンジン側 assets の network.json を読んでいるので、
        // プロジェクトの assets/network.json で読み直す(無ければ既定値)。
        NetworkConfig cfg;
        cfg.Load(PathResolver::AssetsDir() + "network.json");
        m_networkSystem->SetConfig(cfg);
    }
    if (m_editorLayer)
        m_editorLayer->SetAssetRoots(PathResolver::AssetsDir(), PathResolver::ScriptsDir());

    // 3) プロジェクトの game.lua を読み込み直す
    LoadGameScript();
    {
        std::string scriptPath = PathResolver::GameLuaPath();
        if (fs::exists(scriptPath))
            m_scriptLastWriteTime = fs::last_write_time(scriptPath);  // ホットリロード用（エディタ）
    }

    // 4) 開始シーンを決定してロード（フレーム境界で実行）
    std::string sceneRel = info.defaultScene.empty() ? "scenes/default.json" : info.defaultScene;
    std::string sceneFull = PathResolver::AssetsDir() + sceneRel;
    m_currentSceneRel = sceneRel;

    if (fs::exists(sceneFull))
    {
        // 既存シーンを次フレームで安全にロード
        m_editorCtx->pendingLoadPath       = sceneFull;
        m_editorCtx->pendingLoadSkipConfirm = true;   // ロード中はモーダルを描けない（上の解説参照）
    }
    else
    {
        // 新規プロジェクト: グリッド + 平行光源のスターターシーンを生成して保存
        fs::create_directories(fs::path(sceneFull).parent_path());
        m_editorCtx->pendingNewScenePath = sceneFull;
        m_editorCtx->pendingNewScene = true;
    }

    // 5) ウィンドウタイトルにプロジェクト名
    //    info.name は UTF-8。byte 単位の widen（std::wstring(begin,end)）だと
    //    日本語等のマルチバイトが文字化けするので CP_UTF8 で正しく変換する。
    if (m_window)
    {
        std::wstring title = L"DX12 Engine - ";
        if (!info.name.empty())
        {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, info.name.c_str(),
                                           static_cast<int>(info.name.size()), nullptr, 0);
            if (wlen > 0)
            {
                std::wstring wname(static_cast<size_t>(wlen), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, info.name.c_str(),
                                    static_cast<int>(info.name.size()), wname.data(), wlen);
                title += wname;
            }
        }
        m_window->SetTitle(title);
    }

    // 6) マテリアル球体サムネイルの事前生成キューを積む。
    //    エディタ使用中にアセットブラウザで初めて表示した瞬間にテクスチャロードで
    //    ヒッチが出ないよう、プロジェクトロード中(UpdateProjectLoad フェーズ4)に全部済ませる。
    if (m_materialEditorPanel)
    {
        const size_t queued =
            m_materialEditorPanel->GetPreviewRenderer().ScanAllMaterials(PathResolver::AssetsDir());
        m_matThumbPreloadTotal = m_materialEditorPanel->GetPreviewRenderer().GetPendingThumbnailCount();
        if (queued > 0)
            Logger::Info("マテリアルサムネイル事前生成: {} 件をキューに追加", queued);
    }

    Logger::Info("Project loaded: {} ({})", info.name, info.rootDir);
}

void Application::SaveCurrentProject()
{
    namespace fs = std::filesystem;

    // 現在シーンを保存
    if (!m_editorCtx->currentScenePath.empty())
    {
        if (SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir()))
            MarkSceneClean();
        ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
    }

    // .dx12proj を保存（プロジェクトを開いている場合のみ）
    if (!m_projectInfo.rootDir.empty())
    {
        m_projectInfo.defaultScene    = m_currentSceneRel.empty() ? m_projectInfo.defaultScene : m_currentSceneRel;
        m_projectInfo.lastOpenedScene = m_currentSceneRel;
        std::string projPath = (fs::path(m_projectInfo.rootDir) / (m_projectInfo.name + ".dx12proj")).string();
        Project::Save(m_projectInfo, projPath);
    }
    m_editorCtx->buildCompleteFlash = 2.0f;
    Logger::Info("Project saved");
}

void Application::RenderProjectWindow()
{
    ImGui::Begin("Project");

    if (m_projectInfo.rootDir.empty())
    {
        ImGui::TextDisabled("(組み込みデフォルトプロジェクト)");
    }
    else
    {
        ImGui::Text("名前: %s", m_projectInfo.name.c_str());
        ImGui::TextWrapped("場所: %s", m_projectInfo.rootDir.c_str());
        ImGui::TextWrapped("シーン: %s", m_currentSceneRel.c_str());
    }
    ImGui::Separator();

    auto icon = [](u64 h, float s) { if (h) { ImGui::Image(static_cast<ImTextureID>(h), ImVec2(s, s)); ImGui::SameLine(); } };

    icon(m_icons.save, 22);
    if (ImGui::Button("プロジェクトを保存", ImVec2(-1, 30)))
        SaveCurrentProject();

    icon(m_icons.newProject, 22);
    if (ImGui::Button("新規プロジェクト...", ImVec2(-1, 0)))
    {
        ProjectInfo created;
        if (ProjectManager::NewProjectDialog(created, m_window->GetHwnd()))
            BeginProjectLoad(created, /*isNew=*/true);
    }
    icon(m_icons.openProject, 22);
    if (ImGui::Button("プロジェクトを開く...", ImVec2(-1, 0)))
    {
        ProjectInfo opened;
        if (ProjectManager::OpenProjectDialog(opened, m_window->GetHwnd()))
            BeginProjectLoad(opened, /*isNew=*/false);
    }
    icon(m_icons.recent, 22);
    if (ImGui::Button("ランチャーに戻る", ImVec2(-1, 0)))
        m_editorCtx->pendingCloseProject = true;   // 未保存の確認を挟むため直接は遷移しない

    ImGui::End();
}

bool Application::HandleWindowCloseRequest()
{
    if (m_isGameMode) return true;              // GameRuntime.exe: 従来通りそのまま終了

    if (m_showLauncher || m_loading) return true; // ランチャー表示中/ロード中はそのまま終了して良い

    if (m_engineMode == EngineMode::Playing)
    {
        // Play 中にいきなり閉じようとした→まず停止するだけに留める（誤操作で未保存の作業が消えるのを防ぐ）。
        // もう一度 X を押せばプロジェクトを閉じてランチャーへ戻る（上の分岐に入る）。
        m_pendingMode = EngineMode::Editor;
        m_modeChangeRequested = true;
        return false;
    }

    // プロジェクトを開いた状態で X → ファイルメニュー「プロジェクトを閉じる」と同じ扱い。
    // ★ここで直接 m_showLauncher を立てない。立てた瞬間に EditorLayer が呼ばれなくなり、
    //   未保存の確認モーダルを出す場所が無くなる（＝黙って作業が消える）。
    //   pendingCloseProject へ回して、フレーム内の消化側で確認してから遷移させる。
    m_editorCtx->pendingCloseProject = true;
    return false;
}

void Application::RenderVersionControlWindow()
{
    // 非表示タブ/折りたたみ時は中身を一切実行しない（git の外部プロセス起動を毎フレーム回さない）。
    // 表示名は "Git 変更" だが ImGui ID は従来通り（### 以降）にしてドッキング配置を維持する。
    if (!ImGui::Begin("Git 変更###Version Control (Git)"))
    {
        ImGui::End();
        return;
    }

    // インストール操作が完了していたら再チェックさせる（このパネルが表示されているフレームでのみ検出）
    if (m_gitInstallPending && !m_gitOpRunning)
    {
        m_gitInstallPending = false;
        m_gitChecked = false;
    }

    // git/gh の存在チェック（1 度だけ、インストール完了時は上で再度リセットされる）
    if (!m_gitChecked)
    {
        m_gitAvailable = GitIntegration::IsGitAvailable();
        m_ghAvailable  = GitIntegration::IsGhAvailable();
        m_gitChecked   = true;
    }

    namespace th = dx12e::theme;

    // 実行中スピナー / 成功・失敗バナー。結果が出るまで残るので「押したのに反映されたか分からん」を無くす。
    auto statusBanner = [&]()
    {
        if (m_gitOpStatus == GitOpStatus::Running)
        {
            const char frames[] = { '|', '/', '-', '\\' };
            char sp = frames[(int)(m_gitSpin * 10.0f) & 3];
            ImGui::PushStyleColor(ImGuiCol_Text, th::Accent);
            ImGui::Text("%c %s 実行中...", sp, m_gitOpLabel.c_str());
            ImGui::PopStyleColor();
        }
        else if (m_gitOpStatus == GitOpStatus::Success)
            ImGui::TextColored(th::Good, "✓ %s 成功", m_gitOpLabel.c_str());
        else if (m_gitOpStatus == GitOpStatus::Failure)
        {
            // pull 等がコンフリクトで止まっただけなら「失敗」ではなく専用の案内に倒す
            // （下のコンフリクト一覧セクションで解消操作ができる）。
            if (m_gitMergeInProgress && !m_gitConflicts.empty())
                ImGui::TextColored(th::Warn, "⚠ %s でコンフリクトが発生したで。下の一覧から解消してや", m_gitOpLabel.c_str());
            else
                ImGui::TextColored(th::Bad, "✗ %s 失敗 (下の出力ログを確認してや)", m_gitOpLabel.c_str());
        }
    };

    // 出力ログ（既定は畳む。エラー時だけ開いて確認）
    auto outputLog = [&]()
    {
        if (m_gitOutput.empty()) return;
        if (ImGui::CollapsingHeader("出力ログ"))
        {
            ImGui::BeginChild("##gitout", ImVec2(0, 120), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(m_gitOutput.c_str());
            ImGui::EndChild();
        }
    };

    if (m_projectInfo.rootDir.empty())
    {
        ImGui::TextWrapped("プロジェクトを開く/作成すると Git を使えるで。");
        ImGui::End();
        return;
    }
    if (!m_gitAvailable)
    {
        ImGui::TextColored(th::Bad, "✗ git が見つからへん。");
        ImGui::BeginDisabled(m_gitOpRunning);
        if (ImGui::Button("Git をインストール"))
        {
            m_gitOutput = "Git をインストール中...（winget があれば自動、無ければブラウザでダウンロード"
                          "ページを開くで。別ウィンドウが出たら指示に従ってや）";
            m_gitInstallPending = true;
            RunGitAsync("Git インストール", [this]{ return GitIntegration::InstallGit(m_gitAbort); });
        }
        ImGui::EndDisabled();
        statusBanner();
        outputLog();
        ImGui::End();
        return;
    }

    const std::string& root = m_projectInfo.rootDir;
    const bool busy = m_gitOpRunning;

    // 新規リポジトリ名の初期値（未入力なら一度だけプロジェクト名で埋める。以降は編集を尊重）
    if (m_gitNewRepoNameBuf[0] == '\0' && !m_projectInfo.name.empty())
        strncpy_s(m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size(),
                  m_projectInfo.name.c_str(), _TRUNCATE);

    // 状態の再取得（ローカル git のみで軽い。開いた瞬間と操作完了直後＋手動「更新」だけ＝定期ヒッチ無し）。
    if (ImGui::IsWindowAppearing() || m_gitForceRefresh)
    {
        m_gitForceRefresh = false;
        m_gitRepoCache = GitIntegration::IsRepo(root);
        m_gitAhead = m_gitBehind = -1;
        if (m_gitRepoCache)
        {
            m_gitBranchCache = GitIntegration::CurrentBranch(root);
            m_gitRemoteCache = GitIntegration::RemoteUrl(root);
            m_gitBranches    = GitIntegration::ListBranches(root);
            m_gitChanges     = GitIntegration::ChangedFiles(root);
            m_gitMergeInProgress = GitIntegration::IsMergeInProgress(root);
            m_gitConflicts       = GitIntegration::ConflictedFiles(root);
            // upstream に対する未取得/未送信コミット数（VS の ↓/↑）。upstream 無しは失敗→-1のまま。
            auto rl = GitIntegration::RunGit(root, "rev-list --left-right --count @{upstream}...HEAD");
            int behind = 0, ahead = 0;
            if (rl.ok() && sscanf_s(rl.output.c_str(), "%d %d", &behind, &ahead) == 2)
            { m_gitBehind = behind; m_gitAhead = ahead; }
        }
        else
        {
            m_gitBranchCache.clear(); m_gitRemoteCache.clear(); m_gitBranches.clear(); m_gitChanges.clear();
            m_gitMergeInProgress = false; m_gitConflicts.clear();
        }
    }

    auto icon = [](u64 h, float s) { if (h) { ImGui::Image(static_cast<ImTextureID>(h), ImVec2(s, s)); ImGui::SameLine(); } };

    // GitHub アカウント行（ログイン状態 + ログインボタン）。リポジトリの有無に関わらず使うので
    // 共通化（未初期化の空状態でも、初回からログイン導線を出すため）。
    auto renderGitHubAccountRow = [&]()
    {
        if (!m_ghUserChecked && !busy)
        {
            m_ghUserChecked = true;
            RunGitAsync("GitHub確認", []{
                GitResult r; r.output = GitIntegration::GitHubUser(); r.exitCode = 0; return r;
            }, /*isLogin*/ true);
        }
        ImGui::AlignTextToFramePadding();
        if (!m_ghUser.empty())
            ImGui::TextColored(th::Good, "● @%s", m_ghUser.c_str());
        else
        {
            ImGui::TextColored(th::Warn, "○ 未ログイン");
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("GitHub にログイン"))
            {
                m_gitOutput = "別ウィンドウでブラウザ認証してや。完了したら自動で反映されるで。";
                // gh auth login --web の子プロセス終了をそのまま待つ＝ブラウザ承認した瞬間に
                // 検知できる（ポーリングより速い。詳細は GitIntegration::LoginAndWait 参照）。
                RunGitAsync("GitHubログイン待ち",
                    [this]{ return GitIntegration::LoginAndWait(m_gitAbort); }, /*isLogin*/ true);
            }
            ImGui::EndDisabled();
        }
    };

    // GitHub に新規リポジトリ作成 → push。needInit=true ならローカル未初期化の状態から面倒見る
    // （「初期化」という別操作を挟まず、「リポジトリ作成」一発で完結させるため）。
    // repoName が空ならプロジェクト名にフォールバック。
    auto createGitHubRepo = [&](bool isPrivate, bool needInit, const std::string& commitMsg,
                                 const std::string& repoName)
    {
        SaveCurrentProject();
        std::string n = repoName.empty() ? m_projectInfo.name : repoName, m = commitMsg;
        RunGitAsync(isPrivate ? "リポジトリ作成(private)" : "リポジトリ作成(public)",
            [root, n, m, isPrivate, needInit]{
                if (needInit)
                {
                    auto i = GitIntegration::Init(root);
                    if (!i.ok()) return i;
                }
                auto c = GitIntegration::CommitAll(root, m);
                bool nothingStaged = GitIntegration::RunGit(root, "diff --cached --quiet").ok();
                if (!c.ok() && !nothingStaged) return c;   // 本当のコミット失敗 → 作成せず失敗
                auto r = GitIntegration::CreateGitHubRepo(root, n, isPrivate);
                r.output = c.output + "\n----\n" + r.output;
                return r;
            });
    };

    // ================= リポジトリ未初期化 =================
    if (!m_gitRepoCache)
    {
        ImGui::TextWrapped("このプロジェクトはまだ Git リポジトリやないで。");
        ImGui::Spacing();
        statusBanner();
        ImGui::Spacing();

        if (m_ghAvailable)
        {
            renderGitHubAccountRow();
            ImGui::Spacing();

            ImGui::TextDisabled("リポジトリ名");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##reponame", m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size());

            ImGui::BeginDisabled(busy || m_ghUser.empty() || m_gitNewRepoNameBuf[0] == '\0');
            icon(m_icons.github, 22);
            if (ImGui::Button("GitHub にリポジトリを作成 (Public)", ImVec2(-1, 32)))
                createGitHubRepo(/*isPrivate=*/false, /*needInit=*/true, "Initial commit",
                                  m_gitNewRepoNameBuf.data());
            icon(m_icons.github, 22);
            if (ImGui::Button("GitHub にリポジトリを作成 (Private)", ImVec2(-1, 32)))
                createGitHubRepo(/*isPrivate=*/true, /*needInit=*/true, "Initial commit",
                                  m_gitNewRepoNameBuf.data());
            ImGui::EndDisabled();
            if (m_ghUser.empty())
                ImGui::TextDisabled("↑ 先に GitHub にログインしてや");

            ImGui::Spacing();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("ローカルだけで管理する（GitHub には後で公開）"))
                RunGitAsync("初期化", [root]{ return GitIntegration::Init(root); });
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::BeginDisabled(busy);
            icon(m_icons.git, 22);
            if (ImGui::Button("Git リポジトリを初期化", ImVec2(-1, 32)))
                RunGitAsync("初期化", [root]{ return GitIntegration::Init(root); });
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("クローン");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##cloneurl", "https://github.com/owner/repo.git",
                                 m_gitCloneBuf.data(), m_gitCloneBuf.size());
        ImGui::BeginDisabled(busy || m_gitCloneBuf[0] == '\0');
        if (ImGui::Button("このプロジェクトの隣にクローン", ImVec2(-1, 0)))
        {
            std::string url    = m_gitCloneBuf.data();
            std::string parent = std::filesystem::path(root).parent_path().string();
            RunGitAsync("クローン", [url, parent]{
                std::string outDir;
                return GitIntegration::Clone(url, parent, outDir);
            });
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        outputLog();
        ImGui::End();
        return;
    }

    // ================= リポジトリあり（VS「Git 変更」風レイアウト）=================
    const std::string& branch = m_gitBranchCache;
    const std::string& remote = m_gitRemoteCache;
    const float fh = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemSpacing.x;

    // ---- 行1: ブランチ コンボ（全幅）。ドロップダウン内で新規ブランチも作れる ----
    {
        ImGui::BeginDisabled(busy);
        const char* curBr = branch.empty() ? "(未コミット)" : branch.c_str();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##branch", curBr))
        {
            for (const auto& b : m_gitBranches)
                if (ImGui::Selectable(b.c_str(), b == branch) && b != branch)
                {
                    std::string target = b;
                    RunGitAsync("ブランチ切替", [root, target]{ return GitIntegration::CheckoutBranch(root, target); });
                }
            ImGui::Separator();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##nb", "+ 新規ブランチ名 → Enter",
                    m_gitNewBranchBuf.data(), m_gitNewBranchBuf.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue) && m_gitNewBranchBuf[0] != '\0')
            {
                std::string nb = m_gitNewBranchBuf.data();
                RunGitAsync("ブランチ作成", [root, nb]{ return GitIntegration::CreateBranch(root, nb); });
                m_gitNewBranchBuf.fill('\0');
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    // ---- 行2: GitHub アカウント ----
    if (m_ghAvailable)
        renderGitHubAccountRow();

    // ---- リポジトリ URL（リモートがある間は常時表示。クリックでブラウザを開く）----
    if (!remote.empty())
    {
        std::string webUrl = GitIntegration::ToWebUrl(remote);
        if (!webUrl.empty() && ImGui::SmallButton(("🔗 " + webUrl).c_str()))
            ShellExecuteA(nullptr, "open", webUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ---- 行3: 同期ツールバー（更新 / ↑↓ / フェッチ / プル↓ / プッシュ↑）----
    {
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("更新")) m_gitForceRefresh = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("変更とブランチ状態を取り直す");
        if (!remote.empty())
        {
            ImGui::SameLine(0, sp * 2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("↑%d ↓%d", m_gitAhead < 0 ? 0 : m_gitAhead, m_gitBehind < 0 ? 0 : m_gitBehind);
            ImGui::SameLine();
            if (ImGui::SmallButton("フェッチ"))
                RunGitAsync("フェッチ", [root]{ return GitIntegration::Fetch(root); });
            ImGui::SameLine();
            if (ImGui::ArrowButton("##pull", ImGuiDir_Down))
                RunGitAsync("プル", [root]{ return GitIntegration::Pull(root); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("プル（受信 ↓）");
            ImGui::SameLine();
            if (ImGui::ArrowButton("##push", ImGuiDir_Up))
                RunGitAsync("プッシュ", [root]{ return GitIntegration::Push(root, true); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("プッシュ（送信 ↑）");
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    statusBanner();
    ImGui::Separator();

    // ---- コンフリクト一覧（pull/merge が競合で止まっている間だけ表示）----
    if (!m_gitConflicts.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.12f, 0.12f, 0.35f));
        ImGui::BeginChild("##conflicts", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextColored(th::Bad, "⚠ コンフリクト %zu 件（解消してからコミットしてや）", m_gitConflicts.size());
        for (const auto& path : m_gitConflicts)
        {
            ImGui::PushID(path.c_str());
            ImGui::TextUnformatted(path.c_str());
            ImGui::BeginDisabled(busy);
            ImGui::SameLine();
            if (ImGui::SmallButton("自分優先"))
                RunGitAsync("コンフリクト解消", [root, path]{ return GitIntegration::ResolveOurs(root, path); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("HEAD（自分側）の内容で解消する");
            ImGui::SameLine();
            if (ImGui::SmallButton("相手優先"))
                RunGitAsync("コンフリクト解消", [root, path]{ return GitIntegration::ResolveTheirs(root, path); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("取り込んだ側（pull元/マージ元）の内容で解消する");
            ImGui::SameLine();
            if (ImGui::SmallButton("開く"))
                GitIntegration::OpenConflictFile(root, path);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("VSCode（無ければ既定アプリ）で開いて手動編集する");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    else if (m_gitMergeInProgress)
    {
        ImGui::TextColored(th::Good, "✓ コンフリクトは全部解消したで。下でコミットしてマージを終わらせてや。");
        ImGui::Spacing();
    }

    // ---- コミットメッセージ（複数行・空ならプレースホルダを重ね描き）----
    ImVec2 msgPos = ImGui::GetCursorScreenPos();
    ImGui::InputTextMultiline("##commitmsg", m_gitCommitMsgBuf.data(), m_gitCommitMsgBuf.size(),
                              ImVec2(-FLT_MIN, fh * 2.2f));
    if (m_gitCommitMsgBuf[0] == '\0')
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(msgPos.x + 6, msgPos.y + ImGui::GetStyle().FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), "メッセージを入力してください <必須>");

    // ---- コミット スプリットボタン（既定=コミット、▼=コミット&プッシュ）----
    auto doCommit = [&](bool alsoPush)
    {
        SaveCurrentProject();                            // シーン保存はメインスレッドで
        std::string msg = m_gitCommitMsgBuf.data();
        if (alsoPush)
            RunGitAsync("コミット&プッシュ", [root, msg]{
                auto c = GitIntegration::CommitAll(root, msg);
                // コミット失敗が「変更なし」(良性)か本当の失敗(hook却下/identity未設定)かを判定。
                bool nothingStaged = GitIntegration::RunGit(root, "diff --cached --quiet").ok();
                auto p = GitIntegration::Push(root, true);
                p.output = c.output + "\n----\n" + p.output;
                if (!c.ok() && !nothingStaged)
                    p.exitCode = c.exitCode ? c.exitCode : 1;  // 本当のコミット失敗は全体失敗
                return p;
            });
        else
            RunGitAsync("コミット", [root, msg]{ return GitIntegration::CommitAll(root, msg); });
    };
    // マージ解消後、選んだ側が HEAD と同一内容だと通常の変更差分は0件になる（それでもマージコミットとして
    // 成立する = git commit は成功する）ので、mid-merge のときは変更0件でもコミットボタンを塞がない。
    const bool canCommit = (!m_gitChanges.empty() || m_gitMergeInProgress) && m_gitCommitMsgBuf[0] != '\0';
    ImGui::BeginDisabled(busy || !canCommit);
    icon(m_icons.commit, 18);
    if (ImGui::Button("すべてをコミット", ImVec2(ImGui::GetContentRegionAvail().x - fh - 1.0f, 0)))
        doCommit(false);
    ImGui::SameLine(0, 1);
    if (ImGui::ArrowButton("##commitdrop", ImGuiDir_Down))
        ImGui::OpenPopup("##commitopts");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##commitopts"))
    {
        if (ImGui::Selectable("コミット"))                 doCommit(false);
        if (!remote.empty() && ImGui::Selectable("コミット & プッシュ")) doCommit(true);
        ImGui::EndPopup();
    }

    ImGui::Spacing();

    // ---- 変更 (N) ツリー ----
    std::string changesHdr = "変更 (" + std::to_string(m_gitChanges.size()) + ")###changes";
    if (ImGui::CollapsingHeader(changesHdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (m_gitChanges.empty())
            ImGui::TextDisabled("変更なし（クリーン）");
        else
        {
            // パスを '/' で分割して階層ツリーを構築
            struct TNode { std::map<std::string, TNode> dirs; std::vector<std::pair<std::string, char>> files; };
            TNode rootNode;
            for (const auto& ch : m_gitChanges)
            {
                TNode* cur = &rootNode;
                size_t start = 0;
                for (;;)
                {
                    size_t slash = ch.path.find('/', start);
                    if (slash == std::string::npos)
                    { cur->files.emplace_back(ch.path.substr(start), ch.status); break; }
                    cur = &cur->dirs[ch.path.substr(start, slash - start)];
                    start = slash + 1;
                }
            }
            auto stColor = [&](char st) -> ImVec4 {
                switch (st) {
                    case 'A': return th::Good;
                    case 'M': return th::Warn;
                    case 'R': return th::Accent;
                    case 'D': case 'U': return th::Bad;
                    default:  return th::TextDim;
                }
            };
            std::function<void(const TNode&)> draw = [&](const TNode& n)
            {
                for (const auto& kv : n.dirs)
                    if (ImGui::TreeNodeEx(kv.first.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
                    { draw(kv.second); ImGui::TreePop(); }
                for (const auto& f : n.files)
                {
                    ImGui::TreeNodeEx(f.first.c_str(), ImGuiTreeNodeFlags_Leaf
                        | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
                    char s[2] = { f.second, 0 };
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(s).x - 2.0f);
                    ImGui::TextColored(stColor(f.second), "%s", s);
                }
            };
            ImGui::BeginChild("##changes", ImVec2(0, 220), true);
            draw(rootNode);
            ImGui::EndChild();
        }
    }

    // ---- リモート未設定: URL 手動設定 + GitHub 新規作成 ----
    if (remote.empty())
    {
        ImGui::SeparatorText("リモート未設定");
        ImGui::TextDisabled("プッシュ先がまだ無いで。URL 設定か GitHub 新規作成してや。");
        ImGui::SetNextItemWidth(-70);
        ImGui::InputTextWithHint("##remote", "https://github.com/owner/repo.git",
                                 m_gitRemoteBuf.data(), m_gitRemoteBuf.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || m_gitRemoteBuf[0] == '\0');
        if (ImGui::Button("設定", ImVec2(-FLT_MIN, 0)))
        {
            std::string url = m_gitRemoteBuf.data();
            RunGitAsync("リモート設定", [root, url]{ return GitIntegration::AddRemote(root, url); });
        }
        ImGui::EndDisabled();

        if (m_ghAvailable)
        {
            ImGui::TextDisabled("リポジトリ名");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##reponame2", m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size());

            ImGui::BeginDisabled(busy || m_gitNewRepoNameBuf[0] == '\0');
            std::string msg2 = m_gitCommitMsgBuf.data();
            if (ImGui::Button("GitHub に作成 (private) & push", ImVec2(-FLT_MIN, 0)))
                createGitHubRepo(/*isPrivate=*/true, /*needInit=*/false, msg2, m_gitNewRepoNameBuf.data());
            if (ImGui::Button("GitHub に作成 (public) & push",  ImVec2(-FLT_MIN, 0)))
                createGitHubRepo(/*isPrivate=*/false, /*needInit=*/false, msg2, m_gitNewRepoNameBuf.data());
            ImGui::EndDisabled();
        }
    }

    ImGui::Spacing();
    outputLog();

    ImGui::End();
}


bool Application::BuildGameStandalone()
{
    // 開始シーンを title.json に（あれば）。無ければ現在の currentScenePath を使う。
    std::string title = PathResolver::AssetsDir() + "scenes/title.json";
    if (std::filesystem::exists(title))
        m_editorCtx->currentScenePath = title;
    return BuildGame();
}

bool Application::BuildGame()
{
    namespace fs = std::filesystem;

    // --- 出力パスの非ASCII（日本語フォルダ名等）検出ガード（最優先）---
    // 出力先に非ASCII文字が含まれると、配布した Game.exe が起動時に std::filesystem の
    // UTF-8↔ANSI 誤変換で即クラッシュする（Windows error 1113 "No mapping for the Unicode
    // character..."）。原因不明の「ビルド成功 → 実行時クラッシュ」を防ぐため、ここで明示的に
    // 失敗させる。chosen は生の std::string（UTF-8でもACPでも日本語は >=0x80 を含む）なので
    // fs::path を経由せず（=ここで例外を出さず）バイト走査で判定する。
    {
        const std::string chosen =
            (m_editorCtx && !m_editorCtx->buildConfig.outputDir.empty())
                ? m_editorCtx->buildConfig.outputDir
                : PathResolver::BaseDir();
        bool nonAscii = false;
        for (unsigned char c : chosen) if (c >= 0x80) { nonAscii = true; break; }
        if (nonAscii)
        {
            Logger::Error("ビルドを中止しました: 出力先パスに非ASCII文字（日本語フォルダ名など）が"
                          "含まれています。このままビルドすると起動時にパスエラーで落ちるため、"
                          "半角英数のみのフォルダを指定してください。パス: {}", chosen);
            if (m_editorCtx)
                m_editorCtx->buildErrorMsg =
                    "出力フォルダのパスに日本語など非ASCII文字が含まれています。\n"
                    "このまま配布すると Game.exe が起動時にクラッシュします。\n"
                    "出力先を半角英数字のみのパスにしてください。\n\n" + chosen;
            return false;
        }
    }

    // ビルド出力先。ユーザーがビルド設定で選んだフォルダの中に「製品名_build」サブフォルダを作る。
    // 選んだフォルダ自体を出力先にして remove_all するとユーザーのデータを消す恐れがあるので必ずサブフォルダ化する。
    // 製品名 = ゲーム名をサニタイズ（英数・空白・_- のみ残す）。空なら "Game"。
    // 出力フォルダ名と exe 名の両方に使う（タイトルバーはマニフェスト title=入力値そのまま）。
    std::string productName;
    if (m_editorCtx)
        for (char c : std::string(m_editorCtx->buildConfig.title))
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ')
                productName += c;
    while (!productName.empty() && productName.back()  == ' ') productName.pop_back();
    while (!productName.empty() && productName.front() == ' ') productName.erase(productName.begin());
    if (productName.empty()) productName = "Game";
    const std::string exeName = productName + ".exe";

    fs::path outputDir;
    if (m_editorCtx && !m_editorCtx->buildConfig.outputDir.empty())
        outputDir = fs::path(m_editorCtx->buildConfig.outputDir) / (productName + "_build");
    else
        outputDir = fs::path(PathResolver::BaseDir()) / "build" / "game";

    // クリーンアップ（安全策: 既存が「前回ビルド or 空」でなければ消さずに中止＝ユーザーデータ保護）
    if (fs::exists(outputDir))
    {
        std::error_code ec;
        bool looksLikeBuild = fs::exists(outputDir / "Game.exe")
                           || fs::exists(outputDir / exeName)
                           || fs::exists(outputDir / "game.pak")
                           || fs::is_empty(outputDir, ec);
        if (!looksLikeBuild)
        {
            Logger::Error("ビルドを中止しました: 出力先に過去のビルド以外のデータが存在します（保護のため中断）: {}",
                          outputDir.string());
            return false;
        }
        fs::remove_all(outputDir, ec);
    }
    fs::create_directories(outputDir);

    // 完了後に Explorer で開くため、最終的な出力先を控える
    if (m_editorCtx)
        m_editorCtx->lastBuildDir = outputDir.string();

    // 1. GameRuntime.exe を Game.exe としてコピー（+ exe 隣の DLL も全部コピー）
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        fs::path exeDir = fs::path(exePath).parent_path();
        fs::path runtimeSrc = exeDir / "GameRuntime.exe";

        if (!fs::exists(runtimeSrc))
        {
            Logger::Error("GameRuntime.exe が見つかりません（{}）。先にエンジンをビルドしてください", runtimeSrc.string());
            return false;
        }

        fs::copy_file(runtimeSrc, outputDir / exeName, fs::copy_options::overwrite_existing);
        Logger::Info("Copied GameRuntime.exe -> {}", exeName);

        // 同じフォルダの .dll をすべて配布フォルダへ。
        // dxcompiler.dll(実行時シェーダーコンパイル専用、エディタのみ必要)だけ除外する。
        // ゲームは ShaderCompiler::LoadFromFile が game.pak から .cso を読むだけで実行時コンパイルは
        // 不要なため、同梱すると無駄に容量が増えるだけ(~25MB)。GameRuntime は dxcompiler.dll を
        // delay-load にしてある(ルート CMakeLists.txt)ので、同梱しなくても exe は正常起動する。
        // ※ dxil.dll は除外しない: これは D3D12 ランタイムが CreatePipelineState 時に
        // (Developer Mode OFF の環境で)DXIL署名検証のため内部で LoadLibrary するもので、
        // 我々のコードがリンクしているわけではない delay-load できない実行時依存。
        // 除外するとユーザー環境次第で PSO 生成が失敗するため、常に同梱する。
        // ★tracyclient.dll: 計測ビルド(cmake --preset windows-tracy)のツリーからゲームを
    //   ビルドしたときに、プロファイラのクライアントが配布物へ紛れ込むのを防ぐ。
    //   BuildGame は exe と同じフォルダの .dll をここに載っていないもの全部コピーする。
    static const std::unordered_set<std::string> kDllExcludeList = { "dxcompiler.dll", "tracyclient.dll" };
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(exeDir, ec))
        {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".dll" && ext != ".DLL") continue;
            std::string lowerName = entry.path().filename().string();
            for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (kDllExcludeList.count(lowerName)) continue;
            fs::copy_file(entry.path(), outputDir / entry.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
            Logger::Info("Copied dll -> {}", entry.path().filename().string());
        }
    }

    // 2+3. assets/ と scripts/ を game.pak にパック（コピーではなく暗号化アーカイブ化）
    {
        // 開始シーンの相対パスを計算。
        // ビルド設定で明示指定があればそれを最優先。無ければ現在開いているシーンから求める。
        std::string startSceneRel = "scenes/default.json";
        if (m_editorCtx && !m_editorCtx->buildConfig.startScene.empty())
        {
            startSceneRel = m_editorCtx->buildConfig.startScene;
        }
        else if (!m_editorCtx->currentScenePath.empty())
        {
            auto norm = [](std::string s) { for (auto& c : s) if (c == '\\') c = '/'; return s; };
            std::string full = norm(m_editorCtx->currentScenePath);
            std::string base = norm(PathResolver::AssetsDir());
            if (!base.empty() && full.rfind(base, 0) == 0)
                startSceneRel = full.substr(base.size());
            else
                startSceneRel = fs::path(full).lexically_relative(fs::path(base)).generic_string();

            if (startSceneRel.empty() || startSceneRel.rfind("..", 0) == 0)
            {
                Logger::Warn("現在のシーンが assets/ の外にあるため、既定の開始シーンを使用します: {}",
                             m_editorCtx->currentScenePath);
                startSceneRel = "scenes/default.json";
            }
        }

        vfs::PakWriter pak;
        if (!pak.Open((outputDir / "game.pak").string()))
        {
            Logger::Error("game.pak を書き込み用に開けません");
            return false;
        }

        // assets/ 配下を全パック（Normalize が "assets/" プレフィックスを剥がす）
        {
            fs::path assetsDir = fs::path(PathResolver::AssetsDir());
            std::error_code ec;
            for (fs::recursive_directory_iterator it(assetsDir, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;
                // "." 始まりのフォルダ（.thumbcache / .texcache）はエディタ専用のキャッシュ。
                // 出荷 pak に入れても実行時には参照されず容量を食うだけなので丸ごと除外する。
                if (it->is_directory(ec))
                {
                    const std::string dirName = it->path().filename().string();
                    if (!dirName.empty() && dirName[0] == '.')
                        it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                std::string relPath = it->path().lexically_relative(assetsDir).generic_string();
                pak.AddFile(it->path().string(), relPath);
            }
        }

        // scripts/ 配下を全パック（"scripts/" プレフィックスを付けて格納）
        {
            fs::path scriptsDir = fs::path(PathResolver::ScriptsDir());
            if (fs::exists(scriptsDir))
            {
                std::error_code ec;
                for (auto& entry : fs::recursive_directory_iterator(scriptsDir, ec))
                {
                    if (!entry.is_regular_file()) continue;
                    std::string relPath = "scripts/" +
                        entry.path().lexically_relative(scriptsDir).generic_string();
                    pak.AddFile(entry.path().string(), relPath);
                }
            }
        }

        // shaders/ 配下の .cso を全パック（"shaders/" プレフィックス付き）。
        // → 出荷フォルダにプレーンな shaders/ を置かず、暗号化して pak に封入する。
        //   実行時は ShaderCompiler::LoadFromFile が VFS 経由で pak から復号する。
        // プロジェクト独自シェーダー(上書き/自作)がある場合は実行時再コンパイルして反映する。
        // コンパイル失敗があれば古い .cso を出荷せずビルド自体を中止する。
        {
            std::vector<std::string> shaderErrors;
            if (m_shaderManager && !m_shaderManager->RecompileAllForBuild(&shaderErrors))
            {
                std::string msg = "プロジェクトのシェーダーのコンパイルに失敗しました。ビルドを中止しました:\n";
                for (const auto& e : shaderErrors) msg += "  - " + e + "\n";
                Logger::Error("{}", msg);
                if (m_editorCtx) m_editorCtx->buildErrorMsg = msg;
                return false;
            }

            fs::path shadersDir = fs::path(PathResolver::ShaderDirW());
            if (fs::exists(shadersDir))
            {
                std::error_code ec;
                for (auto& entry : fs::recursive_directory_iterator(shadersDir, ec))
                {
                    if (!entry.is_regular_file()) continue;
                    std::string relPath = "shaders/" +
                        entry.path().lexically_relative(shadersDir).generic_string();
                    // プロジェクトオーバーライドで再コンパイル済みなら baked .cso より優先する。
                    const std::vector<u8>* overrideBytes = m_shaderManager
                        ? m_shaderManager->TryGetOverride(entry.path().filename().wstring())
                        : nullptr;
                    if (overrideBytes)
                        pak.AddBlob(relPath, overrideBytes->data(), overrideBytes->size());
                    else
                        pak.AddFile(entry.path().string(), relPath);
                }
            }

            // カスタムシェーダー(Registry外、MeshRenderer::shaderPath 割当用)。
            // キー規約は Application::EnsureCustomPso のゲームモード分岐と一致させること。
            if (m_shaderManager)
            {
                for (const std::string& relPath : m_shaderManager->AllValidCustomRelPaths())
                {
                    const std::vector<u8>* vs = m_shaderManager->GetCustomVsBytecode(relPath);
                    const std::vector<u8>* ps = m_shaderManager->GetCustomPsBytecode(relPath);
                    if (!vs || !ps) continue;
                    pak.AddBlob("shaders/custom/" + relPath + "_VS.cso", vs->data(), vs->size());
                    pak.AddBlob("shaders/custom/" + relPath + "_PS.cso", ps->data(), ps->size());
                }
            }
        }

        // ブートマニフェスト（game.json の代替。GameRuntime は pak からこれを読む）。
        // ビルド設定のタイトル/解像度を反映する。
        {
            std::string title = "Game";
            int winW = 1280, winH = 720;
            if (m_editorCtx)
            {
                if (m_editorCtx->buildConfig.title[0] != '\0')
                    title = m_editorCtx->buildConfig.title;
                winW = m_editorCtx->buildConfig.width;
                winH = m_editorCtx->buildConfig.height;
            }
            // JSON 文字列エスケープ（" と \ のみ。タイトルは UTF-8 のまま格納）
            std::string titleEsc;
            for (char c : title)
            {
                if (c == '\\' || c == '"') titleEsc += '\\';
                titleEsc += c;
            }

            std::string manifest =
                std::string("{\n") +
                "  \"title\": \"" + titleEsc + "\",\n" +
                "  \"startScene\": \"" + startSceneRel + "\",\n" +
                "  \"windowWidth\": " + std::to_string(winW) + ",\n" +
                "  \"windowHeight\": " + std::to_string(winH) + "\n" +
                "}\n";
            pak.AddBlob("__manifest__",
                reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
        }

        if (!pak.Finish(/*stripStrings=*/true))
        {
            Logger::Error("game.pak の書き出しに失敗しました");
            return false;
        }
        Logger::Info("Packed game.pak (startScene = {})", startSceneRel);
    }

    // 4. （shaders は手順 2+3 の game.pak に暗号化封入済み＝プレーンな shaders/ は出力しない）

    // 5. 起動用バッチ（GameRuntime は --game 不要: 常にゲームモード）
    {
        std::ofstream bat(outputDir / (productName + ".bat"));
        bat << "@echo off\n";
        bat << "\"" << exeName << "\"\n";
        bat << "pause\n";
    }

    Logger::Info("Game build complete: {}", outputDir.string());
    return true;
}

// 「ビルド設定」ウィンドウ（Unity の Build Settings / Unreal の Packaging 相当）。
// 構成・開始シーン・出力先を決めてから「ビルド」で BuildGame を実行する。
void Application::RenderBuildSettingsWindow()
{
    if (!m_editorCtx || !m_editorCtx->showBuildSettings)
        return;

    namespace fs = std::filesystem;
    auto& cfg = m_editorCtx->buildConfig;
    const auto before = cfg;   // フレーム末尾の変更検知→プロジェクトへ自動保存用

    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ビルド設定", &m_editorCtx->showBuildSettings))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("ゲームを単体 exe + 暗号化アセット(game.pak) に書き出す");
    ImGui::Spacing();

    // ===== シーン =====
    if (ImGui::CollapsingHeader("シーン", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::vector<std::string> scenes;
        std::string scenesDir = PathResolver::AssetsDir() + "scenes";
        if (fs::exists(scenesDir))
            for (auto& e : fs::directory_iterator(scenesDir))
                if (e.is_regular_file() && e.path().extension() == ".json")
                    scenes.push_back("scenes/" + e.path().filename().string());

        ImGui::TextUnformatted("開始シーン");
        const char* curLabel = cfg.startScene.empty()
            ? "(\xe7\x8f\xbe\xe5\x9c\xa8\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3)"  // (現在開いているシーン)
            : cfg.startScene.c_str();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##startScene", curLabel))
        {
            if (ImGui::Selectable("(\xe7\x8f\xbe\xe5\x9c\xa8\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3)",
                                  cfg.startScene.empty()))
                cfg.startScene.clear();
            for (auto& s : scenes)
                if (ImGui::Selectable(s.c_str(), s == cfg.startScene))
                    cfg.startScene = s;
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("\xe2\x80\xbb \xe5\x85\xa8\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x81\x8c game.pak \xe3\x81\xab\xe5\x90\xab\xe3\x81\xbe\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\xe8\xb5\xb7\xe5\x8b\x95\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x82\x92\xe9\x81\xb8\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82");  // ※全シーンがgame.pakに含まれます。起動シーンを選びます。
    }

    // ===== 製品 =====
    if (ImGui::CollapsingHeader("製品", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("ゲーム名（ウィンドウタイトル / exe名）");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##title", cfg.title, sizeof(cfg.title));
        ImGui::TextDisabled("※ exe名/フォルダ名には英数・空白・_- のみが使われます");

        ImGui::TextUnformatted("解像度");
        struct Res { const char* name; int w, h; };
        static const Res presets[] = {
            {"1280 x 720 (HD)",   1280, 720},
            {"1600 x 900",        1600, 900},
            {"1920 x 1080 (FHD)", 1920, 1080},
            {"2560 x 1440 (QHD)", 2560, 1440},
        };
        std::string cur = std::to_string(cfg.width) + " x " + std::to_string(cfg.height);
        ImGui::SetNextItemWidth(210.0f);
        if (ImGui::BeginCombo("##respreset", cur.c_str()))
        {
            for (auto& p : presets)
                if (ImGui::Selectable(p.name, p.w == cfg.width && p.h == cfg.height))
                {
                    cfg.width  = p.w;
                    cfg.height = p.h;
                }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("\xe5\xb9\x85##w", &cfg.width, 0);    // 幅
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("\xe9\xab\x98\xe3\x81\x95##h", &cfg.height, 0);  // 高さ
        cfg.width  = std::clamp(cfg.width,  320, 7680);
        cfg.height = std::clamp(cfg.height, 240, 4320);
    }

    // ===== 出力先 =====
    if (ImGui::CollapsingHeader("出力先", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("配置先フォルダ");
        char pathBuf[1024];
        strncpy_s(pathBuf,
                  cfg.outputDir.empty()
                    ? "(\xe6\x9c\xaa\xe9\x81\xb8\xe6\x8a\x9e \xe2\x80\x94 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe6\x99\x82\xe3\x81\xab\xe9\x81\xb8\xe6\x8a\x9e)"  // (未選択 — ビルド時に選択)
                    : cfg.outputDir.c_str(),
                  _TRUNCATE);
        ImGui::SetNextItemWidth(-92.0f);
        ImGui::InputText("##outdir", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("\xe5\x8f\x82\xe7\x85\xa7...", ImVec2(-1.0f, 0.0f)))  // 参照...
        {
            std::string dir;
            if (ProjectManager::PickFolder(m_window->GetHwnd(), dir, L"ビルドの配置先フォルダを選択"))
                cfg.outputDir = dir;
        }
        ImGui::Checkbox("ビルド後にフォルダを開く", &cfg.openFolderAfterBuild);
        ImGui::TextDisabled("\xe2\x80\xbb \xe9\x81\xb8\xe3\x82\x93\xe3\x81\xa0\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe7\x9b\xb4\xe4\xb8\x8b\xe3\x81\xab \"<\xe8\xa3\xbd\xe5\x93\x81\xe5\x90\x8d>_build\" \xe3\x82\x92\xe4\xbd\x9c\xe3\x81\xa3\xe3\x81\xa6\xe5\x87\xba\xe5\x8a\x9b\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82");  // ※選んだフォルダ直下に "<製品名>_build" を作って出力します。
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ===== ビルド実行 =====
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.42f, 0.68f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.52f, 0.82f, 1.0f));
    const bool doBuild = ImGui::Button("ビルド", ImVec2(-1.0f, 38.0f));
    ImGui::PopStyleColor(2);
    if (doBuild)
    {
        bool proceed = true;
        if (cfg.outputDir.empty())   // 未選択なら今すぐフォルダを選ばせる
        {
            std::string dir;
            if (ProjectManager::PickFolder(m_window->GetHwnd(), dir, L"ビルドの配置先フォルダを選択"))
                cfg.outputDir = dir;
            else
                proceed = false;
        }
        if (proceed)
            m_editorCtx->pendingBuildGame = true;   // フレーム境界で BuildGame 実行
    }

    if (m_editorCtx->buildCompleteFlash > 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
        ImGui::TextUnformatted("\xe2\x9c\x93 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86");  // ✓ ビルド完了
        ImGui::PopStyleColor();
        m_editorCtx->buildCompleteFlash -= m_gameClock.GetDeltaTime();
    }
    else if (m_editorCtx->buildErrorFlash > 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextUnformatted("\xe2\x9c\x97 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xa4\xb1\xe6\x95\x97 (dx12_engine.log)");  // ✗ ビルド失敗
        ImGui::PopStyleColor();
        m_editorCtx->buildErrorFlash -= m_gameClock.GetDeltaTime();
    }

    // 変更があればプロジェクトへ即保存（<ルート>/build_settings.json。開き直しでも保持）
    if (strcmp(before.title, cfg.title) != 0 || before.width != cfg.width ||
        before.height != cfg.height || before.startScene != cfg.startScene ||
        before.outputDir != cfg.outputDir ||
        before.openFolderAfterBuild != cfg.openFolderAfterBuild)
        SaveProjectBuildConfig(*m_editorCtx, m_loadInfo.rootDir);

    ImGui::End();
}



} // namespace dx12e
