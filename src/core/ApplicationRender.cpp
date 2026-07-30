// ===========================================================================
// Application: フレーム描画（Render とその下請け）
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/Profiler.h"

namespace dx12e
{
using namespace appdetail;

namespace
{
// ボーン行列列のハッシュ（FNV-1a 64、8 バイトずつ）。
// 「前フレームとポーズが同じか」だけを見るので暗号強度は不要。
// 256 ボーン = 16KB のスキャンで、SkinningBuffer::Update の memcpy と同程度のコスト。
u64 HashBonesFnv1a(const std::vector<DirectX::XMFLOAT4X4>& mats)
{
    u64 h = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const u8*>(mats.data());
    const size_t len = mats.size() * sizeof(DirectX::XMFLOAT4X4);
    size_t i = 0;
    for (; i + 8 <= len; i += 8)
    {
        u64 block = 0;
        std::memcpy(&block, p + i, 8);
        h = (h ^ block) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < len; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    // 0 は「未計算」の意味で使うので避ける。
    return h ? h : 1ull;
}
} // namespace

// フレーム描画リスト構築（Render() 先頭で1回）。要素の定義は renderer/DrawItem.h、
// 呼び出し文脈は Application.h の m_drawItems 直前のコメント参照。
void Application::BuildDrawList()
{
    using namespace DirectX;

    // 前フレームの描画統計を HUD へ引き渡してからリセット
    if (m_editorCtx)
    {
        m_editorCtx->statDraws  = m_statDraws;
        m_editorCtx->statCulled = m_statCulled;
    }
    m_statDraws  = 0;
    m_statCulled = 0;
    m_statTris   = 0;
    m_passMain = {}; m_passShadow = {}; m_passOther = {};
    m_passBucket = &m_passOther;

    m_drawItems.clear();
    if (!m_scene) return;

    // LOD 選択基準（メインカメラ位置。フレーム内は全パス共通＝深度プリパスとメインの整合を保つ）
    XMVECTOR camPos = XMVectorZero();
    if (m_camera)
    {
        const XMFLOAT3 cp = m_camera->GetPosition();
        camPos = XMLoadFloat3(&cp);
    }

    auto& reg = m_scene->GetRegistry();
    // GridPlane は view の exclude で弾く（1体ごとの all_of プローブぶんの
    // スパースセット参照が丸ごと消える。10万体規模では効く）。
    auto renderView = reg.view<const Transform, const MeshRenderer>(entt::exclude<GridPlane>);
    {
    CpuScopeTimer _scan(&m_cpuMs[CpuBuildList]); DX12_PROFILE_ZONE_N("BuildDrawList");   // 走査部（ソートは listSort で別計上）
    for (auto [e, transform, renderer] : renderView.each())
    {
        if (renderer.meshes.empty()) continue;
        // park 済み（scale≈0 で退避したプール要素）は全パスで不可視＝リストから除外。
        const auto& sc = transform.scale;
        if (sc.x * sc.x + sc.y * sc.y + sc.z * sc.z < 1e-8f) continue;
        // 発光弾(Pfx*) はメインのパス3で instancing 描画・影/深度は落とさない（従来挙動）。
        // rfind より先頭3文字の直接比較の方が速い（10万体×毎フレームなので効く）。
        if (const auto* nt = reg.try_get<NameTag>(e))
        {
            const std::string& nm = nt->name;
            if (nm.size() >= 3 && nm[0] == 'P' && nm[1] == 'f' && nm[2] == 'x') continue;
        }

        XMMATRIX world = (transform.parent != entt::null)
            ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

        DrawItem item{};
        item.e        = e;
        item.renderer = &renderer;
        XMStoreFloat4x4(&item.world, world);
        // 速度バッファ用の前フレームワールド行列。TAA 無効時は追跡しない＝world と同値（速度0）。
        // 新規スポーン直後も PrevWorldMatrix が無いので world と同値になり、初回のゴーストを防ぐ。
        if (m_trackPrevWorld)
        {
            if (const auto* pw = reg.try_get<PrevWorldMatrix>(e)) item.prevWorld = pw->m;
            else                                                  item.prevWorld = item.world;
        }
        else
        {
            item.prevWorld = item.world;
        }

        auto* skel = reg.try_get<SkeletalAnimation>(e);
        item.skin        = skel ? skel->skinningBuffer.get() : nullptr;
        item.hasNodeAnim = reg.all_of<NodeAnimationComp>(e);

        // ワールドスケール＝ワールド行列の行ベクトル長の最大（親スケールも含む。
        // 旧実装のローカル transform.scale 参照より正確）。
        const f32 ms = (std::max)((std::max)(
            XMVectorGetX(XMVector3Length(world.r[0])),
            XMVectorGetX(XMVector3Length(world.r[1]))),
            XMVectorGetX(XMVector3Length(world.r[2])));
        // 全サブメッシュのローカル AABB を合成し、その中心を球の中心にする。
        // 原点にエンティティ位置を使うと、ジオメトリが原点から離れたモデル
        // (例: Stanford Dragon のように bake で位置がベイクされた glb)で
        // 球が実体を覆わず「近づくと消える」誤カリングが起きる。
        XMVECTOR lmn = XMVectorReplicate( FLT_MAX);
        XMVECTOR lmx = XMVectorReplicate(-FLT_MAX);
        bool hasAabb = false;
        for (const auto* m : renderer.meshes)
        {
            if (!m) continue;
            XMFLOAT3 a = m->GetAABBMin(), b = m->GetAABBMax();
            lmn = XMVectorMin(lmn, XMLoadFloat3(&a));
            lmx = XMVectorMax(lmx, XMLoadFloat3(&b));
            hasAabb = true;
        }
        f32 meshRadius = 1.0f;
        XMVECTOR localCenter = XMVectorZero();
        if (hasAabb)
        {
            localCenter = XMVectorScale(XMVectorAdd(lmn, lmx), 0.5f);
            meshRadius  = (std::max)(1.0f,
                0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(lmx, lmn))));
        }
        XMStoreFloat3(&item.center, XMVector3Transform(localCenter, world));
        // スキンド/ノードアニメは変形でバインドポーズ球を超え得るので余裕を大きめに取る。
        const f32 bias = (item.skin || item.hasNodeAnim) ? 2.0f : 1.25f;
        item.radius = meshRadius * ms * bias;

        // LOD 選択: 見かけの大きさ（半径/距離 ≒ 画面高さに占める割合）で段階を落とす。
        // LODが無い/浅いメッシュは Mesh 側で最終LODへクランプされる。
        // 距離もエンティティ原点ではなくバウンディング球の中心から測る
        // （原点からズレたモデルで LOD が不当に粗く選ばれるのを防ぐ）。
        // ★閾値は「人間が見て分かる距離では落とさない」側に振ってある(2026-07-26)。
        //   旧値 0.50/0.25/0.10/0.04 は 4m 幅の壁が 6.4m で LOD1 に落ちる= 一人称視点だと
        //   目の前でベベルの陰影が消えるのが見えてしまっていた。今の値なら同じ壁が
        //   LOD0 = 約 25m / LOD1 = 約 55m まで保つ。屋内の視距離ならほぼ常にフル解像度。
        //   遠景を大量に出すシーンで重くなったら kLodScale を下げる方向で調整すること。
        constexpr f32 kLodScale = 1.0f;   // >1 で更に遠くまでフル解像度を維持
        const f32 dist = XMVectorGetX(XMVector3Length(
            XMVectorSubtract(XMLoadFloat3(&item.center), camPos)));
        const f32 apparent = item.radius / (std::max)(dist, 1e-3f) * kLodScale;
        item.lod = (apparent < 0.008f) ? 4u
                 : (apparent < 0.020f) ? 3u
                 : (apparent < 0.055f) ? 2u
                 : (apparent < 0.125f) ? 1u : 0u;

        // 0=既定static / 1=カスタム不透明 / 2=skinned / 3=カスタム半透明（不透明の後に描く。
        // 旧実装は entt 格納順で半透明の前後関係が運任せだったため、これはむしろ改善）。
        if (item.skin)                          item.sortKey = 2u;
        else if (renderer.shaderPath.empty())   item.sortKey = 0u;
        else                                    item.sortKey = renderer.shaderAlphaBlend ? 3u : 1u;

        // ---- 自動インスタンシングの適格判定 ----
        // 「per-object 定数を一切必要としない静的メッシュ」だけを畳む。
        // アニメ/スキン/カスタムシェーダ/マテリアル差し替え/UVアニメがあると
        // インスタンス間で定数が違うので対象外（従来経路にフォールバック）。
        // ★レイヤーセット付きの地形は専用 PSO（Terrain.hlsl）で描くので、インスタンシングの
        //   対象から外す（インスタンス経路は ForwardInstanced_VS + Forward_PS 固定のため）。
        //   地形は 1 体 1 メッシュでバッチが 1 件しか集まらず、外しても性能は落ちない。
        //   レイヤーセット未設定の地形は判定に掛からない＝従来どおりインスタンス経路のまま。
        bool splatTerrain = false;
        if (const auto* tc = reg.try_get<Terrain>(e))
            splatTerrain = !tc->layerSetPath.empty();

        item.batchKey = 0;
        if (item.sortKey == 0u && !item.skin && !item.hasNodeAnim && !splatTerrain
            && renderer.meshes.size() == 1 && renderer.meshes[0]
            && !renderer.HasMaterialAsset(0) && !renderer.HasAnyTextureOverride(0)
            && renderer.animFrames == 0
            && renderer.uvScrollU == 0.0f && renderer.uvScrollV == 0.0f)
        {
            // FNV-1a でメッシュ/LOD/PBR値/シェーダパラメータを 1 本の鍵に潰す。
            u64 k = 1469598103934665603ull;
            auto mix = [&k](u64 v) { k ^= v; k *= 1099511628211ull; };
            mix(reinterpret_cast<u64>(renderer.meshes[0]));
            mix(item.lod);
            auto bits = [](f32 f) { u32 u; std::memcpy(&u, &f, 4); return static_cast<u64>(u); };
            mix(bits(renderer.overrideMetallic));
            mix(bits(renderer.overrideRoughness));
            mix(bits(renderer.effectValue));
            mix(bits(renderer.shaderParams.x)); mix(bits(renderer.shaderParams.y));
            mix(bits(renderer.shaderParams.z)); mix(bits(renderer.shaderParams.w));
            item.batchKey = k | 1ull;   // 0 は「不可」の予約値なので必ず非 0 にする
        }
        m_drawItems.push_back(item);
    }
    }   // _scan

    // 速度バッファ用に「今フレームのワールド行列」を次フレームの prevWorld として記録する。
    // ★view 走査を抜けてから別ループで書く（走査中に別ストレージへ emplace すると entt の
    //   バージョン次第で挙動が怪しいため。ここなら確実に安全）。
    if (m_trackPrevWorld)
    {
        for (const auto& it : m_drawItems)
            reg.emplace_or_replace<PrevWorldMatrix>(it.e, PrevWorldMatrix{it.world});
    }

    CpuScopeTimer _sort(&m_cpuMs[CpuListSort]); DX12_PROFILE_ZONE_N("SortDrawList");
    // PSO バケツ → シェーダ → メッシュ → LOD → バッチ鍵 の順に整列。
    // 前半はパイプライン/マテリアル/VB の切替最小化（従来通り）、
    // 末尾の LOD/batchKey は「同一キーが連続する」ことを保証してインスタンシングの
    // ラン検出を O(n) の 1 パスにするため（ハッシュマップ不要）。
    std::sort(m_drawItems.begin(), m_drawItems.end(),
        [](const DrawItem& a, const DrawItem& b) {
            if (a.sortKey != b.sortKey) return a.sortKey < b.sortKey;
            if (a.sortKey == 1u || a.sortKey == 3u) {
                const int c = a.renderer->shaderPath.compare(b.renderer->shaderPath);
                if (c != 0) return c < 0;
            }
            if (a.renderer->meshes[0] != b.renderer->meshes[0])
                return a.renderer->meshes[0] < b.renderer->meshes[0];
            if (a.lod != b.lod) return a.lod < b.lod;
            return a.batchKey < b.batchKey;
        });
}

// フレーム末（Present/EndFrame 後）に1回。perf リング履歴の記録と benchmark の収集・完了を行う。
void Application::RecordPerfFrame()
{
    const auto now = std::chrono::high_resolution_clock::now();
    PerfFrame f{};
    f.frameMs = m_perfPrevFrameValid
        ? std::chrono::duration<f32, std::milli>(now - m_perfPrevFrame).count() : 0.0f;
    m_perfPrevFrame = now;
    m_perfPrevFrameValid = true;
    f.workMs      = std::chrono::duration<f32, std::milli>(now - m_frameStart).count();
    f.fenceWaitMs = m_perfFenceWaitMs;
    f.presentMs   = m_perfPresentMs;
    f.draws  = m_statDraws;
    f.culled = m_statCulled;
    f.tris   = m_statTris;
    f.passMain   = m_passMain;
    f.passShadow = m_passShadow;
    for (u32 s = 0; s < GpuTimer::Count; ++s)
        f.gpuMs[s] = m_gpuTimer ? m_gpuTimer->GetMs(static_cast<GpuTimer::Scope>(s)) : 0.0f;
    for (u32 s = 0; s < CpuScopeCount; ++s) { f.cpuMs[s] = m_cpuMs[s]; m_cpuMs[s] = 0.0f; }
    m_perfHistory[static_cast<size_t>(m_perfTotalFrames % kPerfHistory)] = f;
    ++m_perfTotalFrames;

    // benchmark 収集中: サンプルを貯め、満了したら遅延応答を返す
    if (m_benchFramesLeft > 0 && f.frameMs > 0.0f)
    {
        m_benchSamples.push_back(f.frameMs);
        m_benchDraws += f.draws; m_benchCulled += f.culled; m_benchTris += f.tris;
        m_benchWork += f.workMs; m_benchFence += f.fenceWaitMs; m_benchPresent += f.presentMs;
        for (u32 s = 0; s < GpuTimer::Count; ++s) m_benchGpu[s] += f.gpuMs[s];
        for (u32 s = 0; s < CpuScopeCount; ++s)  m_benchCpu[s] += f.cpuMs[s];

        if (--m_benchFramesLeft == 0)
        {
            const size_t n = m_benchSamples.size();
            PerfSummary sum{};
            sum.samples = static_cast<int>(n);
            if (n > 0)
            {
                double total = 0; for (f32 v : m_benchSamples) total += v;
                sum.frameMs = total / static_cast<double>(n);
                sum.fps = (sum.frameMs > 0.0) ? 1000.0 / sum.frameMs : 0.0;
                std::vector<f32> tmp = m_benchSamples;
                sum.frameMsP95 = PerfPercentile(tmp, 0.95);   // 内部でソート
                sum.frameMsMin = tmp.front();
                sum.frameMsMax = tmp.back();
                const double inv = 1.0 / static_cast<double>(n);
                sum.workMs = m_benchWork * inv; sum.fenceWaitMs = m_benchFence * inv;
                sum.presentMs = m_benchPresent * inv;
                sum.draws = m_benchDraws * inv; sum.culled = m_benchCulled * inv;
                sum.tris = m_benchTris * inv;
                for (u32 s = 0; s < GpuTimer::Count; ++s) sum.gpuMs[s] = m_benchGpu[s] * inv;
                for (u32 s = 0; s < CpuScopeCount; ++s)  sum.cpuMs[s] = m_benchCpu[s] * inv;
            }
            nlohmann::json rep = PerfReportJson(sum, m_useVsync, m_fpsLimit);
            rep["frames"] = static_cast<int>(n);
            rep["instancing"] = m_instancingEnabled;
            rep["clustered"]  = m_clusteredEnabled;
            rep["renderScale"]      = m_renderScale;      // #16。GPU 時間の A/B ではここも見ること
            rep["depthPrepass"]     = m_forceDepthPrepass;
            rep["renderResolution"] = {{"width", m_renderW}, {"height", m_renderH}};
            // uncap で外していた FPS 上限/VSync を元に戻す（レポートには計測時の値=解除後を載せる）
            if (m_benchRestore)
            {
                m_benchRestore = false;
                m_fpsLimit = m_benchSavedFpsLimit;
                m_useVsync = m_benchSavedVsync;
                rep["uncapped"] = true;
            }
            // 1% low FPS（p99 フレーム時間の逆数。スパイクの体感指標）
            {
                std::vector<f32> tmp = m_benchSamples;
                const double p99 = PerfPercentile(tmp, 0.99);
                rep["fps1PercentLow"] = (p99 > 0.0) ? std::round(100000.0 / p99) / 100.0 : 0.0;
            }
            if (m_benchReply.client != 0)
                CompleteMcp(m_mcpBridge.get(), m_benchReply, std::move(rep));
            m_benchReply = {};
            m_benchSamples.clear();
        }
    }
}

void Application::RenderSceneMeshes(ID3D12GraphicsCommandList* nativeCmdList, u32 frameIndex,
                                   DirectX::XMMATRIX viewProj, bool isGameView, u32 aoSrvIndex,
                                   bool depthPrepassActive, u32 contactShadowSrvIndex,
                                   u32 ssrSrvIndex, u32 ssgiSrvIndex)
{
    using namespace DirectX;
    auto& reg = m_scene->GetRegistry();
    auto renderView = reg.view<const Transform, const MeshRenderer>();

    // 視錐台カリング（全ビュー）。保守的球判定＝画面に少しでも掛かる物は必ず残るため、
    // 編集ビューでも見え方は不変のまま画面外ドローだけを省ける。
    const Frustum camFrustum = Frustum::FromViewProj(viewProj);

    // ループ内の冗長ステート切替スキップ用（この関数呼び出し内でのみ有効。
    // 描画リストはソート済みなので同一 PSO/マテリアル/VB が連続しやすい）。
    ID3D12PipelineState* lastPso    = nullptr;
    u64                  lastMatSrv = ~0ull;
    const Mesh*          lastVbMesh = nullptr;
    u32                  lastLod    = ~0u;

    // SSAO AO テーブル(t8)を1回バインド（無効/編集ビューは白=1.0 ダミー）。
    // 全 forward 系 PSO が同一 RootSig を共有するため、ここで一括バインドして hazard を防ぐ。
    if (aoSrvIndex != DescriptorHeap::kInvalidIndex)
        m_commandList->SetSRVTable(RootSignature::kSlotAOSRV,
            m_srvHeap->GetGpuHandle(aoSrvIndex));

    // コンタクトシャドウ(t11)も同じ理由でここで 1 回バインド。
    // PS が無条件で Load するので、無効時も白ダミー(1.0)を必ず張ること。
    {
        const u32 csIdx = (contactShadowSrvIndex != DescriptorHeap::kInvalidIndex)
            ? contactShadowSrvIndex : m_ssaoWhiteSrvIndex;
        if (csIdx != DescriptorHeap::kInvalidIndex)
            m_commandList->SetSRVTable(RootSignature::kSlotContactShadowSRV,
                m_srvHeap->GetGpuHandle(csIdx));
    }

    // SSR(t16) / SSGI(t17) も同形。PS が無条件で Load するので、無効時は必ず
    // 1x1 黒ダミー(RGBA16F)を張ること（1x1 なので範囲外 Load=0 ＝ 寄与ゼロになる）。
    {
        const u32 ssr  = (ssrSrvIndex  != DescriptorHeap::kInvalidIndex) ? ssrSrvIndex  : m_ssBlackSrvIndex;
        const u32 ssgi = (ssgiSrvIndex != DescriptorHeap::kInvalidIndex) ? ssgiSrvIndex : m_ssBlackSrvIndex;
        if (ssr != DescriptorHeap::kInvalidIndex)
            m_commandList->SetSRVTable(RootSignature::kSlotSsrSRV, m_srvHeap->GetGpuHandle(ssr));
        if (ssgi != DescriptorHeap::kInvalidIndex)
            m_commandList->SetSRVTable(RootSignature::kSlotSsgiSRV, m_srvHeap->GetGpuHandle(ssgi));
    }

    // パーティクル判定（名前が "Pfx" で始まる＝加算発光で描く。パス3の instancing 集約用）
    auto isPfx = [&](entt::entity e) -> bool {
        const auto* nt = reg.try_get<NameTag>(e);
        return nt && nt->name.rfind("Pfx", 0) == 0;
    };

    // 1エンティティ分の描画（パイプライン選択 + メッシュ描画）。
    // park除外/ワールド行列/カリングは呼び出し側（描画リスト or グリッド walk）で解決済み。
    auto drawEntity = [&](entt::entity e, const MeshRenderer& renderer, XMMATRIX world,
                          SkinningBuffer* skin, bool hasNodeAnim, bool isGrid, u32 lod)
    {
        // ★地形マテリアル（4 レイヤースプラット）のゲートはここ 1 箇所だけ。
        //   layerSetPath が空でなく、レイヤー配列とスプラットが両方揃った時にだけ地形経路へ入る。
        //   b2 の意味を読み替えているので「地形 PSO ではないのに地形の定数を詰める」
        //   逆条件が絶対に起きない書き方にしてある（terrainSrv が 0xFFFFFFFF なら丸ごと従来経路）。
        const Terrain* terrainMat = nullptr;
        u32 terrainSrv = 0xFFFFFFFF;
        if (!isGrid && !skin && m_terrainPipelineState)
        {
            const Terrain* tc = reg.try_get<Terrain>(e);
            if (tc && !tc->layerSetPath.empty())
            {
                terrainSrv = EnsureTerrainSrv(e, *tc, nativeCmdList);
                if (terrainSrv != 0xFFFFFFFF) terrainMat = tc;
            }
        }

        // PSO 選択。同一 PSO が連続する間はバインドをスキップ（リストはソート済み）。
        PipelineState* psoSel;
        if (isGrid)
        {
            psoSel = m_gridPipelineState.get();
        }
        else if (terrainMat)
        {
            psoSel = depthPrepassActive ? m_terrainPipelineStateLEqual.get()
                                        : m_terrainPipelineState.get();
        }
        else if (skin)
        {
            // 深度プリパス併用時は LESS_EQUAL バリアントで同一深度を通す。
            psoSel = depthPrepassActive ? m_skinnedPipelineStateLEqual.get()
                                        : m_skinnedPipelineState.get();
        }
        else
        {
            // カスタムシェーダー割当(静的メッシュのみ)。未コンパイル/生成失敗時は既定 Forward へフォールバック。
            CustomForwardPsos* custom = renderer.shaderPath.empty() ? nullptr : EnsureCustomPso(renderer.shaderPath);
            if (custom)
                psoSel = renderer.shaderAlphaBlend
                    ? (depthPrepassActive ? custom->lequalBlend.get() : custom->lessBlend.get())
                    : (depthPrepassActive ? custom->lequal.get() : custom->less.get());
            else
                psoSel = depthPrepassActive ? m_pipelineStateLEqual.get() : m_pipelineState.get();
        }
        if (psoSel->Get() != lastPso)
        {
            m_commandList->SetPipelineState(*psoSel);
            lastPso = psoSel->Get();
        }
        if (skin)
            m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                m_srvHeap->GetGpuHandle(skin->GetSrvIndex(frameIndex)));
        for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
        {
            const auto* mesh = renderer.meshes[mi];

            XMMATRIX meshWorld = world;
            if (hasNodeAnim && mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
            {
                XMMATRIX nodeMat = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]);
                meshWorld = nodeMat * world;
            }

            // pad は HLSL cbuffer のパッキング(float4 は 16 バイト境界)合わせ。RootSignature.cpp 参照。
            struct PerObjectData { XMMATRIX mvp; XMMATRIX mdl; float effect; XMFLOAT3 _pad; XMFLOAT4 params; } objData;
            objData.mvp = XMMatrixTranspose(meshWorld * viewProj);
            objData.mdl = XMMatrixTranspose(meshWorld);
            if (terrainMat)
            {
                // ★b0 の余り 8 float を地形用に読み替える（バイトレイアウトは 1 バイトも変えない）。
                //   effect → pomHeightScale / _pad(3) → pomFadeStart,End,normalStrength /
                //   params → terrainParams(.x=1/uvScale .y=distTilingStart .z=distTilingFarScale .w=macroStrength)
                objData.effect = terrainMat->pomHeightScale;
                objData._pad   = { terrainMat->pomFadeStart, terrainMat->pomFadeEnd,
                                   terrainMat->normalStrength };
                const f32 uvS  = (terrainMat->uvScale > 1e-4f) ? terrainMat->uvScale : 1.0f;
                objData.params = { 1.0f / uvS, terrainMat->distTilingStart,
                                   terrainMat->distTilingFarScale, terrainMat->macroStrength };
            }
            else
            {
                objData.effect = renderer.effectValue;
                objData._pad   = {};
                objData.params = renderer.shaderParams;
            }
            m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 40, &objData);

            const Material* mat = mesh->GetMaterial();

            // マテリアルアセット(assets/materials/*.dxmat)割当があれば最優先で解決する。
            // 優先度: materialAsset > overrideXxxTexture(テクスチャ個別上書き) > モデル焼き込み Material。
            const MaterialAssetManager::Entry* matAsset = nullptr;
            if (renderer.HasMaterialAsset(mi))
            {
                const MaterialAssetManager::Entry* loaded = m_materialAssetManager->GetOrLoad(
                    MeshRenderer::SafeGetOverride(renderer.materialAsset, mi), nativeCmdList);
                if (loaded && loaded->valid) matAsset = loaded;
            }

            // PBR テクスチャ SRV ブロックをバインド。インスタンス単位のテクスチャ上書き
            // (D&Dでのマテリアル割当、MeshRenderer::overrideAlbedoTexture 等)があれば
            // 専用ブロックを優先する(mat は同一モデルの全インスタンスで共有されるため直接は触らない)。
            u32 overrideBlock = EnsureMaterialOverrideSrv(e, mi, renderer, mat, nativeCmdList);
            D3D12_GPU_DESCRIPTOR_HANDLE matSrv;
            if (terrainMat)
                matSrv = m_srvHeap->GetGpuHandle(terrainSrv);   // t0/t1=レイヤー配列, t2=スプラット
            else if (matAsset)
                matSrv = m_srvHeap->GetGpuHandle(matAsset->srvBlockStart);
            else if (overrideBlock != 0xFFFFFFFF)
                matSrv = m_srvHeap->GetGpuHandle(overrideBlock);
            else if (mat && mat->srvBlockIndex != 0xFFFFFFFF)
                matSrv = m_srvHeap->GetGpuHandle(mat->srvBlockIndex);
            else
            {
                Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
                matSrv = m_srvHeap->GetGpuHandle(tex->GetSrvIndex());
            }
            // 同一マテリアル連続時はディスクリプタテーブル張り替えをスキップ
            if (matSrv.ptr != lastMatSrv)
            {
                m_commandList->SetSRVTable(RootSignature::kSlotSRVTable, matSrv);
                lastMatSrv = matSrv.ptr;
            }

            // ★地形は b2（8 DWORD）をまるごと TerrainMaterial として読み替える。
            //   Terrain.hlsl の cbuffer TerrainMaterial とバイト単位で一致させること。
            if (terrainMat)
            {
                const auto* ls = m_terrainLayerSets->GetOrLoad(terrainMat->layerSetPath, nativeCmdList);
                const u32 layerCount = (ls && ls->valid) ? ls->layerCount : 1u;
                u32 flags = terrainMat->terrainMatFlags & 0xFFu;
                flags |= (layerCount & 0xFu) << 8;
                flags |= (std::min)(terrainMat->pomMaxSteps, 255u) << 16;

                struct { float heightBlendDepth; float triplanarSharpness; u32 flags; float macroScale;
                         float tile0, tile1, tile2, tile3; } tp;
                tp.heightBlendDepth   = terrainMat->heightBlendDepth;
                tp.triplanarSharpness = (terrainMat->triplanarSharpness > 0.1f)
                                      ? terrainMat->triplanarSharpness : 4.0f;
                tp.flags              = flags;
                tp.macroScale         = (terrainMat->macroScale > 1e-3f) ? terrainMat->macroScale : 90.0f;
                tp.tile0 = ls ? ls->tiling[0] : 0.35f;
                tp.tile1 = ls ? ls->tiling[1] : 0.35f;
                tp.tile2 = ls ? ls->tiling[2] : 0.35f;
                tp.tile3 = ls ? ls->tiling[3] : 0.35f;
                nativeCmdList->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 8, &tp, 0);

                if (mesh != lastVbMesh || lod != lastLod)
                {
                    m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                    m_commandList->SetIndexBuffer(mesh->GetIndexBufferLod(lod).GetView());
                    lastVbMesh = mesh;
                    lastLod    = lod;
                }
                m_commandList->DrawIndexedInstanced(mesh->GetIndexCountLod(lod));
                ++m_statDraws;
                m_statTris += mesh->GetIndexCountLod(lod) / 3;
                ++m_passBucket->draws;
                m_passBucket->tris += mesh->GetIndexCountLod(lod) / 3;
                continue;
            }

            // PBR Material Constants (Slot 5)
            struct { float metallic; float roughness; u32 flags; float pad;
                     float uvScaleX, uvScaleY, uvOffsetX, uvOffsetY; } pbrParams;
            if (matAsset)
            {
                // MeshRenderer のスカラーオーバーライドは materialAsset の係数よりさらに優先
                // (エンティティ単位の微調整用。glTF 意味論なのでテクスチャの有無に関わらず係数は常に効く)。
                pbrParams.metallic  = (renderer.overrideMetallic  >= 0.0f) ? renderer.overrideMetallic  : matAsset->data.metallic;
                pbrParams.roughness = (renderer.overrideRoughness >= 0.0f) ? renderer.overrideRoughness : matAsset->data.roughness;
                pbrParams.flags = 0;
                if (matAsset->hasNormalTex) pbrParams.flags |= 1u;
                if (matAsset->hasMRTex)     pbrParams.flags |= 2u;
                pbrParams.pad = 0;
            }
            else
            {
                // MeshRenderer のオーバーライド値を優先、なければ Material の値
                pbrParams.metallic  = (renderer.overrideMetallic  >= 0.0f) ? renderer.overrideMetallic
                                    : (mat ? mat->defaultMetallic : 0.0f);
                pbrParams.roughness = (renderer.overrideRoughness >= 0.0f) ? renderer.overrideRoughness
                                    : (mat ? mat->defaultRoughness : 0.5f);
                pbrParams.flags     = 0;
                // ★上書きテクスチャ(MeshRenderer::overrideNormalTexture /
                //   overrideMetalRoughnessTexture)も見る。EnsureMaterialOverrideSrv は既に
                //   t1/t2 を差し替えているのに、ここが Material 側しか見ていなかったので
                //   「set_texture で法線/ORM を貼っても SRV だけ差し替わりシェーダが無視する」
                //   状態だった(#26)。ブロックが確保できた時だけ立てる(確保に失敗したら
                //   t1/t2 は 3 本ブロックではないので絶対にサンプルさせない)。
                const bool ovBlockOk = (overrideBlock != 0xFFFFFFFFu);
                const bool ovNormal  = ovBlockOk &&
                    !MeshRenderer::SafeGetOverride(renderer.overrideNormalTexture, mi).empty();
                const bool ovMR      = ovBlockOk &&
                    !MeshRenderer::SafeGetOverride(renderer.overrideMetalRoughnessTexture, mi).empty();
                if (ovNormal || (mat && mat->normalMapTexture))      pbrParams.flags |= 1u;
                // ★metallic/roughness のスカラーは glTF 意味論の「係数」として扱い、
                //   MR テクスチャを殺さない(matAsset 経路と同じ規則に揃えた)。
                //   SceneSerializer が全モデルに material{metallic,roughness} を必ず書くため、
                //   旧実装では「保存し直したシーンでは ORM テクスチャが必ず死ぬ」状態だった。
                if (ovMR || (mat && mat->metalRoughnessTexture))     pbrParams.flags |= 2u;
                pbrParams.pad = 0;
            }
            // UV 変換: 連番アニメ > UVスクロール > 恒等 の優先順（renderer/SpriteAnim.h の純関数）
            pbrParams.uvScaleX = 1.0f; pbrParams.uvScaleY = 1.0f;
            pbrParams.uvOffsetX = 0.0f; pbrParams.uvOffsetY = 0.0f;
            if (renderer.animFrames > 0)
            {
                const SpriteUvRect r = ComputeFlipbookUvEx(
                    renderer.animFrames, renderer.animFps, renderer.animCols,
                    renderer.animRow, renderer.animRows, renderer.animMode, renderer._animT);
                // セル矩形へ写す: uv' = uv * (幅,高) + (左,上)
                pbrParams.uvScaleX  = r.u1 - r.u0;
                pbrParams.uvScaleY  = r.v1 - r.v0;
                pbrParams.uvOffsetX = r.u0;
                pbrParams.uvOffsetY = r.v0;
            }
            else if (renderer.uvScrollU != 0.0f || renderer.uvScrollV != 0.0f)
            {
                // オフセットは [0,1) に折り返す（長時間再生での float 精度劣化を防ぐ。
                // サンプラーは WRAP なので見た目は連続する）
                float du = renderer.uvScrollU * renderer._animT;
                float dv = renderer.uvScrollV * renderer._animT;
                pbrParams.uvOffsetX = du - std::floor(du);
                pbrParams.uvOffsetY = dv - std::floor(dv);
            }
            nativeCmdList->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 8, &pbrParams, 0);

            // 同一メッシュ・同一LOD連続時（同モデルの複数エンティティ等）は IA バインドをスキップ
            if (mesh != lastVbMesh || lod != lastLod)
            {
                m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                m_commandList->SetIndexBuffer(mesh->GetIndexBufferLod(lod).GetView());
                lastVbMesh = mesh;
                lastLod    = lod;
            }
            m_commandList->DrawIndexedInstanced(mesh->GetIndexCountLod(lod));
            ++m_statDraws;
            m_statTris += mesh->GetIndexCountLod(lod) / 3;
            ++m_passBucket->draws;
            m_passBucket->tris += mesh->GetIndexCountLod(lod) / 3;
        }
    };

    // パス1: 不透明（グリッド・パーティクル以外）＝フレーム描画リスト（ソート済み）を
    // カメラ視錐台でカリングしながら描く。リストは Render() 先頭の BuildDrawList() で構築済み。
    //
    // ★自動インスタンシング: batchKey が同じ連続ラン（= 同メッシュ・同LOD・同マテリアル）は
    //   1 回の DrawIndexedInstanced に畳む。リストは batchKey が連続するようソート済みなので
    //   ハッシュマップ無しの 1 パスで検出できる。適格外(batchKey==0)は従来の per-object 描画。
    PipelineState* instPso = !m_instancingEnabled ? nullptr
                           : depthPrepassActive   ? m_pipelineStateInstLEqual.get()
                                                  : m_pipelineStateInst.get();
    const XMMATRIX passVpT = XMMatrixTranspose(viewProj);   // cbuffer 列優先再解釈で hlsl 上は VP

    // 描画は単一スレッドなので関数ローカル static で使い回す（毎フレームの再確保を避ける）。
    static std::vector<MeshInstanceData> instScratch;

    const size_t itemCount = m_drawItems.size();
    for (size_t i = 0; i < itemCount; )
    {
        const DrawItem& head = m_drawItems[i];
        if (head.batchKey == 0 || !instPso)
        {
            XMMATRIX world = XMLoadFloat4x4(&head.world);
            if (!camFrustum.SphereVisible(XMLoadFloat3(&head.center), head.radius)) ++m_statCulled;
            else drawEntity(head.e, *head.renderer, world, head.skin, head.hasNodeAnim,
                            /*isGrid*/ false, head.lod);
            ++i;
            continue;
        }

        // 同一 batchKey のランのうち「見えている物」だけインスタンスへ積む
        instScratch.clear();
        size_t j = i;
        for (; j < itemCount && m_drawItems[j].batchKey == head.batchKey; ++j)
        {
            XMMATRIX w = XMLoadFloat4x4(&m_drawItems[j].world);
            if (!camFrustum.SphereVisible(XMLoadFloat3(&m_drawItems[j].center), m_drawItems[j].radius)) { ++m_statCulled; continue; }
            XMMATRIX t = XMMatrixTranspose(w);
            MeshInstanceData d;
            XMStoreFloat4(&d.r0, t.r[0]);
            XMStoreFloat4(&d.r1, t.r[1]);
            XMStoreFloat4(&d.r2, t.r[2]);
            d.color = {1.0f, 1.0f, 1.0f, 1.0f};   // 非インスタンシング経路と同じ＝頂点色のみ
            instScratch.push_back(d);
        }

        const u32 n = static_cast<u32>(instScratch.size());
        if (n == 0) { i = j; continue; }

        if (m_instanceCursor + n > kMaxInstances)
        {
            // リング溢れ: 見た目を保つため従来経路で描き切る（起きたら kMaxInstances を上げる）
            for (size_t k = i; k < j; ++k)
            {
                const DrawItem& it = m_drawItems[k];
                XMMATRIX w = XMLoadFloat4x4(&it.world);
                if (!camFrustum.SphereVisible(XMLoadFloat3(&it.center), it.radius)) continue;
                drawEntity(it.e, *it.renderer, w, it.skin, it.hasNodeAnim, false, it.lod);
            }
            i = j;
            continue;
        }

        const u32 base = m_instanceCursor;
        std::memcpy(m_instanceMapped[frameIndex] + static_cast<size_t>(base) * sizeof(MeshInstanceData),
                    instScratch.data(), static_cast<size_t>(n) * sizeof(MeshInstanceData));
        m_instanceCursor += n;

        if (instPso->Get() != lastPso)
        {
            m_commandList->SetPipelineState(*instPso);
            lastPso = instPso->Get();
        }
        m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 16, &passVpT);

        const Mesh*     mesh = head.renderer->meshes[0];
        const Material* mat  = mesh->GetMaterial();
        D3D12_GPU_DESCRIPTOR_HANDLE matSrv;
        if (mat && mat->srvBlockIndex != 0xFFFFFFFF)
            matSrv = m_srvHeap->GetGpuHandle(mat->srvBlockIndex);
        else
        {
            Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture
                                                       : m_resourceManager->GetDefaultWhiteTexture();
            matSrv = m_srvHeap->GetGpuHandle(tex->GetSrvIndex());
        }
        if (matSrv.ptr != lastMatSrv)
        {
            m_commandList->SetSRVTable(RootSignature::kSlotSRVTable, matSrv);
            lastMatSrv = matSrv.ptr;
        }

        // 適格判定で materialAsset/上書きテクスチャ/UVアニメは除外済みなので単純形でよい
        struct { float metallic; float roughness; u32 flags; float pad;
                 float uvScaleX, uvScaleY, uvOffsetX, uvOffsetY; } pbrParams;
        const MeshRenderer& r = *head.renderer;
        pbrParams.metallic  = (r.overrideMetallic  >= 0.0f) ? r.overrideMetallic
                                                            : (mat ? mat->defaultMetallic : 0.0f);
        pbrParams.roughness = (r.overrideRoughness >= 0.0f) ? r.overrideRoughness
                                                            : (mat ? mat->defaultRoughness : 0.5f);
        pbrParams.flags = 0;
        if (mat && mat->normalMapTexture)      pbrParams.flags |= 1u;
        // ★metallic/roughness のスカラーは glTF 意味論の「係数」＝ MR テクスチャを殺さない
        //   （非インスタンス経路 / matAsset 経路と同じ規則。#26 で揃えた）
        if (mat && mat->metalRoughnessTexture) pbrParams.flags |= 2u;
        pbrParams.pad = 0.0f;
        pbrParams.uvScaleX = 1.0f; pbrParams.uvScaleY = 1.0f;
        pbrParams.uvOffsetX = 0.0f; pbrParams.uvOffsetY = 0.0f;
        nativeCmdList->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 8, &pbrParams, 0);

        m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());              // slot0
        m_commandList->SetIndexBuffer(mesh->GetIndexBufferLod(head.lod).GetView());
        D3D12_VERTEX_BUFFER_VIEW iv = m_instanceVbView[frameIndex];
        iv.BufferLocation += static_cast<u64>(base) * sizeof(MeshInstanceData);
        iv.SizeInBytes     = n * sizeof(MeshInstanceData);
        nativeCmdList->IASetVertexBuffers(1, 1, &iv);                                   // slot1
        lastVbMesh = mesh;
        lastLod    = head.lod;

        const u32 idx = mesh->GetIndexCountLod(head.lod);
        m_commandList->DrawIndexedInstanced(idx, n);
        ++m_statDraws;
        m_statTris += idx / 3 * n;
        ++m_passBucket->draws;
        m_passBucket->tris += idx / 3 * n;

        i = j;
    }

    // パス2: エディタ用グリッド。線だけを後描きする（ForwardGrid 側で線以外 alpha=0）。
    // 床全体へ半透明の膜を被せず、グリッド表示だけ維持する。
    // グリッドは描画リスト対象外なので従来どおり registry を直接走査（エディタのみ・少数）。
    if (!isGameView)
    {
        for (auto [e, transform, renderer] : renderView.each())
        {
            const auto* gp = reg.try_get<GridPlane>(e);
            if (!gp || !gp->enabled) continue;
            XMMATRIX world = (transform.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();
            drawEntity(e, renderer, world, nullptr, reg.all_of<NodeAnimationComp>(e), /*isGrid*/ true, /*lod*/ 0u);
        }
    }

    // パス3: 発光弾(Pfx) を GPU instancing で加算合成。同一メッシュ(共有)を1ドローに集約。
    // 弾が数百発でも「メッシュ種類ぶんのドロー」だけで済む（boss3 弾幕の draw 数を一定化）。
    {
        std::unordered_map<const Mesh*, std::vector<MeshInstanceData>> byMesh;
        for (auto [e, transform, renderer] : renderView.each())
        {
            if (!isPfx(e)) continue;
            const auto& sc = transform.scale;
            if (sc.x * sc.x + sc.y * sc.y + sc.z * sc.z < 1e-8f) continue;   // park スキップ
            if (renderer.meshes.empty() || !renderer.meshes[0]) continue;

            XMMATRIX world = (transform.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();
            XMMATRIX t = XMMatrixTranspose(world);
            MeshInstanceData inst;
            XMStoreFloat4(&inst.r0, t.r[0]);
            XMStoreFloat4(&inst.r1, t.r[1]);
            XMStoreFloat4(&inst.r2, t.r[2]);
            inst.color = renderer.instanceColor;
            byMesh[renderer.meshes[0]].push_back(inst);
        }

        if (!byMesh.empty())
        {
            struct InstBucket { const Mesh* mesh; u32 base; u32 count; };
            std::vector<InstBucket> buckets;
            u32 cursor = m_instanceCursor;   // メイン/プレビューで同フレームバッファを連番共有
            uint8_t* dst = m_instanceMapped[frameIndex];
            for (auto& [mesh, vec] : byMesh)
            {
                if (cursor >= kMaxInstances) break;
                u32 n = (std::min)(static_cast<u32>(vec.size()), kMaxInstances - cursor);
                if (n == 0) continue;
                memcpy(dst + static_cast<size_t>(cursor) * sizeof(MeshInstanceData),
                       vec.data(), static_cast<size_t>(n) * sizeof(MeshInstanceData));
                buckets.push_back({mesh, cursor, n});
                cursor += n;
            }
            m_instanceCursor = cursor;   // 次の RenderSceneMeshes 呼び出し（プレビュー）へ連番を引き継ぐ

            m_commandList->SetPipelineState(*m_emissivePipelineState);
            XMMATRIX vpT = XMMatrixTranspose(viewProj);   // cbuffer 列優先再解釈で hlsl 上は VP
            m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 16, &vpT);
            m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (auto& b : buckets)
            {
                m_commandList->SetVertexBuffer(b.mesh->GetVertexBuffer().GetView());   // slot0
                m_commandList->SetIndexBuffer(b.mesh->GetIndexBuffer().GetView());
                D3D12_VERTEX_BUFFER_VIEW iv = m_instanceVbView[frameIndex];
                iv.BufferLocation += static_cast<u64>(b.base) * sizeof(MeshInstanceData);
                iv.SizeInBytes     = b.count * sizeof(MeshInstanceData);
                nativeCmdList->IASetVertexBuffers(1, 1, &iv);                          // slot1
                m_commandList->DrawIndexedInstanced(b.mesh->GetIndexCount(), b.count);
                ++m_statDraws;
                m_statTris += b.mesh->GetIndexCount() / 3 * b.count;
                ++m_passBucket->draws;
                m_passBucket->tris += b.mesh->GetIndexCount() / 3 * b.count;
            }
        }
    }
}

void Application::DrawWorldSprites(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj,
                                  DirectX::XMFLOAT3 camRight, DirectX::XMFLOAT3 camUp,
                                  D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                  u32 vpX, u32 vpY, u32 vpW, u32 vpH, float time)
{
    using namespace DirectX;
    if (!m_spriteRenderer || !m_scene) return;

    auto& sreg = m_scene->GetRegistry();
    m_spriteRenderer->BeginWorldFrame();
    for (auto [e, sp] : sreg.view<const Sprite2D>().each())
    {
        if (!sp.worldSpace || sp.texturePath.empty()) continue;
        if (!sreg.all_of<Transform>(e)) continue;
        const std::string absPath = PathResolver::AssetsDir() + sp.texturePath;
        std::wstring wpath = PathResolver::Utf8ToWide(absPath);
        Texture* tex = m_resourceManager->GetOrLoadTexture(wpath, cmd);
        if (!tex) continue;

        WorldSpriteDesc d;
        XMStoreFloat4x4(&d.world, ComputeWorldMatrix(sreg, e));
        d.size      = sp.size;
        d.uvMin     = sp.uvMin;
        d.uvMax     = sp.uvMax;
        // フリップブック/UVスクロール（Editor 中もプレビュー再生。SpriteAnim.h の純関数）
        if (sp.animFrames > 0)
        {
            const SpriteUvRect r = ComputeFlipbookUvEx(sp.animFrames, sp.animFps, sp.animCols,
                                                       sp.animRow, sp.animRows, sp.animMode,
                                                       sp._animT);
            d.uvMin = {r.u0, r.v0}; d.uvMax = {r.u1, r.v1};
        }
        else if (sp.scrollU != 0.0f || sp.scrollV != 0.0f)
        {
            const SpriteUvRect r = ComputeScrollUv(sp.uvMin.x, sp.uvMin.y, sp.uvMax.x, sp.uvMax.y,
                                                   sp.scrollU, sp.scrollV, time);
            d.uvMin = {r.u0, r.v0}; d.uvMax = {r.u1, r.v1};
        }
        d.color     = sp.color;
        d.srvIndex  = tex->GetSrvIndex();
        d.layer     = static_cast<float>(sp.layer);
        d.billboard = sp.billboard;
        d.effect    = sp.effectValue;
        d.params    = sp.shaderParams;
        if (!sp.shaderPath.empty())
        {
            if (CustomSpritePsos* custom = EnsureCustomSpritePso(sp.shaderPath))
                d.customPso = sp.shaderAlphaBlend ? custom->blend->Get() : custom->opaque->Get();
        }
        m_spriteRenderer->SubmitWorld(d);
    }

    if (m_spriteRenderer->HasAnyWorld())
    {
        cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);   // 深度テストのため DSV もバインド
        m_commandList->SetViewportAndScissor(vpX, vpY, vpW, vpH);
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        m_spriteRenderer->RenderWorld(cmd, viewProj, camRight, camUp, time);
    }
}

// CSM: カメラ視錐台を near→far で kNumCascades 分割し、各カスケードをライト視点へタイトフィット。
// 結果は m_cascadeViewProj[]（行優先 world*VP 用、非転置）と m_cascadeSplitsView[]（各遠端 view 深度）に格納。
// 深度専用シーン描画（CSM各カスケード/スポット影/ポイント影の各面/SSAOプリパスで共用）。
// RTVなし/DSVのみを前提に、Transform+MeshRenderer を全走査して viewProj で変換し描画する。
// GridPlane・park済み(scale≈0)・発光弾(Pfx*)は除外（元のシャドウパスのフィルタをそのまま踏襲）。
void Application::RenderDepthOnlyScene(DirectX::XMMATRIX viewProj, PipelineState& staticPSO,
                                       PipelineState& skinnedPSO, bool updateSkinning, u32 frameIndex,
                                       u32 lodBias, PipelineState* instPSO,
                                       const PrepassParams* prepass,
                                       bool skipRtCovered,
                                       f32 cascadeTexelWorld)
{
    using namespace DirectX;

    auto& reg = m_scene->GetRegistry();

    // 速度モード（PrepassMode::DepthVelocityGBuffer）。b0 のレイアウトが変わり、
    // インスタンシング経路は slot2 に前フレームワールドを積み、スキンドは t12 に前ボーンを張る。
    const bool velocityMode = prepass && prepass->mode == PrepassMode::DepthVelocityGBuffer;
    const XMMATRIX prevViewProj = velocityMode ? XMLoadFloat4x4(&prepass->prevViewProj)
                                               : XMMatrixIdentity();
    const XMFLOAT2 jitterNdc = prepass ? prepass->jitterNdc : XMFLOAT2{0.0f, 0.0f};
    const bool skipTransparent = prepass && prepass->skipTransparent;
    if (velocityMode && !m_instancePrevMapped[frameIndex]) instPSO = nullptr;  // 前ワールドVBが無ければ従来経路

    // 速度+G-Buffer パスの b0。静的/スキンドは 40 DWORD（ルート定数の上限ぴったり）、
    // インスタンシングは 36 DWORD（法線はインスタンスデータから取れるので行列を送らない）。
    // ★ 静的/スキンド版で prevMvp の z 列を送っていないのは、そこで浮いた 4 DWORD を
    //   法線行列(3x3=9)に回すため。prevClip は x/y/w しか使わないので情報は落ちていない。
    struct VelocityObjCB
    {
        XMMATRIX mvpJ;                        // 16  transpose(world * viewProjJittered)
        XMFLOAT4 prevC0, prevC1, prevC3;      // 12  transpose(prevWorld*prevVP) の row0/1/3
        XMFLOAT3 nrm0; float jitterX;         //  4  transpose(world).row0.xyz
        XMFLOAT3 nrm1; float jitterY;         //  4
        XMFLOAT3 nrm2; float matPacked;       //  4  round(rough*255)+round(metal*255)*256
    };
    static constexpr u32 kVelocityObjNum32 = 40;
    static_assert(sizeof(VelocityObjCB) == kVelocityObjNum32 * sizeof(float),
                  "VelocityObjCB は shaders/velocity/VelocityPrepass{,Skinned}.hlsl の cbuffer と一致させること");

    struct VelocityInstObjCB { XMMATRIX vpJ; XMMATRIX prevVp; XMFLOAT2 jitter; float matPacked; float pad; };
    static constexpr u32 kVelocityInstObjNum32 = 36;
    static_assert(sizeof(VelocityInstObjCB) == kVelocityInstObjNum32 * sizeof(float),
                  "VelocityInstObjCB は shaders/velocity/VelocityPrepassInstanced.hlsl と一致させること");

    // b0 の 1 float に roughness/metallic を詰める（ScreenSpaceCommon.hlsli の SS_UnpackMaterial と対）。
    auto packMaterial = [](float roughness, float metallic) -> float
    {
        const float r = std::round(std::clamp(roughness, 0.0f, 1.0f) * 255.0f);
        const float m = std::round(std::clamp(metallic,  0.0f, 1.0f) * 255.0f);
        return r + m * 256.0f;
    };
    // transpose(world) の row0/1/2 の xyz を取り出す（= world 3x3 の列）。
    // forward の mul(normal, (float3x3)model) と同じ値になる並び。
    auto fillNormalRows = [](VelocityObjCB& c, const XMMATRIX& worldT)
    {
        XMFLOAT4 r0, r1, r2;
        XMStoreFloat4(&r0, worldT.r[0]);
        XMStoreFloat4(&r1, worldT.r[1]);
        XMStoreFloat4(&r2, worldT.r[2]);
        c.nrm0 = {r0.x, r0.y, r0.z};
        c.nrm1 = {r1.x, r1.y, r1.z};
        c.nrm2 = {r2.x, r2.y, r2.z};
    };
    // transpose(prevMvp) の row0/1/3（= prevMvp の col0/1/3）。row2(z) は prevClip で使わない。
    auto fillPrevCols = [](VelocityObjCB& c, const XMMATRIX& prevMvpT)
    {
        XMStoreFloat4(&c.prevC0, prevMvpT.r[0]);
        XMStoreFloat4(&c.prevC1, prevMvpT.r[1]);
        XMStoreFloat4(&c.prevC3, prevMvpT.r[3]);
    };
    // G-Buffer に書く roughness/metallic。RenderSceneMeshes の PBR ルート定数と同じ優先順
    // （materialAsset > MeshRenderer のスカラー上書き > モデル焼き込み Material）。
    // v1 では metalRoughness テクスチャは読まない（プリパスで t0-t2 を毎ドロー貼らないため）。
    auto resolvePbr = [&](const MeshRenderer& r, const Mesh* m, u32 mi, float& rough, float& metal)
    {
        const Material* mat = m ? m->GetMaterial() : nullptr;
        float baseM = mat ? mat->defaultMetallic  : 0.0f;
        float baseR = mat ? mat->defaultRoughness : 0.5f;
        if (m_materialAssetManager && r.HasMaterialAsset(mi))
        {
            const MaterialAssetManager::Entry* loaded = m_materialAssetManager->GetOrLoad(
                MeshRenderer::SafeGetOverride(r.materialAsset, mi), m_commandList->GetNative());
            if (loaded && loaded->valid) { baseM = loaded->data.metallic; baseR = loaded->data.roughness; }
        }
        metal = (r.overrideMetallic  >= 0.0f) ? r.overrideMetallic  : baseM;
        rough = (r.overrideRoughness >= 0.0f) ? r.overrideRoughness : baseR;
        rough = (std::max)(rough, 0.04f);   // Forward.hlsl:179 と同じ下限
    };

    // このパスの視錐台（ライト/カメラ視点）で描画リストを球カリングする。
    // CSM はタイトフィット正射 + DepthClipEnable=TRUE のため、落とすのは
    // 「現状でもラスタライザにクリップされている範囲」だけ＝影の見た目は不変。
    const Frustum frustum = Frustum::FromViewProj(viewProj);

    // ★このパスで使う LOD。カメラ距離で決まる item.lod に加えて、
    //   「シャドウマップ上で何テクセルを占めるか」でも粗くする。
    //   遠カスケードはスライス錐台の外接球にフィットする都合で 1 テクセルが 20cm 級になり、
    //   半径 5m の物でも 40 テクセル幅しかない。そこへフル解像度のメッシュを投げると
    //   1 テクセルあたり数トライアングルになる（実測: stress_5000 で影が GPU の 45%）。
    //   ★max を取るので、現在より細かい LOD になることは無い。1 テクセルが 3cm の
    //     cascade0 では tl が常に 0 側に出るので近景の絵は変わらない。
    const auto passLod = [&](const DrawItem& it) -> u32
    {
        u32 lod = it.lod + lodBias;
        if (cascadeTexelWorld > 0.0f)
        {
            const f32 texels = 2.0f * it.radius / cascadeTexelWorld;   // 影マップ上の直径
            const u32 tl = (texels <  32.0f) ? 4u
                         : (texels <  96.0f) ? 3u
                         : (texels < 288.0f) ? 2u
                         : (texels < 864.0f) ? 1u : 0u;
            lod = (std::max)(lod, tl);
        }
        return lod;
    };

    ID3D12PipelineState* lastPso    = nullptr;
    const Mesh*          lastVbMesh = nullptr;
    u32                  lastLod    = ~0u;

    const size_t itemCount = m_drawItems.size();
    const XMMATRIX passVpT = XMMatrixTranspose(viewProj);   // instanced VS の b0 用
    // 描画は単一スレッドなので関数ローカル static で使い回す。
    static std::vector<MeshInstanceData>     depthInstScratch;
    static std::vector<MeshInstancePrevData> depthInstPrevScratch;
    bool instOverflow = false;   // リングが尽きたら以降このパスは従来経路のみ

    for (size_t i = 0; i < itemCount; ++i)
    {
        const DrawItem& item = m_drawItems[i];

        // 半透明（カスタムシェーダ + shaderAlphaBlend）はカメラのプリパスから除外する。
        // ＝プリパスが半透明の深度を書いてしまうと、その裏の不透明が forward の LESS_EQUAL で
        //   弾かれて「ガラス越しにクリア色が見える」（00-COORDINATION §6 B3）。
        // 影パスでは skipTransparent=false のままなので、半透明の影は従来どおり落ちる。
        // 半透明は velocity も書かない（TAA は背後の不透明の速度で再投影する＝標準的な扱い）。
        if (skipTransparent && item.sortKey == 3u) continue;

        // ★RT サン影が有効なとき、CSM は「DXR の TLAS に入っていないもの」だけを描く。
        //   CSM と RT で担当を排他にしておかないと、フォワードの
        //     shadow = min(csmShadow, contactShadowTex)
        //   が暗い方を採るせいで CSM のアクネ（本来明るいのに縞状に暗い）と
        //   カスケード境界の段差が残ってしまう（RT 影を入れる意味の半分がここ）。
        //   判定は必ず IsRaytracedItem()（renderer/DrawItem.h）に一本化すること。
        if (skipRtCovered && IsRaytracedItem(item, m_rtSkinnedActiveThisFrame)) continue;

        // ★自動インスタンシング: 同一 batchKey の連続ランを 1 ドローに畳む。
        //   batchKey は item.lod 込みなので、このパスの lodBias を足しても run 内は同一 LOD
        //   だった。**テクセル基準の LOD を足した今はそうではない**: batchKey に item.radius は
        //   入っていないので、同じメッシュでもワールドスケールが違えば passLod が割れる。
        //   ラン条件に passLod の一致を足さないと、先頭要素の LOD で全インスタンスを描く
        //   （＝スケールの違う物の影の形が変わる）。
        if (instPSO && m_instancingEnabled && item.batchKey != 0 && !instOverflow)
        {
            depthInstScratch.clear();
            depthInstPrevScratch.clear();
            const u32 runLod = passLod(item);
            size_t j = i;
            for (; j < itemCount && m_drawItems[j].batchKey == item.batchKey
                                 && passLod(m_drawItems[j]) == runLod; ++j)
            {
                XMMATRIX w = XMLoadFloat4x4(&m_drawItems[j].world);
                if (!frustum.SphereVisible(XMLoadFloat3(&m_drawItems[j].center), m_drawItems[j].radius)) { ++m_statCulled; continue; }
                XMMATRIX t = XMMatrixTranspose(w);
                MeshInstanceData d;
                XMStoreFloat4(&d.r0, t.r[0]);
                XMStoreFloat4(&d.r1, t.r[1]);
                XMStoreFloat4(&d.r2, t.r[2]);
                d.color = {1.0f, 1.0f, 1.0f, 1.0f};
                // ★不変条件: depthInstScratch と depthInstPrevScratch は必ず同じ要素数・同じ順序。
                //   同じ base オフセットへ別々のストライドで memcpy し、slot1/slot2 として
                //   同じインスタンスに割り当てられるため。**この2つの push_back の間に
                //   early-continue を挟むと無言で対応がズレる**（前フレームのワールド行列が
                //   別のインスタンスに付き、そのオブジェクトだけ盛大にゴーストする）。
                depthInstScratch.push_back(d);
                if (velocityMode)
                {
                    XMMATRIX pt = XMMatrixTranspose(XMLoadFloat4x4(&m_drawItems[j].prevWorld));
                    MeshInstancePrevData p;
                    XMStoreFloat4(&p.p0, pt.r[0]);
                    XMStoreFloat4(&p.p1, pt.r[1]);
                    XMStoreFloat4(&p.p2, pt.r[2]);
                    depthInstPrevScratch.push_back(p);
                }
            }
            const u32 n = static_cast<u32>(depthInstScratch.size());
            if (n == 0) { i = j - 1; continue; }

            if (m_instanceCursor + n <= kMaxInstances)
            {
                const u32 base = m_instanceCursor;
                std::memcpy(m_instanceMapped[frameIndex] + static_cast<size_t>(base) * sizeof(MeshInstanceData),
                            depthInstScratch.data(), static_cast<size_t>(n) * sizeof(MeshInstanceData));
                if (velocityMode)
                    std::memcpy(m_instancePrevMapped[frameIndex] + static_cast<size_t>(base) * sizeof(MeshInstancePrevData),
                                depthInstPrevScratch.data(), static_cast<size_t>(n) * sizeof(MeshInstancePrevData));
                m_instanceCursor += n;

                if (instPSO->Get() != lastPso)
                {
                    m_commandList->SetPipelineState(*instPSO);
                    lastPso = instPSO->Get();
                }
                if (velocityMode)
                {
                    // batchKey にマテリアルが含まれているので、バッチ単位で 1 回貼れば正しい。
                    float rough = 0.5f, metal = 0.0f;
                    resolvePbr(*item.renderer, item.renderer->meshes[0], 0, rough, metal);

                    VelocityInstObjCB vc;
                    vc.vpJ       = passVpT;                            // transpose(jittered VP)
                    vc.prevVp    = XMMatrixTranspose(prevViewProj);    // transpose(prev non-jittered VP)
                    vc.jitter    = jitterNdc;
                    vc.matPacked = packMaterial(rough, metal);
                    vc.pad       = 0.0f;
                    m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, kVelocityInstObjNum32, &vc);
                }
                else
                {
                    m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 16, &passVpT);
                }

                const Mesh* mesh = item.renderer->meshes[0];
                const u32   lodI = runLod;   // ラン条件で全要素が同一と保証済み
                m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                m_commandList->SetIndexBuffer(mesh->GetIndexBufferLod(lodI).GetView());
                D3D12_VERTEX_BUFFER_VIEW iv = m_instanceVbView[frameIndex];
                iv.BufferLocation += static_cast<u64>(base) * sizeof(MeshInstanceData);
                iv.SizeInBytes     = n * sizeof(MeshInstanceData);
                m_commandList->GetNative()->IASetVertexBuffers(1, 1, &iv);
                if (velocityMode)
                {
                    D3D12_VERTEX_BUFFER_VIEW pv = m_instancePrevVbView[frameIndex];
                    pv.BufferLocation += static_cast<u64>(base) * sizeof(MeshInstancePrevData);
                    pv.SizeInBytes     = n * sizeof(MeshInstancePrevData);
                    m_commandList->GetNative()->IASetVertexBuffers(2, 1, &pv);
                }
                lastVbMesh = mesh;
                lastLod    = lodI;

                const u32 idx = mesh->GetIndexCountLod(lodI);
                m_commandList->DrawIndexedInstanced(idx, n);
                ++m_statDraws;
                m_statTris += idx / 3 * n;
                ++m_passBucket->draws;
                m_passBucket->tris += idx / 3 * n;

                i = j - 1;
                continue;
            }
            instOverflow = true;   // 以降は従来経路（見た目は同じ）
        }

        XMMATRIX world = XMLoadFloat4x4(&item.world);
        if (!frustum.SphereVisible(XMLoadFloat3(&item.center), item.radius)) { ++m_statCulled; continue; }
        const u32 lod = passLod(item);   // Mesh 側で最終LODへクランプ

        if (item.skin)
        {
            if (updateSkinning)
            {
                auto& skelAnim = reg.get<SkeletalAnimation>(item.e);
                skelAnim.skinningBuffer->Update(skelAnim.animator->GetSkinningMatrices(), frameIndex);
            }
            if (skinnedPSO.Get() != lastPso)
            {
                m_commandList->SetPipelineState(skinnedPSO);
                lastPso = skinnedPSO.Get();
            }
            m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                m_srvHeap->GetGpuHandle(item.skin->GetSrvIndex(frameIndex)));
            if (velocityMode)
            {
                // SkinningBuffer は frameCount 枚を多重化していて毎フレーム frameIndex の
                // スロットだけを書く＝前フレームのスロットに前フレームの行列がそのまま残っている。
                // 履歴が無い初回は現フレームを前としても使う（＝速度0）。
                const u32 prevSlot = m_prevFrameIndexValid ? m_prevFrameIndex : frameIndex;
                m_commandList->SetSRVTable(RootSignature::kSlotPrevBonesSRV,
                    m_srvHeap->GetGpuHandle(item.skin->GetSrvIndex(prevSlot)));
            }
        }
        else if (staticPSO.Get() != lastPso)
        {
            m_commandList->SetPipelineState(staticPSO);
            lastPso = staticPSO.Get();
        }

        const MeshRenderer& renderer = *item.renderer;
        for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
        {
            const auto* mesh = renderer.meshes[mi];

            XMMATRIX meshWorld = world;
            if (item.hasNodeAnim && mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
            {
                XMMATRIX nodeMat = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]);
                meshWorld = nodeMat * world;
            }

            if (velocityMode)
            {
                // 前フレームのメッシュワールド。ノードアニメ(meshNodeTransforms)は現在値しか
                // 持っていないので、そのメッシュの速度は「親のワールドの動き」だけの近似になる
                // （既知の制限。精密にやるなら MeshRenderer に prevMeshNodeTransforms が要る）。
                XMMATRIX prevMeshWorld = XMLoadFloat4x4(&item.prevWorld);
                if (item.hasNodeAnim && mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
                    prevMeshWorld = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]) * prevMeshWorld;

                float rough = 0.5f, metal = 0.0f;
                resolvePbr(renderer, mesh, mi, rough, metal);

                VelocityObjCB vc;
                vc.mvpJ    = XMMatrixTranspose(meshWorld * viewProj);   // viewProj はジッタ込み
                fillPrevCols(vc, XMMatrixTranspose(prevMeshWorld * prevViewProj)); // 前は非ジッタ
                fillNormalRows(vc, XMMatrixTranspose(meshWorld));
                vc.jitterX   = jitterNdc.x;
                vc.jitterY   = jitterNdc.y;
                vc.matPacked = packMaterial(rough, metal);
                m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, kVelocityObjNum32, &vc);
            }
            else
            {
                struct PerObjectData { XMMATRIX mvp; XMMATRIX mdl; } objData;
                objData.mvp = XMMatrixTranspose(meshWorld * viewProj);
                objData.mdl = XMMatrixTranspose(meshWorld);
                m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);
            }

            if (mesh != lastVbMesh || lod != lastLod)
            {
                m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                m_commandList->SetIndexBuffer(mesh->GetIndexBufferLod(lod).GetView());
                lastVbMesh = mesh;
                lastLod    = lod;
            }
            m_commandList->DrawIndexedInstanced(mesh->GetIndexCountLod(lod));
            ++m_statDraws;
            m_statTris += mesh->GetIndexCountLod(lod) / 3;
            ++m_passBucket->draws;
            m_passBucket->tris += mesh->GetIndexCountLod(lod) / 3;
        }
    }
}

void Application::ComputeCascades(const DirectX::XMVECTOR& lightDir, f32 camNear, f32 camFar)
{
    using namespace DirectX;
    const u32 N = kNumCascades;

    // CSM は透視前提（スライス錐台を XMMatrixPerspectiveFovLH で復元する）。
    // 編集2Dビュー等でカメラが正射になると錐台復元が破綻し影が崩れるため、
    // 正射時は CSM を無効化（無影フォールバック）する。
    // cascadeViewProj=identity + cascadeSplitsView=巨大正値 で、PS の SelectCascade は
    // 必ず cascade0 を返し、identity 変換で UV が [0,1] 外へ出て SampleCascade が 1.0(無影)。
    // シーンで影を無効化した場合も同じ無影センチネルを書く＝シェーダは全面ライト(黒画面にならない)。
    const bool shadowsOff = !(m_scene && m_scene->GetShadowsEnabled());
    if (m_camera->IsOrthographic() || shadowsOff)
    {
        XMMATRIX id = XMMatrixIdentity();
        for (u32 i = 0; i < N; ++i)
        {
            XMStoreFloat4x4(&m_cascadeViewProj[i], id);
            m_cascadeSplitsView[i] = 1e9f;  // 全成分を遠端 → cascade0 固定
            m_cascadeRadius[i]     = 0.0f;  // テクセル基準の LOD/棄却を無効化
        }
        return;
    }

    const f32 camFovY   = m_camera->GetFovY();
    const f32 camAspect = m_camera->GetAspect();

    // 1) 分割距離（対数 × 一様の混合, lambda）
    f32 splits[kNumCascades];
    f32 range = camFar - camNear;
    f32 ratio = camFar / (std::max)(camNear, 1e-4f);
    for (u32 i = 0; i < N; ++i)
    {
        f32 p    = (i + 1) / static_cast<f32>(N);
        f32 logS = camNear * std::pow(ratio, p);
        f32 uniS = camNear + range * p;
        splits[i] = m_cascadeSplitLambda * logS + (1.0f - m_cascadeSplitLambda) * uniS;
        m_cascadeSplitsView[i] = splits[i];  // PS のカスケード選択に渡す view 深度（正値）
    }

    // 2) カメラビュー（スライス錐台の隅を world へ戻すのに使用）
    XMMATRIX camView = m_camera->GetViewMatrix();

    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    // 退化回避: lightDir が up とほぼ平行なら up を Z 軸へ
    if (fabsf(XMVectorGetX(XMVector3Dot(lightDir, up))) > 0.99f)
        up = XMVectorSet(0, 0, 1, 0);

    f32 prevSplit = camNear;
    for (u32 ci = 0; ci < N; ++ci)
    {
        f32 n = prevSplit, f = splits[ci];

        // スライス専用の透視投影で NDC 隅 → world
        XMMATRIX sliceProj = XMMatrixPerspectiveFovLH(camFovY, camAspect, n, f);
        XMMATRIX invVP     = XMMatrixInverse(nullptr, camView * sliceProj);

        const XMVECTOR ndc[8] = {
            {-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, 1, 0, 1},
            {-1, -1, 1, 1}, {1, -1, 1, 1}, {-1, 1, 1, 1}, {1, 1, 1, 1}};
        XMVECTOR corners[8];
        XMVECTOR center = XMVectorZero();
        for (int k = 0; k < 8; ++k)
        {
            XMVECTOR w = XMVector4Transform(ndc[k], invVP);
            w = XMVectorScale(w, 1.0f / XMVectorGetW(w));
            corners[k] = w;
            center = XMVectorAdd(center, w);
        }
        center = XMVectorScale(center, 1.0f / 8.0f);

        // 包む球半径（回転不変＝テクセルスイム抑制）
        f32 radius = 0.0f;
        for (int k = 0; k < 8; ++k)
            radius = (std::max)(radius,
                XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[k], center))));
        radius = std::ceil(radius * 16.0f) / 16.0f;
        m_cascadeRadius[ci] = radius;   // 1 テクセル = 2*radius / m_shadowMapSize [m]

        // ライトビュー: 球中心から -lightDir 方向へ後退
        XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(lightDir, radius));
        XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);

        // タイトフィット正射（球で対称）
        XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            -radius, radius, -radius, radius, 0.0f, radius * 2.0f);

        // テクセルスナップ（シャドウのちらつき防止）
        XMMATRIX shadowVP = lightView * lightProj;
        XMVECTOR origin   = XMVector4Transform(XMVectorSet(0, 0, 0, 1), shadowVP);
        origin = XMVectorScale(origin, static_cast<f32>(m_shadowMapSize) / 2.0f);
        XMVECTOR rounded = XMVectorRound(origin);
        XMVECTOR offset  = XMVectorScale(XMVectorSubtract(rounded, origin),
                                         2.0f / static_cast<f32>(m_shadowMapSize));
        offset = XMVectorSetZ(XMVectorSetW(offset, 0.0f), 0.0f);
        XMMATRIX snap = XMMatrixTranslationFromVector(offset);
        shadowVP = shadowVP * snap;

        XMStoreFloat4x4(&m_cascadeViewProj[ci], shadowVP);  // 行優先（world*VP 用、非転置）
        prevSplit = f;
    }
}

void Application::Render()
{
    using namespace DirectX;

    // シーンの Skybox 設定 → ランタイム値を毎フレーム引き直す。
    // 以前は「Skybox / IBL 窓を開いている間」と再ベイク時しか同期しておらず、
    // ライティング・パネル / Lua の scene:setSkybox / MCP set_scene_settings で
    // 強度や背景 ON/OFF を変えても再ベイクするまで絵に出なかった（値の反映だけなので安い）。
    if (m_scene)
    {
        const auto& skyNow = m_scene->GetSkyboxSettings();
        m_iblIntensity    = skyNow.iblIntensity;
        m_skyboxIntensity = skyNow.skyboxIntensity;
        m_drawSkybox      = skyNow.drawSkybox;
    }
    // ライティング・パネルの「環境マップを適用 / 再ベイク」要求（envMapPath を変えた時だけ必要）
    if (m_editorCtx && m_editorCtx->pendingSkyboxRebake)
    {
        m_editorCtx->pendingSkyboxRebake = false;
        m_loadedSkyboxPath.clear();   // 旧パス一致による再ロードスキップを防ぐ
        m_skyboxDirty = true;
    }

    // Skybox 再ベイク要求（エディタでパス変更時）。専用 cmdList + WaitIdle で安全に処理。
    if (m_skyboxDirty && m_iblBaker)
    {
        m_skyboxDirty = false;
        m_commandQueue->WaitIdle();   // 前フレームの GPU 完了を待ってから SRV を入れ替える
        auto* bakeCmd = m_frameResources->BeginFrame(*m_commandQueue);
        LoadSkyboxIfNeeded(bakeCmd);
        ThrowIfFailed(bakeCmd->Close());
        m_commandQueue->ExecuteCommandList(bakeCmd);
        m_commandQueue->WaitIdle();
        m_frameResources->EndFrame(*m_commandQueue);
        if (m_envCubeTex) m_envCubeTex->FinishUpload();
        m_resourceManager->FinishUploads();
    }

    // フェンス待ち時間を計測（GPU がフレームを消化しきれていないとここが伸びる＝GPUバウンドの指標）
    const auto perfFenceT0 = std::chrono::high_resolution_clock::now();
    // ★ここが長い = GPU バウンド（CPU が GPU の完了を待っている）。
    //   Update/記録より長いなら CPU 最適化をしても FPS は上がらない、の判断材料。
    auto* nativeCmdList = [&] {
        DX12_PROFILE_ZONE_N("Wait/Fence");
        return m_frameResources->BeginFrame(*m_commandQueue);
    }();
    m_perfFenceWaitMs = std::chrono::duration<f32, std::milli>(
        std::chrono::high_resolution_clock::now() - perfFenceT0).count();
    m_commandList->Wrap(nativeCmdList);

    // GPU パス計測: このスロットの前回結果（約3フレーム前）を読み戻してから今フレームの計測開始
    static_assert(GpuTimer::Count <= kPerfGpuScopes, "kPerfGpuScopes を GpuTimer::Count に合わせて広げること");
    m_gpuTimer->NewFrame(m_swapChain->GetCurrentBackBufferIndex());
    m_gpuTimer->Begin(nativeCmdList, GpuTimer::Total);

    // マテリアルアセット(.dxmat)のホットリロード監視。エディタのみ(内部で0.5秒間隔にスロットリング)。
    if (!m_isGameMode && m_materialAssetManager)
        m_materialAssetManager->PollHotReload(m_gameClock.GetDeltaTime(), nativeCmdList);
    // 地形レイヤーセット(.terrainlayers)のホットリロード監視（同上）。
    if (!m_isGameMode && m_terrainLayerSets)
        m_terrainLayerSets->PollHotReload(m_gameClock.GetDeltaTime(), nativeCmdList);

    // Deferred: new scene（描画前に処理しないと GPU リソース解放でクラッシュする）
    // 未保存なら先に確認する（保存 / 破棄 / 取り消し）。取り消されたら要求ごと捨てる。
    // モーダル表示中は pending を残して次フレームへ持ち越す。
    bool newSceneAllowed = false;
    if (m_editorCtx->pendingNewScene && m_engineMode == EngineMode::Editor)
    {
        bool cancelled = false;
        newSceneAllowed = m_editorCtx->pendingNewSceneSkipConfirm || ConfirmDiscardScene(cancelled);
        if (cancelled) m_editorCtx->pendingNewScene = false;
    }
    if (newSceneAllowed)
    {
        m_editorCtx->pendingNewScene        = false;
        m_editorCtx->pendingNewSceneSkipConfirm = false;
        // プロジェクト新規作成の初期シーンなら保存先が指定されている
        std::string starterPath = std::move(m_editorCtx->pendingNewScenePath);
        m_editorCtx->pendingNewScenePath.clear();
        m_editorCtx->ClearSelection();
        m_editorCtx->undoSystem.Clear();
        // 新規シーンは「作った直後」を基準にする（保存先が指定されていればこの後 Save される）
        MarkSceneClean();
        m_editorCtx->currentScenePath = starterPath;  // 空なら未保存の新規シーン
        m_scene->Clear();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        // ---- 再生に必要な最低限のデフォルト配置 ----
        m_scene->SpawnPlane("Grid", {0, 0, 0}, kEditorGridSize, true);   // エディタ用グリッド
        m_scene->SpawnPlane("Ground", {0, 0, 0}, 20.0f, false); // 実体のある床
        m_scene->SpawnBox("Cube", {0, 0.5f, 0}, {0, 0, 0}, {1, 1, 1}); // サンプルオブジェクト
        {
            auto& reg = m_scene->GetRegistry();
            // 平行光源
            auto lightE = reg.create();
            reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
            reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
            reg.emplace<DirectionalLight>(lightE);

            // メインカメラ（CameraComponent が無いと Play で映らないため必須）
            auto camE = reg.create();
            reg.emplace<NameTag>(camE, NameTag{"MainCamera"});
            reg.emplace<Transform>(camE, Transform{{0.0f, 6.0f, -12.0f}, {22.0f, 0.0f, 0.0f}, {1,1,1}});
            CameraComponent cam;
            cam.isActive = true;
            reg.emplace<CameraComponent>(camE, cam);
        }
        if (!starterPath.empty())
            m_currentSceneRel = ToAssetRel(starterPath);
        // 作成と同時にシーンファイルを保存
        if (!m_editorCtx->currentScenePath.empty())
        {
            if (SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir()))
            {
                MarkSceneClean();
                ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
                m_editorCtx->hotReloadFlash = 1.5f;
            }
            else
            {
                // 新規シーンのファイルを作れなかった。緑の Saved を出すと
                // 「作られている」と誤解したまま作業が進む。
                Logger::Error("新規シーンを保存できませんでした: {}", m_editorCtx->currentScenePath);
                m_editorCtx->saveErrorFlash = 6.0f;
            }
        }
        m_editorLayer->RefreshAssetBrowser();
        // 新シーンの SkyboxSettings で IBL/skybox を次フレーム冒頭に再ベイク。
        // m_loadedSkyboxPath をクリアして「偶然旧パス一致でスキップ」の取りこぼしを防ぐ。
        m_loadedSkyboxPath.clear();
        m_skyboxDirty = true;
        ++m_sceneGeneration;   // 古い entity id を無効化(MCP の STALE_SCENE 検出用)
        m_mcpIdempotency.clear();   // 別シーンの entity を idempotentReplay で誤返却しないようクリア
        Logger::Info("New scene created");
    }

    // Deferred: scene load（描画前に処理）
    // 参照アセットが多いシーンはここで一気に読まず、段階ロードのジョブへ積む。
    // （同期のまま読むとメッセージポンプが止まり「エンジンが固まった」ように見える）
    // オートセーブ復旧の選択を消化する（モーダルは ToolbarPanel が描く）。
    if (m_editorCtx->autosaveChoice != EditorContext::AutosaveChoice::None)
    {
        const auto choice = m_editorCtx->autosaveChoice;
        m_editorCtx->autosaveChoice       = EditorContext::AutosaveChoice::None;
        m_editorCtx->showAutosaveRecovery = false;

        const std::string dir = AutosaveDir();
        if (choice == EditorContext::AutosaveChoice::Restore)
        {
            // 本体ではなくオートセーブを読み込む。読み終わったら FinishSceneLoad が
            // currentScenePath を本来のシーンへ戻す（m_autosaveRestoreTarget）。
            m_autosaveRestoreTarget             = m_editorCtx->currentScenePath;
            m_editorCtx->pendingLoadPath        = dir + "scene.json";
            m_editorCtx->pendingLoadSkipConfirm = true;   // 本人が選んだ操作。二重に聞かない
        }
        else
        {
            // 破棄。消しておかないと次回起動でも同じことを聞き続ける。
            std::error_code ec;
            std::filesystem::remove(dir + "scene.json", ec);
            std::filesystem::remove(dir + "meta.json", ec);
            Logger::Info("自動保存を破棄しました");
        }
    }

    // 未保存なら先に確認する（別シーンを開くと今の変更は完全に消える）。
    bool loadAllowed = false;
    if (!m_editorCtx->pendingLoadPath.empty() && m_engineMode == EngineMode::Editor
        && !m_sceneLoadJob)
    {
        bool cancelled = false;
        loadAllowed = m_editorCtx->pendingLoadSkipConfirm || ConfirmDiscardScene(cancelled);
        if (cancelled)
        {
            m_editorCtx->pendingLoadPath.clear();
            m_editorCtx->pendingLoadSkipConfirm = false;
            // 保険: 誰かが MCP の遅延応答を積んだままここへ来たら、必ず失敗を返して解放する。
            // 応答を宙に浮かせると m_mcpLoadReply が残り、以後 open_scene が
            // 「already in progress」で永久に失敗する（設計を間違えて一度踏んだ）。
            if (m_mcpLoadReply.client != 0)
            {
                FailMcp(m_mcpBridge.get(), m_mcpLoadReply, McpErr::ModeConflict,
                        "scene load cancelled by the unsaved-changes prompt");
                m_mcpLoadReply = {};
            }
        }
    }
    if (loadAllowed)
    {
        std::string loadPath = std::move(m_editorCtx->pendingLoadPath);
        m_editorCtx->pendingLoadPath.clear();
        m_editorCtx->pendingLoadSkipConfirm = false;

        BeginSceneLoadJob(loadPath, ToAssetRel(loadPath), false);
        if (!m_sceneLoadJob)   // 軽いシーンは従来どおり即ロード
            FinishSceneLoad(loadPath, ToAssetRel(loadPath), false, nativeCmdList);
    }

    // Lua preloadScene: 次シーンが参照するテクスチャ/モデルをキャッシュへ先読み
    // (シーン切替はしない。トランジション中間点の同期ロードを軽くしてカクつきを消す)
    if (!m_pendingScenePreloads.empty() && nativeCmdList)
    {
        for (const auto& rel : m_pendingScenePreloads)
            DoScenePreload(rel, nativeCmdList);
        m_pendingScenePreloads.clear();
    }

    // Play 中のシーン切替（Lua loadScene/nextScene、またはトランジション中間点）
    {
        // トランジションが中間点に達したら、保留中のターゲットをロード対象にする
        if (m_sceneTransition && m_sceneTransition->ConsumeHalfway() && !m_transitionTargetScene.empty())
        {
            m_editorCtx->pendingGameLoadPath = m_transitionTargetScene;
            m_transitionTargetScene.clear();
        }

        if (!m_editorCtx->pendingGameLoadPath.empty() && m_engineMode == EngineMode::Playing)
        {
            if (!m_sceneLoadJob)   // ロード中なら消化後に拾う
            {
                std::string rel = std::move(m_editorCtx->pendingGameLoadPath);
                m_editorCtx->pendingGameLoadPath.clear();
                // 重いシーンは段階ロード（ローディング UI を出しながら数フレームに分ける）
                BeginSceneLoadJob(PathResolver::AssetsDir() + rel, rel, true);
                if (!m_sceneLoadJob)
                    DoRuntimeSceneLoad(rel, nativeCmdList);
            }
        }
        else if (!m_editorCtx->pendingGameLoadPath.empty())
        {
            // Play 中でなければ無視（誤発火防止）
            m_editorCtx->pendingGameLoadPath.clear();
        }
    }

    // 段階ロードのジョブを1フレームぶん進める（先読み完了で実体化まで済ませる）。
    UpdateSceneLoadJob(nativeCmdList);

    // ネットワーク複製: サーバーが RequestSpawn した(またはクライアントが Spawn パケットを
    // 受信した)エンティティをフレーム境界で実体化する。InstantiatePrefab はモデルロードに
    // 現在フレームの cmdList が要るため net:spawn 呼び出しの場では即時実行できない。
    if (m_networkSystem && m_engineMode == EngineMode::Playing)
    {
        auto netSpawns = m_networkSystem->ConsumePendingSpawns();
        if (!netSpawns.empty())
        {
            m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                                m_srvHeap.get(), nativeCmdList);
            auto& netReg = m_scene->GetRegistry();
            for (auto& sp : netSpawns)
            {
                entt::entity root = SceneSerializer::InstantiatePrefab(
                    *m_scene, PathResolver::AssetsDir() + sp.prefabPath, PathResolver::AssetsDir());
                if (root == entt::null)
                {
                    Logger::Warn("ネットワークスポーン失敗(プレハブ読込エラー): {}", sp.prefabPath);
                    continue;
                }
                if (netReg.all_of<Transform>(root))
                    netReg.get<Transform>(root).position = { sp.x, sp.y, sp.z };
                m_networkSystem->OnEntityInstantiated(sp.netId, sp.owner, root, netReg);
            }
        }
    }

    // エンティティ生成（前フレームのドラッグ&ドロップ等から遅延実行）
    if (!m_editorCtx->pendingSpawns.empty())
    {
        char dbgBuf[128];
        snprintf(dbgBuf, sizeof(dbgBuf), "[PendingSpawns] count=%zu mode=%s\n",
            m_editorCtx->pendingSpawns.size(),
            m_engineMode == EngineMode::Editor ? "Editor" : "Playing");
        OutputDebugStringA(dbgBuf);
    }
    if (!m_editorCtx->pendingSpawns.empty() && m_engineMode == EngineMode::Editor)
    {
        auto spawns = std::move(m_editorCtx->pendingSpawns);
        m_editorCtx->pendingSpawns.clear();

        // Scene の内部 cmdList を今フレームのものに更新
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        for (auto& req : spawns)
        {
            std::string name = std::filesystem::path(req.modelPath).stem().string();
            if (!req.name.empty()) name = req.name;   // MCP 等からの任意名で上書き

            // マーカー生成物は stem だと "__primitive_box__" がそのまま名前になるので人間向けへ
            if (req.name.empty())
            {
                if      (req.modelPath == "__primitive_box__")    name = "Box";
                else if (req.modelPath == "__primitive_sphere__") name = "Sphere";
                else if (req.modelPath == "__primitive_plane__")  name = "Plane";
                else if (req.modelPath == "__empty__")            name = "Empty";
                else if (req.modelPath == "__camera__")           name = "Camera";
                else if (req.modelPath == "__directional_light__") name = "DirectionalLight";
                else if (req.modelPath == "__point_light__")      name = "PointLight";
                else if (req.modelPath == "__spot_light__")       name = "SpotLight";
                else if (req.modelPath == "__particle_emitter__") name = "ParticleEmitter";
                else if (req.modelPath == "__trigger__")          name = "Trigger";
                else if (req.modelPath == "__decal__")            name = "Decal";
            }
            // 同名エンティティがいると Trigger / Lua の名前参照が区別できず、
            // コピペ時の参照リマップも誤った相手に付く。生成時点で連番ユニーク化する。
            {
                auto& reg = m_scene->GetRegistry();
                auto exists = [&](const std::string& n)
                {
                    for (auto [e, tag] : reg.view<const NameTag>().each())
                        if (tag.name == n) return true;
                    return false;
                };
                if (exists(name))
                {
                    std::string stem = name;
                    auto p = stem.rfind(" (");
                    if (p != std::string::npos && stem.back() == ')')
                        stem = stem.substr(0, p);
                    for (int i = 1; i < 1000; ++i)
                    {
                        std::string cand = stem + " (" + std::to_string(i) + ")";
                        if (!exists(cand)) { name = cand; break; }
                    }
                }
            }
            entt::entity spawnedEntity = entt::null;
            entt::entity mcpPrefabRoot = entt::null;          // prefab 経路の MCP 応答用ルート
            std::vector<entt::entity> mcpPrefabAll;           // prefab 経路の全 entity
            entt::entity mcpUiPrimary = entt::null;           // __ui_*__ 経路の MCP 応答用本体
            std::vector<entt::entity> mcpUiCreated;           // 同・生成された全 entity(ラベル子等)

            if (req.modelPath == "__primitive_box__")
            {
                auto e = m_scene->SpawnBox(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__primitive_sphere__")
            {
                auto e = m_scene->SpawnSphere(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__primitive_plane__")
            {
                auto e = m_scene->SpawnPlane(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__empty__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                // MCP の position 指定を反映（デフォルト Transform だと (0,0,0) 固定になってた）
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__camera__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                // 他にアクティブカメラがなければ自動で isActive=true
                bool hasActive = false;
                for (auto [oe, oc] : reg.view<const CameraComponent>().each())
                    if (oc.isActive) { hasActive = true; break; }
                reg.emplace<CameraComponent>(e, CameraComponent{60.0f, 0.1f, 1000.0f, !hasActive});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__directional_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e, Transform{req.position, {-30.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<DirectionalLight>(e, DirectionalLight{{0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__point_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<PointLight>(e, PointLight{{1.0f, 1.0f, 1.0f}, 1.0f, 10.0f});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__spot_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e, Transform{req.position, {-60.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<SpotLight>(e, SpotLight{});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__gimmick_spike__" || req.modelPath == "__gimmick_slide__" ||
                     req.modelPath == "__gimmick_wall__")
            {
                // ステージギミックのプリセット: 着色ボックス + Gimmick コンポーネント
                auto& reg = m_scene->GetRegistry();
                Gimmick g;
                const char* nm = "Gimmick";
                float sx = 2.0f, sy = 1.4f, sz = 1.2f, cr = 0.86f, cg = 0.16f, cb = 0.12f;
                if (req.modelPath == "__gimmick_spike__")
                {
                    nm = "Spike"; g.kind = 1; g.period = 3.6f; g.amplitude = 1.6f;
                    g.threshold = 0.5f; g.deadly = true;
                    sx = 2.0f; sy = 1.4f; sz = 1.2f; cr = 0.86f; cg = 0.16f; cb = 0.12f;
                }
                else if (req.modelPath == "__gimmick_slide__")
                {
                    nm = "SlideWall"; g.kind = 2; g.period = 5.0f; g.amplitude = 3.8f;
                    sx = 5.2f; sy = 1.5f; sz = 1.1f; cr = 0.72f; cg = 0.40f; cb = 0.14f;
                }
                else
                {
                    nm = "Wall"; g.kind = 0;
                    sx = 4.0f; sy = 1.6f; sz = 1.0f; cr = 0.30f; cg = 0.32f; cb = 0.40f;
                }
                auto e = m_scene->SpawnBox(nm, req.position);
                auto h = e.GetHandle();
                reg.get<Transform>(h).scale = {sx, sy, sz};
                if (auto* dev = m_scene->GetDevice())
                    if (auto* mr = reg.try_get<MeshRenderer>(h))
                        for (auto* mesh : mr->meshes)
                            if (mesh) mesh->SetVertexColor(*dev, cr, cg, cb, 1.0f);
                reg.emplace<Gimmick>(h, g);
                spawnedEntity = h;
            }
            else if (req.modelPath == "__particle_emitter__")
            {
                // 配置エフェクト: 空エンティティ + ParticleEmitter（エディタで即プレビュー表示）
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<ParticleEmitter>(e, ParticleEmitter{});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__trigger__")
            {
                // イベント範囲: 空エンティティ + Trigger（Inspector でアクションを組む）
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<Trigger>(e, Trigger{});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__decal__")
            {
                // 投影デカール: 空エンティティ + DecalComponent。
                // scale が投影ボックスなので、床向けに「広く薄く」を既定にする。
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{name});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 1.0f}});
                reg.emplace<DecalComponent>(e, DecalComponent{});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__ui_canvas__" || req.modelPath == "__ui_image__" ||
                     req.modelPath == "__ui_text__"   || req.modelPath == "__ui_button__" ||
                     req.modelPath == "__ui_slider__" || req.modelPath == "__ui_toggle__" ||
                     req.modelPath == "__ui_scrollview__")
            {
                // ゲーム内UI: Canvas はルートに単体生成。Image/Text/Button は
                // 「選択中エンティティが UI ツリー内（自身か祖先に UICanvas）ならその子 →
                //  無ければシーン最初の UICanvas の子 → UICanvas 不在なら自動生成して子」に配置。
                auto& reg = m_scene->GetRegistry();
                std::vector<entt::entity> created;      // 生成順（root 先頭）。複数生成時の Undo 用
                entt::entity uiPrimary = entt::null;    // ユーザーが要求した本体（選択用）

                auto makeUiEntity = [&](const std::string& entName, entt::entity parent)
                {
                    auto e = reg.create();
                    reg.emplace<NameTag>(e, NameTag{entName});
                    Transform t{};
                    t.parent = parent;
                    reg.emplace<Transform>(e, t);
                    created.push_back(e);
                    return e;
                };

                if (req.modelPath == "__ui_canvas__")
                {
                    auto e = makeUiEntity(req.name.empty() ? std::string("UICanvas") : req.name,
                                          entt::null);
                    reg.emplace<UICanvas>(e, UICanvas{});
                    uiPrimary = e;
                }
                else
                {
                    // 配置先の親を解決（MCP の明示指定 > 選択中の UI ツリー > 最初の Canvas > 自動生成）
                    entt::entity uiParent = entt::null;
                    if (req.parent != entt::null && reg.valid(req.parent))
                    {
                        uiParent = req.parent;
                    }
                    else
                    {
                        entt::entity it = m_editorCtx->selectedEntity;
                        for (int guard = 0; guard < 64 && it != entt::null && reg.valid(it); ++guard)
                        {
                            if (reg.all_of<UICanvas>(it))
                            {
                                uiParent = m_editorCtx->selectedEntity;   // 選択そのものの子にする
                                break;
                            }
                            const auto* t = reg.try_get<Transform>(it);
                            it = t ? t->parent : entt::null;
                        }
                    }
                    if (uiParent == entt::null)
                    {
                        auto canvasView = reg.view<const UICanvas>();
                        if (canvasView.begin() != canvasView.end())
                            uiParent = *canvasView.begin();
                    }
                    if (uiParent == entt::null)
                    {
                        auto c = makeUiEntity("UICanvas", entt::null);
                        reg.emplace<UICanvas>(c, UICanvas{});
                        uiParent = c;
                    }

                    if (req.modelPath == "__ui_image__")
                    {
                        auto e = makeUiEntity(req.name.empty() ? std::string("UIImage") : req.name,
                                              uiParent);
                        reg.emplace<UIRect>(e, UIRect{});
                        reg.emplace<UIImage>(e, UIImage{});
                        uiPrimary = e;
                    }
                    else if (req.modelPath == "__ui_text__")
                    {
                        auto e = makeUiEntity(req.name.empty() ? std::string("UIText") : req.name,
                                              uiParent);
                        UIRect r{};
                        r.offsetMin = {-120.0f, -25.0f};
                        r.offsetMax = { 120.0f,  25.0f};
                        reg.emplace<UIRect>(e, r);
                        reg.emplace<UIText>(e, UIText{});
                        uiPrimary = e;
                    }
                    else if (req.modelPath == "__ui_scrollview__")
                    {
                        // スクロールビュー: UIRect(枠) + UIScrollView + 薄い背景 UIImage。
                        // 子はこの下にぶら下げる(UIエディタの階層ツリーで D&D)。
                        auto e = makeUiEntity(req.name.empty() ? std::string("UIScrollView")
                                                               : req.name, uiParent);
                        UIRect r{};
                        r.offsetMin = {-160.0f, -120.0f};
                        r.offsetMax = { 160.0f,  120.0f};
                        reg.emplace<UIRect>(e, r);
                        UIImage bg{};
                        bg.color = {0.0f, 0.0f, 0.0f, 0.25f};   // 枠が分かる薄い背景
                        bg.cornerRadius  = 6.0f;
                        bg.raycastBlock  = false;               // 背面のボタンを邪魔しない
                        reg.emplace<UIImage>(e, bg);
                        reg.emplace<UIScrollView>(e, UIScrollView{});
                        uiPrimary = e;
                    }
                    else if (req.modelPath == "__ui_slider__")
                    {
                        // スライダー: UIRect(横長) + UISlider（見た目は自前描画なので UIImage 不要）
                        auto e = makeUiEntity(req.name.empty() ? std::string("UISlider") : req.name,
                                              uiParent);
                        UIRect r{};
                        r.offsetMin = {-140.0f, -14.0f};
                        r.offsetMax = { 140.0f,  14.0f};
                        reg.emplace<UIRect>(e, r);
                        reg.emplace<UISlider>(e, UISlider{});
                        uiPrimary = e;
                    }
                    else if (req.modelPath == "__ui_toggle__")
                    {
                        // トグル: UIRect(正方形の箱) + UIToggle + 右隣に子ラベル UIText
                        auto e = makeUiEntity(req.name.empty() ? std::string("UIToggle") : req.name,
                                              uiParent);
                        UIRect r{};
                        r.offsetMin = {-18.0f, -18.0f};
                        r.offsetMax = { 18.0f,  18.0f};
                        reg.emplace<UIRect>(e, r);
                        reg.emplace<UIToggle>(e, UIToggle{});
                        uiPrimary = e;

                        auto label = makeUiEntity("Label", e);
                        UIRect lr{};
                        lr.anchorMin = {1.0f, 0.5f};   // 箱の右端に添える
                        lr.anchorMax = {1.0f, 0.5f};
                        lr.offsetMin = {10.0f, -14.0f};
                        lr.offsetMax = {210.0f, 14.0f};
                        reg.emplace<UIRect>(label, lr);
                        UIText lt{};
                        lt.text  = "トグル";
                        lt.alignH = 0;   // 左寄せ
                        reg.emplace<UIText>(label, lt);
                    }
                    else   // __ui_button__
                    {
                        // ボタン一式: UIRect + UIImage + UIButton + 子に UIText（ラベル）
                        auto e = makeUiEntity(req.name.empty() ? std::string("UIButton") : req.name,
                                              uiParent);
                        UIRect r{};
                        r.offsetMin = {-110.0f, -32.0f};
                        r.offsetMax = { 110.0f,  32.0f};
                        reg.emplace<UIRect>(e, r);
                        UIImage img{};
                        img.color = {0.22f, 0.35f, 0.62f, 1.0f};   // ラベルが読める濃さの青系
                        img.cornerRadius = 8.0f;
                        reg.emplace<UIImage>(e, img);
                        reg.emplace<UIButton>(e, UIButton{});
                        uiPrimary = e;

                        auto label = makeUiEntity("Label", e);
                        UIRect lr{};
                        lr.anchorMin = {0.0f, 0.0f};   // 親ボタン全面にストレッチ
                        lr.anchorMax = {1.0f, 1.0f};
                        lr.offsetMin = {0.0f, 0.0f};
                        lr.offsetMax = {0.0f, 0.0f};
                        reg.emplace<UIRect>(label, lr);
                        UIText lt{};
                        lt.text = "ボタン";
                        reg.emplace<UIText>(label, lt);
                    }
                }

                // Undo: 単体は下の共通 SpawnEntityCommand、複数生成（Canvas 自動生成 / Button の
                // ラベル子）はサブツリー対応の SpawnPrefabCommand でまとめて積む
                if (created.size() == 1)
                {
                    spawnedEntity = created[0];
                }
                else if (!created.empty())
                {
                    if (uiPrimary != entt::null)
                        m_editorCtx->Select(uiPrimary);   // 自動生成 Canvas の下でも本体を選択状態に
                    m_editorCtx->undoSystem.PushCommand(
                        std::make_unique<SpawnPrefabCommand>(
                            m_scene.get(), PathResolver::AssetsDir(), created));
                }
                // MCP 応答用（単体でも複数でも「本体 + 生成された全 id」を返す）
                mcpUiPrimary = (uiPrimary != entt::null) ? uiPrimary
                             : (created.empty() ? entt::null : created[0]);
                mcpUiCreated = created;
            }
            else if (std::filesystem::path(req.modelPath).extension().string() == ".prefab")
            {
                // プレハブ（再利用テンプレート）を展開。子も含めてサブツリーごと生成する。
                // MCP spawn_prefab は assets 相対で来る(エディタUI経由は絶対)。相対のままだと
                // InstantiatePrefab のディスクフォールバックが CWD 基準になり開けないので絶対化する。
                std::string prefabPath = req.modelPath;
                if (std::filesystem::path(prefabPath).is_relative())
                    prefabPath = PathResolver::AssetsDir() + prefabPath;
                std::vector<entt::entity> all;
                entt::entity root = SceneSerializer::InstantiatePrefab(
                    *m_scene, prefabPath, PathResolver::AssetsDir(), &all);
                if (root != entt::null)
                {
                    auto& reg = m_scene->GetRegistry();
                    if (reg.all_of<Transform>(root))
                        reg.get<Transform>(root).position = req.position;

                    // UI エディタのキャンバスへドロップされた UI プレハブ:
                    // 親を指定の UI ノードにして、ドロップ位置（キャンバス px）へ移す。
                    // 矩形サイズはプレハブが持っているものを維持し、中心だけを合わせる。
                    if (req.placeInUiCanvas && req.parent != entt::null && reg.valid(req.parent))
                    {
                        if (reg.all_of<Transform>(root))
                            reg.get<Transform>(root).parent = req.parent;
                        if (auto* rect = reg.try_get<UIRect>(root))
                        {
                            // 親矩形の解決を通さずに済むよう、アンカーを左上に寄せて
                            // offset をキャンバス絶対座標として扱う（ドロップ直後の見た目を確定させる）
                            const f32 w = rect->offsetMax.x - rect->offsetMin.x;
                            const f32 h = rect->offsetMax.y - rect->offsetMin.y;
                            rect->anchorMin = {0.0f, 0.0f};
                            rect->anchorMax = {0.0f, 0.0f};
                            rect->offsetMin = {req.uiCanvasPos.x - w * 0.5f,
                                               req.uiCanvasPos.y - h * 0.5f};
                            rect->offsetMax = {rect->offsetMin.x + w, rect->offsetMin.y + h};
                        }
                    }
                    m_editorCtx->Select(root);
                    m_editorCtx->undoSystem.PushCommand(
                        std::make_unique<SpawnPrefabCommand>(
                            m_scene.get(), PathResolver::AssetsDir(), all));
                    Logger::Info("Prefab instantiated ({} entities): {}", all.size(), req.modelPath);
                    mcpPrefabRoot = root;          // MCP 応答にルート + 全 id を返す
                    mcpPrefabAll  = std::move(all);
                }
                // spawnedEntity は null のまま（独自に Undo を積んだので下の汎用 SpawnEntityCommand はスキップ）
            }
            else
            {
                // 拡張子で振り分ける。画像を Assimp（モデルローダ）に食わせると
                // インポータ総当たりでフリーズ→クラッシュするため、モデル拡張子のみ Spawn へ。
                std::string ext = std::filesystem::path(req.modelPath).extension().string();
                for (auto& ch : ext) if (ch >= 'A' && ch <= 'Z') ch += 32;   // 小文字化
                auto extIs = [&ext](std::initializer_list<const char*> list) {
                    for (const char* s : list) if (ext == s) return true;
                    return false;
                };

                if (extIs({".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".gif"}))
                {
                    // 画像 → ワールド空間の 2D スプライトとして配置（texturePath は assets 相対の規約）。
                    // D&D は絶対パス・MCP spawn_model は assets 相対で来るので両対応。
                    std::string relStr;
                    const std::filesystem::path inPath(req.modelPath);
                    if (inPath.is_absolute())
                    {
                        std::error_code rec;
                        auto rel = std::filesystem::relative(inPath, PathResolver::AssetsDir(), rec);
                        relStr = rec ? std::string() : rel.generic_string();
                    }
                    else
                    {
                        relStr = inPath.generic_string();
                    }
                    if (relStr.empty() || relStr.rfind("..", 0) == 0)
                    {
                        Logger::Warn("ドロップされた画像が assets/ の外にあるため、スプライトとして配置できません: {}",
                                     req.modelPath);
                    }
                    else
                    {
                        auto& reg = m_scene->GetRegistry();
                        auto e = reg.create();
                        reg.emplace<NameTag>(e, NameTag{name});
                        // 床(Y=0)ドロップだと Z ファイトするので、既定サイズ(1)の半分だけ持ち上げる
                        DirectX::XMFLOAT3 pos = req.position;
                        pos.y += 0.5f;
                        reg.emplace<Transform>(e, Transform{pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                        Sprite2D sp{};
                        sp.texturePath = relStr;
                        sp.worldSpace  = true;
                        sp.billboard   = false;   // 既定はTransformの回転に従う（ビルボードはInspectorでON可）
                        reg.emplace<Sprite2D>(e, sp);
                        spawnedEntity = e;
                        Logger::Info("Placed world sprite: {}", relStr);
                    }
                }
                else if (extIs({".gltf", ".glb", ".obj", ".fbx", ".dae", ".stl", ".ply", ".3ds"}))
                {
                    // MCP spawn_model は assets 相対で来る(D&D は絶対)。相対のままだと
                    // VfsIOSystem のディスクフォールバックが CWD 基準になり開けないので絶対化する。
                    std::string modelPath = req.modelPath;
                    if (std::filesystem::path(modelPath).is_relative())
                        modelPath = PathResolver::AssetsDir() + modelPath;
                    auto entity = m_scene->Spawn(name, modelPath, req.position);

                    // ★スケールは 1 のまま = ファイルが持っている実寸で置く（D&D も MCP も）。
                    //   昔ここに「AABB から自動スケーリング」と称して *無条件に* scale=0.01 を
                    //   入れる処理があった(maxExtent は計算するだけで捨てていた)。実体は
                    //   Fox.glb(約 155 単位)に合わせた決め打ちで、DamagedHelmet.glb(約 1.9m)は
                    //   2cm、1m の立方体は 1cm になっていた = 「モデルが極端に小さく読まれる」バグ。
                    //   単位の正規化は ModelLoader 側(FBX の cm→m)で済ませてあるので、
                    //   ここで見た目合わせをしてはいけない（オーサリングされた寸法が壊れる）。
                    //
                    //   ★ここに昔あった「glTF/glb は Z-up なので X 軸 90 度回転」も削除済み(B13)。
                    //     glTF 2.0 は仕様上 Y-up 固定で、assimp も軸変換を一切足さない。
                    //     Z-up でオーサリングされたファイル(CesiumMan.glb 等)は自分の中に
                    //     変換ノード("Z_UP")を持っており、それは ModelLoader が
                    //     BoneNode::preTransform として畳み込む。
                    spawnedEntity = entity.GetHandle();
                }
                else
                {
                    Logger::Warn("未対応のファイルがシーンにドロップされたためスキップしました: {} "
                                 "（モデル: gltf/glb/obj/fbx/dae/stl/ply/3ds, 画像: png/jpg/bmp/tga/dds）",
                                 req.modelPath);
                }
            }

            // Undo に Spawn コマンドを積む
            if (spawnedEntity != entt::null)
            {
                m_editorCtx->undoSystem.PushCommand(
                    std::make_unique<SpawnEntityCommand>(
                        m_scene.get(), PathResolver::AssetsDir(), spawnedEntity));
            }
            Logger::Info("Spawned: {}", name);

            // MCP create_entity / spawn_model / spawn_prefab の遅延応答(本物の entityId を返す)。
            if (req.mcp.client != 0)
            {
                auto& reg = m_scene->GetRegistry();
                if (mcpPrefabRoot != entt::null)
                {
                    nlohmann::json ids = nlohmann::json::array();
                    for (auto a : mcpPrefabAll) ids.push_back(static_cast<u32>(a));
                    CompleteMcp(m_mcpBridge.get(), req.mcp,
                        nlohmann::json{{"entityId", static_cast<u32>(mcpPrefabRoot)},
                                       {"rootEntityId", static_cast<u32>(mcpPrefabRoot)},
                                       {"entityIds", ids}, {"name", name},
                                       {"sceneGeneration", m_sceneGeneration}});
                    if (!req.mcp.idempotencyKey.empty())
                        m_mcpIdempotency[req.mcp.idempotencyKey] = static_cast<u32>(mcpPrefabRoot);
                }
                else if (mcpUiPrimary != entt::null && reg.valid(mcpUiPrimary))
                {
                    // UI 要素: 本体 id + 一緒に生成された全 id(自動 Canvas / ラベル子)を返す
                    nlohmann::json ids = nlohmann::json::array();
                    for (auto a : mcpUiCreated) ids.push_back(static_cast<u32>(a));
                    CompleteMcp(m_mcpBridge.get(), req.mcp,
                        nlohmann::json{{"entityId", static_cast<u32>(mcpUiPrimary)},
                                       {"entityIds", ids}, {"name", name},
                                       {"sceneGeneration", m_sceneGeneration}});
                    if (!req.mcp.idempotencyKey.empty())
                        m_mcpIdempotency[req.mcp.idempotencyKey] = static_cast<u32>(mcpUiPrimary);
                }
                else if (spawnedEntity != entt::null && reg.valid(spawnedEntity))
                {
                    CompleteMcp(m_mcpBridge.get(), req.mcp,
                        nlohmann::json{{"entityId", static_cast<u32>(spawnedEntity)},
                                       {"name", name}, {"sceneGeneration", m_sceneGeneration}});
                    if (!req.mcp.idempotencyKey.empty())
                        m_mcpIdempotency[req.mcp.idempotencyKey] = static_cast<u32>(spawnedEntity);
                }
                else
                {
                    FailMcp(m_mcpBridge.get(), req.mcp, McpErr::Internal,
                            "spawn failed (model load error? check dx12_get_log): " + req.modelPath);
                }
            }
        }
    }

    // ---- MCP 由来の地形 / スカルプト生成（GPU メッシュ構築に cmdList が要るのでフレーム境界）----
    // pendingSpawns と同じ流儀。生成後に本物の entityId を遅延応答で返す。
    if (!m_editorCtx->mcpProcCreates.empty() && m_engineMode == EngineMode::Editor)
    {
        auto procReqs = std::move(m_editorCtx->mcpProcCreates);
        m_editorCtx->mcpProcCreates.clear();

        // Scene の内部 cmdList を今フレームのものに更新（pendingSpawns と同じ理由）
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        auto& reg = m_scene->GetRegistry();

        for (const auto& req : procReqs)
        {
            Entity      created{};
            std::string err;

            if (req.kind == McpPendingProcCreate::Kind::Terrain)
            {
                Terrain tp;
                tp.resolution = req.resolution;
                tp.worldSize  = req.worldSize;
                tp.maxHeight  = req.maxHeight;
                created = m_scene->SpawnTerrain(req.name, req.position, tp, nativeCmdList);
            }
            else if (req.kind == McpPendingProcCreate::Kind::Sculpt)
            {
                SculptMesh sp;   // meshPath 空 = 素体から作る
                created = m_scene->SpawnSculpt(req.name, req.position, sp,
                                               static_cast<SculptPrimitive>(req.primitive),
                                               req.subdivisions, req.size, nativeCmdList);
            }
            else   // SculptFromEntity: 既存モデルを「彫れるコピー」にする（元アセットは読むだけ）
            {
                if (!reg.valid(req.source))
                {
                    err = "source entity is gone (シーンを開き直した？ dx12_list_entities で取り直してくれ)";
                }
                else
                {
                    auto data = std::make_shared<SculptMeshData>();
                    if (!SculptPanel::MakeEditable(reg, req.source, *data))
                    {
                        err = "this model has no CPU vertex cache, so it cannot be made editable "
                              "(dx12_sculpt_create で素体から彫るか、別のモデルを使ってくれ)";
                    }
                    else
                    {
                        Transform         srcTf{};
                        DirectX::XMFLOAT3 pos{0.0f, 0.0f, 0.0f};
                        if (reg.all_of<Transform>(req.source))
                        {
                            srcTf = reg.get<Transform>(req.source);
                            pos   = srcTf.position;
                        }
                        SculptMesh sp;   // 素体は最小で作って直後に差し替える（SculptPanel と同じ手順）
                        created = m_scene->SpawnSculpt(req.name, pos, sp,
                                                       SculptPrimitive::Plane, 1u, 1.0f, nativeCmdList);
                        if (created.IsValid())
                        {
                            auto& sc = created.GetComponent<SculptMesh>();
                            sc._data = data;
                            sc.MarkDirty();
                            if (created.HasComponent<Transform>())
                            {
                                Transform&         tf          = created.GetComponent<Transform>();
                                const entt::entity keepParent = tf.parent;
                                tf        = srcTf;   // 元モデルと同じ姿勢へ（頂点はローカルのまま焼いてある）
                                tf.parent = keepParent;
                            }
                            m_scene->RebuildSculptMesh(created.GetHandle(), nativeCmdList);
                        }
                    }
                }
            }

            if (created.IsValid())
            {
                m_editorCtx->undoSystem.PushCommand(
                    std::make_unique<SpawnEntityCommand>(
                        m_scene.get(), PathResolver::AssetsDir(), created.GetHandle()));
                m_editorCtx->Select(created.GetHandle());
            }

            if (req.mcp.client != 0)
            {
                if (created.IsValid())
                {
                    const entt::entity ce = created.GetHandle();
                    nlohmann::json r{
                        {"entityId", static_cast<u32>(ce)},
                        {"name", reg.all_of<NameTag>(ce) ? reg.get<NameTag>(ce).name : req.name},
                        {"created", true},
                        {"sceneGeneration", m_sceneGeneration}};
                    if (req.kind == McpPendingProcCreate::Kind::Terrain)
                    {
                        const Terrain& t = reg.get<Terrain>(ce);
                        r["resolution"] = t.resolution;
                        r["worldSize"]  = t.worldSize;
                        r["maxHeight"]  = t.maxHeight;
                        r["note"] = "平坦な地形を作った(静的コライダー付き)。"
                                    "次は dx12_terrain_generate で山を作るか dx12_terrain_sculpt で彫る。";
                    }
                    else
                    {
                        const auto* sc = reg.try_get<SculptMesh>(ce);
                        r["vertexCount"]   = (sc && sc->_data) ? sc->_data->VertexCount()   : size_t{0};
                        r["triangleCount"] = (sc && sc->_data) ? sc->_data->TriangleCount() : size_t{0};
                        if (req.kind == McpPendingProcCreate::Kind::SculptFromEntity)
                            r["sourceEntityId"] = static_cast<u32>(req.source);
                        r["note"] = "dx12_sculpt_brush で彫る。当てる場所は dx12_pick / "
                                    "dx12_raycast_precise が返す worldPos をそのまま position に渡すのが確実。";
                    }
                    CompleteMcp(m_mcpBridge.get(), req.mcp, std::move(r));
                }
                else
                {
                    FailMcp(m_mcpBridge.get(), req.mcp, McpErr::Internal,
                            err.empty() ? "failed to create the procedural mesh (dx12_get_log を確認してくれ)"
                                        : err);
                }
            }
        }
    }

    // グループ化（Ctrl+G / 右クリック）: 選択をまとめる空の親を作ってぶら下げる。
    // 親は原点・無回転・スケール1なので子のワールド行列は変わらない＝見た目は一切動かない。
    if (m_editorCtx->pendingGroupSelection && m_engineMode == EngineMode::Editor)
    {
        m_editorCtx->pendingGroupSelection = false;
        auto& reg = m_scene->GetRegistry();

        // 祖先も一緒に選ばれている子は除く（親ごと動くので二重に付け替えない）
        auto ancestorSelected = [&](entt::entity e) {
            const auto* t = reg.try_get<Transform>(e);
            entt::entity cur = t ? t->parent : entt::null;
            for (int d = 0; cur != entt::null && reg.valid(cur) && d < 4096; ++d)
            {
                if (m_editorCtx->IsSelected(cur)) return true;
                const auto* pt = reg.try_get<Transform>(cur);
                cur = pt ? pt->parent : entt::null;
            }
            return false;
        };

        std::vector<std::pair<entt::entity, entt::entity>> members;   // (子, 元の親)
        for (entt::entity e : m_editorCtx->selectedEntities)
        {
            if (!reg.valid(e) || !reg.all_of<Transform>(e)) continue;
            if (reg.all_of<GridPlane>(e)) continue;      // 内部用グリッドは巻き込まない
            if (ancestorSelected(e)) continue;
            members.emplace_back(e, reg.get<Transform>(e).parent);
        }

        if (!members.empty())
        {
            // 元の親が全員同じならグループもそこへ入れる（階層の位置を保つ）
            const entt::entity commonParent = members.front().second;
            const bool sameParent = std::all_of(members.begin(), members.end(),
                [&](const auto& m) { return m.second == commonParent; });

            entt::entity group = reg.create();
            reg.emplace<NameTag>(group, NameTag{"Group"});
            Transform gt{};
            if (sameParent) gt.parent = commonParent;
            reg.emplace<Transform>(group, gt);

            for (const auto& [child, oldParent] : members)
            {
                (void)oldParent;
                reg.get<Transform>(child).parent = group;
            }

            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<GroupCommand>(&reg, "Group", group,
                                               sameParent ? commonParent : entt::null, members));
            m_editorCtx->Select(group);
            m_editorCtx->requestRenameEntity = group;   // その場で名前を入力させる
            Logger::Info("グループ化: {} 件をまとめました", members.size());
        }
    }

    // プレハブ書き出し（選択エンティティ + 子孫を assets/prefabs/<name>.prefab へ保存）
    if (m_editorCtx->pendingCreatePrefab != entt::null && m_engineMode == EngineMode::Editor)
    {
        entt::entity root = m_editorCtx->pendingCreatePrefab;
        m_editorCtx->pendingCreatePrefab = entt::null;

        auto& reg = m_scene->GetRegistry();
        if (reg.valid(root) && reg.all_of<NameTag>(root))
        {
            namespace fs = std::filesystem;
            std::string base = reg.get<NameTag>(root).name;
            if (base.empty()) base = "Prefab";
            // UI 要素は assets/prefabs/ui/ へ分ける。UIエディタのパレットがこの階層を直接読むので、
            // 3D プレハブと混ざらへんようにしておく。
            const bool isUi = reg.any_of<UIRect, UICanvas>(root);
            fs::path dir = fs::path(PathResolver::AssetsDir()) / "prefabs";
            if (isUi) dir /= "ui";
            std::error_code ec; fs::create_directories(dir, ec);
            fs::path file = dir / (base + ".prefab");
            for (int n = 1; fs::exists(file); ++n)
                file = dir / (base + " (" + std::to_string(n) + ").prefab");

            if (SceneSerializer::SavePrefab(*m_scene, root, file.string(), PathResolver::AssetsDir()))
            {
                // 作った元のエンティティを、そのプレハブの「インスタンス」に格上げする
                // （Unity と同じ挙動。以後この場で編集して「適用」で元へ戻せる）。
                std::string rel = fs::relative(file, fs::path(PathResolver::AssetsDir()), ec)
                                      .generic_string();
                if (ec) rel = file.filename().string();
                reg.emplace_or_replace<PrefabLink>(root, PrefabLink{rel});

                m_editorCtx->hotReloadFlash = 1.0f;   // 保存通知のフラッシュを流用
                Logger::Info("Prefab created: {}", file.string());
            }
        }
    }

    // プレハブのリンク操作（適用 / 元に戻す / 他インスタンスへ伝播）。
    // Revert はエンティティを作り直すので、他の pending* と同じくフレーム境界で実行する。
    if (m_engineMode == EngineMode::Editor && m_scene)
    {
        auto& reg = m_scene->GetRegistry();
        const std::string assets = PathResolver::AssetsDir();

        for (entt::entity e : m_editorCtx->pendingPrefabApply)
        {
            if (!reg.valid(e)) continue;
            // 適用は他インスタンスへの伝播まで含む（各インスタンスの手直しは 3-way マージで残る）。
            // 伝播は元に戻せないので Undo 履歴は汚さず、件数をログへ残して何が起きたか追えるようにする。
            int propagated = 0;
            if (SceneSerializer::ApplyPrefabInstance(*m_scene, e, assets, &propagated))
            {
                m_editorCtx->hotReloadFlash = 1.0f;
                Logger::Info("Prefab applied: {} (他 {} インスタンスへ反映)",
                             reg.get<PrefabLink>(e).sourcePath, propagated);
            }
        }
        m_editorCtx->pendingPrefabApply.clear();

        for (entt::entity e : m_editorCtx->pendingPrefabRevert)
        {
            if (!reg.valid(e) || !reg.all_of<PrefabLink>(e)) continue;

            // Undo 用に「戻す前の姿」と外部親を捕まえてから作り直す
            const std::string before = SceneSerializer::SerializeSubtree(*m_scene, e, assets);
            const entt::entity extParent =
                reg.all_of<Transform>(e) ? reg.get<Transform>(e).parent : entt::null;

            const entt::entity newRoot = SceneSerializer::RevertPrefabInstance(*m_scene, e, assets);
            if (newRoot == entt::null) continue;

            // 新サブツリーの全エンティティを集める（コマンドが Undo 時に消す対象）
            std::vector<entt::entity> after{newRoot};
            for (size_t head = 0; head < after.size(); ++head)
                for (auto [child, tf] : reg.view<const Transform>().each())
                    if (tf.parent == after[head]
                        && std::find(after.begin(), after.end(), child) == after.end())
                        after.push_back(child);

            m_editorCtx->undoSystem.PushCommand(std::make_unique<PrefabRevertCommand>(
                m_scene.get(), assets, before, after, extParent));
            m_editorCtx->Select(newRoot);   // 元の ID は消えたので選択を張り替える
        }
        m_editorCtx->pendingPrefabRevert.clear();

        for (entt::entity e : m_editorCtx->pendingPrefabPropagate)
        {
            if (!reg.valid(e) || !reg.all_of<PrefabLink>(e)) continue;
            const std::string src = reg.get<PrefabLink>(e).sourcePath;
            // 伝播は元に戻せない（多数のサブツリーを作り直すため）。Undo 履歴は汚さず、
            // 代わりに件数をログへ残して何が起きたか追えるようにする。
            const int n = SceneSerializer::RefreshPrefabInstances(*m_scene, src, assets, e);
            Logger::Info("Prefab propagated to {} instance(s): {}", n, src);
            m_editorCtx->hotReloadFlash = 1.0f;
        }
        m_editorCtx->pendingPrefabPropagate.clear();
    }

    // ファイルメニュー「プロジェクトを閉じる」→ ランチャーに戻す。
    // 「ランチャーに戻る」ボタン（RenderProjectWindow）と同じ遷移＝ファイル削除等は一切不要。
    if (m_editorCtx->pendingCloseProject)
    {
        // ランチャーへ戻ると EditorLayer も TerrainPanel も呼ばれなくなる＝確認モーダルを
        // 出す場所が無くなる。だから m_showLauncher を立てる「前」に必ず聞く。
        bool cancelled = false;
        if (ConfirmDiscardScene(cancelled))
        {
            m_editorCtx->pendingCloseProject = false;
            m_showLauncher = true;
        }
        else if (cancelled)
        {
            m_editorCtx->pendingCloseProject = false;
        }
    }

    // Undo/Redo（エンティティ復元がモデル再ロードを伴うため cmdList 有効時に実行）
    if ((m_editorCtx->pendingUndo || m_editorCtx->pendingRedo)
        && m_engineMode == EngineMode::Editor)
    {
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        if (m_editorCtx->pendingUndo) m_editorCtx->undoSystem.Undo();
        if (m_editorCtx->pendingRedo) m_editorCtx->undoSystem.Redo();
        m_editorCtx->pendingUndo = false;
        m_editorCtx->pendingRedo = false;
    }

    // エンティティ複製（Ctrl+D / 右クリック複製。全コンポーネントのディープコピー）
    if (!m_editorCtx->pendingDuplications.empty() && m_engineMode == EngineMode::Editor)
    {
        auto sources = std::move(m_editorCtx->pendingDuplications);
        m_editorCtx->pendingDuplications.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        m_editorCtx->ClearSelection();
        // 削除と同じ理由で 1 エントリに束ねる（5 個複製したら Ctrl+Z 1 回で 5 個消える）。
        auto dupBatch = std::make_unique<CompositeCommand>("Duplicate");
        for (auto src : SceneSerializer::TopmostRoots(*m_scene, sources))
        {
            std::vector<entt::entity> all;
            entt::entity copy = SceneSerializer::DuplicateSubtree(
                *m_scene, src, PathResolver::AssetsDir(), &all);
            if (copy == entt::null) continue;

            m_editorCtx->AddToSelection(copy);
            dupBatch->Add(
                std::make_unique<SpawnPrefabCommand>(
                    m_scene.get(), PathResolver::AssetsDir(), std::move(all)));
            Logger::Info("Duplicated entity: {}",
                         m_scene->GetRegistry().get<NameTag>(copy).name);
        }
        if (!dupBatch->Empty())
            m_editorCtx->undoSystem.PushCommand(std::move(dupBatch));
    }

    // Deferred: MCP entity deletion（サブツリー削除後に deletedCount を遅延応答で返す）。
    // ★エディタUIブランチの中ではなく Render トップレベルに置く(mcpDuplications と同格)。
    //   ランチャー表示中でも drain され、MCP の delete_entity が未応答ハングしない。
    if (!m_editorCtx->mcpDeletions.empty() && m_engineMode == EngineMode::Editor)
    {
        auto dels = std::move(m_editorCtx->mcpDeletions);
        m_editorCtx->mcpDeletions.clear();
        auto& reg = m_scene->GetRegistry();
        for (auto& d : dels)
        {
            const entt::entity root = d.entity;
            if (!reg.valid(root))
            {
                // 既に(先行サブツリー等で)削除済み。冪等に成功扱い(deletedCount=0)。
                CompleteMcp(m_mcpBridge.get(), d.mcp,
                    nlohmann::json{{"deletedEntityId", static_cast<u32>(root)},
                                   {"deletedCount", 0}, {"sceneGeneration", m_sceneGeneration}});
                continue;
            }
            // サブツリー収集（親→子の順。BFS）— 既存削除ブロックと同手順。
            std::vector<entt::entity> subtree{root};
            for (size_t i = 0; i < subtree.size(); ++i)
                for (auto [c, t] : reg.view<const Transform>().each())
                    if (t.parent == subtree[i]) subtree.push_back(c);

            std::vector<DeletedEntityRecord> records;
            records.reserve(subtree.size());
            entt::entity externalParent = reg.all_of<Transform>(root)
                ? reg.get<Transform>(root).parent : entt::null;
            for (auto e : subtree)
            {
                DeletedEntityRecord rec;
                rec.snapshot = SceneSerializer::SerializeEntity(*m_scene, e, PathResolver::AssetsDir());
                if (reg.all_of<Transform>(e))
                {
                    auto parent = reg.get<Transform>(e).parent;
                    auto it = std::find(subtree.begin(), subtree.end(), parent);
                    if (it != subtree.end())
                        rec.parentLocalIndex = static_cast<int>(it - subtree.begin());
                }
                records.push_back(std::move(rec));
            }
            const int deletedCount = static_cast<int>(subtree.size());
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<DeleteEntityCommand>(
                    m_scene.get(), PathResolver::AssetsDir(),
                    std::move(records), subtree, externalParent));
            for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
                if (reg.valid(*it)) m_scene->Remove(Entity(*it, &reg));

            CompleteMcp(m_mcpBridge.get(), d.mcp,
                nlohmann::json{{"deletedEntityId", static_cast<u32>(root)},
                               {"deletedCount", deletedCount},
                               {"sceneGeneration", m_sceneGeneration}});
        }
        // 削除で無効になった選択をクリーンアップ
        auto& sel = m_editorCtx->selectedEntities;
        sel.erase(std::remove_if(sel.begin(), sel.end(),
                  [&](entt::entity e) { return !reg.valid(e); }), sel.end());
        if (m_editorCtx->selectedEntity != entt::null && !reg.valid(m_editorCtx->selectedEntity))
            m_editorCtx->selectedEntity = sel.empty() ? entt::null : sel.back();
    }

    // Deferred: MCP entity duplication（複製先 entityId を遅延応答で返す）
    if (!m_editorCtx->mcpDuplications.empty() && m_engineMode == EngineMode::Editor)
    {
        auto dups = std::move(m_editorCtx->mcpDuplications);
        m_editorCtx->mcpDuplications.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        auto& reg = m_scene->GetRegistry();
        for (auto& d : dups)
        {
            if (!reg.valid(d.entity))
            {
                FailMcp(m_mcpBridge.get(), d.mcp, McpErr::NotFound, "source entity no longer valid");
                continue;
            }
            std::vector<entt::entity> all;
            entt::entity copy = SceneSerializer::DuplicateSubtree(*m_scene, d.entity, PathResolver::AssetsDir(), &all);
            if (copy == entt::null)
            {
                FailMcp(m_mcpBridge.get(), d.mcp, McpErr::Internal, "duplicate failed");
                continue;
            }
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<SpawnPrefabCommand>(m_scene.get(), PathResolver::AssetsDir(), std::move(all)));
            std::string nm = reg.all_of<NameTag>(copy) ? reg.get<NameTag>(copy).name : std::string();
            Logger::Info("Duplicated entity (MCP): {}", nm);
            CompleteMcp(m_mcpBridge.get(), d.mcp,
                nlohmann::json{{"entityId", static_cast<u32>(copy)}, {"name", nm},
                               {"sceneGeneration", m_sceneGeneration}});
        }
    }

    // エンティティペースト（Ctrl+V。コピー時の JSON スナップショットから生成）
    if (!m_editorCtx->pendingPastes.empty() && m_engineMode == EngineMode::Editor)
    {
        auto pastes = std::move(m_editorCtx->pendingPastes);
        m_editorCtx->pendingPastes.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        m_editorCtx->ClearSelection();
        for (const auto& snap : pastes)
        {
            std::vector<entt::entity> all;
            entt::entity e = SceneSerializer::InstantiateSubtree(
                *m_scene, snap, PathResolver::AssetsDir(), &all);
            if (e == entt::null) continue;

            auto& reg = m_scene->GetRegistry();
            // 元と重ならないよう root だけ少しずらして配置（子孫は相対のまま追従）
            if (reg.all_of<Transform>(e))
                reg.get<Transform>(e).position.x += 1.0f;

            m_editorCtx->AddToSelection(e);
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<SpawnPrefabCommand>(
                    m_scene.get(), PathResolver::AssetsDir(), std::move(all)));
            Logger::Info("Pasted entity: {}", reg.get<NameTag>(e).name);
        }
    }

    // スクリプトアタッチ遅延処理
    if (!m_editorCtx->pendingScriptAttachments.empty())
    {
        auto attachments = std::move(m_editorCtx->pendingScriptAttachments);
        m_editorCtx->pendingScriptAttachments.clear();

        char dbgBuf[128];
        snprintf(dbgBuf, sizeof(dbgBuf),
            "[PendingScriptAttachments] processing %zu\n", attachments.size());
        OutputDebugStringA(dbgBuf);

        auto& reg = m_scene->GetRegistry();
        for (const auto& req : attachments)
        {
            if (!m_scriptEngine) break;
            if (!reg.valid(req.entity))
            {
                OutputDebugStringA("[PendingScriptAttachments] SKIP invalid entity\n");
                continue;
            }

            // Undo 用に現状を保存
            bool        hadBefore  = reg.all_of<LuaScript>(req.entity);
            std::string oldPath;
            bool        oldEnabled = true;
            // ★props も退避する。無いと「別スクリプトへ差し替え → Undo」でパスは戻るのに
            //   エンティティ個別に詰めた公開プロパティが全部消える（シーン JSON に載る値）。
            std::vector<ScriptProp> oldProps;
            if (hadBefore)
            {
                const auto& cur = reg.get<LuaScript>(req.entity);
                oldPath    = cur.scriptPath;
                oldEnabled = cur.enabled;
                oldProps   = cur.props;
            }

            m_scriptEngine->AttachScriptToEntity(req.entity, req.scriptPath);

            auto attachCmd = std::make_unique<AttachScriptCommand>(
                &reg, req.entity,
                hadBefore, oldPath, oldEnabled,
                req.scriptPath);
            attachCmd->SetOldProps(std::move(oldProps));
            m_editorCtx->undoSystem.PushCommand(std::move(attachCmd));
        }
    }

    // モデル差し替え 遅延処理（Inspector の MeshRenderer へのD&D/コンボ選択）。
    // モデルロードを伴うため cmdList 有効なフレーム境界で SwapEntityModel を実行する。
    // ★以前は `&& m_engineMode == EngineMode::Editor` を条件に含めていたので、
    //   Play 中に積まれた要求が**キューに溜まったまま**になっていた。Inspector は Play 中も
    //   描かれるのでコンボや D&D は普通に push できる。そして Stop はシーンを丸ごと作り直して
    //   entity id を全部振り直すため、溜まっていた要求は Stop 後に
    //   **まったく別のエンティティ**へ着弾する（reg.valid() は再生成後の別 entity でも真）。
    //   Play 中の分は溜めずにここで捨て、理由を画面に出す。
    if (!m_editorCtx->pendingModelSwaps.empty() && m_engineMode != EngineMode::Editor)
    {
        Logger::Warn("Play 中のモデル差し替えは適用しません（{} 件を破棄）。"
                     "Stop してからやり直してください",
                     m_editorCtx->pendingModelSwaps.size());
        m_editorCtx->pendingModelSwaps.clear();
        m_editorCtx->errorMessage = "Play 中はモデルを差し替えられません（Stop してください）";
        m_editorCtx->errorFlash   = 3.0f;
    }
    if (!m_editorCtx->pendingModelSwaps.empty() && m_engineMode == EngineMode::Editor)
    {
        auto swaps = std::move(m_editorCtx->pendingModelSwaps);
        m_editorCtx->pendingModelSwaps.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        auto& reg = m_scene->GetRegistry();
        for (const auto& req : swaps)
        {
            if (!reg.valid(req.entity) || !reg.all_of<MeshRenderer>(req.entity))
                continue;

            const std::string oldPath = reg.get<MeshRenderer>(req.entity).modelPath;
            const bool wasSelected = m_editorCtx->IsSelected(req.entity);

            entt::entity ne = SceneSerializer::SwapEntityModel(
                *m_scene, req.entity, req.newModelPath, PathResolver::AssetsDir());
            if (ne == entt::null)
            {
                m_editorCtx->errorMessage = "モデル差し替え失敗: " + req.newModelPath;
                m_editorCtx->errorFlash = 3.0f;
                continue;
            }

            if (wasSelected)
                m_editorCtx->Select(ne);
            m_editorCtx->undoSystem.PushCommand(std::make_unique<ModelSwapCommand>(
                m_scene.get(), PathResolver::AssetsDir(), ne,
                oldPath, req.newModelPath));
            Logger::Info("モデル差し替え: {} -> {}", oldPath, req.newModelPath);
        }
        // 差し替えで無効になった選択をクリーンアップ
        auto& sel = m_editorCtx->selectedEntities;
        sel.erase(std::remove_if(sel.begin(), sel.end(),
                  [&](entt::entity e) { return !reg.valid(e); }), sel.end());
        if (m_editorCtx->selectedEntity != entt::null && !reg.valid(m_editorCtx->selectedEntity))
            m_editorCtx->selectedEntity = sel.empty() ? entt::null : sel.back();
    }

    // マテリアルテクスチャD&D割当 遅延処理（アセットブラウザ→SceneView/Inspector）
    if (!m_editorCtx->pendingMaterialTextureDrops.empty())
    {
        auto drops = std::move(m_editorCtx->pendingMaterialTextureDrops);
        m_editorCtx->pendingMaterialTextureDrops.clear();

        auto& reg = m_scene->GetRegistry();
        std::string base = std::filesystem::path(PathResolver::AssetsDir()).lexically_normal().string();
        std::replace(base.begin(), base.end(), '\\', '/');

        for (const auto& req : drops)
        {
            if (!reg.valid(req.entity) || !reg.all_of<MeshRenderer>(req.entity))
                continue;

            // 絶対パス → assets 相対パスへ正規化(HierarchyPanel のスクリプトD&Dと同じ手順)
            std::string abs = std::filesystem::path(req.texturePath).lexically_normal().string();
            std::replace(abs.begin(), abs.end(), '\\', '/');
            std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

            auto& mr = reg.get<MeshRenderer>(req.entity);
            MeshRenderer before = mr;   // Undo 用スナップショット(値コピー、生ポインタは共有でOK)
            switch (req.slot)
            {
                case MaterialTextureSlot::Albedo:
                    MeshRenderer::SetOverride(mr.overrideAlbedoTexture, req.submeshIndex, rel);
                    break;
                case MaterialTextureSlot::Normal:
                    MeshRenderer::SetOverride(mr.overrideNormalTexture, req.submeshIndex, rel);
                    break;
                case MaterialTextureSlot::MetalRoughness:
                    MeshRenderer::SetOverride(mr.overrideMetalRoughnessTexture, req.submeshIndex, rel);
                    break;
            }
            m_editorCtx->undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                &reg, req.entity, before, mr, "Material Texture"));

            // キャッシュは消さない（EnsureMaterialOverrideSrv がパス不一致を検知して同じ
            // blockStart 上へ CreateSRV し直す。erase すると AllocateBlock が再度走り
            // 前のブロックを解放しないまま SRV ヒープを浪費するので避ける）。

            Logger::Info("マテリアルテクスチャ割当: entity={} submesh={} slot={} -> {}",
                static_cast<u32>(req.entity), req.submeshIndex, static_cast<int>(req.slot), rel);
        }
    }

    // サムネイルテクスチャのロード（描画コマンドの前に実行）
    m_editorLayer->LoadPendingThumbnails(nativeCmdList);
    if (m_materialLibraryPanel)
        m_materialLibraryPanel->LoadPendingThumbnails(nativeCmdList);

    // モデルサムネイルのオフスクリーンレンダリング
    m_thumbRenderer->RenderPending(nativeCmdList, m_swapChain->GetCurrentBackBufferIndex());

    // マテリアル球体サムネイルのオフスクリーンレンダリング(アセットブラウザの .dxmat 表示用)。
    // ModelThumbnailRenderer と同様、後段のパスが自分でRT/ビューポートを設定するのでここで戻す必要はない。
    if (m_materialEditorPanel)
        m_materialEditorPanel->GetPreviewRenderer().RenderPendingThumbnails(*m_commandList);

    u32 frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    f32 totalTime = m_gameClock.GetTotalTime();

    // ★決定論キャプチャ（#31）: 時間依存の要素を全部固定する。
    //   totalTime は deband ディザ / フィルムグレイン / ウェーブ / グリッチ / パーティクル /
    //   カスタムシェーダの time が全部見ているので、ここ 1 箇所で止まる。
    //   TAA / フォグ / SSGI は位相カウンタを毎フレーム 0 へ戻して「常に同じジッタ」にする
    //   （＝時間蓄積は不動点へ収束する。数フレーム回してから撮ればビット再現する）。
    if (m_deterministicCapture)
    {
        totalTime = kDeterministicTime;
        if (m_taaPass)           m_taaPass->ResetJitter();
        if (m_volumetricFogPass) m_volumetricFogPass->ResetTemporalPhase();
        if (m_screenSpaceGi)     m_screenSpaceGi->ResetTemporalPhase();
    }

    // ===== フット IK（接地補正）=====
    // 物理ステップが終わった後・スキニングバッファのアップロード前に走らせる
    // （IK 後のボーン行列がアップロードされるように）。
    // Play 中だけ（PhysicsSystem が body を持つのが Play 中だけのため）。
    ApplyFootIkPass();

    // ===== スケルタルアニメのボーン行列を毎フレーム GPU へアップロード =====
    // 以前は CSM シャドウパス(ci==0)内でのみ skinningBuffer->Update していたため、
    // 影OFFシーンや正射カメラ(2Dゲーム。シャドウパスが丸ごとスキップされる)では
    // ボーン行列が一度もアップロードされず、Animator が進んでいてもポーズが凍結して見えた。
    // シャドウパスの有無に依存しないよう、ここで無条件に1回だけ更新する。
    {
        auto& skinReg = m_scene->GetRegistry();
        for (auto [se, skelAnim] : skinReg.view<SkeletalAnimation>().each())
        {
            if (skelAnim.animator && skelAnim.skinningBuffer)
                skelAnim.skinningBuffer->Update(skelAnim.animator->GetSkinningMatrices(), frameIndex);
        }
    }

    // シャドウマップ再作成（ImGuiで解像度変更時、前フレーム完了後に実行）
    if (m_shadowMapDirty)
    {
        m_shadowMapDirty = false;
        m_shadowMap.Reset();

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = m_shadowMapSize;
        shadowDesc.Height = m_shadowMapSize;
        shadowDesc.DepthOrArraySize = static_cast<u16>(kNumCascades);
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc = {1, 0};
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &shadowDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&m_shadowMap)));

        // DSV はハンドル再利用（初回 Allocate 済み）。スライス毎に再作成。
        for (u32 i = 0; i < kNumCascades; ++i)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.FirstArraySlice = i;
            dsvDesc.Texture2DArray.ArraySize = 1;
            dsvDesc.Texture2DArray.MipSlice = 0;
            m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                m_shadowMap.Get(), &dsvDesc, m_shadowDsvHandles[i]);
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = kNumCascades;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_shadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_shadowSrvIndex));
    }

    // ===== メインパスのビューポートとカメラ投影を先に確定 =====
    // 以降の CSM / SSAO / forward はすべてこの同じカメラ状態を前提にする。
    // ここがシャドウ計算より後だと、Scene カメラの投影で影を作って GameCamera で描くなどの
    // 経路差が起き、Scene/Game で光の強さが違って見える。
    // ★#16: 「表示（display）」と「レンダー（render）」の 2 つに分かれている。混ぜないこと。
    //   vpLeft/vpTop/vpW/vpH … バックバッファ上の矩形。uber パス以降だけが使う
    //   rW/rH               … シーン系 RT のサイズ。シーンは必ずその全面 (0,0,rW,rH) に描く
    u32 vpLeft, vpTop, vpW, vpH;
    GetDisplayViewport(vpLeft, vpTop, vpW, vpH);
    const u32 rW = (m_renderW > 0) ? m_renderW : vpW;
    const u32 rH = (m_renderH > 0) ? m_renderH : vpH;

    // アスペクトは**表示側**を使う（レンダー解像度は同じアスペクトで縮めるだけ。
    // ここをレンダー側にすると renderScale の丸め誤差で絵が伸びる）。
    const f32 renderAspect = static_cast<f32>(vpW) / static_cast<f32>(vpH);

    // ===== 2D ビューモード: エディタカメラを正射＋XY平面正対(forward +Z)へ固定 =====
    // 回転/ドリーは入力側で無効化済み。Play 中は CameraComponent 同期が優先する。
    if (!m_isGameMode && m_engineMode == EngineMode::Editor)
    {
        if (m_editorCtx->view2D)
        {
            // 3D→2D に入った瞬間だけ、3Dカメラの位置/向きを退避（戻した時に復元する）。
            if (!m_editorWas2D)
            {
                m_cam3DSnapshot.position = m_camera->GetPosition();
                m_cam3DSnapshot.yaw      = m_camera->GetYaw();
                m_cam3DSnapshot.pitch    = m_camera->GetPitch();
                m_has3DSnapshot = true;
            }
            m_camera->SetYaw(0.0f);
            m_camera->SetPitch(0.0f);
            XMFLOAT3 p = m_camera->GetPosition();
            p.z = -100.0f;                                   // XY 平面(z=0)を十分手前から見る
            m_camera->SetPosition(p);
            m_camera->SetOrthographic(2.0f * m_editorCtx->view2DZoom,
                                      renderAspect, 0.1f, 2000.0f);
        }
        else
        {
            // 2D→3D に戻った瞬間だけ、退避してあった 3Dカメラ状態を復元（視点が壊れないように）。
            if (m_editorWas2D && m_has3DSnapshot)
            {
                m_camera->SetPosition(m_cam3DSnapshot.position);
                m_camera->SetYaw(m_cam3DSnapshot.yaw);
                m_camera->SetPitch(m_cam3DSnapshot.pitch);
            }
            m_camera->SetPerspective(DirectX::XM_PIDIV4, renderAspect, 0.1f, 1000.0f);
        }
        m_editorWas2D = m_editorCtx->view2D;
    }
    else
    {
        // Play / 単体ゲーム: アクティブな CameraComponent の投影（透視/正射・FOV・orthoSize・near/far）を
        // 実ビューポートのアスペクトで m_camera に毎フレーム反映する。
        bool applied = false;
        auto& reg = m_scene->GetRegistry();
        for (auto [e, cam] : reg.view<const CameraComponent>().each())
        {
            if (!cam.isActive) continue;
            if (cam.projection == CameraProjection::Orthographic)
                m_camera->SetOrthographic(2.0f * cam.orthoSize, renderAspect, cam.nearClip, cam.farClip);
            else
                m_camera->SetPerspective(DirectX::XMConvertToRadians(cam.fovDegrees),
                                         renderAspect, cam.nearClip, cam.farClip);
            applied = true;
            break;
        }
        if (!applied)
            m_camera->SetAspect(renderAspect);  // アクティブカメラが無ければアスペクトのみ更新
    }

    // ===== カメラ非有限値ガード =====
    // スクリプト/物理の発散で NaN 化した Transform がカメラ同期・focus_camera 経由で m_camera に
    // 入ると、ビュー行列が NaN 化し全描画が消えて「シーンビューが真っ青」のまま戻らなくなる
    // (Stop してもエディタカメラは復元されない)。毎フレーム検査し、非有限になったら直近の
    // 正常姿勢へ戻して、原因調査用に直近の MCP コマンドと共にログを残す。
    {
        static XMFLOAT3 s_lastGoodCamPos{-14.7f, 9.6f, -9.0f};
        static f32  s_lastGoodCamYaw = 0.0f, s_lastGoodCamPitch = 0.0f;
        static bool s_camWasBad = false;
        const XMFLOAT3 cp = m_camera->GetPosition();
        const bool camFinite =
            std::isfinite(cp.x) && std::isfinite(cp.y) && std::isfinite(cp.z) &&
            std::isfinite(m_camera->GetYaw()) && std::isfinite(m_camera->GetPitch());
        if (camFinite)
        {
            s_lastGoodCamPos   = cp;
            s_lastGoodCamYaw   = m_camera->GetYaw();
            s_lastGoodCamPitch = m_camera->GetPitch();
            s_camWasBad = false;
        }
        else
        {
            if (!s_camWasBad)
            {
                std::string lastMcp = "(なし)";
                if (m_mcpBridge)
                {
                    const auto cmds = m_mcpBridge->RecentCommands();
                    if (!cmds.empty()) lastMcp = cmds.back().method;
                }
                Logger::Error("カメラが非有限値になりました pos=({}, {}, {}) yaw={} pitch={} "
                              "直近MCP={} — 直前の正常姿勢へ復元します",
                              cp.x, cp.y, cp.z, m_camera->GetYaw(), m_camera->GetPitch(), lastMcp);
                s_camWasBad = true;
            }
            m_camera->SetPosition(s_lastGoodCamPos);
            m_camera->SetYaw(s_lastGoodCamYaw);
            m_camera->SetPitch(s_lastGoodCamPitch);
        }
    }

    // ライトの向きを Transform 回転に追従させる（回転の変化分=デルタを direction に適用）。
    // これでインスペクターの Transform 回転でもギズモ回転でも光の向きが変わる。
    // direction を真実とし回転はデルタのみ与えるので、Direction 欄の直接編集とも共存できる。
    {
        auto& reg = m_scene->GetRegistry();
        auto eq = [](const XMFLOAT3& r) {
            return XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(r.x), XMConvertToRadians(r.y), XMConvertToRadians(r.z));
        };
        auto applyDelta = [&](XMFLOAT3& dir, XMFLOAT3& prevRot, bool& init, const XMFLOAT3& rot) {
            if (!init) { prevRot = rot; init = true; return; }
            if (rot.x == prevRot.x && rot.y == prevRot.y && rot.z == prevRot.z) return;
            XMVECTOR delta = XMQuaternionMultiply(XMQuaternionInverse(eq(prevRot)), eq(rot));
            XMVECTOR nd = XMVector3Normalize(XMVector3Rotate(XMLoadFloat3(&dir), delta));
            XMStoreFloat3(&dir, nd);
            prevRot = rot;
        };
        for (auto [e, dl, tf] : reg.view<dx12e::DirectionalLight, const Transform>().each())
            applyDelta(dl.direction, dl._prevRot, dl._prevRotInit, tf.rotation);
        for (auto [e, sl, tf] : reg.view<dx12e::SpotLight, const Transform>().each())
            applyDelta(sl.direction, sl._prevRot, sl._prevRotInit, tf.rotation);
    }

    // ライト方向/色: ECS の DirectionalLight から取得。
    // ★ DirectionalLight が無い場合は「太陽光なし」(色=黒) にする。
    //   以前はフル強度の既定太陽にフォールバックしていたため、ライトを消しても
    //   明るいまま＆既定方向の影が出る、という分かりにくい挙動だった。
    //   ambient だけ残すので真っ暗にはならない。
    XMFLOAT3 lightDirF3   = {-0.3f, -1.0f, -0.5f};  // 影の方向（色が黒なら影は出ない）
    XMFLOAT3 lightColorF3 = {0.0f, 0.0f, 0.0f};      // 既定=太陽なし
    float    lightAmbient = 0.25f;
    {
        auto& reg = m_scene->GetRegistry();
        auto dlView = reg.view<const dx12e::DirectionalLight>();
        if (!dlView.empty())
        {
            auto first = *dlView.begin();
            const auto& dl = dlView.get<const dx12e::DirectionalLight>(first);
            lightDirF3 = dl.direction;
            lightColorF3 = {dl.color.x * dl.intensity,
                            dl.color.y * dl.intensity,
                            dl.color.z * dl.intensity};
            lightAmbient = dl.ambient;
        }
    }
    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirF3));
    XMStoreFloat3(&lightDirF3, lightDir);  // 正規化した値を書き戻す
    // CSM: 確定済みの描画カメラ視錐台を 4 分割し、各カスケードをライト視点へタイトフィット。
    // 結果は m_cascadeViewProj[] / m_cascadeSplitsView[] に格納される。
    // CSM は透視前提。正射カメラでは ComputeCascades 側が無影センチネルへ切り替える。
    {
        f32 camNear = m_camera->GetNearZ();
        f32 camFar  = m_camera->GetFarZ();
        // 影が無限遠まで必要なわけではないので、現実的な距離にクランプ（タイトに保つ）。
        camFar = (std::min)(camFar, 200.0f);
        ComputeCascades(lightDir, camNear, camFar);
    }

    // SRV ヒープをバインド（シャドウパスでもボーンSRVが必要）
    m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
    m_commandList->SetRootSignature(*m_rootSignature);

    // ===== TAA の有効判定（BuildDrawList より前に決めること）=====
    // 「速度バッファ用に前フレームのワールド行列を追跡するか」がここで決まるため、
    // 描画リスト構築より前に確定させる必要がある。
    // 正射（2D ビュー / 俯瞰ゲーム）では無効: サブピクセルジッタと深度再投影が透視前提で、
    // 深度プリパス自体も透視限定の運用になっている。
    const TaaSettings& taaCfg = m_scene->GetTaaSettings();
    const bool taaViewOk = !(m_editorCtx && m_editorCtx->view2D) && !m_camera->IsOrthographic();
    const bool taaActive = taaCfg.enabled && m_taaPass && m_taaPass->IsReady() && taaViewOk;
    // SSR/SSGI も速度+G-Buffer プリパスを要求する（TAA が OFF でも）。速度バッファはヒット点の
    // 再投影に使うので、前フレームのワールド行列の追跡もインスタンス用の前ワールド VB も要る。
    // ★ここを taaActive だけにすると、SSR だけ ON のときに動く物体の速度が 0 になり、
    //   プリパスのインスタンシング経路も無効化されて無駄に遅くなる。
    const bool ssGiWantsPrepass =
        m_screenSpaceGi && m_screenSpaceGi->IsReady() && taaViewOk
        && (m_scene->GetSsrSettings().enabled || m_scene->GetSsgiSettings().enabled);
    if (taaActive || ssGiWantsPrepass) EnsureInstancePrevBuffer();
    m_trackPrevWorld = taaActive || ssGiWantsPrepass;

    // ★かつてここに「ビューポート矩形が変わったら履歴を捨てる」ブロックがあったが、
    //   #16 でシーンは常に RT 全面に描くようになり、履歴の UV 対応が表示矩形に依存しなくなった。
    //   レンダー解像度が変わったときは ApplyRenderResolution() が履歴を捨てる。

    // ===== TAA ジッタ =====
    // ハルトン(2,3)列で投影行列を ±0.5px ずらす。Camera 自体には一切入れない
    //   → エディタのピッキング / ギズモ / MCP project_world_to_screen が影響を受けない。
    // ★ジッタ行列の数学（DirectXMath は行ベクトル規約 v' = v * M）:
    //   clip = v * P の後に T = XMMatrixTranslation(jx, jy, 0) を掛けると
    //   clip' = clip * T で clip'.x = clip.x + clip.w * jx。
    //   透視では clip.w = z_view、正射では clip.w = 1 なので、どちらでも NDC が jx 平行移動する
    //   （_31/_32 を直接いじる透視専用の方法より汎用で安全）。
    // ★ジッタ幅は「レンダー解像度の 1px」基準（ラスタライズするのはレンダー解像度）。
    if (taaActive) m_taaJitterNdc = m_taaPass->NextJitterNdc(
        static_cast<u32>(taaCfg.sampleCount), rW, rH, taaCfg.jitterScale);
    else           m_taaJitterNdc = {0.0f, 0.0f};
    const XMFLOAT2 jitterNdc = m_taaJitterNdc;

    // ラスタライズ用（ジッタあり）と、計算用（ジッタなし）を明確に分ける。
    //   camVPJ … 深度プリパス/フォワード/パーティクル/スプライト＝実際に画を出すもの全部
    //   camVP  … 再投影・深度線形化・太陽投影・カスケード分割・ピッキング＝ジッタ厳禁のもの
    const XMMATRIX camVP  = m_camera->GetViewProjMatrix();
    const XMMATRIX camVPJ = taaActive
        ? XMMatrixMultiply(m_camera->GetViewMatrix(),
              XMMatrixMultiply(m_camera->GetProjectionMatrix(),
                               XMMatrixTranslation(jitterNdc.x, jitterNdc.y, 0.0f)))
        : camVP;

    // フレーム描画リストを構築（1回）。以降の影/プリパス/メイン/プレビューの全パスで共有する。
    BuildDrawList();   // 内部で buildList / listSort を別々に計時する

    // instancing リングのカーソルをフレーム先頭でリセット。
    // 影4カスケード → 深度プリパス → メイン → プレビューの順に連番で追記していく。
    m_instanceCursor = 0;

    // ===== DXR: BLAS の遅延構築 + TLAS の再構築（計画09 Step 1）=====
    // ★CSM のパスより前に建てる。RT サン影が有効かどうかが決まらないと、
    //   CSM が「RT の担当ぶんを描かない」排他モードに入れないため。
    m_rtShadowActiveThisFrame  = false;
    m_rtSkinnedActiveThisFrame = false;
    bool rtAoActive = false;
    // ★DDGI のプローブ更新は「ここ」では回せない。プローブが点光源/スポットを拾うには
    //   m_clusteredLighting->UploadLights() が済んでいる必要があり、それは後段（クラスタ
    //   カリング）で起きる。ここでは TLAS のアドレスだけ持ち越して、更新は後段でやる。
    bool ddgiTlasOk = false;
    D3D12_GPU_VIRTUAL_ADDRESS ddgiTlas = 0, ddgiGeoInfo = 0;
    if (m_dxrEnabled && m_rtScene && m_rtScreenPass && m_rtScreenPass->IsReady() && m_scene)
    {
        const RtSettings& rtCfg = m_scene->GetRtSettings();
        // 深度からワールドを復元する都合で透視カメラ限定（SSAO / コンタクトシャドウと同じ条件）。
        const bool rtViewOk = !(m_editorCtx && m_editorCtx->view2D) && !m_camera->IsOrthographic();
        const bool wantDebug = (m_renderDebugMode == static_cast<u32>(RenderDebugMode::RtHit)
                             || m_renderDebugMode == static_cast<u32>(RenderDebugMode::RtDiff)
                                 || m_renderDebugMode == static_cast<u32>(RenderDebugMode::RtAlbedo));
        // ★DDGI も TLAS が要る。ここに入れておかないと「ddgiEnabled:true にしたのに
        //   forceBuildTlas も一緒に立てないと何も起きない」という罠になる。
        //   DDGI はワールド空間なので rtViewOk（透視カメラ限定）は課さない。
        const bool ddgiWant = m_ddgi && m_scene->GetDdgiSettings().enabled;
        const bool want = (rtViewOk
                        && (rtCfg.shadowEnabled || rtCfg.aoEnabled || rtCfg.forceBuildTlas || wantDebug))
                        || ddgiWant;
        if (want)
        {
            // シーンを作り直したら BLAS キャッシュを丸ごと捨てる。Mesh* は解放後に
            // 同じアドレスへ再確保され得るので、内容比較だけでは検出できない（N30 と同じ話）。
            if (m_rtSceneGenSeen != m_sceneGeneration)
            {
                m_rtScene->Invalidate();
                m_rtSceneGenSeen = m_sceneGeneration;
            }

            m_gpuTimer->Begin(nativeCmdList, GpuTimer::Raytracing);
            const XMFLOAT3 camP = m_camera->GetPosition();

            // スキンドを TLAS に入れられるのは compute スキニングが生きているフレームだけ。
            // ★この 1 変数を IsRaytracedItem() の両方の呼び出し側が見る（CSM 排他 / TLAS 詰め込み）。
            const bool skinnedInTlas = (m_skinningCompute != nullptr);
            m_rtSkinnedActiveThisFrame = skinnedInTlas;
            if (m_skinningCompute) m_skinningCompute->BeginFrame();

            // 除外された数は診断（dx12_diagnose の dxr 検査）で「なぜキャラの影が
            // RT に出ないのか」を説明するために数えておく。
            u32 skippedSkin = 0, skippedTransp = 0;
            for (const DrawItem& it : m_drawItems)
            {
                if (it.skin && !skinnedInTlas) ++skippedSkin;
                else if (it.sortKey == 3u)     ++skippedTransp;
            }
            m_rtScene->BeginFrame(camP, skippedSkin, skippedTransp);

            auto& rtReg = m_scene->GetRegistry();

            // レイのヒット点で頂点属性 / アルベドを引くための表（計画09 Step 5）。
            // ★SRV の払い出しはここが初回＝RT を使わないプロジェクトでは 1 個も消費しない。
            // ★スキンドでも「属性用 VB」は元メッシュのものを渡す。変形後バッファには
            //   位置しか入っていない（UV も法線も無い）が、インデックスは共有なので
            //   PrimitiveIndex と バリセントリック はそのまま使える。
            const bool wantGeoInfo = m_graphicsDevice->SupportsDynamicResources();
            auto makeGeoInfo = [&](Mesh* mesh) -> RaytracingScene::GeometryInfo
            {
                RaytracingScene::GeometryInfo g{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u};
                if (!wantGeoInfo || !mesh) return g;
                mesh->EnsureRaytracingSrvs(*m_graphicsDevice, *m_srvHeap);
                g.vbSrvIndex = mesh->GetVbSrvIndex();
                g.ibSrvIndex = mesh->GetIbSrvIndex();
                // アルベド: srvBlockIndex は albedo/normal/metalRoughness の 3 連続ブロックの
                // 先頭＝そのまま albedo。ブロックが無いモデルはテクスチャ単体の SRV を使う。
                if (const Material* mat = mesh->GetMaterial())
                {
                    if (mat->srvBlockIndex != 0xFFFFFFFFu)
                        g.baseColorSrvIndex = mat->srvBlockIndex;
                    else if (mat->albedoTexture)
                        g.baseColorSrvIndex = mat->albedoTexture->GetSrvIndex();
                }
                return g;
            };

            for (const DrawItem& it : m_drawItems)
            {
                if (!IsRaytracedItem(it, skinnedInTlas)) continue;
                const MeshRenderer& r = *it.renderer;
                const XMMATRIX world = XMLoadFloat4x4(&it.world);

                // ---- スキンド: compute でローカル空間の変形後頂点を作ってから BLAS へ ----
                if (it.skin)
                {
                    const auto* skelAnim = rtReg.try_get<SkeletalAnimation>(it.e);
                    if (!skelAnim || !skelAnim->animator) continue;
                    // ポーズのハッシュ。前フレームと同じならスキニングも BLAS 再構築も省く。
                    const auto& mats = skelAnim->animator->GetSkinningMatrices();
                    const u64 poseHash = HashBonesFnv1a(mats);
                    const D3D12_GPU_VIRTUAL_ADDRESS bonesVa = it.skin->GetGpuAddress(frameIndex);
                    for (u32 mi = 0; mi < static_cast<u32>(r.meshes.size()); ++mi)
                    {
                        if (!r.meshes[mi]) continue;
                        // ★キーは (エンティティ, サブメッシュ)。Mesh* は複数エンティティで
                        //   共有されるのでキーに使えない（同じキャラ 10 体が同じポーズになる）。
                        const u64 key = (static_cast<u64>(entt::to_integral(it.e)) << 32) | (mi + 1);
                        const auto va = m_skinningCompute->Skin(nativeCmdList, *m_graphicsDevice,
                                                                key, *r.meshes[mi], bonesVa, poseHash);
                        if (va == 0) continue;
                        const auto& vbv = r.meshes[mi]->GetVertexBuffer().GetView();
                        const u32 vcount = (vbv.StrideInBytes != 0)
                                         ? vbv.SizeInBytes / vbv.StrideInBytes : 0;
                        // 変形後頂点はローカル空間なので world は静的とまったく同じものを渡す。
                        XMMATRIX meshWorld = world;
                        if (it.hasNodeAnim && mi < static_cast<u32>(r.meshNodeTransforms.size()))
                            meshWorld = XMLoadFloat4x4(&r.meshNodeTransforms[mi]) * world;
                        m_rtScene->AddSkinnedInstance(key, r.meshes[mi], va, vcount, poseHash,
                                                      meshWorld, it.center,
                                                      makeGeoInfo(r.meshes[mi]));
                    }
                    continue;
                }

                for (u32 mi = 0; mi < static_cast<u32>(r.meshes.size()); ++mi)
                {
                    if (!r.meshes[mi]) continue;
                    // ノードアニメのメッシュ単位変換はラスタ側と同じ式で合成する
                    // （RenderSceneMeshes の meshWorld = nodeMat * world）。ここがズレると
                    // render_debug の rtDiff で一発で分かる。
                    XMMATRIX meshWorld = world;
                    if (it.hasNodeAnim && mi < static_cast<u32>(r.meshNodeTransforms.size()))
                        meshWorld = XMLoadFloat4x4(&r.meshNodeTransforms[mi]) * world;
                    m_rtScene->AddInstance(r.meshes[mi], meshWorld, it.center,
                                           makeGeoInfo(r.meshes[mi]));
                }
            }

            // compute の書き込みを AS ビルド入力（NON_PIXEL_SHADER_RESOURCE）へ遷移させる。
            // ★静的メッシュで「バリア不要」なのは VB が GENERIC_READ で置かれているからで、
            //   compute の出力には当てはまらない。ここを抜かすとゴミの BVH ができる。
            if (m_skinningCompute)
                m_skinningCompute->TransitionForAccelerationStructureBuild(nativeCmdList);

            RaytracingScene::BuildDesc bd;
            bd.frameIndex   = frameIndex;
            bd.maxInstances = (rtCfg.maxInstances > 0)
                            ? static_cast<u32>(rtCfg.maxInstances) : RaytracingScene::kMaxRtInstances;
            const bool tlasOk = m_rtScene->Build(*m_graphicsDevice, nativeCmdList, bd);
            if (m_skinningCompute) m_skinningCompute->EndFrame();
            m_gpuTimer->End(nativeCmdList, GpuTimer::Raytracing);

            if (tlasOk)
            {
                m_rtShadowActiveThisFrame = rtCfg.shadowEnabled;
                rtAoActive                = rtCfg.aoEnabled;

                // DDGI 用に TLAS を持ち越す（更新自体はライトが揃う後段で回す）。
                ddgiTlasOk  = true;
                ddgiTlas    = m_rtScene->GetTlasAddress();
                ddgiGeoInfo = m_rtScene->GetGeometryInfoAddress();
            }
        }
    }
    // エディタのライティング窓へ実行時状態を流す（非対応 GPU で理由を出すため）。
    if (m_editorCtx)
    {
        m_editorCtx->dxrSupported   = m_dxrEnabled;
        m_editorCtx->dxrTier        = m_graphicsDevice
                                    ? static_cast<int>(m_graphicsDevice->GetRaytracingTier()) : 0;
        m_editorCtx->dxrShaderModel = m_graphicsDevice
                                    ? static_cast<int>(m_graphicsDevice->GetHighestShaderModel()) : 0;
        if (m_rtScene)
        {
            const auto& st = m_rtScene->GetStats();
            m_editorCtx->dxrInstances      = st.instances;
            m_editorCtx->dxrSkippedSkinned = st.skippedSkinned;
            m_editorCtx->dxrAsBytes        = st.blasBytes + st.tlasBytes
                                           + st.scratchBytes + st.instanceDescBytes;
        }
    }

    // ===== スポットライト影スロット割当（castShadows なライトをカメラに近い順で最大kMaxShadowSpot灯）=====
    // 結果は m_spotShadowViewProj[] / m_spotShadowEntity[] に格納し、直後の影パス描画と
    // 後段のライト収集(shadowIndex書き込み)の両方で使う。
    m_numSpotShadowSlots = 0;
    {
        auto& reg = m_scene->GetRegistry();
        struct SpotShadowCandidate { entt::entity e; f32 distSq; };
        std::vector<SpotShadowCandidate> candidates;
        XMFLOAT3 camPosF3 = m_camera->GetPosition();
        XMVECTOR camPos = XMLoadFloat3(&camPosF3);
        auto slView = reg.view<const dx12e::SpotLight, const Transform>();
        for (auto [e, sl, tf] : slView.each())
        {
            if (!sl.castShadows) continue;
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR d = XMVectorSubtract(world.r[3], camPos);
            candidates.push_back({e, XMVectorGetX(XMVector3LengthSq(d))});
        }
        std::sort(candidates.begin(), candidates.end(),
                 [](const auto& a, const auto& b) { return a.distSq < b.distSq; });

        const u32 n = (std::min)(static_cast<u32>(candidates.size()), kMaxShadowSpot);
        for (u32 i = 0; i < n; ++i)
        {
            entt::entity e = candidates[i].e;
            const auto& sl = reg.get<const dx12e::SpotLight>(e);
            const auto& tf = reg.get<const Transform>(e);
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR pos = world.r[3];
            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&sl.direction));
            // dir がワールドYにほぼ平行だと LookToLH の up ベクトルが縮退するので切り替える。
            XMVECTOR up = (std::fabs(XMVectorGetY(dir)) > 0.99f)
                ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f) : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

            f32 outerDeg = (std::max)(sl.outerConeDeg, sl.innerConeDeg);
            f32 fov = (std::min)(XMConvertToRadians(outerDeg) * 2.0f * 1.02f, XMConvertToRadians(170.0f));
            f32 range = (std::max)(sl.range, 0.2f);

            XMMATRIX lightView = XMMatrixLookToLH(pos, dir, up);
            XMMATRIX lightProj = XMMatrixPerspectiveFovLH(fov, 1.0f, 0.1f, range);
            XMStoreFloat4x4(&m_spotShadowViewProj[i], lightView * lightProj);
            m_spotShadowEntity[i] = e;
        }
        m_numSpotShadowSlots = n;
    }

    m_gpuTimer->Begin(nativeCmdList, GpuTimer::Shadows);
    m_passBucket = &m_passShadow;

    // ===== スポットライト影パス =====
    if (m_scene && m_scene->GetShadowsEnabled() && m_numSpotShadowSlots > 0)
    {
        m_commandList->TransitionResource(m_spotShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_VIEWPORT spotVp{};
        spotVp.Width = spotVp.Height = static_cast<f32>(kSpotShadowMapSize);
        spotVp.MinDepth = 0.0f;
        spotVp.MaxDepth = 1.0f;
        D3D12_RECT spotScissor = {0, 0, static_cast<LONG>(kSpotShadowMapSize), static_cast<LONG>(kSpotShadowMapSize)};
        nativeCmdList->RSSetViewports(1, &spotVp);
        nativeCmdList->RSSetScissorRects(1, &spotScissor);

        for (u32 i = 0; i < m_numSpotShadowSlots; ++i)
        {
            XMMATRIX lvp = XMLoadFloat4x4(&m_spotShadowViewProj[i]);
            m_commandList->ClearDepthStencil(m_spotShadowDsvHandles[i]);
            nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_spotShadowDsvHandles[i]);
            RenderDepthOnlyScene(lvp, *m_shadowPipelineState, *m_shadowSkinnedPipelineState,
                                 /*updateSkinning*/ false, frameIndex, /*lodBias*/ 1,
                                 m_shadowPipelineStateInst.get());
        }

        m_commandList->TransitionResource(m_spotShadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ===== ポイントライト影スロット割当（castShadows なライトをカメラに近い順で最大kMaxShadowPoint灯）=====
    m_numPointShadowSlots = 0;
    {
        auto& reg = m_scene->GetRegistry();
        struct PointShadowCandidate { entt::entity e; f32 distSq; };
        std::vector<PointShadowCandidate> candidates;
        XMFLOAT3 camPosF3 = m_camera->GetPosition();
        XMVECTOR camPos = XMLoadFloat3(&camPosF3);
        auto plShadowView = reg.view<const dx12e::PointLight, const Transform>();
        for (auto [e, pl, tf] : plShadowView.each())
        {
            if (!pl.castShadows) continue;
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR d = XMVectorSubtract(world.r[3], camPos);
            candidates.push_back({e, XMVectorGetX(XMVector3LengthSq(d))});
        }
        std::sort(candidates.begin(), candidates.end(),
                 [](const auto& a, const auto& b) { return a.distSq < b.distSq; });

        const u32 n = (std::min)(static_cast<u32>(candidates.size()), kMaxShadowPoint);
        for (u32 i = 0; i < n; ++i)
            m_pointShadowEntity[i] = candidates[i].e;
        m_numPointShadowSlots = n;
    }

    // ===== ポイントライト影パス（灯ごとに6面。D3Dキューブ面順: +X,-X,+Y,-Y,+Z,-Z）=====
    if (m_scene && m_scene->GetShadowsEnabled() && m_numPointShadowSlots > 0)
    {
        static const XMFLOAT3 kFaceDir[6] = {
            { 1,  0,  0}, {-1,  0,  0}, { 0,  1,  0}, { 0, -1,  0}, { 0,  0,  1}, { 0,  0, -1},
        };
        static const XMFLOAT3 kFaceUp[6] = {
            {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0},
        };

        m_commandList->TransitionResource(m_pointShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_VIEWPORT pointVp{};
        pointVp.Width = pointVp.Height = static_cast<f32>(kPointShadowMapSize);
        pointVp.MinDepth = 0.0f;
        pointVp.MaxDepth = 1.0f;
        D3D12_RECT pointScissor = {0, 0, static_cast<LONG>(kPointShadowMapSize), static_cast<LONG>(kPointShadowMapSize)};
        nativeCmdList->RSSetViewports(1, &pointVp);
        nativeCmdList->RSSetScissorRects(1, &pointScissor);

        auto& reg = m_scene->GetRegistry();
        for (u32 i = 0; i < m_numPointShadowSlots; ++i)
        {
            entt::entity e = m_pointShadowEntity[i];
            const auto& pl = reg.get<const dx12e::PointLight>(e);
            const auto& tf = reg.get<const Transform>(e);
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMVECTOR pos = world.r[3];
            f32 range = (std::max)(pl.range, 0.2f);
            XMMATRIX faceProj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, range);

            for (u32 f = 0; f < 6; ++f)
            {
                XMMATRIX faceView = XMMatrixLookToLH(pos, XMLoadFloat3(&kFaceDir[f]), XMLoadFloat3(&kFaceUp[f]));
                u32 slice = i * 6 + f;
                m_commandList->ClearDepthStencil(m_pointShadowDsvHandles[slice]);
                nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_pointShadowDsvHandles[slice]);
                RenderDepthOnlyScene(faceView * faceProj, *m_shadowPipelineState, *m_shadowSkinnedPipelineState,
                                     /*updateSkinning*/ false, frameIndex, /*lodBias*/ 1,
                                     m_shadowPipelineStateInst.get());
            }
        }

        m_commandList->TransitionResource(m_pointShadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ===== シャドウパス（CSM: カスケード毎に kNumCascades 回描画）=====
    // シーンで影 OFF / 正射カメラのときは丸ごとスキップ＝(全アクティブ敵 × 4カスケード)の
    // ドローと 2048²×4 のデプスフィルを撤廃（lv35 の主因）。m_shadowMap は生成時 PSR のまま＝
    // forward の t4 バインドは有効（センチネルで読まれないので未クリアでも安全）。
    if (m_scene && m_scene->GetShadowsEnabled() && !m_camera->IsOrthographic())
    {
        // 配列リソース全体を一括で DEPTH_WRITE へ遷移（カスケードループの外で1回）
        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // シャドウマップ用ビューポート（全カスケード共通＝各スライス同サイズ正方）
        D3D12_VIEWPORT shadowVp{};
        shadowVp.Width    = static_cast<f32>(m_shadowMapSize);
        shadowVp.Height   = static_cast<f32>(m_shadowMapSize);
        shadowVp.MinDepth = 0.0f;
        shadowVp.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor = {0, 0, static_cast<LONG>(m_shadowMapSize), static_cast<LONG>(m_shadowMapSize)};
        nativeCmdList->RSSetViewports(1, &shadowVp);
        nativeCmdList->RSSetScissorRects(1, &shadowScissor);

        CpuScopeTimer _tShadow(&m_cpuMs[CpuShadowRec]); DX12_PROFILE_ZONE_N("Rec/Shadows");
        for (u32 ci = 0; ci < kNumCascades; ++ci)
        {
            XMMATRIX cascadeVP = XMLoadFloat4x4(&m_cascadeViewProj[ci]);

            m_commandList->ClearDepthStencil(m_shadowDsvHandles[ci]);
            // RTVなし、DSVのみ（該当カスケードのスライス）
            nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandles[ci]);

            // skinningBuffer はフレーム先頭で全 SkeletalAnimation を一括 Update 済み
            // （シャドウパスは影OFF/正射カメラでスキップされるため、ここでは更新しない）。
            // ★RT サン影が有効なフレームは、CSM は「RT が担当できないもの」だけを描く
            //   （スキンド / 半透明）。担当を排他にすることで、フォワードの min() 合成が
            //   静的ジオメトリのアクネ・peter-panning・カスケード境界を完全に消す。
            //   副産物として CSM のドロー数も大きく減る。
            // このカスケードの 1 テクセルが何メートルか。遠カスケードほど大きくなり、
            //   そこへフル解像度のメッシュを投げる無駄を passLod が落とす。
            const f32 texelWorld = 2.0f * m_cascadeRadius[ci] / static_cast<f32>(m_shadowMapSize);
            RenderDepthOnlyScene(cascadeVP, *m_shadowPipelineState, *m_shadowSkinnedPipelineState,
                                 /*updateSkinning*/ false, frameIndex, /*lodBias*/ 1,
                                 m_shadowPipelineStateInst.get(), /*prepass*/ nullptr,
                                 /*skipRtCovered*/ m_rtShadowActiveThisFrame,
                                 /*cascadeTexelWorld*/ texelWorld);
        }

        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    m_gpuTimer->End(nativeCmdList, GpuTimer::Shadows);
    m_passBucket = &m_passOther;

    // ===== メインパス（オフスクリーン RT へ描画）=====

    // ===== 深度プリパス → SSAO / コンタクトシャドウ（透視のみ。2D 正射ビューや無効時は素通し）=====
    // どちらもカメラ視点の深度を m_depthBuffer へ先に完成させてから、その深度を読んで作る。
    // プリパスは 1 回だけ走らせて 2 つのパスで共有する。
    const SSAOSettings& ssaoCfg = m_scene->GetSSAOSettings();
    const ContactShadowSettings& csCfg = m_scene->GetContactShadowSettings();
    // SSAO は透視前提（深度線形化が透視射影に依存）。正射カメラ（俯瞰ゲーム/2Dビュー）では
    // AO 計算が壊れて全面 AO≈0 になり、ambient を黒く潰す（ゲームだけ真っ暗の原因）。→ 正射は無効化。
    // コンタクトシャドウもビュー空間でレイを飛ばす＝同じ理由で透視限定。
    const bool viewSupportsScreenSpace = !(m_editorCtx && m_editorCtx->view2D)
                                       && !m_camera->IsOrthographic();
    // DXR（計画09）。深度からワールドを復元してレイを飛ばすので、こちらも深度プリパスが要る。
    // ★m_rtShadowActiveThisFrame / rtAoActive は BuildDrawList 直後の TLAS 構築ブロックで
    //   既に確定している（CSM の排他描画の判定に先に必要だったため）。ここで作り直さないこと。
    const bool useRtShadow = m_rtShadowActiveThisFrame && m_rtScene && m_rtScene->IsReady();
    const bool useRtAo     = rtAoActive && m_rtScene && m_rtScene->IsReady();
    const bool useRtDebug  = m_rtScene && m_rtScene->IsReady() && m_rtScreenPass
                           && (m_renderDebugMode == static_cast<u32>(RenderDebugMode::RtHit)
                            || m_renderDebugMode == static_cast<u32>(RenderDebugMode::RtDiff)
                                || m_renderDebugMode == static_cast<u32>(RenderDebugMode::RtAlbedo));

    // ★RT-AO が走るフレームは SSAO を走らせない（どちらも同じ t8 枠へ書くので後勝ちになるだけ）。
    //   ただし aoCombineWithSsao のときは min 合成の相手として SSAO の結果が要るので走らせる。
    const bool useSSAO = ssaoCfg.enabled && m_ssaoPass && m_ssaoPass->IsReady()
                       && viewSupportsScreenSpace
                       && (!useRtAo || m_scene->GetRtSettings().aoCombineWithSsao);
    // ★RT 影が走るフレームはコンタクトシャドウを走らせない。どちらも同じ t11 枠へ書くので
    //   先に書いた方が捨てられるだけ（RT 影は接地の遮蔽も正しく拾うので上位互換）。
    const bool useContactShadow = csCfg.enabled && m_contactShadowPass
                                && m_contactShadowPass->IsReady()
                                && viewSupportsScreenSpace && !useRtShadow;
    // SSR / SSGI（計画04）。G-Buffer と前フレームカラーが要るので、有効なら速度プリパスを走らせる。
    const SsrSettings&  ssrCfg  = m_scene->GetSsrSettings();
    const SsgiSettings& ssgiCfg = m_scene->GetSsgiSettings();
    // m_iblBaker は SSGI のミス時フォールバック（TextureCube t5）のバインドに必須。
    // 未ベイクでも SRV ブロックは有効なので、居るかどうかだけ見る。
    const bool ssGiReady = m_screenSpaceGi && m_screenSpaceGi->IsReady()
                         && viewSupportsScreenSpace && m_iblBaker != nullptr;
    const bool useSsr    = ssrCfg.enabled  && ssGiReady;
    const bool useSsgi   = ssgiCfg.enabled && ssGiReady;

    // ★TAA 有効時は SSAO/コンタクトシャドウが無効でも必ずプリパスを走らせる（速度バッファのため）。
    //   新しく深度を要求するパスを足したら、この行に OR で足すこと（00-COORDINATION §2 H2）。
    // ★m_forceDepthPrepass（settings.json "render_depth_prepass" / MCP set_depth_prepass）は
    //   「深度プリパス単独の損得」を測るための A/B スイッチ（計画10 A2）。
    //   viewSupportsScreenSpace を必ず掛けること（正射 / 2D ビューでプリパスを走らせても
    //   意味が無く、LESS_EQUAL の PSO 選択だけが変わって z-fight の温床になる）。
    const bool useDepthPrepass = useSSAO || useContactShadow || taaActive || useSsr || useSsgi
                               || useRtShadow || useRtAo || useRtDebug
                               || (m_forceDepthPrepass && viewSupportsScreenSpace);
    // 速度＋G-Buffer モードで走らせるか（PSO 3 本が揃っていることが条件）。
    // ★SSR/SSGI は G-Buffer が必要なので TAA が OFF でもこのモードで走らせる。
    //   速度バッファは書かれるが TAA が読まないだけ（fp16×2ch のフィル 1 枚ぶん ≒ 0.05ms）。
    //   PSO を 3 モードに分けるより安い（00-COORDINATION §5.5）。
    // ★RT-AO も G-Buffer の法線を使う（レイ方向とデノイザの bilateral 重み）。
    //   ここに足さないと G-Buffer が書かれず、AO は ddx/ddy 法線へ落ち、
    //   空間デノイザは丸ごとスキップされる。
    const bool velocityPrepass = (taaActive || useSsr || useSsgi || useRtAo)
                               && m_velocityPSO && m_velocityPSOSkinned;
    // TAA 解決まで走らせるか。速度が書かれていないフレームでは絶対に解決しない
    // （履歴を速度なしで再投影すると全面がゴーストする）。
    // ★taaActive を必ず併記すること。SSR だけ ON のときに TAA が勝手に効いてしまう。
    const bool taaResolveActive = taaActive && velocityPrepass && m_taaPass->IsResolveReady();
    // 走らないフレームでは履歴を捨てる。TAA を OFF→ON したり 2D ビューを往復したときに
    // 何十フレームも前の絵が半透明で残るのを防ぐ。
    if (!taaResolveActive && m_taaPass) m_taaPass->InvalidateHistory();
    u32 aoSrv = m_ssaoWhiteSrvIndex;  // 既定 = 白（AO=1.0 素通し）
    u32 csSrv = m_ssaoWhiteSrvIndex;  // 既定 = 白（遮蔽なし素通し。SSAO と同じ 1x1 白を共用）
    u32 ssrSrv  = DescriptorHeap::kInvalidIndex;   // 無効 = 黒ダミー（RenderSceneMeshes が差し替える）
    u32 ssgiSrv = DescriptorHeap::kInvalidIndex;
    u32 rtDebugSrv = DescriptorHeap::kInvalidIndex;   // render_debug の rt / rtDiff 用

    // ★RAII で囲まない: この関数の残り全部が生存範囲になり、後続の lights 等と
    //   二重計上になる（実際に踏んだ。lights+prepass の合計が workMs を超えた）。
    //   計りたいのは GpuTimer::PrepassSSAO の Begin..End と同じ区間なので明示的に取る。
    const auto _prepassT0 = std::chrono::high_resolution_clock::now();
    m_gpuTimer->Begin(nativeCmdList, GpuTimer::PrepassSSAO);
    if (useDepthPrepass)
    {
        // --- 深度プリパス（カメラ視点で m_depthBuffer/m_dsvHandle へ書く）---
        // ★ラスタライズは camVPJ（ジッタあり）。ここをジッタなしにするとフォワードと
        //   深度がビット一致せず LESS_EQUAL で面が欠落する。
        m_commandList->ClearDepthStencil(m_dsvHandle);
        m_commandList->SetViewportAndScissor(rW, rH);

        // 半透明（sortKey==3）はカメラのプリパスから除外する（00-COORDINATION §6 B3）。
        // 影パスは従来どおり半透明も描く＝影の見た目は不変。
        PrepassParams pp{};
        pp.skipTransparent = true;
        pp.jitterNdc       = jitterNdc;

        if (velocityPrepass)
        {
            // 深度 + 速度を同時に書く（RTV=速度RT / DSV=m_dsvHandle）。
            // 前フレームは「ジッタなし」viewProj。履歴が無い初回は現フレームを使う＝速度0。
            pp.mode = PrepassMode::DepthVelocityGBuffer;
            XMStoreFloat4x4(&pp.prevViewProj,
                m_prevViewProjNJValid ? XMLoadFloat4x4(&m_prevViewProjNoJitter) : camVP);
            // RTV1 = G-Buffer。全面 0 クリア（背景は深度 1.0 で弾かれるので中身は問われない）。
            m_gbufferRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float gbufZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            m_commandList->ClearRenderTarget(m_gbufferRT->GetRtv(), gbufZero);
            m_taaPass->BeginVelocity(*m_commandList, m_dsvHandle, m_gbufferRT->GetRtv(),
                                     0u, 0u, rW, rH);
            m_gpuTimer->Begin(nativeCmdList, GpuTimer::DepthPrepass);
            RenderDepthOnlyScene(camVPJ, *m_velocityPSO, *m_velocityPSOSkinned,
                                 /*updateSkinning*/ false, frameIndex, /*lodBias*/ 0,
                                 m_velocityPSOInst.get(), &pp);
            m_gpuTimer->End(nativeCmdList, GpuTimer::DepthPrepass);
            m_taaPass->EndVelocity(*m_commandList);
            m_gbufferRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else
        {
            nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_dsvHandle);
            // skinningBuffer は毎フレーム1回どこかで Update されていれば良い（このプリパスより前に
            // シャドウパスの ci==0 で更新済み＝ここでは false）。
            m_gpuTimer->Begin(nativeCmdList, GpuTimer::DepthPrepass);
            RenderDepthOnlyScene(camVPJ, *m_depthPrepassPSO, *m_depthPrepassSkinnedPSO,
                                 /*updateSkinning*/ false, frameIndex, /*lodBias*/ 0,
                                 m_depthPrepassPSOInst.get(), &pp);
            m_gpuTimer->End(nativeCmdList, GpuTimer::DepthPrepass);
        }

        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // --- SSAO 生成（depth SRV を読み AO→Blur）---
        if (useSSAO)
        {
            aoSrv = m_ssaoPass->Generate(nativeCmdList, m_srvHeap.get(),
                m_srvHeap->GetGpuHandle(m_depthSrvIndex), ssaoCfg,
                m_camera->GetProjectionMatrix(), m_camera->GetNearZ(), m_camera->GetFarZ(),
                0u, 0u, rW, rH, frameIndex);
            // 生成失敗（未準備）時は白ダミー(AO=1.0)へフォールバック。誤テクスチャの読み出しを防ぐ。
            if (aoSrv == DescriptorHeap::kInvalidIndex)
                aoSrv = m_ssaoWhiteSrvIndex;
        }

        // --- コンタクトシャドウ生成（同じ深度を太陽方向へレイマーチ）---
        if (useContactShadow)
        {
            csSrv = m_contactShadowPass->Generate(nativeCmdList,
                m_srvHeap->GetGpuHandle(m_depthSrvIndex), csCfg,
                m_camera->GetViewMatrix(), m_camera->GetProjectionMatrix(), lightDirF3,
                0u, 0u, rW, rH, frameIndex);
            if (csSrv == DescriptorHeap::kInvalidIndex)
                csSrv = m_ssaoWhiteSrvIndex;
        }

        // --- DXR: RT サン影 / RT-AO / RT デバッグ（同じ深度 + TLAS）---
        // ★出力先は既存の枠。RT 影 → コンタクトシャドウ枠(t11)、RT-AO → SSAO 枠(t8)。
        //   ルートシグネチャも b1 も 1 ビットも増えない。フォワード PS は無改造のまま。
        if (useRtShadow || useRtAo || useRtDebug)
        {
            m_gpuTimer->Begin(nativeCmdList, GpuTimer::RtScreen);
            RtScreenPass::GenerateDesc rd;
            rd.depthSrv  = m_srvHeap->GetGpuHandle(m_depthSrvIndex);
            // SSAO が実際に生成されていれば min 合成の相手にする。
            // 白 1x1 ダミーのままだと範囲外 Load が 0 を返して画面が真っ黒になるので、
            // ssaoValid を必ず添えること。
            rd.ssaoSrv   = m_srvHeap->GetGpuHandle(aoSrv);
            rd.ssaoValid = (aoSrv != m_ssaoWhiteSrvIndex);
            rd.tlas      = m_rtScene->GetTlasAddress();
            rd.view      = m_camera->GetViewMatrix();
            rd.proj      = m_camera->GetProjectionMatrix();   // ★ジッタなし
            rd.cameraPos = m_camera->GetPosition();
            rd.lightDir  = lightDirF3;
            rd.zNear     = m_camera->GetNearZ();
            rd.zFar      = m_camera->GetFarZ();
            rd.vpLeft = 0; rd.vpTop = 0; rd.vpW = rW; rd.vpH = rH;
            rd.frameIndex  = frameIndex;
            // 時間ディザは TAA が有効なときだけ回す（無効時に回すとチラつくだけ。PCSS と同じ方針）。
            rd.frameJitter = taaActive
                ? std::fmod(static_cast<f32>(m_perfTotalFrames & 0xFFFFull) * 0.61803398875f, 1.0f)
                : 0.0f;
            rd.debugRange  = m_renderDebugDepthRange;
            // G-Buffer（xy=oct 法線）。velocityPrepass が走ったフレームだけ中身が正しい。
            rd.gbufferSrv   = m_gbufferRT ? m_srvHeap->GetGpuHandle(m_gbufferRT->GetSrvIndex())
                                          : rd.depthSrv;
            rd.gbufferValid = velocityPrepass && m_gbufferRT != nullptr;
            // ★TAA の有無に関係なく必ず回す連番。frameJitter（TAA 有効時のみ非0）とは別物で、
            //   これが止まっているとレイ方向が毎フレーム同じになり、デノイザが収束しない。
            //   決定論キャプチャ中は 0 に固定してビット再現させる。
            rd.denoiseFrame = m_deterministicCapture
                            ? 0u : static_cast<u32>(m_perfTotalFrames & 0xFFFFull);
            // GeometryInfo テーブル（Build() が毎フレーム詰め直す）。0 なら
            // ヒット点のバインドレス情報は使えない＝アルベド可視化は黒になる。
            rd.geometryInfo = m_rtScene->GetGeometryInfoAddress();

            const RtSettings& rtCfg = m_scene->GetRtSettings();
            if (useRtShadow)
            {
                const u32 s = m_rtScreenPass->GenerateShadow(nativeCmdList, rd, rtCfg);
                if (s != DescriptorHeap::kInvalidIndex) csSrv = s;
            }
            if (useRtAo)
            {
                const u32 s = m_rtScreenPass->GenerateAo(nativeCmdList, rd, rtCfg);
                if (s != DescriptorHeap::kInvalidIndex) aoSrv = s;
            }
            if (useRtDebug)
            {
                rtDebugSrv = (m_renderDebugMode == static_cast<u32>(RenderDebugMode::RtAlbedo))
                           ? m_rtScreenPass->GenerateAlbedo(nativeCmdList, rd)
                           : m_rtScreenPass->GenerateDebug(nativeCmdList, rd);
            }
            m_gpuTimer->End(nativeCmdList, GpuTimer::RtScreen);
        }

        // --- SSR / SSGI 生成（深度 + G-Buffer + 速度 + 前フレームカラーをレイマーチ）---
        // ★前フレームカラーが無いフレーム（初回 / シーン切替直後 / リサイズ直後）は
        //   ScreenSpaceGiPass::Generate が何もせず kInvalidIndex を返す＝黒ダミーへフォールバック。
        m_gpuTimer->Begin(nativeCmdList, GpuTimer::ScreenSpaceGI);
        if ((useSsr || useSsgi) && velocityPrepass && m_screenSpaceGi->HasHistory())
        {
            ScreenSpaceGiPass::GenerateDesc gd;
            gd.ssr        = useSsr  ? &ssrCfg  : nullptr;
            gd.ssgi       = useSsgi ? &ssgiCfg : nullptr;
            gd.view       = m_camera->GetViewMatrix();
            gd.proj       = m_camera->GetProjectionMatrix();   // ジッタなし（深度線形化は無誤差）
            gd.zNear      = m_camera->GetNearZ();
            gd.zFar       = m_camera->GetFarZ();
            gd.vpLeft = 0; gd.vpTop = 0; gd.vpW = rW; gd.vpH = rH;
            gd.frameIndex = frameIndex;
            gd.hasIbl     = (m_iblReady && m_iblBaker != nullptr);
            gd.depthSrv     = m_srvHeap->GetGpuHandle(m_depthSrvIndex);
            gd.gbufferSrv   = m_srvHeap->GetGpuHandle(m_gbufferRT->GetSrvIndex());
            gd.velocitySrv  = m_srvHeap->GetGpuHandle(m_taaPass->GetVelocitySrvIndex());
            // irradiance キューブ(t5)。IBLBaker が居れば SRV ブロックは常に有効なので
            // 未ベイクでも型の合った TextureCube を張れる（中身は hasIbl=0 で読まれない）。
            // ★ここに Texture2D の黒ダミーを張ってはいけない（TextureCube 宣言と型不一致）。
            gd.irradianceSrv = m_srvHeap->GetGpuHandle(m_iblBaker->GetIrradianceSrv());
            m_screenSpaceGi->Generate(*m_commandList, gd, ssrSrv, ssgiSrv);
        }
        m_gpuTimer->End(nativeCmdList, GpuTimer::ScreenSpaceGI);

        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // プリパス/SSAO/コンタクトシャドウ/SSR/SSGI で RootSig/PSO/RT/ヒープを切り替えたので forward 用に再設定
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        m_commandList->SetRootSignature(*m_rootSignature);
    }
    m_gpuTimer->End(nativeCmdList, GpuTimer::PrepassSSAO);
    m_cpuMs[CpuPrepass] += std::chrono::duration<f32, std::milli>(
        std::chrono::high_resolution_clock::now() - _prepassT0).count();

    m_sceneRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    constexpr float clearColor[4] = {0.127f, 0.306f, 0.850f, 1.0f};  // リニア空間のコーンフラワーブルー
    m_commandList->ClearRenderTarget(m_sceneRT->GetRtv(), clearColor);
    // プリパス有効時は深度が完成済みなので forward では clear しない（再利用）。
    if (!useDepthPrepass)
        m_commandList->ClearDepthStencil(m_dsvHandle);
    m_commandList->SetRenderTarget(m_sceneRT->GetRtv(), m_dsvHandle);
    m_commandList->SetViewportAndScissor(rW, rH);

    m_commandList->SetPipelineState(*m_pipelineState);

    // PerFrame CB（ライト本体はクラスタードライティングの StructuredBuffer(t13) 側）
    // レイアウトは shaders/forward/Lighting.hlsli の PerFrameConstants と完全一致させること。
    struct FrameConstants {
        XMFLOAT4X4 view;
        XMFLOAT4X4 proj;
        XMFLOAT3   lightDir;
        float      time;
        XMFLOAT3   lightColor;
        float      ambientStrength;
        XMFLOAT4X4 cascadeViewProj[kNumCascades]; // 256B
        XMFLOAT4   cascadeSplitsView;             // 16B
        XMFLOAT4   shadowParams;                  // 16B
        XMFLOAT3   cameraPos;
        float      aoEnabled;   // 1=実AOを読む / 0=AO読まず ao=1（白ダミー1x1の範囲外Load=0で環境光が消えるのを防ぐ）
        u32        numPointLights;   // 統計/デバッグ用（シェーダは読まない）
        u32        numSpotLights;
        float      spotShadowTexel;   // 1/kSpotShadowMapSize
        float      pointShadowNear;
        // ▼ クラスタードライティング 64B (offset 480)。旧 pointLights[8]/spotLights[8] の跡地。
        XMFLOAT4   clusterParams;    // .x=zNear .y=zFar(クラスタ用) .z=sliceScale .w=sliceBias
        XMFLOAT4   clusterGrid;      // .x=gridX .y=gridY .z=gridZ .w=クラスタード有効(1/0)
        XMFLOAT4   clusterViewport;  // .xy=ビューポート原点(RT px) .zw=(gridX/vpW, gridY/vpH)
        XMFLOAT4   clusterExtra;     // .x=総灯数 .y=maxLightsPerCluster .z=デバッグ表示 .w=予約
        XMFLOAT4   pcssParams;                       // 16B  (offset 544) PCSS: .x=tanTheta(0で無効) .y=maxPenumbraTexels .z=時間ディザ位相 .w=探索半径texel
        // ▼ DDGI 48B (offset 560)。ddgiOrigin.w=0 なら PS は t22 を一切読まない
        XMFLOAT4   ddgiOrigin;                       // 16B  (offset 560) .xyz=格子の原点 .w=強さ(0=無効)
        XMFLOAT4   ddgiSpacing;                      // 16B  (offset 576) .xyz=プローブ間隔 .w=法線バイアス(m)
        XMFLOAT4   ddgiCounts;                       // 16B  (offset 592) .xyz=各軸のプローブ数
        XMFLOAT4   _clusterReserved[40];             // 640B (offset 608..1247)
        XMFLOAT4X4 spotShadowMatrix[kMaxShadowSpot]; // 256B (offset 1248)
        // ▼ IBL 制御 16B
        float iblIntensity;
        float maxPrefilterMip;
        u32   hasIBL;
        float skyboxIntensity;
        // ▼ コンタクトシャドウ制御 16B
        float contactShadowEnabled;  // 1=実テクスチャ(t11)を読む / 0=読まず 1.0（白ダミー1x1の範囲外Load=0対策）
        XMFLOAT3 _csPad;
    };
    static_assert(sizeof(FrameConstants) == 1536, "FrameConstants must be 1536 bytes");

    FrameConstants fc{};
    XMStoreFloat4x4(&fc.view, XMMatrixTranspose(m_camera->GetViewMatrix()));
    XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(m_camera->GetProjectionMatrix()));
    fc.lightDir = lightDirF3;
    fc.time = totalTime;
    fc.lightColor = lightColorF3;
    fc.ambientStrength = lightAmbient;
    // 編集用の照らし込み（F2 / 表示メニュー）。シーンの環境光へ「下限」として被せるだけなので、
    // 元から明るいシーンでは何も起きない。★Editor モード限定＝ゲームの絵は絶対に変わらないし、
    // DirectionalLight.ambient 自体は触っていないのでシーンにも保存されない。
    if (m_engineMode == EngineMode::Editor && m_editorCtx && m_editorCtx->viewportFill > 0.0f)
        fc.ambientStrength = (std::max)(fc.ambientStrength, m_editorCtx->viewportFill);
    // CSM: カスケード行列（HLSL は列優先 mul(row,mat) なので転置して格納）
    for (u32 i = 0; i < kNumCascades; ++i)
        XMStoreFloat4x4(&fc.cascadeViewProj[i],
            XMMatrixTranspose(XMLoadFloat4x4(&m_cascadeViewProj[i])));
    fc.cascadeSplitsView = {m_cascadeSplitsView[0], m_cascadeSplitsView[1],
                            m_cascadeSplitsView[2], m_cascadeSplitsView[3]};
    fc.shadowParams = {1.0f / static_cast<f32>(m_shadowMapSize), m_shadowDepthBias,
                       m_cascadeBlendBand, m_showCascadeDebug ? 1.0f : 0.0f};
    fc.cameraPos = m_camera->GetPosition();
    fc.spotShadowTexel = 1.0f / static_cast<f32>(kSpotShadowMapSize);
    fc.pointShadowNear = 0.1f;

    // ---- PCSS（ソフトシャドウ）----
    // .x = 0 なら HLSL 側は従来の 3x3 PCF 経路を通る＝絵はビット一致。
    // 時間ディザはフレーム連番 × 黄金比。★TAA が無効なときに回すとチラつくだけなので 0 にする
    //  （時間蓄積を持つパスはディザをフレーム連番×黄金比で回さないと収束しない、の裏返し）。
    {
        const ShadowPcssSettings& pcss = m_scene->GetShadowPcssSettings();
        const bool pcssOn = pcss.enabled && !m_camera->IsOrthographic()
                         && m_scene->GetShadowsEnabled();
        f32 phase = 0.0f;
        if (pcssOn && pcss.temporalDither && taaActive)
            phase = std::fmod(static_cast<f32>(m_perfTotalFrames & 0xFFFFull) * 0.61803398875f, 1.0f);
        fc.pcssParams = {
            pcssOn ? (std::max)(pcss.lightTanAngle, 1e-4f) : 0.0f,
            (std::max)(pcss.maxPenumbraTexels,   1.0f),
            phase,
            (std::max)(pcss.blockerSearchTexels, 1.0f)};
    }
    // ===== DDGI: フォワード PS へのバインド（計画09 Step 6 / 段階1）=====
    // ★プローブ更新（上の TLAS 直後）はもう終わっているので、ここでは「今フレーム読めるか」
    //   だけを見る。読めないなら 1x1 黒ダミーで t22 を埋めて ddgiOrigin.w=0 にする
    //   ＝ PS は 1 テクセルも読まず、絵は DDGI 導入前とビット一致する。
    {
        const DdgiSettings& ddgiCfg = m_scene->GetDdgiSettings();
        const bool ddgiActive = m_ddgi && ddgiCfg.enabled && ddgiCfg.intensity > 0.0f
                             && m_ddgi->GetIrradianceSrvIndex() != DescriptorHeap::kInvalidIndex;

        // t22 は slot11 テーブルの中（＝専用 SRV index では届かない）。毎フレーム書き直す。
        // ディスクリプタ 1 本の CreateSRV は数百 ns なので、変化検出を持つより安い。
        // ★BeginFrame がこのフレームの GPU 完了を待っているので、いま書いて安全。
        if (m_clusteredLighting && m_clusteredLighting->IsReady() && m_srvHeap)
        {
            const u32 block = m_clusteredLighting->GetSrvTableIndex(frameIndex);
            if (block != DescriptorHeap::kInvalidIndex)
            {
                const auto dst = m_srvHeap->GetCpuHandle(
                    block + ClusteredLightCulling::kDdgiSrvOffset);
                const bool wrote = ddgiActive
                                && m_ddgi->WriteIrradianceSrv(*m_graphicsDevice, dst);
                if (!wrote && m_ssBlackTex)
                    m_ssBlackTex->CreateSRV(*m_graphicsDevice, dst);   // 1x1 黒 RGBA16F

                // t23: 距離モーメント（段階2 の Chebyshev 可視性）。同じ扱い。
                const auto dstDist = m_srvHeap->GetCpuHandle(
                    block + ClusteredLightCulling::kDdgiDistSrvOffset);
                const bool wroteDist = ddgiActive
                                    && m_ddgi->WriteDistanceSrv(*m_graphicsDevice, dstDist);
                if (!wroteDist && m_ssBlackTex)
                    m_ssBlackTex->CreateSRV(*m_graphicsDevice, dstDist);
            }
        }

        if (ddgiActive)
        {
            // .w は「読むか」の 1/0 だけ。★intensity は BlendCS がアトラスへ書く時点で
            //   既に掛かっているので、ここで渡すと 2 乗になる（実機で踏んだ）。
            //   その代わり intensity の変更は hysteresis ぶんかけて絵に効く。
            fc.ddgiOrigin  = {ddgiCfg.originX, ddgiCfg.originY, ddgiCfg.originZ, 1.0f};
            fc.ddgiSpacing = {ddgiCfg.spacing, ddgiCfg.spacing, ddgiCfg.spacing,
                              (std::max)(ddgiCfg.normalBias, 0.0f)};
            fc.ddgiCounts  = {static_cast<f32>(ddgiCfg.probeCountX),
                              static_cast<f32>(ddgiCfg.probeCountY),
                              static_cast<f32>(ddgiCfg.probeCountZ), 0.0f};
        }
        else
        {
            fc.ddgiOrigin = fc.ddgiSpacing = fc.ddgiCounts = {0.0f, 0.0f, 0.0f, 0.0f};
        }
        m_ddgiActiveThisFrame = ddgiActive;
    }

    // スポット影行列（HLSL は列優先 mul(row,mat) なので転置して格納。上で割り当てたスロット分だけ埋める）
    for (u32 i = 0; i < kMaxShadowSpot; ++i)
        XMStoreFloat4x4(&fc.spotShadowMatrix[i],
            i < m_numSpotShadowSlots ? XMMatrixTranspose(XMLoadFloat4x4(&m_spotShadowViewProj[i])) : XMMatrixIdentity());

    // IBL 制御
    fc.iblIntensity    = m_iblReady ? m_iblIntensity : 0.0f;
    fc.maxPrefilterMip = m_iblBaker ? m_iblBaker->GetMaxPrefilterMip() : 4.0f;
    fc.hasIBL          = (m_iblReady && m_iblBaker && m_iblBaker->HasEnvironment()) ? 1u : 0u;
    fc.skyboxIntensity = m_skyboxIntensity;

    // AO: 実 AO テクスチャがバインドされている時だけシェーダで読む。SSAO 無効/正射/フォールバック時は
    // 白ダミー(1x1)で、Load は範囲外 0 を返して環境光を潰すため、シェーダ側で読まず ao=1 にする。
    fc.aoEnabled = (aoSrv != m_ssaoWhiteSrvIndex) ? 1.0f : 0.0f;

    // コンタクトシャドウも同じ規約（白ダミーが張られている時はシェーダ側で読まない）。
    fc.contactShadowEnabled = (csSrv != m_ssaoWhiteSrvIndex) ? 1.0f : 0.0f;

    // ===== ライト収集（point / spot を 1 本の配列へ統合してクラスタード用 SB へ送る）=====
    // 旧 8 灯固定配列は撤廃。上限は ClusteredLightCulling::kMaxSceneLights（1024）。
    // m_clusterLights はフレーム間で使い回すメンバ（毎フレーム malloc しない）。
    using ClusterLightGPU = ClusteredLightCulling::LightGPU;
    // ★ここは ECS を全走査して灯ごとに ComputeWorldMatrix を回す。灯が増えるほど効く。
    //   prepass と同じ理由で RAII を使わない（区間の外まで生きて二重計上になる）。
    const auto _lightsT0 = std::chrono::high_resolution_clock::now();
    m_clusterLights.clear();
    m_clusterLights.reserve(64);
    fc.numPointLights = 0;
    fc.numSpotLights  = 0;

    // PointLight を ECS から収集
    {
        auto& reg = m_scene->GetRegistry();
        auto plView = reg.view<const dx12e::PointLight, const Transform>();
        for (auto [e, pl, tf] : plView.each())
        {
            if (m_clusterLights.size() >= ClusteredLightCulling::kMaxSceneLights) break;
            ClusterLightGPU pld{};
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMStoreFloat3(&pld.position, world.r[3]);
            pld.range = pl.range;
            pld.color = {pl.color.x * pl.intensity,
                         pl.color.y * pl.intensity,
                         pl.color.z * pl.intensity};
            pld.type      = 0.0f;   // point
            pld.direction = {0.0f, 0.0f, 1.0f};
            pld.cosOuter  = -1.0f;
            pld.cosInner  = 1.0f;
            pld.sinOuter  = 0.0f;

            // 影スロット割当（上で計算済みの m_pointShadowEntity[]）と突合
            pld.shadowIndex = -1.0f;
            for (u32 si = 0; si < m_numPointShadowSlots; ++si)
            {
                if (m_pointShadowEntity[si] == e) { pld.shadowIndex = static_cast<f32>(si); break; }
            }

            m_clusterLights.push_back(pld);
            fc.numPointLights++;
        }
    }

    // SpotLight を ECS から収集（位置=Transform、軸=direction、内外コーン角を cos へ）
    {
        auto& reg = m_scene->GetRegistry();
        auto slView = reg.view<const dx12e::SpotLight, const Transform>();
        for (auto [e, sl, tf] : slView.each())
        {
            if (m_clusterLights.size() >= ClusteredLightCulling::kMaxSceneLights) break;
            ClusterLightGPU sld{};
            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();
            XMStoreFloat3(&sld.position, world.r[3]);
            sld.range = sl.range;
            sld.type  = 1.0f;   // spot

            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&sl.direction));
            XMStoreFloat3(&sld.direction, dir);

            // outer >= inner を保証してから cos 化（cos は単調減少なので inner の cos の方が大きい）
            float outerDeg = (std::max)(sl.outerConeDeg, sl.innerConeDeg);
            sld.cosInner = std::cos(XMConvertToRadians(sl.innerConeDeg));
            sld.cosOuter = std::cos(XMConvertToRadians(outerDeg));
            // 円錐カリング用の sin（GPU で acos を回さないよう CPU で 1 回だけ）
            sld.sinOuter = std::sqrt((std::max)(0.0f, 1.0f - sld.cosOuter * sld.cosOuter));

            sld.color = {sl.color.x * sl.intensity,
                         sl.color.y * sl.intensity,
                         sl.color.z * sl.intensity};

            // 影スロット割当（上で計算済みの m_spotShadowEntity[]）と突合
            sld.shadowIndex = -1.0f;
            for (u32 si = 0; si < m_numSpotShadowSlots; ++si)
            {
                if (m_spotShadowEntity[si] == e) { sld.shadowIndex = static_cast<f32>(si); break; }
            }

            m_clusterLights.push_back(sld);
            fc.numSpotLights++;
        }
    }

    // パーティクルライト: light=true の明るい粒子上位を空き枠へ注ぐ
    // （炎や魔法が実際に周囲を照らす。シーン配置のライトが優先）
    if (m_particleSystem && m_clusterLights.size() < ClusteredLightCulling::kMaxSceneLights)
    {
        const u32 room = ClusteredLightCulling::kMaxSceneLights
                       - static_cast<u32>(m_clusterLights.size());
        // 粒子ライトは実用上せいぜい数十灯。1024 枠ぶんの一時配列をスタックへ積むのは
        // 無駄なので受け皿は 64 で頭打ちにする（従来は 8 だった）。
        constexpr u32 kMaxParticleLights = 64;
        ParticleSystem::LightInfo pls[kMaxParticleLights];
        const u32 want = (std::min)(room, kMaxParticleLights);
        const u32 got = m_particleSystem->CollectLights(want, pls);
        for (u32 li = 0; li < got; ++li)
        {
            ClusterLightGPU pld{};
            pld.position    = pls[li].pos;
            pld.range       = pls[li].range;
            pld.color       = pls[li].color;
            pld.type        = 0.0f;
            pld.direction   = {0.0f, 0.0f, 1.0f};
            pld.cosOuter    = -1.0f;
            pld.cosInner    = 1.0f;
            pld.sinOuter    = 0.0f;
            pld.shadowIndex = -1.0f;
            m_clusterLights.push_back(pld);
            fc.numPointLights++;
        }
    }

    // ===== デカールの収集（sortOrder 昇順）=====
    // ★クラスタのフォールバック経路（正射 / プレビュー / 設定 OFF）にはデカールリストが無いので、
    //   そのときは 0 個扱いにしてフォワード PS の分岐ごと切る。
    m_decalEntries.clear();
    if (m_decalSystem && m_decalSystem->IsReady())
    {
        auto& reg = m_scene->GetRegistry();
        auto dView = reg.view<const Transform, const DecalComponent>();
        for (auto [e, tf, dc] : dView.each())
        {
            if (m_decalEntries.size() >= DecalSystem::kMaxDecals) break;
            if (dc.opacity <= 0.0f) continue;

            XMMATRIX world = (tf.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : tf.GetWorldMatrix();

            DecalEntry entry{};
            entry.sortOrder = dc.sortOrder;
            auto& d = entry.gpu;
            XMStoreFloat4x4(&d.invWorld, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
            // 投影軸 = ローカル +Y のワールド方向 / 接線 = ローカル +X のワールド方向。
            // invWorld から逆算せずここで直接送る（PS 側の逆行列計算を丸ごと省ける）。
            XMStoreFloat3(&d.axisW,    XMVector3Normalize(world.r[1]));
            XMStoreFloat3(&d.tangentW, XMVector3Normalize(world.r[0]));
            d.atlasUV        = dc.atlasUV;
            d.atlasUVNormal  = dc.atlasUVNormal;
            d.tint           = dc.tint;
            d.opacity        = dc.opacity;
            d.emissive       = dc.emissive;
            d.normalStrength = dc.normalStrength;
            d.roughness      = dc.roughness;
            d.metallic       = dc.metallic;
            // cos は CPU で 1 回だけ（GPU で acos/cos を回さない。クラスタライトの sinOuter と同じ流儀）
            const f32 fadeDeg = (std::min)((std::max)(dc.angleFadeDeg, 0.0f), 89.0f);
            d.cosAngleFade   = std::cos(XMConvertToRadians(fadeDeg));
            d.fadeEdge       = (std::max)(dc.fadeEdge, 0.001f);
            m_decalEntries.push_back(entry);
        }
        std::stable_sort(m_decalEntries.begin(), m_decalEntries.end(),
                         [](const DecalEntry& a, const DecalEntry& b) { return a.sortOrder < b.sortOrder; });
        m_decalGpu.clear();
        m_decalGpu.reserve(m_decalEntries.size());
        for (const auto& en : m_decalEntries) m_decalGpu.push_back(en.gpu);
    }

    // ===== クラスタードライティングのパラメータ =====
    // クラスタ AABB の構築が透視前提なので、正射カメラ（俯瞰ゲーム / 2D ビュー）と
    // 設定 OFF のときは「先頭 64 灯の総当たり」フォールバックへ倒す（旧 8 灯より緩い）。
    const u32 numClusterLights = static_cast<u32>(m_clusterLights.size());
    const bool clusterOn = m_clusteredEnabled && m_clusteredLighting
                        && m_clusteredLighting->IsReady() && !m_camera->IsOrthographic();
    {
        const float zN    = (std::max)(m_camera->GetNearZ(), 0.001f);
        const float zFcam = (std::max)(m_camera->GetFarZ(), zN + 1.0f);
        const float zFcl  = (std::max)((std::min)(zFcam, cluster::kClusterFarLimit), zN + 1.0f);

        fc.clusterParams = {zN, zFcl,
                            cluster::SliceScale(zN, zFcl), cluster::SliceBias(zN, zFcl)};
        fc.clusterGrid   = {static_cast<f32>(cluster::kGridX),
                            static_cast<f32>(cluster::kGridY),
                            static_cast<f32>(cluster::kGridZ),
                            clusterOn ? 1.0f : 0.0f};
        // SV_Position.xy は RT 座標。#16 でシーンは RT 全面に描くようになったので原点は常に 0
        // （かつては m_sceneRT のサブ矩形に描いていたので vpLeft/vpTop を引いていた）。
        fc.clusterViewport = {0.0f, 0.0f,
                              static_cast<f32>(cluster::kGridX) / static_cast<f32>(rW),
                              static_cast<f32>(cluster::kGridY) / static_cast<f32>(rH)};
        // デバッグ表示はエディタのライティング窓から（ゲームモードは常に 0）
        m_clusterDebugMode = m_editorCtx ? m_editorCtx->clusterDebugMode : 0u;
        // ★clusterExtra.w は計画02 が「予約」として空けておいた枠。
        //   計画06 のデカール数がここに入る（PerFrameConstants のレイアウトは 1 バイトも動かない）。
        //   0 ならフォワード PS のデカールブロックが [branch] で丸ごと飛ぶ。
        fc.clusterExtra    = {static_cast<f32>(numClusterLights),
                              static_cast<f32>(cluster::kMaxLightsPerCluster),
                              static_cast<f32>(m_clusterDebugMode),
                              clusterOn ? static_cast<f32>(m_decalGpu.size()) : 0.0f};
    }

    m_perFrameCB->Update(&fc, sizeof(fc), frameIndex);

    // ===== クラスタライトカリング（compute 2 パス）=====
    // ライトを UPLOAD リングへ書いてから AABB 構築 → カリング。呼び出し後は
    // インデックス/カウントが PIXEL_SHADER_RESOURCE 状態になる。
    // ★compute は PSO を共有するので、直後にグラフィクスの RootSig/PSO を必ず再設定する。
    u32 ddgiLightSrvIndex = DescriptorHeap::kInvalidIndex;   // DDGI が読む t13 の SRV index
    u32 ddgiLightCount    = 0;
    if (m_clusteredLighting && m_clusteredLighting->IsReady())
    {
        const u32 uploaded = m_clusteredLighting->UploadLights(
            m_clusterLights.data(), numClusterLights, frameIndex);
        if (clusterOn)
        {
            XMFLOAT4X4 projF;
            XMStoreFloat4x4(&projF, m_camera->GetProjectionMatrix());
            m_gpuTimer->Begin(nativeCmdList, GpuTimer::ClusterCull);
            m_clusteredLighting->Dispatch(nativeCmdList, m_camera->GetViewMatrix(),
                                          projF._11, projF._22,
                                          fc.clusterParams.x, fc.clusterParams.y,
                                          m_camera->GetFarZ(), uploaded, frameIndex);
            m_gpuTimer->End(nativeCmdList, GpuTimer::ClusterCull);
            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
            m_commandList->SetRootSignature(*m_rootSignature);
            m_commandList->SetPipelineState(*m_pipelineState);
        }
        else
        {
            // フォールバック（正射 / 設定 OFF）でもテーブルはバインドするので、
            // インデックス/カウントは読取状態にしておく（中身は読まれない）。
            m_clusteredLighting->EnsureReadable(nativeCmdList);
        }
        ddgiLightSrvIndex = m_clusteredLighting->GetSrvTableIndex(frameIndex);  // = t13
        ddgiLightCount    = uploaded;
    }
    // ライト収集〜クラスタ転送まで（灯数に比例する CPU コスト）。
    m_cpuMs[CpuLights] += std::chrono::duration<f32, std::milli>(
        std::chrono::high_resolution_clock::now() - _lightsT0).count();

    // ===== DDGI: プローブ更新（計画09 Step 6）=====
    // ★必ず UploadLights の【後】に置くこと。プローブは点光源/スポットを t13 から
    //   総当たりで拾うので、ここより前で回すと前フレームの灯りを見てしまう
    //   （影スロットの割当も上のシャドウパスで初めて確定する）。
    // ★t14/t15（クラスタのインデックス/カウント）は使わない。あれは画面空間のクラスタで、
    //   視錐台の外にあるプローブには対応するクラスタが存在しないため。
    // プローブは画面空間ではないので compute。ヒット点は Step 5 のバインドレスを流用。
    // ★compute は PSO を共有するので、直後にグラフィクスの RootSig/PSO を再設定する。
    if (ddgiTlasOk && m_ddgi && m_ddgi->IsReady())
    {
        m_gpuTimer->Begin(nativeCmdList, GpuTimer::Ddgi);
        DdgiVolume::UpdateDesc dd;
        dd.tlas         = ddgiTlas;
        dd.geometryInfo = ddgiGeoInfo;
        dd.sunDir       = lightDirF3;
        // ★lightColorF3 には既に intensity が掛かっている（color * intensity）。
        //   ここで再度掛けると二重になるので 1.0 にする。
        dd.sunColor     = lightColorF3;
        dd.sunIntensity = 1.0f;
        // ミス時の放射輝度。envMap があるならフォワードの拡散 IBL と
        // 【同じ irradiance キューブ】を bindless で引かせる（段階1）。
        // ★これが無いと空の見えている面まで真っ暗になり、DDGI を ON にした
        //   瞬間にシーン全体が暗くなる（実機で踏んだ）。
        // envMap が無い屋内は従来どおり環境光の下限をスカラーで使う。
        //   ここを明るくしすぎると壁の外から光が漏れてくるので控えめに。
        dd.skyColor     = {lightAmbient, lightAmbient, lightAmbient};
        dd.skyCubeSrvIndex =
            (m_iblReady && m_iblBaker && m_iblBaker->HasEnvironment())
                ? m_iblBaker->GetIrradianceSrv() : 0xFFFFFFFFu;
        // 点光源/スポット（t13 のライト配列を bindless で引く）。屋内はこれが本体。
        dd.lightSrvIndex = ddgiLightSrvIndex;
        dd.lightCount    = ddgiLightCount;
        dd.frameIndex   = m_deterministicCapture
                        ? 0u : static_cast<u32>(m_perfTotalFrames & 0xFFFFull);
        m_ddgi->Update(nativeCmdList, *m_graphicsDevice, m_scene->GetDdgiSettings(), dd);
        m_gpuTimer->End(nativeCmdList, GpuTimer::Ddgi);
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        m_commandList->SetRootSignature(*m_rootSignature);
        m_commandList->SetPipelineState(*m_pipelineState);
    }

    // ===== デカール: アトラス解決 → アップロード → クラスタへビニング =====
    // ★クラスタカリングの直後（同じ compute のかたまり）に置く。
    //   フォワード PS は t18..t21 を kSlotClusterSRV のテーブル越しに読むので、
    //   バインドはクラスタライトと同じ 1 本のままで増えない。
    if (m_decalSystem && m_decalSystem->IsReady()
        && m_clusteredLighting && m_clusteredLighting->IsReady())
    {
        // アトラスの解決（パスが変わったときだけ。空なら 1x1 黒ダミー＝アルファ 0 で不可視）。
        const std::string& atlasPath = m_scene->GetDecalAtlasPath();
        if (atlasPath != m_decalAtlasLoaded)
        {
            m_decalAtlasLoaded   = atlasPath;
            m_decalAtlasSrvIndex = DescriptorHeap::kInvalidIndex;
            m_decalAtlasTex      = nullptr;
            if (!atlasPath.empty() && m_resourceManager)
            {
                const std::wstring wpath =
                    PathResolver::Utf8ToWide(PathResolver::AssetsDir() + atlasPath);
                if (Texture* tex = m_resourceManager->GetOrLoadTexture(
                        wpath, nativeCmdList, /*srgb*/ true, TextureUsage::BaseColor))
                {
                    m_decalAtlasSrvIndex = tex->GetSrvIndex();
                    m_decalAtlasTex      = tex;
                }
                if (m_decalAtlasSrvIndex == DescriptorHeap::kInvalidIndex)
                    Logger::Warn("デカールアトラスを読めませんでした: {}", atlasPath);
            }
            m_decalSrvDirty = true;
        }
        if (m_decalSrvDirty)
        {
            Texture* atlasTex = m_decalAtlasTex ? m_decalAtlasTex : m_ssBlackTex.get();
            if (atlasTex)
            {
                for (u32 f = 0; f < DecalSystem::kFrameCount; ++f)
                {
                    const u32 block = m_clusteredLighting->GetSrvTableIndex(f);
                    m_decalSystem->WriteSrvsInto(*m_graphicsDevice, *m_srvHeap, block, f, atlasTex);
                }
                m_decalSrvDirty = false;
            }
        }

        const u32 uploadedDecals =
            m_decalSystem->Upload(m_decalGpu.data(), static_cast<u32>(m_decalGpu.size()), frameIndex);
        if (clusterOn && uploadedDecals > 0)
        {
            XMFLOAT4X4 projF;
            XMStoreFloat4x4(&projF, m_camera->GetProjectionMatrix());
            m_decalSystem->Cull(nativeCmdList, m_camera->GetViewMatrix(),
                                projF._11, projF._22,
                                fc.clusterParams.x, fc.clusterParams.y,
                                m_camera->GetFarZ(), uploadedDecals, frameIndex);
            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
            m_commandList->SetRootSignature(*m_rootSignature);
            m_commandList->SetPipelineState(*m_pipelineState);
        }
        else
        {
            // デカール 0 個でもテーブルはバインドするので読取状態にしておく
            // （フォワード PS は clusterExtra.w==0 で読まないが、状態は正しく保つ）。
            m_decalSystem->EnsureReadable(nativeCmdList);
        }
    }

    // ===== ボリュメトリックフォグ: froxel ボリュームの構築（compute 3 パス）=====
    // クラスタカリングの直後に置く理由: 散乱パスがクラスタライトリストを読むため。
    // ここは (1) CSM が完成済み・(2) カスケード行列/分割が確定済み・(3) 深度は DEPTH_WRITE のまま
    // ＝ フォグの compute が誰の状態も壊さない位置。合成はパーティクル直前で行う（下）。
    // ★compute は PSO を graphics と共有するので、直後に RootSig/PSO/ヒープを必ず再設定する。
    const VolumetricFogSettings& fogCfg = m_scene->GetVolumetricFogSettings();
    // 透視限定（froxel の Z 分布と深度線形化が透視前提）。正射 / 2D ビューでは丸ごと素通し。
    // ★クラスタライトリスト（t3..t5）は散乱シェーダが必ず参照する＝テーブルを必ずバインドする
    //   必要があるので、クラスタードライティングが生きていることをフォグの前提条件にしている。
    //   （生きていないのは初期化に失敗したときだけ。その場合フォグを諦める方が安全。）
    const bool volFogActive = fogCfg.enabled && fogCfg.density > 0.0f
                            && m_volumetricFogPass && m_volumetricFogPass->IsReady()
                            && m_clusteredLighting && m_clusteredLighting->IsReady()
                            && viewSupportsScreenSpace;
    bool volFogBuilt = false;
    if (volFogActive)
    {
        m_gpuTimer->Begin(nativeCmdList, GpuTimer::VolumetricFog);
        // 影テクスチャは PIXEL_SHADER_RESOURCE で置かれている。compute から読むには NON_PIXEL が要る。
        // （クラスタのインデックス/カウントは ClusteredLightCulling が最初から
        //   PIXEL|NON_PIXEL の合成状態で置いているので遷移不要。）
        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_commandList->TransitionResource(m_spotShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_commandList->TransitionResource(m_pointShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        VolumetricFogPass::ViewParams fv{};
        fv.view         = m_camera->GetViewMatrix();          // ジッタなし
        fv.prevViewProj = m_prevViewProjNJValid ? XMLoadFloat4x4(&m_prevViewProjNoJitter) : camVP;
        {
            XMFLOAT4X4 projF;
            XMStoreFloat4x4(&projF, m_camera->GetProjectionMatrix());
            fv.proj11 = projF._11;
            fv.proj22 = projF._22;
        }
        fv.cameraPos        = fc.cameraPos;
        fv.sunDir           = lightDirF3;
        fv.sunColor         = lightColorF3;
        fv.cascadeViewProj  = m_cascadeViewProj;              // 非転置（パス内で転置する）
        fv.cascadeSplits    = fc.cascadeSplitsView;
        fv.shadowParams     = fc.shadowParams;
        fv.clusterParams    = fc.clusterParams;
        fv.clusterGrid      = fc.clusterGrid;
        fv.vpLeft = 0; fv.vpTop = 0; fv.vpW = rW; fv.vpH = rH;
        fv.nearZ = m_camera->GetNearZ();
        fv.farZ  = m_camera->GetFarZ();
        fv.numLights     = numClusterLights;
        fv.maxPerCluster = cluster::kMaxLightsPerCluster;
        fv.spotShadowMatrixTransposed = fc.spotShadowMatrix;   // ★転置済み（未使用枠は単位行列）
        fv.spotShadowTexel = fc.spotShadowTexel;
        fv.pointShadowNear = fc.pointShadowNear;
        fv.csmSrv          = m_srvHeap->GetGpuHandle(m_shadowSrvIndex);
        fv.clusterSrv      = m_clusteredLighting->GetSrvTable(frameIndex);
        // t9,t10 と同じ「スポット影配列 → ポイント影キューブ配列」の 2 本連番。
        fv.punctualShadowSrv = m_srvHeap->GetGpuHandle(m_spotShadowSrvIndex);

        m_volumetricFogPass->BuildVolumes(nativeCmdList, *m_graphicsDevice, fogCfg, fv, frameIndex);
        volFogBuilt = m_volumetricFogPass->VolumesAllocated();

        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->TransitionResource(m_spotShadowMap.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->TransitionResource(m_pointShadowMap.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_gpuTimer->End(nativeCmdList, GpuTimer::VolumetricFog);

        // compute で PSO/RootSig を奪ったので forward 用に戻す（クラスタカリング直後と同じ作法）。
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        m_commandList->SetRootSignature(*m_rootSignature);
        m_commandList->SetPipelineState(*m_pipelineState);
        m_commandList->SetRenderTarget(m_sceneRT->GetRtv(), m_dsvHandle);
        m_commandList->SetViewportAndScissor(rW, rH);
    }

    // ===== Skybox（不透明描画の前に全画面塗り。深度テスト OFF なので後続不透明が上書き）=====
    // skybox は自前 RootSig/PSO を bind するため、直後にメイン RootSig/PSO を再設定してから
    // per-frame CBV / shadow / IBL を bind し直す。
    if (m_iblReady && m_drawSkybox && m_skyboxIntensity > 0.0f && m_skyboxRenderer &&
        m_iblBaker && m_iblBaker->HasEnvironment() &&
        m_envCubeSrvIndex != DescriptorHeap::kInvalidIndex)
    {
        // スカイボックスもジッタさせる（させないと TAA で空だけ滲む）。
        XMFLOAT4X4 invVP;
        XMStoreFloat4x4(&invVP, XMMatrixTranspose(XMMatrixInverse(nullptr, camVPJ)));
        m_skyboxRenderer->Render(nativeCmdList, m_srvHeap->GetGpuHandle(m_envCubeSrvIndex),
                                 invVP, m_skyboxIntensity);
        // メイン RootSig / PSO を再設定
        m_commandList->SetRootSignature(*m_rootSignature);
        m_commandList->SetPipelineState(*m_pipelineState);
    }

    m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, m_perFrameCB->GetGpuAddress(frameIndex));

    // シャドウマップSRVをバインド
    m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
        m_srvHeap->GetGpuHandle(m_shadowSrvIndex));

    // スポット/ポイント影SRV(t9,t10)をバインド（連番確保なのでスポット側の1個渡しで2枚とも有効になる）
    m_commandList->SetSRVTable(RootSignature::kSlotPunctualShadowSRV,
        m_srvHeap->GetGpuHandle(m_spotShadowSrvIndex));

    // IBL テーブル(t5,t6,t7)をバインド（常に有効＝ダミー含む。hasIBL で読むか分岐）
    if (m_iblReady && m_iblBaker)
        m_commandList->SetSRVTable(RootSignature::kSlotIBLTable,
            m_srvHeap->GetGpuHandle(m_iblBaker->GetIrradianceSrv()));

    // クラスタライト テーブル(t13,t14,t15 + デカール予約 t18..t21)をバインド。
    // フォワード PS が t13..t15 を参照する以上、クラスタード無効時（フォールバック経路）でも
    // 必ずバインドが要る（未バインドのテーブルはデバッグレイヤ違反）。
    if (m_clusteredLighting && m_clusteredLighting->IsReady())
        m_commandList->SetSRVTable(RootSignature::kSlotClusterSRV,
            m_clusteredLighting->GetSrvTable(frameIndex));

    // フォワード本体はジッタあり（深度プリパスとビット一致させる）。
    XMMATRIX viewProj = camVPJ;

    // 全Entityを描画（メインパス: 編集カメラ視点）。AO / コンタクトシャドウは有効時のみ実テクスチャ、
    // 無効時は白（＝素通し）。深度プリパスが走ったときだけ深度が完成済み →
    // LESS_EQUAL forward PSO で再利用する。
    m_gpuTimer->Begin(nativeCmdList, GpuTimer::MainScene);
    m_passBucket = &m_passMain;
    {
        CpuScopeTimer _tMain(&m_cpuMs[CpuMainRec]); DX12_PROFILE_ZONE_N("Rec/Main");
        RenderSceneMeshes(nativeCmdList, frameIndex, viewProj,
                          (m_isGameMode || m_engineMode == EngineMode::Playing), aoSrv,
                          useDepthPrepass, csSrv, ssrSrv, ssgiSrv);
    }
    m_passBucket = &m_passOther;
    m_gpuTimer->End(nativeCmdList, GpuTimer::MainScene);

    // ---- Physics Debug Draw（オフスクリーン RT へ）----
    if (m_physicsDebugDraw && m_physicsDebugRenderer->IsEnabled())
    {
        m_physicsDebugRenderer->BeginFrame();
        m_physicsDebugRenderer->CollectFromRegistry(m_scene->GetRegistry());

        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, XMMatrixTranspose(camVPJ));
        m_physicsDebugRenderer->Render(nativeCmdList, vp);
    }

    // ---- パーティクル（プロシージャル質感ビルボード）: HDR scene RT へ ----
    // エディタ編集中も描画する（配置エミッタ/トレイルの常時プレビュー。従来は Play/ゲームのみ）
    m_gpuTimer->Begin(nativeCmdList, GpuTimer::Particles);
    bool particleDistortDrawn = false;
    if (m_particleSystem)
    {
        // 深度を読み取り可能へ遷移し soft particles 用 SRV を供給。DSV はバインドせず PS で手動オクルージョン。
        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        auto srtv = m_sceneRT->GetRtv();
        nativeCmdList->OMSetRenderTargets(1, &srtv, FALSE, nullptr);
        m_commandList->SetViewportAndScissor(rW, rH);
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());

        // ---- ボリュメトリックフォグの合成（フルスクリーン 1 枚をブレンドで scene RT へ）----
        // ★ここに挿すのは偶然ではない。(1) 深度が既に PIXEL_SHADER_RESOURCE、
        //   (2) OMSetRenderTargets が DSV を意図的にバインドしていない、
        //   (3) scene RT がまだ RENDER_TARGET —— の 3 条件が同時に揃う唯一の場所で、
        //   深度の遷移を 1 回も追加せずに済む。
        // パーティクルより「前」なので、加算合成のパーティクルにはフォグがかからない（意図どおり）。
        // ★合成の GPU 時間は GpuTimer::Particles の内数になる（GpuTimer は 1 スコープにつき
        //   フレーム 1 回の Begin/End しか記録できないので、volFog スコープは compute 3 パス専用）。
        if (volFogBuilt)
        {
            m_volumetricFogPass->Composite(nativeCmdList,
                m_srvHeap->GetGpuHandle(m_depthSrvIndex),
                0u, 0u, rW, rH, frameIndex);
            // フォグ用の RootSig/PSO を張ったので、後続（パーティクル）のために作法どおり戻す。
            nativeCmdList->OMSetRenderTargets(1, &srtv, FALSE, nullptr);
            m_commandList->SetViewportAndScissor(rW, rH);
            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        }

        XMMATRIX invView = XMMatrixInverse(nullptr, m_camera->GetViewMatrix());
        XMFLOAT3 camRight, camUp, camPos;
        XMStoreFloat3(&camRight, invView.r[0]);
        XMStoreFloat3(&camUp,    invView.r[1]);
        XMStoreFloat3(&camPos,   invView.r[3]);

        XMFLOAT4X4 proj; XMStoreFloat4x4(&proj, m_camera->GetProjectionMatrix());
        const float rtw = static_cast<float>(m_sceneRT->GetWidth());
        const float rth = static_cast<float>(m_sceneRT->GetHeight());
        if (m_depthSrvIndex != DescriptorHeap::kInvalidIndex)
            m_particleSystem->SetSceneDepth(m_srvHeap->GetGpuHandle(m_depthSrvIndex),
                proj._33, proj._43, 1.0f / rtw, 1.0f / rth);
        else
            m_particleSystem->DisableSceneDepth();
        m_particleSystem->SetTime(totalTime);
        // ラスタライズ系はジッタあり（TAA でアンチエイリアスされる）。
        m_particleSystem->Render(nativeCmdList, camVPJ, camRight, camUp, camPos);

        // ---- GPUパーティクル（compute シム + ExecuteIndirect）: 同じ HDR RT へ加算 ----
        if (m_gpuParticles)
        {
            if (m_depthSrvIndex != DescriptorHeap::kInvalidIndex)
                m_gpuParticles->SetSceneDepth(m_srvHeap->GetGpuHandle(m_depthSrvIndex),
                    proj._33, proj._43, 1.0f / rtw, 1.0f / rth);
            else
                m_gpuParticles->DisableSceneDepth();
            // 決定論キャプチャ中は dt=0（#31。GPU 粒子が前進すると 2 枚が一致しない）。
            m_gpuParticles->SimulateAndRender(nativeCmdList,
                                              m_deterministicCapture ? 0.0f : m_gameClock.GetDeltaTime(),
                                              totalTime, camVPJ, camRight, camUp);
        }

        // ---- 歪みパーティクル（熱ゆらぎ/衝撃波）: 歪みバッファ(RG16F)へ ----
        // Render() と同一フレームのインスタンスバッファを共有するため直後に描く。
        if (m_particleSystem->HasDistortion() && m_distortRT)
        {
            m_distortRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float distClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            m_commandList->ClearRenderTarget(m_distortRT->GetRtv(), distClear);
            auto drtv = m_distortRT->GetRtv();
            nativeCmdList->OMSetRenderTargets(1, &drtv, FALSE, nullptr);
            m_commandList->SetViewportAndScissor(rW, rH);
            m_particleSystem->RenderDistortion(nativeCmdList, camVPJ, camRight, camUp);
            m_distortRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            particleDistortDrawn = true;
        }

        m_commandList->TransitionResource(m_depthBuffer.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    m_gpuTimer->End(nativeCmdList, GpuTimer::Particles);

    // ---- ワールド空間 2D スプライト（Sprite2D, worldSpace=true）: HDR scene RT へ ----
    // 各スプライトをエンティティのワールド行列で配置（3D 空間の任意位置/向き/スケール、billboard 可）。
    // layer 昇順ソート・アルファブレンド・深度テスト(書込みOFF)。PostProcess 前なのでブルーム等の対象。
    // ゲームビュー/Play に加え、エディタのシーンビュー(編集中)でも描画して配置を可視化する。
    if (m_spriteRenderer) m_spriteRenderer->BeginWorldVertexFrame();  // 本フレームの頂点書込みを先頭へ
    {
        XMMATRIX invView = XMMatrixInverse(nullptr, m_camera->GetViewMatrix());
        XMFLOAT3 camRight, camUp;
        XMStoreFloat3(&camRight, invView.r[0]);
        XMStoreFloat3(&camUp,    invView.r[1]);
        DrawWorldSprites(nativeCmdList, camVPJ, camRight, camUp,
                         m_sceneRT->GetRtv(), m_dsvHandle, 0u, 0u, rW, rH, totalTime);
    }

    // ===== 中間バッファ可視化（dx12_render_debug）=====
    // ★ここに挿す理由: シーン RT がまだ RENDER_TARGET で、ポストチェーンより前。
    //   readback（CaptureSceneScreenshot）は m_sceneRT を読むので必ず絵に写る（B5 の罠を回避）。
    //   フォワード PS には 1 行も足していない（N24: [branch] でも occupancy が落ちる）。
    if (m_renderDebugMode != 0 && m_renderDebugPass && m_renderDebugPass->IsReady()
        && m_depthSrvIndex != DescriptorHeap::kInvalidIndex)
    {
        const auto dbgMode = static_cast<RenderDebugMode>(m_renderDebugMode);
        u32 srcIdx = DescriptorHeap::kInvalidIndex;
        switch (dbgMode)
        {
        case RenderDebugMode::Normal:
        case RenderDebugMode::Roughness:
        case RenderDebugMode::Metallic:
            if (velocityPrepass && m_gbufferRT) srcIdx = m_gbufferRT->GetSrvIndex();
            break;
        case RenderDebugMode::Depth:          srcIdx = m_depthSrvIndex; break;   // t0 は使わないが有効な物を渡す
        case RenderDebugMode::Ao:             srcIdx = aoSrv; break;
        case RenderDebugMode::ContactShadow:  srcIdx = csSrv; break;
        case RenderDebugMode::Velocity:
            if (velocityPrepass && m_taaPass) srcIdx = m_taaPass->GetVelocitySrvIndex();
            break;
        case RenderDebugMode::Ssr:            srcIdx = ssrSrv;  break;
        case RenderDebugMode::Ssgi:           srcIdx = ssgiSrv; break;
        case RenderDebugMode::RtHit:
        case RenderDebugMode::RtDiff:
        case RenderDebugMode::RtAlbedo:       srcIdx = rtDebugSrv; break;
        default: break;
        }

        if (srcIdx != DescriptorHeap::kInvalidIndex)
        {
            // 深度は DEPTH_WRITE のままなので PS から読める状態へ往復させる。
            m_commandList->TransitionResource(m_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            auto drtv = m_sceneRT->GetRtv();
            nativeCmdList->OMSetRenderTargets(1, &drtv, FALSE, nullptr);   // DSV は張らない
            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());

            XMFLOAT4X4 dbgProj; XMStoreFloat4x4(&dbgProj, m_camera->GetProjectionMatrix());

            RenderDebugPass::DrawDesc dd{};
            dd.mode       = dbgMode;
            dd.sourceSrv  = m_srvHeap->GetGpuHandle(srcIdx);
            dd.depthSrv   = m_srvHeap->GetGpuHandle(m_depthSrvIndex);
            dd.vpLeft = 0; dd.vpTop = 0; dd.vpW = rW; dd.vpH = rH;
            dd.gain       = m_renderDebugGain;
            dd.projA      = dbgProj._33;
            dd.projB      = dbgProj._43;
            dd.depthRange = m_renderDebugDepthRange;
            dd.exposure   = m_renderDebugExposure;
            m_renderDebugPass->Draw(*m_commandList, dd);

            m_commandList->TransitionResource(m_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }

    // ===== 次フレームの SSR/SSGI 用に、このフレームの HDR シーンカラーを退避 =====
    // ★ポストチェーン（DoF/モーションブラー/ブルーム/トーンマップ）より前でなければならない。
    //   トーンマップ後の絵を GI ソースにすると露出変動でフィードバックが暴れる。
    //   m_sceneRT はリニア HDR で露出が焼き込まれていないので、その事故が構造的に起きない。
    //   この位置ならパーティクル / ワールドスプライト / 歪みまで含んだ「見えている絵」が入る。
    // ★m_sceneRT の ping-pong 化は禁止（33 参照あり）。CopyResource 1 発が唯一安全な手段。
    // ===== ポストプロセス: オフスクリーン RT → バックバッファ =====
    m_gpuTimer->Begin(nativeCmdList, GpuTimer::PostFX);
    if (m_screenSpaceGi && (useSsr || useSsgi))
        m_screenSpaceGi->CaptureSceneColor(*m_commandList, *m_sceneRT);
    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
    auto  rtv        = m_swapChain->GetCurrentRTV();

    // シーンRT は PS（ブルーム/uber）と CS（自動露出）の両方から読むので複合読取状態へ遷移
    m_sceneRT->Transition(*m_commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_commandList->TransitionResource(backBuffer,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    {
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());

        // ★#16: シーンは RT 全面に描かれているので「サブ矩形の UV」は常に (0,0,1,1)。
        //   かつてここで計算していた uvOfs/uvScl は 7 パスから丸ごと消えた。
        //   fullW/fullH ＝ レンダー解像度（テクセルサイズの供給元）。
        const f32 fullW = static_cast<f32>(m_sceneRT->GetWidth());
        const f32 fullH = static_cast<f32>(m_sceneRT->GetHeight());
        const auto sceneSrvGpu = m_srvHeap->GetGpuHandle(m_sceneRT->GetSrvIndex());

        // ポストエフェクトも Scene/Game で同じ設定を適用する。
        // ここを分けると「Scene では明るいのに Play すると暗い」など、ライティング調整が破綻する。
        PostProcessSettings ppApplied = m_scene->GetPostSettings();
        const bool isGameView = (m_isGameMode || m_engineMode == EngineMode::Playing);

        // ヒット時の画面インパクト（fx:pulse）: クロマ + 放射ブラーを瞬間的に上乗せ
        if (isGameView && m_particleSystem)
        {
            float pulse = m_particleSystem->GetPulse();
            if (pulse > 0.001f)
            {
                ppApplied.chromaticOn = true;
                ppApplied.chromatic   = (std::max)(ppApplied.chromatic, pulse * 1.2f);
                ppApplied.radialOn    = true;
                ppApplied.radial      = (std::max)(ppApplied.radial, pulse * 0.8f);
            }
        }

        // ---- TAA と FXAA の排他 ----
        // TAA 解決済みの絵に FXAA を掛けると輪郭が二重にぼける。TAA が走るなら FXAA は落とす。
        const bool taaResolve = taaResolveActive;
        if (taaResolve) ppApplied.fxaaOn = false;

        // ---- 深度依存パス（TAA/DoF/モーションブラー/ゴッドレイ）の準備 ----
        // 透視カメラのみ（正射は CoC/再投影/太陽投影が破綻するため無効）
        const bool persp = !m_camera->IsOrthographic();
        const bool wantDepthPost = ppApplied.enabled && persp &&
            m_depthBuffer && m_depthSrvIndex != DescriptorHeap::kInvalidIndex &&
            (ppApplied.dofOn || ppApplied.motionBlurOn || ppApplied.godraysOn);
        // TAA も深度を読む（空の速度再構成 + closest-depth dilation）。深度の遷移は
        // 1 箇所にまとめる＝二重遷移で D3D12 の状態追跡が壊れるのを防ぐ。
        const bool needDepthSrv = wantDepthPost || taaResolve;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu{};
        if (needDepthSrv)
        {
            m_commandList->TransitionResource(m_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            depthSrvGpu = m_srvHeap->GetGpuHandle(m_depthSrvIndex);
        }

        // ---- シーン変換チェーン: TAA → DoF → モーションブラー（結果を以降の「シーン」として使う）----
        D3D12_GPU_DESCRIPTOR_HANDLE curSceneSrv = sceneSrvGpu;

        // ★TAA はチェーンの先頭。トーンマップ前のリニア HDR に対して解決する。
        //   露出に依存するアーティファクトを避けるためと、以降の DoF / モーションブラー /
        //   自動露出 / ブルーム / レンズフレアが「安定した絵」を入力に取れるようにするため
        //   （ブルームとレンズフレアのちらつきが目に見えて減る）。
        if (taaResolve)
        {
            XMFLOAT4X4 invVpT, prevVpT;
            XMStoreFloat4x4(&invVpT, XMMatrixTranspose(XMMatrixInverse(nullptr, camVP)));
            XMStoreFloat4x4(&prevVpT, XMMatrixTranspose(
                m_prevViewProjNJValid ? XMLoadFloat4x4(&m_prevViewProjNoJitter) : camVP));
            const u32 o = m_taaPass->Resolve(*m_commandList, curSceneSrv, depthSrvGpu,
                invVpT, prevVpT, rW, rH, taaCfg);
            if (o != DescriptorHeap::kInvalidIndex)
                curSceneSrv = m_srvHeap->GetGpuHandle(o);
            // TaaPass は自前のルートシグネチャ/PSO を張るので、後段のために SRV ヒープを張り直す。
            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
        }
        if (wantDepthPost && ppApplied.dofOn && m_dofPass)
        {
            XMFLOAT4X4 projF;
            XMStoreFloat4x4(&projF, m_camera->GetProjectionMatrix());
            const u32 o = m_dofPass->Apply(*m_commandList, m_srvHeap.get(),
                curSceneSrv, depthSrvGpu, projF._33, projF._43, ppApplied);
            if (o != DescriptorHeap::kInvalidIndex)
                curSceneSrv = m_srvHeap->GetGpuHandle(o);
        }
        if (wantDepthPost && ppApplied.motionBlurOn && m_motionBlurPass && m_prevViewProjValid)
        {
            // モーションブラーの再投影はジッタなし（ジッタ込みだと毎フレーム微ブラーが乗る）。
            const XMMATRIX vp  = camVP;
            const XMMATRIX inv = XMMatrixInverse(nullptr, vp);
            XMFLOAT4X4 invT, prevT;
            XMStoreFloat4x4(&invT,  XMMatrixTranspose(inv));
            XMStoreFloat4x4(&prevT, XMMatrixTranspose(XMLoadFloat4x4(&m_prevViewProj)));
            // 速度バッファが今フレーム書かれていれば、それを使って「オブジェクト毎の」
            // モーションブラーにする（従来の深度再構成はカメラの動きしか拾えない）。
            // 使えない時は深度 SRV をダミーとして張る（params.w=0 なのでシェーダは読まない）。
            const u32 velIdx = velocityPrepass ? m_taaPass->GetVelocitySrvIndex()
                                               : DescriptorHeap::kInvalidIndex;
            const bool mbUseVelocity = (velIdx != DescriptorHeap::kInvalidIndex);
            const auto velSrvGpu = mbUseVelocity ? m_srvHeap->GetGpuHandle(velIdx) : depthSrvGpu;
            const u32 o = m_motionBlurPass->Apply(*m_commandList, m_srvHeap.get(),
                curSceneSrv, depthSrvGpu, velSrvGpu, mbUseVelocity, invT, prevT,
                rW, rH, ppApplied);
            if (o != DescriptorHeap::kInvalidIndex)
                curSceneSrv = m_srvHeap->GetGpuHandle(o);
        }

        // ---- 自動露出（compute。ビューポート矩形のヒストグラム→露出値を GPU 内バッファへ）----
        if (m_autoExposure && ppApplied.enabled && ppApplied.autoExposureOn)
            m_autoExposure->Generate(nativeCmdList, curSceneSrv, 0u, 0u, rW, rH,
                                     m_gameClock.GetDeltaTime(), ppApplied);
        D3D12_GPU_VIRTUAL_ADDRESS exposureVA = 0;
        if (m_autoExposure)
        {
            m_autoExposure->EnsureReadable(nativeCmdList);
            exposureVA = m_autoExposure->GetExposureBufferVA();
        }

        // ---- ブルーム（レンズフレアの入力も兼ねる。内部で RT/ビューポート切替）----
        u32 bloomSrv = DescriptorHeap::kInvalidIndex;
        if (m_bloomPass && ppApplied.enabled && (ppApplied.bloomOn || ppApplied.lensflareOn))
            bloomSrv = m_bloomPass->Generate(*m_commandList, m_srvHeap.get(), curSceneSrv,
                                             1.0f / fullW, 1.0f / fullH, ppApplied);
        const bool bloomReady = (bloomSrv != DescriptorHeap::kInvalidIndex);
        const auto bloomSrvGpu = m_srvHeap->GetGpuHandle(bloomReady ? bloomSrv : m_ssaoWhiteSrvIndex);

        // ---- ゴッドレイ（太陽=最初の平行光源をスクリーンへ投影）----
        u32 godraysSrv = DescriptorHeap::kInvalidIndex;
        if (wantDepthPost && ppApplied.godraysOn && m_godRaysPass)
        {
            XMFLOAT3 sunDir{}; XMFLOAT3 sunColI{}; bool hasSun = false;
            auto& greg = m_scene->GetRegistry();
            auto dlView = greg.view<DirectionalLight>();
            if (dlView.begin() != dlView.end())
            {
                const auto& dl = dlView.get<DirectionalLight>(*dlView.begin());
                sunDir  = dl.direction;
                sunColI = XMFLOAT3(dl.color.x * dl.intensity,
                                   dl.color.y * dl.intensity,
                                   dl.color.z * dl.intensity);
                hasSun = true;
            }
            if (hasSun)
            {
                // 太陽ワールド位置 ≒ カメラ位置 - 光方向×遠距離 → スクリーン投影
                const XMFLOAT3 camPos = m_camera->GetPosition();
                XMVECTOR d  = XMVector3Normalize(XMLoadFloat3(&sunDir));
                XMVECTOR wp = XMVectorSubtract(XMLoadFloat3(&camPos), XMVectorScale(d, 5000.0f));
                XMVECTOR clip = XMVector4Transform(XMVectorSetW(wp, 1.0f), camVP);  // 太陽投影はジッタなし
                const f32 cw = XMVectorGetW(clip);
                if (cw > 0.01f)
                {
                    const f32 lu = XMVectorGetX(clip) / cw * 0.5f + 0.5f;   // ローカルUV
                    const f32 lv = 0.5f - XMVectorGetY(clip) / cw * 0.5f;
                    // 画面中心からの距離でフェード（画面外に離れると消える）
                    const f32 dc = std::sqrt((lu - 0.5f) * (lu - 0.5f) + (lv - 0.5f) * (lv - 0.5f));
                    const f32 fade = (std::min)((std::max)((1.1f - dc) / 0.4f, 0.0f), 1.0f);
                    if (fade > 0.001f)
                    {
                        // ★#16: シーンは RT 全面なので、ローカル UV がそのまま RT の UV。
                        //   （かつては lu * uvScl + uvOfs でサブ矩形へ写していた）
                        godraysSrv = m_godRaysPass->Generate(*m_commandList, m_srvHeap.get(),
                            depthSrvGpu, rW, rH, lu, lv, fade, sunColI, ppApplied);
                    }
                }
            }
        }
        const bool grReady = (godraysSrv != DescriptorHeap::kInvalidIndex);

        // ---- レンズフレア（ブルームチェーンの縮小ミップから生成）----
        u32 flareSrv = DescriptorHeap::kInvalidIndex;
        if (m_lensFlarePass && ppApplied.enabled && ppApplied.lensflareOn && bloomReady)
        {
            const u32 mip = m_bloomPass->GetMipSrvIndex(1);
            if (mip != DescriptorHeap::kInvalidIndex)
                flareSrv = m_lensFlarePass->Generate(*m_commandList, m_srvHeap.get(),
                    m_srvHeap->GetGpuHandle(mip), ppApplied);
        }
        const bool lfReady = (flareSrv != DescriptorHeap::kInvalidIndex);

        // 深度を DSV 用途（エディタアイコン等）へ戻す（遷移した時だけ。needDepthSrv と対で閉じる）
        if (needDepthSrv)
            m_commandList->TransitionResource(m_depthBuffer.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // ---- 3D LUT（assets 相対パス。sRGB 無効=バイト列そのままロード。ストリップ形式 N*N x N）----
        auto lutSrvGpu = m_srvHeap->GetGpuHandle(m_ssaoWhiteSrvIndex);
        f32  lutSize   = 0.0f;
        if (ppApplied.enabled && ppApplied.lutOn && !ppApplied.lutPath.empty() && m_resourceManager)
        {
            const std::string lutAbs = PathResolver::AssetsDir() + ppApplied.lutPath;
            if (Texture* lut = m_resourceManager->GetOrLoadTexture(
                    PathResolver::Utf8ToWide(lutAbs), nativeCmdList, /*srgb=*/false))
            {
                if (lut->GetHeight() >= 2 &&
                    lut->GetWidth() == lut->GetHeight() * lut->GetHeight())
                {
                    lutSrvGpu = m_srvHeap->GetGpuHandle(lut->GetSrvIndex());
                    lutSize   = static_cast<f32>(lut->GetHeight());
                }
            }
        }

        // ---- 最終(uber)パス: バックバッファへ ----
        constexpr float bbClear[4] = {0.05f, 0.05f, 0.06f, 1.0f};
        m_commandList->ClearRenderTarget(rtv, bbClear);
        nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);  // 深度なし
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

        const auto whiteDummy = m_srvHeap->GetGpuHandle(m_ssaoWhiteSrvIndex);
        PostProcess::Inputs pin{};
        pin.sceneSrv     = curSceneSrv;
        pin.bloomSrv     = bloomSrvGpu;
        pin.lutSrv       = lutSrvGpu;
        pin.godraysSrv   = grReady ? m_srvHeap->GetGpuHandle(godraysSrv) : whiteDummy;
        pin.flareSrv     = lfReady ? m_srvHeap->GetGpuHandle(flareSrv)   : whiteDummy;
        pin.distortSrv   = (particleDistortDrawn && m_distortRT)
                         ? m_srvHeap->GetGpuHandle(m_distortRT->GetSrvIndex()) : whiteDummy;
        pin.lutSize      = lutSize;
        pin.exposureVA   = exposureVA;
        pin.bloomReady   = bloomReady;
        pin.godraysReady = grReady;
        pin.flareReady   = lfReady;
        pin.distortReady = particleDistortDrawn;

        // ★ここが唯一の「レンダー解像度 → 表示解像度」の橋渡し。ビューポートが表示矩形で、
        //   シーン RT の全面をサンプルする＝renderScale < 1 なら自動的にバイリニア拡大になる。
        m_postProcess->Apply(nativeCmdList, pin, ppApplied,
            1.0f / fullW, 1.0f / fullH, totalTime, frameIndex);

        // ---- 速度バッファのデバッグ可視化（ポスト後のバックバッファを上書き）----
        // 静止時に全面が均一な (0.5,0.5,0.5) グレーになるのが「ジッタが正しく除去されている」証拠。
        if (taaActive && taaCfg.debugVelocity && m_taaPass)
        {
            const u32 velSrv = m_taaPass->GetVelocitySrvIndex();
            if (velSrv != DescriptorHeap::kInvalidIndex)
            {
                nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                m_taaPass->DrawVelocityDebug(*m_commandList, m_srvHeap->GetGpuHandle(velSrv),
                    vpLeft, vpTop, vpW, vpH, /*scale*/ 20.0f);
            }
        }

        // 次フレームのモーションブラー用に今フレームの viewProj を保存
        XMStoreFloat4x4(&m_prevViewProj, camVP);   // ジッタなし（従来より正確になる）
        m_prevViewProjValid = true;
        // 速度バッファ/TAA 用に「ジッタなし」viewProj と frameIndex を保存する。
        // frameIndex は SkinningBuffer の前フレームスロットを指すため
        //（GetCurrentBackBufferIndex() の巡回順は DXGI 仕様上保証されないので明示記録）。
        XMStoreFloat4x4(&m_prevViewProjNoJitter, camVP);
        m_prevViewProjNJValid = true;
        m_prevFrameIndex      = frameIndex;
        m_prevFrameIndexValid = true;
    }
    m_gpuTimer->End(nativeCmdList, GpuTimer::PostFX);
    m_gpuTimer->Begin(nativeCmdList, GpuTimer::UI);

    // ---- Editor Icon Draw（ポスト後のバックバッファへ, エディタモードのみ）----
    if (m_engineMode == EngineMode::Editor && !m_isGameMode)
    {
        // ★DSV は張らない。アイコンは DepthEnable=FALSE で深度テストをしないうえ、
        //   #16 でメイン深度はレンダー解像度に縮んだので、表示解像度のバックバッファへ
        //   束ねると RTV より小さい DSV になる（PSO 側も DSVFormat=UNKNOWN にしてある）。
        m_commandList->SetRenderTarget(rtv);
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

        m_editorIconRenderer->BeginFrame();
        m_editorIconRenderer->CollectFromRegistry(m_scene->GetRegistry(), *m_editorCtx);

        XMFLOAT4X4 vpIcon;
        // エディタアイコンはポスト後のバックバッファ＝TAA の対象外なのでジッタなし
        // （ジッタさせると選択アイコンだけが 1px 揺れる）。
        XMStoreFloat4x4(&vpIcon, XMMatrixTranspose(camVP));
        m_editorIconRenderer->Render(nativeCmdList, vpIcon, vpW, vpH);
    }

    // ---- 2D スプライト / ゲーム内 UI 画像（バックバッファ全面へ）----
    if (m_spriteRenderer && (m_isGameMode || m_engineMode == EngineMode::Playing))
    {
        m_spriteRenderer->BeginFrame();
        // Lua の ui:image() コマンドをテクスチャ読み込み＋サブミット
        for (const auto& c : m_uiCommands)
        {
            if (c.type != UICommand::Type::Image || c.text.empty()) continue;
            std::wstring wpath = PathResolver::Utf8ToWide(c.text);
            Texture* tex = m_resourceManager->GetOrLoadTexture(wpath, nativeCmdList);
            if (!tex) continue;
            SpriteDesc s;
            s.pos      = {c.x, c.y};
            s.size     = {c.w, c.h};
            s.color    = {c.r, c.g, c.b, c.a};
            s.srvIndex = tex->GetSrvIndex();
            m_spriteRenderer->Submit(s);
        }

        // Sprite2D(worldSpace=false): HUD として画面ピクセル座標で描く。
        // 位置は Transform の並進 x/y を「画面ピクセル中心」とみなす（ワールド側と同じく中心基準で
        // size を展開）。Transform が無ければ画面左上(0,0)中心。layer 昇順は Render 内でソート。
        if (m_scene)
        {
            auto& sreg = m_scene->GetRegistry();
            for (auto [e, sp] : sreg.view<const Sprite2D>().each())
            {
                if (sp.worldSpace || sp.texturePath.empty()) continue;
                const std::string absPath = PathResolver::AssetsDir() + sp.texturePath;
                std::wstring wpath = PathResolver::Utf8ToWide(absPath);
                Texture* tex = m_resourceManager->GetOrLoadTexture(wpath, nativeCmdList);
                if (!tex) continue;
                float cx = 0.0f, cy = 0.0f;
                if (sreg.all_of<Transform>(e))
                {
                    XMFLOAT3 wp; XMStoreFloat3(&wp, ComputeWorldMatrix(sreg, e).r[3]);
                    cx = wp.x; cy = wp.y;
                }
                SpriteDesc s;
                s.pos      = {cx - sp.size.x * 0.5f, cy - sp.size.y * 0.5f};
                s.size     = sp.size;
                s.uvMin    = sp.uvMin;
                s.uvMax    = sp.uvMax;
                // フリップブック/UVスクロール（ワールド側 DrawWorldSprites と同じ純関数を適用）
                if (sp.animFrames > 0)
                {
                    const SpriteUvRect r = ComputeFlipbookUvEx(sp.animFrames, sp.animFps, sp.animCols,
                                                               sp.animRow, sp.animRows, sp.animMode,
                                                               sp._animT);
                    s.uvMin = {r.u0, r.v0}; s.uvMax = {r.u1, r.v1};
                }
                else if (sp.scrollU != 0.0f || sp.scrollV != 0.0f)
                {
                    const SpriteUvRect r = ComputeScrollUv(sp.uvMin.x, sp.uvMin.y, sp.uvMax.x, sp.uvMax.y,
                                                           sp.scrollU, sp.scrollV, totalTime);
                    s.uvMin = {r.u0, r.v0}; s.uvMax = {r.u1, r.v1};
                }
                s.color    = sp.color;
                s.srvIndex = tex->GetSrvIndex();
                s.layer    = static_cast<float>(sp.layer);
                m_spriteRenderer->Submit(s);
            }
        }

        if (m_spriteRenderer->HasAny())
        {
            nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            // ゲームビューポート矩形に合わせる（エディタ Play の 16:9 中央矩形でも UI 画像が
            // テキスト/矩形と同じ座標系になり、ずれない）。
            m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
            m_spriteRenderer->Render(nativeCmdList, vpW, vpH);
        }
    }

    // ===== カメラプレビュー（選択カメラ視点を小窓へ）=====
    // 選択中エンティティにカメラがあれば、その視点でシーンを専用 RT に再描画。
    // EditorLayer がシーンビュー隅に小窓表示する（Play を押さずに見える）。
    m_editorCtx->cameraPreviewTexHandle = 0;
    if (m_engineMode == EngineMode::Editor && !m_isGameMode && m_cameraPreviewRT)
    {
        auto& reg = m_scene->GetRegistry();
        entt::entity camEnt = entt::null;
        for (auto e : m_editorCtx->selectedEntities)
            if (reg.valid(e) && reg.all_of<CameraComponent, Transform>(e)) { camEnt = e; break; }

        if (camEnt != entt::null)
        {
            const auto& tf  = reg.get<Transform>(camEnt);
            const auto& cam = reg.get<CameraComponent>(camEnt);

            // 回転（quat）からビュー行列を構築（カメラアイコン/フラスタムと同じ向き）
            XMFLOAT4 q;
            if (tf.useQuaternion)
                q = tf.quaternion;
            else
                XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(
                    XMConvertToRadians(tf.rotation.x),
                    XMConvertToRadians(tf.rotation.y),
                    XMConvertToRadians(tf.rotation.z)));
            XMMATRIX rot  = XMMatrixRotationQuaternion(XMLoadFloat4(&q));
            XMVECTOR eye  = XMLoadFloat3(&tf.position);
            XMMATRIX view = XMMatrixLookToLH(eye, rot.r[2], rot.r[1]);

            const u32 pw = m_cameraPreviewRT->GetWidth();
            const u32 ph = m_cameraPreviewRT->GetHeight();
            const f32 paspect = static_cast<f32>(pw) / static_cast<f32>(ph);
            // 正射カメラはプレビューも正射投影に（ゲームの SetOrthographic と同じ: 縦=2*orthoSize）。
            XMMATRIX proj = (cam.projection == CameraProjection::Orthographic)
                ? XMMatrixOrthographicLH(2.0f * cam.orthoSize * paspect, 2.0f * cam.orthoSize,
                                         cam.nearClip, cam.farClip)
                : XMMatrixPerspectiveFovLH(XMConvertToRadians(cam.fovDegrees), paspect,
                                           cam.nearClip, cam.farClip);
            XMMATRIX camViewProj = view * proj;

            // メインパスの fc（ライト等）を流用し、視点だけ差し替えて専用 CB へ
            FrameConstants fcp = fc;
            XMStoreFloat4x4(&fcp.view, XMMatrixTranspose(view));
            XMStoreFloat4x4(&fcp.proj, XMMatrixTranspose(proj));
            fcp.cameraPos = tf.position;
            fcp.aoEnabled = 0.0f;   // プレビューは白ダミー AO（SSAO 非対応）なので AO を読まない
            fcp.contactShadowEnabled = 0.0f;   // 同上（コンタクトシャドウもプレビューでは作らない）
            // クラスタは「メインカメラ視点」で作ってあるので、別視点のプレビューで引くと
            // 完全に間違ったライトリストになる。総当たりフォールバックへ倒す。
            // ★これでデカール（計画06）もプレビューでは無効になる（ApplyDecals が
            //   clusterGrid.w <= 0.5 で丸ごと return する）。ビニングが別視点なので正しい挙動。
            fcp.clusterGrid.w  = 0.0f;
            fcp.clusterExtra.z = 0.0f;   // デバッグ可視化もプレビューでは出さない
            m_previewFrameCB->Update(&fcp, sizeof(fcp), frameIndex);

            m_cameraPreviewRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float pvClear[4] = {0.127f, 0.306f, 0.850f, 1.0f};  // リニア空間のコーンフラワーブルー
            m_commandList->ClearRenderTarget(m_cameraPreviewRT->GetRtv(), pvClear);
            m_commandList->ClearDepthStencil(m_previewDsvHandle);
            m_commandList->SetRenderTarget(m_cameraPreviewRT->GetRtv(), m_previewDsvHandle);
            m_commandList->SetViewportAndScissor(pw, ph);

            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
            m_commandList->SetRootSignature(*m_rootSignature);
            m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame,
                m_previewFrameCB->GetGpuAddress(frameIndex));
            m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
                m_srvHeap->GetGpuHandle(m_shadowSrvIndex));
            if (m_iblReady && m_iblBaker)
                m_commandList->SetSRVTable(RootSignature::kSlotIBLTable,
                    m_srvHeap->GetGpuHandle(m_iblBaker->GetIrradianceSrv()));
            // クラスタテーブルはフォールバック時も必ずバインドする（PS が t13..t15 を参照するため）
            if (m_clusteredLighting && m_clusteredLighting->IsReady())
                m_commandList->SetSRVTable(RootSignature::kSlotClusterSRV,
                    m_clusteredLighting->GetSrvTable(frameIndex));

            // グリッドは出さない＝isGameView=true。プレビューは SSAO 非対応＝白ダミー。
            RenderSceneMeshes(nativeCmdList, frameIndex, camViewProj, true, m_ssaoWhiteSrvIndex,
                              /*depthPrepassActive=*/false, m_ssaoWhiteSrvIndex);

            // ワールド空間スプライトもプレビューへ（このカメラ視点で。ビルボードは行列の右/上ベクトル）。
            XMFLOAT3 pvRight, pvUp;
            XMStoreFloat3(&pvRight, rot.r[0]);
            XMStoreFloat3(&pvUp,    rot.r[1]);
            DrawWorldSprites(nativeCmdList, camViewProj, pvRight, pvUp,
                             m_cameraPreviewRT->GetRtv(), m_previewDsvHandle, 0u, 0u, pw, ph, totalTime);

            m_cameraPreviewRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            // プレビューRT(リニアHDR)をトーンマップして LDR RT へ解決する。
            // ImGui へ FP16 の SRV を直接渡すとトーンマップ/ガンマ無しで暗く表示されるため。
            // enabled=false → mask=0 = PostProcess はトーンマップ+ガンマのみ適用。
            if (m_cameraPreviewLdrRT && m_postProcess && m_postProcess->IsReady())
            {
                m_cameraPreviewLdrRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
                D3D12_CPU_DESCRIPTOR_HANDLE ldrRtv = m_cameraPreviewLdrRT->GetRtv();
                nativeCmdList->OMSetRenderTargets(1, &ldrRtv, FALSE, nullptr);  // 深度なし
                m_commandList->SetViewportAndScissor(pw, ph);

                PostProcessSettings pvPost{};
                pvPost.enabled = false;
                // トーンマッパはシーン設定と揃える（プレビューと本画面の見た目一致）
                pvPost.tonemapper = m_scene->GetPostSettings().tonemapper;
                const auto pvDummy = m_srvHeap->GetGpuHandle(m_ssaoWhiteSrvIndex);
                PostProcess::Inputs pvIn{};
                pvIn.sceneSrv   = m_srvHeap->GetGpuHandle(m_cameraPreviewRT->GetSrvIndex());
                pvIn.bloomSrv   = pvDummy;
                pvIn.lutSrv     = pvDummy;
                pvIn.godraysSrv = pvDummy;
                pvIn.flareSrv   = pvDummy;
                pvIn.distortSrv = pvDummy;
                pvIn.exposureVA = m_autoExposure ? m_autoExposure->GetExposureBufferVA() : 0;
                m_postProcess->Apply(nativeCmdList, pvIn, pvPost,
                    1.0f / static_cast<f32>(pw), 1.0f / static_cast<f32>(ph), totalTime, frameIndex);

                m_cameraPreviewLdrRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_editorCtx->cameraPreviewTexHandle =
                    m_srvHeap->GetGpuHandle(m_cameraPreviewLdrRT->GetSrvIndex()).ptr;
            }
            else
            {
                m_editorCtx->cameraPreviewTexHandle =
                    m_srvHeap->GetGpuHandle(m_cameraPreviewRT->GetSrvIndex()).ptr;
            }

            // プレビュー描画でRT/ビューポートを切り替えたので、バックバッファへ戻す。
            // これをしないと直後の ImGui がプレビューRTへ描かれ、画面に出なくなる。
            m_commandList->SetRenderTarget(rtv);   // 深度は張らない（#16: メイン深度はレンダー解像度）
            m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
        }
    }

    // ===== パーティクルエディタのプレビュー（専用オフスクリーンRT。UI本体は後段のImGuiパスで描く）=====
    if (m_vfxEditorPanel && m_editorCtx->showVfxEditor)
    {
        m_vfxEditorPanel->RenderPreview3D(*m_editorCtx, *m_commandList, m_gameClock.GetDeltaTime());
        // プレビュー描画でRT/ビューポートを切り替えたので、バックバッファへ戻す。
        m_commandList->SetRenderTarget(rtv);   // 深度は張らない（#16: メイン深度はレンダー解像度）
        m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
    }

    // ===== マテリアルエディタの3Dプレビュー（専用オフスクリーンRT。UI本体は後段のImGuiパスで描く）=====
    if (m_materialEditorPanel && m_editorCtx->showMaterialEditor)
    {
        m_materialEditorPanel->RenderPreview3D(*m_editorCtx, *m_commandList);
        // プレビュー描画でRT/ビューポートを切り替えたので、バックバッファへ戻す。
        m_commandList->SetRenderTarget(rtv);   // 深度は張らない（#16: メイン深度はレンダー解像度）
        m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
    }

    // ---- MCP screenshot_final: ImGui を描く前のバックバッファ（＝ポスト適用後の絵だけ）を撮る ----
    //   ここより後は ImGui のパネル / ギズモ / オーバーレイが乗るので、必ずこの位置で撮ること（§6 B5）。
    CaptureFinalBackBufferRegion(nativeCmdList, backBuffer, vpLeft, vpTop, vpW, vpH);

    // ---- ImGui フレーム ----
    m_imguiManager->BeginFrame();
    ImGuizmo::BeginFrame();

    // フローティングツール窓のホバー判定ラッチ。各窓は ImGui パス後半(EditorLayer::Render より後)で
    // ThisFrame を立てるため、読む側(HandleCameraNavigation 等)は前フレームの確定値を参照する。
    m_editorCtx->floatingToolWindowHovered = m_editorCtx->floatingToolWindowHoveredThisFrame;
    m_editorCtx->floatingToolWindowHoveredThisFrame = false;

    // 版が変わった初回起動だけ「更新内容」モーダルを最前面に出す（ランチャー/エディタの上）。
    RenderWhatsNewPopup();

    if (!m_isGameMode && m_loading)
    {
        // ---- ローディングオーバーレイ（プロジェクト作成/読込中）----
        RenderLoadingOverlay();
    }
    else if (!m_isGameMode && m_showLauncher)
    {
        // ---- プロジェクトランチャー（起動直後 / 「ランチャーに戻る」選択時）----
        // カスタムタイトルバー有効時、ツールバー(タイトルバー役)が無いこの画面でも
        // ウィンドウの移動/最小化/閉じるができるよう、最小限の操作を上端に重ねる。
        if (m_window->IsCustomTitleBar())
        {
            const ImGuiIO& lio = ImGui::GetIO();
            const ImGuiViewport* mainVp = ImGui::GetMainViewport();
            const float kBarH = 36.0f, kBtnW = 44.0f;
            ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x + mainVp->Size.x - kBtnW * 2.0f, mainVp->Pos.y),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(kBtnW * 2.0f, kBarH), ImGuiCond_Always);
            ImGui::SetNextWindowViewport(mainVp->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##LauncherCaption", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto capBtn = [&](const char* id, bool isClose) -> bool
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton(id, ImVec2(kBtnW, kBarH));
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 p1(p0.x + kBtnW, p0.y + kBarH);
                if (hovered)
                    dl->AddRectFilled(p0, p1, isClose ? IM_COL32(232, 17, 35, 255)
                                                      : IM_COL32(255, 255, 255, 16));
                const ImU32 fg = (isClose && hovered) ? IM_COL32(255, 255, 255, 255)
                                                      : IM_COL32(198, 200, 207, 255);
                const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                const float r = 5.0f;
                if (isClose)
                {
                    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 1.0f);
                    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), fg, 1.0f);
                }
                else
                    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, 1.0f);
                ImGui::SameLine(0.0f, 0.0f);
                return clicked;
            };
            if (capBtn("##ln_min", false)) m_window->Minimize();
            if (capBtn("##ln_close", true)) m_window->RequestClose();
            ImGui::End();
            ImGui::PopStyleVar();

            // 上端帯(ボタン部を除く)をドラッグ可能として報告(マウスはビューポート相対へ変換)
            const ImVec2 mp(lio.MousePos.x - mainVp->Pos.x, lio.MousePos.y - mainVp->Pos.y);
            const bool inBand = mp.y >= 0.0f && mp.y < kBarH
                             && mp.x >= 0.0f && mp.x < mainVp->Size.x - kBtnW * 2.0f;
            m_window->SetCaptionInfo(static_cast<u32>(kBarH), inBand);
        }

        LauncherIcons li;
        li.logo        = m_icons.logo;
        li.newProject  = m_icons.newProject;
        li.openProject = m_icons.openProject;
        li.recent      = m_icons.recent;
        li.tmplFps     = m_icons.tmplFps;
        li.tmplTps     = m_icons.tmplTps;
        li.tmpl2d      = m_icons.tmpl2d;
        li.tmplEmpty   = m_icons.tmplEmpty;

        ProjectInfo selected;
        LauncherAction action = ProjectManager::RenderLauncher(selected, m_window->GetHwnd(), li);
        if (action == LauncherAction::CreateNew)
            BeginProjectLoad(selected, /*isNew=*/true);
        else if (action == LauncherAction::OpenExisting)
            BeginProjectLoad(selected, /*isNew=*/false);
        else if (action == LauncherAction::Skip)
        {
            LoadProject(selected);
            m_showLauncher = false;
        }
    }
    else if (!m_isGameMode)
    {
        bool pendingPlayMode = false;
        CpuScopeTimer _tUi(&m_cpuMs[CpuEditorUi]); DX12_PROFILE_ZONE_N("EditorUI");
        m_editorLayer->Render(
            m_engineMode == EngineMode::Playing,
            m_scene.get(), m_camera.get(), m_window.get(),
            m_scriptEngine.get(), m_audioSystem.get(),
            m_physicsDebugRenderer.get(), m_physicsDebugDraw,
            m_useVsync, m_shadowQualityIndex, m_shadowMapSize,
            m_shadowMapDirty, m_cascadeSplitLambda, m_cascadeBlendBand,
            m_showCascadeDebug, &m_gameClock,
            m_modeChangeRequested, pendingPlayMode,
            PathResolver::AssetsDir(), kLeftPanelWidth, kToolbarHeight,
            m_resourceManager.get(), m_srvHeap.get(), nativeCmdList);

        // ★エンジン設定の VSync チェックボックスは m_useVsync を参照で直接書くだけで、
        //   settings.json へは保存されていなかった（PersistSet("video_vsync") を呼ぶのは
        //   Lua の display:setVSync だけ。ApplicationScene.cpp:397）。
        //   結果「エディタで VSync を切っても次の起動で戻る」。Inspector 側に
        //   コールバックを引き回すより、変わった時にここで 1 回書く方が短い。
        //   ベンチ中(m_benchRestore)は VSync を一時的に外しているので書かない。
        //   書くと計測の途中でユーザーの設定が一瞬 OFF で保存され、そこで落ちると戻らない。
        if (m_useVsync != m_persistedVsync && !m_benchRestore)
        {
            m_persistedVsync = m_useVsync;
            PersistSet("video_vsync", m_useVsync ? 1.0 : 0.0);
        }

        if (m_modeChangeRequested)
            m_pendingMode = pendingPlayMode ? EngineMode::Playing : EngineMode::Editor;

        // ---- ポストプロセス: ON/OFF ウィンドウ と パラメータ ウィンドウ ----
        {
            auto& pp  = m_scene->GetPostSettings();
            auto& taa = m_scene->GetTaaSettings();   // TAA は PostProcessSettings とは別（下の注記参照）

            // 全エフェクトのメタ情報（トグルとパラメータ描画を一元定義）
            struct PostFx {
                const char* cat;                 // カテゴリ見出し
                const char* label;               // 表示名
                const char* help;                // 説明（null可）
                bool*       on;                  // 有効フラグ
                std::function<void()> params;    // パラメータ描画
            };
            const std::vector<PostFx> fx = {
                {"カラー", "露出 Exposure", "明るさを乗算で調整", &pp.exposureOn,
                    [&]{ ImGui::SliderFloat("値##exposure", &pp.exposure, 0.1f, 4.0f, "%.2f"); }},
                {"カラー", "自動露出 Auto Exposure", "平均輝度に合わせて露出を自動追従（目の順応）", &pp.autoExposureOn,
                    [&]{ ImGui::SliderFloat("適応速度##aespd", &pp.aeSpeed, 0.1f, 10.0f, "%.1f");
                         ImGui::SliderFloat("EV補正##aeev", &pp.aeEvComp, -4.0f, 4.0f, "%.1f"); }},
                {"カラー", "コントラスト Contrast", nullptr, &pp.contrastOn,
                    [&]{ ImGui::SliderFloat("値##contrast", &pp.contrast, 0.0f, 2.0f, "%.2f"); }},
                {"カラー", "明るさ Brightness", "加算で明暗を調整", &pp.brightnessOn,
                    [&]{ ImGui::SliderFloat("値##brightness", &pp.brightness, -0.5f, 0.5f, "%.2f"); }},
                {"カラー", "彩度 Saturation", nullptr, &pp.saturationOn,
                    [&]{ ImGui::SliderFloat("値##saturation", &pp.saturation, 0.0f, 2.0f, "%.2f"); }},
                {"カラー", "色温度 Warmth", "+で暖色、-で寒色", &pp.warmthOn,
                    [&]{ ImGui::SliderFloat("値##warmth", &pp.warmth, -1.0f, 1.0f, "%.2f"); }},
                {"カラー", "色相回転 Hue", "色相を回す（度）", &pp.hueOn,
                    [&]{ ImGui::SliderFloat("角度##hue", &pp.hueShift, 0.0f, 360.0f, "%.0f°"); }},
                {"カラー", "色味 Tint", "RGB を乗算", &pp.tintOn,
                    [&]{ ImGui::ColorEdit3("色##tint", &pp.tint.x); }},

                {"ブルーム/ビネット", "ブルーム Bloom", "明部が咲く（物理ベース・ダウンサンプルチェーン）", &pp.bloomOn,
                    [&]{ ImGui::SliderFloat("強度##bloom", &pp.bloom, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderFloat("しきい値##bloomth", &pp.bloomThreshold, 0.0f, 4.0f, "%.2f");
                         ImGui::SliderFloat("ニー(肩)##bloomknee", &pp.bloomKnee, 0.0f, 1.0f, "%.2f");
                         ImGui::SliderFloat("広がり##bloomrad", &pp.bloomRadius, 0.05f, 0.95f, "%.2f"); }},
                {"ブルーム/ビネット", "ビネット Vignette", "周辺減光", &pp.vignetteOn,
                    [&]{ ImGui::SliderFloat("強度##vig", &pp.vignette, 0.0f, 1.0f, "%.2f"); }},

                {"ライト/カメラ", "ゴッドレイ God Rays", "太陽(平行光源)からの光条。太陽が画面内/近くにある時に見える(透視カメラのみ)", &pp.godraysOn,
                    [&]{ ImGui::SliderFloat("強度##gri", &pp.grIntensity, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderFloat("長さ##grd", &pp.grDensity, 0.1f, 1.0f, "%.2f");
                         ImGui::SliderFloat("減衰##grdc", &pp.grDecay, 0.8f, 0.999f, "%.3f"); }},
                {"ライト/カメラ", "レンズフレア Lens Flare", "ゴースト+ハロー。強い光源があると出る(ブルームと入力共有)", &pp.lensflareOn,
                    [&]{ ImGui::SliderFloat("強度##lfi", &pp.lfIntensity, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderInt("ゴースト数##lfg", &pp.lfGhosts, 1, 8);
                         ImGui::SliderFloat("間隔##lfd", &pp.lfDispersal, 0.05f, 1.0f, "%.2f");
                         ImGui::SliderFloat("ハロー##lfh", &pp.lfHalo, 0.0f, 1.0f, "%.2f");
                         ImGui::SliderFloat("色収差##lfc", &pp.lfChroma, 0.0f, 0.05f, "%.3f"); }},
                {"ライト/カメラ", "被写界深度 DoF", "フォーカス距離の前後がボケる(透視カメラのみ)", &pp.dofOn,
                    [&]{ ImGui::SliderFloat("フォーカス距離##doff", &pp.dofFocusDist, 0.1f, 100.0f, "%.1f");
                         ImGui::SliderFloat("シャープ範囲##dofr", &pp.dofFocusRange, 0.1f, 50.0f, "%.1f");
                         ImGui::SliderFloat("最大ボケpx##dofb", &pp.dofBlurSize, 1.0f, 32.0f, "%.0f"); }},
                {"ライト/カメラ", "モーションブラー Motion Blur", "カメラの動きで残像(深度再構成方式・透視カメラのみ)", &pp.motionBlurOn,
                    [&]{ ImGui::SliderFloat("強度##mbs", &pp.mbStrength, 0.0f, 2.0f, "%.2f");
                         ImGui::SliderInt("サンプル数##mbn", &pp.mbSamples, 4, 16); }},

                {"スタイライズ", "色収差 Chromatic", "画面端でRGBがズレる", &pp.chromaticOn,
                    [&]{ ImGui::SliderFloat("強度##chroma", &pp.chromatic, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "ピクセル化 Pixelize", "ブロック状にモザイク", &pp.pixelizeOn,
                    [&]{ ImGui::SliderFloat("ブロックpx##pix", &pp.pixelSize, 1.0f, 64.0f, "%.0f"); }},
                {"スタイライズ", "ポスタライズ Posterize", "色数を段階化", &pp.posterizeOn,
                    [&]{ ImGui::SliderInt("階調##post", &pp.posterize, 2, 16); }},
                {"スタイライズ", "ディザ Dither", "順序ディザで階調化", &pp.ditherOn,
                    [&]{ ImGui::SliderInt("階調##dither", &pp.ditherLevels, 2, 8); }},
                {"スタイライズ", "CRT走査線 Scanline", "走査線＋画面湾曲", &pp.scanlineOn,
                    [&]{ ImGui::SliderFloat("強度##scan", &pp.scanline, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "シャープ Sharpen", "輪郭を強調", &pp.sharpenOn,
                    [&]{ ImGui::SliderFloat("強度##sharp", &pp.sharpen, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "フィルムグレイン Grain", "ザラつきノイズ", &pp.grainOn,
                    [&]{ ImGui::SliderFloat("強度##grain", &pp.grain, 0.0f, 1.0f, "%.2f"); }},

                {"カラー操作", "色反転 Invert", nullptr, &pp.invertOn,
                    [&]{ ImGui::SliderFloat("強度##inv", &pp.invert, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "セピア Sepia", nullptr, &pp.sepiaOn,
                    [&]{ ImGui::SliderFloat("強度##sepia", &pp.sepia, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "グレースケール Grayscale", nullptr, &pp.grayscaleOn,
                    [&]{ ImGui::SliderFloat("強度##gray", &pp.grayscale, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "LUT グレーディング", "ストリップ画像(N*N x N, 例:1024x32)で色変換。Photoshop等で作った LUT を適用", &pp.lutOn,
                    [&]{ static char lutBuf[260] = "";
                         ImGui::InputTextWithHint("##lutpath", "assets からの相対パス (例: luts/warm.png)", lutBuf, sizeof(lutBuf));
                         if (ImGui::IsItemDeactivatedAfterEdit()) pp.lutPath = lutBuf;
                         if (!ImGui::IsItemActive() && pp.lutPath != lutBuf)
                         {
                             size_t n = pp.lutPath.size();
                             if (n >= sizeof(lutBuf)) n = sizeof(lutBuf) - 1;
                             std::memcpy(lutBuf, pp.lutPath.c_str(), n);
                             lutBuf[n] = '\0';
                         }
                         ImGui::SliderFloat("適用量##lutamt", &pp.lutAmount, 0.0f, 1.0f, "%.2f"); }},

                {"歪み", "レンズ歪み Lens", "バレル/魚眼", &pp.lensOn,
                    [&]{ ImGui::SliderFloat("強度##lens", &pp.lens, -1.0f, 1.0f, "%.2f"); }},
                {"歪み", "波ゆらぎ Wave", "水中/陽炎のゆれ", &pp.waveOn,
                    [&]{ ImGui::SliderFloat("振幅##wamp", &pp.waveAmp, 0.0f, 0.05f, "%.3f");
                         ImGui::SliderFloat("周波数##wfreq", &pp.waveFreq, 1.0f, 40.0f, "%.1f");
                         ImGui::SliderFloat("速度##wspd", &pp.waveSpeed, 0.0f, 8.0f, "%.1f"); }},
                {"歪み", "放射ブラー Radial", "中心へズームブラー", &pp.radialOn,
                    [&]{ ImGui::SliderFloat("強度##rad", &pp.radial, 0.0f, 1.0f, "%.2f"); }},
                {"歪み", "グリッチ Glitch", "デジタル乱れ", &pp.glitchOn,
                    [&]{ ImGui::SliderFloat("強度##glitch", &pp.glitch, 0.0f, 1.0f, "%.2f"); }},

                {"輪郭", "輪郭線 Outline", "Sobelエッジ検出", &pp.outlineOn,
                    [&]{ ImGui::SliderFloat("強度##outl", &pp.outline, 0.0f, 4.0f, "%.2f");
                         ImGui::ColorEdit3("線の色##outlc", &pp.outlineColor.x); }},

                {"アンチエイリアス", "FXAA", "簡易アンチエイリアス（TAA が有効なら無視されます）", &pp.fxaaOn, {}},
                // TAA は PostProcessSettings ではなく TaaSettings（シーン単位の独立設定）に住む。
                // uber パスの「マスク付きエフェクト」ではなく、チェーンの構造そのものを変える
                // （深度+速度プリパスの強制・投影行列のジッタ・FXAA 排他）ため。
                {"アンチエイリアス", "TAA (テンポラル)",
                 "速度バッファ + 前フレームの履歴でサブピクセル AA。動くものもぼけません。"
                 "有効にすると FXAA は自動で無視されます（透視ビューのみ。2D 正射では無効）",
                 &taa.enabled,
                    [&]{ int sc = (taa.sampleCount <= 4) ? 0 : (taa.sampleCount >= 16 ? 2 : 1);
                         if (ImGui::Combo("ジッタ数##taasc", &sc, "4 (シャープ)\0" "8 (標準)\0" "16 (滑らか)\0"))
                             taa.sampleCount = (sc == 0) ? 4 : (sc == 2 ? 16 : 8);
                         ImGui::SliderFloat("ジッタ量##taajs", &taa.jitterScale, 0.0f, 1.0f, "%.2f");
                         if (ImGui::IsItemHovered()) ImGui::SetTooltip("1.0 = ±0.5px。ブラーが強すぎるなら下げる");
                         ImGui::SliderFloat("履歴 最小##taafbmin", &taa.feedbackMin, 0.5f, 0.98f, "%.3f");
                         if (ImGui::IsItemHovered()) ImGui::SetTooltip("現フレームと食い違うピクセルで使う履歴の比率");
                         ImGui::SliderFloat("履歴 最大##taafbmax", &taa.feedbackMax, 0.5f, 0.995f, "%.3f");
                         if (ImGui::IsItemHovered()) ImGui::SetTooltip("安定しているピクセルで使う履歴の比率。高いほど滑らかだがゴーストしやすい");
                         ImGui::SliderFloat("クリップ幅##taavg", &taa.varianceGamma, 0.25f, 3.0f, "%.2f");
                         if (ImGui::IsItemHovered()) ImGui::SetTooltip("近傍色の許容幅 (μ±γσ)。下げるとゴーストが減りチラつきが増える");
                         ImGui::Checkbox("速度バッファを可視化##taadbg", &taa.debugVelocity);
                         if (ImGui::IsItemHovered()) ImGui::SetTooltip("静止時に全面が均一なグレーになるのが正常"); }},

                {"仕上げ", "デバンディング Deband", "TPDFディザで空/ビネットの縞(バンディング)を除去", &pp.debandOn, {}},
            };

            // 有効中エフェクト数（両窓で使うので、窓の表示有無に関わらず先に数える）
            int enabledCount = 0;
            for (const auto& fEff : fx)
                if (*fEff.on) ++enabledCount;

            // ===== ウィンドウ1: ON/OFF チェックリスト（ツール窓・トグル表示）=====
            if (m_editorCtx->showPostProcess)
            {
            ImGui::Begin("Post Process");
            ImGui::Checkbox("有効（マスター）", &pp.enabled);
            // トーンマップ（表示変換）はマスターOFF でも常に適用されるのでディセーブル外
            ImGui::SetNextItemWidth(200.0f);
            ImGui::Combo("トーンマップ", &pp.tonemapper, "ACES\0AgX\0なし(ガンマのみ)\0");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted("ACES: コントラスト強めの定番\nAgX: 高輝度・高彩度光源(ネオン/発光体)の色割れがない\nなし: ガンマのみ(デバッグ/2D向け)");
                ImGui::EndTooltip();
            }
            ImGui::TextDisabled("SceneビューとGameビューへ同じ見た目を適用します / パラメータは「Post Process パラメータ」窓で");
            ImGui::Separator();

            ImGui::BeginDisabled(!pp.enabled);
            const char* curCat = nullptr;
            for (const auto& f : fx)
            {
                if (curCat == nullptr || std::strcmp(curCat, f.cat) != 0)
                {
                    curCat = f.cat;
                    ImGui::SeparatorText(curCat);
                }
                ImGui::Checkbox(f.label, f.on);
                if (f.help)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::BeginItemTooltip())
                    { ImGui::TextUnformatted(f.help); ImGui::EndTooltip(); }
                }
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::Button("すべてOFF"))
                for (const auto& f : fx) *f.on = false;
            ImGui::SameLine();
            if (ImGui::Button("初期値に戻す"))
                pp = PostProcessSettings{};
            ImGui::SameLine();
            ImGui::TextDisabled("有効中: %d", enabledCount);
            ImGui::End();
            } // if showPostProcess

            // ===== ウィンドウ2: 有効なエフェクトのパラメータ（ツール窓・トグル表示）=====
            if (m_editorCtx->showPostParams)
            {
            ImGui::Begin("Post Process パラメータ");
            if (!pp.enabled)
                ImGui::TextDisabled("マスターが OFF です（Post Process 窓で有効化）");
            else if (enabledCount == 0)
                ImGui::TextDisabled("エフェクトを有効にすると、ここに調整項目が出ます");
            else
            {
                ImGui::PushItemWidth(-120.0f);
                for (const auto& f : fx)
                {
                    if (!*f.on || !f.params) continue;
                    ImGui::SeparatorText(f.label);
                    ImGui::PushID(f.label);
                    f.params();
                    ImGui::PopID();
                }
                ImGui::PopItemWidth();
            }
            ImGui::End();
            } // if showPostParams
        }

        // ---- Skybox / IBL 設定ウィンドウ（シーン単位の環境マップ・トグル表示）----
        if (m_scene && m_editorCtx->showSkybox)
        {
            auto& sk = m_scene->GetSkyboxSettings();
            ImGui::Begin("Skybox / IBL");
            ImGui::TextWrapped("環境キューブ(.dds, TEXTURECUBE) から irradiance / prefiltered / BRDF LUT を生成し、"
                               "ambient を IBL 化する。空欄なら従来 ambient。");
            ImGui::Separator();

            // env map パス入力
            static char pathBuf[260];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", sk.envMapPath.c_str());
            if (ImGui::InputText("Env Map (.dds, assets相対)", pathBuf, sizeof(pathBuf)))
                sk.envMapPath = pathBuf;

            ImGui::SliderFloat("IBL Intensity", &sk.iblIntensity, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Skybox Intensity", &sk.skyboxIntensity, 0.0f, 3.0f, "%.2f");
            ImGui::Checkbox("Draw Skybox (背景を描く)", &sk.drawSkybox);

            // ランタイム値へ即時反映（強度/描画フラグは再ベイク不要）
            m_iblIntensity    = sk.iblIntensity;
            m_skyboxIntensity = sk.skyboxIntensity;
            m_drawSkybox      = sk.drawSkybox;

            ImGui::Separator();
            if (ImGui::Button("環境マップ適用 / 再ベイク"))
                m_skyboxDirty = true;   // 次フレーム冒頭で再ベイク（WaitIdle 込み）
            ImGui::SameLine();
            ImGui::TextDisabled(m_iblReady && m_iblBaker && m_iblBaker->HasEnvironment()
                                ? "IBL: 有効" : "IBL: フォールバック(ambient)");
            ImGui::End();
        }

        // ---- SSAO 設定ウィンドウ（シーン単位・グローバルレンダ設定・トグル表示）----
        if (m_scene && m_editorCtx->showSSAO)
        {
            auto& ss = m_scene->GetSSAOSettings();
            ImGui::Begin("SSAO");
            ImGui::TextWrapped("深度プリパス + 深度から法線再構築の半球カーネル AO。"
                               "ambient/IBL へ ao を乗算する。透視ビューのみ（2D 正射では無効）。");
            ImGui::Separator();
            ImGui::Checkbox("SSAO 有効", &ss.enabled);
            ImGui::BeginDisabled(!ss.enabled);
            ImGui::SliderFloat("半径 Radius",  &ss.radius,    0.05f, 2.0f, "%.2f");
            ImGui::SliderFloat("バイアス Bias", &ss.bias,     0.0f,  0.1f, "%.3f");
            ImGui::SliderFloat("強度 Intensity", &ss.intensity, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("べき Power",    &ss.power,     0.5f,  4.0f, "%.2f");
            {
                int s16 = (ss.sampleCount >= 16) ? 1 : 0;
                if (ImGui::Combo("サンプル数", &s16, "8\0" "16\0"))
                    ss.sampleCount = s16 ? 16 : 8;
            }
            ImGui::Checkbox("ブラー Blur", &ss.blur);
            ImGui::EndDisabled();
            ImGui::End();
        }

        // ---- SSR / SSGI 設定ウィンドウ（シーン単位・グローバルレンダ設定・トグル表示）----
        if (m_scene && m_editorCtx->showScreenSpaceGi)
        {
            auto& sr = m_scene->GetSsrSettings();
            auto& sg = m_scene->GetSsgiSettings();
            ImGui::Begin("SSR / SSGI");
            ImGui::TextWrapped("深度プリパスの G-Buffer（法線/ラフネス/メタリック）と"
                               "前フレームのシーンカラーをレイマーチする。透視ビューのみ。"
                               "どちらか有効にすると深度+速度プリパスが常時走る。"
                               "反射/間接光は 1 フレーム遅れる。");
            ImGui::Separator();

            ImGui::SeparatorText("SSR（スクリーン空間反射）");
            ImGui::Checkbox("SSR 有効", &sr.enabled);
            ImGui::BeginDisabled(!sr.enabled);
            ImGui::SliderFloat("強度##ssr",        &sr.intensity,       0.0f, 1.0f,   "%.2f");
            ImGui::SliderFloat("最大距離(m)",       &sr.maxDistance,     1.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("厚み(m)##ssr",      &sr.thickness,       0.05f, 2.0f,  "%.2f");
            ImGui::SliderInt  ("ステップ数",         &sr.maxSteps,        16, 128);
            ImGui::SliderFloat("歩幅(px)",          &sr.stride,          1.0f, 8.0f,   "%.1f");
            ImGui::SliderFloat("ラフネス上限",       &sr.roughnessCutoff, 0.05f, 1.0f,  "%.2f");
            ImGui::SliderFloat("画面端フェード",      &sr.edgeFade,        0.0f, 0.5f,   "%.2f");
            ImGui::SliderFloat("バイアス(m)##ssr",   &sr.bias,            0.0f, 0.5f,   "%.3f");
            ImGui::TextDisabled("ラフネス上限を超える面はレイを打たず IBL に任せる");
            ImGui::EndDisabled();

            ImGui::SeparatorText("SSGI（スクリーン空間GI）");
            ImGui::Checkbox("SSGI 有効", &sg.enabled);
            ImGui::BeginDisabled(!sg.enabled);
            ImGui::SliderFloat("強度##ssgi",       &sg.intensity,  0.0f, 2.0f,  "%.2f");
            ImGui::SliderFloat("到達距離(m)",       &sg.radius,     0.5f, 30.0f, "%.1f");
            ImGui::SliderFloat("厚み(m)##ssgi",     &sg.thickness,  0.05f, 2.0f, "%.2f");
            ImGui::SliderInt  ("レイ数/px",         &sg.rayCount,   1, 4);
            ImGui::SliderInt  ("ステップ数##ssgi",   &sg.stepCount,  4, 24);
            ImGui::SliderFloat("輝度クランプ",       &sg.clampValue, 0.1f, 20.0f, "%.2f");
            ImGui::SliderFloat("時間蓄積 Feedback", &sg.feedback,   0.0f, 0.98f, "%.2f");
            ImGui::Checkbox("画面外は IBL で埋める", &sg.iblFallback);
            ImGui::TextDisabled("IBL 埋めを切るとカメラを回すたびに明るさが変動する");
            ImGui::EndDisabled();
            ImGui::End();
        }

        // ---- ボリュメトリックフォグ設定ウィンドウ（シーン単位・グローバルレンダ設定・トグル表示）----
        if (m_scene && m_editorCtx->showVolumetricFog)
        {
            auto& f = m_scene->GetVolumetricFogSettings();
            ImGui::Begin("Volumetric Fog");
            ImGui::TextWrapped("視錐台に沿った 3D テクスチャ（160x90x64）へ散乱を焼いてから "
                               "画面へ合成する。空気そのものが光る＝光の筋（ゴッドレイ）が "
                               "立体的に見える。透視ビューのみ。有効にした時点で 28MB 確保する。");
            ImGui::Separator();

            ImGui::Checkbox("有効", &f.enabled);
            ImGui::BeginDisabled(!f.enabled);

            ImGui::SeparatorText("媒質");
            ImGui::SliderFloat("濃度##fog",     &f.density,       0.0f, 0.3f,  "%.4f");
            ImGui::ColorEdit3 ("散乱アルベド",   &f.albedo.x);
            ImGui::SliderFloat("異方性 g",      &f.anisotropy,   -0.9f, 0.9f,  "%.2f");
            ImGui::TextDisabled("g>0 = 前方散乱（太陽の方を向くと明るい）。0.6-0.8 で強いシャフト");
            ImGui::SliderFloat("高さ減衰(1/m)", &f.heightFalloff, 0.0f, 0.5f,  "%.3f");
            ImGui::DragFloat  ("基準高さ(Y)",   &f.heightRef,     0.1f, -500.0f, 500.0f, "%.1f");

            ImGui::SeparatorText("ボリューム");
            ImGui::SliderFloat("到達距離(m)",   &f.distance,        10.0f, 500.0f, "%.0f");
            ImGui::SliderFloat("深度分布 k",    &f.depthDistribution, 1.0f, 4.0f, "%.2f");
            ImGui::TextDisabled("z = 距離 * w^k。1=線形 / 大きいほど手前が細かい");
            ImGui::Checkbox("到達距離の外を解析フォグで延長", &f.extendBeyondRange);

            ImGui::SeparatorText("ライティング");
            ImGui::ColorEdit3 ("環境散乱",      &f.ambient.x);
            ImGui::SliderFloat("太陽の寄与",    &f.sunIntensity, 0.0f, 5.0f, "%.2f");
            ImGui::Checkbox("点光源/スポットも散乱させる", &f.lightScattering);
            ImGui::TextDisabled("クラスタライトリストを引く（クラスタード無効時はスキップ）");

            ImGui::SeparatorText("時間再投影");
            ImGui::Checkbox("有効##fogTemporal", &f.temporal);
            ImGui::BeginDisabled(!f.temporal);
            ImGui::SliderFloat("現フレーム比率", &f.temporalBlend, 0.01f, 1.0f, "%.3f");
            ImGui::TextDisabled("小さいほど滑らかだがゴーストが増える（既定 0.08）");
            ImGui::EndDisabled();

            ImGui::SeparatorText("デバッグ表示（保存されない）");
            ImGui::Combo("表示##fogDebug", &f.debugMode,
                         "オフ\0散乱だけ\0透過率だけ\0froxel スライス\0\0");
            ImGui::EndDisabled();
            ImGui::End();
        }

        // ---- Scene Flow 設定ウィンドウ（シーンの流れ・トグル表示）----
        if (m_sceneFlow && m_editorCtx->showSceneFlow)
        {
            namespace fs = std::filesystem;
            std::vector<std::string> scenes;
            std::string scenesDir = PathResolver::AssetsDir() + "scenes";
            if (fs::exists(scenesDir))
            {
                for (auto& e : fs::directory_iterator(scenesDir))
                    if (e.is_regular_file() && e.path().extension() == ".json")
                        scenes.push_back("scenes/" + e.path().filename().string());
            }

            ImGui::Begin("Scene Flow");
            ImGui::TextWrapped("ゲーム開始シーンと、各シーンの次シーンを設定する。");

            std::string start = m_sceneFlow->Start();
            if (ImGui::BeginCombo("Start Scene", start.empty() ? "(none)" : start.c_str()))
            {
                for (auto& s : scenes)
                    if (ImGui::Selectable(s.c_str(), s == start))
                        m_sceneFlow->SetStart(s);
                ImGui::EndCombo();
            }

            ImGui::SeparatorText("Next scene");
            for (auto& s : scenes)
            {
                std::string nx = m_sceneFlow->Next(s);
                ImGui::PushID(s.c_str());
                if (ImGui::BeginCombo(s.c_str(), nx.empty() ? "(none)" : nx.c_str()))
                {
                    if (ImGui::Selectable("(none)", nx.empty()))
                        m_sceneFlow->SetNext(s, "");
                    for (auto& t : scenes)
                        if (ImGui::Selectable(t.c_str(), t == nx))
                            m_sceneFlow->SetNext(s, t);
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            if (ImGui::Button("Save sceneflow.json"))
                m_sceneFlow->Save(PathResolver::AssetsDir() + "sceneflow.json");
            ImGui::End();
        }

        // ---- ビルド設定ウィンドウ（構成/開始シーン/出力先 → ビルド実行・トグル表示）----
        RenderBuildSettingsWindow();

        // Deferred: game build
        // ビルド設定パネルの「ビルド」で pendingBuildGame が立つ。配置先などは buildConfig から読む。
        // 完了したら（設定で有効なら）成果物フォルダを Explorer で開く。
        if (m_editorCtx->pendingBuildGame)
        {
            m_editorCtx->pendingBuildGame = false;
            const bool ok = BuildGame();
            if (ok)
            {
                m_editorCtx->buildCompleteFlash = 3.0f;
                if (m_editorCtx->buildConfig.openFolderAfterBuild && !m_editorCtx->lastBuildDir.empty())
                    ShellExecuteA(nullptr, "open", m_editorCtx->lastBuildDir.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
            }
            else
            {
                m_editorCtx->buildErrorFlash = 6.0f;
                m_editorCtx->errorMessage = m_editorCtx->buildErrorMsg.empty()
                    ? "ビルドに失敗しました。\n詳細は dx12_engine.log を確認してください。"
                    : m_editorCtx->buildErrorMsg;
                m_editorCtx->buildErrorMsg.clear();  // 次回ビルドへ持ち越さない
                m_editorCtx->errorFlash = 1.0f;   // 中央モーダルで通知
            }
        }

        // Deferred: entity deletion
        if (!m_editorCtx->pendingDeletions.empty())
        {
            auto deletions = std::move(m_editorCtx->pendingDeletions);
            m_editorCtx->pendingDeletions.clear();
            // ★ルートごとに PushCommand していたので、5 個選んで Del すると
            //   Undo エントリが 5 個できて Ctrl+Z 1 回では 1 個しか戻らなかった
            //   （残りは消えたまま。ユーザーから見れば「Undo が壊れている」）。
            //   1 回の操作は 1 エントリに束ねる（マルチ選択ギズモは既にそうしている）。
            auto deleteBatch = std::make_unique<CompositeCommand>("Delete");
            for (auto root : deletions)
            {
                auto& reg = m_scene->GetRegistry();
                if (!reg.valid(root)) continue;  // 先行削除のサブツリーに含まれていた場合

                // サブツリー収集（親→子の順。BFS）。壊れたデータで親子が相互参照して
                // いても止まるよう、訪問済み集合で重複追加を弾く（無いと無限膨張する）
                std::vector<entt::entity> subtree{root};
                std::unordered_set<entt::entity> visited{root};
                for (size_t i = 0; i < subtree.size(); ++i)
                {
                    for (auto [c, t] : reg.view<const Transform>().each())
                    {
                        if (t.parent == subtree[i] && visited.insert(c).second)
                            subtree.push_back(c);
                    }
                }

                // Undo 用スナップショット（全コンポーネント + ローカル親インデックス）
                std::vector<DeletedEntityRecord> records;
                records.reserve(subtree.size());
                entt::entity externalParent = reg.all_of<Transform>(root)
                    ? reg.get<Transform>(root).parent : entt::null;
                for (auto e : subtree)
                {
                    DeletedEntityRecord rec;
                    rec.snapshot = SceneSerializer::SerializeEntity(
                        *m_scene, e, PathResolver::AssetsDir());
                    if (reg.all_of<Transform>(e))
                    {
                        auto parent = reg.get<Transform>(e).parent;
                        auto it = std::find(subtree.begin(), subtree.end(), parent);
                        if (it != subtree.end())
                            rec.parentLocalIndex = static_cast<int>(it - subtree.begin());
                    }
                    records.push_back(std::move(rec));
                }

                deleteBatch->Add(
                    std::make_unique<DeleteEntityCommand>(
                        m_scene.get(), PathResolver::AssetsDir(),
                        std::move(records), subtree, externalParent));

                // 子から順に削除
                for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
                {
                    if (reg.valid(*it))
                        m_scene->Remove(Entity(*it, &reg));
                }
            }
            if (!deleteBatch->Empty())
                m_editorCtx->undoSystem.PushCommand(std::move(deleteBatch));

            // 削除で無効になった選択をクリーンアップ
            {
                auto& reg = m_scene->GetRegistry();
                auto& sel = m_editorCtx->selectedEntities;
                sel.erase(std::remove_if(sel.begin(), sel.end(),
                          [&](entt::entity e) { return !reg.valid(e); }),
                          sel.end());
                if (m_editorCtx->selectedEntity != entt::null
                    && !reg.valid(m_editorCtx->selectedEntity))
                {
                    m_editorCtx->selectedEntity =
                        sel.empty() ? entt::null : sel.back();
                }
            }
        }

        // ---- プロジェクト / バージョン管理(Git) ウィンドウ（トグル表示）----
        if (m_editorCtx->showProject)        RenderProjectWindow();
        if (m_editorCtx->showVersionControl) RenderVersionControlWindow();
        if (m_editorCtx->showMcpBridge && m_mcpBridge)
            McpBridgePanel::Render(*m_mcpBridge, *m_editorCtx);
        if (m_networkPanel && m_networkSystem)
        {
            if (m_editorCtx->showNetworkStatus)
                m_networkPanel->RenderStatus(*m_networkSystem, m_scene->GetRegistry(), *m_editorCtx);
            if (m_editorCtx->showNetworkSettings)
                m_networkPanel->RenderSettings(*m_networkSystem, *m_editorCtx, PathResolver::AssetsDir());
        }
        if (m_vfxEditorPanel)
            m_vfxEditorPanel->RenderWindow(m_scene->GetRegistry(), *m_editorCtx, PathResolver::AssetsDir(),
                                           m_scene.get());
        if (m_uiEditorPanel)
            m_uiEditorPanel->RenderWindow(m_scene->GetRegistry(), *m_editorCtx, PathResolver::AssetsDir(),
                                          m_resourceManager.get(), m_srvHeap.get(), nativeCmdList);
        if (m_animEditorPanel)
            m_animEditorPanel->RenderWindow(m_scene->GetRegistry(), *m_editorCtx,
                                            PathResolver::AssetsDir(), m_uiAnimRuntime.get());
        if (m_spriteSheetEditorPanel)
            m_spriteSheetEditorPanel->RenderWindow(m_scene->GetRegistry(), *m_editorCtx,
                                                   PathResolver::AssetsDir(), m_resourceManager.get(),
                                                   m_srvHeap.get(), nativeCmdList,
                                                   m_uiAnimRuntime.get());
        if (m_materialEditorPanel)
            m_materialEditorPanel->RenderWindow(*m_editorCtx, PathResolver::AssetsDir());
        if (m_materialLibraryPanel)
            m_materialLibraryPanel->RenderWindow(*m_editorCtx, PathResolver::AssetsDir());
        // 地形ツール（ハイトフィールドのスカルプト）。窓が閉じていても呼ぶ＝Undo で戻した
        // 高さがメッシュへ反映される（中で _meshDirty を見て作り直している）。
        TerrainPanel::Render(*m_scene, *m_editorCtx, PathResolver::AssetsDir(), nativeCmdList);
        // スカルプト窓（任意メッシュの頂点スカルプト）。地形ツールと同じ理由で
        // 窓が閉じていても呼ぶ＝Undo で戻した頂点がメッシュへ反映される。
        SculptPanel::Render(*m_scene, *m_editorCtx, PathResolver::AssetsDir(), nativeCmdList);
    }

    // ---- ゲーム内 UI: テキスト/ボタン（ImGui オーバーレイ・ゲーム/Play 中のみ）----
    if (m_isGameMode || m_engineMode == EngineMode::Playing)
    {
        // multi-viewport有効時、ImGui座標はスクリーン座標になるためメインビューポート原点基準で置く
        const ImGuiViewport* mainVp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainVp->Pos);
        ImGui::SetNextWindowSize(mainVp->Size);
        ImGui::SetNextWindowViewport(mainVp->ID);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
        // ボーダー無効化必須: ImGui はウィンドウ外周に 1px の枠を描くため、フルスクリーンの
        // ##GameUI では画面の最外周 1px に線が出る（ゲーム画面を縁取る「余白」に見える）
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##GameUI", nullptr, flags);
        ImGui::PopStyleVar();

        auto* dl = ImGui::GetWindowDrawList();
        // ゲーム UI はビューポート原点へオフセットし、矩形でクリップ＝パネル下に潜らない
        // (vpLeft/vpTop はクライアント基準なので ImGui 座標へはメインビューポート原点を足す)
        const float ox = mainVp->Pos.x + static_cast<float>(vpLeft);
        const float oy = mainVp->Pos.y + static_cast<float>(vpTop);
        dl->PushClipRect(ImVec2(ox, oy),
                         ImVec2(ox + static_cast<float>(vpW), oy + static_cast<float>(vpH)), true);

        // retained-mode UI（UICanvas ツリー）を即時 UI コマンドより先に描く＝即時 ui:* が手前。
        // クリック確定はここでは配信せず、次フレーム Update 冒頭（Lua OnUpdate より前）で
        // EventBus へ Emit される（UISystem::DispatchPendingClicks）。
        if (m_uiSystem && m_scene)
        {
            // フォーカスナビ入力（キーボード矢印/Enter/Space + パッド0 の D-pad/左スティック/A）。
            // 押しっぱなし状態を渡すだけ（エッジ/リピートは UISystem 側）。
            UiNavInput nav;
            if (m_inputSystem)
            {
                auto& in = *m_inputSystem;
                constexpr float kStick = 0.5f;   // スティックの方向判定しきい値
                nav.left    = in.IsKeyDown(VK_LEFT)  || in.IsPadButtonDown(0, XINPUT_GAMEPAD_DPAD_LEFT)
                           || in.GetPadLeftStickX(0) < -kStick;
                nav.right   = in.IsKeyDown(VK_RIGHT) || in.IsPadButtonDown(0, XINPUT_GAMEPAD_DPAD_RIGHT)
                           || in.GetPadLeftStickX(0) >  kStick;
                nav.up      = in.IsKeyDown(VK_UP)    || in.IsPadButtonDown(0, XINPUT_GAMEPAD_DPAD_UP)
                           || in.GetPadLeftStickY(0) >  kStick;
                nav.down    = in.IsKeyDown(VK_DOWN)  || in.IsPadButtonDown(0, XINPUT_GAMEPAD_DPAD_DOWN)
                           || in.GetPadLeftStickY(0) < -kStick;
                nav.confirm = in.IsKeyDown(VK_RETURN) || in.IsKeyDown(VK_SPACE)
                           || in.IsPadButtonDown(0, XINPUT_GAMEPAD_A);
            }
            m_uiSystem->RenderAndUpdateInput(m_scene->GetRegistry(), dl, ox, oy,
                                             static_cast<float>(vpW), static_cast<float>(vpH),
                                             m_resourceManager.get(), m_srvHeap.get(),
                                             nativeCmdList, nav);
            // UIButton の hover/click 効果音（UISystem は AudioSystem 非依存なのでここで配信）
            if (m_audioSystem)
                for (const std::string& sfx : m_uiSystem->TakePendingSfx())
                    m_audioSystem->PlaySFX(sfx);
        }

        std::unordered_set<std::string> nowPressed;
        for (const auto& c : m_uiCommands)
        {
            if (c.type == UICommand::Type::Rect)
            {
                ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
                dl->AddRectFilled(ImVec2(ox + c.x, oy + c.y),
                                  ImVec2(ox + c.x + c.w, oy + c.y + c.h), col, c.size);
            }
            else if (c.type == UICommand::Type::Text)
            {
                ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
                dl->AddText(ImGui::GetFont(), c.size, ImVec2(ox + c.x, oy + c.y), col, c.text.c_str());
            }
            else if (c.type == UICommand::Type::Button)
            {
                // SetCursorPos はウィンドウローカル座標(窓原点=メインビューポート原点)なので
                // スクリーン座標の ox/oy ではなくクライアント基準の vpLeft/vpTop を使う
                ImGui::SetCursorPos(ImVec2(static_cast<float>(vpLeft) + c.x,
                                           static_cast<float>(vpTop)  + c.y));
                if (ImGui::Button(c.text.c_str(), ImVec2(c.w, c.h)))
                    nowPressed.insert(c.text);
            }
        }
        dl->PopClipRect();
        ImGui::End();
        m_pressedButtons = std::move(nowPressed);
    }
    else
    {
        m_pressedButtons.clear();
    }
    m_uiCommands.clear();

    // 段階ロード中のローディング UI。パネルより手前に出すため ImGui の最後に描く。
    RenderSceneLoadingOverlay();

    // エンジン診断パネル(ツール > エンジン診断)。ImGui の最後に描く
    if (m_uiTests) m_uiTests->DrawDiagnosticsPanel(&m_editorCtx->showEngineDiagnostics,
                                                   &m_editorCtx->floatingToolWindowHoveredThisFrame);

    { CpuScopeTimer _tImGui(&m_cpuMs[CpuImGui]); DX12_PROFILE_ZONE_N("Rec/ImGui");
      m_imguiManager->EndFrame(nativeCmdList); }

    // ---- シーントランジション オーバーレイ（ImGui の上に被せる）----
    // フェードは 3D ビューにだけ適用する:
    //   エディタ/Play 中 … 中央ビューポート矩形だけにスシザーを絞り、周りの
    //                      エディタUI（パネル/ツールバー）には掛からないようにする。
    //   ゲーム単体       … ウィンドウ全体。
    // 段階ロード中は描かない: このオーバーレイは ImGui より後＝ローディング UI を塗り潰して
    // しまう。ロード中の画面はローディング UI 自身が不透明に覆っているので目隠しは足りている。
    if (m_sceneTransition && m_sceneTransition->IsActive() && !m_sceneLoadJob)
    {
        u32 tLeft = 0, tTop = 0;
        u32 tW = m_window->GetWidth(), tH = m_window->GetHeight();
        if (!m_isGameMode && m_editorLayer)
        {
            auto vpos  = m_editorLayer->GetViewportPos();
            auto vsize = m_editorLayer->GetViewportSize();
            tLeft = static_cast<u32>(vpos.x);
            tTop  = static_cast<u32>(vpos.y);
            tW    = static_cast<u32>(vsize.x);
            tH    = static_cast<u32>(vsize.y);
            if (tW < 1) tW = 1;
            if (tH < 1) tH = 1;
        }

        nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_commandList->SetViewportAndScissor(tLeft, tTop, tW, tH);
        float aspect = (tH > 0) ? static_cast<f32>(tW) / static_cast<f32>(tH) : 1.0f;
        m_sceneTransition->Render(nativeCmdList, aspect);
    }

    m_gpuTimer->End(nativeCmdList, GpuTimer::UI);
    m_gpuTimer->End(nativeCmdList, GpuTimer::Total);
    m_gpuTimer->Resolve(nativeCmdList);

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    // multi-viewport: 引き出したフローティング窓(セカンダリスワップチェイン)の描画+Present。
    // メインリストのExecute後に呼ぶことで、メインリスト内で遷移したテクスチャの状態が正しく見える。
    m_imguiManager->RenderPlatformWindows();
    const auto perfPresentT0 = std::chrono::high_resolution_clock::now();
    { DX12_PROFILE_ZONE_N("Present");
      m_swapChain->Present(m_useVsync); }
    m_frameResources->EndFrame(*m_commandQueue);
    m_perfPresentMs = std::chrono::duration<f32, std::milli>(
        std::chrono::high_resolution_clock::now() - perfPresentT0).count();

    // UI 自動テスト: Present 後にテストのコルーチンを進める(--ui-tests-run-all なら完走で終了要求)
    if (m_uiTests)
    {
        m_uiTests->PostRender();
        if (m_uiTests->WantsExit())
        {
            m_uiTestExitCode = m_uiTests->ExitCode();
            PostQuitMessage(0);
        }
    }

    // フェンス連動の遅延解放（DeferredRelease）。リモート側の「アップロードを積んだ
    // フレームだけ WaitIdle」方式より強い保証:
    //   - アップロードのあるフレームでも GPU 全停止しない（スポーン時のヒッチ無し）
    //   - メッシュ再生成/シーンClear/RT再作成など GpuResource 系の解放も
    //     フェンス完了までキューで保護される
    //   1. 今フレームでロードされたテクスチャのアップロードステージングを解放キューへ
    //   2. キュー内の未確定分に今フレームの Signal 値を刻む
    //   3. GPU が完了したフェンス値以下の分を実際に解放
    m_resourceManager->DeferPendingUploads();
    DeferredRelease::Stamp(m_commandQueue->GetLastSignaledValue());
    DeferredRelease::Collect(m_commandQueue->GetCompletedValue());

    RecordPerfFrame();
}


} // namespace dx12e
