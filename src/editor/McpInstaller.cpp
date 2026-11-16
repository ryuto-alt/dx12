#include "editor/McpInstaller.h"
#include "editor/McpClientConfig.h"

#include "core/AtomicFile.h"
#include "core/Http.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/SafeRemove.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#pragma comment(lib, "bcrypt.lib")

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>

namespace dx12e::mcpinst
{

namespace fs = std::filesystem;

namespace
{

// Node の取得元。latest-v24.x は v24 系の最新（MCP サーバは .ts を型ストリップで直接実行するので v24+ が要る）
constexpr int kNodeMajorRequired = 24;
constexpr const wchar_t* kNodeDistBase = L"https://nodejs.org/dist/latest-v24.x/";
// MCP サーバの配布リポジトリ（git が無くても落とせる zip）
constexpr const wchar_t* kServerZipUrl = L"https://github.com/ryuto-alt/dx12-mcp/archive/refs/heads/main.zip";

std::wstring W(const std::string& s) { return PathResolver::Utf8ToWide(s); }
std::string  U(const std::wstring& s) { return PathResolver::WideToUtf8(s); }
fs::path     P(const std::string& s) { return atomicfile::PathFromUtf8(s); }
std::string  S(const fs::path& p) { return atomicfile::Utf8FromPath(p); }

bool IsFile(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
bool IsDir(const fs::path& p)  { std::error_code ec; return fs::is_directory(p, ec); }

std::string EnvVar(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return U(buf);
}

std::string KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = U(p);
    if (p) ::CoTaskMemFree(p);
    return out;
}

std::string Home()
{
    std::string h = KnownFolder(FOLDERID_Profile);
    if (h.empty()) h = EnvVar(L"USERPROFILE");
    return h;
}

// 不正な UTF-8（CP932 で出てくる tar のメッセージなど）を '?' に置き換える（ImGui に渡すので）
std::string Utf8Lossy(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c < 0xE0) len = 2;
        else if (c >= 0xE0 && c < 0xF0) len = 3;
        else if (c >= 0xF0 && c < 0xF5) len = 4;
        bool ok = len > 0 && i + len <= s.size();
        for (size_t k = 1; ok && k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
        if (!ok) { o += '?'; ++i; continue; }
        o.append(s, i, len);
        i += len;
    }
    return o;
}

std::string Sha256Hex(const std::vector<unsigned char>& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32] = {};
    bool ok = BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    ok = ok && BCRYPT_SUCCESS(::BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
    ok = ok && BCRYPT_SUCCESS(::BCryptHashData(h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0));
    ok = ok && BCRYPT_SUCCESS(::BCryptFinishHash(h, digest, static_cast<ULONG>(sizeof(digest)), 0));
    if (h) ::BCryptDestroyHash(h);
    if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) { out += hex[b >> 4]; out += hex[b & 0xF]; }
    return out;
}

std::string Lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// 子プロセスに渡す環境ブロック（PATH の先頭に prefix を足す）。prefix が空なら空（＝親の環境をそのまま継承）
std::wstring EnvBlockWithPathPrefix(const std::wstring& prefix)
{
    std::wstring block;
    if (prefix.empty()) return block;
    LPWCH env = ::GetEnvironmentStringsW();
    bool hadPath = false;
    if (env)
    {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1)
        {
            std::wstring entry(p);
            if (entry.size() >= 5 && _wcsnicmp(entry.c_str(), L"PATH=", 5) == 0)
            {
                entry = L"PATH=" + prefix + L";" + entry.substr(5);
                hadPath = true;
            }
            block += entry;
            block.push_back(L'\0');
        }
        ::FreeEnvironmentStringsW(env);
    }
    if (!hadPath) { block += L"PATH=" + prefix; block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

std::string Timestamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

bool ReadAll(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// dir 直下で index.ts を持つ最初のフォルダ（GitHub の zip は dx12-mcp-main/ の 1 段下に入っている）
fs::path FindServerRootIn(const fs::path& dir)
{
    if (IsFile(dir / "index.ts")) return dir;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && IsFile(e.path() / "index.ts")) return e.path();
    return {};
}

// フォルダを消す（必ず ManagedRoot の中だけ。saferm が外を断る）
void RemoveManaged(const fs::path& p)
{
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    saferm::RemoveAll(p, ec, P(ManagedRoot()));
}

} // namespace

// ---------------------------------------------------------------------------
// 場所
// ---------------------------------------------------------------------------

std::string ManagedRoot()
{
    const std::string la = KnownFolder(FOLDERID_LocalAppData);
    return la.empty() ? std::string() : S(P(la) / L"UnoEngine");
}

std::string ManagedServerDir()
{
    const std::string r = ManagedRoot();
    return r.empty() ? std::string() : S(P(r) / L"mcp-server");
}

std::string ResolveServerDir(const std::string& fallbackDir)
{
    // 1) 手で clone した標準の場所（従来の install.ps1 の案内先）
    const std::string home = Home();
    if (!home.empty() && IsFile(P(home) / L"dx12-mcp" / L"index.ts"))
        return S((P(home) / L"dx12-mcp").generic_wstring());

    // 2) エンジンが入れた場所（このファイルの McpInstaller）
    const std::string managed = ManagedServerDir();
    if (!managed.empty() && IsFile(P(managed) / L"index.ts"))
        return S(P(managed).generic_wstring());

    // 3) ソースからのビルド: exe から上へ最大 8 階層辿って tools/mcp-server/index.ts
    //    （GetModuleFileNameW はバッファ不足で MAX_PATH ちょうどを返し NUL 終端しないので、それは不採用）
    wchar_t buf[MAX_PATH] = {};
    const DWORD exeLen = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH)
    {
        fs::path dir = fs::path(buf).parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (IsFile(dir / L"tools" / L"mcp-server" / L"index.ts"))
                return S((dir / L"tools" / L"mcp-server").generic_wstring());
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }

    // 4) ビルド時マクロ（このマシンでビルドし、repo を動かしていない場合のみ有効）
    if (!fallbackDir.empty() && IsFile(P(fallbackDir) / L"index.ts"))
        return S(P(fallbackDir).generic_wstring());
    return {};
}

// ---------------------------------------------------------------------------
// McpInstaller
// ---------------------------------------------------------------------------

const char* McpInstaller::StepLabel(Step s)
{
    switch (s)
    {
    case Step::Node:     return "Node.js を準備しています";
    case Step::Server:   return "MCP サーバを準備しています";
    case Step::Npm:      return "依存パッケージを入れています (npm ci)";
    case Step::Register: return "AI ツールへ登録しています";
    case Step::Done:     return "完了";
    case Step::Failed:   return "失敗";
    default:             return "待機中";
    }
}

void McpInstaller::SetStep(Step s)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_step = s;
    m_progress = 0.0f;
}

void McpInstaller::SetProgress(float p)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_progress = p;
}

void McpInstaller::Log(const std::string& line)
{
    const std::string clean = Utf8Lossy(line.size() > 400 ? line.substr(0, 400) : line);
    Logger::Info("[mcp-install] {}", clean);
    std::lock_guard<std::mutex> lk(m_mu);
    m_log.push_back(clean);
    if (m_log.size() > 300) m_log.pop_front();
}

void McpInstaller::LogText(const char* data, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        const char c = data[i];
        if (c == '\n' || c == '\r')
        {
            if (!m_partial.empty()) { Log("  " + m_partial); m_partial.clear(); }
        }
        else m_partial += c;
    }
}

int McpInstaller::RunProcess(const std::string& cmdLine, const std::string& workDir, const std::string& pathPrefix,
                             std::string* capture)
{
    if (!capture) Log("> " + cmdLine);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) { Log("パイプを作れませんでした"); return -1; }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = W(cmdLine);
    const std::wstring wd = W(workDir);
    std::wstring envBlock = EnvBlockWithPathPrefix(W(pathPrefix));
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                     envBlock.empty() ? nullptr : envBlock.data(), wd.empty() ? nullptr : wd.c_str(), &si, &pi);
    ::CloseHandle(wr);
    if (!ok)
    {
        if (!capture) Log("起動できませんでした（Win32 エラー " + std::to_string(::GetLastError()) + "）");
        ::CloseHandle(rd);
        return -1;
    }
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        ::AssignProcessToJobObject(job, pi.hProcess);
    }
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_process = pi.hProcess;
        m_job = job;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    char buf[4096];
    DWORD got = 0;
    if (capture)
    {
        while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) capture->append(buf, got);
    }
    else
    {
        while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) LogText(buf, got);
        LogText("\n", 1);
    }
    ::CloseHandle(rd);

    DWORD ec = 1;
    ::WaitForSingleObject(pi.hProcess, 10000);
    ::GetExitCodeProcess(pi.hProcess, &ec);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_process = nullptr;
        m_job = nullptr;
    }
    if (job) ::CloseHandle(job);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(ec);
}

bool McpInstaller::ExtractZip(const std::string& zip, const std::string& dest)
{
    // tar.exe（Windows 10 1803 以降に標準。zip も展開できる）→ だめなら PowerShell の Expand-Archive
    std::error_code ec;
    fs::create_directories(P(dest), ec);
    if (RunProcess("tar.exe -xf \"" + zip + "\" -C \"" + dest + "\"", dest, {}) == 0) return true;
    if (m_cancel.load()) return false;
    auto q = [](const std::string& s) {   // PowerShell の単引用符内でのエスケープ
        std::string o;
        for (char c : s) { o += c; if (c == '\'') o += '\''; }
        return o;
    };
    const std::string ps = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                           q(zip) + "' -DestinationPath '" + q(dest) + "' -Force\"";
    return RunProcess(ps, dest, {}) == 0;
}

bool McpInstaller::Download(const std::wstring& url, std::vector<unsigned char>& out, const char* what)
{
    Log(std::string(what) + " をダウンロードしています: " + U(url));
    const std::function<void(uint64_t, uint64_t)> prog = [this](uint64_t done, uint64_t total) {
        SetProgress(total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f);
    };
    std::vector<uint8_t> bytes;
    if (!http::Get(url, bytes, &prog, &m_cancel)) return false;
    out.assign(bytes.begin(), bytes.end());
    SetProgress(1.0f);
    return true;
}

int McpInstaller::NodeMajor(const std::string& nodeExe)
{
    std::string out;
    if (RunProcess("\"" + nodeExe + "\" --version", {}, {}, &out) != 0) return 0;
    return mcpcfg::NodeMajorFromVersion(out);
}

std::string McpInstaller::EnsureNode(std::string& err)
{
    const fs::path root = P(ManagedRoot());
    const fs::path managedNode = root / L"node" / L"node.exe";

    // 1) エンジンが前に入れた Node
    if (IsFile(managedNode) && NodeMajor(S(managedNode)) >= kNodeMajorRequired)
    {
        Log("Node.js: " + S(managedNode) + "（エンジンが入れたもの）");
        return S(managedNode);
    }
    // 2) PATH 上の Node（v24 以上なら使う）
    {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = ::SearchPathW(nullptr, L"node.exe", nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr);
        if (n > 0 && n < std::size(buf))
        {
            const std::string sys = U(buf);
            const int major = NodeMajor(sys);
            if (major >= kNodeMajorRequired)
            {
                Log("Node.js: " + sys + "（v" + std::to_string(major) + "・インストール済みのもの）");
                return sys;
            }
            Log("PATH の Node.js は v" + std::to_string(major) + " で古いので、v24 を別に入れます（今の Node はそのまま）");
        }
    }
    if (m_cancel.load()) { err = "中断されました"; return {}; }

    // 3) nodejs.org から公式 zip を落とす（SHA-256 を照合）
    //    エンジン自体が x64 ビルドなので Node も x64（ARM64 の Windows でもエミュレーションで動く）
    const std::string suffix = "-win-x64.zip";

    std::vector<unsigned char> sumsBytes;
    if (!Download(std::wstring(kNodeDistBase) + L"SHASUMS256.txt", sumsBytes, "Node.js の一覧"))
    {
        err = m_cancel.load() ? "中断されました" : "nodejs.org に繋がりませんでした（ネットワークを確認してください）";
        return {};
    }
    std::string zipName, expected;
    if (!mcpcfg::FindNodeZipInShasums(std::string(sumsBytes.begin(), sumsBytes.end()), suffix, zipName, expected))
    {
        err = "nodejs.org の一覧に Windows 用の zip が見つかりませんでした";
        return {};
    }
    std::vector<unsigned char> zipBytes;
    if (!Download(std::wstring(kNodeDistBase) + W(zipName), zipBytes, zipName.c_str()))
    {
        err = m_cancel.load() ? "中断されました" : "Node.js をダウンロードできませんでした";
        return {};
    }
    const std::string actual = Sha256Hex(zipBytes);
    if (actual.empty() || Lower(actual) != Lower(expected))
    {
        err = "Node.js の zip のハッシュが一致しません（改ざんか通信の破損の可能性。もう一度試してください）";
        return {};
    }
    Log("SHA-256 一致: " + actual);

    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path zipPath = root / L"node-download.zip";
    const fs::path tmp = root / L"node.tmp";
    {
        std::ofstream f(zipPath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(zipBytes.data()), static_cast<std::streamsize>(zipBytes.size()));
        if (!f) { err = "Node.js の zip を保存できませんでした: " + S(zipPath); return {}; }
    }
    RemoveManaged(tmp);
    const bool extracted = ExtractZip(S(zipPath), S(tmp));
    fs::remove(zipPath, ec);
    fs::path inner;
    if (extracted)
        for (const auto& e : fs::directory_iterator(tmp, ec))
            if (e.is_directory(ec) && IsFile(e.path() / L"node.exe")) { inner = e.path(); break; }
    if (inner.empty())
    {
        RemoveManaged(tmp);
        err = m_cancel.load() ? "中断されました" : "Node.js の zip を展開できませんでした";
        return {};
    }
    RemoveManaged(root / L"node");   // 古くて使えない Node が残っていれば入れ替える
    fs::rename(inner, root / L"node", ec);
    RemoveManaged(tmp);
    if (ec || !IsFile(managedNode))
    {
        err = "Node.js を配置できませんでした（" + Utf8Lossy(ec.message()) + "）";
        return {};
    }
    const int major = NodeMajor(S(managedNode));
    if (major < kNodeMajorRequired)
    {
        err = "入れた Node.js が起動しませんでした";
        return {};
    }
    Log("Node.js v" + std::to_string(major) + " を入れました: " + S(managedNode));
    return S(managedNode);
}

std::string McpInstaller::EnsureServer(const std::string& fallbackDir, std::string& err)
{
    const std::string found = ResolveServerDir(fallbackDir);
    const fs::path managed = P(ManagedServerDir());
    if (!found.empty() && P(found) != managed && P(found).lexically_normal() != managed.lexically_normal())
    {
        Log("MCP サーバ: " + found + "（既にあるものを使います）");
        return S(P(found).make_preferred());
    }

    // 配布リポジトリの main を zip で取る。押し直すと最新版に入れ替わる。
    std::vector<unsigned char> zipBytes;
    if (!Download(kServerZipUrl, zipBytes, "MCP サーバ (github.com/ryuto-alt/dx12-mcp)"))
    {
        err = m_cancel.load() ? "中断されました" : "MCP サーバをダウンロードできませんでした（github.com に繋がるか確認してください）";
        return {};
    }
    const fs::path root = P(ManagedRoot());
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path zipPath = root / L"mcp-server-download.zip";
    const fs::path tmp = root / L"mcp-server.tmp";
    const fs::path old = root / L"mcp-server.old";
    {
        std::ofstream f(zipPath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(zipBytes.data()), static_cast<std::streamsize>(zipBytes.size()));
        if (!f) { err = "MCP サーバの zip を保存できませんでした: " + S(zipPath); return {}; }
    }
    RemoveManaged(tmp);
    const bool extracted = ExtractZip(S(zipPath), S(tmp));
    fs::remove(zipPath, ec);
    const fs::path inner = extracted ? FindServerRootIn(tmp) : fs::path();
    if (inner.empty())
    {
        RemoveManaged(tmp);
        err = m_cancel.load() ? "中断されました" : "MCP サーバの zip を展開できませんでした";
        return {};
    }

    // 入れ替え。前の版の実行時データ（.dx12/）は引き継ぐ。
    if (IsDir(managed))
    {
        if (IsDir(managed / L".dx12") && !IsDir(inner / L".dx12"))
            fs::rename(managed / L".dx12", inner / L".dx12", ec);
        RemoveManaged(old);
        ec.clear();
        fs::rename(managed, old, ec);
        if (ec)
        {
            RemoveManaged(tmp);
            err = "今の MCP サーバを入れ替えられませんでした（Codex / Claude などが使用中の可能性。"
                  "AI ツールを閉じてからもう一度押してください）";
            return {};
        }
    }
    ec.clear();
    fs::rename(inner, managed, ec);
    if (ec)
    {
        std::error_code ec2;
        if (IsDir(old)) fs::rename(old, managed, ec2);   // 元へ戻す
        RemoveManaged(tmp);
        err = "MCP サーバを配置できませんでした（" + Utf8Lossy(ec.message()) + "）";
        return {};
    }
    RemoveManaged(tmp);
    RemoveManaged(old);
    Log("MCP サーバを入れました: " + S(managed));
    return S(managed);
}

bool McpInstaller::InstallDeps(const std::string& node, const std::string& serverDir, std::string& err)
{
    const fs::path nodeDir = P(node).parent_path();
    const fs::path npmCli = nodeDir / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
    // npm は Node に付属のものを node で直接起こす（npm.cmd の PATH 依存を避ける）。PATH の先頭にも Node を足す
    // （依存のインストールスクリプトが "node" を呼ぶため）。
    const std::string npm = IsFile(npmCli) ? "\"" + node + "\" \"" + S(npmCli) + "\"" : std::string("cmd.exe /d /c npm");
    const std::string flags = " --no-audit --no-fund --loglevel=error";
    const bool hasLock = IsFile(P(serverDir) / L"package-lock.json");
    if (hasLock && RunProcess(npm + " ci" + flags, serverDir, S(nodeDir)) == 0) return true;
    if (m_cancel.load()) { err = "中断されました"; return false; }
    if (hasLock) Log("npm ci が失敗したので npm install でやり直します");
    if (RunProcess(npm + " install" + flags, serverDir, S(nodeDir)) == 0) return true;
    err = m_cancel.load() ? "中断されました" : "依存パッケージを入れられませんでした（ログを確認してください。registry.npmjs.org への接続が必要です）";
    return false;
}

std::vector<ClientResult> McpInstaller::RegisterClients(const std::string& node, const std::string& serverDir)
{
    std::vector<ClientResult> results;
    const std::string home = Home();
    const std::string appData = KnownFolder(FOLDERID_RoamingAppData);
    const std::string index = S(P(serverDir) / L"index.ts");

    mcpcfg::ServerSpec full;    // ツール検索がある Claude Code は全ツール
    full.command = node;
    full.args = {index};
    mcpcfg::ServerSpec core = full;   // ツール検索が無い / 弱いクライアントは core 面（README の推奨）
    core.env = {{"DX12_MCP_SURFACE", "core"}};

    // 書き換え（中身が変わるときだけ。元のファイルは退避してから）
    auto apply = [&](const std::string& client, const fs::path& file, bool mustExistDir, const fs::path& marker,
                     const std::function<bool(const std::string&, std::string&, std::string&)>& transform) {
        ClientResult r;
        r.client = client;
        r.path = S(file);
        if (mustExistDir && !IsDir(marker) && !IsFile(marker))
        {
            r.status = ClientResult::Status::NotInstalled;
            results.push_back(r);
            return;
        }
        std::string before;
        const bool existed = IsFile(file) && ReadAll(file, before);
        std::string after, err;
        if (!transform(before, after, err))
        {
            r.status = ClientResult::Status::Failed;
            r.note = err;
            results.push_back(r);
            return;
        }
        if (existed && after == before)
        {
            r.status = ClientResult::Status::Unchanged;
            results.push_back(r);
            return;
        }
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (existed)
        {
            fs::path bak = file;
            bak += L"." + W("unoengine-" + Timestamp()) + L".bak";
            fs::copy_file(file, bak, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                r.status = ClientResult::Status::Failed;
                r.note = "退避できなかったので書き換えませんでした（" + Utf8Lossy(ec.message()) + "）";
                results.push_back(r);
                return;
            }
            r.note = "元のファイルを退避: " + S(bak);
        }
        const atomicfile::Result w = atomicfile::WriteFile(file, after);
        r.status = w.ok ? ClientResult::Status::Written : ClientResult::Status::Failed;
        if (!w.ok) r.note = w.error;
        results.push_back(r);
    };

    auto toml = [&](const mcpcfg::ServerSpec& spec) {
        return [spec](const std::string& in, std::string& out, std::string&) { out = mcpcfg::UpsertCodexToml(in, spec); return true; };
    };
    auto json = [&](const mcpcfg::ServerSpec& spec, bool withType) {
        return [spec, withType](const std::string& in, std::string& out, std::string& err) {
            return mcpcfg::UpsertJsonText(in, spec, withType, out, &err);
        };
    };

    // Codex: CODEX_HOME があればそこ。Codex をまだ入れていなくても書いておく（後から入れればそのまま繋がる）
    {
        mcpcfg::ServerSpec codex = core;
        codex.startupTimeoutSec = 30;   // 初回は .ts の読み込みで 10 秒（既定）を越えることがある
        const std::string codexHome = EnvVar(L"CODEX_HOME");
        const fs::path dir = !codexHome.empty() ? P(codexHome) : P(home) / L".codex";
        apply("Codex", dir / L"config.toml", false, {}, toml(codex));
    }
    // Claude Code: ~/.claude.json（CLAUDE_CONFIG_DIR があればその中）の user スコープ。入っている場合だけ
    {
        const std::string cfgDir = EnvVar(L"CLAUDE_CONFIG_DIR");
        const fs::path file = (!cfgDir.empty() ? P(cfgDir) : P(home)) / L".claude.json";
        apply("Claude Code", file, true, file, json(full, true));
    }
    // Claude Desktop: %APPDATA%\Claude\claude_desktop_config.json。アプリのフォルダがある場合だけ
    if (!appData.empty())
    {
        const fs::path dir = P(appData) / L"Claude";
        apply("Claude Desktop", dir / L"claude_desktop_config.json", true, dir, json(core, false));
    }
    // Cursor: ~/.cursor/mcp.json。Cursor のフォルダがある場合だけ
    {
        const fs::path dir = P(home) / L".cursor";
        apply("Cursor", dir / L"mcp.json", true, dir, json(core, false));
    }

    // 評価役サブエージェント（agents/*.md）を ~/.claude/agents/ へ（install.ps1 と同じ。Claude Code がある場合だけ）
    const fs::path agentsSrc = P(serverDir) / L"agents";
    const fs::path claudeDir = P(home) / L".claude";
    if (IsDir(agentsSrc) && IsDir(claudeDir))
    {
        std::error_code ec;
        fs::create_directories(claudeDir / L"agents", ec);
        int copied = 0;
        for (const auto& e : fs::directory_iterator(agentsSrc, ec))
        {
            if (!e.is_regular_file(ec) || e.path().extension() != L".md") continue;
            std::error_code cec;
            fs::copy_file(e.path(), claudeDir / L"agents" / e.path().filename(), fs::copy_options::overwrite_existing, cec);
            if (!cec) ++copied;
        }
        if (copied > 0) Log("サブエージェント " + std::to_string(copied) + " 本を " + S(claudeDir / L"agents") + " へ置きました");
    }

    for (const ClientResult& r : results)
    {
        switch (r.status)
        {
        case ClientResult::Status::Written:      Log("登録: " + r.client + " → " + r.path); break;
        case ClientResult::Status::Unchanged:    Log("登録済み（変更なし）: " + r.client + " → " + r.path); break;
        case ClientResult::Status::NotInstalled: Log("見つからないので飛ばしました: " + r.client); break;
        case ClientResult::Status::Failed:       Log("登録できませんでした: " + r.client + "（" + r.note + "）"); break;
        }
    }
    return results;
}

bool McpInstaller::Start(const std::string& fallbackDir)
{
    if (m_running.load()) return false;
    if (m_worker.joinable()) m_worker.join();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_step = Step::Node;
        m_progress = 0.0f;
        m_error.clear();
        m_log.clear();
        m_partial.clear();
        m_clients.clear();
        m_nodePath.clear();
        m_serverDir.clear();
    }
    m_cancel.store(false);
    m_running.store(true);
    m_generation.fetch_add(1);
    m_worker = std::thread([this, fallbackDir]() { Run(fallbackDir); });
    return true;
}

void McpInstaller::Shutdown()
{
    m_cancel.store(true);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_job) ::TerminateJobObject(static_cast<HANDLE>(m_job), 1);
        else if (m_process) ::TerminateProcess(static_cast<HANDLE>(m_process), 1);
    }
    if (m_worker.joinable()) m_worker.join();
    m_cancel.store(false);
}

void McpInstaller::Wait()
{
    if (m_worker.joinable()) m_worker.join();
}

McpInstaller::Snapshot McpInstaller::Get() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    Snapshot s;
    s.step = m_step;
    s.progress = m_progress;
    s.running = m_running.load();
    s.error = m_error;
    s.nodePath = m_nodePath;
    s.serverDir = m_serverDir;
    s.log.assign(m_log.begin(), m_log.end());
    s.clients = m_clients;
    return s;
}

void McpInstaller::Run(std::string fallbackDir)
{
    // ★背景スレッドの最上位。ここから例外が出ると std::terminate でエンジンごと落ちるので全部受けて Failed にする
    try
    {
        RunImpl(fallbackDir);
    }
    catch (const std::exception& e)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = "内部エラー: " + Utf8Lossy(e.what());
        m_step = Step::Failed;
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = "内部エラー: 不明な例外";
        m_step = Step::Failed;
    }
    m_running.store(false);
}

void McpInstaller::RunImpl(const std::string& fallbackDir)
{
    auto fail = [this](const std::string& msg) {
        Log("失敗: " + msg);
        Logger::Warn("[mcp-install] 失敗: {}", Utf8Lossy(msg));
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = msg;
        m_step = Step::Failed;
    };
    if (ManagedRoot().empty()) { fail("保存先（LocalAppData）を取得できませんでした"); return; }
    std::string err;

    SetStep(Step::Node);
    const std::string node = EnsureNode(err);
    if (node.empty()) { fail(err); return; }
    { std::lock_guard<std::mutex> lk(m_mu); m_nodePath = node; }

    SetStep(Step::Server);
    const std::string server = EnsureServer(fallbackDir, err);
    if (server.empty()) { fail(err); return; }
    { std::lock_guard<std::mutex> lk(m_mu); m_serverDir = server; }

    SetStep(Step::Npm);
    if (!InstallDeps(node, server, err)) { fail(err); return; }

    SetStep(Step::Register);
    std::vector<ClientResult> clients = RegisterClients(node, server);
    bool anyOk = false;
    for (const ClientResult& r : clients)
        if (r.status == ClientResult::Status::Written || r.status == ClientResult::Status::Unchanged) anyOk = true;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_clients = std::move(clients);
    }
    if (!anyOk) { fail("どの AI ツールにも登録できませんでした"); return; }

    Log("完了。Codex / Claude などの AI ツールを再起動すると dx12_* ツールが使えます");
    SetStep(Step::Done);
}

} // namespace dx12e::mcpinst
