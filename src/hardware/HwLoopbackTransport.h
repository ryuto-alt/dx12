#pragma once
// ============================================================================
// 仮想デバイス（プロセス内でプロトコル v1 を話す偽ファーム）。
// テストと MCP の hw_simulate 用。実機なしで Lua / 入力割当を確かめられる。
//   HwLoopbackDevice    … 偽ファーム本体（状態を持つ。スレッド安全）。トランスポートより長生きし、
//                         再接続しても入力値・出力値が残る（実機のファームと同じ）。
//   HwLoopbackTransport … IHwTransport としてそれへつなぐ薄い層。
// ============================================================================

#include "hardware/HwProtocol.h"
#include "hardware/HwTransport.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dx12e::hw
{

class HwLoopbackDevice
{
public:
    HwLoopbackDevice(std::string name, std::vector<HwChannelDecl> channels, int fw = 1, std::string board = "loopback");

    const std::string& Name() const { return m_name; }
    const std::vector<HwChannelDecl>& Channels() const { return m_channels; }

    // ---- トランスポート側 ----
    bool OnOpen();                              // 起動（@hello 一式を送る）。!present なら false
    void OnClose();
    int  Read(uint8_t* buf, int max);           // 無ければ 0。!present なら -1
    bool Write(std::string_view data);

    // ---- 外（テスト / MCP）から ----
    bool SetInput(const std::string& ch, double raw);        // 入力チャンネルの値を流し込む（接続中なら即送信）
    bool GetInput(const std::string& ch, double& raw) const;
    bool GetOutput(const std::string& ch, double& raw);      // 心拍切れの判定込み
    void SetPresent(bool present);                           // false = USB を抜いた状態
    bool IsPresent() const;
    void SetHeartbeatTimeoutMs(int ms);                      // 既定 1000
    std::vector<std::string> ReceivedLines() const;          // エンジンから受けた行（全部）
    void ClearReceived();
    int  SafeTripCount() const;                              // 心拍切れで safe に戻した回数

private:
    void CheckHeartbeatLocked();
    void QueueLocked(const std::string& line);
    void QueueBootLocked();
    void QueueInputsLocked();
    void SetAllSafeLocked();
    void HandleLineLocked(const std::string& line);

    using Clock = std::chrono::steady_clock;

    std::string                m_name;
    std::vector<HwChannelDecl> m_channels;
    int                        m_fw;
    std::string                m_board;

    mutable std::mutex         m_mutex;
    bool                       m_present = true;
    bool                       m_open    = false;
    std::string                m_out;                          // デバイス → エンジンの未読バイト
    std::map<std::string, double> m_inputs;
    std::map<std::string, double> m_outputs;
    Clock::time_point          m_lastHeartbeat = Clock::now();
    int                        m_hbTimeoutMs = 1000;
    int                        m_safeTrips = 0;
    HwLineAssembler            m_asm;
    std::vector<std::string>   m_received;
};

class HwLoopbackTransport final : public IHwTransport
{
public:
    explicit HwLoopbackTransport(std::shared_ptr<HwLoopbackDevice> dev);
    ~HwLoopbackTransport() override;

    bool Open() override;
    void Close() override;
    bool IsOpen() const override { return m_open; }
    int  Read(uint8_t* buf, int max) override;
    bool Write(std::string_view data) override;
    std::string Describe() const override;
    std::string PortName() const override;
    std::string LastError() const override { return m_lastError; }

private:
    std::shared_ptr<HwLoopbackDevice> m_dev;
    bool        m_open = false;
    std::string m_lastError;
};

// 仮想ポート名の接頭辞
inline constexpr const char* kVirtualPortPrefix = "virtual:";

} // namespace dx12e::hw
