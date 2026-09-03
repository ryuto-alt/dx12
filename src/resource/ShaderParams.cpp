#include "resource/ShaderParams.h"

#include "resource/ShaderDiagnostics.h"

#include <d3d12shader.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace dx12e::shaderparams
{
namespace
{

bool IsIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string ToLower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 詰め物として書かれた変数は Inspector に出さない。テンプレートの `float3 _reserved;` が
// そのまま 3 本のスライダーになると、既存シェーダーの見た目が勝手に増えてしまう。
bool LooksLikePadding(const std::string& name)
{
    std::string s;
    for (char c : name)
        if (c != '_') s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s == "pad" || s == "padding" || s == "reserved" || s == "dummy" || s == "unused";
}

// 名前から色っぽさを推測する（@color を書かなくてもカラーピッカーになる）。
bool LooksLikeColor(const std::string& name)
{
    const std::string s = ToLower(name);
    static const char* kWords[] = {"color", "colour", "tint", "albedo", "emissive"};
    for (const char* w : kWords)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

Kind KindFrom(const D3D12_SHADER_TYPE_DESC& td, const std::string& name, bool forceColor)
{
    if (td.Elements > 0)             return Kind::Unsupported;   // 配列
    if (td.Type != D3D_SVT_FLOAT)    return Kind::Unsupported;   // int / bool / half など
    if (td.Rows != 1)                return Kind::Unsupported;   // 行列

    if (td.Class == D3D_SVC_SCALAR && td.Columns == 1) return Kind::Float;
    if (td.Class != D3D_SVC_VECTOR)                    return Kind::Unsupported;

    const bool color = forceColor || LooksLikeColor(name);
    switch (td.Columns)
    {
    case 2:  return Kind::Float2;
    case 3:  return color ? Kind::Color3 : Kind::Float3;
    case 4:  return color ? Kind::Color4 : Kind::Float4;
    default: return Kind::Unsupported;
    }
}

// ---- ソース注釈 `// @range(min,max)` / `// @color` ----
struct Annot
{
    bool hasRange = false;
    f32  minV     = 0.0f;
    f32  maxV     = 1.0f;
    bool color    = false;
};

// 行末コメントの直前にある最後の識別子を「その行で宣言された変数名」とみなす。
//   `    float3 _TintColor;   // @color`  →  "_TintColor"
std::string LastIdentifier(const std::string& decl)
{
    size_t e = decl.size();
    while (e > 0 && !IsIdentChar(decl[e - 1])) --e;   // 末尾の ';' や空白を飛ばす
    size_t b = e;
    while (b > 0 && IsIdentChar(decl[b - 1])) --b;
    return decl.substr(b, e - b);
}

// .hlsl を 1 行ずつ見て注釈を集める。読めなければ何も入れない（注釈は任意）。
void ParseAnnotations(const std::wstring& hlslPath, std::unordered_map<std::string, Annot>& out)
{
    if (hlslPath.empty()) return;

    std::error_code ec;
    const std::filesystem::path p(hlslPath);
    if (!std::filesystem::exists(p, ec) || ec) return;

    std::ifstream f(p);
    if (!f) return;

    std::string line;
    while (std::getline(f, line))
    {
        const size_t slash = line.find("//");
        if (slash == std::string::npos) continue;

        const std::string comment = line.substr(slash + 2);
        if (comment.find('@') == std::string::npos) continue;

        const std::string name = LastIdentifier(line.substr(0, slash));
        if (name.empty()) continue;

        Annot a;
        const size_t r = comment.find("@range");
        const size_t lp = (r == std::string::npos) ? std::string::npos : comment.find('(', r);
        if (lp != std::string::npos)
        {
            // "(0, 4)" を手で読む。sscanf は /WX + C4996 で使えない。
            const char* loBegin = comment.c_str() + lp + 1;
            char*       loEnd   = nullptr;
            const f32   lo      = std::strtof(loBegin, &loEnd);
            if (loEnd != loBegin)
            {
                while (*loEnd == ' ' || *loEnd == '\t') ++loEnd;
                if (*loEnd == ',')
                {
                    const char* hiBegin = loEnd + 1;
                    char*       hiEnd   = nullptr;
                    const f32   hi      = std::strtof(hiBegin, &hiEnd);
                    if (hiEnd != hiBegin && hi > lo)
                    {
                        a.hasRange = true;
                        a.minV     = lo;
                        a.maxV     = hi;
                    }
                }
            }
        }
        a.color = comment.find("@color") != std::string::npos;

        if (a.hasRange || a.color) out[name] = a;
    }
}

// 変数のオフセットがどちらの契約の自由枠に載っているか。どちらでもなければ false。
// ★2 つの範囲は重ならない（メッシュ側の 32..47 は mvp の内側で、行列の StartOffset は 0 の
//   ため拾われない。画面側の cbuffer は 80 バイトしか無く 128 以降に変数を置けない）。
bool ClassifySpace(u32 startOffset, u32 size, Space& out)
{
    if (startOffset >= kMeshFreeBegin && startOffset + size <= kMeshFreeEnd)
    {
        out = Space::MeshObject;
        return true;
    }
    if (startOffset >= kScreenFreeBegin && startOffset + size <= kScreenFreeEnd)
    {
        out = Space::Screen;
        return true;
    }
    return false;
}

// ---- b0 の cbuffer から自由枠の変数を拾う ----
void ReflectOne(const void* bytecode, size_t size,
                const std::unordered_map<std::string, Annot>& annots,
                std::vector<Param>& out, bool& reflectedAny)
{
    ComPtr<ID3D12ShaderReflection> refl = shaderdiag::CreateReflection(bytecode, size);
    if (!refl) return;

    D3D12_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) return;
    reflectedAny = true;

    for (u32 i = 0; i < sd.ConstantBuffers; ++i)
    {
        ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        if (!cb) continue;

        D3D12_SHADER_BUFFER_DESC bd{};
        if (FAILED(cb->GetDesc(&bd))) continue;
        if (bd.Type != D3D_CT_CBUFFER || !bd.Name) continue;

        // register(b0, space0) に載っているものだけが per-object 定数。
        // バインド情報が引けない場合だけ、サイズで自由枠まで届いているかを見て代用する。
        D3D12_SHADER_INPUT_BIND_DESC ib{};
        if (SUCCEEDED(refl->GetResourceBindingDescByName(bd.Name, &ib)))
        {
            if (ib.Type != D3D_SIT_CBUFFER || ib.BindPoint != 0 || ib.Space != 0) continue;
        }
        else if (bd.Size < kScreenFreeEnd)
        {
            continue;   // バインド情報が引けないときは、せめて画面用の枠に届く大きさかで足切りする
        }

        for (u32 v = 0; v < bd.Variables; ++v)
        {
            ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            if (!var) continue;

            D3D12_SHADER_VARIABLE_DESC vd{};
            if (FAILED(var->GetDesc(&vd)) || !vd.Name) continue;

            // ★自由枠の外（mvp/model や、画面シェーダーの解像度・時刻など）は触らせない。
            Space space = Space::MeshObject;
            if (!ClassifySpace(vd.StartOffset, vd.Size, space)) continue;

            const std::string name = vd.Name;
            if (LooksLikePadding(name)) continue;

            // 同じ cbuffer を VS/PS 両方が宣言しているので、オフセットで重複を畳む。
            if (std::any_of(out.begin(), out.end(),
                            [&](const Param& p) { return p.offset == vd.StartOffset; }))
                continue;

            D3D12_SHADER_TYPE_DESC td{};
            ID3D12ShaderReflectionType* type = var->GetType();
            if (!type || FAILED(type->GetDesc(&td))) continue;

            const auto it = annots.find(name);
            const bool forceColor = (it != annots.end()) && it->second.color;

            Param p;
            p.name     = name;
            p.typeName = td.Name ? td.Name : "";
            p.offset   = vd.StartOffset;
            p.space    = space;
            p.kind     = KindFrom(td, name, forceColor);
            if (it != annots.end() && it->second.hasRange)
            {
                p.hasRange = true;
                p.minV     = it->second.minV;
                p.maxV     = it->second.maxV;
            }
            out.push_back(std::move(p));
        }
    }
}

// ---- 置き場 ----
std::mutex& StoreMutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::vector<Param>>& Store()
{
    static std::unordered_map<std::string, std::vector<Param>> s;
    return s;
}

} // namespace

u32 Param::ComponentCount() const
{
    switch (kind)
    {
    case Kind::Float:                    return 1;
    case Kind::Float2:                   return 2;
    case Kind::Float3: case Kind::Color3: return 3;
    case Kind::Float4: case Kind::Color4: return 4;
    default:                             return 0;
    }
}

bool Reflect(const void* vs, size_t vsSize, const void* ps, size_t psSize,
             const std::wstring& hlslSourcePath, std::vector<Param>& out)
{
    std::unordered_map<std::string, Annot> annots;
    ParseAnnotations(hlslSourcePath, annots);

    bool reflectedAny = false;
    // PS を先に見る（パラメーターはたいてい PS で読むので、宣言もそちらが素直）。
    ReflectOne(ps, psSize, annots, out, reflectedAny);
    ReflectOne(vs, vsSize, annots, out, reflectedAny);

    // 宣言順ではなくオフセット順に並べる（HLSL のパッキングで前後することがある）。
    std::sort(out.begin(), out.end(),
              [](const Param& a, const Param& b) { return a.offset < b.offset; });
    return reflectedAny;
}

void Set(const std::string& key, std::vector<Param> params)
{
    std::lock_guard<std::mutex> lock(StoreMutex());
    Store()[key] = std::move(params);
}

void Clear(const std::string& key)
{
    std::lock_guard<std::mutex> lock(StoreMutex());
    Store().erase(key);
}

std::vector<Param> Get(const std::string& key)
{
    std::lock_guard<std::mutex> lock(StoreMutex());
    auto it = Store().find(key);
    return it == Store().end() ? std::vector<Param>{} : it->second;
}

std::vector<Param> GetIn(const std::string& key, Space space)
{
    std::vector<Param> out;
    std::lock_guard<std::mutex> lock(StoreMutex());
    auto it = Store().find(key);
    if (it == Store().end()) return out;
    for (const Param& p : it->second)
        if (p.space == space) out.push_back(p);
    return out;
}

bool Find(const std::string& key, const std::string& name, Param& out)
{
    std::lock_guard<std::mutex> lock(StoreMutex());
    auto it = Store().find(key);
    if (it == Store().end()) return false;
    for (const Param& p : it->second)
    {
        if (p.name != name) continue;
        out = p;
        return true;
    }
    return false;
}

} // namespace dx12e::shaderparams
