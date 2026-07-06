#include "CrashHandler.h"
#include "Version.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>

#pragma comment(lib, "dbghelp.lib")

namespace dx12e
{
namespace
{

// クラッシュ処理中は通常のヒープ/CRT/ロガーが壊れている可能性があるため、
// このファイル内では Win32 API と snprintf/fprintf だけで完結させる。

const char* ExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
    case 0xE06D7363:                         return "CPP_EXCEPTION (未捕捉のC++例外)";
    case 0xE0000001:                         return "TERMINATE/ABORT (std::terminate・abort・純粋仮想呼び出し)";
    default:                                 return "UNKNOWN";
    }
}

// PC アドレス → "Module.exe!Function +0x12 (file.cpp:34)" 形式で out へ1行書く。
void WriteFrame(FILE* out, int index, DWORD64 pc)
{
    HANDLE proc = GetCurrentProcess();

    char modName[MAX_PATH] = "?";
    DWORD64 modBase = SymGetModuleBase64(proc, pc);
    if (modBase != 0)
    {
        char full[MAX_PATH] = {};
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(modBase), full, MAX_PATH))
        {
            const char* base = std::strrchr(full, '\\');
            std::snprintf(modName, sizeof(modName), "%s", base ? base + 1 : full);
        }
    }

    // シンボル名(PDB がある開発ビルドのみ解決される)
    unsigned char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;
    DWORD64 symDisp = 0;
    const bool hasSym = SymFromAddr(proc, pc, &symDisp, sym) != FALSE;

    // ファイル:行(こちらも PDB 前提)
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    const bool hasLine = SymGetLineFromAddr64(proc, pc, &lineDisp, &line) != FALSE;

    if (hasSym && hasLine)
    {
        const char* file = std::strrchr(line.FileName, '\\');
        std::fprintf(out, "  #%02d %s!%s +0x%llx (%s:%lu)\n", index, modName, sym->Name,
                     static_cast<unsigned long long>(symDisp),
                     file ? file + 1 : line.FileName, line.LineNumber);
    }
    else if (hasSym)
    {
        std::fprintf(out, "  #%02d %s!%s +0x%llx\n", index, modName, sym->Name,
                     static_cast<unsigned long long>(symDisp));
    }
    else
    {
        // PDB 無し(配布版): モジュール+オフセットを残す。dx12_crash.dmp と PDB があれば後から解決できる。
        std::fprintf(out, "  #%02d %s +0x%llx (pc=0x%016llx)\n", index, modName,
                     static_cast<unsigned long long>(modBase ? pc - modBase : 0),
                     static_cast<unsigned long long>(pc));
    }
}

void WriteStackTrace(FILE* out, const CONTEXT* crashCtx)
{
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);

    CONTEXT ctx = *crashCtx;   // StackWalk64 が書き換えるためコピー
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset    = ctx.Rip;  sf.AddrPC.Mode    = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;  sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;  sf.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 64; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0)
            break;
        WriteFrame(out, i, sf.AddrPC.Offset);
    }
}

void WriteMiniDump(EXCEPTION_POINTERS* ep)
{
    HANDLE file = CreateFileA("dx12_crash.dmp", GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    // MiniDumpWithDataSegs: グローバル変数も入る(既定 Normal + α程度のサイズ)。
    // ponytail: フルメモリダンプは巨大になるので撮らない。足りなければ WithFullMemory へ。
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MiniDumpWithDataSegs, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep)
{
    // 再入ガード(レポート書き込み中に再クラッシュしたら何もせず死ぬ)
    static volatile LONG s_inCrash = 0;
    if (InterlockedExchange(&s_inCrash, 1) != 0)
        return EXCEPTION_EXECUTE_HANDLER;

    WriteMiniDump(ep);

    FILE* out = nullptr;
    if (fopen_s(&out, "dx12_crash.log", "w") == 0 && out)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char exePath[MAX_PATH] = "?";
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);

        std::fprintf(out, "=== DX12 Engine クラッシュレポート ===\n");
        std::fprintf(out, "time    : %04u-%02u-%02u %02u:%02u:%02u\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::fprintf(out, "version : v%s\n", kEngineVersion);
        std::fprintf(out, "exe     : %s\n", exePath);
        std::fprintf(out, "thread  : %lu\n", GetCurrentThreadId());

        if (ep && ep->ExceptionRecord)
        {
            const auto* er = ep->ExceptionRecord;
            std::fprintf(out, "except  : 0x%08lX %s\n", er->ExceptionCode, ExceptionName(er->ExceptionCode));
            std::fprintf(out, "address : 0x%016llx\n",
                         reinterpret_cast<unsigned long long>(er->ExceptionAddress));
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            {
                const char* op = er->ExceptionInformation[0] == 0 ? "読み取り"
                               : er->ExceptionInformation[0] == 1 ? "書き込み" : "実行(DEP)";
                std::fprintf(out, "detail  : アドレス 0x%016llx への%sで違反\n",
                             static_cast<unsigned long long>(er->ExceptionInformation[1]), op);
            }
        }

        std::fprintf(out, "\n--- スタックトレース(クラッシュスレッド) ---\n");
        if (ep && ep->ContextRecord)
            WriteStackTrace(out, ep->ContextRecord);

        std::fprintf(out, "\nミニダンプ: dx12_crash.dmp (Visual Studio で開くと該当行へ飛べる)\n");
        std::fprintf(out, "直前の実行ログ: dx12_engine.log\n");
        std::fclose(out);
    }

    return EXCEPTION_EXECUTE_HANDLER;   // WER のダイアログは出さずプロセス終了
}

// std::terminate / abort / 純粋仮想呼び出しを「現在位置のコンテキスト付きSEH」に変換して
// 上のレポート経路へ流す(スタックトレースが取れる)。
void ReportHereAndDie()
{
    __try
    {
        RaiseException(0xE0000001, 0, 0, nullptr);
    }
    __except (OnUnhandledException(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
    {
    }
    TerminateProcess(GetCurrentProcess(), 3);
}

} // namespace

void CrashHandler::Install()
{
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(ReportHereAndDie);
    _set_purecall_handler(ReportHereAndDie);
    std::signal(SIGABRT, [](int) { ReportHereAndDie(); });

    // スタックオーバーフロー時でもハンドラが動けるよう予備スタックを確保しておく。
    // ponytail: メインスレッドのみ。ワーカーでのスタックオーバーフローはダンプ無しで死ぬ可能性あり。
    ULONG reserve = 32 * 1024;
    SetThreadStackGuarantee(&reserve);
}

} // namespace dx12e
