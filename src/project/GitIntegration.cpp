#include "project/GitIntegration.h"
#include "core/Logger.h"

#include <Windows.h>
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

    // コマンドライン構築: "exe args"（exe は PATH 解決のためそのまま渡す）
    std::string cmdLine = exe + " " + args;
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError  = writePipe;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),
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

bool GitIntegration::LaunchLogin()
{
    // gh auth login は対話式（ブラウザでのデバイス認証）。別コンソールを開いて
    // ユーザーが操作できるようにする。完了後に git の資格情報ヘルパも gh に設定。
    // /k で完了後もウィンドウを残す（ワンタイムコード等の確認用）。
    std::string cmd =
        "cmd.exe /k \"gh auth login --hostname github.com --git-protocol https --web "
        "&& gh auth setup-git && echo. && echo [ログイン処理が完了したらこのウィンドウは閉じてOK]\"";

    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (ok)
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok == TRUE;
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
