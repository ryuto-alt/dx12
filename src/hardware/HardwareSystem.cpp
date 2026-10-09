#include "hardware/HardwareSystem.h"

#include "core/Logger.h"
#include "hardware/HwSerialTransport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iterator>

namespace dx12e::hw
{

namespace
{
using Clock = std::chrono::steady_clock;

// システム内の単調時計（ms）。0 を「未受信」の印に使うので 1 から始める。
double NowMs()
{
    static const Clock::time_point t0 = Clock::now();
    return 1.0 + std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

constexpr size_t kLogCapacity = 200;
constexpr double kPingIntervalMs = 200.0;
constexpr double kReadyWaitMs = 2000.0;

// 実効レンジ（校正値 > 宣言）。レンジが無ければ false（生値をそのまま使う）。
bool EffRange(const HwChannelState& c, double& mn, double& mx)
{
    const bool ranged = c.cfg.min.has_value() || c.cfg.max.has_value() || c.decl.hasRange;
    mn = c.cfg.min.has_value() ? *c.cfg.min : (c.decl.hasRange ? c.decl.min : 0.0);
    mx = c.cfg.max.has_value() ? *c.cfg.max : (c.decl.hasRange ? c.decl.max : 1.0);
    return ranged;
}

double Clamp01(double v)
{
    if (!std::isfinite(v)) return 0.0;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// 生値 → 0..1（deadzone / 平滑の前）
double Normalize(const HwChannelState& c, double raw)
{
    double mn = 0, mx = 1;
    if (!EffRange(c, mn, mx)) return raw;
    if (mx == mn) return 0.0;
    return Clamp01((raw - mn) / (mx - mn));
}

// 0..1 → 生値（出力用）
double ToRaw(const HwChannelState& c, double v01)
{
    if (c.decl.type == HwChType::Bool) return v01 >= 0.5 ? 1.0 : 0.0;
    double mn = 0, mx = 1;
    double raw = v01;
    if (EffRange(c, mn, mx)) raw = mn + v01 * (mx - mn);
    if (c.decl.type == HwChType::Int) raw = std::round(raw);
    return raw;
}

double ApplyDeadzone(double n, double dz)
{
    if (dz <= 0.0) return n;
    if (n <= dz) return 0.0;
    if (n >= 1.0 - dz) return 1.0;
    return (n - dz) / (1.0 - 2.0 * dz);
}

bool IEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool IsVirtualPort(const std::string& p) { return p.rfind(kVirtualPortPrefix, 0) == 0; }
} // namespace

const char* HwStatusName(HwStatus s)
{
    switch (s)
    {
    case HwStatus::Disconnected: return "disconnected";
    case HwStatus::Opening:      return "opening";
    case HwStatus::WaitingHello: return "waiting_hello";
    case HwStatus::Ready:        return "ready";
    case HwStatus::Error:        return "error";
    }
    return "unknown";
}

// ===========================================================================
// 起動・停止
// ===========================================================================

HardwareSystem::HardwareSystem() = default;
HardwareSystem::~HardwareSystem() { Shutdown(); }

HwDevice& HardwareSystem::AddSlotLocked(const HwDeviceConfig& cfg, bool fromConfig)
{
    auto d = std::make_unique<HwDevice>();
    d->name = cfg.name;
    d->cfg = cfg;
    d->fromConfig = fromConfig;
    m_devices.push_back(std::move(d));
    return *m_devices.back();
}

void HardwareSystem::Initialize(const HwConfig& cfg, const HwSystemOptions& opts)
{
    Shutdown();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_opts = opts;
        m_devices.clear();
        m_cooldown.clear();
        for (const auto& dc : cfg.devices)
        {
            if (dc.transport != "serial")
                Logger::Warn("[hw] デバイス {} の transport \"{}\" は未対応です（serial のみ）", dc.name, dc.transport);
            AddSlotLocked(dc, true);
        }
    }
    m_stop.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { IoThreadMain(); });
}

void HardwareSystem::Shutdown()
{
    if (m_thread.joinable())
    {
        m_stop.store(true);
        m_thread.join();   // IO スレッドの終端で !safe → close する
    }
    m_running.store(false);
    std::lock_guard<std::mutex> lk(m_mutex);
    m_devices.clear();
}

// ===========================================================================
// 検索
// ===========================================================================

HwDevice* HardwareSystem::FindLocked(const std::string& name)
{
    for (auto& d : m_devices) if (d->name == name) return d.get();
    return nullptr;
}
const HwDevice* HardwareSystem::FindLocked(const std::string& name) const
{
    for (const auto& d : m_devices) if (d->name == name) return d.get();
    return nullptr;
}
HwChannelState* HardwareSystem::FindChLocked(HwDevice& d, const std::string& ch)
{
    auto it = d.chIndex.find(ch);
    return it == d.chIndex.end() ? nullptr : &d.channels[it->second];
}
const HwChannelState* HardwareSystem::FindChLocked(const HwDevice& d, const std::string& ch) const
{
    auto it = d.chIndex.find(ch);
    return it == d.chIndex.end() ? nullptr : &d.channels[it->second];
}

// ===========================================================================
// フレーム
// ===========================================================================

void HardwareSystem::FlushOutputsLocked()
{
    for (auto& dp : m_devices)
    {
        HwDevice& d = *dp;
        if (d.status != HwStatus::Ready || d.pendingOut.empty()) continue;   // Ready になるまで溜めたままにする
        std::vector<std::pair<std::string, double>> kv;
        for (const auto& [ch, raw] : d.pendingOut)
        {
            auto it = d.lastSent.find(ch);
            if (it != d.lastSent.end() && it->second == raw) continue;   // 変化なしは送らない
            kv.emplace_back(ch, raw);
            d.lastSent[ch] = raw;
        }
        d.pendingOut.clear();
        for (auto& l : HwFormatSetLines(kv)) d.sendQueue.push_back(std::move(l));
    }
}

void HardwareSystem::FlushOutputs()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    FlushOutputsLocked();
}

void HardwareSystem::BeginFrame()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    FlushOutputsLocked();

    for (auto& dp : m_devices)
    {
        for (auto& c : dp->channels)
        {
            if (c.decl.isOutput) continue;
            const bool have = c.simulated || (c.hasValue && dp->status == HwStatus::Ready);
            const double raw = !have ? 0.0 : (c.simulated ? c.simRaw : c.latestRaw);

            c.prevDownF = c.downF;
            c.rawF = raw;
            if (!have)
            {
                c.valueF = 0.0;
                c.downF = false;
                c.smoothInit = false;
            }
            else if (c.decl.type == HwChType::Bool)
            {
                // bool: raw!=0。invert は「押すと 0」になるプルアップ入力のため（down を反転）
                c.downF = (raw != 0.0) != c.cfg.invert;
                c.valueF = c.downF ? 1.0 : 0.0;
            }
            else
            {
                double n = Normalize(c, raw);
                if (c.cfg.invert) n = 1.0 - n;
                n = ApplyDeadzone(n, c.cfg.deadzone);
                if (!c.smoothInit || c.cfg.smooth <= 0.0) { c.valueF = n; c.smoothInit = true; }
                else c.valueF = c.cfg.smooth * c.valueF + (1.0 - c.cfg.smooth) * n;
                c.downF = c.valueF >= 0.5;
            }
            c.pressedF  = c.downF && !c.prevDownF;
            c.releasedF = !c.downF && c.prevDownF;
        }
    }
}

// ===========================================================================
// 読む
// ===========================================================================

bool HardwareSystem::IsConnected(const std::string& dev) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    return d && d->status == HwStatus::Ready;
}

double HardwareSystem::Get(const std::string& dev, const std::string& ch) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    const HwChannelState* c = d ? FindChLocked(*d, ch) : nullptr;
    return c ? c->valueF : 0.0;
}

double HardwareSystem::GetRaw(const std::string& dev, const std::string& ch) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    const HwChannelState* c = d ? FindChLocked(*d, ch) : nullptr;
    return c ? c->rawF : 0.0;
}

bool HardwareSystem::Down(const std::string& dev, const std::string& ch) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    const HwChannelState* c = d ? FindChLocked(*d, ch) : nullptr;
    return c && c->downF;
}

bool HardwareSystem::Pressed(const std::string& dev, const std::string& ch) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    const HwChannelState* c = d ? FindChLocked(*d, ch) : nullptr;
    return c && c->pressedF;
}

bool HardwareSystem::Released(const std::string& dev, const std::string& ch) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    const HwChannelState* c = d ? FindChLocked(*d, ch) : nullptr;
    return c && c->releasedF;
}

// ===========================================================================
// 書く
// ===========================================================================

bool HardwareSystem::Set(const std::string& dev, const std::string& ch, double value01)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(dev);
    if (!d || d->status != HwStatus::Ready) return false;
    HwChannelState* c = FindChLocked(*d, ch);
    if (!c || !c->decl.isOutput) return false;

    double v = Clamp01(value01);
    if (c->cfg.dangerous) v = std::min(v, c->cfg.maxValue);
    c->outNorm = v;
    d->pendingOut[ch] = ToRaw(*c, v);
    return true;
}

void HardwareSystem::ResetOutputs()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& dp : m_devices)
    {
        HwDevice& d = *dp;
        d.pendingOut.clear();
        d.sendQueue.clear();
        d.lastSent.clear();
        for (auto& c : d.channels)
            if (c.decl.isOutput) c.outNorm = Normalize(c, c.decl.safe);
        if (d.status == HwStatus::Ready) d.safeRequested = true;
    }
}

// ===========================================================================
// 接続
// ===========================================================================

bool HardwareSystem::Connect(const std::string& dev, const std::string& port)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(dev);
    if (!d) return false;
    d->userDisabled = false;
    d->connectRequested = true;
    d->connectPort = port;
    return true;
}

bool HardwareSystem::Disconnect(const std::string& dev)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(dev);
    if (!d) return false;
    d->userDisabled = true;
    d->closeRequested = true;
    d->connectRequested = false;
    return true;
}

// ===========================================================================
// 状態
// ===========================================================================

HwDeviceInfo HardwareSystem::MakeInfoLocked(const HwDevice& d, double nowMs) const
{
    HwDeviceInfo i;
    i.name = d.name;
    i.status = d.status;
    i.connected = d.status == HwStatus::Ready;
    i.isVirtual = d.loopback != nullptr;
    i.userDisabled = d.userDisabled;
    i.port = d.port;
    i.describe = d.describe;
    i.lastError = d.lastError;
    i.helloName = d.helloName;
    i.board = d.board;
    i.fw = d.fw;
    i.proto = d.proto;
    i.stats = d.stats;
    i.stats.rttMs = d.rttMs;
    i.stats.lastRxAgeMs = d.lastRxMs > 0.0 ? nowMs - d.lastRxMs : -1.0;
    i.calibrating = d.calibrating;
    i.sampling = d.sampling;
    for (const auto& c : d.channels)
    {
        HwChannelInfo ci;
        ci.name = c.decl.name;
        ci.isOutput = c.decl.isOutput;
        ci.type = c.decl.type;
        double mn = 0, mx = 1;
        ci.hasRange = EffRange(c, mn, mx);
        ci.min = mn; ci.max = mx;
        ci.raw = c.simulated ? c.simRaw : c.latestRaw;
        ci.value = c.valueF;
        ci.hasValue = c.hasValue || c.simulated;
        ci.simulated = c.simulated;
        ci.dangerous = c.cfg.dangerous;
        ci.maxValue = c.cfg.maxValue;
        ci.outValue = c.outNorm;
        i.channels.push_back(std::move(ci));
    }
    return i;
}

std::vector<HwDeviceInfo> HardwareSystem::ListDevices() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const double now = NowMs();
    std::vector<HwDeviceInfo> out;
    for (const auto& d : m_devices) out.push_back(MakeInfoLocked(*d, now));
    return out;
}

bool HardwareSystem::GetDeviceInfo(const std::string& dev, HwDeviceInfo& out) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    if (!d) return false;
    out = MakeInfoLocked(*d, NowMs());
    return true;
}

std::vector<HwLogEntry> HardwareSystem::GetLog(const std::string& dev, size_t n) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(dev);
    std::vector<HwLogEntry> out;
    if (!d) return out;
    const size_t start = d->log.size() > n ? d->log.size() - n : 0;
    for (size_t i = start; i < d->log.size(); ++i) out.push_back(d->log[i]);
    return out;
}

std::vector<HwActionBinding> HardwareSystem::GetActionBindings() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<HwActionBinding> out;
    for (const auto& d : m_devices)
        for (const auto& [ch, act] : d->cfg.actions) out.push_back({d->name, ch, act});
    return out;
}

HwConfig HardwareSystem::Config() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwConfig c;
    for (const auto& d : m_devices)
    {
        if (!d->fromConfig) continue;
        c.devices.push_back(d->cfg);
        // AddVirtualDevice が設定のデバイスへ割り当てた仮想ポート（virtual:名前）は、書き戻す設定へ持ち込まない
        // （hardware.json に残ると実機をつないだときに照合できなくなる）
        std::string& p = c.devices.back().match.port;
        if (d->loopback && p.rfind(kVirtualPortPrefix, 0) == 0) p.clear();
    }
    return c;
}

// ===========================================================================
// 仮想デバイス・シミュレーション
// ===========================================================================

std::shared_ptr<HwLoopbackDevice> HardwareSystem::AddVirtualDevice(const std::string& name,
                                                                   const std::vector<HwChannelDecl>& channels)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(name);
    if (d && d->loopback) return nullptr;
    auto lb = std::make_shared<HwLoopbackDevice>(name, channels);
    if (d)
    {
        d->loopback = lb;
        d->cfg.match.port = std::string(kVirtualPortPrefix) + name;
    }
    else
    {
        HwDeviceConfig cfg;
        cfg.name = name;
        cfg.transport = "loopback";
        cfg.match.port = std::string(kVirtualPortPrefix) + name;
        AddSlotLocked(cfg, false).loopback = lb;
    }
    return lb;
}

std::shared_ptr<HwLoopbackDevice> HardwareSystem::GetVirtualDevice(const std::string& name) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const HwDevice* d = FindLocked(name);
    return d ? d->loopback : nullptr;
}

bool HardwareSystem::Simulate(const std::string& dev, const std::string& ch, double raw)
{
    std::shared_ptr<HwLoopbackDevice> lb;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        HwDevice* d = FindLocked(dev);
        if (!d) return false;
        if (!d->loopback)
        {
            HwChannelState* c = FindChLocked(*d, ch);
            if (!c || c->decl.isOutput) return false;
            c->simulated = true;
            c->simRaw = raw;
            return true;
        }
        lb = d->loopback;
    }
    return lb->SetInput(ch, raw);   // 偽ファームの入力として、実機と同じ経路（IO スレッド）で届く
}

void HardwareSystem::ClearSimulation(const std::string& dev, const std::string& ch)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(dev);
    if (!d) return;
    for (auto& c : d->channels)
        if (ch.empty() || c.decl.name == ch) c.simulated = false;
}

// ===========================================================================
// 校正・サンプル
// ===========================================================================

bool HardwareSystem::BeginCalibration(const std::string& dev)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(dev);
    if (!d) return false;
    d->calibrating = true;
    for (auto& c : d->channels)
    {
        c.calMin = std::numeric_limits<double>::infinity();
        c.calMax = -std::numeric_limits<double>::infinity();
    }
    return true;
}

std::map<std::string, HwCalRange> HardwareSystem::EndCalibration(const std::string& dev)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::map<std::string, HwCalRange> out;
    HwDevice* d = FindLocked(dev);
    if (!d) return out;
    d->calibrating = false;
    for (const auto& c : d->channels)
        if (!c.decl.isOutput && c.calMin <= c.calMax) out[c.decl.name] = {c.calMin, c.calMax};
    return out;
}

bool HardwareSystem::SetChannelRange(const std::string& dev, const std::string& ch, double min, double max)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(dev);
    if (!d) return false;
    HwChannelConfig& cc = d->cfg.channels[ch];
    cc.min = min;
    cc.max = max;
    if (HwChannelState* c = FindChLocked(*d, ch)) { c->cfg.min = min; c->cfg.max = max; c->smoothInit = false; }
    return true;
}

bool HardwareSystem::BeginSampling(const std::string& dev)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    HwDevice* d = FindLocked(dev);
    if (!d) return false;
    d->sampling = true;
    for (auto& c : d->channels) { c.sCount = 0; c.sMin = c.sMax = c.sMean = c.sM2 = 0.0; }
    return true;
}

std::map<std::string, HwSampleStats> HardwareSystem::EndSampling(const std::string& dev)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::map<std::string, HwSampleStats> out;
    HwDevice* d = FindLocked(dev);
    if (!d) return out;
    d->sampling = false;
    for (const auto& c : d->channels)
    {
        if (c.decl.isOutput || c.sCount == 0) continue;
        HwSampleStats s;
        s.count = c.sCount;
        s.min = c.sMin; s.max = c.sMax; s.mean = c.sMean;
        s.stddev = std::sqrt(c.sM2 / static_cast<double>(c.sCount));
        out[c.decl.name] = s;
    }
    return out;
}

// ===========================================================================
// IO スレッド
// ===========================================================================

void HardwareSystem::PushLogLocked(HwDevice& d, bool outgoing, const std::string& text)
{
    d.log.push_back({NowMs() / 1000.0, outgoing, text});
    while (d.log.size() > kLogCapacity) d.log.pop_front();
}

std::vector<HwPortInfo> HardwareSystem::ListPorts()
{
    std::vector<HwPortInfo> ports;
    if (m_opts.lister) ports = m_opts.lister();
    else if (m_opts.scanRealPorts) ports = EnumerateComPorts();

    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& d : m_devices)
    {
        if (!d->loopback || !d->loopback->IsPresent()) continue;
        HwPortInfo p;
        p.portName = std::string(kVirtualPortPrefix) + d->name;
        p.friendlyName = "仮想デバイス " + d->name;
        p.guessedBoard = "loopback";
        ports.push_back(std::move(p));
    }
    return ports;
}

std::unique_ptr<IHwTransport> HardwareSystem::MakeTransport(const std::string& portName, int baud)
{
    if (m_opts.factory) return m_opts.factory(portName, baud);
    if (IsVirtualPort(portName))
    {
        std::shared_ptr<HwLoopbackDevice> lb;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            const HwDevice* d = FindLocked(portName.substr(std::char_traits<char>::length(kVirtualPortPrefix)));
            if (d) lb = d->loopback;
        }
        if (!lb) return nullptr;
        return std::make_unique<HwLoopbackTransport>(std::move(lb));
    }
    return std::make_unique<HwSerialTransport>(portName, baud);
}

void HardwareSystem::IoThreadMain()
{
    double lastScan = -1e9;
    std::vector<HwPortInfo> ports;

    std::string lastErr;   // 同じ文言の例外はログに 1 回だけ（毎ループのスパム防止）
    while (!m_stop.load())
    {
        // ★IO スレッドの最上位。例外が漏れると std::terminate でエンジンごと落ちるので、1 周ぶんを丸ごと受けて続行する。
        try
        {
            const double now = NowMs();
            std::vector<HwDevice*> devs;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                for (auto& d : m_devices) devs.push_back(d.get());
            }

            const bool scanTick = now - lastScan >= static_cast<double>(m_opts.scanIntervalMs);
            if (scanTick) { ports = ListPorts(); lastScan = now; }

            bool activity = false;
            for (HwDevice* d : devs) activity |= ServiceDevice(*d, now, ports, scanTick);

            if (!activity) std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        catch (const std::exception& e)
        {
            const std::string msg = e.what();
            if (msg != lastErr) { lastErr = msg; Logger::Error("[hw] IO スレッドで例外（続行します）: {}", msg); }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        catch (...)
        {
            if (lastErr != "?") { lastErr = "?"; Logger::Error("[hw] IO スレッドで不明な例外（続行します）"); }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // 終了: 出力を安全値へ戻してから閉じる
    try
    {
        std::vector<HwDevice*> devs;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto& d : m_devices) devs.push_back(d.get());
        }
        for (HwDevice* d : devs)
            if (d->transport) CloseDevice(*d, "終了", true, false);
    }
    catch (...) {}
}

bool HardwareSystem::ServiceDevice(HwDevice& d, double now, const std::vector<HwPortInfo>& ports, bool scanTick)
{
    bool activity = false;

    bool closeReq = false, connReq = false, disabled = false;
    std::string connPort;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        closeReq = d.closeRequested;     d.closeRequested = false;
        connReq  = d.connectRequested;   d.connectRequested = false;
        connPort = d.connectPort;
        disabled = d.userDisabled;
    }

    if (d.transport)
    {
        std::string curPort;
        { std::lock_guard<std::mutex> lk(m_mutex); curPort = d.port; }
        if (closeReq || (connReq && !connPort.empty() && connPort != curPort))
        {
            CloseDevice(d, closeReq ? "切断しました" : "接続先を切り替えます", true, false);
            activity = true;
        }
    }

    if (!d.transport)
    {
        if (connReq) TryOpen(d, now, ports, connPort);
        else if (!disabled && scanTick) TryOpen(d, now, ports, {});
        return activity || d.transport != nullptr;
    }

    // ---- 受信 ----
    uint8_t buf[512];
    for (int iter = 0; iter < 8 && d.transport; ++iter)
    {
        const int n = d.transport->Read(buf, static_cast<int>(sizeof(buf)));
        if (n < 0)
        {
            CloseDevice(d, "読み取りエラー（切断された可能性）: " + d.transport->LastError(), false, true, 1000.0);
            return true;
        }
        if (n == 0) break;
        activity = true;

        std::vector<std::string> lines;
        d.assembler.Feed(buf, static_cast<size_t>(n), lines);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            d.stats.rxBytes += static_cast<uint64_t>(n);
            d.stats.overflow = d.assembler.OverflowCount();
        }
        for (const auto& l : lines)
        {
            OnLine(d, l, now);
            if (!d.pendingFail.empty()) break;
        }
        if (!d.pendingFail.empty()) break;
        if (n < static_cast<int>(sizeof(buf))) break;
    }
    if (!d.transport) return true;

    // ---- タイマー ----
    HwStatus st;
    double lastRx;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        st = d.status;
        lastRx = d.lastRxMs;
    }
    if (d.pendingFail.empty())
    {
        if (!d.helloReceived)
        {
            if (!d.helloAsked && now - d.openedAtMs > m_opts.helloWaitMs)
            {
                SendLine(d, HwFormatHello());
                d.helloAsked = true;
                d.helloAskedAtMs = now;
            }
            else if (d.helloAsked && now - d.helloAskedAtMs > m_opts.helloRetryMs)
                d.pendingFail = "@hello が来ませんでした（プロトコル v1 のファームではない可能性）";
        }
        else
        {
            if (st != HwStatus::Ready && now - d.helloAtMs > kReadyWaitMs)
                d.pendingFail = "@ready が来ませんでした";
            else if (lastRx > 0.0 && now - lastRx > m_opts.silenceTimeoutMs)
                d.pendingFail = "無受信が続いたため切断とみなしました";
        }
    }

    // ---- 心拍（IO スレッドから。メインスレッドの引っかかりで切れないように） ----
    if (d.pendingFail.empty() && d.helloReceived && now - d.lastPingMs >= kPingIntervalMs)
    {
        const int seq = ++d.pingSeq;
        d.pingSent[seq] = now;
        while (d.pingSent.size() > 16) d.pingSent.erase(d.pingSent.begin());
        d.lastPingMs = now;
        SendLine(d, HwFormatPing(seq));
    }

    // ---- 送信 ----
    if (d.pendingFail.empty())
    {
        bool safe = false;
        std::vector<std::string> out;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (d.status == HwStatus::Ready)
            {
                safe = d.safeRequested;
                d.safeRequested = false;
                out.swap(d.sendQueue);
                st = d.status;
            }
        }
        if (st == HwStatus::Ready)
        {
            if (!d.pinsSent)
            {
                d.pinsSent = true;
                for (const auto& p : d.cfg.pins) SendLine(d, HwFormatPin(p.pin, p.mode, p.name));
            }
            if (safe) SendLine(d, HwFormatSafe());
            for (const auto& l : out) SendLine(d, l);
            if (!out.empty() || safe) activity = true;
        }
    }

    // ---- 受信レート ----
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (d.rateWindowStartMs == 0.0) d.rateWindowStartMs = now;
        const double span = now - d.rateWindowStartMs;
        if (span >= 1000.0)
        {
            d.stats.rxRateHz = static_cast<double>(d.rateWindowLines) * 1000.0 / span;
            d.rateWindowStartMs = now;
            d.rateWindowLines = 0;
        }
    }

    if (!d.pendingFail.empty())
    {
        const std::string r = std::move(d.pendingFail);
        d.pendingFail.clear();
        CloseDevice(d, r, true, true, 5000.0);
        return true;
    }
    return activity;
}

void HardwareSystem::TryOpen(HwDevice& d, double now, const std::vector<HwPortInfo>& ports, const std::string& explicitPort)
{
    std::vector<std::string> used;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto& o : m_devices)
            if (o.get() != &d && !o->port.empty()) used.push_back(o->port);
    }
    auto isUsed = [&](const std::string& p) { return std::find(used.begin(), used.end(), p) != used.end(); };

    std::vector<std::string> candidates;
    if (!explicitPort.empty())
    {
        m_cooldown.erase(explicitPort);   // 人が明示した接続は待たせない
        if (!isUsed(explicitPort)) candidates.push_back(explicitPort);
    }
    else
    {
        const HwMatchConfig& m = d.cfg.match;
        for (const auto& p : ports)
        {
            if (isUsed(p.portName)) continue;
            auto cd = m_cooldown.find(p.portName);
            if (cd != m_cooldown.end() && cd->second > now) continue;
            if (!m.port.empty() && !IEquals(m.port, p.portName)) continue;
            if (!m.vid.empty() && !IEquals(m.vid, p.vid)) continue;
            if (!m.pid.empty() && !IEquals(m.pid, p.pid)) continue;
            // port 指定が無ければ USB シリアル（VID が取れるもの）だけを開いて名乗りを待つ。
            // ★Bluetooth の仮想 COM やマザーボードの COM1 は VID が無い。前者は CreateFile が数秒固まり、
            //   無関係な Arduino も開くだけでリセットされるので、手当たり次第には開かない。
            if (m.port.empty() && p.vid.empty() && !IsVirtualPort(p.portName)) continue;
            candidates.push_back(p.portName);
        }
    }

    for (const auto& portName : candidates)
    {
        std::unique_ptr<IHwTransport> t = MakeTransport(portName, d.cfg.baud);
        if (!t) continue;
        { std::lock_guard<std::mutex> lk(m_mutex); d.status = HwStatus::Opening; }

        if (!t->Open())
        {
            const std::string err = portName + ": " + t->LastError();
            m_cooldown[portName] = now + 3000.0;
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                changed = d.lastError != err;
                d.status = HwStatus::Error;
                d.lastError = err;
            }
            if (changed) Logger::Warn("[hw:{}] 開けません: {}", d.name, err);
            continue;
        }

        d.transport = std::move(t);
        d.assembler.Reset();
        d.pingSent.clear();
        d.openedAtMs = now;
        d.helloAsked = d.helloReceived = d.pinsSent = false;
        d.helloAskedAtMs = d.helloAtMs = d.lastPingMs = 0.0;
        d.rateWindowStartMs = 0.0;
        d.rateWindowLines = 0;
        d.pendingFail.clear();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            d.status = HwStatus::WaitingHello;
            d.port = portName;
            d.describe = d.transport->Describe();
            d.lastError.clear();
            d.channels.clear();
            d.chIndex.clear();
            d.helloName.clear(); d.board.clear(); d.fw = 0; d.proto = 0;
            d.lastRxMs = 0.0;
            d.rttMs = -1.0;
            d.pendingOut.clear(); d.lastSent.clear(); d.sendQueue.clear();
            d.safeRequested = false;
            d.stats.rxRateHz = 0.0;
            PushLogLocked(d, true, "(opened " + d.describe + ")");
        }
        Logger::Info("[hw:{}] 開きました: {}", d.name, d.describe);
        return;
    }
}

void HardwareSystem::CloseDevice(HwDevice& d, const std::string& reason, bool sendSafe, bool isError, double cooldownMs)
{
    std::string portName;
    if (d.transport)
    {
        portName = d.transport->PortName();
        if (sendSafe && d.helloReceived && d.transport->IsOpen()) d.transport->Write("!safe\n");
        d.transport->Close();
        d.transport.reset();
        if (cooldownMs > 0.0) m_cooldown[portName] = NowMs() + cooldownMs;
    }
    d.assembler.Reset();
    d.pingSent.clear();
    d.helloAsked = d.helloReceived = d.pinsSent = false;
    d.pendingFail.clear();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        d.status = isError ? HwStatus::Error : HwStatus::Disconnected;
        d.lastError = isError ? reason : std::string();
        d.port.clear();
        d.describe.clear();
        for (auto& c : d.channels) { c.hasValue = false; }
        d.pendingOut.clear(); d.lastSent.clear(); d.sendQueue.clear();
        d.safeRequested = false;
        d.stats.rxRateHz = 0.0;
        d.rttMs = -1.0;
        PushLogLocked(d, true, "(closed: " + reason + ")");
    }
    if (isError) Logger::Warn("[hw:{}] 閉じました: {}", d.name, reason);
    else         Logger::Info("[hw:{}] 閉じました: {}", d.name, reason);
}

bool HardwareSystem::SendLine(HwDevice& d, const std::string& text)
{
    if (!d.transport) return false;
    const bool ok = d.transport->Write(text + "\n");
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!ok)
    {
        if (d.pendingFail.empty()) d.pendingFail = "書き込みに失敗しました: " + d.transport->LastError();
        return false;
    }
    ++d.stats.txLines;
    if (text.rfind("?ping", 0) != 0) PushLogLocked(d, true, text);   // 心拍は 5 回/秒で流れを埋めるのでログに載せない
    return true;
}

void HardwareSystem::OnLine(HwDevice& d, const std::string& line, double now)
{
    const HwMessage m = ParseDeviceLine(line);
    std::lock_guard<std::mutex> lk(m_mutex);

    ++d.stats.rxLines;
    ++d.rateWindowLines;
    d.lastRxMs = now;
    if (m.type != HwMsgType::Pong) PushLogLocked(d, false, line);

    switch (m.type)
    {
    case HwMsgType::Pong:
    {
        auto it = d.pingSent.find(m.seq);
        if (it != d.pingSent.end())
        {
            d.rttMs = NowMs() - it->second;
            d.pingSent.erase(d.pingSent.begin(), std::next(it));
        }
        break;
    }
    case HwMsgType::Hello:
    {
        if (m.proto != 0 && m.proto != 1)
        {
            ++d.stats.errors;
            d.pendingFail = "非対応のプロトコル版です (proto=" + std::to_string(m.proto) + ")";
            break;
        }
        const std::string expect = !d.cfg.match.hello.empty() ? d.cfg.match.hello
                                 : (d.cfg.match.Empty() ? d.name : std::string());
        if (!expect.empty() && m.name != expect)
        {
            d.pendingFail = "@hello の名前が違います (期待 " + expect + " / 実際 " + m.name + ")";
            break;
        }
        d.helloName = m.name;
        d.board = m.board;
        d.fw = m.fw;
        d.proto = m.proto == 0 ? 1 : m.proto;
        d.channels.clear();
        d.chIndex.clear();
        d.status = HwStatus::WaitingHello;   // 宣言を受け直す（ファームが再起動したときも）
        d.helloReceived = true;
        d.helloAtMs = now;
        break;
    }
    case HwMsgType::Channel:
    {
        if (!d.helloReceived) break;
        HwChannelState c;
        c.decl = m.channel;
        auto cfgIt = d.cfg.channels.find(c.decl.name);
        if (cfgIt != d.cfg.channels.end()) c.cfg = cfgIt->second;
        if (c.decl.isOutput) c.outNorm = Normalize(c, c.decl.safe);
        auto idx = d.chIndex.find(c.decl.name);
        if (idx != d.chIndex.end()) d.channels[idx->second] = std::move(c);   // ?pin への応答などで上書き
        else { d.chIndex[c.decl.name] = d.channels.size(); d.channels.push_back(std::move(c)); }
        break;
    }
    case HwMsgType::Ready:
        if (d.helloReceived)
        {
            d.status = HwStatus::Ready;
            d.lastSent.clear();
        }
        break;
    case HwMsgType::Values:
        if (d.status != HwStatus::Ready) break;
        for (const auto& [name, v] : m.values)
        {
            HwChannelState* c = FindChLocked(d, name);
            if (!c || c->decl.isOutput) continue;
            c->latestRaw = v;
            c->hasValue = true;
            if (d.calibrating)
            {
                c->calMin = std::min(c->calMin, v);
                c->calMax = std::max(c->calMax, v);
            }
            if (d.sampling)
            {
                if (c->sCount == 0) { c->sMin = c->sMax = v; }
                c->sMin = std::min(c->sMin, v);
                c->sMax = std::max(c->sMax, v);
                ++c->sCount;
                const double delta = v - c->sMean;
                c->sMean += delta / static_cast<double>(c->sCount);
                c->sM2 += delta * (v - c->sMean);
            }
        }
        break;
    case HwMsgType::Log:
        Logger::Info("[hw:{}] {}", d.name, m.text);
        break;
    case HwMsgType::Err:
        ++d.stats.errors;
        Logger::Warn("[hw:{}] {}", d.name, m.text);
        break;
    case HwMsgType::Unknown:
        ++d.stats.badLines;
        break;
    }
}

} // namespace dx12e::hw
