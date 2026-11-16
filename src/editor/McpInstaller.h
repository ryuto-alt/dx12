#pragma once
// ===========================================================================
// MCP サーバのワンクリック導入（エディタの「MCP / AI Bridge」窓のボタン / `DX12Engine.exe --install-mcp`）。
// エンジンをダウンロードしただけの人が、ボタン 1 回で Codex / Claude Code などから使えるようにする。
//
//   1) Node.js v24+ : PATH にあればそれを使う。無ければ nodejs.org から公式 zip を
//                     %LOCALAPPDATA%\UnoEngine\node へ落とす（SHASUMS256.txt で SHA-256 を照合）
//   2) MCP サーバ   : 既にある（%USERPROFILE%\dx12-mcp の clone / ソースからのビルドの tools\mcp-server）なら
//                     それを使う。無ければ GitHub の ryuto-alt/dx12-mcp を zip で
//                     %LOCALAPPDATA%\UnoEngine\mcp-server へ落とす（git は不要。押し直すと最新版へ更新）
//   3) npm ci       : 依存を入れる（npm は 1) の Node に付属のものを使う）
//   4) 登録         : Codex(~/.codex/config.toml) は必ず、Claude Code(~/.claude.json) /
//                     Claude Desktop / Cursor は入っているものだけ。書き換える前に元のファイルを
//                     <名前>.unoengine-<日時>.bak へ退避し、dx12-engine のエントリだけを差し替える
//                     （中身の計算は McpClientConfig.h の純関数）
//
// 子プロセスは CREATE_NO_WINDOW | BELOW_NORMAL（窓を出さない）。背景ワーカー 1 本。
// ===========================================================================
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dx12e::mcpinst
{

// %LOCALAPPDATA%\UnoEngine（末尾の区切りなし・UTF-8）。取れなければ空。
std::string ManagedRoot();
// エンジンが入れる MCP サーバの場所（<ManagedRoot>\mcp-server）
std::string ManagedServerDir();

// MCP サーバ（index.ts があるフォルダ）を探す。見つからなければ空。'/' 区切り。
// 探索順: %USERPROFILE%\dx12-mcp（手で clone）→ エンジンが入れた場所 → exe から上へ辿った tools/mcp-server（ソースからのビルド）
//         → ビルド時マクロ fallbackDir
std::string ResolveServerDir(const std::string& fallbackDir);

// 登録先 1 件の結果
struct ClientResult
{
    enum class Status { Written, Unchanged, NotInstalled, Failed };
    std::string client;    // "Codex" など
    std::string path;      // 設定ファイル
    Status      status = Status::NotInstalled;
    std::string note;      // 失敗の理由・退避先など
};

class McpInstaller
{
public:
    enum class Step { Idle, Node, Server, Npm, Register, Done, Failed };

    struct Snapshot
    {
        Step        step = Step::Idle;
        float       progress = 0.0f;     // ダウンロード中の 0..1（不明なら 0）
        bool        running = false;
        std::string error;
        std::string nodePath;            // 使った node.exe
        std::string serverDir;           // 使った MCP サーバ
        std::vector<std::string> log;    // 末尾（最大 300 行）
        std::vector<ClientResult> clients;
    };

    McpInstaller() = default;
    ~McpInstaller() { Shutdown(); }
    McpInstaller(const McpInstaller&) = delete;
    McpInstaller& operator=(const McpInstaller&) = delete;

    // 導入を始める。すでに走っていれば false。fallbackDir は ResolveServerDir へそのまま渡す。
    bool Start(const std::string& fallbackDir);
    // 走っているなら止めて回収する（子プロセスとダウンロードごと）
    void Shutdown();
    // 走り終わるまで待つ（--install-mcp 用）
    void Wait();
    Snapshot Get() const;
    bool Running() const { return m_running.load(); }
    // Start のたびに増える（窓が「結果が変わった」ことを知るため）
    unsigned Generation() const { return m_generation.load(); }

    static const char* StepLabel(Step s);

private:
    void Run(std::string fallbackDir);
    void RunImpl(const std::string& fallbackDir);
    void SetStep(Step s);
    void SetProgress(float p);
    void Log(const std::string& line);
    void LogText(const char* data, size_t n);
    // 隠しプロセスを走らせる。出力はログへ（capture があればそこにも）。pathPrefix は PATH の先頭へ足すフォルダ。
    // 終了コード（起動できなければ -1）
    int  RunProcess(const std::string& cmdLine, const std::string& workDir, const std::string& pathPrefix,
                    std::string* capture = nullptr);
    bool ExtractZip(const std::string& zip, const std::string& dest);
    bool Download(const std::wstring& url, std::vector<unsigned char>& out, const char* what);

    std::string EnsureNode(std::string& err);
    std::string EnsureServer(const std::string& fallbackDir, std::string& err);
    bool        InstallDeps(const std::string& node, const std::string& serverDir, std::string& err);
    std::vector<ClientResult> RegisterClients(const std::string& node, const std::string& serverDir);
    // node.exe の major 版（動かなければ 0）
    int NodeMajor(const std::string& nodeExe);

    mutable std::mutex m_mu;
    std::thread        m_worker;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_cancel{false};
    std::atomic<unsigned> m_generation{0};
    Step               m_step = Step::Idle;
    float              m_progress = 0.0f;
    std::string        m_error;
    std::string        m_nodePath;
    std::string        m_serverDir;
    std::deque<std::string> m_log;
    std::string        m_partial;
    std::vector<ClientResult> m_clients;
    void*              m_job = nullptr;       // HANDLE
    void*              m_process = nullptr;   // HANDLE
};

} // namespace dx12e::mcpinst
