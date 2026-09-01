#include "resource/ShaderDiagnostics.h"

#include "core/Logger.h"

#include <dxcapi.h>
#include <d3d12shader.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace dx12e::shaderdiag
{
namespace
{

// ---- ルートシグネチャが受け付けるレジスタ範囲 ----
struct SlotRange
{
    char kind  = '?';
    u32  space = 0;
    u32  first = 0;
    u32  last  = 0;   // 含む。無制限テーブルは 0xFFFFFFFF
};

char KindFromRangeType(D3D12_DESCRIPTOR_RANGE_TYPE t)
{
    switch (t)
    {
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:     return 't';
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:     return 'u';
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:     return 'b';
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER: return 's';
    }
    return '?';
}

char KindFromBindType(D3D_SHADER_INPUT_TYPE t)
{
    switch (t)
    {
    case D3D_SIT_CBUFFER:
    case D3D_SIT_TBUFFER:
        return 'b';
    case D3D_SIT_SAMPLER:
        return 's';
    case D3D_SIT_UAV_RWTYPED:
    case D3D_SIT_UAV_RWSTRUCTURED:
    case D3D_SIT_UAV_RWBYTEADDRESS:
    case D3D_SIT_UAV_APPEND_STRUCTURED:
    case D3D_SIT_UAV_CONSUME_STRUCTURED:
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
    case D3D_SIT_UAV_FEEDBACKTEXTURE:
        return 'u';
    default:
        return 't';
    }
}

void AddRange(std::vector<SlotRange>& out, char kind, u32 space, u32 base, u32 count)
{
    if (kind == '?') return;
    SlotRange r{};
    r.kind  = kind;
    r.space = space;
    r.first = base;
    // NumDescriptors が UINT_MAX（無制限テーブル）なら上限なし。
    r.last  = (count == UINT_MAX || count == 0) ? 0xFFFFFFFFu : base + count - 1;
    out.push_back(r);
}

// 直列化済みルートシグネチャ blob → 受け付けるレジスタ範囲。
bool ParseRootSignature(const void* blob, size_t size, std::vector<SlotRange>& out)
{
    if (!blob || size == 0) return false;

    ComPtr<ID3D12VersionedRootSignatureDeserializer> deser;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&deser))))
        return false;

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* versioned = nullptr;
    if (FAILED(deser->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &versioned)) ||
        !versioned)
        return false;

    const D3D12_ROOT_SIGNATURE_DESC1& d = versioned->Desc_1_1;
    for (u32 i = 0; i < d.NumParameters; ++i)
    {
        const D3D12_ROOT_PARAMETER1& p = d.pParameters[i];
        switch (p.ParameterType)
        {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            for (u32 r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r)
            {
                const D3D12_DESCRIPTOR_RANGE1& range = p.DescriptorTable.pDescriptorRanges[r];
                AddRange(out, KindFromRangeType(range.RangeType), range.RegisterSpace,
                         range.BaseShaderRegister, range.NumDescriptors);
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            AddRange(out, 'b', p.Constants.RegisterSpace, p.Constants.ShaderRegister, 1);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
            AddRange(out, 'b', p.Descriptor.RegisterSpace, p.Descriptor.ShaderRegister, 1);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
            AddRange(out, 't', p.Descriptor.RegisterSpace, p.Descriptor.ShaderRegister, 1);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            AddRange(out, 'u', p.Descriptor.RegisterSpace, p.Descriptor.ShaderRegister, 1);
            break;
        default:
            break;
        }
    }
    for (u32 i = 0; i < d.NumStaticSamplers; ++i)
        AddRange(out, 's', d.pStaticSamplers[i].RegisterSpace, d.pStaticSamplers[i].ShaderRegister, 1);

    return true;
}

bool Covers(const std::vector<SlotRange>& ranges, const Binding& b)
{
    // 配列宣言（Texture2D t[4] : register(t5)）は末尾まで収まっている必要がある。
    const u32 lastUsed = (b.count == 0 || b.count == UINT_MAX)
                             ? 0xFFFFFFFFu
                             : b.reg + b.count - 1;
    for (const SlotRange& r : ranges)
    {
        if (r.kind != b.kind || r.space != b.space) continue;
        if (b.reg >= r.first && lastUsed <= r.last) return true;
    }
    return false;
}

const char* KindLabel(char kind)
{
    switch (kind)
    {
    case 'b': return "定数バッファ cbuffer";
    case 't': return "テクスチャ / 読み取りバッファ";
    case 'u': return "書き込み UAV";
    case 's': return "サンプラー";
    }
    return "";
}

std::string HrText(HRESULT hr)
{
    if (hr == E_INVALIDARG)   return "0x80070057 (E_INVALIDARG)";
    if (hr == E_OUTOFMEMORY)  return "0x8007000E (E_OUTOFMEMORY)";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// レジスタ範囲を「b0, t0-t2」のような 1 行にまとめる。
std::string CompactRanges(const std::vector<SlotRange>& ranges, char kind)
{
    std::vector<std::pair<u32, u32>> spans;
    for (const SlotRange& r : ranges)
        if (r.kind == kind && r.space == 0) spans.emplace_back(r.first, r.last);
    if (spans.empty()) return {};
    std::sort(spans.begin(), spans.end());

    std::ostringstream oss;
    bool first = true;
    for (const auto& span : spans)
    {
        if (!first) oss << ", ";
        first = false;
        oss << kind << span.first;
        if (span.second != span.first)
        {
            if (span.second == 0xFFFFFFFFu) oss << "以降すべて";
            else                            oss << "-" << kind << span.second;
        }
    }
    return oss.str();
}

std::string SlotSection(const Contract& c, const std::vector<SlotRange>& ranges)
{
    std::ostringstream oss;
    // 注記のあるスロットは 1 行ずつ丁寧に。実際に使える範囲は必ず併記する
    // （注記の書き忘れでウソを教えないため、範囲はルートシグネチャ実体から出す）。
    const char kinds[] = {'b', 't', 'u', 's'};
    for (char kind : kinds)
    {
        std::vector<std::string> lines;
        for (size_t i = 0; i < c.noteCount; ++i)
        {
            const SlotNote& n = c.notes[i];
            if (n.kind != kind) continue;
            std::ostringstream l;
            l << "    " << kind << n.reg << "  " << n.jp;
            lines.push_back(l.str());
        }
        const std::string compact = CompactRanges(ranges, kind);
        if (lines.empty() && compact.empty()) continue;

        oss << "  [" << KindLabel(kind) << "]\n";
        for (const std::string& l : lines) oss << l << "\n";
        if (!compact.empty()) oss << "    使える範囲: " << compact << "\n";
    }
    return oss.str();
}

// --- 置き場（プロセス内グローバル。エディタ UI・診断・MCP から読む）---
std::mutex& StoreMutex()
{
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, std::string>& HelpStore()
{
    static std::unordered_map<std::string, std::string> s;
    return s;
}
std::unordered_map<std::string, std::string>& IssueStore()
{
    static std::unordered_map<std::string, std::string> s;
    return s;
}

} // namespace

bool Reflect(const void* bytecode, size_t size, std::vector<Binding>& out)
{
    if (!bytecode || size == 0) return false;

    ComPtr<IDxcUtils> utils;
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || !utils)
        return false;

    DxcBuffer buf{};
    buf.Ptr      = bytecode;
    buf.Size     = size;
    buf.Encoding = 0;

    ComPtr<ID3D12ShaderReflection> refl;
    if (FAILED(utils->CreateReflection(&buf, IID_PPV_ARGS(&refl))) || !refl)
        return false;   // リフレクション部が剥がされている等。診断は諦めて汎用文言へ。

    D3D12_SHADER_DESC desc{};
    if (FAILED(refl->GetDesc(&desc))) return false;

    for (u32 i = 0; i < desc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC bd{};
        if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
        Binding b;
        b.kind  = KindFromBindType(bd.Type);
        b.reg   = bd.BindPoint;
        b.space = bd.Space;
        b.count = bd.BindCount;
        b.name  = bd.Name ? bd.Name : "";
        out.push_back(b);
    }
    return true;
}

std::vector<Binding> FindUnsupportedBindings(const Contract& c,
                                             const void* vs, size_t vsSize,
                                             const void* ps, size_t psSize)
{
    std::vector<Binding> bad;

    std::vector<SlotRange> ranges;
    if (!ParseRootSignature(c.rsBlob, c.rsBlobSize, ranges))
        return bad;   // 照合できない＝黙っておく（誤検知で嘘をつくより無言のほうがマシ）

    std::vector<Binding> used;
    const bool okVs = Reflect(vs, vsSize, used);
    const bool okPs = Reflect(ps, psSize, used);
    if (!okVs && !okPs) return bad;

    for (const Binding& b : used)
    {
        if (Covers(ranges, b)) continue;
        // 同じ register を VS/PS 両方で宣言していると 2 回出るので畳む。
        const bool dup = std::any_of(bad.begin(), bad.end(), [&](const Binding& o)
                                     { return o.kind == b.kind && o.reg == b.reg && o.space == b.space; });
        if (!dup) bad.push_back(b);
    }
    return bad;
}

std::string DescribeContract(const Contract& c)
{
    std::vector<SlotRange> ranges;
    ParseRootSignature(c.rsBlob, c.rsBlobSize, ranges);

    std::ostringstream oss;
    oss << "■ " << c.title << " の書式\n";
    if (!c.entryNote.empty())
        oss << "  エントリポイント: " << c.entryNote << "\n";
    oss << "\n"
           "  宣言してよいレジスタ（★これ以外を 1 つでも書くと PSO の生成に失敗します）\n";
    oss << SlotSection(c, ranges);
    if (!c.extra.empty())
        oss << "\n" << c.extra << "\n";
    return oss.str();
}

std::string ExplainPsoFailure(const Contract& c, HRESULT hr,
                              const void* vs, size_t vsSize,
                              const void* ps, size_t psSize,
                              const std::string& detail)
{
    std::ostringstream oss;
    oss << c.title << ": PSO（パイプライン）の生成に失敗しました";
    if (hr != S_OK) oss << "  hr=" << HrText(hr);
    oss << "\n";
    if (!detail.empty()) oss << "  " << detail << "\n";
    oss << "\n";

    const std::vector<Binding> bad = FindUnsupportedBindings(c, vs, vsSize, ps, psSize);
    if (!bad.empty())
    {
        oss << "■ 原因: このパスに無いレジスタを宣言しています\n";
        for (const Binding& b : bad)
        {
            oss << "    NG  " << b.kind << b.reg;
            if (b.space != 0)    oss << " (space" << b.space << ")";
            if (b.count > 1)     oss << " [" << b.count << " 個の配列]";
            if (!b.name.empty()) oss << "  \"" << b.name << "\"";
            oss << "\n";
        }
        oss << "  → この宣言を消すか、下の「使えるレジスタ」へ付け替えてください。\n"
               "     （使っていなくても、宣言しただけでルートシグネチャに要求されます）\n\n";
    }
    else
    {
        oss << "■ 原因: レジスタの不一致は見つかりませんでした。次を確認してください\n"
               "    ・PSMain の戻り値が  float4 ... : SV_TARGET  になっているか\n"
               "    ・VSMain の出力に  float4 ... : SV_POSITION  があるか\n"
               "    ・頂点入力（VSInput）の並びがこのパスの約束と合っているか\n"
               "    ・cbuffer のサイズがエンジンの送る DWORD 数を超えていないか\n\n";
    }

    oss << DescribeContract(c);
    oss << "\n  ※ 直るまでは、このシェーダーを割り当てた対象は既定の描画に戻ります（絵は消えません）。\n";
    return oss.str();
}

void RegisterHelp(const std::string& contractId, std::string text)
{
    std::lock_guard<std::mutex> lock(StoreMutex());
    HelpStore()[contractId] = std::move(text);
}

std::string GetHelp(const std::string& contractId)
{
    std::lock_guard<std::mutex> lock(StoreMutex());
    auto it = HelpStore().find(contractId);
    return it != HelpStore().end() ? it->second : std::string();
}

void SetIssue(const std::string& key, std::string text)
{
    const std::string k = NormalizeKey(key);
    std::lock_guard<std::mutex> lock(StoreMutex());
    IssueStore()[k] = std::move(text);
}

void ClearIssue(const std::string& key)
{
    const std::string k = NormalizeKey(key);
    std::lock_guard<std::mutex> lock(StoreMutex());
    IssueStore().erase(k);
}

std::string GetIssue(const std::string& key)
{
    const std::string k = NormalizeKey(key);
    std::lock_guard<std::mutex> lock(StoreMutex());
    auto it = IssueStore().find(k);
    return it != IssueStore().end() ? it->second : std::string();
}

std::string FindIssueKeyIn(const std::string& text)
{
    std::string hay = text;
    for (char& ch : hay)
    {
        if (ch == '\\') ch = '/';
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    std::string best;
    std::lock_guard<std::mutex> lock(StoreMutex());
    for (const auto& kv : IssueStore())
    {
        // 同じ名前で長さ違いのキー（foo.hlsl と sub/foo.hlsl）があり得るので長い方を勝たせる。
        if (kv.first.empty() || kv.first.size() <= best.size()) continue;
        if (hay.find(kv.first) != std::string::npos) best = kv.first;
    }
    return best;
}

std::string NormalizeKey(const std::string& shaderRel)
{
    std::string key = shaderRel;
    for (char& ch : key)
    {
        if (ch == '\\') ch = '/';
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (key.rfind("shaders/", 0) == 0) key.erase(0, 8);
    return key;
}

} // namespace dx12e::shaderdiag
