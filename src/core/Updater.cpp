#include "core/Updater.h"
#include "core/Version.h"
#include "core/Logger.h"

#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <cstdlib>
#include <cwchar>

// MSVC のみ運用（VS2022）。WinHTTP / コモンコントロール(プログレスバー)を自動リンク（CMake 変更不要）。
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")

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

// 進捗メーター窓（Win32 ネイティブ）。エンジンのウィンドウ/ImGui 生成より前に動くため、
// ImGui ではなく comctl32 のプログレスバーで「ダウンロード/展開/適用」の進捗を表示する。
struct ProgressUI
{
    HWND hwnd = nullptr, bar = nullptr, label = nullptr, pct = nullptr;

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        return DefWindowProcW(h, m, w, l);
    }

    bool Create()
    {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);

        const wchar_t* cls = L"DX12EngineUpdaterProgress";
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc   = Proc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = cls;
            wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            RegisterClassW(&wc);
            registered = true;
        }

        const int w = 460, h = 168;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        // WS_SYSMENU を付けない＝閉じる×無し（ダウンロード中の誤操作防止）
        // ※ 文字列は生の日本語ワイド文字リテラル。CMake で Core ターゲットに /utf-8 を渡しているので
        //   ソース(UTF-8)が正しく UTF-16 に変換される。以前は UTF-8 バイト列を \x で
        //   ワイド文字リテラルに直書きしていて 1 バイト＝1 wchar になり文字化けしていた。
        hwnd = CreateWindowExW(WS_EX_TOPMOST, cls, L"DX12 Engine アップデート",
            WS_POPUP | WS_CAPTION | WS_BORDER, x, y, w, h,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return false;

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        label = CreateWindowExW(0, L"STATIC",
            L"アップデートを準備しています...",
            WS_CHILD | WS_VISIBLE, 20, 18, 410, 22, hwnd, nullptr, nullptr, nullptr);
        bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE, 20, 52, 418, 26, hwnd, nullptr, nullptr, nullptr);
        pct = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, 20, 88, 410, 22, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(pct,   WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(bar, PBM_SETRANGE32, 0, 100);

        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        Pump();
        return true;
    }

    void SetLabel(const wchar_t* s) { if (label) SetWindowTextW(label, s); }

    // percent<0 で不確定（マーキー）。done/total はバイト数（pct テキスト表示用）。
    void SetProgress(int percent, uint64_t done, uint64_t total)
    {
        if (bar)
        {
            LONG style = GetWindowLongW(bar, GWL_STYLE);
            if (percent < 0)
            {
                if (!(style & PBS_MARQUEE))
                {
                    SetWindowLongW(bar, GWL_STYLE, style | PBS_MARQUEE);
                    SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
                }
            }
            else
            {
                if (style & PBS_MARQUEE)
                {
                    SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
                    SetWindowLongW(bar, GWL_STYLE, style & ~PBS_MARQUEE);
                }
                SendMessageW(bar, PBM_SETPOS, static_cast<WPARAM>(percent), 0);
            }
        }
        if (pct)
        {
            wchar_t buf[160];
            if (percent < 0)
            {
                if (done > 0) swprintf(buf, 160, L"%.1f MB", done / 1048576.0);
                else          buf[0] = L'\0';
            }
            else if (total > 0)
            {
                swprintf(buf, 160, L"%d%%   ( %.1f / %.1f MB )",
                         percent, done / 1048576.0, total / 1048576.0);
            }
            else
            {
                swprintf(buf, 160, L"%d%%", percent);
            }
            SetWindowTextW(pct, buf);
        }
    }

    void Pump()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void Destroy()
    {
        if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
        Pump();
    }
};

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
bool RunHiddenPumped(const std::wstring& cmdLine, ProgressUI* ui, DWORD& code)
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
        // 100ms ごとに起きて進捗窓のメッセージを処理（マーキー animation と応答性を維持）。
        DWORD w = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 100, QS_ALLINPUT);
        if (ui) ui->Pump();
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
bool ExtractZip(const fs::path& zip, const fs::path& dest, ProgressUI* ui = nullptr)
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
            if (RunHiddenPumped(cmd, ui, code) && code == 0)
                return true;
        }
    }

    // 2) フォールバック: PowerShell Expand-Archive（低速だが tar 不在環境向け）
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"Expand-Archive -Force -LiteralPath '" + zip.wstring() +
        L"' -DestinationPath '" + dest.wstring() + L"'\"";
    DWORD code = 1;
    return RunHiddenPumped(cmd, ui, code) && code == 0;
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

    // 2) ユーザーに確認
    std::wstring msg =
        L"新しいバージョン " + Widen(tag) + L" が公開されています（現在 " +
        Widen(kEngineVersion) + L"）。\n\n";
    if (retryingStuckUpdate)
    {
        msg += L"※前回この更新の適用を試みましたが反映されていませんでした"
               L"（ファイルが使用中で上書きに失敗した可能性があります）。\n\n";
    }
    msg +=
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

    // 進捗メーター窓を表示（ダウンロード→展開→適用の各段階を可視化）
    ProgressUI ui;
    ui.Create();
    ui.SetLabel(L"アップデートをダウンロードしています...");

    std::wstring zipW = zip.wstring();
    Logger::Info("Updater: downloading {} ...", assetUrl);
    std::function<void(uint64_t, uint64_t)> onProgress =
        [&ui](uint64_t done, uint64_t total)
        {
            int pct = (total > 0) ? static_cast<int>((done * 100) / total) : -1;
            ui.SetProgress(pct, done, total);
            ui.Pump();
        };
    if (!HttpsFetch(Widen(assetUrl), nullptr, &zipW, &onProgress) || !fs::exists(zip, ec))
    {
        ui.Destroy();
        MessageBoxW(nullptr, L"アップデートのダウンロードに失敗しました。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }

    // 4) 展開（時間が読めないのでマーキー表示）
    ui.SetLabel(L"ファイルを展開しています...");
    ui.SetProgress(-1, 0, 0);
    ui.Pump();
    if (!ExtractZip(zip, extract, &ui))
    {
        ui.Destroy();
        MessageBoxW(nullptr, L"アップデートの展開に失敗しました。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }
    fs::path srcDir = FindEngineDir(extract);
    if (srcDir.empty())
    {
        ui.Destroy();
        MessageBoxW(nullptr, L"ダウンロードした更新に DX12Engine.exe が見つかりません。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }

    // 5) 更新バッチを起動して本体を終了（バッチが上書き→再起動する）
    // 適用を試みる前にタグを記録 → もし更新後も版が上がらなければ、次回起動時に
    // 上の ReadLastUpdateTag ガードで再プロンプトを止めて無限ループを断つ。
    WriteLastUpdateTag(tag);
    ui.SetLabel(L"更新を適用して再起動します...");
    ui.SetProgress(100, 0, 0);
    ui.Pump();
    if (!LaunchUpdaterBatch(srcDir, installDir, tmpRoot))
    {
        ui.Destroy();
        MessageBoxW(nullptr, L"アップデータの起動に失敗しました。\n通常起動します。",
            L"DX12 Engine アップデート", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }

    ui.Destroy();
    Logger::Info("Updater: applying update to {}. exiting for restart.", tag);
    return true;  // 呼び出し側（main）は即終了する
}
} // namespace dx12e
