// HardwareSystem（src/hardware/）の結合テスト。実機は一切開かず、仮想デバイス（Loopback）と
// 台本どおりに喋る偽トランスポートだけで確かめる。
//   接続→@hello→Ready / Simulate した値の正規化・invert・deadzone・平滑 / Pressed が 1 フレームだけ /
//   Set の合流（同フレーム 3 回→1 行）/ dangerous の maxValue / 心拍途絶で safe / Disconnect→再接続 /
//   自動再接続 / 校正・サンプリング / match（VID）での照合とファクトリ差し替え / hardware.json の読み書き往復。

#include "hardware/HardwareSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

using namespace dx12e::hw;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) { std::printf("FAIL: %s %s (%s:%d)\n", #cond, "" __VA_ARGS__, __FILE__, __LINE__); ++g_failed; } \
    } while (0)
#define CHECKM(cond, msg)                                                      \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s [%s] (%s:%d)\n", #cond, std::string(msg).c_str(), __FILE__, __LINE__); ++g_failed; } } while (0)
#define NEAR(a, b) (std::fabs((a) - (b)) < 1e-6)

static bool WaitFor(const std::function<bool()>& cond, int timeoutMs = 3000)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (!cond())
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// IO スレッドが受け取った最新の生値（BeginFrame を呼ばずに待つため）
static double LiveRaw(HardwareSystem& s, const std::string& dev, const std::string& ch)
{
    HwDeviceInfo i;
    if (!s.GetDeviceInfo(dev, i)) return std::nan("");
    for (const auto& c : i.channels) if (c.name == ch) return c.raw;
    return std::nan("");
}
static bool WaitRaw(HardwareSystem& s, const std::string& dev, const std::string& ch, double v)
{
    return WaitFor([&] { return LiveRaw(s, dev, ch) == v; });
}
static HwStatus StatusOf(HardwareSystem& s, const std::string& dev)
{
    HwDeviceInfo i;
    return s.GetDeviceInfo(dev, i) ? i.status : HwStatus::Error;
}

static std::vector<HwChannelDecl> RadioChannels()
{
    auto in = [](const char* n, HwChType t, bool range, double mn, double mx) {
        HwChannelDecl d; d.name = n; d.type = t; d.hasRange = range; d.min = mn; d.max = mx; return d;
    };
    auto out = [](const char* n, HwChType t, double mn, double mx) {
        HwChannelDecl d; d.name = n; d.isOutput = true; d.type = t; d.hasRange = true; d.min = mn; d.max = mx; return d;
    };
    return {
        in("dial", HwChType::Int, true, 0, 4095),
        in("inv", HwChType::Float, true, 0, 1),
        in("dz", HwChType::Float, true, 0, 1),
        in("btn", HwChType::Bool, false, 0, 1),
        out("led", HwChType::Float, 0, 1),
        out("heat", HwChType::Float, 0, 1),
        out("pwm", HwChType::Int, 0, 255),
    };
}

static HwConfig RadioConfig()
{
    HwConfig cfg;
    HwDeviceConfig d;
    d.name = "radio";
    d.channels["dial"].smooth = 0.5;
    d.channels["inv"].invert = true;
    d.channels["dz"].deadzone = 0.1;
    d.channels["heat"].dangerous = true;
    d.channels["heat"].maxValue = 0.6;
    d.actions["btn"] = "Jump";
    cfg.devices.push_back(d);
    return cfg;
}

static HwSystemOptions FastOptions()
{
    HwSystemOptions o;
    o.scanRealPorts = false;   // 実 COM ポート（ESP32 がつながっている）は絶対に開かない
    o.scanIntervalMs = 50;
    return o;
}

// 接続して全入力が一度届くまで待つ
static std::shared_ptr<HwLoopbackDevice> StartRadio(HardwareSystem& sys, HwConfig cfg = RadioConfig())
{
    sys.Initialize(cfg, FastOptions());
    auto lb = sys.AddVirtualDevice("radio", RadioChannels());
    const bool ready = WaitFor([&] { return sys.IsConnected("radio"); });
    CHECK(ready, "radio が Ready にならない");
    const bool values = WaitFor([&] {
        HwDeviceInfo i;
        if (!sys.GetDeviceInfo("radio", i)) return false;
        for (const auto& c : i.channels) if (!c.isOutput && !c.hasValue) return false;
        return !i.channels.empty();
    });
    CHECK(values, "初期の入力値が届かない");
    return lb;
}

// ---------------------------------------------------------------------------

static void TestConnect()
{
    HardwareSystem sys;
    auto lb = StartRadio(sys);
    HwDeviceInfo i;
    CHECK(sys.GetDeviceInfo("radio", i));
    CHECK(i.status == HwStatus::Ready && i.connected && i.isVirtual);
    CHECK(i.helloName == "radio" && i.proto == 1 && i.board == "loopback");
    CHECK(i.channels.size() == 7);
    CHECK(i.port == "virtual:radio");
    CHECK(i.stats.rxLines >= 5);
    CHECK(sys.ListDevices().size() == 1);
    // 心拍が回っている（RTT が測れる）
    CHECK(WaitFor([&] { HwDeviceInfo x; sys.GetDeviceInfo("radio", x); return x.stats.rttMs >= 0.0; }), "rtt");
    // ログに @hello が残り、?ping は載らない
    const auto log = sys.GetLog("radio", 200);
    bool sawHello = false, sawPing = false;
    for (const auto& e : log) { if (e.text.rfind("@hello", 0) == 0) sawHello = true; if (e.text.rfind("?ping", 0) == 0) sawPing = true; }
    CHECK(sawHello && !sawPing);
    CHECK(!sys.IsConnected("nonexistent"));
    CHECK(!sys.Set("radio", "nonexistent", 1.0));
    CHECK(!sys.Set("radio", "dial", 1.0));   // 入力には書けない
    // アクション割当が読める
    const auto b = sys.GetActionBindings();
    CHECK(b.size() == 1 && b[0].device == "radio" && b[0].channel == "btn" && b[0].action == "Jump");
    // 二重追加は拒否
    CHECK(sys.AddVirtualDevice("radio", RadioChannels()) == nullptr);
    CHECK(sys.GetVirtualDevice("radio") == lb);
    sys.Shutdown();
}

static void TestValues()
{
    HardwareSystem sys;
    StartRadio(sys);

    // 平滑: 初回はそのまま、以降は 0.5*prev + 0.5*target
    sys.BeginFrame();                        // 初期値 raw=0 を確定
    CHECK(NEAR(sys.Get("radio", "dial"), 0.0));
    CHECK(sys.Simulate("radio", "dial", 4095));
    CHECK(WaitRaw(sys, "radio", "dial", 4095));
    sys.BeginFrame();
    CHECK(NEAR(sys.Get("radio", "dial"), 0.5));
    CHECK(NEAR(sys.GetRaw("radio", "dial"), 4095));
    // フレームの途中では値が変わらない
    CHECK(sys.Simulate("radio", "dial", 0));
    CHECK(WaitRaw(sys, "radio", "dial", 0));
    CHECK(NEAR(sys.Get("radio", "dial"), 0.5));
    sys.BeginFrame();
    CHECK(NEAR(sys.Get("radio", "dial"), 0.25));   // target=0 へ平滑

    // invert
    CHECK(sys.Simulate("radio", "inv", 0.25));
    CHECK(WaitRaw(sys, "radio", "inv", 0.25));
    sys.BeginFrame();
    CHECK(NEAR(sys.Get("radio", "inv"), 0.75));

    // deadzone 0.1: 端 0.1 を 0 / 1 に潰し、間を伸ばす
    CHECK(sys.Simulate("radio", "dz", 0.05));
    CHECK(WaitRaw(sys, "radio", "dz", 0.05));
    sys.BeginFrame();
    CHECK(NEAR(sys.Get("radio", "dz"), 0.0));
    CHECK(sys.Simulate("radio", "dz", 0.5));
    CHECK(WaitRaw(sys, "radio", "dz", 0.5));
    sys.BeginFrame();
    CHECK(NEAR(sys.Get("radio", "dz"), 0.5));
    CHECK(sys.Simulate("radio", "dz", 0.95));
    CHECK(WaitRaw(sys, "radio", "dz", 0.95));
    sys.BeginFrame();
    CHECK(NEAR(sys.Get("radio", "dz"), 1.0));
    CHECK(sys.Down("radio", "dz"));   // 数値は value>=0.5

    CHECK(!sys.Simulate("radio", "nonexistent", 1));
    sys.Shutdown();
}

static void TestPressed()
{
    HardwareSystem sys;
    StartRadio(sys);
    sys.BeginFrame();
    CHECK(!sys.Down("radio", "btn") && !sys.Pressed("radio", "btn"));

    CHECK(sys.Simulate("radio", "btn", 1));
    CHECK(WaitRaw(sys, "radio", "btn", 1));
    sys.BeginFrame();
    CHECK(sys.Pressed("radio", "btn") && sys.Down("radio", "btn") && !sys.Released("radio", "btn"));
    sys.BeginFrame();
    CHECK(!sys.Pressed("radio", "btn") && sys.Down("radio", "btn"));   // Pressed は 1 フレームだけ

    CHECK(sys.Simulate("radio", "btn", 0));
    CHECK(WaitRaw(sys, "radio", "btn", 0));
    sys.BeginFrame();
    CHECK(sys.Released("radio", "btn") && !sys.Down("radio", "btn") && !sys.Pressed("radio", "btn"));
    sys.BeginFrame();
    CHECK(!sys.Released("radio", "btn"));
    sys.Shutdown();
}

static std::vector<std::string> BangLines(const std::shared_ptr<HwLoopbackDevice>& lb)
{
    std::vector<std::string> out;
    for (const auto& l : lb->ReceivedLines()) if (!l.empty() && l[0] == '!') out.push_back(l);
    return out;
}

static void TestOutputs()
{
    HardwareSystem sys;
    auto lb = StartRadio(sys);
    sys.BeginFrame();
    lb->ClearReceived();

    // 同じフレームで何度 Set しても 1 行・最新値だけ
    CHECK(sys.Set("radio", "led", 0.1));
    CHECK(sys.Set("radio", "led", 0.2));
    CHECK(sys.Set("radio", "led", 0.3));
    CHECK(sys.Set("radio", "pwm", 0.5));    // int 0..255 → 127.5 → 128
    sys.BeginFrame();
    CHECK(WaitFor([&] { return !BangLines(lb).empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto lines = BangLines(lb);
    CHECK(lines.size() == 1);
    CHECKM(lines.size() == 1 && lines[0] == "!led=0.3 pwm=128", lines.empty() ? std::string() : lines[0]);
    double v = -1;
    CHECK(lb->GetOutput("led", v) && NEAR(v, 0.3));
    CHECK(lb->GetOutput("pwm", v) && NEAR(v, 128));

    // 変化が無ければ送らない
    lb->ClearReceived();
    CHECK(sys.Set("radio", "led", 0.3));
    sys.BeginFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(BangLines(lb).empty());

    // dangerous は maxValue(0.6) で切る
    lb->ClearReceived();
    CHECK(sys.Set("radio", "heat", 1.0));
    sys.BeginFrame();
    CHECK(WaitFor([&] { return !BangLines(lb).empty(); }));
    CHECK(lb->GetOutput("heat", v) && NEAR(v, 0.6));
    HwDeviceInfo i;
    sys.GetDeviceInfo("radio", i);
    for (const auto& c : i.channels) if (c.name == "heat") { CHECK(NEAR(c.outValue, 0.6) && c.dangerous); }

    // ResetOutputs（Play 停止）で !safe
    CHECK(sys.Set("radio", "led", 1.0));
    sys.FlushOutputs();
    CHECK(WaitFor([&] { double x = 0; lb->GetOutput("led", x); return NEAR(x, 1.0); }));
    sys.ResetOutputs();
    CHECK(WaitFor([&] { double x = 1; lb->GetOutput("led", x); double h = 1; lb->GetOutput("heat", h); return NEAR(x, 0.0) && NEAR(h, 0.0); }));

    // Disconnect の前にも !safe が飛ぶ
    CHECK(sys.Set("radio", "led", 0.7));
    sys.FlushOutputs();
    CHECK(WaitFor([&] { double x = 0; lb->GetOutput("led", x); return NEAR(x, 0.7); }));
    CHECK(sys.Disconnect("radio"));
    CHECK(WaitFor([&] { return StatusOf(sys, "radio") == HwStatus::Disconnected; }));
    CHECK(lb->GetOutput("led", v) && NEAR(v, 0.0));
    CHECK(!sys.Set("radio", "led", 0.5));   // 未接続では書けない
    sys.Shutdown();
}

static void TestHeartbeatSafe()
{
    // 偽ファーム単体: 最後の ?ping から 1000ms 無ければ出力が safe に戻る
    HwChannelDecl led; led.name = "led"; led.isOutput = true; led.hasRange = true; led.min = 0; led.max = 1; led.safe = 0.0;
    HwChannelDecl heat = led; heat.name = "heat"; heat.safe = 0.25;
    auto lb = std::make_shared<HwLoopbackDevice>("fw", std::vector<HwChannelDecl>{led, heat});
    HwLoopbackTransport t(lb);
    CHECK(t.Open());
    CHECK(t.Write("!led=1 heat=0.9\n"));
    double v = -1;
    CHECK(lb->GetOutput("led", v) && NEAR(v, 1.0));
    CHECK(lb->GetOutput("heat", v) && NEAR(v, 0.9));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(t.Write("?ping 1\n"));            // 心拍で延長
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(lb->GetOutput("led", v) && NEAR(v, 1.0));   // 最後の ping から 700ms: まだ生きている
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(lb->GetOutput("led", v) && NEAR(v, 0.0));   // 1100ms: safe
    CHECK(lb->GetOutput("heat", v) && NEAR(v, 0.25)); // safe= の値
    CHECK(lb->SafeTripCount() == 1);
}

static void TestReconnect()
{
    HardwareSystem sys;
    auto lb = StartRadio(sys);

    // Disconnect → 自動再接続しない → Connect で戻る
    CHECK(sys.Disconnect("radio"));
    CHECK(WaitFor([&] { return StatusOf(sys, "radio") == HwStatus::Disconnected; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(StatusOf(sys, "radio") == HwStatus::Disconnected);
    CHECK(sys.Connect("radio"));
    CHECK(WaitFor([&] { return sys.IsConnected("radio"); }), "Connect で再接続できない");

    // 抜く → 切断検出 → 挿し直す → 自動で Ready
    lb->SetPresent(false);
    CHECK(WaitFor([&] { return !sys.IsConnected("radio"); }));
    sys.BeginFrame();
    CHECK(!sys.Down("radio", "btn"));
    lb->SetPresent(true);
    CHECK(WaitFor([&] { return sys.IsConnected("radio"); }, 5000), "自動再接続しない");
    // 再接続後も値が流れる
    CHECK(sys.Simulate("radio", "inv", 0.5));
    CHECK(WaitRaw(sys, "radio", "inv", 0.5));
    sys.Shutdown();
}

static void TestCalibrationAndSampling()
{
    HardwareSystem sys;
    StartRadio(sys);

    CHECK(sys.BeginCalibration("radio"));
    for (double v : {100.0, 3000.0, 1500.0})
    {
        CHECK(sys.Simulate("radio", "dial", v));
        CHECK(WaitRaw(sys, "radio", "dial", v));
    }
    const auto cal = sys.EndCalibration("radio");
    CHECK(cal.count("dial") == 1);
    CHECK(cal.count("dial") == 1 && NEAR(cal.at("dial").min, 100) && NEAR(cal.at("dial").max, 3000));
    CHECK(cal.count("led") == 0);   // 出力は含まない

    // 校正値を反映 → 以後の正規化に使われる（smooth を避けるため平滑済みの値でなく式を確認）
    CHECK(sys.SetChannelRange("radio", "dial", 100, 3000));
    for (int k = 0; k < 20; ++k) sys.BeginFrame();
    CHECK(std::fabs(sys.Get("radio", "dial") - (1500.0 - 100.0) / 2900.0) < 1e-3);

    // Config() に校正値が入っていて、書き戻せる
    const HwConfig c = sys.Config();
    const HwDeviceConfig* dc = c.FindDevice("radio");
    CHECK(dc && dc->channels.count("dial") == 1);
    CHECK(dc && dc->channels.at("dial").min && *dc->channels.at("dial").min == 100);
    CHECK(dc && dc->channels.at("dial").max && *dc->channels.at("dial").max == 3000);
    CHECK(dc && NEAR(dc->channels.at("dial").smooth, 0.5));   // 他の設定は壊れない
    // 仮想デバイスへ割り当てた仮想ポート（virtual:radio）は書き戻す設定に持ち込まない（hardware.json に残ると実機を照合できない）
    CHECK(dc && dc->match.port.empty());

    // サンプリング
    CHECK(sys.BeginSampling("radio"));
    for (double v : {10.0, 20.0, 30.0})
    {
        CHECK(sys.Simulate("radio", "dial", v));
        CHECK(WaitRaw(sys, "radio", "dial", v));
    }
    const auto s = sys.EndSampling("radio");
    CHECK(s.count("dial") == 1);
    if (s.count("dial") == 1)
    {
        const auto& d = s.at("dial");
        CHECK(d.count == 3 && NEAR(d.min, 10) && NEAR(d.max, 30) && NEAR(d.mean, 20));
        CHECK(std::fabs(d.stddev - std::sqrt(200.0 / 3.0)) < 1e-6);
    }
    CHECK(s.count("inv") == 0);   // 受信の無かった ch は含まない
    CHECK(!sys.BeginCalibration("nonexistent"));
    sys.Shutdown();
}

// 台本どおりに喋る偽トランスポート（Loopback では作れない異常系用）
class ScriptedTransport final : public IHwTransport
{
public:
    explicit ScriptedTransport(std::string script, std::string portName)
        : m_script(std::move(script)), m_port(std::move(portName)) {}
    bool Open() override { m_open = true; m_buf = m_script; return true; }
    void Close() override { m_open = false; }
    bool IsOpen() const override { return m_open; }
    int Read(uint8_t* buf, int max) override
    {
        if (!m_open) return -1;
        const int n = static_cast<int>(std::min<size_t>(m_buf.size(), static_cast<size_t>(max)));
        std::memcpy(buf, m_buf.data(), static_cast<size_t>(n));
        m_buf.erase(0, static_cast<size_t>(n));
        return n;
    }
    bool Write(std::string_view data) override
    {
        const size_t p = data.find("?ping ");
        if (p != std::string_view::npos)
        {
            const size_t e = data.find('\n', p);
            m_buf += "@pong " + std::string(data.substr(p + 6, e - p - 6)) + "\n";
        }
        return m_open;
    }
    std::string Describe() const override { return m_port + " (script)"; }
    std::string PortName() const override { return m_port; }
private:
    std::string m_script, m_port, m_buf;
    bool m_open = false;
};

static void TestMatchAndFactory()
{
    HwConfig cfg;
    HwDeviceConfig d;
    d.name = "radio";
    d.match.hello = "radio";
    d.match.vid = "1a86";   // 小文字でも照合できる
    d.match.pid = "7523";
    cfg.devices.push_back(d);

    HwSystemOptions o = FastOptions();
    std::atomic<int> openedCom98{0}, openedCom99{0};
    const std::string script =
        "@hello name=radio proto=1 fw=1 board=test\n@ch a in int 0 10\n@ready\n"
        + std::string(200, 'x') + "\n"      // 128 バイト超: 捨てて数える
        + "junk line\n"                      // 不正行
        + "@err boom\n"
        + "a=5\n";
    o.lister = [] {
        HwPortInfo wrong; wrong.portName = "COM98"; wrong.vid = "0403"; wrong.pid = "6001";
        HwPortInfo right; right.portName = "COM99"; right.vid = "1A86"; right.pid = "7523";
        return std::vector<HwPortInfo>{wrong, right};
    };
    o.factory = [&](const std::string& port, int) -> std::unique_ptr<IHwTransport> {
        if (port == "COM98") ++openedCom98;
        if (port == "COM99") ++openedCom99;
        return std::make_unique<ScriptedTransport>(script, port);
    };

    HardwareSystem sys;
    sys.Initialize(cfg, o);
    CHECK(WaitFor([&] { return sys.IsConnected("radio"); }));
    CHECK(openedCom98 == 0, "VID が違うポートを開いてしまった");
    CHECK(openedCom99 >= 1);
    CHECK(WaitFor([&] { HwDeviceInfo i; sys.GetDeviceInfo("radio", i); return i.stats.errors >= 1 && i.stats.rxLines >= 5; }));
    HwDeviceInfo i;
    sys.GetDeviceInfo("radio", i);
    CHECK(i.port == "COM99" && i.board == "test");
    CHECK(i.stats.overflow == 1);
    CHECK(i.stats.badLines == 1);
    CHECK(i.stats.errors == 1);
    CHECK(WaitRaw(sys, "radio", "a", 5));
    sys.BeginFrame();
    CHECK(NEAR(sys.Get("radio", "a"), 0.5));   // 0..10 の 5
    sys.Shutdown();

    // hello の名前が違えば接続しない（開いて確かめてから閉じる）
    HwConfig cfg2;
    HwDeviceConfig d2;
    d2.name = "other";
    d2.match.hello = "other";
    cfg2.devices.push_back(d2);
    HardwareSystem sys2;
    sys2.Initialize(cfg2, o);   // lister は COM98 / COM99 を返し、どちらも hello=radio と名乗る
    CHECK(WaitFor([&] { HwDeviceInfo x; sys2.GetDeviceInfo("other", x); return x.status == HwStatus::Error && !x.lastError.empty(); }));
    CHECK(!sys2.IsConnected("other"));
    sys2.Shutdown();
}

static void TestConfig()
{
    const char* json = R"({
      "devices": [
        {
          "name": "radio",
          "transport": "serial",
          "match": { "hello": "radio", "vid": "1a86", "pid": "7523" },
          "baud": 115200,
          "pins": [ { "pin": 34, "mode": "adc", "name": "dial" } ],
          "channels": {
            "dial": { "min": 0, "max": 4095, "deadzone": 0.01, "smooth": 0.2, "invert": false },
            "heat": { "dangerous": true, "maxValue": 0.6 }
          },
          "actions": { "btn": "Jump" }
        }
      ]
    })";
    HwConfig cfg;
    std::string err;
    CHECKM(ParseHwConfig(json, cfg, err), err);
    CHECK(cfg.devices.size() == 1);
    const HwDeviceConfig* d = cfg.FindDevice("radio");
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(d->match.hello == "radio" && d->match.vid == "1A86" && d->match.pid == "7523");
    CHECK(d->baud == 115200 && d->pins.size() == 1 && d->pins[0].pin == 34 && d->pins[0].mode == "adc" && d->pins[0].name == "dial");
    CHECK(d->channels.at("dial").min && *d->channels.at("dial").min == 0 && *d->channels.at("dial").max == 4095);
    CHECK(NEAR(d->channels.at("dial").deadzone, 0.01) && NEAR(d->channels.at("dial").smooth, 0.2));
    CHECK(d->channels.at("heat").dangerous && NEAR(d->channels.at("heat").maxValue, 0.6) && !d->channels.at("heat").min);
    CHECK(d->actions.at("btn") == "Jump");

    // 書き出し → 読み戻しで同じ
    const std::string dumped = DumpHwConfig(cfg);
    HwConfig back;
    CHECKM(ParseHwConfig(dumped, back, err), err);
    CHECK(DumpHwConfig(back) == dumped);

    // ファイルへ原子的に書いて読み戻す
    const fs::path dir = fs::temp_directory_path() / "dx12_hardware_test";
    fs::create_directories(dir);
    const fs::path file = dir / "hardware.json";
    CHECKM(SaveHwConfigFile(file, cfg, err), err);
    std::ifstream f(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    HwConfig fromFile;
    CHECKM(ParseHwConfig(text, fromFile, err), err);
    CHECK(DumpHwConfig(fromFile) == dumped);
    std::error_code ec;
    fs::remove(file, ec);

    // 空文字列は 0 件で成功 / 不正は失敗
    HwConfig empty;
    CHECK(ParseHwConfig("", empty, err) && empty.devices.empty());
    CHECK(ParseHwConfig("{}", empty, err) && empty.devices.empty());
    CHECK(!ParseHwConfig("{ not json", empty, err) && !err.empty());
    CHECK(!ParseHwConfig("[1,2]", empty, err));
    CHECK(!ParseHwConfig(R"({"devices":[{"transport":"serial"}]})", empty, err));
    CHECK(!ParseHwConfig(R"({"devices":[{"name":"a"},{"name":"a"}]})", empty, err));
}

int main()
{
    TestConfig();
    TestConnect();
    TestValues();
    TestPressed();
    TestOutputs();
    TestHeartbeatSafe();
    TestReconnect();
    TestCalibrationAndSampling();
    TestMatchAndFactory();
    std::printf("hardware_system_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
