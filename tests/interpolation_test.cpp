// SnapshotBuffer(受信スナップショットの補間バッファ)の単体テスト。
// GPU非依存・DirectXMathのみ依存の純ロジック。
#include "network/Interpolation.h"

#include <cstdio>

using namespace dx12e;

static int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static void TestEmptyBufferReturnsFalse()
{
    SnapshotBuffer buf;
    SnapshotBuffer::Sample s;
    CHECK(buf.Empty());
    CHECK(!buf.TrySample(0.0f, s));
}

static void TestSingleSampleReturnsItself()
{
    SnapshotBuffer buf;
    SnapshotBuffer::Sample in;
    in.time = 1.0f;
    in.hasPosition = true;
    in.position = { 1.0f, 2.0f, 3.0f };
    buf.Push(in);

    SnapshotBuffer::Sample out;
    CHECK(buf.TrySample(0.0f, out));   // 範囲外(古い)でも端のサンプルを返す
    CHECK(out.position.x == 1.0f && out.position.y == 2.0f && out.position.z == 3.0f);
    CHECK(buf.TrySample(5.0f, out));   // 範囲外(新しい)でも端のサンプルを返す
    CHECK(out.position.x == 1.0f);
}

static void TestLinearInterpolationBetweenTwoSamples()
{
    SnapshotBuffer buf;

    SnapshotBuffer::Sample a;
    a.time = 0.0f;
    a.hasPosition = true;
    a.position = { 0.0f, 0.0f, 0.0f };
    buf.Push(a);

    SnapshotBuffer::Sample b;
    b.time = 1.0f;
    b.hasPosition = true;
    b.position = { 10.0f, 0.0f, 0.0f };
    buf.Push(b);

    SnapshotBuffer::Sample out;
    CHECK(buf.TrySample(0.5f, out));
    CHECK(out.position.x > 4.9f && out.position.x < 5.1f);

    CHECK(buf.TrySample(0.25f, out));
    CHECK(out.position.x > 2.4f && out.position.x < 2.6f);
}

static void TestRotationSlerp()
{
    using namespace DirectX;
    SnapshotBuffer buf;

    SnapshotBuffer::Sample a;
    a.time = 0.0f;
    a.hasRotation = true;
    a.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };   // identity
    buf.Push(a);

    SnapshotBuffer::Sample b;
    b.time = 1.0f;
    b.hasRotation = true;
    XMStoreFloat4(&b.rotation, XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XMConvertToRadians(90.0f)));
    buf.Push(b);

    SnapshotBuffer::Sample out;
    CHECK(buf.TrySample(0.5f, out));
    // 中間姿勢はidentityでも90度でもないはず(何らかの補間が起きている)。
    const bool looksLikeIdentity = (out.rotation.x == 0.0f && out.rotation.y == 0.0f && out.rotation.z == 0.0f);
    CHECK(!looksLikeIdentity);
}

static void TestOutOfOrderSamplesAreIgnored()
{
    SnapshotBuffer buf;
    SnapshotBuffer::Sample a; a.time = 2.0f; a.hasPosition = true; a.position = { 5.0f, 0.0f, 0.0f };
    buf.Push(a);
    SnapshotBuffer::Sample stale; stale.time = 1.0f; stale.hasPosition = true; stale.position = { 999.0f, 0.0f, 0.0f };
    buf.Push(stale);   // timeが逆行しているので無視されるはず

    SnapshotBuffer::Sample out;
    CHECK(buf.TrySample(2.0f, out));
    CHECK(out.position.x == 5.0f);
}

static void TestClearEmptiesBuffer()
{
    SnapshotBuffer buf;
    SnapshotBuffer::Sample a; a.time = 0.0f; a.hasPosition = true;
    buf.Push(a);
    CHECK(!buf.Empty());
    buf.Clear();
    CHECK(buf.Empty());
}

int main()
{
    TestEmptyBufferReturnsFalse();
    TestSingleSampleReturnsItself();
    TestLinearInterpolationBetweenTwoSamples();
    TestRotationSlerp();
    TestOutOfOrderSamplesAreIgnored();
    TestClearEmptiesBuffer();

    if (g_failures == 0) { std::printf("InterpolationTests: all passed\n"); return 0; }
    std::printf("InterpolationTests: %d failure(s)\n", g_failures);
    return 1;
}
