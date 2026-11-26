// DescriptorDeferredFreeTests（W05-06 / AUD-04）: ディスクリプタの遅延返却。
// D3D12 のデバイスは作らず、DescriptorHeap の管理表だけを使う(GPU 不要)。
#include <cstdio>
#include <new>

#include "graphics/DeferredRelease.h"
#include "graphics/DescriptorHeap.h"

using namespace dx12e;

static int g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

// 同じフレームで Free → Allocate しても同じ index は返らず、フェンス完了後に返る。
static void Test_SingleIndex()
{
    DescriptorHeap heap;
    heap.InitializeBookkeepingOnly(4);
    DeferredRelease::Enable();

    u32 a = heap.AllocateIndex();
    heap.Free(a);
    CHECK(heap.GetFreeCount() == 3);              // まだ戻っていない
    u32 b = heap.AllocateIndex();
    CHECK(b != a);

    DeferredRelease::Stamp(10);
    DeferredRelease::Collect(9);                  // 未完了
    CHECK(heap.GetFreeCount() == 2);
    DeferredRelease::Collect(10);                 // 完了
    CHECK(heap.GetFreeCount() == 3);

    // フリーリストの先頭に戻る実装でも、確実に a が再び確保できること
    bool gotA = false;
    for (int i = 0; i < 3; ++i)
        gotA |= (heap.AllocateIndex() == a);
    CHECK(gotA);

    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
}

static void Test_Block()
{
    DescriptorHeap heap;
    heap.InitializeBookkeepingOnly(8);
    DeferredRelease::Enable();

    u32 s = heap.AllocateBlock(3);
    heap.FreeBlock(s, 3);
    CHECK(heap.GetFreeCount() == 5);
    u32 s2 = heap.AllocateBlock(3);
    CHECK(s2 + 3 <= s || s2 >= s + 3);            // 重ならない

    DeferredRelease::Stamp(20);
    DeferredRelease::Collect(20);
    CHECK(heap.GetFreeCount() == 8 - 3);

    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
}

// Disable 中(初期化前/終了後)は従来どおり即時に戻る。
static void Test_ImmediateWhenDisabled()
{
    DeferredRelease::Disable();
    DescriptorHeap heap;
    heap.InitializeBookkeepingOnly(4);
    u32 a = heap.AllocateIndex();
    heap.Free(a);
    CHECK(heap.GetFreeCount() == 4);
}

// ヒープより後に FlushAll / Collect が走っても落ちない。
static void Test_HeapDestroyedBeforeCollect()
{
    DeferredRelease::Enable();
    {
        DescriptorHeap heap;
        heap.InitializeBookkeepingOnly(4);
        heap.Free(heap.AllocateIndex());
    }
    DeferredRelease::Stamp(30);
    DeferredRelease::Collect(30);
    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
    CHECK(true);
}

// Stamp の後に Free した分は、次のフェンスまで回収されない。
static void Test_FreeAfterStamp()
{
    DescriptorHeap heap;
    heap.InitializeBookkeepingOnly(4);
    DeferredRelease::Enable();

    u32 a = heap.AllocateIndex();
    DeferredRelease::Stamp(40);
    heap.Free(a);                                 // Stamp の後
    DeferredRelease::Collect(40);
    CHECK(heap.GetFreeCount() == 3);              // 戻らない
    DeferredRelease::Stamp(41);
    DeferredRelease::Collect(41);
    CHECK(heap.GetFreeCount() == 4);

    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
}

// 破棄したヒープと同じアドレスに別のヒープが再構築されても、古いコールバックはそれを書き換えない。
// 同じヒープの Initialize し直しでも同様。
static void Test_StaleCallback()
{
    alignas(DescriptorHeap) unsigned char buf[sizeof(DescriptorHeap)];
    DeferredRelease::Enable();

    auto* h1 = new (buf) DescriptorHeap();
    h1->InitializeBookkeepingOnly(4);
    h1->Free(h1->AllocateIndex());
    h1->~DescriptorHeap();

    auto* h2 = new (buf) DescriptorHeap();
    h2->InitializeBookkeepingOnly(4);
    u32 x = h2->AllocateIndex();
    DeferredRelease::Stamp(50);
    DeferredRelease::Collect(50);
    CHECK(h2->GetFreeCount() == 3);               // 古いコールバックが二重に Free していない
    (void)x;

    // Initialize し直し
    h2->Free(h2->AllocateIndex());
    h2->InitializeBookkeepingOnly(4);
    u32 y = h2->AllocateIndex();
    (void)y;
    DeferredRelease::Stamp(51);
    DeferredRelease::Collect(51);
    CHECK(h2->GetFreeCount() == 3);
    h2->~DescriptorHeap();

    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
}

// シェーダから見えないヒープ(RTV/DSV 等)は即時に戻る。
static void Test_NonShaderVisibleImmediate()
{
    DescriptorHeap heap;
    heap.InitializeBookkeepingOnly(4, false);
    DeferredRelease::Enable();
    u32 a = heap.AllocateIndex();
    heap.Free(a);
    CHECK(heap.GetFreeCount() == 4);
    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
}

// コールバックの中から Defer しても(Collect が lock を持たないので)デッドロックしない。
static void Test_ReentrantCallback()
{
    DeferredRelease::Enable();
    static bool s_ran;
    s_ran = false;
    DeferredRelease::Defer(
        [](u64, u32, u32)
        {
            DeferredRelease::Defer([](u64, u32, u32) { s_ran = true; }, 0, 0, 0);
        },
        0, 0, 0);
    DeferredRelease::Stamp(60);
    DeferredRelease::Collect(60);
    DeferredRelease::Stamp(61);
    DeferredRelease::Collect(61);
    CHECK(s_ran);
    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
}

// Block も同じ: フェンス完了後に同じブロックを再度確保できる。
static void Test_BlockReallocAfterFence()
{
    DescriptorHeap heap;
    heap.InitializeBookkeepingOnly(4);
    DeferredRelease::Enable();
    u32 b = heap.AllocateBlock(4);
    CHECK(b != DescriptorHeap::kInvalidIndex);
    heap.FreeBlock(b, 4);
    CHECK(heap.GetFreeCount() == 0);   // まだ戻っていない(枯渇時の AllocateBlock は throw するので数で見る)
    DeferredRelease::Stamp(70);
    DeferredRelease::Collect(70);
    CHECK(heap.AllocateBlock(4) == b);
    DeferredRelease::Disable();
    DeferredRelease::FlushAll();
}

// kInvalidIndex の Free は無視され、キューにも積まれない。Disable 後の Free は即時。
static void Test_InvalidAndDisabledMix()
{
    DescriptorHeap heap;
    heap.InitializeBookkeepingOnly(4);
    DeferredRelease::Enable();
    u32 a = heap.AllocateIndex();
    u32 b = heap.AllocateIndex();
    heap.Free(DescriptorHeap::kInvalidIndex);
    heap.FreeBlock(DescriptorHeap::kInvalidIndex, 2);
    heap.Free(a);                                  // 遅延
    CHECK(DeferredRelease::GetPendingCount() == 1);
    DeferredRelease::Disable();
    heap.Free(b);                                  // 無効時は即時
    CHECK(heap.GetFreeCount() == 3);
    DeferredRelease::FlushAll();                   // 遅延分も戻る
    CHECK(heap.GetFreeCount() == 4);
    CHECK(DeferredRelease::GetPendingCount() == 0);
}

int main()
{
    Test_BlockReallocAfterFence();
    Test_InvalidAndDisabledMix();
    Test_FreeAfterStamp();
    Test_StaleCallback();
    Test_NonShaderVisibleImmediate();
    Test_ReentrantCallback();
    Test_SingleIndex();
    Test_Block();
    Test_ImmediateWhenDisabled();
    Test_HeapDestroyedBeforeCollect();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
