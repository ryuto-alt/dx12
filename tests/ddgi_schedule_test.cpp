// DDGI のカメラ追従スクロール格子 + 更新スケジュール + ライトグリッドの純ロジック（src/renderer/DdgiSchedule.h。GI S4）の単体テスト。
// GPU も窓も要らない（ヘッダだけ）。
//   ・窓の追従: 初回は中心に置いて全リセット / ヒステリシス内では動かない（リセットのばたつき防止）/ 動いたら storage = (local + scroll) mod counts
//   ・リングバッファの性質: 窓が動いても残るプローブの記憶領域は変わらない / 入ってきたプローブだけがリセット待ちになる（全方向・複数セル・斜め）
//   ・ラウンドロビン: 予算 k なら ceil(n/k) フレームで全プローブを 1 周する / リセット待ちは飛ばす
//   ・予算: 超過なら下げる・余裕なら上げる・[min,max] に収まる / 近いカスケードの更新頻度は遠い方の kNearWeight 倍
//   ・ライトグリッド: セルの影響灯リストは「その点に効く灯」を取りこぼさない（総当たりと同じ集合）

#include "renderer/DdgiSchedule.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

using namespace dx12e::ddgi;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

// 決定的な乱数（テストの再現性）
uint32_t g_rng = 12345u;
float Rand01() { g_rng = g_rng * 1664525u + 1013904223u; return static_cast<float>((g_rng >> 8) & 0xFFFFFF) / 16777216.0f; }
float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

// 窓の中の格子座標（絶対）→ 記憶領域の番号（リングバッファの定義そのもの: lattice mod counts）
uint32_t StorageOfLattice(const int lat[3], const int counts[3])
{
    const int x = PosMod(lat[0], counts[0]), y = PosMod(lat[1], counts[1]), z = PosMod(lat[2], counts[2]);
    return static_cast<uint32_t>(x + y * counts[0] + z * counts[0] * counts[1]);
}

void TestWindowBasics()
{
    const int counts[3] = {16, 12, 16};
    Window w;
    const float cam[3] = {3.1f, 1.7f, -4.2f};
    const Move m0 = UpdateWindow(w, cam, 0.6f, counts);
    CHECK(w.valid && m0.fullReset);
    // カメラが窓の中心（± 0.5 セル）に来る
    for (int a = 0; a < 3; ++a)
    {
        const float centerLat = w.base[a] + (counts[a] - 1) * 0.5f + kLatticePhase;
        CHECK(std::fabs(centerLat - cam[a] / 0.6f) <= 0.5f + 1e-4f);
        CHECK(w.scroll[a] == PosMod(w.base[a], counts[a]));
    }
    float org[3];
    WindowOrigin(w, 0.6f, org);
    for (int a = 0; a < 3; ++a) CHECK(std::fabs(org[a] - (w.base[a] + kLatticePhase) * 0.6f) < 1e-5f);

    // ヒステリシス内（0.75 セル未満）は動かない
    float cam2[3] = {cam[0] + 0.4f * 0.6f, cam[1] - 0.3f * 0.6f, cam[2] + 0.2f * 0.6f};
    const Window before = w;
    const Move m1 = UpdateWindow(w, cam2, 0.6f, counts);
    CHECK(!m1.moved());
    CHECK(w.base[0] == before.base[0] && w.base[1] == before.base[1] && w.base[2] == before.base[2]);

    // 境界でカメラが ±0.4 セルふらついても、窓は動かない（リセットのばたつき防止）
    for (int i = 0; i < 40; ++i)
    {
        float c3[3] = {cam[0] + ((i & 1) ? 0.4f : -0.4f) * 0.6f, cam[1], cam[2]};
        CHECK(!UpdateWindow(w, c3, 0.6f, counts).moved());
    }

    // 3 セル分動けば 3 セル動く
    float cam4[3] = {cam[0] + 3.0f * 0.6f, cam[1], cam[2]};
    const Move m2 = UpdateWindow(w, cam4, 0.6f, counts);
    CHECK(m2.shift[0] == 3 && m2.shift[1] == 0 && m2.shift[2] == 0 && !m2.fullReset);
    CHECK(w.base[0] == before.base[0] + 3);

    // 窓より大きく動いたら全リセット
    float cam5[3] = {cam[0] + 100.0f, cam[1], cam[2]};
    CHECK(UpdateWindow(w, cam5, 0.6f, counts).fullReset);
}

void TestStorageIsPermutation()
{
    const int counts[3] = {5, 3, 4};
    Window w;
    w.valid = true;
    for (int trial = 0; trial < 50; ++trial)
    {
        for (int a = 0; a < 3; ++a) { w.base[a] = static_cast<int>(RandRange(-50.0f, 50.0f)); w.scroll[a] = PosMod(w.base[a], counts[a]); }
        std::set<uint32_t> seen;
        int c[3];
        for (c[2] = 0; c[2] < counts[2]; ++c[2])
            for (c[1] = 0; c[1] < counts[1]; ++c[1])
                for (c[0] = 0; c[0] < counts[0]; ++c[0])
                {
                    const uint32_t st = StorageIndex(c, w, counts);
                    seen.insert(st);
                    int back[3];
                    LocalFromStorage(st, w, counts, back);
                    CHECK(back[0] == c[0] && back[1] == c[1] && back[2] == c[2]);
                    // 定義: 記憶領域 = 格子座標 mod counts
                    const int lat[3] = {w.base[0] + c[0], w.base[1] + c[1], w.base[2] + c[2]};
                    CHECK(st == StorageOfLattice(lat, counts));
                }
        CHECK(static_cast<int>(seen.size()) == counts[0] * counts[1] * counts[2]);
    }
}

// 窓を動かしたとき: 残るプローブの記憶領域は変わらない / 入ってきたプローブだけがリセット待ちになる
void TestEnteringOnlyNewProbes()
{
    const int counts[3] = {8, 6, 7};
    const int n = counts[0] * counts[1] * counts[2];
    for (int trial = 0; trial < 200; ++trial)
    {
        Window w;
        float cam[3] = {RandRange(-20, 20), RandRange(-3, 6), RandRange(-20, 20)};
        UpdateWindow(w, cam, 0.5f, counts);
        const Window old = w;
        // いろいろな動き（1 軸 / 斜め / 大きめ）
        float cam2[3];
        for (int a = 0; a < 3; ++a) cam2[a] = cam[a] + RandRange(-2.5f, 2.5f);
        const Move mv = UpdateWindow(w, cam2, 0.5f, counts);
        std::vector<uint8_t> pending(n, 0);
        MarkEntering(w, mv, counts, pending);
        // 期待: 新しい窓の格子座標のうち古い窓に無かったものの記憶領域
        std::set<uint32_t> expected;
        int c[3];
        for (c[2] = 0; c[2] < counts[2]; ++c[2])
            for (c[1] = 0; c[1] < counts[1]; ++c[1])
                for (c[0] = 0; c[0] < counts[0]; ++c[0])
                {
                    const int lat[3] = {w.base[0] + c[0], w.base[1] + c[1], w.base[2] + c[2]};
                    bool inOld = true;
                    for (int a = 0; a < 3; ++a) inOld = inOld && lat[a] >= old.base[a] && lat[a] < old.base[a] + counts[a];
                    const uint32_t st = StorageOfLattice(lat, counts);
                    if (!inOld) expected.insert(st);
                    else
                    {
                        // 残るプローブは記憶領域が変わらない（= 履歴をそのまま使える）
                        int oldLocal[3] = {lat[0] - old.base[0], lat[1] - old.base[1], lat[2] - old.base[2]};
                        CHECK(StorageIndex(oldLocal, old, counts) == st);
                        CHECK(!pending[st] || mv.fullReset);
                    }
                }
        std::set<uint32_t> got;
        for (int i = 0; i < n; ++i) if (pending[i]) got.insert(static_cast<uint32_t>(i));
        if (mv.fullReset) CHECK(static_cast<int>(got.size()) == n);
        else              CHECK(got == expected);
        // 動いていなければ何もリセットしない
        if (!mv.moved()) CHECK(got.empty());
    }
}

void TestRoundRobinAndBudget()
{
    // 1 周: n 個を 1 フレーム k 個ずつ → ceil(n/k) フレームで全部に当たる
    const uint32_t n = 1000, k = 130;
    std::vector<uint8_t> pending(n, 0);
    uint32_t cursor = 0;
    std::set<uint32_t> hit;
    uint32_t frames = 0;
    while (hit.size() < n && frames < 100)
    {
        std::vector<uint32_t> out;
        PickRoundRobin(n, k, cursor, 0, pending, out);
        CHECK(out.size() == k);
        for (uint32_t i : out) hit.insert(i);
        ++frames;
    }
    CHECK(hit.size() == n);
    CHECK(frames == (n + k - 1) / k);

    // リセット待ちは飛ばす（別枠で入るので二重に入らない）
    for (uint32_t i = 0; i < n; i += 3) pending[i] = 1;
    {
        std::vector<uint32_t> out;
        cursor = 0;
        PickRoundRobin(n, 200, cursor, 0, pending, out);
        CHECK(out.size() == 200);
        for (uint32_t i : out) CHECK(pending[i] == 0);
        // オフセット（2 番目のカスケード）
        std::vector<uint8_t> pend2(2 * n, 0);
        std::vector<uint32_t> out2;
        uint32_t cur2 = 0;
        PickRoundRobin(n, 10, cur2, n, pend2, out2);
        for (uint32_t i : out2) CHECK(i >= n && i < 2 * n);
    }
    // 全部リセット待ちなら 0 個で止まる（無限ループしない）
    {
        std::vector<uint8_t> all(n, 1);
        std::vector<uint32_t> out;
        CHECK(PickRoundRobin(n, 50, cursor, 0, all, out) == 0);
    }

    // 予算の分配
    {
        const Split full = SplitBudget(5000, 2000, 2000);
        CHECK(full.k0 == 2000 && full.k1 == 2000);
        const Split part = SplitBudget(1000, 2000, 2000);
        CHECK(part.k0 + part.k1 <= 1001 && part.k0 + part.k1 >= 999);
        // 近いカスケードは遠い方の kNearWeight 倍の頻度（周期の比 = 1 : kNearWeight）
        const float t0 = 2000.0f / part.k0, t1 = 2000.0f / part.k1;
        CHECK(std::fabs(t1 / t0 - kNearWeight) < 0.1f);
        const Split one = SplitBudget(300, 2000, 0);
        CHECK(one.k0 == 300 && one.k1 == 0);
        const Split tiny = SplitBudget(1, 2000, 2000);
        CHECK(tiny.k0 >= 1 && tiny.k1 >= 1);
    }
    // 予算の制御
    {
        uint32_t p = 4000;
        // 費用が高い（0.5ms / 1000 プローブ・目標 1.0ms → 2000 個が適正）: 下がっていって 2000 付近へ収束する
        for (int i = 0; i < 40; ++i) p = NextProbeBudget(p, 0.0005f, 1.0f, 64, 4000);
        CHECK(p >= 1900 && p <= 2100);
        // 余裕がある（目標 1.0ms・費用 0.0001ms/個 → 10000 個入る）: 上限まで上がる
        for (int i = 0; i < 80; ++i) p = NextProbeBudget(p, 0.0001f, 1.0f, 64, 4000);
        CHECK(p == 4000);
        // 超過したら 1 回で大きく下げる（放置しない）
        CHECK(NextProbeBudget(4000, 0.002f, 1.0f, 64, 4000) <= 2800);
        // 下限
        { uint32_t q = 100; for (int i = 0; i < 20; ++i) q = NextProbeBudget(q, 10.0f, 1.0f, 64, 4000); CHECK(q == 64); }
        // 未計測は上限から始める
        CHECK(NextProbeBudget(0, 0.0f, 1.0f, 64, 4000) == 4000);
    }
    // 間引いたぶんはヒステリシスを h^周期 に（実時間の収束の速さを保つ）
    {
        CHECK(std::fabs(EffectiveHysteresis(0.97f, 1000, 1000) - 0.97f) < 1e-6f);
        CHECK(std::fabs(EffectiveHysteresis(0.97f, 1000, 250) - std::pow(0.97f, 4.0f)) < 1e-5f);
        CHECK(EffectiveHysteresis(0.97f, 1000, 10) < 0.05f);
    }
}

// ライトグリッドの割り当て: セルの影響灯リスト（球とセルの重なり）が、総当たりで「効く灯」を取りこぼさない
void TestLightGrid()
{
    const float gridOrigin[3] = {-10.0f, -2.0f, -10.0f};
    const float cell = 2.4f;
    const int dims[3] = {10, 5, 10};
    struct L { float p[3]; float range; };
    std::vector<L> lights;
    for (int i = 0; i < 60; ++i)
        lights.push_back({{RandRange(-12, 12), RandRange(-3, 8), RandRange(-12, 12)}, RandRange(1.0f, 7.0f)});

    // グリッドを作る（上限なし）
    const int cells = dims[0] * dims[1] * dims[2];
    std::vector<std::vector<int>> grid(cells);
    for (int cz = 0; cz < dims[2]; ++cz)
        for (int cy = 0; cy < dims[1]; ++cy)
            for (int cx = 0; cx < dims[0]; ++cx)
            {
                const float cmin[3] = {gridOrigin[0] + cx * cell, gridOrigin[1] + cy * cell, gridOrigin[2] + cz * cell};
                for (size_t i = 0; i < lights.size(); ++i)
                    if (SphereOverlapsCell(lights[i].p, lights[i].range, cmin, cell))
                        grid[cx + cy * dims[0] + cz * dims[0] * dims[1]].push_back(static_cast<int>(i));
            }
    // ランダムな点で: 総当たりで「効く灯（距離 < range）」の集合 == セルの灯のうち距離 < range のもの（取りこぼし無し・余計な灯は距離で落ちる）
    int inside = 0;
    for (int t = 0; t < 4000; ++t)
    {
        const float p[3] = {RandRange(-10, 14), RandRange(-2, 10), RandRange(-10, 14)};
        int c[3];
        if (!CellOf(p, gridOrigin, cell, dims, c)) continue;
        ++inside;
        const std::vector<int>& cl = grid[c[0] + c[1] * dims[0] + c[2] * dims[0] * dims[1]];
        std::set<int> viaGrid;
        for (int i : cl)
        {
            float d2 = 0; for (int a = 0; a < 3; ++a) { const float d = lights[i].p[a] - p[a]; d2 += d * d; }
            if (d2 < lights[i].range * lights[i].range) viaGrid.insert(i);
        }
        std::set<int> brute;
        for (size_t i = 0; i < lights.size(); ++i)
        {
            float d2 = 0; for (int a = 0; a < 3; ++a) { const float d = lights[i].p[a] - p[a]; d2 += d * d; }
            if (d2 < lights[i].range * lights[i].range) brute.insert(static_cast<int>(i));
        }
        CHECK(viaGrid == brute);
        // リストは灯の添字の昇順（足す順序が総当たりと同じ）
        CHECK(std::is_sorted(cl.begin(), cl.end()));
    }
    CHECK(inside > 500);
    // セルの境界・範囲外
    int c[3];
    const float outP[3] = {-10.5f, 0.0f, 0.0f};
    CHECK(!CellOf(outP, gridOrigin, cell, dims, c));
    const float edgeP[3] = {gridOrigin[0] + dims[0] * cell, 0.0f, 0.0f};   // 最後のセルの外
    CHECK(!CellOf(edgeP, gridOrigin, cell, dims, c));
    const float inP[3] = {gridOrigin[0] + 1e-3f, gridOrigin[1] + 1e-3f, gridOrigin[2] + 1e-3f};
    CHECK(CellOf(inP, gridOrigin, cell, dims, c) && c[0] == 0 && c[1] == 0 && c[2] == 0);
}
} // namespace

int main()
{
    TestWindowBasics();
    TestStorageIsPermutation();
    TestEnteringOnlyNewProbes();
    TestRoundRobinAndBudget();
    TestLightGrid();
    std::printf("DdgiScheduleTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
