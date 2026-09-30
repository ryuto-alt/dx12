#pragma once

// ===== ノードグラフ UI の純ロジック: 座標変換 / ベジェ / スプリング / 空間分割 =====
// ImGui 非依存（tests/nodegraph_test.cpp が単体で検証する）。

#include "editor/nodegraph/GraphTypes.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dx12e::ng
{

inline constexpr float kMinZoom = 0.10f;
inline constexpr float kMaxZoom = 4.00f;

inline float ClampZoom(float z) { return std::min(kMaxZoom, std::max(kMinZoom, z)); }

// ---- ビューポート（グラフ座標 ⇔ 画面座標）----
// 画面 = origin + (g - pan) * scale,  scale = zoom * dpi。pan はキャンバス左上に来るグラフ座標。
struct Viewport
{
    Vec2  origin;           // キャンバスの画面上の左上（物理 px）
    Vec2  size;             // キャンバスの大きさ（物理 px）
    Vec2  pan;              // キャンバス左上のグラフ座標
    float zoom = 1.0f;      // 0.1 .. 4.0
    float dpi  = 1.0f;      // 表示倍率（theme::Scale()）

    float Scale() const { return zoom * dpi; }
    Vec2  ToScreen(Vec2 g) const { const float s = Scale(); return {origin.x + (g.x - pan.x) * s, origin.y + (g.y - pan.y) * s}; }
    Vec2  ToGraph(Vec2 p) const { const float s = Scale(); return {(p.x - origin.x) / s + pan.x, (p.y - origin.y) / s + pan.y}; }
    Rect  ToScreen(const Rect& r) const { return Rect(ToScreen(r.min), ToScreen(r.max)); }
    // 画面に見えているグラフ座標の範囲
    Rect  VisibleGraphRect() const { return Rect(ToGraph(origin), ToGraph(origin + size)); }

    // anchor（画面座標）の下にあるグラフ座標を動かさずに zoom を newZoom へ（カーソル中心ズーム）。
    void ZoomAt(Vec2 anchorScreen, float newZoom)
    {
        newZoom = ClampZoom(newZoom);
        const Vec2 g = ToGraph(anchorScreen);
        zoom = newZoom;
        const float s = Scale();
        pan = {g.x - (anchorScreen.x - origin.x) / s, g.y - (anchorScreen.y - origin.y) / s};
    }
    // グラフ矩形 r をキャンバスの中央に収める（margin は画面 px。zoomMax で拡大を抑える）。
    void FitRect(const Rect& r, float marginPx, float zoomMax)
    {
        if (IsEmptyRect(r) || size.x < 8.0f || size.y < 8.0f) return;
        const float w = std::max(1.0f, r.Width()), h = std::max(1.0f, r.Height());
        const float availW = std::max(16.0f, size.x - marginPx * 2.0f), availH = std::max(16.0f, size.y - marginPx * 2.0f);
        float z = std::min(availW / (w * dpi), availH / (h * dpi));
        z = std::min(std::max(z, kMinZoom), std::min(zoomMax, kMaxZoom));
        zoom = z;
        const Vec2 c = r.Center();
        const float s = Scale();
        pan = {c.x - size.x * 0.5f / s, c.y - size.y * 0.5f / s};
    }
};

// ---- ワイヤ（3 次ベジェ。水平接線）----
struct Bezier
{
    Vec2 p0, p1, p2, p3;
    Vec2 At(float t) const
    {
        const float u = 1.0f - t;
        const float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
        return {a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
    }
    // 制御点を含む外接矩形（ベジェは凸包に収まるので保守的なカリング用）。
    Rect Bounds() const
    {
        Rect r(p0, p0);
        for (Vec2 p : {p1, p2, p3})
        {
            r.min.x = std::min(r.min.x, p.x); r.min.y = std::min(r.min.y, p.y);
            r.max.x = std::max(r.max.x, p.x); r.max.y = std::max(r.max.y, p.y);
        }
        return r;
    }
};

// 出力ピン a（右向きに出る）→ 入力ピン b（左から入る）。距離に応じて張りを持たせる。
// 後ろ向き（b が a の左）のときは張りを大きくして「回り込む」形にする。
inline Bezier MakeWire(Vec2 a, Vec2 b)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    float t;
    if (dx >= 0.0f) t = std::max(36.0f, dx * 0.5f);
    else            t = std::max(36.0f, -dx * 0.5f + 60.0f + std::min(120.0f, std::fabs(dy) * 0.25f));
    t = std::min(t, 420.0f);
    return Bezier{a, {a.x + t, a.y}, {b.x - t, b.y}, b};
}

// 点 p からベジェまでの最短距離（グラフ座標）。segments 個の折れ線で近似。
inline float DistanceToBezier(Vec2 p, const Bezier& c, int segments = 24)
{
    float best = 1e30f;
    Vec2 prev = c.p0;
    for (int i = 1; i <= segments; ++i)
    {
        const Vec2 cur = c.At(static_cast<float>(i) / static_cast<float>(segments));
        // 線分 prev-cur への距離
        const Vec2 d = cur - prev;
        const float len2 = d.x * d.x + d.y * d.y;
        float t = 0.0f;
        if (len2 > 1e-9f) t = std::min(1.0f, std::max(0.0f, ((p.x - prev.x) * d.x + (p.y - prev.y) * d.y) / len2));
        const float qx = prev.x + d.x * t - p.x, qy = prev.y + d.y * t - p.y;
        best = std::min(best, std::sqrt(qx * qx + qy * qy));
        prev = cur;
    }
    return best;
}

// 線分 a-b とベジェ（折れ線近似）が交差するか（Ctrl+ドラッグの切断ストローク用）。
inline bool SegmentsIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    auto cross = [](Vec2 o, Vec2 p, Vec2 q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
    const float d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}
inline bool BezierIntersectsSegment(const Bezier& c, Vec2 a, Vec2 b, int segments = 24)
{
    Vec2 prev = c.p0;
    for (int i = 1; i <= segments; ++i)
    {
        const Vec2 cur = c.At(static_cast<float>(i) / static_cast<float>(segments));
        if (SegmentsIntersect(prev, cur, a, b)) return true;
        prev = cur;
    }
    return false;
}

// ---- スナップ ----
inline float SnapValue(float v, float step) { return step > 0.0f ? std::floor(v / step + 0.5f) * step : v; }
inline Vec2  SnapVec(Vec2 v, float step) { return {SnapValue(v.x, step), SnapValue(v.y, step)}; }

// ---- 連続アニメ用スプリング（臨界減衰に近い 2 次系。linear は使わない）----
// value が target へ滑らかに寄る。damping < 1 でわずかにオーバーシュート（選択の「ぽんっ」）。
struct Spring
{
    float value = 0.0f, vel = 0.0f;
    // freq: 固有振動数 [rad/s]（大きいほど速い）、damping: 減衰比（1 = 臨界）。半陰的オイラー（dt を刻んで安定）。
    void Step(float target, float dt, float freq = 26.0f, float damping = 1.0f)
    {
        if (dt <= 0.0f) return;
        if (dt > 0.1f) dt = 0.1f;
        // 暗黙オイラー（無条件に安定）。刻みを細かくして数値減衰を抑える（60fps で 4 分割）。
        const int sub = std::max(1, static_cast<int>(std::ceil(dt * 240.0f)));
        const float h = dt / static_cast<float>(sub);
        const float w2 = freq * freq;
        for (int i = 0; i < sub; ++i)
        {
            vel = (vel + h * w2 * (target - value)) / (1.0f + 2.0f * damping * freq * h + w2 * h * h);
            value += vel * h;
        }
        if (std::fabs(target - value) < 0.0005f && std::fabs(vel) < 0.005f) { value = target; vel = 0.0f; }
    }
    void Snap(float v) { value = v; vel = 0.0f; }
    bool Settled(float target) const { return value == target && vel == 0.0f; }
};

// ---- 空間分割（一様グリッド。ノード / コメント / ワイヤの外接矩形の高速な絞り込み）----
// セルはグラフ座標 256 単位。Query は「矩形と重なる項目」の候補を重複なしで返す
// （セル単位の絞り込みなので、呼び出し側が正確な判定をする）。
class SpatialGrid
{
public:
    explicit SpatialGrid(float cell = 256.0f) : m_cell(cell) {}
    void Clear() { m_cells.clear(); m_big.clear(); m_stamp = 0; m_seen.clear(); m_count = 0; }
    int  Count() const { return m_count; }
    void Insert(uint32_t id, const Rect& r)
    {
        ++m_count;
        int x0, y0, x1, y1;
        CellRange(r, x0, y0, x1, y1);
        // 巨大な矩形（全体を覆うコメント等）でセルを爆発させない: 上限を超えたら「大物」リストへ。
        if (static_cast<int64_t>(x1 - x0 + 1) * static_cast<int64_t>(y1 - y0 + 1) > 4096)
        {
            m_big.push_back(id);
            return;
        }
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                m_cells[Key(x, y)].push_back(id);
    }
    // r に触れるセルにある id を out へ追加（重複なし）。
    void Query(const Rect& r, std::vector<uint32_t>& out) const
    {
        int x0, y0, x1, y1;
        CellRange(r, x0, y0, x1, y1);
        ++m_stamp;
        if (m_stamp == 0) { std::fill(m_seen.begin(), m_seen.end(), 0u); m_stamp = 1; }
        auto push = [&](uint32_t id)
        {
            if (id >= m_seen.size()) m_seen.resize(static_cast<size_t>(id) + 1024u, 0u);
            uint32_t& s = m_seen[id];
            if (s == m_stamp) return;
            s = m_stamp;
            out.push_back(id);
        };
        if (static_cast<int64_t>(x1 - x0 + 1) * static_cast<int64_t>(y1 - y0 + 1) > 4096)
        {
            for (const auto& kv : m_cells) for (uint32_t id : kv.second) push(id);
        }
        else
        {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    auto it = m_cells.find(Key(x, y));
                    if (it == m_cells.end()) continue;
                    for (uint32_t id : it->second) push(id);
                }
        }
        for (uint32_t id : m_big) push(id);
    }

private:
    static uint64_t Key(int x, int y) { return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y); }
    void CellRange(const Rect& r, int& x0, int& y0, int& x1, int& y1) const
    {
        auto c = [&](float v) { return static_cast<int>(std::floor(v / m_cell)); };
        x0 = c(r.min.x); y0 = c(r.min.y); x1 = c(r.max.x); y1 = c(r.max.y);
    }
    float m_cell;
    int   m_count = 0;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_cells;
    std::vector<uint32_t> m_big;
    mutable std::vector<uint32_t> m_seen;   // id をそのまま添字に使う（重複除去の刻印）
    mutable uint32_t m_stamp = 0;
};

} // namespace dx12e::ng
