#include "resource/GltfShadingExt.h"

#include <assimp/material.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>

namespace dx12e
{

namespace
{
using json = nlohmann::json;

const json* Find(const json& obj, const char* key)
{
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return (it == obj.end()) ? nullptr : &*it;
}

void ReadNum(const json& obj, const char* key, f32& out)
{
    if (const json* v = Find(obj, key); v && v->is_number()) out = v->get<f32>();
}

void ReadVec3(const json& obj, const char* key, f32 (&out)[3])
{
    const json* v = Find(obj, key);
    if (!v || !v->is_array() || v->size() < 3) return;
    for (size_t i = 0; i < 3; ++i)
        if ((*v)[i].is_number()) out[i] = (*v)[i].get<f32>();
}
} // namespace

std::vector<GltfShadingEntry> ParseGltfShading(const uint8_t* data, size_t size)
{
    std::vector<GltfShadingEntry> out;
    if (!data || size < 4) return out;

    const char* text = nullptr;
    size_t      len  = 0;
    if (std::memcmp(data, "glTF", 4) == 0)
    {
        // glb: 12B ヘッダ（magic, version, length）の後にチャンク（length, type, data）が並ぶ。最初が JSON（0x4E4F534A）
        size_t pos = 12;
        while (pos + 8 <= size)
        {
            uint32_t clen = 0, ctype = 0;
            std::memcpy(&clen, data + pos, 4);
            std::memcpy(&ctype, data + pos + 4, 4);
            pos += 8;
            if (clen > size - pos) return out;
            if (ctype == 0x4E4F534Au) { text = reinterpret_cast<const char*>(data + pos); len = clen; break; }
            pos += clen;
        }
        if (!text) return out;
    }
    else
    {
        text = reinterpret_cast<const char*>(data);
        len  = size;
    }

    const json root = json::parse(text, text + len, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object()) return out;
    const json* mats = Find(root, "materials");
    if (!mats || !mats->is_array()) return out;

    out.resize(mats->size());
    for (size_t i = 0; i < mats->size(); ++i)
    {
        const json* ext = Find((*mats)[i], "extensions");
        if (!ext || !ext->is_object()) continue;
        GltfShadingEntry& e = out[i];
        ShadingExt& s = e.shading;

        if (const json* c = Find(*ext, "KHR_materials_clearcoat"))
        {
            e.any = true;
            ReadNum(*c, "clearcoatFactor", s.clearCoat);
            ReadNum(*c, "clearcoatRoughnessFactor", s.clearCoatRoughness);
        }
        if (const json* a = Find(*ext, "KHR_materials_anisotropy"))
        {
            e.any = true;
            ReadNum(*a, "anisotropyStrength", s.anisotropy);
            ReadNum(*a, "anisotropyRotation", s.anisotropyRotation);
        }
        if (const json* sh = Find(*ext, "KHR_materials_sheen"))
        {
            e.any = true;
            ReadVec3(*sh, "sheenColorFactor", s.sheenColor);
            ReadNum(*sh, "sheenRoughnessFactor", s.sheenRoughness);
        }
        if (const json* sp = Find(*ext, "KHR_materials_specular"))
        {
            e.any = true;
            ReadNum(*sp, "specularFactor", s.specularFactor);
            ReadVec3(*sp, "specularColorFactor", s.specularColor);
        }
        if (const json* io = Find(*ext, "KHR_materials_ior"))
        {
            e.any = true;
            ReadNum(*io, "ior", s.ior);
        }
        if (e.any) ClampShadingExt(s);
    }
    return out;
}

bool ReadAssimpShading(const aiMaterial* mat, ShadingExt& out)
{
    if (!mat) return false;
    ShadingExt s;
    bool any = false;

    float f = 0.0f;
    if (mat->Get(AI_MATKEY_CLEARCOAT_FACTOR, f) == AI_SUCCESS)
    {
        any = true;
        s.clearCoat = f;
        if (mat->Get(AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, f) == AI_SUCCESS) s.clearCoatRoughness = f;
    }
    aiColor3D c(0, 0, 0);
    if (mat->Get(AI_MATKEY_SHEEN_COLOR_FACTOR, c) == AI_SUCCESS)
    {
        any = true;
        s.sheenColor[0] = c.r; s.sheenColor[1] = c.g; s.sheenColor[2] = c.b;
        if (mat->Get(AI_MATKEY_SHEEN_ROUGHNESS_FACTOR, f) == AI_SUCCESS) s.sheenRoughness = f;
    }
    // KHR_materials_specular だけが AI_MATKEY_SPECULAR_FACTOR を書く（spec-gloss は GLOSSINESS_FACTOR）
    if (mat->Get(AI_MATKEY_SPECULAR_FACTOR, f) == AI_SUCCESS)
    {
        any = true;
        s.specularFactor = f;
        aiColor3D sc(1, 1, 1);
        if (mat->Get(AI_MATKEY_COLOR_SPECULAR, sc) == AI_SUCCESS)
        { s.specularColor[0] = sc.r; s.specularColor[1] = sc.g; s.specularColor[2] = sc.b; }
    }
    if (mat->Get(AI_MATKEY_REFRACTI, f) == AI_SUCCESS)
    {
        any = true;
        s.ior = f;
    }
    if (mat->Get(AI_MATKEY_ANISOTROPY_FACTOR, f) == AI_SUCCESS)   // Assimp 5.x の glTF importer は書かない（将来の版・他形式用）
    {
        any = true;
        s.anisotropy = f;
    }
    if (!any) return false;
    ClampShadingExt(s);
    out = s;
    return true;
}

} // namespace dx12e
