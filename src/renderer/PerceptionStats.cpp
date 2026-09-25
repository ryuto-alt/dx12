// 知覚層の集計ロジック（純関数）。指標の定義は PerceptionStats.h の先頭にまとめてある。
#include "renderer/PerceptionStats.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace dx12e::perception
{
namespace
{
inline float Sat01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// 1 エンティティ（またはグループ）ぶんの 1 パス目の積算。
struct Accum
{
    u32    pixels = 0;
    double sumX = 0, sumY = 0;
    u32    minX = ~0u, minY = ~0u, maxX = 0, maxY = 0;
    double sumLuma = 0, sumLuma2 = 0, sumSat = 0;
    double sumDist = 0;
    u32    distCount = 0;
    float  minDist = 0.0f;

    void Add(u32 x, u32 y, float luma, float sat, float dist)
    {
        ++pixels;
        sumX += x; sumY += y;
        minX = (std::min)(minX, x); minY = (std::min)(minY, y);
        maxX = (std::max)(maxX, x); maxY = (std::max)(maxY, y);
        sumLuma += luma; sumLuma2 += static_cast<double>(luma) * luma;
        sumSat  += sat;
        if (dist > 0.0f)
        {
            if (distCount == 0 || dist < minDist) minDist = dist;
            sumDist += dist;
            ++distCount;
        }
    }
};

struct RegionAccum
{
    u32    total = 0, empty = 0;
    double sumLuma = 0, sumLuma2 = 0;
    double sumDist = 0;
    u32    distCount = 0;

    void Add(bool isEmpty, float luma, float dist)
    {
        ++total;
        if (isEmpty) ++empty;
        sumLuma += luma; sumLuma2 += static_cast<double>(luma) * luma;
        if (!isEmpty && dist > 0.0f) { sumDist += dist; ++distCount; }
    }
    RegionStats Finish() const
    {
        RegionStats r;
        if (total == 0) return r;
        r.empty = static_cast<float>(empty) / total;
        const double m = sumLuma / total;
        r.luma    = static_cast<float>(m);
        r.lumaStd = static_cast<float>(std::sqrt((std::max)(0.0, sumLuma2 / total - m * m)));
        if (distCount > 0) r.distance = static_cast<float>(sumDist / distCount);
        return r;
    }
};

// 1 パス目の積算から Stats の「画素だけで決まる部分」を埋める。
void FillBasic(Stats& s, const Accum& a, u32 w, u32 h)
{
    const double total = static_cast<double>(w) * h;
    s.pixels = a.pixels;
    s.share  = total > 0 ? static_cast<float>(a.pixels / total) : 0.0f;
    if (a.pixels == 0) return;
    s.bbox   = { static_cast<float>(a.minX) / w, static_cast<float>(a.minY) / h,
                 static_cast<float>(a.maxX + 1) / w, static_cast<float>(a.maxY + 1) / h };
    s.center = { static_cast<float>((a.sumX / a.pixels + 0.5) / w),
                 static_cast<float>((a.sumY / a.pixels + 0.5) / h) };
    const double m = a.sumLuma / a.pixels;
    s.luma       = static_cast<float>(m);
    s.lumaStd    = static_cast<float>(std::sqrt((std::max)(0.0, a.sumLuma2 / a.pixels - m * m)));
    s.saturation = static_cast<float>(a.sumSat / a.pixels);
    if (a.distCount > 0)
    {
        s.distance    = static_cast<float>(a.sumDist / a.distCount);
        s.distanceMin = a.minDist;
    }
}

// AABB と光源の影響範囲（球）が交わるか。平行光は常に true。
bool LightReaches(const Light& L, bool hasAabb, const Vec3& mn, const Vec3& mx)
{
    if (L.radiance <= 0.0f) return false;
    if (L.type == Light::Directional || !hasAabb) return true;
    const float cx = std::clamp(L.position.x, mn.x, mx.x);
    const float cy = std::clamp(L.position.y, mn.y, mx.y);
    const float cz = std::clamp(L.position.z, mn.z, mx.z);
    const float dx = cx - L.position.x, dy = cy - L.position.y, dz = cz - L.position.z;
    return dx * dx + dy * dy + dz * dz < L.range * L.range;
}

// 2 パス目: 周囲リング・直接光。member(p) がその対象の画素かを返す。
template <class Member>
void FillSecondPass(Stats& s, const Accum& a, const PixelFrame& f, const std::vector<float>& luma,
                    const std::vector<Light>& lights, bool hasAabb, const Vec3& mn, const Vec3& mx,
                    const Vec3& camPos, const Options& opt, Member member)
{
    if (a.pixels == 0) return;
    const u32 w = f.width, h = f.height;

    // ---- 周囲リング ----
    {
        const u32 bw = a.maxX - a.minX + 1, bh = a.maxY - a.minY + 1;
        const u32 m  = (std::max)(2u, static_cast<u32>(std::lround(opt.ringMarginFrac * (std::max)(bw, bh))));
        const u32 rx0 = a.minX > m ? a.minX - m : 0u;
        const u32 ry0 = a.minY > m ? a.minY - m : 0u;
        const u32 rx1 = (std::min)(w - 1, a.maxX + m);
        const u32 ry1 = (std::min)(h - 1, a.maxY + m);
        double sum = 0;
        u32 n = 0;
        for (u32 y = ry0; y <= ry1; ++y)
            for (u32 x = rx0; x <= rx1; ++x)
            {
                // 元の矩形の内側は数えない。★自分の画素は必ず外接矩形の内側にあるので、
                //   ここで弾けば「自分以外の画素」だけが残る（隣の物・背景・空）。
                if (x >= a.minX && x <= a.maxX && y >= a.minY && y <= a.maxY) continue;
                const size_t p = static_cast<size_t>(y) * w + x;
                sum += luma[p];
                ++n;
            }
        s.ringPixels = n;
        if (n >= kMinRingPixels)
        {
            s.lumaRing = static_cast<float>(sum / n);
            s.contrast = (s.luma + kContrastEps) / (*s.lumaRing + kContrastEps);
        }
    }

    // ---- 直接光（影なし）----
    if (!f.posDist || !f.normal) return;
    std::vector<u32> active;
    for (u32 i = 0; i < static_cast<u32>(lights.size()); ++i)
        if (LightReaches(lights[i], hasAabb, mn, mx)) active.push_back(i);

    std::vector<double> perAbs(active.size(), 0.0), perFront(active.size(), 0.0);
    double front = 0, back = 0;
    u32 used = 0, backFacing = 0;
    for (u32 y = a.minY; y <= a.maxY; ++y)
        for (u32 x = a.minX; x <= a.maxX; ++x)
        {
            const size_t p = static_cast<size_t>(y) * w + x;
            if (!member(p)) continue;
            const float* pd = f.posDist + p * 4;
            const float* nn = f.normal + p * 4;
            Vec3 N{ nn[0], nn[1], nn[2] };
            const float len = std::sqrt(Dot(N, N));
            if (len < 0.5f) continue;   // 法線が取れていない画素
            N = { N.x / len, N.y / len, N.z / len };
            const Vec3 P{ pd[0], pd[1], pd[2] };
            ++used;
            const Vec3 toCam{ camPos.x - P.x, camPos.y - P.y, camPos.z - P.z };
            if (Dot(N, toCam) < 0.0f) ++backFacing;
            for (size_t k = 0; k < active.size(); ++k)
            {
                float fr = 0.0f, bk = 0.0f;
                const float c = AccumulateLight(lights[active[k]], P, N, fr, bk);
                perAbs[k]   += c;
                perFront[k] += fr;
                front += fr;
                back  += bk;
            }
        }
    if (used == 0) return;
    s.backFacing = static_cast<float>(backFacing) / static_cast<float>(used);
    const double total = front + back;
    if (total <= 1e-9)
    {
        s.unlit = true;
        return;
    }
    s.litFacing = static_cast<float>(front / total);
    size_t best = 0;
    for (size_t k = 1; k < active.size(); ++k)
        if (perAbs[k] > perAbs[best]) best = k;
    if (!active.empty() && perAbs[best] > 1e-12)
    {
        s.mainLight       = lights[active[best]].name;
        s.mainLightFacing = static_cast<float>(perFront[best] / perAbs[best]);
    }
}
} // namespace

float LumaOf(u8 r, u8 g, u8 b)
{
    return (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
}

float SaturationOf(u8 r, u8 g, u8 b)
{
    const u8 mx = (std::max)({r, g, b});
    const u8 mn = (std::min)({r, g, b});
    return mx == 0 ? 0.0f : static_cast<float>(mx - mn) / mx;
}

void ProjectAabb(const float M[16], const Vec3& mn, const Vec3& mx,
                 bool& fullyInView, std::optional<std::array<float, 2>>& extent)
{
    fullyInView = true;
    extent.reset();
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int c = 0; c < 8; ++c)
    {
        const float v[4] = { (c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y, (c & 4) ? mx.z : mn.z, 1.0f };
        float clip[4];
        for (int j = 0; j < 4; ++j)
            clip[j] = v[0] * M[0 * 4 + j] + v[1] * M[1 * 4 + j] + v[2] * M[2 * 4 + j] + v[3] * M[3 * 4 + j];
        if (clip[3] <= 1e-6f) { fullyInView = false; return; }   // カメラの後ろ＝投影できない
        const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
        if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f) fullyInView = false;
        x0 = (std::min)(x0, nx); x1 = (std::max)(x1, nx);
        y0 = (std::min)(y0, ny); y1 = (std::max)(y1, ny);
    }
    extent = std::array<float, 2>{ (x1 - x0) * 0.5f, (y1 - y0) * 0.5f };
}

float AccumulateLight(const Light& L, const Vec3& P, const Vec3& N, float& front, float& back)
{
    if (L.radiance <= 0.0f) return 0.0f;
    Vec3  Ldir;
    float r = L.radiance;
    if (L.type == Light::Directional)
    {
        Ldir = { -L.direction.x, -L.direction.y, -L.direction.z };
    }
    else
    {
        const Vec3 d{ L.position.x - P.x, L.position.y - P.y, L.position.z - P.z };
        const float dist = std::sqrt(Dot(d, d));
        if (dist >= L.range) return 0.0f;
        const float inv = 1.0f / (std::max)(dist, 1e-4f);
        Ldir = { d.x * inv, d.y * inv, d.z * inv };
        // Lighting.hlsli と同じ減衰: saturate(1 - d/range)^2
        float att = Sat01(1.0f - dist / L.range);
        r *= att * att;
        if (L.type == Light::Spot)
        {
            const float cd   = -Dot(L.direction, Ldir);
            float cone = Sat01((cd - L.cosOuter) / (std::max)(L.cosInner - L.cosOuter, 0.001f));
            cone *= cone;
            if (cone <= 0.0f) return 0.0f;
            r *= cone;
        }
    }
    const float c = Dot(N, Ldir);
    if (c > 0.0f) front += r * c;
    else          back  += r * -c;
    return r * std::fabs(c);
}

Result Analyze(const PixelFrame& f, const std::vector<EntityMeta>& meta,
               const std::vector<Group>& groups, const std::vector<Light>& lights,
               const CameraInfo& cam, const Options& opt)
{
    Result res;
    const u32 w = f.width, h = f.height;
    res.scene.width = w;
    res.scene.height = h;
    const size_t n = static_cast<size_t>(w) * h;
    if (n == 0 || !f.ids || !f.rgba) return res;

    // ID の上限。meta に無い ID（本来起きない）も数えられるよう、実際の最大まで広げる。
    u32 maxId = meta.empty() ? 0u : static_cast<u32>(meta.size() - 1);
    for (size_t p = 0; p < n; ++p) maxId = (std::max)(maxId, f.ids[p]);

    // グループの所属ビット（最大 32 グループ）
    const u32 groupCount = (std::min)(static_cast<u32>(groups.size()), 32u);
    std::vector<u32> groupBits(static_cast<size_t>(maxId) + 1, 0u);
    for (u32 g = 0; g < groupCount; ++g)
        for (u32 id : groups[g].ids)
            if (id != 0 && id <= maxId) groupBits[id] |= (1u << g);

    std::vector<Accum> acc(static_cast<size_t>(maxId) + 1);
    std::vector<Accum> gacc(groupCount);
    std::vector<float> luma(n);
    RegionAccum rTop, rBottom, rLeft, rRight;
    std::array<u32, 256> hist{};
    double lumaSum = 0;
    u32 empty = 0, crushed = 0, clipped = 0;
    float farthest = 0.0f;
    bool anyDist = false;

    for (u32 y = 0; y < h; ++y)
    {
        const bool isTop = (y < h / 2);
        for (u32 x = 0; x < w; ++x)
        {
            const size_t p = static_cast<size_t>(y) * w + x;
            const u8* c = f.rgba + p * 4;
            const float L = LumaOf(c[0], c[1], c[2]);
            luma[p] = L;
            lumaSum += L;
            ++hist[static_cast<size_t>(std::clamp(static_cast<int>(std::lround(L * 255.0f)), 0, 255))];
            if (L <= kCrushedLuma) ++crushed;
            if (L >= kClippedLuma) ++clipped;

            const u32 id = f.ids[p];
            const float dist = (f.posDist && id != 0) ? f.posDist[p * 4 + 3] : 0.0f;
            const bool isEmpty = (id == 0);
            if (isEmpty) ++empty;
            if (dist > 0.0f) { farthest = anyDist ? (std::max)(farthest, dist) : dist; anyDist = true; }

            (isTop ? rTop : rBottom).Add(isEmpty, L, dist);
            (x < w / 2 ? rLeft : rRight).Add(isEmpty, L, dist);

            if (isEmpty) continue;
            const float S = SaturationOf(c[0], c[1], c[2]);
            acc[id].Add(x, y, L, S, dist);
            if (u32 bits = groupBits[id])
                for (u32 g = 0; g < groupCount; ++g)
                    if (bits & (1u << g)) gacc[g].Add(x, y, L, S, dist);
        }
    }

    // ---- シーン全体 ----
    {
        auto& s = res.scene;
        s.empty  = static_cast<float>(empty) / n;
        s.top    = rTop.Finish();    s.bottom = rBottom.Finish();
        s.left   = rLeft.Finish();   s.right  = rRight.Finish();
        s.lumaMean = static_cast<float>(lumaSum / n);
        s.crushed  = static_cast<float>(crushed) / n;
        s.clipped  = static_cast<float>(clipped) / n;
        auto pct = [&](double q) {
            const double want = q * n;
            u64 cum = 0;
            for (u32 b = 0; b < 256; ++b) { cum += hist[b]; if (cum >= want) return b / 255.0f; }
            return 1.0f;
        };
        s.lumaP5  = pct(0.05);
        s.lumaP50 = pct(0.50);
        s.lumaP95 = pct(0.95);
        if (anyDist) s.farthest = farthest;
        for (u32 id = 1; id <= maxId; ++id) if (acc[id].pixels) ++s.visibleEntities;
    }

    auto metaOf = [&](u32 id) -> const EntityMeta* { return id < meta.size() ? &meta[id] : nullptr; };

    // ---- 上位 N 件（単体）----
    {
        std::vector<u32> order;
        for (u32 id = 1; id <= maxId; ++id) if (acc[id].pixels) order.push_back(id);
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
            if (acc[a].pixels != acc[b].pixels) return acc[a].pixels > acc[b].pixels;
            return a < b;   // 同数なら ID 順（決定論）
        });
        if (order.size() > opt.top) order.resize(opt.top);
        for (u32 id : order)
        {
            Stats s;
            s.id = id;
            const EntityMeta* m = metaOf(id);
            s.name   = m ? m->name : ("#" + std::to_string(id));
            s.parent = m ? m->parent : std::string();
            s.transparentMembers = (m && m->transparent) ? 1u : 0u;
            FillBasic(s, acc[id], w, h);
            const bool hasAabb = m && m->hasAabb;
            const Vec3 mn = hasAabb ? m->aabbMin : Vec3{}, mx = hasAabb ? m->aabbMax : Vec3{};
            if (hasAabb)
            {
                bool fiv = false;
                ProjectAabb(cam.viewProj, mn, mx, fiv, s.projectedExtent);
                s.fullyInView = fiv;
            }
            FillSecondPass(s, acc[id], f, luma, lights, hasAabb, mn, mx, cam.position, opt,
                           [&](size_t p) { return f.ids[p] == id; });
            res.top.push_back(std::move(s));
        }
    }

    // ---- 名前で指定された対象（グループ）----
    for (u32 g = 0; g < static_cast<u32>(groups.size()); ++g)
    {
        const Group& G = groups[g];
        Stats s;
        s.name    = G.name;
        s.members = static_cast<u32>(G.ids.size());
        for (u32 id : G.ids)
            if (const EntityMeta* m = metaOf(id); m && m->transparent) ++s.transparentMembers;
        if (G.ids.size() == 1)
        {
            s.id = G.ids[0];
            if (const EntityMeta* m = metaOf(s.id)) s.parent = m->parent;
        }
        if (g >= groupCount) { res.targets.push_back(std::move(s)); continue; }   // 33 件目以降は数えない
        FillBasic(s, gacc[g], w, h);
        if (G.hasAabb)
        {
            bool fiv = false;
            ProjectAabb(cam.viewProj, G.aabbMin, G.aabbMax, fiv, s.projectedExtent);
            s.fullyInView = fiv;
        }
        if (G.isolatedMask)
        {
            u32 iso = 0;
            for (size_t p = 0; p < n; ++p) if (G.isolatedMask[p]) ++iso;
            s.isolatedPixels = iso;
            if (iso > 0)
                s.occlusion = Sat01(1.0f - static_cast<float>(s.pixels) / static_cast<float>(iso));
        }
        const u32 bit = 1u << g;
        FillSecondPass(s, gacc[g], f, luma, lights, G.hasAabb, G.aabbMin, G.aabbMax, cam.position, opt,
                       [&](size_t p) { const u32 id = f.ids[p]; return id != 0 && (groupBits[id] & bit) != 0; });
        res.targets.push_back(std::move(s));
    }
    return res;
}

} // namespace dx12e::perception
