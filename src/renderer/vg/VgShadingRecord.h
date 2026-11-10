#pragma once
// VgShadingRecord.h ― .vgeo v1.1 の拡張 'MSHD'（材質ごとの拡張シェーディング）の 1 材質ぶんの詰め方。
//   ShadingExt（renderer/ShadingExt.h）の 13 個の f32 をその並びで置き、残り 3 語は 0。GPU 形（PackShadingExt の 8 DWORD）は
//   読み手が作る（ファイルは「意味の値」を持ち、GPU の詰め方が変わっても作り直さなくてよい）。
//   書き手: resource/VgAutoCook.cpp / 読み手: VirtualGeometrySystem（VG の材質表）・VgeoProxyLoader（プロキシの Material::shading）。
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "renderer/ShadingExt.h"

namespace dx12e::vg
{
// .vgeo の 'MSHD' の 1 材質ぶん（ShadingExt の 13 個の f32 + 0 詰め）と、その逆
inline std::array<uint32_t, 16> PackShadingRecord(const ShadingExt& e)
{
    const float f[16] = {e.clearCoat, e.clearCoatRoughness, e.anisotropy, e.anisotropyRotation, e.sheenColor[0], e.sheenColor[1],
                         e.sheenColor[2], e.sheenRoughness, e.specularFactor, e.specularColor[0], e.specularColor[1], e.specularColor[2],
                         e.ior, 0.0f, 0.0f, 0.0f};
    std::array<uint32_t, 16> w{};
    std::memcpy(w.data(), f, sizeof f);
    return w;
}
inline ShadingExt UnpackShadingRecord(const std::array<uint32_t, 16>& w)
{
    float f[16];
    std::memcpy(f, w.data(), sizeof f);
    ShadingExt e;
    e.clearCoat = f[0]; e.clearCoatRoughness = f[1]; e.anisotropy = f[2]; e.anisotropyRotation = f[3];
    e.sheenColor[0] = f[4]; e.sheenColor[1] = f[5]; e.sheenColor[2] = f[6]; e.sheenRoughness = f[7];
    e.specularFactor = f[8]; e.specularColor[0] = f[9]; e.specularColor[1] = f[10]; e.specularColor[2] = f[11]; e.ior = f[12];
    for (float v : f) if (!std::isfinite(v)) return ShadingExt{};   // 壊れた値は既定へ
    ClampShadingExt(e);
    return e;
}

} // namespace dx12e::vg
