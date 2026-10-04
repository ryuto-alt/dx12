#pragma once
// ============================================================================
// デバイス 1 台ぶんの状態（HardwareSystem の内部表現）と、外へ返すスナップショット型。
// 内部状態（HwDevice）は HardwareSystem の m_mutex が守る。transport / assembler など
// 「IO スレッド専用」と書いたものだけはロック無しで IO スレッドが触る。
// ============================================================================

#include "hardware/HwConfig.h"
#include "hardware/HwLoopbackTransport.h"
#include "hardware/HwProtocol.h"
#include "hardware/HwTransport.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dx12e::hw
{

enum class HwStatus { Disconnected, Opening, WaitingHello, Ready, Error };
const char* HwStatusName(HwStatus s);

// ---- 外へ返すスナップショット ----------------------------------------------

struct HwStats
{
    uint64_t rxLines  = 0;
    uint64_t rxBytes  = 0;
    uint64_t txLines  = 0;
    double   rxRateHz = 0.0;      // 直近 1 秒の受信行レート
    double   lastRxAgeMs = -1.0;  // 最後に受信してからの経過（未受信は -1）
    double   rttMs    = -1.0;     // 直近の ?ping → @pong（未計測は -1）
    uint64_t errors   = 0;        // @err の数
    uint64_t overflow = 0;        // 128 バイト超で捨てた行
    uint64_t badLines = 0;        // 解釈できなかった行
};

struct HwChannelInfo
{
    std::string name;
    bool        isOutput  = false;
    HwChType    type      = HwChType::Float;
    bool        hasRange  = false;
    double      min = 0.0, max = 1.0;   // 校正値込みの実効値
    double      raw       = 0.0;        // 最新の生値（IO スレッドが書く）
    double      value     = 0.0;        // 直近の BeginFrame で確定した正規化値
    bool        hasValue  = false;
    bool        simulated = false;
    bool        dangerous = false;
    double      maxValue  = 1.0;
    double      outValue  = 0.0;        // 出力: 最後に Set した正規化値
};

struct HwLogEntry
{
    double      timeSec  = 0.0;   // システム起動からの秒
    bool        outgoing = false; // true = エンジン → デバイス
    std::string text;
};

struct HwDeviceInfo
{
    std::string name;
    HwStatus    status = HwStatus::Disconnected;
    bool        connected = false;
    bool        isVirtual = false;
    bool        userDisabled = false;
    std::string port;
    std::string describe;
    std::string lastError;
    std::string helloName, board;
    int         fw = 0, proto = 0;
    HwStats     stats;
    std::vector<HwChannelInfo> channels;
    bool        calibrating = false;
    bool        sampling = false;
};

struct HwCalRange { double min = 0.0; double max = 0.0; };

struct HwSampleStats
{
    uint64_t count  = 0;
    double   min    = 0.0;
    double   max    = 0.0;
    double   mean   = 0.0;
    double   stddev = 0.0;
};

struct HwActionBinding
{
    std::string device;
    std::string channel;
    std::string action;
};

// ---- 内部状態 ---------------------------------------------------------------

struct HwChannelState
{
    HwChannelDecl   decl;
    HwChannelConfig cfg;

    // 入力（IO スレッドが m_mutex 下で書く）
    double latestRaw = 0.0;
    bool   hasValue  = false;
    bool   simulated = false;
    double simRaw    = 0.0;

    // フレームのスナップショット（BeginFrame が確定）
    double rawF = 0.0, valueF = 0.0;
    bool   downF = false, prevDownF = false, pressedF = false, releasedF = false;
    bool   smoothInit = false;

    // 出力
    double outNorm = 0.0;

    // 校正（in チャンネルの raw min/max）
    double calMin = std::numeric_limits<double>::infinity();
    double calMax = -std::numeric_limits<double>::infinity();

    // サンプル集計（Welford）
    uint64_t sCount = 0;
    double   sMin = 0.0, sMax = 0.0, sMean = 0.0, sM2 = 0.0;
};

struct HwDevice
{
    std::string    name;
    HwDeviceConfig cfg;
    bool           fromConfig = true;     // false = AddVirtualDevice だけで生えたもの
    std::shared_ptr<HwLoopbackDevice> loopback;   // 仮想デバイスなら非 null

    // ---- m_mutex が守る ----
    HwStatus    status = HwStatus::Disconnected;
    std::string port, describe, lastError;
    std::string helloName, board;
    int         fw = 0, proto = 0;

    std::vector<HwChannelState>             channels;
    std::unordered_map<std::string, size_t> chIndex;

    HwStats stats;
    double  lastRxMs = 0.0;               // 0 = 未受信
    double  rttMs = -1.0;
    uint64_t overflowBase = 0;            // assembler のカウントとの差分用

    bool userDisabled     = false;        // Disconnect 後は自動再接続しない
    bool closeRequested   = false;
    bool connectRequested = false;
    std::string connectPort;
    bool safeRequested    = false;

    std::map<std::string, double> pendingOut;   // Set で溜める（BeginFrame / FlushOutputs で確定）
    std::map<std::string, double> lastSent;     // 重複送信の抑止
    std::vector<std::string>      sendQueue;    // 確定済みの送信行（IO スレッドが送る）
    std::deque<HwLogEntry>        log;

    bool calibrating = false;
    bool sampling    = false;

    // ---- IO スレッド専用 ----
    std::unique_ptr<IHwTransport> transport;
    HwLineAssembler assembler;
    double openedAtMs = 0, helloAskedAtMs = 0, helloAtMs = 0, lastPingMs = 0;
    bool   helloAsked = false, helloReceived = false, pinsSent = false;
    int    pingSeq = 0;
    std::map<int, double> pingSent;
    double rateWindowStartMs = 0;
    uint64_t rateWindowLines = 0;
    std::string pendingFail;              // 行の処理中に決まった「閉じる理由」
};

} // namespace dx12e::hw
