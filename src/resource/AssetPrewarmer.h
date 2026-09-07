#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Types.h"
#include "resource/TextureLoader.h"

namespace dx12e
{

// プロジェクト内のテクスチャの BC 圧縮キャッシュ(assets/.texcache/)を、
// バックグラウンドで先に作っておくための小さなワーカー。
//
// ■ なぜ要るか
//   BC7 圧縮は 1 枚あたり数秒かかる CPU 処理で、これまでは「シーンを開いた時に、
//   メインスレッドで、必要になった順に」実行していた。実測(GTX 1650 ノート /
//   62 モデルのシーン)ではキャッシュが冷えていると 1 シーン開くのに 49 秒かかり、
//   その間エディタは進捗表示以外なにもできない。git でブランチを切り替えるたびに
//   これを踏むので、作業が実際に止まっていた。
//
// ■ 何をするか
//   TextureLoader::PrewarmCompressedCache を回すだけ。これは D3D12 に触れず、
//   成果物は .texcache に置かれる .dds ファイルなので、
//   **描画側のコードは 1 行も変えずに** 後続の読み込みがキャッシュヒットになる。
//
// ■ 方針
//   - ワーカーは 1 本、優先度 BELOW_NORMAL。BC 圧縮は内部で
//     TEX_COMPRESS_PARALLEL により全コアを使うので、ワーカーを増やしても速くならず
//     編集中のエディタを邪魔するだけ。優先度を下げてあるので、操作している間は
//     メインスレッドが必ず勝つ。
//   - 既にキャッシュがある物は即座に飛ばす(起動のたびに作り直さない)。
//   - 中断はいつでも可能。プロジェクトを閉じる/終了する前に必ず Stop() すること。
class AssetPrewarmer
{
public:
    struct Item
    {
        std::wstring absPath;                             // テクスチャの絶対パス
        bool         srgb  = true;                        // 実ロードと同じ値にすること
        TextureUsage usage = TextureUsage::BaseColor;     // 同上(キャッシュキーに入る)
    };

    ~AssetPrewarmer();

    // items を先読みする。既に走っていれば何もしない。
    void Start(std::vector<Item> items);
    // 中断してワーカーを回収する(実行中でなければ何もしない)。デストラクタからも呼ばれる。
    void Stop();

    bool        Running() const { return m_running.load(std::memory_order_relaxed); }
    size_t      Total() const   { return m_total.load(std::memory_order_relaxed); }
    size_t      Done() const    { return m_done.load(std::memory_order_relaxed); }
    // 実際に圧縮した数(= 時間がかかった数)。既にキャッシュ済みだった物は含まない。
    size_t      Compressed() const { return m_compressed.load(std::memory_order_relaxed); }
    // いま処理中のファイル名(UI 表示用)。
    std::string Current() const;

private:
    void Run(std::vector<Item> items);

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_abort{false};
    std::atomic<size_t> m_total{0};
    std::atomic<size_t> m_done{0};
    std::atomic<size_t> m_compressed{0};

    mutable std::mutex m_currentMutex;
    std::string        m_current;
};

} // namespace dx12e
