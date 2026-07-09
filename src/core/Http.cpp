#include "core/Http.h"

#include <windows.h>
#include <winhttp.h>

// Updater.cpp と同じく MSVC 運用のみ想定。CMake 変更なしで自動リンク。
#pragma comment(lib, "winhttp.lib")

namespace dx12e::http
{

bool Get(const std::wstring& url, std::vector<uint8_t>& out,
         const std::function<void(uint64_t, uint64_t)>* progress,
         const std::atomic<bool>* cancel)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    uc.lpszHostName = host;  uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"DX12Engine-MaterialLibrary/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    // ダウンロード(画像含む)は起動時チェックより時間がかかりうるので受信タイムアウトは長めに取る。
    WinHttpSetTimeouts(hSession, 5000, 5000, 15000, 60000);

    bool good = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (hConnect)
    {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hReq)
        {
            BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

            DWORD status = 0, sz = sizeof(status);
            if (ok)
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

            if (ok && status == 200)
            {
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
                    if (cancel && cancel->load()) { good = false; break; }

                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(hReq, &avail)) { good = false; break; }
                    if (avail == 0) break;
                    std::vector<uint8_t> buf(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(hReq, buf.data(), avail, &read)) { good = false; break; }
                    if (read == 0) break;
                    out.insert(out.end(), buf.data(), buf.data() + read);
                    done += read;
                    if (progress && *progress) (*progress)(done, total);
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return good;
}

} // namespace dx12e::http
