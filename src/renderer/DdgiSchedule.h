#pragma once
//
// DdgiSchedule — DDGI（GI モード New）のカメラ追従スクロール格子と更新スケジュールの純ロジック（GI S4）。
//
// ★D3D12 に依存しない（ctest で固定する。tests/ddgi_schedule_test.cpp）。
//
// ■ スクロール格子（RTXGI の infinite scrolling と同じ）
//   格子は「間隔単位の整数格子」に載る。窓の最小コーナーの格子座標を base（int3）とすると、
//   ローカル座標 c（0..counts-1）のプローブのワールド位置 = (base + c + kLatticePhase) * spacing。
//   プローブの記憶領域（アトラス上の位置）は「ローカル座標をずらしたもの」:
//       storage = (c + scroll) mod counts      scroll = base mod counts
//   窓が 1 セル動いても残るプローブの storage は変わらない（履歴を保てる）。窓からはみ出した列だけが
//   反対側の新しい列へ「再利用」され、そのプローブの履歴だけを捨てる（= リセット）。
//
// ■ 更新スケジュール
//   1 フレームに更新するプローブ（更新リスト）= 「リセット待ちのプローブ（必ず）」+「予算ぶんのラウンドロビン」。
//   近いカスケードは遠いカスケードの kNearWeight 倍の頻度で更新する。予算は GPU 時間の実測から AIMD で決める。
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dx12e::ddgi
{

// 格子点のずらし（セルの何割か）。プローブが「壁・床の整数座標」に乗りにくくする。
constexpr float kLatticePhase = 0.5f;
// 窓を動かすしきい値（窓の中心からカメラまでの距離がこのセル数を超えたら動かす）。境界でのばたつき（リセットの連発）防止。
constexpr float kWindowHysteresisCells = 0.75f;
// 近いカスケードの更新頻度 : 遠いカスケード = kNearWeight : 1
constexpr float kNearWeight = 4.0f;
// 近いカスケードの外縁の混ぜ幅（セル数）。この幅の中で遠いカスケードへ滑らかに切り替える。
constexpr float kCascadeBlendCells = 1.0f;

constexpr uint32_t kResetBit = 0x80000000u;   // 更新リストの要素の最上位ビット = このプローブは履歴を捨てて埋め直す

inline int PosMod(int a, int n)
{
    const int r = a % n;
    return r < 0 ? r + n : r;
}

// 窓（1 カスケードぶん）。
struct Window
{
    bool valid = false;
    int  base[3]   = {0, 0, 0};   // 窓の最小コーナーの格子座標（間隔単位）
    int  scroll[3] = {0, 0, 0};   // base mod counts
};

// 窓の移動結果。
struct Move
{
    bool fullReset = false;       // 初回 / 1 軸でも counts 以上動いた → 全プローブをリセット
    int  shift[3]  = {0, 0, 0};   // 今回動いたセル数（fullReset でないとき）
    bool moved() const { return fullReset || shift[0] != 0 || shift[1] != 0 || shift[2] != 0; }
};

// カメラ位置 cam（ワールド）に合わせて窓を更新する。窓が無効（初回）なら中心に置いて fullReset。
inline Move UpdateWindow(Window& w, const float cam[3], float spacing, const int counts[3])
{
    Move m;
    const float sp = std::max(spacing, 1.0e-4f);
    if (!w.valid)
    {
        for (int a = 0; a < 3; ++a)
        {
            const float centerLat = cam[a] / sp - kLatticePhase;            // カメラの格子座標（位相補正済み）
            w.base[a] = static_cast<int>(std::floor(centerLat - (counts[a] - 1) * 0.5f + 0.5f));
            w.scroll[a] = PosMod(w.base[a], counts[a]);
        }
        w.valid = true;
        m.fullReset = true;
        return m;
    }
    for (int a = 0; a < 3; ++a)
    {
        const float centerLat = w.base[a] + (counts[a] - 1) * 0.5f;         // 窓の中心の格子座標
        const float camLat    = cam[a] / sp - kLatticePhase;
        const float diff      = camLat - centerLat;
        if (std::fabs(diff) > kWindowHysteresisCells)
        {
            const int s = static_cast<int>(std::lround(diff));
            m.shift[a] = s;
            w.base[a] += s;
            if (std::abs(s) >= counts[a]) m.fullReset = true;
        }
        w.scroll[a] = PosMod(w.base[a], counts[a]);
    }
    return m;
}

// 窓の最小コーナーのワールド座標。
inline void WindowOrigin(const Window& w, float spacing, float out[3])
{
    for (int a = 0; a < 3; ++a) out[a] = (static_cast<float>(w.base[a]) + kLatticePhase) * spacing;
}

// ローカル座標 → プローブの記憶領域の通し番号（カスケード内）。x + y*cx + z*cx*cy。
inline uint32_t StorageIndex(const int local[3], const Window& w, const int counts[3])
{
    const int x = PosMod(local[0] + w.scroll[0], counts[0]);
    const int y = PosMod(local[1] + w.scroll[1], counts[1]);
    const int z = PosMod(local[2] + w.scroll[2], counts[2]);
    return static_cast<uint32_t>(x + y * counts[0] + z * counts[0] * counts[1]);
}

// 記憶領域の通し番号 → ローカル座標（StorageIndex の逆）。
inline void LocalFromStorage(uint32_t storage, const Window& w, const int counts[3], int out[3])
{
    const int sx = static_cast<int>(storage % static_cast<uint32_t>(counts[0]));
    const int sy = static_cast<int>((storage / static_cast<uint32_t>(counts[0])) % static_cast<uint32_t>(counts[1]));
    const int sz = static_cast<int>(storage / static_cast<uint32_t>(counts[0] * counts[1]));
    out[0] = PosMod(sx - w.scroll[0], counts[0]);
    out[1] = PosMod(sy - w.scroll[1], counts[1]);
    out[2] = PosMod(sz - w.scroll[2], counts[2]);
}

// 窓の移動で「新しく入ってきた」ローカル座標か。
inline bool IsEntering(const int local[3], const Move& m, const int counts[3])
{
    if (m.fullReset) return true;
    for (int a = 0; a < 3; ++a)
    {
        const int s = m.shift[a];
        if (s > 0 && local[a] >= counts[a] - s) return true;
        if (s < 0 && local[a] < -s) return true;
    }
    return false;
}

// 窓の移動で入ってきたプローブの記憶領域の番号を pending（0/1 の配列。要素数 = counts の積）へ立てる。
inline void MarkEntering(const Window& w, const Move& m, const int counts[3], std::vector<uint8_t>& pending, uint32_t base = 0)
{
    if (!m.moved()) return;
    int c[3];
    for (c[2] = 0; c[2] < counts[2]; ++c[2])
        for (c[1] = 0; c[1] < counts[1]; ++c[1])
            for (c[0] = 0; c[0] < counts[0]; ++c[0])
                if (IsEntering(c, m, counts))
                    pending[base + StorageIndex(c, w, counts)] = 1;
}

// ラウンドロビン: n 個のうち、cursor から数えて count 個を out へ足す（pending が立っているものは別枠で入るので飛ばす）。
// 戻り値は実際に足した数。cursor は進める。
inline uint32_t PickRoundRobin(uint32_t n, uint32_t count, uint32_t& cursor, uint32_t offset,
                               const std::vector<uint8_t>& pending, std::vector<uint32_t>& out)
{
    if (n == 0) return 0;
    count = std::min(count, n);
    uint32_t added = 0;
    uint32_t scanned = 0;
    while (added < count && scanned < n)
    {
        const uint32_t i = cursor % n;
        cursor = (cursor + 1) % n;
        ++scanned;
        if (pending[offset + i]) continue;   // リセット枠で入る
        out.push_back(offset + i);
        ++added;
    }
    return added;
}

// 1 フレームの更新プローブ数の予算（全カスケード合計）を、計測した GPU 時間から決める（AIMD）。
//   cur        : 今の予算（プローブ数 / フレーム）
//   costMsPer  : 1 プローブあたりの GPU 時間(ms)の見積り（<= 0 なら未計測）
//   budgetMs   : 目標の GPU 時間(ms)
// 未計測のうちは maxP から始める（最初の 1 フレームで計測が付く）。結果は [minP, maxP]。
inline uint32_t NextProbeBudget(uint32_t cur, float costMsPer, float budgetMs, uint32_t minP, uint32_t maxP)
{
    if (maxP < minP) maxP = minP;
    if (!(costMsPer > 0.0f) || !(budgetMs > 0.0f)) return std::clamp(cur == 0 ? maxP : cur, minP, maxP);
    const float target = budgetMs / costMsPer;                 // 予算に収まるプローブ数
    float next = static_cast<float>(cur == 0 ? maxP : cur);
    // 下げるのは速く（超過を放置しない）、上げるのは緩く（振動を避ける）
    next = (target < next) ? std::max(target, next * 0.7f) : std::min(target, next * 1.15f + 8.0f);
    return std::clamp(static_cast<uint32_t>(next), minP, maxP);
}

// 予算 P をカスケードへ分ける。近い方は遠い方の kNearWeight 倍の頻度（= 1 プローブあたりの更新周期が 1/kNearWeight）。
// n0 / n1 は各カスケードのプローブ数（n1 = 0 で 1 カスケード）。結果は各カスケードの 1 フレームの更新数。
struct Split { uint32_t k0 = 0, k1 = 0; };
inline Split SplitBudget(uint32_t P, uint32_t n0, uint32_t n1)
{
    Split s;
    if (n1 == 0) { s.k0 = std::min(P, n0); return s; }
    // 全部更新できるなら全部
    if (P >= n0 + n1) { s.k0 = n0; s.k1 = n1; return s; }
    // 周期 T0 = n0/k0, T1 = n1/k1 = kNearWeight * T0 → k1 = n1 * k0 / (kNearWeight * n0)、k0 + k1 = P
    const float a = static_cast<float>(n1) / (kNearWeight * static_cast<float>(n0));
    const float k0f = static_cast<float>(P) / (1.0f + a);
    s.k0 = std::min(n0, std::max(1u, static_cast<uint32_t>(k0f + 0.5f)));
    const uint32_t rest = P > s.k0 ? P - s.k0 : 0u;
    s.k1 = std::min(n1, std::max(1u, rest));
    return s;
}

// 更新周期（フレーム数）の見積り。有効ヒステリシスを h^period にして、間引いても実時間の収束速度を保つ。
inline float EffectiveHysteresis(float h, uint32_t n, uint32_t k)
{
    if (k == 0 || n == 0) return h;
    const float period = std::max(1.0f, static_cast<float>(n) / static_cast<float>(k));
    return std::pow(std::clamp(h, 0.0f, 0.995f), period);
}

// ---- ライトグリッド（ヒット点の点光源を絞る）の CPU 側の参照実装 -------------------------------------------
// シェーダ（DdgiLightGrid_CS）と同じ割り当て規則。球（中心 pos・半径 range）と立方体セルの重なり判定。
inline bool SphereOverlapsCell(const float pos[3], float range, const float cellMin[3], float cellSize)
{
    float d2 = 0.0f;
    for (int a = 0; a < 3; ++a)
    {
        const float lo = cellMin[a], hi = cellMin[a] + cellSize;
        const float v = pos[a] < lo ? lo - pos[a] : (pos[a] > hi ? pos[a] - hi : 0.0f);
        d2 += v * v;
    }
    return d2 < range * range;   // シェーダ側の「dist >= range なら寄与 0」と同じ向きの不等号
}

// ワールド座標 → セル番号（範囲外なら false）。
inline bool CellOf(const float pos[3], const float gridOrigin[3], float cellSize, const int dims[3], int out[3])
{
    for (int a = 0; a < 3; ++a)
    {
        const float g = (pos[a] - gridOrigin[a]) / cellSize;
        if (g < 0.0f || g >= static_cast<float>(dims[a])) return false;
        out[a] = static_cast<int>(std::floor(g));
    }
    return true;
}

} // namespace dx12e::ddgi
