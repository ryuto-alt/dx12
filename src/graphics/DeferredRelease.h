#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <D3D12MemAlloc.h>

#include <mutex>
#include <vector>

#include "core/Types.h"

namespace dx12e
{

// GPU が使い終わるまで ID3D12Resource / D3D12MA::Allocation の解放を遅延させる仕組み。
//
// 毎フレーム末尾の WaitIdle(GPU全停止) を撤去してフレームを多重化(in-flight)すると、
// 「解放したいリソースを前フレームのGPUがまだ読んでいる」状況が生まれる。
// そこで解放要求を一旦キューに積み、フレーム送出時の Signal 値を刻んで(Stamp)、
// フェンス完了が確認できた分だけ実際に Release する(Collect)。
//
// 運用:
//   - Application::Initialize 末尾で Enable()。Enable 前/Disable 後の Defer は即時解放。
//   - GpuResource::ReleaseResource / Texture::FinishUpload が Defer() で登録。
//   - Application::Render 末尾: Stamp(送出フェンス値) → Collect(完了値)。
//   - Shutdown: WaitIdle 後に Disable() + FlushAll()。
//
// エンジンはデバイス/メインキューが1つなので static で持つ（プロジェクトローダ等の
// ワーカースレッドから間接的に呼ばれても安全なよう mutex で保護）。
class DeferredRelease
{
public:
    static void Enable();
    static void Disable();
    static bool IsEnabled();

    // res / alloc のどちらか一方だけでもよい（テクスチャのアップロードステージングは res のみ）。
    // 所有権を受け取る。alloc は呼び出し側でメンバを nullptr にすること。
    static void Defer(Microsoft::WRL::ComPtr<ID3D12Resource> res, D3D12MA::Allocation* alloc);

    // 任意の後始末を、現在投入中のフレームのフェンス完了後に実行する(W05-06: ディスクリプタの遅延返却)。
    // Enable 前/Disable 後は即時に実行する。fn は Collect/FlushAll の中で、
    // mutex の外で呼ばれる(fn から Defer / DescriptorHeap::Free を呼んでもよい)。
    // <functional> を避けるため、関数ポインタ + 3 つのペイロードで受ける(std::function の確保も無い)。
    using Callback = void (*)(u64 a, u32 b, u32 c);
    static void Defer(Callback fn, u64 a, u32 b, u32 c);

    // 診断: 現在キューに積まれている件数と、その最大値(Enable 以降)。
    static size_t GetPendingCount();
    static size_t GetPeakPendingCount();

    // 未確定(フェンス未割当)の登録分に「このフェンス値まで待て」を刻む。フレーム送出直後に呼ぶ。
    static void Stamp(u64 fenceValue);

    // 完了済みフェンス値以下の登録分を実際に解放する。
    static void Collect(u64 completedValue);

    // 全件即時解放。GPU が完全に止まっている(WaitIdle済み)前提で呼ぶこと。
    static void FlushAll();

private:
    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        D3D12MA::Allocation*                   alloc = nullptr;
        Callback                               fn = nullptr;
        u64                                    pa = 0;
        u32                                    pb = 0;
        u32                                    pc = 0;
        u64                                    fence = 0;   // 0 = 未確定(Stamp待ち)
    };

    static void ReleaseEntry(Entry& e);

    static std::mutex         s_mutex;
    static std::vector<Entry> s_entries;
    static bool               s_enabled;
    static size_t             s_peak;
};

} // namespace dx12e
