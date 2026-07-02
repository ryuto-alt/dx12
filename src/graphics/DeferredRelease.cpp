#include "DeferredRelease.h"

#include <algorithm>

namespace dx12e
{

std::mutex                          DeferredRelease::s_mutex;
std::vector<DeferredRelease::Entry> DeferredRelease::s_entries;
bool                                DeferredRelease::s_enabled = false;

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

    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_enabled)
    {
        Entry e{std::move(res), alloc, 0};
        ReleaseEntry(e);
        return;
    }
    s_entries.push_back(Entry{std::move(res), alloc, 0});
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
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = std::remove_if(s_entries.begin(), s_entries.end(),
        [&](Entry& e)
        {
            // fence==0 は今フレーム登録されたばかり(Stamp前) = まだ解放できない
            if (e.fence == 0 || e.fence > completedValue)
                return false;
            ReleaseEntry(e);
            return true;
        });
    s_entries.erase(it, s_entries.end());
}

void DeferredRelease::FlushAll()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& e : s_entries)
        ReleaseEntry(e);
    s_entries.clear();
}

} // namespace dx12e
