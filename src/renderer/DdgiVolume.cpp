#include "renderer/DdgiVolume.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/DeferredRelease.h"

#include <algorithm>
#include <cstring>

using namespace DirectX;

namespace dx12e
{
namespace
{

// shaders/ddgi/DdgiProbeUpdate.hlsl の cbuffer DdgiCB と完全一致させること。
struct DdgiCB
{
    // DdgiConstants
    XMFLOAT3 originWS;   float rayLength;
    XMFLOAT3 spacing;    float hysteresis;
    u32      probeCountX, probeCountY, probeCountZ, frameIndex;
    float    intensity, normalBias, bounceIntensity, pad1;
    // 光源
    XMFLOAT3 sunDir;     float sunIntensity;
    XMFLOAT3 sunColor;   float pad2;
    XMFLOAT3 skyColor;   u32   skyCubeIndex;
    // 前フレームの irradiance / 距離アトラスを TraceCS が SRV として引くための
    // bindless index（段階3）。0xFFFFFFFF = 履歴が無い＝バウンスを丸ごとスキップ。
    // ★テクスチャは CreateCommittedResource で確保するだけでクリアしていないので、
    //   初回フレームに読むと未初期化メモリ（NaN もあり得る）を拾う。ガードは必須。
    u32      lightSrvIndex, lightCount, prevIrradianceSrv, prevDistanceSrv;
    // ---- GI モード New（giFlags bit0）だけが読む。Legacy は 0 埋めのまま＝シェーダは従来経路 ----
    u32      giFlags;          // bit0 = New / bit1 = 履歴リセット（今フレームは probeData を「オフセット 0・有効」とみなす）
    u32      envCubeIndex;     // 空の放射輝度キューブ（0xFFFFFFFF = 無し）
    float    skyScale;         // ミスの放射輝度に掛ける（iblIntensity）
    float    minFrontDist;     // 再配置: 面からこれ未満に近いプローブは離す（m）
    float    viewBias;         // 補間の surface bias（視線方向。m）
    float    normalBiasNew;    // 補間の surface bias（法線方向。m）
    float    relocLimit;       // 再配置オフセットの上限（格子間隔に対する比。RTXGI の 0.45）
    float    unitScale;        // 物理ライティング単位 → 内部の単位（従来単位）への倍率。従来単位なら 1
    // GI S4（giFlags bit6 = 更新リスト方式）: 更新計画バッファの bindless index。他の窓の情報はこのバッファのヘッダにある。
    u32      planSrvIndex;
    u32      pad3[3];
};
static_assert(sizeof(DdgiCB) % 16 == 0, "DdgiCB は 16B 境界に揃えること");
constexpr u32 kDdgiCBNum32 = sizeof(DdgiCB) / sizeof(u32);

// irradiance アトラスのサイズ。プローブは (X*Y) 列 × Z 行 に並べる。
void AtlasSize(u32 px, u32 py, u32 pz, u32 tile, u32& w, u32& h)
{
    w = px * py * tile;
    h = pz * tile;
}

} // namespace

bool DdgiVolume::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                            const std::wstring& shaderDir)
{
    m_srvHeap   = srvHeap;
    m_shaderDir = shaderDir;
    // ★DDGI は inline RayQuery とバインドレスの両方を使う。どちらか欠けたら諦める。
    if (!device.SupportsInlineRaytracing() || !device.SupportsDynamicResources())
    {
        Logger::Info("DDGI: 非対応（inline RT か Dynamic Resources が無い）。無効のまま続行します");
        return false;
    }
    CreateRootSignature(device);
    RecreatePipelines(device);
    return IsReady();
}

void DdgiVolume::Shutdown()
{
    if (m_srvHeap)
    {
        if (m_rayDataUav    != 0xFFFFFFFFu) m_srvHeap->FreeBlock(m_rayDataUav, 5);
        for (u32 f = 0; f < kPlanRing; ++f)
            if (m_planSrv[f] != 0xFFFFFFFFu) m_srvHeap->Free(m_planSrv[f]);
        if (m_irradianceSrv != 0xFFFFFFFFu) m_srvHeap->Free(m_irradianceSrv);
        if (m_distanceSrv   != 0xFFFFFFFFu) m_srvHeap->Free(m_distanceSrv);
        if (m_probeDataSrv  != 0xFFFFFFFFu) m_srvHeap->Free(m_probeDataSrv);
        for (const Retired& r : m_retired)   // 退役中のぶんも返す（Shutdown は GPU 停止後）
        {
            if (r.uavBlock != 0xFFFFFFFFu) m_srvHeap->FreeBlock(r.uavBlock, 5);
            for (u32 i : r.srv) if (i != 0xFFFFFFFFu) m_srvHeap->Free(i);
        }
    }
    for (u32 f = 0; f < kPlanRing; ++f)
    {
        if (m_planBuf[f] && m_planMapped[f]) m_planBuf[f]->Unmap(0, nullptr);
        m_planMapped[f] = nullptr;
        m_planBuf[f].Reset();
        m_planSrv[f] = 0xFFFFFFFFu;
    }
    m_lightGrid.Reset();
    m_lightGridUav = 0xFFFFFFFFu;
    m_retired.clear();
    m_rayDataUav = m_irradianceUav = m_distanceUav = m_probeDataUav = 0xFFFFFFFFu;
    m_irradianceSrv = m_distanceSrv = m_probeDataSrv = 0xFFFFFFFFu;
    m_rayData.Reset();
    m_irradiance.Reset();
    m_distance.Reset();
    m_probeData.Reset();
    m_psoProbeData.Reset();
    m_psoLightGrid.Reset();
    m_psoTrace.Reset();
    m_psoBlend.Reset();
    m_psoBlendDist.Reset();
    m_rootSig.Reset();
    m_probesX = m_probesY = m_probesZ = 0;
    m_layoutCascades = 0;
    m_historyValid = false;
    m_planKey = PlanKey{};
    m_planReady = false;
}

void DdgiVolume::CreateRootSignature(GraphicsDevice& device)
{
    // b0(32bit定数) + t0(TLAS root SRV) + t1(GeometryInfo root SRV) + u0..u4(UAV テーブル)
    // DWORD: kDdgiCBNum32(44) + t0(2) + t1(2) + テーブル(1) = 49/64。
    // 専用ルートシグネチャなので PBR の 61/64 とは無関係。
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kDdgiCBNum32;

    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t0 = TLAS
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t1 = GeometryInfo
    params[2].Descriptor.ShaderRegister = 1;

    // ★u0/u1 は Texture2D なのでルート UAV にできない（ルート記述子はバッファ専用）。
    //   連続 2 スロットのディスクリプタテーブルにする。
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 5;   // u0 = RayData / u1 = Irradiance / u2 = Distance / u3 = ProbeData / u4 = LightGrid
    uavRange.BaseShaderRegister = 0;
    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &uavRange;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD         = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = _countof(params);
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &samp;
    // ★バインドレス（ResourceDescriptorHeap[]）を使うので必須。
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &serialized, &error);
    if (FAILED(hr))
    {
        if (error) Logger::Error("DDGI のルートシグネチャ: {}",
                                 static_cast<const char*>(error->GetBufferPointer()));
        return;
    }
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
}

void DdgiVolume::RecreatePipelines(GraphicsDevice& device)
{
    if (!m_rootSig) return;
    auto make = [&](const wchar_t* name, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + name);
        if (bc.GetSize() == 0) { out.Reset(); return; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        if (FAILED(device.GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out))))
        {
            Logger::Warn("DDGI: compute の PSO を作れませんでした");
            out.Reset();
        }
    };
    make(L"DdgiTrace_CS.cso", m_psoTrace);
    make(L"DdgiBlend_CS.cso", m_psoBlend);
    make(L"DdgiBlendDist_CS.cso", m_psoBlendDist);
    make(L"DdgiProbeData_CS.cso", m_psoProbeData);
    make(L"DdgiLightGrid_CS.cso", m_psoLightGrid);
}

bool DdgiVolume::MatchesGrid(const DdgiSettings& s) const
{
    const u32 px = static_cast<u32>(std::clamp(s.probeCountX, 1, 32));
    const u32 py = static_cast<u32>(std::clamp(s.probeCountY, 1, 32));
    const u32 pz = static_cast<u32>(std::clamp(s.probeCountZ, 1, 32));
    return m_rayData && m_irradiance && m_distance && m_probeData
        && m_probesX == px && m_probesY == py && m_probesZ == pz
        && m_layoutCascades == m_planCascades;
}

// 作り直しの直前に呼ぶ。
// ★TDR の原因だった（GI S2/S3 で確定）: 格子が変わるとここでテクスチャを作り直すが、
//   (1) 旧テクスチャを ComPtr 代入で即解放していた＝直前の 1〜2 フレームがまだ GPU で走っている
//   (2) 同じ index へ新テクスチャのディスクリプタを上書きしていた＝走行中のフレームが別物を引く
//   (3) フォワードの b1 / t22 の書き込み（FillSceneFrameConstants）は Update より前に済んでおり、
//       旧アトラスを指したまま。ここで旧アトラスが消えると同じフレームの PS が解放済みを読む
//   のどれも未定義動作で、デバイス ハングになる。(1)(2) はここで、(3) は MatchesGrid（フォワードが
//   対応していない間は DDGI を読まない）で防ぐ。
void DdgiVolume::RetireOldResources()
{
    auto defer = [](Microsoft::WRL::ComPtr<ID3D12Resource>& r)
    {
        if (r) DeferredRelease::Defer(std::move(r), nullptr);
        r.Reset();
    };
    defer(m_rayData);
    defer(m_irradiance);
    defer(m_distance);
    defer(m_probeData);
    defer(m_lightGrid);
    if (m_rayDataUav != 0xFFFFFFFFu || m_irradianceSrv != 0xFFFFFFFFu)
    {
        Retired r;
        r.uavBlock = m_rayDataUav;
        r.srv[0]   = m_irradianceSrv;
        r.srv[1]   = m_distanceSrv;
        r.srv[2]   = m_probeDataSrv;
        m_retired.push_back(r);
    }
    m_rayDataUav = m_irradianceUav = m_distanceUav = m_probeDataUav = m_lightGridUav = 0xFFFFFFFFu;
    m_irradianceSrv = m_distanceSrv = m_probeDataSrv = 0xFFFFFFFFu;
}

// 退役ディスクリプタは「GPU が先行しうる最大フレーム数」より十分あとに返す（再利用されても安全）。
void DdgiVolume::TickRetired()
{
    constexpr u32 kRetireFrames = 8;
    for (auto it = m_retired.begin(); it != m_retired.end();)
    {
        if (++it->age < kRetireFrames) { ++it; continue; }
        if (m_srvHeap)
        {
            if (it->uavBlock != 0xFFFFFFFFu) m_srvHeap->FreeBlock(it->uavBlock, 5);
            for (u32 i : it->srv) if (i != 0xFFFFFFFFu) m_srvHeap->Free(i);
        }
        it = m_retired.erase(it);
    }
}

bool DdgiVolume::EnsureResources(GraphicsDevice& device, const DdgiSettings& s, u32 cascades)
{
    const u32 px = static_cast<u32>(std::clamp(s.probeCountX, 1, 32));
    const u32 py = static_cast<u32>(std::clamp(s.probeCountY, 1, 32));
    const u32 pz = static_cast<u32>(std::clamp(s.probeCountZ, 1, 32));
    const u32 total = px * py * pz;
    if (total == 0 || total > kMaxProbes) return false;
    cascades = std::clamp(cascades, 1u, 2u);
    // ★MatchesGrid は m_planCascades（PlanFrame の結果）と比べる。Update が別の値で呼ぶことは無いが念のため揃える。
    if (m_rayData && m_irradiance && m_distance && m_probeData
        && m_probesX == px && m_probesY == py && m_probesZ == pz && m_layoutCascades == cascades)
        return true;

    auto* dev = device.GetDevice();
    auto makeTex = [&](u32 w, u32 h, DXGI_FORMAT fmt,
                       Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = w;
        desc.Height           = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = fmt;
        desc.SampleDesc       = {1, 0};
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };

    // ★カスケードはアトラスの「行」に積む（両カスケードの格子数は同じ）。プローブの通し番号 = カスケード * total + 記憶領域の番号。
    const u32 allProbes = total * cascades;
    u32 aw = 0, ah = 0, dw = 0, dh = 0;
    AtlasSize(px, py, pz * cascades, kProbeTile,    aw, ah);
    AtlasSize(px, py, pz * cascades, kDistanceTile, dw, dh);
    Microsoft::WRL::ComPtr<ID3D12Resource> rayData, irradiance, distance, probeData, lightGrid;
    // ライトグリッド: セルごとに [個数, 灯の添字 × kLightGridMaxPerCell]。窓は最大 (格子数 + 3) セル四方。
    const u64 lgCells = static_cast<u64>(px + 3) * (py + 3) * (pz + 3);
    const u64 lgWords = lgCells * (1 + kLightGridMaxPerCell);
    bool ok = true;
    // ★距離は 2 成分（平均 / 二乗平均）なので RG16F で足りる。RGBA16F にすると
    //   14x14 タイルは irradiance の 4 倍の面積があるので VRAM が倍要る。
    ok = ok && makeTex(kRaysPerProbe, allProbes, DXGI_FORMAT_R16G16B16A16_FLOAT, rayData);
    ok = ok && makeTex(aw, ah,                   DXGI_FORMAT_R16G16B16A16_FLOAT, irradiance);
    ok = ok && makeTex(dw, dh,                   DXGI_FORMAT_R16G16_FLOAT,       distance);
    ok = ok && makeTex(px * py, pz * cascades,   DXGI_FORMAT_R16G16B16A16_FLOAT, probeData);   // 1 テクセル = 1 プローブ
    if (ok)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = lgWords * sizeof(u32);
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ok = SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&lightGrid)));
    }
    if (!ok)
    {
        Logger::Warn("DDGI: プローブ用テクスチャを確保できません（{} プローブ）", allProbes);
        return false;
    }
    // 旧リソース / ディスクリプタは GPU が使い終わるまで預ける（TDR 対策。RetireOldResources のコメント参照）。
    RetireOldResources();
    TickRetired();
    m_rayData    = std::move(rayData);
    m_irradiance = std::move(irradiance);
    m_distance   = std::move(distance);
    m_probeData  = std::move(probeData);
    m_lightGrid  = std::move(lightGrid);
    m_lightGridCapCells = static_cast<u32>(lgCells);
    // 作りたては全部 UNORDERED_ACCESS。作り直したら追跡中のステートも戻す。
    m_irradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_distanceState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_probeDataState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // ディスクリプタは作り直しのたびに新しい index を取る（旧 index は退役中＝走行中のフレームが引き続き読める）。
    // ★u0..u4 はディスクリプタテーブルなので**連続**でなければならない。
    m_rayDataUav    = m_srvHeap->AllocateBlock(5);
    m_irradianceUav = m_rayDataUav + 1;
    m_distanceUav   = m_rayDataUav + 2;
    m_probeDataUav  = m_rayDataUav + 3;
    m_lightGridUav  = m_rayDataUav + 4;
    m_irradianceSrv = m_srvHeap->AllocateIndex();
    m_distanceSrv   = m_srvHeap->AllocateIndex();
    m_probeDataSrv  = m_srvHeap->AllocateIndex();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(m_rayData.Get(), nullptr, &uav,
                                   m_srvHeap->GetCpuHandle(m_rayDataUav));
    dev->CreateUnorderedAccessView(m_irradiance.Get(), nullptr, &uav,
                                   m_srvHeap->GetCpuHandle(m_irradianceUav));
    dev->CreateUnorderedAccessView(m_probeData.Get(), nullptr, &uav,
                                   m_srvHeap->GetCpuHandle(m_probeDataUav));
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;   // ★距離だけフォーマットが違う
    dev->CreateUnorderedAccessView(m_distance.Get(), nullptr, &uav,
                                   m_srvHeap->GetCpuHandle(m_distanceUav));
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC bu{};
        bu.Format                      = DXGI_FORMAT_UNKNOWN;
        bu.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
        bu.Buffer.FirstElement         = 0;
        bu.Buffer.NumElements          = static_cast<UINT>(lgWords);
        bu.Buffer.StructureByteStride  = sizeof(u32);
        dev->CreateUnorderedAccessView(m_lightGrid.Get(), nullptr, &bu,
                                       m_srvHeap->GetCpuHandle(m_lightGridUav));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    dev->CreateShaderResourceView(m_irradiance.Get(), &srv,
                                  m_srvHeap->GetCpuHandle(m_irradianceSrv));
    dev->CreateShaderResourceView(m_probeData.Get(), &srv,
                                  m_srvHeap->GetCpuHandle(m_probeDataSrv));
    srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    dev->CreateShaderResourceView(m_distance.Get(), &srv,
                                  m_srvHeap->GetCpuHandle(m_distanceSrv));

    m_probesX = px; m_probesY = py; m_probesZ = pz;
    m_layoutCascades = cascades;
    m_historyValid = false;   // 格子が変わった＝履歴の意味が変わる
    m_justRecreated = true;
    m_stats.probes = allProbes;
    m_stats.cascades = cascades;
    m_stats.bytes  = static_cast<u64>(kRaysPerProbe) * allProbes * 8   // RayData  RGBA16F
                   + static_cast<u64>(aw) * ah * 8                  // irradiance RGBA16F
                   + static_cast<u64>(dw) * dh * 4                  // distance   RG16F
                   + static_cast<u64>(allProbes) * 8                // probeData  RGBA16F
                   + lgWords * 4;                                   // ライトグリッド
    Logger::Info("DDGI: プローブ {}x{}x{} x {} カスケード = {} 個 / irradiance {}x{} / 距離 {}x{} / {:.2f} MB",
                 px, py, pz, cascades, allProbes, aw, ah, dw, dh,
                 static_cast<double>(m_stats.bytes) / (1024.0 * 1024.0));
    return true;
}

// 更新計画バッファ（UPLOAD。ヘッダ + 更新リスト）を飛行中のフレーム数ぶん作る。格子に依らないので 1 回だけ。
bool DdgiVolume::EnsurePlanBuffers(GraphicsDevice& device)
{
    if (m_planBuf[0] && m_planMapped[0]) return true;
    auto* dev = device.GetDevice();
    const u64 bytes = static_cast<u64>(kPlanHeaderWords + kMaxListEntries) * sizeof(u32);
    for (u32 f = 0; f < kPlanRing; ++f)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = bytes;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_planBuf[f]))))
        {
            Logger::Warn("DDGI: 更新計画バッファを確保できません");
            return false;
        }
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (FAILED(m_planBuf[f]->Map(0, &readRange, &mapped))) return false;
        m_planMapped[f] = static_cast<u8*>(mapped);
        std::memset(m_planMapped[f], 0, static_cast<size_t>(bytes));
        m_planSrv[f] = m_srvHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format                  = DXGI_FORMAT_UNKNOWN;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement     = 0;
        srv.Buffer.NumElements      = kPlanHeaderWords + kMaxListEntries;
        srv.Buffer.StructureByteStride = sizeof(u32);
        dev->CreateShaderResourceView(m_planBuf[f].Get(), &srv, m_srvHeap->GetCpuHandle(m_planSrv[f]));
    }
    return true;
}

bool DdgiVolume::WriteProbeDataSrv(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    if (!m_probeData || dst.ptr == 0) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device.GetDevice()->CreateShaderResourceView(m_probeData.Get(), &srv, dst);
    return true;
}

u32 DdgiVolume::GetIrradianceSrvIndex() const
{
    return m_irradiance ? m_irradianceSrv : 0xFFFFFFFFu;
}

u32 DdgiVolume::GetDistanceSrvIndex() const
{
    return m_distance ? m_distanceSrv : 0xFFFFFFFFu;
}

bool DdgiVolume::WriteIrradianceSrv(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    if (!m_irradiance || dst.ptr == 0) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device.GetDevice()->CreateShaderResourceView(m_irradiance.Get(), &srv, dst);
    return true;
}

bool DdgiVolume::WriteDistanceSrv(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    if (!m_distance || dst.ptr == 0) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device.GetDevice()->CreateShaderResourceView(m_distance.Get(), &srv, dst);
    return true;
}

// ---------------------------------------------------------------------------
//  GI S4: 窓の追従 + 更新リスト
// ---------------------------------------------------------------------------
void DdgiVolume::BuildList(const DdgiSettings& s)
{
    const u32 perCasc = static_cast<u32>(m_counts[0]) * m_counts[1] * m_counts[2];
    const u32 nc      = m_planCascades;
    const u32 total   = perCasc * nc;
    m_list.clear();
    m_listReset = 0;
    if (m_pending.size() != total) return;

    // 1) リセット待ちのプローブは必ず入れる（スクロールで入ってきた列 / 設定変更 / 履歴の破棄）
    for (u32 i = 0; i < total; ++i)
        if (m_pending[i]) { m_list.push_back(i | ddgi::kResetBit); ++m_listReset; }

    // 2) 予算ぶんのラウンドロビン（近いカスケードを kNearWeight 倍の頻度で）
    const u32 budget = m_budget == 0 ? total : std::min(m_budget, total);
    u32 rrP = budget > m_listReset ? budget - m_listReset : 0u;
    rrP = std::max(rrP, budget / 8u);
    const ddgi::Split sp = ddgi::SplitBudget(rrP, perCasc, nc > 1 ? perCasc : 0u);
    ddgi::PickRoundRobin(perCasc, sp.k0, m_cursor[0], 0, m_pending, m_list);
    if (nc > 1) ddgi::PickRoundRobin(perCasc, sp.k1, m_cursor[1], perCasc, m_pending, m_list);

    // 間引いたぶんは実効ヒステリシスを h^周期 にして、実時間の収束の速さを保つ
    const float h = std::clamp(s.hysteresis, 0.0f, 0.995f);
    m_hyst[0] = ddgi::EffectiveHysteresis(h, perCasc, sp.k0);
    m_hyst[1] = nc > 1 ? ddgi::EffectiveHysteresis(h, perCasc, sp.k1) : m_hyst[0];
}

void DdgiVolume::PlanFrame(const DdgiSettings& s, bool giNew, const XMFLOAT3& cam, float lastGpuMs)
{
    m_planReady = false;
    ++m_planSeq;
    m_countHist[m_planSeq & 7u] = 0;
    m_planGiNew = giNew && m_psoProbeData != nullptr && IsReady();
    if (!m_planGiNew)
    {
        m_planFollow   = false;
        m_planCascades = 1;
        m_list.clear();
        m_listReset = 0;
        return;
    }
    m_planFollow   = s.followCamera;
    m_planCascades = m_planFollow ? 2u : 1u;
    m_counts[0] = std::clamp(s.probeCountX, 1, 32);
    m_counts[1] = std::clamp(s.probeCountY, 1, 32);
    m_counts[2] = std::clamp(s.probeCountZ, 1, 32);
    const u32 perCasc = static_cast<u32>(m_counts[0]) * m_counts[1] * m_counts[2];
    const u32 nc      = m_planCascades;
    const u32 total   = perCasc * nc;

    m_spacing[0] = std::max(s.spacing, 0.05f);
    m_spacing[1] = m_planFollow ? std::max(s.spacing1, m_spacing[0] * 1.25f) : m_spacing[0];

    PlanKey key;
    for (int a = 0; a < 3; ++a) key.counts[a] = m_counts[a];
    key.cascades = nc;
    key.spacing0 = m_spacing[0];
    key.spacing1 = m_spacing[1];
    key.follow   = m_planFollow;
    if (!m_planFollow) { key.origin[0] = s.originX; key.origin[1] = s.originY; key.origin[2] = s.originZ; }
    if (!(key == m_planKey) || m_pending.size() != total)
    {
        // 格子の形 / 間隔 / 追従の切り替え: 全部作り直し（全プローブをリセット待ちにする）
        m_planKey = key;
        m_pending.assign(total, 1);
        m_win[0] = ddgi::Window{};
        m_win[1] = ddgi::Window{};
        m_cursor[0] = m_cursor[1] = 0;
        m_budget = 0;
        m_requestResetAll = false;
    }
    else if (m_requestResetAll)
    {
        // 履歴の破棄（シーン切替 / リセット）: 全プローブをリセット待ちに。窓は位置をカメラへ取り直す。
        std::fill(m_pending.begin(), m_pending.end(), static_cast<u8>(1));
        m_win[0] = ddgi::Window{};
        m_win[1] = ddgi::Window{};
        m_requestResetAll = false;
    }

    if (m_planFollow)
    {
        const float camv[3] = {cam.x, cam.y, cam.z};
        for (u32 c = 0; c < nc; ++c)
        {
            const ddgi::Move mv = ddgi::UpdateWindow(m_win[c], camv, m_spacing[c], m_counts);
            ddgi::WindowOrigin(m_win[c], m_spacing[c], m_origin[c]);
            ddgi::MarkEntering(m_win[c], mv, m_counts, m_pending, c * perCasc);
        }
    }
    else
    {
        m_origin[0][0] = s.originX; m_origin[0][1] = s.originY; m_origin[0][2] = s.originZ;
        m_win[0].valid = true;
        for (int a = 0; a < 3; ++a) { m_win[0].base[a] = 0; m_win[0].scroll[a] = 0; }
    }

    // ---- 予算: GPU 時間の実測（kLag フレーム遅れ）から 1 プローブの費用を見積もり、更新するプローブ数を決める ----
    constexpr u32 kLag = 3;   // GpuTimer::kFrames
    if (lastGpuMs > 0.0f)
    {
        const u32 cnt = m_countHist[(m_planSeq - kLag) & 7u];
        if (cnt > 0)
        {
            const float cost = lastGpuMs / static_cast<float>(cnt);
            m_costPerProbe = m_costPerProbe > 0.0f ? m_costPerProbe + (cost - m_costPerProbe) * 0.15f : cost;
        }
    }
    if (m_budget == 0) m_budget = total;
    m_budget = ddgi::NextProbeBudget(m_budget, m_costPerProbe, std::max(s.budgetMs, 0.05f),
                                     std::min<u32>(total, 64u), total);

    // ---- ライトグリッドの置き場所（窓の和 + 1 セルの余白。セルの大きさ = 最も粗い間隔）----
    float mn[3] = { 1e30f,  1e30f,  1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    for (u32 c = 0; c < nc; ++c)
        for (int a = 0; a < 3; ++a)
        {
            mn[a] = std::min(mn[a], m_origin[c][a]);
            mx[a] = std::max(mx[a], m_origin[c][a] + static_cast<float>(m_counts[a] - 1) * m_spacing[c]);
        }
    m_lgCell = m_spacing[nc - 1];
    for (int a = 0; a < 3; ++a)
    {
        m_lgOrigin[a] = mn[a] - m_lgCell;
        m_lgDims[a]   = std::clamp(static_cast<int>(std::ceil((mx[a] - mn[a]) / m_lgCell)) + 3, 1, m_counts[a] + 3);
    }

    BuildList(s);
    m_planReady = true;
}

DdgiVolume::SampleParams DdgiVolume::GetSampleParams(const DdgiSettings& s) const
{
    SampleParams p{};
    p.origin0  = {m_origin[0][0], m_origin[0][1], m_origin[0][2], 1.0f};
    const float sp0 = m_spacing[0];
    p.spacing0 = {sp0, sp0, sp0, std::max(s.normalBias, 0.0f)};
    p.counts   = {static_cast<float>(m_counts[0]), static_cast<float>(m_counts[1]), static_cast<float>(m_counts[2]),
                  static_cast<float>(m_planCascades)};
    p.c1       = {m_origin[1][0], m_origin[1][1], m_origin[1][2], m_spacing[1]};
    p.scroll0  = {static_cast<float>(m_win[0].scroll[0]), static_cast<float>(m_win[0].scroll[1]),
                  static_cast<float>(m_win[0].scroll[2]), 0.0f};
    p.scroll1  = {static_cast<float>(m_win[1].scroll[0]), static_cast<float>(m_win[1].scroll[1]),
                  static_cast<float>(m_win[1].scroll[2]), 0.0f};
    return p;
}

void DdgiVolume::Update(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                        const DdgiSettings& s, const UpdateDesc& d)
{
    m_stats.raysCast = 0;
    m_stats.updatedProbes = 0;
    m_stats.resetProbes = 0;
    if (!cmd || !IsReady() || !s.enabled || d.tlas == 0 || d.geometryInfo == 0)
        return;
    // ★GeometryInfo が無いフレームは走らせない。ヒット点のアルベドが引けず真っ黒になる。

    // GI モード New は分類 + 再配置の PSO と PlanFrame の結果が要る（PSO が無ければ Legacy へ落とす）。
    const bool giNew = d.giNew && m_psoProbeData != nullptr;
    if (giNew && (!m_planReady || !m_planGiNew)) return;   // PlanFrame が呼ばれていない（呼び出し側の順序の誤り）。何もしない

    // 遅延確保: DDGI を実際に ON にしたフレームで初めてテクスチャを取る
    //（OFF のプロジェクトでは VRAM もディスクリプタも 1 バイトも消費しない）。
    m_justRecreated = false;
    if (!EnsureResources(device, s, giNew ? m_planCascades : 1u))
        return;
    TickRetired();
    if (giNew && !EnsurePlanBuffers(device)) return;
    if (giNew && m_justRecreated)
    {
        // 作り直したフレーム: 全プローブを埋め直す（計画を作った時点では古いテクスチャだった）
        std::fill(m_pending.begin(), m_pending.end(), static_cast<u8>(1));
        BuildList(s);
    }

    const u32 perCasc = m_probesX * m_probesY * m_probesZ;
    const u32 total = giNew ? perCasc * m_layoutCascades : perCasc;
    if (total == 0) return;

    const GiMode curMode = giNew ? GiMode::New : GiMode::Legacy;
    // モードが変わるとアトラスの値の意味が変わる（空の項 / .a の中身）。履歴を捨てて埋め直す。
    if (curMode != m_lastGiMode)
    {
        m_historyValid = false;
        m_lastGiMode   = curMode;
        if (giNew)
        {
            std::fill(m_pending.begin(), m_pending.end(), static_cast<u8>(1));
            BuildList(s);
        }
    }

    DdgiCB cb{};
    cb.originWS   = giNew ? XMFLOAT3{m_origin[0][0], m_origin[0][1], m_origin[0][2]} : XMFLOAT3{s.originX, s.originY, s.originZ};
    cb.rayLength  = std::max(s.rayLength, 0.1f);
    const float sp0 = giNew ? m_spacing[0] : s.spacing;
    cb.spacing    = {sp0, sp0, sp0};
    cb.hysteresis = giNew ? m_hyst[0] : (m_historyValid ? std::clamp(s.hysteresis, 0.0f, 0.995f) : 0.0f);
    cb.probeCountX = m_probesX; cb.probeCountY = m_probesY; cb.probeCountZ = m_probesZ;
    cb.frameIndex  = d.frameIndex;
    cb.intensity   = std::max(s.intensity, 0.0f);
    cb.normalBias  = std::max(s.normalBias, 0.0f);
    cb.sunDir      = d.sunDir;
    cb.sunIntensity = d.sunIntensity;
    cb.sunColor    = d.sunColor;
    cb.skyColor    = d.skyColor;
    cb.skyCubeIndex = d.skyCubeSrvIndex;
    cb.lightSrvIndex = d.lightSrvIndex;
    cb.lightCount    = d.lightCount;
    cb.bounceIntensity = std::clamp(s.bounceIntensity, 0.0f, 1.0f);
    // 履歴が無いフレーム（初回 / InvalidateHistory 直後）はバウンスを切る。
    // アトラスはクリアしていないので、読むと未初期化メモリを拾う。
    constexpr u32 kNoSrv = 0xFFFFFFFFu;
    const bool wantBounce = m_historyValid && cb.bounceIntensity > 0.0f;
    cb.prevIrradianceSrv = wantBounce ? m_irradianceSrv : kNoSrv;
    cb.prevDistanceSrv   = wantBounce ? m_distanceSrv   : kNoSrv;

    if (giNew)
    {
        // バイアス類はプローブ間隔に比例させる（RTXGI も間隔に合わせた値を推奨している）。カスケード 1 はシェーダが間隔比で拡大する。
        const float sp = std::max(sp0, 0.01f);
        // 検証用ビット（debugStage をそのまま渡す）: bit0 = 旧の空の項 / bit1 = 再配置なし / bit2 = 全プローブ有効（分類なし）
        // bit4(16) = ライトグリッドを使わず総当たり（giFlags bit7）。bit6 = 更新リスト方式（履歴リセットはリストの要素ごと）
        cb.giFlags       = 1u | 64u
                         | ((d.giStage & 1u) ? 4u : 0u) | ((d.giStage & 2u) ? 8u : 0u) | ((d.giStage & 4u) ? 16u : 0u)
                         | ((d.giStage & 16u) ? 128u : 0u);
        cb.envCubeIndex  = d.envCubeSrvIndex;
        cb.skyScale      = std::max(d.skyScale, 0.0f);
        cb.minFrontDist  = std::clamp(kMinFrontRatio * sp, 0.03f, 0.5f);
        cb.viewBias      = kViewBiasRatio * sp;
        cb.normalBiasNew = kNormalBiasRatio * sp;
        cb.relocLimit    = kRelocLimit;
        cb.unitScale     = d.unitScale;
        if (d.physicalUnits) cb.giFlags |= 32u;
        cb.planSrvIndex  = m_planSrv[d.ringIndex % kPlanRing];
    }
    else
    {
        cb.envCubeIndex = kNoSrv;
        cb.unitScale    = 1.0f;
        cb.planSrvIndex = kNoSrv;
    }

    // ---- 更新計画（ヘッダ + 更新リスト）を UPLOAD バッファへ書く ----
    u32 listCount = total;
    u32 lgCells = 0;
    if (giNew)
    {
        listCount = static_cast<u32>(m_list.size());
        u32* w = reinterpret_cast<u32*>(m_planMapped[d.ringIndex % kPlanRing]);
        std::memset(w, 0, kPlanHeaderWords * sizeof(u32));
        auto putF = [&](u32 i, float v) { std::memcpy(&w[i], &v, sizeof(float)); };
        auto putI = [&](u32 i, int v)   { std::memcpy(&w[i], &v, sizeof(int)); };
        w[0] = listCount;
        w[1] = m_layoutCascades;
        w[2] = perCasc;
        w[3] = kLightGridMaxPerCell;
        for (int a = 0; a < 3; ++a)
        {
            putI(4 + a,  m_win[0].scroll[a]);
            putI(8 + a,  m_win[1].scroll[a]);
            putF(12 + a, m_origin[0][a]);
            putF(16 + a, m_origin[1][a]);
            putF(20 + a, m_lgOrigin[a]);
            w[24 + a] = static_cast<u32>(m_lgDims[a]);
        }
        putF(7,  m_spacing[0]);
        putF(11, m_spacing[1]);
        putF(15, m_hyst[0]);
        putF(19, m_hyst[1]);
        putF(23, m_lgCell);
        lgCells = static_cast<u32>(m_lgDims[0]) * m_lgDims[1] * m_lgDims[2];
        const bool useGrid = m_psoLightGrid != nullptr && d.lightSrvIndex != kNoSrv && d.lightCount > 0
                          && lgCells <= m_lightGridCapCells;
        w[27] = useGrid ? 1u : 0u;
        w[28] = 8u;   // ライトグリッドの外のヒット点は、灯がこの数以下のシーンだけ総当たり
        if (listCount > 0) std::memcpy(&w[kPlanHeaderWords], m_list.data(), listCount * sizeof(u32));
    }
    if (listCount == 0)
    {
        if (giNew) m_planReady = false;
        return;
    }

    // ★段階3: TraceCS が【前フレームの】アトラスを SRV で読むので、Trace の前に
    //   NON_PIXEL_SHADER_RESOURCE へ、Blend の直前に UNORDERED_ACCESS へ、と 2 段に分ける。
    //   ping-pong は要らない。この 1 フレームの並びは
    //     [SRV へ] → Trace（読む） → [UAV へ] → Blend（書く） → [PS SRV へ]
    //   で、遷移バリアが完全な同期点なので読みが書きより先に完了することが保証される。
    // 状態を一括で移すヘルパ（同じ状態なら何もしない）。
    //   ★probeData は compute からは常に UAV で触る（Trace が u3 から読み、ProbeData が書く）。
    //   なので NON_PIXEL_SHADER_RESOURCE への遷移では動かさず、UAV と PS SRV にだけ追従する。
    //   Legacy フレームでは触らない（フォワードも読まない）。
    const auto transition = [&](D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b[3]{};
        u32 n = 0;
        auto one = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES target)
        {
            if (state == target) return;
            b[n].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[n].Transition.pResource   = res;
            b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[n].Transition.StateBefore = state;
            b[n].Transition.StateAfter  = target;
            ++n;
            state = target;
        };
        one(m_irradiance.Get(), m_irradianceState, to);
        one(m_distance.Get(),   m_distanceState,   to);
        if (giNew)
            one(m_probeData.Get(), m_probeDataState,
                to == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : to);
        if (n > 0) cmd->ResourceBarrier(n, b);
    };

    // バウンスするフレームだけ、Trace の前に読み取り状態へ移す。
    if (wantBounce) transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // New: probeData は Trace の前に UAV にしておく（前フレームの最後は PS SRV）。
    if (giNew && m_probeDataState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER pb{};
        pb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pb.Transition.pResource   = m_probeData.Get();
        pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pb.Transition.StateBefore = m_probeDataState;
        pb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmd->ResourceBarrier(1, &pb);
        m_probeDataState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kDdgiCBNum32, &cb, 0);
    cmd->SetComputeRootShaderResourceView(1, d.tlas);
    cmd->SetComputeRootShaderResourceView(2, d.geometryInfo);
    cmd->SetComputeRootDescriptorTable(3, m_srvHeap->GetGpuHandle(m_rayDataUav));

    // ---- ライトグリッド（GI モード New: ヒット点の点光源を絞る。1 スレッド = 1 セル）----
    if (giNew && reinterpret_cast<const u32*>(m_planMapped[d.ringIndex % kPlanRing])[27] != 0u)
    {
        D3D12_RESOURCE_BARRIER lgB{};
        lgB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        lgB.UAV.pResource = m_lightGrid.Get();
        cmd->ResourceBarrier(1, &lgB);   // 前フレームの Trace が読み終わってから書く
        cmd->SetPipelineState(m_psoLightGrid.Get());
        cmd->Dispatch((lgCells + 63u) / 64u, 1, 1);
        cmd->ResourceBarrier(1, &lgB);   // Trace が読む前に書き終える
    }

    // ---- レイトレ（1 スレッド = 1 レイ）----
    cmd->SetPipelineState(m_psoTrace.Get());
    cmd->Dispatch(1, listCount, 1);   // x は numthreads が kRaysPerProbe なので 1 グループ。y = 更新リストの長さ（Legacy は全プローブ）

    // フォワード PS が t22/t23 から読むようになったので、書く前に UAV へ戻す。
    transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // RayData の書き込みを Blend が読む前に直列化する。
    D3D12_RESOURCE_BARRIER uavB{};
    uavB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavB.UAV.pResource = m_rayData.Get();
    cmd->ResourceBarrier(1, &uavB);

    if (giNew)
    {
        // ---- 分類 + 再配置（1 スレッド = 1 プローブ）。RayData を読んで probeData を更新する ----
        // ★Trace は probeData の旧オフセットを読み、ここが新オフセットを書く。Blend はここの新しい状態を読む。
        cmd->SetPipelineState(m_psoProbeData.Get());
        cmd->Dispatch((listCount + 63u) / 64u, 1, 1);
        D3D12_RESOURCE_BARRIER pdB{};
        pdB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        pdB.UAV.pResource = m_probeData.Get();
        cmd->ResourceBarrier(1, &pdB);
    }

    // ---- ブレンド（1 グループ = 1 プローブ / 1 スレッド = 1 テクセル）----
    // ★irradiance と距離モーメントは別テクスチャなので、間に UAV バリアは要らない
    //   （どちらも同じ RayData を読むだけ。上のバリアで直列化済み）。
    cmd->SetPipelineState(m_psoBlend.Get());
    cmd->Dispatch(listCount, 1, 1);
    cmd->SetPipelineState(m_psoBlendDist.Get());
    cmd->Dispatch(listCount, 1, 1);

    // フォワードパスが PS から読めるようにしておく（段階1 / 段階2）。
    transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_stats.raysCast = listCount * kRaysPerProbe;
    m_stats.updatedProbes = listCount;
    m_stats.resetProbes = giNew ? m_listReset : 0u;
    m_stats.budgetProbes = giNew ? m_budget : total;
    m_stats.costUsPerProbe = m_costPerProbe * 1000.0f;
    m_stats.lightGridCells = lgCells;
    m_stats.hysteresis0 = giNew ? m_hyst[0] : s.hysteresis;
    m_stats.hysteresis1 = giNew ? m_hyst[1] : s.hysteresis;
    m_stats.cascades = giNew ? m_layoutCascades : 1u;
    m_historyValid   = true;
    if (giNew)
    {
        // このフレームでリセットを済ませたプローブの待ちを外す
        for (u32 e : m_list)
            if (e & ddgi::kResetBit) m_pending[e & ~ddgi::kResetBit] = 0;
        m_countHist[m_planSeq & 7u] = listCount;
        m_planReady = false;
    }
}

} // namespace dx12e
