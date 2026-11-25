#include "DredReport.h"

#include <nlohmann/json.hpp>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include "core/CrashReport.h"

using Microsoft::WRL::ComPtr;

namespace dx12e::dred
{

namespace
{

std::string WideToUtf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.pop_back();
    return s;
}

std::string AnsiToUtf8(const char* a)
{
    if (!a || !*a) return {};
    const int wn = ::MultiByteToWideChar(CP_ACP, 0, a, -1, nullptr, 0);
    if (wn <= 1) return {};
    std::wstring w(static_cast<size_t>(wn), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, a, -1, w.data(), wn);
    return WideToUtf8(w.c_str());
}

// ObjectNameW を優先し、無ければ ObjectNameA。どちらも無ければ "(unnamed)"（空にしない）。
std::string NameOf(const wchar_t* w, const char* a)
{
    std::string s = WideToUtf8(w);
    if (s.empty()) s = AnsiToUtf8(a);
    return s.empty() ? std::string("(unnamed)") : s;
}

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
    case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
    default: return "Other";
    }
}

const char* AllocTypeName(D3D12_DRED_ALLOCATION_TYPE t)
{
    switch (t)
    {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "CommandQueue";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "CommandAllocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PipelineState";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "CommandList";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "Fence";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DescriptorHeap";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "Heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QueryHeap";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "CommandSignature";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "PipelineLibrary";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "Resource";
    case D3D12_DRED_ALLOCATION_TYPE_PASS: return "Pass";
    case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "StateObject";
    case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "MetaCommand";
    default: return "Other";
    }
}

// 連結リストは壊れていても無限ループしないよう、個数に上限を付けて辿る。
constexpr int kMaxNodes = 256;
constexpr UINT kMaxOpsPerList = 4096;

nlohmann::json AllocationList(const D3D12_DRED_ALLOCATION_NODE1* head)
{
    nlohmann::json arr = nlohmann::json::array();
    int n = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1* p = head; p && n < kMaxNodes; p = p->pNext, ++n)
        arr.push_back({{"name", NameOf(p->ObjectNameW, p->ObjectNameA)}, {"type", AllocTypeName(p->AllocationType)}});
    return arr;
}

const char* DeviceStateName(D3D12_DRED_DEVICE_STATE s)
{
    switch (s)
    {
    case D3D12_DRED_DEVICE_STATE_UNKNOWN: return "unknown";
    case D3D12_DRED_DEVICE_STATE_HUNG: return "hung";
    case D3D12_DRED_DEVICE_STATE_FAULT: return "fault";
    case D3D12_DRED_DEVICE_STATE_PAGEFAULT: return "pagefault";
    default: return "none";
    }
}

std::string UtcStamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

std::string Hex(UINT64 v)
{
    char b[32]{};
    std::snprintf(b, sizeof(b), "0x%016llX", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

CmdLine ParseCommandLine(const wchar_t* commandLine)
{
    if (!commandLine) return CmdLine::None;
    // 空白区切りの単語として --dred / --no-dred を探す（後ろに書いたものが勝つ）。
    CmdLine r = CmdLine::None;
    const std::wstring s(commandLine);
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
        size_t j = i;
        bool quoted = false;
        while (j < s.size() && (quoted || (s[j] != L' ' && s[j] != L'\t')))
        {
            if (s[j] == L'"') quoted = !quoted;
            ++j;
        }
        const std::wstring w = s.substr(i, j - i);
        if (w == L"--dred") r = CmdLine::ForceOn;
        else if (w == L"--no-dred") r = CmdLine::ForceOff;
        i = j;
    }
    return r;
}

bool IsGameRuntimeExe()
{
    wchar_t exe[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0) return false;
    std::wstring stem = std::filesystem::path(std::wstring(exe, n)).stem().wstring();
    for (auto& c : stem) c = static_cast<wchar_t>(::towlower(c));
    return stem.find(L"gameruntime") != std::wstring::npos;
}

bool Decide(bool requested, CmdLine cmd, bool gameRuntime)
{
    if (cmd == CmdLine::ForceOff) return false;
    if (!requested) return false;
    if (cmd == CmdLine::ForceOn) return true;
    return !gameRuntime;
}

bool ResolveEnabled(bool requested)
{
    return Decide(requested, ParseCommandLine(::GetCommandLineW()), IsGameRuntimeExe());
}

bool ResolveForMode(bool gameMode)
{
    return Decide(true, ParseCommandLine(::GetCommandLineW()), gameMode);
}

bool EnableBeforeDeviceCreation(bool on)
{
    // 設定はデバイス作成前に適用すればよく、保持は不要（ローカル。static を持たないのでスレッド競合もない）。
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> s;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&s))) || !s) return false;
    const D3D12_DRED_ENABLEMENT e = on ? D3D12_DRED_ENABLEMENT_FORCED_ON : D3D12_DRED_ENABLEMENT_FORCED_OFF;
    s->SetAutoBreadcrumbsEnablement(e);
    s->SetPageFaultEnablement(e);
    s->SetBreadcrumbContextEnablement(e);
    return true;
}

std::string BuildReportJsonFromData(const ReportData& d, HRESULT removedReason, bool dredEnabled)
{
    nlohmann::json j;
    try
    {
        j["schema"] = 1;
        j["removedReason"] = Hex(static_cast<UINT>(removedReason));
        j["removedReasonCode"] = static_cast<long long>(static_cast<UINT>(removedReason));
        j["dredEnabled"] = dredEnabled;
        j["breadcrumbs"] = nlohmann::json::array();
        j["pageFault"] = {{"va", Hex(0)}, {"existing", nlohmann::json::array()}, {"recentFreed", nlohmann::json::array()}};
        if (d.hasState) j["deviceState"] = DeviceStateName(d.deviceState);
        j["breadcrumbsHr"] = Hex(static_cast<UINT>(d.breadcrumbsHr));

        int n = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* p = d.breadcrumbs.pHeadAutoBreadcrumbNode; p && n < kMaxNodes; p = p->pNext, ++n)
        {
            nlohmann::json node;
            node["commandList"] = NameOf(p->pCommandListDebugNameW, p->pCommandListDebugNameA);
            node["commandQueue"] = NameOf(p->pCommandQueueDebugNameW, p->pCommandQueueDebugNameA);
            const UINT total = p->BreadcrumbCount;
            const UINT last = p->pLastBreadcrumbValue ? *p->pLastBreadcrumbValue : 0;
            node["breadcrumbCount"] = total;
            node["lastCompleted"] = last;
            // last は「完了した個数」。0 なら 1 つも終わっていない。
            if (p->pCommandHistory && last > 0 && last <= total)
                node["lastCompletedOp"] = OpName(p->pCommandHistory[last - 1]);
            if (p->pCommandHistory && last < total)
                node["nextOp"] = OpName(p->pCommandHistory[last]);
            nlohmann::json ops = nlohmann::json::array();
            if (p->pCommandHistory)
                for (UINT k = 0; k < total && k < kMaxOpsPerList; ++k)
                    ops.push_back(OpName(p->pCommandHistory[k]));
            node["ops"] = std::move(ops);
            nlohmann::json ctx = nlohmann::json::array();
            if (p->pBreadcrumbContexts)
                for (UINT k = 0; k < p->BreadcrumbContextsCount && k < kMaxOpsPerList; ++k)
                    ctx.push_back({{"index", p->pBreadcrumbContexts[k].BreadcrumbIndex},
                                   {"context", WideToUtf8(p->pBreadcrumbContexts[k].pContextString)}});
            node["contexts"] = std::move(ctx);
            j["breadcrumbs"].push_back(std::move(node));
        }

        if (d.hasPageFault)
        {
            j["pageFault"] = {{"va", Hex(d.pageFault.PageFaultVA)},
                              {"existing", AllocationList(d.pageFault.pHeadExistingAllocationNode)},
                              {"recentFreed", AllocationList(d.pageFault.pHeadRecentFreedAllocationNode)}};
        }
    }
    catch (...)
    {
        j["error"] = "report building failed";
    }
    try { return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace); }
    catch (...) { return "{}"; }
}

std::string BuildReportJson(ID3D12Device* device, HRESULT removedReason, bool dredEnabled)
{
    ReportData d;
    try
    {
        ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
        if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred2))) && dred2)
        {
            d.deviceState = dred2->GetDeviceState();
            d.hasState = true;
            d.breadcrumbsHr = dred2->GetAutoBreadcrumbsOutput1(&d.breadcrumbs);
            if (FAILED(d.breadcrumbsHr)) d.breadcrumbs = {};
            d.hasPageFault = SUCCEEDED(dred2->GetPageFaultAllocationOutput1(&d.pageFault));
        }
    }
    catch (...) {}
    return BuildReportJsonFromData(d, removedReason, dredEnabled);
}

std::filesystem::path DefaultReportDir()
{
    return CrashReport::DefaultRoot();
}

std::string WriteReport(ID3D12Device* device, HRESULT removedReason, bool dredEnabled, const std::filesystem::path& dirIn)
{
    try
    {
        namespace fs = std::filesystem;
        const fs::path dir = dirIn.empty() ? DefaultReportDir() : dirIn;
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string body = BuildReportJson(device, removedReason, dredEnabled);
        fs::path path = dir / ("device_removed_" + UtcStamp() + ".json");
        for (int i = 2; fs::exists(path, ec) && i < 100; ++i)
            path = dir / ("device_removed_" + UtcStamp() + "_" + std::to_string(i) + ".json");
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        f.close();
        if (!f) return {};
        // 古い device_removed_*.json を落とす（CrashReport はディレクトリしか数えないので、ここで新しい 20 件に絞る）。
        try
        {
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(dir, ec))
            {
                const std::wstring n = e.path().filename().wstring();
                if (n.rfind(L"device_removed_", 0) == 0 && e.path().extension() == L".json") files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());   // 名前に UTC 時刻が入っているので辞書順 = 古い順
            constexpr size_t kKeep = 20;
            for (size_t i = 0; files.size() > kKeep && i + kKeep < files.size(); ++i) fs::remove(files[i], ec);
        }
        catch (...) {}
        const std::u8string u8 = path.u8string();
        return std::string(u8.begin(), u8.end());
    }
    catch (...)
    {
        return {};
    }
}

void SetNameIfUnnamed(ID3D12Object* object, const wchar_t* name)
{
    if (!object || !name) return;
    UINT size = 0;
    if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, nullptr)) && size > 0) return;
    object->SetName(name);
}

void NameTexture(ID3D12Object* object, const wchar_t* kind, UINT64 width, UINT height, UINT mips, DXGI_FORMAT format)
{
    wchar_t buf[96]{};
    _snwprintf_s(buf, _TRUNCATE, L"%s %llux%u mip=%u fmt=%d", kind, static_cast<unsigned long long>(width), height, mips,
                 static_cast<int>(format));
    SetNameIfUnnamed(object, buf);
}

void NameBuffer(ID3D12Object* object, const wchar_t* kind, UINT64 bytes)
{
    wchar_t buf[64]{};
    _snwprintf_s(buf, _TRUNCATE, L"%s %lluB", kind, static_cast<unsigned long long>(bytes));
    SetNameIfUnnamed(object, buf);
}

} // namespace dx12e::dred
