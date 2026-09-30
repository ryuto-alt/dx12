// マテリアルグラフ G2a: フォワードのシェーディング尾部（shaders/forward/ForwardShade.hlsli）の検査。GPU 不要。
//
//   1. 契約: shaders/forward/UnoSurface.hlsli の構造体（UnoSurface / UnoMatInput / UnoSurfaceDefault）が
//      G1 の参照実装（src/renderer/matgraph/hlsl/UnoMatContractRef.hlsli）と 1 文字（空白・コメントを除く）も違わない。
//      グラフが生成する HLSL（UnoMatEval）はこの構造体を書き込む＝ずれると G2b で黙って値が化ける。
//   2. 構造: Forward / ForwardSkinned / Terrain が ForwardShade.hlsli を include し、尾部（CalcShadow・クラスタライト累積・
//      IBL など）の複製が各シェーダに復活していない（複製を戻すと DDGI のような「片方にだけ入れ忘れる」事故が再発する）。
//   3. DXC: ShaderRegistry の全バリアント（実行時ホットリロードと同じ引数）+ ALPHA_TEST 派生 + G2b 予行の probe
//      （tests/data/forward_shade_probe.hlsl。ps_6_6 + ResourceDescriptorHeap[] + UnoShadeForward）がコンパイルできる。
//
//   ctest ラベル "dxc"。dxcompiler.dll / dxil.dll が無い環境では 3. だけスキップ（終了コード 0 + "SKIP" 表示）。
//   DXIL が外出し前後で同値であることの検証は tools/shader_dxil_equiv.ps1（変更前ツリーが要るので ctest には入れない）。
#include "resource/ShaderRegistry.h"

#include <windows.h>
#include <d3d12.h>      // WIN32_LEAN_AND_MEAN 下で dxcapi.h が要る IUnknown / BSTR を引き込む
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks   = 0;
}

#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);    \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)
#define CHECK(cond) CHECK_MSG(cond, "")

namespace
{

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// コメント（// と /* */）を落とし、空白を全部消す。構造体の中身を「意味だけ」で比べる。
std::string StripCommentsAndSpace(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size();)
    {
        if (s.compare(i, 2, "//") == 0) { while (i < s.size() && s[i] != '\n') ++i; continue; }
        if (s.compare(i, 2, "/*") == 0) { i += 2; while (i + 1 < s.size() && s.compare(i, 2, "*/") != 0) ++i; i += 2; continue; }
        const char c = s[i++];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out += c;
    }
    return out;
}

// 「header」で始まる宣言から、対応する閉じ波括弧までの本文（{ } の中）を返す。見つからなければ空。
std::string BodyAfter(const std::string& src, const std::string& header)
{
    const size_t h = src.find(header);
    if (h == std::string::npos) return {};
    const size_t open = src.find('{', h);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (size_t i = open; i < src.size(); ++i)
    {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}' && --depth == 0) return src.substr(open + 1, i - open - 1);
    }
    return {};
}

// コメント行と空行を除いたテキスト（構造検査用。コメント内の関数名を拾わない）
std::string StripLineComments(const std::string& s)
{
    std::istringstream in(s);
    std::string line, out;
    while (std::getline(in, line))
    {
        const size_t c = line.find("//");
        if (c != std::string::npos) line.erase(c);
        out += line;
        out += '\n';
    }
    return out;
}

int Count(const std::string& hay, const std::string& needle)
{
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// 1. 契約
// ---------------------------------------------------------------------------
void Test_Contract()
{
    const fs::path shaders = fs::path(DX12E_SHADER_DIR);
    const std::string engine = ReadFile(shaders / "forward" / "UnoSurface.hlsli");
    const std::string ref    = ReadFile(fs::path(DX12E_MATGRAPH_HLSL_DIR) / "UnoMatContractRef.hlsli");
    CHECK_MSG(!engine.empty(), "UnoSurface.hlsli が読めない");
    CHECK_MSG(!ref.empty(), "UnoMatContractRef.hlsli が読めない");
    if (engine.empty() || ref.empty()) return;

    for (const char* header : { "struct UnoSurface", "struct UnoMatInput", "UnoSurface UnoSurfaceDefault()" })
    {
        const std::string a = StripCommentsAndSpace(BodyAfter(engine, header));
        const std::string b = StripCommentsAndSpace(BodyAfter(ref, header));
        CHECK_MSG(!a.empty() && !b.empty(), "%s が見つからない", header);
        CHECK_MSG(a == b, "%s の本体が参照実装と違う（G1 の契約 = docs/MATGRAPH_FORMAT.md と揃えること）", header);
    }

    // UnoSurface のフィールド名が契約の全部を持つこと（順序は本体一致で担保済み。ここは可読な保険）
    const std::string body = StripCommentsAndSpace(BodyAfter(engine, "struct UnoSurface"));
    for (const char* f : { "baseColor;", "metallic;", "roughness;", "normalTS;", "hasNormal;", "emissive;", "ao;", "opacity;",
                           "opacityMask;", "subsurfaceColor;", "subsurfaceOpacity;", "clearCoat;", "clearCoatRoughness;",
                           "anisotropy;", "shadingModel;" })
        CHECK_MSG(body.find(f) != std::string::npos, "UnoSurface に %s が無い", f);

    // cbuffer / リソース宣言を置かない（b0 衝突の罠。UnoCustom.hlsli と同じ規約）
    const std::string code = StripLineComments(engine);
    CHECK_MSG(code.find("cbuffer") == std::string::npos, "UnoSurface.hlsli に cbuffer がある");
    CHECK_MSG(code.find("register(") == std::string::npos, "UnoSurface.hlsli にリソース宣言（register）がある");
    CHECK_MSG(Count(code, "float3 UnoNormalFromTangentSpace(") == 1, "UnoNormalFromTangentSpace が UnoSurface.hlsli に 1 個だけ無い");
}

// ---------------------------------------------------------------------------
// 2. 構造（尾部の複製が復活していない）
// ---------------------------------------------------------------------------
void Test_Structure()
{
    const fs::path fwd = fs::path(DX12E_SHADER_DIR) / "forward";
    const std::string shade = StripLineComments(ReadFile(fwd / "ForwardShade.hlsli"));
    CHECK_MSG(!shade.empty(), "ForwardShade.hlsli が読めない");
    // 尾部の本体はここに 1 個ずつだけある
    CHECK(Count(shade, "float CalcShadow(") == 1);
    CHECK(Count(shade, "int SelectCascade(") == 1);
    CHECK(Count(shade, "AccumulatePunctualLights(") == 1);
    CHECK(Count(shade, "FilterShadingNormal(") == 1);
    CHECK(Count(shade, "ApplyDecals(") == 1);
    CHECK(Count(shade, "SampleDdgi(") == 1);
    CHECK(Count(shade, "float4 UnoShadeForward(") == 1);
    CHECK(Count(shade, "float3 UnoShadeLighting(") == 1);
    CHECK(Count(shade, "float3 UnoShadeFinish(") == 1);
    CHECK_MSG(shade.find("cbuffer") == std::string::npos, "ForwardShade.hlsli に cbuffer がある（宣言はシェーダ側の責務）");
    CHECK_MSG(shade.find(": register(") == std::string::npos, "ForwardShade.hlsli にリソース宣言がある（宣言順で DXIL の ID が動く）");

    for (const char* name : { "Forward.hlsl", "ForwardSkinned.hlsl", "Terrain.hlsl" })
    {
        const std::string raw = ReadFile(fwd / name);
        const std::string code = StripLineComments(raw);
        CHECK_MSG(!raw.empty(), "%s が読めない", name);
        CHECK_MSG(code.find("#include \"ForwardShade.hlsli\"") != std::string::npos, "%s が ForwardShade.hlsli を include していない", name);
        for (const char* dup : { "float CalcShadow(", "int SelectCascade(", "AccumulatePunctualLights(", "g_irradianceMap.SampleLevel",
                                 "FilterShadingNormal(", "ApplyClusterDebug(" })
            CHECK_MSG(code.find(dup) == std::string::npos, "%s に尾部の複製（%s）が残っている / 戻っている", name, dup);
        CHECK_MSG(code.find("UnoShadeLighting(") != std::string::npos, "%s が UnoShadeLighting を呼んでいない", name);
        CHECK_MSG(code.find("UnoShadeFinish(") != std::string::npos, "%s が UnoShadeFinish を呼んでいない", name);
    }

    // 登録表: 3 本とも ForwardShade.hlsli / UnoSurface.hlsli を依存に持つ（ShaderIncludeDepsTests も見張る）
    for (const char* rel : { "forward/Forward.hlsl", "forward/ForwardSkinned.hlsl", "forward/Terrain.hlsl" })
    {
        const dx12e::ShaderSource* src = dx12e::FindShaderSourceByRelPath(rel);
        CHECK_MSG(src != nullptr, "%s が ShaderRegistry に無い", rel);
        if (!src) continue;
        bool shadeDep = false, surfDep = false;
        for (const char* d : src->staticDeps)
        {
            if (std::string(d) == "forward/ForwardShade.hlsli") shadeDep = true;
            if (std::string(d) == "forward/UnoSurface.hlsli") surfDep = true;
        }
        CHECK_MSG(shadeDep && surfDep, "%s の staticDeps に ForwardShade.hlsli / UnoSurface.hlsli が無い", rel);
    }
}

// ---------------------------------------------------------------------------
// 3. DXC
// ---------------------------------------------------------------------------
struct Dxc
{
    ComPtr<IDxcUtils>          utils;
    ComPtr<IDxcCompiler3>      compiler;
    ComPtr<IDxcIncludeHandler> inc;

    bool Init()
    {
        HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
        if (!dll) return false;
        auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
        if (!create) return false;
        if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return false;
        if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return false;
        return SUCCEEDED(utils->CreateDefaultIncludeHandler(&inc));
    }

    // 実行時ホットリロード（ShaderRuntimeCompiler）と同じ引数: -E -T [-D] -I。ファイルのあるフォルダも -I に足す。
    bool Compile(const fs::path& file, const std::wstring& entry, const std::wstring& profile, const wchar_t* define,
                 const fs::path& shaderRoot, std::string& log) const
    {
        const std::string src = ReadFile(file);
        if (src.empty()) { log = "ソースが読めない: " + file.string(); return false; }
        std::vector<std::wstring> a = { L"-E", entry, L"-T", profile, L"-I", file.parent_path().wstring(), L"-I", shaderRoot.wstring() };
        if (define && *define) { a.push_back(L"-D"); a.push_back(define); }
        std::vector<LPCWSTR> args;
        for (const auto& s : a) args.push_back(s.c_str());
        DxcBuffer buf{ src.data(), src.size(), DXC_CP_UTF8 };
        ComPtr<IDxcResult> res;
        if (FAILED(compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), inc.Get(), IID_PPV_ARGS(&res)))) { log = "Compile 呼び出しに失敗"; return false; }
        HRESULT st = E_FAIL;
        res->GetStatus(&st);
        ComPtr<IDxcBlobUtf8> errs;
        if (SUCCEEDED(res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength() > 0)
            log.assign(errs->GetStringPointer(), errs->GetStringLength());
        return SUCCEEDED(st);
    }
};

void Test_Dxc()
{
    Dxc dxc;
    if (!dxc.Init())
    {
        std::printf("SKIP: dxcompiler.dll が読めない（DXC のコンパイル検査は実施しない）\n");
        return;
    }
    const fs::path shaders = fs::weakly_canonical(fs::path(DX12E_SHADER_DIR));
    int compiled = 0;
    for (const dx12e::ShaderSource& src : dx12e::AllShaderSources())
    {
        for (const dx12e::ShaderVariant& v : src.variants)
        {
            std::string log;
            const bool ok = dxc.Compile(shaders / src.relPath, v.entry, v.profile, v.define, shaders, log);
            std::wstring name = v.csoName;
            CHECK_MSG(ok, "%ls のコンパイルに失敗:\n%s", name.c_str(), log.substr(0, 1500).c_str());
            ++compiled;
        }
    }
    CHECK_MSG(compiled >= 60, "レジストリのバリアントが %d 本しか無い（60 本以上のはず）", compiled);

    // CMake だけが作るバリアント（レジストリ外）: アルファクリップ（early-Z を失うので別 CSO）
    for (const char* rel : { "forward/Forward.hlsl", "forward/ForwardSkinned.hlsl" })
    {
        std::string log;
        const bool ok = dxc.Compile(shaders / rel, L"PSMain", L"ps_6_0", L"ALPHA_TEST=1", shaders, log);
        CHECK_MSG(ok, "%s の ALPHA_TEST バリアントのコンパイルに失敗:\n%s", rel, log.substr(0, 1500).c_str());
    }

    // G2b 予行: バインドレス + UnoShadeForward（ps_6_6）
    {
        std::string log;
        const fs::path probe = fs::path(DX12E_TEST_DATA_DIR) / "forward_shade_probe.hlsl";
        const bool ok = dxc.Compile(probe, L"PSMain", L"ps_6_6", nullptr, shaders, log);
        CHECK_MSG(ok, "forward_shade_probe.hlsl（ps_6_6）のコンパイルに失敗:\n%s", log.substr(0, 2000).c_str());
    }
    std::printf("DXC: レジストリ %d 本 + ALPHA_TEST 2 本 + probe 1 本をコンパイル\n", compiled);
}

} // namespace

int main()
{
    Test_Contract();
    Test_Structure();
    Test_Dxc();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
