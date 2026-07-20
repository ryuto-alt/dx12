// RpcValue/RpcArgs(net:rpc の引数シリアライズ)の往復テスト。
// GPU非依存・DirectXMathのみ依存の純ロジック。
#include "network/Rpc.h"

#include <cstdio>

using namespace dx12e;

static int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static void TestRoundtripAllTypes()
{
    RpcArgs args;
    args.push_back(RpcValue{});                                  // Nil
    args.push_back(RpcValue::MakeBool(true));
    args.push_back(RpcValue::MakeBool(false));
    args.push_back(RpcValue::MakeNumber(3.14159265358979));
    args.push_back(RpcValue::MakeNumber(-42.0));
    args.push_back(RpcValue::MakeString("hello, rpc"));
    args.push_back(RpcValue::MakeString(""));
    args.push_back(RpcValue::MakeVec3({ 1.0f, 2.0f, 3.0f }));

    NetWriter w;
    WriteRpcArgs(w, args);

    NetReader r(w.Data().data(), w.Data().size());
    RpcArgs out = ReadRpcArgs(r);

    CHECK(out.size() == args.size());
    if (out.size() != args.size()) return;

    CHECK(out[0].type == RpcValue::Type::Nil);

    CHECK(out[1].type == RpcValue::Type::Bool);
    CHECK(out[1].b == true);
    CHECK(out[2].type == RpcValue::Type::Bool);
    CHECK(out[2].b == false);

    CHECK(out[3].type == RpcValue::Type::Number);
    CHECK(out[3].num > 3.14159265358978 && out[3].num < 3.14159265358980);
    CHECK(out[4].type == RpcValue::Type::Number);
    CHECK(out[4].num == -42.0);

    CHECK(out[5].type == RpcValue::Type::String);
    CHECK(out[5].str == "hello, rpc");
    CHECK(out[6].type == RpcValue::Type::String);
    CHECK(out[6].str == "");

    CHECK(out[7].type == RpcValue::Type::Vec3);
    CHECK(out[7].vec.x == 1.0f && out[7].vec.y == 2.0f && out[7].vec.z == 3.0f);
}

static void TestEmptyArgsRoundtrip()
{
    RpcArgs args;
    NetWriter w;
    WriteRpcArgs(w, args);
    NetReader r(w.Data().data(), w.Data().size());
    RpcArgs out = ReadRpcArgs(r);
    CHECK(out.empty());
}

int main()
{
    TestRoundtripAllTypes();
    TestEmptyArgsRoundtrip();

    if (g_failures == 0) { std::printf("RpcArgsTests: all passed\n"); return 0; }
    std::printf("RpcArgsTests: %d failure(s)\n", g_failures);
    return 1;
}
