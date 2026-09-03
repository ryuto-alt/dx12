#pragma once

#include <atomic>
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

// 変更ファイル1件（git status の1行）。VS の「変更」リスト表示用。
struct GitFileChange
{
    std::string path;     // リポジトリ相対パス（リネームは新名）。区切りは '/'。
    char        status;   // 表示用1文字: 'A'追加/未追跡 'M'変更 'D'削除 'R'リネーム 'U'競合
    bool        staged;   // index に乗っているか（今は表示用。コミットは add -A 一括）
};

class GitIntegration
{
public:
    // git / gh が PATH に存在するか
    static bool IsGitAvailable();
    static bool IsGhAvailable();

    // git が未インストールの環境向け: winget があれば `winget install Git.Git` を別コンソールで実行し
    // 終了を待つ（gh ログインと同じ待ち方）。winget が無ければ公式ダウンロードページをブラウザで開く。
    // abortFlag はシャットダウン時の待機切り上げ用（LoginAndWait と同様、子プロセス自体は残る）。
    static GitResult InstallGit(const std::atomic<bool>& abortFlag);

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
    static std::vector<GitFileChange> ChangedFiles(const std::string& workDir); // 変更ファイル一覧（porcelain 解析）
    static std::string CurrentBranch(const std::string& workDir);
    static std::string RemoteUrl(const std::string& workDir);               // origin の URL（無ければ空）

    // ---- マージコンフリクト ----
    // pull/merge の途中か（.git/MERGE_HEAD の有無）。解消後 commit するまで true のまま。
    static bool IsMergeInProgress(const std::string& workDir);
    // 未解消（'U'）のファイルのリポジトリ相対パス一覧
    static std::vector<std::string> ConflictedFiles(const std::string& workDir);
    // 指定ファイルを自分側(HEAD)の内容で解消して add する
    static GitResult ResolveOurs(const std::string& workDir, const std::string& path);
    // 指定ファイルを相手側(取り込んだブランチ)の内容で解消して add する
    static GitResult ResolveTheirs(const std::string& workDir, const std::string& path);
    // コンフリクト中のファイルを外部アプリで開く（VSCode があればそれ、無ければ既定アプリ）
    static void OpenConflictFile(const std::string& workDir, const std::string& relPath);

    // gh でリポジトリ作成してプッシュ（gh が必要）。private=true で非公開。
    static GitResult CreateGitHubRepo(const std::string& workDir,
                                      const std::string& repoName,
                                      bool isPrivate);

    // ---- GitHub ログイン（gh CLI） ----
    // gh の認証ユーザー名（未ログイン/未インストールなら空）。ログイン判定にも使う。
    static std::string GitHubUser();
    // 別コンソールウィンドウで `gh auth login --web` を起動し、子プロセスの終了をそのまま待つ
    // （ポーリングではなくイベント待ちなので、ブラウザ承認した瞬間に検知できる）。
    // abortFlag が立ったら待機だけ切り上げる（子プロセス自体は残る＝ユーザーは認証継続可）。
    // 成功したら output にログインユーザー名を入れて返す（git の資格情報ヘルパも gh に設定済み）。
    static GitResult LoginAndWait(const std::atomic<bool>& abortFlag);

    // ---- クローン ----
    // url を destDir（クローン先の親フォルダ直下に repo 名で展開される）へ clone する。
    // outRepoDir に実際の作業ツリー（destDir/<repo名>）を返す。
    static GitResult Clone(const std::string& url, const std::string& destParentDir,
                           std::string& outRepoDir);
    // git URL から末尾のリポジトリ名（.git を除く）を取り出す
    static std::string RepoNameFromUrl(const std::string& url);
    // remote の URL（https/ssh どちらの形式でも）を https://github.com/owner/repo 形式に変換。
    // github.com 以外やパース不能なら空文字を返す（ブラウザで開くリンク表示用）。
    static std::string ToWebUrl(const std::string& remoteUrl);

    // ---- ブランチ ----
    static std::vector<std::string> ListBranches(const std::string& workDir); // ローカルブランチ名
    static GitResult CheckoutBranch(const std::string& workDir, const std::string& name); // 既存へ切替
    static GitResult CreateBranch(const std::string& workDir, const std::string& name);   // 新規作成して切替
    // ブランチ名の変更（git branch -m）。oldName が今いるブランチでも動く。
    static GitResult RenameBranch(const std::string& workDir,
                                  const std::string& oldName, const std::string& newName);
    // ブランチの削除。force=false は -d（未マージなら git が拒否する＝安全側）、true は -D（強制）。
    // 今いるブランチは削除できない（git がエラーにする）ので、呼ぶ側で先に切り替えること。
    static GitResult DeleteBranch(const std::string& workDir, const std::string& name, bool force);

    // ---- マージ ----
    // name を「今いるブランチ」へ取り込む（git merge）。
    // noFastForward=true なら --no-ff ＝ 早送りできる場合でもマージコミットを必ず作る
    // （どのブランチから来たかが履歴に残る。機能ブランチの取り込みではこちらが好まれる）。
    // ★コンフリクトで止まった場合も ok()==false で返る。IsMergeInProgress が true の間は
    //   ConflictedFiles / ResolveOurs / ResolveTheirs で解消し、コミットして完了させる。
    static GitResult Merge(const std::string& workDir, const std::string& name, bool noFastForward);

    // 進行中のマージを取り消して元の状態へ戻す（git merge --abort）。
    // コンフリクトを解消しきれないときの逃げ道。マージ前の作業ツリーへ戻る。
    static GitResult AbortMerge(const std::string& workDir);

    // target を今いるブランチへ取り込むと何コミット入るか（git rev-list --count HEAD..target）。
    // 取得できなければ -1、0 なら「取り込むものが無い＝既に最新」。
    // ★押す前に「何も起きない」と分かるようにするためのもの。git を 1 回起動するので
    //   毎フレームではなく、マージのダイアログを開く瞬間だけ呼ぶこと。
    static int CommitsToMerge(const std::string& workDir, const std::string& target);

    // ブランチ名として git が受け付ける形か（空 / 空白 / 制御文字 / ~^:?*[ / .. / 先頭末尾の記号）。
    // git に投げる前に弾くのは、エラー出力より「なぜダメか」を UI で言える方が親切なため。
    static bool IsValidBranchName(const std::string& name);

    // 標準的な .gitignore を workDir に書き出す（既存なら何もしない）
    static void WriteGitignore(const std::string& workDir);

    // コミット用の author 情報（user.name / user.email）が解決できなければ、
    // このリポジトリ(ローカル設定)に既定値を書き込む。新規マシンでは global 未設定が普通で、
    // その場合 git commit が "Author identity unknown" で失敗する＝コミットできない原因になる。
    // 値は gh のログインユーザーから推定（無ければ汎用の noreply）。global は触らない。
    static void EnsureIdentity(const std::string& workDir);

private:
    // exe を引数付きで workDir 実行（コンソールウィンドウを出さない）。-1=起動失敗
    static GitResult Run(const std::string& exe, const std::string& args,
                         const std::string& workDir);
};

} // namespace dx12e
