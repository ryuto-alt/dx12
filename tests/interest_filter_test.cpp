// IsRelevant(興味管理の距離判定)の単体テスト。GPU非依存・DirectXMathのみ依存。
#include "network/Interest.h"

#include <cstdio>

using namespace dx12e;

static int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static void TestZeroRadiusAlwaysRelevant()
{
    DirectX::XMFLOAT3 observer{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 farAway{ 10000.0f, 10000.0f, 10000.0f };
    CHECK(IsRelevant(observer, farAway, 0.0f));
    CHECK(IsRelevant(observer, farAway, -1.0f));   // 負値も「常に関連」扱い
}

static void TestWithinRadius()
{
    DirectX::XMFLOAT3 observer{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 near{ 5.0f, 0.0f, 0.0f };
    CHECK(IsRelevant(observer, near, 10.0f));
}

static void TestOutsideRadius()
{
    DirectX::XMFLOAT3 observer{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 far{ 20.0f, 0.0f, 0.0f };
    CHECK(!IsRelevant(observer, far, 10.0f));
}

static void TestExactBoundaryIsRelevant()
{
    DirectX::XMFLOAT3 observer{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 boundary{ 10.0f, 0.0f, 0.0f };
    CHECK(IsRelevant(observer, boundary, 10.0f));   // <= なので境界ちょうどは関連
}

static void TestThreeDimensionalDistance()
{
    DirectX::XMFLOAT3 observer{ 0.0f, 0.0f, 0.0f };
    // 3-4-... : sqrt(3^2+4^2) = 5
    DirectX::XMFLOAT3 p{ 3.0f, 4.0f, 0.0f };
    CHECK(IsRelevant(observer, p, 5.0f));
    CHECK(!IsRelevant(observer, p, 4.9f));
}

int main()
{
    TestZeroRadiusAlwaysRelevant();
    TestWithinRadius();
    TestOutsideRadius();
    TestExactBoundaryIsRelevant();
    TestThreeDimensionalDistance();

    if (g_failures == 0) { std::printf("InterestFilterTests: all passed\n"); return 0; }
    std::printf("InterestFilterTests: %d failure(s)\n", g_failures);
    return 1;
}
