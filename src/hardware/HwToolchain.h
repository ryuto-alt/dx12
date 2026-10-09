#pragma once
// ===========================================================================
// Arduino の書き込み道具（arduino-cli + ボードのコア esp32:esp32 / arduino:avr）の自動導入。
//   場所      : %LOCALAPPDATA%\UnoEngine\arduino-cli\arduino-cli.exe（ManagedCliPath）
//   目印      : 同フォルダの cores.json {"cores":[...], "libs":[...], "cli":"..."}（入れ終えたコア。プロセスを起こさず「準備済み」を判定する）
//   導入      : 背景ワーカー 1 本（HwToolchain）。CLI が無ければ downloads.arduino.cc から落として展開 →
//               core update-index → core install esp32:esp32 / arduino:avr / arduino:megaavr → lib install Servo → cores.json を書く。
//   子プロセス: 常に CREATE_NO_WINDOW | BELOW_NORMAL（窓を出さず、操作に響かせない）。
// ===========================================================================
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace dx12e::hw
{

// %LOCALAPPDATA%\UnoEngine（末尾の区切りなし）。取れなければ空。
std::string UnoEngineDataDir();
std::string ManagedCliDir();     // <UnoEngineDataDir>\arduino-cli
std::string ManagedCliPath();    // <ManagedCliDir>\arduino-cli.exe

// arduino-cli.exe の場所: hardware.json の arduinoCli → 管理下 → PATH → 既定のインストール先。
// 見つからなければ空。tried に探した場所を積む。
std::string FindArduinoCli(const std::string& fromConfig, std::vector<std::string>& tried);

// FQBN の先頭 2 つ（"esp32:esp32:esp32" → "esp32:esp32"）。足りなければ空。
std::string CoreOfFqbn(const std::string& fqbn);

// cores.json に載っているコア。
std::vector<std::string> InstalledCoresInMarker();
bool CoreInMarker(const std::string& core);
// コアを cores.json に足す（cli は最後に使った arduino-cli のパス）。
void AddCoreToMarker(const std::string& core, const std::string& cli);
// cores.json の "libs"（arduino-cli lib install 済みのライブラリ。Servo など）
std::vector<std::string> InstalledLibsInMarker();
bool LibInMarker(const std::string& lib);
void AddLibToMarker(const std::string& lib, const std::string& cli);

// core install の追加引数（esp32 は公式の索引に無いので Espressif の索引を足す）
extern const char* const kEspressifIndexUrl;
// 要るコア
extern const char* const kRequiredCores[3];   // esp32:esp32 / arduino:avr / arduino:megaavr（Nano Every）
// 要るライブラリ（arduino-cli lib install。Servo ライブラリを使うスケッチ用）
extern const char* const kRequiredLibs[1];

// 不正な UTF-8 を '?' に置き換える
std::string SanitizeUtf8Lossy(const std::string& s);

class HwToolchain
{
public:
    enum class State { Idle, Downloading, InstallingCores, Ready, Failed };

    struct Snapshot
    {
        State       state = State::Idle;
        float       progress = 0.0f;            // Downloading のとき 0..1
        std::string cli;                         // 見つかっている arduino-cli（無ければ空）
        std::vector<std::string> cores;          // cores.json のコア
        std::vector<std::string> libs;           // cores.json のライブラリ（Servo など）
        std::string error;
        std::vector<std::string> log;            // 末尾（最大 200 行）
        bool        ready = false;
        bool        running = false;
    };

    HwToolchain() = default;
    ~HwToolchain() { Shutdown(); }
    HwToolchain(const HwToolchain&) = delete;
    HwToolchain& operator=(const HwToolchain&) = delete;

    // 導入を始める。すでに走っていれば false。
    // forceDownload: arduino-cli がすでにあっても管理下（ManagedCliPath）へ落とし直して使う（取り直し・検証用）。
    bool Start(const std::string& configCli, bool forceDownload = false);
    // 走っているなら止めて回収する（子プロセスごと）
    void Shutdown();
    // 状態。Idle / Ready / Failed のときは軽く（1 秒に 1 回まで）cores.json と CLI の有無を見て「準備済み」を判定する。
    Snapshot Get(const std::string& configCli);
    bool Running() const { return m_running.load(); }

    static const char* StateName(State s);

private:
    void Run(std::string configCli, bool forceDownload);       // スレッドの入口（例外を全部受ける）
    void RunImpl(std::string configCli, bool forceDownload);   // 本体
    void SetState(State s);
    void Log(const std::string& line);
    void LogText(const char* data, size_t n);
    // 隠しプロセスを走らせて出力をログへ。終了コード（起動できなければ -1）
    int  RunLogged(const std::string& cmdLine, const std::string& workDir);
    bool ExtractZip(const std::string& zip, const std::string& dest);

    mutable std::mutex m_mu;
    std::thread        m_worker;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_cancel{false};
    State              m_state = State::Idle;
    float              m_progress = 0.0f;
    std::string        m_error;
    std::deque<std::string> m_log;
    std::string        m_partial;
    HANDLE             m_job = nullptr;
    HANDLE             m_process = nullptr;
    // 準備済み判定のキャッシュ
    double             m_lastCheckSec = -100.0;
    bool               m_lastReady = false;
    std::string        m_lastCli;
    std::vector<std::string> m_lastCores;
    std::vector<std::string> m_lastLibs;
};

} // namespace dx12e::hw
