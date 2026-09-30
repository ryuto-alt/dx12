#include "renderer/matgraph/PbrTemplate.h"

#include <cstdio>

namespace dx12e::matgraph
{
namespace
{

// 文字列の FNV-1a 32（テンプレートの GUID を名前から決定論に作る）
uint32_t Fnv32(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

struct Flags
{
    int n = 0, m = 0, e = 0;
};

Flags FlagsOf(const LegacyPbr& p)
{
    Flags f;
    f.n = p.normal.empty() ? 0 : 1;
    f.m = p.metalRoughness.empty() ? 0 : 1;
    f.e = !PbrHasEmissive(p) ? 0 : (p.emissive.empty() ? 1 : 2);
    return f;
}

} // namespace

bool PbrHasEmissive(const LegacyPbr& m)
{
    return m.emissiveIntensity > 0.0f || !m.emissive.empty();
}

std::string PbrTemplateName(const LegacyPbr& m)
{
    const Flags f = FlagsOf(m);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "pbr_std_n%dm%de%d", f.n, f.m, f.e);
    return buf;
}

void BuildPbrTemplateGraph(const LegacyPbr& m, MaterialGraph& g)
{
    const Flags f = FlagsOf(m);
    const std::string name = PbrTemplateName(m);

    g.Clear();
    char guid[16];
    std::snprintf(guid, sizeof(guid), "%08x", Fnv32(name));
    g.SetGuid(guid);
    g.SetName(name);
    GraphSettings st;                 // Opaque / DefaultLit / 片面設定は無視（エンジンは両面描画）
    g.SetSettings(st);

    auto add = [&](const char* type, const char* id, float x, float yy) { g.AddNode(type, x, yy, id); };
    auto setp = [&](const char* id, const char* key, const Json& v) { g.SetNodeProp(id, key, v); };
    auto link = [&](const char* from, const char* fromPin, const char* to, const char* toPin) {
        g.Connect(from, fromPin, to, toPin);
    };
    auto texParam = [&](const char* id, const char* pname, const char* sampler, float x, float yy, int prio) {
        add("TextureParameter", id, x, yy);
        setp(id, "name", pname);
        setp(id, "sampler", sampler);
        setp(id, "group", "Textures");
        setp(id, "priority", prio);
    };
    auto scalarParam = [&](const char* id, const char* pname, float def, float lo, float hi, float x, float yy, int prio) {
        add("ScalarParameter", id, x, yy);
        setp(id, "name", pname);
        setp(id, "default", def);
        setp(id, "range", Json::array({lo, hi}));
        setp(id, "group", "Surface");
        setp(id, "priority", prio);
    };

    // ---- 座標と BaseColor ----------------------------------------------------------
    add("MaterialOutput", "out", 700.0f, 100.0f);
    add("TexCoord", "uv", -520.0f, 0.0f);
    texParam("tAlb", "Albedo", "Color", -520.0f, -160.0f, 0);
    add("TextureSample", "sAlb", -260.0f, -60.0f);
    link("tAlb", "Out", "sAlb", "Tex");
    link("uv", "UV", "sAlb", "UV");
    add("VertexColor", "vc", -260.0f, 100.0f);
    add("Multiply", "mAlb", 20.0f, -40.0f);
    link("sAlb", "RGB", "mAlb", "A");
    link("vc", "RGB", "mAlb", "B");
    link("mAlb", "Out", "out", "BaseColor");

    // ---- 法線 ---------------------------------------------------------------------
    if (f.n)
    {
        texParam("tNrm", "Normal", "Normal", -520.0f, 200.0f, 1);
        add("NormalMap", "nNrm", 20.0f, 220.0f);
        link("tNrm", "Out", "nNrm", "Tex");
        link("uv", "UV", "nNrm", "UV");
        g.SetLiteral("nNrm", "Strength", Value::Float(1.0f));
        link("nNrm", "Out", "out", "Normal");
    }

    // ---- ラフネス / メタリック --------------------------------------------------------
    scalarParam("pRough", "Roughness", 1.0f, 0.0f, 1.0f, -260.0f, 320.0f, 0);
    scalarParam("pMetal", "Metallic", 1.0f, 0.0f, 1.0f, -260.0f, 420.0f, 1);
    if (f.m)
    {
        texParam("tMr", "MetalRoughness", "LinearColor", -520.0f, 340.0f, 2);
        add("TextureSample", "sMr", -260.0f, 500.0f);
        link("tMr", "Out", "sMr", "Tex");
        link("uv", "UV", "sMr", "UV");
        add("Multiply", "mRough", 20.0f, 340.0f);
        link("sMr", "G", "mRough", "A");
        link("pRough", "Out", "mRough", "B");
        add("Multiply", "mMetal", 20.0f, 440.0f);
        link("sMr", "B", "mMetal", "A");
        link("pMetal", "Out", "mMetal", "B");
        link("mRough", "Out", "out", "Roughness");
        link("mMetal", "Out", "out", "Metallic");
    }
    else
    {
        link("pRough", "Out", "out", "Roughness");
        link("pMetal", "Out", "out", "Metallic");
    }

    // ---- 発光 ---------------------------------------------------------------------
    if (f.e)
    {
        add("VectorParameter", "pEmiC", -260.0f, 620.0f);
        setp("pEmiC", "name", "EmissiveColor");
        setp("pEmiC", "default", Json::array({1.0f, 1.0f, 1.0f, 1.0f}));
        setp("pEmiC", "group", "Emissive");
        setp("pEmiC", "priority", 0);
        scalarParam("pEmiI", "EmissiveIntensity", 1.0f, 0.0f, 64.0f, -260.0f, 740.0f, 1);
        add("Multiply", "eMul2", 300.0f, 660.0f);
        if (f.e == 2)
        {
            texParam("tEmi", "Emissive", "Color", -520.0f, 600.0f, 3);
            add("TextureSample", "sEmi", -260.0f, 860.0f);
            link("tEmi", "Out", "sEmi", "Tex");
            link("uv", "UV", "sEmi", "UV");
            add("Multiply", "eMul1", 20.0f, 620.0f);
            link("sEmi", "RGB", "eMul1", "A");
            link("pEmiC", "RGB", "eMul1", "B");
            link("eMul1", "Out", "eMul2", "A");
        }
        else
        {
            link("pEmiC", "RGB", "eMul2", "A");
        }
        link("pEmiI", "Out", "eMul2", "B");
        link("eMul2", "Out", "out", "Emissive");
    }
}

std::vector<PbrParam> PbrTemplateParams(const LegacyPbr& m)
{
    const Flags f = FlagsOf(m);
    std::vector<PbrParam> out;
    auto tex = [&](const char* name, const std::string& path) {
        PbrParam p; p.name = name; p.kind = PbrParam::Kind::Texture; p.texture = path;
        out.push_back(std::move(p));
    };
    auto scalar = [&](const char* name, float v) {
        PbrParam p; p.name = name; p.kind = PbrParam::Kind::Scalar; p.n = 1; p.v[0] = v;
        out.push_back(std::move(p));
    };
    if (!m.albedo.empty()) tex("Albedo", m.albedo);
    if (f.n) tex("Normal", m.normal);
    if (f.m) tex("MetalRoughness", m.metalRoughness);
    scalar("Roughness", m.roughness);
    scalar("Metallic", m.metallic);
    if (f.e)
    {
        if (f.e == 2) tex("Emissive", m.emissive);
        PbrParam c; c.name = "EmissiveColor"; c.kind = PbrParam::Kind::Vector; c.n = 3;
        c.v[0] = m.emissiveColor[0]; c.v[1] = m.emissiveColor[1]; c.v[2] = m.emissiveColor[2];
        out.push_back(std::move(c));
        scalar("EmissiveIntensity", m.emissiveIntensity);
    }
    return out;
}

} // namespace dx12e::matgraph
