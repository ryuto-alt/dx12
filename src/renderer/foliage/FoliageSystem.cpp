#include "renderer/foliage/FoliageSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>

#include <nlohmann/json.hpp>

#include "core/Logger.h"
#include "ecs/Components.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/PipelineState.h"
#include "graphics/Texture.h"
#include "graphics/RootSignature.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"
#include "renderer/foliage/FoliageLayerOps.h"
#include "resource/ResourceManager.h"
#include "resource/ShaderCompiler.h"

using Microsoft::WRL::ComPtr;

namespace dx12e::foliage
{
namespace
{
// ---- 頂点入力: slot0 = メッシュ（Vertex の先頭 5 要素）/ slot1 = compact インスタンス（64B PER_INSTANCE）----
const D3D12_INPUT_ELEMENT_DESC kFoliageLayout[] =
{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 8,  DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    {"TEXCOORD", 9,  DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    {"TEXCOORD", 10, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    {"TEXCOORD", 11, DXGI_FORMAT_R32G32B32A32_UINT,  1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
};

enum Scope : u32 { kScopeCull = 0, kScopeShadow0, kScopeShadow1, kScopeDepth, kScopeMain, kScopeCount };
constexpr u32 kTsPerFrame = kScopeCount * 2;

// b0 に積むレイヤー別の風 / 色
struct LayerWindConst { f32 bend, flutter, bendExp, heightLocal; };

std::string ResolveModelPath(const std::string& assetsDir, const std::string& rel)
{
    std::error_code ec;
    if (std::filesystem::path(rel).is_absolute()) return rel;
    return assetsDir + rel;
}

std::string LayerNameOf(entt::registry& reg, entt::entity e)
{
    const NameTag* nt = reg.try_get<NameTag>(e);
    return nt ? nt->name : std::string("Foliage");
}
} // namespace

// ===========================================================================
struct FoliageSystem::Impl
{
    Deps d;
    ID3D12Device* dev = nullptr;
    FoliageCuller culler;
    ComPtr<ID3D12CommandSignature> cmdSig;
    std::unique_ptr<PipelineState> psoMain[2][2];   // [mask][depthLEqual]
    std::unique_ptr<PipelineState> psoDepth[2];     // [mask]
    std::unique_ptr<PipelineState> psoShadow[2];    // [mask]
    std::unique_ptr<PipelineState> psoVel[2];       // [mask]
    bool physApplied = false;

    ComPtr<ID3D12Resource> windCb[8];
    u8* windCbMapped[8] = {};
    u32 frameCount = 3;

    // 計測
    ComPtr<ID3D12QueryHeap> qHeap;
    ComPtr<ID3D12Resource> qReadback;
    u64 tsFreq = 0;
    u32 tsWritten[8] = {};      // フレームスロットごとの「書いた scope」ビット
    u32 curSlot = 0;

    u64 lastFrameId = ~0ull;
    struct Retired { u64 frame; std::unique_ptr<FoliageCuller::Layer> layer; std::vector<ComPtr<ID3D12Resource>> res; };
    std::vector<Retired> retired;
    u64 curFrameId = 0;
    SceneWind wind;

    void Stamp(ID3D12GraphicsCommandList* cmd, Scope s, bool end)
    {
        if (!qHeap) return;
        cmd->EndQuery(qHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, curSlot * kTsPerFrame + s * 2 + (end ? 1 : 0));
        if (end) tsWritten[curSlot] |= (1u << s);
    }
    bool BuildPsos();
    bool BuildForwardPsos();
};

struct FoliageSystem::LayerRt
{
    struct Group
    {
        u32 variant = 0, lod = 0, submesh = 0, slot = 0;
        Mesh* mesh = nullptr;
        const Material* mat = nullptr;
        bool mask = false;
        f32 heightLocal = 1.0f;
        u32 indexCount = 0;
        // b2（PBR 定数）を作るための素の値
        f32 metallic = 0.0f, roughness = 0.5f;
        u32 flags = 0, packedTint = 0, packedEmissive = 0;
        u64 tris = 0;
    };
    u32 id = 0;
    entt::entity e = entt::null;
    std::string name;
    FoliageLayer params;                    // 今フレームのパラメータ（_set は持たない）
    std::unique_ptr<FoliageCuller::Layer> gpu;
    std::vector<Group> groups;
    std::string sig;                        // モデル構成 + 容量のシグネチャ
    u32 instVersion = 0;
    u32 instCount = 0;
    const void* instPtr = nullptr;   // 実体のポインタ（Undo が古い実体へ戻すと version / 個数が偶然一致し得るので、ポインタでも見る）
    u32 variantCount = 0, lodStride = 1;
    u32 lodCount[kMaxVariants] = {1, 1, 1, 1};
    f32 boundCenterY = 0.0f, boundRadius = 1.0f;
    f32 heightLocal[kMaxVariants] = {1, 1, 1, 1};
    Affine34 layerAffine;
    f32 layerScale = 1.0f;
    bool drawable = false;
    bool culledThisFrame = false;
    bool shadowCulled[2] = {false, false};
    std::string note;
    const CachedModel* models[kMaxVariants][kMaxLods] = {};
    u32 capMain = 0, capShadow = 0;
    u64 seenFrame = 0;
};

FoliageSystem::FoliageSystem() = default;
FoliageSystem::~FoliageSystem() { Shutdown(); }

// ---------------------------------------------------------------------------
bool FoliageSystem::Impl::BuildForwardPsos()
{
    const wchar_t* ps[2] = {physApplied ? L"ForwardFoliagePhys_PS.cso" : L"ForwardFoliage_PS.cso",
                            physApplied ? L"ForwardFoliageMaskPhys_PS.cso" : L"ForwardFoliageMask_PS.cso"};
    auto vs = ShaderCompiler::LoadFromFile(d.shaderDir + L"ForwardFoliage_VS.cso");
    if (vs.GetSize() == 0) { Logger::Warn("植生: ForwardFoliage_VS.cso が読めない"); return false; }
    for (int mask = 0; mask < 2; ++mask)
    {
        auto p = ShaderCompiler::LoadFromFile(d.shaderDir + ps[mask]);
        if (p.GetSize() == 0) { Logger::Warn("植生: フォワード PS が読めない"); return false; }
        PipelineStateBuilder b;
        b.SetRootSignature(d.rootSig->Get())
         .SetVertexShader(vs.GetData(), vs.GetSize())
         .SetPixelShader(p.GetData(), p.GetSize())
         .SetInputLayout(kFoliageLayout, static_cast<u32>(std::size(kFoliageLayout)))
         .SetRenderTargetFormat(d.sceneColorFormat)
         .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
         .SetDepthEnabled(true)
         .SetCullMode(D3D12_CULL_MODE_NONE);
        for (int leq = 0; leq < 2; ++leq)
        {
            b.SetDepthFunc(leq ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_LESS);
            if (!psoMain[mask][leq]) psoMain[mask][leq] = std::make_unique<PipelineState>();
            psoMain[mask][leq]->Initialize(*d.gdev, b);
        }
    }
    return true;
}

bool FoliageSystem::Impl::BuildPsos()
{
    if (!BuildForwardPsos()) return false;
    auto vsD = ShaderCompiler::LoadFromFile(d.shaderDir + L"FoliageDepth_VS.cso");
    auto vsV = ShaderCompiler::LoadFromFile(d.shaderDir + L"FoliageVelocity_VS.cso");
    if (vsD.GetSize() == 0 || vsV.GetSize() == 0) { Logger::Warn("植生: 深度 / 速度の VS が読めない"); return false; }
    for (int mask = 0; mask < 2; ++mask)
    {
        auto psD = ShaderCompiler::LoadFromFile(d.shaderDir + (mask ? L"FoliageDepthMask_PS.cso" : L"FoliageDepth_PS.cso"));
        auto psV = ShaderCompiler::LoadFromFile(d.shaderDir + (mask ? L"FoliageVelocityMask_PS.cso" : L"FoliageVelocity_PS.cso"));
        if (psD.GetSize() == 0 || psV.GetSize() == 0) { Logger::Warn("植生: 深度 / 速度の PS が読めない"); return false; }
        {
            PipelineStateBuilder b;   // カメラの深度プリパス（バイアス無し）
            b.SetRootSignature(d.rootSig->Get())
             .SetVertexShader(vsD.GetData(), vsD.GetSize())
             .SetPixelShader(psD.GetData(), psD.GetSize())
             .SetInputLayout(kFoliageLayout, static_cast<u32>(std::size(kFoliageLayout)))
             .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
             .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
             .SetDepthEnabled(true)
             .SetCullMode(D3D12_CULL_MODE_NONE);
            if (!psoDepth[mask]) psoDepth[mask] = std::make_unique<PipelineState>();
            psoDepth[mask]->Initialize(*d.gdev, b);
            b.SetDepthBias(8000, 2.0f);   // 影（既存の ShadowMask と同じバイアス）
            if (!psoShadow[mask]) psoShadow[mask] = std::make_unique<PipelineState>();
            psoShadow[mask]->Initialize(*d.gdev, b);
        }
        {
            const DXGI_FORMAT rt[2] = {d.velocityFormat, d.gbufferFormat};
            PipelineStateBuilder b;
            b.SetRootSignature(d.rootSig->Get())
             .SetVertexShader(vsV.GetData(), vsV.GetSize())
             .SetPixelShader(psV.GetData(), psV.GetSize())
             .SetInputLayout(kFoliageLayout, static_cast<u32>(std::size(kFoliageLayout)))
             .SetRenderTargetFormats(2, rt)
             .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
             .SetDepthEnabled(true)
             .SetCullMode(D3D12_CULL_MODE_NONE);
            if (!psoVel[mask]) psoVel[mask] = std::make_unique<PipelineState>();
            psoVel[mask]->Initialize(*d.gdev, b);
        }
    }
    return true;
}

bool FoliageSystem::Initialize(const Deps& d, std::string* err)
{
    Shutdown();
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!d.gdev || !d.rootSig || !d.srvHeap || !d.resources) return fail("依存が足りない");
    m_impl = std::make_unique<Impl>();
    Impl& I = *m_impl;
    I.d = d;
    I.dev = d.gdev->GetDevice();
    I.frameCount = std::min(std::max(d.frameCount, 1u), 8u);
    std::string e;
    if (!I.culler.Initialize(I.dev, d.shaderDir, I.frameCount, &e)) { if (err) *err = "カリング compute: " + e; return false; }
    if (!I.BuildPsos()) return fail("PSO の作成に失敗（.cso が無い？ ビルドし直してください）");

    // コマンドシグネチャ（DrawIndexedInstanced 引数だけ。ルート引数は差し替えない＝ルートシグネチャ無し）
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC cs{};
        cs.ByteStride = 20;
        cs.NumArgumentDescs = 1;
        cs.pArgumentDescs = &arg;
        if (FAILED(I.dev->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&I.cmdSig)))) return fail("コマンドシグネチャの作成に失敗");
    }
    // 風の専用 CBV（深度 / 影 / 速度の VS が b1 で読む）
    for (u32 f = 0; f < I.frameCount; ++f)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 256; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(I.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&I.windCb[f])))) return fail("風 CBV の作成に失敗");
        void* m = nullptr; D3D12_RANGE none{0, 0};
        I.windCb[f]->Map(0, &none, &m);
        I.windCbMapped[f] = static_cast<u8*>(m);
        std::memset(m, 0, 256);
    }
    // GPU タイマー
    if (d.queue && SUCCEEDED(d.queue->GetTimestampFrequency(&I.tsFreq)) && I.tsFreq > 0)
    {
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = kTsPerFrame * I.frameCount;
        if (SUCCEEDED(I.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&I.qHeap))))
        {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 8ull * qd.Count; rd.Height = 1; rd.DepthOrArraySize = 1;
            rd.MipLevels = 1; rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(I.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&I.qReadback))))
                I.qHeap.Reset();
        }
    }
    m_ready = true;
    Logger::Info("植生: 初期化（GPU カリング + ExecuteIndirect）");
    return true;
}

void FoliageSystem::Shutdown()
{
    m_layers.clear();
    if (m_impl)
    {
        for (auto& c : m_impl->windCb) if (c) c->Unmap(0, nullptr);
        m_impl->culler.Shutdown();
    }
    m_impl.reset();
    m_ready = false;
    m_drawable = 0;
    m_stats.clear();
}

u64 FoliageSystem::TotalGpuBytes() const
{
    u64 t = 0;
    for (const auto& [id, l] : m_layers) if (l->gpu) t += l->gpu->GpuBytes();
    return t;
}

// ---------------------------------------------------------------------------
bool FoliageSystem::BeginFrame(const FrameIn& in)
{
    if (!m_ready || !in.reg || !in.cmd) return false;
    Impl& I = *m_impl;
    if (I.lastFrameId == in.frameId) return m_drawable > 0;   // フレームで 1 回
    I.lastFrameId = in.frameId;
    I.curFrameId = in.frameId;
    const u32 slot = in.frameSlot % I.frameCount;
    I.curSlot = slot;

    // ---- 風のフレーム値（PerFrame の予約領域 / 専用 CBV へ）----
    I.wind = in.wind ? *in.wind : SceneWind{};
    {
        const WindFrame w = MakeWindFrame(I.wind, in.time, in.prevTime);
        m_windA = {w.dirX, w.dirZ, w.speed, w.gustStrength};
        m_windB = {w.gustFreq, w.turbulence, w.time, w.prevTime};
        f32 cb[8] = {m_windA.x, m_windA.y, m_windA.z, m_windA.w, m_windB.x, m_windB.y, m_windB.z, m_windB.w};
        std::memcpy(I.windCbMapped[slot], cb, sizeof(cb));
    }

    // ---- 物理ライティング単位の切替（PS の差し替え）----
    if (I.physApplied != in.physicalLightUnits)
    {
        I.physApplied = in.physicalLightUnits;
        I.BuildForwardPsos();
    }

    // ---- GPU タイマーの読み戻し（このスロットの前回の結果）----
    if (I.qReadback && I.tsWritten[slot] != 0)
    {
        D3D12_RANGE rr{static_cast<SIZE_T>(slot * kTsPerFrame * 8), static_cast<SIZE_T>((slot + 1) * kTsPerFrame * 8)};
        void* m = nullptr;
        if (SUCCEEDED(I.qReadback->Map(0, &rr, &m)))
        {
            const u64* ts = static_cast<const u64*>(m) + slot * kTsPerFrame;
            f32 ms[kScopeCount] = {};
            for (u32 s = 0; s < kScopeCount; ++s)
                if (I.tsWritten[slot] & (1u << s))
                    ms[s] = static_cast<f32>(static_cast<double>(ts[s * 2 + 1] - ts[s * 2]) * 1000.0 / static_cast<double>(I.tsFreq));
            D3D12_RANGE none{0, 0};
            I.qReadback->Unmap(0, &none);
            m_gpuMs.cull = ms[kScopeCull];
            m_gpuMs.shadowDraw = ms[kScopeShadow0] + ms[kScopeShadow1];
            m_gpuMs.depthDraw = ms[kScopeDepth];
            m_gpuMs.mainDraw = ms[kScopeMain];
        }
        I.tsWritten[slot] = 0;
    }

    // ---- 退避（GPU が使い終わったもの）を解放 ----
    for (size_t i = 0; i < I.retired.size();)
    {
        if (in.frameId > I.retired[i].frame + I.frameCount + 1) { I.retired.erase(I.retired.begin() + static_cast<long>(i)); }
        else ++i;
    }

    // ---- レイヤー収集 ----
    entt::registry& reg = *in.reg;
    std::set<u32> seen;
    m_drawable = 0;
    m_stats.clear();
    auto view = reg.view<FoliageLayer>();
    for (auto [e, layer] : view.each())
    {
        const u32 id = static_cast<u32>(entt::to_integral(e));
        seen.insert(id);
        auto& up = m_layers[id];
        if (!up) { up = std::make_unique<LayerRt>(); up->id = id; }
        LayerRt& rt = *up;
        rt.e = e;
        rt.name = LayerNameOf(reg, e);
        rt.seenFrame = in.frameId;
        rt.culledThisFrame = false;
        rt.shadowCulled[0] = rt.shadowCulled[1] = false;
        // パラメータのコピー（_set は持たない）
        {
            const auto keep = layer._set;
            (void)keep;
            FoliageLayer p = layer;
            p._set.reset();
            rt.params = std::move(p);
        }
        rt.drawable = false;
        rt.note.clear();
        {
            // レイヤーの世界行列（親子込み。Renderer は ECS ライブラリにリンクしないので呼び出し側が解決する）
            DirectX::XMFLOAT4X4 wf;
            if (in.worldOf) wf = in.worldOf(e);
            else DirectX::XMStoreFloat4x4(&wf, DirectX::XMMatrixIdentity());
            rt.layerAffine = AffineFromWorld(wf);
            rt.layerScale = std::max(MaxColumnLength(rt.layerAffine), 1e-4f);
        }

        FoliageStatsLayer st;
        st.entity = id; st.name = rt.name;

        if (!layer.enabled) { rt.note = "disabled"; st.note = rt.note; m_stats.push_back(st); continue; }
        const u32 nVar = CountVariants(layer);
        if (nVar == 0) { rt.note = "variant0 (model) is empty"; st.note = rt.note; m_stats.push_back(st); continue; }
        std::string loadErr;
        if (!EnsureLoaded(layer, &loadErr) && !layer._set)
        {
            rt.note = layer.instancePath.empty() ? "no instances (scatter first)" : ("cannot load " + layer.instancePath);
            st.note = rt.note; m_stats.push_back(st); continue;
        }
        if (!layer._set || layer._set->Count() == 0) { rt.note = "0 instances"; st.note = rt.note; m_stats.push_back(st); continue; }
        FoliageInstanceSet& set = *layer._set;

        // ワールド行列（呼び出し側が Transform の親子を解決して layer に持たせられないので、ここでは Transform だけを見る）
        // ★親子は Application 側で処理済みの _world を使いたいが、コンポーネントに載せない方針なので registry の Transform を読む。
        // 構造のシグネチャ
        std::string sig;
        sig.reserve(256);
        for (u32 v = 0; v < nVar; ++v) { sig += VariantString(layer, v); sig += '|'; }
        sig += std::to_string(layer.maxVisible);
        const bool structChanged = !rt.gpu || rt.sig != sig;
        if (structChanged)
        {
            // モデルを読んでグループを組む
            rt.groups.clear();
            std::memset(rt.models, 0, sizeof(rt.models));
            rt.variantCount = 0;
            rt.lodStride = 1;
            rt.note.clear();
            f32 mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            bool ok = true;
            for (u32 v = 0; v < nVar && ok; ++v)
            {
                const std::vector<std::string> lods = ParseLodList(VariantString(layer, v));
                u32 got = 0;
                for (u32 l = 0; l < lods.size(); ++l)
                {
                    const CachedModel* cm = I.d.resources->GetOrLoadModel(ResolveModelPath(I.d.assetsDir, lods[l]), in.cmd);
                    if (!cm || cm->meshes.empty()) { if (l == 0) rt.note = "cannot load model: " + lods[l]; break; }
                    if (cm->skeleton) { rt.note = "skinned model is not supported: " + lods[l]; break; }
                    rt.models[v][l] = cm;
                    ++got;
                }
                if (got == 0)
                {
                    if (v == 0) ok = false;   // 種別 0 が無いと何も描けない
                    break;
                }
                rt.lodCount[v] = got;
                rt.variantCount = v + 1;
                rt.lodStride = std::max(rt.lodStride, got);
            }
            if (!ok || rt.variantCount == 0)
            {
                rt.gpu.reset();
                if (rt.note.empty()) rt.note = "no drawable model";
                st.note = rt.note; m_stats.push_back(st); continue;
            }
            for (u32 v = 0; v < rt.variantCount; ++v)
            {
                f32 hMax = 0.01f;
                for (u32 l = 0; l < rt.lodCount[v]; ++l)
                {
                    const CachedModel* cm = rt.models[v][l];
                    for (u32 s = 0; s < cm->meshes.size(); ++s)
                    {
                        Mesh* mesh = cm->meshes[s].get();
                        if (!mesh || mesh->GetIndexCount() == 0) continue;
                        LayerRt::Group g;
                        g.variant = v; g.lod = l; g.submesh = s; g.slot = v * rt.lodStride + l;
                        g.mesh = mesh;
                        g.mat = mesh->GetMaterial();
                        g.indexCount = mesh->GetIndexCount();
                        g.tris = g.indexCount / 3;
                        const AlphaParams ap = ResolveAlphaParams(g.mat, -1, -1.0f, 1.0f);
                        g.mask = (ap.mode != AlphaMode::Opaque);   // Blend も Mask 扱い（葉のカードを半透明で描くと深度が壊れる）
                        AlphaParams eff = ap;
                        if (eff.mode == AlphaMode::Blend) { eff.mode = AlphaMode::Mask; eff.cutoff = 0.5f; }
                        g.metallic = g.mat ? g.mat->defaultMetallic : 0.0f;
                        g.roughness = g.mat ? g.mat->defaultRoughness : 0.5f;
                        g.flags = 0;
                        if (g.mat && g.mat->normalMapTexture) g.flags |= 1u;
                        if (g.mat && g.mat->metalRoughnessTexture) g.flags |= 2u;
                        if (g.mat && g.mat->emissiveTexture) g.flags |= kPbrFlagEmissiveTex;
                        g.flags = PackAoFlags(g.flags, ResolveAoStrength(g.mat, -1.0f));
                        g.flags = PackAlphaTestFlags(g.flags, eff);
                        g.packedTint = PackTintWithOpacity(0x00FFFFFFu, 1.0f);
                        g.packedEmissive = PackEmissive(ResolveEmissiveParams(
                            g.mat ? g.mat->emissiveColor : DirectX::XMFLOAT3{0, 0, 0}, g.mat ? g.mat->emissiveIntensity : 0.0f,
                            DirectX::XMFLOAT3{-1, -1, -1}, -1.0f));
                        if (l == 0)
                        {
                            const auto a = mesh->GetAABBMin(), b = mesh->GetAABBMax();
                            mn[0] = std::min(mn[0], a.x); mn[1] = std::min(mn[1], a.y); mn[2] = std::min(mn[2], a.z);
                            mx[0] = std::max(mx[0], b.x); mx[1] = std::max(mx[1], b.y); mx[2] = std::max(mx[2], b.z);
                            hMax = std::max(hMax, b.y);
                        }
                        rt.groups.push_back(g);
                    }
                }
                rt.heightLocal[v] = hMax;
            }
            if (rt.groups.empty()) { rt.gpu.reset(); rt.note = "models have no meshes"; st.note = rt.note; m_stats.push_back(st); continue; }
            rt.boundCenterY = 0.5f * (mn[1] + mx[1]);
            f32 r2 = 0.0f;
            for (int c = 0; c < 8; ++c)
            {
                const f32 x = (c & 1) ? mx[0] : mn[0], y = (c & 2) ? mx[1] : mn[1], z = (c & 4) ? mx[2] : mn[2];
                const f32 dy = y - rt.boundCenterY;
                r2 = std::max(r2, x * x + dy * dy + z * z);
            }
            rt.boundRadius = std::sqrt(r2);
            rt.capMain = std::clamp(layer.maxVisible, 1024u, 4u * 1024u * 1024u);
            rt.capShadow = std::max(1024u, std::min(rt.capMain, 65536u));

            LayerGpuDesc gd;
            gd.instances = set.instances.data();
            gd.instanceCount = set.Count();
            gd.chunks = set.chunks.data();
            gd.chunkCount = static_cast<u32>(set.chunks.size());
            gd.numViews = 3;
            gd.lodStride = rt.lodStride;
            gd.slotsPerView = rt.variantCount * rt.lodStride;
            gd.capMain = rt.capMain;
            gd.capShadow = rt.capShadow;
            for (const auto& g : rt.groups) { gd.groupSlot.push_back(g.slot); gd.groupIndexCount.push_back(g.indexCount); }
            std::vector<ComPtr<ID3D12Resource>> keep;
            std::string cerr;
            if (rt.gpu) I.retired.push_back({in.frameId, std::move(rt.gpu), {}});
            rt.gpu = I.culler.CreateLayer(gd, in.cmd, keep, &cerr);
            if (!rt.gpu)
            {
                rt.note = "GPU resources failed: " + cerr;
                Logger::Warn("植生: {} のGPUリソースを作れません（{}）", rt.name, cerr);
                st.note = rt.note; m_stats.push_back(st); continue;
            }
            // アップロードのステージングは GPU が使い終わるまで保持
            I.retired.push_back({in.frameId, nullptr, std::move(keep)});
            rt.sig = sig;
            rt.instVersion = set.version;
            rt.instCount = set.Count();
            rt.instPtr = &set;
            Logger::Info("植生: レイヤー '{}' を構築（{} 個, {} チャンク, {} グループ, GPU {:.1f} MB）", rt.name, set.Count(),
                         gd.chunkCount, rt.groups.size(), static_cast<double>(rt.gpu->GpuBytes()) / (1024.0 * 1024.0));
        }
        else if (rt.instVersion != set.version || rt.instCount != set.Count() || rt.instPtr != &set)
        {
            // インスタンスだけが変わった（散布 / ブラシ）: 構造は同じなので丸ごと作り直す（頻繁でないので単純さを優先）
            LayerGpuDesc gd;
            gd.instances = set.instances.data();
            gd.instanceCount = set.Count();
            gd.chunks = set.chunks.data();
            gd.chunkCount = static_cast<u32>(set.chunks.size());
            gd.numViews = 3;
            gd.lodStride = rt.lodStride;
            gd.slotsPerView = rt.variantCount * rt.lodStride;
            gd.capMain = rt.capMain;
            gd.capShadow = rt.capShadow;
            for (const auto& g : rt.groups) { gd.groupSlot.push_back(g.slot); gd.groupIndexCount.push_back(g.indexCount); }
            std::vector<ComPtr<ID3D12Resource>> keep;
            std::string cerr;
            I.retired.push_back({in.frameId, std::move(rt.gpu), {}});
            rt.gpu = I.culler.CreateLayer(gd, in.cmd, keep, &cerr);
            if (!rt.gpu) { rt.note = "GPU resources failed: " + cerr; st.note = rt.note; m_stats.push_back(st); continue; }
            I.retired.push_back({in.frameId, nullptr, std::move(keep)});
            rt.instVersion = set.version;
            rt.instCount = set.Count();
            rt.instPtr = &set;
        }

        // 前回の統計の読み戻し
        st.instances = rt.instCount;
        st.chunks = static_cast<u32>(set.chunks.size());
        st.gpuBytes = rt.gpu->GpuBytes();
        st.lodStride = rt.lodStride;
        st.variants = rt.variantCount;
        st.ready = true;
        {
            LayerCounters lc;
            if (FoliageCuller::ReadCounters(*rt.gpu, slot, lc) && lc.valid)
            {
                const u32 S = lc.slotsPerView;
                for (u32 s = 0; s < S && s < kMaxVariants * kMaxLods; ++s)
                {
                    st.visible[s] = lc.counts[s];
                    st.visibleTotal += lc.counts[s];
                    if (lc.counts[s] > rt.capMain) st.overflow += lc.counts[s] - rt.capMain;
                }
                for (u32 v = 1; v < lc.numViews; ++v)
                    for (u32 s = 0; s < S; ++s)
                    {
                        const u32 c = lc.counts[v * S + s];
                        st.shadowVisible += std::min(c, rt.capShadow);
                        if (c > rt.capShadow) st.overflow += c - rt.capShadow;
                    }
                for (const auto& g : rt.groups)
                    if (g.slot < S) st.trianglesMain += static_cast<u64>(std::min(lc.counts[g.slot], rt.capMain)) * g.tris;
            }
        }
        m_stats.push_back(st);
        rt.drawable = true;
        ++m_drawable;
    }
    // 消えたレイヤーを退避
    for (auto it = m_layers.begin(); it != m_layers.end();)
    {
        if (!seen.count(it->first))
        {
            if (it->second->gpu) I.retired.push_back({in.frameId, std::move(it->second->gpu), {}});
            it = m_layers.erase(it);
        }
        else ++it;
    }
    return m_drawable > 0;
}

// ---------------------------------------------------------------------------
void FoliageSystem::Cull(ID3D12GraphicsCommandList* cmd, const ViewIn& v, const FrameIn& f)
{
    if (!m_ready || m_drawable == 0) return;
    Impl& I = *m_impl;
    I.Stamp(cmd, kScopeCull, false);
    if (v.hzb.resource) I.culler.SetHzbResource(v.hzb.resource, v.hzb.mips);
    else I.culler.SetHzbResource(nullptr, 0);

    for (auto& [id, rp] : m_layers)
    {
        LayerRt& rt = *rp;
        if (!rt.drawable || !rt.gpu) continue;
        CullParams base = MakeLayerCullParams(rt.params, rt.variantCount, rt.lodCount);
        base.boundCenterY = rt.boundCenterY;
        base.boundRadius = rt.boundRadius;
        base.layerScale = rt.layerScale;
        base.camPos[0] = v.camPos.x; base.camPos[1] = v.camPos.y; base.camPos[2] = v.camPos.z;

        std::vector<CullDispatch> ds;
        {
            CullDispatch d;
            d.view = 0;
            d.p = base;
            PlanesFromViewProj(v.viewProj, d.p.planes);
            d.hzb = rt.params.hzbCulling && v.hzb.resource != nullptr;
            d.prevVP = v.hzb.prevVP;
            d.hzbW = v.hzb.width; d.hzbH = v.hzb.height; d.hzbMips = v.hzb.mips;
            ds.push_back(d);
        }
        if (v.shadows && rt.params.castShadow)
        {
            for (u32 c = 0; c < 2; ++c)
            {
                if (!v.cascade[c].valid) continue;
                CullDispatch d;
                d.view = 1 + c;
                d.p = base;
                d.p.shadowPass = true;
                d.p.cullDist = std::max(1.0f, std::min(rt.params.shadowDistance, rt.params.cullDistance));
                d.p.thinStart = 1.0f - 1e-3f;   // 影は間引かない（密度フェードは主ビューだけ）
                PlanesFromViewProj(v.cascade[c].viewProj, d.p.planes);
                ds.push_back(d);
                rt.shadowCulled[c] = true;
            }
        }
        if (I.culler.Cull(cmd, *rt.gpu, rt.layerAffine, ds, f.frameSlot, f.appHeap)) rt.culledThisFrame = true;
    }
    I.Stamp(cmd, kScopeCull, true);
}

// ---------------------------------------------------------------------------
namespace
{
struct MainB0 { f32 vpT[16]; f32 layerWind[4]; f32 tint[4]; };
static_assert(sizeof(MainB0) == 24 * 4, "MainB0 は 24 DWORD");
struct DepthB0 { f32 vpT[16]; f32 layerWind[4]; };
static_assert(sizeof(DepthB0) == 20 * 4, "DepthB0 は 20 DWORD");
struct VelB0 { f32 vpJT[16]; f32 prevVpT[16]; f32 jitter[2]; f32 matPacked; f32 pad; f32 layerWind[4]; };
static_assert(sizeof(VelB0) == 40 * 4, "VelB0 は 40 DWORD（ルート定数の上限ぴったり）");

void StoreTranspose(const DirectX::XMFLOAT4X4& m, f32 out[16])
{
    DirectX::XMFLOAT4X4 t;
    DirectX::XMStoreFloat4x4(&t, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&m)));
    std::memcpy(out, &t, 64);
}
} // namespace

void FoliageSystem::DrawMain(ID3D12GraphicsCommandList* cmd, const DirectX::XMFLOAT4X4& viewProj, bool depthLEqual)
{
    if (!m_ready || m_drawable == 0) return;
    Impl& I = *m_impl;
    I.Stamp(cmd, kScopeMain, false);
    ID3D12PipelineState* lastPso = nullptr;
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    MainB0 b0{};
    StoreTranspose(viewProj, b0.vpT);
    u64 lastMat = ~0ull;
    for (auto& [id, rp] : m_layers)
    {
        LayerRt& rt = *rp;
        if (!rt.drawable || !rt.gpu || !rt.culledThisFrame) continue;
        for (const auto& g : rt.groups)
        {
            PipelineState* pso = I.psoMain[g.mask ? 1 : 0][depthLEqual ? 1 : 0].get();
            if (!pso || !pso->Get()) continue;
            if (pso->Get() != lastPso) { cmd->SetPipelineState(pso->Get()); lastPso = pso->Get(); }
            const bool wind = rt.params.windEnabled;
            b0.layerWind[0] = wind ? rt.params.windBend : 0.0f;
            b0.layerWind[1] = wind ? rt.params.windFlutter : 0.0f;
            b0.layerWind[2] = std::max(rt.params.windBendExp, 0.1f);
            b0.layerWind[3] = rt.heightLocal[g.variant];
            b0.tint[0] = rt.params.tint.x; b0.tint[1] = rt.params.tint.y; b0.tint[2] = rt.params.tint.z;
            b0.tint[3] = std::clamp(rt.params.aoStrength, 0.0f, 1.0f);
            cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPerObject, 24, &b0, 0);

            D3D12_GPU_DESCRIPTOR_HANDLE srv;
            if (g.mat && g.mat->srvBlockIndex != 0xFFFFFFFFu) srv = I.d.srvHeap->GetGpuHandle(g.mat->srvBlockIndex);
            else
            {
                Texture* tex = (g.mat && g.mat->albedoTexture) ? g.mat->albedoTexture : I.d.resources->GetDefaultWhiteTexture();
                srv = I.d.srvHeap->GetGpuHandle(tex->GetSrvIndex());
            }
            if (srv.ptr != lastMat) { cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotSRVTable, srv); lastMat = srv.ptr; }
            struct { f32 metallic, roughness; u32 flags, packedTint; f32 uvsx, uvsy, uvox, uvoy; u32 packedEmissive; } pbr;
            pbr.metallic = g.metallic; pbr.roughness = g.roughness; pbr.flags = g.flags; pbr.packedTint = g.packedTint;
            pbr.uvsx = 1.0f; pbr.uvsy = 1.0f; pbr.uvox = 0.0f; pbr.uvoy = 0.0f; pbr.packedEmissive = g.packedEmissive;
            cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 9, &pbr, 0);

            const D3D12_VERTEX_BUFFER_VIEW vb0 = g.mesh->GetVertexBuffer().GetView();
            cmd->IASetVertexBuffers(0, 1, &vb0);
            const D3D12_INDEX_BUFFER_VIEW ib = g.mesh->GetIndexBuffer().GetView();
            cmd->IASetIndexBuffer(&ib);
            const D3D12_VERTEX_BUFFER_VIEW vb1 = rt.gpu->VisibleView(0, g.slot);
            cmd->IASetVertexBuffers(1, 1, &vb1);
            const u32 gi = static_cast<u32>(&g - rt.groups.data());
            cmd->ExecuteIndirect(I.cmdSig.Get(), 1, rt.gpu->ArgsResource(), rt.gpu->ArgsOffset(0, gi), nullptr, 0);
        }
    }
    I.Stamp(cmd, kScopeMain, true);
    // タイマーの解決（このフレームの全 scope）。主ビューの本体描画が 1 フレームの最後の植生コマンド。
    if (I.qHeap)
    {
        // 今フレームで使わなかった scope（影を描かない等）にも空のスタンプを打っておく（未書き込みのクエリを解決しないため）
        for (u32 sc = 0; sc < kScopeCount; ++sc)
            if (!(I.tsWritten[I.curSlot] & (1u << sc))) { I.Stamp(cmd, static_cast<Scope>(sc), false); I.Stamp(cmd, static_cast<Scope>(sc), true); }
        cmd->ResolveQueryData(I.qHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, I.curSlot * kTsPerFrame, kTsPerFrame, I.qReadback.Get(),
                              static_cast<u64>(I.curSlot) * kTsPerFrame * 8);
    }
}

void FoliageSystem::DrawDepth(ID3D12GraphicsCommandList* cmd, const DirectX::XMFLOAT4X4& viewProjJ, bool velocity,
                              const DirectX::XMFLOAT4X4& prevVP, const DirectX::XMFLOAT2& jitter)
{
    if (!m_ready || m_drawable == 0) return;
    Impl& I = *m_impl;
    I.Stamp(cmd, kScopeDepth, false);
    ID3D12PipelineState* lastPso = nullptr;
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootConstantBufferView(RootSignature::kSlotPerFrame, I.windCb[I.curSlot]->GetGPUVirtualAddress());
    DepthB0 db{};
    VelB0 vb{};
    if (velocity)
    {
        StoreTranspose(viewProjJ, vb.vpJT);
        StoreTranspose(prevVP, vb.prevVpT);
        vb.jitter[0] = jitter.x; vb.jitter[1] = jitter.y;
    }
    else StoreTranspose(viewProjJ, db.vpT);
    u64 lastMat = ~0ull;
    for (auto& [id, rp] : m_layers)
    {
        LayerRt& rt = *rp;
        if (!rt.drawable || !rt.gpu || !rt.culledThisFrame) continue;
        for (const auto& g : rt.groups)
        {
            PipelineState* pso = velocity ? I.psoVel[g.mask ? 1 : 0].get() : I.psoDepth[g.mask ? 1 : 0].get();
            if (!pso || !pso->Get()) continue;
            if (pso->Get() != lastPso) { cmd->SetPipelineState(pso->Get()); lastPso = pso->Get(); }
            const bool wind = rt.params.windEnabled;
            const f32 lw[4] = {wind ? rt.params.windBend : 0.0f, wind ? rt.params.windFlutter : 0.0f,
                               std::max(rt.params.windBendExp, 0.1f), rt.heightLocal[g.variant]};
            if (velocity)
            {
                std::memcpy(vb.layerWind, lw, sizeof(lw));
                // roughness / metallic を 1 float に詰める（VelocityPrepass の packMaterial と同じ）
                const f32 r = std::round(std::clamp(std::max(g.roughness, 0.04f), 0.0f, 1.0f) * 255.0f);
                const f32 m = std::round(std::clamp(g.metallic, 0.0f, 1.0f) * 255.0f);
                vb.matPacked = r + m * 256.0f;
                cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPerObject, 40, &vb, 0);
            }
            else
            {
                std::memcpy(db.layerWind, lw, sizeof(lw));
                cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPerObject, 20, &db, 0);
            }
            if (g.mask)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE srv;
                if (g.mat && g.mat->srvBlockIndex != 0xFFFFFFFFu) srv = I.d.srvHeap->GetGpuHandle(g.mat->srvBlockIndex);
                else
                {
                    Texture* tex = (g.mat && g.mat->albedoTexture) ? g.mat->albedoTexture : I.d.resources->GetDefaultWhiteTexture();
                    srv = I.d.srvHeap->GetGpuHandle(tex->GetSrvIndex());
                }
                if (srv.ptr != lastMat) { cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotSRVTable, srv); lastMat = srv.ptr; }
                struct { f32 metallic, roughness; u32 flags, packedTint; f32 uvsx, uvsy, uvox, uvoy; u32 packedEmissive; } pbr;
                pbr.metallic = 0.0f; pbr.roughness = 0.5f; pbr.flags = g.flags; pbr.packedTint = g.packedTint;
                pbr.uvsx = 1.0f; pbr.uvsy = 1.0f; pbr.uvox = 0.0f; pbr.uvoy = 0.0f; pbr.packedEmissive = 0u;
                cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 9, &pbr, 0);
            }
            const D3D12_VERTEX_BUFFER_VIEW vb0 = g.mesh->GetVertexBuffer().GetView();
            cmd->IASetVertexBuffers(0, 1, &vb0);
            const D3D12_INDEX_BUFFER_VIEW ib = g.mesh->GetIndexBuffer().GetView();
            cmd->IASetIndexBuffer(&ib);
            const D3D12_VERTEX_BUFFER_VIEW vb1 = rt.gpu->VisibleView(0, g.slot);
            cmd->IASetVertexBuffers(1, 1, &vb1);
            const u32 gi = static_cast<u32>(&g - rt.groups.data());
            cmd->ExecuteIndirect(I.cmdSig.Get(), 1, rt.gpu->ArgsResource(), rt.gpu->ArgsOffset(0, gi), nullptr, 0);
        }
    }
    I.Stamp(cmd, kScopeDepth, true);
}

void FoliageSystem::DrawShadow(ID3D12GraphicsCommandList* cmd, u32 cascade, const DirectX::XMFLOAT4X4& viewProj)
{
    if (!m_ready || m_drawable == 0 || cascade >= 2) return;
    Impl& I = *m_impl;
    const Scope sc = cascade == 0 ? kScopeShadow0 : kScopeShadow1;
    I.Stamp(cmd, sc, false);
    ID3D12PipelineState* lastPso = nullptr;
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootConstantBufferView(RootSignature::kSlotPerFrame, I.windCb[I.curSlot]->GetGPUVirtualAddress());
    DepthB0 db{};
    StoreTranspose(viewProj, db.vpT);
    u64 lastMat = ~0ull;
    for (auto& [id, rp] : m_layers)
    {
        LayerRt& rt = *rp;
        if (!rt.drawable || !rt.gpu || !rt.culledThisFrame || !rt.shadowCulled[cascade]) continue;
        for (const auto& g : rt.groups)
        {
            if (g.lod > static_cast<u32>(std::max(rt.params.shadowMaxLod, 0))) continue;
            PipelineState* pso = I.psoShadow[g.mask ? 1 : 0].get();
            if (!pso || !pso->Get()) continue;
            if (pso->Get() != lastPso) { cmd->SetPipelineState(pso->Get()); lastPso = pso->Get(); }
            const bool wind = rt.params.windEnabled;
            db.layerWind[0] = wind ? rt.params.windBend : 0.0f;
            db.layerWind[1] = wind ? rt.params.windFlutter : 0.0f;
            db.layerWind[2] = std::max(rt.params.windBendExp, 0.1f);
            db.layerWind[3] = rt.heightLocal[g.variant];
            cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPerObject, 20, &db, 0);
            if (g.mask)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE srv;
                if (g.mat && g.mat->srvBlockIndex != 0xFFFFFFFFu) srv = I.d.srvHeap->GetGpuHandle(g.mat->srvBlockIndex);
                else
                {
                    Texture* tex = (g.mat && g.mat->albedoTexture) ? g.mat->albedoTexture : I.d.resources->GetDefaultWhiteTexture();
                    srv = I.d.srvHeap->GetGpuHandle(tex->GetSrvIndex());
                }
                if (srv.ptr != lastMat) { cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotSRVTable, srv); lastMat = srv.ptr; }
                struct { f32 metallic, roughness; u32 flags, packedTint; f32 uvsx, uvsy, uvox, uvoy; u32 packedEmissive; } pbr;
                pbr.metallic = 0.0f; pbr.roughness = 0.5f; pbr.flags = g.flags; pbr.packedTint = g.packedTint;
                pbr.uvsx = 1.0f; pbr.uvsy = 1.0f; pbr.uvox = 0.0f; pbr.uvoy = 0.0f; pbr.packedEmissive = 0u;
                cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 9, &pbr, 0);
            }
            const D3D12_VERTEX_BUFFER_VIEW vb0 = g.mesh->GetVertexBuffer().GetView();
            cmd->IASetVertexBuffers(0, 1, &vb0);
            const D3D12_INDEX_BUFFER_VIEW ib = g.mesh->GetIndexBuffer().GetView();
            cmd->IASetIndexBuffer(&ib);
            const D3D12_VERTEX_BUFFER_VIEW vb1 = rt.gpu->VisibleView(1 + cascade, g.slot);
            cmd->IASetVertexBuffers(1, 1, &vb1);
            const u32 gi = static_cast<u32>(&g - rt.groups.data());
            cmd->ExecuteIndirect(I.cmdSig.Get(), 1, rt.gpu->ArgsResource(), rt.gpu->ArgsOffset(1 + cascade, gi), nullptr, 0);
        }
    }
    I.Stamp(cmd, sc, true);
}

// ---------------------------------------------------------------------------
nlohmann::json FoliageSystem::StatsJson() const
{
    using nlohmann::json;
    json j;
    j["ready"] = m_ready;
    j["layers"] = json::array();
    auto r3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    u64 bytes = 0;
    for (const auto& s : m_stats)
    {
        json l;
        l["entity"] = s.entity;
        l["name"] = s.name;
        l["ready"] = s.ready;
        if (!s.note.empty()) l["note"] = s.note;
        l["instances"] = s.instances;
        l["chunks"] = s.chunks;
        l["gpuMB"] = r3(static_cast<double>(s.gpuBytes) / (1024.0 * 1024.0));
        bytes += s.gpuBytes;
        if (s.ready)
        {
            l["variants"] = s.variants;
            l["lodStride"] = s.lodStride;
            json vis = json::array();
            for (u32 v = 0; v < s.variants; ++v)
            {
                json row = json::array();
                for (u32 lod = 0; lod < s.lodStride; ++lod) row.push_back(s.visible[v * s.lodStride + lod]);
                vis.push_back(row);
            }
            l["visiblePerVariantLod"] = vis;   // 主ビュー。行 = variant、列 = LOD（容量超過分を含む生の数。1〜2 フレーム遅れ）
            l["visibleTotal"] = s.visibleTotal;
            l["shadowVisible"] = s.shadowVisible;
            l["overflow"] = s.overflow;
            l["trianglesMain"] = s.trianglesMain;
        }
        j["layers"].push_back(l);
    }
    j["gpuMB"] = r3(static_cast<double>(bytes) / (1024.0 * 1024.0));
    j["gpuMs"] = {{"cull", r3(m_gpuMs.cull)}, {"shadowDraw", r3(m_gpuMs.shadowDraw)}, {"depthDraw", r3(m_gpuMs.depthDraw)},
                  {"mainDraw", r3(m_gpuMs.mainDraw)}, {"total", r3(m_gpuMs.Total())}};
    j["drawableLayers"] = m_drawable;
    return j;
}

} // namespace dx12e::foliage
