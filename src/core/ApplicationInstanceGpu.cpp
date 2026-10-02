// ===========================================================================
// GPU 駆動のインスタンス群（設計 docs/SCENE_FORMAT_DESIGN.md §4.2 = 4-3）: Application 側の糊。
// ---------------------------------------------------------------------------
// 本体は renderer/instancegpu/InstanceGpuCuller（compute: 永続バッファ + チャンク → インスタンスの GPU カリング + LOD + 間接引数）。
//   ・BuildDrawList が「GPU 経路に乗る群」を frame へ積む（DrawItem へは展開しない）。条件は GpuInstTakeGroup。
//   ・描画パス（本体 / 深度プリパス / 速度 / CSM / スポット / ポイント）は、そのパスの視錐台で群ごとに Cull → ExecuteIndirect する。
//     描画シェーダは既存のインスタンシング版（ForwardInstanced / ShadowPassInstanced / VelocityPrepassInstanced）をそのまま使う
//     ＝ CPU 経路のバッチと頂点計算がビット同一。ルートシグネチャの増分は 0。
//   ・★しきい値 0（無効）/ 条件を満たさない群 / 知覚・パストレーサーの要求フレームは従来の DrawItem 展開（絵は不変）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "ecs/InstanceGroup.h"
#include "renderer/ViewDesc.h"
#include "editor/ScenePick.h"   // SetPickExtraSource（GPU 経路の群のピッキング）
#include "core/PathTracerHost.h"   // PtHost::Phase（スナップショット要求フレームは従来の展開にする）
#include "core/mcp/McpPerceive.h"   // McpPerceiveJob::Phase（知覚の要求フレームも同じ）

namespace dx12e
{
using namespace appdetail;

namespace
{
// RenderDepthOnlyScene の VelocityInstObjCB / packMaterial と同じもの（shaders/velocity/VelocityPrepassInstanced.hlsl の b0）。
struct VelocityInstObjCB { DirectX::XMMATRIX vpJ; DirectX::XMMATRIX prevVp; DirectX::XMFLOAT2 jitter; float matPacked; float pad; };
static_assert(sizeof(VelocityInstObjCB) == 36 * sizeof(float), "VelocityPrepassInstanced.hlsl の cbuffer と一致させること");

float PackMaterialForVelocity(float roughness, float metallic)
{
    const float r = std::round(std::clamp(roughness, 0.0f, 1.0f) * 255.0f);
    const float m = std::round(std::clamp(metallic, 0.0f, 1.0f) * 255.0f);
    return r + m * 256.0f;
}

u64 Mix64(u64 h, u64 v) { return (h ^ v) * 1099511628211ull; }
bool SameBounds(const instgpu::LocalBounds& a, const instgpu::LocalBounds& b)
{
    return a.valid == b.valid && a.mn.x == b.mn.x && a.mn.y == b.mn.y && a.mn.z == b.mn.z && a.mx.x == b.mx.x && a.mx.y == b.mx.y && a.mx.z == b.mx.z;
}
// 群のローカル境界（全サブメッシュの AABB の合成。BuildDrawList の lmn / lmx）
instgpu::LocalBounds GroupLocalBounds(const MeshRenderer& r)
{
    using namespace DirectX;
    XMVECTOR lmn = XMVectorReplicate(FLT_MAX), lmx = XMVectorReplicate(-FLT_MAX);
    instgpu::LocalBounds lb;
    for (const Mesh* m : r.meshes)
    {
        if (!m) continue;
        const XMFLOAT3 a = m->GetAABBMin(), b = m->GetAABBMax();
        lmn = XMVectorMin(lmn, XMLoadFloat3(&a)); lmx = XMVectorMax(lmx, XMLoadFloat3(&b));
        lb.valid = true;
    }
    XMStoreFloat3(&lb.mn, lmn); XMStoreFloat3(&lb.mx, lmx);
    return lb;
}

// ピッキング / 輪郭用の球の表（群の内容が変わらない間キャッシュ）
GpuInstPickCache& EnsurePickCache(GpuInstState& S, u32 key, const instgroup::InstanceSet& set, const std::vector<DirectX::XMFLOAT4X4>& worlds,
                                  const instgpu::LocalBounds& lb)
{
    GpuInstPickCache& pc = S.pick[key];
    if (pc.setId != set.id || pc.worldKey != set.worldKey || pc.recs.size() != worlds.size() || !SameBounds(pc.lb, lb))
    {
        pc.recs.resize(worlds.size());
        for (size_t i = 0; i < worlds.size(); ++i) pc.recs[i] = instgpu::MakeRecord(worlds[i], lb);
        pc.setId = set.id; pc.worldKey = set.worldKey; pc.lb = lb;
    }
    return pc;
}
} // namespace

// ---------------------------------------------------------------------------
// 利用可否 / 遅延初期化
// ---------------------------------------------------------------------------
bool Application::GpuInstUsable() const
{
    if (m_gpuInst.threshold == 0 || m_gpuInst.unavailable || m_gpuInst.forceCpuThisFrame) return false;
    // 描画に使うインスタンシング PSO が全部あること（無いと、DrawItem へ展開しない群が描かれなくなる）
    if (!m_instancingEnabled || !m_pipelineStateInst || !m_pipelineStateInstLEqual) return false;
    if (!m_shadowPipelineStateInst || !m_depthPrepassPSOInst) return false;
    if (m_velocityPSO && !m_velocityPSOInst) return false;
    return m_graphicsDevice != nullptr;
}

bool Application::GpuInstEnsureSystem()
{
    if (m_gpuInst.culler) return m_gpuInst.culler->IsReady() && m_gpuInst.cmdSig;
    if (m_gpuInst.unavailable || !m_graphicsDevice) return false;
    auto c = std::make_unique<instgpu::InstanceGpuCuller>();
    std::string err;
    if (!c->Initialize(m_graphicsDevice->GetDevice(), PathResolver::ShaderDirW(), FrameResources::kFrameCount, &err))
    {
        Logger::Warn("GPU 駆動のインスタンス群: 初期化に失敗したので無効にする（{}）。大きな群は従来の展開で描く", err);
        m_gpuInst.unavailable = true;
        return false;
    }
    // コマンドシグネチャ（DrawIndexedInstanced 引数だけ。ルート引数は差し替えない＝ルートシグネチャ無し）
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC cs{};
    cs.ByteStride = 20;
    cs.NumArgumentDescs = 1;
    cs.pArgumentDescs = &arg;
    if (FAILED(m_graphicsDevice->GetDevice()->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&m_gpuInst.cmdSig))))
    {
        Logger::Warn("GPU 駆動のインスタンス群: コマンドシグネチャの作成に失敗したので無効にする");
        m_gpuInst.unavailable = true;
        return false;
    }
    m_gpuInst.culler = std::move(c);
    SetPickExtraSource([this](const PickInstanceVisitor& fn) { GpuInstVisitPick(fn); });   // ピッキング / ホバー / MCP のレイキャストが群のインスタンスも拾う
    Logger::Info("GPU 駆動のインスタンス群: 初期化（しきい値 {} 個以上の不透明な群を GPU カリング + ExecuteIndirect）", m_gpuInst.threshold);
    return true;
}

void Application::ShutdownGpuInst()
{
    m_gpuInst.frame.clear();
    SetPickExtraSource(nullptr);
    m_gpuInst.pick.clear();
    m_gpuInst.groups.clear();   // Group のデストラクタが DeferredRelease へ渡す（呼び出し側が GPU を止めた後なので即時でもよい）
    m_gpuInst.cmdSig.Reset();
    if (m_gpuInst.culler) m_gpuInst.culler->Shutdown();
    m_gpuInst.culler.reset();
}

// BuildDrawList の先頭で呼ぶ（フレームの状態をリセット）。
void Application::GpuInstBeginFrame(const DirectX::XMFLOAT3& camPos)
{
    GpuInstState& S = m_gpuInst;
    S.lastStats = S.stats;
    S.stats = GpuInstStats{};
    S.frame.clear();
    S.camPos = camPos;
    ++S.frameCounter;
    // 知覚 / パストレーサーの要求フレームは、DrawItem を全部要る（ID パス / スナップショット）ので従来の展開にする。
    S.forceCpuThisFrame = (m_mcpPerceive && m_mcpPerceive->phase == McpPerceiveJob::Phase::Pending) || (m_ptHost && m_ptHost->phase == PtHost::Phase::Requested);
    // 古い群の状態を掃除（30 フレーム使われなかったもの）。
    if ((S.frameCounter & 31u) == 0u && !S.groups.empty())
        for (auto it = S.groups.begin(); it != S.groups.end();)
            it = (S.frameCounter - it->second.lastFrame > 30u) ? S.groups.erase(it) : std::next(it);
}

// ---------------------------------------------------------------------------
// BuildDrawList から: この群を GPU 経路に乗せるか。probe = インスタンス 0 から作った DrawItem（emitItem の分類結果）。
// true を返したら呼び出し側は DrawItem を積まない。false なら従来どおり展開する。
// ---------------------------------------------------------------------------
bool Application::GpuInstTakeGroup(entt::entity e, const MeshRenderer& r, const DrawItem& probe,
                                   const std::shared_ptr<instgroup::InstanceSet>& set, const DirectX::XMFLOAT4X4& groupWorld,
                                   bool moved, const DirectX::XMFLOAT4X4& prevGroupWorld, u64 guid)
{
    GpuInstState& S = m_gpuInst;
    auto why = [&](const char* msg)
    {
        ++S.stats.cpuFallbackGroups;
        if (S.stats.reasons.size() < 600)
        {
            S.stats.reasons += msg;
            S.stats.reasons += "; ";
        }
        return false;
    };
    if (!GpuInstEnsureSystem()) return why("GPU 経路が使えない");
    if (probe.sortKey != 0u || probe.alphaClass != 0u) return why("半透明 / アルファテスト / カスタムシェーダ");
    if (probe.skin || probe.hasNodeAnim || probe.vg) return why("スキン / ノードアニメ / 仮想ジオメトリ");
    if (r.animFrames != 0 || r.uvScrollU != 0.0f || r.uvScrollV != 0.0f) return why("UV アニメ");
    if (r.meshes.empty() || r.meshes.size() > 32) return why("サブメッシュ数");
    for (u32 mi = 0; mi < static_cast<u32>(r.meshes.size()); ++mi)
    {
        const Mesh* m = r.meshes[mi];
        if (!m || m->GetIndexCount() == 0) return why("メッシュが未読み込み");
        if (r.HasMaterialAsset(mi) || r.HasAnyTextureOverride(mi)) return why("材質アセット / テクスチャ上書き");
    }
    if (const auto* tc = m_scene->GetRegistry().try_get<Terrain>(e))
        if (!tc->layerSetPath.empty()) return why("地形");

    GpuInstFrameGroup fg;
    fg.e = e;
    fg.renderer = &r;
    fg.set = set;
    fg.count = set->Count();
    fg.groupWorld = groupWorld;
    fg.moved = moved;
    fg.prevGroupWorld = prevGroupWorld;
    fg.guid = guid;
    S.frame.push_back(std::move(fg));
    ++S.stats.groups;
    S.stats.instances += set->Count();
    return true;
}

// ---------------------------------------------------------------------------
// 群の GPU リソースを今の内容にそろえる（Group の作成 / 作り直し・インスタンス表のアップロード）。
// フレームで 1 回（最初に使われたビューの中で）。失敗は nullptr。
// ---------------------------------------------------------------------------
GpuInstGroupRt* Application::GpuInstPrepare(ID3D12GraphicsCommandList* cmd, const GpuInstFrameGroup& fg)
{
    using namespace DirectX;
    GpuInstState& S = m_gpuInst;
    GpuInstGroupRt& rt = S.groups[static_cast<u32>(entt::to_integral(fg.e))];
    if (rt.lastFrame == S.frameCounter) return (rt.gpu && !rt.failed && rt.gpu->HasData()) ? &rt : nullptr;   // 今フレームは準備済み
    rt.lastFrame = S.frameCounter;
    const MeshRenderer& r = *fg.renderer;
    const u32 nSub = static_cast<u32>(r.meshes.size());

    // ---- メッシュ構成（ポインタ + インデックス数）。変われば Group を作り直す ----
    u64 sig = 1469598103934665603ull;
    sig = Mix64(sig, nSub);
    instgpu::LocalBounds lb;
    u32 maxLods = 1;
    std::vector<u32> indexCounts(static_cast<size_t>(nSub) * instgpu::kLists);
    XMVECTOR lmn = XMVectorReplicate(FLT_MAX), lmx = XMVectorReplicate(-FLT_MAX);
    for (u32 s = 0; s < nSub; ++s)
    {
        const Mesh* m = r.meshes[s];
        sig = Mix64(sig, reinterpret_cast<u64>(m));
        maxLods = (std::max)(maxLods, m->GetLodCount());
        for (u32 l = 0; l < instgpu::kLists; ++l)
        {
            indexCounts[static_cast<size_t>(s) * instgpu::kLists + l] = m->GetIndexCountLod(l);
            sig = Mix64(sig, indexCounts[static_cast<size_t>(s) * instgpu::kLists + l]);
        }
        const XMFLOAT3 a = m->GetAABBMin(), b = m->GetAABBMax();
        lmn = XMVectorMin(lmn, XMLoadFloat3(&a));
        lmx = XMVectorMax(lmx, XMLoadFloat3(&b));
        lb.valid = true;
    }
    XMStoreFloat3(&lb.mn, lmn);
    XMStoreFloat3(&lb.mx, lmx);
    u64 boundsHash = 1469598103934665603ull;
    if (lb.valid) { boundsHash = Mix64(boundsHash, *reinterpret_cast<const u32*>(&lb.mn.x)); boundsHash = Mix64(boundsHash, *reinterpret_cast<const u32*>(&lb.mn.y));
                    boundsHash = Mix64(boundsHash, *reinterpret_cast<const u32*>(&lb.mn.z)); boundsHash = Mix64(boundsHash, *reinterpret_cast<const u32*>(&lb.mx.x));
                    boundsHash = Mix64(boundsHash, *reinterpret_cast<const u32*>(&lb.mx.y)); boundsHash = Mix64(boundsHash, *reinterpret_cast<const u32*>(&lb.mx.z)); }

    if (!rt.gpu || rt.meshSig != sig)
    {
        std::string err;
        rt.gpu.reset();
        rt.gpu = S.culler->CreateGroup(nSub, indexCounts, &err);
        if (!rt.gpu) { Logger::Warn("GPU 駆動のインスタンス群: 群の作成に失敗（{}）", err); rt.failed = true; return nullptr; }
        rt.meshSig = sig;
        rt.submeshCount = nSub;
        rt.uploadedSetId = 0; rt.uploadedWorldKey = 0; rt.uploadedCount = 0;
        rt.failed = false;
    }
    rt.maxList = (std::min)(maxLods, instgpu::kLists) - 1u;
    rt.bounds = lb;

    // ---- インスタンス表 ----
    const instgroup::InstanceSet& set = *fg.set;
    auto& reg = m_scene->GetRegistry();
    const std::vector<XMFLOAT4X4>& worlds = instgroup::WorldMatrices(reg, fg.e, set);   // キャッシュ（群の行列が変わらない間は再計算しない）
    const bool stale = rt.uploadedSetId != set.id || rt.uploadedWorldKey != set.worldKey || rt.uploadedCount != fg.count
                    || rt.uploadedBoundsHash != boundsHash || !rt.gpu->HasData();
    if (stale)
    {
        static thread_local std::vector<instgpu::InstanceRecord> recs;
        static thread_local std::vector<instgpu::PrevRecord> prevs;
        recs.resize(fg.count);
        for (u32 i = 0; i < fg.count; ++i) recs[i] = instgpu::MakeRecord(worlds[i], lb);
        const bool needPrev = fg.moved && m_trackPrevWorld;
        if (needPrev)
        {
            // 前フレームの群ワールドから local * prevGroup（DrawItem 経路と同じ式）。動いた直後の 1 フレームだけ。
            prevs.resize(fg.count);
            const XMMATRIX pgw = XMLoadFloat4x4(&fg.prevGroupWorld);
            for (u32 i = 0; i < fg.count; ++i)
            {
                XMFLOAT4X4 pw;
                XMStoreFloat4x4(&pw, instgroup::LocalMatrix(set.items[i]) * pgw);
                prevs[i] = instgpu::MakePrevRecord(pw);
            }
        }
        std::string err;
        if (!S.culler->Upload(*rt.gpu, cmd, recs.data(), fg.count, needPrev ? prevs.data() : nullptr, nullptr, &err))
        {
            Logger::Warn("GPU 駆動のインスタンス群: アップロードに失敗（{}）", err);
            rt.failed = true;
            return nullptr;
        }
        rt.failed = false;
        rt.uploadedSetId = set.id;
        rt.uploadedWorldKey = set.worldKey;
        rt.uploadedCount = fg.count;
        rt.uploadedBoundsHash = boundsHash;
        ++S.stats.uploads;
    }
    else if (rt.gpu->HasPrev() && !fg.moved)
    {
        S.culler->ClearPrev(*rt.gpu);   // 動いた翌フレーム: 前フレームは「現在と同じ」（速度 0）へ戻す
    }
    S.stats.gpuBytes += rt.gpu->GpuBytes();
    return &rt;
}

// ---------------------------------------------------------------------------
// 1 ビューのカリング（同じビューが続くなら省く）。戻り値 = 描ける状態か。
// ---------------------------------------------------------------------------
bool Application::GpuInstCull(ID3D12GraphicsCommandList* cmd, GpuInstGroupRt& rt, const DirectX::XMMATRIX& viewProj, u32 lodBias,
                              f32 texelWorld, const float color[4], bool writePrev, u32 frameIndex, bool mainView)
{
    using namespace DirectX;
    GpuInstState& S = m_gpuInst;
    instgpu::CullView cv;
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, viewProj);
    instgpu::PlanesFromViewProj(vp, cv.view.planes);
    cv.view.camPos[0] = S.camPos.x; cv.view.camPos[1] = S.camPos.y; cv.view.camPos[2] = S.camPos.z;
    cv.view.lodBias = lodBias;
    cv.view.texelWorld = texelWorld;
    cv.view.maxList = rt.maxList;
    std::memcpy(cv.color, color, sizeof(cv.color));
    cv.writePrev = writePrev;
    if (!instgpu::InstanceGpuCuller::NeedsCull(*rt.gpu, cv)) { ++S.stats.culled_skipped; return true; }
    if (!S.culler->Cull(cmd, *rt.gpu, cv, frameIndex % FrameResources::kFrameCount, mainView))
    {
        static bool warned = false;
        if (!warned) { warned = true; Logger::Warn("GPU 駆動のインスタンス群: カリングの記録に失敗（1 フレームのビュー数が多すぎる？）"); }
        return false;
    }
    ++S.stats.culls;
    return true;
}

// 群の描画に使う ExecuteIndirect の並び（サブメッシュ × LOD）。VB / IB は呼び出し側が PSO と定数を張った後に呼ぶ。
void Application::GpuInstExecute(ID3D12GraphicsCommandList* cmd, const GpuInstFrameGroup& fg, GpuInstGroupRt& rt, u32 sub, bool prevStream)
{
    const MeshRenderer& r = *fg.renderer;
    const Mesh* mesh = r.meshes[sub];
    const D3D12_VERTEX_BUFFER_VIEW mv = mesh->GetVertexBuffer().GetView();
    cmd->IASetVertexBuffers(0, 1, &mv);
    const D3D12_VERTEX_BUFFER_VIEW iv = rt.gpu->VisibleView();
    cmd->IASetVertexBuffers(1, 1, &iv);
    if (prevStream)
    {
        const D3D12_VERTEX_BUFFER_VIEW pv = rt.gpu->VisiblePrevView();
        cmd->IASetVertexBuffers(2, 1, &pv);
    }
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (u32 l = 0; l <= rt.maxList; ++l)
    {
        const D3D12_INDEX_BUFFER_VIEW ib = mesh->GetIndexBufferLod(l).GetView();
        cmd->IASetIndexBuffer(&ib);
        cmd->ExecuteIndirect(m_gpuInst.cmdSig.Get(), 1, rt.gpu->ArgsResource(), rt.gpu->ArgsOffset(sub, l), nullptr, 0);
        ++m_gpuInst.stats.draws;
        ++m_statDraws;
        ++m_passBucket->draws;
    }
}

// ---------------------------------------------------------------------------
// 深度系パス（CSM / スポット / ポイント / 深度プリパス / 速度）。RenderDepthOnlyScene の最後に呼ぶ。
// instPSO = そのパスのインスタンシング版 PSO（影: m_shadowPipelineStateInst / プリパス: m_depthPrepassPSOInst / 速度: m_velocityPSOInst）。
// ---------------------------------------------------------------------------
void Application::GpuInstDrawDepth(const DirectX::XMMATRIX& viewProj, PipelineState* instPSO, u32 frameIndex, u32 lodBias, f32 texelWorld,
                                   bool velocityMode, const DirectX::XMMATRIX& prevViewProj, const DirectX::XMFLOAT2& jitterNdc)
{
    using namespace DirectX;
    GpuInstState& S = m_gpuInst;
    if (S.frame.empty() || !instPSO || !S.culler) return;
    ID3D12GraphicsCommandList* cmd = m_commandList->GetNative();
    const XMMATRIX passVpT = XMMatrixTranspose(viewProj);
    const float white[4] = {1, 1, 1, 1};
    bool psoSet = false;
    for (const GpuInstFrameGroup& fg : S.frame)
    {
        GpuInstGroupRt* rt = GpuInstPrepare(cmd, fg);
        if (!rt) continue;
        if (!GpuInstCull(cmd, *rt, viewProj, lodBias, texelWorld, white, velocityMode, frameIndex, /*mainView（読み戻し）*/ lodBias == 0 && texelWorld == 0.0f)) continue;
        // 起こり得る compute の PSO を捨てて、深度用のインスタンシング PSO を張り直す（グラフィックスのルートシグネチャ / ヒープは compute と独立）
        cmd->SetPipelineState(instPSO->Get());
        psoSet = true;
        const MeshRenderer& r = *fg.renderer;
        if (!velocityMode) m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 16, &passVpT);
        for (u32 s = 0; s < static_cast<u32>(r.meshes.size()); ++s)
        {
            if (velocityMode)
            {
                const Material* mat = r.meshes[s]->GetMaterial();
                float metal = (r.overrideMetallic >= 0.0f) ? r.overrideMetallic : (mat ? mat->defaultMetallic : 0.0f);
                float rough = (r.overrideRoughness >= 0.0f) ? r.overrideRoughness : (mat ? mat->defaultRoughness : 0.5f);
                rough = (std::max)(rough, 0.04f);   // RenderDepthOnlyScene の resolvePbr と同じ（Forward.hlsl:179 と同じ下限）
                VelocityInstObjCB vc;
                vc.vpJ       = passVpT;                          // transpose(jittered VP)
                vc.prevVp    = XMMatrixTranspose(prevViewProj);  // transpose(prev non-jittered VP)
                vc.jitter    = jitterNdc;
                vc.matPacked = PackMaterialForVelocity(rough, metal);
                vc.pad       = 0.0f;
                m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 36, &vc);
            }
            GpuInstExecute(cmd, fg, *rt, s, velocityMode);
        }
    }
    (void)psoSet;
}

// ---------------------------------------------------------------------------
// 本体（Forward+ の不透明パス）。RenderSceneMeshes の不透明ループの後に呼ぶ。
// ---------------------------------------------------------------------------
void Application::GpuInstDrawMain(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const DirectX::XMMATRIX& viewProj, bool depthPrepassActive)
{
    using namespace DirectX;
    GpuInstState& S = m_gpuInst;
    if (S.frame.empty() || !S.culler) return;
    PipelineState* instPso = depthPrepassActive ? m_pipelineStateInstLEqual.get() : m_pipelineStateInst.get();
    if (!instPso) return;
    const XMMATRIX passVpT = XMMatrixTranspose(viewProj);
    for (const GpuInstFrameGroup& fg : S.frame)
    {
        GpuInstGroupRt* rt = GpuInstPrepare(cmd, fg);
        if (!rt) continue;
        const MeshRenderer& r = *fg.renderer;
        // エンティティ単位の一律色ティントは per-instance 色（MeshInstanceData.color）で渡す（CPU 経路のバッチと同じ）
        const float color[4] = {r.hasColorTint ? r.colorTint.x : 1.0f, r.hasColorTint ? r.colorTint.y : 1.0f,
                                r.hasColorTint ? r.colorTint.z : 1.0f, r.hasColorTint ? r.colorTint.w : 1.0f};
        // 主ビューの可視数の読み戻し（このスロットの前回ぶん = 3 フレーム前。GPU 完了済み）。統計の三角形数に使う
        {
            u32 c[instgpu::kLists];
            if (instgpu::InstanceGpuCuller::ReadCounters(*rt->gpu, frameIndex % FrameResources::kFrameCount, c))
            {
                std::memcpy(rt->lastCounts, c, sizeof(c));
                rt->countsValid = true;
            }
        }
        if (!GpuInstCull(cmd, *rt, viewProj, 0, 0.0f, color, false, frameIndex, true)) continue;
        if (rt->countsValid)
        {
            for (u32 l = 0; l < instgpu::kLists; ++l)
            {
                S.stats.visibleMain[l] += rt->lastCounts[l];
                for (u32 s = 0; s < static_cast<u32>(r.meshes.size()); ++s) m_statTris += static_cast<u64>(rt->lastCounts[l]) * (r.meshes[s]->GetIndexCountLod(l) / 3);
            }
            S.stats.visibleValid = true;
        }
        cmd->SetPipelineState(instPso->Get());
        m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 16, &passVpT);
        for (u32 s = 0; s < static_cast<u32>(r.meshes.size()); ++s)
        {
            const Mesh* mesh = r.meshes[s];
            const Material* mat = mesh->GetMaterial();
            // 透明パラメータ（不透明の群だけが来るので OPAQUE のはず。バッチ経路と同じ規則で詰める）
            const AlphaParams alphaP = ResolveAlphaParams(mat, r.alphaModeOverride, r.alphaCutoffOverride, r.opacity);
            D3D12_GPU_DESCRIPTOR_HANDLE matSrv;
            if (mat && mat->srvBlockIndex != 0xFFFFFFFF)
                matSrv = m_srvHeap->GetGpuHandle(mat->srvBlockIndex);
            else
            {
                Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
                matSrv = m_srvHeap->GetGpuHandle(tex->GetSrvIndex());
            }
            m_commandList->SetSRVTable(RootSignature::kSlotSRVTable, matSrv);
            struct { float metallic; float roughness; u32 flags; u32 packedTint;
                     float uvScaleX, uvScaleY, uvOffsetX, uvOffsetY; u32 packedEmissive; } pbr;
            pbr.metallic  = (r.overrideMetallic  >= 0.0f) ? r.overrideMetallic  : (mat ? mat->defaultMetallic : 0.0f);
            pbr.roughness = (r.overrideRoughness >= 0.0f) ? r.overrideRoughness : (mat ? mat->defaultRoughness : 0.5f);
            pbr.flags = 0;
            if (mat && mat->normalMapTexture)      pbr.flags |= 1u;
            if (mat && mat->metalRoughnessTexture) pbr.flags |= 2u;
            if (mat && mat->emissiveTexture)       pbr.flags |= kPbrFlagEmissiveTex;
            pbr.packedTint = 0x00FFFFFFu;   // 色は per-instance 頂点ストリームで掛かる
            pbr.flags      = PackAlphaTestFlags(pbr.flags, alphaP);
            pbr.packedTint = PackTintWithOpacity(pbr.packedTint, alphaP.opacity);
            pbr.uvScaleX = 1.0f; pbr.uvScaleY = 1.0f; pbr.uvOffsetX = 0.0f; pbr.uvOffsetY = 0.0f;
            pbr.packedEmissive = PackEmissive(ResolveEmissiveParams(
                mat ? mat->emissiveColor : DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f}, mat ? mat->emissiveIntensity : 0.0f,
                r.overrideEmissiveColor, r.overrideEmissiveIntensity));
            cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 9, &pbr, 0);
            GpuInstExecute(cmd, fg, *rt, s, false);
        }
    }
}


// ---------------------------------------------------------------------------
// ピッキング / 矩形選択 / ホバー用: GPU 経路の群のインスタンスを DrawItem と同じ world / 球 / 番号で列挙する（ScenePick のフック）。
// 球の表（InstanceRecord）は群の内容が変わらない間キャッシュする（10 万個で 6.4MB。ピックが起きた群だけ）。
// ---------------------------------------------------------------------------
void Application::GpuInstVisitPick(const std::function<void(entt::entity, const DirectX::XMFLOAT4X4&, const DirectX::XMFLOAT3&, f32, u32)>& fn)
{
    using namespace DirectX;
    GpuInstState& S = m_gpuInst;
    if (S.frame.empty() || !m_scene) return;
    auto& reg = m_scene->GetRegistry();
    for (const GpuInstFrameGroup& fg : S.frame)
    {
        if (!reg.valid(fg.e) || !fg.set) continue;
        const std::vector<XMFLOAT4X4>& worlds = instgroup::WorldMatrices(reg, fg.e, *fg.set);
        const GpuInstPickCache& pc = EnsurePickCache(S, static_cast<u32>(entt::to_integral(fg.e)), *fg.set, worlds, GroupLocalBounds(*fg.renderer));
        for (u32 i = 0; i < static_cast<u32>(worlds.size()); ++i)
        {
            const instgpu::InstanceRecord& rec = pc.recs[i];
            fn(fg.e, worlds[i], XMFLOAT3{rec.center[0], rec.center[1], rec.center[2]}, rec.radius, i);
        }
    }
}


// ---------------------------------------------------------------------------
// エディタの選択 / ホバーの輪郭用: 描画リスト + 「選択 / ホバー（祖先を含む）に居る GPU 経路の群」を展開した DrawItem。
// 該当する群が無い（普通のフレーム）なら m_drawItems をそのまま返す（コピーしない）。
// ---------------------------------------------------------------------------
const std::vector<DrawItem>* Application::GpuInstOverlayItems()
{
    using namespace DirectX;
    GpuInstState& S = m_gpuInst;
    if (S.frame.empty() || !m_editorCtx || !m_scene) return &m_drawItems;
    auto& reg = m_scene->GetRegistry();
    std::unordered_set<entt::entity> roots(m_editorCtx->selectedEntities.begin(), m_editorCtx->selectedEntities.end());
    if (m_editorCtx->hoveredEntity != entt::null) roots.insert(m_editorCtx->hoveredEntity);
    if (roots.empty()) return &m_drawItems;
    auto inRoots = [&](entt::entity e)
    {
        if (roots.count(e)) return true;
        const Transform* t = reg.valid(e) ? reg.try_get<Transform>(e) : nullptr;
        for (int depth = 0; t && t->parent != entt::null && depth < 64; ++depth)
        {
            if (!reg.valid(t->parent)) break;
            if (roots.count(t->parent)) return true;
            t = reg.try_get<Transform>(t->parent);
        }
        return false;
    };
    bool any = false;
    for (const GpuInstFrameGroup& fg : S.frame) if (reg.valid(fg.e) && inRoots(fg.e)) { any = true; break; }
    if (!any) return &m_drawItems;
    S.overlayItems = m_drawItems;
    for (const GpuInstFrameGroup& fg : S.frame)
    {
        if (!reg.valid(fg.e) || !inRoots(fg.e)) continue;
        const std::vector<XMFLOAT4X4>& worlds = instgroup::WorldMatrices(reg, fg.e, *fg.set);
        // 輪郭の対象は「画面に入っているインスタンスのうちカメラに近い順」の上限 4096 個（CollectOutlineTargets の既定の上限。従来の経路でも
        // 描画リストが LOD（= カメラ距離）順なので近いものから 4096 個が選ばれていた）。球の表は群の内容が変わらない間キャッシュ。
        const instgpu::LocalBounds lb = GroupLocalBounds(*fg.renderer);
        const GpuInstPickCache& pc = EnsurePickCache(S, static_cast<u32>(entt::to_integral(fg.e)), *fg.set, worlds, lb);
        const XMFLOAT3 cp = m_camera ? m_camera->GetPosition() : XMFLOAT3{0, 0, 0};
        const Frustum fr = m_camera ? Frustum::FromViewProj(m_camera->GetViewProjMatrix()) : Frustum::FromViewProj(XMMatrixIdentity());
        std::vector<std::pair<f32, u32>> order;
        order.reserve(fg.count);
        for (u32 i = 0; i < fg.count; ++i)
        {
            const instgpu::InstanceRecord& rec = pc.recs[i];
            if (!fr.SphereVisible(XMVectorSet(rec.center[0], rec.center[1], rec.center[2], 1.0f), rec.radius)) continue;
            const f32 dx = rec.center[0] - cp.x, dy = rec.center[1] - cp.y, dz = rec.center[2] - cp.z;
            order.push_back({dx * dx + dy * dy + dz * dz, i});
        }
        const size_t keep = (std::min)(order.size(), static_cast<size_t>(4096));
        std::partial_sort(order.begin(), order.begin() + keep, order.end());
        const XMVECTOR lmn = XMLoadFloat3(&lb.mn), lmx = XMLoadFloat3(&lb.mx);
        for (size_t k = 0; k < keep; ++k)
        {
            const u32 i = order[k].second;
            DrawItem it{};
            it.e = fg.e;
            it.renderer = fg.renderer;
            it.world = worlds[i];
            it.prevWorld = worlds[i];
            it.center = {pc.recs[i].center[0], pc.recs[i].center[1], pc.recs[i].center[2]};
            it.radius = pc.recs[i].radius;
            {
                const XMMATRIX w = XMLoadFloat4x4(&worlds[i]);
                XMVECTOR wmn = XMVectorReplicate(FLT_MAX), wmx = XMVectorReplicate(-FLT_MAX);
                for (int c = 0; c < 8; ++c)
                {
                    const XMVECTOR corner = XMVectorSelect(lmn, lmx, XMVectorSelectControl((c & 1) ? 1u : 0u, (c & 2) ? 1u : 0u, (c & 4) ? 1u : 0u, 0u));
                    const XMVECTOR p = XMVector3Transform(corner, w);
                    wmn = XMVectorMin(wmn, p); wmx = XMVectorMax(wmx, p);
                }
                XMStoreFloat3(&it.aabbMin, wmn);
                XMStoreFloat3(&it.aabbMax, wmx);
            }
            it.skin = nullptr;
            it.guid = fg.guid;
            it.instanceIndex = i;
            S.overlayItems.push_back(it);
        }
    }
    return &S.overlayItems;
}

nlohmann::json Application::GpuInstStatsJson() const
{
    const GpuInstState& S = m_gpuInst;
    const GpuInstStats& t = S.lastStats;
    nlohmann::json j;
    j["threshold"] = S.threshold;
    j["ready"] = S.culler && S.culler->IsReady();
    j["unavailable"] = S.unavailable;
    j["groups"] = t.groups;
    j["instances"] = t.instances;
    j["culls"] = t.culls;
    j["cullsSkipped"] = t.culled_skipped;
    j["draws"] = t.draws;
    j["uploads"] = t.uploads;
    j["gpuBytes"] = t.gpuBytes;
    j["cpuFallbackGroups"] = t.cpuFallbackGroups;
    if (!t.reasons.empty()) j["fallbackReasons"] = t.reasons;
    j["forceCpuThisFrame"] = S.forceCpuThisFrame;
    if (t.visibleValid) j["visibleMainByLod"] = std::vector<u32>(t.visibleMain, t.visibleMain + instgpu::kLists);
    return j;
}

} // namespace dx12e
