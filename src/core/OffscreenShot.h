#pragma once
// ===========================================================================
// OffscreenShot — 任意解像度のオフスクリーン出力（screenshot_final {width,height} / 起動引数 --size WxH）の純ロジック。
// ---------------------------------------------------------------------------
// 依存は標準ライブラリだけ（GPU も Windows ヘッダも要らない）。単体テスト tests/offscreen_shot_test.cpp。
//   ・"1920x1080" の解釈
//   ・上限の検証（1 辺 / 総画素 / 線形 float 出力の総画素）とエラー文
//   ・GPU メモリの見積（シーン系 RT 一式の 1 画素あたりのバイト数 × 画素数）と利用可能量との照合
// 実際の描画は Application::ServiceOffscreenShot（ApplicationOffscreenShot.cpp）と RenderPostChain が担う。
// ===========================================================================
#include <cstdint>
#include <string>

namespace dx12e::offshot
{

inline constexpr uint32_t kMinSide         = 16;            // これ未満は RT を作れない / 意味が無い
inline constexpr uint32_t kMaxSide         = 8192;          // 1 辺の上限（D3D12 の 16384 より控えめ。GPU メモリのため）
inline constexpr uint64_t kMaxPixels       = 33'554'432ull; // 総画素の上限 = 8192 x 4096
inline constexpr uint64_t kMaxLinearPixels = 16'777'216ull; // 線形 float 出力(pfm/exr)の上限 = 4096 x 4096（float4 の UAV + readback で 2 倍要る）
// シーン系 RT 一式（シーン HDR / 深度 / AO / Hi-Z / ブルーム / TAA 履歴 / G-Buffer / SSGI・SSR / DoF / MB ほか）の
// 1 画素あたりの概算バイト数（1080p の実測から丸めた上限寄りの値）。見積は安全側（大きめ）に置く。
inline constexpr uint64_t kBytesPerPixel   = 160;
// 利用可能な GPU メモリ（予算 − 使用中）のうち、撮影のために食ってよい割合。
inline constexpr double   kVramShare       = 0.6;

enum class Verdict
{
    Ok,
    TooSmall,        // 幅か高さが kMinSide 未満
    TooWide,         // 1 辺が kMaxSide 超
    TooManyPixels,   // 総画素が kMaxPixels 超
    LinearTooBig,    // 線形 float 出力なのに総画素が kMaxLinearPixels 超
    NoVram,          // 見積が利用可能量を超える
};

// "1920x1080" / "1920X1080" / "1920,1080" / "1920 1080"。前後の空白は無視。0 や負値、数字以外は false。
inline bool ParseSize(const std::string& s, uint32_t& w, uint32_t& h)
{
    size_t i = 0;
    auto skipWs = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; };
    auto number = [&](uint64_t& out) -> bool
    {
        skipWs();
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        out = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        {
            out = out * 10 + static_cast<uint64_t>(s[i] - '0');
            if (out > 1'000'000ull) return false;   // 桁あふれの早期打ち切り（上限検証は別）
            ++i;
        }
        return true;
    };
    uint64_t a = 0, b = 0;
    if (!number(a)) return false;
    const size_t afterA = i;
    skipWs();
    const bool hadWs = i > afterA;
    if (i < s.size() && (s[i] == 'x' || s[i] == 'X' || s[i] == ',')) ++i;   // "1920x1080" / "1920 x 1080" / "1920,1080"
    else if (!hadWs) return false;                                        // "1920 1080"（空白だけの区切り）以外は不可
    if (!number(b)) return false;
    skipWs();
    if (i != s.size() || a == 0 || b == 0) return false;
    w = static_cast<uint32_t>(a);
    h = static_cast<uint32_t>(b);
    return true;
}

inline Verdict Validate(uint32_t w, uint32_t h, bool linearFloat)
{
    if (w < kMinSide || h < kMinSide) return Verdict::TooSmall;
    if (w > kMaxSide || h > kMaxSide) return Verdict::TooWide;
    const uint64_t px = static_cast<uint64_t>(w) * h;
    if (px > kMaxPixels) return Verdict::TooManyPixels;
    if (linearFloat && px > kMaxLinearPixels) return Verdict::LinearTooBig;
    return Verdict::Ok;
}

inline uint64_t EstimateVramBytes(uint32_t w, uint32_t h, bool linearFloat)
{
    const uint64_t px = static_cast<uint64_t>(w) * h;
    // 線形出力は float4 の UAV バッファ + readback（各 16 B/px）を一時的に足す
    return px * kBytesPerPixel + (linearFloat ? px * 32ull : 0ull);
}

// availableBytes = GPU の予算 − 使用中。0 なら「取得できなかった」ので照合しない（Ok）。
inline Verdict CheckVram(uint32_t w, uint32_t h, bool linearFloat, uint64_t availableBytes)
{
    if (availableBytes == 0) return Verdict::Ok;
    const double allow = static_cast<double>(availableBytes) * kVramShare;
    return static_cast<double>(EstimateVramBytes(w, h, linearFloat)) > allow ? Verdict::NoVram : Verdict::Ok;
}

inline std::string Describe(Verdict v, uint32_t w, uint32_t h)
{
    const std::string sz = std::to_string(w) + "x" + std::to_string(h);
    switch (v)
    {
    case Verdict::Ok:           return "ok";
    case Verdict::TooSmall:     return "size " + sz + " is too small (min " + std::to_string(kMinSide) + " per side)";
    case Verdict::TooWide:      return "size " + sz + " exceeds the per-side limit " + std::to_string(kMaxSide);
    case Verdict::TooManyPixels:return "size " + sz + " exceeds the pixel limit " + std::to_string(kMaxPixels) + " (8192x4096)";
    case Verdict::LinearTooBig: return "size " + sz + " is too large for a linear float output (pfm/exr limit "
                                     + std::to_string(kMaxLinearPixels) + " px = 4096x4096); use png or a smaller size";
    case Verdict::NoVram:       return "size " + sz + " needs more GPU memory than is available right now";
    }
    return "invalid size";
}

// 起動引数 --size で指定された既定の撮影サイズ（0 = 未指定）。screenshot_final が width/height を省いたときに使う。
struct CliDefault { uint32_t w = 0, h = 0; };
inline CliDefault& Cli() { static CliDefault d; return d; }

} // namespace dx12e::offshot
