#pragma once
// クラッシュ報告の最小サポート（M9）。
//   CrashHandler は落ちた瞬間に CWD へ dx12_crash.log / dx12_crash.dmp を書く（クラッシュ経路はヒープ禁止なので触らない）。
//   ここは【次回起動時】に純 std::filesystem でそれを拾い、直前ログ 200 行と一緒に保管して、MCP の crash_report / doctor から読めるようにする。
//     <destRoot>/<yyyymmdd-hhmmss>/{crash.log, dx12_crash.dmp, log_tail.txt, report.json, ack(確認済みのとき)}
//   1 度拾えば元ファイルは移動済みなので二度検知されない。古い報告は 20 件まで。
//   spdlog にも nlohmann にも依存しない（ctest が単体で建てる）。
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace dx12e
{

struct CrashInfo
{
    std::string id;             // フォルダ名（yyyymmdd-hhmmss）
    std::string at;             // crash.log の time 行
    std::string dir;
    std::string exception;      // "0xC0000005 ACCESS_VIOLATION"
    std::string address;
    std::string thread;
    std::string engineVersion;
    std::string exe;
    std::string detail;
    std::string stack;          // スタックトレース節（テキスト）
    std::string breadcrumbs;    // 直前の操作節（テキスト）
    std::string dmp;            // ミニダンプのパス（無ければ空）
    std::vector<std::string> logTail;   // 直前のログ最大 200 行
    bool        acknowledged = false;
    bool        newThisLaunch = false;  // 今回の起動で検知したもの
};

class CrashReport
{
public:
    // DX12E_DATA_DIR があれば <それ>/crash、無ければ %LOCALAPPDATA%/UnoEngine/crash。
    static std::filesystem::path DefaultRoot();

    // cwd に dx12_crash.log があれば destRoot へ保管して true。**dx12_engine.log が truncate される前**に呼ぶこと。例外は投げない。
    static bool CaptureIfPresent(const std::filesystem::path& cwd, const std::filesystem::path& destRoot,
                                 std::string* capturedId = nullptr, size_t tailLines = 200);

    // 新しい順。includeLog=false なら logTail を読まない。
    static std::vector<CrashInfo> LoadAll(const std::filesystem::path& destRoot, bool includeLog = true, size_t maxCount = 20);

    // 全部を確認済みにする（ack ファイルを置く）。戻り値 = 新たに確認済みにした件数。
    static size_t AcknowledgeAll(const std::filesystem::path& destRoot);

    // 今回の起動で拾った報告の id（無ければ空）。
    static std::string LastCapturedId();
};

} // namespace dx12e
