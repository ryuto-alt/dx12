// UnoFX の .uvfx の読み書き・検証・命令列への変換（W1-BD4）。GPU 不要。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "renderer/vfx/VfxAsset.h"

using namespace dx12e::vfx;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

const char* kGood = R"({
  "uvfx": 1, "name": "t", "duration": 2.5, "loop": false,
  "emitters": [ {
    "name": "a", "maxParticles": 100,
    "spawn": { "rate": 50, "startDelay": 0.25, "bursts": [ { "t": 1.0, "count": 5 }, { "t": 0.5, "count": 3 } ] },
    "init": [ { "module": "shape", "kind": "cone", "radius": 0.3, "angle": 20, "length": 0.5 },
              { "module": "velocity", "mode": "shape", "speed": [1, 2], "spread": 10 },
              { "module": "lifetime", "range": [0.5, 1.5] },
              { "module": "size", "start": 0.2, "end": [0.0, 0.1] },
              { "module": "color", "start": [1, 0.5, 0.2, 1], "end": [0, 0, 0, 0], "variation": 0.2 },
              { "module": "rotation", "angle": [0, 360], "rate": [-10, 10] } ],
    "update": [ { "module": "gravity", "value": [0, -9.8, 0] }, { "module": "drag" },
                { "module": "curlNoise", "strength": 2, "scale": 1.5 },
                { "module": "vortex", "axis": [0, 2, 0], "strength": 1 },
                { "module": "attractor", "position": [1, 2, 3] },
                { "module": "floor", "bounce": 0.5, "kill": true } ],
    "render": { "kind": "flipbook", "blend": "alpha", "fade": "linear", "softDistance": 0.3, "intensity": 2,
                "texture": "x.dds", "flipbook": { "cols": 4, "rows": 4, "fps": 24 } }
  } ]
})";

bool Fails(const std::string& text, const char* needle)
{
    const ParseResult r = ParseUvfx(text);
    if (r.ok) return false;
    if (needle && r.error.find(needle) == std::string::npos)
    {
        std::printf("  エラー文に %s が無い: %s\n", needle, r.error.c_str());
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    const ParseResult r = ParseUvfx(kGood);
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s\n", r.error.c_str()); return 1; }
    CHECK(r.asset.emitters.size() == 1);
    const EmitterDesc& e = r.asset.emitters[0];
    CHECK(e.maxParticles == 100);
    CHECK(e.spawn.bursts.size() == 2 && e.spawn.bursts[0].t == 0.5f);   // t の昇順に整列
    CHECK(e.init.size() == 6 && e.update.size() == 6);
    CHECK(e.render.kind == RenderKind::Flipbook && e.render.blend == Blend::Alpha && e.render.fade == Fade::Linear);
    CHECK(e.render.flipCols == 4 && e.render.flipFps == 24.0f);
    CHECK(!r.asset.loop && r.asset.duration == 2.5f);

    // 往復: 書き出し -> 読み込み -> もう一度書き出しで同じ文字列（正規形）
    const std::string s1 = SerializeUvfx(r.asset);
    const ParseResult r2 = ParseUvfx(s1);
    CHECK(r2.ok);
    CHECK(SerializeUvfx(r2.asset) == s1);

    // フリップブックの既定値と規約（W1-BD7）: 炎族は根元固定、mvRange は NB2 の焼き込みと同じ 0.5
    CHECK(e.render.flipChannels == "fire" && e.render.flipFrames == 0 && e.render.flipMv.empty() && e.render.flipMvRange == 0.5f);
    CHECK(e.render.AnchorBottom());
    CHECK(s1.find("\"anchor\": \"bottom\"") != std::string::npos);
    {
        const ParseResult fr = ParseUvfx(R"({"uvfx":1,"emitters":[{"render":{"kind":"flipbook","texture":"s.dds",
            "flipbook":{"cols":6,"rows":6,"frames":36,"fps":12,"channels":"smoke6way","dbf":"d.dds","mv":"m.dds","mvRange":0.5,"anchor":"center","tempMin":900,"tempMax":2400,"emission":2,"erodeStart":0.2,"erodeWidth":0.1,"rootFade":0.15}}}]})");
        CHECK(fr.ok);
        CHECK(fr.asset.emitters[0].render.flipRootFade == 0.15f);
        const RenderDesc& rd = fr.asset.emitters[0].render;
        CHECK(rd.flipFrames == 36 && rd.flipChannels == "smoke6way" && rd.flipDbf == "d.dds" && rd.flipMv == "m.dds");
        CHECK(!rd.AnchorBottom() && rd.flipTempMin == 900.0f && rd.flipTempMax == 2400.0f && rd.flipEmission == 2.0f);
        CHECK(rd.flipErodeStart == 0.2f && rd.flipErodeWidth == 0.1f);
        const std::string t1 = SerializeUvfx(fr.asset);
        const ParseResult fr2 = ParseUvfx(t1);
        CHECK(fr2.ok && SerializeUvfx(fr2.asset) == t1);
        // channels が color なら既定は center、smoke6way の既定は bottom
        const ParseResult col = ParseUvfx(R"({"uvfx":1,"emitters":[{"render":{"kind":"flipbook","flipbook":{"channels":"color"}}}]})");
        CHECK(col.ok && !col.asset.emitters[0].render.AnchorBottom());
    }
    CHECK(Fails(R"({"uvfx":1,"emitters":[{"render":{"flipbook":{"channels":"neon"}}}]})", "neon"));
    CHECK(Fails(R"({"uvfx":1,"emitters":[{"render":{"flipbook":{"cols":4,"rows":4,"frames":17}}}]})", "frames"));
    CHECK(Fails(R"({"uvfx":1,"emitters":[{"render":{"flipbook":{"anchor":"top"}}}]})", "anchor"));
    CHECK(Fails(R"({"uvfx":1,"emitters":[{"render":{"kind":"flipbook","flipbook":{"channels":"smoke6way"}}}]})", "dbf"));
    CHECK(Fails(R"({"uvfx":1,"emitters":[{"render":{"flipbook":{"rootFade":0.9}}}]})", "rootFade"));
    CHECK(Fails(R"({"uvfx":1,"emitters":[{"render":{"flipbook":{"tempMin":3000,"tempMax":2000}}}]})", "tempMax"));
    CHECK(Fails(R"({"uvfx":1,"emitters":[{"render":{"flipbook":{"mvRange":-1}}}]})", "mvRange"));

    // 命令列
    CompiledEmitter ce; std::string err;
    CHECK(CompileEmitter(e, ce, err));
    CHECK(ce.init.size() == 6 && ce.update.size() == 6);
    CHECK(ce.init[0].op == kOpShape && ce.init[0].a[0] == 3.0f && ce.init[0].a[2] == 20.0f && ce.init[0].b[3] == 0.5f);
    CHECK(ce.init[2].op == kOpLifetime && ce.init[2].c[0] == 0.5f && ce.init[2].c[1] == 1.5f);
    CHECK(ce.maxLife == 1.5f);
    CHECK(ce.update[3].op == kOpVortex && ce.update[3].a[1] == 1.0f);   // 軸は正規化される
    CHECK(sizeof(GOp) == 48 && sizeof(GPart) == 64);

    // 検証（黙って捨てない）
    CHECK(Fails("not json", "JSON"));
    CHECK(Fails("{\"uvfx\":2,\"emitters\":[{}]}", "uvfx"));
    CHECK(Fails("{\"uvfx\":1}", "emitters"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[]}", "emitters"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"init\":[{\"module\":\"warp\"}]}]}", "warp"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"update\":[{\"module\":\"shape\"}]}]}", "update"));   // init 専用を update に置けない
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"maxParticles\":0}]}", "maxParticles"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"maxParticles\":99999999}]}", "maxParticles"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"init\":[{\"module\":\"lifetime\",\"range\":[2,1]}]}]}", "最小"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"init\":[{\"module\":\"lifetime\",\"range\":[0,1]}]}]}", "範囲"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"init\":[{\"module\":\"shape\",\"kind\":\"torus\"}]}]}", "torus"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"update\":[{\"module\":\"gravity\",\"value\":[0,1]}]}]}", "3 要素"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"update\":[{\"module\":\"vortex\",\"axis\":[0,0,0]}]}]}", "0 ベクトル"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"render\":{\"blend\":\"multiply\"}}]}", "multiply"));
    CHECK(Fails("{\"uvfx\":1,\"emitters\":[{\"spawn\":{\"bursts\":[{\"t\":1}]}}]}", "bursts"));
    CHECK(!ParseUvfx("{\"uvfx\":1,\"emitters\":[{\"init\":[{\"module\":\"drag\"}]}]}").ok);   // drag は update 専用
    // 未知のキーは警告（読み込める）
    const ParseResult w2 = ParseUvfx("{\"uvfx\":1,\"zzz\":1,\"emitters\":[{\"update\":[{\"module\":\"drag\",\"foo\":1}]}]}");
    CHECK(w2.ok && w2.warnings.size() == 2);

    // 同梱のアセットが読める（引数でパスを渡されたとき）
    for (int i = 1; i < argc; ++i)
    {
        std::ifstream f(argv[i], std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        const ParseResult ar = ParseUvfx(ss.str());
        CHECK(ar.ok);
        if (!ar.ok) std::printf("  %s: %s\n", argv[i], ar.error.c_str());
        else
        {
            CHECK(ar.warnings.empty());
            const std::string a = SerializeUvfx(ar.asset);
            CHECK(SerializeUvfx(ParseUvfx(a).asset) == a);
        }
    }

    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
