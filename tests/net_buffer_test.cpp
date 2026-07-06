// NetWriter/NetReader の往復テスト(GPU/entt非依存の純ロジック)。
#include "network/NetBuffer.h"

#include <cstdio>
#include <string>

using namespace dx12e;

static int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static void TestIntegerRoundtrip()
{
    NetWriter w;
    w.WriteU8(0xAB);
    w.WriteU16(0x1234);
    w.WriteU32(0xDEADBEEFu);
    w.WriteU64(0x0123456789ABCDEFull);
    w.WriteI32(-12345);

    NetReader r(w.Data().data(), w.Data().size());
    CHECK(r.ReadU8() == 0xAB);
    CHECK(r.ReadU16() == 0x1234);
    CHECK(r.ReadU32() == 0xDEADBEEFu);
    CHECK(r.ReadU64() == 0x0123456789ABCDEFull);
    CHECK(r.ReadI32() == -12345);
    CHECK(r.Remaining() == 0);
}

static void TestFloatRoundtrip()
{
    NetWriter w;
    w.WriteF32(3.14159f);
    NetReader r(w.Data().data(), w.Data().size());
    float v = r.ReadF32();
    CHECK(v > 3.1414f && v < 3.1417f);
}

static void TestBoolAndStringRoundtrip()
{
    NetWriter w;
    w.WriteBool(true);
    w.WriteBool(false);
    w.WriteString("hello, dx12");
    w.WriteString("");

    NetReader r(w.Data().data(), w.Data().size());
    CHECK(r.ReadBool() == true);
    CHECK(r.ReadBool() == false);
    CHECK(r.ReadString() == "hello, dx12");
    CHECK(r.ReadString() == "");
    CHECK(r.Remaining() == 0);
}

static void TestBytesRoundtrip()
{
    const u8 src[4] = { 1, 2, 3, 4 };
    NetWriter w;
    w.WriteBytes(src, sizeof(src));
    NetReader r(w.Data().data(), w.Data().size());
    u8 dst[4] = {};
    r.ReadBytes(dst, sizeof(dst));
    CHECK(dst[0] == 1 && dst[1] == 2 && dst[2] == 3 && dst[3] == 4);
}

static void TestOutOfRangeThrows()
{
    NetWriter w;
    w.WriteU8(1);
    NetReader r(w.Data().data(), w.Data().size());
    r.ReadU8();
    bool threw = false;
    try { r.ReadU32(); }
    catch (const NetReadError&) { threw = true; }
    CHECK(threw);
}

static void TestStringLengthOutOfRangeThrows()
{
    NetWriter w;
    w.WriteU32(1000); // 長さだけ書いて本体は書かない -> 壊れたパケットを模擬
    NetReader r(w.Data().data(), w.Data().size());
    bool threw = false;
    try { r.ReadString(); }
    catch (const NetReadError&) { threw = true; }
    CHECK(threw);
}

int main()
{
    TestIntegerRoundtrip();
    TestFloatRoundtrip();
    TestBoolAndStringRoundtrip();
    TestBytesRoundtrip();
    TestOutOfRangeThrows();
    TestStringLengthOutOfRangeThrows();

    if (g_failures == 0) { std::printf("NetBufferTests: all passed\n"); return 0; }
    std::printf("NetBufferTests: %d failure(s)\n", g_failures);
    return 1;
}
