#include "DeferredRelease.h"

#include <algorithm>
#include <iterator>

namespace dx12e
{

std::mutex                          DeferredRelease::s_mutex;
std::vector<DeferredRelease::Entry> DeferredRelease::s_entries;
bool                                DeferredRelease::s_enabled = false;
size_t                              DeferredRelease::s_peak    = 0;

void DeferredRelease::Enable()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_enabled = true;
}

void DeferredRelease::Disable()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_enabled = false;
}

bool DeferredRelease::IsEnabled()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_enabled;
}

void DeferredRelease::ReleaseEntry(Entry& e)
{
    // D3D12MA の流儀: resource の参照を落としてから allocation を Release する
    if (e.fn)
    {
        auto fn = e.fn;
        e.fn = nullptr;
        fn(e.pa, e.pb, e.pc);
    }
    e.res.Reset();
    if (e.alloc)
    {
        e.alloc->Release();
        e.alloc = nullptr;
    }
}

void DeferredRelease::Defer(Microsoft::WRL::ComPtr<ID3D12Resource> res, D3D12MA::Allocation* alloc)
{
    if (!res && !alloc)
        return;

    Entry e{std::move(res), alloc};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_enabled)
        {
            s_entries.push_back(std::move(e));
            s_peak = std::max(s_peak, s_entries.size());
            return;
        }
    }
    ReleaseEntry(e);   // 無効時は lock の外で即時解放
}

void DeferredRelease::Defer(Callback fn, u64 a, u32 b, u32 c)
{
    if (!fn)
        return;

    Entry e;
    e.fn = fn;
    e.pa = a;
    e.pb = b;
    e.pc = c;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_enabled)
        {
            s_entries.push_back(std::move(e));
            s_peak = std::max(s_peak, s_entries.size());
            return;
        }
    }
    ReleaseEntry(e);   // 無効時は lock の外で即時実行
}

size_t DeferredRelease::GetPendingCount()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_entries.size();
}

size_t DeferredRelease::GetPeakPendingCount()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_peak;
}

void DeferredRelease::Stamp(u64 fenceValue)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& e : s_entries)
    {
        if (e.fence == 0)
            e.fence = fenceValue;
    }
}

void DeferredRelease::Collect(u64 completedValue)
{
    // 回収分を lock の中でローカルへ移し、解放(コールバック含む)は lock の外で行う。
    // コールバックから Defer / DescriptorHeap::Free を呼んでもデッドロックしない。
    // 毎フレーム呼ばれるので、回収対象が無いときは確保しない(ready は回収があるときだけ伸びる)。
    std::vector<Entry> ready;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_entries.empty())
            return;
        // 順序は意味を持たない(回収対象は全部解放する)ので in-place の partition で足りる。
        // fence==0 は今フレーム登録されたばかり(Stamp前) = まだ解放できない
        auto mid = std::partition(s_entries.begin(), s_entries.end(),
            [&](const Entry& e) { return e.fence == 0 || e.fence > completedValue; });
        if (mid == s_entries.end())
            return;
        ready.assign(std::make_move_iterator(mid), std::make_move_iterator(s_entries.end()));
        s_entries.erase(mid, s_entries.end());
    }
    for (auto& e : ready)
        ReleaseEntry(e);
}

void DeferredRelease::FlushAll()
{
    std::vector<Entry> all;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        all.swap(s_entries);
    }
    for (auto& e : all)
        ReleaseEntry(e);
}

} // namespace dx12e
