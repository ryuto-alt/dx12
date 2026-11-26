#include "renderer/vfx/VfxCurl.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>

#include "renderer/vfx/VfxSpawnMath.h"

namespace dx12e::vfx
{

namespace
{
constexpr int N = static_cast<int>(kCurlRes);

float Quintic(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// 周期 P（セル数）の値ノイズ。x, y, z は 0..P のセル座標。格子の値は固定ハッシュ（成分 comp ごとに別）。
float PeriodicValueNoise(float x, float y, float z, int P, u32 comp)
{
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y)), zi = static_cast<int>(std::floor(z));
    const float fx = Quintic(x - static_cast<float>(xi)), fy = Quintic(y - static_cast<float>(yi)), fz = Quintic(z - static_cast<float>(zi));
    auto lat = [&](int a, int b, int c)
    {
        const u32 ua = static_cast<u32>(((a % P) + P) % P), ub = static_cast<u32>(((b % P) + P) % P), uc = static_cast<u32>(((c % P) + P) % P);
        return Hash01(0xC0FFEEu + comp * 7919u, ua + (ub << 8) + (uc << 16), static_cast<u32>(P)) * 2.0f - 1.0f;
    };
    float v[2][2][2];
    for (int dz = 0; dz < 2; ++dz) for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
        v[dz][dy][dx] = lat(xi + dx, yi + dy, zi + dz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(v[0][0][0], v[0][0][1], fx), x10 = lerp(v[0][1][0], v[0][1][1], fx);
    const float x01 = lerp(v[1][0][0], v[1][0][1], fx), x11 = lerp(v[1][1][0], v[1][1][1], fx);
    return lerp(lerp(x00, x10, fy), lerp(x01, x11, fy), fz);
}

// ψ の成分 comp（0..2）。セル (ix, iy, iz) は 0..N-1。オクターブは P = 4, 8, 16（N の約数なので 64 で周期が閉じる）。
float Psi(int ix, int iy, int iz, u32 comp)
{
    float sum = 0.0f, amp = 1.0f;
    const int Ps[3] = { 4, 8, 16 };
    for (int o = 0; o < 3; ++o)
    {
        const float s = static_cast<float>(Ps[o]) / static_cast<float>(N);
        sum += amp * PeriodicValueNoise(static_cast<float>(ix) * s, static_cast<float>(iy) * s, static_cast<float>(iz) * s, Ps[o], comp);
        amp *= 0.5f;
    }
    return sum;
}

inline size_t Idx(int x, int y, int z)
{
    x = (x % N + N) % N; y = (y % N + N) % N; z = (z % N + N) % N;
    return (static_cast<size_t>(z) * N * N + static_cast<size_t>(y) * N + static_cast<size_t>(x)) * 4;
}
} // namespace

std::vector<float> GenerateCurlField()
{
    const size_t total = static_cast<size_t>(N) * N * N;
    std::vector<float> psi(total * 3);
    for (int z = 0; z < N; ++z) for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const size_t i = (static_cast<size_t>(z) * N * N + static_cast<size_t>(y) * N + static_cast<size_t>(x)) * 3;
        for (u32 c = 0; c < 3; ++c) psi[i + c] = Psi(x, y, z, c);
    }
    auto P = [&](int x, int y, int z, int c)
    {
        x = (x % N + N) % N; y = (y % N + N) % N; z = (z % N + N) % N;
        return psi[(static_cast<size_t>(z) * N * N + static_cast<size_t>(y) * N + static_cast<size_t>(x)) * 3 + c];
    };
    std::vector<float> out(total * 4, 0.0f);
    float maxLen = 0.0f;
    for (int z = 0; z < N; ++z) for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const float dPz_dy = (P(x, y + 1, z, 2) - P(x, y - 1, z, 2)) * 0.5f;
        const float dPy_dz = (P(x, y, z + 1, 1) - P(x, y, z - 1, 1)) * 0.5f;
        const float dPx_dz = (P(x, y, z + 1, 0) - P(x, y, z - 1, 0)) * 0.5f;
        const float dPz_dx = (P(x + 1, y, z, 2) - P(x - 1, y, z, 2)) * 0.5f;
        const float dPy_dx = (P(x + 1, y, z, 1) - P(x - 1, y, z, 1)) * 0.5f;
        const float dPx_dy = (P(x, y + 1, z, 0) - P(x, y - 1, z, 0)) * 0.5f;
        float* o = &out[Idx(x, y, z)];
        o[0] = dPz_dy - dPy_dz;
        o[1] = dPx_dz - dPz_dx;
        o[2] = dPy_dx - dPx_dy;
        maxLen = std::max(maxLen, std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]));
    }
    const float inv = maxLen > 0.0f ? 1.0f / maxLen : 1.0f;
    for (size_t i = 0; i < total; ++i) { out[i * 4] *= inv; out[i * 4 + 1] *= inv; out[i * 4 + 2] *= inv; }
    return out;
}

std::vector<u16> GenerateCurlFieldHalf()
{
    const std::vector<float> f = GenerateCurlField();
    std::vector<u16> h(f.size());
    for (size_t i = 0; i < f.size(); ++i) h[i] = DirectX::PackedVector::XMConvertFloatToHalf(f[i]);
    return h;
}

void SampleCurl(const std::vector<float>& field, float u, float v, float w, float out[3])
{
    // GPU の三重線形（ラップ）: テクセル座標 = uvw * N - 0.5
    const float p[3] = { u * N - 0.5f, v * N - 0.5f, w * N - 0.5f };
    int i0[3]; float t[3];
    for (int k = 0; k < 3; ++k) { const float fl = std::floor(p[k]); i0[k] = static_cast<int>(fl); t[k] = p[k] - fl; }
    out[0] = out[1] = out[2] = 0.0f;
    for (int dz = 0; dz < 2; ++dz) for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
    {
        const float wgt = (dx ? t[0] : 1.0f - t[0]) * (dy ? t[1] : 1.0f - t[1]) * (dz ? t[2] : 1.0f - t[2]);
        const float* s = &field[Idx(i0[0] + dx, i0[1] + dy, i0[2] + dz)];
        out[0] += s[0] * wgt; out[1] += s[1] * wgt; out[2] += s[2] * wgt;
    }
}

float MaxDivergence(const std::vector<float>& f)
{
    float m = 0.0f;
    for (int z = 0; z < N; ++z) for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const float d = (f[Idx(x + 1, y, z)] - f[Idx(x - 1, y, z)]) * 0.5f
                      + (f[Idx(x, y + 1, z) + 1] - f[Idx(x, y - 1, z) + 1]) * 0.5f
                      + (f[Idx(x, y, z + 1) + 2] - f[Idx(x, y, z - 1) + 2]) * 0.5f;
        m = std::max(m, std::fabs(d));
    }
    return m;
}

} // namespace dx12e::vfx
