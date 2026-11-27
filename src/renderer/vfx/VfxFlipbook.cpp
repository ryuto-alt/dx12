#include "renderer/vfx/VfxFlipbook.h"

#include <cstring>

#include "renderer/vfx/VfxSpawnMath.h"

namespace dx12e::vfx::flip
{

namespace
{
constexpr u32 kDxgiRgba16F = 10, kDxgiRgba16Snorm = 13, kDxgiRgba8 = 28, kDxgiRgba8Srgb = 29, kDxgiRg16Snorm = 37;
constexpr u32 kFourCcDx10 = 0x30315844u;   // "DX10"

float HalfToFloat(u16 h)
{
    const u32 s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400u), static_cast<int>(e) - 25);
    return s ? -v : v;
}
float Snorm16(i16 v) { return std::max(float(v) / 32767.0f, -1.0f); }

u32 Rd32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }

float Smoothstep(float a, float b, float x)
{
    const float t = Saturate((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

u32 BytesPerTexel(u32 dxgi)
{
    switch (dxgi)
    {
    case kDxgiRgba8: case kDxgiRgba8Srgb: case kDxgiRg16Snorm: return 4;
    case kDxgiRgba16F: case kDxgiRgba16Snorm: return 8;
    default: return 0;
    }
}

u32 StartFrame(u32 particleId, u32 frames)
{
    return frames == 0 ? 0u : HashU(0xF11Bu, particleId, 3u) % frames;
}

FramePick PickFrames(const Grid& g, float age, float life, float fps, u32 startFrame)
{
    FramePick r;
    const u32 n = std::max(1u, g.frames);
    if (fps > 0.0f)
    {
        const float f = std::max(age, 0.0f) * fps + float(startFrame);
        const float fl = std::floor(f);
        r.t = f - fl;
        // u32 へ落とす前に周期で畳む（大きい f でも桁あふれしない）
        const float wrapped = std::fmod(fl, float(n));
        r.a = static_cast<u32>(wrapped);
        r.b = (r.a + 1u) % n;
    }
    else
    {
        const float f = Saturate(life > 0.0f ? age / life : 0.0f) * float(n - 1u);
        const float fl = std::floor(f);
        r.a = std::min(static_cast<u32>(fl), n - 1u);
        r.t = f - fl;
        r.b = std::min(r.a + 1u, n - 1u);
        if (r.a == n - 1u) r.t = 0.0f;
    }
    return r;
}

V4 CpuTex::Texel(int x, int y) const
{
    x = std::min(std::max(x, 0), int(w) - 1);
    y = std::min(std::max(y, 0), int(h) - 1);
    const u8* p = &px[(size_t(y) * w + size_t(x)) * bpp];
    switch (dxgi)
    {
    case kDxgiRgba8: case kDxgiRgba8Srgb: return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    case kDxgiRg16Snorm: { i16 v[2]; std::memcpy(v, p, 4); return {Snorm16(v[0]), Snorm16(v[1]), 0.0f, 1.0f}; }
    case kDxgiRgba16Snorm: { i16 v[4]; std::memcpy(v, p, 8); return {Snorm16(v[0]), Snorm16(v[1]), Snorm16(v[2]), Snorm16(v[3])}; }
    case kDxgiRgba16F: { u16 v[4]; std::memcpy(v, p, 8); return {HalfToFloat(v[0]), HalfToFloat(v[1]), HalfToFloat(v[2]), HalfToFloat(v[3])}; }
    default: return {};
    }
}

V4 CpuTex::Bilinear(V2 uv) const
{
    const float fx = uv.x * float(w) - 0.5f, fy = uv.y * float(h) - 0.5f;
    const float flx = std::floor(fx), fly = std::floor(fy);
    const int x0 = int(flx), y0 = int(fly);
    const float tx = fx - flx, ty = fy - fly;
    const V4 a = Lerp(Texel(x0, y0), Texel(x0 + 1, y0), tx);
    const V4 b = Lerp(Texel(x0, y0 + 1), Texel(x0 + 1, y0 + 1), tx);
    return Lerp(a, b, ty);
}

bool ParseDdsHeader(const u8* d, size_t size, DdsInfo& out, std::string& err)
{
    if (size < 148 || std::memcmp(d, "DDS ", 4) != 0) { err = "DDS ではない"; return false; }
    if (Rd32(d + 4) != 124u) { err = "DDS のヘッダサイズが違う"; return false; }
    const u32 h = Rd32(d + 12), w = Rd32(d + 16), depth = Rd32(d + 24), mips = Rd32(d + 28);
    const u32 caps2 = Rd32(d + 112);
    if ((Rd32(d + 80) & 0x4u) == 0u || Rd32(d + 84) != kFourCcDx10) { err = "DDS は DX10 拡張だけ対応"; return false; }
    const u32 dxgi = Rd32(d + 128), dim = Rd32(d + 132), arr = Rd32(d + 140);
    if (dim != 3u) { err = "DDS は 2D だけ対応"; return false; }
    if (depth > 1u || (caps2 & 0x200u) || (caps2 & 0x200000u)) { err = "DDS はキューブ / 3D 非対応"; return false; }
    if (mips > 1u) { err = "DDS はミップ無しだけ対応"; return false; }
    if (arr > 1u) { err = "DDS は配列非対応"; return false; }
    const u32 bpp = BytesPerTexel(dxgi);
    if (bpp == 0) { err = "DDS の形式（DXGI " + std::to_string(dxgi) + "）に対応していない"; return false; }
    if (w == 0 || h == 0 || w > 16384u || h > 16384u) { err = "DDS の大きさが不正"; return false; }
    if (size < 148 + size_t(w) * h * bpp) { err = "DDS のデータが足りない"; return false; }
    out.w = w; out.h = h; out.dxgi = dxgi; out.bpp = bpp; out.dataOffset = 148;
    return true;
}

bool LoadDds(const u8* data, size_t size, CpuTex& out, std::string& err)
{
    DdsInfo info;
    if (!ParseDdsHeader(data, size, info, err)) return false;
    out.w = info.w; out.h = info.h; out.dxgi = info.dxgi; out.bpp = info.bpp;
    out.px.assign(data + info.dataOffset, data + info.dataOffset + size_t(info.w) * info.h * info.bpp);
    return true;
}

V4 SampleFrame(const CpuTex& atlas, const Grid& g, u32 frame, V2 uv)
{
    const V2 ft = {float(atlas.w / g.cols), float(atlas.h / g.rows)};
    return atlas.Bilinear(AtlasUv(ClampInFrame(uv, ft), g, frame));
}

V2 SampleMv(const CpuTex& mv, const Grid& g, u32 frame, V2 uv, float mvRange)
{
    const V4 s = SampleFrame(mv, g, frame, uv);
    return {s.x * mvRange, s.y * mvRange};
}

V3 BlackbodyColor(float kelvin)
{
    const float T = std::min(std::max(kelvin, 1667.0f), 25000.0f);
    const float T2 = T * T, T3 = T2 * T;
    float x;
    if (T <= 4000.0f) x = -0.2661239e9f / T3 - 0.2343589e6f / T2 + 0.8776956e3f / T + 0.179910f;
    else              x = -3.0258469e9f / T3 + 2.1070379e6f / T2 + 0.2226347e3f / T + 0.240390f;
    const float x2 = x * x, x3 = x2 * x;
    float y;
    if (T <= 2222.0f)      y = -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
    else if (T <= 4000.0f) y = -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
    else                   y =  3.0817580f * x3 - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;
    const float X = x / y, Y = 1.0f, Z = (1.0f - x - y) / y;
    float r =  3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
    float g = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
    float b =  0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;
    r = std::max(r, 0.0f); g = std::max(g, 0.0f); b = std::max(b, 0.0f);
    const float m = std::max(r, std::max(g, b));
    const float inv = m > 0.0f ? 1.0f / m : 1.0f;
    return {r * inv, g * inv, b * inv};
}

float Erosion(float er, float th, float w)
{
    w = std::max(w, 1e-4f);
    return Smoothstep(th, th + w, er + w);
}

float ErodeThreshold(float tLife, float erodeStart, float w)
{
    const float k = Saturate((tLife - erodeStart) / std::max(1.0f - erodeStart, 1e-4f));
    return k * (1.0f + std::max(w, 1e-4f));
}

Shaded ShadeFire(V4 s, float tLife, const FireParams& p)
{
    const float th = ErodeThreshold(tLife, p.erodeStart, p.erodeWidth);
    const float a = Saturate(s.y) * Erosion(s.z, th, p.erodeWidth);
    const float kelvin = p.tempMin + (p.tempMax - p.tempMin) * Saturate(s.x);
    const float rad = std::pow(std::max(kelvin / std::max(p.tempMax, 1.0f), 0.0f), 4.0f);   // 放射 ∝ T^4（tempMax で 1）
    const V3 bb = BlackbodyColor(kelvin);
    const float k = rad * p.emission * Saturate(s.w);
    return {{bb.x * k, bb.y * k, bb.z * k}, a};
}

float SixWayLight(V3 L, V4 rlu, V4 dbf)
{
    return L.x * L.x * (L.x > 0.0f ? rlu.x : rlu.y)
         + L.y * L.y * (L.y > 0.0f ? rlu.z : dbf.x)
         + L.z * L.z * (L.z > 0.0f ? dbf.z : dbf.y);
}

float SixWayAmbient(V4 rlu, V4 dbf)
{
    return (rlu.x + rlu.y + rlu.z + dbf.x + dbf.y + dbf.z) * (1.0f / 6.0f);
}

Shaded ShadeSmoke(V4 rlu, V4 dbf, V3 L, float tLife, V3 albedo, V3 ambient, V3 sun, const SmokeParams& p)
{
    const float th = ErodeThreshold(tLife, p.erodeStart, p.erodeWidth);
    const float a = Saturate(rlu.w) * Erosion(dbf.w, th, p.erodeWidth);
    const float direct = SixWayLight(L, rlu, dbf);
    const float amb = SixWayAmbient(rlu, dbf);
    return {{albedo.x * (ambient.x * amb + sun.x * direct),
             albedo.y * (ambient.y * amb + sun.y * direct),
             albedo.z * (ambient.z * amb + sun.z * direct)}, a};
}

V3 LightToBillboard(V3 lw, V3 right, V3 up, V3 towardCam, float rot)
{
    auto dot = [](V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const float cx = dot(lw, right), cy = dot(lw, up), cz = dot(lw, towardCam);
    const float c = std::cos(rot), s = std::sin(rot);
    return {cx * c + cy * s, -cx * s + cy * c, cz};
}

} // namespace dx12e::vfx::flip
