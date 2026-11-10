#pragma once

// ShadingExt.h - 材質の拡張シェーディング（クリアコート / 異方性 / シーン(布) / スペキュラ・IOR）の
//   CPU 側の唯一の定義。glTF 拡張（KHR_materials_clearcoat / _anisotropy / _sheen / _specular / _ior）と
//   同じ意味で持ち、GPU へは 8 DWORD の「詰めた形」（PackShadingExt）で渡す。
//
//   ★既定値（IsDefault()==true）なら PackShadingExt は 8 個とも 0 を返す＝シェーダは拡張の分岐を丸ごと飛ばし、
//     既存の材質は 1 ビットも変わらない（マルチスキャッタのエネルギー補償は別のスイッチ。こちらとは無関係）。
//   ★詰め方はシェーダ側 shaders/forward/ShadingExt.hlsli の UnoDecodeShadingExt と一対一。片方だけ直さないこと
//     （tests/shading_ext_test.cpp が CPU 側の往復を見張る）。
//   ★同じ 8 DWORD を 4 か所へ置く: フォワードの b0 の自由枠（offset 128..159）/ パストレーサーの PtMaterial /
//     RT の GeometryInfo / 仮想ジオメトリの VgMaterialGpu。
//
//   GPU / DirectX に依存しない純データ（単体テスト可能）。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/Types.h"

namespace dx12e
{

struct ShadingExt
{
    // ---- KHR_materials_clearcoat ----
    f32 clearCoat          = 0.0f;   // 0..1。コート層の強さ（0 = 無し）
    f32 clearCoatRoughness = 0.0f;   // 0..1（知覚的ラフネス。α = r^2）
    // ---- KHR_materials_anisotropy ----
    f32 anisotropy         = 0.0f;   // -1..1（glTF は 0..1。負は従法線方向に伸ばす）
    f32 anisotropyRotation = 0.0f;   // ラジアン。接線（UV の +U 方向）から接空間で反時計回り
    // ---- KHR_materials_sheen ----
    f32 sheenColor[3]      = {0.0f, 0.0f, 0.0f};   // リニア 0..1（0 = 無し）
    f32 sheenRoughness     = 0.0f;   // 0..1
    // ---- KHR_materials_specular / KHR_materials_ior（誘電体の F0 / F90）----
    f32 specularFactor     = 1.0f;   // 0..1（F90 と F0 の両方に掛かる）
    f32 specularColor[3]   = {1.0f, 1.0f, 1.0f};   // リニア。F0 に掛かる（glTF は 1 超も可）
    f32 ior                = 1.5f;   // 1.0..（1.5 → F0 = 0.04）

    bool HasClearCoat()  const { return clearCoat > 0.0f; }
    bool HasAnisotropy() const { return anisotropy != 0.0f; }
    bool HasSheen()      const { return sheenColor[0] > 0.0f || sheenColor[1] > 0.0f || sheenColor[2] > 0.0f; }
    bool HasSpecular()   const
    {
        return specularFactor != 1.0f || ior != 1.5f
            || specularColor[0] != 1.0f || specularColor[1] != 1.0f || specularColor[2] != 1.0f;
    }
    bool IsDefault() const { return !HasClearCoat() && !HasAnisotropy() && !HasSheen() && !HasSpecular(); }

    bool operator==(const ShadingExt& o) const
    {
        return clearCoat == o.clearCoat && clearCoatRoughness == o.clearCoatRoughness
            && anisotropy == o.anisotropy && anisotropyRotation == o.anisotropyRotation
            && sheenColor[0] == o.sheenColor[0] && sheenColor[1] == o.sheenColor[1] && sheenColor[2] == o.sheenColor[2]
            && sheenRoughness == o.sheenRoughness && specularFactor == o.specularFactor
            && specularColor[0] == o.specularColor[0] && specularColor[1] == o.specularColor[1]
            && specularColor[2] == o.specularColor[2] && ior == o.ior;
    }
    bool operator!=(const ShadingExt& o) const { return !(*this == o); }
};

// エンティティ単位の上書き（MeshRenderer）。各欄「負 = 材質に従う」（metallic / roughness の上書きと同じ流儀）。
//   anisotropy は -1..1 の値域なので、従うかどうかは anisotropySet で持つ。
//   sheenColor / specularColor は [0] < 0 で従う。
struct ShadingExtOverride
{
    f32  clearCoat          = -1.0f;
    f32  clearCoatRoughness = -1.0f;
    bool anisotropySet      = false;
    f32  anisotropy         = 0.0f;
    f32  anisotropyRotation = -1.0f;   // <0 = 従う（値は 0..2π に丸めて保存する）
    f32  sheenColor[3]      = {-1.0f, -1.0f, -1.0f};
    f32  sheenRoughness     = -1.0f;
    f32  specularFactor     = -1.0f;
    f32  specularColor[3]   = {-1.0f, -1.0f, -1.0f};
    f32  ior                = -1.0f;

    bool Any() const
    {
        return clearCoat >= 0.0f || clearCoatRoughness >= 0.0f || anisotropySet || anisotropyRotation >= 0.0f
            || sheenColor[0] >= 0.0f || sheenRoughness >= 0.0f || specularFactor >= 0.0f
            || specularColor[0] >= 0.0f || ior >= 0.0f;
    }
};

// 上書きの全欄のビットを 1 本の 64bit に畳む（インスタンシングのバッチ鍵・RT の更新検知用。値が違えば別の鍵）
inline u64 HashShadingExtOverride(const ShadingExtOverride& ov)
{
    u64 k = 1469598103934665603ull;
    auto mixF = [&k](f32 f) { u32 u; std::memcpy(&u, &f, 4); k ^= u; k *= 1099511628211ull; };
    mixF(ov.clearCoat); mixF(ov.clearCoatRoughness);
    mixF(ov.anisotropySet ? 1.0f : 0.0f); mixF(ov.anisotropy); mixF(ov.anisotropyRotation);
    mixF(ov.sheenColor[0]); mixF(ov.sheenColor[1]); mixF(ov.sheenColor[2]); mixF(ov.sheenRoughness);
    mixF(ov.specularFactor); mixF(ov.specularColor[0]); mixF(ov.specularColor[1]); mixF(ov.specularColor[2]);
    mixF(ov.ior);
    return k;
}

inline ShadingExt ResolveShadingExt(const ShadingExt& base, const ShadingExtOverride& ov)
{
    ShadingExt e = base;
    if (ov.clearCoat          >= 0.0f) e.clearCoat          = ov.clearCoat;
    if (ov.clearCoatRoughness >= 0.0f) e.clearCoatRoughness = ov.clearCoatRoughness;
    if (ov.anisotropySet)              e.anisotropy         = ov.anisotropy;
    if (ov.anisotropyRotation >= 0.0f) e.anisotropyRotation = ov.anisotropyRotation;
    if (ov.sheenColor[0]      >= 0.0f) { e.sheenColor[0] = ov.sheenColor[0]; e.sheenColor[1] = ov.sheenColor[1]; e.sheenColor[2] = ov.sheenColor[2]; }
    if (ov.sheenRoughness     >= 0.0f) e.sheenRoughness     = ov.sheenRoughness;
    if (ov.specularFactor     >= 0.0f) e.specularFactor     = ov.specularFactor;
    if (ov.specularColor[0]   >= 0.0f) { e.specularColor[0] = ov.specularColor[0]; e.specularColor[1] = ov.specularColor[1]; e.specularColor[2] = ov.specularColor[2]; }
    if (ov.ior                >= 0.0f) e.ior                = ov.ior;
    return e;
}

// 値域へ丸める（読み込み・MCP・Inspector の入口で共通に使う）
inline void ClampShadingExt(ShadingExt& e)
{
    auto c01 = [](f32 v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
    e.clearCoat          = c01(e.clearCoat);
    e.clearCoatRoughness = c01(e.clearCoatRoughness);
    e.anisotropy         = std::isfinite(e.anisotropy) ? std::clamp(e.anisotropy, -1.0f, 1.0f) : 0.0f;
    if (!std::isfinite(e.anisotropyRotation)) e.anisotropyRotation = 0.0f;
    const f32 twoPi = 6.28318530718f;
    e.anisotropyRotation = std::fmod(e.anisotropyRotation, twoPi);
    if (e.anisotropyRotation < 0.0f) e.anisotropyRotation += twoPi;
    for (f32& v : e.sheenColor) v = c01(v);
    e.sheenRoughness = c01(e.sheenRoughness);
    e.specularFactor = c01(e.specularFactor);
    for (f32& v : e.specularColor) v = std::isfinite(v) ? std::clamp(v, 0.0f, 16.0f) : 1.0f;
    e.ior = std::isfinite(e.ior) ? std::clamp(e.ior, 1.0f, 5.0f) : 1.5f;
}

// 誘電体の F0（glTF KHR_materials_specular / _ior の式）: min(((ior-1)/(ior+1))^2 * specularColor, 1) * specularFactor
inline f32 IorToF0(f32 ior)
{
    const f32 r = (ior - 1.0f) / (ior + 1.0f);
    return r * r;
}
inline void DielectricF0(const ShadingExt& e, f32 outF0[3], f32& outF90)
{
    const f32 f0 = IorToF0(e.ior);
    for (int i = 0; i < 3; ++i) outF0[i] = std::min(f0 * e.specularColor[i], 1.0f) * e.specularFactor;
    outF90 = e.specularFactor;
}

// ---- GPU へ渡す 8 DWORD ------------------------------------------------------------------------
//   w0 = 0xA5000000 | 機能ビット（bit0 コート / bit1 異方性 / bit2 シーン / bit3 スペキュラ）。全部 0 なら w0 = 0（＝無し）
//   w1 = コート強さ unorm16 (bit0..15) | コート粗さ unorm16 (bit16..31)
//   w2 = 異方性の強さ snorm16 (bit0..15) | 回転 unorm16 (bit16..31、0..65535 → 0..2π)
//   w3 = シーン色 RGB888 (R=bit16..23, G=bit8..15, B=bit0..7) | シーン粗さ unorm8 (bit24..31)
//   w4..w6 = 誘電体 F0 の RGB（float。bit3 のときだけ意味を持つ）
//   w7 = F90（float。bit3 のときだけ）
// ★w0 の上位 8bit を固定値（0xA5）にしてあるのは、フォワードの b0 の自由枠（カスタムシェーダの名前付きパラメーター
//   と共用の 8 float）に作者の値が残っていても「拡張あり」と誤読しないため（float の 0xA5xxxxxx は ≒ -1e-16 前後で、
//   作者が書く値とまず一致しない）。描画側は既定シェーダのドローで必ずこの 8 DWORD を書き直す。
constexpr u32 kShadingExtMagic    = 0xA5000000u;
constexpr u32 kShadingExtMagicMask = 0xFF000000u;
constexpr u32 kShadingExtCoat     = 1u;
constexpr u32 kShadingExtAniso    = 2u;
constexpr u32 kShadingExtSheen    = 4u;
constexpr u32 kShadingExtSpecular = 8u;

using PackedShadingExt = std::array<u32, 8>;

inline u32 ShadingExtQuantU16(f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); }
inline u32 ShadingExtQuantU8(f32 v)  { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
inline u32 ShadingExtQuantS16(f32 v)
{
    const int q = static_cast<int>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    return static_cast<u32>(static_cast<u16>(static_cast<int16_t>(q)));
}

inline PackedShadingExt PackShadingExt(const ShadingExt& in)
{
    PackedShadingExt w{};   // 全部 0
    ShadingExt e = in;
    ClampShadingExt(e);
    u32 flags = 0;
    if (e.HasClearCoat() && ShadingExtQuantU16(e.clearCoat) > 0u)
    {
        flags |= kShadingExtCoat;
        w[1] = ShadingExtQuantU16(e.clearCoat) | (ShadingExtQuantU16(e.clearCoatRoughness) << 16);
    }
    if (e.HasAnisotropy() && ShadingExtQuantS16(e.anisotropy) != 0u)
    {
        flags |= kShadingExtAniso;
        const f32 rot01 = e.anisotropyRotation / 6.28318530718f;
        w[2] = ShadingExtQuantS16(e.anisotropy) | ((static_cast<u32>(std::clamp(rot01, 0.0f, 1.0f) * 65535.0f + 0.5f) & 0xFFFFu) << 16);
    }
    if (e.HasSheen())
    {
        const u32 rgb = (ShadingExtQuantU8(e.sheenColor[0]) << 16) | (ShadingExtQuantU8(e.sheenColor[1]) << 8)
                      | ShadingExtQuantU8(e.sheenColor[2]);
        if (rgb != 0u)
        {
            flags |= kShadingExtSheen;
            w[3] = rgb | (ShadingExtQuantU8(e.sheenRoughness) << 24);
        }
    }
    if (e.HasSpecular())
    {
        f32 f0[3], f90;
        DielectricF0(e, f0, f90);
        flags |= kShadingExtSpecular;
        std::memcpy(&w[4], &f0[0], 4);
        std::memcpy(&w[5], &f0[1], 4);
        std::memcpy(&w[6], &f0[2], 4);
        std::memcpy(&w[7], &f90, 4);
    }
    if (flags == 0u) return PackedShadingExt{};
    w[0] = kShadingExtMagic | flags;
    return w;
}

// シェーダ側と対の復元（テスト用。シェーダの UnoDecodeShadingExt と同じ式）
struct DecodedShadingExt
{
    u32 flags = 0;
    f32 clearCoat = 0, clearCoatRoughness = 0, anisotropy = 0, anisotropyRotation = 0;
    f32 sheenColor[3] = {0, 0, 0};
    f32 sheenRoughness = 0;
    f32 f0[3] = {0.04f, 0.04f, 0.04f};
    f32 f90 = 1.0f;
};
inline DecodedShadingExt UnpackShadingExt(const PackedShadingExt& w)
{
    DecodedShadingExt d;
    if ((w[0] & kShadingExtMagicMask) != kShadingExtMagic) return d;
    d.flags = w[0] & 0xFFu;
    if (d.flags & kShadingExtCoat)
    {
        d.clearCoat          = static_cast<f32>(w[1] & 0xFFFFu) / 65535.0f;
        d.clearCoatRoughness = static_cast<f32>(w[1] >> 16) / 65535.0f;
    }
    if (d.flags & kShadingExtAniso)
    {
        const int16_t s = static_cast<int16_t>(static_cast<u16>(w[2] & 0xFFFFu));
        d.anisotropy         = std::max(static_cast<f32>(s) / 32767.0f, -1.0f);
        d.anisotropyRotation = static_cast<f32>(w[2] >> 16) / 65535.0f * 6.28318530718f;
    }
    if (d.flags & kShadingExtSheen)
    {
        d.sheenColor[0]  = static_cast<f32>((w[3] >> 16) & 0xFFu) / 255.0f;
        d.sheenColor[1]  = static_cast<f32>((w[3] >>  8) & 0xFFu) / 255.0f;
        d.sheenColor[2]  = static_cast<f32>( w[3]        & 0xFFu) / 255.0f;
        d.sheenRoughness = static_cast<f32>( w[3] >> 24)          / 255.0f;
    }
    if (d.flags & kShadingExtSpecular)
    {
        std::memcpy(&d.f0[0], &w[4], 4);
        std::memcpy(&d.f0[1], &w[5], 4);
        std::memcpy(&d.f0[2], &w[6], 4);
        std::memcpy(&d.f90,   &w[7], 4);
    }
    return d;
}

} // namespace dx12e
