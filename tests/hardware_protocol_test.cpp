// ハードウェア連携プロトコル v1 の純ロジック（src/hardware/HwProtocol.{h,cpp}）のテスト。
// 行の組み立て（\r 無視・128 バイト超の破棄）・各行種のパース・不正行・エンジン → デバイスの行の生成。
// Win32 にも Logger にも依存しない。

#include "hardware/HwProtocol.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e::hw;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) { std::printf("FAIL: %s %s (%s:%d)\n", #cond, "" __VA_ARGS__, __FILE__, __LINE__); ++g_failed; } \
    } while (0)

static std::vector<std::string> FeedAll(HwLineAssembler& a, const std::string& s)
{
    std::vector<std::string> lines;
    a.Feed(s.data(), s.size(), lines);
    return lines;
}

static void TestAssembler()
{
    {
        HwLineAssembler a;
        auto l = FeedAll(a, "hello\r\nworld\n\n\r\nx=1\n");
        CHECK(l.size() == 3);
        CHECK(l.size() == 3 && l[0] == "hello" && l[1] == "world" && l[2] == "x=1");
    }
    {   // 分割到着
        HwLineAssembler a;
        auto l1 = FeedAll(a, "dial=20");
        CHECK(l1.empty());
        auto l2 = FeedAll(a, "48 btn=1\nnext");
        CHECK(l2.size() == 1 && l2[0] == "dial=2048 btn=1");
        auto l3 = FeedAll(a, "\n");
        CHECK(l3.size() == 1 && l3[0] == "next");
    }
    {   // ちょうど 128 バイトは通る / 129 バイトは捨てる
        HwLineAssembler a;
        auto ok = FeedAll(a, std::string(128, 'a') + "\n");
        CHECK(ok.size() == 1 && ok[0].size() == 128);
        CHECK(a.OverflowCount() == 0);
        auto bad = FeedAll(a, std::string(129, 'b') + "\n");
        CHECK(bad.empty());
        CHECK(a.OverflowCount() == 1);
        // 捨てた行の後の行は無事
        auto after = FeedAll(a, "good=1\n");
        CHECK(after.size() == 1 && after[0] == "good=1");
    }
    {   // 超長行が複数チャンクにまたがっても 1 回だけ数える・次の行は無傷
        HwLineAssembler a;
        std::vector<std::string> l;
        for (int i = 0; i < 10; ++i) { const std::string chunk(50, 'z'); a.Feed(chunk.data(), chunk.size(), l); }
        CHECK(l.empty());
        FeedAll(a, "\n");
        CHECK(a.OverflowCount() == 1);
        auto after = FeedAll(a, "ok=2\n");
        CHECK(after.size() == 1 && after[0] == "ok=2");
        // 2 本続けて超過
        auto two = FeedAll(a, std::string(200, 'x') + "\n" + std::string(300, 'y') + "\nok=3\n");
        CHECK(two.size() == 1 && two[0] == "ok=3");
        CHECK(a.OverflowCount() == 3);
    }
}

static void TestParse()
{
    {
        const auto m = ParseDeviceLine("@hello name=radio proto=1 fw=3 board=esp32");
        CHECK(m.type == HwMsgType::Hello);
        CHECK(m.name == "radio" && m.proto == 1 && m.fw == 3 && m.board == "esp32");
    }
    {   // 前方互換: 未知キーは無視。name 無し・不正名は不正行
        CHECK(ParseDeviceLine("@hello name=a_1 proto=1 extra=zzz").type == HwMsgType::Hello);
        CHECK(ParseDeviceLine("@hello proto=1").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("@hello name=BadName proto=1").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("@hello name=this_name_is_way_too_long_for_it").type == HwMsgType::Unknown);
    }
    {
        const auto m = ParseDeviceLine("@ch dial in int 0 4095");
        CHECK(m.type == HwMsgType::Channel);
        CHECK(m.channel.name == "dial" && !m.channel.isOutput && m.channel.type == HwChType::Int);
        CHECK(m.channel.hasRange && m.channel.min == 0 && m.channel.max == 4095);
    }
    {
        const auto m = ParseDeviceLine("@ch btn in bool");
        CHECK(m.type == HwMsgType::Channel && m.channel.type == HwChType::Bool && !m.channel.hasRange);
    }
    {
        const auto m = ParseDeviceLine("@ch led out float 0 1 safe=0.25");
        CHECK(m.type == HwMsgType::Channel && m.channel.isOutput && m.channel.safe == 0.25);
        CHECK(m.channel.hasRange && m.channel.max == 1);
    }
    {   // 不正な @ch
        CHECK(ParseDeviceLine("@ch x sideways int").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("@ch x in double").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("@ch x in int 5").type == HwMsgType::Unknown);   // min だけは不可
        CHECK(ParseDeviceLine("@ch x in").type == HwMsgType::Unknown);
    }
    CHECK(ParseDeviceLine("@ready").type == HwMsgType::Ready);
    {
        const auto m = ParseDeviceLine("@pong 17");
        CHECK(m.type == HwMsgType::Pong && m.seq == 17);
        CHECK(ParseDeviceLine("@pong x").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("@pong").type == HwMsgType::Unknown);
    }
    {
        const auto l = ParseDeviceLine("@log boot ok  v2");
        CHECK(l.type == HwMsgType::Log && l.text == "boot ok  v2");
        const auto e = ParseDeviceLine("@err sensor timeout");
        CHECK(e.type == HwMsgType::Err && e.text == "sensor timeout");
    }
    {   // 値行: 複数 k=v は同じ行
        const auto m = ParseDeviceLine("dial=2048 btn=0 temp=-12.5");
        CHECK(m.type == HwMsgType::Values && m.values.size() == 3);
        CHECK(m.values.size() == 3 && m.values[0].first == "dial" && m.values[0].second == 2048);
        CHECK(m.values.size() == 3 && m.values[2].first == "temp" && m.values[2].second == -12.5);
        const auto one = ParseDeviceLine("btn=1");
        CHECK(one.type == HwMsgType::Values && one.values.size() == 1 && one.values[0].second == 1);
    }
    {   // 不正行
        CHECK(ParseDeviceLine("").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("   ").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("garbage").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("dial=abc").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("dial=1 oops").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("dial=1 =2").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("dial=nan").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("dial=inf").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("123=1").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("@unknown foo").type == HwMsgType::Unknown);
        CHECK(ParseDeviceLine("?ping 1").type == HwMsgType::Unknown);
        const auto u = ParseDeviceLine("%%junk%%");
        CHECK(u.type == HwMsgType::Unknown && u.text == "%%junk%%");
    }
}

static void TestFormat()
{
    CHECK(HwFormatHello() == "?hello");
    CHECK(HwFormatPing(17) == "?ping 17");
    CHECK(HwFormatSafe() == "!safe");
    CHECK(HwFormatPin(34, "adc", "dial") == "?pin 34 adc dial");
    CHECK(HwFormatValue(0.4) == "0.4");
    CHECK(HwFormatValue(1) == "1");
    CHECK(HwFormatValue(4095) == "4095");
    CHECK(HwFormatValue(-3) == "-3");
    CHECK(HwFormatValue(0.123456789) == "0.1235");
    CHECK(HwFormatSet({{"led", 0.4}}) == "!led=0.4");
    CHECK(HwFormatSet({{"a", 1}, {"b", 0.5}}) == "!a=1 b=0.5");
    {   // 長いときだけ複数行に分かれ、各行が 128 バイト以内
        std::vector<std::pair<std::string, double>> kv;
        for (int i = 0; i < 40; ++i) kv.emplace_back("channel_" + std::to_string(i), 0.123456);
        const auto lines = HwFormatSetLines(kv);
        CHECK(lines.size() > 1);
        size_t total = 0;
        for (const auto& l : lines) { CHECK(l.size() <= kMaxLineBytes); CHECK(!l.empty() && l[0] == '!'); total += l.size(); }
        CHECK(total > 0);
        CHECK(HwFormatSetLines({{"led", 1}}).size() == 1);
    }
    {   // 作った行はデバイス側（Loopback 同様）の値行パーサで読み戻せる
        const auto s = HwFormatSet({{"a", 1}, {"b", 0.5}});
        const auto m = ParseDeviceLine(std::string_view(s).substr(1));
        CHECK(m.type == HwMsgType::Values && m.values.size() == 2 && m.values[1].second == 0.5);
    }
}

int main()
{
    TestAssembler();
    TestParse();
    TestFormat();
    std::printf("hardware_protocol_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
