// IndexAllocator の単体テスト。
//
// 外部テストフレームワークに依存しない (ctest が終了コードで合否を判定する)。
// IndexAllocator は GPU/D3D12 非依存のヘッダオンリーなので、デバイス無しで検証できる。
//
// 実行: ctest --output-on-failure (失敗があれば終了コード 1)

#include "core/IndexAllocator.h"

#include <cstdio>

using dx12e::IndexAllocator;
using dx12e::u32;

namespace
{
int g_failures = 0;
int g_checks   = 0;
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void Test_SequentialAllocation()
{
    IndexAllocator a(8);
    CHECK(a.Capacity() == 8);
    CHECK(a.FreeCount() == 8);
    CHECK(a.UsedCount() == 0);

    CHECK(a.Allocate() == 0);
    CHECK(a.Allocate() == 1);
    CHECK(a.Allocate() == 2);
    CHECK(a.UsedCount() == 3);
    CHECK(a.FreeCount() == 5);
}

static void Test_FreeAndReuse()
{
    IndexAllocator a(8);
    u32 i0 = a.Allocate(); // 0
    u32 i1 = a.Allocate(); // 1
    u32 i2 = a.Allocate(); // 2
    CHECK(i0 == 0 && i1 == 1 && i2 == 2);

    a.Free(i1);
    CHECK(a.UsedCount() == 2);
    // 解放した穴(1)が最初に再利用される
    CHECK(a.Allocate() == 1);
    CHECK(a.UsedCount() == 3);
}

static void Test_ContiguousBlock()
{
    IndexAllocator a(8);
    u32 single = a.Allocate();          // 0
    u32 block  = a.AllocateBlock(3);    // 1,2,3
    CHECK(single == 0);
    CHECK(block == 1);
    CHECK(a.UsedCount() == 4);

    // ブロックは連続していること
    u32 next = a.Allocate();            // 4
    CHECK(next == 4);
}

static void Test_BlockCoalescing()
{
    IndexAllocator a(8);
    u32 b0 = a.AllocateBlock(2); // 0,1
    u32 b1 = a.AllocateBlock(2); // 2,3
    CHECK(b0 == 0 && b1 == 2);
    CHECK(a.UsedCount() == 4);

    // 隣接する2ブロックを返却すると結合され、より大きな連続確保が可能になる
    a.FreeBlock(b0, 2);
    a.FreeBlock(b1, 2);
    CHECK(a.FreeCount() == 8);
    CHECK(a.FreeRangeCount() == 1); // 完全に結合され単一レンジ

    u32 big = a.AllocateBlock(5);
    CHECK(big == 0);
    CHECK(big != IndexAllocator::kInvalid);
}

static void Test_Fragmentation()
{
    IndexAllocator a(4);
    a.Allocate(); // 0
    a.Allocate(); // 1
    a.Allocate(); // 2
    a.Allocate(); // 3
    CHECK(a.FreeCount() == 0);

    // 非隣接の穴を作る -> 合計2空きだが連続2は取れない
    a.Free(0);
    a.Free(2);
    CHECK(a.FreeCount() == 2);
    CHECK(a.AllocateBlock(2) == IndexAllocator::kInvalid);

    // 単一なら取れる
    CHECK(a.Allocate() != IndexAllocator::kInvalid);
}

static void Test_Overflow()
{
    IndexAllocator a(2);
    CHECK(a.Allocate() == 0);
    CHECK(a.Allocate() == 1);
    // 枯渇 -> kInvalid を返す(クラッシュしない)
    CHECK(a.Allocate() == IndexAllocator::kInvalid);
    CHECK(a.AllocateBlock(1) == IndexAllocator::kInvalid);

    // 1つ返せば再び確保可能
    a.Free(0);
    CHECK(a.Allocate() == 0);
}

static void Test_FullCycle()
{
    IndexAllocator a(16);
    u32 idx[16];
    for (u32 i = 0; i < 16; ++i)
    {
        idx[i] = a.Allocate();
        CHECK(idx[i] == i);
    }
    CHECK(a.FreeCount() == 0);

    for (u32 i = 0; i < 16; ++i)
    {
        a.Free(idx[i]);
    }
    // 全返却で容量全部に戻り、単一の連続レンジへ結合されている
    CHECK(a.FreeCount() == 16);
    CHECK(a.FreeRangeCount() == 1);
    CHECK(a.AllocateBlock(16) == 0);
}

static void Test_EdgeCases()
{
    IndexAllocator a(4);
    // 0 個確保は無効
    CHECK(a.AllocateBlock(0) == IndexAllocator::kInvalid);
    // 容量超のブロックは取れない
    CHECK(a.AllocateBlock(5) == IndexAllocator::kInvalid);
    // kInvalid/0個の返却は no-op (クラッシュしない)
    a.Free(IndexAllocator::kInvalid);
    a.FreeBlock(0, 0);
    CHECK(a.FreeCount() == 4);

    // 空容量
    IndexAllocator empty(0);
    CHECK(empty.Capacity() == 0);
    CHECK(empty.Allocate() == IndexAllocator::kInvalid);
}

int main()
{
    Test_SequentialAllocation();
    Test_FreeAndReuse();
    Test_ContiguousBlock();
    Test_BlockCoalescing();
    Test_Fragmentation();
    Test_Overflow();
    Test_FullCycle();
    Test_EdgeCases();

    std::printf("IndexAllocator: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
