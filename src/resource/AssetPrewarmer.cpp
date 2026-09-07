#include "resource/AssetPrewarmer.h"

#include "core/Logger.h"

#include <Windows.h>
#include <objbase.h>   // CoInitializeEx（WIC がスレッドごとに要求する）

#include <chrono>
#include <filesystem>

namespace dx12e
{

AssetPrewarmer::~AssetPrewarmer()
{
    Stop();
}

void AssetPrewarmer::Start(std::vector<Item> items)
{
    if (m_running.load()) return;
    if (items.empty())    return;

    m_abort.store(false);
    m_total.store(items.size());
    m_done.store(0);
    m_compressed.store(0);
    {
        std::lock_guard<std::mutex> lock(m_currentMutex);
        m_current.clear();
    }
    m_running.store(true);
    m_thread = std::thread(&AssetPrewarmer::Run, this, std::move(items));
}

void AssetPrewarmer::Stop()
{
    m_abort.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
}

std::string AssetPrewarmer::Current() const
{
    std::lock_guard<std::mutex> lock(m_currentMutex);
    return m_current;
}

void AssetPrewarmer::Run(std::vector<Item> items)
{
    // WIC(LoadFromWICFile / GetMetadataFromWICFile)はスレッドごとに COM の初期化を要求する。
    // これを忘れると全件 Failed になって「先読みしているのに一向に速くならない」になる。
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsCoUninit = SUCCEEDED(coHr);

    // 編集中のエディタより必ず後回しにする。BC 圧縮は内部で全コアを使うので、
    // 優先度を下げておかないと操作している側がカクつく。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    const auto t0 = std::chrono::steady_clock::now();
    size_t compressed = 0;

    for (const Item& it : items)
    {
        if (m_abort.load(std::memory_order_relaxed)) break;

        {
            std::lock_guard<std::mutex> lock(m_currentMutex);
            m_current = std::filesystem::path(it.absPath).filename().string();
        }

        const auto r = TextureLoader::PrewarmCompressedCache(it.absPath, it.srgb, it.usage);
        if (r == TextureLoader::PrewarmResult::Compressed)
        {
            ++compressed;
            m_compressed.store(compressed, std::memory_order_relaxed);
        }
        m_done.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(m_currentMutex);
        m_current.clear();
    }

    const f64 sec = std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
    // 行頭の "AssetPrewarm" は ASCII の目印。ログを機械的に追う計測スクリプトが
    // 日本語部分の文字コードに左右されずに完了を検出できるようにしてある。
    if (m_abort.load())
    {
        Logger::Info("AssetPrewarm aborted: アセット先読みを中断しました "
                     "({}/{} 件, うち圧縮 {} 件, {:.1f} 秒)",
                     m_done.load(), m_total.load(), compressed, sec);
    }
    else if (compressed > 0)
    {
        Logger::Info("AssetPrewarm done: アセット先読み完了 {} 件中 {} 件を圧縮 ({:.1f} 秒)。"
                     "以後このプロジェクトのシーンは待たずに開けます",
                     m_total.load(), compressed, sec);
    }
    else
    {
        Logger::Info("AssetPrewarm done: アセット先読み完了 {} 件すべてキャッシュ済み ({:.1f} 秒)",
                     m_total.load(), sec);
    }

    if (needsCoUninit) CoUninitialize();
    m_running.store(false);
}

} // namespace dx12e
