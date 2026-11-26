#pragma once
// パーティクルエディタの「適用」の純ロジック（W1-A15 / BUG-21 の暫定策）。GPU / ImGui に依存しない。
// 旧 ParticleLayer のデータ形式は変えない（コピー先の項目も従来のまま）。

#include <string>
#include <DirectXMath.h>

#include "core/Types.h"
#include "ecs/Components.h"

namespace dx12e
{

// 名前付き VFX プリセット（assets/vfx/*.json）。ParticleEmitter コンポーネントの
// フィールドをほぼそのまま保持しつつ、Lua fx:burst{} 生成用のフィールド（burstCount）も
// 併せ持つ上位互換のデータ形式。配置エンティティへの「適用」と Lua コード生成の両方に使える。
struct VfxAsset
{
    std::string name = "NewEffect";

    int  kind  = 1;   // ParticleKind（既定 = Fire）
    int  blend = 0;   // ParticleBlend
    int  orient = 0;  // 粒子の向き 0=ビルボード 1=水平(地面) 2=垂直(+Z)

    // 配置エンティティ（継続放出）用
    f32  rate        = 30.0f;
    bool playOnStart  = true;
    bool looping      = true;
    f32  duration     = 1.0f;
    // Lua fx:burst{} 生成用（一度に発生させる粒子数）
    int  burstCount   = 24;

    DirectX::XMFLOAT3 dir{0.0f, 1.0f, 0.0f};
    f32  spread   = 0.4f;
    f32  speed    = 3.0f;
    f32  speedVar = 0.4f;

    f32  size    = 0.3f;
    f32  sizeMid = -1.0f;   // >=0 で3キーサイズカーブ
    f32  sizeEnd = 0.0f;
    f32  life    = 0.8f;
    f32  lifeVar = 0.3f;

    DirectX::XMFLOAT3 color{1.0f, 0.6f, 0.2f};
    DirectX::XMFLOAT3 colorMid{1.0f, 0.6f, 0.2f};
    DirectX::XMFLOAT3 colorEnd{1.0f, 0.12f, 0.05f};
    bool hasColorMid = false;
    f32  intensity   = 3.0f;

    f32  gravity = 0.0f;
    f32  drag    = 1.0f;
    f32  up      = 0.0f;
    f32  stretch = 0.0f;

    f32  turbStrength = 0.0f;
    f32  turbFreq     = 1.0f;

    f32  flicker     = 0.0f;
    f32  flickerFreq = 18.0f;

    f32  distort    = 0.0f;
    bool light      = false;
    f32  lightRange = 3.0f;
    std::string texturePath;   // assets 相対パス。空ならプロシージャル質感(kind依存)
};

namespace vfxapply
{

// 「置き換え」で上書きされる 1 枚目に、既存の内容（既定値と違う）があるか。
// レイヤーが無い（空の放出器）または既定値のままなら失うものが無いので確認は要らない。
inline bool ReplaceNeedsConfirm(const ParticleEmitter& em)
{
    if (em.layers.empty()) return false;
    return em.layers[0] != ParticleLayer{};
}

// VfxAsset の中身を 1 枚のレイヤーへ写す（gpu / offset / shaderPath / name は触らない。従来どおり）。
inline void CopyAssetToLayer(ParticleLayer& t, const VfxAsset& a)
{
    t.kind = a.kind;   t.blend = a.blend;
    t.orient = a.orient;
    t.rate = a.rate;   t.playOnStart = a.playOnStart;
    t.looping = a.looping; t.duration = a.duration;
    t.dir = a.dir;     t.spread = a.spread;
    t.speed = a.speed; t.speedVar = a.speedVar;
    t.size = a.size;   t.sizeEnd = a.sizeEnd; t.sizeMid = a.sizeMid;
    t.life = a.life;   t.lifeVar = a.lifeVar;
    t.color = a.color; t.colorMid = a.colorMid; t.colorEnd = a.colorEnd;
    t.hasColorMid = a.hasColorMid;
    t.intensity = a.intensity;
    t.gravity = a.gravity; t.drag = a.drag; t.up = a.up;
    t.stretch = a.stretch;
    t.turbStrength = a.turbStrength; t.turbFreq = a.turbFreq;
    t.distort = a.distort;
    t.light = a.light; t.lightRange = a.lightRange;
    t.flicker = a.flicker; t.flickerFreq = a.flickerFreq;
    t.texturePath = a.texturePath;
}

// 適用後の放出器を返す（before は変えない）。
// asNewLayer=false … 1 枚目を置き換える（従来の「適用」）。レイヤーが無ければ 1 枚足す。
// asNewLayer=true  … レイヤーとして足す。
inline ParticleEmitter BuildApplied(const ParticleEmitter& before, const VfxAsset& asset,
                                    const std::string& assetPath, bool asNewLayer)
{
    ParticleEmitter after = before;
    if (asNewLayer || after.layers.empty())
        after.layers.emplace_back();
    ParticleLayer& dst = asNewLayer ? after.layers.back() : after.layers[0];
    // どの VFX アセットから来たかを残す（表示専用の来歴。値はコピー済み）。
    dst.vfxPath = assetPath;
    if (dst.name.empty()) dst.name = asset.name;
    CopyAssetToLayer(dst, asset);
    // gpu はこのエディタの対象外（CPUパーティクルのまま据え置く）
    return after;
}

}  // namespace vfxapply
}  // namespace dx12e
