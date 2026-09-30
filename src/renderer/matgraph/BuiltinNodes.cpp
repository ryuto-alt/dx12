// ============================================================================
// BuiltinNodes.cpp — 組み込みノードの登録表（G1: 約 50 種）
//
//   ノードを足すときはここへ RegisterXxx() の中に 1 件書くだけ。
//   ・inputs / outputs / props : ピンとプロパティの宣言
//   ・hlsl                    : 式テンプレート。{Pin} は接続元（型キャスト済み）、{P:prop} はプロパティ、
//                               {SLOT:prop} はパラメータプールの読み出し、{T} は結果の HLSL 型
//   ・eval                    : CPU 参照実装（数値テストの正解）。HLSL と同じ式を手で写す
//   ・テンプレートで書けない分岐は emit、既定の型推論で足りない場合は infer
// ============================================================================
#include "renderer/matgraph/NodeLibrary.h"

#include <algorithm>
#include <cmath>

namespace dx12e::matgraph
{
namespace
{

// ---- ピン宣言の短縮 ---------------------------------------------------------
PinDecl PolyIn(const char* name, bool required, float def, const char* desc, int group = 1)
{
    PinDecl p;
    p.name = name; p.mode = PinMode::Poly; p.polyGroup = group; p.required = required; p.desc = desc;
    if (!required) { p.hasDefault = true; p.defaultValue = Value::Float(def); }
    return p;
}
PinDecl FixedIn(const char* name, ValueType t, const char* desc)
{
    PinDecl p; p.name = name; p.mode = PinMode::Fixed; p.type = t; p.desc = desc; return p;
}
PinDecl FixedInReq(const char* name, ValueType t, const char* desc)
{
    PinDecl p = FixedIn(name, t, desc); p.required = true; return p;
}
PinDecl FixedInDef(const char* name, ValueType t, const Value& def, const char* desc)
{
    PinDecl p = FixedIn(name, t, desc); p.hasDefault = true; p.defaultValue = def; return p;
}
PinDecl FreeIn(const char* name, bool required, const char* desc, bool allowTex = false)
{
    PinDecl p; p.name = name; p.mode = PinMode::Free; p.required = required; p.allowTexture = allowTex; p.desc = desc; return p;
}
OutPinDecl PolyOut(const char* name = "Out", const char* desc = "")
{
    OutPinDecl o; o.name = name; o.mode = PinMode::Poly; o.desc = desc; return o;
}
OutPinDecl FixedOut(const char* name, ValueType t, const char* desc = "")
{
    OutPinDecl o; o.name = name; o.mode = PinMode::Fixed; o.type = t; o.desc = desc; return o;
}
OutPinDecl SwzOut(const char* name, const char* swz, int needDim, const char* desc = "")
{
    OutPinDecl o; o.name = name; o.mode = PinMode::Fixed;
    o.swizzle = swz; o.needDim = needDim;
    o.type = FloatType(static_cast<int>(o.swizzle.size()));
    o.desc = desc;
    return o;
}

// ---- プロパティ宣言の短縮 ----------------------------------------------------
PropDecl Prop(const char* name, PropType t, PropRole role, Json def, const char* desc = "")
{
    PropDecl p; p.name = name; p.type = t; p.role = role; p.defaultValue = std::move(def); p.desc = desc; return p;
}
PropDecl EnumProp(const char* name, PropRole role, std::vector<std::string> values, const char* def, const char* desc = "")
{
    PropDecl p; p.name = name; p.type = PropType::Enum; p.role = role; p.enumValues = std::move(values);
    p.defaultValue = def; p.desc = desc; return p;
}
Json J2(double a, double b)                      { return Json::array({a, b}); }
Json J3(double a, double b, double c)            { return Json::array({a, b, c}); }
Json J4(double a, double b, double c, double d)  { return Json::array({a, b, c, d}); }

// ---- CPU 評価の道具 -----------------------------------------------------------
CpuVal Make(ValueType t) { CpuVal r; r.type = t; return r; }
inline float Sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

using F1Fn = std::function<float(float)>;
using F2Fn = std::function<float(float, float)>;
using F3Fn = std::function<float(float, float, float)>;

NodeDef Unary(const char* type, const char* disp, const char* desc, const char* kw, const char* tmpl, F1Fn f,
              const char* pinName = "X")
{
    NodeDef d;
    d.type = type; d.displayName = disp; d.category = "数学"; d.description = desc; d.keywords = kw;
    const std::string pin = pinName;
    d.inputs  = {PolyIn(pinName, true, 0.0f, "入力")};
    d.outputs = {PolyOut()};
    d.resultMode = PinMode::Poly;
    d.hlsl = tmpl;
    d.eval = [f, pin](EvalCtx& c) {
        const CpuVal& x = c.In(pin);
        CpuVal r = Make(c.ResultType());
        for (int k = 0; k < Dim(r.type); ++k) r.v[k] = f(x.v[k]);
        return r;
    };
    return d;
}

NodeDef Binary(const char* type, const char* disp, const char* desc, const char* kw,
               const char* pa, bool reqA, float defA, const char* pb, bool reqB, float defB,
               const char* tmpl, F2Fn f)
{
    NodeDef d;
    d.type = type; d.displayName = disp; d.category = "数学"; d.description = desc; d.keywords = kw;
    const std::string sa = pa, sb = pb;
    d.inputs  = {PolyIn(pa, reqA, defA, "入力 A"), PolyIn(pb, reqB, defB, "入力 B")};
    d.outputs = {PolyOut()};
    d.resultMode = PinMode::Poly;
    d.hlsl = tmpl;
    d.eval = [f, sa, sb](EvalCtx& c) {
        const CpuVal& a = c.In(sa);
        const CpuVal& b = c.In(sb);
        CpuVal r = Make(c.ResultType());
        for (int k = 0; k < Dim(r.type); ++k) r.v[k] = f(a.v[k], b.v[k]);
        return r;
    };
    return d;
}

NodeDef Ternary(const char* type, const char* disp, const char* desc, const char* kw,
                PinDecl p0, PinDecl p1, PinDecl p2, const char* tmpl, F3Fn f)
{
    NodeDef d;
    d.type = type; d.displayName = disp; d.category = "数学"; d.description = desc; d.keywords = kw;
    const std::string n0 = p0.name, n1 = p1.name, n2 = p2.name;
    d.inputs  = {std::move(p0), std::move(p1), std::move(p2)};
    d.outputs = {PolyOut()};
    d.resultMode = PinMode::Poly;
    d.hlsl = tmpl;
    d.eval = [f, n0, n1, n2](EvalCtx& c) {
        const CpuVal& a = c.In(n0);
        const CpuVal& b = c.In(n1);
        const CpuVal& x = c.In(n2);
        CpuVal r = Make(c.ResultType());
        for (int k = 0; k < Dim(r.type); ++k) r.v[k] = f(a.v[k], b.v[k], x.v[k]);
        return r;
    };
    return d;
}

std::string SamplerMacro(const std::string& camel)
{
    // "AnisoWrap" → "UNO_SAMP_ANISO_WRAP"
    std::string s = "UNO_SAMP_";
    for (size_t k = 0; k < camel.size(); ++k)
    {
        const char ch = camel[k];
        if (ch >= 'A' && ch <= 'Z')
        {
            if (k > 0) s += '_';
            s += ch;
        }
        else
            s += static_cast<char>(ch - 'a' + 'A');
    }
    return s;
}

const std::vector<std::string>& SamplerStates()
{
    static const std::vector<std::string> v = {"AnisoWrap", "AnisoClamp", "LinearWrap", "LinearClamp", "PointWrap", "PointClamp"};
    return v;
}

// ---- 定数 ----------------------------------------------------------------------
NodeDef ConstNode(const char* type, const char* disp, ValueType t, PropType pt, Json def, const char* desc)
{
    NodeDef d;
    d.type = type; d.displayName = disp; d.category = "定数"; d.description = desc;
    d.keywords = "constant const 定数 数値";
    d.outputs = {FixedOut("Out", t)};
    d.resultMode = PinMode::Fixed; d.resultType = t;
    d.props = {Prop("value", pt, PropRole::SlotValue, std::move(def), "値（スロットに入るので、編集しても再コンパイルされない）"),
               Prop("inline", PropType::Bool, PropRole::Code, false, "true = HLSL のリテラルに畳み込む（値の編集で再コンパイル）")};
    d.hlsl = "{SLOT:value}";
    d.eval = [](EvalCtx& c) { return c.Slot("value"); };
    return d;
}

void RegisterConstants(NodeLibrary& lib)
{
    lib.Register(ConstNode("Float",  "Float",  ValueType::F1, PropType::Float, 0.0, "スカラー定数。"));
    lib.Register(ConstNode("Float2", "Float2", ValueType::F2, PropType::Vec2,  J2(0, 0), "2 成分の定数。"));
    lib.Register(ConstNode("Float3", "Float3", ValueType::F3, PropType::Vec3,  J3(0, 0, 0), "3 成分の定数（色ピッカー表示は UI 側の指定）。"));
    {
        NodeDef d = ConstNode("Float4", "Float4 / Color", ValueType::F4, PropType::Color, J4(1, 1, 1, 1),
                              "4 成分の定数（RGBA の色として使える）。srgb を有効にすると、格納時にリニアへ変換される。");
        d.keywords += " color 色";
        d.props.push_back(Prop("srgb", PropType::Bool, PropRole::Meta, false, "true = 値を sRGB として扱い、格納時にリニアへ変換する"));
        d.outputs = {FixedOut("Out", ValueType::F4), SwzOut("RGB", "xyz", 3), SwzOut("A", "w", 4)};
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Int"; d.displayName = "Int"; d.category = "定数"; d.description = "整数定数（HLSL のリテラルとして埋め込まれる）。";
        d.keywords = "integer 整数";
        d.outputs = {FixedOut("Out", ValueType::Int)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::Int;
        d.props = {Prop("value", PropType::Int, PropRole::Code, 0)};
        d.hlsl = "{P:value}";
        d.eval = [](EvalCtx& c) { CpuVal r = Make(ValueType::Int); r.v[0] = static_cast<float>(c.Props().Int("value")); return r; };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Bool"; d.displayName = "Bool"; d.category = "定数"; d.description = "真偽値の定数。Select の条件に使う。";
        d.keywords = "boolean true false 真偽";
        d.outputs = {FixedOut("Out", ValueType::Bool)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::Bool;
        d.props = {Prop("value", PropType::Bool, PropRole::Code, false)};
        d.hlsl = "{P:value}";
        d.eval = [](EvalCtx& c) { CpuVal r = Make(ValueType::Bool); r.v[0] = c.Props().Bool("value") ? 1.0f : 0.0f; return r; };
        lib.Register(std::move(d));
    }
}

// ---- パラメータ -----------------------------------------------------------------
void RegisterParameters(NodeLibrary& lib)
{
    {
        NodeDef d;
        d.type = "ScalarParameter"; d.displayName = "ScalarParameter"; d.category = "パラメータ";
        d.description = "名前つきのスカラーパラメータ。マテリアルインスタンスで値を上書きできる。";
        d.keywords = "parameter param scalar パラメータ 数値";
        d.isParameter = true;
        d.outputs = {FixedOut("Out", ValueType::F1)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F1;
        d.props = {Prop("name", PropType::String, PropRole::Meta, "Scalar", "パラメータ名（グラフ内で一意）"),
                   Prop("default", PropType::Float, PropRole::SlotValue, 0.0, "既定値"),
                   Prop("range", PropType::Vec2, PropRole::Meta, J2(0, 1), "スライダーの範囲"),
                   Prop("group", PropType::String, PropRole::Meta, "", "インスペクタでのグループ名"),
                   Prop("priority", PropType::Int, PropRole::Meta, 0, "表示順")};
        d.hlsl = "{SLOT:default}";
        d.eval = [](EvalCtx& c) { return c.Slot("default"); };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "VectorParameter"; d.displayName = "VectorParameter"; d.category = "パラメータ";
        d.description = "名前つきのベクトル（色）パラメータ。RGBA の各成分を別の出力から取り出せる。";
        d.keywords = "parameter param vector color colour パラメータ 色";
        d.isParameter = true;
        d.outputs = {FixedOut("Out", ValueType::F4), SwzOut("RGB", "xyz", 3), SwzOut("R", "x", 1), SwzOut("G", "y", 2),
                     SwzOut("B", "z", 3), SwzOut("A", "w", 4)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F4;
        d.props = {Prop("name", PropType::String, PropRole::Meta, "Vector", "パラメータ名（グラフ内で一意）"),
                   Prop("default", PropType::Color, PropRole::SlotValue, J4(1, 1, 1, 1), "既定値"),
                   Prop("srgb", PropType::Bool, PropRole::Meta, false, "true = 値を sRGB として扱い、格納時にリニアへ変換する"),
                   Prop("group", PropType::String, PropRole::Meta, "", "インスペクタでのグループ名"),
                   Prop("priority", PropType::Int, PropRole::Meta, 0, "表示順")};
        d.hlsl = "{SLOT:default}";
        d.eval = [](EvalCtx& c) { return c.Slot("default"); };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "TextureParameter"; d.displayName = "TextureParameter"; d.category = "パラメータ";
        d.description = "名前つきのテクスチャパラメータ。バインドレスの SRV 添字がスロットに入る。";
        d.keywords = "parameter param texture image テクスチャ 画像";
        d.isParameter = true;
        d.outputs = {FixedOut("Out", ValueType::Tex2D)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::Tex2D;
        d.props = {Prop("name", PropType::String, PropRole::Meta, "Texture", "パラメータ名（グラフ内で一意）"),
                   Prop("default", PropType::Texture, PropRole::SlotValue, "", "既定のテクスチャ（assets 相対パス）"),
                   EnumProp("sampler", PropRole::Meta, {"Color", "LinearColor", "Normal", "Mask"}, "Color",
                            "読み込み種別（Color = sRGB / LinearColor・Mask = リニア / Normal = 法線）"),
                   Prop("group", PropType::String, PropRole::Meta, "", "インスペクタでのグループ名"),
                   Prop("priority", PropType::Int, PropRole::Meta, 0, "表示順")};
        d.hlsl = "{SLOT:default}";
        lib.Register(std::move(d));   // テクスチャは CPU では評価しない（eval なし）
    }
}

// ---- 座標・時間 -----------------------------------------------------------------
NodeDef InputNode(const char* type, const char* disp, const char* desc, const char* kw, ValueType t, const char* tmpl, EvalFn ev)
{
    NodeDef d;
    d.type = type; d.displayName = disp; d.category = "座標"; d.description = desc; d.keywords = kw;
    d.outputs = {FixedOut("Out", t)};
    d.resultMode = PinMode::Fixed; d.resultType = t;
    d.hlsl = tmpl;
    d.eval = std::move(ev);
    return d;
}

void RegisterCoords(NodeLibrary& lib)
{
    {
        NodeDef d;
        d.type = "TexCoord"; d.displayName = "TexCoord"; d.category = "座標";
        d.description = "メッシュの UV に Tiling と Offset を掛けた座標。頂点に焼かれた UV スケールの上に乗る。";
        d.keywords = "uv texcoord coordinate タイリング 座標";
        d.outputs = {FixedOut("UV", ValueType::F2)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F2;
        d.props = {Prop("tiling", PropType::Vec2, PropRole::SlotValue, J2(1, 1), "UV の倍率"),
                   Prop("offset", PropType::Vec2, PropRole::SlotValue, J2(0, 0), "UV のずらし量"),
                   Prop("inline", PropType::Bool, PropRole::Code, false, "true = HLSL のリテラルに畳み込む")};
        d.hlsl = "(mi.uv * {SLOT:tiling} + {SLOT:offset})";
        d.eval = [](EvalCtx& c) {
            CpuVal t = c.Slot("tiling"), o = c.Slot("offset");
            CpuVal r = Make(ValueType::F2);
            r.v[0] = c.Env().uv[0] * t.v[0] + o.v[0];
            r.v[1] = c.Env().uv[1] * t.v[1] + o.v[1];
            return r;
        };
        lib.Register(std::move(d));
    }
    lib.Register(InputNode("WorldPosition", "WorldPosition", "ピクセルのワールド座標。", "position world 位置 ワールド座標", ValueType::F3,
        "mi.worldPos", [](EvalCtx& c) { CpuVal r = Make(ValueType::F3); for (int k = 0; k < 3; ++k) r.v[k] = c.Env().worldPos[k]; return r; }));
    lib.Register(InputNode("VertexNormalWS", "VertexNormalWS", "法線マップを適用する前の幾何法線（ワールド空間）。", "normal 法線", ValueType::F3,
        "mi.vertexNormalWS", [](EvalCtx& c) { CpuVal r = Make(ValueType::F3); for (int k = 0; k < 3; ++k) r.v[k] = c.Env().vertexNormalWS[k]; return r; }));
    lib.Register(InputNode("CameraPosition", "CameraPosition", "カメラのワールド座標。", "camera eye カメラ 視点", ValueType::F3,
        "mi.cameraPos", [](EvalCtx& c) { CpuVal r = Make(ValueType::F3); for (int k = 0; k < 3; ++k) r.v[k] = c.Env().cameraPos[k]; return r; }));
    lib.Register(InputNode("CameraVector", "CameraVector", "ピクセルからカメラへ向かう単位ベクトル（ワールド空間）。", "view vector 視線 カメラ", ValueType::F3,
        "normalize(mi.cameraPos - mi.worldPos)",
        [](EvalCtx& c) {
            CpuVal r = Make(ValueType::F3);
            float d[3], l2 = 0;
            for (int k = 0; k < 3; ++k) { d[k] = c.Env().cameraPos[k] - c.Env().worldPos[k]; l2 += d[k] * d[k]; }
            const float inv = 1.0f / std::sqrt(l2);
            for (int k = 0; k < 3; ++k) r.v[k] = d[k] * inv;
            return r;
        }));
    {
        NodeDef d = InputNode("VertexColor", "VertexColor", "頂点カラー。", "vertex color 頂点カラー", ValueType::F4,
            "mi.vertexColor", [](EvalCtx& c) { CpuVal r = Make(ValueType::F4); for (int k = 0; k < 4; ++k) r.v[k] = c.Env().vertexColor[k]; return r; });
        d.outputs = {FixedOut("Out", ValueType::F4), SwzOut("RGB", "xyz", 3), SwzOut("R", "x", 1), SwzOut("G", "y", 2),
                     SwzOut("B", "z", 3), SwzOut("A", "w", 4)};
        lib.Register(std::move(d));
    }
    {
        NodeDef d = InputNode("Time", "Time", "起動からの経過秒。", "time seconds 時間 経過", ValueType::F1,
            "mi.time", [](EvalCtx& c) { CpuVal r = Make(ValueType::F1); r.v[0] = c.Env().time; return r; });
        d.category = "時間";
        lib.Register(std::move(d));
    }
}

// ---- 数学 -----------------------------------------------------------------------
void RegisterMath(NodeLibrary& lib)
{
    lib.Register(Binary("Add", "Add", "A + B。次元が違うときはどちらかがスカラー（float）であること。", "plus 加算 足す",
                        "A", false, 0.0f, "B", false, 0.0f, "({A} + {B})", [](float a, float b) { return a + b; }));
    lib.Register(Binary("Subtract", "Subtract", "A - B。", "minus 減算 引く",
                        "A", false, 0.0f, "B", false, 0.0f, "({A} - {B})", [](float a, float b) { return a - b; }));
    lib.Register(Binary("Multiply", "Multiply", "A * B（成分ごと）。", "mul times 乗算 掛ける",
                        "A", false, 1.0f, "B", false, 1.0f, "({A} * {B})", [](float a, float b) { return a * b; }));
    {
        NodeDef d = Binary("Divide", "Divide", "A / B。safe を有効にすると B を 1e-6 以上に制限してゼロ除算を避ける。", "div 除算 割る",
                           "A", false, 1.0f, "B", false, 1.0f, "({A} / {B})", nullptr);
        d.props = {Prop("safe", PropType::Bool, PropRole::Code, false, "true = B を max(B, 1e-6) にする")};
        d.emit = [](EmitCtx& c) { return c.Format(c.Props().Bool("safe") ? "({A} / max({B}, 1e-6))" : "({A} / {B})"); };
        d.eval = [](EvalCtx& c) {
            const CpuVal& a = c.In("A");
            const CpuVal& b = c.In("B");
            const bool safe = c.Props().Bool("safe");
            CpuVal r = Make(c.ResultType());
            for (int k = 0; k < Dim(r.type); ++k)
                r.v[k] = a.v[k] / (safe ? std::max(b.v[k], 1e-6f) : b.v[k]);
            return r;
        };
        lib.Register(std::move(d));
    }
    lib.Register(Ternary("Lerp", "Lerp", "A と B の線形補間。Alpha は float または同じ次元。", "mix blend 補間 ブレンド",
                         PolyIn("A", false, 0.0f, "Alpha=0 のときの値"), PolyIn("B", false, 1.0f, "Alpha=1 のときの値"),
                         PolyIn("Alpha", false, 0.5f, "補間係数"),
                         "lerp({A}, {B}, {Alpha})", [](float a, float b, float t) { return a + t * (b - a); }));
    lib.Register(Ternary("Clamp", "Clamp", "X を Min〜Max に制限する。", "limit 制限",
                         PolyIn("X", true, 0.0f, "入力"), PolyIn("Min", false, 0.0f, "下限"), PolyIn("Max", false, 1.0f, "上限"),
                         "clamp({X}, {Min}, {Max})", [](float x, float mn, float mx) { return std::min(std::max(x, mn), mx); }));
    lib.Register(Unary("Saturate", "Saturate", "0〜1 に制限する。", "clamp01 制限", "saturate({X})", [](float x) { return Sat(x); }));
    lib.Register(Unary("OneMinus", "OneMinus", "1 - X。", "invert 反転 1-x", "(1.0 - {X})", [](float x) { return 1.0f - x; }));
    lib.Register(Unary("Abs", "Abs", "絶対値。", "absolute 絶対値", "abs({X})", [](float x) { return std::fabs(x); }));
    lib.Register(Binary("Power", "Power", "Base の Exp 乗。Base は 0 以上に制限される。", "pow 累乗 べき乗",
                        "Base", true, 0.0f, "Exp", false, 2.0f, "pow(max({Base}, 0.0), {Exp})",
                        [](float b, float e) { return std::pow(std::max(b, 0.0f), e); }));
    lib.Register(Binary("Min", "Min", "小さい方。", "minimum 最小", "A", false, 0.0f, "B", false, 0.0f, "min({A}, {B})",
                        [](float a, float b) { return std::min(a, b); }));
    lib.Register(Binary("Max", "Max", "大きい方。", "maximum 最大", "A", false, 0.0f, "B", false, 0.0f, "max({A}, {B})",
                        [](float a, float b) { return std::max(a, b); }));
    lib.Register(Unary("Frac", "Frac", "小数部分（X - Floor(X)）。", "fraction 小数", "frac({X})", [](float x) { return x - std::floor(x); }));
    lib.Register(Unary("Floor", "Floor", "切り捨て。", "round down 切り捨て", "floor({X})", [](float x) { return std::floor(x); }));
    lib.Register(Unary("Ceil", "Ceil", "切り上げ。", "round up 切り上げ", "ceil({X})", [](float x) { return std::ceil(x); }));
    lib.Register(Unary("Sqrt", "Sqrt", "平方根（負数は 0 として扱う）。", "square root 平方根", "sqrt(max({X}, 0.0))",
                       [](float x) { return std::sqrt(std::max(x, 0.0f)); }));
    lib.Register(Unary("Sin", "Sin", "サイン（ラジアン）。", "sine sin 三角関数", "sin({X})", [](float x) { return std::sin(x); }));
    lib.Register(Unary("Cos", "Cos", "コサイン（ラジアン）。", "cosine cos 三角関数", "cos({X})", [](float x) { return std::cos(x); }));
    lib.Register(Binary("Step", "Step", "X が Edge 以上なら 1、未満なら 0。", "threshold しきい値",
                        "Edge", false, 0.5f, "X", true, 0.0f, "step({Edge}, {X})",
                        [](float e, float x) { return x >= e ? 1.0f : 0.0f; }));
    lib.Register(Ternary("Smoothstep", "Smoothstep", "Min〜Max の間をなめらかに 0〜1 へ補間する（Min と Max は別の値にすること）。", "smooth なめらか",
                         PolyIn("Min", false, 0.0f, "下端"), PolyIn("Max", false, 1.0f, "上端"), PolyIn("X", true, 0.0f, "入力"),
                         "smoothstep({Min}, {Max}, {X})",
                         [](float mn, float mx, float x) {
                             const float t = Sat((x - mn) / (mx - mn));
                             return t * t * (3.0f - 2.0f * t);
                         }));

    // ---- ベクトル ----
    {
        NodeDef d;
        d.type = "Dot"; d.displayName = "Dot"; d.category = "ベクトル"; d.description = "内積。A と B は同じ次元にそろう。";
        d.keywords = "dot product 内積";
        d.inputs = {PolyIn("A", true, 0.0f, "ベクトル A"), PolyIn("B", true, 0.0f, "ベクトル B")};
        d.outputs = {FixedOut("Out", ValueType::F1)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F1;
        d.emit = [](EmitCtx& c) {
            // dot(float, float) を避け、スカラーは掛け算にする
            return c.Format(Dim(c.InType("A")) == 1 ? "({A} * {B})" : "dot({A}, {B})");
        };
        d.eval = [](EvalCtx& c) {
            const CpuVal& a = c.In("A");
            const CpuVal& b = c.In("B");
            CpuVal r = Make(ValueType::F1);
            for (int k = 0; k < Dim(a.type); ++k) r.v[0] += a.v[k] * b.v[k];
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Cross"; d.displayName = "Cross"; d.category = "ベクトル"; d.description = "外積（float3）。";
        d.keywords = "cross product 外積";
        d.inputs = {FixedInReq("A", ValueType::F3, "ベクトル A"), FixedInReq("B", ValueType::F3, "ベクトル B")};
        d.outputs = {FixedOut("Out", ValueType::F3)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F3;
        d.hlsl = "cross({A}, {B})";
        d.eval = [](EvalCtx& c) {
            const CpuVal& a = c.In("A");
            const CpuVal& b = c.In("B");
            CpuVal r = Make(ValueType::F3);
            r.v[0] = a.v[1] * b.v[2] - a.v[2] * b.v[1];
            r.v[1] = a.v[2] * b.v[0] - a.v[0] * b.v[2];
            r.v[2] = a.v[0] * b.v[1] - a.v[1] * b.v[0];
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Normalize"; d.displayName = "Normalize"; d.category = "ベクトル";
        d.description = "正規化（長さ 1 に）。長さ 0 のときは 0 に近い値を返し、NaN にならない。";
        d.keywords = "unit 正規化";
        d.inputs = {PolyIn("X", true, 0.0f, "入力")};
        d.outputs = {PolyOut()};
        d.resultMode = PinMode::Poly;
        d.emit = [](EmitCtx& c) {
            return c.Format(Dim(c.InType("X")) == 1 ? "({X} * rsqrt(max({X} * {X}, 1e-12)))"
                                                    : "({X} * rsqrt(max(dot({X}, {X}), 1e-12)))");
        };
        d.eval = [](EvalCtx& c) {
            const CpuVal& x = c.In("X");
            CpuVal r = Make(c.ResultType());
            float l2 = 0;
            for (int k = 0; k < Dim(r.type); ++k) l2 += x.v[k] * x.v[k];
            const float inv = 1.0f / std::sqrt(std::max(l2, 1e-12f));
            for (int k = 0; k < Dim(r.type); ++k) r.v[k] = x.v[k] * inv;
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Length"; d.displayName = "Length"; d.category = "ベクトル"; d.description = "ベクトルの長さ。";
        d.keywords = "magnitude 長さ";
        d.inputs = {PolyIn("X", true, 0.0f, "入力")};
        d.outputs = {FixedOut("Out", ValueType::F1)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F1;
        d.emit = [](EmitCtx& c) { return c.Format(Dim(c.InType("X")) == 1 ? "abs({X})" : "length({X})"); };
        d.eval = [](EvalCtx& c) {
            const CpuVal& x = c.In("X");
            CpuVal r = Make(ValueType::F1);
            float l2 = 0;
            for (int k = 0; k < Dim(x.type); ++k) l2 += x.v[k] * x.v[k];
            r.v[0] = std::sqrt(l2);
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Append"; d.displayName = "Append"; d.category = "ベクトル";
        d.description = "ベクトルを連結する（float + float3 → float4 など）。合計 4 成分まで。";
        d.keywords = "combine make vector 合成 連結";
        d.inputs = {FreeIn("A", true, "前半"), FreeIn("B", true, "後半")};
        d.outputs = {PolyOut()};
        d.resultMode = PinMode::Poly;
        d.infer = [](InferCtx& c) {
            const int n = Dim(c.inTypes[0]) + Dim(c.inTypes[1]);
            if (c.inTypes[0] == ValueType::Invalid || c.inTypes[1] == ValueType::Invalid) return;
            if (n > 4)
            {
                c.Error(code::kAppendOverflow, "B",
                        std::string("Append: 合計 ") + std::to_string(n) + " 成分になります。4 成分までです",
                        "ComponentMask で成分を減らしてください");
                c.result = ValueType::Invalid;
            }
            else
                c.result = FloatType(n);
        };
        d.hlsl = "{T}({A}, {B})";
        d.eval = [](EvalCtx& c) {
            const CpuVal& a = c.In("A");
            const CpuVal& b = c.In("B");
            CpuVal r = Make(c.ResultType());
            const int na = Dim(a.type), nb = Dim(b.type);
            for (int k = 0; k < na; ++k) r.v[k] = a.v[k];
            for (int k = 0; k < nb; ++k) r.v[na + k] = b.v[k];
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "ComponentMask"; d.displayName = "ComponentMask"; d.category = "ベクトル";
        d.description = "ベクトルから R / G / B / A のうち選んだ成分を取り出す。";
        d.keywords = "swizzle mask split 分解 成分";
        d.inputs = {FreeIn("X", true, "入力ベクトル")};
        d.outputs = {PolyOut()};
        d.resultMode = PinMode::Poly;
        d.props = {Prop("r", PropType::Bool, PropRole::Code, true), Prop("g", PropType::Bool, PropRole::Code, true),
                   Prop("b", PropType::Bool, PropRole::Code, true), Prop("a", PropType::Bool, PropRole::Code, false)};
        d.infer = [](InferCtx& c) {
            if (c.inTypes[0] == ValueType::Invalid) return;
            const bool sel[4] = {c.props->Bool("r"), c.props->Bool("g"), c.props->Bool("b"), c.props->Bool("a")};
            int count = 0, maxIdx = -1;
            for (int k = 0; k < 4; ++k)
                if (sel[k]) { ++count; maxIdx = k; }
            if (count == 0)
            {
                c.Error(code::kMaskEmpty, "X", "ComponentMask: 取り出す成分が 1 つも選ばれていません", "R / G / B / A のどれかを有効にしてください");
                c.result = ValueType::Invalid;
                return;
            }
            const int dim = Dim(c.inTypes[0]);
            if (maxIdx >= dim)
            {
                c.Error(code::kMaskRange, "X",
                        std::string("ComponentMask: 入力は ") + TypeName(c.inTypes[0]) + " ですが、存在しない成分を選んでいます",
                        "入力の次元に合わせて成分を選び直してください");
                c.result = ValueType::Invalid;
                return;
            }
            c.result = FloatType(count);
        };
        auto selected = [](const PropView& p) {
            std::string s;
            if (p.Bool("r")) s += 'x';
            if (p.Bool("g")) s += 'y';
            if (p.Bool("b")) s += 'z';
            if (p.Bool("a")) s += 'w';
            return s;
        };
        d.emit = [selected](EmitCtx& c) {
            const std::string sw = selected(c.Props());
            if (c.InIsLiteral("X"))
            {
                // リテラルへ swizzle を書くと読みにくく壊れやすい（0.5.x）ので、コンパイル時に畳み込む
                const Value v = c.InLiteral("X");
                float out[4] = {0, 0, 0, 0};
                for (size_t k = 0; k < sw.size(); ++k)
                    out[k] = v.f[sw[k] == 'x' ? 0 : sw[k] == 'y' ? 1 : sw[k] == 'z' ? 2 : 3];
                return FormatHlslValue(Value::Vec(static_cast<int>(sw.size()), out));
            }
            if (Dim(c.InType("X")) == 1) return c.In("X");   // 1 成分（x）を選んだスカラー
            return c.In("X") + "." + sw;
        };
        d.eval = [selected](EvalCtx& c) {
            const CpuVal& x = c.In("X");
            const std::string sw = selected(c.Props());
            CpuVal r = Make(c.ResultType());
            for (size_t k = 0; k < sw.size(); ++k)
                r.v[k] = x.v[sw[k] == 'x' ? 0 : sw[k] == 'y' ? 1 : sw[k] == 'z' ? 2 : 3];
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Split"; d.displayName = "Split"; d.category = "ベクトル";
        d.description = "ベクトルを R / G / B / A の float に分解する。";
        d.keywords = "break split 分解";
        d.inputs = {FreeIn("X", true, "入力ベクトル")};
        d.outputs = {SwzOut("R", "x", 1), SwzOut("G", "y", 2), SwzOut("B", "z", 3), SwzOut("A", "w", 4)};
        d.resultMode = PinMode::Poly;
        d.infer = [](InferCtx& c) { c.result = c.inTypes[0]; };
        d.hlsl = "{X}";
        d.eval = [](EvalCtx& c) { return c.In("X"); };
        lib.Register(std::move(d));
    }
}

// ---- テクスチャ・色・シェーディング入力 ------------------------------------------
void RegisterTextureAndShading(NodeLibrary& lib)
{
    {
        NodeDef d;
        d.type = "TextureSample"; d.displayName = "TextureSample"; d.category = "テクスチャ";
        d.description = "テクスチャをサンプルする。Tex が未接続なら、このノードの texture プロパティが使われる（省略記法）。";
        d.keywords = "sample texture image テクスチャ サンプル 画像";
        PinDecl tex = FixedIn("Tex", ValueType::Tex2D, "テクスチャ（TextureParameter など）");
        tex.implicitTexture = true;
        PinDecl uv = FixedIn("UV", ValueType::F2, "UV 座標（未接続ならメッシュの UV）");
        uv.defaultBuiltin = "uv";
        d.inputs = {tex, uv, FixedInDef("MipBias", ValueType::F1, Value::Float(0.0f), "ミップの偏り")};
        d.outputs = {FixedOut("RGBA", ValueType::F4), SwzOut("RGB", "xyz", 3), SwzOut("R", "x", 1), SwzOut("G", "y", 2),
                     SwzOut("B", "z", 3), SwzOut("A", "w", 4)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F4;
        d.props = {Prop("texture", PropType::Texture, PropRole::SlotValue, "", "Tex が未接続のときに使うテクスチャ（assets 相対パス）"),
                   EnumProp("usage", PropRole::Meta, {"Color", "LinearColor", "Normal", "Mask"}, "Color",
                            "Tex が未接続のときの読み込み種別（Color = sRGB / それ以外 = リニア）"),
                   EnumProp("samplerState", PropRole::Code, SamplerStates(), "AnisoWrap", "サンプラー（フィルタ × アドレス）")};
        d.emit = [](EmitCtx& c) {
            const std::string samp = SamplerMacro(c.Props().Str("samplerState"));
            const bool noBias = !c.Has("MipBias") || (c.InIsLiteral("MipBias") && c.InLiteral("MipBias").f[0] == 0.0f);
            if (noBias) return "UnoSample(" + c.In("Tex") + ", " + samp + ", " + c.In("UV") + ")";
            return "UnoSampleBias(" + c.In("Tex") + ", " + samp + ", " + c.In("UV") + ", " + c.In("MipBias") + ")";
        };
        d.eval = [](EvalCtx& c) {
            const int slot = c.TextureSlot("Tex");
            if (slot < 0 || !c.Env().sampleTexture)
            {
                c.Fail("テクスチャは CPU で評価できません（EvalEnv::sampleTexture が未設定）");
                return Make(ValueType::F4);
            }
            CpuVal r = Make(ValueType::F4);
            c.Env().sampleTexture(slot, c.In("UV").v, c.In("MipBias").v[0], r.v);
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "NormalMap"; d.displayName = "NormalMap"; d.category = "テクスチャ";
        d.description = "法線マップを接空間の法線へ展開する（xy を 2 倍して 1 を引き、強さを掛けて z を再構成。OpenGL 規約）。";
        d.keywords = "normal map 法線マップ 法線";
        PinDecl tex = FixedIn("Tex", ValueType::Tex2D, "法線テクスチャ");
        tex.implicitTexture = true;
        PinDecl uv = FixedIn("UV", ValueType::F2, "UV 座標（未接続ならメッシュの UV）");
        uv.defaultBuiltin = "uv";
        d.inputs = {tex, uv, FixedInDef("Strength", ValueType::F1, Value::Float(1.0f), "強さ（1 = そのまま）")};
        d.outputs = {FixedOut("Out", ValueType::F3, "接空間の法線")};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F3;
        d.props = {Prop("texture", PropType::Texture, PropRole::SlotValue, "", "Tex が未接続のときに使うテクスチャ（assets 相対パス）"),
                   EnumProp("usage", PropRole::Meta, {"Color", "LinearColor", "Normal", "Mask"}, "Normal",
                            "Tex が未接続のときの読み込み種別"),
                   EnumProp("samplerState", PropRole::Code, SamplerStates(), "AnisoWrap", "サンプラー（フィルタ × アドレス）")};
        d.helperName = "MG_NormalUnpack";
        d.helperHlsl =
            "float3 MG_NormalUnpack(float3 rgb, float strength)\n"
            "{\n"
            "    float2 xy = (rgb.xy * 2.0 - 1.0) * strength;\n"
            "    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));\n"
            "}\n";
        d.emit = [](EmitCtx& c) {
            const std::string samp = SamplerMacro(c.Props().Str("samplerState"));
            return "MG_NormalUnpack(UnoSample(" + c.In("Tex") + ", " + samp + ", " + c.In("UV") + ").xyz, " + c.In("Strength") + ")";
        };
        d.eval = [](EvalCtx& c) {
            const int slot = c.TextureSlot("Tex");
            if (slot < 0 || !c.Env().sampleTexture)
            {
                c.Fail("テクスチャは CPU で評価できません（EvalEnv::sampleTexture が未設定）");
                return Make(ValueType::F3);
            }
            float rgba[4];
            c.Env().sampleTexture(slot, c.In("UV").v, 0.0f, rgba);
            const float s = c.In("Strength").v[0];
            const float x = (rgba[0] * 2.0f - 1.0f) * s, y = (rgba[1] * 2.0f - 1.0f) * s;
            CpuVal r = Make(ValueType::F3);
            r.v[0] = x; r.v[1] = y; r.v[2] = std::sqrt(Sat(1.0f - (x * x + y * y)));
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Fresnel"; d.displayName = "Fresnel"; d.category = "シェーディング";
        d.description = "視線と法線のなす角による反射率（Schlick 近似）。縁に近いほど 1 に近づく。";
        d.keywords = "fresnel rim フレネル リム";
        PinDecl n = FixedIn("Normal", ValueType::F3, "法線（未接続なら幾何法線）");
        n.defaultBuiltin = "vertexNormalWS";
        d.inputs = {n, FixedInDef("Exponent", ValueType::F1, Value::Float(5.0f), "指数（大きいほど縁だけが光る）"),
                    FixedInDef("BaseReflect", ValueType::F1, Value::Float(0.04f), "正面での反射率")};
        d.outputs = {FixedOut("Out", ValueType::F1)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F1;
        d.hlsl = "({BaseReflect} + (1.0 - {BaseReflect}) * pow(saturate(1.0 - saturate(dot(normalize({Normal}), "
                 "normalize(mi.cameraPos - mi.worldPos)))), {Exponent}))";
        d.eval = [](EvalCtx& c) {
            const CpuVal& nv = c.In("Normal");
            float n[3] = {nv.v[0], nv.v[1], nv.v[2]}, v[3];
            float ln = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (float& x : n) x /= ln;
            float lv = 0;
            for (int k = 0; k < 3; ++k) { v[k] = c.Env().cameraPos[k] - c.Env().worldPos[k]; lv += v[k] * v[k]; }
            lv = std::sqrt(lv);
            float dt = 0;
            for (int k = 0; k < 3; ++k) dt += n[k] * (v[k] / lv);
            const float f0 = c.In("BaseReflect").v[0];
            CpuVal r = Make(ValueType::F1);
            r.v[0] = f0 + (1.0f - f0) * std::pow(Sat(1.0f - Sat(dt)), c.In("Exponent").v[0]);
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Desaturation"; d.displayName = "Desaturation"; d.category = "色";
        d.description = "彩度を落とす（Rec.709 の輝度へ補間）。Fraction=1 で完全な白黒。";
        d.keywords = "desaturate grayscale saturation 彩度 白黒";
        d.inputs = {FixedInReq("Color", ValueType::F3, "入力色"), FixedInDef("Fraction", ValueType::F1, Value::Float(1.0f), "落とす割合")};
        d.outputs = {FixedOut("Out", ValueType::F3)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::F3;
        d.hlsl = "lerp({Color}, dot({Color}, float3(0.2126, 0.7152, 0.0722)).xxx, {Fraction})";
        d.eval = [](EvalCtx& c) {
            const CpuVal& col = c.In("Color");
            const float f = c.In("Fraction").v[0];
            const float lum = col.v[0] * 0.2126f + col.v[1] * 0.7152f + col.v[2] * 0.0722f;
            CpuVal r = Make(ValueType::F3);
            for (int k = 0; k < 3; ++k) r.v[k] = col.v[k] + f * (lum - col.v[k]);
            return r;
        };
        lib.Register(std::move(d));
    }
}

// ---- 制御・出力 ------------------------------------------------------------------
void RegisterControlAndOutput(NodeLibrary& lib)
{
    {
        NodeDef d;
        d.type = "Custom"; d.displayName = "Custom"; d.category = "制御";
        d.description = "HLSL の式または関数本体を書く。入力は In0〜In7（接続したものだけが引数になる）、"
                        "本文に return があれば関数本体、無ければ式として扱う。テクスチャは Texture2D として渡される。";
        d.keywords = "custom hlsl code 式 コード";
        d.isCustom = true; d.noCse = true;
        for (int k = 0; k < 8; ++k)
        {
            const std::string nm = "In" + std::to_string(k);
            PinDecl p = FreeIn("", false, "入力", true);
            p.name = nm;
            d.inputs.push_back(std::move(p));
        }
        d.outputs = {PolyOut()};
        d.resultMode = PinMode::Poly;
        d.props = {Prop("code", PropType::Text, PropRole::Code, "0.0", "HLSL（式、または return を含む関数本体）"),
                   EnumProp("outputType", PropRole::Code, {"float", "float2", "float3", "float4"}, "float", "出力の型")};
        d.infer = [](InferCtx& c) {
            ValueType t = ValueType::F1;
            ParseTypeName(c.props->Str("outputType"), t);
            c.result = t;
        };
        // 関数の生成はコンパイラが行う（isCustom）。eval なし = CPU では評価不能
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Reroute"; d.displayName = "Reroute"; d.category = "制御";
        d.description = "ワイヤの中継点。型はそのまま引き継ぐ。生成コードには現れない。";
        d.keywords = "reroute knot 中継";
        d.isTransparent = true;
        d.inputs = {FreeIn("In", true, "入力", true)};
        d.outputs = {PolyOut()};
        d.resultMode = PinMode::Poly;
        d.infer = [](InferCtx& c) { c.result = c.inTypes[0]; };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Select"; d.displayName = "Select"; d.category = "制御";
        d.description = "Cond が true なら A、false なら B。両方を評価してから選ぶ（テクスチャサンプルの微分を壊さない）。";
        d.keywords = "if switch branch 条件 分岐";
        d.inputs = {PolyIn("A", false, 1.0f, "true のときの値"), PolyIn("B", false, 0.0f, "false のときの値"),
                    FixedInDef("Cond", ValueType::Bool, Value::Bool1(false), "条件")};
        d.outputs = {PolyOut()};
        d.resultMode = PinMode::Poly;
        d.hlsl = "({Cond} ? {A} : {B})";
        d.eval = [](EvalCtx& c) {
            const bool cond = c.In("Cond").v[0] != 0.0f;
            const CpuVal& a = c.In("A");
            const CpuVal& b = c.In("B");
            CpuVal r = Make(c.ResultType());
            for (int k = 0; k < Dim(r.type); ++k) r.v[k] = cond ? a.v[k] : b.v[k];
            return r;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "Compare"; d.displayName = "Compare"; d.category = "制御";
        d.description = "A と B を比較して bool を返す。";
        d.keywords = "compare less greater equal 比較 条件";
        d.inputs = {FixedInDef("A", ValueType::F1, Value::Float(0.0f), "左辺"), FixedInDef("B", ValueType::F1, Value::Float(0.0f), "右辺")};
        d.outputs = {FixedOut("Out", ValueType::Bool)};
        d.resultMode = PinMode::Fixed; d.resultType = ValueType::Bool;
        d.props = {EnumProp("mode", PropRole::Code, {"Less", "LessEqual", "Greater", "GreaterEqual", "Equal", "NotEqual"}, "Less")};
        auto op = [](const std::string& m) {
            if (m == "Less") return "<";
            if (m == "LessEqual") return "<=";
            if (m == "Greater") return ">";
            if (m == "GreaterEqual") return ">=";
            if (m == "Equal") return "==";
            return "!=";
        };
        d.emit = [op](EmitCtx& c) { return c.Format(std::string("({A} ") + op(c.Props().Str("mode")) + " {B})"); };
        d.eval = [](EvalCtx& c) {
            const float a = c.In("A").v[0], b = c.In("B").v[0];
            const std::string m = c.Props().Str("mode");
            bool r = false;
            if (m == "Less") r = a < b;
            else if (m == "LessEqual") r = a <= b;
            else if (m == "Greater") r = a > b;
            else if (m == "GreaterEqual") r = a >= b;
            else if (m == "Equal") r = a == b;
            else r = a != b;
            CpuVal v = Make(ValueType::Bool);
            v.v[0] = r ? 1.0f : 0.0f;
            return v;
        };
        lib.Register(std::move(d));
    }
    {
        NodeDef d;
        d.type = "MaterialOutput"; d.displayName = "MaterialOutput"; d.category = "出力";
        d.description = "マテリアルの出力。グラフに 1 個だけ置く。未接続のピンは既定のサーフェス値のまま。灰色のピンは将来用の予約（接続不可）。";
        d.keywords = "output result 出力 マテリアル";
        d.isOutput = true;
        auto outPin = [](const char* name, ValueType t, const char* field, const Value& def, const char* desc, bool reserved = false) {
            PinDecl p = FixedIn(name, t, desc);
            p.surfaceField = field;
            p.hasDefault = true;
            p.defaultValue = def;
            p.reserved = reserved;
            return p;
        };
        d.inputs = {
            outPin("BaseColor",        ValueType::F3, "baseColor", Value::Float3(0.5f, 0.5f, 0.5f), "ベースカラー"),
            outPin("Metallic",         ValueType::F1, "metallic",  Value::Float(0.0f), "金属度"),
            outPin("Roughness",        ValueType::F1, "roughness", Value::Float(0.5f), "粗さ（下限 0.04 は共通のシェーディング側で掛かる）"),
            outPin("Normal",           ValueType::F3, "normalTS",  Value::Float3(0, 0, 1), "接空間の法線（接続したときだけ有効）"),
            outPin("Emissive",         ValueType::F3, "emissive",  Value::Float3(0, 0, 0), "自己発光（影・AO を通さず加算）"),
            outPin("AmbientOcclusion", ValueType::F1, "ao",        Value::Float(1.0f), "環境光へ乗算する遮蔽"),
            outPin("Opacity",          ValueType::F1, "opacity",   Value::Float(1.0f), "不透明度（Translucent 時）"),
            outPin("OpacityMask",      ValueType::F1, "opacityMask", Value::Float(1.0f), "マスク値（Masked 時。G3b で影・深度へ対応）"),
            // ---- 予約（接続不可） ----
            outPin("Specular",           ValueType::F1, "",                   Value::Float(0.5f), "予約: スペキュラ", true),
            outPin("Anisotropy",         ValueType::F1, "anisotropy",         Value::Float(0.0f), "予約: 異方性", true),
            outPin("SubsurfaceColor",    ValueType::F3, "subsurfaceColor",    Value::Float3(0, 0, 0), "予約: サブサーフェスの色", true),
            outPin("SubsurfaceOpacity",  ValueType::F1, "subsurfaceOpacity",  Value::Float(0.0f), "予約: サブサーフェスの強さ", true),
            outPin("ClearCoat",          ValueType::F1, "clearCoat",          Value::Float(0.0f), "予約: クリアコート", true),
            outPin("ClearCoatRoughness", ValueType::F1, "clearCoatRoughness", Value::Float(0.0f), "予約: クリアコートの粗さ", true),
            outPin("Tangent",            ValueType::F3, "",                   Value::Float3(1, 0, 0), "予約: 接線", true),
            outPin("WorldPositionOffset",ValueType::F3, "",                   Value::Float3(0, 0, 0), "予約: 頂点のオフセット", true),
            outPin("PixelDepthOffset",   ValueType::F1, "",                   Value::Float(0.0f), "予約: 深度オフセット", true),
            outPin("Refraction",         ValueType::F3, "",                   Value::Float3(0, 0, 0), "予約: 屈折", true),
        };
        lib.Register(std::move(d));
    }
}

} // namespace

void RegisterBuiltinNodes(NodeLibrary& lib)
{
    RegisterConstants(lib);
    RegisterParameters(lib);
    RegisterCoords(lib);
    RegisterMath(lib);
    RegisterTextureAndShading(lib);
    RegisterControlAndOutput(lib);
}

} // namespace dx12e::matgraph
