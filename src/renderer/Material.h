#pragma once

#include <algorithm>

#include "core/Types.h"

namespace dx12e
{

class Texture;

// 透明の扱い（glTF 2.0 の alphaMode と同じ意味論）。
//   Opaque … アルファを完全に無視する（既定。既存シーンは全部これ＝絵は 1 ピクセルも変わらない）
//   Mask   … baseColor.a < cutoff の断片を discard する。葉・枝カード・フェンス・角膜など。
//             ★不透明パスと同じ深度書き込みのまま＝ソート不要・Hi-Z とも喧嘩しない。
//             影パス / 深度プリパスでも同じ discard をしないと「葉の影が板の影」になる。
//   Blend  … 通常のアルファブレンド。不透明の後ろにカメラ距離で後→前に並べて描く。
//             深度書き込み OFF / 深度テスト ON。深度プリパス・速度バッファ・TLAS からは外れる。
enum class AlphaMode : u32
{
    Opaque = 0,
    Mask   = 1,
    Blend  = 2,
};

struct Material
{
    Texture* albedoTexture         = nullptr;
    Texture* normalMapTexture      = nullptr;  // PBR: 法線マップ
    Texture* metalRoughnessTexture = nullptr;  // PBR: R=unused, G=roughness, B=metallic

    float defaultMetallic  = 1.0f;   // スケーリングファクター（1.0=テクスチャ値そのまま）
    float defaultRoughness = 1.0f;

    // ---- 透明（モデルに焼き込まれた値。glTF の alphaMode / alphaCutoff / baseColorFactor.a）----
    // ★既定は Opaque / 1.0。ModelLoader がここを埋めない限り従来と完全に同じ絵になる。
    AlphaMode alphaMode      = AlphaMode::Opaque;
    float     alphaCutoff    = 0.5f;   // Mask のしきい値（glTF 既定 0.5）
    float     baseColorAlpha = 1.0f;   // Blend の基準不透明度（glTF baseColorFactor.a）

    u32 srvBlockIndex = 0xFFFFFFFF;  // SRVヒープ上の連続3スロットの先頭
};

// マテリアル（モデル焼き込み）とエンティティ側オーバーライド（MeshRenderer）を合成した実効値。
struct AlphaParams
{
    AlphaMode mode    = AlphaMode::Opaque;
    f32       cutoff  = 0.5f;
    f32       opacity = 1.0f;
};

// modeOverride  : <0 でマテリアルに従う（MeshRenderer::alphaModeOverride）
// cutoffOverride: <0 でマテリアルに従う（MeshRenderer::alphaCutoffOverride）
// opacityScale  : マテリアルの baseColorAlpha に掛ける係数（MeshRenderer::opacity、既定 1）
inline AlphaParams ResolveAlphaParams(const Material* mat, int modeOverride,
                                      f32 cutoffOverride, f32 opacityScale)
{
    AlphaParams a;
    a.mode = (modeOverride >= 0) ? static_cast<AlphaMode>(static_cast<u32>(modeOverride))
                                 : (mat ? mat->alphaMode : AlphaMode::Opaque);
    a.cutoff = (cutoffOverride >= 0.0f) ? cutoffOverride : (mat ? mat->alphaCutoff : 0.5f);
    a.opacity = (mat ? mat->baseColorAlpha : 1.0f) * opacityScale;
    a.cutoff  = std::clamp(a.cutoff,  0.0f, 1.0f);
    a.opacity = std::clamp(a.opacity, 0.0f, 1.0f);
    return a;
}

// ---- b2（ルート定数 8 DWORD）への詰め方 -------------------------------------
// ★ルート定数の予算は 61/64 で埋まっている（RootSignature.cpp 参照）＝ float を 1 本も足せない。
//   そこで既存フィールドの**空きビット**へ詰める。レイアウトは 1 バイトも変わらないので
//   b0/b2 のバイトオフセットに依存しているカスタムシェーダも地形も一切影響を受けない。
//     pbrFlags   bit2      = アルファテスト有効
//     pbrFlags   bit8..15  = alphaCutoff を 8bit 量子化（0..255 → 0..1）
//     packedTint bit24..31 = opacity を 8bit 量子化（255 = 不透明 = 従来と同じ）
// ★packedTint の上位バイトは従来 0 が入っていた。opacity として読むようになったので
//   **不透明でも必ず 255 を書くこと**（0 のままだと全部消える）。
constexpr u32 kPbrFlagAlphaTest = 4u;

inline u32 QuantizeUnorm8(f32 v)
{
    return static_cast<u32>(static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f));
}

// pbrFlags へアルファテスト情報を焼く（既存のビット 0/1 は呼び出し側で立ててから渡すこと）
inline u32 PackAlphaTestFlags(u32 flags, const AlphaParams& a)
{
    if (a.mode == AlphaMode::Mask) flags |= kPbrFlagAlphaTest;
    return (flags & ~0x0000FF00u) | (QuantizeUnorm8(a.cutoff) << 8);
}

// packedTint（RGB888）へ opacity を足す
inline u32 PackTintWithOpacity(u32 rgb888, f32 opacity)
{
    return (rgb888 & 0x00FFFFFFu) | (QuantizeUnorm8(opacity) << 24);
}

} // namespace dx12e
