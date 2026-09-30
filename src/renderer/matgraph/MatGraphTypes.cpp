#include "renderer/matgraph/MatGraphTypes.h"

#include <charconv>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace dx12e::matgraph
{

const char* TypeName(ValueType t)
{
    switch (t)
    {
    case ValueType::F1:      return "float";
    case ValueType::F2:      return "float2";
    case ValueType::F3:      return "float3";
    case ValueType::F4:      return "float4";
    case ValueType::Int:     return "int";
    case ValueType::Bool:    return "bool";
    case ValueType::Tex2D:   return "texture2D";
    case ValueType::Sampler: return "sampler";
    case ValueType::Mat3:    return "float3x3";
    case ValueType::Mat4:    return "float4x4";
    default:                 return "invalid";
    }
}

const char* HlslTypeName(ValueType t)
{
    switch (t)
    {
    case ValueType::Tex2D:   return "Texture2D<float4>";
    case ValueType::Sampler: return "SamplerState";
    default:                 return TypeName(t);
    }
}

bool ParseTypeName(const std::string& s, ValueType& out)
{
    static const ValueType all[] = {
        ValueType::F1, ValueType::F2, ValueType::F3, ValueType::F4, ValueType::Int, ValueType::Bool,
        ValueType::Tex2D, ValueType::Sampler, ValueType::Mat3, ValueType::Mat4};
    for (ValueType t : all)
        if (s == TypeName(t)) { out = t; return true; }
    if (s == "float1") { out = ValueType::F1; return true; }
    return false;
}

int Dim(ValueType t)
{
    switch (t)
    {
    case ValueType::F1: case ValueType::Int: case ValueType::Bool: return 1;
    case ValueType::F2: return 2;
    case ValueType::F3: return 3;
    case ValueType::F4: return 4;
    default:            return 0;
    }
}

bool IsFloatVec(ValueType t) { return t >= ValueType::F1 && t <= ValueType::F4; }
bool IsNumeric(ValueType t)  { return IsFloatVec(t) || t == ValueType::Int || t == ValueType::Bool; }

ValueType FloatType(int dim)
{
    switch (dim)
    {
    case 1: return ValueType::F1;
    case 2: return ValueType::F2;
    case 3: return ValueType::F3;
    case 4: return ValueType::F4;
    default: return ValueType::Invalid;
    }
}

CastResult CheckCast(ValueType from, ValueType to)
{
    CastResult r;
    auto fail = [&](CastKind k, std::string why) { r.ok = false; r.kind = k; r.reason = std::move(why); return r; };

    if (from == ValueType::Invalid || to == ValueType::Invalid)
        return fail(CastKind::Forbidden, "型が決まっていません");

    if (from == to)
    {
        r.ok = true; r.kind = CastKind::Identity;
        return r;
    }

    const bool fromObj = (from == ValueType::Tex2D || from == ValueType::Sampler || from == ValueType::Mat3 || from == ValueType::Mat4);
    const bool toObj   = (to   == ValueType::Tex2D || to   == ValueType::Sampler || to   == ValueType::Mat3 || to   == ValueType::Mat4);
    if (fromObj || toObj)
    {
        if (from == ValueType::Tex2D && IsNumeric(to))
            return fail(CastKind::Forbidden, "テクスチャを float に繋げません。TextureSample を通してください");
        if (to == ValueType::Tex2D && IsNumeric(from))
            return fail(CastKind::Forbidden, std::string(TypeName(from)) + " を texture2D に繋げません。TextureParameter を使ってください");
        return fail(CastKind::Forbidden, std::string(TypeName(from)) + " を " + TypeName(to) + " に変換できません");
    }

    // ここから先は数値（F1..F4 / Int / Bool）どうし
    if (IsFloatVec(from) && IsFloatVec(to))
    {
        const int n = Dim(from), m = Dim(to);
        if (n == 1) { r.ok = true; r.kind = CastKind::Splat; return r; }
        if (m < n)
        {
            r.ok = true; r.kind = CastKind::Truncate; r.warn = true;
            r.reason = std::string(TypeName(from)) + " を " + TypeName(to) + " に切り詰めます";
            return r;
        }
        return fail(CastKind::Forbidden, std::string(TypeName(from)) + " を " + TypeName(to) + " に拡張できません。Append を使ってください");
    }
    if (from == ValueType::Int && IsFloatVec(to)) { r.ok = true; r.kind = CastKind::IntToFloat;  return r; }
    if (from == ValueType::Bool && IsFloatVec(to)) { r.ok = true; r.kind = CastKind::BoolToFloat; return r; }
    if (from == ValueType::Bool && to == ValueType::Int) { r.ok = true; r.kind = CastKind::BoolToInt; return r; }

    if (IsFloatVec(from) && to == ValueType::Int)
        return fail(CastKind::Forbidden, std::string(TypeName(from)) + " を int に暗黙変換できません（整数が必要な入力です）");
    if ((IsFloatVec(from) || from == ValueType::Int) && to == ValueType::Bool)
        return fail(CastKind::Forbidden, std::string(TypeName(from)) + " を bool に暗黙変換できません。Compare ノードで真偽値にしてください");
    return fail(CastKind::Forbidden, std::string(TypeName(from)) + " を " + TypeName(to) + " に変換できません");
}

bool Value::operator==(const Value& o) const
{
    if (type != o.type) return false;
    switch (type)
    {
    case ValueType::Int:  return i == o.i;
    case ValueType::Bool: return b == o.b;
    default:
        for (int k = 0; k < Dim(type); ++k)
            if (f[k] != o.f[k]) return false;
        return true;
    }
}

std::string FormatFloatShort(float v)
{
    if (std::isnan(v)) v = 0.0f;
    if (std::isinf(v)) v = v > 0 ? FLT_MAX : -FLT_MAX;
    if (v == 0.0f) v = 0.0f;   // -0 を 0 に
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

std::string FormatHlslFloat(float v)
{
    std::string s = FormatFloatShort(v);
    if (s.find_first_of(".eE") == std::string::npos)
        s += ".0";
    else if (s.find('.') == std::string::npos)
    {
        // "1e-05" のような指数だけの表記は HLSL では OK だが、読みやすさのため "1.0e-05" へ
        const size_t e = s.find_first_of("eE");
        s.insert(e, ".0");
    }
    if (!s.empty() && s[0] == '-')
        s = "(" + s + ")";
    return s;
}

std::string FormatHlslValue(const Value& v)
{
    switch (v.type)
    {
    case ValueType::Int:  return std::to_string(v.i);
    case ValueType::Bool: return v.b ? "true" : "false";
    case ValueType::F1:   return FormatHlslFloat(v.f[0]);
    case ValueType::F2: case ValueType::F3: case ValueType::F4:
    {
        std::string s = TypeName(v.type);
        s += "(";
        for (int k = 0; k < Dim(v.type); ++k)
        {
            if (k) s += ", ";
            s += FormatHlslFloat(v.f[k]);
        }
        s += ")";
        return s;
    }
    default: return "0";
    }
}

} // namespace dx12e::matgraph
