// DLSS の配線の検査（W05-07）。GPU は使わない。
//   - DlssBackend::CompiledIn() が CMake の UNO_DLSS_ACTIVE と一致する（get_upscaler の dlss.compiledIn と同じ値）。
//   - デバイスが無い Initialize は失敗し、理由が入る（例外を投げない）。
// 標準出力の最後の行 "compiledIn=0|1" を ctest DlssWiringCheck（dlss_wiring_check.cmake）が exe の隣の nvngx_dlss.dll と突き合わせる。
#include "renderer/DlssBackend.h"

#include <cstdio>

#ifndef UNO_DLSS_EXPECT_COMPILED_IN
#error "UNO_DLSS_EXPECT_COMPILED_IN (0|1) は tests/CMakeLists.txt が渡す"
#endif

using namespace dx12e;

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    CHECK(DlssBackend::CompiledIn() == (UNO_DLSS_EXPECT_COMPILED_IN != 0));

    DlssBackend b;
    CHECK(!b.Initialize(nullptr, L""));
    CHECK(!b.Available());
    CHECK(!b.Reason().empty());

    std::printf("compiledIn=%d\n", DlssBackend::CompiledIn() ? 1 : 0);
    return g_failures ? 1 : 0;
}
