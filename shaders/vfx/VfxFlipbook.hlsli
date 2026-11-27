// VfxFlipbook.hlsli - UnoFX フリップブックの算術（W1-BD7）。src/renderer/vfx/VfxFlipbook.{h,cpp} の CPU 参照と同じ式
//   （規約の正は assets/engine/vfx/vfx_bake_manifest.json の conventions。VfxFlipbook.h の先頭にも要約がある）。
//   枠は左上から行優先、枠内 uv は左上が (0,0)・v 下向き。引く uv は枠の矩形の半テクセル内側へクランプしてからアトラスのオフセットを足す。
//   MV は引く位置で 2 回評価し直す。サンプラは線形・クランプ（半テクセル内側なので隣の枠は混ざらない）。
#ifndef UNO_VFX_FLIPBOOK_HLSLI
#define UNO_VFX_FLIPBOOK_HLSLI
#include "VfxCommon.hlsli"

uint FbStartFrame(uint particleId, uint frames)
{
    // VfxCommon.hlsli の HashU。VfxFlipbook.cpp の StartFrame と同じ
    return frames == 0u ? 0u : HashU(0xF11Bu, particleId, 3u) % frames;
}

struct FbPick { uint a; uint b; float t; };

FbPick FbPickFrames(uint frames, float age, float life, float fps, uint startFrame)
{
    FbPick r;
    uint n = max(1u, frames);
    if (fps > 0.0)
    {
        float f = max(age, 0.0) * fps + float(startFrame);
        float fl = floor(f);
        r.t = f - fl;
        float wrapped = fmod(fl, float(n));
        r.a = (uint)wrapped;
        r.b = (r.a + 1u) % n;
    }
    else
    {
        float f = saturate(life > 0.0 ? age / life : 0.0) * float(n - 1u);
        float fl = floor(f);
        r.a = min((uint)fl, n - 1u);
        r.t = f - fl;
        r.b = min(r.a + 1u, n - 1u);
        if (r.a == n - 1u) r.t = 0.0;
    }
    return r;
}

float2 FbClampInFrame(float2 uv, float2 frameTexels)
{
    float2 h = 0.5 / frameTexels;
    return min(max(uv, h), 1.0 - h);
}

float2 FbAtlasUv(float2 uv, uint2 grid, uint frame)
{
    uint c = frame % grid.x, r = frame / grid.x;
    return (float2(c, r) + uv) / float2(grid);
}

// アトラスから、枠 frame の枠内 uv（クランプ前）で引く
float4 FbSampleFrame(Texture2D<float4> tex, SamplerState s, uint2 grid, uint frame, float2 uv)
{
    uint w, h;
    tex.GetDimensions(w, h);
    float2 ft = float2(w / grid.x, h / grid.y);
    return tex.SampleLevel(s, FbAtlasUv(FbClampInFrame(uv, ft), grid, frame), 0);
}

float2 FbSampleMv(Texture2D<float2> mv, SamplerState s, uint2 grid, uint frame, float2 uv, float mvRange)
{
    uint w, h;
    mv.GetDimensions(w, h);
    float2 ft = float2(w / grid.x, h / grid.y);
    return mv.SampleLevel(s, FbAtlasUv(FbClampInFrame(uv, ft), grid, frame), 0) * mvRange;
}

// MV で補間するときの『絵を引く位置』（枠内 uv。クランプ前）。2 回の不動点反復
void FbResolveTaps(Texture2D<float2> mv, SamplerState s, uint2 grid, float mvRange, float2 uv, float t, uint a, uint b,
                   out float2 pa, out float2 pb)
{
    pa = uv; pb = uv;
    [unroll] for (int k = 0; k < 2; ++k)
    {
        pa = uv - FbSampleMv(mv, s, grid, a, pa, mvRange) * t;
        pb = uv + FbSampleMv(mv, s, grid, b, pb, mvRange) * (1.0 - t);
    }
}

// ---- 炎族 ----
float3 FbBlackbodyColor(float kelvin)
{
    float T = clamp(kelvin, 1667.0, 25000.0);
    float T2 = T * T, T3 = T2 * T;
    float x = T <= 4000.0 ? (-0.2661239e9 / T3 - 0.2343589e6 / T2 + 0.8776956e3 / T + 0.179910)
                          : (-3.0258469e9 / T3 + 2.1070379e6 / T2 + 0.2226347e3 / T + 0.240390);
    float x2 = x * x, x3 = x2 * x;
    float y = T <= 2222.0 ? (-1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683)
            : T <= 4000.0 ? (-0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867)
                          : (3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483);
    float X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
    float3 c = float3(3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z,
                      -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z,
                      0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z);
    c = max(c, 0.0);
    float m = max(c.x, max(c.y, c.z));
    return m > 0.0 ? c / m : c;
}

float FbErosion(float er, float th, float w)
{
    w = max(w, 1e-4);
    float t = saturate((er + w - th) / w);
    return t * t * (3.0 - 2.0 * t);
}

float FbErodeThreshold(float tLife, float erodeStart, float w)
{
    float k = saturate((tLife - erodeStart) / max(1.0 - erodeStart, 1e-4));
    return k * (1.0 + max(w, 1e-4));
}

struct FbShaded { float3 rgb; float a; };

// p0 = (tempMin, tempMax, emission, 0)、p1 = (erodeStart, erodeWidth, 0, 0)
FbShaded FbShadeFire(float4 s, float tLife, float3 p0, float2 p1)
{
    float th = FbErodeThreshold(tLife, p1.x, p1.y);
    FbShaded o;
    o.a = saturate(s.y) * FbErosion(s.z, th, p1.y);
    float kelvin = p0.x + (p0.y - p0.x) * saturate(s.x);
    float rad = pow(max(kelvin / max(p0.y, 1.0), 0.0), 4.0);
    o.rgb = FbBlackbodyColor(kelvin) * (rad * p0.z * saturate(s.w));
    return o;
}
#endif
