#pragma once
// ===========================================================================
// パストレーサーの独立した CPU リファレンス(倍精度・BVH 付き・マルチスレッド)。
// ---------------------------------------------------------------------------
// GPU 版(shaders/pt/PathTrace.hlsl)と【別に】書いた実装。仕様(docs/PATH_TRACER.md)だけを共有し、
// サンプリング戦略は意図的に変えてある(= 推定量の食い違いを検出できる):
//   ・点光源: GPU は期待寄与に比例した 1 灯選択(リザーバ)/ CPU は全灯を明示評価
//   ・エミッシブ三角形: GPU は Walker alias / CPU は累積分布 + 二分探索
//   ・環境: GPU はコサイン重み方向 / CPU は球面一様
//   ・乱数: GPU は PCG ハッシュ / CPU は mt19937_64
// 共有するもの: BRDF の式(フォワードの PBR そのもの = 仕様)、MIS(パワーヒューリスティック)、
//   bounces の意味(1 = 直接光のみ)、アルファ / 発光 / 両面の規則。
// ★テスト専用(Renderer にリンクしない)。小さなシーン用。
// ===========================================================================
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

namespace dx12e::ptref
{

constexpr double kPi = 3.14159265358979323846;

struct V3
{
    double x = 0, y = 0, z = 0;
    V3() = default;
    V3(double a, double b, double c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    V3 operator*(const V3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    V3 operator/(double s) const { return {x / s, y / s, z / s}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(const V3& a) { const double l = Len(a); return l > 0 ? a / l : V3(0, 1, 0); }
inline double Luma(const V3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
inline V3 Lerp(const V3& a, const V3& b, double t) { return a * (1 - t) + b * t; }
inline V3 Reflect(const V3& i, const V3& n) { return i - n * (2.0 * Dot(i, n)); }

struct Tri
{
    V3 p[3];
    V3 n[3];
    double alpha[3] = {1, 1, 1};   // 頂点色のアルファ
    int material = 0;
};

enum AlphaMode { kOpaque = 0, kMask = 1, kBlend = 2 };

struct Material
{
    V3 albedo{0.8, 0.8, 0.8};
    double metallic = 0.0;
    double roughness = 0.5;
    V3 emission{0, 0, 0};
    bool lambert = false;
    int alphaMode = kOpaque;
    double cutoff = 0.5;
    double opacity = 1.0;
    bool emitBoth = false;
};

struct PointLight
{
    V3 pos;
    double range = 10.0;
    V3 color{1, 1, 1};          // intensity 乗算済み
    bool spot = false;
    V3 dir{0, -1, 0};
    double cosInner = 1.0, cosOuter = 0.0;
};

struct Sun
{
    bool enabled = false;
    V3 toLight{0, 1, 0};
    double tanRadius = 0.0;
    V3 E{0, 0, 0};
};

struct Env
{
    V3 uniform{0, 0, 0};
    double lightScale = 1.0;
    double bgScale = 0.0;
};

struct Camera
{
    V3 pos{0, 0, 0}, right{1, 0, 0}, up{0, 1, 0}, fwd{0, 0, 1};
    double tanHalfFovY = 0.41421356237;
    double aspect = 1.0;
};

struct Scene
{
    std::vector<Tri> tris;
    std::vector<Material> mats;
    std::vector<PointLight> lights;
    Sun sun;
    Env env;
    Camera cam;
    bool physFalloff = false;
    bool forceLambert = false;
    double rrStart = 3;
};

struct RenderParams
{
    int w = 64, h = 64, spp = 64, bounces = 8;
    uint64_t seed = 1;
    bool rr = true;
    int threads = 4;
};

// ---------------------------------------------------------------------------
//  BRDF(フォワードと同じ式。仕様)
// ---------------------------------------------------------------------------
struct Surf { V3 albedo; double metallic, roughness; V3 F0; bool lambert; };

inline double GgxD(double NH, double r) { const double a = r * r, a2 = a * a; const double d = NH * NH * (a2 - 1) + 1; return a2 / std::max(kPi * d * d, 1e-7); }
inline double G1(double NX, double r) { const double rr = r + 1, k = rr * rr / 8; return NX / (NX * (1 - k) + k); }
inline V3 Fresnel(double c, const V3& F0) { const double f = std::pow(std::clamp(1 - c, 0.0, 1.0), 5.0); return F0 + (V3(1, 1, 1) - F0) * f; }

inline V3 EvalBrdf(const V3& N, const V3& V, const V3& L, const Surf& s)
{
    if (s.lambert) return s.albedo / kPi;
    const V3 H = Norm(V + L);
    const double NV = std::max(Dot(N, V), 0.001), NL = std::max(Dot(N, L), 0.0), NH = std::max(Dot(N, H), 0.0);
    const double D = GgxD(NH, s.roughness);
    const double G = G1(std::max(Dot(N, V), 0.0), s.roughness) * G1(NL, s.roughness);
    const V3 F = Fresnel(std::max(Dot(H, V), 0.0), s.F0);
    const V3 kD = (V3(1, 1, 1) - F) * (1.0 - s.metallic);
    const V3 spec = F * (D * G / (4.0 * NV * NL + 0.0001));
    return kD * s.albedo / kPi + spec;
}

inline Surf MakeSurf(const Material& m, bool forceLambert)
{
    Surf s;
    s.albedo = m.albedo;
    s.metallic = m.metallic;
    s.roughness = std::max(m.roughness, 0.04);
    s.F0 = Lerp(V3(0.04, 0.04, 0.04), m.albedo, m.metallic);
    s.lambert = m.lambert || forceLambert;
    return s;
}

// 方向 L の pdf と混合確率
inline double SpecProb(const V3& N, const V3& V, const Surf& s)
{
    if (s.lambert) return 0.0;
    const double NV = std::clamp(Dot(N, V), 0.0, 1.0);
    const V3 Fv = Fresnel(NV, s.F0);
    const double ws = Luma(Fv);
    const double wd = (1 - s.metallic) * Luma(s.albedo) * (1 - ws);
    if (wd < 1e-4 || s.metallic > 0.999) return 1.0;
    return std::clamp(ws / (ws + wd), 0.05, 0.95);
}
inline double BsdfPdf(const V3& N, const V3& V, const V3& L, const Surf& s, double pSpec)
{
    const double NL = Dot(N, L);
    if (NL <= 0) return 0;
    double pdf = (1 - pSpec) * NL / kPi;
    if (pSpec > 0)
    {
        const V3 H = Norm(V + L);
        const double NH = std::max(Dot(N, H), 0.0), VH = std::max(Dot(V, H), 1e-4);
        pdf += pSpec * GgxD(NH, s.roughness) * NH / (4 * VH);
    }
    return pdf;
}
inline double Mis(double a, double b) { const double x = a * a, y = b * b; return (x + y > 0) ? x / (x + y) : 0; }

inline void Basis(const V3& n, V3& t, V3& b)
{
    const V3 up = std::fabs(n.y) < 0.999 ? V3(0, 1, 0) : V3(1, 0, 0);
    t = Norm(Cross(up, n));
    b = Cross(n, t);
}
inline V3 ToWorld(const V3& l, const V3& n) { V3 t, b; Basis(n, t, b); return t * l.x + b * l.y + n * l.z; }

// ---------------------------------------------------------------------------
//  BRDF の方向アルベド(数値積分)= ∫ f(V,L) cos dω。テストの参照値。
// ---------------------------------------------------------------------------
inline V3 DirectionalAlbedo(const Material& m, double NdotV, int n = 1024)
{
    const Surf s = MakeSurf(m, false);
    const V3 N(0, 0, 1);
    const V3 V(std::sqrt(std::max(0.0, 1 - NdotV * NdotV)), 0, NdotV);
    V3 sum;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 2 * n; ++j)
        {
            const double th = (i + 0.5) / n * (kPi / 2), ph = (j + 0.5) / (2 * n) * 2 * kPi;
            const V3 L(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            sum += EvalBrdf(N, V, L, s) * (std::cos(th) * std::sin(th));
        }
    return sum * ((kPi / 2 / n) * (2 * kPi / (2 * n)));
}

// ---------------------------------------------------------------------------
//  BVH(中央値分割)
// ---------------------------------------------------------------------------
struct Bvh
{
    struct Node { V3 lo, hi; int left = -1, right = -1, first = 0, count = 0; };
    std::vector<Node> nodes;
    std::vector<int> order;
    const std::vector<Tri>* tris = nullptr;

    static V3 Centroid(const Tri& t) { return (t.p[0] + t.p[1] + t.p[2]) / 3.0; }
    void Build(const std::vector<Tri>& t)
    {
        tris = &t;
        nodes.clear();
        order.resize(t.size());
        for (size_t i = 0; i < t.size(); ++i) order[i] = static_cast<int>(i);
        if (t.empty()) return;
        nodes.reserve(t.size() * 2);
        BuildRec(0, static_cast<int>(t.size()));
    }
    int BuildRec(int first, int count)
    {
        const int idx = static_cast<int>(nodes.size());
        nodes.emplace_back();
        V3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30), clo = lo, chi = hi;
        for (int i = first; i < first + count; ++i)
        {
            const Tri& t = (*tris)[order[i]];
            for (int k = 0; k < 3; ++k)
            {
                lo = {std::min(lo.x, t.p[k].x), std::min(lo.y, t.p[k].y), std::min(lo.z, t.p[k].z)};
                hi = {std::max(hi.x, t.p[k].x), std::max(hi.y, t.p[k].y), std::max(hi.z, t.p[k].z)};
            }
            const V3 c = Centroid(t);
            clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
            chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
        }
        nodes[idx].lo = lo; nodes[idx].hi = hi;
        if (count <= 4) { nodes[idx].first = first; nodes[idx].count = count; return idx; }
        const V3 ext = chi - clo;
        const int axis = (ext.x > ext.y && ext.x > ext.z) ? 0 : (ext.y > ext.z ? 1 : 2);
        const int mid = first + count / 2;
        std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
            [&](int a, int b) { return Centroid((*tris)[a])[axis] < Centroid((*tris)[b])[axis]; });
        const int l = BuildRec(first, mid - first);
        const int r = BuildRec(mid, first + count - mid);
        nodes[idx].left = l; nodes[idx].right = r;
        return idx;
    }
};

struct Rng
{
    std::mt19937_64 g;
    explicit Rng(uint64_t s) : g(s) {}
    double Next() { return std::uniform_real_distribution<double>(0.0, 1.0)(g); }
};

struct Hit { int tri = -1; double t = 0, b1 = 0, b2 = 0; };

inline bool RayTri(const Tri& tr, const V3& o, const V3& d, double tmax, double& t, double& b1, double& b2)
{
    const V3 e1 = tr.p[1] - tr.p[0], e2 = tr.p[2] - tr.p[0];
    const V3 p = Cross(d, e2);
    const double det = Dot(e1, p);
    if (std::fabs(det) < 1e-14) return false;
    const double inv = 1.0 / det;
    const V3 s = o - tr.p[0];
    b1 = Dot(s, p) * inv;
    if (b1 < 0 || b1 > 1) return false;
    const V3 q = Cross(s, e1);
    b2 = Dot(d, q) * inv;
    if (b2 < 0 || b1 + b2 > 1) return false;
    t = Dot(e2, q) * inv;
    return t > 0 && t < tmax;
}

class Tracer
{
public:
    explicit Tracer(const Scene& s) : m_s(s)
    {
        m_bvh.Build(s.tris);
        // エミッシブ三角形の累積分布
        double total = 0;
        for (size_t i = 0; i < s.tris.size(); ++i)
        {
            const Material& m = s.mats[s.tris[i].material];
            if (Luma(m.emission) <= 0) continue;
            const Tri& t = s.tris[i];
            const double area = 0.5 * Len(Cross(t.p[1] - t.p[0], t.p[2] - t.p[0]));
            total += area * Luma(m.emission);
            m_emTri.push_back(static_cast<int>(i));
            m_emCdf.push_back(total);
            m_emArea.push_back(area);
        }
        m_emTotal = total;
        m_emIndex.assign(s.tris.size(), -1);
        for (size_t k = 0; k < m_emTri.size(); ++k) m_emIndex[m_emTri[k]] = static_cast<int>(k);
    }

    // 画素の平均(rgb)
    std::vector<float> Render(const RenderParams& rp) const
    {
        std::vector<float> out(static_cast<size_t>(rp.w) * rp.h * 3, 0.0f);
        std::atomic<int> nextRow{0};
        auto work = [&]() {
            for (;;)
            {
                const int y = nextRow.fetch_add(1);
                if (y >= rp.h) break;
                for (int x = 0; x < rp.w; ++x)
                {
                    Rng rng(rp.seed * 1000003ull + static_cast<uint64_t>(y) * 4099ull + static_cast<uint64_t>(x) + 7ull);
                    V3 sum;
                    for (int s = 0; s < rp.spp; ++s)
                    {
                        const double u = (x + rng.Next()) / rp.w, v = (y + rng.Next()) / rp.h;
                        const double nx = (2 * u - 1) * m_s.cam.aspect * m_s.cam.tanHalfFovY;
                        const double ny = (1 - 2 * v) * m_s.cam.tanHalfFovY;
                        const V3 d = Norm(m_s.cam.fwd + m_s.cam.right * nx + m_s.cam.up * ny);
                        sum += Path(m_s.cam.pos, d, rp, rng);
                    }
                    sum = sum / rp.spp;
                    float* o = &out[(static_cast<size_t>(y) * rp.w + x) * 3];
                    o[0] = static_cast<float>(sum.x); o[1] = static_cast<float>(sum.y); o[2] = static_cast<float>(sum.z);
                }
            }
        };
        std::vector<std::thread> th;
        for (int i = 0; i < std::max(1, rp.threads); ++i) th.emplace_back(work);
        for (auto& t : th) t.join();
        return out;
    }

private:
    struct Geo { V3 pos, Ng, Ns; double area; int mat; double alpha; };

    Geo Fetch(int tri, double b1, double b2) const
    {
        const Tri& t = m_s.tris[tri];
        const double b0 = 1 - b1 - b2;
        Geo g;
        g.pos = t.p[0] * b0 + t.p[1] * b1 + t.p[2] * b2;
        V3 ng = Cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
        g.area = 0.5 * Len(ng);
        ng = Norm(ng);
        const V3 ns = Norm(t.n[0] * b0 + t.n[1] * b1 + t.n[2] * b2);
        g.Ns = ns;
        g.Ng = (Dot(ng, ns) < 0) ? -ng : ng;
        g.mat = t.material;
        g.alpha = t.alpha[0] * b0 + t.alpha[1] * b1 + t.alpha[2] * b2;
        return g;
    }

    bool AlphaPass(int tri, double b1, double b2, Rng& rng) const
    {
        const Tri& t = m_s.tris[tri];
        const Material& m = m_s.mats[t.material];
        if (m.alphaMode == kOpaque) return true;
        const double b0 = 1 - b1 - b2;
        const double a = t.alpha[0] * b0 + t.alpha[1] * b1 + t.alpha[2] * b2;
        if (m.alphaMode == kMask) return a >= m.cutoff;
        return rng.Next() < std::clamp(a * m.opacity, 0.0, 1.0);
    }

    // 最も近い(アルファを通った)ヒット。
    bool Closest(const V3& o, const V3& d, double tmax, Rng& rng, Hit& hit) const
    {
        hit = Hit{};
        if (m_bvh.nodes.empty()) return false;
        double best = tmax;
        int stack[128], sp = 0;
        stack[sp++] = 0;
        const V3 inv(1.0 / (d.x != 0 ? d.x : 1e-30), 1.0 / (d.y != 0 ? d.y : 1e-30), 1.0 / (d.z != 0 ? d.z : 1e-30));
        while (sp)
        {
            const Bvh::Node& n = m_bvh.nodes[stack[--sp]];
            double t0 = 0, t1 = best;
            for (int a = 0; a < 3; ++a)
            {
                double ta = (n.lo[a] - o[a]) * inv[a], tb = (n.hi[a] - o[a]) * inv[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta); t1 = std::min(t1, tb);
            }
            if (t0 > t1) continue;
            if (n.count > 0)
            {
                for (int i = n.first; i < n.first + n.count; ++i)
                {
                    const int ti = m_bvh.order[i];
                    double t, b1, b2;
                    if (RayTri(m_s.tris[ti], o, d, best, t, b1, b2) && AlphaPass(ti, b1, b2, rng))
                    { best = t; hit.tri = ti; hit.t = t; hit.b1 = b1; hit.b2 = b2; }
                }
            }
            else { stack[sp++] = n.left; stack[sp++] = n.right; }
        }
        return hit.tri >= 0;
    }

    bool Occluded(const V3& o, const V3& d, double tmax, Rng& rng) const
    {
        Hit h;
        return Closest(o, d, tmax, rng, h);
    }

    static V3 Offset(const V3& p, const V3& n) { return p + n * 1e-4; }

    V3 EnvRad(double scale) const { return m_s.env.uniform * scale; }
    bool EnvActive() const { return Luma(m_s.env.uniform) * m_s.env.lightScale > 0; }

    double PunctAtt(const PointLight& L, double dist, const V3& Ld) const
    {
        double att;
        if (m_s.physFalloff) att = 1.0 / std::max(dist * dist, 1e-4);
        else { if (dist >= L.range) return 0; att = std::clamp(1 - dist / L.range, 0.0, 1.0); att *= att; }
        if (L.spot)
        {
            const double cd = Dot(L.dir, -Ld);
            double cone = std::clamp((cd - L.cosOuter) / std::max(L.cosInner - L.cosOuter, 0.001), 0.0, 1.0);
            att *= cone * cone;
        }
        return att;
    }

    V3 Path(V3 o, V3 d, const RenderParams& rp, Rng& rng) const
    {
        V3 thr(1, 1, 1), rad;
        double prevPdf = 0;
        V3 prevPos = o, prevN(0, 1, 0);
        for (int depth = 0; depth <= rp.bounces; ++depth)
        {
            Hit h;
            if (!Closest(o, d, 1e30, rng, h))
            {
                if (depth == 0) rad += thr * EnvRad(m_s.env.bgScale);
                else
                {
                    double w = 1.0;
                    if (EnvActive()) w = Mis(prevPdf, 1.0 / (4 * kPi));   // 球面一様の pdf
                    rad += thr * EnvRad(m_s.env.lightScale) * w;
                }
                break;
            }
            Geo g = Fetch(h.tri, h.b1, h.b2);
            const Material& M = m_s.mats[g.mat];
            const bool front = Dot(g.Ng, -d) > 0;
            const V3 Ng = front ? g.Ng : -g.Ng;
            const V3 N = front ? g.Ns : -g.Ns;
            const Surf surf = MakeSurf(M, m_s.forceLambert);

            if (Luma(M.emission) > 0 && (front || M.emitBoth))
            {
                double w = 1.0;
                const int k = m_emIndex[h.tri];
                if (depth > 0 && k >= 0)
                {
                    const double pmf = area_weight(k);
                    const V3 dv = g.pos - prevPos;
                    const double d2 = Dot(dv, dv), cosL = std::fabs(Dot(g.Ng, d));
                    w = Mis(prevPdf, pmf * d2 / std::max(m_emArea[k] * cosL, 1e-12));
                }
                rad += thr * M.emission * w;
            }
            if (depth == rp.bounces) break;

            const V3 V = -d;
            const double pSpec = SpecProb(N, V, surf);
            rad += thr * Direct(g.pos, Ng, N, V, surf, pSpec, rng);

            // BSDF サンプリング
            V3 L;
            const double u1 = rng.Next(), u2 = rng.Next(), u3 = rng.Next();
            if (u3 < pSpec)
            {
                const double a = surf.roughness * surf.roughness, a2 = a * a;
                const double cosH = std::sqrt((1 - u1) / (1 + (a2 - 1) * u1)), sinH = std::sqrt(std::max(0.0, 1 - cosH * cosH));
                const double ph = 2 * kPi * u2;
                const V3 H = ToWorld(V3(sinH * std::cos(ph), sinH * std::sin(ph), cosH), N);
                L = Reflect(-V, H);
            }
            else
            {
                const double r = std::sqrt(u1), ph = 2 * kPi * u2;
                L = ToWorld(V3(r * std::cos(ph), r * std::sin(ph), std::sqrt(std::max(0.0, 1 - u1))), N);
            }
            const double NL = Dot(N, L);
            if (NL <= 0) break;
            const double pdf = BsdfPdf(N, V, L, surf, pSpec);
            if (pdf <= 1e-12) break;
            if (Dot(Ng, L) <= 0) break;
            thr = thr * (EvalBrdf(N, V, L, surf) * (NL / pdf));
            prevPdf = pdf; prevPos = g.pos; prevN = N;
            o = Offset(g.pos, Ng);
            d = L;
            if (rp.rr && depth + 1 >= m_s.rrStart)
            {
                const double q = std::clamp(std::max(thr.x, std::max(thr.y, thr.z)), 0.05, 0.95);
                if (rng.Next() > q) break;
                thr = thr / q;
            }
        }
        return rad;
    }

    double area_weight(int k) const
    {
        const double prev = (k > 0) ? m_emCdf[k - 1] : 0.0;
        return (m_emCdf[k] - prev) / m_emTotal;
    }

    V3 Direct(const V3& pos, const V3& Ng, const V3& N, const V3& V, const Surf& s, double pSpec, Rng& rng) const
    {
        V3 sum;
        auto origin = [&](const V3& L) { return Offset(pos, Dot(Ng, L) >= 0 ? Ng : -Ng); };

        // 太陽
        if (m_s.sun.enabled)
        {
            V3 L = m_s.sun.toLight;
            if (m_s.sun.tanRadius > 0)
            {
                const double r = std::sqrt(rng.Next()) * m_s.sun.tanRadius, ph = 2 * kPi * rng.Next();
                L = Norm(ToWorld(V3(r * std::cos(ph), r * std::sin(ph), 1.0), m_s.sun.toLight));
            }
            const double NL = Dot(N, L);
            if (NL > 0 && Dot(Ng, L) > 0 && !Occluded(origin(L), L, 1e30, rng))
                sum += EvalBrdf(N, V, L, s) * m_s.sun.E * NL;
        }
        // 点 / スポット(全灯を明示評価)
        for (const PointLight& Lt : m_s.lights)
        {
            const V3 dv = Lt.pos - pos;
            const double dist = Len(dv);
            const V3 Ld = dv / std::max(dist, 1e-4);
            const double NL = Dot(N, Ld);
            if (NL <= 0 || Dot(Ng, Ld) <= 0) continue;
            const double att = PunctAtt(Lt, dist, Ld);
            if (att <= 0) continue;
            if (Occluded(origin(Ld), Ld, dist * 0.999, rng)) continue;
            sum += EvalBrdf(N, V, Ld, s) * Lt.color * (NL * att);
        }
        // エミッシブ三角形(累積分布 + 二分探索)
        if (!m_emTri.empty())
        {
            const double u = rng.Next() * m_emTotal;
            size_t k = std::lower_bound(m_emCdf.begin(), m_emCdf.end(), u) - m_emCdf.begin();
            k = std::min(k, m_emCdf.size() - 1);
            const int ti = m_emTri[k];
            const double su = std::sqrt(rng.Next()), v2 = rng.Next();
            const double b1 = v2 * su, b2 = su * (1 - v2);
            const Geo lg = Fetch(ti, b1, b2);
            const Material& LM = m_s.mats[lg.mat];
            const V3 dv = lg.pos - pos;
            const double d2 = Dot(dv, dv);
            if (d2 > 1e-10)
            {
                const double dist = std::sqrt(d2);
                const V3 L = dv / dist;
                const double cosL = Dot(lg.Ng, -L);
                const double cosLa = LM.emitBoth ? std::fabs(cosL) : cosL;
                const double NL = Dot(N, L);
                if (cosLa > 1e-5 && NL > 0 && Dot(Ng, L) > 0)
                {
                    const double pdfLight = area_weight(static_cast<int>(k)) * d2 / std::max(m_emArea[k] * cosLa, 1e-12);
                    if (!Occluded(origin(L), L, dist * (1 - 1e-3), rng))
                    {
                        const double pdfB = BsdfPdf(N, V, L, s, pSpec);
                        sum += EvalBrdf(N, V, L, s) * LM.emission * (NL * Mis(pdfLight, pdfB) / pdfLight);
                    }
                }
            }
        }
        // 環境(球面一様)
        if (EnvActive())
        {
            const double z = 1 - 2 * rng.Next(), r = std::sqrt(std::max(0.0, 1 - z * z)), ph = 2 * kPi * rng.Next();
            const V3 L(r * std::cos(ph), r * std::sin(ph), z);
            const double NL = Dot(N, L);
            if (NL > 1e-5 && Dot(Ng, L) > 0 && !Occluded(origin(L), L, 1e30, rng))
            {
                const double pdfE = 1.0 / (4 * kPi), pdfB = BsdfPdf(N, V, L, s, pSpec);
                sum += EvalBrdf(N, V, L, s) * EnvRad(m_s.env.lightScale) * (NL * Mis(pdfE, pdfB) / pdfE);
            }
        }
        return sum;
    }

    const Scene& m_s;
    Bvh m_bvh;
    std::vector<int> m_emTri, m_emIndex;
    std::vector<double> m_emCdf, m_emArea;
    double m_emTotal = 0;
};

// ---------------------------------------------------------------------------
//  シーンを組む小道具(テスト用)
// ---------------------------------------------------------------------------
// 球(緯度経度で細分化。法線は滑らか)
inline void AddSphere(Scene& s, const V3& c, double r, int mat, int slices = 96, int stacks = 48, bool inward = false)
{
    auto P = [&](int i, int j) {
        const double th = kPi * i / stacks, ph = 2 * kPi * j / slices;
        return V3(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
    };
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j)
        {
            const V3 a = P(i, j), b = P(i + 1, j), c2 = P(i + 1, j + 1), d = P(i, j + 1);
            auto push = [&](const V3& u, const V3& v, const V3& w) {
                Tri t;
                t.p[0] = c + u * r; t.p[1] = c + v * r; t.p[2] = c + w * r;
                const double sg = inward ? -1.0 : 1.0;
                t.n[0] = u * sg; t.n[1] = v * sg; t.n[2] = w * sg;
                t.material = mat;
                s.tris.push_back(t);
            };
            if (i != 0) push(a, b, d);
            if (i != stacks - 1) push(d, b, c2);
        }
}

// 四角形(頂点は順に v0..v3、法線は n)。三角形 2 枚。
inline void AddQuad(Scene& s, const V3& a, const V3& b, const V3& c, const V3& d, const V3& n, int mat, double alpha = 1.0)
{
    Tri t1, t2;
    t1.p[0] = a; t1.p[1] = b; t1.p[2] = c;
    t2.p[0] = a; t2.p[1] = c; t2.p[2] = d;
    for (int k = 0; k < 3; ++k) { t1.n[k] = n; t2.n[k] = n; t1.alpha[k] = alpha; t2.alpha[k] = alpha; }
    t1.material = t2.material = mat;
    s.tris.push_back(t1);
    s.tris.push_back(t2);
}

} // namespace dx12e::ptref
