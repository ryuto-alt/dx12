#pragma once

#include <string>
#include <vector>

namespace dx12e
{

// プロジェクトフォルダに対する Git / GitHub(gh) 操作のラッパー。
// すべて外部 CLI（git / gh）を CreateProcess で実行し、stdout+stderr をまとめて取得する。
// 認証はユーザーの既存の git/gh 資格情報（Credential Manager 等）に委ねる。
struct GitResult
{
    int         exitCode = -1;
    std::string output;          // stdout + stderr
    bool        ok() const { return exitCode == 0; }
};

class GitIntegration
{
public:
    // git / gh が PATH に存在するか
    static bool IsGitAvailable();
    static bool IsGhAvailable();

    // workDir が git リポジトリ（.git を持つ作業ツリー）か
    static bool IsRepo(const std::string& workDir);

    // 任意の git サブコマンドを workDir で実行
    static GitResult RunGit(const std::string& workDir, const std::string& args);
    // 任意の gh サブコマンドを workDir で実行
    static GitResult RunGh(const std::string& workDir, const std::string& args);

    // よく使う操作
    static GitResult Init(const std::string& workDir);                       // git init + 既定ブランチ main
    static GitResult AddRemote(const std::string& workDir, const std::string& url);  // origin 設定/更新
    static GitResult CommitAll(const std::string& workDir, const std::string& message); // add -A && commit
    static GitResult Push(const std::string& workDir, bool setUpstream);     // git push (-u origin <branch>)
    static GitResult Pull(const std::string& workDir);                       // git pull
    static GitResult Fetch(const std::string& workDir);                      // git fetch --all --prune
    static GitResult Status(const std::string& workDir);                     // git status --short --branch
    static std::string CurrentBranch(const std::string& workDir);
    static std::string RemoteUrl(const std::string& workDir);               // origin の URL（無ければ空）

    // gh でリポジトリ作成してプッシュ（gh が必要）。private=true で非公開。
    static GitResult CreateGitHubRepo(const std::string& workDir,
                                      const std::string& repoName,
                                      bool isPrivate);

    // ---- GitHub ログイン（gh CLI） ----
    // gh の認証ユーザー名（未ログイン/未インストールなら空）。ログイン判定にも使う。
    static std::string GitHubUser();
    // 別コンソールウィンドウで `gh auth login` を対話起動する（デバイス認証フロー）。
    // 起動できたら true。完了後は git の資格情報ヘルパも gh に設定する。
    static bool LaunchLogin();

    // ---- クローン ----
    // url を destDir（クローン先の親フォルダ直下に repo 名で展開される）へ clone する。
    // outRepoDir に実際の作業ツリー（destDir/<repo名>）を返す。
    static GitResult Clone(const std::string& url, const std::string& destParentDir,
                           std::string& outRepoDir);
    // git URL から末尾のリポジトリ名（.git を除く）を取り出す
    static std::string RepoNameFromUrl(const std::string& url);

    // ---- ブランチ ----
    static std::vector<std::string> ListBranches(const std::string& workDir); // ローカルブランチ名
    static GitResult CheckoutBranch(const std::string& workDir, const std::string& name); // 既存へ切替
    static GitResult CreateBranch(const std::string& workDir, const std::string& name);   // 新規作成して切替

    // 標準的な .gitignore を workDir に書き出す（既存なら何もしない）
    static void WriteGitignore(const std::string& workDir);

private:
    // exe を引数付きで workDir 実行（コンソールウィンドウを出さない）。-1=起動失敗
    static GitResult Run(const std::string& exe, const std::string& args,
                         const std::string& workDir);
};

} // namespace dx12e
