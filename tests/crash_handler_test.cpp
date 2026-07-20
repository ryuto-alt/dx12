// CrashHandler 統合テスト。
// 自分自身を "--crash-child" 付きで再起動し、子プロセスがアクセス違反で死んだ後に
// dx12_crash.log(ACCESS_VIOLATION とスタックトレース) と dx12_crash.dmp が
// 生成されていることを親が確認する。
#include "core/CrashHandler.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--crash-child") == 0)
    {
        dx12e::CrashHandler::Install();
        // パンくずが「新しい順」で残ることも確認する（古いものが押し出されるリング）
        dx12e::CrashHandler::Breadcrumb("crumb-oldest");
        dx12e::CrashHandler::Breadcrumb("crumb-newest");
        volatile int* p = nullptr;
        *p = 42;      // アクセス違反 → CrashHandler がレポートを書いてプロセス終了
        return 0;     // 到達しない
    }

    std::error_code ec;
    fs::remove("dx12_crash.log", ec);
    fs::remove("dx12_crash.dmp", ec);

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" --crash-child";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        std::printf("FAIL: CreateProcess (%lu)\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!fs::exists("dx12_crash.log")) { std::printf("FAIL: dx12_crash.log が生成されない\n"); return 1; }
    if (!fs::exists("dx12_crash.dmp")) { std::printf("FAIL: dx12_crash.dmp が生成されない\n"); return 1; }

    std::ifstream ifs("dx12_crash.log", std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string report = ss.str();

    if (report.find("ACCESS_VIOLATION") == std::string::npos)
    {
        std::printf("FAIL: レポートに ACCESS_VIOLATION が無い:\n%s\n", report.c_str());
        return 1;
    }
    if (report.find("#00") == std::string::npos)
    {
        std::printf("FAIL: レポートにスタックトレースが無い:\n%s\n", report.c_str());
        return 1;
    }

    const size_t newest = report.find("crumb-newest");
    const size_t oldest = report.find("crumb-oldest");
    if (newest == std::string::npos || oldest == std::string::npos)
    {
        std::printf("FAIL: レポートにパンくずが無い:\n%s\n", report.c_str());
        return 1;
    }
    if (newest > oldest)
    {
        std::printf("FAIL: パンくずが新しい順に並んでいない:\n%s\n", report.c_str());
        return 1;
    }

    std::printf("PASS: crash report generated\n%s\n", report.c_str());
    return 0;
}
