#include "project/GitIntegration.h"
#include "core/Logger.h"

#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <vector>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace dx12e
{

GitResult GitIntegration::Run(const std::string& exe, const std::string& args,
                              const std::string& workDir)
{
    GitResult result;

    // 子プロセス(git/gh)が資格情報やパスフレーズを対話で要求すると、コンソール非表示
    // (CREATE_NO_WINDOW)+stdin 無しのため固まる(WaitForSingleObject INFINITE)。
    // これらを切って「即失敗→出力にエラー」に倒す。プロセス全体に一度だけ設定（子が継承する）。
    // gh の資格情報ヘルパが効いていれば通常はそもそもプロンプトは出ない。
    static const bool s_promptOff = []{
        SetEnvironmentVariableA("GIT_TERMINAL_PROMPT", "0");   // git 自身のプロンプト抑止
        SetEnvironmentVariableA("GCM_INTERACTIVE", "Never");   // Git Credential Manager の GUI 抑止
        return true;
    }();
    (void)s_promptOff;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        result.output = "CreatePipe failed";
        return result;
    }
    // 親側の read ハンドルは継承させない
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // コマンドライン構築: "exe args"（exe は PATH 解決のためそのまま渡す）。
    // コミットメッセージ等は UTF-8 で保持しているため、CreateProcessA（システムANSI
    // コードページ＝日本語環境では通常 Shift-JIS で解釈）に渡すと文字化けし、稀に
    // 変換結果に " が現れて引数の区切りが壊れ commit 自体が失敗する（→push も失敗扱い）。
    // UTF-8→UTF-16 に明示変換して CreateProcessW へ渡す。
    auto Utf8ToWide = [](const std::string& s) -> std::wstring {
        if (s.empty()) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
        return w;
    };

    std::string cmdLine = exe + " " + args;
    std::wstring wCmdLine = Utf8ToWide(cmdLine);
    std::vector<wchar_t> cmdBuf(wCmdLine.begin(), wCmdLine.end());
    cmdBuf.push_back(L'\0');

    std::wstring wWorkDir = Utf8ToWide(workDir);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError  = writePipe;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        wWorkDir.empty() ? nullptr : wWorkDir.c_str(),
        &si, &pi);

    // 書き込み側は親では使わない → 閉じておかないと read が EOF を検出できない
    CloseHandle(writePipe);

    if (!ok)
    {
        CloseHandle(readPipe);
        result.exitCode = -1;
        result.output   = "Failed to launch: " + exe;
        return result;
    }

    // 出力を読み切る
    std::string output;
    std::array<char, 4096> buf{};
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buf.data(), (DWORD)buf.size(), &bytesRead, nullptr) && bytesRead > 0)
        output.append(buf.data(), bytesRead);
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    result.exitCode = (int)code;
    result.output   = std::move(output);
    return result;
}

GitResult GitIntegration::RunGit(const std::string& workDir, const std::string& args)
{
    return Run("git", args, workDir);
}

GitResult GitIntegration::RunGh(const std::string& workDir, const std::string& args)
{
    return Run("gh", args, workDir);
}

bool GitIntegration::IsGitAvailable()
{
    return Run("git", "--version", "").ok();
}

bool GitIntegration::IsGhAvailable()
{
    return Run("gh", "--version", "").ok();
}

GitResult GitIntegration::InstallGit(const std::atomic<bool>& abortFlag)
{
    GitResult result;
    static const char* kDownloadPage = "https://git-scm.com/download/win";

    // winget（Win10 1809+/Win11 標準搭載）があればサイレントインストールを試す。
    if (Run("winget", "--version", "").ok())
    {
        std::string cmd = "winget install --id Git.Git -e --source winget "
                           "--accept-package-agreements --accept-source-agreements";
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // gh ログインと同じ流儀: 別コンソールで進捗/確認プロンプトをそのまま見せる。
        BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                 CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
        if (!ok)
        {
            result.exitCode = -1;
            result.output   = "winget の起動に失敗したで。手動でダウンロードページを開くで。";
            ShellExecuteA(nullptr, "open", kDownloadPage, nullptr, nullptr, SW_SHOWNORMAL);
            return result;
        }

        DWORD code = 1;
        for (;;)
        {
            DWORD wr = WaitForSingleObject(pi.hProcess, 500);
            if (wr == WAIT_OBJECT_0) { GetExitCodeProcess(pi.hProcess, &code); break; }
            if (abortFlag.load()) break;   // シャットダウン中: 子は残しても無害
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        result.exitCode = (int)code;
        if (code == 0)
        {
            result.output = "Git をインストールしたで。このパネルを開き直すと反映されるで"
                             "（反映されなければ一度エディタを再起動してや＝PATH の再読み込みのため）。";
        }
        else
        {
            result.output = "winget でのインストールに失敗した（コード " + std::to_string((int)code) +
                             "）。手動でダウンロードページを開くで。";
            ShellExecuteA(nullptr, "open", kDownloadPage, nullptr, nullptr, SW_SHOWNORMAL);
        }
        return result;
    }

    // winget が無い環境: 公式ダウンロードページを開いて手動インストールしてもらう
    ShellExecuteA(nullptr, "open", kDownloadPage, nullptr, nullptr, SW_SHOWNORMAL);
    result.exitCode = 0;
    result.output = "winget が見つからへんかったから、ブラウザで公式ダウンロードページを開いたで。"
                    "インストーラーを実行してから、このパネルを開き直してや。";
    return result;
}

bool GitIntegration::IsRepo(const std::string& workDir)
{
    if (workDir.empty()) return false;
    std::error_code ec;
    if (fs::exists(fs::path(workDir) / ".git", ec)) return true;
    // サブフォルダから開いた場合に備えて git にも確認
    auto r = RunGit(workDir, "rev-parse --is-inside-work-tree");
    return r.ok() && r.output.find("true") != std::string::npos;
}

GitResult GitIntegration::Init(const std::string& workDir)
{
    auto r = RunGit(workDir, "init -b main");
    if (!r.ok())
    {
        // 古い git は -b 非対応 → 通常 init 後にブランチ名変更
        r = RunGit(workDir, "init");
        if (r.ok())
            RunGit(workDir, "checkout -b main");
    }
    WriteGitignore(workDir);
    EnsureIdentity(workDir);   // 初回コミットで identity 未設定で詰まないよう、作成時に補っておく
    return r;
}

GitResult GitIntegration::AddRemote(const std::string& workDir, const std::string& url)
{
    // 既存 origin があれば set-url、無ければ add
    auto existing = RemoteUrl(workDir);
    if (existing.empty())
        return RunGit(workDir, "remote add origin " + url);
    return RunGit(workDir, "remote set-url origin " + url);
}

GitResult GitIntegration::CommitAll(const std::string& workDir, const std::string& message)
{
    // commit が "Author identity unknown" で落ちないよう、未設定なら identity を補う。
    EnsureIdentity(workDir);

    auto add = RunGit(workDir, "add -A");
    if (!add.ok()) return add;

    // メッセージ中の " をエスケープ
    std::string msg = message;
    std::string escaped;
    for (char c : msg)
    {
        if (c == '"') escaped += "\\\"";
        else escaped += c;
    }
    auto commit = RunGit(workDir, "commit -m \"" + escaped + "\"");
    commit.output = add.output + commit.output;
    return commit;
}

GitResult GitIntegration::Push(const std::string& workDir, bool setUpstream)
{
    std::string branch = CurrentBranch(workDir);
    if (branch.empty()) branch = "main";
    if (setUpstream)
        return RunGit(workDir, "push -u origin " + branch);
    return RunGit(workDir, "push");
}

GitResult GitIntegration::Pull(const std::string& workDir)
{
    return RunGit(workDir, "pull");
}

GitResult GitIntegration::Fetch(const std::string& workDir)
{
    return RunGit(workDir, "fetch --all --prune");
}

GitResult GitIntegration::Status(const std::string& workDir)
{
    return RunGit(workDir, "status --short --branch");
}

std::vector<GitFileChange> GitIntegration::ChangedFiles(const std::string& workDir)
{
    std::vector<GitFileChange> out;
    // core.quotepath=false で日本語パスを生 UTF-8 で受け取る（octal エスケープ回避）。
    auto r = RunGit(workDir, "-c core.quotepath=false status --porcelain=v1 -uall");
    if (!r.ok()) return out;

    std::istringstream iss(r.output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 4) continue;           // "XY path"

        char X = line[0], Y = line[1];           // index / worktree ステータス
        std::string path = line.substr(3);

        // リネーム "old -> new" は新名を採用
        size_t arrow = path.find(" -> ");
        if (arrow != std::string::npos) path = path.substr(arrow + 4);
        // 空白等を含むパスは git が "..." で囲む → 外す
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);

        // コンフリクト（未マージ）は X/Y の組み合わせで判定: UU/AA/DD/AU/UA/UD/DU。
        // 単純に X を見るだけだと AA(両者追加) や DD(両者削除) が 'A'/'D' 扱いになって見逃す。
        bool isConflict = (X == 'U' || Y == 'U' || (X == 'A' && Y == 'A') || (X == 'D' && Y == 'D'));

        GitFileChange c;
        c.path   = path;
        c.staged = (X != ' ' && X != '?');
        char raw = (X != ' ') ? X : Y;           // staged 優先、無ければ worktree
        if (raw == '?') raw = 'A';               // 未追跡=追加扱い（VS と同じ）
        if (isConflict) raw = 'U';
        c.status = raw;
        out.push_back(std::move(c));
    }
    return out;
}

std::string GitIntegration::CurrentBranch(const std::string& workDir)
{
    auto r = RunGit(workDir, "rev-parse --abbrev-ref HEAD");
    if (!r.ok()) return "";
    std::string b = r.output;
    // 末尾の改行/空白を除去
    while (!b.empty() && (b.back() == '\n' || b.back() == '\r' || b.back() == ' '))
        b.pop_back();
    if (b == "HEAD") return "";  // コミットがまだ無い等
    return b;
}

std::string GitIntegration::RemoteUrl(const std::string& workDir)
{
    auto r = RunGit(workDir, "remote get-url origin");
    if (!r.ok()) return "";
    std::string url = r.output;
    while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' '))
        url.pop_back();
    return url;
}

bool GitIntegration::IsMergeInProgress(const std::string& workDir)
{
    std::error_code ec;
    return fs::exists(fs::path(workDir) / ".git" / "MERGE_HEAD", ec);
}

std::vector<std::string> GitIntegration::ConflictedFiles(const std::string& workDir)
{
    std::vector<std::string> out;
    for (const auto& c : ChangedFiles(workDir))
        if (c.status == 'U') out.push_back(c.path);
    return out;
}

GitResult GitIntegration::ResolveOurs(const std::string& workDir, const std::string& path)
{
    // checkout --ours は削除系コンフリクト(DU/UD/DD)では対象が片側に無く失敗しうる。
    // その場合の判定はエラー出力に委ねる（呼び出し側でログ表示）。
    auto r = RunGit(workDir, "checkout --ours -- \"" + path + "\"");
    if (!r.ok()) return r;
    auto add = RunGit(workDir, "add -- \"" + path + "\"");
    add.output = r.output + add.output;
    return add;
}

GitResult GitIntegration::ResolveTheirs(const std::string& workDir, const std::string& path)
{
    auto r = RunGit(workDir, "checkout --theirs -- \"" + path + "\"");
    if (!r.ok()) return r;
    auto add = RunGit(workDir, "add -- \"" + path + "\"");
    add.output = r.output + add.output;
    return add;
}

void GitIntegration::OpenConflictFile(const std::string& workDir, const std::string& relPath)
{
    std::error_code ec;
    fs::path full = fs::absolute(fs::path(workDir) / relPath, ec);
    std::string fullStr = full.string();
    // VSCode があればそれで開く（コンフリクトマーカーの色分け/マージエディタが効く）。
    // ShellExecuteA の "open" は起動を待たない（フリーズ回避）。
    HINSTANCE h = ShellExecuteA(nullptr, "open", "code", ("\"" + fullStr + "\"").c_str(),
                                workDir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32)   // code が PATH に無い等で起動失敗 → 既定アプリで開く
        ShellExecuteA(nullptr, "open", fullStr.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

GitResult GitIntegration::CreateGitHubRepo(const std::string& workDir,
                                           const std::string& repoName,
                                           bool isPrivate)
{
    // gh repo create <name> --source=. --push --private/--public
    std::string vis = isPrivate ? "--private" : "--public";
    std::string args = "repo create \"" + repoName + "\" --source=. --remote=origin --push " + vis;
    return RunGh(workDir, args);
}

std::string GitIntegration::GitHubUser()
{
    // gh の認証ユーザー名を取得（未ログインなら失敗）
    auto r = RunGh("", "api user --jq .login");
    if (!r.ok()) return "";
    std::string u = r.output;
    // 前後の空白/改行を除去
    size_t b = u.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = u.find_last_not_of(" \t\r\n");
    return u.substr(b, e - b + 1);
}

GitResult GitIntegration::LoginAndWait(const std::atomic<bool>& abortFlag)
{
    // gh auth login --web は gh 自身がブラウザ承認をポーリングして待ち、終わったらプロセスが
    // 終了する。なので「子プロセスの終了をそのまま待つ」だけでイベント駆動の完了検知になる
    // （こちらで gh api user を何度も叩いてポーリングする必要が無い＝検知が速い）。
    // 別コンソールでワンタイムコード等の対話表示はそのまま見せる。
    std::string cmd = "gh auth login --hostname github.com --git-protocol https --web";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    GitResult result;
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (!ok)
    {
        result.exitCode = -1;
        result.output   = "ログイン画面の起動に失敗したで";
        return result;
    }

    // 500ms 刻みで「プロセス終了」か「abort 要求」だけ見る（gh api user の連打ポーリングより軽い）。
    DWORD code = 1;
    for (;;)
    {
        DWORD wr = WaitForSingleObject(pi.hProcess, 500);
        if (wr == WAIT_OBJECT_0) { GetExitCodeProcess(pi.hProcess, &code); break; }
        if (abortFlag.load()) break;   // シャットダウン中: 子は残しても無害（ユーザーは認証継続可）
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (code == 0)
    {
        RunGh("", "auth setup-git");   // git の資格情報ヘルパも gh に統一（失敗しても致命的ではない）
        result.exitCode = 0;
        result.output   = GitHubUser();
    }
    else
    {
        result.exitCode = 1;
    }
    return result;
}

std::string GitIntegration::RepoNameFromUrl(const std::string& url)
{
    std::string u = url;
    // 末尾の空白/改行/スラッシュを除去
    while (!u.empty() && (u.back() == '\n' || u.back() == '\r' || u.back() == ' ' || u.back() == '/'))
        u.pop_back();
    // ".git" を除去
    if (u.size() > 4 && u.compare(u.size() - 4, 4, ".git") == 0)
        u.erase(u.size() - 4);
    // 最後の '/' か ':' 以降を名前とする（https と scp 形式 git@host:owner/repo の両対応）
    size_t slash = u.find_last_of("/:");
    std::string name = (slash == std::string::npos) ? u : u.substr(slash + 1);
    return name;
}

std::string GitIntegration::ToWebUrl(const std::string& remoteUrl)
{
    std::string u = remoteUrl;
    size_t b = u.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = u.find_last_not_of(" \t\r\n");
    u = u.substr(b, e - b + 1);

    // プレフィックス文字数は手計算せず size() で取る（オフバイワンを構造的に防ぐ）
    static const std::string kPrefixes[] = {
        "git@github.com:", "https://github.com/", "http://github.com/", "ssh://git@github.com/"
    };
    std::string path;   // "owner/repo"
    for (const auto& prefix : kPrefixes)
    {
        if (u.compare(0, prefix.size(), prefix) == 0)
        {
            path = u.substr(prefix.size());
            break;
        }
    }
    if (path.empty()) return "";   // github.com 以外、またはパース不能

    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".git") == 0)
        path.erase(path.size() - 4);
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    if (path.empty()) return "";
    return "https://github.com/" + path;
}

GitResult GitIntegration::Clone(const std::string& url, const std::string& destParentDir,
                                std::string& outRepoDir)
{
    std::string repoName = RepoNameFromUrl(url);
    if (repoName.empty())
    {
        GitResult r; r.exitCode = -1; r.output = "URL からリポジトリ名を取得できませんでした"; return r;
    }
    fs::path target = fs::path(destParentDir) / repoName;
    outRepoDir = target.string();

    // git clone <url> "<target>"
    std::string args = "clone \"" + url + "\" \"" + target.string() + "\"";
    return RunGit(destParentDir, args);
}

std::vector<std::string> GitIntegration::ListBranches(const std::string& workDir)
{
    std::vector<std::string> branches;
    auto r = RunGit(workDir, "branch --format=%(refname:short)");
    if (!r.ok()) return branches;

    std::istringstream iss(r.output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t b = line.find_first_not_of(" \t*");  // 先頭の "* " や空白を除去
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t");
        std::string name = line.substr(b, e - b + 1);
        if (!name.empty()) branches.push_back(name);
    }
    return branches;
}

GitResult GitIntegration::CheckoutBranch(const std::string& workDir, const std::string& name)
{
    return RunGit(workDir, "checkout \"" + name + "\"");
}

GitResult GitIntegration::CreateBranch(const std::string& workDir, const std::string& name)
{
    return RunGit(workDir, "checkout -b \"" + name + "\"");
}

void GitIntegration::EnsureIdentity(const std::string& workDir)
{
    if (workDir.empty()) return;

    // 実効値(local→global→system)が空かどうかを見る。出力が空なら未設定。
    auto isSet = [&](const char* key) -> bool {
        auto r = RunGit(workDir, std::string("config ") + key);
        return r.output.find_first_not_of(" \t\r\n") != std::string::npos;
    };
    const bool haveName  = isSet("user.name");
    const bool haveEmail = isSet("user.email");
    if (haveName && haveEmail) return;   // 既に設定済み（global 等）→ 触らない

    // gh のログインユーザーから推定。取れなければ汎用の noreply に倒す。
    std::string login = GitHubUser();   // 例: "ryuto-alt"（未ログインなら空）
    std::string name  = login.empty() ? std::string("DX12 Engine User") : login;
    std::string email = login.empty()
        ? std::string("dx12-engine@users.noreply.github.com")
        : login + "@users.noreply.github.com";

    // global は汚さず、このリポジトリの local 設定だけ書く。
    if (!haveName)  RunGit(workDir, "config user.name \""  + name  + "\"");
    if (!haveEmail) RunGit(workDir, "config user.email \"" + email + "\"");
    Logger::Info("Git: set local identity for {} ({} <{}>)", workDir, name, email);
}

void GitIntegration::WriteGitignore(const std::string& workDir)
{
    fs::path path = fs::path(workDir) / ".gitignore";
    std::error_code ec;
    if (fs::exists(path, ec)) return;

    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs <<
        "# DX12 Engine project\n"
        "build/\n"
        "out/\n"
        "*.user\n"
        "imgui.ini\n"
        ".vs/\n"
        "*.tmp\n";
    Logger::Info("Wrote .gitignore to {}", workDir);
}

} // namespace dx12e
