// slot11（クラスタのディスクリプタテーブル）の宣言表（renderer/ClusterTableLayout.h）の検査。W05-24。
// レジスタの重複が無い、総数の一致、ダミーで埋める本数、他のルートパラメータとの衝突が無い。
#include "renderer/ClusterTableLayout.h"
#include "renderer/MaterialTableLayout.h"

#include <cstdio>
#include <set>

using namespace dx12e;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    // 総数: count の総和 = 28（既存 14 + 将来分 14）。
    u32 sum = 0;
    for (const auto& r : kClusterRanges) sum += r.count;
    CHECK(sum == kClusterTableCount);
    CHECK(kClusterTableCount == 28);
    CHECK(kClusterTableExisting == 14);

    // レジスタの重複が無い。
    std::set<u32> regs;
    for (const auto& r : kClusterRanges)
    {
        CHECK(r.count >= 1);
        for (u32 i = 0; i < r.count; ++i) CHECK(regs.insert(r.baseRegister + i).second);
    }
    CHECK(regs.size() == kClusterTableCount);

    // 他のルートパラメータ（RootSignature.cpp）が持つレジスタとは重ならない:
    // t0-t12（材質 t0-2・t24・t25、ボーン t3、影 t4、IBL t5-7、AO t8、ローカル影 t9-10、コンタクト t11、前ボーン t12）、t16 / t17（SSR / SSGI）、t24 / t25。
    const u32 other[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,16,17,24,25};
    for (u32 o : other) CHECK(regs.count(o) == 0);

    // 既存 14 本のオフセット（ClusteredLightCulling の kXxxSrvOffset と同じ値）。
    CHECK(ClusterTableOffsetOf(13) == 0);
    CHECK(ClusterTableOffsetOf(18) == 3);
    CHECK(ClusterTableOffsetOf(22) == 7);
    CHECK(ClusterTableOffsetOf(30) == 9);
    CHECK(ClusterTableOffsetOf(31) == 10);
    CHECK(ClusterTableOffsetOf(32) == 11);
    CHECK(ClusterTableOffsetOf(33) == 12);
    CHECK(ClusterTableOffsetOf(34) == 13);
    // 将来分は既存の後ろに宣言順で並ぶ。
    CHECK(ClusterTableOffsetOf(35) == 14);
    CHECK(ClusterTableOffsetOf(46) == 15);
    CHECK(ClusterTableOffsetOf(49) == 26);
    CHECK(ClusterTableOffsetOf(99) == 0xFFFFFFFFu);

    // 先頭の既存分がちょうど kClusterTableExisting 本で終わる（行の途中で切れない）。
    u32 acc = 0; bool boundary = false;
    for (const auto& r : kClusterRanges) { acc += r.count; if (acc == kClusterTableExisting) boundary = true; }
    CHECK(boundary);

    // ダミーで埋める本数 = 全長 - 既存分。型はテーブルの宣言どおり。
    CHECK(kClusterTableCount - kClusterTableExisting == 14);
    CHECK(ClusterDummyKindAt(0) == DummyKind::Buffer);
    CHECK(ClusterDummyKindAt(10) == DummyKind::Tex2DArray);   // t31 クッキー
    CHECK(ClusterDummyKindAt(12) == DummyKind::TexCubeArray); // t33
    CHECK(ClusterDummyKindAt(14) == DummyKind::Tex2DArray);   // t35 LTC
    CHECK(ClusterDummyKindAt(15) == DummyKind::Buffer);       // t46 SH9

    // 材質テーブル（slot 2）の枚数。
    CHECK(kMaterialTableCount == 5);

    std::printf("ClusterTableLayoutTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
