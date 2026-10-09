#pragma once
// ===========================================================================
// SplitScreenLayout — 画面分割（2〜4 区画）の矩形計算と、Lua が書く要求の置き場。ヘッダオンリー。
//   n=2     : 既定は左右 2 分割（Cols: area0=左 / area1=右）。Rows 指定なら上下（area0=上 / area1=下）
//   n=3 / 4 : 2x2（area0=左上 / 1=右上 / 2=左下 / 3=右下。n=3 の右下は空き）
// 区画の間は kSplitGap px（バックバッファのクリア色が見える＝仕切り線）。w/h は必ず 1 以上。
// ===========================================================================
#include <algorithm>
#include <string>

#include "core/Types.h"

namespace dx12e
{

constexpr u32 kSplitGap     = 2;
constexpr u32 kSplitMaxAreas = 4;

// n=2 のときだけ効く並べ方。Cols=左右（既定）/ Rows=上下。
enum class SplitLayout : u8 { Cols = 0, Rows = 1 };

struct SplitRect
{
    u32 x = 0, y = 0, w = 1, h = 1;
};

// 全体矩形 full を n 分割したときの区画 i（0 始まり）。n<=1 / i 範囲外は full をそのまま返す。
inline SplitRect ComputeSplitRect(const SplitRect& full, u32 n, u32 i, SplitLayout layout = SplitLayout::Cols)
{
    SplitRect fullOk = full;
    if (fullOk.w < 1) fullOk.w = 1;
    if (fullOk.h < 1) fullOk.h = 1;
    if (n < 2) return fullOk;
    if (n > kSplitMaxAreas) n = kSplitMaxAreas;
    if (i >= n) i = n - 1;

    const u32 gap = kSplitGap;
    auto half = [gap](u32 total, u32 idx, u32& off, u32& len)
    {
        // 2 本に割り、間に gap を挟む。小さすぎて割れない場合は 1px ずつ（重なり許容）。
        const u32 usable = (total > gap + 1) ? total - gap : 1;
        const u32 a = usable / 2;
        const u32 b = usable - a;
        if (idx == 0) { off = 0;            len = a; }
        else          { off = a + (total > gap + 1 ? gap : 0); len = b; }
        if (len < 1) len = 1;
    };

    SplitRect r{};
    if (n == 2)
    {
        if (layout == SplitLayout::Rows)
        {
            r.x = 0; r.w = fullOk.w;
            half(fullOk.h, i, r.y, r.h);
        }
        else
        {
            r.y = 0; r.h = fullOk.h;
            half(fullOk.w, i, r.x, r.w);
        }
    }
    else
    {
        half(fullOk.w, i % 2, r.x, r.w);
        half(fullOk.h, i / 2, r.y, r.h);
    }
    r.x += fullOk.x;
    r.y += fullOk.y;
    return r;
}

// Lua（scene:setSplitScreen / setSplitView）が書く要求。GPU 資源は持たない。
struct SplitPose
{
    bool set = false;
    f32  pos[3]    = {0, 0, 0};
    f32  target[3] = {0, 0, 1};
    f32  fovDeg    = 60.0f;
};

struct SplitScreenRequest
{
    u32 n = 0;   // 0 = 無効、2..4 = 有効
    SplitLayout layout = SplitLayout::Cols;   // n=2 の並べ方（Lua: setSplitScreen(2, "rows")）
    SplitPose poses[kSplitMaxAreas];   // [1..3] を使う（[0] はメインカメラ）

    void Reset() { *this = SplitScreenRequest{}; }
    // Lua の layout 文字列。"rows" のみ上下、それ以外（nil/"cols"/不明）は左右。
    void SetLayout(const std::string& s) { layout = (s == "rows") ? SplitLayout::Rows : SplitLayout::Cols; }
    // Lua からの n（0 / 1 → 無効、2..4、超過はクランプ）
    void SetCount(int v)
    {
        if (v < 2) { n = 0; return; }
        n = static_cast<u32>((std::min)(v, static_cast<int>(kSplitMaxAreas)));
    }
};

} // namespace dx12e
