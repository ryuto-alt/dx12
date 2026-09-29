#pragma once

// スプラッシュ系スレッド（窓 / 音）からの診断メッセージ置き場。
// Logger は Application::Initialize の途中で初期化される共有ポインタで、別スレッドから触ると競合するので、
// スプラッシュのスレッドは直接ログせず、ここへ積む。メインスレッドが SplashScreen::FlushDiagnostics() で
// Logger へ流す（Application が Logger 初期化後・スプラッシュ終了時に呼ぶ）。OutputDebugString にも即時に出す。

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>

namespace dx12e
{

struct SplashDiagStore
{
    std::mutex mu;
    std::vector<std::string> lines;
    std::atomic<int> count{0};     // lines.size()（ロックを取らずに「流すものがあるか」を見る）
};

inline SplashDiagStore& SplashDiagStoreInstance()
{
    static SplashDiagStore* s = new SplashDiagStore();   // 意図的にリーク（終了時のスレッド競合を避ける）
    return *s;
}

inline void SplashDiag(const std::string& msg)
{
    OutputDebugStringA(("[splash] " + msg + "\n").c_str());
    auto& s = SplashDiagStoreInstance();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.lines.size() < 256) { s.lines.push_back(msg); s.count.store(static_cast<int>(s.lines.size())); }
}

inline bool SplashDiagPending() { return SplashDiagStoreInstance().count.load() > 0; }

inline std::vector<std::string> SplashDiagTake()
{
    auto& s = SplashDiagStoreInstance();
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<std::string> out;
    out.swap(s.lines);
    s.count.store(0);
    return out;
}

} // namespace dx12e
