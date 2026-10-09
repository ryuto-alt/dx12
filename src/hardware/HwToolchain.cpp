#include "hardware/HwToolchain.h"

#include "core/AtomicFile.h"
#include "core/Http.h"
#include "core/Logger.h"
#include "core/PathResolver.h"

#include <nlohmann/json.hpp>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>

namespace dx12e::hw
{

namespace fs = std::filesystem;
using nlohmann::json;

const char* const kEspressifIndexUrl = "https://espressif.github.io/arduino-esp32/package_esp32_index.json";
const char* const kRequiredCores[3] = {"esp32:esp32", "arduino:avr", "arduino:megaavr"};
const char* const kRequiredLibs[1] = {"Servo"};

namespace
{

constexpr const wchar_t* kCliZipUrl = L"https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip";

double SteadySec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::wstring W(const std::string& s) { return PathResolver::Utf8ToWide(s); }
std::string  U(const std::wstring& s) { return PathResolver::WideToUtf8(s); }

bool IsFile(const std::string& p)
{
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(W(p), ec);
}

std::string CoresJsonPath() { return ManagedCliDir().empty() ? std::string() : ManagedCliDir() + "\\cores.json"; }

} // namespace

std::string UnoEngineDataDir()
{
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p)
        out = U((fs::path(p) / L"UnoEngine").wstring());
    if (p) ::CoTaskMemFree(p);
    return out;
}

std::string ManagedCliDir()
{
    const std::string d = UnoEngineDataDir();
    return d.empty() ? std::string() : d + "\\arduino-cli";
}

std::string ManagedCliPath()
{
    const std::string d = ManagedCliDir();
    return d.empty() ? std::string() : d + "\\arduino-cli.exe";
}

std::string FindArduinoCli(const std::string& fromConfig, std::vector<std::string>& tried)
{
    if (!fromConfig.empty())
    {
        tried.push_back(fromConfig + "（hardware.json の arduinoCli）");
        if (IsFile(fromConfig)) return fromConfig;
    }
    {
        const std::string m = ManagedCliPath();
        tried.push_back(m + "（エンジンが自動で入れる場所）");
        if (IsFile(m)) return m;
    }
    {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = ::SearchPathW(nullptr, L"arduino-cli.exe", nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr);
        tried.push_back("PATH 上の arduino-cli.exe");
        if (n > 0 && n < std::size(buf)) return U(buf);
    }
    const std::string def = "C:\\Program Files\\Arduino CLI\\arduino-cli.exe";
    tried.push_back(def);
    if (IsFile(def)) return def;
    return {};
}

std::string CoreOfFqbn(const std::string& fqbn)
{
    const size_t a = fqbn.find(':');
    if (a == std::string::npos) return {};
    const size_t b = fqbn.find(':', a + 1);
    if (b == std::string::npos) return {};
    return fqbn.substr(0, b);
}

std::vector<std::string> InstalledCoresInMarker()
{
    std::vector<std::string> out;
    const std::string path = CoresJsonPath();
    if (path.empty()) return out;
    std::ifstream f(W(path), std::ios::binary);
    if (!f) return out;
    try
    {
        const json j = json::parse(f, nullptr, true, true);
        if (j.contains("cores") && j["cores"].is_array())
            for (const json& c : j["cores"]) if (c.is_string()) out.push_back(c.get<std::string>());
    }
    catch (...) {}
    return out;
}

bool CoreInMarker(const std::string& core)
{
    const auto v = InstalledCoresInMarker();
    return std::find(v.begin(), v.end(), core) != v.end();
}

std::vector<std::string> InstalledLibsInMarker()
{
    std::vector<std::string> out;
    const std::string path = CoresJsonPath();
    if (path.empty()) return out;
    std::ifstream f(W(path), std::ios::binary);
    if (!f) return out;
    try
    {
        const json j = json::parse(f, nullptr, true, true);
        if (j.contains("libs") && j["libs"].is_array())
            for (const json& c : j["libs"]) if (c.is_string()) out.push_back(c.get<std::string>());
    }
    catch (...) {}
    return out;
}

bool LibInMarker(const std::string& lib)
{
    const auto v = InstalledLibsInMarker();
    return std::find(v.begin(), v.end(), lib) != v.end();
}

namespace
{
// cores.json を書く（コアとライブラリの両方を持ち回る）。例外は外へ出さない。
void WriteMarkerFile(const std::vector<std::string>& cores, const std::vector<std::string>& libs, const std::string& cli)
{
    try
    {
        const std::string path = CoresJsonPath();
        if (path.empty()) return;
        std::error_code ec;
        fs::create_directories(W(ManagedCliDir()), ec);
        const json j{{"cores", cores}, {"libs", libs}, {"cli", cli}};
        // 日本語ユーザー名のパスなど、不正な UTF-8 が混ざっても dump が投げないよう置換モードで出す
        const atomicfile::Result r = atomicfile::WriteFile(atomicfile::PathFromUtf8(path), j.dump(2, ' ', false, json::error_handler_t::replace));
        if (!r.ok) Logger::Warn("[hw] cores.json を書けませんでした: {}", r.error);
    }
    catch (const std::exception& e)
    {
        Logger::Warn("[hw] cores.json を書けませんでした: {}", SanitizeUtf8Lossy(e.what()));
    }
    catch (...) {}
}
} // namespace

void AddCoreToMarker(const std::string& core, const std::string& cli)
{
    auto cores = InstalledCoresInMarker();
    if (std::find(cores.begin(), cores.end(), core) == cores.end()) cores.push_back(core);
    WriteMarkerFile(cores, InstalledLibsInMarker(), cli);
}

void AddLibToMarker(const std::string& lib, const std::string& cli)
{
    auto libs = InstalledLibsInMarker();
    if (std::find(libs.begin(), libs.end(), lib) == libs.end()) libs.push_back(lib);
    WriteMarkerFile(InstalledCoresInMarker(), libs, cli);
}

std::string SanitizeUtf8Lossy(const std::string& s)
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

const char* HwToolchain::StateName(State s)
{
    switch (s)
    {
    case State::Downloading:      return "downloading";
    case State::InstallingCores:  return "installing_cores";
    case State::Ready:            return "ready";
    case State::Failed:           return "failed";
    default:                      return "idle";
    }
}

void HwToolchain::SetState(State s)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_state = s;
}

void HwToolchain::Log(const std::string& line)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_log.push_back(SanitizeUtf8Lossy(line.size() > 300 ? line.substr(0, 300) : line));
    if (m_log.size() > 200) m_log.pop_front();
}

void HwToolchain::LogText(const char* data, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        const char c = data[i];
        if (c == '\n' || c == '\r')
        {
            if (!m_partial.empty()) { Log(m_partial); m_partial.clear(); }
        }
        else m_partial += c;
    }
}

int HwToolchain::RunLogged(const std::string& cmdLine, const std::string& workDir)
{
    Log("> " + cmdLine);
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
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = W(cmdLine);
    const std::wstring wd = W(workDir);
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                     nullptr, wd.empty() ? nullptr : wd.c_str(), &si, &pi);
    ::CloseHandle(wr);
    if (!ok)
    {
        Log("起動できませんでした（Win32 エラー " + std::to_string(::GetLastError()) + "）");
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

    char buf[2048];
    DWORD got = 0;
    while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) LogText(buf, got);
    LogText("\n", 1);
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

bool HwToolchain::ExtractZip(const std::string& zip, const std::string& dest)
{
    // tar.exe（Windows 10 1803 以降に標準。zip も展開できる）→ だめなら PowerShell の Expand-Archive
    std::error_code ec;
    fs::create_directories(W(dest), ec);
    if (RunLogged("tar.exe -xf \"" + zip + "\" -C \"" + dest + "\"", dest) == 0) return true;
    if (m_cancel.load()) return false;
    auto q = [](const std::string& s) {   // PowerShell の単引用符内でのエスケープ
        std::string o;
        for (char c : s) { o += c; if (c == '\'') o += '\''; }
        return o;
    };
    const std::string ps = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                           q(zip) + "' -DestinationPath '" + q(dest) + "' -Force\"";
    return RunLogged(ps, dest) == 0;
}

bool HwToolchain::Start(const std::string& configCli, bool forceDownload)
{
    if (m_running.load()) return false;
    if (m_worker.joinable()) m_worker.join();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        std::vector<std::string> tried;
        m_state = (forceDownload || FindArduinoCli(configCli, tried).empty()) ? State::Downloading : State::InstallingCores;
        m_progress = 0.0f;
        m_error.clear();
        m_log.clear();
        m_partial.clear();
        m_lastCheckSec = -100.0;
    }
    m_cancel.store(false);
    m_running.store(true);
    m_worker = std::thread([this, configCli, forceDownload]() { Run(configCli, forceDownload); });
    return true;
}

void HwToolchain::Shutdown()
{
    m_cancel.store(true);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_job) ::TerminateJobObject(m_job, 1);
        else if (m_process) ::TerminateProcess(m_process, 1);
    }
    if (m_worker.joinable()) m_worker.join();
    m_cancel.store(false);
}

void HwToolchain::Run(std::string configCli, bool forceDownload)
{
    // ★背景スレッドの最上位。ここから例外が出ると std::terminate でエンジンごと落ちるので、全部ここで受けて Failed にする。
    try
    {
        RunImpl(std::move(configCli), forceDownload);
    }
    catch (const std::exception& e)
    {
        const std::string msg = "内部エラー: " + SanitizeUtf8Lossy(e.what());
        Logger::Warn("[hw] 書き込み道具の準備で例外: {}", msg);
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = msg;
        m_state = State::Failed;
    }
    catch (...)
    {
        Logger::Warn("[hw] 書き込み道具の準備で不明な例外");
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = "内部エラー: 不明な例外";
        m_state = State::Failed;
    }
    // どの出口でも必ず「走っていない」に戻す（Start のやり直し・窓の「準備する」ボタンが固まらないように）
    m_running.store(false);
}

void HwToolchain::RunImpl(std::string configCli, bool forceDownload)
{
    auto fail = [this](const std::string& msg) {
        Logger::Warn("[hw] 書き込み道具の準備に失敗: {}", msg);
        std::lock_guard<std::mutex> lk(m_mu);
        m_error = msg;
        m_state = State::Failed;
        m_running.store(false);
    };

    std::vector<std::string> tried;
    std::string cli = FindArduinoCli(configCli, tried);

    // 1) arduino-cli が無ければ落として展開
    if (cli.empty() || forceDownload)
    {
        SetState(State::Downloading);
        const std::string dir = ManagedCliDir();
        if (dir.empty()) { fail("保存先（LocalAppData）を取得できませんでした"); return; }
        std::error_code ec;
        fs::create_directories(W(dir), ec);
        Log("arduino-cli をダウンロードします（約 18MB）");
        std::vector<uint8_t> bytes;
        const std::function<void(uint64_t, uint64_t)> prog = [this](uint64_t done, uint64_t total) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_progress = total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f;
        };
        if (!http::Get(kCliZipUrl, bytes, &prog, &m_cancel))
        {
            fail(m_cancel.load() ? "中断されました" : "arduino-cli をダウンロードできませんでした（ネットワークを確認してください）");
            return;
        }
        const std::string zip = dir + "\\arduino-cli_download.zip";
        {
            std::ofstream f(W(zip), std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            f.flush();
            if (!f) { fail("ダウンロードしたファイルを保存できませんでした"); return; }
        }
        Log("展開します");
        const bool extracted = ExtractZip(zip, dir);
        fs::remove(W(zip), ec);   // 単一ファイルの削除
        if (!extracted) { fail(m_cancel.load() ? "中断されました" : "arduino-cli の zip を展開できませんでした"); return; }
        cli = ManagedCliPath();
        if (!IsFile(cli)) { fail("展開したが arduino-cli.exe が見つかりません"); return; }
        std::lock_guard<std::mutex> lk(m_mu);
        m_progress = 1.0f;
    }

    // 2) コア（索引 → esp32 → avr → megaavr）とライブラリ（Servo）。入っていれば一瞬で終わる
    SetState(State::InstallingCores);
    const std::string extra = std::string(" --additional-urls ") + kEspressifIndexUrl;
    if (RunLogged("\"" + cli + "\" core update-index" + extra, "") != 0)
    {
        fail(m_cancel.load() ? "中断されました" : "ボード索引（core update-index）の更新に失敗しました（ネットワークを確認してください）");
        return;
    }
    for (const char* core : kRequiredCores)
    {
        if (m_cancel.load()) { fail("中断されました"); return; }
        if (RunLogged("\"" + cli + "\" core install " + core + extra, "") != 0)
        {
            fail(m_cancel.load() ? std::string("中断されました") : std::string(core) + " を入れられませんでした（ログを参照）");
            return;
        }
        AddCoreToMarker(core, cli);
    }
    for (const char* lib : kRequiredLibs)
    {
        if (m_cancel.load()) { fail("中断されました"); return; }
        if (RunLogged("\"" + cli + "\" lib install " + lib, "") != 0)
        {
            fail(m_cancel.load() ? std::string("中断されました") : std::string("ライブラリ ") + lib + " を入れられませんでした（ログを参照）");
            return;
        }
        AddLibToMarker(lib, cli);
    }
    Logger::Info("[hw] 書き込み道具の準備ができました: {}", cli);
    std::lock_guard<std::mutex> lk(m_mu);
    m_state = State::Ready;
    m_lastCheckSec = -100.0;
    m_running.store(false);
}

HwToolchain::Snapshot HwToolchain::Get(const std::string& configCli)
{
    Snapshot s;
    std::lock_guard<std::mutex> lk(m_mu);
    s.running = m_running.load();
    s.progress = m_progress;
    s.error = m_error;
    s.log.assign(m_log.begin(), m_log.end());
    s.state = m_state;

    // 軽い確認（1 秒に 1 回まで）: CLI があり、cores.json に必要なコアが全部載っているか
    const double now = SteadySec();
    if (now - m_lastCheckSec > 1.0)
    {
        m_lastCheckSec = now;
        std::vector<std::string> tried;
        m_lastCli = FindArduinoCli(configCli, tried);
        m_lastCores = InstalledCoresInMarker();
        m_lastLibs = InstalledLibsInMarker();
        const std::vector<std::string>& libs = m_lastLibs;
        m_lastReady = !m_lastCli.empty();
        for (const char* c : kRequiredCores)
            if (std::find(m_lastCores.begin(), m_lastCores.end(), c) == m_lastCores.end()) m_lastReady = false;
        for (const char* l : kRequiredLibs)
            if (std::find(libs.begin(), libs.end(), l) == libs.end()) m_lastReady = false;
    }
    if (!s.running)
    {
        if (m_lastReady) { m_state = State::Ready; s.state = State::Ready; }
        else if (s.state == State::Ready) { m_state = State::Idle; s.state = State::Idle; }   // マーカーが消えた
    }
    s.cli = m_lastCli;
    s.cores = m_lastCores;
    s.libs = m_lastLibs;
    s.ready = s.state == State::Ready;
    return s;
}

} // namespace dx12e::hw
