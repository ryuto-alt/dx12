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

// ---- 窓の置き場所（カメラが地面から離れたとき）----------------------------------------------------------
// 窓はカメラを中心に追従するが、縦の幅は (counts.y-1)*spacing しかない（既定 8 段: 近景 0.8m で 5.6m / 遠景 3m で 21m）。
// カメラが上がると窓が地面の上へ抜け、路面・壁の下の方が DDGI の外＝遮蔽の無い IBL（空）で照らされる。
// Bistro で、高さ 15m を超えると日陰がどんどん明るく浮き、下りると暗く戻っていた（2026-10-17）。
// そこで GPU のレイでカメラの真下と視線の先の面を測り（DdgiGround_CS）、窓の下端が「面の 1 セル下」より
// 上へ行かないように窓の中心を下げる。下げた分だけ、水平方向も視線の先（見えている所）へ寄せる。
// ★面より 1 セル下まで下げずに済む高さ（＝地上の視点）では cam をそのまま返す＝従来と同じ窓。
struct GroundHits
{
    bool  downHit = false;  float down[3] = {0, 0, 0};   // カメラの真下の面
    bool  viewHit = false;  float view[3] = {0, 0, 0};   // 視線の中心の先の面
};

inline void AnchorForCascade(const float cam[3], const GroundHits& g, float spacing, const int counts[3], float out[3])
{
    for (int a = 0; a < 3; ++a) out[a] = cam[a];
    const float sp    = std::max(spacing, 1.0e-4f);
    const float halfX = 0.5f * static_cast<float>(counts[0] - 1) * sp;
    const float halfY = 0.5f * static_cast<float>(counts[1] - 1) * sp;
    const float halfZ = 0.5f * static_cast<float>(counts[2] - 1) * sp;
    // 視線の先の面の効き wv: 水平距離が窓の半幅までは 1、そこから半幅の 2 倍で 0 へ線形に落とす。
    //   地平線の向こうの低い面で窓を引っぱらない。★しきい値で入り切りすると、カメラが数 cm 動いただけで
    //   窓が半幅（遠景で 31.5m）一気に飛び、日陰の明るさが前後移動で進んだり戻ったりして見えた（2026-10-17）。
    //   ここに出てくる量はすべてカメラ位置とヒット点に対して連続にしてある。
    const float lim  = std::min(halfX, halfZ);
    const float vdx = g.view[0] - cam[0], vdz = g.view[2] - cam[2];
    const float vlen = std::sqrt(vdx * vdx + vdz * vdz);
    const float wv = g.viewHit ? std::clamp((2.0f * lim - vlen) / std::max(lim, 1.0e-4f), 0.0f, 1.0f) : 0.0f;
    if (!g.downHit && wv <= 0.0f) return;
    float refY;
    if (g.downHit) refY = g.down[1] - wv * std::max(g.down[1] - g.view[1], 0.0f);   // 低い方へ wv の割合だけ寄せる
    else           refY = g.view[1];                                              // 真下に何も無い: 視線の先だけ（wv > 0）
    const float maxCenterY = refY + halfY - sp;   // 窓の下端 = refY - 1 セル
    if (cam[1] <= maxCenterY) return;
    out[1] = maxCenterY;
    if (wv <= 0.0f || vlen <= 1.0e-4f) return;
    // 下げた量が縦の半分に達したら、視線の先へ最大で窓の半幅まで寄せる（途中は比例）。真下だけで決めたとき（wv が小さい）は寄せも小さい。
    const float k     = std::min((cam[1] - maxCenterY) / std::max(halfY, 1.0e-4f), 1.0f);
    const float scale = (vlen > lim ? lim / vlen : 1.0f) * k * wv;
    out[0] += vdx * scale;
    out[2] += vdz * scale;
}

// 真下の面の高さのならし（屋根の上を横切るたびに窓が上下へ飛ばないように）。
// 下がるときは即座に追う（路面が窓の外へ出るのが本題の不具合なので遅らせない）。上がるときはゆっくり
// （1 回あたり差の kRiseRatio + kRiseMeters）。屋根を 1 秒（60 回）横切っても 25m 中 7m 程度しか上がらない。
constexpr float kGroundRiseRatio  = 0.005f;
constexpr float kGroundRiseMeters = 0.03f;
inline float FilterGroundY(bool hadPrev, float prev, float now)
{
    if (!hadPrev || now <= prev) return now;
    return std::min(now, prev + (now - prev) * kGroundRiseRatio + kGroundRiseMeters);
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

// カスケード数の上限（クリップマップ。段 c の間隔は c0 の約 2^c 倍）。
constexpr int kMaxCascades = 4;

// N 段（1..kMaxCascades）への一般化。段 c の周期 Tc = kNearWeight^c * T0（近い段の kNearWeight 倍ずつ長い）。
//   kc = nc / Tc、Σkc = P → k0 = P / (1 + Σ_{c>=1} nc / (kNearWeight^c * n0))。
//   中間の段は理想値を四捨五入、最後の段は残り（N=2 のときは従来の式そのもの）。
struct SplitN { uint32_t k[kMaxCascades] = {0, 0, 0, 0}; };
inline SplitN SplitBudgetN(uint32_t P, const uint32_t* n, int count)
{
    SplitN s;
    count = std::clamp(count, 1, kMaxCascades);
    if (count == 1) { s.k[0] = std::min(P, n[0]); return s; }
    uint32_t sum = 0;
    for (int c = 0; c < count; ++c) sum += n[c];
    // 全部更新できるなら全部
    if (P >= sum) { for (int c = 0; c < count; ++c) s.k[c] = n[c]; return s; }
    // 周期 T0 = n0/k0, T1 = n1/k1 = kNearWeight * T0 → k1 = n1 * k0 / (kNearWeight * n0)、k0 + k1 = P
    float a = 0.0f;
    float wc = 1.0f;
    float ratio[kMaxCascades] = {1.0f, 0.0f, 0.0f, 0.0f};   // nc / (w^c * n0)
    for (int c = 1; c < count; ++c)
    {
        wc *= kNearWeight;
        ratio[c] = static_cast<float>(n[c]) / (wc * static_cast<float>(n[0]));
        a += ratio[c];
    }
    const float k0f = static_cast<float>(P) / (1.0f + a);
    s.k[0] = std::min(n[0], std::max(1u, static_cast<uint32_t>(k0f + 0.5f)));
    uint32_t rest = P > s.k[0] ? P - s.k[0] : 0u;
    for (int c = 1; c < count - 1; ++c)
    {
        const uint32_t kc = std::min(n[c], std::max(1u, static_cast<uint32_t>(k0f * ratio[c] + 0.5f)));
        s.k[c] = kc;
        rest = rest > kc ? rest - kc : 0u;
    }
    s.k[count - 1] = std::min(n[count - 1], std::max(1u, rest));
    return s;
}

inline Split SplitBudget(uint32_t P, uint32_t n0, uint32_t n1)
{
    Split s;
    const uint32_t n[2] = {n0, n1};
    const SplitN r = SplitBudgetN(P, n, n1 == 0 ? 1 : 2);
    s.k0 = r.k[0];
    s.k1 = r.k[1];
    return s;
}

// 更新周期（フレーム数）の見積り。有効ヒステリシスを h^period にして、間引いても実時間の収束速度を保つ。
// ★ただし kMinEffectiveHysteresis で下を止める。遠景は約 29 フレームに 1 回しか更新されず、0.97^29 = 0.41 だと
//   1 回の更新で 64 本のレイの推定値が履歴の 6 割を置き換え、更新のたびに日陰の明るさが揺れる
//   （定常のノイズの分散は 1 回の推定の (1-h)/(1+h) 倍 = 0.42 倍。0.9 なら 0.053 倍）。収束は遅くなる（遠景で約 5 秒）。
//   設定のヒステリシスがこれより低いときは設定を優先する（下限は min(h, 0.9)）。
constexpr float kMinEffectiveHysteresis = 0.9f;
inline float EffectiveHysteresis(float h, uint32_t n, uint32_t k)
{
    const float hc = std::clamp(h, 0.0f, 0.995f);
    if (k == 0 || n == 0) return hc;
    const float period = std::max(1.0f, static_cast<float>(n) / static_cast<float>(k));
    return std::max(std::pow(hc, period), std::min(hc, kMinEffectiveHysteresis));
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
