#include "core/Updater.h"
#include "core/Version.h"
#include "core/Logger.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdlib>

// MSVC のみ運用（VS2022）。WinHTTP を自動リンク（CMake 変更不要）。
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
bool HttpsFetch(const std::wstring& url, std::vector<char>* outBytes, const std::wstring* outFile)
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
    WinHttpSetTimeouts(hSession, 8000, 8000, 15000, 30000);

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

// PowerShell の Expand-Archive で zip を展開（標準機能・追加依存なし）。
bool ExtractZip(const fs::path& zip, const fs::path& dest)
{
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"Expand-Archive -Force -LiteralPath '" + zip.wstring() +
        L"' -DestinationPath '" + dest.wstring() + L"'\"";

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
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

// 本体終了を待って新ファイルを上書きし再起動する更新バッチを生成・起動する。
bool LaunchUpdaterBatch(const fs::path& srcDir, const fs::path& installDir, const fs::path& tmpRoot)
{
    wchar_t tmpW[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpW);
    fs::path bat = fs::path(tmpW) / "dx12_apply_update.bat";

    const DWORD pid = GetCurrentProcessId();

    std::ofstream b(bat, std::ios::trunc);
    if (!b.is_open()) return false;
    b << "@echo off\r\n";
    b << ":wait\r\n";
    b << "tasklist /FI \"PID eq " << pid << "\" 2>nul | findstr /I /C:\"" << pid << "\" >nul\r\n";
    b << "if %errorlevel%==0 (\r\n";
    b << "  ping -n 2 127.0.0.1 >nul\r\n";
    b << "  goto wait\r\n";
    b << ")\r\n";
    // /E=サブフォルダ込み /IS,/IT=既存/変更も上書き（ミラーはしない＝余分なファイルは消さない）
    b << "robocopy \"" << srcDir.string() << "\" \"" << installDir.string()
      << "\" /E /IS /IT /R:3 /W:1 /NFL /NDL /NJH /NJS /NP >nul\r\n";
    b << "start \"\" \"" << (installDir / "DX12Engine.exe").string() << "\"\r\n";
    b << "rmdir /S /Q \"" << tmpRoot.string() << "\" >nul 2>&1\r\n";
    b << "del \"%~f0\" >nul 2>&1\r\n";
    b.close();

    std::wstring cmd = L"cmd.exe /c \"" + bat.wstring() + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
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

    // 1) 最新リリース情報を取得
    std::wstring apiUrl = L"https://api.github.com/repos/" +
        Widen(kUpdateRepoOwner) + L"/" + Widen(kUpdateRepoName) + L"/releases/latest";
    std::vector<char> body;
    if (!HttpsFetch(apiUrl, &body, nullptr) || body.empty())
    {
        Logger::Info("Updater: no release info (offline / none / private). skip.");
        return false;
    }
    std::string json(body.begin(), body.end());
    std::string tag = JsonString(json, "tag_name");
    if (tag.empty())
    {
        Logger::Info("Updater: latest release has no tag_name. skip.");
        return false;
    }
    if (!IsNewer(tag, kEngineVersion))
    {
        Logger::Info("Updater: up to date (current={}, latest={}).", kEngineVersion, tag);
        return false;
    }
    std::string assetUrl = FirstZipAssetUrl(json);
    if (assetUrl.empty())
    {
        Logger::Warn("Updater: release {} has no .zip asset. skip.", tag);
        return false;
    }

    // 2) ユーザーに確認
    std::wstring msg =
        L"新しいバージョン " + Widen(tag) + L" が公開されています（現在 " +
        Widen(kEngineVersion) + L"）。\n\n"
        L"今すぐダウンロードして更新しますか？\n"
        L"（更新後にエンジンが自動で再起動します）";
    int r = MessageBoxW(nullptr, msg.c_str(), L"DX12 Engine アップデート",
        MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST);
    if (r != IDYES)
    {
        Logger::Info("Updater: user skipped update to {}.", tag);
        return false;
    }

    // 3) ダウンロード
    wchar_t tmpW[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpW);
    fs::path tmpRoot = fs::path(tmpW) / "dx12_update";
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);
    fs::path zip     = tmpRoot / "update.zip";
    fs::path extract = tmpRoot / "extract";
    fs::create_directories(extract, ec);

    std::wstring zipW = zip.wstring();
    Logger::Info("Updater: downloading {} ...", assetUrl);
    if (!HttpsFetch(Widen(assetUrl), nullptr, &zipW) || !fs::exists(zip, ec))
    {
        MessageBoxW(nullptr, L"アップデートのダウンロードに失敗しました。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }

    // 4) 展開
    if (!ExtractZip(zip, extract))
    {
        MessageBoxW(nullptr, L"アップデートの展開に失敗しました。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }
    fs::path srcDir = FindEngineDir(extract);
    if (srcDir.empty())
    {
        MessageBoxW(nullptr, L"ダウンロードした更新に DX12Engine.exe が見つかりません。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }

    // 5) 更新バッチを起動して本体を終了（バッチが上書き→再起動する）
    if (!LaunchUpdaterBatch(srcDir, installDir, tmpRoot))
    {
        MessageBoxW(nullptr, L"アップデータの起動に失敗しました。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }

    Logger::Info("Updater: applying update to {}. exiting for restart.", tag);
    return true;  // 呼び出し側（main）は即終了する
}
} // namespace dx12e
