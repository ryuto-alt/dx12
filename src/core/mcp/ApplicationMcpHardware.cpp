// ===========================================================================
// 物理ハードウェア連携（Arduino / ESP32）: Application への組み込みと MCP method（docs/HARDWARE.md）
// ---------------------------------------------------------------------------
//   前半: Application のメンバ（InitHardware / ReinitHardware / ApplyHardwareActionBindings /
//         ServiceHardwareReads / ShutdownHardware）。エディタのプロジェクトロードと配布ゲームの起動の
//         両方から呼ばれる（hardware.json は vfs 経由なので pak の中でも読める）。
//   後半: MCP method
//     hw_list_ports / hw_status / hw_connect / hw_disconnect / hw_read / hw_monitor /
//     hw_write / hw_write_dangerous / hw_simulate / hw_calibrate / hw_config_get / hw_config_set /
//     hw_flash / hw_flash_status / hw_setup
//
// ★hw_read は応答をメインスレッドを止めずに遅らせる（m_hwReads を毎フレーム見て、時間が来たら返す）。
// ★hw_flash は arduino-cli をワーカースレッドで走らせる（CREATE_NO_WINDOW + BELOW_NORMAL）。
//   走らせる間は対象デバイスのポートを放し、終わったら Connect し直す。同時に 1 本だけ。
//   子プロセス（esptool など）ごと終わらせられるよう Job オブジェクトに入れる（エンジンが落ちても残さない）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"
#include "core/AtomicFile.h"
#include "hardware/HwPortEnum.h"
#include "hardware/HwToolchain.h"
#include "editor/EditorContext.h"
#include "project/ProjectManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;   // P(...)

namespace fs = std::filesystem;

namespace
{
using nlohmann::json;

double SteadySec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// 不正な UTF-8 を '?' に置き換える（arduino-cli の出力が CP932 で混ざっても JSON が壊れないように）
std::string SanitizeUtf8(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c < 0xE0) len = 2;
        else if (c >= 0xE0 && c < 0xF0) len = 3;
        else if (c >= 0xF0 && c < 0xF5) len = 4;
        else { o += '?'; ++i; continue; }
        if (i + len > s.size()) { o += '?'; ++i; continue; }
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { ok = false; break; }
        if (!ok) { o += '?'; ++i; continue; }
        o.append(s, i, len);
        i += len;
    }
    return o;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---- hw_flash の状態 -------------------------------------------------------
struct HwAppState
{
    std::string arduinoCli;   // hardware.json の arduinoCli（保存し直しても落とさないよう、ここで持ち回る）

    std::mutex  m;
    std::thread worker;
    bool        running = false;
    std::string state = "idle";    // idle / compiling / uploading / reconnecting / done / failed
    std::string device, port, fqbn, sketch, command, error;
    int         exitCode = -1;
    bool        reconnected = false;
    double      startedSec = 0.0, endedSec = 0.0;
    std::deque<std::string> log;   // 末尾 200 行
    std::string partial;
    HANDLE      process = nullptr; // 実行中の arduino-cli（キャンセル用）
    HANDLE      job = nullptr;
    std::atomic<bool> cancel{false};
};

HwAppState& St(std::shared_ptr<void>& p)
{
    if (!p) p = std::make_shared<HwAppState>();
    return *static_cast<HwAppState*>(p.get());
}

// ハードウェア窓まわりの実行時状態（Application::m_hwUiRt。メインスレッドだけが触る）
struct HwUiRt
{
    double lastTick = -100.0, lastPorts = -100.0, lastSketches = -100.0;
    bool   hasSerial = false;
    std::string autoStartBase;                  // 自動導入を試したプロジェクト（同じプロジェクトで繰り返さない）
    bool   notifyOnDone = false;                // 導入が終わったらトーストで知らせる
    std::vector<hwui::PortRow> ports;
    std::vector<std::string>   sketches;
    std::string                sketchesBase;
    std::map<std::string, double> portSeen;     // 既知 VID のポートを初めて見た時刻（連続して見えている間だけ残す）
    std::set<std::string>         prompted;     // 案内を出したポート（セッションで 1 回）
};

HwUiRt& Rt(std::shared_ptr<void>& p)
{
    if (!p) p = std::make_shared<HwUiRt>();
    return *static_cast<HwUiRt*>(p.get());
}

bool KnownBoardVid(const std::string& vid)
{
    static const char* const kVids[] = {"1A86", "10C4", "0403", "2341", "2A03", "303A"};
    std::string v = vid;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const char* k : kVids) if (v == k) return true;
    return false;
}

void PushLogText(HwAppState& st, const char* data, size_t n)
{
    std::lock_guard<std::mutex> lk(st.m);
    for (size_t i = 0; i < n; ++i)
    {
        const char c = data[i];
        if (c == '\n' || c == '\r')
        {
            if (!st.partial.empty())
            {
                if (st.partial.size() > 300) st.partial.resize(300);
                const std::string line = SanitizeUtf8(st.partial);
                // コンパイルが終わって書き込みに入った合図（arduino-cli は compile → upload の順に出す）
                if (st.state == "compiling" &&
                    (line.find("Sketch uses") != std::string::npos || line.find("Global variables") != std::string::npos ||
                     line.find("スケッチ") != std::string::npos || line.find("グローバル変数") != std::string::npos ||   // 日本語ロケールの arduino-cli
                     line.find("Uploading") != std::string::npos || line.find("esptool") != std::string::npos ||
                     line.find("avrdude") != std::string::npos || line.find("Writing at") != std::string::npos))
                    st.state = "uploading";
                st.log.push_back(line);
                if (st.log.size() > 200) st.log.pop_front();
                st.partial.clear();
            }
        }
        else st.partial += c;
    }
}

void SetState(HwAppState& st, const char* s)
{
    std::lock_guard<std::mutex> lk(st.m);
    st.state = s;
}

// 走っているフラッシュを止めて回収する（メインスレッド。ワーカーは Connect し直さずに終わる）
void StopFlash(HwAppState& st)
{
    st.cancel.store(true);
    {
        std::lock_guard<std::mutex> lk(st.m);
        if (st.job) ::TerminateJobObject(st.job, 1);   // 子プロセスごと
        else if (st.process) ::TerminateProcess(st.process, 1);
    }
    if (st.worker.joinable()) st.worker.join();
    st.cancel.store(false);
}

std::wstring ToWide(const std::string& s) { return PathResolver::Utf8ToWide(s); }

// arduino-cli.exe の場所は hw::FindArduinoCli（hardware/HwToolchain.h。hardware.json → 管理下 → PATH → 既定の場所）

// エンジンのリポジトリ（hardware/firmware がある所）。開発ビルドだけ。見つからなければ空。
fs::path FindRepoRoot()
{
    auto good = [](const fs::path& p) {
        std::error_code ec;
        return !p.empty() && fs::is_directory(p / "hardware" / "firmware", ec);
    };
    std::vector<fs::path> starts;
    {
        std::wstring s = PathResolver::ShaderSourceDirW();
        while (!s.empty() && (s.back() == L'/' || s.back() == L'\\')) s.pop_back();
        if (!s.empty()) starts.push_back(fs::path(s).parent_path());   // <repo>/shaders → <repo>
    }
    {
        // ビルド時に焼き込まれたソースツリーの場所（assets/ の 2 つ上 = リポジトリ）。fleet のように exe を別の場所へ
        // コピーして動かしていても、ビルドしたマシンの上ならリポジトリを指す（配布先では存在しないので無視される）。
        fs::path a = ASSETS_DIR;
        while (!a.empty() && !a.has_filename() && a != a.root_path()) a = a.parent_path();   // 末尾 "/" を落とす
        starts.push_back(a.parent_path());
    }
    {
        wchar_t buf[MAX_PATH * 2] = {};
        if (::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf))) > 0)
            starts.push_back(fs::path(buf).parent_path());
    }
    for (const fs::path& s0 : starts)
    {
        fs::path p = s0;
        for (int up = 0; up < 8 && !p.empty(); ++up)
        {
            if (good(p)) return p;
            const fs::path parent = p.parent_path();
            if (parent == p) break;
            p = parent;
        }
    }
    return {};
}

// スケッチを解決する。.ino かそのフォルダ。相対パスはプロジェクト → <repo>/hardware/firmware/ の順。
// 戻り値は arduino-cli に渡すパス（空 = 見つからない）。tried に探した場所を積む。
fs::path ResolveSketch(const std::string& in, const fs::path& repoRoot, std::vector<std::string>& tried)
{
    std::vector<fs::path> cands;
    const fs::path given = PathResolver::Utf8ToWide(in);
    if (given.is_absolute()) cands.push_back(given);
    else
    {
        cands.push_back(fs::path(PathResolver::Utf8ToWide(PathResolver::BaseDir())) / given);
        if (!repoRoot.empty()) cands.push_back(repoRoot / "hardware" / "firmware" / given);
    }
    for (const fs::path& c : cands)
    {
        std::error_code ec;
        tried.push_back(PathResolver::WideToUtf8(c.wstring()));
        if (fs::is_directory(c, ec))
        {
            // フォルダ内に <フォルダ名>.ino がある（arduino-cli の規約）
            if (fs::is_regular_file(c / (c.filename().wstring() + L".ino"), ec)) return c;
            // 末尾スラッシュ付きで filename が空のとき
            if (c.filename().empty() && fs::is_regular_file(c.parent_path() / (c.parent_path().filename().wstring() + L".ino"), ec))
                return c.parent_path();
        }
        else if (fs::is_regular_file(c, ec) && Lower(PathResolver::WideToUtf8(c.extension().wstring())) == ".ino")
        {
            // フォルダ名と .ino 名が合っていればフォルダを、そうでなければ .ino をそのまま渡す
            if (c.parent_path().filename() == c.stem()) return c.parent_path();
            return c;
        }
    }
    return {};
}

// 推定ボード（hello の board / VID/PID の推定）→ FQBN。分からなければ空。
std::string FqbnFromBoard(const std::string& board)
{
    const std::string b = Lower(board);
    if (b.find("esp32") != std::string::npos || b.find("espressif") != std::string::npos) return "esp32:esp32:esp32";
    // Nano Every（ATmega4809）は megaavr コア。汎用の "nano" / "arduino" → uno の規則より先に見る
    if (b.find("every") != std::string::npos || b.find("nona4809") != std::string::npos || b.find("megaavr") != std::string::npos)
        return "arduino:megaavr:nona4809";
    if (b.find("uno") != std::string::npos || b.find("arduino") != std::string::npos || b.find("avr") != std::string::npos ||
        b.find("nano") != std::string::npos)
        return "arduino:avr:uno";
    return {};
}

// ---- JSON ------------------------------------------------------------------

json DeviceJson(const hw::HwDeviceInfo& d)
{
    json chs = json::array();
    for (const hw::HwChannelInfo& c : d.channels)
    {
        json cj{{"name", c.name}, {"dir", c.isOutput ? "out" : "in"}, {"type", hw::HwChTypeName(c.type)},
                {"hasRange", c.hasRange}, {"min", c.min}, {"max", c.max}};
        if (c.isOutput)
        {
            cj["outValue"] = c.outValue;
            if (c.dangerous) { cj["dangerous"] = true; cj["maxValue"] = c.maxValue; }
        }
        else
        {
            cj["raw"] = c.raw;
            cj["value"] = c.value;
            cj["hasValue"] = c.hasValue;
            if (c.simulated) cj["simulated"] = true;
        }
        chs.push_back(std::move(cj));
    }
    json j{{"name", d.name},
           {"status", hw::HwStatusName(d.status)},
           {"connected", d.connected},
           {"virtual", d.isVirtual},
           {"port", d.port},
           {"describe", d.describe},
           {"hello", {{"name", d.helloName}, {"board", d.board}, {"fw", d.fw}, {"proto", d.proto}}},
           {"channels", std::move(chs)},
           {"stats", {{"rxLines", d.stats.rxLines}, {"rxBytes", d.stats.rxBytes}, {"txLines", d.stats.txLines},
                      {"rxRateHz", d.stats.rxRateHz}, {"rttMs", d.stats.rttMs}, {"errors", d.stats.errors},
                      {"badLines", d.stats.badLines}, {"overflow", d.stats.overflow}}},
           {"lastRxAgeMs", d.stats.lastRxAgeMs},
           {"calibrating", d.calibrating},
           {"sampling", d.sampling}};
    if (d.userDisabled) j["userDisabled"] = true;
    if (!d.lastError.empty()) j["lastError"] = d.lastError;
    return j;
}

hw::HwChannelDecl ParseChannelDecl(const json& cj)
{
    hw::HwChannelDecl d;
    d.name = cj.value("name", std::string());
    d.isOutput = cj.value("dir", std::string("in")) == "out";
    const std::string t = cj.value("type", std::string("float"));
    d.type = t == "bool" ? hw::HwChType::Bool : t == "int" ? hw::HwChType::Int : hw::HwChType::Float;
    if (cj.contains("min") && cj["min"].is_number() && cj.contains("max") && cj["max"].is_number())
    {
        d.hasRange = true;
        d.min = cj["min"].get<double>();
        d.max = cj["max"].get<double>();
    }
    d.safe = cj.value("safe", 0.0);
    return d;
}

McpError NoDevice(const std::string& name, hw::HardwareSystem& sys)
{
    std::vector<std::string> names;
    for (const auto& d : sys.ListDevices()) names.push_back(d.name);
    McpError e(McpErr::NotFound, "ハードウェアのデバイス '" + name + "' がありません",
               "hw_status で名前を確認する。設定に無いデバイスは hw_connect {device, port} で足せる、"
               "実機なしなら hw_simulate {device, channels} で仮想デバイスを作れる", names);
    e.name = "E_HW_NO_DEVICE";
    e.didYouMean = McpSuggest(name, names, 3);
    e.fix.push_back(MakeMcpFix("hw_status", json::object(), "デバイスの一覧と状態を見る"));
    return e;
}

hw::HwDeviceInfo RequireDevice(hw::HardwareSystem& sys, const std::string& name)
{
    hw::HwDeviceInfo info;
    if (!sys.GetDeviceInfo(name, info)) throw NoDevice(name, sys);
    return info;
}

std::string StrParam(const json& p, const char* key)
{
    auto it = p.find(key);
    return (it != p.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// hw_write / hw_write_dangerous の本体
json DoWrite(hw::HardwareSystem& sys, const json& params, bool allowDangerous)
{
    const std::string dev = StrParam(params, "device");
    const hw::HwDeviceInfo info = RequireDevice(sys, dev);
    const json& vals = params["values"];
    if (!vals.is_object() || vals.empty())
        throw McpError(McpErr::InvalidParam, "values が空です", "values:{\"led\":0.5} のようにチャンネル名と 0..1 の値を渡す");

    auto findCh = [&](const std::string& ch) -> const hw::HwChannelInfo* {
        for (const auto& c : info.channels) if (c.name == ch) return &c;
        return nullptr;
    };

    if (!allowDangerous)
    {
        std::vector<std::string> bad;
        for (auto it = vals.begin(); it != vals.end(); ++it)
        {
            const hw::HwChannelInfo* c = findCh(it.key());
            if (c && c->dangerous) bad.push_back(it.key());
        }
        if (!bad.empty())
        {
            std::string list;
            for (const auto& b : bad) list += (list.empty() ? "" : ", ") + b;
            McpError e(McpErr::Guarded, "dangerous なチャンネルへの書き込みは hw_write では出来ません: " + list,
                       "hw_write_dangerous（確認が要る）で書く。値は config の maxValue で切られる");
            e.name = "E_HW_DANGEROUS";
            e.cause = "hardware.json で dangerous:true にされた出力（ペルチェ・ヒーター・モーターなど）が含まれている";
            e.fix.push_back(MakeMcpFix("hw_write_dangerous", params, "dangerous な出力は確認付きの hw_write_dangerous で書く"));
            e.details = {{"channels", bad}};
            throw e;
        }
    }

    json written = json::object();
    json rejected = json::array();
    for (auto it = vals.begin(); it != vals.end(); ++it)
    {
        if (!it->is_number())
        {
            rejected.push_back({{"channel", it.key()}, {"reason", "数値ではない"}});
            continue;
        }
        const double v = it->get<double>();
        const hw::HwChannelInfo* c = findCh(it.key());
        if (!c) { rejected.push_back({{"channel", it.key()}, {"reason", info.connected ? "宣言されていないチャンネル" : "未接続でチャンネルが未宣言"}}); continue; }
        if (!c->isOutput) { rejected.push_back({{"channel", it.key()}, {"reason", "入力チャンネルには書けない"}}); continue; }
        if (!sys.Set(dev, it.key(), v)) { rejected.push_back({{"channel", it.key()}, {"reason", "デバイスが Ready でない"}}); continue; }
        double eff = std::clamp(v, 0.0, 1.0);
        if (c->dangerous) eff = (std::min)(eff, c->maxValue);
        written[it.key()] = eff;
    }
    sys.FlushOutputs();   // フレームを待たずに確定（IO スレッドが次のループで送る）
    if (written.empty())
    {
        McpError e(McpErr::InvalidParam, "1 本も書けませんでした: " + rejected.dump(),
                   info.connected ? "hw_status の channels で out のチャンネル名を確認する"
                                  : "デバイスが未接続。hw_connect してから書く");
        e.name = "E_HW_WRITE_FAILED";
        e.details = {{"rejected", rejected}};
        throw e;
    }
    return {{"device", dev}, {"written", written}, {"rejected", rejected},
            {"note", "書いた値は正規化値（dangerous は maxValue で切り済み）。Play を止めると安全値へ戻る"}};
}

// ---- フラッシュのワーカー ----------------------------------------------------

struct FlashJob
{
    std::shared_ptr<void> stateHolder;   // HwAppState の所有（ワーカーが生きている間つなぎ止める）
    hw::HardwareSystem*   sys = nullptr;
    std::string           device;        // 空 = ポートだけ指定（接続管理なし）
    std::string           port;
    std::string           reconnectPort; // Connect に渡すポート（match で探せるなら空）
    std::vector<std::string> pausedOthers;   // 書き込み中だけ止める他のデバイス（未接続で、hello 探しにポートを開きに来るもの）
    std::string           cmdLine;       // 引数込みのコマンドライン（UTF-8）
    std::string           workDir;
    std::string           cli;           // arduino-cli のパス（コアの目印に書く用）
    std::string           preCmdLine;    // コンパイルの前に走らせる core install（空 = 要らない）
    std::string           preCore;       // その core（"esp32:esp32" / "arduino:megaavr" など vendor:arch なら何でも）
    std::string           preIndexCmdLine; // core install の前の core update-index（索引が無いと入らない。失敗しても進む）
    std::string           preLibCmdLine; // コンパイルの前に走らせる lib install（空 = 要らない。Servo など）
    std::string           preLib;        // そのライブラリ名
};

// 1 本のコマンドを隠しプロセスで走らせて出力を st.log に積む。終了コードを返す。起動できなければ -1 と error。
// st.process / st.job にキャンセル用のハンドルを預ける（StopFlash が止める）。
int RunLoggedProc(HwAppState& st, const std::string& cmdLine, const std::string& workDir, std::string& error)
{
    int exitCode = -1;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) { error = "パイプを作れませんでした"; return -1; }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = ToWide(cmdLine);
    const std::wstring wd = ToWide(workDir);
    // ★窓を出さず（CREATE_NO_WINDOW）、操作に響かないよう優先度を下げる（BELOW_NORMAL。子の esptool も継承する）。
    //   CREATE_SUSPENDED で Job に入れてから走らせる（子プロセスを取りこぼさない）。
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                     nullptr, wd.empty() ? nullptr : wd.c_str(), &si, &pi);
    ::CloseHandle(wr);
    if (!ok)
    {
        error = "arduino-cli を起動できませんでした（Win32 エラー " + std::to_string(::GetLastError()) + "）";
        ::CloseHandle(rd);
        return -1;
    }
    HANDLE job2 = ::CreateJobObjectW(nullptr, nullptr);
    if (job2)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job2, JobObjectExtendedLimitInformation, &li, sizeof(li));
        ::AssignProcessToJobObject(job2, pi.hProcess);
    }
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.process = pi.hProcess;
        st.job = job2;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    char buf[2048];
    DWORD got = 0;
    while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        PushLogText(st, buf, got);
    PushLogText(st, "\n", 1);
    ::CloseHandle(rd);

    DWORD ec = 1;
    ::WaitForSingleObject(pi.hProcess, 10000);
    ::GetExitCodeProcess(pi.hProcess, &ec);
    exitCode = static_cast<int>(ec);
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.process = nullptr;
        st.job = nullptr;
    }
    if (job2) ::CloseHandle(job2);   // KILL_ON_JOB_CLOSE: 残った子があれば終わらせる
    ::CloseHandle(pi.hProcess);
    return exitCode;
}

// RunFlash の途中経過（例外で途中終了しても、ここまでの結果と「つなぎ直し済みか」を外で見られるように）
struct FlashOutcome
{
    std::string error;
    int         exitCode = -1;
    bool        reconnected = false;
    bool        restored = false;   // 3) のつなぎ直し（Connect）まで済んだ
};

void RunFlashSteps(FlashJob& job, HwAppState& st, FlashOutcome& out)
{
    std::string& error = out.error;
    int& exitCode = out.exitCode;

    // 1) ポートを放す（IO スレッドが閉じるまで待つ）。
    //    ★未接続の他デバイス（match に VID が無いものなど）は、hello 探しに全ポートを順に開きに来る。
    //      書き込み中の COM を横取りされると esptool が「port is busy」で失敗するので、書き込みの間だけ止める。
    if (job.sys && (!job.device.empty() || !job.pausedOthers.empty()))
    {
        std::vector<std::string> names = job.pausedOthers;
        if (!job.device.empty()) names.push_back(job.device);
        for (const std::string& n : names) job.sys->Disconnect(n);
        for (int i = 0; i < 80 && !st.cancel.load(); ++i)
        {
            bool allClosed = true;
            for (const std::string& n : names)
            {
                hw::HwDeviceInfo info;
                if (!job.sys->GetDeviceInfo(n, info)) continue;
                if (info.status != hw::HwStatus::Disconnected && info.status != hw::HwStatus::Error) allClosed = false;
            }
            if (allClosed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // ドライバがポートを手放す余裕
    }

    // 2) arduino-cli を起動して出力を取り込む（必要ならコンパイルの前にコア・ライブラリを入れる）
    if (!st.cancel.load() && !job.preCmdLine.empty())
    {
        SetState(st, "installing_core");   // ★"compiling" のままだと core install の esptool の文字で "uploading" に化ける
        const std::string msg = "ボードのコア " + job.preCore + " を入れます（初回だけ数分）\n";
        PushLogText(st, msg.data(), msg.size());
        std::string perr;
        if (!job.preIndexCmdLine.empty() && !st.cancel.load()) RunLoggedProc(st, job.preIndexCmdLine, "", perr);   // 失敗は無視（下の install が答えを出す）
        perr.clear();
        const int pec = RunLoggedProc(st, job.preCmdLine, "", perr);
        if (!perr.empty()) error = perr;
        else if (pec != 0 && !st.cancel.load()) error = "ボードのコア " + job.preCore + " を入れられませんでした（ログ末尾を参照）";
        else if (pec == 0) hw::AddCoreToMarker(job.preCore, job.cli);
    }
    if (!st.cancel.load() && error.empty() && !job.preLibCmdLine.empty())
    {
        SetState(st, "installing_core");
        const std::string msg = "ライブラリ " + job.preLib + " を入れます\n";
        PushLogText(st, msg.data(), msg.size());
        std::string perr;
        const int pec = RunLoggedProc(st, job.preLibCmdLine, "", perr);
        if (!perr.empty()) error = perr;
        else if (pec != 0 && !st.cancel.load()) error = "ライブラリ " + job.preLib + " を入れられませんでした（ログ末尾を参照）";
        else if (pec == 0) hw::AddLibToMarker(job.preLib, job.cli);
    }
    if (error.empty() && (!job.preCmdLine.empty() || !job.preLibCmdLine.empty())) SetState(st, "compiling");
    if (!st.cancel.load() && error.empty())
    {
        std::string perr;
        const int ec = RunLoggedProc(st, job.cmdLine, job.workDir, perr);
        if (!perr.empty()) error = perr;
        else exitCode = ec;
    }
    if (st.cancel.load() && error.empty()) error = "中断されました";

    // 3) つなぎ直す（失敗しても必ず Connect を出す＝自動再接続を元に戻す）
    bool& reconnected = out.reconnected;
    if (!job.device.empty() && job.sys)
    {
        const bool cancelled = st.cancel.load();
        if (!cancelled)
        {
            SetState(st, "reconnecting");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));   // ボードが再起動してポートが戻るのを待つ
        }
        job.sys->Connect(job.device, job.reconnectPort);
        if (!cancelled)
        {
            const double until = SteadySec() + 25.0;
            while (SteadySec() < until)
            {
                hw::HwDeviceInfo info;
                if (!job.sys->GetDeviceInfo(job.device, info)) break;
                if (info.connected) { reconnected = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    // 止めていた他のデバイスの自動再接続を戻す（match で探す。ポートは渡さない）
    if (job.sys) for (const std::string& n : job.pausedOthers) job.sys->Connect(n);
    out.restored = true;
}

void RunFlash(FlashJob job)
{
    HwAppState& st = *static_cast<HwAppState*>(job.stateHolder.get());
    FlashOutcome out;
    // ★背景スレッドの最上位。例外が外へ出ると std::terminate でエンジンごと落ちるので、全部ここで受ける。
    try
    {
        RunFlashSteps(job, st, out);
    }
    catch (const std::exception& e)
    {
        out.error = "内部エラー: " + SanitizeUtf8(e.what());
    }
    catch (...)
    {
        out.error = "内部エラー: 不明な例外";
    }
    // 途中で例外になってつなぎ直しが済んでいなければ、できる範囲で戻す（止めたままにしない）
    if (!out.restored && job.sys)
    {
        try
        {
            if (!job.device.empty()) job.sys->Connect(job.device, job.reconnectPort);
            for (const std::string& n : job.pausedOthers) job.sys->Connect(n);
        }
        catch (...) {}
    }

    try
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.exitCode = out.exitCode;
        st.reconnected = out.reconnected;
        st.endedSec = SteadySec();
        if (out.error.empty() && out.exitCode != 0) out.error = "arduino-cli が終了コード " + std::to_string(out.exitCode) + " で終わりました（ログ末尾を参照）";
        if (out.error.empty() && !job.device.empty() && !out.reconnected)
            out.error = "書き込みは終わったがデバイスに再接続できませんでした（hw_status を確認）";
        st.error = out.error;
        st.state = out.error.empty() ? "done" : "failed";
        st.running = false;
    }
    catch (...)
    {
        // 状態文字列の確保に失敗しても、「実行中」のまま固まらないことだけは守る
        std::lock_guard<std::mutex> lk(st.m);
        st.state = "failed";
        st.running = false;
    }
}

} // namespace

// ===========================================================================
// Application のメンバ
// ===========================================================================

void Application::InitHardware()
{
    hw::HwConfig cfg;
    std::string err;
    bool found = false;
    if (!hw::LoadHwConfigFromAssets(cfg, err, &found))
    {
        // 壊れた hardware.json でゲームが起動しなくなる方が困る。空設定で進む（MCP の hw_connect / 仮想は使える）。
        Logger::Error("[hw] hardware.json を読めませんでした（ハードウェア無しで進みます）: {}", err);
        cfg = hw::HwConfig{};
    }
    else if (found)
        Logger::Info("[hw] hardware.json を読みました（デバイス {} 件）", cfg.devices.size());
    ReinitHardware(cfg);
}

void Application::ReinitHardware(const hw::HwConfig& cfg)
{
    if (!m_hardware) m_hardware = std::make_unique<hw::HardwareSystem>();

    // 走っているフラッシュと保留中の hw_read は、デバイス表が作り直される前に片付ける
    HwAppState& st = St(m_hwFlash);
    StopFlash(st);
    for (const HwReadPending& r : m_hwReads)
        FailMcp(m_mcpBridge.get(), r.reply, McpErr::Cancelled, "ハードウェアの設定が作り直されたため hw_read を中断しました");
    m_hwReads.clear();

    // 設定に無い仮想デバイス（hw_simulate で作ったもの）は作り直しても残す
    struct Virt { std::string name; std::vector<hw::HwChannelDecl> channels; };
    std::vector<Virt> keep;
    for (const hw::HwDeviceInfo& d : m_hardware->ListDevices())
        if (d.isVirtual && !cfg.FindDevice(d.name))
            if (auto lb = m_hardware->GetVirtualDevice(d.name)) keep.push_back({d.name, lb->Channels()});

    m_hardware->Shutdown();
    m_hardware->Initialize(cfg);
    st.arduinoCli = cfg.arduinoCli;
    for (const Virt& v : keep) m_hardware->AddVirtualDevice(v.name, v.channels);

    ApplyHardwareActionBindings();
}

void Application::ApplyHardwareActionBindings()
{
    // 前のバインドを外してから付け直す（actions は Play のたびに lua が bind し直すが、擬似キーは Application が持つ）
    m_actionMap.RemoveKeysFrom(kHwKeyBase);
    if (m_hardware)
    {
        const auto binds = m_hardware->GetActionBindings();
        for (size_t i = 0; i < binds.size(); ++i)
            m_actionMap.Bind(binds[i].action, kHwKeyBase + static_cast<int>(i));
        if (!binds.empty()) Logger::Info("[hw] actions へハードのチャンネルを {} 件割り当てました", binds.size());
    }
    if (m_scriptEngine) m_scriptEngine->SetHardware(m_hardware.get());   // 擬似キー表を取り直す
}

void Application::ServiceHardwareReads()
{
    if (m_hwReads.empty() || !m_hardware) return;
    const double now = SteadySec();
    for (size_t i = 0; i < m_hwReads.size();)
    {
        HwReadPending& r = m_hwReads[i];
        if (now < r.endSec) { ++i; continue; }
        const std::map<std::string, hw::HwSampleStats> stats = m_hardware->EndSampling(r.device);
        hw::HwDeviceInfo info;
        m_hardware->GetDeviceInfo(r.device, info);
        json chs = json::object();
        for (const auto& [name, s] : stats)
            chs[name] = {{"count", s.count}, {"min", s.min}, {"max", s.max}, {"mean", s.mean}, {"stddev", s.stddev}};
        json out{{"device", r.device}, {"channels", chs}, {"connected", info.connected}};
        if (stats.empty()) out["note"] = "サンプルが 1 つも無かった（入力チャンネルが無い / 値が流れていない / 途中で切断）";
        if (!info.connected) out["warning"] = "集計の途中で切断された";
        out["stats"] = {{"rttMs", info.stats.rttMs}, {"rxRateHz", info.stats.rxRateHz}, {"errors", info.stats.errors}};
        CompleteMcp(m_mcpBridge.get(), r.reply, std::move(out));
        m_hwReads.erase(m_hwReads.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void Application::ShutdownHardware()
{
    if (m_hwFlash) StopFlash(St(m_hwFlash));
    if (m_hwToolchain) m_hwToolchain->Shutdown();   // 導入中の arduino-cli / ダウンロードを止めて回収
    for (const HwReadPending& r : m_hwReads)
        FailMcp(m_mcpBridge.get(), r.reply, McpErr::Cancelled, "エンジンの終了で hw_read を中断しました");
    m_hwReads.clear();
    if (m_hardware) m_hardware->Shutdown();   // 全出力を !safe してから閉じる
}

hw::HwToolchain& Application::HwToolchainRef()
{
    if (!m_hwToolchain) m_hwToolchain = std::make_shared<hw::HwToolchain>();
    return *m_hwToolchain;
}

void Application::ServiceHardwareUi()
{
    if (!m_editorCtx || m_isGameMode) return;
    // 起動時に積んだ知らせ（サンプルを置いた）は、エディタが出てから 1 回だけ
    if (!m_startupToast.empty() && !m_showLauncher)
    {
        m_editorCtx->Notify(ui::ToastKind::Success, m_startupToast, 7.0f);
        m_startupToast.clear();
    }
    if (m_showLauncher || !m_hardware) return;

    HwUiRt& rt = Rt(m_hwUiRt);
    HwAppState& st = St(m_hwFlash);
    hwui::UiState& hu = m_editorCtx->hwUi;
    hw::HwToolchain& tc = HwToolchainRef();
    const bool automation = m_headless || m_bgOptions.Active() || m_virtualInputRequested;
    const double now = SteadySec();
    const std::string base = PathResolver::BaseDir();

    // ---- 窓からの要求 ----
    if (hu.requestToolchainInstall)
    {
        hu.requestToolchainInstall = false;
        if (tc.Start(st.arduinoCli) && !automation) rt.notifyOnDone = true;
    }
    if (hu.requestFlash)
    {
        hu.requestFlash = false;
        HwFlashRequest rq;
        rq.sketch = hu.reqSketch;
        rq.device = hu.reqDevice;
        rq.port = hu.reqPort;
        rq.fqbn = hu.reqFqbn;
        std::string err;
        if (StartHwFlash(rq, err, nullptr))
        {
            hu.requestError.clear();
            hu.promptMessage.clear();
        }
        else
        {
            hu.requestError = err;
            m_editorCtx->Notify(ui::ToastKind::Error, "書き込みを始められませんでした: " + err, 8.0f);
        }
    }

    const bool open = m_editorCtx->showHardware;
    const bool tick = now - rt.lastTick >= 1.0;
    if (!open && !tick) return;
    if (tick)
    {
        rt.lastTick = now;
        rt.hasSerial = false;
        for (const hw::HwDeviceConfig& d : m_hardware->Config().devices)
            if (d.transport == "serial") { rt.hasSerial = true; break; }
    }

    const hw::HwToolchain::Snapshot tcs = tc.Get(st.arduinoCli);

    // ---- 書き込み道具の自動導入（プロジェクトを開いたら 1 回。自動化中は勝手に落とさない）----
    if (tick && !automation && rt.hasSerial && !tcs.ready && !tcs.running && rt.autoStartBase != base)
    {
        rt.autoStartBase = base;
        if (tc.Start(st.arduinoCli))
        {
            rt.notifyOnDone = true;
            m_editorCtx->Notify(ui::ToastKind::Info, "Arduino の書き込み道具を準備しています（初回だけ数分）", 6.0f);
        }
    }
    if (rt.notifyOnDone && !tcs.running && (tcs.state == hw::HwToolchain::State::Ready || tcs.state == hw::HwToolchain::State::Failed))
    {
        rt.notifyOnDone = false;
        if (tcs.state == hw::HwToolchain::State::Ready)
            m_editorCtx->Notify(ui::ToastKind::Success, "Arduino の書き込み道具の準備ができました", 5.0f);
        else
            m_editorCtx->Notify(ui::ToastKind::Error, "Arduino の書き込み道具を準備できませんでした: " + tcs.error + "（ハードウェア窓の「やり直す」から再試行できます）", 10.0f);
    }

    // ---- デバイス一覧 ----
    const std::vector<hw::HwDeviceInfo> devs = m_hardware->ListDevices();

    // ---- ボード検出の案内（エディタのみ。書き込みは自動でしない）----
    bool anyReady = false;
    for (const hw::HwDeviceInfo& d : devs) if (!d.isVirtual && d.status == hw::HwStatus::Ready) { anyReady = true; break; }
    const bool wantPrompt = tick && !automation && rt.hasSerial && tcs.ready && !anyReady;

    // ---- COM ポート・スケッチ（1Hz。窓が開いているか、案内の判定に要るときだけ）----
    if ((open || wantPrompt) && now - rt.lastPorts >= 1.0)
    {
        rt.lastPorts = now;
        rt.ports.clear();
        for (const hw::HwPortInfo& p : hw::EnumerateComPorts())
        {
            hwui::PortRow r;
            r.port = p.portName; r.friendlyName = p.friendlyName; r.vid = p.vid; r.pid = p.pid; r.guessedBoard = p.guessedBoard;
            r.knownVid = KnownBoardVid(p.vid);
            for (const hw::HwDeviceInfo& d : devs)
                if (!d.port.empty() && d.port == p.portName && d.status != hw::HwStatus::Disconnected) { r.usedBy = d.name; break; }
            rt.ports.push_back(std::move(r));
        }
    }
    if (open && (now - rt.lastSketches >= 1.0 || rt.sketchesBase != base))
    {
        rt.lastSketches = now;
        rt.sketchesBase = base;
        rt.sketches.clear();
        std::error_code ec;
        const fs::path fw = fs::path(PathResolver::Utf8ToWide(base)) / L"firmware";
        if (fs::is_directory(fw, ec))
            for (const fs::directory_entry& e : fs::directory_iterator(fw, ec))
            {
                if (!e.is_directory(ec)) continue;
                const fs::path name = e.path().filename();
                if (fs::is_regular_file(e.path() / (name.wstring() + L".ino"), ec)) rt.sketches.push_back(PathResolver::WideToUtf8(name.wstring()));
            }
        std::sort(rt.sketches.begin(), rt.sketches.end());
    }

    if (wantPrompt)
    {
        std::set<std::string> present;
        for (const hwui::PortRow& p : rt.ports)
        {
            if (!p.knownVid) continue;
            present.insert(p.port);
            const auto it = rt.portSeen.try_emplace(p.port, now).first;
            if (now - it->second >= 5.0 && rt.prompted.insert(p.port).second)
            {
                m_editorCtx->showHardware = true;
                hu.promptMessage = "ボードが見つかりました。まだ書き込まれていないようです。";
                hu.promptPort = p.port;
                hu.selPort = p.port;
                hu.boardTouched = false;
                Logger::Info("[hw] ボード {} を検出（hello なし）。ハードウェア窓で書き込みを案内します", p.port);
            }
        }
        for (auto it = rt.portSeen.begin(); it != rt.portSeen.end();)
            it = present.count(it->first) ? std::next(it) : rt.portSeen.erase(it);
    }
    else if (tick)
    {
        rt.portSeen.clear();
        if (anyReady) { hu.promptMessage.clear(); hu.promptPort.clear(); }
    }

    if (!m_editorCtx->showHardware) return;

    // ---- スナップショットを窓へ ----
    hu.projectOpen = !base.empty();
    hu.devices.clear();
    const hw::HwConfig cfgNow = m_hardware->Config();
    for (const hw::HwDeviceInfo& d : devs)
    {
        hwui::DeviceRow r;
        r.name = d.name; r.status = hw::HwStatusName(d.status); r.port = d.port;
        r.board = !d.board.empty() ? d.board : d.describe;
        r.lastError = d.lastError;
        r.isVirtual = d.isVirtual;
        const hw::HwDeviceConfig* dc = cfgNow.FindDevice(d.name);
        r.serial = !d.isVirtual && (!dc || dc->transport == "serial");
        hu.devices.push_back(std::move(r));
    }
    hu.ports = rt.ports;
    hu.sketches = rt.sketches;
    hu.tcState = hw::HwToolchain::StateName(tcs.state);
    hu.tcProgress = tcs.progress;
    hu.tcReady = tcs.ready;
    hu.tcRunning = tcs.running;
    hu.tcCli = tcs.cli;
    hu.tcError = tcs.error;
    hu.tcLog = tcs.log;
    {
        std::lock_guard<std::mutex> lk(st.m);
        hu.flashState = st.state;
        hu.flashRunning = st.running;
        const double el = st.running ? now - st.startedSec : (st.endedSec > 0.0 ? st.endedSec - st.startedSec : 0.0);
        hu.flashElapsedSec = static_cast<float>(el);
        hu.flashError = st.error;
        hu.flashDevice = st.device;
        hu.flashPort = st.port;
        hu.flashLog.clear();
        const size_t from = st.log.size() > 60 ? st.log.size() - 60 : 0;
        for (size_t i = from; i < st.log.size(); ++i) hu.flashLog.push_back(st.log[i]);
    }
}

// ===========================================================================
// hw_flash の本体（MCP とエディタのハードウェア窓の両方から呼ぶ）
// ===========================================================================
void Application::StartHwFlashImpl(const HwFlashRequest& rq, nlohmann::json* outInfo)
{
    if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
    HwAppState& st = St(m_hwFlash);
    {
        std::lock_guard<std::mutex> lk(st.m);
        if (st.running)
            throw McpError(McpErr::Busy, "別の hw_flash が実行中です（同時に 1 本だけ）", "hw_flash_status で終わるのを待つ");
    }
    if (st.worker.joinable()) st.worker.join();   // 前回の終了済みスレッドを回収

    const std::string sketchIn = rq.sketch;
    std::string dev = rq.device;
    std::string port = rq.port;
    std::string fqbn = rq.fqbn;
    if (sketchIn.empty()) throw McpError(McpErr::InvalidParam, "sketch が空です", ".ino かそのフォルダを渡す");
    if (dev.empty() && port.empty()) throw McpError(McpErr::InvalidParam, "device か port のどちらかが要ります", "device:\"generic\" か port:\"COM8\"");

    // 対象（デバイス / ポート）
    hw::HwDeviceInfo info;
    bool haveInfo = false;
    if (!dev.empty())
    {
        info = RequireDevice(*m_hardware, dev);
        haveInfo = true;
        if (info.isVirtual) throw McpError(McpErr::InvalidParam, "仮想デバイスには書き込めません", "実機のデバイスを指定する");
        if (port.empty()) port = info.port;
        if (port.empty())
            throw McpError(McpErr::InvalidParam, "デバイス '" + dev + "' のポートが分かりません（未接続）",
                           "port:\"COM8\" を渡す（hw_list_ports で確認）");
    }
    else
    {
        for (const hw::HwDeviceInfo& d : m_hardware->ListDevices())
            if (d.port == port && d.status != hw::HwStatus::Disconnected) { dev = d.name; info = d; haveInfo = true; break; }
    }

    // 書き込み道具（arduino-cli + コア）の自動導入中は待たせる
    hw::HwToolchain& tc = HwToolchainRef();
    if (tc.Running())
    {
        McpError e(McpErr::Busy, "書き込み道具を準備中です", "hw_setup {action:\"status\"} で終わるのを待つ（初回だけ数分）");
        e.name = "E_HW_TOOLCHAIN_BUSY";
        throw e;
    }

    // arduino-cli / スケッチ / ライブラリ
    std::vector<std::string> triedCli;
    const std::string cli = hw::FindArduinoCli(st.arduinoCli, triedCli);
    if (cli.empty())
    {
        // エディタ / 配布物では自動で入れる。待ってから撃ち直してもらう
        const bool started = !m_isGameMode && tc.Start(st.arduinoCli);
        McpError e(McpErr::NotFound, started ? "arduino-cli が見つかりません。書き込み道具の自動準備を始めました（初回だけ数分）" : "arduino-cli が見つかりません",
                   started ? "hw_setup {action:\"status\"} で ready になるのを待ってから撃ち直す"
                           : "hw_setup {action:\"install_toolchain\"} で入れるか、hardware.json の arduinoCli に絶対パスを書く（hw_config_set）");
        e.name = "E_HW_NO_ARDUINO_CLI";
        e.details = {{"tried", triedCli}};
        throw e;
    }
    const fs::path repo = FindRepoRoot();
    std::vector<std::string> triedSk;
    const fs::path sketch = ResolveSketch(sketchIn, repo, triedSk);
    if (sketch.empty())
    {
        McpError e(McpErr::NotFound, "スケッチ '" + sketchIn + "' が見つかりません", "絶対パスで渡すか、プロジェクト / <repo>/hardware/firmware/ の下に置く");
        e.name = "E_HW_NO_SKETCH";
        e.details = {{"tried", triedSk}};
        throw e;
    }
    std::vector<std::string> libs = rq.libraries;
    if (libs.empty())
    {
        if (repo.empty())
            throw McpError(McpErr::InvalidParam, "UnoLink ライブラリの場所が分かりません（配布物にはリポジトリが無い）",
                           "libraries に UnoLink フォルダの絶対パスを渡す");
        libs.push_back(PathResolver::WideToUtf8((repo / "hardware" / "firmware" / "UnoLink").wstring()));
    }

    // fqbn
    if (fqbn.empty())
    {
        if (haveInfo) fqbn = FqbnFromBoard(info.board);
        if (fqbn.empty())
            for (const hw::HwPortInfo& p : hw::EnumerateComPorts())
                if (p.portName == port) { fqbn = FqbnFromBoard(p.guessedBoard); break; }
        if (fqbn.empty())
        {
            McpError e(McpErr::InvalidParam, "ボードを推定できませんでした", "fqbn を渡す（例 esp32:esp32:esp32 / arduino:avr:uno / arduino:megaavr:nona4809）");
            e.name = "E_HW_FQBN_REQUIRED";
            e.validValues = {"esp32:esp32:esp32", "arduino:avr:uno", "arduino:avr:nano", "arduino:megaavr:nona4809"};
            throw e;
        }
    }

    const std::string* const quoted[] = {&cli, &port, &fqbn};
    for (const std::string* s : quoted)
        if (s->find('"') != std::string::npos)
            throw McpError(McpErr::InvalidParam, "引数に引用符は使えません", "パス・ポート・fqbn から \" を除く");

    FlashJob job;
    job.stateHolder = m_hwFlash;
    job.sys = m_hardware.get();
    job.device = dev;
    job.port = port;
    for (const hw::HwDeviceInfo& d : m_hardware->ListDevices())
        if (d.name != dev && !d.isVirtual && !d.userDisabled && d.status != hw::HwStatus::Ready)
            job.pausedOthers.push_back(d.name);
    // 設定の match で探せるデバイスは Connect にポートを渡さない（挿し直しで COM 番号が変わっても追従する）
    {
        const hw::HwConfig cfg = m_hardware->Config();
        const hw::HwDeviceConfig* dc = cfg.FindDevice(dev);
        job.reconnectPort = (dc && !dc->match.Empty()) ? std::string() : port;
    }
    std::string cmd = "\"" + cli + "\" compile --upload -p " + port + " --fqbn " + fqbn;
    for (const std::string& l : libs) cmd += " --library \"" + l + "\"";
    cmd += " \"" + PathResolver::WideToUtf8(sketch.wstring()) + "\"";
    job.cmdLine = cmd;
    job.cli = cli;
    {
        // コアが目印に無ければ、コンパイルの前に入れる（vendor:arch ならどのコアでも。入っていれば一瞬で終わる）
        const std::string core = hw::CoreOfFqbn(fqbn);
        if (!core.empty() && !hw::CoreInMarker(core))
        {
            job.preCore = core;
            job.preIndexCmdLine = "\"" + cli + "\" core update-index --additional-urls " + hw::kEspressifIndexUrl;
            job.preCmdLine = "\"" + cli + "\" core install " + core + " --additional-urls " + hw::kEspressifIndexUrl;
        }
        // Servo ライブラリ（Servo を使うスケッチ用。目印に無ければ入れる。入っていれば一瞬で終わる）
        for (const char* lib : hw::kRequiredLibs)
            if (!hw::LibInMarker(lib))
            {
                job.preLib = lib;
                job.preLibCmdLine = "\"" + cli + "\" lib install " + lib;
                break;
            }
    }
    job.workDir = PathResolver::WideToUtf8(sketch.parent_path().wstring());

    {
        std::lock_guard<std::mutex> lk(st.m);
        st.running = true;
        st.state = "compiling";
        st.device = dev; st.port = port; st.fqbn = fqbn;
        st.sketch = PathResolver::WideToUtf8(sketch.wstring());
        st.command = cmd;
        st.error.clear();
        st.exitCode = -1;
        st.reconnected = false;
        st.startedSec = SteadySec();
        st.endedSec = 0.0;
        st.log.clear();
        st.partial.clear();
    }
    st.cancel.store(false);
    Logger::Info("[hw] hw_flash 開始: {}", cmd);
    st.worker = std::thread([job = std::move(job)]() mutable { RunFlash(std::move(job)); });

    if (outInfo)
        *outInfo = {{"started", true}, {"device", dev}, {"port", port}, {"fqbn", fqbn},
                    {"sketch", PathResolver::WideToUtf8(sketch.wstring())}, {"libraries", libs}, {"command", cmd},
                    {"note", "非同期。hw_flash_status で compiling → uploading → reconnecting → done を見る（ESP32 は 30〜60 秒。コアが未導入なら先に installing_core）"}};
}

bool Application::StartHwFlash(const HwFlashRequest& rq, std::string& err, nlohmann::json* outInfo)
{
    try
    {
        StartHwFlashImpl(rq, outInfo);
        return true;
    }
    catch (const McpError& e)
    {
        err = e.what();
        if (!e.hint.empty()) err += "（" + e.hint + "）";
        return false;
    }
    catch (const std::exception& e)
    {
        // std::filesystem_error など（日本語 Windows では what() が ANSI なので UTF-8 に整えてから返す）
        err = "内部エラー: " + SanitizeUtf8(e.what());
        return false;
    }
    catch (...)
    {
        err = "内部エラー: 不明な例外";
        return false;
    }
}

// ===========================================================================
// MCP method
// ===========================================================================

void Application::RegisterMcpHardwareMethods()
{
    // ---- hw_list_ports ----
    {
        McpMeta m;
        m.summary  = "PC につながっている COM ポートの一覧（VID/PID・推定ボード名・どのハードウェアデバイスが使用中か）。"
                     "Arduino / ESP32 を挿して COM 番号と VID/PID を知りたいときに最初に呼ぶ。";
        m.keywords = "hardware ハードウェア arduino esp32 uno シリアル serial com port ポート usb vid pid ch340 一覧 list 実機 接続";
        m.category = "hardware"; m.group = "hardware"; m.target = "ports";
        m.effect = McpEffect::Read; m.mode = "any"; m.timeoutMs = 10000; m.idempotent = true;
        m.next = {{"hw_config_set", "match に vid/pid を書いてデバイスを登録する"}, {"hw_connect", "ポートを指定して接続する"}};
        m.examples = {{"{}", "COM 一覧"}};
        McpDefine("hw_list_ports", McpMeta(m), DX12E_MCP_HANDLER
            {
                json ports = json::array();
                std::vector<hw::HwDeviceInfo> devs;
                if (m_hardware) devs = m_hardware->ListDevices();
                for (const hw::HwPortInfo& p : hw::EnumerateComPorts())
                {
                    json pj{{"port", p.portName}, {"friendlyName", p.friendlyName}, {"vid", p.vid}, {"pid", p.pid},
                            {"guessedBoard", p.guessedBoard}};
                    for (const hw::HwDeviceInfo& d : devs)
                        if (!d.port.empty() && d.port == p.portName && d.status != hw::HwStatus::Disconnected)
                            { pj["usedBy"] = d.name; pj["usedByStatus"] = hw::HwStatusName(d.status); break; }
                    ports.push_back(std::move(pj));
                }
                resp["ok"] = true;
                resp["result"] = {{"ports", ports}, {"count", ports.size()}};
            });
    }

    // ---- hw_status ----
    {
        McpMeta m;
        m.summary  = "ハードウェアデバイスの状態: 接続状態・ポート・hello・チャンネル（向き/型/範囲/生値/正規化値）・統計（受信行数・レート・RTT・エラー・不正行）・最後の受信からの経過。device で 1 台に絞れる。";
        m.keywords = "hardware ハードウェア arduino esp32 状態 status センサー sensor 接続 connected rtt チャンネル channel 値 value led シリアル serial";
        m.category = "hardware"; m.group = "hardware"; m.target = "status";
        m.effect = McpEffect::Read; m.mode = "any"; m.timeoutMs = 10000; m.idempotent = true;
        // ★expose="core" にはしない: Core 面は 40 本ちょうどが上限で（toolSurface / coreSurface テスト）、命名規約
        //   （dx12_<動詞>_<対象>）にも合わない。dx12_tool_search / dx12_call {name:"hw_status"} で使える。
        m.params = {P("device", "string", false, nullptr, nullptr, nullptr, nullptr, "デバイス名。省略で全部")};
        m.next = {{"hw_read", "一定時間の min/max/平均を集計する"}, {"hw_monitor", "生の送受信ログを見る"}};
        m.examples = {{"{}", "全デバイスの状態"}, {"{\"device\":\"generic\"}", "1 台だけ"}};
        McpDefine("hw_status", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                const std::string only = StrParam(params, "device");
                json devs = json::array();
                if (!only.empty()) devs.push_back(DeviceJson(RequireDevice(*m_hardware, only)));
                else for (const auto& d : m_hardware->ListDevices()) devs.push_back(DeviceJson(d));
                resp["ok"] = true;
                resp["result"] = {{"devices", devs}, {"count", devs.size()}, {"initialized", m_hardware->IsInitialized()}};
            });
    }

    // ---- hw_connect / hw_disconnect ----
    {
        McpMeta m;
        m.summary  = "ハードウェアデバイスを接続する（非同期。つながったかは hw_status）。hardware.json に無い名前で呼ぶと、その名前でデバイスを足して接続する"
                     "（port 指定ならそのポート、無ければ @hello の名前＝device で照合）。足した設定はファイルへは書かない（残すなら hw_config_set）。";
        m.keywords = "hardware ハードウェア arduino esp32 接続 connect 再接続 reconnect シリアル serial com port";
        m.category = "hardware"; m.group = "hardware"; m.target = "connect";
        m.effect = McpEffect::Runtime; m.mode = "any"; m.timeoutMs = 10000;
        m.params = {P("device", "string", true, nullptr, nullptr, nullptr, nullptr, "デバイス名（hardware.json の name、または新しく足す名前）"),
                    P("port", "string", false, nullptr, nullptr, nullptr, nullptr, "COM8 など。省略で match（hello / VID / PID）から探す")};
        m.next = {{"hw_status", "つながったか・チャンネルを確認する"}};
        m.examples = {{"{\"device\":\"generic\"}", "設定済みのデバイスを接続"}, {"{\"device\":\"radio\",\"port\":\"COM8\"}", "設定に無ければ足して接続"}};
        McpDefine("hw_connect", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                const std::string dev = StrParam(params, "device");
                const std::string port = StrParam(params, "port");
                if (dev.empty()) throw McpError(McpErr::InvalidParam, "device が空です", "デバイス名を渡す");
                bool added = false;
                hw::HwDeviceInfo info;
                if (!m_hardware->GetDeviceInfo(dev, info))
                {
                    if (St(m_hwFlash).running)
                        throw McpError(McpErr::Busy, "hw_flash の実行中はデバイスを足せません", "hw_flash_status で終わるのを待つ");
                    hw::HwConfig cfg = m_hardware->Config();
                    cfg.arduinoCli = St(m_hwFlash).arduinoCli;
                    hw::HwDeviceConfig dc;
                    dc.name = dev;
                    if (!port.empty()) dc.match.port = port;
                    else dc.match.hello = dev;
                    cfg.devices.push_back(std::move(dc));
                    ReinitHardware(cfg);
                    added = true;
                }
                if (!m_hardware->Connect(dev, port))
                    throw NoDevice(dev, *m_hardware);
                resp["ok"] = true;
                resp["result"] = {{"device", dev}, {"requested", true}, {"addedToConfig", added}, {"persisted", false},
                                  {"note", "接続は非同期（Uno/ESP32 は起動待ちで 1〜3 秒）。hw_status で status が Ready になるのを確認する"
                                           + std::string(added ? "。設定を残すなら hw_config_set" : "")}};
            });

        McpMeta d;
        d.summary  = "ハードウェアデバイスを切断する。全出力を安全値へ戻してポートを放す。hw_connect するまで自動再接続しない。";
        d.keywords = "hardware ハードウェア arduino esp32 切断 disconnect ポート解放 シリアル serial";
        d.category = "hardware"; d.group = "hardware"; d.target = "disconnect";
        d.effect = McpEffect::Runtime; d.mode = "any"; d.timeoutMs = 10000; d.idempotent = true;
        d.params = {P("device", "string", true, nullptr, nullptr, nullptr, nullptr, "デバイス名")};
        d.next = {{"hw_connect", "つなぎ直す"}};
        McpDefine("hw_disconnect", McpMeta(d), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                const std::string dev = StrParam(params, "device");
                if (St(m_hwFlash).running)
                    throw McpError(McpErr::Busy, "hw_flash の実行中は切断できません（書き込みのため既に放してあります）", "hw_flash_status で終わるのを待つ");
                if (!m_hardware->Disconnect(dev)) throw NoDevice(dev, *m_hardware);
                resp["ok"] = true;
                resp["result"] = {{"device", dev}, {"requested", true}, {"note", "自動再接続はしない。つなぎ直すには hw_connect"}};
            });
    }

    // ---- hw_read ----
    {
        McpMeta m;
        m.summary  = "指定ミリ秒だけ入力値を集計して、チャンネルごとの count / min / max / mean / stddev（ジッタ）を返す。"
                     "メインスレッドは止めない（応答をフレーム境界まで遅らせる）。センサーのノイズ・ダイヤルの範囲・ボタンの有無を実機で確かめる。";
        m.keywords = "hardware ハードウェア arduino esp32 読む read センサー sensor 集計 平均 mean ジッタ jitter ノイズ noise ダイヤル dial 計測";
        m.category = "hardware"; m.group = "hardware"; m.target = "read";
        m.effect = McpEffect::Read; m.mode = "any"; m.timeoutMs = 20000; m.deferred = true;
        m.params = {P("device", "string", true, nullptr, nullptr, nullptr, nullptr, "デバイス名（接続済み）"),
                    P("durationMs", "int", false, nullptr, "100", "10000", "1000", "集計する時間（ミリ秒）")};
        m.next = {{"hw_calibrate", "範囲を校正して hardware.json へ保存する"}};
        m.examples = {{"{\"device\":\"generic\",\"durationMs\":1000}", "1 秒集計"}};
        McpDefine("hw_read", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                const std::string dev = StrParam(params, "device");
                const hw::HwDeviceInfo info = RequireDevice(*m_hardware, dev);
                if (!info.connected)
                {
                    McpError e(McpErr::ModeConflict, "デバイス '" + dev + "' は未接続です（" + hw::HwStatusName(info.status) + "）",
                               "hw_connect してから呼ぶ。状態は hw_status");
                    e.name = "E_HW_NOT_CONNECTED";
                    e.fix.push_back(MakeMcpFix("hw_connect", json{{"device", dev}}, "接続する"));
                    throw e;
                }
                for (const HwReadPending& r : m_hwReads)
                    if (r.device == dev)
                        throw McpError(McpErr::Busy, "このデバイスは集計中です", "前の hw_read の応答を待ってから呼ぶ");
                const int ms = std::clamp(params.value("durationMs", 1000), 100, 10000);
                m_hardware->BeginSampling(dev);
                HwReadPending p;
                p.reply = deferred;
                p.device = dev;
                p.endSec = SteadySec() + ms / 1000.0;
                m_hwReads.push_back(std::move(p));
                isDeferred = true;
            });
    }

    // ---- hw_monitor ----
    {
        McpMeta m;
        m.summary  = "デバイスとの生の送受信ログ（rx=デバイス→エンジン / tx=エンジン→デバイス）の末尾。プロトコルの行をそのまま見て、ファームの不具合や配線を切り分ける。";
        m.keywords = "hardware ハードウェア arduino esp32 モニタ monitor ログ log シリアル serial 生ログ プロトコル 送受信 デバッグ";
        m.category = "hardware"; m.group = "hardware"; m.target = "monitor";
        m.effect = McpEffect::Read; m.mode = "any"; m.timeoutMs = 10000; m.idempotent = true;
        m.params = {P("device", "string", true, nullptr, nullptr, nullptr, nullptr, "デバイス名"),
                    P("lines", "int", false, nullptr, "1", "500", "50", "返す行数（末尾から）")};
        m.examples = {{"{\"device\":\"generic\",\"lines\":20}", "末尾 20 行"}};
        McpDefine("hw_monitor", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                const std::string dev = StrParam(params, "device");
                const hw::HwDeviceInfo info = RequireDevice(*m_hardware, dev);
                const size_t n = static_cast<size_t>(std::clamp(params.value("lines", 50), 1, 500));
                json lines = json::array();
                for (const hw::HwLogEntry& e : m_hardware->GetLog(dev, n))
                    lines.push_back({{"t", e.timeSec}, {"dir", e.outgoing ? "tx" : "rx"}, {"text", e.text}});
                resp["ok"] = true;
                resp["result"] = {{"device", dev}, {"status", hw::HwStatusName(info.status)}, {"lines", lines}};
            });
    }

    // ---- hw_write / hw_write_dangerous ----
    {
        McpMeta m;
        m.summary  = "出力チャンネルへ正規化値（0..1）を書く（LED の明るさ・ブザー・振動など）。書いたらすぐ確定して送る。"
                     "config で dangerous:true のチャンネル（ペルチェ・ヒーター等）が含まれていたら拒否する → hw_write_dangerous を使う。";
        m.keywords = "hardware ハードウェア arduino esp32 書く write 出力 output led 振動 ブザー pwm モーター シリアル serial";
        m.category = "hardware"; m.group = "hardware"; m.target = "write";
        m.effect = McpEffect::Runtime; m.mode = "any"; m.timeoutMs = 10000;
        m.params = {P("device", "string", true, nullptr, nullptr, nullptr, nullptr, "デバイス名（接続済み）"),
                    P("values", "object", true, nullptr, nullptr, nullptr, nullptr, "{チャンネル名: 0..1 の値}。例 {\"led\":0.5}")};
        m.next = {{"hw_monitor", "tx に !led=0.5 が出たか確認する"}, {"hw_write_dangerous", "dangerous なチャンネルへ書くとき"}};
        m.examples = {{"{\"device\":\"generic\",\"values\":{\"led\":0.5}}", "LED を半分の明るさに"}};
        McpDefine("hw_write", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                resp["ok"] = true;
                resp["result"] = DoWrite(*m_hardware, params, /*allowDangerous=*/false);
            });

        McpMeta g = m;
        g.summary  = "dangerous:true の出力チャンネル（ペルチェ・ヒーター・モーターなど）へ書く。config の maxValue で上限を切る。"
                     "実機を傷める/やけどの恐れがあるので確認が要る（guarded）。";
        g.target = "write_dangerous";
        g.effect = McpEffect::Guarded;
        g.next = {{"hw_write", "安全なチャンネルは hw_write"}, {"hw_status", "outValue と dangerous / maxValue を確認する"}};
        g.examples = {{"{\"device\":\"cooler\",\"values\":{\"heat\":0.3}}", "ペルチェを 30% で駆動"}};
        McpDefine("hw_write_dangerous", McpMeta(g), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                resp["ok"] = true;
                resp["result"] = DoWrite(*m_hardware, params, /*allowDangerous=*/true);
            });
    }

    // ---- hw_simulate ----
    {
        McpMeta m;
        m.summary  = "入力チャンネルへ仮想の生値を流し込む（実機なしで Lua の hw.* を確かめる）。channels（[{name,dir,type,min,max}]）を渡し、"
                     "そのデバイスが無ければ仮想デバイスを作る。clear:true で流し込みを止める（実機の値へ戻る）。";
        m.keywords = "hardware ハードウェア arduino esp32 シミュレート simulate 仮想 virtual 偽 mock センサー sensor 実機なし テスト";
        m.category = "hardware"; m.group = "hardware"; m.target = "simulate";
        m.effect = McpEffect::Runtime; m.mode = "any"; m.timeoutMs = 10000;
        m.params = {P("device", "string", true, nullptr, nullptr, nullptr, nullptr, "デバイス名（無ければ channels で仮想デバイスを作る）"),
                    P("values", "object", false, nullptr, nullptr, nullptr, nullptr, "{入力チャンネル名: 生値}"),
                    P("channels", "array", false, nullptr, nullptr, nullptr, nullptr, "仮想デバイス用のチャンネル宣言 [{name,dir:in|out,type:bool|int|float,min,max}]"),
                    P("clear", "bool", false, nullptr, nullptr, nullptr, "false", "true で流し込みを解除（values のキーだけ、無ければ全部）")};
        m.next = {{"lua_step", "hw.device(名前):get(\"ch\") で Lua から読めるか確かめる"}, {"hw_status", "チャンネルの値を見る"}};
        m.examples = {{"{\"device\":\"virt\",\"channels\":[{\"name\":\"dial\",\"dir\":\"in\",\"type\":\"int\",\"min\":0,\"max\":4095}],\"values\":{\"dial\":2048}}", "仮想デバイスを作って値を流す"},
                      {"{\"device\":\"generic\",\"clear\":true}", "流し込みを止める"}};
        McpDefine("hw_simulate", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                const std::string dev = StrParam(params, "device");
                if (dev.empty()) throw McpError(McpErr::InvalidParam, "device が空です", "デバイス名を渡す");
                bool created = false;
                hw::HwDeviceInfo info;
                const bool exists = m_hardware->GetDeviceInfo(dev, info);
                const bool haveChannels = params.contains("channels") && params["channels"].is_array() && !params["channels"].empty();
                // 無いデバイスは作る。設定にあるが実機がつながっていない（仮想でもない）デバイスも、channels があれば仮想ポートを割り当てる
                //（hardware.json はそのまま・実機なしで Lua を確かめる用途）。つながっている実機は上書きしない。
                if (!exists || (haveChannels && !info.isVirtual && !info.connected))
                {
                    if (!haveChannels) throw NoDevice(dev, *m_hardware);
                    std::vector<hw::HwChannelDecl> decls;
                    for (const json& cj : params["channels"])
                    {
                        if (!cj.is_object() || StrParam(cj, "name").empty())
                            throw McpError(McpErr::InvalidParam, "channels の要素に name がありません", "[{name,dir,type,min,max}] の形で渡す");
                        decls.push_back(ParseChannelDecl(cj));
                    }
                    if (!m_hardware->AddVirtualDevice(dev, decls))
                        throw McpError(McpErr::InvalidParam, "仮想デバイス '" + dev + "' を作れませんでした", "同名の仮想デバイスが既にある");
                    created = true;
                    m_hardware->Connect(dev);   // userDisabled を解いて仮想ポートへつなぐ
                }
                const bool clear = params.value("clear", false);
                json done = json::object();
                json failed = json::array();
                if (params.contains("values") && params["values"].is_object())
                {
                    for (auto it = params["values"].begin(); it != params["values"].end(); ++it)
                    {
                        if (clear) { m_hardware->ClearSimulation(dev, it.key()); done[it.key()] = nullptr; continue; }
                        if (!it->is_number()) { failed.push_back({{"channel", it.key()}, {"reason", "数値ではない"}}); continue; }
                        if (m_hardware->Simulate(dev, it.key(), it->get<double>())) done[it.key()] = it->get<double>();
                        else failed.push_back({{"channel", it.key()}, {"reason", "入力チャンネルとして宣言されていない（接続前は実機のチャンネルが未宣言）"}});
                    }
                }
                else if (clear) m_hardware->ClearSimulation(dev);
                m_hardware->GetDeviceInfo(dev, info);
                resp["ok"] = true;
                resp["result"] = {{"device", dev}, {"created", created}, {"virtual", info.isVirtual}, {"status", hw::HwStatusName(info.status)},
                                  {"simulated", done}, {"failed", failed}, {"cleared", clear},
                                  {"note", info.connected ? "値は次のフレーム頭（BeginFrame）から Lua / hw_status で見える"
                                                         : "仮想デバイスは作成から最大 1〜2 秒で Ready になる（その後に Lua から読める）"}};
            });
    }

    // ---- hw_calibrate ----
    {
        McpMeta m;
        m.summary  = "入力チャンネルの範囲を校正する。start で min/max の追跡を始め → センサーを端から端まで動かし → finish で範囲を確定して"
                     "プロジェクトの assets/hardware.json に保存する（正規化値 0..1 の基準になる）。cancel で捨てる。";
        m.keywords = "hardware ハードウェア arduino esp32 校正 calibrate キャリブレーション 範囲 range min max ダイヤル dial センサー sensor 保存";
        m.category = "hardware"; m.group = "hardware"; m.target = "calibrate";
        m.effect = McpEffect::WriteFile; m.mode = "any"; m.timeoutMs = 10000;
        m.params = {P("device", "string", true, nullptr, nullptr, nullptr, nullptr, "デバイス名（接続済み）"),
                    P("mode", "enum", true, "start|finish|cancel", nullptr, nullptr, nullptr, "start=追跡開始 / finish=確定して保存 / cancel=捨てる")};
        m.next = {{"hw_status", "channels の min/max が校正値になったか確認する"}};
        m.examples = {{"{\"device\":\"generic\",\"mode\":\"start\"}", "追跡を始める"}, {"{\"device\":\"generic\",\"mode\":\"finish\"}", "範囲を確定して保存"}};
        McpDefine("hw_calibrate", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                const std::string dev = StrParam(params, "device");
                const std::string mode = StrParam(params, "mode");
                const hw::HwDeviceInfo info = RequireDevice(*m_hardware, dev);
                if (mode == "start")
                {
                    if (!info.connected)
                        throw McpError(McpErr::ModeConflict, "デバイス '" + dev + "' は未接続です", "hw_connect してから校正を始める");
                    m_hardware->BeginCalibration(dev);
                    resp["ok"] = true;
                    resp["result"] = {{"device", dev}, {"calibrating", true},
                                      {"note", "センサー（ダイヤルなど）を端から端まで動かしてから mode:\"finish\" を呼ぶ"}};
                    return;
                }
                if (!info.calibrating)
                    throw McpError(McpErr::ModeConflict, "校正は始まっていません", "先に mode:\"start\" を呼ぶ");
                const auto ranges = m_hardware->EndCalibration(dev);
                if (mode == "cancel")
                {
                    resp["ok"] = true;
                    resp["result"] = {{"device", dev}, {"calibrating", false}, {"cancelled", true}};
                    return;
                }
                json rj = json::object();
                json skipped = json::array();
                for (const auto& [ch, r] : ranges)
                {
                    if (!(r.max > r.min)) { skipped.push_back({{"channel", ch}, {"reason", "値が動かなかった（min == max）"}}); continue; }
                    m_hardware->SetChannelRange(dev, ch, r.min, r.max);
                    rj[ch] = {{"min", r.min}, {"max", r.max}};
                }
                if (rj.empty())
                    throw McpError(McpErr::InvalidParam, "範囲を確定できるチャンネルがありませんでした（値が動かなかった）",
                                   "start の後にセンサーを実際に動かしてから finish する");
                bool saved = false;
                std::string path, note;
                const hw::HwConfig cfg0 = m_hardware->Config();
                if (m_isGameMode) note = "配布ゲームでは hardware.json を書けないので保存しなかった（メモリ上だけ反映）";
                else if (!cfg0.FindDevice(dev)) note = "このデバイスは hardware.json に無い（仮想デバイスなど）ので保存しなかった。hw_config_set で登録する";
                else
                {
                    hw::HwConfig cfg = cfg0;
                    cfg.arduinoCli = St(m_hwFlash).arduinoCli;
                    path = PathResolver::AssetsDir() + "hardware.json";
                    std::string err;
                    if (!hw::SaveHwConfigFile(atomicfile::PathFromUtf8(path), cfg, err))
                        throw McpError(McpErr::FileIo, "hardware.json を保存できませんでした: " + err, "assets/ の書き込み権限と空き容量を確認する");
                    saved = true;
                }
                resp["ok"] = true;
                resp["result"] = {{"device", dev}, {"calibrating", false}, {"ranges", rj}, {"skipped", skipped}, {"saved", saved}, {"path", path}};
                if (!note.empty()) resp["result"]["note"] = note;
            });
    }

    // ---- hw_config_get / hw_config_set ----
    {
        McpMeta m;
        m.summary  = "いまのハードウェア設定（assets/hardware.json 相当。校正値の反映済み）を丸ごと返す。arduinoCli の場所・デバイスの match / pins / channels / actions。";
        m.keywords = "hardware ハードウェア 設定 config hardware.json 読む get 校正値 actions pins match";
        m.category = "hardware"; m.group = "hardware"; m.target = "config_get";
        m.effect = McpEffect::Read; m.mode = "any"; m.timeoutMs = 10000; m.idempotent = true;
        m.next = {{"hw_config_set", "書き換える"}};
        McpDefine("hw_config_get", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                hw::HwConfig cfg = m_hardware->Config();
                cfg.arduinoCli = St(m_hwFlash).arduinoCli;
                std::string err;
                bool found = false;
                hw::HwConfig onDisk;
                hw::LoadHwConfigFromAssets(onDisk, err, &found);
                resp["ok"] = true;
                resp["result"] = {{"config", json::parse(hw::DumpHwConfig(cfg))}, {"fileFound", found},
                                  {"path", PathResolver::AssetsDir() + "hardware.json"}};
            });

        McpMeta s;
        s.summary  = "hardware.json を丸ごと書き換え、ハードウェア層を Shutdown→Initialize で作り直す（接続し直し・actions の割当も更新）。"
                     "パースエラーは書かずにエラーで返す。config は {arduinoCli?, devices:[{name, match, pins, channels, actions}]}。";
        s.keywords = "hardware ハードウェア 設定 config hardware.json 書く set 登録 デバイス pins match actions arduino esp32";
        s.category = "hardware"; s.group = "hardware"; s.target = "config_set";
        s.effect = McpEffect::WriteFile; s.mode = "any"; s.timeoutMs = 10000;
        s.params = {P("config", "object", true, nullptr, nullptr, nullptr, nullptr, "hardware.json の中身（docs/HARDWARE.md §3）")};
        s.next = {{"hw_status", "つながって Ready になったか確認する"}};
        s.examples = {{"{\"config\":{\"devices\":[{\"name\":\"generic\",\"match\":{\"vid\":\"1A86\",\"pid\":\"7523\",\"hello\":\"generic\"},"
                       "\"pins\":[{\"pin\":2,\"mode\":\"pwm\",\"name\":\"led\"}],\"actions\":{\"boot\":\"Jump\"}}]}}", "汎用スケッチの ESP32 を登録"}};
        McpDefine("hw_config_set", McpMeta(s), DX12E_MCP_HANDLER
            {
                if (!m_hardware) throw McpError(McpErr::Internal, "ハードウェア層が未初期化", "プロジェクトを開いてから呼ぶ");
                if (m_isGameMode)
                    throw McpError(McpErr::ModeConflict, "配布ゲームでは hardware.json を書き換えられません", "エディタで設定する");
                if (St(m_hwFlash).running)
                    throw McpError(McpErr::Busy, "hw_flash の実行中は設定を変えられません", "hw_flash_status で終わるのを待つ");
                hw::HwConfig cfg;
                std::string err;
                if (!hw::ParseHwConfig(params["config"].dump(), cfg, err))
                {
                    McpError e(McpErr::InvalidParam, "hardware.json の内容が不正です: " + err, "config を直して撃ち直す（ファイルは書いていない）");
                    e.name = "E_HW_BAD_CONFIG";
                    e.cause = err;
                    throw e;
                }
                const std::string path = PathResolver::AssetsDir() + "hardware.json";
                if (!hw::SaveHwConfigFile(atomicfile::PathFromUtf8(path), cfg, err))
                    throw McpError(McpErr::FileIo, "hardware.json を保存できませんでした: " + err, "assets/ の書き込み権限と空き容量を確認する");
                ReinitHardware(cfg);
                json names = json::array();
                for (const auto& d : cfg.devices) names.push_back(d.name);
                resp["ok"] = true;
                resp["result"] = {{"path", path}, {"devices", names}, {"actions", m_hardware->GetActionBindings().size()},
                                  {"note", "接続は非同期。hw_status で Ready を確認する"}};
            });
    }

    // ---- hw_flash / hw_flash_status ----
    {
        McpMeta m;
        m.summary  = "arduino-cli でスケッチをコンパイルして書き込む（非同期。進み具合は hw_flash_status）。対象デバイスのポートを放し、"
                     "終わったら自動で再接続する。同時に 1 本だけ。sketch は .ino かそのフォルダ（相対はプロジェクト → <repo>/hardware/firmware/）。"
                     "fqbn 省略時は推定ボード（ESP32 系 → esp32:esp32:esp32 / Nano Every → arduino:megaavr:nona4809 / Arduino → arduino:avr:uno）。ボードの中身を上書きするので確認が要る。";
        m.keywords = "hardware ハードウェア arduino esp32 書き込み flash upload アップロード コンパイル compile スケッチ sketch ファーム firmware arduino-cli ino";
        m.category = "hardware"; m.group = "hardware"; m.target = "flash";
        m.effect = McpEffect::Guarded; m.mode = "any"; m.timeoutMs = 10000;
        m.params = {P("sketch", "string", true, nullptr, nullptr, nullptr, nullptr, ".ino かそのフォルダ。例 \"UnoLinkGeneric\""),
                    P("device", "string", false, nullptr, nullptr, nullptr, nullptr, "書き込む先のデバイス名（ポートを放して終わったら再接続する）"),
                    P("port", "string", false, nullptr, nullptr, nullptr, nullptr, "COM8 など。device の代わりにポートで指定"),
                    P("fqbn", "string", false, nullptr, nullptr, nullptr, nullptr, "ボード ID。省略で推定（分からなければ必須）。例 esp32:esp32:esp32 / arduino:avr:uno / arduino:avr:nano / arduino:megaavr:nona4809"),
                    P("libraries", "any", false, nullptr, nullptr, nullptr, nullptr, "追加ライブラリのパス（文字列か配列）。既定は <repo>/hardware/firmware/UnoLink")};
        m.next = {{"hw_flash_status", "進み具合・ログ末尾・完了を見る"}};
        m.examples = {{"{\"sketch\":\"UnoLinkGeneric\",\"device\":\"generic\"}", "汎用スケッチを書き直す"}};
        McpDefine("hw_flash", McpMeta(m), DX12E_MCP_HANDLER
            {
                HwFlashRequest rq;
                rq.sketch = StrParam(params, "sketch");
                rq.device = StrParam(params, "device");
                rq.port = StrParam(params, "port");
                rq.fqbn = StrParam(params, "fqbn");
                if (params.contains("libraries") && params["libraries"].is_string()) rq.libraries.push_back(params["libraries"].get<std::string>());
                else if (params.contains("libraries") && params["libraries"].is_array())
                    for (const json& l : params["libraries"]) if (l.is_string()) rq.libraries.push_back(l.get<std::string>());
                json info;
                StartHwFlashImpl(rq, &info);   // エラーは McpError のまま上へ（error_code は従来どおり）
                resp["ok"] = true;
                resp["result"] = std::move(info);
            });

        McpMeta s;
        s.summary  = "hw_flash の進み具合: 状態（idle / installing_core / compiling / uploading / reconnecting / done / failed）・経過秒・終了コード・ログ末尾・再接続できたか。";
        s.keywords = "hardware ハードウェア arduino esp32 書き込み flash status 進捗 progress ログ log arduino-cli 状態";
        s.category = "hardware"; s.group = "hardware"; s.target = "flash_status";
        s.effect = McpEffect::Read; s.mode = "any"; s.timeoutMs = 10000; s.idempotent = true;
        s.params = {P("lines", "int", false, nullptr, "0", "200", "30", "返すログの末尾行数")};
        s.next = {{"hw_status", "done なら自動で再接続されて Ready になっているはず"}};
        McpDefine("hw_flash_status", McpMeta(s), DX12E_MCP_HANDLER
            {
                HwAppState& st = St(m_hwFlash);
                const size_t n = static_cast<size_t>(std::clamp(params.value("lines", 30), 0, 200));
                json r;
                {
                    std::lock_guard<std::mutex> lk(st.m);
                    const double now = SteadySec();
                    const double el = st.running ? now - st.startedSec : (st.endedSec > 0.0 ? st.endedSec - st.startedSec : 0.0);
                    json tail = json::array();
                    const size_t from = st.log.size() > n ? st.log.size() - n : 0;
                    for (size_t i = from; i < st.log.size(); ++i) tail.push_back(SanitizeUtf8(st.log[i]));
                    r = {{"state", st.state}, {"running", st.running}, {"elapsedSec", el}, {"exitCode", st.exitCode},
                         {"device", st.device}, {"port", st.port}, {"fqbn", st.fqbn}, {"sketch", st.sketch},
                         {"reconnected", st.reconnected}, {"logTail", tail}};
                    if (!st.error.empty()) r["error"] = SanitizeUtf8(st.error);
                    if (!st.command.empty()) r["command"] = st.command;
                }
                if (m_hardware && !r["device"].get<std::string>().empty())
                {
                    hw::HwDeviceInfo info;
                    if (m_hardware->GetDeviceInfo(r["device"].get<std::string>(), info))
                        r["deviceStatus"] = hw::HwStatusName(info.status);
                }
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // ---- hw_setup ----
    {
        McpMeta m;
        m.summary  = "Arduino の書き込み道具と同梱サンプルの状態を見る / 動かす。action: status（既定。道具箱 = arduino-cli + コア esp32:esp32・arduino:avr・arduino:megaavr（Nano Every）+ ライブラリ Servo の状態・進み具合・ログ、"
                     "同梱サンプルの有無と配置済み）/ install_toolchain（道具箱の自動導入を背景で始める。冪等）/ install_samples（<exe>/samples の ArduinoLab などを "
                     "ドキュメント\\UnoProjects へ置く。dest で置き場を変えられる。force で「置いた」目印を無視）。道具箱はプロジェクトを開くと自動でも入る。";
        m.keywords = "hardware ハードウェア arduino esp32 セットアップ setup 道具 書き込み道具 toolchain arduino-cli core サンプル sample ArduinoLab 導入 インストール install 準備";
        m.category = "hardware"; m.group = "hardware"; m.target = "setup";
        m.effect = McpEffect::Runtime; m.mode = "any"; m.timeoutMs = 10000;
        m.params = {P("action", "string", false, "status|install_toolchain|install_samples", nullptr, nullptr, "status", "status / install_toolchain / install_samples"),
                    P("dest", "string", false, nullptr, nullptr, nullptr, nullptr, "install_samples の置き場（既定 ドキュメント\\UnoProjects）。指定時は目印・最近のプロジェクトに触らない（検証用）"),
                    P("forceDownload", "bool", false, nullptr, nullptr, nullptr, "false", "install_toolchain: arduino-cli がすでにあっても管理下（LocalAppData/UnoEngine/arduino-cli）へ落とし直して使う（検証用）"),
                    P("force", "bool", false, nullptr, nullptr, nullptr, "false", "install_samples: 「置いた」目印を無視して置く（コピー先が空でなければやはり上書きしない）")};
        m.next = {{"hw_flash", "道具箱が ready になったらスケッチを書き込む"}, {"hw_status", "デバイスの状態"}};
        m.examples = {{"{}", "道具箱とサンプルの状態"}, {"{\"action\":\"install_toolchain\"}", "arduino-cli とコアを入れ始める"}};
        McpDefine("hw_setup", McpMeta(m), DX12E_MCP_HANDLER
            {
                const std::string action = params.contains("action") ? StrParam(params, "action") : std::string("status");
                HwAppState& st = St(m_hwFlash);
                hw::HwToolchain& tc = HwToolchainRef();
                json extra = json::object();
                if (action == "install_toolchain")
                {
                    if (m_isGameMode)
                        throw McpError(McpErr::ModeConflict, "配布ゲームでは書き込み道具を入れられません", "エディタで行う");
                    const bool started = tc.Start(st.arduinoCli, params.value("forceDownload", false));
                    extra["started"] = started;
                    if (!started) extra["note"] = "すでに導入中です（status で進み具合を見る）";
                }
                else if (action == "install_samples")
                {
                    if (m_isGameMode)
                        throw McpError(McpErr::ModeConflict, "配布ゲームではサンプルを置けません", "エディタで行う");
                    extra["samplesResult"] = InstallBundledSamples(StrParam(params, "dest"), params.value("force", false), false);
                }
                else if (action != "status")
                {
                    McpError e(McpErr::InvalidParam, "action '" + action + "' は不明です", "status / install_toolchain / install_samples のどれか",
                               {"status", "install_toolchain", "install_samples"});
                    throw e;
                }
                const hw::HwToolchain::Snapshot s = tc.Get(st.arduinoCli);
                json tail = json::array();
                const size_t from = s.log.size() > 30 ? s.log.size() - 30 : 0;
                for (size_t i = from; i < s.log.size(); ++i) tail.push_back(SanitizeUtf8(s.log[i]));   // 不正な UTF-8 が混ざっても dump が投げないように
                json tcj{{"state", hw::HwToolchain::StateName(s.state)}, {"ready", s.ready}, {"running", s.running}, {"progress", s.progress},
                         {"cli", SanitizeUtf8(s.cli)}, {"managedCli", SanitizeUtf8(hw::ManagedCliPath())}, {"cores", s.cores}, {"libs", s.libs}, {"logTail", tail}};
                if (!s.error.empty()) tcj["error"] = SanitizeUtf8(s.error);
                json r = std::move(extra);
                r["toolchain"] = std::move(tcj);
                r["samples"] = HwSamplesStatus();
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }
}

} // namespace dx12e
