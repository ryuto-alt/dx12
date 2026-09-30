#include "renderer/GraphMaterialSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "core/Assert.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/PipelineState.h"
#include "graphics/RootSignature.h"
#include "graphics/Texture.h"
#include "renderer/Mesh.h"
#include "renderer/matgraph/GraphIO.h"
#include "resource/MaterialGraphize.h"
#include "resource/ResourceManager.h"
#include "resource/ShaderRuntimeCompiler.h"

namespace fs = std::filesystem;

namespace dx12e
{
namespace
{

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string Utf8Of(const fs::path& p)
{
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

// エンジンの HLSL ソースのルート（forward/ForwardGraph.hlsl があるフォルダ）。無ければ空。
//   配布 = exe 隣の shaders-src/、開発 = リポジトリの shaders/。エージェント用インスタンス（exe のコピー）は
//   環境変数 UNO_SHADER_SRC_DIR か、tools/engine_instance.ps1 が作る shaders-src/ を使う。
std::string EngineShaderSrcDir()
{
    std::error_code ec;
    auto ok = [&](const fs::path& p) { return !p.empty() && fs::is_regular_file(p / "forward" / "ForwardGraph.hlsl", ec); };
    fs::path p(PathResolver::ShaderSourceDirW());
    if (ok(p)) return Utf8Of(p);
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "UNO_SHADER_SRC_DIR") == 0 && env)
    {
        const fs::path q(env);
        free(env);
        if (ok(q)) return Utf8Of(q);
    }
    return {};
}

std::wstring Wide(const std::string& s) { return PathResolver::Utf8ToWide(s); }

// 描画の頂点シェーダー確認用の最小ソース（VS は UnoMatEval を使わない。グラフごとに VS を作り直さないための共通スタブ）
const char* kVsStub =
    "// @sm 6_6\n"
    "#include \"forward/ForwardGraph.hlsl\"\n"
    "void UnoMatEval(UnoMatInput mi, UnoMatPool pool, out UnoSurface s) { s = UnoSurfaceDefault(); }\n";

const char* kDxcArgsKey = "-HV 2021";

// 種別ごとの PS エントリ / 追加の -D / キャッシュキーに混ぜる名前
struct KindInfo
{
    const wchar_t* entry;
    const char*    entryName;
    std::vector<std::wstring> defines;
    const char*    tag;
};
KindInfo InfoFor(GraphMaterialSystem::Kind k)
{
    using K = GraphMaterialSystem::Kind;
    switch (k)
    {
    case K::PreviewLite: return {L"PSMain", "PSMain", {L"UNO_SHADE_LITE=1"}, "lite"};
    case K::NodeLdr:     return {L"PSNodePreview", "PSNodePreview", {}, "nodeldr"};
    case K::NodeRaw:     return {L"PSNodePreview", "PSNodePreview", {L"UNO_NODE_PREVIEW_RAW=1"}, "noderaw"};
    default:             return {L"PSMain", "PSMain", {}, "fwd"};
    }
}

} // namespace

std::vector<matgraph::Diagnostic> GraphMaterialSystem::CollectEngineDiagnostics(const matgraph::CompileResult& cr, const MaterialAssetData* data)
{
    std::vector<matgraph::Diagnostic> out;
    auto warn = [&](const char* code, const std::string& msg, const std::string& hint) {
        matgraph::Diagnostic d;
        d.severity = matgraph::Severity::Warning;
        d.code = code;
        d.message = msg;
        d.hint = hint;
        out.push_back(std::move(d));
    };
    if (cr.settings.blendMode != "Opaque")
        warn("W_G2B_BLEND", "blendMode「" + cr.settings.blendMode + "」は G3b で対応します。今は不透明として描きます（Opacity / OpacityMask は無視）",
             "blendMode を Opaque にしてください");
    if (cr.settings.shadingModel != "DefaultLit")
        warn("W_G2B_SHADING", "shadingModel「" + cr.settings.shadingModel + "」は未対応です。DefaultLit として描きます", "shadingModel を DefaultLit にしてください");
    if (data && cr.ok)
    {
        std::set<std::string> known;
        for (const auto& s : cr.slots) if (!s.name.empty()) known.insert(s.name);
        for (const auto& [name, p] : data->graphParams)
        {
            (void)p;
            if (!known.count(name))
                warn("W_PARAM_UNKNOWN", "params に「" + name + "」がありますが、グラフにそのパラメータはありません（無視します）",
                     "パラメータ名を確かめてください（大文字小文字も区別します）");
        }
    }
    return out;
}

GraphMaterialSystem::GraphMaterialSystem() = default;

GraphMaterialSystem::~GraphMaterialSystem()
{
    Shutdown();
}

std::string GraphMaterialSystem::NormalizeKey(const std::string& rel)
{
    std::string key = rel;
    for (char& c : key)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

// ---------------------------------------------------------------------------------------------
// 初期化 / 終了
// ---------------------------------------------------------------------------------------------
bool GraphMaterialSystem::Initialize(const InitDesc& d)
{
    Shutdown();
    m_desc = d;
    m_available = false;
    if (!d.device || !d.rootSignature || !d.resources || !d.srvHeap) return false;
    if (!d.rootSignature->IsBindless())
    {
        Logger::Warn("マテリアルグラフ: バインドレス（SM 6.6 + Resource Binding Tier 3）が使えない GPU / 設定のため、グラフ材質は従来の代理材質（単色）で描きます");
        return false;
    }

    try
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(kPoolCapacity) * 16;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Device5* dev = d.device->GetDevice();
        for (u32 i = 0; i < kFrames; ++i)
        {
            ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&m_poolBuf[i])));
            void* mapped = nullptr;
            ThrowIfFailed(m_poolBuf[i]->Map(0, nullptr, &mapped));
            m_poolMapped[i] = static_cast<u8*>(mapped);
            std::memset(mapped, 0, static_cast<size_t>(kPoolCapacity) * 16);
            m_poolSrv[i] = d.srvHeap->AllocateIndex();
            if (m_poolSrv[i] == DescriptorHeap::kInvalidIndex) throw std::runtime_error("SRV heap exhausted");
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement = 0;
            sd.Buffer.NumElements = kPoolCapacity;
            sd.Buffer.StructureByteStride = 16;
            dev->CreateShaderResourceView(m_poolBuf[i].Get(), &sd, d.srvHeap->GetCpuHandle(m_poolSrv[i]));
        }
    }
    catch (const std::exception& e)
    {
        Logger::Error("マテリアルグラフ: パラメータプールの確保に失敗しました: {}", e.what());
        Shutdown();
        return false;
    }

    m_shadow.assign(static_cast<size_t>(kPoolCapacity) * 4, 0.0f);
    m_alloc.Reset(kPoolCapacity);
    m_poolVersion = 1;
    for (u32 i = 0; i < kFrames; ++i) { m_syncedVersion[i] = 0; m_syncedHigh[i] = 0; }
    m_cache = std::make_shared<matgraph::ShaderDiskCache>(d.cacheDir, d.engineVersion);
    m_counters = Counters{};
    m_counters.cacheDir = m_cache->Dir();
    m_stop = false;
    m_staged = d.stagedCompile;
    m_runningOpt = 0;
    // デバッグ / QA: 環境変数 UNO_MATGRAPH_WORKER_DELAY_MS でワーカーの処理を遅らせる（「コンパイル中は旧版で描き続ける」を目で確かめるため）。
    {
        char* env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env, &len, "UNO_MATGRAPH_WORKER_DELAY_MS") == 0 && env)
        {
            m_workerDelayMs = std::atoi(env);
            free(env);
        }
    }
    // ワーカー本数: 指定 > 環境変数 > 論理コアの 1/8（2〜4 本）。ゲームモード（DXC 無し）は 1 本。AGENT_RULES: スレッド数は論理コアの 1/4 程度まで
    int nWorkers = d.workerCount;
    if (nWorkers <= 0)
    {
        char* wenv = nullptr;
        size_t wlen = 0;
        if (_dupenv_s(&wenv, &wlen, "UNO_MATGRAPH_WORKERS") == 0 && wenv)
        {
            nWorkers = std::atoi(wenv);
            free(wenv);
        }
    }
    if (nWorkers <= 0)
    {
        const int hw = static_cast<int>(std::thread::hardware_concurrency());
        nWorkers = d.allowCompile ? std::min(4, std::max(2, hw / 8)) : 1;
    }
    nWorkers = std::min(8, std::max(1, nWorkers));
    m_maxOptJobs = std::max(1, nWorkers - 1);
    m_counters.workers = static_cast<u32>(nWorkers);
    for (int i = 0; i < nWorkers; ++i)
        m_workers.emplace_back([this, i] { WorkerMain(i); });
    m_available = true;
    Logger::Info("マテリアルグラフ: 初期化しました（プール {} float4 x {} フレーム / キャッシュ {} / ワーカー {} 本{}）", kPoolCapacity, kFrames,
                 m_cache->Dir(), nWorkers, m_staged ? " / 段階コンパイル" : "");
    return true;
}

void GraphMaterialSystem::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();
    m_jobs.Clear();
    m_results.clear();
    m_inFlight = 0;
    m_instances.clear();
    m_variants.clear();
    m_peek.clear();
    m_graveRecords.clear();
    m_graveVariants.clear();
    m_gravePsos.clear();
    for (u32 i = 0; i < kFrames; ++i)
    {
        if (m_poolBuf[i]) m_poolBuf[i]->Unmap(0, nullptr);
        m_poolBuf[i].Reset();
        m_poolMapped[i] = nullptr;
        if (m_desc.srvHeap && m_poolSrv[i] != 0xFFFFFFFFu) m_desc.srvHeap->Free(m_poolSrv[i]);
        m_poolSrv[i] = 0xFFFFFFFFu;
    }
    m_available = false;
}

// ---------------------------------------------------------------------------------------------
// プール
// ---------------------------------------------------------------------------------------------
bool GraphMaterialSystem::AllocRecord(u32 count, PoolRecord& out)
{
    if (count == 0) count = 1;   // スロットの無いグラフでも 1 個確保する（b2 の recordBase を常に有効にする）
    u32 base = 0;
    if (!m_alloc.Alloc(count, base)) return false;
    out.base = base;
    out.count = count;
    out.valid = true;
    return true;
}

void GraphMaterialSystem::FreeRecord(PoolRecord& r)
{
    if (!r.valid) return;
    // GPU がまだ読んでいるフレームがあり得るので、数フレーム置いてから返す（墓場）
    m_graveRecords.push_back({r, m_frame});
    r = PoolRecord{};
}

void GraphMaterialSystem::SyncPool(u32 frameIndex)
{
    if (!m_available) return;
    const u32 f = frameIndex % kFrames;
    if (m_syncedVersion[f] == m_poolVersion) return;
    const u32 high = m_alloc.HighWater();
    if (high > 0) std::memcpy(m_poolMapped[f], m_shadow.data(), static_cast<size_t>(high) * 16);
    m_syncedVersion[f] = m_poolVersion;
    m_syncedHigh[f] = high;
}

// ---------------------------------------------------------------------------------------------
// ワーカー（DXC + PSO）
// ---------------------------------------------------------------------------------------------
void GraphMaterialSystem::EnqueueJob(Job j)
{
    const int prio = JobPriority(j);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.Push(std::move(j), prio);
    }
    ++m_inFlight;
    ++m_counters.jobsQueued;
    m_cv.notify_all();
}

void GraphMaterialSystem::WorkerMain(int /*index*/)
{
    // ユーザー操作（UI・描画）を阻害しない優先度。DXC + PSO は CPU を長く握るので、メインスレッドより下げる
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ShaderRuntimeCompiler compiler;
    const bool dxcOk = m_desc.allowCompile && compiler.Initialize();
    const std::string dxcVersion = dxcOk ? compiler.GetVersionString() : "none";
    if (!dxcOk && m_desc.allowCompile)
        Logger::Warn("マテリアルグラフ: DXC を初期化できません。ディスクキャッシュにあるシェーダーだけ使えます");

    for (;;)
    {
        Job job;
        bool isOpt = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;)
            {
                if (m_stop) return;
                // 最適化版（背景）は workers-1 本まで。残り 1 本は高速版 / プレビュー / サムネイルのために空けておく
                const int maxOpt = m_maxOptJobs;
                const int running = m_runningOpt;
                if (m_jobs.Pop(job, [&](const Job& j) { return !(j.kind == Kind::Forward && j.stage == 2) || running < maxOpt; })) break;
                m_cv.wait(lock);
            }
            isOpt = job.kind == Kind::Forward && job.stage == 2;
            if (isOpt) ++m_runningOpt;
        }
        const int delay = m_workerDelayMs.load();
        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        JobResult res;
        res.hash = job.hash;
        res.kind = job.kind;
        res.level = job.stage;
        if (job.phase) job.phase->store(1);   // DXC（キャッシュの参照を含む）
        if (dxcOk || !job.force)
            ProcessJob(job, compiler, dxcVersion, res);
        else
            res.errorLog = "DXC が使えないため force 再コンパイルできません";
        if (job.phase) job.phase->store(3);
        // 段階の結果を意図的に遅らせる（順不同に届く状況のテスト）
        const int sdelay = job.stage == 1 ? m_stage1DelayMs.load() : m_stage2DelayMs.load();
        if (sdelay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sdelay));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_results.push_back(std::move(res));
            if (isOpt) --m_runningOpt;
        }
        --m_inFlight;
        m_cv.notify_all();   // 最適化版の枠が空いた
    }
}

// 生成 HLSL → DXIL（VS / PS）。ディスクキャッシュを先に引き、無ければ DXC。DXC が使えない（allowCompile=false）ときの入口は ProcessJob 側。
//   job.stage == 1（高速版）: 先に最適化版のキャッシュを引く（あればそれを返して out.level = 2 にする）→ 無ければ -Od でコンパイル。
//   job.stage == 2（最適化版）: 最適化版のキャッシュ → 無ければ通常のコンパイル。
bool GraphMaterialSystem::CompileBlobs(const Job& job, ShaderRuntimeCompiler& compiler, const std::string& dxcVersion, JobResult& out,
                                       std::vector<u8>& vs, std::vector<u8>& ps)
{
    const std::string srcDir = EngineShaderSrcDir();
    if (srcDir.empty())
    {
        out.errorLog = "エンジンの HLSL ソース（shaders/forward/ForwardGraph.hlsl）が見つかりません。"
                       "配布版は exe 隣の shaders-src/、開発は環境変数 UNO_SHADER_SRC_DIR を確認してください";
        return false;
    }
    std::vector<std::string> dirs = {PathResolver::ProjectShaderDir(), srcDir, srcDir + (srcDir.back() == '/' ? "" : "/") + "forward"};
    const matgraph::IncludeScan inc = matgraph::ScanIncludes(job.hlsl, dirs);
    if (!inc.missing.empty())
    {
        out.errorLog = "include が見つかりません: " + inc.missing.front();
        return false;
    }

    const KindInfo ki = InfoFor(job.kind);
    auto psKeyFor = [&](bool od) {
        matgraph::ShaderKeyInput psIn;
        psIn.hlsl = job.hlsl;
        psIn.includeHash = inc.hash;
        psIn.dxcVersion = dxcVersion;
        psIn.engineVersion = m_desc.engineVersion;
        psIn.kind = "ps";
        // Forward の最適化版は G2b と同じ文字列（既存のディスクキャッシュのキーを変えない）。他は種別 / -Od を足す
        psIn.extra = std::string("ps_6_6|") + ki.entryName + "|" + kDxcArgsKey;
        if (job.kind != Kind::Forward) psIn.extra += std::string("|") + ki.tag;
        if (od) psIn.extra += "|Od";
        return matgraph::MakeShaderKey(psIn);
    };
    matgraph::ShaderKeyInput vsIn;
    vsIn.hlsl = kVsStub;
    vsIn.includeHash = inc.hash;
    vsIn.dxcVersion = dxcVersion;
    vsIn.engineVersion = m_desc.engineVersion;
    vsIn.kind = "vs";
    vsIn.extra = std::string("vs_6_6|VSMain|") + kDxcArgsKey;
    const u64 vsKey = matgraph::MakeShaderKey(vsIn);

    auto compile = [&](const std::string& text, const wchar_t* entry, const wchar_t* profile, const std::string& name, bool od,
                       const std::vector<std::wstring>& defines, std::vector<u8>& bytes) -> bool {
        ShaderRuntimeCompiler::CompileRequest req;
        req.hlslPath = Wide(name);
        req.sourceText = text;
        req.entry = entry;
        req.profile = profile;
        req.defines = defines;
        for (const std::string& dd : dirs) req.includeDirs.push_back(Wide(dd));
        req.extraArgs = {L"-HV", L"2021"};
        if (od) req.extraArgs.push_back(L"-Od");
        const auto t0 = Clock::now();
        ShaderRuntimeCompiler::CompileResult r = compiler.Compile(req);
        out.dxcMs += MsSince(t0);
        ++m_dxcCompiles;
        if (!r.success)
        {
            out.errorLog = r.errorLog.empty() ? std::string("DXC のコンパイルに失敗しました") : r.errorLog;
            out.mapped = matgraph::MapDxcLog(out.errorLog);
            return false;
        }
        bytes = std::move(r.dxil);
        return true;
    };

    const bool vsHit = !job.force && m_cache->Load(vsKey, "vs", vs);
    bool psHit = false;
    bool od = false;
    if (job.stage == 1)
    {
        // 高速版: 最適化版がキャッシュにあればそれで済む（-Od の版は作らない）
        if (!job.force && m_cache->Load(psKeyFor(false), "ps", ps)) { psHit = true; out.level = 2; }
        else od = true;
    }
    const u64 psKey = psKeyFor(od);
    if (!psHit) psHit = !job.force && m_cache->Load(psKey, "ps", ps);
    out.cacheHit = vsHit && psHit;

    if (!psHit)
    {
        if (!compile(job.hlsl, ki.entry, L"ps_6_6", "_graph/material.hlsl", od, ki.defines, ps)) return false;
        if (od) ++m_fastCompiles; else ++m_optCompiles;
        m_cache->Store(psKey, "ps", ps);
    }
    if (!vsHit)
    {
        if (!compile(kVsStub, L"VSMain", L"vs_6_6", "_graph/vs_stub.hlsl", false, {}, vs))
        {
            out.errorLog = "頂点シェーダー: " + out.errorLog;
            return false;
        }
        m_cache->Store(vsKey, "vs", vs);
    }
    return true;
}

// ---- PSO。D3D12 デバイスはフリースレッドなのでワーカーで作る -----------------------------------------
//   Forward: less / lequal を並列に作る（壁時計は長い方だけ）。PreviewLite: less だけ。Node*: 深度なし・1 本。
bool GraphMaterialSystem::BuildPsos(Kind kind, const std::vector<u8>& vs, const std::vector<u8>& ps, JobResult& out)
{
    const auto t0 = Clock::now();
    auto make = [&](D3D12_COMPARISON_FUNC depthFunc, Microsoft::WRL::ComPtr<ID3D12PipelineState>& dst, std::string& err) {
        try
        {
            PipelineStateBuilder b;
            b.SetRootSignature(m_desc.rootSignature->Get())
             .SetVertexShader(vs.data(), vs.size())
             .SetPixelShader(ps.data(), ps.size())
             .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
             .SetCullMode(D3D12_CULL_MODE_NONE);   // 従来の Forward と同じ（両面描画）
            if (kind == Kind::NodeLdr || kind == Kind::NodeRaw)
            {
                b.SetRenderTargetFormat(kind == Kind::NodeLdr ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT)
                 .SetDepthStencilFormat(DXGI_FORMAT_UNKNOWN)
                 .SetDepthEnabled(false);
            }
            else
            {
                b.SetRenderTargetFormat(m_desc.colorFormat)
                 .SetDepthStencilFormat(m_desc.depthFormat)
                 .SetDepthEnabled(true)
                 .SetDepthFunc(depthFunc);
            }
            dst = b.Build(*m_desc.device);
            ++m_psoCreates;
        }
        catch (const std::exception& e)
        {
            err = std::string("PSO の作成に失敗しました（ルートシグネチャとシェーダーが合っていません）: ") + e.what();
            dst.Reset();
        }
    };
    std::string errLess, errLequal;
    if (kind == Kind::Forward)
    {
        std::thread th([&] { make(D3D12_COMPARISON_FUNC_LESS_EQUAL, out.lequal, errLequal); });
        make(D3D12_COMPARISON_FUNC_LESS, out.less, errLess);
        th.join();
    }
    else
        make(D3D12_COMPARISON_FUNC_LESS, out.less, errLess);
    if (!errLess.empty() || !errLequal.empty())
    {
        out.errorLog = !errLess.empty() ? errLess : errLequal;
        out.less.Reset();
        out.lequal.Reset();
        return false;
    }
    out.psoMs = MsSince(t0);
    return true;
}

// 配布ビルドの pak に焼くシェーダーのキー（BakeForBuild と ProcessJob が同じ名前を使う）
static std::string BakedShaderRel(u64 hash) { return "shaders/graph/" + matgraph::Hex16(hash) + "_PS.cso"; }
static const char* const kBakedVsRel = "shaders/graph/vs.cso";

void GraphMaterialSystem::ProcessJob(const Job& job, ShaderRuntimeCompiler& compiler, const std::string& dxcVersion, JobResult& out)
{
    std::vector<u8> vs, ps;
    if (m_desc.allowCompile)
    {
        if (!CompileBlobs(job, compiler, dxcVersion, out, vs, ps)) return;
    }
    else
    {
        // ゲームモード（DXC を使わない）: BuildGame が pak に焼いた DXIL（shaders/graph/<HLSL ハッシュ>_PS.cso + 共通の vs.cso）から PSO を作る
        //   （Forward 以外の種別 = エディタ専用のプレビュー / サムネイルは焼かない）
        if (job.kind != Kind::Forward)
        {
            out.errorLog = "プレビュー用のシェーダーはエディタでだけ使えます";
            return;
        }
        out.level = 2;
        ps = vfs::ReadAsset(BakedShaderRel(job.hash));
        vs = vfs::ReadAsset(kBakedVsRel);
        if (ps.empty() || vs.empty())
        {
            out.errorLog = "配布ビルドにこのグラフのシェーダーが入っていません（" + BakedShaderRel(job.hash) + "）。エディタでゲームをビルドし直してください";
            return;
        }
        out.cacheHit = true;
    }
    if (job.phase) job.phase->store(2);   // PSO
    if (!BuildPsos(job.kind, vs, ps, out)) return;
    out.ok = true;
}

// 配布ビルド用: assets 内の全グラフ材質（.dxmat の graph キー）の HLSL を DXC で DXIL にして返す（PSO は作らない）。
bool GraphMaterialSystem::BakeForBuild(std::vector<BakedBlob>& out, std::vector<std::string>& errors)
{
    if (!m_available || !m_desc.allowCompile)
    {
        errors.push_back("グラフ材質のシェーダーをコンパイルできません（DXC が使えない / バインドレス非対応）");
        return false;
    }
    ShaderRuntimeCompiler compiler;
    if (!compiler.Initialize())
    {
        errors.push_back("DXC を初期化できません");
        return false;
    }
    const std::string dxcVersion = compiler.GetVersionString();
    std::set<u64> done;
    std::vector<u8> vsShared;
    bool ok = true;
    std::error_code ec;
    const std::string rootUtf8 = PathResolver::AssetsDir();
    const fs::path root(std::u8string(rootUtf8.begin(), rootUtf8.end()));
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code fec;
        if (!it->is_regular_file(fec) || it->path().extension() != ".dxmat") continue;
        const std::u8string rel8 = fs::relative(it->path(), root, fec).generic_u8string();
        const std::string rel(rel8.begin(), rel8.end());
        const std::vector<u8> bytes = vfs::ReadAsset(rel);
        MaterialAssetData data;
        if (bytes.empty() || !ParseMaterialAsset(bytes, data) || !data.IsGraph()) continue;

        Instance tmp;   // LoadAndCompile を再利用（診断 / 状態はここで捨てる）
        tmp.key = NormalizeKey(rel);
        tmp.graphRel = data.graphPath;
        tmp.data = data;
        std::shared_ptr<const matgraph::CompileResult> cr;
        if (!LoadAndCompile(tmp, cr))
        {
            ok = false;
            errors.push_back(rel + ": " + (tmp.errorLog.empty() ? std::string("グラフをコンパイルできません") : tmp.errorLog));
            continue;
        }
        if (!done.insert(cr->hash).second) continue;
        Job job;
        job.hash = cr->hash;
        job.hlsl = cr->hlsl;
        JobResult jr;
        std::vector<u8> vs, ps;
        if (!CompileBlobs(job, compiler, dxcVersion, jr, vs, ps))
        {
            ok = false;
            errors.push_back(rel + ": " + jr.errorLog);
            continue;
        }
        out.push_back({BakedShaderRel(cr->hash), std::move(ps)});
        if (vsShared.empty()) vsShared = std::move(vs);
    }
    if (!vsShared.empty()) out.push_back({kBakedVsRel, std::move(vsShared)});
    return ok;
}

// ---------------------------------------------------------------------------------------------
// バリアント（HLSL ハッシュ = PSO の共有単位）
// ---------------------------------------------------------------------------------------------
std::shared_ptr<GraphMaterialSystem::Variant> GraphMaterialSystem::GetOrCreateVariant(const matgraph::CompileResult& cr, Kind kind, bool force)
{
    const u64 vkey = VKey(cr.hash, kind);
    auto it = m_variants.find(vkey);
    if (it != m_variants.end() && !force)
    {
        it->second->lastUsedFrame = m_frame;
        return it->second;
    }
    auto v = std::make_shared<Variant>();
    v->hash = cr.hash;
    v->kind = kind;
    v->createdAt = Clock::now();
    v->lastUsedFrame = m_frame;
    if (it != m_variants.end())
    {
        // force: 古い変種は墓場へ（使用中の PSO を今すぐは捨てない）
        m_graveVariants.push_back({it->second, m_frame});
        it->second = v;
    }
    else
        m_variants[vkey] = v;
    // 段階コンパイル: 高速版（-Od。先に出る）と最適化版（背景）を同時に投入する。届く順は保証しない（StageGate が段階で採否を決める）
    const bool twoStage = m_staged && kind == Kind::Forward;
    if (twoStage)
    {
        Job f;
        f.hash = cr.hash; f.kind = kind; f.hlsl = cr.hlsl; f.force = force; f.stage = 1;
        f.phase = v->phaseFast;
        EnqueueJob(std::move(f));
        v->optQueued = true;
    }
    Job j;
    j.hash = cr.hash;
    j.kind = kind;
    j.hlsl = cr.hlsl;
    j.force = force;
    j.stage = 2;
    j.phase = v->phaseOpt;
    EnqueueJob(std::move(j));
    return v;
}

void GraphMaterialSystem::CancelVariant(const std::shared_ptr<Variant>& v)
{
    if (!v) return;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        n = m_jobs.CancelIf([&](const Job& j) { return j.hash == v->hash && j.kind == v->kind; });
    }
    if (n > 0)
    {
        m_inFlight -= static_cast<u32>(n);
        m_counters.jobsCancelled += static_cast<u32>(n);
    }
    auto it = m_variants.find(VKey(v->hash, v->kind));
    if (it != m_variants.end() && it->second == v) m_variants.erase(it);
}

// レコードを墓場へ。この Build の変種を他に誰も使っていない未完了のものなら、未着手のジョブを取り消して変種も捨てる（古い編集の間引き）
void GraphMaterialSystem::ReleaseBuild(std::unique_ptr<Build>& b)
{
    if (!b) return;
    FreeRecord(b->rec);
    std::shared_ptr<Variant> v = std::move(b->variant);
    b.reset();
    // use_count: この v + m_variants の 1 本 = 2 のときだけ「誰も使っていない」
    if (v && v->state == Variant::State::Compiling && v.use_count() == 2) CancelVariant(v);
}

void GraphMaterialSystem::DrainResults()
{
    std::deque<JobResult> done;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        done.swap(m_results);
    }
    for (JobResult& r : done)
    {
        ++m_counters.jobsDone;
        auto it = m_variants.find(VKey(r.hash, r.kind));
        if (it == m_variants.end()) continue;   // 取り消された / 捨てられた変種（古い編集）の結果
        Variant& v = *it->second;
        if (r.ok)
        {
            // ★順序保証: 届いた順ではなく段階で採否を決める。すでに同じ / 上の段階を受理していたら捨てる
            if (!v.gate.Accept(r.level))
            {
                ++m_counters.staleIgnored;
                continue;
            }
            if (v.less || v.lequal) m_gravePsos.push_back({std::move(v.less), std::move(v.lequal), m_frame});   // 差し替えで置き換わる PSO
            v.less = std::move(r.less);
            v.lequal = std::move(r.lequal);
            v.state = Variant::State::Ready;
            v.errorLog.clear();
            v.dxcMs = r.dxcMs;
            v.psoMs = r.psoMs;
            v.cacheHit = r.cacheHit;
            const double t = MsSince(v.createdAt);
            if (v.firstUsableMs == 0.0) v.firstUsableMs = t;
            if (r.level >= 2)
            {
                v.optDxcMs = r.dxcMs;
                v.optPsoMs = r.psoMs;
                v.finalMs = t;
                // 最適化版が出来た（高速版のジョブのキャッシュ命中を含む）: まだ待っている高速版のジョブは要らない
                size_t n = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    n = m_jobs.CancelIf([&](const Job& j) { return j.hash == v.hash && j.kind == v.kind; });
                }
                if (n > 0) { m_inFlight -= static_cast<u32>(n); m_counters.jobsCancelled += static_cast<u32>(n); }
            }
            else
            {
                v.fastDxcMs = r.dxcMs;
                v.fastPsoMs = r.psoMs;
            }
            Logger::Info("マテリアルグラフ: シェーダー準備完了 {:016x}（{} / DXC {:.0f} ms / PSO {:.0f} ms{}）", v.hash,
                         r.level >= 2 ? "最適化版" : "高速版 -Od", v.dxcMs, v.psoMs, v.cacheHit ? " / キャッシュ" : "");
        }
        else
        {
            if (v.gate.level >= 1)
            {
                // すでに描ける版がある（高速版が出来ていて、最適化版だけ失敗した等）: 描画は止めない。旧 / 高速版のまま
                Logger::Warn("マテリアルグラフ: 上の段階のコンパイルに失敗しました {:016x}（{} の版のまま描画します）\n{}", v.hash,
                             v.gate.level >= 2 ? "最適化" : "高速", r.errorLog);
                continue;
            }
            v.state = Variant::State::Failed;
            v.errorLog = std::move(r.errorLog);
            v.mapped = std::move(r.mapped);
            ++m_counters.jobsFailed;
            // 同じ変種の残りのジョブ（もう片方の段階）は同じエラーになるので取り消す
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                n = m_jobs.CancelIf([&](const Job& j) { return j.hash == v.hash && j.kind == v.kind; });
            }
            if (n > 0) { m_inFlight -= static_cast<u32>(n); m_counters.jobsCancelled += static_cast<u32>(n); }
            Logger::Error("マテリアルグラフ: シェーダーのコンパイルに失敗しました {:016x}（旧版があればそのまま描画します）\n{}", v.hash, v.errorLog);
        }
    }
}

void GraphMaterialSystem::CollectGarbage()
{
    // 墓場: 4 フレーム過ぎたら解放（フレーム多重化 3 + 余裕 1）
    for (size_t i = 0; i < m_graveRecords.size();)
    {
        if (m_frame >= m_graveRecords[i].frame + 4)
        {
            PoolRecord r = m_graveRecords[i].rec;
            m_alloc.Free(r.base, r.count);
            m_graveRecords[i] = m_graveRecords.back();
            m_graveRecords.pop_back();
        }
        else
            ++i;
    }
    for (size_t i = 0; i < m_gravePsos.size();)
    {
        if (m_frame >= m_gravePsos[i].frame + 4)
        {
            m_gravePsos[i] = std::move(m_gravePsos.back());
            m_gravePsos.pop_back();
        }
        else
            ++i;
    }
    for (size_t i = 0; i < m_graveVariants.size();)
    {
        if (m_frame >= m_graveVariants[i].frame + 4)
        {
            m_graveVariants[i] = std::move(m_graveVariants.back());
            m_graveVariants.pop_back();
        }
        else
            ++i;
    }
    // 誰も使っていない変種（インスタンスが参照していない）を、しばらく使われなかったら墓場へ
    for (auto it = m_variants.begin(); it != m_variants.end();)
    {
        const Variant& v = *it->second;
        if (it->second.use_count() == 1 && v.state != Variant::State::Compiling && m_frame > v.lastUsedFrame + 600)
        {
            m_graveVariants.push_back({it->second, m_frame});
            it = m_variants.erase(it);
        }
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------------------------
// インスタンス（.dxmat 1 個）
// ---------------------------------------------------------------------------------------------
bool GraphMaterialSystem::LoadAndCompile(Instance& inst, std::shared_ptr<const matgraph::CompileResult>& outCr)
{
    inst.diags.clear();
    inst.errorLog.clear();
    auto fail = [&](const char* code, const std::string& msg) {
        matgraph::Diagnostic d;
        d.severity = matgraph::Severity::Error;
        d.code = code;
        d.message = msg;
        inst.diags.push_back(d);
        inst.errorLog = msg;
        Logger::Error("マテリアルグラフ: {} ({})", msg, inst.key);
        return false;
    };

    // ★G2c: エディタが作った CompileResult をそのまま使う（ファイルの読み込みも CompileGraph も省く。プレビュー / ノード内サムネイル）
    if (inst.injected)
    {
        const matgraph::CompileResult& c = *inst.injected;
        inst.diags = c.diagnostics;
        if (inst.kind == Kind::Forward || inst.kind == Kind::PreviewLite)
            for (auto& d : CollectEngineDiagnostics(c, &inst.data)) inst.diags.push_back(std::move(d));
        if (!c.ok)
        {
            std::string msg = "グラフのコンパイルに失敗しました: " + inst.key;
            for (const auto& d : c.diagnostics)
                if (d.severity == matgraph::Severity::Error && d.reachable)
                    msg += "\n  [" + d.code + "] " + (d.nodeId.empty() ? std::string() : d.nodeId + ": ") + d.message;
            inst.errorLog = msg;
            return false;
        }
        outCr = inst.injected;
        return true;
    }

    std::string text;
    if (!inst.memText.empty())
        text = inst.memText;   // エディタのプレビュー: 保存前の本文
    else
    {
        const std::vector<u8> bytes = vfs::ReadAsset(inst.graphRel);
        if (bytes.empty()) return fail("E_GRAPH_LOAD", "グラフ（.dxmg）を読めません: " + inst.graphRel);
        text.assign(bytes.begin(), bytes.end());
    }
    matgraph::MaterialGraph graph;
    std::string err;
    if (!matgraph::LoadDxmg(text, graph, &err))
        return fail("E_GRAPH_PARSE", "グラフ（.dxmg）を解釈できません: " + inst.graphRel + "（" + err + "）");

    const auto t0 = Clock::now();
    matgraph::CompileOptions opt;      // sourceName は空 = 同じ構造なら同じ HLSL 全文（バリアントとキャッシュの共有のため）
    matgraph::CompileResult cr = matgraph::CompileGraph(graph, opt);
    inst.codegenMs = MsSince(t0);
    ++m_counters.codegen;
    inst.diags = cr.diagnostics;

    // ---- G2b で未対応の設定 / 無効な上書きは警告（描画は不透明 / DefaultLit として続ける）----
    if (inst.kind == Kind::Forward || inst.kind == Kind::PreviewLite)
        for (auto& d : CollectEngineDiagnostics(cr, &inst.data)) inst.diags.push_back(std::move(d));

    if (!cr.ok)
    {
        std::string msg = "グラフのコンパイルに失敗しました: " + inst.graphRel;
        for (const auto& d : cr.diagnostics)
            if (d.severity == matgraph::Severity::Error && d.reachable)
                msg += "\n  [" + d.code + "] " + (d.nodeId.empty() ? std::string() : d.nodeId + ": ") + d.message;
        inst.errorLog = msg;
        Logger::Error("マテリアルグラフ: {}", msg);
        return false;
    }
    outCr = std::make_shared<const matgraph::CompileResult>(std::move(cr));

    if (!vfs::InGameMode() && inst.memText.empty())
    {
        std::error_code ec;
        const auto t = fs::last_write_time(PathResolver::AssetsDir() + inst.graphRel, ec);
        if (!ec) inst.graphMtime = t;
    }
    return true;
}

void GraphMaterialSystem::RepackBuild(Instance& inst, Build& b, ID3D12GraphicsCommandList* cmd)
{
    if (!b.cr || !b.rec.valid) return;
    ResourceManager* rm = m_desc.resources;
    static std::set<std::string> warnedMissing;   // 同じパスの警告は 1 回だけ
    const matgraph::TextureResolver resolver = [&](const std::string& path, const std::string& usage) -> matgraph::TextureBinding {
        bool srgb = false;
        TextureUsage tu = TextureUsage::NonColor;
        Texture* fallback = rm->GetDefaultWhiteTexture();
        if (usage == "Color") { srgb = true; tu = TextureUsage::BaseColor; }
        else if (usage == "Normal") { srgb = false; tu = TextureUsage::Normal; fallback = rm->GetDefaultNormalTexture(); }
        Texture* t = nullptr;
        if (!path.empty())
        {
            if (!vfs::Exists(path))
            {
                if (warnedMissing.insert(inst.key + "|" + path).second)
                    Logger::Warn("マテリアルグラフ: テクスチャが見つかりません: {}（{}。既定のテクスチャで描きます）", path, inst.key);
            }
            else
                t = rm->GetOrLoadTexture(Wide(PathResolver::AssetsDir() + path), cmd, srgb, tu);
        }
        if (!t || t->GetArraySize() > 1) t = fallback;
        matgraph::TextureBinding tb;
        tb.srvIndex = t->GetSrvIndex();
        tb.width = static_cast<float>(t->GetWidth());
        tb.height = static_cast<float>(t->GetHeight());
        return tb;
    };
    const matgraph::ParamOverrides ov = ToParamOverrides(inst.data);
    const std::vector<float> rec = matgraph::PackParamRecord(*b.cr, ov, resolver);
    const size_t n = std::min(rec.size(), static_cast<size_t>(b.rec.count) * 4);
    float* dst = &m_shadow[static_cast<size_t>(b.rec.base) * 4];
    if (std::memcmp(dst, rec.data(), n * sizeof(float)) != 0)
    {
        std::memcpy(dst, rec.data(), n * sizeof(float));
        ++m_poolVersion;
    }
}

void GraphMaterialSystem::Promote(Instance& inst)
{
    // 最適化版が出来た時刻（描いている版の変種が最高の段階に達した）
    auto stampFinal = [&]() {
        if (inst.active && inst.active->variant && inst.active->variant->gate.level >= 2 && inst.finalMs <= 0.0)
            inst.finalMs = MsSince(inst.active->createdAt);
    };
    if (!inst.pending || !inst.pending->variant) { stampFinal(); return; }
    Variant& v = *inst.pending->variant;
    if (v.state == Variant::State::Ready)
    {
        if (inst.active) FreeRecord(inst.active->rec);
        inst.active = std::move(inst.pending);
        inst.activeHash = inst.active->cr->hash;
        m_peek[inst.key] = inst.activeHash;
        inst.failed = false;
        inst.errorLog.clear();
        inst.firstUsableMs = MsSince(inst.active->createdAt);
        inst.finalMs = 0.0;
        stampFinal();
    }
    else if (v.state == Variant::State::Failed)
    {
        inst.failed = true;
        inst.errorLog = v.errorLog;
        // DXC エラーの nodeId 逆引きを診断へ足す（グラフの診断は残す）
        for (const auto& d : v.mapped) inst.diags.push_back(d);
        ReleaseBuild(inst.pending);
    }
    else
        stampFinal();
}

void GraphMaterialSystem::RebuildInstance(Instance& inst, ID3D12GraphicsCommandList* cmd)
{
    const bool wantGraph = inst.graphDirty;
    inst.graphDirty = false;
    inst.paramsDirty = false;
    inst.built = true;

    bool structural = false;
    if (wantGraph)
    {
        std::shared_ptr<const matgraph::CompileResult> cr;
        if (!LoadAndCompile(inst, cr))
        {
            inst.failed = true;   // active / pending はそのまま（旧版維持）。値の更新は下で続ける
        }
        else
        {
            inst.failed = false;
            const u64 h = cr->hash;
            if (inst.active && inst.active->cr->hash == h)
            {
                inst.active->cr = cr;   // 同じ HLSL: スロット表の既定値だけ変わりうる
                ReleaseBuild(inst.pending);
            }
            else if (inst.pending && inst.pending->cr->hash == h)
            {
                inst.pending->cr = cr;
            }
            else
            {
                auto nb = std::make_unique<Build>();
                nb->cr = cr;
                if (!AllocRecord(static_cast<u32>(cr->slotCount), nb->rec))
                {
                    inst.failed = true;
                    inst.errorLog = "パラメータプールが満杯です（グラフ材質を増やしすぎています）";
                    Logger::Error("マテリアルグラフ: {} ({})", inst.errorLog, inst.key);
                }
                else
                {
                    nb->variant = GetOrCreateVariant(*cr, inst.kind, m_forceNext);
                    ReleaseBuild(inst.pending);   // 未完了の古い編集は捨てる（他に使う人がいなければ未着手のジョブも取り消す）
                    inst.pending = std::move(nb);
                    ++inst.generation;
                    structural = true;
                }
            }
        }
    }
    // ★値の更新: active / pending の両方のレコードを書き直す（再コンパイルなし）
    if (inst.active) RepackBuild(inst, *inst.active, cmd);
    if (inst.pending) RepackBuild(inst, *inst.pending, cmd);
    if (!structural)
    {
        ++inst.valueUpdates;
        ++m_counters.valueUpdates;
    }
    Promote(inst);
}

bool GraphMaterialSystem::Resolve(const std::string& dxmatRel, const MaterialAssetManager::Entry& entry,
                                  ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out)
{
    return ResolveData(dxmatRel, entry.data, entry.loadSerial, cmd, frameIndex, lequal, out);
}

GraphMaterialSystem::Instance& GraphMaterialSystem::InstanceFor(const std::string& key)
{
    const std::string k = NormalizeKey(key);
    auto& slot = m_instances[k];
    if (!slot) { slot = std::make_unique<Instance>(); slot->key = k; }
    return *slot;
}

void GraphMaterialSystem::SetInstanceGraphText(const std::string& instanceKey, const std::string& dxmgText)
{
    Instance& inst = InstanceFor(instanceKey);
    inst.memText = dxmgText;
    inst.graphDirty = true;
}

void GraphMaterialSystem::SetInstanceKind(const std::string& instanceKey, Kind kind)
{
    InstanceFor(instanceKey).kind = kind;
}

void GraphMaterialSystem::SetInstanceCompiled(const std::string& instanceKey, std::shared_ptr<const matgraph::CompileResult> cr)
{
    Instance& inst = InstanceFor(instanceKey);
    inst.injected = std::move(cr);
    inst.graphDirty = true;
}

void GraphMaterialSystem::RemoveInstance(const std::string& instanceKey)
{
    const std::string k = NormalizeKey(instanceKey);
    auto it = m_instances.find(k);
    if (it == m_instances.end()) return;
    Instance& inst = *it->second;
    ReleaseBuild(inst.active);
    ReleaseBuild(inst.pending);
    m_peek.erase(k);
    m_instances.erase(it);
}

// 描画の共通の尾（インスタンスの更新の後）: 描ける版があれば PSO とレコードの位置を返す
bool GraphMaterialSystem::ResolveCommon(Instance& inst, ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out)
{
    (void)cmd;
    const Build* b = inst.active.get();
    if (!b || !b->variant || b->variant->state != Variant::State::Ready)
    {
        ++m_counters.fallbackDraws;
        return false;
    }
    b->variant->lastUsedFrame = m_frame;
    SyncPool(frameIndex);
    // lequal を持たない種別（PreviewLite / Node*）は less だけ。段階の差し替えで PSO が入れ替わるので毎回変種から引く
    out.pso = (lequal && b->variant->lequal) ? b->variant->lequal.Get() : b->variant->less.Get();
    out.recordBase = b->rec.base;
    out.poolSrvIndex = m_poolSrv[frameIndex % kFrames];
    return out.pso != nullptr;
}

bool GraphMaterialSystem::ResolveInstance(const std::string& instanceKey, ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out)
{
    if (!m_available) return false;
    const auto it = m_instances.find(NormalizeKey(instanceKey));
    if (it == m_instances.end()) return false;
    Instance& inst = *it->second;
    inst.lastUsedFrame = m_frame;
    if (!inst.built || inst.graphDirty || inst.paramsDirty) RebuildInstance(inst, cmd);
    else Promote(inst);
    return ResolveCommon(inst, cmd, frameIndex, lequal, out);
}

bool GraphMaterialSystem::ResolveData(const std::string& dxmatRel, const MaterialAssetData& dataIn, u32 dataSerial,
                                      ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out)
{
    if (!m_available || !dataIn.IsGraph()) return false;
    Instance& inst = InstanceFor(dxmatRel);
    inst.lastUsedFrame = m_frame;

    if (!inst.built || inst.dxmatSerial != dataSerial || inst.graphRel != dataIn.graphPath)
    {
        const bool graphChanged = !inst.built || inst.graphRel != dataIn.graphPath;
        inst.data = dataIn;
        inst.graphRel = dataIn.graphPath;
        inst.dxmatSerial = dataSerial;
        if (graphChanged) inst.graphDirty = true;
        else inst.paramsDirty = true;
        RebuildInstance(inst, cmd);
    }
    else if (inst.graphDirty || inst.paramsDirty)
        RebuildInstance(inst, cmd);
    else
        Promote(inst);
    return ResolveCommon(inst, cmd, frameIndex, lequal, out);
}

u64 GraphMaterialSystem::PeekShaderHash(const std::string& dxmatRel) const
{
    const auto it = m_peek.find(NormalizeKey(dxmatRel));
    return it == m_peek.end() ? 0 : it->second;
}

void GraphMaterialSystem::Update(f32 dt, u32 frameIndex, ID3D12GraphicsCommandList* cmd)
{
    if (!m_available) return;
    ++m_frame;
    DrainResults();

    // .dxmg のホットリロード（0.5 秒ごとに更新時刻を見る。エディタのみ）
    m_pollTimer += dt;
    if (m_desc.pollFiles && !vfs::InGameMode() && m_pollTimer >= 0.5f)
    {
        m_pollTimer = 0.0f;
        for (auto& [key, p] : m_instances)
        {
            Instance& inst = *p;
            if (!inst.built || inst.graphRel.empty() || !inst.memText.empty() || inst.injected) continue;
            std::error_code ec;
            const auto t = fs::last_write_time(PathResolver::AssetsDir() + inst.graphRel, ec);
            if (!ec && t != inst.graphMtime)
            {
                inst.graphMtime = t;
                inst.graphDirty = true;
            }
        }
    }
    for (auto& [key, p] : m_instances)
    {
        Instance& inst = *p;
        if (inst.graphDirty || inst.paramsDirty) RebuildInstance(inst, cmd);
        else Promote(inst);
    }
    m_forceNext = false;
    CollectGarbage();
    SyncPool(frameIndex);
}

void GraphMaterialSystem::NotifyGraphFileChanged(const std::string& graphRel)
{
    const std::string key = NormalizeKey(graphRel);
    for (auto& [k, p] : m_instances)
        if (NormalizeKey(p->graphRel) == key) p->graphDirty = true;
}

bool GraphMaterialSystem::Rebuild(const std::string& dxmatRel, bool force)
{
    const auto it = m_instances.find(NormalizeKey(dxmatRel));
    if (it == m_instances.end()) return false;
    it->second->graphDirty = true;
    if (force) m_forceNext = true;
    return true;
}

void GraphMaterialSystem::RebuildAll(bool force)
{
    for (auto& [key, p] : m_instances) p->graphDirty = true;
    if (force) m_forceNext = true;
}

bool GraphMaterialSystem::HasPending() const
{
    if (m_inFlight.load() > 0) return true;
    for (const auto& [key, p] : m_instances)
        if (p->pending && p->pending->variant && p->pending->variant->state == Variant::State::Compiling) return true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_results.empty()) return true;
    }
    return false;
}

bool GraphMaterialSystem::WaitIdle(int timeoutMs, ID3D12GraphicsCommandList* cmd)
{
    const auto t0 = Clock::now();
    for (;;)
    {
        DrainResults();
        for (auto& [key, p] : m_instances)
        {
            Instance& inst = *p;
            if (inst.graphDirty || inst.paramsDirty) RebuildInstance(inst, cmd);
            else Promote(inst);
        }
        m_forceNext = false;
        if (!HasPending()) return true;
        if (MsSince(t0) > timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---------------------------------------------------------------------------------------------
// 状態
// ---------------------------------------------------------------------------------------------
GraphMaterialSystem::InstanceStatus GraphMaterialSystem::MakeStatus(const Instance& inst) const
{
    InstanceStatus s;
    s.dxmat = inst.key;
    s.graph = inst.graphRel;
    s.kind = inst.kind;
    s.hasActive = inst.active && inst.active->variant && inst.active->variant->state == Variant::State::Ready;
    if (s.hasActive) s.level = inst.active->variant->gate.level;
    s.optimizing = s.hasActive && s.level == 1 && inst.active->variant->optQueued;
    s.firstUsableMs = inst.firstUsableMs;
    s.finalMs = inst.finalMs;
    s.compiling = inst.pending && inst.pending->variant && inst.pending->variant->state == Variant::State::Compiling;
    s.failed = inst.failed;
    s.activeHash = inst.active && inst.active->cr ? inst.active->cr->hash : 0;
    s.pendingHash = inst.pending && inst.pending->cr ? inst.pending->cr->hash : 0;
    const Build* ref = inst.active ? inst.active.get() : inst.pending.get();
    if (ref && ref->cr)
    {
        s.slotCount = static_cast<u32>(ref->cr->slotCount);
        s.textureCount = static_cast<u32>(ref->cr->stats.textures);
        s.recordBase = ref->rec.base;
    }
    s.generation = inst.generation;
    s.valueUpdates = inst.valueUpdates;
    s.errorLog = inst.errorLog;
    s.diagnostics = inst.diags;
    s.codegenMs = inst.codegenMs;
    const Variant* v = inst.active && inst.active->variant ? inst.active->variant.get()
                     : (inst.pending && inst.pending->variant ? inst.pending->variant.get() : nullptr);
    if (v)
    {
        s.dxcMs = v->dxcMs;
        s.psoMs = v->psoMs;
        s.cacheHit = v->cacheHit;
        s.fastDxcMs = v->fastDxcMs; s.fastPsoMs = v->fastPsoMs;
        s.optDxcMs = v->optDxcMs;   s.optPsoMs = v->optPsoMs;
    }
    // ワーカーの進み具合: 作っている最中の新しい版（段階なら高速版の方が先に進む）/ 描きながら作っている最適化版
    if (s.compiling && inst.pending && inst.pending->variant)
    {
        const Variant& pv = *inst.pending->variant;
        s.pendingPhase = pv.optQueued ? std::max(pv.phaseFast->load(), 0) : pv.phaseOpt->load();
    }
    if (s.optimizing) s.optPhase = inst.active->variant->phaseOpt->load();
    return s;
}

std::vector<GraphMaterialSystem::InstanceStatus> GraphMaterialSystem::ListInstances() const
{
    std::vector<InstanceStatus> out;
    for (const auto& [key, p] : m_instances) out.push_back(MakeStatus(*p));
    return out;
}

bool GraphMaterialSystem::GetInstanceStatus(const std::string& dxmatRel, InstanceStatus& out) const
{
    const auto it = m_instances.find(NormalizeKey(dxmatRel));
    if (it == m_instances.end()) return false;
    out = MakeStatus(*it->second);
    return true;
}

bool GraphMaterialSystem::DebugReadRecord(const std::string& dxmatRel, std::vector<float>& out) const
{
    const auto it = m_instances.find(NormalizeKey(dxmatRel));
    if (it == m_instances.end() || !it->second->active || !it->second->active->rec.valid) return false;
    const PoolRecord& r = it->second->active->rec;
    out.assign(m_shadow.begin() + static_cast<size_t>(r.base) * 4, m_shadow.begin() + static_cast<size_t>(r.base + r.count) * 4);
    return true;
}

GraphMaterialSystem::Counters GraphMaterialSystem::GetCounters() const
{
    Counters c = m_counters;
    c.instances = static_cast<u32>(m_instances.size());
    c.variants = static_cast<u32>(m_variants.size());
    c.poolUsed = m_alloc.Used();
    c.poolCapacity = kPoolCapacity;
    c.dxcCompiles = m_dxcCompiles.load();
    c.fastCompiles = m_fastCompiles.load();
    c.optCompiles = m_optCompiles.load();
    c.psoCreates = m_psoCreates.load();
    c.workers = static_cast<u32>(m_workers.size());
    if (m_cache)
    {
        c.cacheHits = m_cache->hits.load();
        c.cacheMisses = m_cache->misses.load();
        c.cacheStores = m_cache->stores.load();
    }
    return c;
}

} // namespace dx12e
