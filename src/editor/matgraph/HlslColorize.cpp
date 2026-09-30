#include "editor/matgraph/HlslColorize.h"

#include <cctype>
#include <cstring>

namespace dx12e::mg
{

namespace
{
bool IsIdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool In(std::string_view w, const char* const* list, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (w == list[i]) return true;
    return false;
}

const char* const kKeywords[] = {"return", "if", "else", "for", "while", "do", "break", "continue", "discard", "void", "struct", "static", "const",
                                 "in", "out", "inout", "true", "false", "uniform", "switch", "case", "default", "cbuffer"};
const char* const kTypes[] = {"float", "float2", "float3", "float4", "int", "int2", "int3", "int4", "uint", "uint2", "uint3", "uint4", "bool",
                              "half", "half2", "half3", "half4", "float3x3", "float4x4", "float2x2", "Texture2D", "SamplerState", "Texture2DArray"};
const char* const kIntrinsics[] = {"lerp", "saturate", "pow", "dot", "normalize", "sin", "cos", "tan", "frac", "floor", "ceil", "sqrt", "abs", "min",
                                   "max", "clamp", "smoothstep", "step", "cross", "length", "mul", "exp", "exp2", "log", "log2", "sign", "reflect",
                                   "asuint", "asfloat", "ddx", "ddy", "fmod", "rsqrt", "round", "saturate", "distance", "transpose"};
} // namespace

std::vector<HlslSpan> ColorizeHlslLine(std::string_view s, bool* inBlock)
{
    std::vector<HlslSpan> out;
    const uint32_t n = static_cast<uint32_t>(s.size());
    auto push = [&](uint32_t b, uint32_t e, HlslTok k)
    {
        if (e <= b) return;
        if (!out.empty() && out.back().kind == k && out.back().end == b) out.back().end = e;
        else out.push_back(HlslSpan{b, e, k});
    };
    uint32_t i = 0;
    bool block = inBlock ? *inBlock : false;

    // 前の行から続くブロックコメント
    if (block)
    {
        const size_t close = s.find("*/");
        if (close == std::string_view::npos) { push(0, n, HlslTok::Comment); return out; }
        push(0, static_cast<uint32_t>(close) + 2, HlslTok::Comment);
        i = static_cast<uint32_t>(close) + 2;
        block = false;
    }
    // 行頭の # はプリプロセッサ行（行全体）
    {
        uint32_t k = i;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
        if (k < n && s[k] == '#')
        {
            push(i, n, HlslTok::Preproc);
            if (inBlock) *inBlock = false;
            return out;
        }
    }
    while (i < n)
    {
        const char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { push(i, n, HlslTok::Comment); i = n; break; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*')
        {
            const size_t close = s.find("*/", i + 2);
            if (close == std::string_view::npos) { push(i, n, HlslTok::Comment); block = true; i = n; break; }
            push(i, static_cast<uint32_t>(close) + 2, HlslTok::Comment);
            i = static_cast<uint32_t>(close) + 2;
            continue;
        }
        if (IsIdStart(c))
        {
            uint32_t j = i + 1;
            while (j < n && IsIdChar(s[j])) ++j;
            const std::string_view w = s.substr(i, j - i);
            HlslTok k = HlslTok::Plain;
            if (In(w, kKeywords, sizeof(kKeywords) / sizeof(kKeywords[0]))) k = HlslTok::Keyword;
            else if (In(w, kTypes, sizeof(kTypes) / sizeof(kTypes[0]))) k = HlslTok::Type;
            else if (In(w, kIntrinsics, sizeof(kIntrinsics) / sizeof(kIntrinsics[0]))) k = HlslTok::Intrinsic;
            else if (w.rfind("MG_", 0) == 0 || w.rfind("Uno", 0) == 0 || w.rfind("UNO_", 0) == 0) k = HlslTok::Engine;
            push(i, j, k);
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1]))))
        {
            uint32_t j = i + 1;
            while (j < n)
            {
                const char d = s[j];
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '.') { ++j; continue; }
                if ((d == '+' || d == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E')) { ++j; continue; }
                break;
            }
            push(i, j, HlslTok::Number);
            i = j;
            continue;
        }
        if (c == ' ' || c == '\t') { push(i, i + 1, HlslTok::Plain); ++i; continue; }
        push(i, i + 1, HlslTok::Punct);
        ++i;
    }
    if (inBlock) *inBlock = block;
    return out;
}

std::string_view NodeIdOfLineDirective(std::string_view line)
{
    if (line.rfind("#line", 0) != 0) return {};
    const size_t q = line.find("\"node:");
    if (q == std::string_view::npos) return {};
    const size_t b = q + 6;
    const size_t e = line.find('"', b);
    if (e == std::string_view::npos) return {};
    return line.substr(b, e - b);
}

} // namespace dx12e::mg
