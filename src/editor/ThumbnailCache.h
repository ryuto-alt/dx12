#pragma once

// ===== テクスチャのサムネイル（非同期デコード + ディスクキャッシュ + 予算つきアップロード）=====
// 以前は 256px 上限のテクスチャを GetOrLoadTexture でメインスレッド読み込みしていた（1 フレーム 3 枚。4K の PNG は 1 枚で数十〜百 ms）。
// しかも可視外のセルも全件キューに積んでいた。今は:
//   ・要求は「今フレームに描いたセル」だけ（Get() を呼んだものだけ。呼ばれなくなった要求は自動で捨てる）。
//   ・デコード（DirectXTex）と縮小（長辺 128px）はワーカースレッド。結果は <assets>/.thumbcache/<hash>.utb に保存
//     （ヘッダに元ファイルの更新時刻 / サイズ / 生成バージョンを持ち、合わなければ作り直し）。
//   ・メインスレッドは Pump() で「できあがった RGBA を GPU テクスチャへ上げる」だけ（1 フレーム N 枚まで）。
//   ・常駐数に上限を置き、使われなくなった物から解放する（GPU が使い終わるまで遅延解放）。
// モデルのサムネイル（ModelThumbnailRenderer）も同じ .utb 形式と ThumbGpu を使う。

#include "core/Types.h"
#include "editor/AssetBrowserLogic.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include <directx/d3d12.h>

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;

// RGBA8 の小さな画像を持つ GPU テクスチャ + SRV。
struct ThumbGpu
{
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;   // コピーが GPU で終わるまで保持する（呼び出し側が遅延解放）
    u32 srv    = 0xFFFFFFFFu;
    u64 handle = 0;                                   // ImTextureID（GPU デスクリプタハンドル）
    u32 width = 0, height = 0;
    bool Valid() const { return tex != nullptr && handle != 0; }
};

// RGBA8（w*h*4 バイト）から GPU テクスチャを作ってコピーを cmdList へ積む。成功で true。
bool CreateThumbTexture(GraphicsDevice& device, DescriptorHeap& srvHeap, ID3D12GraphicsCommandList* cmdList,
                        const uint8_t* rgba, u32 w, u32 h, ThumbGpu& out);

class ThumbnailCache
{
public:
    ~ThumbnailCache() { Shutdown(); }

    void Initialize(GraphicsDevice* device, DescriptorHeap* srvHeap);
    void Shutdown();
    // プロジェクトの assets フォルダ。サムネイルは <assetsDir>/.thumbcache/ に置く。切替で常駐を全部捨てる。
    void SetAssetsDir(const std::string& assetsDir);
    const std::string& CacheDir() const { return m_cacheDir; }

    enum class State : u8 { None, Queued, Ready, Failed };

    // 可視セルが毎フレーム呼ぶ。Ready なら GPU ハンドルを返す。未要求なら要求を積む（mtime / size は一覧の値）。
    u64 Get(const std::string& absPath, int64_t mtime, uint64_t size, State* stateOut = nullptr);

    // メインスレッド・フレーム先頭（cmdList が有効な間）に 1 回。できあがった分を GPU へ上げ、古い物を解放する。
    void Pump(ID3D12GraphicsCommandList* cmdList);

    // 常駐しているサムネイルの寸法（縦横比を保って描くため）。無ければ false。
    bool SizeOf(const std::string& absPath, u32& w, u32& h) const
    {
        auto it = m_items.find(absPath);
        if (it == m_items.end() || !it->second.gpu.Valid()) return false;
        w = it->second.gpu.width; h = it->second.gpu.height;
        return true;
    }
    size_t ResidentCount() const { return m_resident; }
    size_t QueuedCount() const;
    u64    UploadedTotal() const { return m_uploadedTotal; }
    u64    DecodedTotal() const { return m_decodedTotal.load(); }
    u64    CacheHitTotal() const { return m_cacheHits.load(); }

    static constexpr size_t kMaxResident   = 1500;   // これを超えたら、しばらく使われていない物から解放
    static constexpr int    kUploadsPerFrame = 8;    // 1 フレームに GPU へ上げる枚数の上限
    static constexpr u64    kWantedTimeoutFrames = 180;   // これだけ Get されなければ未処理の要求を捨てる

private:
    struct Item
    {
        State    state = State::None;
        int64_t  mtime = 0;
        uint64_t size = 0;
        u64      lastWanted = 0;    // 最後に Get された Pump カウンタ
        ThumbGpu gpu;
    };
    struct Job { std::string path; int64_t mtime = 0; uint64_t size = 0; u64 wanted = 0; };
    struct Done
    {
        std::string path;
        int64_t mtime = 0; uint64_t size = 0;
        bool ok = false, dropped = false;
        u32 w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    struct Grave { ThumbGpu gpu; u64 frame = 0; };

    void WorkerLoop();

    GraphicsDevice* m_device = nullptr;
    DescriptorHeap* m_srvHeap = nullptr;
    std::string     m_cacheDir;

    // メインスレッド専有
    std::unordered_map<std::string, Item> m_items;
    std::deque<Grave> m_graveyard;
    // アップロードバッファはコピーが GPU で終わるまで保持する（frame 付き）
    std::deque<std::pair<u64, Microsoft::WRL::ComPtr<ID3D12Resource>>> m_uploadKeep;
    size_t m_resident = 0;
    u64    m_uploadedTotal = 0;
    u64    m_frame = 0;

    // ワーカーとの受け渡し
    std::thread             m_worker;
    mutable std::mutex      m_mtx;
    std::condition_variable m_cv;
    std::vector<Job>        m_jobs;
    std::vector<Done>       m_done;
    bool                    m_stop = false;
    std::string             m_workerCacheDir;    // ワーカーが読む（m_mtx で保護）
    std::atomic<u64>        m_frameAtomic{0};
    std::atomic<u64>        m_decodedTotal{0};
    std::atomic<u64>        m_cacheHits{0};
};

} // namespace dx12e
