#include "editor/nodegraph/SandboxSamples.h"

#include <random>

namespace dx12e::ng
{

namespace
{
struct Builder
{
    GraphDocument& doc;
    SandboxGraphModel& m;
    SampleReport rep;

    NodeId N(const char* type, float x, float y)
    {
        const NodeId id = doc.AddNode(type, Vec2(x, y));
        if (id) ++rep.nodes;
        return id;
    }
    // a の出力 outName → b の入力 inName
    void L(NodeId a, const char* outName, NodeId b, const char* inName)
    {
        const NodeData* na = m.FindNode(a);
        const NodeData* nb = m.FindNode(b);
        if (!na || !nb) { ++rep.failedLinks; return; }
        const int o = m.OutputIndex(na->type, outName);
        const int i = m.InputIndex(nb->type, inName);
        if (o < 0 || i < 0 || !doc.Connect(PinRef{a, static_cast<uint16_t>(o), true}, PinRef{b, static_cast<uint16_t>(i), false}))
            ++rep.failedLinks;
    }
    void V(NodeId n, const char* inName, const PinValue& v)
    {
        const NodeData* nd = m.FindNode(n);
        if (!nd) return;
        const int i = m.InputIndex(nd->type, inName);
        if (i < 0) return;
        PinValue nv = v;
        doc.SetPinValue(PinRef{n, static_cast<uint16_t>(i), false}, nv);
    }
    static PinValue F(float a) { PinValue v; v.kind = ValueKind::Float; v.f[0] = a; return v; }
    static PinValue E(int a) { PinValue v; v.kind = ValueKind::Enum; v.f[0] = static_cast<float>(a); return v; }
    static PinValue C(float r, float g, float b, float a = 1.0f) { PinValue v; v.kind = ValueKind::Color; v.f[0] = r; v.f[1] = g; v.f[2] = b; v.f[3] = a; return v; }
    static PinValue V2(float a, float b) { PinValue v; v.kind = ValueKind::Vec2; v.f[0] = a; v.f[1] = b; return v; }
};
} // namespace

SampleReport BuildSampleGraph(GraphDocument& doc, SandboxGraphModel& model)
{
    doc.Clear();
    Builder b{doc, model, {}};

    // ---- 列 0: 座標と時間 ----
    const NodeId uv    = b.N("texcoord", 0, 0);
    const NodeId time  = b.N("time", 0, 176);
    const NodeId speed = b.N("float", 0, 256);
    b.V(speed, "Value", Builder::F(0.02f));
    const NodeId sc    = b.N("scalarParam", 0, 352);
    b.V(sc, "Default", Builder::F(0.75f));

    // ---- 列 1 ----
    const NodeId mulT   = b.N("multiply", 288, 192);
    const NodeId app2   = b.N("append2", 288, 320);
    const NodeId panner = b.N("panner", 528, 32);
    b.L(time, "Seconds", mulT, "A");
    b.L(speed, "Out", mulT, "B");
    b.L(mulT, "Out", app2, "A");
    b.L(app2, "XY", panner, "Speed");
    b.L(uv, "UV", panner, "UV");

    // ---- 列 2: テクスチャ ----
    const NodeId texR = b.N("texture", 528, 208);
    b.V(texR, "Asset", Builder::E(1));
    const NodeId texM = b.N("texture", 528, 336);
    b.V(texM, "Asset", Builder::E(4));
    const NodeId texN = b.N("texture", 528, 464);
    b.V(texN, "Asset", Builder::E(2));

    const NodeId smpR = b.N("sample", 800, 32);
    const NodeId smpM = b.N("sample", 800, 288);
    const NodeId smpN = b.N("sample", 800, 544);
    b.L(texR, "Tex", smpR, "Tex");
    b.L(panner, "UV", smpR, "UV");
    b.L(texM, "Tex", smpM, "Tex");
    b.L(uv, "UV", smpM, "UV");
    b.L(texN, "Tex", smpN, "Tex");
    b.L(uv, "UV", smpN, "UV");

    // ---- 列 3: ノイズで苔の量を決める ----
    const NodeId noise = b.N("noise", 1088, 400);
    b.V(noise, "Scale", Builder::F(6.0f));
    const NodeId pow1  = b.N("power", 1344, 416);
    const NodeId sat1  = b.N("saturate", 1600, 432);
    b.L(uv, "UV", noise, "UV");
    b.L(noise, "Out", pow1, "Base");
    b.L(pow1, "Out", sat1, "X");

    const NodeId tint   = b.N("tint", 1088, 208);
    const NodeId lerpC  = b.N("lerp", 1344, 96);
    const NodeId desat  = b.N("desaturate", 1600, 96);
    const NodeId colorP = b.N("colorParam", 1088, 32);
    const NodeId mulC   = b.N("multiply", 1856, 96);
    b.L(smpM, "RGB", tint, "Color");
    b.L(smpR, "RGB", lerpC, "A");
    b.L(tint, "Out", lerpC, "B");
    b.L(sat1, "Out", lerpC, "Alpha");
    b.L(lerpC, "Out", desat, "Color");
    b.L(desat, "Out", mulC, "A");
    b.L(colorP, "RGB", mulC, "B");

    // ---- 粗さ・金属 ----
    const NodeId one    = b.N("oneminus", 1856, 400);
    const NodeId lerpR  = b.N("lerp", 1856, 512);
    const NodeId clampR = b.N("clamp", 2112, 512);
    b.L(sat1, "Out", one, "X");
    b.L(smpR, "R", lerpR, "A");
    b.L(one, "Out", lerpR, "B");
    b.L(sc, "Value", lerpR, "Alpha");
    b.L(lerpR, "Out", clampR, "X");
    b.V(clampR, "Min", Builder::F(0.2f));
    b.V(clampR, "Max", Builder::F(0.95f));

    // ---- 法線（リルートで長いワイヤを整える）----
    const NodeId rr1 = b.N("reroute", 1280, 650);
    const NodeId rr2 = b.N("reroute", 2048, 650);
    b.L(smpN, "RGB", rr1, "");
    b.L(rr1, "", rr2, "");

    // ---- エミッシブ（フレネル）----
    const NodeId fres  = b.N("fresnel", 1344, 736);
    b.V(fres, "Exponent", Builder::F(4.0f));
    const NodeId mulE  = b.N("multiply", 1600, 800);
    const NodeId colE  = b.N("color", 1344, 864);
    const NodeId sine  = b.N("sine", 1088, 816);
    const NodeId mulF  = b.N("multiply", 1856, 800);
    b.L(fres, "Out", mulE, "A");
    b.L(colE, "RGB", mulE, "B");
    b.L(time, "Seconds", sine, "X");
    b.L(mulE, "Out", mulF, "A");
    b.L(sine, "Out", mulF, "B");
    b.V(sine, "Period", Builder::F(3.0f));
    const NodeId sat2 = b.N("saturate", 2112, 800);
    b.L(mulF, "Out", sat2, "X");

    // ---- 出力 ----
    const NodeId out = b.N("output", 2400, 224);
    b.L(mulC, "Out", out, "BaseColor");
    b.L(clampR, "Out", out, "Roughness");
    b.L(rr2, "", out, "Normal");
    b.L(sat2, "Out", out, "Emissive");
    b.V(out, "Metallic", Builder::F(0.05f));

    // ---- コメント ----
    b.rep.comments = 0;
    auto cm = [&](std::vector<NodeId> ids, const char* title, int color) { if (doc.AddCommentAround(ids, title, color)) ++b.rep.comments; };
    cm({uv, time, speed, mulT, app2}, "UV / アニメーション", 0);
    cm({panner, texR, texM, texN, smpR, smpM, smpN}, "テクスチャ", 2);
    cm({tint, lerpC, desat, colorP, mulC}, "ベースカラー", 3);
    cm({noise, pow1, sat1, one, lerpR, clampR}, "マスクと粗さ", 1);
    cm({fres, mulE, colE, sine, mulF, sat2}, "エミッシブ", 4);
    cm({out}, "出力", 5);

    b.rep.edges = static_cast<int>(model.Edges().size());
    doc.History().Clear();
    return b.rep;
}

SampleReport BuildStressGraph(GraphDocument& doc, SandboxGraphModel& model, int nodeCount, int edgeCount, uint32_t seed)
{
    doc.Clear();
    SampleReport rep;
    std::mt19937 rng(seed);
    static const char* kTypes[] = {"add", "multiply", "lerp", "subtract", "power", "saturate", "oneminus", "divide", "clamp", "abs", "sample", "noise"};
    constexpr int kTypeN = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));
    const int rows = 22;
    std::vector<NodeId> ids;
    ids.reserve(static_cast<size_t>(nodeCount));
    for (int i = 0; i < nodeCount; ++i)
    {
        const int col = i / rows, row = i % rows;
        const char* type = (col == 0) ? "texcoord" : kTypes[rng() % static_cast<uint32_t>(kTypeN)];
        // 縦位置は少しばらす（16 の倍数）
        const float y = static_cast<float>(row) * 176.0f + static_cast<float>(rng() % 3u) * 16.0f;
        const NodeId id = doc.AddNode(type, Vec2(static_cast<float>(col) * 320.0f, y));
        if (id) { ids.push_back(id); ++rep.nodes; }
    }
    int edges = 0, guard = 0;
    if (ids.size() <= static_cast<size_t>(rows)) { doc.History().Clear(); return rep; }
    while (edges < edgeCount && guard++ < edgeCount * 40)
    {
        const size_t toIdx = rows + (rng() % (ids.size() - rows));
        const int toCol = static_cast<int>(toIdx) / rows;
        const int fromCol = toCol - 1 - static_cast<int>(rng() % 2u);
        if (fromCol < 0) continue;
        const size_t fromIdx = static_cast<size_t>(fromCol * rows) + rng() % rows;
        if (fromIdx >= ids.size()) continue;
        const NodeData* nf = model.FindNode(ids[fromIdx]);
        const NodeData* nt = model.FindNode(ids[toIdx]);
        if (!nf || !nt || nf->desc->outputs.empty()) continue;
        std::vector<int> freeIns;
        for (size_t i = 0; i < nt->desc->inputs.size(); ++i)
            if (!nt->desc->inputs[i].propertyOnly && !model.FindEdgeTo(PinRef{ids[toIdx], static_cast<uint16_t>(i), false})) freeIns.push_back(static_cast<int>(i));
        if (freeIns.empty()) continue;
        const int in = freeIns[rng() % freeIns.size()];
        const int out = static_cast<int>(rng() % nf->desc->outputs.size());
        if (doc.Connect(PinRef{ids[fromIdx], static_cast<uint16_t>(out), true}, PinRef{ids[toIdx], static_cast<uint16_t>(in), false})) ++edges;
    }
    rep.edges = edges;
    rep.failedLinks = 0;
    doc.History().Clear();
    (void)model;
    return rep;
}

} // namespace dx12e::ng
