#include "sequencer/SeqCurve.h"

#include <algorithm>
#include <cmath>

namespace dx12e::seq
{

// ===========================================================================
// イージング統一表
// ===========================================================================
namespace
{
constexpr EaseInfo kEase[] = {
    { EaseId::Linear,     "linear",     0,  true,  "linear"    },
    { EaseId::InQuad,     "inQuad",     -1, true,  "in"        },
    { EaseId::OutQuad,    "outQuad",    -1, true,  "out"       },
    { EaseId::InOutQuad,  "inOutQuad",  -1, true,  "inOut"     },
    { EaseId::InCubic,    "inCubic",    1,  true,  nullptr     },
    { EaseId::OutCubic,   "outCubic",   2,  true,  nullptr     },
    { EaseId::InOutCubic, "inOutCubic", 3,  false, nullptr     },
    { EaseId::InOutSine,  "inOutSine",  11, true,  nullptr     },
    { EaseId::OutBack,    "outBack",    4,  true,  "outBack"   },
    { EaseId::OutBounce,  "outBounce",  5,  true,  "outBounce" },
    { EaseId::OutElastic, "outElastic", 6,  false, nullptr     },
    { EaseId::OutExpo,    "outExpo",    7,  false, nullptr     },
    { EaseId::InBack,     "inBack",     8,  false, nullptr     },
    { EaseId::InOutBack,  "inOutBack",  9,  false, nullptr     },
    { EaseId::OutQuint,   "outQuint",   10, false, nullptr     },
};
static_assert(sizeof(kEase) / sizeof(kEase[0]) == static_cast<std::size_t>(EaseId::Count),
              "EaseId を足したら kEase も更新すること");

constexpr double kPi = 3.14159265358979323846;
} // namespace

const EaseInfo& GetEaseInfo(EaseId id)
{
    const std::size_t i = static_cast<std::size_t>(id);
    return kEase[i < static_cast<std::size_t>(EaseId::Count) ? i : 0];
}
const char* EaseName(EaseId id) { return GetEaseInfo(id).name; }

bool EaseFromName(std::string_view name, EaseId& out)
{
    for (const auto& e : kEase)
        if (name == e.name) { out = e.id; return true; }
    for (const auto& e : kEase)
        if (e.legacyTs && name == e.legacyTs) { out = e.id; return true; }
    return false;
}

bool EaseFromUiIndex(int t, EaseId& out)
{
    for (const auto& e : kEase)
        if (e.uiIndex == t) { out = e.id; return true; }
    return false;
}
int UiIndexOf(EaseId id) { return GetEaseInfo(id).uiIndex; }

double EvalEase(EaseId id, double p)
{
    if (!(p > 0.0)) return 0.0;      // NaN も 0
    if (p >= 1.0) return 1.0;
    constexpr double c1 = 1.70158;
    constexpr double c3 = c1 + 1.0;
    switch (id)
    {
    case EaseId::Linear:  return p;
    case EaseId::InQuad:  return p * p;
    case EaseId::OutQuad: { const double q = 1.0 - p; return 1.0 - q * q; }
    case EaseId::InOutQuad:
        if (p < 0.5) return 2.0 * p * p;
        { const double q = -2.0 * p + 2.0; return 1.0 - q * q / 2.0; }
    case EaseId::InCubic: return p * p * p;
    case EaseId::OutCubic: { const double q = 1.0 - p; return 1.0 - q * q * q; }
    case EaseId::InOutCubic:
        if (p < 0.5) return 4.0 * p * p * p;
        { const double q = -2.0 * p + 2.0; return 1.0 - q * q * q * 0.5; }
    case EaseId::InOutSine: return 0.5 - 0.5 * std::cos(p * kPi);
    case EaseId::OutBack: { const double q = p - 1.0; return 1.0 + c3 * q * q * q + c1 * q * q; }
    case EaseId::OutBounce:
    {
        constexpr double n1 = 7.5625, d1 = 2.75;
        if (p < 1.0 / d1) return n1 * p * p;
        if (p < 2.0 / d1) { p -= 1.5 / d1;   return n1 * p * p + 0.75; }
        if (p < 2.5 / d1) { p -= 2.25 / d1;  return n1 * p * p + 0.9375; }
        p -= 2.625 / d1;  return n1 * p * p + 0.984375;
    }
    case EaseId::OutElastic:
    {
        constexpr double c4 = 2.0 * kPi / 3.0;
        return std::pow(2.0, -10.0 * p) * std::sin((p * 10.0 - 0.75) * c4) + 1.0;
    }
    case EaseId::OutExpo: return 1.0 - std::pow(2.0, -10.0 * p);
    case EaseId::InBack:  return c3 * p * p * p - c1 * p * p;
    case EaseId::InOutBack:
    {
        constexpr double c2 = c1 * 1.525;
        if (p < 0.5) { const double q = 2.0 * p; return q * q * ((c2 + 1.0) * q - c2) * 0.5; }
        const double q = 2.0 * p - 2.0;
        return (q * q * ((c2 + 1.0) * q + c2) + 2.0) * 0.5;
    }
    case EaseId::OutQuint: { const double q = 1.0 - p; const double q2 = q * q; return 1.0 - q2 * q2 * q; }
    default: return p;
    }
}

// ===========================================================================
// 色空間
// ===========================================================================
double SrgbToLinear(double c)
{
    const double a = std::fabs(c);
    const double l = (a <= 0.04045) ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
    return std::copysign(l, c);
}
double LinearToSrgb(double c)
{
    const double a = std::fabs(c);
    const double s = (a <= 0.0031308) ? a * 12.92 : 1.055 * std::pow(a, 1.0 / 2.4) - 0.055;
    return std::copysign(s, c);
}
bool ChannelIsAlpha(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    const std::string_view tail = (dot == std::string_view::npos) ? name : name.substr(dot + 1);
    return tail == "a";
}

// ===========================================================================
// スカラーカーブ
// ===========================================================================
namespace
{
inline double Val(const Key& k, bool srgb) { return srgb ? SrgbToLinear(k.v) : k.v; }
inline double Cv(double v, bool srgb) { return srgb ? SrgbToLinear(v) : v; }
} // namespace

double Hermite(double v0, double m0, double v1, double m1, double h, double p)
{
    const double p2 = p * p, p3 = p2 * p;
    const double h00 = 2.0 * p3 - 3.0 * p2 + 1.0;
    const double h10 = p3 - 2.0 * p2 + p;
    const double h01 = -2.0 * p3 + 3.0 * p2;
    const double h11 = p3 - p2;
    return h00 * v0 + h10 * h * m0 + h01 * v1 + h11 * h * m1;
}

double AutoTangent(const std::vector<Key>& k, std::size_t i, bool srgb)
{
    const std::size_t n = k.size();
    const bool hasL = i > 0 && k[i].t > k[i - 1].t;
    const bool hasR = i + 1 < n && k[i + 1].t > k[i].t;
    if (!hasL && !hasR) return 0.0;
    const double vi = Val(k[i], srgb);
    const double dl = hasL ? (vi - Val(k[i - 1], srgb)) / static_cast<double>(k[i].t - k[i - 1].t) : 0.0;
    const double dr = hasR ? (Val(k[i + 1], srgb) - vi) / static_cast<double>(k[i + 1].t - k[i].t) : 0.0;
    if (!hasL) return dr;
    if (!hasR) return dl;
    if (dl * dr <= 0.0) return 0.0;   // 極値(または平坦)では接線 0
    const double m = (Val(k[i + 1], srgb) - Val(k[i - 1], srgb)) / static_cast<double>(k[i + 1].t - k[i - 1].t);
    const double limit = 3.0 * std::min(std::fabs(dl), std::fabs(dr));
    const double mag = std::min(std::fabs(m), limit);
    return (dl > 0.0) ? mag : -mag;
}

void DefaultBezierHandles(const std::vector<Key>& k, std::size_t i,
                          Tick& inDt, double& inDv, Tick& outDt, double& outDv)
{
    const std::size_t n = k.size();
    const double m = AutoTangent(k, i, false);
    outDt = 0; outDv = 0.0; inDt = 0; inDv = 0.0;
    if (i + 1 < n && k[i + 1].t > k[i].t)
    {
        outDt = (k[i + 1].t - k[i].t) / 3;
        outDv = m * static_cast<double>(outDt);
    }
    if (i > 0 && k[i].t > k[i - 1].t)
    {
        inDt = (k[i].t - k[i - 1].t) / 3;
        inDv = m * static_cast<double>(inDt);
    }
    if (outDv == 0.0) outDv = 0.0;
    if (inDv == 0.0) inDv = 0.0;
}

Tick WrapTimeForExtrap(Extrap mode, Tick t, Tick first, Tick last)
{
    if (mode != Extrap::Loop && mode != Extrap::PingPong) return t;
    if (t >= first && t <= last) return t;
    const Tick span = last - first;
    if (span <= 0) return first;
    if (mode == Extrap::Loop) return first + FloorMod(t - first, span);
    const Tick m = FloorMod(t - first, 2 * span);
    return first + (m <= span ? m : 2 * span - m);
}

double UnwrapDegrees(double prev, double cur)
{
    return cur - 360.0 * std::round((cur - prev) / 360.0);
}

namespace
{
// 前端(最初のキーから前へ延長する)接線
double FrontSlope(const std::vector<Key>& k, bool srgb)
{
    const Key& a = k.front();
    switch (a.ip)
    {
    case Interp::Auto: return AutoTangent(k, 0, srgb);
    case Interp::Linear:
    {
        for (std::size_t j = 1; j < k.size(); ++j)
            if (k[j].t > a.t) return (Val(k[j], srgb) - Val(a, srgb)) / static_cast<double>(k[j].t - a.t);
        return 0.0;
    }
    case Interp::Bezier:
        if (a.outDt > 0) return (Cv(a.v + a.outDv, srgb) - Val(a, srgb)) / static_cast<double>(a.outDt);
        return 0.0;
    default: return 0.0;
    }
}

// 後端(最後のキーから後ろへ延長する)接線。直前の区間の補間で決める。
double BackSlope(const std::vector<Key>& k, bool srgb)
{
    const std::size_t n = k.size();
    const Key& b = k[n - 1];
    std::size_t j = n - 1;
    while (j > 0 && k[j - 1].t >= b.t) --j;      // b と同時刻のキーを飛ばす
    if (j == 0) return 0.0;
    const Key& a = k[j - 1];                      // b より前の最後のキー
    switch (a.ip)
    {
    case Interp::Auto: return AutoTangent(k, n - 1, srgb);
    case Interp::Linear: return (Val(b, srgb) - Val(a, srgb)) / static_cast<double>(b.t - a.t);
    case Interp::Bezier:
        if (b.ip == Interp::Bezier && b.inDt > 0) return (Val(b, srgb) - Cv(b.v - b.inDv, srgb)) / static_cast<double>(b.inDt);
        return (Val(b, srgb) - Val(a, srgb)) / static_cast<double>(b.t - a.t);
    default: return 0.0;
    }
}

// ベジェ区間。a.ip == Bezier。
double EvalBezierSegment(const std::vector<Key>& k, std::size_t idx, Tick t, bool srgb)
{
    const Key& a = k[idx];
    const Key& b = k[idx + 1];
    const double span = static_cast<double>(b.t - a.t);
    double o = static_cast<double>(a.outDt), ov = a.outDv;
    double in, iv;
    if (b.ip == Interp::Bezier)
    {
        in = static_cast<double>(b.inDt); iv = b.inDv;
    }
    else
    {
        in = span / 3.0;
        iv = AutoTangent(k, idx + 1, false) * in;   // 既定の入りハンドル(Auto の見た目に合わせる)
    }
    // x が単調になるようにハンドルの dt を制限する(dv は傾きを保つよう同率で縮める)
    if (o > span) { if (o > 0.0) ov *= span / o; o = span; }
    if (in > span) { if (in > 0.0) iv *= span / in; in = span; }
    if (o + in > span && o + in > 0.0)
    {
        const double s = span / (o + in);
        o *= s; in *= s; ov *= s; iv *= s;
    }
    const double x0 = 0.0, x1 = o, x2 = span - in, x3 = span;
    const double y0 = Val(a, srgb), y1 = Cv(a.v + ov, srgb), y2 = Cv(b.v - iv, srgb), y3 = Val(b, srgb);
    const double tau = static_cast<double>(t - a.t);

    auto X = [&](double s) {
        const double u = 1.0 - s;
        return u * u * u * x0 + 3.0 * u * u * s * x1 + 3.0 * u * s * s * x2 + s * s * s * x3;
    };
    auto dX = [&](double s) {
        const double u = 1.0 - s;
        return 3.0 * u * u * (x1 - x0) + 6.0 * u * s * (x2 - x1) + 3.0 * s * s * (x3 - x2);
    };
    double lo = 0.0, hi = 1.0, s = tau / span;
    for (int iter = 0; iter < 48; ++iter)
    {
        const double e = X(s) - tau;
        if (std::fabs(e) < 1e-12 * span) break;
        if (e > 0.0) hi = s; else lo = s;
        const double d = dX(s);
        double sn = (d > 1e-14 * span) ? s - e / d : 0.5 * (lo + hi);
        if (!(sn > lo && sn < hi)) sn = 0.5 * (lo + hi);
        s = sn;
    }
    const double u = 1.0 - s;
    return u * u * u * y0 + 3.0 * u * u * s * y1 + 3.0 * u * s * s * y2 + s * s * s * y3;
}
} // namespace

double EvalChannel(const Channel& ch, Tick t, bool srgb)
{
    const std::vector<Key>& k = ch.keys;
    const std::size_t n = k.size();
    if (n == 0) return 0.0;
    if (n == 1) return Val(k[0], srgb);
    const Tick first = k.front().t, last = k.back().t;

    Tick tt = t;
    if (t < first)
    {
        switch (ch.pre)
        {
        case Extrap::Linear: return Val(k.front(), srgb) + FrontSlope(k, srgb) * static_cast<double>(t - first);
        case Extrap::Loop:
        case Extrap::PingPong: tt = WrapTimeForExtrap(ch.pre, t, first, last); break;
        default: return Val(k.front(), srgb);
        }
    }
    else if (t > last)
    {
        switch (ch.post)
        {
        case Extrap::Linear: return Val(k.back(), srgb) + BackSlope(k, srgb) * static_cast<double>(t - last);
        case Extrap::Loop:
        case Extrap::PingPong: tt = WrapTimeForExtrap(ch.post, t, first, last); break;
        default: return Val(k.back(), srgb);
        }
    }

    // tt 以下で最後のキー(同時刻は後ろが勝つ)
    const auto it = std::upper_bound(k.begin(), k.end(), tt, [](Tick v, const Key& kk) { return v < kk.t; });
    const std::size_t idx = static_cast<std::size_t>(it - k.begin()) - 1;
    if (idx + 1 >= n) return Val(k[n - 1], srgb);

    const Key& a = k[idx];
    const Key& b = k[idx + 1];
    const double span = static_cast<double>(b.t - a.t);
    const double p = static_cast<double>(tt - a.t) / span;
    const double va = Val(a, srgb), vb = Val(b, srgb);
    switch (a.ip)
    {
    case Interp::Step:   return va;
    case Interp::Linear: return va + (vb - va) * p;
    case Interp::Ease:   return va + (vb - va) * EvalEase(a.ease, p);
    case Interp::Auto:
        return Hermite(va, AutoTangent(k, idx, srgb), vb, AutoTangent(k, idx + 1, srgb), span, p);
    case Interp::Bezier: return EvalBezierSegment(k, idx, tt, srgb);
    }
    return va;
}

// ===========================================================================
// クォータニオン
// ===========================================================================
double QuatDot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Quat QuatNormalize(const Quat& q)
{
    const double l2 = QuatDot(q, q);
    if (!(l2 > 1e-24)) return Quat{};
    const double inv = 1.0 / std::sqrt(l2);
    return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
}

Quat QuatNeg(const Quat& q) { return { -q.x, -q.y, -q.z, -q.w }; }

namespace
{
// 符号を反転しない球面線形補間(a, b をそのままの点として結ぶ弧)。squad の内側/外側の補間に使う。
Quat SlerpRaw(const Quat& a, const Quat& b, double t)
{
    const double d = std::clamp(QuatDot(a, b), -1.0, 1.0);
    if (d > 0.9995 || d < -0.9995)
    {
        // ほぼ平行/反平行: 線形補間 + 正規化(sin θ ≈ 0 のゼロ除算回避)
        return QuatNormalize({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                               a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    }
    const double th = std::acos(d);
    const double sn = std::sin(th);
    const double wa = std::sin((1.0 - t) * th) / sn;
    const double wb = std::sin(t * th) / sn;
    return QuatNormalize({ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb });
}
} // namespace

Quat QuatSlerp(const Quat& a, const Quat& bIn, double t)
{
    return SlerpRaw(a, QuatDot(a, bIn) < 0.0 ? QuatNeg(bIn) : bIn, t);   // 最短経路
}

namespace
{
Quat QMul(const Quat& a, const Quat& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
Quat QConj(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
Quat QLog(const Quat& q)   // 単位クォータニオンの対数(w=0 の純虚)
{
    const double vl = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (vl < 1e-12) return { 0, 0, 0, 0 };
    const double th = std::atan2(vl, std::clamp(q.w, -1.0, 1.0));
    const double f = th / vl;
    return { q.x * f, q.y * f, q.z * f, 0.0 };
}
Quat QExp(const Quat& v)   // 純虚クォータニオンの指数
{
    const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l < 1e-12) return { 0, 0, 0, 1 };
    const double s = std::sin(l) / l;
    return { v.x * s, v.y * s, v.z * s, std::cos(l) };
}
} // namespace

Quat QuatSquadTangent(const Quat& prev, const Quat& cur, const Quat& next)
{
    const Quat inv = QConj(cur);
    const Quat l1 = QLog(QMul(inv, next));
    const Quat l0 = QLog(QMul(inv, prev));
    const Quat e = QExp({ -(l1.x + l0.x) * 0.25, -(l1.y + l0.y) * 0.25, -(l1.z + l0.z) * 0.25, 0.0 });
    return QuatNormalize(QMul(cur, e));
}

Quat QuatSquad(const Quat& q0, const Quat& q1, const Quat& s0, const Quat& s1, double t)
{
    // ★squad は符号を反転しない slerp で組む(反転すると、大きな回転差で内側の弧が反対側へ回り込み、途中で出力が跳ぶ)
    const Quat a = SlerpRaw(q0, q1, t);
    const Quat b = SlerpRaw(s0, s1, t);
    return SlerpRaw(a, b, 2.0 * t * (1.0 - t));
}

double QuatAngle(const Quat& a, const Quat& b)
{
    const double d = std::min(1.0, std::fabs(QuatDot(QuatNormalize(a), QuatNormalize(b))));
    return 2.0 * std::acos(d);
}

Quat EvalQuatChannels(const Channel& cx, const Channel& cy, const Channel& cz, const Channel& cw, Tick t)
{
    const std::size_t n = cx.keys.size();
    if (n == 0) return Quat{};
    auto keyQuat = [&](std::size_t i) {
        return QuatNormalize({ cx.keys[i].v, cy.keys[i].v, cz.keys[i].v, cw.keys[i].v });
    };
    if (n == 1) return keyQuat(0);

    const Tick first = cx.keys.front().t, last = cx.keys.back().t;
    Tick tt = t;
    if (t < first)
    {
        if (cx.pre == Extrap::Loop || cx.pre == Extrap::PingPong) tt = WrapTimeForExtrap(cx.pre, t, first, last);
        else return keyQuat(0);
    }
    else if (t > last)
    {
        if (cx.post == Extrap::Loop || cx.post == Extrap::PingPong) tt = WrapTimeForExtrap(cx.post, t, first, last);
        else return keyQuat(n - 1);
    }

    const auto it = std::upper_bound(cx.keys.begin(), cx.keys.end(), tt, [](Tick v, const Key& kk) { return v < kk.t; });
    const std::size_t idx = static_cast<std::size_t>(it - cx.keys.begin()) - 1;

    // idx-1 .. idx+2 のキーを、先頭からの半球連続化つきで取り出す(O(idx))
    Quat win[4];
    Quat prev{};
    const std::size_t lo = idx > 0 ? idx - 1 : 0;
    const std::size_t hi = std::min(n - 1, idx + 2);
    for (std::size_t i = 0; i <= hi; ++i)
    {
        Quat q = keyQuat(i);
        if (i > 0 && QuatDot(q, prev) < 0.0) q = QuatNeg(q);
        prev = q;
        if (i >= lo) win[i - lo + (idx > 0 ? 0 : 1)] = q;   // win[0]=idx-1(無ければ空き), win[1]=idx, win[2]=idx+1, win[3]=idx+2
    }
    if (idx + 1 >= n) return win[1];

    const Key& a = cx.keys[idx];
    const Quat& q0 = win[1];
    const Quat& q1 = win[2];
    const bool hasPrev = idx > 0;
    const bool hasNext2 = idx + 2 < n;
    const double p = static_cast<double>(tt - a.t) / static_cast<double>(cx.keys[idx + 1].t - a.t);
    switch (a.ip)
    {
    case Interp::Step:   return q0;
    case Interp::Linear: return QuatSlerp(q0, q1, p);
    case Interp::Ease:   return QuatSlerp(q0, q1, EvalEase(a.ease, p));
    case Interp::Auto:
    case Interp::Bezier:
    {
        const Quat& qp = hasPrev ? win[0] : q0;      // 端は自分自身で代用(接線がそちらへ引かれない)
        const Quat& qn = hasNext2 ? win[3] : q1;
        const Quat s0 = QuatSquadTangent(qp, q0, q1);
        const Quat s1 = QuatSquadTangent(q0, q1, qn);
        return QuatSquad(q0, q1, s0, s1, p);
    }
    }
    return q0;
}

} // namespace dx12e::seq
