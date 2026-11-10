#pragma once

// ShadingExtJson.h - 拡張シェーディング（renderer/ShadingExt.h）の JSON 読み書き。.dxmat と シーン JSON の "material" で共通。
//   キー: clearCoat / clearCoatRoughness / anisotropy / anisotropyRotation(ラジアン) / sheenColor[r,g,b] / sheenRoughness /
//         specularFactor / specularColor[r,g,b] / ior
//   ★書くのは既定値と違う欄だけ（既存ファイルは 1 バイトも変わらない）。読みは欄ごと、型違いは読み捨てる。

#include <charconv>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "renderer/ShadingExt.h"

namespace dx12e
{
namespace shadingjson
{

inline bool Num(const nlohmann::json& j, const char* key, f32& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    out = it->get<f32>();
    return true;
}
inline bool Vec3(const nlohmann::json& j, const char* key, f32 (&out)[3])
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return false;
    for (size_t i = 0; i < 3; ++i)
        if (!(*it)[i].is_number()) return false;
    for (size_t i = 0; i < 3; ++i) out[i] = (*it)[i].get<f32>();
    return true;
}
// float → JSON の数値（0.6f が 0.6000000238418579 にならないよう最短ラウンドトリップ表記を経由）
inline double Sf(f32 v)
{
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    if (r.ec != std::errc()) return static_cast<double>(v);
    *r.ptr = '\0';
    return std::strtod(buf, nullptr);
}
inline nlohmann::json ToArr(const f32 (&v)[3]) { return nlohmann::json::array({Sf(v[0]), Sf(v[1]), Sf(v[2])}); }

// ---- 材質（.dxmat）----
inline void Write(nlohmann::json& j, const ShadingExt& e)
{
    const ShadingExt d;
    if (e.clearCoat          != d.clearCoat)          j["clearCoat"]          = Sf(e.clearCoat);
    if (e.clearCoatRoughness != d.clearCoatRoughness) j["clearCoatRoughness"] = Sf(e.clearCoatRoughness);
    if (e.anisotropy         != d.anisotropy)         j["anisotropy"]         = Sf(e.anisotropy);
    if (e.anisotropyRotation != d.anisotropyRotation) j["anisotropyRotation"] = Sf(e.anisotropyRotation);
    if (e.sheenColor[0] != 0 || e.sheenColor[1] != 0 || e.sheenColor[2] != 0) j["sheenColor"] = ToArr(e.sheenColor);
    if (e.sheenRoughness     != d.sheenRoughness)     j["sheenRoughness"]     = Sf(e.sheenRoughness);
    if (e.specularFactor     != d.specularFactor)     j["specularFactor"]     = Sf(e.specularFactor);
    if (e.specularColor[0] != 1 || e.specularColor[1] != 1 || e.specularColor[2] != 1) j["specularColor"] = ToArr(e.specularColor);
    if (e.ior                != d.ior)                j["ior"]                = Sf(e.ior);
}
// j に拡張のキーが 1 つでもあれば e を埋めて true（読みは ClampShadingExt を通す）
inline bool Read(const nlohmann::json& j, ShadingExt& e)
{
    if (!j.is_object()) return false;
    bool any = false;
    any |= Num(j, "clearCoat", e.clearCoat);
    any |= Num(j, "clearCoatRoughness", e.clearCoatRoughness);
    any |= Num(j, "anisotropy", e.anisotropy);
    any |= Num(j, "anisotropyRotation", e.anisotropyRotation);
    any |= Vec3(j, "sheenColor", e.sheenColor);
    any |= Num(j, "sheenRoughness", e.sheenRoughness);
    any |= Num(j, "specularFactor", e.specularFactor);
    any |= Vec3(j, "specularColor", e.specularColor);
    any |= Num(j, "ior", e.ior);
    if (any) ClampShadingExt(e);
    return any;
}

// ---- エンティティの上書き（シーン JSON の "material"）。設定した欄だけ ----
inline void WriteOverride(nlohmann::json& j, const ShadingExtOverride& o)
{
    if (o.clearCoat          >= 0.0f) j["clearCoat"]          = Sf(o.clearCoat);
    if (o.clearCoatRoughness >= 0.0f) j["clearCoatRoughness"] = Sf(o.clearCoatRoughness);
    if (o.anisotropySet)              j["anisotropy"]         = Sf(o.anisotropy);
    if (o.anisotropyRotation >= 0.0f) j["anisotropyRotation"] = Sf(o.anisotropyRotation);
    if (o.sheenColor[0]      >= 0.0f) j["sheenColor"]         = ToArr(o.sheenColor);
    if (o.sheenRoughness     >= 0.0f) j["sheenRoughness"]     = Sf(o.sheenRoughness);
    if (o.specularFactor     >= 0.0f) j["specularFactor"]     = Sf(o.specularFactor);
    if (o.specularColor[0]   >= 0.0f) j["specularColor"]      = ToArr(o.specularColor);
    if (o.ior                >= 0.0f) j["ior"]                = Sf(o.ior);
}
inline void ReadOverride(const nlohmann::json& j, ShadingExtOverride& o)
{
    if (!j.is_object()) return;
    f32 v = 0.0f;
    if (Num(j, "clearCoat", v))          o.clearCoat          = std::clamp(v, 0.0f, 1.0f);
    if (Num(j, "clearCoatRoughness", v)) o.clearCoatRoughness = std::clamp(v, 0.0f, 1.0f);
    if (Num(j, "anisotropy", v))         { o.anisotropySet = true; o.anisotropy = std::clamp(v, -1.0f, 1.0f); }
    if (Num(j, "anisotropyRotation", v) && std::isfinite(v))
    {
        const f32 twoPi = 6.28318530718f;
        v = std::fmod(v, twoPi);
        if (v < 0.0f) v += twoPi;
        o.anisotropyRotation = v;
    }
    f32 c[3];
    if (Vec3(j, "sheenColor", c))
        for (int i = 0; i < 3; ++i) o.sheenColor[i] = std::clamp(c[i], 0.0f, 1.0f);
    if (Num(j, "sheenRoughness", v))     o.sheenRoughness     = std::clamp(v, 0.0f, 1.0f);
    if (Num(j, "specularFactor", v))     o.specularFactor     = std::clamp(v, 0.0f, 1.0f);
    if (Vec3(j, "specularColor", c))
        for (int i = 0; i < 3; ++i) o.specularColor[i] = std::clamp(c[i], 0.0f, 16.0f);
    if (Num(j, "ior", v))                o.ior                = std::clamp(v, 1.0f, 5.0f);
}

} // namespace shadingjson
} // namespace dx12e
