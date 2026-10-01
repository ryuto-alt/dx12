#include "core/Updater.h"
#include "core/Version.h"
#include "core/Logger.h"
#include "core/UpdateLogic.h"
#include "core/UpdateWindow.h"
#include "core/ReleaseNotes.h"
#include "core/PathResolver.h"
#include "core/SplashScreen.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <algorithm>

#include <nlohmann/json.hpp>

// MSVC のみ運用（VS2022）。WinHTTP を自動リンク（CMake 変更不要）。進捗の窓は core/UpdateWindow（Direct2D）。
#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace dx12e
{
namespace
{
std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// ASCII 前提の wstring → string（タグ名や URL 等、非ASCII を含まないもの専用）。
std::string NarrowAscii(const std::wstring& s)
{
    std::string out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(static_cast<char>(c));
    return out;
}

// "key":"value" の value を取り出す簡易抽出（値にエスケープ無し前提＝tag/URL では十分）。
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0)
{
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return {};
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// 最初の .zip アセットの browser_download_url を返す。
std::string FirstZipAssetUrl(const std::string& json)
{
    size_t from = 0;
    const std::string key = "\"browser_download_url\"";
    for (;;)
    {
        size_t k = json.find(key, from);
        if (k == std::string::npos) return {};
        std::string url = JsonString(json, "browser_download_url", k);
        from = k + key.size();
        if (url.size() >= 4 && url.compare(url.size() - 4, 4, ".zip") == 0)
            return url;
    }
}

// api.github.com は未認証だと 60 req/hour/IP の共有レート制限があり、同一ネットワーク上の
// 他のツール（gh CLI・git・ブラウザ等）だけですぐ枯渇する。枯渇すると 403 が返り、このアプリの
// 更新チェックは（オフライン時と区別できず）黙って諦めていた＝「自動更新が起動しなくなる」の主因。
// github.com への通常の Web リクエストはこのレート制限を受けないため、まずこちらを優先して使う。

// releases/latest の 302 リダイレクト先パス（.../releases/tag/vX.Y.Z）からタグ名だけを読む。
// api.github.com を一切使わない。失敗時は空文字列を返す。
std::string FetchLatestTagViaRedirect(const std::string& owner, const std::string& repo)
{
    std::wstring path = L"/" + Widen(owner) + L"/" + Widen(repo) + L"/releases/latest";

    HINTERNET hSession = WinHttpOpen(L"DX12Engine-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    WinHttpSetTimeouts(hSession, 4000, 4000, 15000, 15000);

    std::string tag;
    HINTERNET hConnect = WinHttpConnect(hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect)
    {
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hReq)
        {
            // 自動リダイレクト追従を止めて、Location ヘッダーからタグだけを読む（本文は取得しない）。
            DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(hReq, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));

            BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

            if (ok)
            {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

                if (status >= 300 && status < 400)
                {
                    wchar_t loc[2048] = {};
                    DWORD locSz = sizeof(loc);
                    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX, loc, &locSz, WINHTTP_NO_HEADER_INDEX))
                    {
                        std::wstring locW(loc);
                        size_t slash = locW.find_last_of(L'/');
                        if (slash != std::wstring::npos && slash + 1 < locW.size())
                        {
                            std::wstring tagW = locW.substr(slash + 1);
                            tag = NarrowAscii(tagW);  // タグは ASCII 前提（vX.Y.Z）
                        }
                    }
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return tag;
}

// URL が実在するか（最終的に 200 か）を HEAD で確認する。本体は取得しない。
// リダイレクト追従はデフォルト（有効）のまま＝ github.com → 署名付き Blob URL まで辿る。
bool UrlExists(const std::wstring& url)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    uc.lpszHostName = host;  uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"DX12Engine-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 4000, 4000, 15000, 15000);

    bool exists = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (hConnect)
    {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"HEAD", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hReq)
        {
            BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
            if (ok)
            {
                DWORD status = 0, sz = sizeof(status);
                if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX))
                    exists = (status == 200);
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return exists;
}

// "vX.Y.Z" / "X.Y.Z" → 数値 3 要素（足りない分は 0）。
void ParseSemver(const std::string& s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;  // 先頭の 'v' 等を飛ばす
    int idx = 0;
    while (i < s.size() && idx < 3)
    {
        int v = 0; bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; any = true; }
        if (any) out[idx++] = v;
        while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;
    }
}

bool IsNewer(const std::string& latest, const std::string& current)
{
    int a[3], b[3];
    ParseSemver(latest, a);
    ParseSemver(current, b);
    for (int i = 0; i < 3; ++i)
    {
        if (a[i] > b[i]) return true;
        if (a[i] < b[i]) return false;
    }
    return false;
}

// HTTPS GET（リダイレクト追従）。outFile 指定時はファイルへ、未指定時は outBytes へ。status 200 のみ成功。
// progress 指定時は受信バイト数/総バイト数を逐次コールバック（ダウンロードメーター用）。
bool HttpsFetch(const std::wstring& url, std::vector<char>* outBytes, const std::wstring* outFile,
                const std::function<void(uint64_t, uint64_t)>* progress = nullptr)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    uc.lpszHostName = host;  uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"DX12Engine-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    // 起動時の同期チェックなので名前解決/接続のタイムアウトは短めにして、
    // ネットワーク不調やプロキシ自動検出で起動が長く固まらないようにする。
    WinHttpSetTimeouts(hSession, 4000, 4000, 15000, 30000);

    bool good = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (hConnect)
    {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hReq)
        {
            const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
            BOOL ok = WinHttpSendRequest(hReq, hdr, (DWORD)-1L,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

            DWORD status = 0, sz = sizeof(status);
            if (ok)
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

            if (ok && status == 200)
            {
                std::ofstream fout;
                bool fileOk = true;
                if (outFile)
                {
                    fout.open(*outFile, std::ios::binary | std::ios::trunc);
                    fileOk = fout.is_open();
                }
                if (fileOk)
                {
                    // 総バイト数（Content-Length）。チャンク転送等で不明なら 0。
                    uint64_t total = 0;
                    {
                        DWORD cl = 0, clSz = sizeof(cl);
                        if (WinHttpQueryHeaders(hReq,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &cl, &clSz, WINHTTP_NO_HEADER_INDEX))
                            total = cl;
                    }
                    uint64_t done = 0;
                    if (progress && *progress) (*progress)(0, total);

                    good = true;
                    for (;;)
                    {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(hReq, &avail)) { good = false; break; }
                        if (avail == 0) break;
                        std::vector<char> buf(avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(hReq, buf.data(), avail, &read)) { good = false; break; }
                        if (read == 0) break;
                        if (outFile) fout.write(buf.data(), (std::streamsize)read);
                        else         outBytes->insert(outBytes->end(), buf.data(), buf.data() + read);
                        done += read;
                        if (progress && *progress) (*progress)(done, total);
                    }
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return good;
}

// 隠しコンソールでコマンドを実行し、終了まで「UI のメッセージをポンプしながら」待つ。
// WaitForSingleObject(INFINITE) で待つと展開中に進捗窓が固まって「(応答なし)」になるため、
// MsgWaitForMultipleObjects + Pump で待受窓を生かしたまま待つ。code に終了コードを返す。
bool RunHiddenPumped(const std::wstring& cmdLine, const std::function<void()>& pump, DWORD& code)
{
    code = 1;
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    for (;;)
    {
        // 33ms ごとに起きて進捗窓のメッセージを処理・描き直す（回る弧の動きと応答性を維持）。
        DWORD w = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 33, QS_ALLINPUT);
        if (pump) pump();
        if (w == WAIT_OBJECT_0) break;   // 子プロセス終了
    }

    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// zip を展開する。tar.exe(bsdtar, Windows 10 1803+ 標準) を最優先＝大量の小ファイル
// (assets 数千個)でも数秒で済む。Expand-Archive は同条件で分単位かつ
// UI を固めるため、tar が無い/失敗した時だけのフォールバックに降格。
bool ExtractZip(const fs::path& zip, const fs::path& dest, const std::function<void()>& pump = nullptr)
{
    // 1) tar.exe（フルパス指定。PATH 上の別 tar=MSYS 等を避ける）
    wchar_t sysDir[MAX_PATH] = {};
    if (GetSystemDirectoryW(sysDir, MAX_PATH) > 0)
    {
        fs::path tarExe = fs::path(sysDir) / L"tar.exe";
        std::error_code ec;
        if (fs::exists(tarExe, ec))
        {
            std::wstring cmd = L"\"" + tarExe.wstring() + L"\" -xf \"" + zip.wstring() +
                               L"\" -C \"" + dest.wstring() + L"\"";
            DWORD code = 1;
            if (RunHiddenPumped(cmd, pump, code) && code == 0)
                return true;
        }
    }

    // 2) フォールバック: PowerShell Expand-Archive（低速だが tar 不在環境向け）
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"Expand-Archive -Force -LiteralPath '" + zip.wstring() +
        L"' -DestinationPath '" + dest.wstring() + L"'\"";
    DWORD code = 1;
    return RunHiddenPumped(cmd, pump, code) && code == 0;
}

// 展開先から DX12Engine.exe があるディレクトリを探す（ルート → 直下サブフォルダ）。
fs::path FindEngineDir(const fs::path& extractDir)
{
    std::error_code ec;
    if (fs::exists(extractDir / "DX12Engine.exe", ec)) return extractDir;
    for (auto& e : fs::directory_iterator(extractDir, ec))
    {
        if (e.is_directory(ec) && fs::exists(e.path() / "DX12Engine.exe", ec))
            return e.path();
    }
    return {};
}

// 「最後に適用を試みたリリースタグ」を記録するファイル。exe の場所に依存せず必ず書ける
// %LOCALAPPDATA%\DX12Engine\ に置く（exe が書込不可フォルダにあっても無限ループ防止が効くように）。
fs::path UpdateStatePath()
{
    char* base = nullptr;
    size_t len = 0;
    if (_dupenv_s(&base, &len, "LOCALAPPDATA") != 0 || !base) return {};
    fs::path dir = fs::path(base) / "DX12Engine";
    free(base);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "last_update.txt";
}

// 「この版を飛ばす」で記録した版。もっと新しい版が出るまで案内しない（判定は updatelogic::ShouldPromptUpdate）。
fs::path SkippedStatePath()
{
    fs::path p = UpdateStatePath();
    if (p.empty()) return {};
    return p.parent_path() / "skipped_update.txt";
}

std::string ReadSkippedTag()
{
    fs::path p = SkippedStatePath();
    if (p.empty()) return {};
    std::ifstream f(p);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

void WriteSkippedTag(const std::string& tag)
{
    fs::path p = SkippedStatePath();
    if (p.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    if (f) f << tag;
}

// "v2.1.0" → "2.1.0"
std::string StripV(const std::string& tag)
{
    return (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
}

// リリース JSON（GitHub API）から本文（Markdown）を取り出す。無ければ空。
std::string BodyFromReleaseJson(const std::string& json)
{
    if (json.empty()) return {};
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return {};
    const auto it = j.find("body");
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::string ReadLastUpdateTag()
{
    fs::path p = UpdateStatePath();
    if (p.empty()) return {};
    std::ifstream f(p);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

void WriteLastUpdateTag(const std::string& tag)
{
    fs::path p = UpdateStatePath();
    if (p.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    if (f) f << tag;
}

// 本体終了を待って新ファイルを上書きし再起動する更新バッチを生成・起動する。
bool LaunchUpdaterBatch(const fs::path& srcDir, const fs::path& installDir, const fs::path& tmpRoot)
{
    wchar_t tmpW[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpW);
    fs::path bat = fs::path(tmpW) / "dx12_apply_update.bat";

    const DWORD pid = GetCurrentProcessId();

    const std::string exePath = (installDir / "DX12Engine.exe").string();

    std::ofstream b(bat, std::ios::trunc);
    if (!b.is_open()) return false;
    b << "@echo off\r\n";
    b << "setlocal enabledelayedexpansion\r\n";
    // 本体プロセスの終了を待つ。万一 PID が消えなくても無限待ちにならないよう上限を設ける
    // （上限到達でも robocopy /R で上書きを試みる）。"すぐ再起動" のため待ちは短い間隔でポーリング。
    b << "set tries=0\r\n";
    b << ":wait\r\n";
    b << "tasklist /FI \"PID eq " << pid << "\" 2>nul | findstr /I /C:\"" << pid << "\" >nul || goto apply\r\n";
    b << "set /a tries+=1\r\n";
    b << "if !tries! geq 50 goto apply\r\n";
    b << "ping -n 2 127.0.0.1 >nul\r\n";
    b << "goto wait\r\n";
    b << ":apply\r\n";
    b << "set copytries=0\r\n";
    b << ":copy\r\n";
    // /E=サブフォルダ込み /IS,/IT=既存/変更も上書き（ミラーはしない＝余分なファイルは消さない）
    // /R:20 /W:1=直後はまだ旧 exe がファイルロック解放中のことがあるため粘り強く再試行する。
    b << "robocopy \"" << srcDir.string() << "\" \"" << installDir.string()
      << "\" /E /IS /IT /R:20 /W:1 /NFL /NDL /NJH /NJS /NP >nul\r\n";
    // robocopy の終了コードは 0-7 が成功系、8 以上はコピー失敗を含む（MSDNの仕様）。
    b << "if !errorlevel! LSS 8 goto copydone\r\n";
    b << "set /a copytries+=1\r\n";
    b << "if !copytries! geq 2 goto copydone\r\n";
    b << "ping -n 3 127.0.0.1 >nul\r\n";
    b << "goto copy\r\n";
    b << ":copydone\r\n";
    // 起動時の更新チェックは常に毎回走る（コピーの成否に関わらずフラグ無しで起動）ので、
    // ここでコピーが失敗していても次回起動時に必ず再度プロンプトが出て再試行できる。
    b << "start \"\" \"" << exePath << "\"\r\n";
    b << "rmdir /S /Q \"" << tmpRoot.string() << "\" >nul 2>&1\r\n";
    b << "del \"%~f0\" >nul 2>&1\r\n";
    b.close();

    std::wstring cmd = L"cmd.exe /c \"" + bat.wstring() + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    // CREATE_NO_WINDOW のみ（DETACHED_PROCESS との併用は MSDN 上は無効な組み合わせで
    // 環境次第で再起動に失敗し得る）。隠しコンソールで batch を実行し、本体終了後も生き残る。
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}
} // namespace

bool Updater::RunStartupCheck()
{
    std::error_code ec;

    // exe のあるディレクトリ。配布レイアウト（exe 隣に assets/）でのみ自動更新する。
    wchar_t exePathW[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    fs::path installDir = fs::path(exePathW).parent_path();
    if (!fs::exists(installDir / "assets", ec))
        return false;  // 開発ビルド等。何もしない。

    // 1) 最新タグを取得。api.github.com はレート制限の共有枯渇で無言で使えなくなることがあるため、
    //    まず github.com の releases/latest リダイレクトで確認する（レート制限を受けない）。
    //    それが失敗した場合のみ REST API にフォールバックする。
    std::string tag = FetchLatestTagViaRedirect(kUpdateRepoOwner, kUpdateRepoName);
    std::string latestJson;  // フォールバック時のみ埋まる（後段のアセット探索でも再利用する）

    if (tag.empty())
    {
        Logger::Info("Updater: redirect check failed. falling back to REST API.");
        std::wstring apiUrl = L"https://api.github.com/repos/" +
            Widen(kUpdateRepoOwner) + L"/" + Widen(kUpdateRepoName) + L"/releases/latest";
        std::vector<char> body;
        if (!HttpsFetch(apiUrl, &body, nullptr) || body.empty())
        {
            Logger::Info("Updater: no release info (offline / none / private / rate-limited). skip.");
            return false;
        }
        latestJson.assign(body.begin(), body.end());
        tag = JsonString(latestJson, "tag_name");
        if (tag.empty())
        {
            Logger::Info("Updater: latest release has no tag_name. skip.");
            return false;
        }
    }
    if (!IsNewer(tag, kEngineVersion))
    {
        Logger::Info("Updater: up to date (current={}, latest={}).", kEngineVersion, tag);
        return false;
    }

    // このタグへの適用を既に一度試みたのに、まだ古い版で動いている
    // = リリース zip 内の exe が版を据え置き（公開時の版上げ忘れ等）か、上書き処理の失敗
    //   （ファイルロック等でコピーしきれなかった）の可能性がある。
    // 「毎回絶対に確認・通知してほしい」という要求のため、ここで黙ってスキップはしない。
    // 下の確認ダイアログにその旨を追記した上で、再試行するかどうかは毎回ユーザーに委ねる。
    bool retryingStuckUpdate = (ReadLastUpdateTag() == tag);
    if (retryingStuckUpdate)
    {
        Logger::Warn("Updater: 更新 {} を適用済みのはずですが、実行中の版が {} のままです。"
                     "通知は継続し、再試行するかユーザーに確認します。", tag, kEngineVersion);
    }

    // ダウンロード URL を解決。まず命名規約（installer/build.ps1 が必ずこの名前で zip を作る）
    //    に沿った直リンクを試す。github.com の署名付きダウンロードなので api.github.com 不要。
    //    命名が規約から外れた古い/手動リリースでは 404 になるので、その時だけ REST API で
    //    アセット一覧を引く（latestJson が未取得ならここで初めて取得する）。
    std::wstring directUrl = L"https://github.com/" + Widen(kUpdateRepoOwner) + L"/" +
        Widen(kUpdateRepoName) + L"/releases/download/" + Widen(tag) + L"/dx12-engine-" + Widen(tag) + L".zip";
    std::string assetUrl;
    if (UrlExists(directUrl))
    {
        assetUrl = NarrowAscii(directUrl);  // タグ・パスは ASCII 前提
    }
    else
    {
        if (latestJson.empty())
        {
            std::wstring tagApiUrl = L"https://api.github.com/repos/" +
                Widen(kUpdateRepoOwner) + L"/" + Widen(kUpdateRepoName) + L"/releases/tags/" + Widen(tag);
            std::vector<char> body;
            if (HttpsFetch(tagApiUrl, &body, nullptr) && !body.empty())
                latestJson.assign(body.begin(), body.end());
        }
        assetUrl = FirstZipAssetUrl(latestJson);
    }
    if (assetUrl.empty())
    {
        Logger::Warn("Updater: リリース {} に .zip がないためスキップします。", tag);
        return false;
    }

    // この版は「飛ばす」と記録済みなら、もっと新しい版が出るまで案内しない。
    if (!updatelogic::ShouldPromptUpdate(tag, ReadSkippedTag()))
    {
        Logger::Info("Updater: {} は「この版を飛ばす」で記録済みのため案内しません。", tag);
        return false;
    }

    // 2) リリース本文（Markdown）を取得して、案内の窓に出す。取れなくても案内は出す（本文なしで）。
    //    api.github.com はレート制限があるので、ここだけの任意の取得（失敗しても更新自体は続ける）。
    std::string bodyMd = BodyFromReleaseJson(latestJson);
    if (bodyMd.empty())
    {
        std::wstring tagApiUrl = L"https://api.github.com/repos/" +
            Widen(kUpdateRepoOwner) + L"/" + Widen(kUpdateRepoName) + L"/releases/tags/" + Widen(tag);
        std::vector<char> body;
        if (HttpsFetch(tagApiUrl, &body, nullptr) && !body.empty())
            bodyMd = BodyFromReleaseJson(std::string(body.begin(), body.end()));
    }

    // 3) 案内（起動画面と同じ Direct2D の窓。作れなければ旧来の MessageBox に縮退）
    //    更新の案内・ダウンロード中は起動音を鳴らさない（起動音は起動の演出。待ちの間ループし続けていた）
    SplashScreen::StopSound();
    const std::wstring logoPath = (installDir / "assets" / "editor" / "icons" / "logo.png").wstring();
    updateui::UpdateWindow win;
    const bool haveUi = win.Create(logoPath);
    updateui::Choice choice = updateui::Choice::Later;
    if (haveUi)
    {
        choice = win.Prompt(kEngineVersion, StripV(tag), bodyMd, !bodyMd.empty(), retryingStuckUpdate);
    }
    else
    {
        std::wstring msg =
            L"新しいバージョン " + Widen(tag) + L" が公開されています（現在 " + Widen(kEngineVersion) + L"）。\n\n";
        if (retryingStuckUpdate)
            msg += L"※前回この更新の適用を試みましたが反映されていませんでした（ファイルが使用中で上書きに失敗した可能性があります）。\n\n";
        msg += L"今すぐダウンロードして更新しますか？\n（更新後にエンジンが自動で再起動します）";
        choice = MessageBoxW(nullptr, msg.c_str(), L"Uno Engine アップデート", MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) == IDYES
                     ? updateui::Choice::UpdateNow : updateui::Choice::Later;
    }
    if (choice == updateui::Choice::SkipVersion)
    {
        WriteSkippedTag(tag);
        Logger::Info("Updater: user skipped version {} (will ask again only for a newer version).", tag);
        win.Destroy();
        return false;
    }
    if (choice != updateui::Choice::UpdateNow)
    {
        Logger::Info("Updater: user postponed update to {}.", tag);
        win.Destroy();
        return false;
    }

    // 4) ダウンロード → 展開 → 適用。失敗は窓の中で理由を出し、「もう一度」「このまま起動」を選ばせる。
    wchar_t tmpW[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpW);
    const fs::path tmpRoot = fs::path(tmpW) / "dx12_update";

    // 失敗の通知。戻り値 true = もう一度やる。
    auto askRetry = [&](const char* detail) -> bool
    {
        Logger::Warn("Updater: {}", detail);
        if (haveUi) return win.ShowError("更新できませんでした", detail) == updateui::Choice::Retry;
        MessageBoxW(nullptr, (Widen(detail) + L"\n通常起動します。").c_str(), L"Uno Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    };

    for (;;)
    {
        fs::remove_all(tmpRoot, ec);
        fs::create_directories(tmpRoot, ec);
        const fs::path zip     = tmpRoot / "update.zip";
        const fs::path extract = tmpRoot / "extract";
        fs::create_directories(extract, ec);

        // ダウンロード（速度と残り時間は直近 3 秒の受信量から見積もる）
        std::wstring zipW = zip.wstring();
        Logger::Info("Updater: downloading {} ...", assetUrl);
        updatelogic::RateTracker rate(3.0);
        LARGE_INTEGER qf, q0;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        std::function<void(uint64_t, uint64_t)> onProgress =
            [&](uint64_t done, uint64_t total)
            {
                LARGE_INTEGER q;
                QueryPerformanceCounter(&q);
                rate.Add(static_cast<double>(q.QuadPart - q0.QuadPart) / static_cast<double>(qf.QuadPart), done);
                if (haveUi) win.SetDownloading(done, total, rate.BytesPerSec(), rate.EtaSeconds(done, total));
            };
        if (haveUi) win.SetDownloading(0, 0, 0.0, -1.0);
        if (!HttpsFetch(Widen(assetUrl), nullptr, &zipW, &onProgress) || !fs::exists(zip, ec))
        {
            if (askRetry("ダウンロードに失敗しました。ネットワークの接続を確認して、もう一度お試しください。"
                         "プロキシやセキュリティソフトが GitHub への接続を止めている場合もあります。")) continue;
            win.Destroy();
            return false;
        }

        // 展開（時間が読めないので回る弧）
        if (haveUi) win.SetExtracting();
        if (!ExtractZip(zip, extract, [&] { if (haveUi) win.Pump(); }))
        {
            if (askRetry("ダウンロードしたファイルの展開に失敗しました。ディスクの空き容量を確認して、もう一度お試しください。")) continue;
            win.Destroy();
            return false;
        }
        const fs::path srcDir = FindEngineDir(extract);
        if (srcDir.empty())
        {
            if (askRetry("ダウンロードした更新の中に DX12Engine.exe が見つかりませんでした。しばらくしてから、もう一度お試しください。")) continue;
            win.Destroy();
            return false;
        }

        // 適用: 更新バッチを起動して本体を終了（バッチが上書き→再起動する）。
        // 適用を試みる前にタグを記録 → もし更新後も版が上がらなければ、次回起動時に
        // 上の ReadLastUpdateTag ガードが働き、案内に「前回反映されなかった」旨が付く。
        WriteLastUpdateTag(tag);
        if (haveUi)
        {
            win.SetApplying();
            // 「適用して再起動します」を一瞬でも見せる（0.5 秒。窓は動き続ける）
            const ULONGLONG until = GetTickCount64() + 500;
            while (GetTickCount64() < until) { win.Pump(); Sleep(16); }
        }
        if (!LaunchUpdaterBatch(srcDir, installDir, tmpRoot))
        {
            if (askRetry("更新を適用するプログラムを起動できませんでした。もう一度お試しください。")) continue;
            win.Destroy();
            return false;
        }
        break;
    }

    win.Destroy();
    Logger::Info("Updater: applying update to {}. exiting for restart.", tag);
    return true;  // 呼び出し側（main）は即終了する
}

// ---------------------------------------------------------------------------
// 更新の流れの見本（--demo-update[=error]）。ネットワーク・ファイルには一切触れない。
// ---------------------------------------------------------------------------
bool Updater::RunDemo(bool fail)
{
    namespace fs = std::filesystem;
    // 見本の「次の版」= 今の版のパッチを 1 つ上げたもの。本文は今の版のリリースノートを使う
    // （本物の案内も GitHub の本文 = 同じデータから書き出した Markdown を出すので、見え方が同じになる）。
    int v[3] = {0, 0, 0};
    relnotes::ParseVersion(kEngineVersion, v);
    const std::string nextVer = std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2] + 1);
    std::string bodyMd;
    const auto& all = relnotes::All();
    if (const relnotes::Release* r = relnotes::Find(all, kEngineVersion))
        bodyMd = relnotes::ToMarkdown(*r, kEngineName);

    SplashScreen::StopSound();   // 本物と同じく、案内からは無音
    const std::wstring logoPath = Widen(PathResolver::AssetsDir() + "editor/icons/logo.png");
    updateui::UpdateWindow win;
    if (!win.Create(logoPath))
    {
        Logger::Warn("Updater(demo): 更新の窓を作れませんでした。");
        return false;
    }
    Logger::Info("Updater(demo): 更新の流れの見本を表示します（実際の更新は行いません）。");

    const updateui::Choice choice = win.Prompt(kEngineVersion, nextVer, bodyMd, !bodyMd.empty(), false);
    if (choice != updateui::Choice::UpdateNow)
    {
        Logger::Info("Updater(demo): 「{}」が選ばれました。通常どおり起動します。",
                     choice == updateui::Choice::SkipVersion ? "この版を飛ばす" : "後で");
        win.Destroy();
        return false;
    }

    // 見本のダウンロード: 実際の配布 zip と同じくらいの大きさを、回線速度が少し揺れる体で約 7 秒かけて進める。
    const uint64_t total = 49397031ull;
    bool failedOnce = false;
    for (;;)
    {
        updatelogic::RateTracker rate(3.0);
        win.SetDownloading(0, 0, 0.0, -1.0);
        const ULONGLONG t0 = GetTickCount64();
        uint64_t done = 0;
        bool aborted = false;
        while (done < total)
        {
            const double t = static_cast<double>(GetTickCount64() - t0) / 1000.0;
            const double bps = 6.5e6 * (1.0 + 0.25 * std::sin(t * 2.3));   // 約 6.5 MB/s を中心に揺らす
            done = (std::min)(total, done + static_cast<uint64_t>(bps * 0.016));
            rate.Add(t, done);
            win.SetDownloading(done, total, rate.BytesPerSec(), rate.EtaSeconds(done, total));
            if (fail && !failedOnce && done > total * 6 / 10) { aborted = true; break; }
            Sleep(16);
        }
        if (aborted)
        {
            failedOnce = true;
            const updateui::Choice c = win.ShowError("更新できませんでした",
                "ダウンロードに失敗しました。ネットワークの接続を確認して、もう一度お試しください。"
                "（これは見本の失敗です。「もう一度」で続きを見られます）");
            if (c == updateui::Choice::Retry) continue;
            win.Destroy();
            return false;
        }
        break;
    }

    win.SetExtracting();
    for (ULONGLONG until = GetTickCount64() + 1600; GetTickCount64() < until; ) { win.Pump(); Sleep(16); }
    win.SetApplying();
    for (ULONGLONG until = GetTickCount64() + 1200; GetTickCount64() < until; ) { win.Pump(); Sleep(16); }
    win.Destroy();
    Logger::Info("Updater(demo): 見本の更新が終わりました（ファイルは変えていません）。");
    return true;
}
} // namespace dx12e
