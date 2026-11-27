// UNO_SHADING_V2 の軸（W1-C3 / MH-0a）の静的な検査。GPU も DXC も使わず、宣言表と C++ のソースをテキストとして読む。
//
//   1. lane_c.cmake の *V2_PS は、V1 の宣言（ShaderManifest.cmake）と FILE / ENTRY / PROFILE / DEFINES が同じで、
//      DEFINES に UNO_SHADING_V2=1 が 1 つ増えただけ。NEW が付いている（-HV 2021 -WX）。
//   2. V1 の宣言に UNO_SHADING_V2 が入っていない（V1 の .cso を変えていない）。
//   3. ApplicationPipeline.cpp が、結線済みの V2 の .cso 名をすべて参照している。
//   4. BrdfV2.hlsli が UNO_SHADING_V2 で守られた枠になっている。
//   5. GraphMaterialSystem が rev=1 のキーを変えない形になっている（|sv の接尾辞は rev>1 のときだけ）。
//
// 実行: ctest --output-on-failure -R ShadingV2Axis

#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef DX12E_SOURCE_ROOT
#error DX12E_SOURCE_ROOT が必要
#endif

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool ok, const std::string& msg)
{
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  NG  %s\n", msg.c_str()); }
}

std::string ReadFile(const std::string& rel)
{
    std::ifstream f(std::string(DX12E_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Decl
{
    std::string name, file, entry, profile;
    std::set<std::string> defines;
    bool isNew = false;
};

std::vector<std::string> Tokens(const std::string& s)
{
    std::vector<std::string> t;
    std::istringstream in(s);
    std::string w;
    while (in >> w) t.push_back(w);
    return t;
}

// uno_shader(...) を 1 行 1 宣言として読む（この 2 つの表は 1 行 1 宣言）。
std::vector<Decl> ParseDecls(const std::string& text)
{
    std::vector<Decl> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        const std::size_t p = line.find("uno_shader(");
        if (p == std::string::npos) continue;
        if (line.find_first_not_of(" \t") != p) continue;   // コメントアウトは拾わない
        const std::size_t e = line.rfind(')');
        if (e == std::string::npos) continue;
        const auto tok = Tokens(line.substr(p + 11, e - p - 11));
        Decl d;
        enum { None, Name, File, Entry, Profile, Defs, Deps } mode = None;
        for (const auto& w : tok)
        {
            if (w == "NAME") mode = Name;
            else if (w == "FILE") mode = File;
            else if (w == "ENTRY") mode = Entry;
            else if (w == "PROFILE") mode = Profile;
            else if (w == "DEFINES") mode = Defs;
            else if (w == "DEPENDS") mode = Deps;
            else if (w == "NEW") { d.isNew = true; mode = None; }
            else if (w == "PDB") mode = None;
            else if (mode == Name) d.name = w;
            else if (mode == File) d.file = w;
            else if (mode == Entry) d.entry = w;
            else if (mode == Profile) d.profile = w;
            else if (mode == Defs) d.defines.insert(w);
        }
        out.push_back(std::move(d));
    }
    return out;
}
} // namespace

int main()
{
    const std::string manifest = ReadFile("shaders/ShaderManifest.cmake");
    const std::string laneC    = ReadFile("shaders/manifest/lane_c.cmake");
    Check(!manifest.empty() && !laneC.empty(), "宣言表を読めない");

    const auto v1 = ParseDecls(manifest);
    std::map<std::string, Decl> v1ByName;
    for (const auto& d : v1)
    {
        v1ByName[d.name] = d;
        Check(d.defines.count("UNO_SHADING_V2=1") == 0, "V1 の宣言が UNO_SHADING_V2 を定義している: " + d.name);
    }

    std::vector<Decl> v2;
    for (const auto& d : ParseDecls(laneC))
        if (d.defines.count("UNO_SHADING_V2=1")) v2.push_back(d);
    Check(!v2.empty(), "V2 の宣言が 1 件も無い");

    std::set<std::string> v2Names;
    for (const auto& d : v2)
    {
        v2Names.insert(d.name);
        Check(d.isNew, "V2 に NEW が無い（-HV 2021 -WX が付かない）: " + d.name);
        const std::size_t p = d.name.rfind("V2_PS");
        Check(p != std::string::npos && p + 5 == d.name.size(), "V2 の名前の形が違う（<V1 の名前から _PS を除いたもの>V2_PS）: " + d.name);
        if (p == std::string::npos) continue;
        const std::string v1Name = d.name.substr(0, p) + "_PS";
        auto it = v1ByName.find(v1Name);
        Check(it != v1ByName.end(), "対になる V1 の宣言が無い: " + d.name + " → " + v1Name);
        if (it == v1ByName.end()) continue;
        const Decl& o = it->second;
        Check(o.file == d.file, d.name + ": FILE が V1 と違う");
        Check(o.entry == d.entry, d.name + ": ENTRY が V1 と違う");
        Check(o.profile == d.profile, d.name + ": PROFILE が V1 と違う（SM の引き上げは F 段で別の .cso）");
        std::set<std::string> want = o.defines;
        want.insert("UNO_SHADING_V2=1");
        Check(want == d.defines, d.name + ": DEFINES が「V1 + UNO_SHADING_V2=1」と違う");
    }

    // 結線済み（ApplicationPipeline.cpp が選ぶ）V2 の .cso
    const std::string pipe = ReadFile("src/core/ApplicationPipeline.cpp");
    for (const char* n : {"ForwardV2_PS", "ForwardPhysV2_PS", "ForwardMaskV2_PS", "ForwardMaskPhysV2_PS", "TerrainV2_PS", "TerrainPhysV2_PS"})
    {
        Check(v2Names.count(n) == 1, std::string("結線済みの V2 が宣言表に無い: ") + n);
        Check(pipe.find(std::string("L\"") + n + ".cso\"") != std::string::npos, std::string("ApplicationPipeline.cpp が参照していない: ") + n);
    }

    // ホットリロードの宣言表（ShaderRegistry.cpp）にも V2 が載っている（W1-FU-E-26）。
    // レジストリに無いと、V2 の .hlsl を保存しても V2 の .cso だけ再コンパイルされず古いまま動く。
    {
        const std::string reg = ReadFile("src/resource/ShaderRegistry.cpp");
        Check(!reg.empty(), "ShaderRegistry.cpp を読めない");
        for (const char* n : {"ForwardV2_PS", "ForwardPhysV2_PS", "TerrainV2_PS"})
        {
            const std::size_t p = reg.find(std::string("L\"") + n + ".cso\"");
            Check(p != std::string::npos, std::string("ShaderRegistry.cpp に V2 の .cso が無い: ") + n);
            if (p == std::string::npos) continue;
            const std::size_t e = reg.find("},", p);
            Check(e != std::string::npos && reg.substr(p, e - p).find("UNO_SHADING_V2=1") != std::string::npos,
                  std::string("ShaderRegistry.cpp の V2 の define に UNO_SHADING_V2=1 が無い: ") + n);
        }
    }

    // 空の枠
    const std::string brdf = ReadFile("shaders/forward/BrdfV2.hlsli");
    Check(brdf.find("#ifdef UNO_SHADING_V2") != std::string::npos, "BrdfV2.hlsli が UNO_SHADING_V2 で守られていない");
    Check(brdf.find("#ifndef UNO_BRDF_V2_HLSLI") != std::string::npos, "BrdfV2.hlsli にインクルードガードが無い");

    // BrdfV2.hlsli を #include するシェーダが現れたら、配布ビルド（GraphMaterialSystem::BakeForBuild は V1 のみ焼く）と
    // エディタの描画が分かれる。BakeForBuild を V2 対応にするまで、この検査で止める（C5 / MH-2 は同じ変更で直す）。
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(fs::path(DX12E_SOURCE_ROOT) / "shaders", ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file()) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".hlsl" && ext != ".hlsli") continue;
            if (it->path().filename() == "BrdfV2.hlsli") continue;
            const std::string rel = fs::relative(it->path(), DX12E_SOURCE_ROOT).generic_string();
            const std::string src = ReadFile(rel);
            Check(src.find("BrdfV2.hlsli") == std::string::npos,
                  rel + " が BrdfV2.hlsli を include している: GraphMaterialSystem::BakeForBuild の V2 対応と ShaderRegistry の staticDeps が先");
        }
    }

    // ForwardGraph: rev=1 のキーを変えない
    const std::string gms = ReadFile("src/renderer/GraphMaterialSystem.cpp");
    Check(gms.find("if (job.rev > 1) psIn.extra") != std::string::npos, "ディスクキャッシュのキーが rev>1 のときだけの形になっていない");
    Check(gms.find("UNO_SHADING_V2=1") != std::string::npos, "ForwardGraph のコンパイル引数に UNO_SHADING_V2 が無い");
    const std::string gmh = ReadFile("src/renderer/GraphMaterialSystem.h");
    Check(gmh.find("rev > 1 ? static_cast<u64>(rev)") != std::string::npos, "VKey が rev=1 のとき従来の値になっていない");

    std::printf("ShadingV2Axis: %d checks, %d failures (V2 宣言 %zu 件)\n", g_checks, g_failures, v2.size());
    return g_failures == 0 ? 0 : 1;
}
