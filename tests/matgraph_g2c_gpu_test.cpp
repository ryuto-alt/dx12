// マテリアルグラフ G2c: プレビュー / ノード内サムネイル / 段階コンパイルの実機検証（実 GPU + DXC。無ければ SKIP）。
//
//   1. ノード内サムネイルの値: 見本グラフの各ノードを部分グラフ出力 → GPU で 64x64 評価（NodeRaw = RGBA32F）→ G1 の CPU 評価器と全画素サンプルで一致。
//      NodeLdr（RGBA8・ガンマ表示）も期待値と 1/255 以内。
//   2. プレビュー: 4 形状 x 3 環境が描ける・同じ入力は同じ絵（決定論）・値の変更は再コンパイルなしで絵が変わる。
//   3. PreviewLite（UNO_SHADE_LITE）== 通常経路（影 / クラスタ / GI を実行時に全部無効にしたもの）。プレビューと実シーンで輝度が系統的にずれない根拠。
//   4. 段階コンパイル: 高速版（-Od）が先に出る・最適化版で差し替わる・順不同に届いても下がらない（遅れた古い結果は捨てる）・
//      古い編集が新しい編集を上書きしない・ディスクキャッシュに最適化版があれば高速版を省く。
//   5. 実測ゲート（推定を書かない）: 見本 26 ノード / 大きめ 200 ノードで 編集 → 見た目更新のレイテンシ（毎回 HLSL が変わる = ドライバの PSO キャッシュに当たらない）。
//      値のみの変更・ディスクキャッシュ命中・ワーカー稼働中のメインスレッドの呼び出し時間（UI が止まらない）。
//   出力: 標準出力（実測表）+ 環境変数 G2C_OUT_DIR があればプレビュー画像（BMP）と実測表（CSV）を書く。
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/Window.h"
#include "editor/matgraph/MatGraphEditor.h"
#include "editor/matgraph/NodeThumbs.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/RootSignature.h"
#include "graphics/Texture.h"
#include "renderer/GraphMaterialSystem.h"
#include "renderer/GraphPreviewRenderer.h"
#include "renderer/matgraph/CpuEval.h"
#include "renderer/matgraph/GraphIO.h"
#include "resource/ModelLoader.h"
#include "resource/ResourceManager.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace dx12e;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace mat = dx12e::matgraph;
using SteadyClock = std::chrono::steady_clock;

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

double MsSince(SteadyClock::time_point t0) { return std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count(); }

std::string EnvStr(const char* name)
{
    char* buf = nullptr;
    size_t len = 0;
    std::string r;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) r = buf;
    std::free(buf);
    return r;
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

// テクスチャ（PIXEL_SHADER_RESOURCE 状態）を読み戻す。bpp = 1 画素のバイト数
std::vector<uint8_t> ReadTexture(Exec& ex, ID3D12Device* dev, ID3D12Resource* tex, uint32_t w, uint32_t h, uint32_t bpp)
{
    const uint32_t rowPitch = (w * bpp + 255) & ~255u;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(rowPitch) * h;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return {};
    auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = tex;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b;
        ex.list->ResourceBarrier(1, &br);
    };
    barrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = tex->GetDesc().Format;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ex.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ex.Submit();
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * bpp);
    uint8_t* mapped = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return {};
    for (uint32_t y = 0; y < h; ++y) std::memcpy(&out[static_cast<size_t>(y) * w * bpp], mapped + static_cast<size_t>(y) * rowPitch, static_cast<size_t>(w) * bpp);
    rb->Unmap(0, nullptr);
    return out;
}

float HalfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t f;
    if (e == 0) { if (m == 0) f = s << 31; else { int ee = -1; uint32_t mm = m; do { ++ee; mm <<= 1; } while (!(mm & 0x400)); f = (s << 31) | ((127 - 15 - ee) << 23) | ((mm & 0x3FF) << 13); } }
    else if (e == 31) f = (s << 31) | 0x7F800000 | (m << 13);
    else f = (s << 31) | ((e + 127 - 15) << 23) | (m << 13);
    float r;
    std::memcpy(&r, &f, 4);
    return r;
}

void WriteBmp(const fs::path& path, const uint8_t* rgba, int W, int H)
{
    std::vector<uint8_t> f(54 + static_cast<size_t>(W) * H * 3, 0);
    auto le32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) f[o + i] = uint8_t(v >> (8 * i)); };
    f[0] = 'B'; f[1] = 'M';
    le32(2, uint32_t(f.size())); le32(10, 54); le32(14, 40); le32(18, W); le32(22, static_cast<uint32_t>(-H)); f[26] = 1; f[28] = 24; le32(34, W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const uint8_t* p = rgba + (static_cast<size_t>(y) * W + x) * 4;
            uint8_t* d = &f[54 + (static_cast<size_t>(y) * W + x) * 3];
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
        }
    fs::create_directories(path.parent_path());
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
}

struct Stat { double p50 = 0, p95 = 0, mx = 0, mn = 0; };
Stat Stats(std::vector<double> v)
{
    Stat s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5))]; };
    s.p50 = pct(0.5); s.p95 = pct(0.95); s.mx = v.back(); s.mn = v.front();
    return s;
}

using dx12e::mg::BuildBigGraph;
using dx12e::mg::SaltGraph;

std::shared_ptr<const mat::CompileResult> Compile(const mat::MaterialGraph& g)
{
    return std::make_shared<const mat::CompileResult>(mat::CompileGraph(g));
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!EnvStr("DX12_D3D_DEBUG").empty()) Logger::Init();
    Window window;
    GraphicsDevice dev;
    try { dev.Initialize(window); }
    catch (...) { std::printf("SKIP: D3D12 デバイスが作れない（GPU なし）\n"); return 0; }
    if (!dev.SupportsDynamicResources()) { std::printf("SKIP: SM 6.6 + Resource Binding Tier 3 が無い GPU\n"); return 0; }
    RootSignature rs;
    rs.Initialize(dev);
    if (!rs.IsBindless()) { std::printf("SKIP: メイン RS がバインドレスでない\n"); return 0; }
    if (!LoadLibraryW(L"dxcompiler.dll")) { std::printf("SKIP: dxcompiler.dll が読めない\n"); return 0; }
    _putenv_s("UNO_SHADER_SRC_DIR", DX12E_SHADER_SRC_DIR);

    const std::string outDir = EnvStr("G2C_OUT_DIR");
    const bool quick = !EnvStr("G2C_QUICK").empty();
    const int iters = quick ? 3 : 8;

    Exec ex;
    CHECK(ex.Init(dev.GetDevice()));
    DescriptorHeap srvHeap;
    srvHeap.Initialize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true);
    ResourceManager rm;
    rm.Initialize(&dev, &srvHeap, ex.list.Get());
    ex.Submit();
    rm.FinishUploads();

    const fs::path proj = fs::temp_directory_path() / ("dx12e_g2c_gpu_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(proj, ec);
    PathResolver::SetProjectRoot(proj.generic_string());
    const std::string cacheDir = (proj / "shadercache").generic_string();

    GraphMaterialSystem::InitDesc d;
    d.device = &dev; d.rootSignature = &rs; d.resources = &rm; d.srvHeap = &srvHeap;
    d.allowCompile = true; d.pollFiles = false; d.engineVersion = "g2c-test"; d.cacheDir = cacheDir;
    d.stagedCompile = true;
    auto sys = std::make_unique<GraphMaterialSystem>();
    CHECK(sys->Initialize(d));
    if (!sys->IsAvailable()) return 1;
    std::printf("workers = %d / staged = %d / logical cores = %u\n", sys->WorkerCount(), sys->StagedCompile() ? 1 : 0, std::thread::hardware_concurrency());

    GraphPreviewRenderer pr;
    GraphPreviewRenderer::InitDesc pd;
    pd.device = &dev; pd.rootSignature = &rs; pd.resources = &rm; pd.srvHeap = &srvHeap; pd.graphs = sys.get();
    pd.shaderDirW = fs::path(DX12E_SHADER_BIN_DIR).wstring();
    CHECK(pr.Initialize(pd));
    if (!pr.IsValid()) return 1;

    ID3D12GraphicsCommandList* cmd = ex.list.Get();
    u32 frame = 0;
    auto pump = [&]() { sys->Update(0.016f, frame % 3, cmd); ++frame; };
    auto waitReady = [&](const std::string& key, int timeoutMs, bool wantFinal) {
        const auto t0 = SteadyClock::now();
        for (;;)
        {
            pump();
            GraphMaterialSystem::DrawParams dp{};
            if (sys->ResolveInstance(key, cmd, frame % 3, false, dp))
            {
                GraphMaterialSystem::InstanceStatus st;
                if (!wantFinal) return true;
                if (sys->GetInstanceStatus(key, st) && st.level >= 2) return true;
            }
            if (MsSince(t0) > timeoutMs) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // ==================================================================================
    // 1. ノード内サムネイルの値: GPU（NodeRaw / NodeLdr）== CPU 評価（G1）
    // ==================================================================================
    std::printf("stage: node thumbnails vs CPU eval\n");
    {
        mat::MaterialGraph g;
        dx12e::mg::BuildSampleGraph(g);
        int tested = 0, skipped = 0;
        double worst = 0.0;
        for (const auto& kv : g.Nodes())
        {
            const mat::NodeDef* def = g.DefOf(kv.first);
            if (!dx12e::mg::NodeThumbs::WantsThumb(def)) continue;
            mat::CompileOptions opt;
            opt.previewNode = kv.first;
            auto cr = std::make_shared<const mat::CompileResult>(mat::CompileGraph(g, opt));
            CHECK_MSG(cr->ok, "node %s の部分グラフがコンパイルできない", kv.first.c_str());
            if (!cr->ok) continue;
            const std::string key = "__t/raw/" + kv.first;
            sys->SetInstanceKind(key, GraphMaterialSystem::Kind::NodeRaw);
            sys->SetInstanceCompiled(key, cr);
            CHECK_MSG(waitReady(key, 20000, false), "node %s のサムネイルシェーダーが 20 秒で出来ない", kv.first.c_str());
            CHECK(pr.RenderRawTile(cmd, frame % 3, key));
            ex.Submit();
            rm.FinishUploads();
            const std::vector<uint8_t> raw = ReadTexture(ex, dev.GetDevice(), pr.RawTileResource(), 64, 64, 16);
            CHECK(raw.size() == 64u * 64u * 16u);
            if (raw.size() != 64u * 64u * 16u) continue;

            // CPU 評価: 環境は quad の頂点属性と同じ（uv = 画素中心・worldPos = (2u-1, 1-2v, 0)・法線 -Z・カメラ (0,0,-1)）
            mat::EvalEnv env;
            env.vertexNormalWS[0] = 0; env.vertexNormalWS[1] = 0; env.vertexNormalWS[2] = -1;
            env.cameraPos[0] = 0; env.cameraPos[1] = 0; env.cameraPos[2] = -1;
            env.time = 0;
            const auto* crp = cr.get();
            env.sampleTexture = [crp](int slot, const float[2], float, float out[4]) {
                // 既定のテクスチャ（パスが無い → 白 / 法線は既定の法線）
                const bool normal = slot >= 0 && slot < static_cast<int>(crp->slots.size()) && crp->slots[static_cast<size_t>(slot)].textureUsage == "Normal";
                out[0] = normal ? 128.0f / 255.0f : 1.0f; out[1] = normal ? 128.0f / 255.0f : 1.0f; out[2] = 1.0f; out[3] = 1.0f;
            };
            bool cpuOk = true;
            double nodeWorst = 0.0;
            int samples = 0;
            for (int sy = 0; sy < 64 && cpuOk; sy += 9)
                for (int sx = 0; sx < 64; sx += 7)
                {
                    const float u = (static_cast<float>(sx) + 0.5f) / 64.0f, v = (static_cast<float>(sy) + 0.5f) / 64.0f;
                    env.uv[0] = u; env.uv[1] = v;
                    env.worldPos[0] = 2.0f * u - 1.0f; env.worldPos[1] = 1.0f - 2.0f * v; env.worldPos[2] = 0.0f;
                    const mat::CpuEvalResult cpu = mat::EvaluateCpu(*cr, env);
                    if (!cpu.ok) { cpuOk = false; break; }
                    const float* px = reinterpret_cast<const float*>(&raw[(static_cast<size_t>(sy) * 64 + sx) * 16]);
                    const float exp3[3] = {cpu.surface.baseColor[0], cpu.surface.baseColor[1], cpu.surface.baseColor[2]};
                    for (int k = 0; k < 3; ++k)
                    {
                        if (!std::isfinite(exp3[k])) continue;
                        const double err = std::fabs(static_cast<double>(px[k]) - exp3[k]) / std::max(1.0, std::fabs(static_cast<double>(exp3[k])));
                        nodeWorst = std::max(nodeWorst, err);
                    }
                    ++samples;
                }
            if (!cpuOk) { ++skipped; continue; }   // Custom（HLSL 本文）は CPU で評価できない
            ++tested;
            worst = std::max(worst, nodeWorst);
            CHECK_MSG(nodeWorst < 2e-3, "node %s: GPU と CPU 評価の最大相対誤差 %.3e", kv.first.c_str(), nodeWorst);
        }
        std::printf("  サムネイル GPU==CPU: %d ノード検証（CPU 評価できない Custom 等 %d ノードは除外）・最大相対誤差 %.2e\n", tested, skipped, worst);
        CHECK(tested >= 8);

        // LDR（RGBA8・ガンマ）: VectorParameter（定数色。srgb 指定 = 格納時にリニア化 → 表示でガンマ 2.2）をアトラスのタイル 5 へ
        {
            struct Ldr { const char* node; float rgb[3]; };
            const Ldr cases[] = {{"tint", {0.85f, 0.78f, 0.7f}}, {"rim", {0.35f, 0.65f, 1.0f}}};
            int tile = 5;
            std::vector<GraphPreviewRenderer::TileRequest> reqs;
            for (const Ldr& c : cases)
            {
                mat::CompileOptions opt;
                opt.previewNode = c.node;
                auto cr = std::make_shared<const mat::CompileResult>(mat::CompileGraph(g, opt));
                const std::string key = std::string("__t/ldr/") + c.node;
                sys->SetInstanceKind(key, GraphMaterialSystem::Kind::NodeLdr);
                sys->SetInstanceCompiled(key, cr);
                CHECK(waitReady(key, 20000, false));
                reqs.push_back({tile++, key});
            }
            std::vector<int> drawn;
            const int n = pr.RenderTiles(cmd, frame % 3, reqs, &drawn);
            CHECK(n == 2 && drawn.size() == 2);
            ex.Submit();
            const auto atlas = ReadTexture(ex, dev.GetDevice(), pr.AtlasResource(), GraphPreviewRenderer::kAtlasSize, GraphPreviewRenderer::kAtlasSize, 4);
            CHECK(!atlas.empty());
            for (size_t i = 0; i < reqs.size() && !atlas.empty(); ++i)
            {
                const int cx = reqs[i].tile % 8, cy = reqs[i].tile / 8;
                const size_t px = (static_cast<size_t>(cy * 64 + 32) * GraphPreviewRenderer::kAtlasSize + static_cast<size_t>(cx * 64 + 32)) * 4;
                for (int k = 0; k < 3; ++k)
                {
                    const double expect = 255.0 * std::pow(static_cast<double>(mat::SrgbToLinear(cases[i].rgb[k])), 1.0 / 2.2);
                    CHECK_MSG(std::fabs(static_cast<double>(atlas[px + static_cast<size_t>(k)]) - expect) <= 1.6, "LDR タイル %s ch%d: GPU %u / 期待 %.1f", cases[i].node, k, atlas[px + static_cast<size_t>(k)], expect);
                }
            }
            // 隣のタイル（0 番）は触っていない = 初期色のまま
            const size_t p0 = (static_cast<size_t>(32) * GraphPreviewRenderer::kAtlasSize + 32) * 4;
            CHECK(!atlas.empty() && atlas[p0] < 40 && atlas[p0 + 1] < 40);
        }
    }

    // ==================================================================================
    // 2. メインプレビュー: 4 形状 x 3 環境・決定論・値の変更で絵が変わる（再コンパイルなし）
    // ==================================================================================
    std::printf("stage: main preview\n");
    mat::MaterialGraph sample;
    dx12e::mg::BuildSampleGraph(sample);
    auto sampleCr = Compile(sample);
    CHECK(sampleCr->ok);
    const std::string pkey = "__preview/sample";
    sys->SetInstanceKind(pkey, GraphMaterialSystem::Kind::PreviewLite);
    sys->SetInstanceCompiled(pkey, sampleCr);
    CHECK(waitReady(pkey, 30000, false));
    ex.Submit();
    rm.FinishUploads();
    {
        CHECK(pr.EnsureEnvironments());
        for (int env = 0; env < 3; ++env)
            for (int shape = 0; shape < 4; ++shape)
            {
                mat::PreviewSettings st;
                st.shape = static_cast<mat::PreviewShape>(shape);
                st.env = static_cast<mat::PreviewEnv>(env);
                const bool drawn = pr.RenderMain(cmd, frame % 3, pkey, st, 512, 0.0f);
                CHECK_MSG(drawn, "shape=%d env=%d が描けない", shape, env);
                ex.Submit();
                const auto disp = ReadTexture(ex, dev.GetDevice(), pr.MainDisplayResource(), 512, 512, 4);
                // 材質が無いキー = 背景だけ。物体の画素 = 背景と違う画素（形状ごとに一定以上の面積がある）
                CHECK(!pr.RenderMain(cmd, frame % 3, "__no_such_instance", st, 512, 0.0f));
                ex.Submit();
                const auto bg = ReadTexture(ex, dev.GetDevice(), pr.MainDisplayResource(), 512, 512, 4);
                size_t diffPx = 0;
                for (size_t i = 0; i + 4 <= disp.size(); i += 4)
                    if (std::abs(int(disp[i]) - int(bg[i])) + std::abs(int(disp[i + 1]) - int(bg[i + 1])) + std::abs(int(disp[i + 2]) - int(bg[i + 2])) > 3) ++diffPx;
                CHECK_MSG(diffPx > 512u * 512u / 20, "物体が描かれていない shape=%d env=%d（差のある画素 %zu）", shape, env, diffPx);
                if (!outDir.empty())
                {
                    char nm[96];
                    std::snprintf(nm, sizeof(nm), "preview_env%d_shape%d.bmp", env, shape);
                    WriteBmp(fs::path(outDir) / nm, disp.data(), 512, 512);
                }
            }
        // 決定論: 同じ入力 → 同じ画素（時間は固定）
        mat::PreviewSettings st;
        pr.RenderMain(cmd, frame % 3, pkey, st, 384, 0.0f);
        ex.Submit();
        const auto a = ReadTexture(ex, dev.GetDevice(), pr.MainDisplayResource(), 512, 512, 4);
        pr.RenderMain(cmd, (frame + 1) % 3, pkey, st, 384, 0.0f);
        ex.Submit();
        const auto b = ReadTexture(ex, dev.GetDevice(), pr.MainDisplayResource(), 512, 512, 4);
        CHECK_MSG(a == b, "同じ入力でプレビューが一致しない（非決定論）");

        // 値の変更（SetInstanceCompiled に同じ構造・違う値）: 再コンパイルなし + 絵が変わる
        const auto c0 = sys->GetCounters();
        mat::MaterialGraph edited;
        dx12e::mg::BuildSampleGraph(edited);
        edited.SetNodeProp("tint", "default", mat::Json::array({0.2, 0.5, 1.0, 1.0}));
        edited.SetNodeProp("rMin", "default", 0.05);
        auto editedCr = Compile(edited);
        CHECK(editedCr->hash == sampleCr->hash);
        sys->SetInstanceCompiled(pkey, editedCr);
        pr.RenderMain(cmd, (frame + 2) % 3, pkey, st, 384, 0.0f);
        ex.Submit();
        const auto c = ReadTexture(ex, dev.GetDevice(), pr.MainDisplayResource(), 512, 512, 4);
        CHECK_MSG(a != c, "値を変えたのに絵が変わらない");
        const auto c1 = sys->GetCounters();
        CHECK_MSG(c1.dxcCompiles == c0.dxcCompiles && c1.psoCreates == c0.psoCreates, "値の変更で再コンパイルした（DXC %u → %u）", c0.dxcCompiles, c1.dxcCompiles);
    }

    // ==================================================================================
    // 3. PreviewLite == 通常経路（影 / クラスタ / GI を実行時に無効にした Forward）
    // ==================================================================================
    std::printf("stage: lite == full\n");
    {
        sys->SetInstanceCompiled(pkey, sampleCr);   // 2 で値を変えた版から元の値へ戻す（LITE と Forward を同じ値で比べる）
        const std::string fkey = "__preview/sample_full";
        sys->SetInstanceKind(fkey, GraphMaterialSystem::Kind::Forward);
        sys->SetInstanceCompiled(fkey, sampleCr);
        CHECK(waitReady(fkey, 60000, true));
        ex.Submit();
        rm.FinishUploads();
        for (int env = 0; env < 4; ++env)
        {
            mat::PreviewSettings st;
            st.shape = mat::PreviewShape::Sphere;
            st.env = static_cast<mat::PreviewEnv>(std::min(env, 2));
            GraphPreviewRenderer::FlatLighting fl;
            if (env == 3) pr.SetFlatLightingOverride(&fl); else pr.SetFlatLightingOverride(nullptr);
            pr.RenderMain(cmd, frame % 3, pkey, st, 512, 0.0f);
            ex.Submit();
            const auto lite = ReadTexture(ex, dev.GetDevice(), pr.MainColorResource(), 512, 512, 8);
            if (!outDir.empty())
            {
                const auto dl = ReadTexture(ex, dev.GetDevice(), pr.MainDisplayResource(), 512, 512, 4);
                char nm[64];
                std::snprintf(nm, sizeof(nm), "lite_env%d.bmp", env);
                WriteBmp(fs::path(outDir) / nm, dl.data(), 512, 512);
            }
            pr.RenderMain(cmd, frame % 3, fkey, st, 512, 0.0f);
            ex.Submit();
            const auto full = ReadTexture(ex, dev.GetDevice(), pr.MainColorResource(), 512, 512, 8);
            if (!outDir.empty())
            {
                const auto df = ReadTexture(ex, dev.GetDevice(), pr.MainDisplayResource(), 512, 512, 4);
                char nm[64];
                std::snprintf(nm, sizeof(nm), "full_env%d.bmp", env);
                WriteBmp(fs::path(outDir) / nm, df.data(), 512, 512);
            }
            CHECK(lite.size() == full.size() && !lite.empty());
            double sumAbs = 0, sumLite = 0, maxRel = 0;
            size_t n = 0, nz = 0;
            for (size_t i = 0; i + 8 <= lite.size(); i += 8)
            {
                const uint16_t* l = reinterpret_cast<const uint16_t*>(&lite[i]);
                const uint16_t* f = reinterpret_cast<const uint16_t*>(&full[i]);
                const float lr = HalfToFloat(l[0]), lg = HalfToFloat(l[1]), lb = HalfToFloat(l[2]);
                const float fr = HalfToFloat(f[0]), fg = HalfToFloat(f[1]), fb = HalfToFloat(f[2]);
                if (lr + lg + lb == 0.0f && fr + fg + fb == 0.0f) continue;
                ++nz;
                const double ds = std::fabs(lr - fr) + std::fabs(lg - fg) + std::fabs(lb - fb);
                sumAbs += ds; sumLite += lr + lg + lb;
                maxRel = std::max(maxRel, ds / std::max(0.05, static_cast<double>(fr + fg + fb)));
                ++n;
            }
            std::printf("  env=%d: 描画画素 %zu・平均絶対差 %.3e（平均輝度 %.4f）・最大相対差 %.3e\n", env, nz, n ? sumAbs / static_cast<double>(n) : 0.0, n ? sumLite / static_cast<double>(n) : 0.0, maxRel);
            CHECK_MSG(nz > 10000, "描画画素が少ない");
            CHECK_MSG(maxRel < 5e-3, "LITE と通常経路がずれている（env=%d 最大相対差 %.3e）", env, maxRel);
        }
        pr.SetFlatLightingOverride(nullptr);
    }

    // ==================================================================================
    // 4. 段階コンパイル・順序保証
    // ==================================================================================
    std::printf("stage: staged compile / ordering\n");
    {
        // 4a. 高速版が先に出て、最適化版で差し替わる（Forward）
        mat::MaterialGraph g;
        dx12e::mg::BuildSampleGraph(g);
        SaltGraph(g, 0.001f);
        auto cr = Compile(g);
        const std::string key = "__stage/a";
        sys->SetInstanceKind(key, GraphMaterialSystem::Kind::Forward);
        const auto cA = sys->GetCounters();
        sys->SetInstanceCompiled(key, cr);
        CHECK(waitReady(key, 60000, false));
        GraphMaterialSystem::InstanceStatus st1;
        CHECK(sys->GetInstanceStatus(key, st1));
        const int firstLevel = st1.level;
        CHECK_MSG(firstLevel == 1 || firstLevel == 2, "level=%d", firstLevel);
        CHECK(waitReady(key, 60000, true));
        GraphMaterialSystem::InstanceStatus st2;
        CHECK(sys->GetInstanceStatus(key, st2));
        CHECK_MSG(st2.level == 2, "最適化版に差し替わらない");
        const auto cB = sys->GetCounters();
        std::printf("  最初に描けた版 = %s / 最適化版で差し替え済み / 高速版 DXC %u 回・最適化版 DXC %u 回\n", firstLevel == 1 ? "高速版 -Od" : "最適化版（キャッシュ）",
                    cB.fastCompiles - cA.fastCompiles, cB.optCompiles - cA.optCompiles);

        // 4b. 順不同: 高速版の結果を 800 ms 遅らせる → 最適化版が先に届く。遅れて届いた高速版は最適化版を下げない
        SaltGraph(g, 0.002f);
        auto cr2 = Compile(g);
        const std::string key2 = "__stage/b";
        sys->SetInstanceKind(key2, GraphMaterialSystem::Kind::Forward);
        sys->DebugSetStageDelayMs(1, 900);
        const auto stale0 = sys->GetCounters().staleIgnored;
        sys->SetInstanceCompiled(key2, cr2);
        CHECK(waitReady(key2, 60000, false));
        GraphMaterialSystem::InstanceStatus s3;
        CHECK(sys->GetInstanceStatus(key2, s3));
        CHECK_MSG(s3.level == 2, "最適化版が先に届いたのに level=%d", s3.level);
        GraphMaterialSystem::DrawParams dpBefore{};
        CHECK(sys->ResolveInstance(key2, cmd, frame % 3, false, dpBefore));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // 遅れた高速版が届く
        for (int i = 0; i < 10; ++i) { pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        GraphMaterialSystem::DrawParams dpAfter{};
        CHECK(sys->ResolveInstance(key2, cmd, frame % 3, false, dpAfter));
        CHECK_MSG(dpAfter.pso == dpBefore.pso, "遅れて届いた高速版が最適化版の PSO を上書きした");
        CHECK(sys->GetInstanceStatus(key2, s3) && s3.level == 2);
        CHECK_MSG(sys->GetCounters().staleIgnored > stale0, "遅れた結果を捨てたカウントが増えていない");
        sys->DebugSetStageDelayMs(1, 0);

        // 4c. 古い編集が新しい編集を上書きしない: 連続した 8 回の編集（ワーカーを遅らせて未着手のジョブを溜める）→ 最終的に最後の編集の版だけ。
        //     置き換わった編集の未着手ジョブは取り消される
        sys->DebugSetWorkerDelayMs(300);
        const std::string key3 = "__stage/c";
        sys->SetInstanceKind(key3, GraphMaterialSystem::Kind::Forward);
        const auto cx0 = sys->GetCounters();
        std::shared_ptr<const mat::CompileResult> last;
        for (int i = 0; i < 8; ++i)
        {
            SaltGraph(g, 0.01f + 0.001f * static_cast<float>(i));
            last = Compile(g);
            sys->SetInstanceCompiled(key3, last);
            GraphMaterialSystem::DrawParams tmp{};
            sys->ResolveInstance(key3, cmd, frame % 3, false, tmp);   // 前の編集を pending から置き換える
            pump();
        }
        sys->DebugSetWorkerDelayMs(0);
        CHECK(waitReady(key3, 60000, true));
        GraphMaterialSystem::InstanceStatus s4;
        CHECK(sys->GetInstanceStatus(key3, s4));
        CHECK_MSG(s4.activeHash == last->hash, "古い編集が新しい編集を上書きした");
        const auto cx1 = sys->GetCounters();
        std::printf("  古い編集の間引き: 8 回の連続編集で取り消した未着手ジョブ %u 個・実際に走った DXC（高速版 %u / 最適化版 %u）\n", cx1.jobsCancelled - cx0.jobsCancelled,
                    cx1.fastCompiles - cx0.fastCompiles, cx1.optCompiles - cx0.optCompiles);
        CHECK_MSG(cx1.jobsCancelled > cx0.jobsCancelled, "未着手の古いジョブが取り消されていない");

        // 4d. ディスクキャッシュに最適化版があれば高速版を省く（別のシステムで同じ HLSL）
        {
            auto sys2 = std::make_unique<GraphMaterialSystem>();
            GraphMaterialSystem::InitDesc d2 = d;
            CHECK(sys2->Initialize(d2));
            sys2->SetInstanceKind("__stage/d", GraphMaterialSystem::Kind::Forward);
            sys2->SetInstanceCompiled("__stage/d", last);   // last は上で最適化版までキャッシュへ保存済み
            const auto t0 = SteadyClock::now();
            GraphMaterialSystem::DrawParams p{};
            bool ok = false;
            while (MsSince(t0) < 30000)
            {
                sys2->Update(0.016f, 0, cmd);
                if (sys2->ResolveInstance("__stage/d", cmd, 0, false, p)) { ok = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(ok);
            GraphMaterialSystem::InstanceStatus s5;
            CHECK(sys2->GetInstanceStatus("__stage/d", s5));
            CHECK_MSG(s5.level == 2, "キャッシュに最適化版があるのに level=%d", s5.level);
            const auto c2 = sys2->GetCounters();
            CHECK_MSG(c2.dxcCompiles == 0, "キャッシュ命中なのに DXC を呼んだ（%u 回）", c2.dxcCompiles);
            std::printf("  キャッシュ命中: 描けるまで %.1f ms・DXC %u 回\n", MsSince(t0), c2.dxcCompiles);
            sys2->Shutdown();
        }
    }

    // ==================================================================================
    // 5. 実測ゲート
    // ==================================================================================
    std::printf("stage: latency benchmark (%d iterations)\n", iters);
    struct Row { std::string name; Stat firstUsable, final; };
    std::vector<Row> rows;
    auto bench = [&](const char* label, GraphMaterialSystem::Kind kind, bool big, bool staged) {
        sys->SetStagedCompile(staged);
        std::vector<double> usable, fin;
        for (int i = 0; i < iters; ++i)
        {
            mat::MaterialGraph g;
            const float salt = 0.1f + 0.003f * static_cast<float>(i) + (big ? 0.5f : 0.0f) + (kind == GraphMaterialSystem::Kind::PreviewLite ? 0.25f : 0.0f) + (staged ? 0.125f : 0.0f);
            if (big) BuildBigGraph(g, 200, salt);
            else dx12e::mg::BuildSampleGraph(g);
            SaltGraph(g, salt);
            auto cr = Compile(g);
            const std::string key = std::string("__bench/") + label + std::to_string(i);
            sys->SetInstanceKind(key, kind);
            const auto t0 = SteadyClock::now();
            sys->SetInstanceCompiled(key, cr);
            double tu = -1, tf = -1;
            for (;;)
            {
                pump();
                GraphMaterialSystem::DrawParams dp{};
                if (sys->ResolveInstance(key, cmd, frame % 3, false, dp))
                {
                    if (tu < 0) tu = MsSince(t0);
                    GraphMaterialSystem::InstanceStatus st;
                    if (sys->GetInstanceStatus(key, st) && (st.level >= 2 || !staged || kind != GraphMaterialSystem::Kind::Forward)) { tf = MsSince(t0); break; }
                }
                if (MsSince(t0) > 60000) break;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            CHECK_MSG(tu >= 0, "bench %s #%d が 60 秒で終わらない", label, i);
            if (tu >= 0) usable.push_back(tu);
            if (tf >= 0) fin.push_back(tf);
            sys->RemoveInstance(key);
            ex.Submit();
            rm.FinishUploads();
            for (int k = 0; k < 6; ++k) pump();
        }
        Row r;
        r.name = label;
        r.firstUsable = Stats(usable);
        r.final = Stats(fin);
        rows.push_back(r);
        std::printf("  %-34s 描ける版まで p50 %6.1f ms / p95 %6.1f ms（最小 %6.1f / 最大 %6.1f） | 最終版まで p50 %6.1f ms / p95 %6.1f ms\n", label, r.firstUsable.p50, r.firstUsable.p95,
                    r.firstUsable.mn, r.firstUsable.mx, r.final.p50, r.final.p95);
    };
    // CPU の込み具合（他のプロセスの影響を記録）
    {
        FILETIME i0, k0, u0, i1, k1, u1;
        GetSystemTimes(&i0, &k0, &u0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        GetSystemTimes(&i1, &k1, &u1);
        auto q = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const double idle = static_cast<double>(q(i1) - q(i0)), total = static_cast<double>((q(k1) - q(k0)) + (q(u1) - q(u0)));
        std::printf("  （計測前の CPU 使用率 %.0f%%）\n", total > 0 ? 100.0 * (1.0 - idle / total) : 0.0);
    }
    bench("preview lite 26nodes", GraphMaterialSystem::Kind::PreviewLite, false, false);
    bench("preview lite 200nodes", GraphMaterialSystem::Kind::PreviewLite, true, false);
    bench("scene forward staged 26nodes", GraphMaterialSystem::Kind::Forward, false, true);
    bench("scene forward staged 200nodes", GraphMaterialSystem::Kind::Forward, true, true);
    bench("scene forward opt-only 26nodes", GraphMaterialSystem::Kind::Forward, false, false);
    bench("scene forward opt-only 200nodes", GraphMaterialSystem::Kind::Forward, true, false);
    sys->SetStagedCompile(true);

    // 値のみの変更: 1 フレーム以内（Resolve 1 回の呼び出し時間）・再コンパイル 0
    {
        std::printf("stage: value-only edit latency\n");
        std::vector<double> ms;
        mat::MaterialGraph g;
        dx12e::mg::BuildSampleGraph(g);
        auto cr0 = Compile(g);
        const std::string key = "__value/a";
        sys->SetInstanceKind(key, GraphMaterialSystem::Kind::PreviewLite);
        sys->SetInstanceCompiled(key, cr0);
        CHECK(waitReady(key, 60000, false));
        const auto c0 = sys->GetCounters();
        for (int i = 0; i < 200; ++i)
        {
            g.SetNodeProp("tint", "default", mat::Json::array({0.1 + 0.004 * i, 0.5, 0.5, 1.0}));
            auto cr = Compile(g);
            const auto t0 = SteadyClock::now();
            sys->SetInstanceCompiled(key, cr);
            GraphMaterialSystem::DrawParams dp{};
            sys->ResolveInstance(key, cmd, frame % 3, false, dp);
            ms.push_back(MsSince(t0));
            std::vector<float> rec;
            sys->DebugReadRecord(key, rec);
            const mat::SlotInfo* sl = cr->FindSlotByName("Tint");
            CHECK(sl && rec.size() >= static_cast<size_t>(sl->slot) * 4 + 4 && std::fabs(rec[static_cast<size_t>(sl->slot) * 4] - mat::SrgbToLinear(0.1f + 0.004f * static_cast<float>(i))) < 1e-5f);
            ++frame;
        }
        const auto c1 = sys->GetCounters();
        const Stat s = Stats(ms);
        std::printf("  値のみの変更 200 回: SetInstanceCompiled + Resolve p50 %.3f ms / p95 %.3f ms / 最大 %.3f ms・DXC %u → %u・PSO %u → %u\n", s.p50, s.p95, s.mx, c0.dxcCompiles, c1.dxcCompiles, c0.psoCreates, c1.psoCreates);
        CHECK(c1.dxcCompiles == c0.dxcCompiles && c1.psoCreates == c0.psoCreates);
        CHECK_MSG(s.mx < 16.0, "値のみの変更が 1 フレーム（16 ms）を超えた: %.2f ms", s.mx);
        rows.push_back({"value-only edit (ms)", s, s});
    }

    // ワーカー稼働中のメインスレッドの呼び出し時間（UI が止まらない）: 連続した構造編集をワーカーへ流しながら 60 fps の擬似フレームを回す
    {
        std::printf("stage: main-thread stalls under worker load\n");
        std::vector<double> frameMs;
        mat::MaterialGraph g;
        BuildBigGraph(g, 200, 0.9f);
        const std::string key = "__load/a";
        sys->SetInstanceKind(key, GraphMaterialSystem::Kind::PreviewLite);
        const std::string fkey = "__load/b";
        sys->SetInstanceKind(fkey, GraphMaterialSystem::Kind::Forward);
        const auto tEnd = SteadyClock::now() + std::chrono::milliseconds(quick ? 2500 : 8000);
        int edit = 0;
        auto nextEdit = SteadyClock::now();
        while (SteadyClock::now() < tEnd)
        {
            const auto f0 = SteadyClock::now();
            if (f0 >= nextEdit)
            {
                SaltGraph(g, 2.0f + 0.001f * static_cast<float>(edit++));
                auto cr = Compile(g);
                sys->SetInstanceCompiled(key, cr);
                sys->SetInstanceCompiled(fkey, cr);
                nextEdit = f0 + std::chrono::milliseconds(300);
            }
            pump();
            GraphMaterialSystem::DrawParams dp{};
            sys->ResolveInstance(key, cmd, frame % 3, false, dp);
            sys->ResolveInstance(fkey, cmd, frame % 3, false, dp);
            frameMs.push_back(MsSince(f0));
            // 16.6 ms のフレーム（残りは寝る）
            const double rest = 16.6 - MsSince(f0);
            if (rest > 0) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(rest * 1000.0)));
        }
        const Stat s = Stats(frameMs);
        std::printf("  構造編集 %d 回（300 ms 間隔）を流しながらの 1 フレームのメインスレッド処理 p50 %.3f ms / p95 %.3f ms / 最大 %.3f ms（%zu フレーム）\n", edit, s.p50, s.p95, s.mx, frameMs.size());
        CHECK_MSG(s.p95 < 4.0, "メインスレッドの p95 が大きい %.3f ms", s.p95);
        CHECK_MSG(s.mx < 16.6, "メインスレッドの 1 フレームが 16.6 ms を超えた: %.3f ms", s.mx);
        rows.push_back({"main-thread frame under load (ms)", s, s});
    }

    if (!outDir.empty())
    {
        std::ofstream csv(fs::path(outDir) / "latency.csv");
        csv << "name,usable_p50,usable_p95,usable_min,usable_max,final_p50,final_p95\n";
        for (const Row& r : rows)
            csv << r.name << "," << r.firstUsable.p50 << "," << r.firstUsable.p95 << "," << r.firstUsable.mn << "," << r.firstUsable.mx << "," << r.final.p50 << "," << r.final.p95 << "\n";
    }

    sys->WaitIdle(10000, cmd);
    ex.Submit();
    pr.Shutdown();
    sys->Shutdown();
    if (g_failures == 0) std::printf("PASS: %d checks, 0 failures\n", g_checks);
    else std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
