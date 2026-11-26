#include "renderer/vfx/VfxAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e::vfx
{

using nlohmann::json;

namespace
{

// ---- パラメータの仕様（表駆動。正規化・検証・書き出しを 1 か所で行う）----
enum class PK { Float, Vec3, Vec4, Range, Bool, Enum };

struct ParamSpec
{
    const char* key;
    PK          kind;
    float       d[4];        // 既定値（Range は d[0], d[1]）
    float       lo, hi;      // 各要素の範囲
    const char* enums;       // Enum: "a|b|c"（既定は先頭）
};

struct ModuleSpec
{
    const char* name;
    bool        isInit;
    std::vector<ParamSpec> params;
};

constexpr float kBig = 1.0e6f;

const std::vector<ModuleSpec>& Specs()
{
    static const std::vector<ModuleSpec> s = {
        // ---- init ----
        { "shape", true, {
            { "kind",        PK::Enum,  {0,0,0,0}, 0, 0, "point|sphere|box|cone|disc" },
            { "radius",      PK::Float, {0.1f,0,0,0}, 0, 1000.0f, nullptr },
            { "innerRadius", PK::Float, {0,0,0,0}, 0, 1000.0f, nullptr },
            { "size",        PK::Vec3,  {0.1f,0.1f,0.1f,0}, 0, 1000.0f, nullptr },
            { "angle",       PK::Float, {25.0f,0,0,0}, 0, 89.0f, nullptr },
            { "length",      PK::Float, {0.0f,0,0,0}, 0, 1000.0f, nullptr },
            { "surface",     PK::Bool,  {0,0,0,0}, 0, 1, nullptr },
        }},
        { "velocity", true, {
            { "mode",      PK::Enum,  {0,0,0,0}, 0, 0, "direction|radial|shape" },
            { "direction", PK::Vec3,  {0,1,0,0}, -1000.0f, 1000.0f, nullptr },
            { "speed",     PK::Range, {1.0f,1.0f,0,0}, -1000.0f, 1000.0f, nullptr },
            { "spread",    PK::Float, {0,0,0,0}, 0, 180.0f, nullptr },
            { "inherit",   PK::Float, {0,0,0,0}, 0, 1.0f, nullptr },
        }},
        { "lifetime", true, {
            { "range", PK::Range, {1.0f,1.0f,0,0}, 0.01f, 600.0f, nullptr },
        }},
        { "size", true, {
            { "start", PK::Range, {0.1f,0.1f,0,0}, 0, 1000.0f, nullptr },
            { "end",   PK::Range, {0.1f,0.1f,0,0}, 0, 1000.0f, nullptr },
        }},
        { "color", true, {
            { "start",     PK::Vec4,  {1,1,1,1}, 0, 1000.0f, nullptr },
            { "end",       PK::Vec4,  {1,1,1,0}, 0, 1000.0f, nullptr },
            { "variation", PK::Float, {0,0,0,0}, 0, 1.0f, nullptr },
        }},
        { "rotation", true, {
            { "angle", PK::Range, {0,0,0,0}, -3600.0f, 3600.0f, nullptr },
            { "rate",  PK::Range, {0,0,0,0}, -3600.0f, 3600.0f, nullptr },
        }},
        // ---- update ----
        { "gravity", false, {
            { "value", PK::Vec3, {0,-9.8f,0,0}, -kBig, kBig, nullptr },
        }},
        { "drag", false, {
            { "coefficient", PK::Float, {1.0f,0,0,0}, 0, 1000.0f, nullptr },
        }},
        { "curlNoise", false, {
            { "strength", PK::Float, {1.0f,0,0,0}, -1000.0f, 1000.0f, nullptr },
            { "scale",    PK::Float, {1.0f,0,0,0}, 0.001f, 1000.0f, nullptr },
            { "speed",    PK::Float, {0.5f,0,0,0}, -100.0f, 100.0f, nullptr },
        }},
        { "vortex", false, {
            { "axis",     PK::Vec3,  {0,1,0,0}, -1000.0f, 1000.0f, nullptr },
            { "center",   PK::Vec3,  {0,0,0,0}, -kBig, kBig, nullptr },
            { "strength", PK::Float, {1.0f,0,0,0}, -1000.0f, 1000.0f, nullptr },
            { "radius",   PK::Float, {5.0f,0,0,0}, 0.001f, kBig, nullptr },
        }},
        { "attractor", false, {
            { "position",   PK::Vec3,  {0,0,0,0}, -kBig, kBig, nullptr },
            { "strength",   PK::Float, {1.0f,0,0,0}, -1000.0f, 1000.0f, nullptr },
            { "radius",     PK::Float, {5.0f,0,0,0}, 0.001f, kBig, nullptr },
            { "softening",  PK::Float, {0.1f,0,0,0}, 0.001f, 1000.0f, nullptr },
        }},
        { "floor", false, {
            { "height",   PK::Float, {0,0,0,0}, -kBig, kBig, nullptr },
            { "bounce",   PK::Float, {0.4f,0,0,0}, 0, 1.0f, nullptr },
            { "friction", PK::Float, {0.2f,0,0,0}, 0, 1.0f, nullptr },
            { "kill",     PK::Bool,  {0,0,0,0}, 0, 1, nullptr },
        }},
    };
    return s;
}

const ModuleSpec* FindSpec(const std::string& name, bool isInit)
{
    for (const ModuleSpec& m : Specs())
        if (m.isInit == isInit && name == m.name) return &m;
    return nullptr;
}

std::vector<std::string> EnumList(const char* e)
{
    std::vector<std::string> v;
    std::string cur;
    for (const char* p = e; ; ++p)
    {
        if (*p == '|' || *p == 0) { v.push_back(cur); cur.clear(); if (!*p) break; }
        else cur.push_back(*p);
    }
    return v;
}

bool FiniteIn(double v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

// 1 つのパラメータを読んで正規形を out へ。入力が無ければ既定値。エラーなら err を埋めて false。
bool NormalizeParam(const ParamSpec& s, const json* in, json& out, std::string& err)
{
    auto fail = [&](const std::string& m) { err = std::string(s.key) + ": " + m; return false; };
    auto num = [&](const json& j, float& v) -> bool
    {
        if (!j.is_number()) return false;
        v = j.get<float>();
        return true;
    };
    switch (s.kind)
    {
    case PK::Float:
    {
        float v = s.d[0];
        if (in && !num(*in, v)) return fail("数値が要る");
        if (!FiniteIn(v, s.lo, s.hi)) return fail("範囲 [" + std::to_string(s.lo) + ", " + std::to_string(s.hi) + "] の外");
        out = v;
        return true;
    }
    case PK::Bool:
    {
        bool v = s.d[0] != 0.0f;
        if (in)
        {
            if (!in->is_boolean()) return fail("true / false が要る");
            v = in->get<bool>();
        }
        out = v;
        return true;
    }
    case PK::Enum:
    {
        const std::vector<std::string> vals = EnumList(s.enums);
        std::string v = vals.front();
        if (in)
        {
            if (!in->is_string()) return fail("文字列が要る");
            v = in->get<std::string>();
        }
        if (std::find(vals.begin(), vals.end(), v) == vals.end())
        {
            std::string all;
            for (const auto& x : vals) all += (all.empty() ? "" : " | ") + x;
            return fail("'" + v + "' は使えない（" + all + "）");
        }
        out = v;
        return true;
    }
    case PK::Vec3:
    case PK::Vec4:
    {
        const size_t n = (s.kind == PK::Vec3) ? 3 : 4;
        std::vector<float> v(s.d, s.d + n);
        if (in)
        {
            if (!in->is_array() || in->size() != n) return fail(std::to_string(n) + " 要素の配列が要る");
            for (size_t i = 0; i < n; ++i)
                if (!num((*in)[i], v[i])) return fail("数値の配列が要る");
        }
        for (float x : v)
            if (!FiniteIn(x, s.lo, s.hi)) return fail("範囲の外");
        out = v;
        return true;
    }
    case PK::Range:
    {
        float a = s.d[0], b = s.d[1];
        if (in)
        {
            if (in->is_number()) { a = b = in->get<float>(); }
            else if (in->is_array() && in->size() == 2 && (*in)[0].is_number() && (*in)[1].is_number())
            { a = (*in)[0].get<float>(); b = (*in)[1].get<float>(); }
            else return fail("数値か [最小, 最大] が要る");
        }
        if (!FiniteIn(a, s.lo, s.hi) || !FiniteIn(b, s.lo, s.hi)) return fail("範囲の外");
        if (a > b) return fail("最小が最大より大きい");
        out = std::vector<float>{a, b};
        return true;
    }
    }
    return fail("内部エラー");
}

bool NormalizeModule(const json& in, bool isInit, ModuleDesc& out, std::string& err, std::vector<std::string>& warnings)
{
    if (!in.is_object()) { err = "モジュールはオブジェクト"; return false; }
    if (!in.contains("module") || !in["module"].is_string()) { err = "\"module\" の名前が要る"; return false; }
    const std::string name = in["module"].get<std::string>();
    const ModuleSpec* spec = FindSpec(name, isInit);
    if (!spec)
    {
        std::string all;
        for (const auto& n : isInit ? InitModuleNames() : UpdateModuleNames()) all += (all.empty() ? "" : ", ") + n;
        err = "モジュール '" + name + "' は " + (isInit ? "init" : "update") + " に無い（使えるのは " + all + "）";
        return false;
    }
    out.module = name;
    out.params = json::object();
    for (const ParamSpec& p : spec->params)
    {
        const json* v = in.contains(p.key) ? &in[p.key] : nullptr;
        json norm;
        std::string perr;
        if (!NormalizeParam(p, v, norm, perr)) { err = name + "." + perr; return false; }
        out.params[p.key] = std::move(norm);
    }
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        if (it.key() == "module") continue;
        bool known = false;
        for (const ParamSpec& p : spec->params) if (it.key() == p.key) { known = true; break; }
        if (!known) warnings.push_back(name + ": 未知のキー '" + it.key() + "' は無視した");
    }
    return true;
}

float F(const json& j, const char* k) { return j[k].get<float>(); }
void V3(const json& j, const char* k, float* o) { for (int i = 0; i < 3; ++i) o[i] = j[k][i].get<float>(); }
void V4(const json& j, const char* k, float* o) { for (int i = 0; i < 4; ++i) o[i] = j[k][i].get<float>(); }
float R(const json& j, const char* k, int i) { return j[k][i].get<float>(); }

int EnumIndex(const ModuleSpec& spec, const char* key, const std::string& val)
{
    for (const ParamSpec& p : spec.params)
        if (std::strcmp(p.key, key) == 0)
        {
            const auto vals = EnumList(p.enums);
            for (size_t i = 0; i < vals.size(); ++i) if (vals[i] == val) return static_cast<int>(i);
        }
    return 0;
}

bool CompileModule(const ModuleDesc& m, bool isInit, GOp& op, std::string& err)
{
    const ModuleSpec* spec = FindSpec(m.module, isInit);
    if (!spec) { err = "モジュール '" + m.module + "' が無い"; return false; }
    std::memset(&op, 0, sizeof(op));
    const json& p = m.params;
    const std::string& n = m.module;
    if (n == "shape")
    {
        const int kind = EnumIndex(*spec, "kind", p["kind"].get<std::string>());
        op.op = kOpShape;
        op.a[0] = static_cast<float>(kind);
        op.a[1] = p["surface"].get<bool>() ? 1.0f : 0.0f;
        op.a[2] = F(p, "angle");
        float size[3]; V3(p, "size", size);
        if (kind == 1 || kind == 3 || kind == 4) { op.b[0] = F(p, "radius"); }
        else { op.b[0] = size[0]; op.b[1] = size[1]; op.b[2] = size[2]; }
        if (kind == 3) op.b[3] = F(p, "length");
        op.c[0] = F(p, "innerRadius");
    }
    else if (n == "velocity")
    {
        op.op = kOpVelocity;
        op.a[0] = static_cast<float>(EnumIndex(*spec, "mode", p["mode"].get<std::string>()));
        op.a[1] = F(p, "spread");
        op.a[2] = F(p, "inherit");
        V3(p, "direction", op.b);
        op.c[0] = R(p, "speed", 0); op.c[1] = R(p, "speed", 1);
    }
    else if (n == "lifetime")
    {
        op.op = kOpLifetime;
        op.c[0] = R(p, "range", 0); op.c[1] = R(p, "range", 1);
    }
    else if (n == "size")
    {
        op.op = kOpSize;
        op.b[0] = R(p, "start", 0); op.b[1] = R(p, "start", 1);
        op.b[2] = R(p, "end", 0);   op.b[3] = R(p, "end", 1);
    }
    else if (n == "color")
    {
        op.op = kOpColor;
        V4(p, "start", op.b); V4(p, "end", op.c);
        op.a[0] = F(p, "variation");
    }
    else if (n == "rotation")
    {
        op.op = kOpRotation;
        op.b[0] = R(p, "angle", 0); op.b[1] = R(p, "angle", 1);
        op.b[2] = R(p, "rate", 0);  op.b[3] = R(p, "rate", 1);
    }
    else if (n == "gravity")
    {
        op.op = kOpGravity; V3(p, "value", op.a);
    }
    else if (n == "drag")
    {
        op.op = kOpDrag; op.a[0] = F(p, "coefficient");
    }
    else if (n == "curlNoise")
    {
        op.op = kOpCurl; op.a[0] = F(p, "strength"); op.a[1] = F(p, "scale"); op.a[2] = F(p, "speed");
    }
    else if (n == "vortex")
    {
        op.op = kOpVortex;
        float axis[3]; V3(p, "axis", axis);
        const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (len < 1e-6f) { err = "vortex.axis が 0 ベクトル"; return false; }
        for (int i = 0; i < 3; ++i) op.a[i] = axis[i] / len;
        V3(p, "center", op.b); op.b[3] = F(p, "strength");
        op.c[0] = F(p, "radius");
    }
    else if (n == "attractor")
    {
        op.op = kOpAttract;
        V3(p, "position", op.b); op.b[3] = F(p, "strength");
        op.c[0] = F(p, "radius"); op.c[1] = F(p, "softening");
    }
    else if (n == "floor")
    {
        op.op = kOpFloor;
        op.a[0] = F(p, "height"); op.a[1] = F(p, "bounce"); op.a[2] = F(p, "friction");
        op.b[0] = p["kill"].get<bool>() ? 1.0f : 0.0f;
    }
    else { err = "モジュール '" + n + "' のコンパイルが未実装"; return false; }
    return true;
}

// ---- render ----
bool ParseRender(const json& in, RenderDesc& r, std::string& err, std::vector<std::string>& warnings)
{
    if (!in.is_object()) { err = "render はオブジェクト"; return false; }
    auto strEnum = [&](const char* key, const char* vals, int& idx) -> bool
    {
        idx = 0;
        if (!in.contains(key)) return true;
        if (!in[key].is_string()) { err = std::string("render.") + key + ": 文字列が要る"; return false; }
        const auto list = EnumList(vals);
        const std::string v = in[key].get<std::string>();
        const auto it = std::find(list.begin(), list.end(), v);
        if (it == list.end())
        {
            std::string all; for (const auto& x : list) all += (all.empty() ? "" : " | ") + x;
            err = std::string("render.") + key + ": '" + v + "' は使えない（" + all + "）";
            return false;
        }
        idx = static_cast<int>(it - list.begin());
        return true;
    };
    int k = 0, b = 0, f = 0;
    if (!strEnum("fade", "inout|none|linear|quad", f)) return false;
    if (!strEnum("kind", "sprite|flipbook", k)) return false;
    if (!strEnum("blend", "additive|alpha", b)) return false;
    r.kind  = static_cast<RenderKind>(k);
    r.blend = static_cast<Blend>(b);
    {
        static const Fade map[] = { Fade::InOut, Fade::None, Fade::Linear, Fade::Quad };
        r.fade = map[f];
    }
    auto numKey = [&](const char* key, float& v, float lo, float hi) -> bool
    {
        if (!in.contains(key)) return true;
        if (!in[key].is_number()) { err = std::string("render.") + key + ": 数値が要る"; return false; }
        const float x = in[key].get<float>();
        if (!FiniteIn(x, lo, hi)) { err = std::string("render.") + key + ": 範囲の外"; return false; }
        v = x;
        return true;
    };
    if (!numKey("softDistance", r.softDistance, 0.0f, 1000.0f)) return false;
    if (!numKey("intensity", r.intensity, 0.0f, 10000.0f)) return false;
    if (!numKey("stretch", r.stretch, 0.0f, 100.0f)) return false;
    if (in.contains("texture"))
    {
        if (!in["texture"].is_string()) { err = "render.texture: 文字列が要る"; return false; }
        r.texture = in["texture"].get<std::string>();
    }
    if (in.contains("flipbook"))
    {
        const json& fb = in["flipbook"];
        if (!fb.is_object()) { err = "render.flipbook はオブジェクト"; return false; }
        if (fb.contains("cols"))
        {
            if (!fb["cols"].is_number_integer() || fb["cols"].get<int>() < 1 || fb["cols"].get<int>() > 64) { err = "render.flipbook.cols は 1..64 の整数"; return false; }
            r.flipCols = fb["cols"].get<u32>();
        }
        if (fb.contains("rows"))
        {
            if (!fb["rows"].is_number_integer() || fb["rows"].get<int>() < 1 || fb["rows"].get<int>() > 64) { err = "render.flipbook.rows は 1..64 の整数"; return false; }
            r.flipRows = fb["rows"].get<u32>();
        }
        if (fb.contains("fps"))
        {
            if (!fb["fps"].is_number() || !FiniteIn(fb["fps"].get<float>(), 0.0f, 1000.0f)) { err = "render.flipbook.fps は 0..1000"; return false; }
            r.flipFps = fb["fps"].get<float>();
        }
        if (fb.contains("frames"))
        {
            if (!fb["frames"].is_number_integer() || fb["frames"].get<int>() < 1 || fb["frames"].get<u32>() > r.flipCols * r.flipRows)
            { err = "render.flipbook.frames は 1..cols*rows の整数"; return false; }
            r.flipFrames = fb["frames"].get<u32>();
        }
        auto fnum = [&](const char* key, float& v, float lo, float hi) -> bool
        {
            if (!fb.contains(key)) return true;
            if (!fb[key].is_number() || !FiniteIn(fb[key].get<float>(), lo, hi)) { err = std::string("render.flipbook.") + key + ": 範囲の外"; return false; }
            v = fb[key].get<float>();
            return true;
        };
        auto fstr = [&](const char* key, std::string& v) -> bool
        {
            if (!fb.contains(key)) return true;
            if (!fb[key].is_string()) { err = std::string("render.flipbook.") + key + ": 文字列が要る"; return false; }
            v = fb[key].get<std::string>();
            return true;
        };
        if (!fnum("mvRange", r.flipMvRange, 0.0f, 8.0f) || !fnum("tempMin", r.flipTempMin, 500.0f, 20000.0f) ||
            !fnum("tempMax", r.flipTempMax, 500.0f, 20000.0f) || !fnum("emission", r.flipEmission, 0.0f, 1000.0f) ||
            !fnum("erodeStart", r.flipErodeStart, 0.0f, 1.0f) || !fnum("rootFade", r.flipRootFade, 0.0f, 0.5f) || !fnum("erodeWidth", r.flipErodeWidth, 0.001f, 1.0f) ||
            !fstr("mv", r.flipMv) || !fstr("dbf", r.flipDbf) || !fstr("channels", r.flipChannels)) return false;
        if (r.flipChannels != "fire" && r.flipChannels != "smoke6way" && r.flipChannels != "color")
        { err = "render.flipbook.channels: '" + r.flipChannels + "' は使えない（fire | smoke6way | color）"; return false; }
        if (r.kind == RenderKind::Flipbook && r.flipChannels == "smoke6way" && r.flipDbf.empty())
        { err = "render.flipbook: channels が smoke6way なら dbf が要る"; return false; }
        if (r.flipTempMax < r.flipTempMin) { err = "render.flipbook.tempMax は tempMin 以上"; return false; }
        if (fb.contains("anchor"))
        {
            const std::string a = fb["anchor"].is_string() ? fb["anchor"].get<std::string>() : std::string();
            if (a != "bottom" && a != "center") { err = "render.flipbook.anchor は bottom | center"; return false; }
            r.flipAnchorSet = true; r.flipAnchorBottom = (a == "bottom");
        }
        for (auto it = fb.begin(); it != fb.end(); ++it)
        {
            static const char* fk[] = { "cols", "rows", "fps", "frames", "channels", "dbf", "mv", "mvRange", "anchor", "tempMin", "tempMax", "emission", "erodeStart", "erodeWidth", "rootFade" };
            bool ok = false;
            for (const char* k2 : fk) if (it.key() == k2) ok = true;
            if (!ok) warnings.push_back("render.flipbook: 未知のキー '" + it.key() + "' は無視した");
        }
    }
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        static const char* known[] = { "kind", "blend", "fade", "softDistance", "intensity", "stretch", "texture", "flipbook" };
        bool ok = false;
        for (const char* k2 : known) if (it.key() == k2) ok = true;
        if (!ok) warnings.push_back("render: 未知のキー '" + it.key() + "' は無視した");
    }
    return true;
}

bool ParseEmitter(const json& in, EmitterDesc& e, std::string& err, std::vector<std::string>& warnings)
{
    if (!in.is_object()) { err = "エミッターはオブジェクト"; return false; }
    if (in.contains("name") && in["name"].is_string()) e.name = in["name"].get<std::string>();
    if (in.contains("maxParticles"))
    {
        if (!in["maxParticles"].is_number_integer()) { err = "maxParticles は整数"; return false; }
        const i64 v = in["maxParticles"].get<i64>();
        if (v < 1 || v > static_cast<i64>(kPoolCapacity)) { err = "maxParticles は 1.." + std::to_string(kPoolCapacity); return false; }
        e.maxParticles = static_cast<u32>(v);
    }
    if (in.contains("spawn"))
    {
        const json& s = in["spawn"];
        if (!s.is_object()) { err = "spawn はオブジェクト"; return false; }
        if (s.contains("rate"))
        {
            if (!s["rate"].is_number() || !FiniteIn(s["rate"].get<float>(), 0.0f, 1.0e6f)) { err = "spawn.rate は 0..1e6"; return false; }
            e.spawn.rate = s["rate"].get<float>();
        }
        if (s.contains("startDelay"))
        {
            if (!s["startDelay"].is_number() || !FiniteIn(s["startDelay"].get<float>(), 0.0f, 3600.0f)) { err = "spawn.startDelay は 0..3600"; return false; }
            e.spawn.startDelay = s["startDelay"].get<float>();
        }
        if (s.contains("bursts"))
        {
            if (!s["bursts"].is_array()) { err = "spawn.bursts は配列"; return false; }
            for (size_t i = 0; i < s["bursts"].size(); ++i)
            {
                const json& b = s["bursts"][i];
                if (!b.is_object() || !b.contains("t") || !b["t"].is_number() || !b.contains("count") || !b["count"].is_number_integer())
                { err = "spawn.bursts[" + std::to_string(i) + "]: { \"t\": 秒, \"count\": 整数 } が要る"; return false; }
                BurstDesc bd;
                bd.t = b["t"].get<float>();
                const i64 c = b["count"].get<i64>();
                if (!FiniteIn(bd.t, 0.0f, 3600.0f) || c < 0 || c > static_cast<i64>(kPoolCapacity))
                { err = "spawn.bursts[" + std::to_string(i) + "]: t は 0..3600、count は 0.." + std::to_string(kPoolCapacity); return false; }
                bd.count = static_cast<u32>(c);
                e.spawn.bursts.push_back(bd);
            }
            std::stable_sort(e.spawn.bursts.begin(), e.spawn.bursts.end(), [](const BurstDesc& a, const BurstDesc& b) { return a.t < b.t; });
        }
    }
    for (int stage = 0; stage < 2; ++stage)
    {
        const bool isInit = stage == 0;
        const char* key = isInit ? "init" : "update";
        auto& list = isInit ? e.init : e.update;
        if (!in.contains(key)) continue;
        if (!in[key].is_array()) { err = std::string(key) + " は配列"; return false; }
        if (in[key].size() > kMaxOpsPerStage) { err = std::string(key) + " のモジュールは " + std::to_string(kMaxOpsPerStage) + " 個まで"; return false; }
        for (size_t i = 0; i < in[key].size(); ++i)
        {
            ModuleDesc m;
            std::string merr;
            if (!NormalizeModule(in[key][i], isInit, m, merr, warnings)) { err = std::string(key) + "[" + std::to_string(i) + "]: " + merr; return false; }
            list.push_back(std::move(m));
        }
    }
    if (in.contains("render"))
    {
        std::string rerr;
        if (!ParseRender(in["render"], e.render, rerr, warnings)) { err = rerr; return false; }
    }
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        static const char* known[] = { "name", "maxParticles", "spawn", "init", "update", "render" };
        bool ok = false;
        for (const char* k : known) if (it.key() == k) ok = true;
        if (!ok) warnings.push_back("emitter: 未知のキー '" + it.key() + "' は無視した");
    }
    return true;
}

} // namespace

const std::vector<std::string>& InitModuleNames()
{
    static const std::vector<std::string> v = [] { std::vector<std::string> r; for (const auto& m : Specs()) if (m.isInit) r.push_back(m.name); return r; }();
    return v;
}
const std::vector<std::string>& UpdateModuleNames()
{
    static const std::vector<std::string> v = [] { std::vector<std::string> r; for (const auto& m : Specs()) if (!m.isInit) r.push_back(m.name); return r; }();
    return v;
}

ParseResult ParseUvfx(const std::string& text)
{
    ParseResult res;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) { res.error = "JSON として読めない"; return res; }
    if (!j.is_object()) { res.error = "ルートはオブジェクト"; return res; }
    if (!j.contains("uvfx") || !j["uvfx"].is_number_integer() || j["uvfx"].get<int>() != 1)
    { res.error = "\"uvfx\": 1（版）が要る"; return res; }
    VfxAsset& a = res.asset;
    if (j.contains("name") && j["name"].is_string()) a.name = j["name"].get<std::string>();
    if (j.contains("duration"))
    {
        if (!j["duration"].is_number() || !FiniteIn(j["duration"].get<float>(), 0.0f, 36000.0f)) { res.error = "duration は 0..36000"; return res; }
        a.duration = j["duration"].get<float>();
    }
    if (j.contains("loop"))
    {
        if (!j["loop"].is_boolean()) { res.error = "loop は true / false"; return res; }
        a.loop = j["loop"].get<bool>();
    }
    if (!j.contains("emitters") || !j["emitters"].is_array() || j["emitters"].empty()) { res.error = "emitters（1 個以上の配列）が要る"; return res; }
    if (j["emitters"].size() > kMaxEmittersPerAsset) { res.error = "emitters は " + std::to_string(kMaxEmittersPerAsset) + " 個まで"; return res; }
    for (size_t i = 0; i < j["emitters"].size(); ++i)
    {
        EmitterDesc e;
        std::string err;
        if (!ParseEmitter(j["emitters"][i], e, err, res.warnings)) { res.error = "emitters[" + std::to_string(i) + "]: " + err; return res; }
        // GPU の命令に落ちること（引数の相互チェック）も読み込み時に確かめる
        CompiledEmitter ce;
        if (!CompileEmitter(e, ce, err)) { res.error = "emitters[" + std::to_string(i) + "]: " + err; return res; }
        a.emitters.push_back(std::move(e));
    }
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        static const char* known[] = { "uvfx", "name", "duration", "loop", "emitters" };
        bool ok = false;
        for (const char* k : known) if (it.key() == k) ok = true;
        if (!ok) res.warnings.push_back("未知のキー '" + it.key() + "' は無視した");
    }
    res.ok = true;
    return res;
}

std::string SerializeUvfx(const VfxAsset& a)
{
    json j = json::object();
    j["uvfx"] = 1;
    j["name"] = a.name;
    j["duration"] = a.duration;
    j["loop"] = a.loop;
    j["emitters"] = json::array();
    for (const EmitterDesc& e : a.emitters)
    {
        json je = json::object();
        je["name"] = e.name;
        je["maxParticles"] = e.maxParticles;
        json sp = json::object();
        sp["rate"] = e.spawn.rate;
        sp["startDelay"] = e.spawn.startDelay;
        sp["bursts"] = json::array();
        for (const BurstDesc& b : e.spawn.bursts) sp["bursts"].push_back({ {"t", b.t}, {"count", b.count} });
        je["spawn"] = sp;
        auto mods = [](const std::vector<ModuleDesc>& v)
        {
            json arr = json::array();
            for (const ModuleDesc& m : v)
            {
                json o = json::object();
                o["module"] = m.module;
                for (auto it = m.params.begin(); it != m.params.end(); ++it) o[it.key()] = it.value();
                arr.push_back(std::move(o));
            }
            return arr;
        };
        je["init"] = mods(e.init);
        je["update"] = mods(e.update);
        json r = json::object();
        r["kind"]  = e.render.kind == RenderKind::Flipbook ? "flipbook" : "sprite";
        r["blend"] = e.render.blend == Blend::Alpha ? "alpha" : "additive";
        switch (e.render.fade)
        {
        case Fade::None:   r["fade"] = "none"; break;
        case Fade::Linear: r["fade"] = "linear"; break;
        case Fade::Quad:   r["fade"] = "quad"; break;
        default:           r["fade"] = "inout"; break;
        }
        r["softDistance"] = e.render.softDistance;
        r["intensity"] = e.render.intensity;
        r["stretch"] = e.render.stretch;
        if (!e.render.texture.empty()) r["texture"] = e.render.texture;
        if (e.render.kind == RenderKind::Flipbook || e.render.flipCols != 1 || e.render.flipRows != 1 || e.render.flipFps != 0.0f)
        {
            json fb = { {"cols", e.render.flipCols}, {"rows", e.render.flipRows}, {"fps", e.render.flipFps} };
            if (e.render.flipFrames != 0) fb["frames"] = e.render.flipFrames;
            if (e.render.kind == RenderKind::Flipbook)
            {
                fb["channels"] = e.render.flipChannels;
                if (!e.render.flipDbf.empty()) fb["dbf"] = e.render.flipDbf;
                if (!e.render.flipMv.empty()) fb["mv"] = e.render.flipMv;
                fb["mvRange"] = e.render.flipMvRange;
                fb["anchor"] = e.render.AnchorBottom() ? "bottom" : "center";
                fb["tempMin"] = e.render.flipTempMin; fb["tempMax"] = e.render.flipTempMax; fb["emission"] = e.render.flipEmission;
                fb["erodeStart"] = e.render.flipErodeStart; fb["erodeWidth"] = e.render.flipErodeWidth;
                fb["rootFade"] = e.render.flipRootFade;
            }
            r["flipbook"] = std::move(fb);
        }
        je["render"] = r;
        j["emitters"].push_back(std::move(je));
    }
    return j.dump(2);
}

bool CompileEmitter(const EmitterDesc& e, CompiledEmitter& out, std::string& error)
{
    out = CompiledEmitter{};
    float maxLife = 1.0f;   // lifetime モジュールが無いときの既定（1 秒）
    for (int stage = 0; stage < 2; ++stage)
    {
        const bool isInit = stage == 0;
        const auto& src = isInit ? e.init : e.update;
        auto& dst = isInit ? out.init : out.update;
        for (size_t i = 0; i < src.size(); ++i)
        {
            GOp op{};
            std::string err;
            if (!CompileModule(src[i], isInit, op, err))
            {
                error = std::string(isInit ? "init[" : "update[") + std::to_string(i) + "]: " + err;
                return false;
            }
            if (isInit && op.op == kOpLifetime) maxLife = op.c[1];
            dst.push_back(op);
        }
    }
    out.maxLife = maxLife;
    return true;
}

} // namespace dx12e::vfx
