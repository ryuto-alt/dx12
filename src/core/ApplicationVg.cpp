// ===========================================================================
// 仮想ジオメトリ（Nanite 風）P2: シーンとの接続（インスタンス収集）+ GPU カリングの呼び出し + 統計。
// ---------------------------------------------------------------------------
// 本体は renderer/vg/VirtualGeometrySystem（専用ルートシグネチャ・専用ディスクリプタヒープ。D3D12 デバイスだけで動く）。
// ここは Application 側の糊: VirtualGeometry コンポーネントを持つエンティティの world 行列を集め、
// .vgeo を非同期に読み込み、二相 HZB（m_vgHiZ）を渡して Execute を呼ぶ。
// ★P3: メッシュシェーダ対応 GPU では VG 本体が可視性バッファ + 深度へ描かれ、プロキシは主ビューの深度プリパス / フォワードから外れる
//   （DrawItem::vg。影 / TLAS / ピッキング / 物理はプロキシのまま）。非対応 GPU / raster=false ではプロキシが従来経路で描かれる（P2 と同じ）。
// ★既定 OFF（Scene の VirtualGeometrySettings::enabled）。OFF の間は m_vg も m_vgHiZ も作らない。
// ★P4: 材質 resolve（H3。DrawVirtualGeometryResolve）/ 速度 + G-Buffer（H2。RunVirtualGeometryGBuffer）/ 材質表の登録 /
//   VG 対象外の判定（renderer/vg/VgEligibility.h）/ 決定論（stableOrder）。VG のディスクリプタはアプリの SRV ヒープへ一本化。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "renderer/ViewDesc.h"
#include "renderer/vg/VgEligibility.h"
#include "renderer/vg/VgeoFormat.h"     // P4: 材質の flags（kMat*）/ テクスチャパスの安全検査
#include "resource/VgAutoCook.h"         // 仮想ジオメトリの自動 cook（状態 / 差し替え）
#include "resource/MaterialAssetManager.h"

#include <cctype>
#include <cmath>

namespace dx12e
{

namespace
{
bool IsVgeoPath(const std::string& p)
{
    if (p.size() < 5) return false;
    std::string ext = p.substr(p.size() - 5);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".vgeo";
}

// assets 相対 / 絶対のどちらでも、実在するファイルの絶対パスを返す（無ければ与えられたまま）。
// ★BuildDrawList（毎フレーム）からも呼ばれるので、実在が確かめられた結果だけをキャッシュする（ファイル System 呼び出しを毎フレーム繰り返さない）。
std::string ResolveVgPath(const std::string& p)
{
    namespace fs = std::filesystem;
    static std::unordered_map<std::string, std::string> cache;
    if (auto it = cache.find(p); it != cache.end()) return it->second;
    std::error_code ec;
    const fs::path fp = fs::path(std::u8string(p.begin(), p.end()));
    // ★配布ゲームは assets/ がディスクに無い（pak の中）ので fs::exists だけだと常に偽になり、
    //   相対パスのまま VG に渡って .vgeo を開けず、仮想ジオメトリが丸ごと描かれない。pak の TOC も見る。
    const auto existsAny = [&](const std::string& s, const fs::path& fpath) {
        return vfs::ExistsAbs(s) || fs::exists(fpath, ec);
    };
    if (fp.is_absolute() && existsAny(p, fp)) { cache.emplace(p, p); return p; }
    const std::string underAssets = PathResolver::AssetsDir() + p;
    if (existsAny(underAssets, fs::path(std::u8string(underAssets.begin(), underAssets.end())))) { cache.emplace(p, underAssets); return underAssets; }
    return p;
}

// VG で描くエンティティを列挙する（★プロキシを外す集合・インスタンス収集・統計が同じ列挙を使う）。
//   ・VirtualGeometry コンポーネント（enabled）: パス = vgeoPath > MeshRenderer.modelPath（.vgeo）> MeshRenderer.vgAutoPath。
//     シーンの VG 設定が ON のときだけ。
//   ・自動 cook（MeshRenderer.vgAutoPath。コンポーネント無し）: シーンの VG 設定に関係なく（「置くだけで VG」）。
//     VirtualGeometry{enabled=false} を付ければそのエンティティだけ外せる。
//   fn(entity, path, meshRenderer or null, isAuto)
template <class Fn>
void ForEachVgEntity(entt::registry& reg, bool sceneEnabled, Fn&& fn)
{
    if (sceneEnabled)
        for (auto [e, vgc] : reg.view<VirtualGeometry>().each())
        {
            if (!vgc.enabled) continue;
            const MeshRenderer* mr = reg.try_get<MeshRenderer>(e);
            std::string path = vgc.vgeoPath;
            if (path.empty() && mr) path = IsVgeoPath(mr->modelPath) ? mr->modelPath : mr->vgAutoPath;
            if (path.empty() || !IsVgeoPath(path)) continue;
            fn(e, path, mr, !mr || mr->vgAutoPath.empty() ? false : path == mr->vgAutoPath);
        }
    for (auto [e, mr] : reg.view<MeshRenderer>().each())
    {
        if (mr.vgAutoPath.empty()) continue;
        if (const VirtualGeometry* vgc = reg.try_get<VirtualGeometry>(e))
            if (!vgc->enabled || sceneEnabled) continue;   // 外された / 上のループで済んだ
        fn(e, mr.vgAutoPath, &mr, true);
    }
}

// サブメッシュ番号 → .vgeo の材質番号（CachedModel::vgMaterialOfMesh）。MeshRenderer の先頭メッシュで覚える（差し替えで捨てる）。
std::unordered_map<const Mesh*, std::vector<u32>> g_vgMeshMatMap;

// P4: VG 本体で描いてよいアセットか（準備済み + 材質を登録済み）。★インスタンス収集とプロキシを外す集合の両方が同じ条件を使う。
bool VgAssetDrawable(const vg::VirtualGeometrySystem& sys, u32 id)
{
    vg::AssetInfo a;
    if (!sys.GetAssetInfo(id, a) || !a.ready) return false;
    return a.materialsBound || a.materialCount == 0;
}
} // namespace

bool Application::VirtualGeometryCullWanted(bool primary) const
{
    if (!primary || m_vgUnavailable || !m_scene || !m_graphicsDevice) return false;
    if (!m_graphicsDevice->SupportsDynamicResources()) return false;   // SM 6.6 + Resource Binding Tier 3
    // 自動 cook のエンティティがあればシーンの設定が OFF でも回す（それらだけを描く。ForEachVgEntity）
    return m_scene->GetVirtualGeometrySettings().enabled || m_vgAutoPresent;
}

// P3: このフレーム、VG のラスタ（メッシュシェーダ）を回すか。設定 ON + raster + GPU 対応 + 初期化済み（未初期化なら見込みで true）。
bool Application::VirtualGeometryRasterWanted(bool primary) const
{
    if (!VirtualGeometryCullWanted(primary)) return false;
    if (!m_scene->GetVirtualGeometrySettings().raster) return false;
    if (!m_graphicsDevice->SupportsMeshShaders()) return false;
    if (m_vg && !m_vg->RasterSupported()) return false;
    // P4: VG 本体の最終色は resolve（メイン RS + バインドレス）で塗る。それが使えない環境（メイン RS にバインドレスが無い /
    //   resolve の PSO が作れない）では VG を描かずプロキシを従来経路で描く（暫定シェーディングの灰色で出さない）。
    //   ★resolve:false（A/B 用の暫定シェーディング）のときはこの条件を見ない。
    if (m_scene->GetVirtualGeometrySettings().resolve)
    {
        if (!m_rootSignature || !m_rootSignature->IsBindless()) return false;
        if (m_vg && !m_vg->ResolveSupported()) return false;
    }
    return true;
}

// P3: VG 本体が描くエンティティ（ラスタ有効 + アセット準備済み + VirtualGeometry.enabled）を集める。BuildDrawList が DrawItem::vg を立てる。
void Application::VirtualGeometryCollectProxyHide()
{
    m_vgHiddenEntities.clear();
    // 自動 cook のエンティティがあるか（VirtualGeometryCullWanted が見る。毎フレーム数え直す）
    m_vgAutoPresent = false;
    if (m_scene)
    {
        entt::registry& r = m_scene->GetRegistry();
        for (auto [e, mr] : r.view<MeshRenderer>().each())
        {
            if (mr.vgAutoPath.empty()) continue;
            if (const VirtualGeometry* vgc = r.try_get<VirtualGeometry>(e); vgc && !vgc->enabled) continue;
            m_vgAutoPresent = true;
            break;
        }
    }
    if (!m_scene || !m_vg || !VirtualGeometryRasterWanted(true)) return;
    entt::registry& reg = m_scene->GetRegistry();
    const auto isGraph = VgGraphPredicate();
    ForEachVgEntity(reg, m_scene->GetVirtualGeometrySettings().enabled,
        [&](entt::entity e, const std::string& path, const MeshRenderer*, bool)
        {
            if (vg::VgIneligibleReason(reg, e, isGraph)) return;   // P4: 対象外（スキンド / 半透明 / カスタムシェーダー / グラフ材質 …）はプロキシで描く
            const u32 id = m_vg->FindAsset(ResolveVgPath(path));
            if (id != 0xFFFFFFFFu && VgAssetDrawable(*m_vg, id)) m_vgHiddenEntities.insert(static_cast<u32>(entt::to_integral(e)));
        });
}

std::function<bool(const std::string&)> Application::VgGraphPredicate() const
{
    return [this](const std::string& rel)
    {
        if (!m_materialAssetManager) return false;
        const MaterialAssetManager::Entry* en = m_materialAssetManager->FindLoaded(rel);
        return en && en->data.IsGraph();
    };
}

bool Application::VirtualGeometryHidesProxy(entt::entity e) const
{
    return m_vgHiddenEntities.count(static_cast<u32>(entt::to_integral(e))) != 0;
}

bool Application::DrawVirtualGeometryDebug(ID3D12GraphicsCommandList* cmd, u32 frameIndex, u32 mode, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                           u32 w, u32 h, f32 zFar)
{
    if (!m_vg) return false;
    vg::DebugDrawDesc d;
    d.cmd = cmd;
    d.frameSlot = frameIndex % FrameResources::kFrameCount;
    d.mode = mode;
    d.rtv = rtv;
    d.rtFormat = appdetail::kSceneColorFormat;
    d.width = w; d.height = h;
    d.depthRange = m_renderDebugDepthRange;
    d.overdrawMax = 8.0f;
    d.zFar = zFar;
    d.restoreHeap = m_srvHeap->GetHeap();
    return m_vg->DrawDebug(d);
}

// ---------------------------------------------------------------------------
// P4: 材質表の登録。.vgeo の MaterialRecord（テクスチャは .vgeo のフォルダ基準の相対パス）を読み、
//   テクスチャをアプリの ResourceManager で読んで（永続の SRV 添字。G2a）VgMaterialGpu に詰める。
//   ★パスの作り方・sRGB / 用途は VgeoProxyLoader.cpp と同じ＝プロキシが読んだテクスチャのキャッシュにそのまま当たる。
//   ★係数の既定（emissive テクスチャだけの材質は色 1・強度 1）も VgeoProxyLoader と同じ。
// ---------------------------------------------------------------------------
void Application::VirtualGeometryBindMaterials(ID3D12GraphicsCommandList* cmd)
{
    if (!m_vg || !m_resourceManager) return;
    namespace fs = std::filesystem;
    const u32 n = m_vg->AssetCount();
    for (u32 id = 0; id < n; ++id)
    {
        vg::AssetInfo info;
        if (!m_vg->GetAssetInfo(id, info) || !info.ready || info.materialsBound || info.materialCount == 0) continue;
        std::vector<vg::AssetMaterial> mats;
        if (!m_vg->GetAssetMaterials(id, mats) || mats.empty()) continue;

        const fs::path dir = fs::path(std::u8string(info.path.begin(), info.path.end())).parent_path();
        auto loadTex = [&](const std::string& rel, bool srgb, TextureUsage usage) -> u32
        {
            if (rel.empty()) return VG_NONE;
            if (!vg::detail::IsSafeRelativePath(rel.c_str()))
            {
                Logger::Warn("仮想ジオメトリ: .vgeo のテクスチャパスが安全ではないため無視しました: {}", rel);
                return VG_NONE;
            }
            fs::path p = (dir / fs::path(std::u8string(rel.begin(), rel.end()))).lexically_normal();
            p = fs::path(p.generic_wstring());
            std::error_code ec;
            if (!vfs::ExistsAbs(p.wstring()) && !fs::exists(p, ec))
            {
                Logger::Warn("仮想ジオメトリ: .vgeo のテクスチャが見つかりません: {}", rel);
                return VG_NONE;
            }
            const u32 idx = m_resourceManager->GetOrLoadTextureSrvIndex(p.wstring(), cmd, srgb, usage);
            return (idx == ResourceManager::kInvalidSrvIndex) ? VG_NONE : idx;
        };

        std::vector<vg::VgMaterialGpu> gpu(mats.size());
        for (size_t i = 0; i < mats.size(); ++i)
        {
            const vg::AssetMaterial& m = mats[i];
            vg::VgMaterialGpu& g = gpu[i];
            g = vg::VgMaterialGpu{};
            g.albedoSrv     = loadTex(m.albedo,     true,  TextureUsage::BaseColor);
            g.normalSrv     = loadTex(m.normal,     false, TextureUsage::Normal);
            g.metalRoughSrv = loadTex(m.metalRough, false, TextureUsage::NonColor);
            g.emissiveSrv   = loadTex(m.emissive,   true,  TextureUsage::BaseColor);
            // flags はフォワード（ApplicationRender.cpp の pbrParams.flags）と同じく「テクスチャが実際に読めたか」で立てる
            g.flags = (g.normalSrv != VG_NONE ? VG_MAT_NORMAL_TEX : 0u) | (g.metalRoughSrv != VG_NONE ? VG_MAT_MR_TEX : 0u)
                    | (g.emissiveSrv != VG_NONE ? VG_MAT_EMISSIVE_TEX : 0u) | ((m.flags & vg::kMatDoubleSided) ? VG_MAT_DOUBLE_SIDED : 0u);
            g.metallic  = m.metallic;
            g.roughness = m.roughness;
            // マテリアル AO（.vgeo v1.1。フォワードの PackAoFlags と同じ 8bit 量子化の値。MR が読めたときだけ）
            g.aoStrength = (g.metalRoughSrv != VG_NONE && m.aoStrength > 0.0f)
                         ? static_cast<f32>(QuantizeUnorm8(std::clamp(m.aoStrength, 0.0f, 1.0f))) / 255.0f : 0.0f;
            std::memcpy(g.uvScaleOffset, m.uvScaleOffset, 16);
            std::memcpy(g.emissiveColor, m.emissiveColor, 12);
            g.emissiveIntensity = m.emissiveIntensity;
            {
                const PackedShadingExt ext = PackShadingExt(m.shading);   // 既定なら 8 個とも 0
                std::memcpy(g.ext, ext.data(), sizeof(g.ext));
            }
            if (g.emissiveSrv != VG_NONE && g.emissiveIntensity <= 0.0f)
            {
                // テクスチャだけの指定＝素通し（VgeoProxyLoader / ModelLoader と同じ扱い）
                g.emissiveColor[0] = g.emissiveColor[1] = g.emissiveColor[2] = 1.0f;
                g.emissiveIntensity = 1.0f;
            }
        }
        if (m_vg->SetAssetMaterials(id, gpu.data(), static_cast<u32>(gpu.size())))
            Logger::Info("仮想ジオメトリ: 材質を登録しました（{} / {} 個）", info.path, static_cast<u32>(gpu.size()));
    }
}

// ---------------------------------------------------------------------------
// P4（H2）: VG 画素の速度 + G-Buffer。深度プリパス（DepthVelocityGBuffer モード）が書いた RT の VG 画素だけを上書きする。
//   TAA / SSR / SSGI / RT-AO が VG 画素を正しく扱うための前提（VG_P3_REPORT §8）。
// ---------------------------------------------------------------------------
bool Application::RunVirtualGeometryGBuffer(u32 frameIndex)
{
    if (!m_vg || !m_gbufferRT || !m_taaPass || !m_taaPass->GetVelocityRT()) return false;
    RenderTarget* vel = m_taaPass->GetVelocityRT();
    vel->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_gbufferRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    vg::GBufferDesc d;
    d.cmd = m_commandList->GetNative();
    d.frameSlot = frameIndex % FrameResources::kFrameCount;
    d.velocityRtv = vel->GetRtv();
    d.gbufferRtv = m_gbufferRT->GetRtv();
    d.velocityFormat = TaaPass::kVelocityFormat;
    d.gbufferFormat = appdetail::kGBufferFormat;
    d.restoreHeap = m_srvHeap->GetHeap();
    const bool ok = m_vg->DrawGBuffer(d);
    // 出口の契約（DepthPrepassPass と同じ置き場）
    vel->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_gbufferRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return ok;
}

// ---------------------------------------------------------------------------
// P4（H3）: 材質 resolve。メイン RS + アプリのヒープで、ForwardScenePass / RenderSceneMeshes と同じテーブルを張ってから
//   VG に全画面 PS を描かせる（b0 だけ VG が resolve 用のルート定数に読み替える）。
// ---------------------------------------------------------------------------
bool Application::DrawVirtualGeometryResolve(ID3D12GraphicsCommandList* cmd, const VgResolveIn& in)
{
    if (!m_vg || !m_vg->ResolveSupported() || !m_rootSignature || !m_rootSignature->IsBindless()) return false;
    if (!m_scene || !m_scene->GetVirtualGeometrySettings().resolve) return false;
    // ForwardScenePass::Execute と同じ順序（ヒープ → RS。★フラグ付き RS はヒープが先）
    m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
    m_commandList->SetRootSignature(*m_rootSignature);
    m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, in.perFrameCB);
    m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV, m_srvHeap->GetGpuHandle(m_shadowSrvIndex));
    m_commandList->SetSRVTable(RootSignature::kSlotPunctualShadowSRV, m_srvHeap->GetGpuHandle(m_spotShadowSrvIndex));
    if (m_iblReady && m_iblBaker)
        m_commandList->SetSRVTable(RootSignature::kSlotIBLTable, m_srvHeap->GetGpuHandle(m_iblBaker->GetIrradianceSrv()));
    if (m_clusteredLighting && m_clusteredLighting->IsReady())
        m_commandList->SetSRVTable(RootSignature::kSlotClusterSRV, m_clusteredLighting->GetSrvTable(in.frameIndex));
    // RenderSceneMeshes と同じ AO / コンタクト / SSR / SSGI（無効時は白 / 黒ダミー）
    if (in.aoSrv != DescriptorHeap::kInvalidIndex)
        m_commandList->SetSRVTable(RootSignature::kSlotAOSRV, m_srvHeap->GetGpuHandle(in.aoSrv));
    {
        const u32 cs = (in.csSrv != DescriptorHeap::kInvalidIndex) ? in.csSrv : m_ssaoWhiteSrvIndex;
        if (cs != DescriptorHeap::kInvalidIndex) m_commandList->SetSRVTable(RootSignature::kSlotContactShadowSRV, m_srvHeap->GetGpuHandle(cs));
        const u32 ssr  = (in.ssrSrv  != DescriptorHeap::kInvalidIndex) ? in.ssrSrv  : m_ssBlackSrvIndex;
        const u32 ssgi = (in.ssgiSrv != DescriptorHeap::kInvalidIndex) ? in.ssgiSrv : m_ssBlackSrvIndex;
        if (ssr  != DescriptorHeap::kInvalidIndex) m_commandList->SetSRVTable(RootSignature::kSlotSsrSRV,  m_srvHeap->GetGpuHandle(ssr));
        if (ssgi != DescriptorHeap::kInvalidIndex) m_commandList->SetSRVTable(RootSignature::kSlotSsgiSRV, m_srvHeap->GetGpuHandle(ssgi));
    }
    vg::ResolveDesc d;
    d.cmd = cmd;
    d.frameSlot = in.frameIndex % FrameResources::kFrameCount;
    d.rootSig = m_rootSignature->Get();
    d.rootConstSlot = RootSignature::kSlotPerObject;
    d.rtv = in.rtv;
    d.rtFormat = appdetail::kSceneColorFormat;
    d.width = in.width; d.height = in.height;
    std::memcpy(d.viewProjJ, &in.viewProjJ, 64);
    return m_vg->DrawResolve(d);
}

// ---------------------------------------------------------------------------
// エンティティ単位の材質（2026-11-10。VgEligibility の「材質アセット / テクスチャ上書き / materialRemap は対象外」を外した中身）。
//   アセットの材質（.vgeo の MaterialRecord → VirtualGeometryBindMaterials）を土台に、フォワード（ApplicationRender.cpp の
//   drawEntity）と同じ優先度で差し替える: materialAsset（個別 > materialRemap）> overrideXxxTexture > 焼き込み。
//   係数（metallic / roughness / ティント / 自己発光）のエンティティ上書きはインスタンス表（VgInstance）が持つので、ここは
//   「テクスチャ・材質の土台・AO の強さ・拡張シェーディング」だけ。毎フレーム作り直し、中身が変わったときだけ材質表へ書く
//   （VirtualGeometrySystem::SetKeyedMaterials）。
//   ★両面フラグはアセットの値を残す（K2 の法線コーン棄却はアセットの区間しか見ない。.dxmat / 上書きは両面を変えない）。
//   ★POM（.dxmat の height / parallaxScale）は VG では効かない（VgMaterialGpu に高さの枠が無い。VgResolve.hlsl の冒頭）。
// ---------------------------------------------------------------------------
u32 Application::VgEntityMaterialBase(entt::entity e, const MeshRenderer& mr, u32 assetId, ID3D12GraphicsCommandList* cmd)
{
    if (!m_vg) return VG_NONE;
    mr.RefreshMaterialRemap();
    const u32 nSub = static_cast<u32>(mr.meshes.size());
    const bool shadingOv = HashShadingExtOverride(mr.overrideShading) != HashShadingExtOverride(ShadingExtOverride{});
    bool any = mr.overrideAoStrength >= 0.0f || shadingOv;
    for (u32 si = 0; si < nSub && !any; ++si)
        if (mr.HasMaterialAsset(si) || mr.HasAnyTextureOverride(si)) any = true;
    if (!any) return VG_NONE;

    std::vector<vg::VgMaterialGpu> mats;
    if (!m_vg->GetAssetMaterialsGpu(assetId, mats)) return VG_NONE;
    std::vector<vg::AssetMaterial> src;
    m_vg->GetAssetMaterials(assetId, src);

    // サブメッシュ → .vgeo の材質番号（プロキシは .vgeo の材質ごとに 1 メッシュ。CachedModel::vgMaterialOfMesh）
    const std::vector<u32>* map = nullptr;
    if (!mr.meshes.empty() && m_resourceManager)
    {
        auto it = g_vgMeshMatMap.find(mr.meshes[0]);
        if (it == g_vgMeshMatMap.end())
        {
            std::vector<u32> m;
            const CachedModel* cm = m_resourceManager->FindModel(ResourceManager::NormalizeModelKey(mr.modelPath));
            if (!cm) cm = m_resourceManager->FindModel(ResourceManager::NormalizeModelKey(PathResolver::AssetsDir() + mr.modelPath));
            if (cm && !cm->meshes.empty() && cm->meshes[0].get() == mr.meshes[0]) m = cm->vgMaterialOfMesh;
            it = g_vgMeshMatMap.emplace(mr.meshes[0], std::move(m)).first;
        }
        if (!it->second.empty()) map = &it->second;
    }
    auto quantAo = [](f32 v) { return static_cast<f32>(QuantizeUnorm8(std::clamp(v, 0.0f, 1.0f))) / 255.0f; };

    std::vector<bool> done(mats.size(), false);
    for (u32 si = 0; si < nSub; ++si)
    {
        const u32 k = map ? (si < map->size() ? (*map)[si] : 0xFFFFFFFFu) : si;
        if (k >= mats.size() || done[k]) continue;
        done[k] = true;
        vg::VgMaterialGpu& g = mats[k];
        const u32 doubleSided = g.flags & VG_MAT_DOUBLE_SIDED;
        ShadingExt baseExt = (k < src.size()) ? src[k].shading : ShadingExt{};
        const Material* mat = (si < mr.materials.size() && mr.materials[si]) ? mr.materials[si]
                            : (mr.meshes[si] ? mr.meshes[si]->GetMaterial() : nullptr);

        if (mr.HasMaterialAsset(si) && m_materialAssetManager)
        {
            const MaterialAssetManager::Entry* ma = m_materialAssetManager->GetOrLoad(mr.MaterialAssetOf(si), cmd);
            if (ma && ma->valid && !ma->data.IsGraph() && ma->srvBlockStart != 0xFFFFFFFFu)
            {
                // .dxmat の 5 連続ブロック（albedo / normal / MR / emissive / height）をそのまま添字で引く
                g = vg::VgMaterialGpu{};
                g.albedoSrv     = ma->srvBlockStart;
                g.normalSrv     = ma->hasNormalTex   ? ma->srvBlockStart + 1 : VG_NONE;
                g.metalRoughSrv = ma->hasMRTex       ? ma->srvBlockStart + 2 : VG_NONE;
                g.emissiveSrv   = ma->hasEmissiveTex ? ma->srvBlockStart + 3 : VG_NONE;
                g.flags = (ma->hasNormalTex ? VG_MAT_NORMAL_TEX : 0u) | (ma->hasMRTex ? VG_MAT_MR_TEX : 0u)
                        | (ma->hasEmissiveTex ? VG_MAT_EMISSIVE_TEX : 0u) | doubleSided;
                g.metallic  = ma->data.metallic;
                g.roughness = ma->data.roughness;
                // AO: .dxmat の aoStrength > 0 のときだけ（エンティティ上書きは強さだけ差し替える。フォワードと同じ）
                g.aoStrength = (ma->hasMRTex && ma->data.aoStrength > 0.0f)
                             ? quantAo(mr.overrideAoStrength >= 0.0f ? mr.overrideAoStrength : ma->data.aoStrength) : 0.0f;
                g.uvScaleOffset[0] = 1.0f; g.uvScaleOffset[1] = 1.0f; g.uvScaleOffset[2] = 0.0f; g.uvScaleOffset[3] = 0.0f;
                if (mr.MaterialIsRemapped(si))   // remap 由来の .dxmat だけ uvTiling を描画時に掛ける（フォワードと同じ）
                {
                    g.uvScaleOffset[0] = ma->data.uvTilingU;
                    g.uvScaleOffset[1] = ma->data.uvTilingV;
                }
                std::memcpy(g.emissiveColor, ma->data.emissiveColor, 12);
                g.emissiveIntensity = ma->data.emissiveIntensity;
                baseExt = ma->data.shading;
            }
        }
        else if (mr.HasAnyTextureOverride(si))
        {
            auto load = [&](const std::string& rel, bool srgb, TextureUsage usage) -> u32
            {
                if (rel.empty() || !m_resourceManager || !vfs::Exists(rel)) return VG_NONE;
                const u32 idx = m_resourceManager->GetOrLoadTextureSrvIndex(PathResolver::Utf8ToWide(PathResolver::AssetsDir() + rel), cmd, srgb, usage);
                return idx == ResourceManager::kInvalidSrvIndex ? VG_NONE : idx;
            };
            const u32 a  = load(MeshRenderer::SafeGetOverride(mr.overrideAlbedoTexture, si), true, TextureUsage::BaseColor);
            const u32 n  = load(MeshRenderer::SafeGetOverride(mr.overrideNormalTexture, si), false, TextureUsage::Normal);
            const u32 r  = load(MeshRenderer::SafeGetOverride(mr.overrideMetalRoughnessTexture, si), false, TextureUsage::NonColor);
            const u32 em = load(MeshRenderer::SafeGetOverride(mr.overrideEmissiveTexture, si), true, TextureUsage::BaseColor);
            if (a != VG_NONE) g.albedoSrv = a;
            if (n != VG_NONE) { g.normalSrv = n; g.flags |= VG_MAT_NORMAL_TEX; }
            if (r != VG_NONE) { g.metalRoughSrv = r; g.flags |= VG_MAT_MR_TEX; g.aoStrength = 0.0f; }   // 差し替えた MR の R は AO と限らない（フォワードと同じ）
            if (em != VG_NONE) { g.emissiveSrv = em; g.flags |= VG_MAT_EMISSIVE_TEX; }
            if (r == VG_NONE && mr.overrideAoStrength >= 0.0f && mat && mat->occlusionInMR) g.aoStrength = quantAo(mr.overrideAoStrength);
        }
        else if (mr.overrideAoStrength >= 0.0f && g.aoStrength > 0.0f)
        {
            g.aoStrength = quantAo(mr.overrideAoStrength);   // ResolveAoStrength と同じ（モデルが AO を持つときだけ効く）
        }
        const PackedShadingExt ext = PackShadingExt(ResolveShadingExt(baseExt, mr.overrideShading));
        std::memcpy(g.ext, ext.data(), sizeof(g.ext));
    }
    return m_vg->SetKeyedMaterials(static_cast<u32>(entt::to_integral(e)), mats.data(), static_cast<u32>(mats.size()));
}

// ---------------------------------------------------------------------------
// 自動 cook（resource/VgAutoCook）が終わったモデルの差し替え。毎フレーム、MCP のアセット再読込の直後に呼ぶ。
//   仮の箱 → .vgeo のプロキシ（+ VG 本体）/ 対象外・失敗 → 通常のモデル。手動 cook（vg_cook）した通常モデル → プロキシ + VG。
//   ★古いモデルは GPU が使い終わってから捨てる（WaitIdle。差し替えのあるフレームだけ・1 回）。
// ---------------------------------------------------------------------------
void Application::ProcessVgAutoSwaps(ID3D12GraphicsCommandList* cmdList)
{
    if (!m_resourceManager || !cmdList) return;
    const auto retired = m_resourceManager->PumpVgAuto();
    if (!retired.empty())
    {
        // 使っているエンティティがあるモデルだけ読み直す（無ければ次に要求されたときに読む）
        std::unordered_map<std::string, std::string> wanted;   // key → path
        for (const auto& [k, p] : retired) wanted.emplace(k, p);
        std::unordered_map<std::string, const CachedModel*> byKey;
        if (m_scene)
        {
            const std::string assetsDir = PathResolver::AssetsDir();
            for (auto [e, mr] : m_scene->GetRegistry().view<MeshRenderer>().each())
            {
                if (mr.modelPath.empty() || wanted.empty()) continue;
                std::string k = ResourceManager::NormalizeModelKey(mr.modelPath);
                auto it = wanted.find(k);
                if (it == wanted.end()) { k = ResourceManager::NormalizeModelKey(assetsDir + mr.modelPath); it = wanted.find(k); }
                if (it == wanted.end() || byKey.count(k)) continue;
                byKey[k] = m_resourceManager->GetOrLoadModel(it->second, cmdList);
            }
        }
        const int n = RebindModelEntities(byKey);
        Logger::Info("仮想ジオメトリ: 自動 cook の結果を反映しました（外したモデル {} / 読み直し {} / {} 体）", retired.size(), byKey.size(), n);
        if (m_rtScene) m_rtScene->Invalidate();
        if (m_foliage) m_foliage->InvalidateModels();
    }
    if (m_resourceManager->HasRetiredModels())
    {
        m_commandQueue->WaitIdle();
        m_resourceManager->ReleaseRetiredModels();
    }
}

int Application::RebindModelEntities(const std::unordered_map<std::string, const CachedModel*>& byKey)
{
    if (!m_scene || byKey.empty()) return 0;
    g_vgMeshMatMap.clear();   // Mesh* が入れ替わる（古いアドレスに新しいメッシュが載りうる）
    const std::string assetsDir = PathResolver::AssetsDir();
    int rebound = 0;
    auto& reg = m_scene->GetRegistry();
    for (auto [e, mr] : reg.view<MeshRenderer>().each())
    {
        if (mr.modelPath.empty()) continue;
        // modelPath は「assets 相対」で保存されるが、D&D 配置直後は絶対パスのこともある。
        // GetOrLoadModel と同じ正規化を両方の綴りで試す(片方でも当たれば同じモデル)。
        const CachedModel* cm = nullptr;
        bool hit = false;
        {
            auto it = byKey.find(ResourceManager::NormalizeModelKey(mr.modelPath));
            if (it == byKey.end()) it = byKey.find(ResourceManager::NormalizeModelKey(assetsDir + mr.modelPath));
            if (it != byKey.end()) { cm = it->second; hit = true; }
        }
        if (!hit) continue;
        if (!cm)
        {
            // 読み直しに失敗した: 古い（外した）モデルを指したままにしない（描画で死んだポインタを踏む）
            mr.meshes.clear(); mr.materials.clear(); mr.vgAutoPath.clear();
            ++rebound;
            continue;
        }
        // ★ヒットしたら必ず張り替える。「アニメ付きだから見送る」は死んだポインタを残す＝次の描画で落ちるので絶対にやらない。
        mr.meshes.clear();
        mr.materials.clear();
        for (const auto& m : cm->meshes)    mr.meshes.push_back(m.get());
        for (const auto& m : cm->materials) mr.materials.push_back(m.get());
        mr.vgAutoPath = cm->vgAutoVgeo;
        mr.InvalidateMaterialRemap();
        if (!mr.meshNodeTransforms.empty() || cm->nodeGraph)
        {
            DirectX::XMFLOAT4X4 ident;
            DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
            mr.meshNodeTransforms.assign(mr.meshes.size(), ident);
        }
        ++rebound;
    }
    // BLAS キャッシュのキーは Mesh*。解放したアドレスへ新しいメッシュが載ると
    // 「ラスタは正しいのにレイトレの影/反射だけ古い形」になるので全部捨てる。
    if (m_rtScene) m_rtScene->Invalidate();
    // 植生のレイヤーも Mesh* / Material* を握っている（MeshRenderer と同じく張り替えないと GPU がハングする）
    if (m_foliage) m_foliage->InvalidateModels();
    return rebound;
}

void Application::ShutdownVirtualGeometry()
{
    if (m_vg) m_vg->Shutdown();
    m_vg.reset();
    if (m_vgHiZ) m_vgHiZ->Shutdown();
    m_vgHiZ.reset();
    m_vgHiZHistory = false;
    m_vgPrevWorld.clear();
}

void Application::RunVirtualGeometryCull(ID3D12GraphicsCommandList* cmd, const ViewDesc& view, u32 frameIndex,
                                         D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu, bool depthReady, const VgRasterIn* rasterIn)
{
    using namespace DirectX;
    const vg::VirtualGeometrySettings& st = m_scene->GetVirtualGeometrySettings();

    // ---- 遅延初期化（OFF の間は何も確保しない）----
    if (!m_vg)
    {
        auto sys = std::make_unique<vg::VirtualGeometrySystem>();
        vg::SystemDesc d;
        d.device        = m_graphicsDevice->GetDevice();
        d.directQueue   = m_commandQueue->GetQueue();
        d.shaderDir     = PathResolver::ShaderDirW();
        d.frameCount    = FrameResources::kFrameCount;
        d.vramBudgetMB  = static_cast<u32>(std::max(64, st.vramBudgetMB));
        // P4: VG のディスクリプタをアプリの SRV ヒープへ一本化する（resolve が材質テクスチャと VG のバッファを 1 本のヒープで引く。
        //   VG 設計書 §2.4.8(1)）。VG OFF の間はここに来ないので、ヒープの使用量は導入前と同じ。
        d.externalHeap.heap = m_srvHeap->GetHeap();
        d.externalHeap.allocBlock = [this](u32 n) { return m_srvHeap->AllocateBlock(n); };
        d.externalHeap.freeBlock  = [this](u32 b, u32 n) { m_srvHeap->FreeBlock(b, n); };
        std::string err;
        if (!sys->Initialize(d, &err))
        {
            Logger::Warn("仮想ジオメトリ: 初期化に失敗したので無効にする（{}）。プロキシは従来経路で描かれる", err);
            m_vgUnavailable = true;
            return;
        }
        m_vg = std::move(sys);
        Logger::Info("仮想ジオメトリ: GPU カリングを有効化（P3: メッシュシェーダ ラスタ {}）", m_vg->RasterSupported() ? "使用可" : "使用不可（プロキシを描く）");
    }
    m_vg->SetVramBudgetMB(static_cast<u32>(std::max(64, st.vramBudgetMB)));

    // 二相 HZB（前フレームの深度から作った物 → 二相目で今フレームの深度から作り直す）
    const u32 rW = view.width, rH = view.height;
    if (!m_vgHiZ)
    {
        m_vgHiZ = std::make_unique<HiZPass>();
        m_vgHiZ->Initialize(*m_graphicsDevice, m_srvHeap.get(), rW, rH, PathResolver::ShaderDirW());
        m_vgHiZHistory = false;
    }
    else if (m_vgHiZ->GetWidth() != rW || m_vgHiZ->GetHeight() != rH)
    {
        // 通常は ApplyRenderResolution（WaitIdle 済み）が作り直す。ここに来るのはその前に描いた場合だけ（古いピラミッドが飛行中かも）。
        m_commandQueue->WaitIdle();
        m_vgHiZ->Resize(*m_graphicsDevice, rW, rH);
        m_vgHiZHistory = false;          // 寸法が変わったら履歴は使えない
    }

    m_vg->Pump();                        // 非同期読込が終わったアセットを有効化
    m_vg->CollectStats(frameIndex);      // このスロットの前回の結果（GPU 完了後）
    VirtualGeometryBindMaterials(cmd);   // P4: 準備できたアセットの材質（テクスチャ）を材質表へ

    // ---- インスタンス収集 ----
    entt::registry& reg = m_scene->GetRegistry();
    std::vector<vg::VgInstanceInput> inst;
    std::unordered_map<std::string, u32> idCache;
    std::unordered_map<u32, XMFLOAT4X4> nextPrev;
    const auto isGraph = VgGraphPredicate();
    ForEachVgEntity(reg, st.enabled, [&](entt::entity e, const std::string& path, const MeshRenderer* mr, bool)
    {
        if (vg::VgIneligibleReason(reg, e, isGraph)) return;   // P4: 対象外はプロキシで描く（VirtualGeometryCollectProxyHide と同じ判定）

        u32 id;
        auto it = idCache.find(path);
        if (it != idCache.end()) id = it->second;
        else { id = m_vg->RequestAsset(ResolveVgPath(path)); idCache.emplace(path, id); }
        if (id == 0xFFFFFFFFu || !VgAssetDrawable(*m_vg, id)) return;

        XMFLOAT4X4 w;
        XMStoreFloat4x4(&w, ComputeWorldMatrix(reg, e));
        vg::VgInstanceInput in;
        std::memcpy(in.world, &w, 64);
        const u32 key = static_cast<u32>(entt::to_integral(e));
        auto pv = m_vgPrevWorld.find(key);
        if (pv != m_vgPrevWorld.end()) { std::memcpy(in.prevWorld, &pv->second, 64); in.hasPrev = true; }
        else { std::memcpy(in.prevWorld, &w, 64); in.hasPrev = false; }
        in.assetId = id;
        in.entityId = key;
        // P4: エンティティ単位の上書き（ティント / metallic・roughness / 自己発光）。フォワードの b2 と同じ詰め方・同じ優先度。
        if (mr)
        {
            if (mr->hasColorTint)
                in.packedTint = 0xFF000000u | (QuantizeUnorm8(mr->colorTint.x) << 16) | (QuantizeUnorm8(mr->colorTint.y) << 8)
                              | QuantizeUnorm8(mr->colorTint.z);
            in.overrideMetallic  = mr->overrideMetallic;
            in.overrideRoughness = mr->overrideRoughness;
            in.packedEmissive = 0;
            in.emissiveFlags  = 0;
            if (mr->overrideEmissiveColor.x >= 0.0f)
            {
                in.emissiveFlags |= VG_INST_EMIS_COLOR_OV;
                in.packedEmissive |= (QuantizeUnorm8(mr->overrideEmissiveColor.x) << 16) | (QuantizeUnorm8(mr->overrideEmissiveColor.y) << 8)
                                   | QuantizeUnorm8(mr->overrideEmissiveColor.z);
            }
            if (mr->overrideEmissiveIntensity >= 0.0f)
            {
                in.emissiveFlags |= VG_INST_EMIS_INT_OV;
                const f32 norm = std::sqrt(std::clamp(mr->overrideEmissiveIntensity, 0.0f, kEmissiveIntensityMax) / kEmissiveIntensityMax);
                in.packedEmissive |= QuantizeUnorm8(norm) << 24;
            }
            // エンティティ単位の材質（.dxmat / テクスチャ上書き / materialRemap / AO・拡張シェーディングの上書き）
            in.materialBase = VgEntityMaterialBase(e, *mr, id, cmd);
        }
        inst.push_back(in);
        nextPrev.emplace(key, w);
    });
    m_vg->EndKeyedMaterialFrame();   // 上書きが外れた / 消えたエンティティの材質区間を返す
    m_vgPrevWorld = std::move(nextPrev);

    // ---- 実行 ----
    vg::ExecuteDesc d;
    d.cmd = cmd;
    d.frameSlot = frameIndex % FrameResources::kFrameCount;
    std::memcpy(d.viewProj, &view.viewProjJittered, 64);
    std::memcpy(d.prevViewProj, m_vgHiZHistory ? &m_vgPrevViewProj : &view.viewProjJittered, 64);
    d.camPos[0] = view.position.x; d.camPos[1] = view.position.y; d.camPos[2] = view.position.z;
    d.zNear = view.nearZ;
    // 0.5 * H / tan(fovY / 2)（proj._22 = cot(fovY/2)）。H は出力解像度（DLSS で内部 720p → 出力 1080p なら 1080。VgLodMath.h）
    //   既定は内部解像度の px（lodOutputPixels = false）。
    const u32 lodH = st.lodOutputPixels ? std::max<u32>(rH, m_postH) : rH;
    m_vgLodHeight = lodH;
    d.projScale = vg::LodProjScale(static_cast<f32>(rH), static_cast<f32>(lodH), view.proj._22);
    d.vpX = 0.0f; d.vpY = 0.0f; d.vpW = static_cast<f32>(rW); d.vpH = static_cast<f32>(rH);
    d.instances = inst.data();
    d.instanceCount = static_cast<u32>(inst.size());
    d.settings.tauPx = st.lodPixelError;
    d.settings.hzbCulling = st.hzbCulling && depthReady;
    d.settings.coneCulling = st.coneCulling;
    d.settings.instanceMinPx = st.instanceMinPx;
    d.settings.forceLod0 = st.forceLod0;
    d.settings.hzbExactSphere = st.hzbExactSphere;
    // P4: 決定論（可視リストの安定ソート）。決定論キャプチャ（screenshot_final deterministic）の間は自動で ON。
    d.settings.stableOrder = st.stableOrder || m_deterministicCapture;
    // P4: G-Buffer の速度はジッタ無しの今 / 前フレームの VP（深度プリパスと同じ。履歴が無ければ今と同じ = 速度 0）
    std::memcpy(d.viewProjNJ, &view.viewProj, 64);
    std::memcpy(d.prevViewProjNJ, m_prevViewProjNJValid ? &m_prevViewProjNoJitter : &view.viewProj, 64);
    d.hasVelocityVp = true;
    d.restoreHeap = m_srvHeap->GetHeap();
    // ---- P3: ラスタ（可視性バッファ + 深度）----
    const bool rasterOn = rasterIn && rasterIn->active && m_vg->RasterSupported();
    if (rasterOn)
    {
        const bool dbgOverdraw = m_renderDebugMode == static_cast<u32>(RenderDebugMode::VgOverdraw);
        d.raster.enabled = true;
        d.raster.width = rW; d.raster.height = rH;
        d.raster.depth = rasterIn->depth;
        d.raster.dsv = rasterIn->dsv;
        d.raster.depthToRaster = rasterIn->toRaster;
        d.raster.depthToSample = rasterIn->toSample;
        d.raster.useAs = st.rasterAs;
        d.raster.smallPrimCull = st.smallPrimCull;
        d.raster.measure = st.measure || dbgOverdraw;
        d.raster.overdrawImage = dbgOverdraw;
        d.raster.edgeHist = st.measure;
    }
    if (d.settings.hzbCulling && m_vgHiZ->IsReady())
    {
        d.hzb.prev = m_vgHiZ->GetResource();
        d.hzb.prevValid = m_vgHiZHistory;
        d.hzb.cur = m_vgHiZ->GetResource();
        d.hzb.curValid = false;                       // Build のコールバックが作る
        d.hzb.width = m_vgHiZ->GetWidth();
        d.hzb.height = m_vgHiZ->GetHeight();
        d.hzb.mips = m_vgHiZ->GetMipCount();
        d.hzb.buildBetweenPhases = [this, depthSrvGpu](ID3D12GraphicsCommandList* c)
        {
            ID3D12DescriptorHeap* heaps[] = {m_srvHeap->GetHeap()};   // HiZPass はアプリの SRV ヒープのテーブルを使う
            c->SetDescriptorHeaps(1, heaps);
            m_vgHiZ->Build(c, depthSrvGpu);
        };
        // ラスタあり: 二相目のラスタの後にもう 1 度作り直す（VG 自身の深度が入った最終深度 = 次フレームの一相目の入力）
        if (rasterOn) d.hzb.buildFinal = d.hzb.buildBetweenPhases;
    }
    m_vg->Execute(d);

    // 次フレームの一相目の入力（この HZB = 今フレームの深度）
    m_vgHiZHistory = d.settings.hzbCulling && m_vgHiZ->IsReady();
    std::memcpy(&m_vgPrevViewProj, &view.viewProjJittered, 64);
}

nlohmann::json Application::VirtualGeometryStatsJson() const
{
    using nlohmann::json;
    const vg::VirtualGeometrySettings st = m_scene ? m_scene->GetVirtualGeometrySettings() : vg::VirtualGeometrySettings{};
    json j;
    j["enabled"] = st.enabled;
    j["gpuSupported"] = m_graphicsDevice ? m_graphicsDevice->SupportsDynamicResources() : false;
    j["available"] = !m_vgUnavailable;
    j["active"] = (m_vg != nullptr) && st.enabled;
    j["settings"] = {{"lodPixelError", st.lodPixelError}, {"hzbCulling", st.hzbCulling}, {"coneCulling", st.coneCulling},
                     {"instanceMinPx", st.instanceMinPx}, {"vramBudgetMB", st.vramBudgetMB},
                     {"raster", st.raster}, {"rasterAs", st.rasterAs}, {"smallPrimCull", st.smallPrimCull},
                     {"measure", st.measure}, {"forceLod0", st.forceLod0},
                     {"resolve", st.resolve}, {"stableOrder", st.stableOrder}, {"hzbExactSphere", st.hzbExactSphere},
                     {"lodOutputPixels", st.lodOutputPixels}};
    j["meshShaders"] = m_graphicsDevice ? m_graphicsDevice->SupportsMeshShaders() : false;
    j["rasterWanted"] = VirtualGeometryRasterWanted(true);
    j["hiddenProxies"] = m_vgHiddenEntities.size();     // VG 本体が描くためプロキシを外しているエンティティ数
    j["note"] = "P4: メッシュシェーダ対応 GPU では VG 本体が可視性バッファ + 深度へ描かれ、材質 resolve（フォワードと同じライティング）で"
                "色が付く。プロキシは主ビューの深度プリパス / フォワードから外れる（影 / TLAS / ピッキング / 物理はプロキシ）。"
                "対象外（スキンド / 半透明 / カスタムシェーダー / マテリアルグラフ …）はプロキシで描く（ineligible）。.dxmat・テクスチャ上書き・"
                "materialRemap は VG で描く。ハイポリの glb / fbx / obj は読み込み時に自動 cook（autoCook）。統計は 1〜2 フレーム遅れ";
    // P4: VG の対象外（プロキシで描く）エンティティと理由（先頭 16 件）
    if (m_scene)
    {
        entt::registry& reg = const_cast<entt::registry&>(m_scene->GetRegistry());   // ForEachVgEntity は読むだけ
        json inel = json::array();
        u32 inelCount = 0, autoCount = 0;
        const auto isGraph = VgGraphPredicate();
        ForEachVgEntity(reg, st.enabled, [&](entt::entity e, const std::string&, const MeshRenderer*, bool isAuto)
        {
            if (isAuto) ++autoCount;
            const char* why = vg::VgIneligibleReason(reg, e, isGraph);
            if (!why) return;
            ++inelCount;
            if (inel.size() < 16)
            {
                const NameTag* nt = reg.try_get<NameTag>(e);
                inel.push_back({{"entity", static_cast<u32>(entt::to_integral(e))}, {"name", nt ? nt->name : std::string()}, {"reason", why},
                                {"autoCook", isAuto}});
            }
        });
        j["ineligible"] = {{"count", inelCount}, {"entities", inel}};
        j["autoEntities"] = autoCount;     // 自動 cook（glb / fbx / obj を置いただけ）で VG に乗っているエンティティ数
        // 通常メッシュのまま描かれている「cook 中」「対象外」の理由（自動 cook の判定）
        j["autoCook"] = vgauto::Cooker::Get().StatusJson();
    }
    if (!m_vg) return j;

    const vg::FrameStats& s = m_vg->GetStats();
    auto r3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    j["valid"] = s.valid;
    j["instances"] = s.instances;
    j["instancesInFrustum"] = s.instancesFrustum;
    j["sourceTrisInFrustum"] = s.sourceTrianglesInFrustum;
    j["nodesVisited"] = s.nodesVisited;
    j["clustersTested"] = s.clustersTested;
    j["clustersSelected"] = s.clustersSelected;      // LOD 選択（錐台 / コーン / HZB の前）
    j["visibleClusters"] = s.visibleClusters;
    j["phase2Clusters"] = s.phase2Clusters;          // 二相目（今フレームの HZB）で救われた数
    j["trianglesDrawn"] = s.trianglesDrawn;          // 見かけの三角形数（可視クラスタの合計）
    j["cullGpuMs"] = r3(s.cullGpuMs);              // Execute からラスタを除いた GPU 時間（カリング + HZB 再構築）
    j["executeGpuMs"] = r3(s.executeGpuMs);        // Execute 全体（カリング + ラスタ + HZB 再構築）
    j["raster"] = {
        {"active", s.rasterActive}, {"usedAs", s.rasterUsedAs}, {"supported", m_vg->RasterSupported()},
        {"gpuMs", r3(s.rasterGpuMs)}, {"gpuMsPhase1", r3(s.rasterPhaseGpuMs[0])}, {"gpuMsPhase2", r3(s.rasterPhaseGpuMs[1])},
        {"clusters", s.rasterClusters}, {"trianglesDrawn", s.trianglesDrawn},
        {"trianglesHw", s.trianglesDrawn}, {"trianglesSw", 0},          // SW ラスタは未実装（P3b。実測ゲートの判定は VG_P3_REPORT.md）
        {"clustersHw", s.rasterClusters}, {"clustersSw", 0}};
    if (s.rasterActive && (st.measure || m_renderDebugMode == static_cast<u32>(RenderDebugMode::VgOverdraw)))
    {
        json& r = j["raster"];
        r["asCulledClusters"] = s.asCulled;
        r["msTrianglesOut"] = s.msTrisOut;
        r["msSmallPrimCulled"] = s.msPrimCulled;
        r["psInvocations"] = s.psInvocations;
        r["coveredPixels"] = s.coveredPixels;
        r["overdraw"] = s.coveredPixels ? r3(static_cast<double>(s.psInvocations) / static_cast<double>(s.coveredPixels)) : 0.0;
        // 描いた三角形 / 被覆画素（画素あたりの三角形数）と、1 画素でも残ったクラスタの割合（残らない = 隠れ / 裏面で無駄に描いた）
        r["trianglesPerCoveredPixel"] = s.coveredPixels ? r3(static_cast<double>(s.trianglesDrawn) / static_cast<double>(s.coveredPixels)) : 0.0;
        r["clustersWithPixels"] = s.clustersWithPixels;
        r["trianglesInClustersWithPixels"] = s.trisInClustersWithPixels;
    }
    if (s.rasterActive && st.measure)
    {
        json ec = json::array(), et = json::array();
        for (u32 i = 0; i < VG_EDGE_BINS; ++i) { ec.push_back(s.edgeClusters[i]); et.push_back(s.edgeTris[i]); }
        // 可視クラスタの最長辺（画面 px）: [0,1) [1,2) [2,4) [4,8) [8,16) [16,32) [32,64) [64,inf)
        j["raster"]["edgePxHistogram"] = {{"bins", json::array({"<1", "1-2", "2-4", "4-8", "8-16", "16-32", "32-64", ">=64"})},
                                          {"clusters", ec}, {"triangles", et}};
    }
    // P4: G-Buffer（H2）/ 材質 resolve（H3）/ 決定論
    {
        u32 bound = 0, total = 0;
        const u32 na = m_vg->AssetCount();
        for (u32 i = 0; i < na; ++i)
        {
            vg::AssetInfo a;
            if (!m_vg->GetAssetInfo(i, a) || !a.ready) continue;
            ++total;
            if (a.materialsBound || a.materialCount == 0) ++bound;
        }
        j["resolve"] = {
            {"supported", m_vg->ResolveSupported()}, {"active", s.resolveActive}, {"gpuMs", r3(s.resolveGpuMs)},
            {"gbufferActive", s.gbufferActive}, {"gbufferGpuMs", r3(s.gbufferGpuMs)},
            {"mainRsBindless", m_rootSignature ? m_rootSignature->IsBindless() : false},
            {"assetsWithMaterials", bound}, {"assetsReady", total},
            {"appHeapDescriptors", m_vg->DescriptorsInUse()}, {"sharedAppHeap", m_vg->UsesExternalHeap()}};
        j["stableOrder"] = {{"active", s.stableOrder}, {"sortedClusters", s.sortedClusters}, {"skippedPhases", s.sortSkipped}};
        // VG の GPU 合計（カリング + ラスタ + HZB 再構築 + G-Buffer + resolve）
        j["vgGpuTotalMs"] = r3(s.executeGpuMs + s.gbufferGpuMs + s.resolveGpuMs);
    }
    j["vramMB"] = r3(static_cast<double>(s.vramBytes) / 1048576.0);
    j["overflow"] = s.overflow;
    j["tauUsed"] = r3(s.tauUsed);
    j["lodPixelHeight"] = m_vgLodHeight;   // τ を測る画面の高さ（px）。アップスケール時は出力解像度（内部解像度ではない）
    j["culled"] = {{"clustersFrustum", s.clustersCulledFrustum}, {"clustersCone", s.clustersCulledCone},
                   {"clustersHzbDeferred", s.clustersDeferred}, {"clustersHzbDropped", s.clustersHzbDropped},
                   {"nodesFrustum", s.nodesCulledFrustum}, {"nodesLod", s.nodesCulledLod}, {"nodesHzbDeferred", s.nodesDeferred},
                   {"instancesHzbDeferred", s.instancesHzbRejected}};
    j["overflowDetail"] = {{"nodeQueue", s.overflowNode}, {"groupQueue", s.overflowGroup}, {"visible", s.overflowVisible},
                           {"deferred", s.overflowDeferred}, {"instancesDropped", s.instancesDropped}};
    json hist = json::array();
    u32 last = 0;
    for (u32 i = 0; i < VG_HIST_LEVELS; ++i) if (s.levelHistogram[i]) last = i;
    for (u32 i = 0; i <= last; ++i) hist.push_back(s.levelHistogram[i]);
    j["levelHistogram"] = hist;
    j["hzb"] = {{"history", m_vgHiZHistory},
                {"width", m_vgHiZ ? m_vgHiZ->GetWidth() : 0}, {"height", m_vgHiZ ? m_vgHiZ->GetHeight() : 0},
                {"mips", m_vgHiZ ? m_vgHiZ->GetMipCount() : 0}};
    json assets = json::array();
    const u32 n = m_vg->AssetCount();
    for (u32 i = 0; i < n; ++i)
    {
        vg::AssetInfo a;
        if (!m_vg->GetAssetInfo(i, a)) continue;
        json aj = {{"id", a.id}, {"path", a.path}, {"ready", a.ready}, {"failed", a.failed}};
        if (a.ready)
        {
            aj["pages"] = a.pageCount; aj["clusters"] = a.clusterCount; aj["levels"] = a.levelCount; aj["bvhDepth"] = a.bvhDepth;
            aj["sourceTriangles"] = a.sourceTriangles;
            aj["vramMB"] = r3(static_cast<double>(a.vramBytes) / 1048576.0);
        }
        if (a.failed) aj["error"] = a.error.ToString();
        assets.push_back(aj);
    }
    j["assets"] = assets;
    j["pendingLoads"] = m_vg->PendingLoads();
    j["entityMaterials"] = {{"entities", m_vg->KeyedMaterialCount()}, {"slots", m_vg->InstanceMaterialSlots()}};   // エンティティ単位の材質区間
    return j;
}

} // namespace dx12e
