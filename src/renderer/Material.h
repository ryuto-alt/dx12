#pragma once

#include "core/Types.h"

namespace dx12e
{

class Texture;

struct Material
{
    Texture* albedoTexture         = nullptr;
    Texture* normalMapTexture      = nullptr;  // PBR: 法線マップ
    Texture* metalRoughnessTexture = nullptr;  // PBR: R=unused, G=roughness, B=metallic

    float defaultMetallic  = 1.0f;   // スケーリングファクター（1.0=テクスチャ値そのまま）
    float defaultRoughness = 1.0f;

    u32 srvBlockIndex = 0xFFFFFFFF;  // SRVヒープ上の連続3スロットの先頭
};

} // namespace dx12e
