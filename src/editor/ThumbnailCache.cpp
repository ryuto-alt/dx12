#include "editor/ThumbnailCache.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "core/Logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace dx12e
{

// ============================================================ GPU テクスチャの作成

bool CreateThumbTexture(GraphicsDevice& device, DescriptorHeap& srvHeap, ID3D12GraphicsCommandList* cmdList,
                        const uint8_t* rgba, u32 w, u32 h, ThumbGpu& out)
{
    if (!cmdList || !rgba || w == 0 || h == 0) return false;
    ID3D12Device* dev = device.GetDevice();

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = w;
    texDesc.Height           = h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES defHeap{D3D12_HEAP_TYPE_DEFAULT};
    ThumbGpu g;
    if (FAILED(dev->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.tex))))
        return false;

    const u32 rowPitch = (w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const u64 uploadSize = static_cast<u64>(rowPitch) * h;
    D3D12_HEAP_PROPERTIES upHeap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = uploadSize;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g.upload))))
        return false;

    void* mapped = nullptr;
    if (FAILED(g.upload->Map(0, nullptr, &mapped))) return false;
    for (u32 y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * rowPitch, rgba + static_cast<size_t>(y) * w * 4u, static_cast<size_t>(w) * 4u);
    g.upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g.upload.Get();
    src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = g.tex.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = g.tex.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &b);

    g.srv = srvHeap.AllocateIndex();
    if (g.srv == DescriptorHeap::kInvalidIndex) return false;   // ヒープ枯渇（ここまでの資源は ComPtr が解放する）
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    dev->CreateShaderResourceView(g.tex.Get(), &srvDesc, srvHeap.GetCpuHandle(g.srv));
    g.handle = srvHeap.GetGpuHandle(g.srv).ptr;
    g.width = w; g.height = h;
    out = std::move(g);
    return true;
}

// ============================================================ デコード（ワーカースレッド）

namespace
{
// 画像を読んで長辺 maxDim 以内の RGBA8 にする。失敗で false。COM は呼び出しスレッドで初期化済みであること。
bool DecodeThumb(const std::filesystem::path& file, u32 maxDim, std::vector<uint8_t>& rgba, u32& outW, u32& outH)
{
    using namespace DirectX;
    std::wstring ext = file.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));

    ScratchImage loaded;
    HRESULT hr;
    if (ext == L".dds")      hr = LoadFromDDSFile(file.c_str(), DDS_FLAGS_NONE, nullptr, loaded);
    else if (ext == L".tga") hr = LoadFromTGAFile(file.c_str(), nullptr, loaded);
    else                     hr = LoadFromWICFile(file.c_str(), WIC_FLAGS_NONE, nullptr, loaded);
    if (FAILED(hr)) return false;

    const Image* img = loaded.GetImage(0, 0, 0);   // 配列 / キューブ / ミップ連鎖は先頭の 1 枚だけ
    if (!img) return false;

    ScratchImage decompressed;
    if (IsCompressed(img->format))
    {
        if (FAILED(Decompress(*img, DXGI_FORMAT_R8G8B8A8_UNORM, decompressed))) return false;
        img = decompressed.GetImage(0, 0, 0);
        if (!img) return false;
    }

    ScratchImage converted;
    if (img->format != DXGI_FORMAT_R8G8B8A8_UNORM && img->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        if (FAILED(Convert(*img, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted))) return false;
        img = converted.GetImage(0, 0, 0);
        if (!img) return false;
    }

    u32 nw = 0, nh = 0;
    abl::FitThumbSize(static_cast<u32>(img->width), static_cast<u32>(img->height), maxDim, nw, nh);
    if (nw == 0 || nh == 0) return false;

    ScratchImage resized;
    if (nw != img->width || nh != img->height)
    {
        if (FAILED(Resize(*img, nw, nh, TEX_FILTER_BOX, resized))) return false;
        img = resized.GetImage(0, 0, 0);
        if (!img) return false;
    }

    outW = static_cast<u32>(img->width);
    outH = static_cast<u32>(img->height);
    rgba.resize(static_cast<size_t>(outW) * outH * 4u);
    for (u32 y = 0; y < outH; ++y)
        std::memcpy(rgba.data() + static_cast<size_t>(y) * outW * 4u, img->pixels + static_cast<size_t>(y) * img->rowPitch, static_cast<size_t>(outW) * 4u);
    return true;
}
} // namespace

// ============================================================ ThumbnailCache

void ThumbnailCache::Initialize(GraphicsDevice* device, DescriptorHeap* srvHeap)
{
    m_device = device;
    m_srvHeap = srvHeap;
    if (!m_worker.joinable())
    {
        m_stop = false;
        m_worker = std::thread([this] { WorkerLoop(); });
    }
}

void ThumbnailCache::Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    // GPU が使い終わっている前提の終了時。SRV は ヒープごと破棄されるので個別の Free はしない。
    m_items.clear();
    m_graveyard.clear();
    m_uploadKeep.clear();
    m_resident = 0;
}

void ThumbnailCache::SetAssetsDir(const std::string& assetsDir)
{
    std::string dir = assetsDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir.push_back('/');
    dir += ".thumbcache/";
    if (dir == m_cacheDir) return;
    m_cacheDir = dir;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_workerCacheDir = dir;
        m_jobs.clear();
        m_done.clear();
    }
    // 常駐は全部墓場へ（GPU が使い終わるまで遅延解放）。
    for (auto& kv : m_items)
        if (kv.second.gpu.Valid()) m_graveyard.push_back({std::move(kv.second.gpu), m_frame});
    m_items.clear();
    m_resident = 0;
}

size_t ThumbnailCache::QueuedCount() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_jobs.size();
}

u64 ThumbnailCache::Get(const std::string& absPath, int64_t mtime, uint64_t size, State* stateOut)
{
    Item& it = m_items[absPath];
    it.lastWanted = m_frame;
    // 元ファイルが更新された → 作り直しを要求する（古い絵は新しいのが届くまで出し続ける）。
    const bool changed = (it.state == State::Ready || it.state == State::Failed) && (it.mtime != mtime || it.size != size);
    if (it.state == State::None || changed)
    {
        it.mtime = mtime;
        it.size = size;
        it.state = State::Queued;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_jobs.push_back(Job{absPath, mtime, size, m_frame});
        }
        m_cv.notify_one();
    }
    else if (it.state == State::Queued)
    {
        // 待機中の要求の「最終要求フレーム」を更新（ワーカーは新しい物から処理し、古い物は捨てる）
        std::lock_guard<std::mutex> lk(m_mtx);
        for (Job& j : m_jobs) if (j.path == absPath) { j.wanted = m_frame; break; }
    }
    if (stateOut) *stateOut = it.state;
    return (it.state == State::Ready || (it.gpu.Valid() && it.state == State::Queued)) ? it.gpu.handle : 0;
}

void ThumbnailCache::Pump(ID3D12GraphicsCommandList* cmdList)
{
    ++m_frame;
    m_frameAtomic.store(m_frame);
    if (!m_device || !m_srvHeap) return;

    // ---- 遅延解放（GPU が使い終わった物）----
    constexpr u64 kGraveFrames = 8;
    while (!m_graveyard.empty() && m_frame - m_graveyard.front().frame >= kGraveFrames)
    {
        Grave& g = m_graveyard.front();
        if (g.gpu.srv != 0xFFFFFFFFu) m_srvHeap->Free(g.gpu.srv);
        m_graveyard.pop_front();
    }
    while (!m_uploadKeep.empty() && m_frame - m_uploadKeep.front().first >= kGraveFrames)
        m_uploadKeep.pop_front();

    // ---- できあがった分を GPU へ ----
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        const size_t n = (std::min)(m_done.size(), static_cast<size_t>(kUploadsPerFrame));
        done.assign(std::make_move_iterator(m_done.begin()), std::make_move_iterator(m_done.begin() + static_cast<ptrdiff_t>(n)));
        m_done.erase(m_done.begin(), m_done.begin() + static_cast<ptrdiff_t>(n));
    }
    for (Done& d : done)
    {
        auto it = m_items.find(d.path);
        if (it == m_items.end()) continue;     // プロジェクト切替などで消えた
        Item& item = it->second;
        if (d.dropped)
        {
            // 誰も見ていないので処理しなかった。絵が無ければ未要求へ、古い絵があればそれを出したまま「次に見られたら作り直す」印を付ける。
            if (item.state == State::Queued)
            {
                if (item.gpu.Valid()) { item.state = State::Ready; item.mtime = -1; }
                else item.state = State::None;
            }
            continue;
        }
        if (d.mtime != item.mtime || d.size != item.size) continue;   // 古い版の結果（更新後に再要求済み）
        if (!d.ok || !cmdList) { item.state = State::Failed; continue; }
        ThumbGpu g;
        if (!CreateThumbTexture(*m_device, *m_srvHeap, cmdList, d.rgba.data(), d.w, d.h, g)) { item.state = State::Failed; continue; }
        if (item.gpu.Valid()) m_graveyard.push_back({std::move(item.gpu), m_frame});
        else ++m_resident;
        m_uploadKeep.emplace_back(m_frame, g.upload);
        item.gpu = std::move(g);
        item.gpu.upload.Reset();
        item.state = State::Ready;
        ++m_uploadedTotal;
    }

    // ---- 常駐数の上限: しばらく使われていない物から解放 ----
    if (m_resident > kMaxResident)
    {
        std::vector<std::pair<u64, std::string>> order;
        order.reserve(m_items.size());
        for (auto& kv : m_items)
            if (kv.second.gpu.Valid() && m_frame - kv.second.lastWanted > 60) order.emplace_back(kv.second.lastWanted, kv.first);
        std::sort(order.begin(), order.end());
        const size_t excess = m_resident - kMaxResident + kMaxResident / 8;   // 少し余分に空けて毎フレームは走らせない
        for (size_t i = 0; i < order.size() && i < excess; ++i)
        {
            Item& item = m_items[order[i].second];
            m_graveyard.push_back({std::move(item.gpu), m_frame});
            item.gpu = ThumbGpu{};
            item.state = State::None;
            --m_resident;
        }
    }
}

void ThumbnailCache::WorkerLoop()
{
    const bool comOk = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_lock<std::mutex> lk(m_mtx);
    for (;;)
    {
        m_cv.wait(lk, [&] { return m_stop || !m_jobs.empty(); });
        if (m_stop) break;

        // 一番新しく要求された物から（スクロールで見えなくなった要求は後回し → 期限切れで捨てる）
        size_t best = 0;
        for (size_t i = 1; i < m_jobs.size(); ++i)
            if (m_jobs[i].wanted > m_jobs[best].wanted) best = i;
        Job job = m_jobs[best];
        m_jobs.erase(m_jobs.begin() + static_cast<ptrdiff_t>(best));
        const u64 nowFrame = m_frameAtomic.load();
        const std::string cacheDir = m_workerCacheDir;

        Done d;
        d.path = job.path; d.mtime = job.mtime; d.size = job.size;
        if (nowFrame > job.wanted + kWantedTimeoutFrames)
        {
            d.dropped = true;   // もう誰も見ていない
            m_done.push_back(std::move(d));
            continue;
        }
        lk.unlock();

        // キャッシュの置き場が未設定の間はディスクを使わない（相対パスでカレントへ書き散らさない）。
        const bool useDisk = !cacheDir.empty();
        const std::filesystem::path cacheFile = useDisk ? std::filesystem::path(cacheDir) / abl::ThumbFileName(job.path) : std::filesystem::path();
        bool ok = useDisk && abl::ReadThumbFile(cacheFile, job.mtime, job.size, d.w, d.h, d.rgba);
        if (ok)
        {
            m_cacheHits.fetch_add(1);
        }
        else
        {
            ok = DecodeThumb(std::filesystem::path(job.path), abl::kThumbDim, d.rgba, d.w, d.h);
            if (ok)
            {
                m_decodedTotal.fetch_add(1);
                if (useDisk) abl::WriteThumbFile(cacheFile, d.w, d.h, job.mtime, job.size, d.rgba.data());
            }
        }
        d.ok = ok;

        lk.lock();
        m_done.push_back(std::move(d));
    }
    if (comOk) ::CoUninitialize();
}

} // namespace dx12e
