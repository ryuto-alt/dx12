// DredReportTests（gpu）: W05-05。（合成データの JSON 検査 + 実デバイスの RemoveDevice）
//   ・純関数 Decide / ParseCommandLine（既定: エディタ ON・GameRuntime OFF・--dred・--no-dred）
//   ・テスト用デバイスで BeginEvent("TestPass") を打ち、RemoveDevice() → JSON が書かれ、removedReason が入り、パースでき、
//     パンくずのコンテキストに "TestPass" がある（本物のページフォルトは起こさない）
//   ・Texture / Buffer / RenderTarget の作成時に名前が付く（ページフォルトの割り当て名が空にならない）
//   ・JSON の割り当て名は空にならない（名前が無ければ "(unnamed)"）
#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Window.h"
#include "graphics/Buffer.h"
#include "graphics/CommandQueue.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/DredReport.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/RenderTarget.h"
#include "graphics/Texture.h"

using Microsoft::WRL::ComPtr;
using namespace dx12e;

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("FAIL: " __VA_ARGS__);            \
            std::printf("\n");                            \
            ++g_fail;                                     \
        }                                                 \
    } while (0)

static bool HasName(ID3D12Object* o)
{
    UINT size = 0;
    return o && SUCCEEDED(o->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, nullptr)) && size > 0;
}

static void TestDecide()
{
    using dred::CmdLine;
    using dred::Decide;
    CHECK(Decide(true, CmdLine::None, false), "エディタの既定は ON");
    CHECK(!Decide(true, CmdLine::None, true), "GameRuntime の既定は OFF");
    CHECK(Decide(true, CmdLine::ForceOn, true), "--dred は GameRuntime でも ON");
    CHECK(!Decide(true, CmdLine::ForceOff, false), "--no-dred は OFF");
    CHECK(!Decide(false, CmdLine::ForceOn, false), "要求が false（コードからの明示）は --dred でも OFF");
    // 出荷したゲームは exe が <productName>.exe に改名される。このテスト exe の名前に gameruntime は含まれないが、
    // gameMode=true なら exe 名に関係なく既定 OFF になること（コマンドラインに --dred / --no-dred が無い前提）。
    CHECK(!dred::IsGameRuntimeExe(), "テスト exe は gameruntime を含まない名前（前提）");
    if (dred::ParseCommandLine(::GetCommandLineW()) == CmdLine::None)
    {
        CHECK(!dred::ResolveForMode(true), "改名した exe でも gameMode=true なら DRED 既定 OFF");
        CHECK(dred::ResolveForMode(false), "エディタ（gameMode=false）は既定 ON");
    }
    CHECK(dred::ParseCommandLine(L"DX12Engine.exe --background --no-dred") == CmdLine::ForceOff, "--no-dred の解析");
    CHECK(dred::ParseCommandLine(L"\"C:\\a b\\GameRuntime.exe\" --dred") == CmdLine::ForceOn, "--dred の解析（引用符つき exe）");
    CHECK(dred::ParseCommandLine(L"DX12Engine.exe --dredx --project d") == CmdLine::None, "前方一致で拾わない");
    CHECK(dred::ParseCommandLine(L"x --dred --no-dred") == CmdLine::ForceOff, "後ろが勝つ");
    CHECK(dred::ParseCommandLine(nullptr) == CmdLine::None, "null");
}

int main()
{
    TestDecide();

    namespace fs = std::filesystem;
    const fs::path outDir = fs::temp_directory_path() / "dx12_dred_report_test";
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);

    Window window;
    GraphicsDevice dev;
    GraphicsDeviceOptions opt;
    opt.dred = true;
    try { dev.Initialize(window, opt); }
    catch (...) { std::printf("FAIL: GraphicsDevice::Initialize\n"); return 1; }
    CHECK(dev.GetOptions().dred, "DRED が有効になっていない（テスト exe に --no-dred が付いている？）");

    // リソースの名前（SetName が付く）
    {
        CommandQueue queue;
        queue.Initialize(dev, D3D12_COMMAND_LIST_TYPE_DIRECT);
        ConstantBuffer cb;
        cb.Initialize(dev, 256, 2);
        CHECK(HasName(cb.GetResource()), "Buffer に名前が付かない");
        DescriptorHeap rtvHeap, srvHeap;
        rtvHeap.Initialize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, false);
        srvHeap.Initialize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true);
        RenderTarget rt;
        const float clear[4] = {0, 0, 0, 1};
        rt.Initialize(dev, &rtvHeap, &srvHeap, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, clear);
        CHECK(HasName(rt.GetResource()), "RenderTarget に名前が付かない");
        // Texture::Initialize の自動 SetName（アップロード用の一時バッファ m_uploadBuffer / Buffer の staging は短命なので無名のまま）
        {
            ComPtr<ID3D12CommandAllocator> a;
            ComPtr<ID3D12GraphicsCommandList> l;
            if (SUCCEEDED(dev.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
                SUCCEEDED(dev.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l))))
            {
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                const unsigned int px[16] = {};
                D3D12_SUBRESOURCE_DATA sd{};
                sd.pData = px; sd.RowPitch = 16; sd.SlicePitch = 64;
                Texture tex;
                tex.Initialize(dev, l.Get(), td, &sd, 1);
                CHECK(HasName(tex.GetResource()), "Texture に名前が付かない");
                l->Close();
                queue.ExecuteCommandList(l.Get());
                queue.WaitIdle();
            }
            else CHECK(false, "Texture 用のコマンドリストを作れない");
        }
        queue.WaitIdle();
    }

    // 手で組んだ DRED のデータから JSON を作る（この環境のドライバ/OS が RemoveDevice で DRED を埋めないため、中身はここで確かめる）。
    {
        static const wchar_t kCtx[] = L"TestPass";
        D3D12_DRED_BREADCRUMB_CONTEXT ctx{};
        ctx.BreadcrumbIndex = 1;
        ctx.pContextString = kCtx;
        static const D3D12_AUTO_BREADCRUMB_OP kOps[] = {
            D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT, D3D12_AUTO_BREADCRUMB_OP_DISPATCH, D3D12_AUTO_BREADCRUMB_OP_ENDEVENT};
        UINT last = 2;
        D3D12_AUTO_BREADCRUMB_NODE1 node{};
        node.pCommandListDebugNameW = L"MainCmdList";
        node.pCommandQueueDebugNameW = L"DirectQueue";
        node.BreadcrumbCount = 3;
        node.pLastBreadcrumbValue = &last;
        node.pCommandHistory = kOps;
        node.BreadcrumbContextsCount = 1;
        node.pBreadcrumbContexts = &ctx;
        D3D12_DRED_ALLOCATION_NODE1 named{};
        named.ObjectNameW = L"Texture 256x256 mip=1 fmt=28";
        named.AllocationType = D3D12_DRED_ALLOCATION_TYPE_RESOURCE;
        D3D12_DRED_ALLOCATION_NODE1 unnamed{};
        unnamed.AllocationType = D3D12_DRED_ALLOCATION_TYPE_HEAP;
        named.pNext = &unnamed;

        dred::ReportData d;
        d.breadcrumbs.pHeadAutoBreadcrumbNode = &node;
        d.pageFault.PageFaultVA = 0x1234;
        d.pageFault.pHeadExistingAllocationNode = &named;
        d.hasPageFault = true;
        d.deviceState = D3D12_DRED_DEVICE_STATE_PAGEFAULT;
        d.hasState = true;
        const std::string text = dred::BuildReportJsonFromData(d, DXGI_ERROR_DEVICE_HUNG, true);
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        CHECK(!j.is_discarded(), "合成データの JSON をパースできない");
        if (!j.is_discarded())
        {
            CHECK(j["removedReason"] == "0x00000000887A0006", "removedReason");
            CHECK(j["deviceState"] == "pagefault", "deviceState");
            CHECK(j["breadcrumbs"].size() == 1, "パンくずが 1 件");
            if (j["breadcrumbs"].size() == 1)
            {
                const auto& b = j["breadcrumbs"][0];
                CHECK(b["commandList"] == "MainCmdList" && b["commandQueue"] == "DirectQueue", "コマンドリスト / キュー名");
                CHECK(b["lastCompleted"] == 2 && b["lastCompletedOp"] == "Dispatch" && b["nextOp"] == "EndEvent", "最後に完了した op");
                CHECK(b["contexts"].size() == 1 && b["contexts"][0]["context"] == "TestPass", "コンテキスト文字列 TestPass");
            }
            CHECK(j["pageFault"]["va"] == "0x0000000000001234", "ページフォルトの VA");
            CHECK(j["pageFault"]["existing"].size() == 2, "既存の割り当て 2 件");
            if (j["pageFault"]["existing"].size() == 2)
            {
                CHECK(j["pageFault"]["existing"][0]["name"] == "Texture 256x256 mip=1 fmt=28", "割り当て名");
                CHECK(j["pageFault"]["existing"][1]["name"] == "(unnamed)", "名前なしは (unnamed)（空にしない）");
            }
        }
    }

    // 本物のデバイスで RemoveDevice → JSON が書かれ、removedReason が入り、パースできる。
    //   キューを未 Signal のフェンスで待たせて、リストを未完了のまま RemoveDevice する（完了したリストは DRED から落ちる）。
    bool envSkipped = false;
    ID3D12Device5* d5 = dev.GetDevice();
    {
        CommandQueue queue;
        queue.Initialize(dev, D3D12_COMMAND_LIST_TYPE_DIRECT);
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(d5->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(d5->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
        {
            std::printf("FAIL: コマンドリストを作れない\n");
            return 1;
        }
        alloc->SetName(L"DredTestAllocator");
        list->SetName(L"DredTestList");
        static const wchar_t kEvent[] = L"TestPass";
        list->BeginEvent(0 /*PIX_EVENT_UNICODE_VERSION*/, kEvent, sizeof(kEvent));
        list->SetMarker(0, kEvent, sizeof(kEvent));
        list->EndEvent();
        list->Close();
        // キューを「決して Signal しないフェンス」で待たせ、コマンドリストが RemoveDevice 時点で未完了のまま残るようにする
        // （GPU が終わらせたリストは DRED のパンくずから落ちるので、待たせないと空になるのが普通）。
        ComPtr<ID3D12Fence> gate;
        if (FAILED(d5->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))))
        {
            std::printf("FAIL: フェンスを作れない\n");
            return 1;
        }
        CHECK(SUCCEEDED(queue.GetQueue()->Wait(gate.Get(), 1)), "キューの Wait に失敗");
        queue.ExecuteCommandList(list.Get());

        CHECK(SUCCEEDED(d5->GetDeviceRemovedReason()), "RemoveDevice の前にデバイスが消えている");
        d5->RemoveDevice();
        const HRESULT reason = d5->GetDeviceRemovedReason();
        CHECK(FAILED(reason), "RemoveDevice の後に GetDeviceRemovedReason が失敗しない");
        const std::string path = dred::WriteReport(d5, reason, dev.GetOptions().dred, outDir);
        CHECK(!path.empty(), "レポートを書けない");
        gate->Signal(1);   // レポートを書いた後に解放（キューの破棄が待ちで止まらないように）
        if (!path.empty())
        {
            std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
            CHECK(!j.is_discarded(), "JSON をパースできない");
            if (!j.is_discarded())
            {
                CHECK(j.contains("removedReason") && j["removedReason"].is_string() && !j["removedReason"].get<std::string>().empty(),
                      "removedReason が無い");
                CHECK(j.value("dredEnabled", false), "dredEnabled が false");
                CHECK(j.contains("breadcrumbs") && j.contains("pageFault"), "breadcrumbs / pageFault の欄が無い");
                // この環境のドライバ/OS が RemoveDevice で DRED を埋めない（deviceState=unknown かつパンくず 0 件）ときは、
                // 実デバイスのパンくず検査だけを環境スキップにする（合成データの検査は上で必須。終了コード 77 = ctest の SKIP）。
                const bool envNoDred = j["breadcrumbs"].empty() && j.value("deviceState", "") == "unknown";
                if (envNoDred)
                {
                    envSkipped = true;
                    std::printf("SKIP(env): この環境は RemoveDevice で DRED を埋めない（deviceState=unknown, パンくず 0 件）。実デバイスのパンくず検査を省略\n");
                }
                else
                {
                    bool foundCtx = false;
                    for (const auto& b : j["breadcrumbs"])
                        for (const auto& c : b["contexts"])
                            if (c.value("context", "") == "TestPass") foundCtx = true;
                    // 未完了のリストが残っているので、パンくずとコンテキストは必須。
                    CHECK(!j["breadcrumbs"].empty(), "未完了のコマンドリストがあるのにパンくずが空");
                    CHECK(foundCtx, "コンテキスト TestPass が無い");
                    bool foundList = false;
                    for (const auto& b : j["breadcrumbs"])
                        if (b.value("commandList", "") == "DredTestList")
                        {
                            foundList = true;
                            // lastCompleted == 0（1 つも終わっていない）は正常。lastCompletedOp は 0 より大きいときだけ入る。
                            CHECK(b.contains("lastCompleted") && b.contains("ops") && b.contains("opsTotal"), "lastCompleted / ops / opsTotal が無い");
                            const unsigned last = b.value("lastCompleted", 0u);
                            const unsigned total = b.value("breadcrumbCount", 0u);
                            CHECK(total > 0 && last <= total, "lastCompleted と breadcrumbCount が矛盾");
                            if (last > 0) CHECK(b.contains("lastCompletedOp"), "lastCompleted > 0 なのに lastCompletedOp が無い");
                            if (last < total) CHECK(b.contains("nextOp"), "未完了なのに nextOp が無い");
                        }
                    CHECK(foundList, "commandList == DredTestList のノードが無い");
                }
                std::printf("NOTE: breadcrumbsHr=%s deviceState=%s\n", j.value("breadcrumbsHr", "?").c_str(), j.value("deviceState", "?").c_str());
                std::printf("NOTE: 実デバイスのパンくず %zu\n", j["breadcrumbs"].size());
            }
        }
    }

    // null デバイスでもレポートを書ける（落ちない）。
    {
        const std::string p2 = dred::WriteReport(nullptr, DXGI_ERROR_DEVICE_HUNG, false, outDir);
        CHECK(!p2.empty(), "null デバイスでレポートを書けない");
    }

    fs::remove_all(outDir, ec);
    if (g_fail == 0) std::printf(envSkipped ? "OK (一部を環境スキップ)\n" : "OK\n");
    if (g_fail != 0) return 1;
    return envSkipped ? 77 : 0;   // 77 = ctest の SKIP_RETURN_CODE
}
