#pragma once

// GltfShadingExt.h - glTF の拡張シェーディング系の拡張（KHR_materials_clearcoat / _anisotropy / _sheen / _specular / _ior）を
//   Material::shading（renderer/ShadingExt.h）へ取り込む純関数群。GPU 不要（単体テスト可能）。
//
//   ★2 本立て:
//     1. ReadAssimpShading … Assimp が出す aiMaterial のキーから読む（clearcoat / sheen / specular / ior）。
//        実測と Assimp のソース（glTF2Importer.cpp）で確認した限界:
//          - KHR_materials_anisotropy は **Assimp が読まない**（AI_MATKEY_ANISOTROPY_FACTOR はあるが importer は書かない）。
//          - KHR_materials_specular は「色が (1,1,1) 以外 または factor != 0」のときだけキーが出る
//            ＝ specularFactor = 0 だけの材質（鏡面を消す指定）が落ちる。
//          - sheen は色が (0,0,0) だとキーが出ない（＝無しと同じなので問題なし）。
//     2. ParseGltfShading … glb / gltf の JSON を自分で読む。材質の添字は glTF の materials 配列と Assimp の
//        aiMaterial 番号が一致する（Assimp は既定材質を末尾に足すだけ）。上の限界を全部補う。
//   ModelLoader は glTF/GLB のとき JSON を優先し、無い材質だけ Assimp の値を使う。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "renderer/ShadingExt.h"

struct aiMaterial;

namespace dx12e
{

struct GltfShadingEntry
{
    bool       any = false;   // 5 つの拡張のどれかを持つ材質
    ShadingExt shading;       // any のときだけ意味を持つ（ClampShadingExt 済み）
};

// glb（先頭 "glTF"）または gltf（JSON 本文）のバイト列から、materials 配列の添字順に返す。
// 壊れている / 材質が無い場合は空。
std::vector<GltfShadingEntry> ParseGltfShading(const uint8_t* data, size_t size);

// Assimp の aiMaterial から読む。拡張が何も無ければ false（out は既定のまま）。
bool ReadAssimpShading(const aiMaterial* mat, ShadingExt& out);

} // namespace dx12e
