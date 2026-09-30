#pragma once

// ===== 生成 HLSL の簡易シンタックス色分け（純ロジック。ImGui 非依存）=====
// マテリアルグラフ窓の「HLSL ビューア」用。字句だけ（構文は見ない）。1 行ずつ処理し、/* */ の途中状態は呼び出し側が持つ。

#include <cstdint>
#include <string_view>
#include <vector>

namespace dx12e::mg
{

enum class HlslTok : uint8_t
{
    Plain,     // 識別子・記号
    Keyword,   // return / if / void / out ...
    Type,      // float3 / Texture2D ...
    Intrinsic, // lerp / saturate / pow ...
    Engine,    // MG_* / Uno* / UNO_*（生成コードが使うエンジン側の名前）
    Number,
    Comment,
    Preproc,   // #include / #line
    Punct,
};

struct HlslSpan
{
    uint32_t begin = 0, end = 0;   // バイト位置 [begin, end)
    HlslTok  kind = HlslTok::Plain;
};

// 1 行を色分けする。inBlockComment: 前の行から続く /* ... */ の途中か（更新して返す）。span は begin 昇順で隙間なく行全体を覆う。
std::vector<HlslSpan> ColorizeHlslLine(std::string_view line, bool* inBlockComment);

// `#line N "node:<id>"` の行なら node id を返す（それ以外は空）。
std::string_view NodeIdOfLineDirective(std::string_view line);

} // namespace dx12e::mg
