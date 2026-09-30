// マテリアルグラフ G2b: GraphMaterialSystem（グラフ材質のランタイム）の実機検証。
//
//   実 GPU（ハードウェアアダプタ + SM 6.6 + Resource Binding Tier 3）と dxcompiler.dll が要る。無い環境は SKIP（0 で終わる）。
//   環境変数 DX12_D3D_DEBUG=1 でデバッグレイヤの指摘をコンソールへ出す。
//
//   1. 初回ビルド: ワーカーが DXC + PSO を作る間 Resolve は false（= 従来の代理材質で描く）→ 完了後 true（PSO + レコード）。
//   2. パラメータプール: 材質ごとにレコードのオフセットが違う / 値がレコードに書かれる（テクスチャは SRV 添字 + 1/幅 + 1/高さ）。
//   3. 値の編集は再コンパイルしない: SetGraphParam → DXC 呼び出し数・generation が不変、レコードだけ更新。
//   4. HLSL が変わる編集（構造の変更）: ワーカーが作る間は旧 PSO で描き続け（旧版維持）、完了後に新 PSO へ切り替わる。
//   5. コンパイルエラー: 旧 PSO のまま描画が止まらず、エラーは nodeId つきで保持される。直すと復帰する。
//   6. 同じ HLSL の材質は変種（PSO）を共有する。
//   7. ディスクキャッシュ: 別のインスタンス（別プロセス相当）が同じキャッシュを使うと DXC を 1 回も呼ばない。
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/PakWriter.h"
#include "core/vfs/Vfs.h"
#include "core/Window.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/RootSignature.h"
#include "graphics/Texture.h"
#include "renderer/GraphMaterialSystem.h"
#include "renderer/matgraph/GraphIO.h"
#include "resource/MaterialAssetManager.h"
#include "resource/ModelLoader.h"
#include "resource/ResourceManager.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace dx12e;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace mg = dx12e::matgraph;

namespace
{
int g_failures = 0;
int g_checks = 0;
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

bool EnvSet(const char* name)
{
    char* buf = nullptr;
    size_t len = 0;
    const bool set = (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr);
    std::free(buf);
    return set;
}

void WriteText(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void WriteBmp(const fs::path& path, uint8_t r, uint8_t g, uint8_t b, int W = 4, int H = 4)
{
    std::vector<uint8_t> f(54 + W * H * 3, 0);
    auto le32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) f[o + i] = uint8_t(v >> (8 * i)); };
    f[0] = 'B'; f[1] = 'M';
    le32(2, uint32_t(f.size())); le32(10, 54); le32(14, 40); le32(18, W); le32(22, H);
    f[26] = 1; f[28] = 24; le32(34, W * H * 3);
    for (int i = 0; i < W * H; ++i) { f[54 + i * 3 + 0] = b; f[54 + i * 3 + 1] = g; f[54 + i * 3 + 2] = r; }
    fs::create_directories(path.parent_path());
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

struct Exec
{
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    bool Init(ID3D12Device* dev)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return false;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }
    void Submit()
    {
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        q->ExecuteCommandLists(1, l);
        ++fv;
        q->Signal(fence.Get(), fv);
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
    }
};

// グラフ（.dxmg）を書く: BaseColor = Tint（VectorParameter）* Albedo テクスチャ、Roughness = R（ScalarParameter）
//   variant 0: そのまま / variant 1: Roughness に Multiply(R, 0.5) を挟む（構造が変わる = HLSL が変わる）/ variant 2: Custom ノードに壊れた HLSL
std::string BuildGraph(int variant)
{
    mg::MaterialGraph g;
    g.SetGuid("00c0ffee");
    g.SetName("gm_test");
    g.AddNode(mg::kOutputNodeType, 600, 100, "out");
    g.AddNode("VectorParameter", 0, 0, "tint");
    g.SetNodeProp("tint", "name", "Tint");
    g.SetNodeProp("tint", "default", mg::Json::array({1.0, 0.5, 0.25, 1.0}));
    g.AddNode("TextureParameter", 0, 200, "tex");
    g.SetNodeProp("tex", "name", "Albedo");
    g.SetNodeProp("tex", "sampler", "Color");
    g.AddNode("TexCoord", 0, 400, "uv");
    g.AddNode("TextureSample", 200, 200, "smp");
    g.Connect("tex", "Out", "smp", "Tex");
    g.Connect("uv", "UV", "smp", "UV");
    g.AddNode("Multiply", 400, 100, "mul");
    g.Connect("tint", "RGB", "mul", "A");
    g.Connect("smp", "RGB", "mul", "B");
    g.Connect("mul", "Out", "out", "BaseColor");
    g.AddNode("ScalarParameter", 0, 600, "rough");
    g.SetNodeProp("rough", "name", "R");
    g.SetNodeProp("rough", "default", 0.5);
    if (variant == 1)
    {
        g.AddNode("Multiply", 400, 600, "half");
        g.Connect("rough", "Out", "half", "A");
        g.SetLiteral("half", "B", mg::Value::Float(0.5f));
        g.Connect("half", "Out", "out", "Roughness");
    }
    else if (variant == 2)
    {
        g.AddNode("Custom", 400, 600, "cu");
        g.SetNodeProp("cu", "code", "return no_such_function(In0);");
        g.SetNodeProp("cu", "outputType", "float");
        g.Connect("rough", "Out", "cu", "In0");
        g.Connect("cu", "Out", "out", "Roughness");
    }
    else
        g.Connect("rough", "Out", "out", "Roughness");
    return mg::SaveDxmg(g, false);
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (EnvSet("DX12_D3D_DEBUG")) Logger::Init();
    Window window;
    GraphicsDevice dev;
    try { dev.Initialize(window); }
    catch (...) { std::printf("SKIP: D3D12 デバイスが作れない（GPU なし）\n"); return 0; }
    if (!dev.SupportsDynamicResources())
    {
        std::printf("SKIP: SM 6.6 + Resource Binding Tier 3 が無い GPU\n");
        return 0;
    }
    RootSignature rs;
    rs.Initialize(dev);
    if (!rs.IsBindless()) { std::printf("SKIP: メイン RS がバインドレスでない（DX12_DISABLE_MAIN_BINDLESS）\n"); return 0; }
    {
        HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
        if (!dll) { std::printf("SKIP: dxcompiler.dll が読めない\n"); return 0; }
    }
    _putenv_s("UNO_SHADER_SRC_DIR", DX12E_SHADER_DIR);   // ForwardGraph.hlsl の場所（Core の PathResolver を初期化しない）

    Exec ex;
    CHECK(ex.Init(dev.GetDevice()));
    DescriptorHeap srvHeap;
    srvHeap.Initialize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true);
    ResourceManager rm;
    rm.Initialize(&dev, &srvHeap, ex.list.Get());
    ex.Submit();
    rm.FinishUploads();
    MaterialAssetManager mam;
    mam.Initialize(&rm, &dev, &srvHeap);

    // ---- 使い捨てプロジェクト ----
    const fs::path proj = fs::temp_directory_path() / ("dx12e_graph_material_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(proj, ec);
    PathResolver::SetProjectRoot(proj.generic_string());
    const fs::path assets = proj / "assets";
    WriteBmp(assets / "textures" / "a.bmp", 200, 100, 50, 8, 4);
    WriteBmp(assets / "textures" / "b.bmp", 10, 20, 30, 4, 4);   // 行のバイト数が 4 の倍数（BMP は行を 4 バイト境界へ詰める。2x2 の 24bit は壊れた BMP になる）
    WriteText(assets / "materials" / "g1.dxmg", BuildGraph(0));
    WriteText(assets / "materials" / "m1.dxmat", "{\n  \"graph\": \"materials/g1.dxmg\",\n  \"name\": \"m1\",\n  \"params\": {\n    \"Albedo\": \"textures/a.bmp\",\n    \"R\": 0.8\n  },\n  \"version\": 2\n}");
    WriteText(assets / "materials" / "m2.dxmat", "{\n  \"graph\": \"materials/g1.dxmg\",\n  \"name\": \"m2\",\n  \"params\": {\n    \"Albedo\": \"textures/b.bmp\",\n    \"R\": 0.25,\n    \"Tint\": [0.1, 0.2, 0.3]\n  },\n  \"version\": 2\n}");
    const std::string cacheDir = (proj / "shadercache").generic_string();

    GraphMaterialSystem::InitDesc d;
    d.device = &dev;
    d.rootSignature = &rs;
    d.resources = &rm;
    d.srvHeap = &srvHeap;
    d.colorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.depthFormat = DXGI_FORMAT_D32_FLOAT;
    d.allowCompile = true;
    d.pollFiles = false;             // 更新の通知は NotifyGraphFileChanged で明示する（時刻の解像度に依存しない）
    d.engineVersion = "test";
    d.cacheDir = cacheDir;

    auto sys = std::make_unique<GraphMaterialSystem>();
    CHECK(sys->Initialize(d));
    CHECK(sys->IsAvailable());
    if (!sys->IsAvailable()) return 1;

    ID3D12GraphicsCommandList* cmd = ex.list.Get();
    u32 frame = 0;
    auto entryOf = [&](const char* rel) { return mam.GetOrLoad(rel, cmd); };
    auto resolve = [&](const char* rel, GraphMaterialSystem::DrawParams& out) {
        const MaterialAssetManager::Entry* e = entryOf(rel);
        return e && e->valid && sys->Resolve(rel, *e, cmd, frame % 3, false, out);
    };
    auto pump = [&]() { sys->Update(0.016f, frame % 3, cmd); ++frame; };

    // ---- 1. 初回ビルド（非同期・完了まで false）------------------------------------------------
    std::printf("stage: first build\n");
    sys->DebugSetWorkerDelayMs(300);
    GraphMaterialSystem::DrawParams p1{};
    CHECK_MSG(entryOf("materials/m1.dxmat") && entryOf("materials/m1.dxmat")->valid, "m1.dxmat が読めない");
    CHECK_MSG(entryOf("materials/m1.dxmat")->data.IsGraph(), "m1.dxmat がグラフ材質として読まれていない");
    CHECK_MSG(!resolve("materials/m1.dxmat", p1), "初回は DXC + PSO 待ちなので false（代理材質で描く）のはず");
    {
        GraphMaterialSystem::InstanceStatus st;
        CHECK(sys->GetInstanceStatus("materials/m1.dxmat", st));
        CHECK_MSG(st.compiling && !st.hasActive, "ビルド中の状態が読めない（compiling=%d hasActive=%d）", st.compiling, st.hasActive);
        CHECK(st.slotCount >= 4);   // Tint / Albedo（暗黙でない TextureParameter）/ TexCoord の tiling・offset / R
    }
    CHECK_MSG(sys->WaitIdle(60000, cmd), "ビルドが 60 秒で終わらない");
    ex.Submit();
    rm.FinishUploads();
    CHECK_MSG(resolve("materials/m1.dxmat", p1), "ビルド完了後に Resolve が false");
    CHECK(p1.pso != nullptr);
    {
        GraphMaterialSystem::InstanceStatus st;
        CHECK(sys->GetInstanceStatus("materials/m1.dxmat", st));
        CHECK_MSG(st.hasActive && !st.compiling && !st.failed, "完了後の状態が不正 (active=%d compiling=%d failed=%d)\n%s", st.hasActive, st.compiling, st.failed, st.errorLog.c_str());
        CHECK(st.generation == 1);
        CHECK(st.dxcMs > 0.0 && st.psoMs >= 0.0);
        std::printf("  初回: codegen %.2f ms / DXC %.0f ms / PSO %.0f ms / cacheHit=%d\n", st.codegenMs, st.dxcMs, st.psoMs, st.cacheHit);
    }
    const auto c1 = sys->GetCounters();
    CHECK_MSG(c1.dxcCompiles == 2, "初回の DXC は VS + PS の 2 回のはず（%u 回）", c1.dxcCompiles);
    CHECK(c1.cacheStores == 2 && c1.cacheHits == 0);

    // ---- 2. パラメータプール（レコード）-----------------------------------------------------------
    std::printf("stage: pool\n");
    mg::MaterialGraph g0;
    std::string gerr;
    CHECK(mg::LoadDxmgFile((assets / "materials" / "g1.dxmg").string(), g0, &gerr));
    const mg::CompileResult cr0 = mg::CompileGraph(g0);
    CHECK(cr0.ok);
    const mg::SlotInfo* sR = cr0.FindSlotByName("R");
    const mg::SlotInfo* sTint = cr0.FindSlotByName("Tint");
    const mg::SlotInfo* sAlb = cr0.FindSlotByName("Albedo");
    CHECK(sR && sTint && sAlb);
    std::vector<float> rec1;
    CHECK(sys->DebugReadRecord("materials/m1.dxmat", rec1));
    CHECK(rec1.size() == static_cast<size_t>(cr0.slotCount) * 4);
    if (sR && sTint && sAlb && rec1.size() == static_cast<size_t>(cr0.slotCount) * 4)
    {
        CHECK(std::fabs(rec1[sR->slot * 4] - 0.8f) < 1e-6f);                                  // .dxmat の params が優先
        CHECK(std::fabs(rec1[sTint->slot * 4] - 1.0f) < 1e-6f);                               // 上書き無し = グラフの既定値（1, 0.5, 0.25, 1）
        CHECK(std::fabs(rec1[sTint->slot * 4 + 1] - 0.5f) < 1e-6f);
        uint32_t srvBits = 0;
        std::memcpy(&srvBits, &rec1[sAlb->slot * 4], 4);
        Texture* ta = rm.GetOrLoadTexture(fs::path(assets / "textures" / "a.bmp").generic_wstring(), cmd, true, TextureUsage::BaseColor);
        CHECK_MSG(ta && srvBits == ta->GetSrvIndex(), "テクスチャのスロットに SRV 添字が入っていない（%u vs %u）", srvBits, ta ? ta->GetSrvIndex() : 0u);
        CHECK(std::fabs(rec1[sAlb->slot * 4 + 1] - 1.0f / 8.0f) < 1e-6f);                   // 1 / 幅
        CHECK(std::fabs(rec1[sAlb->slot * 4 + 2] - 1.0f / 4.0f) < 1e-6f);                   // 1 / 高さ
    }
    // 2 枚目（同じグラフ）: レコードのオフセットは別・PSO は共有
    GraphMaterialSystem::DrawParams p2{};
    resolve("materials/m2.dxmat", p2);
    CHECK_MSG(sys->WaitIdle(60000, cmd), "m2 のビルドが終わらない");
    ex.Submit();
    rm.FinishUploads();
    CHECK(resolve("materials/m2.dxmat", p2));
    CHECK_MSG(p2.recordBase != p1.recordBase, "2 枚の材質が同じレコードを共有している");
    CHECK_MSG(p2.recordBase == p1.recordBase + static_cast<u32>(cr0.slotCount) || p2.recordBase + static_cast<u32>(cr0.slotCount) == p1.recordBase,
              "レコードが隙間なく詰まっていない（m1=%u m2=%u len=%d）", p1.recordBase, p2.recordBase, cr0.slotCount);
    CHECK_MSG(p2.pso == p1.pso, "同じ HLSL の材質が PSO を共有していない");
    CHECK(sys->GetCounters().variants == 1);
    CHECK(sys->GetCounters().dxcCompiles == 2);   // 2 枚目は DXC を呼ばない（同じ変種）
    CHECK(p1.poolSrvIndex == p2.poolSrvIndex);
    {
        std::vector<float> rec2;
        CHECK(sys->DebugReadRecord("materials/m2.dxmat", rec2));
        if (sR && sTint && rec2.size() == static_cast<size_t>(cr0.slotCount) * 4)
        {
            CHECK(std::fabs(rec2[sR->slot * 4] - 0.25f) < 1e-6f);
            CHECK(std::fabs(rec2[sTint->slot * 4] - 0.1f) < 1e-6f && std::fabs(rec2[sTint->slot * 4 + 2] - 0.3f) < 1e-6f);
            CHECK(std::fabs(rec2[sTint->slot * 4 + 3] - 1.0f) < 1e-6f);                     // 3 成分の Vector は w = 1
        }
    }

    // ---- 3. 値の編集は再コンパイルしない -------------------------------------------------------
    std::printf("stage: value edit\n");
    {
        const auto before = sys->GetCounters();
        GraphMaterialSystem::InstanceStatus s0;
        sys->GetInstanceStatus("materials/m1.dxmat", s0);
        MaterialAssetData::GraphParam gp;
        gp.kind = MaterialAssetData::GraphParam::Kind::Scalar;
        gp.v[0] = 0.3f;
        CHECK(mam.SetGraphParam("materials/m1.dxmat", "R", gp));
        GraphMaterialSystem::DrawParams p{};
        CHECK(resolve("materials/m1.dxmat", p));
        CHECK_MSG(p.pso == p1.pso && p.recordBase == p1.recordBase, "値の編集で PSO / レコードの場所が変わった");
        GraphMaterialSystem::InstanceStatus s1;
        sys->GetInstanceStatus("materials/m1.dxmat", s1);
        const auto after = sys->GetCounters();
        CHECK_MSG(after.dxcCompiles == before.dxcCompiles, "値の編集で DXC が呼ばれた（%u → %u）", before.dxcCompiles, after.dxcCompiles);
        CHECK(after.psoCreates == before.psoCreates);
        CHECK(s1.generation == s0.generation);
        CHECK_MSG(s1.valueUpdates == s0.valueUpdates + 1, "値の更新が数えられていない");
        std::vector<float> rec;
        CHECK(sys->DebugReadRecord("materials/m1.dxmat", rec));
        if (sR && rec.size() > static_cast<size_t>(sR->slot) * 4) CHECK(std::fabs(rec[sR->slot * 4] - 0.3f) < 1e-6f);
        // グラフ側の定数（Slot のプロパティ）の編集も再コンパイルしない: Tint の default を書き換えて通知
        mg::MaterialGraph g;
        CHECK(mg::LoadDxmgFile((assets / "materials" / "g1.dxmg").string(), g, &gerr));
        g.SetNodeProp("tint", "default", mg::Json::array({0.9, 0.8, 0.7, 1.0}));
        CHECK(mg::SaveDxmgFileIfChanged((assets / "materials" / "g1.dxmg").string(), g));
        sys->NotifyGraphFileChanged("materials/g1.dxmg");
        pump();
        CHECK(resolve("materials/m1.dxmat", p));
        const auto after2 = sys->GetCounters();
        CHECK_MSG(after2.dxcCompiles == before.dxcCompiles && after2.psoCreates == before.psoCreates, "定数の編集で再コンパイルされた");
        CHECK_MSG(p.pso == p1.pso, "定数の編集で PSO が変わった");
        CHECK(sys->DebugReadRecord("materials/m1.dxmat", rec));
        if (sTint && rec.size() > static_cast<size_t>(sTint->slot) * 4 + 1) CHECK(std::fabs(rec[sTint->slot * 4] - 0.9f) < 1e-6f);
    }

    // ---- 4. 構造の編集: 旧版維持 → 新版へ切り替え ---------------------------------------------------
    std::printf("stage: structure edit (old version kept)\n");
    {
        sys->DebugSetWorkerDelayMs(700);
        WriteText(assets / "materials" / "g1.dxmg", BuildGraph(1));
        sys->NotifyGraphFileChanged("materials/g1.dxmg");
        pump();
        GraphMaterialSystem::DrawParams p{};
        int keptOld = 0, sawNew = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
        {
            const bool ok = resolve("materials/m1.dxmat", p);
            CHECK_MSG(ok, "コンパイル中に Resolve が false になった（旧版が消えた）");
            if (!ok) break;
            if (p.pso == p1.pso) ++keptOld; else { ++sawNew; break; }
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK_MSG(keptOld > 3, "新版が完成するまで旧 PSO で描き続けられていない（旧版のまま %d 回）", keptOld);
        CHECK_MSG(sawNew == 1, "新版へ切り替わらなかった");
        GraphMaterialSystem::InstanceStatus st;
        sys->GetInstanceStatus("materials/m1.dxmat", st);
        CHECK(st.generation == 2 && !st.compiling && !st.failed);
        std::printf("  旧版のまま %d 回描画 → 新版へ / DXC %.0f ms\n", keptOld, st.dxcMs);
        CHECK(sys->GetCounters().dxcCompiles == 3);   // 新しい PS だけ（VS は共通スタブでキャッシュ済み。ディスクキャッシュのヒット）
        // 旧版の PSO を使っていた m2 は別のグラフ版のまま（m2 も同じ .dxmg を指すので通知で作り直される）
        pump();
        GraphMaterialSystem::DrawParams q2{};
        for (int i = 0; i < 400 && !(resolve("materials/m2.dxmat", q2) && q2.pso == p.pso); ++i) { pump(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK_MSG(q2.pso == p.pso, "同じグラフを使う m2 も新版へ切り替わって PSO を共有するはず");
        p1 = p;
    }

    // ---- 5. コンパイルエラー: 描画を止めない・nodeId つきで保持 ----------------------------------------
    std::printf("stage: broken HLSL\n");
    {
        sys->DebugSetWorkerDelayMs(0);
        const auto p_before = p1;
        WriteText(assets / "materials" / "g1.dxmg", BuildGraph(2));
        sys->NotifyGraphFileChanged("materials/g1.dxmg");
        pump();
        sys->WaitIdle(60000, cmd);
        GraphMaterialSystem::DrawParams p{};
        CHECK_MSG(resolve("materials/m1.dxmat", p), "壊れた HLSL のあとで Resolve が false（描画が止まる）");
        CHECK_MSG(p.pso == p_before.pso, "壊れた HLSL のあとで旧 PSO が維持されていない");
        GraphMaterialSystem::InstanceStatus st;
        CHECK(sys->GetInstanceStatus("materials/m1.dxmat", st));
        CHECK_MSG(st.failed, "失敗が記録されていない");
        CHECK_MSG(!st.errorLog.empty() && st.errorLog.find("no_such_function") != std::string::npos, "エラー全文が保持されていない: %s", st.errorLog.substr(0, 300).c_str());
        bool mapped = false;
        for (const auto& dg : st.diagnostics)
            if (dg.code == "E_DXC" && dg.nodeId == "cu") mapped = true;
        CHECK_MSG(mapped, "DXC のエラーが Custom ノード（cu）へ逆引きされていない");
        CHECK(st.hasActive);
        // 直す → 復帰（HLSL は 1 つ前と同じ = 変種はキャッシュ済みなので DXC は不要）
        const auto cbefore = sys->GetCounters();
        WriteText(assets / "materials" / "g1.dxmg", BuildGraph(1));
        sys->NotifyGraphFileChanged("materials/g1.dxmg");
        pump();
        sys->WaitIdle(60000, cmd);
        CHECK(resolve("materials/m1.dxmat", p));
        sys->GetInstanceStatus("materials/m1.dxmat", st);
        CHECK_MSG(!st.failed && st.errorLog.empty(), "直したあとも失敗状態が残っている");
        CHECK_MSG(sys->GetCounters().dxcCompiles == cbefore.dxcCompiles, "直したあとで DXC が呼ばれた（同じ HLSL の変種は共有されるはず）");
    }

    // ---- 6. プレビュー球（G2c）の足場: Entry を介さず・保存前のグラフ本文で描ける -------------------------------
    std::printf("stage: preview scaffold\n");
    {
        MaterialAssetData pd;
        pd.graphPath = "__preview__/unsaved.dxmg";          // 実在しないパス（本文は SetInstanceGraphText で渡す）
        MaterialAssetData::GraphParam gp;
        gp.kind = MaterialAssetData::GraphParam::Kind::Scalar;
        gp.v[0] = 0.42f;
        pd.graphParams["R"] = gp;
        sys->SetInstanceGraphText("__preview__/mat", BuildGraph(1));
        GraphMaterialSystem::DrawParams pp{};
        sys->ResolveData("__preview__/mat", pd, 1, cmd, 0, false, pp);   // 同じ HLSL の変種が Ready なら初回から true でよい（待たずに済む）
        CHECK(sys->WaitIdle(60000, cmd));
        ex.Submit();
        rm.FinishUploads();
        CHECK_MSG(sys->ResolveData("__preview__/mat", pd, 1, cmd, 0, false, pp), "保存前のグラフ本文からビルドできない");
        CHECK_MSG(pp.pso == p1.pso, "同じ HLSL の変種を共有しているはず（.dxmg 由来の m1 と PSO が同じ）");
        std::vector<float> rec;
        CHECK(sys->DebugReadRecord("__preview__/mat", rec));
        // 値だけ変える（dataSerial を進める）→ 再コンパイルなし
        const auto before = sys->GetCounters();
        pd.graphParams["R"].v[0] = 0.9f;
        CHECK(sys->ResolveData("__preview__/mat", pd, 2, cmd, 0, false, pp));
        CHECK(sys->GetCounters().dxcCompiles == before.dxcCompiles);
    }

    // ---- 7. ディスクキャッシュ（別インスタンス = 別プロセス相当）--------------------------------------------
    std::printf("stage: disk cache\n");
    {
        WriteText(assets / "materials" / "g1.dxmg", BuildGraph(1));
        GraphMaterialSystem::InitDesc d2 = d;
        auto sys2 = std::make_unique<GraphMaterialSystem>();
        CHECK(sys2->Initialize(d2));
        GraphMaterialSystem::DrawParams p{};
        const MaterialAssetManager::Entry* e = entryOf("materials/m1.dxmat");
        sys2->Resolve("materials/m1.dxmat", *e, cmd, 0, false, p);
        CHECK(sys2->WaitIdle(60000, cmd));
        ex.Submit();
        rm.FinishUploads();
        CHECK(sys2->Resolve("materials/m1.dxmat", *e, cmd, 0, false, p));
        const auto c = sys2->GetCounters();
        CHECK_MSG(c.dxcCompiles == 0, "ディスクキャッシュがあるのに DXC を呼んだ（%u 回）", c.dxcCompiles);
        CHECK_MSG(c.cacheHits >= 2, "キャッシュヒットが数えられていない（%u）", c.cacheHits);
        GraphMaterialSystem::InstanceStatus st;
        sys2->GetInstanceStatus("materials/m1.dxmat", st);
        CHECK(st.cacheHit);
        std::printf("  キャッシュ経由: DXC 0 回 / PSO %.0f ms\n", st.psoMs);
        sys2.reset();
    }

    // ---- 8. 配布ビルド（ゲームモード）: BakeForBuild → pak → DXC 無しで PSO ------------------------------------
    std::printf("stage: game mode (pak)\n");
    {
        WriteText(assets / "materials" / "g1.dxmg", BuildGraph(1));
        std::vector<GraphMaterialSystem::BakedBlob> blobs;
        std::vector<std::string> bakeErrors;
        const bool baked = sys->BakeForBuild(blobs, bakeErrors);
        CHECK_MSG(baked, "BakeForBuild が失敗: %s", bakeErrors.empty() ? "" : bakeErrors[0].c_str());
        // m1 / m2 は同じ HLSL（1 個）+ 共通の vs
        CHECK_MSG(blobs.size() == 2, "焼いたシェーダーが %zu 個（PS 1 + VS 1 のはず）", blobs.size());
        bool hasVs = false, hasPs = false;
        for (const auto& b : blobs)
        {
            hasVs = hasVs || b.relPath == "shaders/graph/vs.cso";
            hasPs = hasPs || (b.relPath.rfind("shaders/graph/", 0) == 0 && b.relPath.find("_PS.cso") != std::string::npos);
            CHECK(!b.bytes.empty());
        }
        CHECK(hasVs && hasPs);

        const fs::path pakPath = proj / "game.pak";
        {
            vfs::PakWriter w;
            CHECK(w.Open(pakPath.generic_string()));
            for (const char* rel : {"materials/g1.dxmg", "materials/m1.dxmat", "textures/a.bmp", "textures/b.bmp"})
                CHECK(w.AddFile((assets / rel).generic_string(), rel));
            for (const auto& b : blobs) CHECK(w.AddBlob(b.relPath, b.bytes.data(), b.bytes.size()));
            const std::string manifest = "{}";
            CHECK(w.AddBlob("__manifest__", reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()));
            CHECK(w.Finish(false));
        }
        CHECK(vfs::MountPak(pakPath.generic_string()));
        CHECK(vfs::InGameMode());

        MaterialAssetManager mam2;
        mam2.Initialize(&rm, &dev, &srvHeap);
        GraphMaterialSystem::InitDesc dg = d;
        dg.allowCompile = false;                       // ゲームモード: DXC を使わない
        dg.pollFiles = false;
        dg.cacheDir = (proj / "shadercache_game").generic_string();   // 空のキャッシュ（キャッシュ経由でないことを確かめる）
        auto sysG = std::make_unique<GraphMaterialSystem>();
        CHECK(sysG->Initialize(dg));
        const MaterialAssetManager::Entry* eg = mam2.GetOrLoad("materials/m1.dxmat", cmd);
        CHECK(eg && eg->valid && eg->data.IsGraph());
        GraphMaterialSystem::DrawParams pg{};
        if (eg)
        {
            sysG->Resolve("materials/m1.dxmat", *eg, cmd, 0, false, pg);
            CHECK(sysG->WaitIdle(60000, cmd));
            ex.Submit();
            rm.FinishUploads();
            CHECK_MSG(sysG->Resolve("materials/m1.dxmat", *eg, cmd, 0, false, pg), "pak の DXIL から PSO を作れない");
            CHECK(pg.pso != nullptr);
            const auto cg = sysG->GetCounters();
            CHECK_MSG(cg.dxcCompiles == 0, "ゲームモードで DXC を呼んだ（%u 回）", cg.dxcCompiles);
            // pak に焼いていない HLSL（構造が違うグラフ）は、DXC が無いので失敗して代理材質のまま（描画は止まらない）
            sysG->SetInstanceGraphText("materials/m1.dxmat", BuildGraph(0));
            sysG->Update(0.016f, 0, cmd);
            sysG->WaitIdle(60000, cmd);
            GraphMaterialSystem::InstanceStatus st;
            CHECK(sysG->GetInstanceStatus("materials/m1.dxmat", st));
            CHECK_MSG(st.failed && st.errorLog.find("配布ビルド") != std::string::npos, "焼かれていない HLSL のエラーが不正: %s", st.errorLog.c_str());
            CHECK_MSG(st.hasActive, "焼かれていない HLSL でも焼かれた版で描き続けるはず");
        }
        sysG.reset();
        vfs::Unmount();
        CHECK(!vfs::InGameMode());
    }

    sys.reset();
    fs::remove_all(proj, ec);
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
