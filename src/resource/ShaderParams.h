#pragma once

#include "core/Types.h"

#include <cstddef>
#include <string>
#include <vector>

// ===== カスタムシェーダーの「名前付きパラメーター」 =====
//
// カスタムシェーダー(MeshRenderer::shaderPath)へ渡せる per-object の値は、共有ルート
// シグネチャ [0] の 32bit 定数 40 DWORD のうち **後半 float 8 個**（cbuffer オフセット
// 128..159 = mvp(64) + model(64) の後ろ）だけ。ルート定数の予算が 61/64 DWORD で埋まって
// いるので、ここを増やすことはできない（src/graphics/RootSignature.cpp の予算表を参照）。
//
// 従来この 8 個は `effectValue`(1) / `_pad`(3、常に 0) / `shaderParams`(4) という固定の
// 割り当てで、Inspector にも「エフェクト値」「パラメーター」という汎用ラベルしか出せず、
// どのスロットが何を意味するかは書いた本人しか分からなかった。
//
// そこで DXIL のリフレクションで b0 の変数宣言を読み、この範囲に置かれた変数の
// **名前・型・オフセット** を取り出す。Inspector はそれを見て
//
//     float  _Glow;          // @range(0,4)   → 「_Glow」という名前のスライダー
//     float3 _TintColor;     // @color        → 「_TintColor」というカラーピッカー
//
// のように、シェーダーを書いた本人の言葉でウィジェットを並べられる。値の置き場は
// 従来と同じ MeshRenderer の 8 float なので、ルートシグネチャもシーン JSON の互換性も
// 一切変わらない（オフセットが同じなら既存シェーダーはそのまま動く）。
namespace dx12e::shaderparams
{

// 自由枠のバイト範囲。★RootSignature.cpp [0] の内訳
//   mvp(16 DWORD) + model(16 DWORD) + 自由枠(8 DWORD) = 40 DWORD
// と必ず一致させること。ここを変えるなら向こうも変える。
inline constexpr u32 kFreeBegin  = 128;
inline constexpr u32 kFreeEnd    = 160;
inline constexpr u32 kFreeFloats = (kFreeEnd - kFreeBegin) / 4;   // 8

// 対応するウィジェット。値の実体は必ず float なので float 系のみ扱う
// （int/bool は同じ 4 バイトでもビット列の意味が違い、JSON 往復で壊れるので非対応）。
enum class Kind : u8
{
    Float,
    Float2,
    Float3,
    Float4,
    Color3,       // float3 + 名前が色っぽい or @color
    Color4,       // float4 + 同上
    Unsupported,  // int / bool / matrix / 配列 など。行は出すが編集させない
};

struct Param
{
    std::string name;                 // HLSL 上の変数名。そのまま Inspector のラベルになる
    std::string typeName;             // "float3" 等。Unsupported の説明に使う
    u32         offset   = kFreeBegin; // cbuffer 先頭からのバイトオフセット
    Kind        kind     = Kind::Float;
    bool        hasRange = false;     // @range(min,max) が付いていた
    f32         minV     = 0.0f;
    f32         maxV     = 1.0f;

    // 自由枠先頭からの float 添字(0..7)。MeshRenderer::CustomParamBase() の添字と一致する。
    u32 Index() const { return (offset - kFreeBegin) / 4; }

    // ウィジェットが触る float の個数。
    u32 ComponentCount() const;
};

// vs/ps の DXIL から b0(space0) の cbuffer を読み、自由枠に載っている変数を列挙する。
// hlslSourcePath が読めれば、宣言行の行末コメント `// @range(min,max)` `// @color` も反映する
// （配布ビルドには .hlsl が無いことがあるので、読めなくても失敗にはしない）。
// 返り値: リフレクション自体が成立したら true（変数が 0 個でも true）。
bool Reflect(const void* vs, size_t vsSize, const void* ps, size_t psSize,
             const std::wstring& hlslSourcePath, std::vector<Param>& out);

// --- 置き場（プロセス内グローバル。ShaderManager が積み、Inspector が読む）---
// ShaderManager はデバイスも ImGui も知らず、InspectorPanel は ShaderManager を持っていない
// ので、shaderdiag の Help/Issue ストアと同じ方式で受け渡す。
// キーは shaderdiag::NormalizeKey() と同じ正規化済み relPath。
void               Set(const std::string& key, std::vector<Param> params);
void               Clear(const std::string& key);
std::vector<Param> Get(const std::string& key);

} // namespace dx12e::shaderparams
