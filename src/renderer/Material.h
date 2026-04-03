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

    float defaultMetallic  = 0.0f;   // テクスチャなしモデル用
    float defaultRoughness = 0.5f;

    u32 srvBlockIndex = 0xFFFFFFFF;  // SRVヒープ上の連続3スロットの先頭
};

} // namespace dx12e
