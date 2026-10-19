#pragma once
// 読み込みの内訳（プロジェクト / シーンを開くときに、どの段で何秒かかったか）。
// 各段で ScopedLoadTimer を置くだけ。複数スレッドから足してよい（atomic）。
// ApplicationProject のロード完了で Logger へ 1 行出して 0 に戻す。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dx12e::loadprof
{

enum class Slot : uint32_t
{
    AssimpRead,     // Assimp の ReadFile（解析 + 後処理）
    MeshOptimize,   // Mesh::Initialize の頂点の融合・キャッシュ最適化
    MeshLod,        // LOD の簡略化
    MeshUpload,     // 頂点 / インデックスバッファの作成
    TexHash,        // テクスチャの中身のハッシュ（BC キャッシュのキー）
    TexCacheRead,   // BC キャッシュ（.texcache）の読み込み
    TexDecode,      // キャッシュに無い / DDS: デコード・ミップ・BC 圧縮
    TexAnalyze,     // 透明の有無・G/B 平均
    TexUpload,      // GPU リソースの作成と転送
    TexFileRead,    // テクスチャのファイルをメモリへ読む（VFS）
    ModelTotal,     // ModelLoader::LoadFromFile 全体（上の assimp / mesh* / tex* を含む）
    Count
};

inline const char* SlotName(Slot s)
{
    static const char* kNames[] = { "assimp", "meshOpt", "meshLod", "meshUpload",
                                    "texHash", "texCache", "texDecode", "texAnalyze", "texUpload",
                                    "texRead", "modelTotal" };
    return kNames[static_cast<uint32_t>(s)];
}

inline std::atomic<int64_t>& Counter(Slot s)
{
    static std::atomic<int64_t> counters[static_cast<uint32_t>(Slot::Count)] = {};
    return counters[static_cast<uint32_t>(s)];
}

inline void Add(Slot s, std::chrono::steady_clock::duration d)
{
    Counter(s).fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(d).count(),
                         std::memory_order_relaxed);
}

// "assimp 4.12s / meshOpt 1.20s / ..." の形で返して 0 に戻す。0 の段は出さない。
inline std::string TakeSummary()
{
    std::string out;
    for (uint32_t i = 0; i < static_cast<uint32_t>(Slot::Count); ++i)
    {
        const int64_t us = Counter(static_cast<Slot>(i)).exchange(0, std::memory_order_relaxed);
        if (us <= 0) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s %.2fs", out.empty() ? "" : " / ",
                 SlotName(static_cast<Slot>(i)), static_cast<double>(us) / 1.0e6);
        out += buf;
    }
    return out;
}

class ScopedLoadTimer
{
public:
    explicit ScopedLoadTimer(Slot s) : m_slot(s), m_t0(std::chrono::steady_clock::now()) {}
    ~ScopedLoadTimer() { Add(m_slot, std::chrono::steady_clock::now() - m_t0); }
    ScopedLoadTimer(const ScopedLoadTimer&) = delete;
    ScopedLoadTimer& operator=(const ScopedLoadTimer&) = delete;

private:
    Slot                                  m_slot;
    std::chrono::steady_clock::time_point m_t0;
};

} // namespace dx12e::loadprof
