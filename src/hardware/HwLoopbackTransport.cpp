#include "hardware/HwLoopbackTransport.h"

#include <algorithm>
#include <cstring>

namespace dx12e::hw
{

HwLoopbackDevice::HwLoopbackDevice(std::string name, std::vector<HwChannelDecl> channels, int fw, std::string board)
    : m_name(std::move(name)), m_channels(std::move(channels)), m_fw(fw), m_board(std::move(board))
{
    for (const auto& c : m_channels)
    {
        if (c.isOutput) m_outputs[c.name] = c.safe;
        else            m_inputs[c.name]  = 0.0;
    }
}

void HwLoopbackDevice::QueueLocked(const std::string& line)
{
    m_out += line;
    m_out += '\n';
}

void HwLoopbackDevice::QueueBootLocked()
{
    QueueLocked("@hello name=" + m_name + " proto=1 fw=" + std::to_string(m_fw) + " board=" + m_board);
    for (const auto& c : m_channels)
    {
        std::string l = "@ch " + c.name + (c.isOutput ? " out " : " in ") + HwChTypeName(c.type);
        if (c.hasRange) l += " " + HwFormatValue(c.min) + " " + HwFormatValue(c.max);
        if (c.isOutput && c.safe != 0.0) l += " safe=" + HwFormatValue(c.safe);
        QueueLocked(l);
    }
    QueueLocked("@ready");
    QueueInputsLocked();
}

void HwLoopbackDevice::QueueInputsLocked()
{
    std::vector<std::pair<std::string, double>> kv(m_inputs.begin(), m_inputs.end());
    if (kv.empty()) return;
    for (const auto& l : HwFormatSetLines(kv))
        QueueLocked(l.substr(1));   // 先頭の '!' を外す（デバイス → エンジンの値行）
}

void HwLoopbackDevice::SetAllSafeLocked()
{
    for (const auto& c : m_channels)
        if (c.isOutput) m_outputs[c.name] = c.safe;
}

void HwLoopbackDevice::CheckHeartbeatLocked()
{
    const auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastHeartbeat).count() <= m_hbTimeoutMs) return;
    bool allSafe = true;
    for (const auto& c : m_channels)
        if (c.isOutput && m_outputs[c.name] != c.safe) { allSafe = false; break; }
    if (!allSafe) { SetAllSafeLocked(); ++m_safeTrips; }
}

bool HwLoopbackDevice::OnOpen()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_present) return false;
    m_open = true;
    m_out.clear();
    m_asm.Reset();
    m_lastHeartbeat = Clock::now();
    QueueBootLocked();
    return true;
}

void HwLoopbackDevice::OnClose()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_open = false;
    m_out.clear();
}

int HwLoopbackDevice::Read(uint8_t* buf, int max)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_present) return -1;
    CheckHeartbeatLocked();
    if (m_out.empty()) return 0;
    const int n = static_cast<int>(std::min<size_t>(m_out.size(), static_cast<size_t>(max)));
    std::memcpy(buf, m_out.data(), static_cast<size_t>(n));
    m_out.erase(0, static_cast<size_t>(n));
    return n;
}

void HwLoopbackDevice::HandleLineLocked(const std::string& line)
{
    m_received.push_back(line);
    if (line.empty()) return;

    if (line == "?hello") { QueueBootLocked(); return; }
    if (line.rfind("?ping", 0) == 0)
    {
        m_lastHeartbeat = Clock::now();
        const size_t sp = line.find(' ');
        QueueLocked("@pong " + (sp == std::string::npos ? std::string("0") : line.substr(sp + 1)));
        return;
    }
    if (line.rfind("?pin", 0) == 0) return;   // 汎用スケッチ専用。仮想デバイスは宣言済みのチャンネルしか持たない
    if (line == "!safe") { SetAllSafeLocked(); return; }
    if (line[0] == '!')
    {
        const HwMessage m = ParseDeviceLine(std::string_view(line).substr(1));
        if (m.type != HwMsgType::Values) { QueueLocked("@err bad output line"); return; }
        for (const auto& [k, v] : m.values)
        {
            auto it = m_outputs.find(k);
            if (it != m_outputs.end()) it->second = v;
        }
        return;
    }
}

bool HwLoopbackDevice::Write(std::string_view data)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_present || !m_open) return false;
    CheckHeartbeatLocked();
    std::vector<std::string> lines;
    m_asm.Feed(data.data(), data.size(), lines);
    for (const auto& l : lines) HandleLineLocked(l);
    return true;
}

bool HwLoopbackDevice::SetInput(const std::string& ch, double raw)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_inputs.find(ch);
    if (it == m_inputs.end()) return false;
    it->second = raw;
    if (m_open && m_present) QueueLocked(ch + "=" + HwFormatValue(raw));
    return true;
}

bool HwLoopbackDevice::GetInput(const std::string& ch, double& raw) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_inputs.find(ch);
    if (it == m_inputs.end()) return false;
    raw = it->second;
    return true;
}

bool HwLoopbackDevice::GetOutput(const std::string& ch, double& raw)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    CheckHeartbeatLocked();
    auto it = m_outputs.find(ch);
    if (it == m_outputs.end()) return false;
    raw = it->second;
    return true;
}

void HwLoopbackDevice::SetPresent(bool present)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_present = present;
    if (!present) { m_open = false; m_out.clear(); }
}

bool HwLoopbackDevice::IsPresent() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_present;
}

void HwLoopbackDevice::SetHeartbeatTimeoutMs(int ms)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_hbTimeoutMs = ms;
}

std::vector<std::string> HwLoopbackDevice::ReceivedLines() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_received;
}

void HwLoopbackDevice::ClearReceived()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_received.clear();
}

int HwLoopbackDevice::SafeTripCount() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_safeTrips;
}

// ---------------------------------------------------------------------------

HwLoopbackTransport::HwLoopbackTransport(std::shared_ptr<HwLoopbackDevice> dev) : m_dev(std::move(dev)) {}
HwLoopbackTransport::~HwLoopbackTransport() { Close(); }

bool HwLoopbackTransport::Open()
{
    Close();
    if (!m_dev->OnOpen()) { m_lastError = "仮想デバイスが抜かれています"; return false; }
    m_open = true;
    return true;
}

void HwLoopbackTransport::Close()
{
    if (m_open) { m_dev->OnClose(); m_open = false; }
}

int HwLoopbackTransport::Read(uint8_t* buf, int max)
{
    if (!m_open) return -1;
    return m_dev->Read(buf, max);
}

bool HwLoopbackTransport::Write(std::string_view data)
{
    return m_open && m_dev->Write(data);
}

std::string HwLoopbackTransport::Describe() const { return PortName() + " (loopback)"; }
std::string HwLoopbackTransport::PortName() const { return std::string(kVirtualPortPrefix) + m_dev->Name(); }

} // namespace dx12e::hw
