// ===========================================================================
// Application: プロジェクト / バージョン管理 / ゲームビルド
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "editor/UiWidgets.h"
#include "editor/PropertyGrid.h"   // フェーズ 1b: ビルド設定窓を pg:: 2 カラムへ
#include "core/ApplicationInternal.h"
#include "core/VirtualGuard.h"   // 仮想入力モード中は ShellExecute / ダイアログを実行しない
#include "resource/AssetPrewarmer.h"   // BeginAssetPrewarm / Stop
#include "core/CrashHandler.h"
#include "core/BuildJob.h"   // ビルド設定窓の進捗表示（BuildProgress）
#include "core/vfs/PakWriter.h"   // BakeTexturesIntoPak: 既存 pak への追記
#include "core/AtomicFileJson.h"   // 原子的な保存
#include <fstream>
#include <chrono>
#include "project/LauncherLogic.h"
#include "editor/WhatsNewScreen.h"   // 更新内容モーダルの描画

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
    // 原子的に書く（途中で落ちても元の build_settings.json は無傷）
    const auto wr = atomicfile::WriteFile(std::filesystem::path(projectRoot) / "build_settings.json",
                                          j.dump(2) + "\n", atomicfile::JsonVerifier());
    if (!wr) Logger::Warn("build_settings.json の保存に失敗しました: {}", wr.error);
}

void Application::BeginProjectLoad(const ProjectInfo& info, bool isNew)
{
    namespace fs = std::filesystem;
    // ゲームのビルド（裏ジョブ）はプロジェクトの assets / scripts を読んでいる最中なので、開き直しは拒否する（安全側）。
    if (IsBuildRunning())
    {
        Logger::Warn("ゲームのビルド中はプロジェクトを開き直せません。ビルドの完了かキャンセルを待ってください");
        if (m_editorCtx)
            m_editorCtx->Notify(ui::ToastKind::Warn, "ゲームのビルド中はプロジェクトを開けません。完了かキャンセルを待ってください");
        return;
    }
    // 直前のスレッドが残っていれば回収
    if (m_loadThread.joinable())
        m_loadThread.join();

    // 前のプロジェクトの先読みは止める。assets ディレクトリが差し替わるので、
    // 走らせたままだと存在しないパスを掴んだまま無駄に回り続ける。
    if (m_assetPrewarmer) m_assetPrewarmer->Stop();

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
    SplashScreen::ShowProjectLoad(kEngineName,
                                  std::string("v") + kEngineVersion,
                                  PathResolver::AssetsDir() + "editor/icons/logo.png",
                                  info.name, isNew);
    SplashScreen::SetProjectInfo(info.name, info.defaultScene);
    if (isNew) SplashScreen::SetStage(splash::Stage::ProjectCreate);
    else SplashScreen::SetStatus(m_loadStatus);

    // ロードが終わるまでメインウィンドウを隠す。ロード中に古いシーンやテンプレートが
    // 一瞬見えるのを防ぐ(表示はスプラッシュのみ。完了時に UpdateProjectLoad が再表示する)。
    // ★--background は隠さない/再表示しない（隠して Show し直すと、表示のたびに Z オーダーや
    //   最小化状態が変わりうる。スプラッシュも出さない設定なので隠す理由が無い）。
    if (m_window && m_window->GetHwnd() && !m_window->IsBackground()
        && IsWindowVisible(m_window->GetHwnd()))
        ShowWindow(m_window->GetHwnd(), SW_HIDE);

    if (isNew)
    {
        // ディスク作成（フォルダ生成・テンプレ書き出し）はワーカースレッドで
        m_loadThreadRunning = true;
        ProjectInfo copy = info;
        m_loadThread = std::thread([this, copy]()
        {
            CrashHandler::PrepareThread();
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
        // ★Play 中のままプロジェクトを切り替えない。Play 開始時のスナップショット（m_playSceneJson）と
        //   保存先（m_playScenePathSnapshot）が前のプロジェクトのまま残り、次の Stop で
        //   **前のシーンが新しいプロジェクトの assets 基準で復元**される（モデルのパスが解決できず
        //   モデル付きエンティティが落ちる）うえ、保存先も前のシーンに戻るので、直後の自動保存で
        //   前のプロジェクトのシーンが欠けた状態で上書きされていた（2026-10-02 に実際に 8→6 体）。
        //   assets がまだ前のプロジェクトを指しているここで Stop する。保留中の Stop 要求は
        //   Run() 側が「既に Editor なら何もしない」で消化し、保留中の Play は読み込み完了まで持ち越される
        //   （どちらも MCP の遅延応答はそこで返る）。
        if (m_engineMode == EngineMode::Playing && !m_playSceneJson.empty())
        {
            Logger::Info("プロジェクトを切り替える前に Play を停止します");
            EnterEditorMode();
        }

        m_loadStatus = "シーンを読み込み中...";
        SplashScreen::SetStage(splash::Stage::ProjectScene);
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
            // %を先頭に出す。スプラッシュは横に長いので、割合 → 件数 → ファイル名の順。
            const int pct = static_cast<int>(
                static_cast<f64>(job.next) / static_cast<f64>(job.assets.size()) * 100.0 + 0.5);
            snprintf(buf, sizeof(buf), "アセットを読み込み中... %d%%  (%zu / %zu)  %s",
                     pct, job.next, job.assets.size(), base);
        }
        else if (job.needsScan)
        {
            snprintf(buf, sizeof(buf), "シーンを解析中...");
        }
        else
        {
            snprintf(buf, sizeof(buf), "シーンを構築中...");
        }
        m_loadStatus = buf;
        if (!job.assets.empty())
        {
            SplashScreen::SetStage(splash::Stage::ProjectAssets, m_loadStatus);
            SplashScreen::SetStageProgress(static_cast<float>(job.next) / static_cast<float>(job.assets.size()));
        }
        else
        {
            SplashScreen::SetStatus(m_loadStatus);
        }
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
                SplashScreen::SetStage(splash::Stage::ProjectMaterials, m_loadStatus);
                if (m_matThumbPreloadTotal > 0)
                    SplashScreen::SetStageProgress(static_cast<float>(m_matThumbPreloadTotal - remaining) / static_cast<float>(m_matThumbPreloadTotal));
                return;
            }
        }

        m_loading            = false;
        m_loadProjectStarted = false;
        m_editorCtx->buildCompleteFlash = 1.5f;

        // ★ここから先はエディタが操作できる状態。プロジェクト内の全シーンのテクスチャを
        //   バックグラウンドで圧縮しておく（優先度は BELOW_NORMAL なので操作の邪魔をしない）。
        //   ロード画面を閉じた【後】に始めるのが要点で、ここより前に始めると
        //   「起動が遅くなった」に見えてしまう。
        BeginAssetPrewarm();

        // キー割り当てはプロジェクト単位（保存先が PathResolver::BaseDir() 基準なので、
        // プロジェクトルートが確定したここで読む）。無ければ既定のまま。
        LoadActionBindings();
        // 物理ハードウェア（assets/hardware.json。無ければ空設定）。プロジェクトを切り替えたら Shutdown→Initialize になる。
        // ★ゲームモードは BeginProjectLoad を通らないので Application::Initialize 側にも同じ呼び出しがある。
        InitHardware();

        // ロード完了: 隠していたメインウィンドウを出してからスプラッシュを閉じる
        // (順序を逆にすると一瞬何も表示されない空白ができる)。
        // 起動直後の遅延表示(--project直開き等でまだ未表示)もここで役目を引き継ぐ。
        // ★ヘッドレスでは窓を出さない。ここを素通しにすると --headless でも窓が出て、
        //   人が作業している画面を奪う（そもそもそれを避けるための機能）。
        m_deferredFirstShow = false;
        SplashScreen::SetStage(splash::Stage::ProjectFinalize);
        // メイン窓は ready の演出のあとにスプラッシュが呼ぶ（PumpMainThread）。スプラッシュが無ければ即出す。
        // ★ヘッドレスでは窓を出さない（人の作業している画面を奪わない）。
        SplashScreen::Finish([this] { if (m_window && !m_headless) m_window->Show(); });

        // --scene: プロジェクトを開き終えた直後に指定シーンを開く。
        // ★ここで直接ロードせず pending にする。この関数はロード完了処理の途中なので、
        //   入れ子でロードを始めると currentScenePath とジョブの状態が食い違う。
        if (!m_startupScene.empty() && !m_startupScenePending)
        {
            m_startupScenePending = true;
            m_editorCtx->pendingLoadPath = PathResolver::AssetsDir() + m_startupScene;
            m_editorCtx->pendingLoadSkipConfirm = true;   // 未保存モーダルを出さない（AI は押せない）
            Logger::Info("起動シーンを開きます: {}", m_startupScene);
        }
    }
}

// ===== ランチャーのサムネイル =====
void Application::RequestProjectThumbnail(bool force)
{
    if (m_isGameMode || m_projectInfo.rootDir.empty()) return;
    m_thumbRequested = true;
    m_thumbForce = m_thumbForce || force;
}

void Application::UpdateProjectThumbnail(f32 dt)
{
    if (m_isGameMode || m_loading || !m_sceneRT) { m_thumbRequested = false; return; }
    const std::string root = m_projectInfo.rootDir;
    if (root.empty()) { m_thumbRequested = false; return; }
    const std::string thumbPath = launcher::ThumbnailPath(root);

    // 自動: プロジェクトを開いて数秒後（絵が安定してから）、サムネイルがまだ無ければ 1 度だけ撮る。
    if (!m_showLauncher && m_engineMode == EngineMode::Editor)
    {
        if (m_thumbAutoRoot != root) { m_thumbAutoRoot = root; m_thumbAutoTimer = 0.0f; m_thumbAutoDone = false; }
        if (!m_thumbAutoDone)
        {
            m_thumbAutoTimer += dt;
            if (m_thumbAutoTimer > 6.0f)
            {
                m_thumbAutoDone = true;
                std::error_code ec;
                if (!std::filesystem::exists(launcher::PathFromUtf8(thumbPath), ec)) { m_thumbRequested = true; }
            }
        }
    }
    if (!m_thumbRequested) return;

    const bool force = m_thumbForce;
    // Play 中は撮らない（ゲーム画面は別物で、読み戻しの GPU 待ちがゲームを止める）。
    if (m_engineMode != EngineMode::Editor && !force) { m_thumbRequested = false; return; }
    // 保存のたびに撮るとディスクと GPU の無駄なので、通常要求は前回から 45 秒以上あけて 1 回だけ。
    const u64 now = GetTickCount64();
    if (!force && m_thumbLastTick != 0 && now - m_thumbLastTick < 45000ull) { m_thumbRequested = false; return; }
    m_thumbRequested = false;
    m_thumbForce = false;

    std::vector<u8> bgra;
    u32 w = 0, h = 0;
    std::string err;
    if (!ReadbackSceneBgra(bgra, w, h, err))
    {
        Logger::Warn("サムネイルの撮影に失敗しました（{}）", err);
        return;
    }
    const launcher::ThumbCrop crop = launcher::CenterCrop16x9(static_cast<int>(w), static_cast<int>(h));
    if (crop.w <= 0 || crop.h <= 0) return;
    // 小さいビューポートは拡大しない（元の大きさまで）。
    const int dw = (std::min)(launcher::kThumbW, crop.w);
    const int dh = (std::max)(1, dw * 9 / 16);
    const std::vector<u8> thumb = launcher::DownscaleBgra(bgra.data(), static_cast<int>(w), static_cast<int>(h),
                                                          static_cast<int>(w) * 4, crop, dw, dh);
    // 暗転中・ロード直後の真っ黒 / 単色は、前の良いサムネイルを上書きしない。
    if (launcher::ThumbnailLooksBlank(thumb, dw, dh))
    {
        Logger::Info("サムネイル: 暗転または単色のため更新しません");
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path out = launcher::PathFromUtf8(thumbPath);
    fs::create_directories(out.parent_path(), ec);
    const fs::path tmp = out.wstring() + L".tmp";
    if (!WriteBgraPng(tmp.wstring(), thumb.data(), static_cast<u32>(dw), static_cast<u32>(dh), err))
    {
        Logger::Warn("サムネイルの書き出しに失敗しました（{}）", err);
        fs::remove(tmp, ec);
        return;
    }
    fs::rename(tmp, out, ec);
    if (ec) { fs::remove(out, ec); ec.clear(); fs::rename(tmp, out, ec); }
    if (!ec) { m_thumbLastTick = now; Logger::Info("サムネイルを更新しました: {}", thumbPath); }
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
        CrashHandler::PrepareThread();
        GitResult r = task();
        m_gitPendingOutput = std::move(r.output);
        m_gitPendingOk     = r.ok();
        m_gitOpDone.store(true);                         // RELEASE: 結果を最後に公開
    });
}

// ===== 変更一覧の自動追従（バージョン管理ウィンドウを開いている間だけ回る） =====
//
// ★なぜスレッドが要るか: git status / git branch はどちらも【プロセス起動】。
//   メインスレッドで毎フレームどころか毎秒回しても、起動と待機で数十 ms のヒッチになる。
//   専用スレッドで一定間隔だけ回し、結果はミューテックス越しにメインへ渡す。
//   ワーカーは m_gitWatchWanted が立っている間しか git を叩かない（＝窓を閉じれば無音）。
void Application::StartGitWatcher()
{
    if (m_gitWatchThread.joinable()) return;
    m_gitWatchStop.store(false);
    m_gitWatchThread = std::thread([this]
    {
        while (!m_gitWatchStop.load())
        {
            // 停止要求に素早く反応するため、待ちは 50ms 刻みで刻む（合計 ~800ms 間隔）。
            for (int i = 0; i < 16 && !m_gitWatchStop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (m_gitWatchStop.load()) break;
            if (!m_gitWatchWanted.load()) continue;   // 窓が閉じている間は git を叩かない

            std::string dir;
            {
                std::lock_guard<std::mutex> lk(m_gitWatchMutex);
                dir = m_gitWatchDir;
            }
            if (dir.empty() || !GitIntegration::IsRepo(dir)) continue;

            auto changes   = GitIntegration::ChangedFiles(dir);
            auto branch    = GitIntegration::CurrentBranch(dir);
            bool merging   = GitIntegration::IsMergeInProgress(dir);
            auto conflicts = GitIntegration::ConflictedFiles(dir);
            if (m_gitWatchStop.load()) break;

            {
                std::lock_guard<std::mutex> lk(m_gitWatchMutex);
                m_gitWatchChanges   = std::move(changes);
                m_gitWatchBranch    = std::move(branch);
                m_gitWatchMerge     = merging;
                m_gitWatchConflicts = std::move(conflicts);
            }
            m_gitWatchReady.store(true);
        }
    });
}

void Application::StopGitWatcher()
{
    m_gitWatchStop.store(true);
    if (m_gitWatchThread.joinable())
        m_gitWatchThread.join();
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
    const float r = ui::Px(34.0f);
    const int   segs = 28;
    float t = m_loadSpinTime * 3.2f;
    for (int i = 0; i < segs; ++i)
    {
        float a0 = t + (float)i / segs * 6.2831853f;
        float a1 = t + (float)(i + 1) / segs * 6.2831853f;
        float alpha = (float)i / segs;  // フェードする尾
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.39f, 0.58f, 0.93f, alpha));
        dl->AddLine(ImVec2(c.x + cosf(a0) * r, c.y + sinf(a0) * r),
                    ImVec2(c.x + cosf(a1) * r, c.y + sinf(a1) * r), col, ui::Px(5.0f));
    }

    // ステータステキスト（中央寄せ）
    const char* msg = m_loadStatus.c_str();
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r + ui::Px(24.0f)),
                IM_COL32(235, 235, 235, 255), msg);

    if (!m_loadInfo.name.empty())
    {
        ImVec2 ns = ImGui::CalcTextSize(m_loadInfo.name.c_str());
        dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y + r + ui::Px(48.0f)),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), m_loadInfo.name.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Application::RenderWhatsNewPopup()
{
    // コマンド「更新内容を表示」（help.whatsNew）: 全履歴を開く。表示済みの記録は書かない。
    if (m_editorCtx && m_editorCtx->whatsNewRequest)
    {
        m_editorCtx->whatsNewRequest = false;
        if (!m_showWhatsNew)
        {
            m_showWhatsNew = true;
            m_whatsNewOpened = false;
            m_whatsNewManual = true;
            m_whatsNewFrom.clear();
        }
    }
    if (!m_showWhatsNew) return;

    static whatsnew::State s_state;
    const char* kId = "更新内容###whatsnew";
    if (!m_whatsNewOpened)
    {
        whatsnew::Setup(s_state, m_whatsNewFrom, kEngineVersion, m_whatsNewManual);
        ImGui::OpenPopup(kId);
        m_whatsNewOpened = true;
    }

    // マルチビューポート有効なので、明示しないとこのモーダルが独立OSウィンドウ化し、
    // 「画面に出ていないのに入力だけ塞ぐ」状態になる（ギズモ消失バグと同じ罠）。
    // 併せて AlwaysAutoResize は使わない: 本文が画面より縦に長いと「閉じる」が画面外に出て詰む
    // （本文は画面側でスクロール領域に入れ、フッタは固定。高さは作業領域の 88% まで）。
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const float wnW = (std::min)(ui::Px(820.0f), vp->WorkSize.x * 0.94f);
    const float wnH = (std::min)(ui::Px(800.0f), vp->WorkSize.y * 0.88f);
    ImGui::SetNextWindowSize(ImVec2(wnW, wnH), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::Px(8.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::Bg1);
    const bool begun = ImGui::BeginPopupModal(kId, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (begun)
    {
        const whatsnew::Action act = whatsnew::Draw(s_state);
        if (act == whatsnew::Action::OpenGithub)
        {
            // ユーザーが押したときだけ既定のブラウザで開く（仮想入力モード中は guard が止める）。
            const std::string url = std::string("https://github.com/") + kUpdateRepoOwner + "/" + kUpdateRepoName
                                    + "/releases/tag/v" + kEngineVersion;
            dx12e::guard::ShellExecuteGuarded(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (act == whatsnew::Action::Close)
        {
            // この版は表示済みとして記録 → 次回以降は版が変わるまで出さない。
            // 手動表示・--show-whats-new（検証）では書かない（自動化の fleet が共有ファイルを書き換えないため）。
            if (!m_whatsNewManual && !m_whatsNewForced) WriteShownVersion(kEngineVersion);
            m_showWhatsNew = false;
            m_whatsNewManual = false;
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
        if (m_window) m_window->SetTitle(kEngineNameW);
        return;
    }

    // 1) パスをプロジェクト配下へ再ポイント（shaders はエンジン側を維持）
    PathResolver::SetProjectRoot(info.rootDir);

    // プロジェクトの settings.json で描画オプションを上書き（A/B 計測用。既定 ON）。
    // ※ SetProjectRoot の後でないと PersistPath がエンジン側を指してしまう。
    // ★VSync もここで読み直す。書き込み側（ApplicationRender.cpp）は PersistPath()＝
    //   <project>/settings.json へ保存しているのに、読み込みは Initialize の 1 回だけで、
    //   その時点の PersistPath() はまだエンジン組み込みの assets を指している。
    //   つまり**保存はプロジェクトへ、読み出しはエンジンから**という食い違いがあり、
    //   エンジン設定で VSync を切っても再起動すると必ず元に戻っていた。
    m_useVsync       = PersistGet("video_vsync", m_useVsync ? 1.0 : 0.0) != 0.0;
    m_persistedVsync = m_useVsync;

    // 既定トランジション（「トランジション」窓のプリセット）。プロジェクトごとの設定なので
    // ここで読み直さないと、前のプロジェクトで選んだ演出を引きずる。
    LoadTransitionPrefs();

    // ★影の解像度と CSM の調整値。書き込み側（ApplicationRender.cpp）とセットで、
    //   これが無いと保存はされるのに読み出されない＝毎回既定に戻る。
    {
        const int qi = static_cast<int>(PersistGet("shadow_quality",
                                                   static_cast<double>(m_shadowQualityIndex)));
        static const u32 kSizes[4] = {1024, 2048, 4096, 8192};
        m_shadowQualityIndex = std::clamp(qi, 0, 3);
        const u32 want = kSizes[m_shadowQualityIndex];
        if (want != m_shadowMapSize) { m_shadowMapSize = want; m_shadowMapDirty = true; }
        m_persistedShadowQuality = m_shadowQualityIndex;

        m_cascadeSplitLambda = std::clamp(
            static_cast<f32>(PersistGet("shadow_csm_lambda", m_cascadeSplitLambda)), 0.0f, 1.0f);
        m_cascadeBlendBand   = std::clamp(
            static_cast<f32>(PersistGet("shadow_csm_band", m_cascadeBlendBand)), 0.0f, 5.0f);
        m_shadowDepthBias    = std::clamp(
            static_cast<f32>(PersistGet("shadow_depth_bias", m_shadowDepthBias)), 0.0f, 0.05f);
        m_persistedSplitLambda = m_cascadeSplitLambda;
        m_persistedBlendBand   = m_cascadeBlendBand;
        m_persistedDepthBias   = m_shadowDepthBias;
    }

    m_instancingEnabled = PersistGet("render_instancing", 1.0) != 0.0;
    Logger::Info("自動インスタンシング: {}", m_instancingEnabled ? "ON" : "OFF");
    // GPU 駆動のインスタンス群（4-3）: このしきい値以上の不透明な群を GPU カリング + ExecuteIndirect で描く。0 = 無効（従来の DrawItem 展開）。
    m_gpuInst.threshold = static_cast<u32>(std::max(0.0, PersistGet("instance_gpu_threshold", 1024.0)));
    Logger::Info("GPU 駆動のインスタンス群: しきい値 {}（0 = 無効。settings.json instance_gpu_threshold）", m_gpuInst.threshold);

    // クラスタードライティング（Forward+）。0 で「先頭 64 灯を総当たり」フォールバックへ倒す。
    m_clusteredEnabled = PersistGet("render_clustered", 1.0) != 0.0;
    Logger::Info("クラスタードライティング: {}", m_clusteredEnabled ? "ON" : "OFF");

    // 深度プリパスの単独強制（計画10 A2）。既定 OFF。
    m_forceDepthPrepass = PersistGet("render_depth_prepass", 0.0) != 0.0;
    if (m_forceDepthPrepass) Logger::Info("深度プリパス: 強制 ON（render_depth_prepass=1）");

    // Hi-Z オクルージョンカリング。既定 OFF。
    m_occlusionCulling = PersistGet("render_occlusion_culling", 0.0) != 0.0;
    if (m_occlusionCulling) Logger::Info("オクルージョンカリング: ON（render_occlusion_culling=1）");

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
        std::wstring title = std::wstring(kEngineNameW) + L" - ";
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
        {
            MarkSceneClean();
            ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
        }
        else
        {
            // 書けていないシーンを「最後に開いていたシーン」として記録しない。
            Logger::Error("シーンを保存できませんでした: {}", m_editorCtx->currentScenePath);
            m_editorCtx->Notify(ui::ToastKind::Error, "シーンを保存できませんでした（詳細は dx12_engine.log）");
        }
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
    m_editorCtx->Notify(ui::ToastKind::Success, "プロジェクトを保存しました");
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

    // ボタンの左に置く Lucide グリフ（旧: 色付きチップ PNG）。ラベルと同じ行の高さに揃える。
    auto icon = [](const char* glyph, float /*s*/) { ImGui::AlignTextToFramePadding(); ui::Icon(glyph, dx12e::theme::TextDim); ImGui::SameLine(); };

    icon(ICON_SAVE, 22);
    if (ImGui::Button("プロジェクトを保存", ImVec2(-1, ui::Px(30.0f))))
        SaveCurrentProject();

    icon(ICON_FILE_PLUS, 22);
    if (ImGui::Button("新規プロジェクト...", ImVec2(-1, 0)))
    {
        ProjectInfo created;
        if (ProjectManager::NewProjectDialog(created, m_window->GetHwnd()))
            BeginProjectLoad(created, /*isNew=*/true);
    }
    icon(ICON_FOLDER_OPEN, 22);
    if (ImGui::Button("プロジェクトを開く...", ImVec2(-1, 0)))
    {
        ProjectInfo opened;
        if (ProjectManager::OpenProjectDialog(opened, m_window->GetHwnd()))
            BeginProjectLoad(opened, /*isNew=*/false);
    }
    icon(ICON_CHEVRON_LEFT, 22);
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
    // 変更一覧の自動追従は「この窓が見えているフレーム」だけ許可する。
    // 毎フレーム false に倒し、下の描画まで到達したら true に戻す＝
    // 窓を閉じた/畳んだ瞬間にワーカーが git を叩くのをやめる。
    m_gitWatchWanted.store(false);

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
                ImGui::TextColored(th::Warn, "⚠ %s でコンフリクトが発生しました。下の一覧から解消してください", m_gitOpLabel.c_str());
            else
                ImGui::TextColored(th::Bad, "✗ %s 失敗 (下の出力ログを確認してください)", m_gitOpLabel.c_str());
        }
    };

    // 出力ログ（既定は畳む。エラー時だけ開いて確認）
    auto outputLog = [&]()
    {
        if (m_gitOutput.empty()) return;
        if (ui::CollapsingHeader("出力ログ"))
        {
            ImGui::BeginChild("##gitout", ImVec2(0, ui::Px(120.0f)), true,
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
        ImGui::TextColored(th::Bad, "✗ git が見つかりません。");
        ImGui::BeginDisabled(m_gitOpRunning);
        if (ImGui::Button("Git をインストール"))
        {
            m_gitOutput = "Git をインストール中...（winget があれば自動、無ければブラウザでダウンロード"
                          "ページを開きます。別ウィンドウが出たら指示に従ってください）";
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

    // ---- 変更一覧の自動追従（ファイルを消したら数百 ms でリストが動く）----
    // 重い取得（プロセス起動）は専用ワーカーが担当。ここは出来上がった結果を取り込むだけ。
    // ★ahead/behind とブランチ【一覧】はここでは触らない。
    //   前者は upstream 参照が要って遅く、後者は勝手に変わらないので、
    //   手動「更新」と git 操作の直後だけで十分（毎秒取り直す価値がない）。
    m_gitWatchWanted.store(true);
    {
        std::lock_guard<std::mutex> lk(m_gitWatchMutex);
        m_gitWatchDir = root;
    }
    if (!m_gitWatchThread.joinable()) StartGitWatcher();
    if (m_gitWatchReady.exchange(false))
    {
        std::lock_guard<std::mutex> lk(m_gitWatchMutex);
        m_gitChanges         = m_gitWatchChanges;
        m_gitBranchCache     = m_gitWatchBranch;
        m_gitMergeInProgress = m_gitWatchMerge;
        m_gitConflicts       = m_gitWatchConflicts;
        m_gitRepoCache       = true;
    }

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

    // ボタンの左に置く Lucide グリフ（旧: 色付きチップ PNG）。ラベルと同じ行の高さに揃える。
    auto icon = [](const char* glyph, float /*s*/) { ImGui::AlignTextToFramePadding(); ui::Icon(glyph, dx12e::theme::TextDim); ImGui::SameLine(); };

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
                m_gitOutput = "別ウィンドウでブラウザ認証してください。完了したら自動で反映されます。";
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
        ImGui::TextWrapped("このプロジェクトはまだ Git リポジトリではありません。");
        ImGui::Spacing();
        statusBanner();
        ImGui::Spacing();

        if (m_ghAvailable)
        {
            renderGitHubAccountRow();
            ImGui::Spacing();

            ImGui::TextDisabled("リポジトリ名");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ui::InputText("##reponame", m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size());

            ImGui::BeginDisabled(busy || m_ghUser.empty() || m_gitNewRepoNameBuf[0] == '\0');
            icon(ICON_GIT_BRANCH, 22);
            if (ImGui::Button("GitHub にリポジトリを作成 (Public)", ImVec2(-1, ui::Px(32.0f))))
                createGitHubRepo(/*isPrivate=*/false, /*needInit=*/true, "Initial commit",
                                  m_gitNewRepoNameBuf.data());
            icon(ICON_GIT_BRANCH, 22);
            if (ImGui::Button("GitHub にリポジトリを作成 (Private)", ImVec2(-1, ui::Px(32.0f))))
                createGitHubRepo(/*isPrivate=*/true, /*needInit=*/true, "Initial commit",
                                  m_gitNewRepoNameBuf.data());
            ImGui::EndDisabled();
            if (m_ghUser.empty())
                ImGui::TextDisabled("↑ 先に GitHub にログインしてください");

            ImGui::Spacing();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("ローカルだけで管理する（GitHub には後で公開）"))
                RunGitAsync("初期化", [root]{ return GitIntegration::Init(root); });
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::BeginDisabled(busy);
            icon(ICON_GIT_BRANCH, 22);
            if (ImGui::Button("Git リポジトリを初期化", ImVec2(-1, ui::Px(32.0f))))
                RunGitAsync("初期化", [root]{ return GitIntegration::Init(root); });
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("クローン");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ui::InputTextWithHint("##cloneurl", "https://github.com/owner/repo.git",
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
        if (ui::BeginCombo("##branch", curBr))
        {
            for (const auto& b : m_gitBranches)
            {
                ImGui::PushID(b.c_str());
                if (ImGui::Selectable(b.c_str(), b == branch) && b != branch)
                {
                    std::string target = b;
                    RunGitAsync("ブランチ切替", [root, target]{ return GitIntegration::CheckoutBranch(root, target); });
                }
                // ★各行を右クリックで「名前を変更 / 削除」。
                //   ポップアップはコンボを閉じてからでないと出せないので、
                //   ここでは要求だけ立てて 1 フレーム持ち越す（下の modal が拾う）。
                if (ImGui::BeginPopupContextItem("##brmenu"))
                {
                    ImGui::TextDisabled("%s", b.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("名前を変更..."))
                    {
                        m_gitBranchOpTarget  = b;
                        m_gitBranchOpRequest = 1;
                        strncpy_s(m_gitRenameBranchBuf.data(), m_gitRenameBranchBuf.size(),
                                  b.c_str(), _TRUNCATE);
                        ImGui::CloseCurrentPopup();
                    }
                    // 自分自身は取り込めない（git がエラーにする）ので出さない。
                    ImGui::BeginDisabled(b == branch);
                    if (ImGui::MenuItem("このブランチを取り込む（マージ）..."))
                    {
                        m_gitBranchOpTarget  = b;
                        m_gitBranchOpRequest = 3;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndDisabled();
                    // 今いるブランチは git が削除を拒否する。押させてエラーを見せるより出さない。
                    ImGui::BeginDisabled(b == branch);
                    if (ImGui::MenuItem("削除..."))
                    {
                        m_gitBranchOpTarget  = b;
                        m_gitBranchOpRequest = 2;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndDisabled();
                    if (b == branch)
                        ImGui::TextDisabled("(今いるブランチはマージ/削除できません)");
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::TextDisabled("右クリックで名前変更 / 削除");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ui::InputTextWithHint("##nb", "+ 新規ブランチ名 → Enter",
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

    // ---- ブランチの名前変更 / 削除（コンボの右クリックメニューから要求される）----
    // コンボが閉じたあとのこの位置で OpenPopup する（コンボの中からは開けない）。
    if (m_gitBranchOpRequest == 1) { ImGui::OpenPopup("##branchRename"); m_gitBranchOpRequest = 0; }
    if (m_gitBranchOpRequest == 2) { ImGui::OpenPopup("##branchDelete"); m_gitBranchOpRequest = 0; }
    if (m_gitBranchOpRequest == 3)
    {
        // ★取り込むコミット数はここで 1 回だけ数える（git のプロセス起動なので毎フレームは不可）。
        //   ツールバーから開いた場合は取り込み元が未選択なので、選ばれた時に数える。
        m_gitMergeCount = m_gitBranchOpTarget.empty()
                        ? -1
                        : GitIntegration::CommitsToMerge(root, m_gitBranchOpTarget);
        ImGui::OpenPopup("##branchMerge");
        m_gitBranchOpRequest = 0;
    }

    if (ImGui::BeginPopupModal("##branchMerge", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // 取り込み元。ブランチを右クリックして開いたときは選択済み、
        // ツールバーのマージ ボタンから開いたときは空なのでここで選ばせる。
        ImGui::Text("今いるブランチ「%s」へ取り込みます", branch.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("取り込み元");
        ImGui::SetNextItemWidth(ui::Px(320.0f));
        if (ui::BeginCombo("##mergesrc",
                m_gitBranchOpTarget.empty() ? "(ブランチを選ぶ)" : m_gitBranchOpTarget.c_str()))
        {
            for (const auto& b : m_gitBranches)
            {
                if (b == branch) continue;   // 自分自身は取り込めない
                if (ImGui::Selectable(b.c_str(), b == m_gitBranchOpTarget))
                {
                    m_gitBranchOpTarget = b;
                    // ★選び直した時だけ数え直す（git のプロセス起動なので毎フレームは不可）。
                    m_gitMergeCount = GitIntegration::CommitsToMerge(root, b);
                }
            }
            if (m_gitBranches.size() <= 1)
                ImGui::TextDisabled("(取り込めるブランチがありません)");
            ImGui::EndCombo();
        }
        ImGui::Spacing();

        if (m_gitBranchOpTarget.empty())
            ImGui::TextDisabled("取り込み元を選ぶと、入るコミット数が出ます");
        else if (m_gitMergeCount == 0)
            ImGui::TextColored(th::Good, "取り込むコミットはありません（すでに最新です）");
        else if (m_gitMergeCount > 0)
            ImGui::Text("取り込まれるコミット: %d 件", m_gitMergeCount);
        else
            ImGui::TextDisabled("取り込まれるコミット数を取得できませんでした");

        // ★未コミットの変更が残っていると git は merge を拒否することがある。
        //   エラーを見せてから気づかせるより、押す前に言う。
        if (!m_gitChanges.empty())
            ImGui::TextColored(th::Warn,
                "⚠ 未コミットの変更が %zu 件あります。先にコミットするか退避してください",
                m_gitChanges.size());

        ImGui::Spacing();
        ui::Checkbox("マージコミットを必ず作る (--no-ff)", &m_gitMergeNoFF);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ON: どのブランチから取り込んだかが履歴に残ります（機能ブランチ向け）\n"
                              "OFF: 早送りできるときは履歴を一直線のままにします");
        ImGui::TextDisabled("コンフリクトしたら下に一覧が出るので、そこで解消してコミットしてください");

        ImGui::Spacing();
        ImGui::BeginDisabled(busy || m_gitBranchOpTarget.empty());
        if (ImGui::Button("マージ", ui::Px(120.0f, 0.0f)))
        {
            const std::string target = m_gitBranchOpTarget;
            const bool        noff   = m_gitMergeNoFF;
            RunGitAsync("マージ",
                [root, target, noff]{ return GitIntegration::Merge(root, target, noff); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("##branchRename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("ブランチ名を変更: %s", m_gitBranchOpTarget.c_str());
        ImGui::SetNextItemWidth(ui::Px(320.0f));
        const bool enter = ui::InputText("##newbrname", m_gitRenameBranchBuf.data(),
                                            m_gitRenameBranchBuf.size(),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);

        const std::string newName = m_gitRenameBranchBuf.data();
        const bool nameOk = GitIntegration::IsValidBranchName(newName) && newName != m_gitBranchOpTarget;
        if (!newName.empty() && !GitIntegration::IsValidBranchName(newName))
            ImGui::TextColored(th::Bad, "ブランチ名に使えない文字が入っています");
        else
            ImGui::TextDisabled("空白や ~ ^ : ? * [ \\ .. は使えません");

        ImGui::BeginDisabled(busy || !nameOk);
        if (ImGui::Button("変更", ui::Px(120.0f, 0.0f)) || (enter && nameOk))
        {
            const std::string oldName = m_gitBranchOpTarget;
            const std::string nn      = newName;
            RunGitAsync("ブランチ名の変更",
                [root, oldName, nn]{ return GitIntegration::RenameBranch(root, oldName, nn); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("##branchDelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("ブランチを削除: %s", m_gitBranchOpTarget.c_str());
        ImGui::TextWrapped("未マージのコミットがある場合、通常の削除は git が拒否します。"
                           "「強制削除」はそのコミットを捨てます（元に戻せません）。");
        ImGui::Spacing();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("削除", ui::Px(120.0f, 0.0f)))
        {
            const std::string target = m_gitBranchOpTarget;
            RunGitAsync("ブランチ削除",
                [root, target]{ return GitIntegration::DeleteBranch(root, target, /*force=*/false); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("強制削除", ui::Px(120.0f, 0.0f)))
        {
            const std::string target = m_gitBranchOpTarget;
            RunGitAsync("ブランチ強制削除",
                [root, target]{ return GitIntegration::DeleteBranch(root, target, /*force=*/true); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ui::Px(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- 行2: GitHub アカウント ----
    if (m_ghAvailable)
        renderGitHubAccountRow();

    // ---- リポジトリ URL（リモートがある間は常時表示。クリックでブラウザを開く）----
    if (!remote.empty())
    {
        std::string webUrl = GitIntegration::ToWebUrl(remote);
        if (!webUrl.empty() && ImGui::SmallButton((ICON_LINK " " + webUrl).c_str()))
            dx12e::guard::ShellExecuteGuarded(nullptr, "open", webUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ---- 行3: 同期ツールバー（アイコンで並べる。ホバーで名前と意味が出る）----
    // ★以前は「更新」「フェッチ」の文字ボタンと ▲▼ の矢印ボタンが混在していて、
    //   どれが送信でどれが受信なのか押すまで分からなかった。色と形が違うアイコンなら
    //   狭いドックでも並びが崩れず、意味も一目で分かる。
    {
        ImGui::BeginDisabled(busy);

        // Lucide のアイコンボタン（フラット。ホバーで名前と意味が出る）。
        auto iconBtn = [&](const char* glyph, const char* id, const char* /*label*/, const char* tip) -> bool
        {
            return ui::IconButton(id, glyph, tip, false, nullptr, ui::Px(28.0f), ui::Px(17.0f));
        };

        if (iconBtn(ICON_REFRESH, "##girefresh", "更新",
                    "更新 — 変更とブランチ状態を取り直す"))
            m_gitForceRefresh = true;

        // マージはリモートが無くても使える（ローカルブランチ同士の取り込み）。
        ImGui::SameLine();
        if (iconBtn(ICON_GIT_MERGE, "##gimerge", "マージ",
                    "マージ — 他のブランチを今いるブランチへ取り込む"))
        {
            m_gitBranchOpTarget.clear();   // 取り込み元はダイアログで選ばせる
            m_gitMergeCount      = -1;
            m_gitBranchOpRequest = 3;
        }

        if (!remote.empty())
        {
            ImGui::SameLine(0, sp * 2);
            if (iconBtn(ICON_DOWNLOAD, "##gifetch", "フェッチ",
                        "フェッチ — リモートの最新を取ってくるだけ（作業ツリーは変えない）"))
                RunGitAsync("フェッチ", [root]{ return GitIntegration::Fetch(root); });
            ImGui::SameLine();
            if (iconBtn(ICON_ARROW_DOWN, "##gipull", "プル",
                        "プル — リモートの変更を取り込む（受信）"))
                RunGitAsync("プル", [root]{ return GitIntegration::Pull(root); });
            ImGui::SameLine();
            if (iconBtn(ICON_ARROW_UP, "##gipush", "プッシュ",
                        "プッシュ — 手元のコミットをリモートへ送る（送信）"))
                RunGitAsync("プッシュ", [root]{ return GitIntegration::Push(root, true); });

            // 未送信/未取得の件数。0 のときは出さない＝「何かある」ときだけ目に入る。
            const int ahead = m_gitAhead < 0 ? 0 : m_gitAhead;
            const int behind = m_gitBehind < 0 ? 0 : m_gitBehind;
            if (ahead > 0 || behind > 0)
            {
                ImGui::SameLine(0, sp * 2);
                ImGui::AlignTextToFramePadding();
                if (ahead > 0)
                {
                    ImGui::TextColored(th::Warn, "↑%d", ahead);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("未プッシュのコミットが %d 件", ahead);
                    if (behind > 0) ImGui::SameLine();
                }
                if (behind > 0)
                {
                    ImGui::TextColored(th::Warn, "↓%d", behind);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("未取得のコミットが %d 件", behind);
                }
            }
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
        ImGui::TextColored(th::Bad, "⚠ コンフリクト %zu 件（解消してからコミットしてください）", m_gitConflicts.size());
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
        // ★どうしても解消できないときの逃げ道。これが無いと、コンフリクトで止まった作業ツリーから
        //   エディタだけでは抜けられず、コマンドラインを開く羽目になる。
        ImGui::Separator();
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("マージを中止"))
            RunGitAsync("マージ中止", [root]{ return GitIntegration::AbortMerge(root); });
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("マージを取り消して、始める前の状態へ戻します（git merge --abort）");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    else if (m_gitMergeInProgress)
    {
        ImGui::TextColored(th::Good, "✓ コンフリクトはすべて解消しました。下でコミットしてマージを完了してください。");
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("マージを中止"))
            RunGitAsync("マージ中止", [root]{ return GitIntegration::AbortMerge(root); });
        ImGui::EndDisabled();
        ImGui::Spacing();
    }

    // ---- コミットメッセージ（複数行・空ならプレースホルダを重ね描き）----
    ImVec2 msgPos = ImGui::GetCursorScreenPos();
    ImGui::InputTextMultiline("##commitmsg", m_gitCommitMsgBuf.data(), m_gitCommitMsgBuf.size(),
                              ImVec2(-FLT_MIN, fh * 2.2f));
    if (m_gitCommitMsgBuf[0] == '\0')
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(msgPos.x + ui::Px(6.0f), msgPos.y + ImGui::GetStyle().FramePadding.y),
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
    icon(ICON_GIT_COMMIT, 18);
    if (ImGui::Button("すべてをコミット", ImVec2(ImGui::GetContentRegionAvail().x - fh - ui::Px(1.0f), 0)))
        doCommit(false);
    ImGui::SameLine(0, ui::Px(1.0f));
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
    if (ui::CollapsingHeader(changesHdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
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
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(s).x - ui::Px(2.0f));
                    ImGui::TextColored(stColor(f.second), "%s", s);
                }
            };
            ImGui::BeginChild("##changes", ImVec2(0, ui::Px(220.0f)), true);
            draw(rootNode);
            ImGui::EndChild();
        }
    }

    // ---- リモート未設定: URL 手動設定 + GitHub 新規作成 ----
    if (remote.empty())
    {
        ImGui::SeparatorText("リモート未設定");
        ImGui::TextDisabled("プッシュ先がまだありません。URL を設定するか、GitHub に新規作成してください。");
        ImGui::SetNextItemWidth(-ui::Px(70.0f));
        ui::InputTextWithHint("##remote", "https://github.com/owner/repo.git",
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
            ui::InputText("##reponame2", m_gitNewRepoNameBuf.data(), m_gitNewRepoNameBuf.size());

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


bool Application::BuildGameStandalone(const std::string& projectRoot)
{
    namespace fs = std::filesystem;
    // ★CLI(--build)はプロジェクトを【開かない】ので BeginProjectLoad を通らず、
    //   buildConfig が空のまま来る。ここで build_settings.json を読まないと開始シーンが
    //   既定の "scenes/default.json" になり、【存在しないシーンを指す配布物】ができる。
    //   (Junction を --build したら startScene = scenes/default.json で焼かれていた)
    if (m_editorCtx && !projectRoot.empty() && m_editorCtx->buildConfig.startScene.empty())
    {
        LoadProjectBuildConfig(*m_editorCtx, projectRoot);
        if (m_editorCtx->buildConfig.startScene.empty())
        {
            // build_settings.json が無いプロジェクトは .dx12proj の defaultScene を使う
            std::error_code ec;
            for (const auto& de : fs::directory_iterator(projectRoot, ec))
            {
                if (de.path().extension() != ".dx12proj") continue;
                ProjectInfo info;
                if (Project::Load(de.path().string(), info) && !info.defaultScene.empty())
                    m_editorCtx->buildConfig.startScene = info.defaultScene;
                break;
            }
        }
        // ★出力先だけは build_settings.json に従わない。CLI の --build は昔から
        //   <projectDir>/build/game に出す約束で、CI もそこを見る。GUI の Build ボタンだけが
        //   outputDir(ユーザーが選んだ配布先)を使う。
        m_editorCtx->buildConfig.outputDir.clear();
        if (!m_editorCtx->buildConfig.startScene.empty())
            Logger::Info("ヘッドレスビルド: 開始シーン = {}", m_editorCtx->buildConfig.startScene);
    }
    // 開始シーンを title.json に（あれば）。無ければ現在の currentScenePath を使う。
    // ★buildConfig.startScene が決まっていれば BuildGame がそちらを優先する。
    std::string title = PathResolver::AssetsDir() + "scenes/title.json";
    if (std::filesystem::exists(title))
        m_editorCtx->currentScenePath = title;
    return BuildGame();
}

// BuildGame / BakeTexturesIntoPak（ゲームのビルド本体と裏ジョブ化）は ApplicationBuild.cpp へ移した。

// 「ビルド設定」ウィンドウ（Unity の Build Settings / Unreal の Packaging 相当）。
// 構成・開始シーン・出力先を決めてから「ビルド」で BuildGame を実行する。
void Application::RenderBuildSettingsWindow()
{
    if (!m_editorCtx || !m_editorCtx->showBuildSettings)
        return;

    namespace fs = std::filesystem;
    auto& cfg = m_editorCtx->buildConfig;
    const auto before = cfg;   // フレーム末尾の変更検知→プロジェクトへ自動保存用

    ImGui::SetNextWindowSize(ui::Px(380.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ビルド設定", &m_editorCtx->showBuildSettings))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("ゲームを単体 exe + 暗号化アセット(game.pak) に書き出す");
    ImGui::Spacing();

    // ===== シーン =====
    if (ui::CollapsingHeader("シーン", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::vector<std::string> scenes;
        std::string scenesDir = PathResolver::AssetsDir() + "scenes";
        if (fs::exists(scenesDir))
            for (auto& e : fs::directory_iterator(scenesDir))
                if (e.is_regular_file() && e.path().extension() == ".json")
                    scenes.push_back("scenes/" + e.path().filename().string());

        const char* curLabel = cfg.startScene.empty()
            ? "(\xe7\x8f\xbe\xe5\x9c\xa8\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3)"  // (現在開いているシーン)
            : cfg.startScene.c_str();
        if (pg::Begin("##bsScene"))
        {
        pg::Label("開始シーン", "全シーンが game.pak に含まれます。起動シーンを選びます。");
        if (ui::BeginCombo("##startScene", curLabel))
        {
            if (ImGui::Selectable("(\xe7\x8f\xbe\xe5\x9c\xa8\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3)",
                                  cfg.startScene.empty()))
                cfg.startScene.clear();
            for (auto& s : scenes)
                if (ImGui::Selectable(s.c_str(), s == cfg.startScene))
                    cfg.startScene = s;
            ImGui::EndCombo();
        }
        pg::End();
        }
    }

    // ===== 製品 =====
    if (ui::CollapsingHeader("製品", ImGuiTreeNodeFlags_DefaultOpen))
    {
        struct Res { const char* name; int w, h; };
        static const Res presets[] = {
            {"1280 x 720 (HD)",   1280, 720},
            {"1600 x 900",        1600, 900},
            {"1920 x 1080 (FHD)", 1920, 1080},
            {"2560 x 1440 (QHD)", 2560, 1440},
        };
        std::string cur = std::to_string(cfg.width) + " x " + std::to_string(cfg.height);
        if (pg::Begin("##bsProduct"))
        {
            pg::InputText("ゲーム名", cfg.title, sizeof(cfg.title), 0, nullptr,
                          "ウィンドウタイトル / exe名。exe名・フォルダ名には英数・空白・_- のみが使われます");
            pg::Label("解像度");
            if (ui::BeginCombo("##respreset", cur.c_str()))
            {
                for (auto& p : presets)
                    if (ImGui::Selectable(p.name, p.w == cfg.width && p.h == cfg.height))
                    {
                        cfg.width  = p.w;
                        cfg.height = p.h;
                    }
                ImGui::EndCombo();
            }
            pg::Int("幅", &cfg.width, 1.0f, 320, 7680);
            pg::Int("高さ", &cfg.height, 1.0f, 240, 4320);
            pg::End();
        }
        cfg.width  = std::clamp(cfg.width,  320, 7680);
        cfg.height = std::clamp(cfg.height, 240, 4320);
    }

    // ===== 出力先 =====
    if (ui::CollapsingHeader("出力先", ImGuiTreeNodeFlags_DefaultOpen))
    {
        char pathBuf[1024];
        strncpy_s(pathBuf,
                  cfg.outputDir.empty()
                    ? "(\xe6\x9c\xaa\xe9\x81\xb8\xe6\x8a\x9e \xe2\x80\x94 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe6\x99\x82\xe3\x81\xab\xe9\x81\xb8\xe6\x8a\x9e)"  // (未選択 — ビルド時に選択)
                    : cfg.outputDir.c_str(),
                  _TRUNCATE);
        if (pg::Begin("##bsOutput"))
        {
            pg::Label("配置先フォルダ", "選んだフォルダ直下に \"<製品名>_build\" を作って出力します。");
            ImGui::SetNextItemWidth(-ui::Px(92.0f));
            ui::InputText("##outdir", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("\xe5\x8f\x82\xe7\x85\xa7...", ImVec2(-1.0f, 0.0f)))  // 参照...
            {
                std::string dir;
                if (ProjectManager::PickFolder(m_window->GetHwnd(), dir, L"ビルドの配置先フォルダを選択"))
                    cfg.outputDir = dir;
            }
            pg::Checkbox("ビルド後にフォルダを開く", &cfg.openFolderAfterBuild);
            pg::End();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ===== ビルド実行 =====
    // ビルドは裏ジョブ（エディタは操作できるまま）。走っている間は進捗とキャンセルを出す。
    if (m_editorCtx->buildRunning && m_buildJob)
    {
        const BuildProgress& bp = m_buildJob->progress;
        const int stage = bp.stage.load();
        ImGui::TextUnformatted("ビルド中...");
        ImGui::SameLine();
        ImGui::TextDisabled("%d/%d %s", stage, BuildProgress::kStageCount, BuildProgress::StageName(stage));
        const bool indeterminate = (stage >= 4 && bp.total.load() == 0);
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%.0f%%  (%.0f 秒)", m_editorCtx->buildFraction * 100.0f, bp.ElapsedSec());
        // 進みが読めない段（テクスチャの事前生成）は流れるバー。それ以外は割合。
        ImGui::ProgressBar(indeterminate ? -1.0f * static_cast<float>(ImGui::GetTime()) : m_editorCtx->buildFraction,
                           ImVec2(-1.0f, ui::Px(20.0f)), indeterminate ? "" : overlay);
        std::string detail;
        { std::lock_guard<std::mutex> lk(bp.mu); detail = bp.detail; }
        if (!detail.empty()) ImGui::TextDisabled("%s", detail.c_str());
        ImGui::TextDisabled("ビルド中も編集できますが、配布物には保存済みの内容が入ります。");
        if (ImGui::Button("キャンセル", ImVec2(-1.0f, ui::Px(30.0f))))
            CancelBuildGame();
    }
    else
    {
    const bool doBuild = ui::PrimaryButton("ビルド", ImVec2(-1.0f, ui::Px(38.0f)));
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
            m_editorCtx->pendingBuildGame = true;   // フレーム境界で裏ジョブとして開始
    }
    }

    if (m_editorCtx->buildCompleteFlash > 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Good);
        ImGui::TextUnformatted("\xe2\x9c\x93 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86");  // ✓ ビルド完了
        ImGui::PopStyleColor();
        m_editorCtx->buildCompleteFlash -= m_gameClock.GetDeltaTime();
    }
    else if (m_editorCtx->buildErrorFlash > 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad);
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
