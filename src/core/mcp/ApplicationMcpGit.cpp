// ===========================================================================
// MCP: Git / GitHub（状態・ブランチ・マージ・コミット・プッシュ）
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の
// DX12E_MCP_HANDLER 付近）。
//
// ★これらは「プロジェクトフォルダの git リポジトリ」に対する操作で、エンジンの状態は
//   一切変えない。エディタの Git 窓が押しているのと同じ GitIntegration を呼ぶだけなので、
//   UI からやったのと結果が食い違うことはない。
//
// ★git のプロセス起動はどれも数十 ms かかる。MCP は同期応答なので、ここでは
//   「呼ばれた回数ぶんだけ起動する」＝毎フレーム走る類のものは 1 つも無い、で問題ない。
//
// ★外向きの操作（push / リモートへ出るもの）は AI が勝手に撃てる場所なので、
//   何をしたかが必ず result に残るようにしている（output に git の生出力をそのまま入れる）。
// ===========================================================================
#include "core/ApplicationInternal.h"

#include "project/GitIntegration.h"

namespace dx12e
{
using namespace appdetail;

namespace
{

// プロジェクトのルート（= git の作業ツリー）。開いていなければ MCP エラー。
// ここを 1 本にしておけば、各ハンドラは「リポジトリがある前提」で書ける。
std::string RequireRepoRoot(const std::string& rootDir)
{
    if (rootDir.empty())
        throw McpError(McpErr::NotFound, "no project is open",
                       "先にプロジェクトを開くこと（dx12_get_mode でエディタの状態を確認できる）");
    if (!GitIntegration::IsGitAvailable())
        throw McpError(McpErr::Internal, "git command not found",
                       "PATH に git が無い。エディタの Git 窓の「Git をインストール」から入れられる");
    if (!GitIntegration::IsRepo(rootDir))
        throw McpError(McpErr::NotFound, "project folder is not a git repository: " + rootDir,
                       "エディタの Git 窓で「Git リポジトリを初期化」してから使うこと");
    return rootDir;
}

// GitResult をそのまま MCP の応答へ。失敗も例外にせず ok=false で返す
// （git の失敗は「壊れた」ではなく「その操作ができない状態だった」なので、
//   AI が output を読んで次の手を決められる形にする方が回復しやすい）。
void PutGitResult(nlohmann::json& resp, const GitResult& r, nlohmann::json extra = {})
{
    resp["ok"] = true;
    nlohmann::json result = {
        {"succeeded", r.ok()},
        {"exitCode",  r.exitCode},
        {"output",    r.output},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it)
        result[it.key()] = it.value();
    resp["result"] = std::move(result);
}

} // namespace

// ---- Git / GitHub ----
void Application::RegisterMcpGitMethods()
{
    using json = nlohmann::json;

    McpDefine("git_status", "", DX12E_MCP_HANDLER
        {
            // 今のブランチ / リモート / 変更ファイル / マージ中かどうかを 1 回で返す。
            // ★何かする前にこれを読めば、「未コミットの変更があるのに merge を撃つ」
            //   のような失敗する操作を事前に避けられる。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);

            json files = json::array();
            for (const GitFileChange& c : GitIntegration::ChangedFiles(root))
                files.push_back({{"path", c.path},
                                 {"status", std::string(1, c.status)},
                                 {"staged", c.staged}});

            json conflicts = json::array();
            for (const std::string& p : GitIntegration::ConflictedFiles(root))
                conflicts.push_back(p);

            resp["ok"] = true;
            resp["result"] = {
                {"branch",          GitIntegration::CurrentBranch(root)},
                {"remoteUrl",       GitIntegration::RemoteUrl(root)},
                {"changedFiles",    files},
                {"mergeInProgress", GitIntegration::IsMergeInProgress(root)},
                {"conflicts",       conflicts},
                {"githubUser",      GitIntegration::GitHubUser()},
            };
        });

    McpDefine("git_branches", "", DX12E_MCP_HANDLER
        {
            // ローカルブランチ一覧と、今いるブランチ。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            const std::string cur  = GitIntegration::CurrentBranch(root);

            json list = json::array();
            for (const std::string& b : GitIntegration::ListBranches(root))
                list.push_back({{"name", b}, {"current", b == cur}});

            resp["ok"] = true;
            resp["result"] = {{"current", cur}, {"branches", list}};
        });

    McpDefine("git_checkout", "create:bool,name:string", DX12E_MCP_HANDLER
        {
            // ブランチを切り替える。create=true なら新規作成して切り替える。
            // ★エディタで開いているシーンのファイルが切り替えで消える/変わることがある。
            //   実行後は dx12_git_status と dx12_list_scenes を読み直すこと。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            const std::string name = params.value("name", std::string());
            if (!GitIntegration::IsValidBranchName(name))
                throw McpError(McpErr::InvalidParam, "invalid branch name: " + name,
                               "空白や ~ ^ : ? * [ \\ .. は git が受け付けない");

            const bool create = params.value("create", false);
            const GitResult r = create ? GitIntegration::CreateBranch(root, name)
                                       : GitIntegration::CheckoutBranch(root, name);
            PutGitResult(resp, r, {{"branch", GitIntegration::CurrentBranch(root)},
                                   {"created", create}});
        });

    McpDefine("git_merge", "name:string,noFastForward:bool", DX12E_MCP_HANDLER
        {
            // name を「今いるブランチ」へ取り込む（エディタの Git 窓のマージと同じ）。
            // ★コンフリクトしたら succeeded=false で返り、conflicts に未解消ファイルが入る。
            //   その状態のまま作業ツリーは止まっているので、
            //   直す → dx12_git_commit で完了、または dx12_git_merge_abort で取り消す。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            const std::string name = params.value("name", std::string());
            const std::string cur  = GitIntegration::CurrentBranch(root);
            if (name.empty())
                throw McpError(McpErr::InvalidParam, "missing name (branch to merge from)",
                               "dx12_git_branches で取り込み元の候補を確認すること");
            if (name == cur)
                throw McpError(McpErr::InvalidParam, "cannot merge a branch into itself: " + name,
                               "dx12_git_branches で取り込み元の候補を確認すること");

            // 未コミットの変更があると git が merge を拒否することがある。
            // 「撃ってから失敗を読む」より、先に理由を返す方が回復が速い。
            if (!GitIntegration::ChangedFiles(root).empty()
                && !GitIntegration::IsMergeInProgress(root))
                throw McpError(McpErr::ModeConflict, "working tree has uncommitted changes",
                               "先に dx12_git_commit でコミットするか、変更を戻してから merge すること");

            const bool noff = params.value("noFastForward", true);
            const GitResult r = GitIntegration::Merge(root, name, noff);

            json conflicts = json::array();
            for (const std::string& p : GitIntegration::ConflictedFiles(root))
                conflicts.push_back(p);

            PutGitResult(resp, r, {{"mergedFrom", name},
                                   {"into", cur},
                                   {"mergeInProgress", GitIntegration::IsMergeInProgress(root)},
                                   {"conflicts", conflicts}});
        });

    McpDefine("git_merge_abort", "", DX12E_MCP_HANDLER
        {
            // 進行中のマージを取り消して、始める前の作業ツリーへ戻す。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            if (!GitIntegration::IsMergeInProgress(root))
                throw McpError(McpErr::ModeConflict, "no merge in progress",
                               "dx12_git_status の mergeInProgress を先に確認すること");
            PutGitResult(resp, GitIntegration::AbortMerge(root));
        });

    McpDefine("git_commit", "message:string", DX12E_MCP_HANDLER
        {
            // 変更を全部ステージしてコミットする（git add -A && git commit）。
            // ★マージのコンフリクトを解消した後の「マージを終わらせるコミット」もこれ。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            const std::string msg  = params.value("message", std::string());
            if (msg.empty())
                throw McpError(McpErr::InvalidParam, "commit message must not be empty");

            // 解消し残したコンフリクトがあるままコミットさせない
            // （git は通らないし、通ったとしても競合マーカー入りのファイルが入るだけ）。
            const std::vector<std::string> unresolved = GitIntegration::ConflictedFiles(root);
            if (!unresolved.empty())
                throw McpError(McpErr::ModeConflict,
                               "unresolved conflicts remain (" + std::to_string(unresolved.size()) + " files)",
                               "dx12_git_status の conflicts を読んで、各ファイルを直してから再実行すること");

            GitIntegration::EnsureIdentity(root);   // user.name/email 未設定の新規マシン救済
            PutGitResult(resp, GitIntegration::CommitAll(root, msg),
                         {{"branch", GitIntegration::CurrentBranch(root)}});
        });

    McpDefine("git_push", "", DX12E_MCP_HANDLER
        {
            // 今のブランチをリモートへ送る（upstream が無ければ -u で設定して送る）。
            // ★これは【外向き】の操作。押した内容は他人から見える場所へ出る。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            if (GitIntegration::RemoteUrl(root).empty())
                throw McpError(McpErr::NotFound, "no remote (origin) is configured",
                               "エディタの Git 窓で GitHub リポジトリを作成するか、リモート URL を設定すること");
            PutGitResult(resp, GitIntegration::Push(root, /*setUpstream=*/true),
                         {{"branch", GitIntegration::CurrentBranch(root)}});
        });

    McpDefine("git_pull", "", DX12E_MCP_HANDLER
        {
            // リモートの変更を取り込む。コンフリクトしたら merge と同じ形で返る。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            const GitResult r = GitIntegration::Pull(root);

            json conflicts = json::array();
            for (const std::string& p : GitIntegration::ConflictedFiles(root))
                conflicts.push_back(p);

            PutGitResult(resp, r, {{"mergeInProgress", GitIntegration::IsMergeInProgress(root)},
                                   {"conflicts", conflicts}});
        });

    McpDefine("git_fetch", "", DX12E_MCP_HANDLER
        {
            // リモートの最新を取ってくるだけ（作業ツリーは変えない）。
            const std::string root = RequireRepoRoot(m_projectInfo.rootDir);
            PutGitResult(resp, GitIntegration::Fetch(root));
        });
}

} // namespace dx12e
