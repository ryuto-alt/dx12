// ============================================================================
// MatGraphTypes.h — マテリアルグラフの型システム・値・診断（純ロジック）
//
//   ・標準ライブラリだけに依存する（GPU / D3D / ImGui / nlohmann も不要）。
//   ・型の自動キャスト規則（CheckCast）はここが唯一の正。UI の接続可否判定、
//     型推論、HLSL 生成のすべてがこの関数を通る。
//   ・仕様の説明は docs/MATGRAPH_FORMAT.md「型規則」を参照。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dx12e::matgraph
{

// ---------------------------------------------------------------------------
// 型
// ---------------------------------------------------------------------------
enum class ValueType : uint8_t
{
    Invalid = 0,  // 型が決まらない（上流のエラー / 未接続）
    F1,           // float
    F2,           // float2
    F3,           // float3
    F4,           // float4
    Int,          // int（現行ノードでは定数ノードと将来の整数ピン用）
    Bool,         // bool（コンパイル時定数ではなく実行時の真偽値。Select / Compare 用）
    Tex2D,        // Texture2D<float4>（値ではなく「どの SRV か」）
    Sampler,      // SamplerState（予約。現行ノードのピンには使わない）
    Mat3,         // float3x3（予約）
    Mat4,         // float4x4（予約）
};

const char* TypeName(ValueType t);      // 表示用: "float3" / "texture2D" ...
const char* HlslTypeName(ValueType t);  // HLSL の型名: "float3" / "Texture2D<float4>" ...
bool ParseTypeName(const std::string& s, ValueType& out);  // "float3" 等 → ValueType

// F1..F4 → 1..4、Int / Bool → 1（スカラー扱い）、それ以外 → 0
int  Dim(ValueType t);
bool IsFloatVec(ValueType t);   // F1..F4
bool IsNumeric(ValueType t);    // F1..F4 / Int / Bool（演算の入力になれる）
ValueType FloatType(int dim);   // 1..4 → F1..F4、それ以外 → Invalid

// ---------------------------------------------------------------------------
// 自動キャスト規則
//
//   Identity  : 同じ型
//   Splat     : F1 → F2/F3/F4（スカラー拡張。警告なし）
//   Truncate  : F(n) → F(m)、m < n（先頭 m 成分。警告 W_TRUNCATE）
//   IntToFloat: Int → F1..F4（(floatN)i。警告なし）
//   BoolToFloat / BoolToInt: 1 / 0 へ（警告なし）
//   Forbidden : 上記以外。理由は reason（日本語）に入る
//     ・F(n) → F(m)、1 < n < m  … 拡張は Append を使う
//     ・F → Int / F → Bool / Int → Bool … 暗黙変換しない（Compare / Floor などを使う）
//     ・Tex2D / Sampler / Mat と他の型との間はすべて禁止
// ---------------------------------------------------------------------------
enum class CastKind : uint8_t
{
    Identity,
    Splat,
    Truncate,
    IntToFloat,
    BoolToFloat,
    BoolToInt,
    Forbidden,
};

struct CastResult
{
    bool     ok     = false;
    CastKind kind   = CastKind::Forbidden;
    bool     warn   = false;   // Truncate のとき true
    std::string reason;        // ok=false の理由 / warn=true の警告文（日本語）
};

CastResult CheckCast(ValueType from, ValueType to);

// ---------------------------------------------------------------------------
// リテラル値（未接続ピンに直書きできる値 / 定数の既定値）
// ---------------------------------------------------------------------------
struct Value
{
    ValueType type = ValueType::F1;
    float     f[4] = {0, 0, 0, 0};   // F1..F4
    int32_t   i    = 0;              // Int
    bool      b    = false;          // Bool

    static Value Float(float x)                               { Value v; v.type = ValueType::F1; v.f[0] = x; return v; }
    static Value Float2(float x, float y)                     { Value v; v.type = ValueType::F2; v.f[0] = x; v.f[1] = y; return v; }
    static Value Float3(float x, float y, float z)            { Value v; v.type = ValueType::F3; v.f[0] = x; v.f[1] = y; v.f[2] = z; return v; }
    static Value Float4(float x, float y, float z, float w)   { Value v; v.type = ValueType::F4; v.f[0] = x; v.f[1] = y; v.f[2] = z; v.f[3] = w; return v; }
    static Value Vec(int dim, const float* p)
    {
        Value v; v.type = FloatType(dim < 1 ? 1 : (dim > 4 ? 4 : dim));
        for (int k = 0; k < Dim(v.type); ++k) v.f[k] = p[k];
        return v;
    }
    static Value Int32(int32_t x)  { Value v; v.type = ValueType::Int;  v.i = x; return v; }
    static Value Bool1(bool x)     { Value v; v.type = ValueType::Bool; v.b = x; return v; }

    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// 数値の文字列化（決定論のため、書式はここに一本化）
// ---------------------------------------------------------------------------
// 最短ラウンドトリップ表記（"0.6" / "1e-05"）。NaN / Inf は 0 / ±FLT_MAX に丸める。
std::string FormatFloatShort(float v);
// HLSL リテラル: 必ず小数点か指数を含む（"1.0"）。負数は括弧で囲む（"(-1.0)"）。
std::string FormatHlslFloat(float v);
// HLSL の値リテラル。F1 "0.5" / F3 "float3(1.0, 0.0, 0.0)" / Int "3" / Bool "true"
std::string FormatHlslValue(const Value& v);

// ---------------------------------------------------------------------------
// 診断（どのノードのどの入力が何故失敗したか）
// ---------------------------------------------------------------------------
enum class Severity : uint8_t { Info = 0, Warning = 1, Error = 2 };

struct Diagnostic
{
    Severity    severity = Severity::Error;
    std::string code;      // "E_TYPE_MISMATCH" など。下の kCode 群
    std::string nodeId;    // 対象ノード（空 = グラフ全体）
    std::string pin;       // 対象の入力ピン名（空 = ノード全体）
    std::string message;   // 日本語の説明
    std::string hint;      // 直し方（空可）
    bool        reachable = true;  // false = 出力に繋がらない（死んだ）ノードの診断。コンパイル成否には影響しない
    int         line = 0;          // DXC エラーの行番号（DXC 由来のときだけ）
};

// 診断コード（文字列は MCP / テストが参照するので変えない）
namespace code
{
constexpr const char* kUnknownNode     = "E_UNKNOWN_NODE";      // 登録表に無いノード型
constexpr const char* kUnknownPin      = "E_UNKNOWN_PIN";       // 存在しないピン名
constexpr const char* kDanglingLink    = "E_DANGLING_LINK";     // 接続元ノードが無い
constexpr const char* kCycle           = "E_CYCLE";             // 循環
constexpr const char* kMissingInput    = "E_MISSING_INPUT";     // 必須入力が未接続
constexpr const char* kTypeMismatch    = "E_TYPE_MISMATCH";     // 型が合わない
constexpr const char* kDimMismatch     = "E_DIM_MISMATCH";      // 次元が揃わない（float3 と float2 など）
constexpr const char* kExpandForbidden = "E_EXPAND_FORBIDDEN";  // float2 → float3 の拡張
constexpr const char* kTruncate        = "W_TRUNCATE";          // 切り詰め（警告）
constexpr const char* kAppendOverflow  = "E_APPEND_OVERFLOW";   // Append の合計が 5 成分以上
constexpr const char* kMaskRange       = "E_MASK_RANGE";        // ComponentMask / Split が存在しない成分を選んだ
constexpr const char* kMaskEmpty       = "E_MASK_EMPTY";        // ComponentMask が 0 成分
constexpr const char* kReservedPin     = "E_RESERVED_PIN";      // 予約ピン（接続不可）
constexpr const char* kBadProp         = "E_BAD_PROP";          // プロパティ値が不正
constexpr const char* kDupParam        = "E_DUP_PARAM";         // パラメータ名の重複 / 空
constexpr const char* kNoOutput        = "E_NO_OUTPUT";         // 出力ノードが無い
constexpr const char* kMultiOutput     = "E_MULTI_OUTPUT";      // 出力ノードが 2 個以上
constexpr const char* kCustomEmpty     = "E_CUSTOM_EMPTY";      // Custom の本文が空
constexpr const char* kCpuUnsupported  = "E_CPU_UNSUPPORTED";   // CPU 評価できないノード
constexpr const char* kDeadNode        = "I_DEAD_NODE";         // 出力に繋がっていない（情報）
constexpr const char* kDxc             = "E_DXC";               // DXC のコンパイルエラー（nodeId 逆引き済み）
} // namespace code

} // namespace dx12e::matgraph
