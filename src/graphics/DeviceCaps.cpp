#include "DeviceCaps.h"

#include <wrl/client.h>

#include <cstdio>
#include <initializer_list>

// CheckFeatureSupport に渡す構造体の版を、使うヘッダの SDK 版で保証する（古いヘッダで黙って 0 を返さない）。
static_assert(D3D12_SDK_VERSION >= 620, "DeviceCaps: D3D12 ヘッダが 1.620 未満です（FENCE_BARRIERS / ASYNC_COMMANDS / OPTIONS22 が要る）");

namespace dx12e
{

namespace
{
// 構造体のサイズを渡す CheckFeatureSupport（失敗したら out はゼロのまま）。
template <typename T>
bool Check(ID3D12Device* dev, D3D12_FEATURE feature, T& out)
{
    out = T{};
    return SUCCEEDED(dev->CheckFeatureSupport(feature, &out, sizeof(T)));
}

u32 Bits(std::initializer_list<BOOL> flags)
{
    u32 b = 0, i = 0;
    for (BOOL f : flags)
    {
        if (f) b |= (1u << i);
        ++i;
    }
    return b;
}

const char* const kOptions9Names[] = { "options9.MeshShaderPipelineStats", "options9.MeshShaderFullRangeRTArrayIndex",
                                       "options9.AtomicInt64OnTypedResource", "options9.AtomicInt64OnGroupShared",
                                       "options9.DerivativesInMeshAndAmplification" };
const char* const kOptions17Names[] = { "options17.NonNormalizedCoordinateSamplers", "options17.ManualWriteTrackingResource" };
} // namespace

DeviceCaps QueryDeviceCaps(ID3D12Device* device)
{
    DeviceCaps c;
    if (!device) return c;

    // --- シェーダモデル: 高い方から 1 段ずつ下げる（ランタイムが知らない値は E_INVALIDARG）---
    {
        static constexpr D3D_SHADER_MODEL kProbe[] = {
            D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
            D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
            D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
        };
        for (D3D_SHADER_MODEL sm : kProbe)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL d{};
            d.HighestShaderModel = sm;
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &d, sizeof(d))))
            {
                c.highestShaderModel = static_cast<u32>(d.HighestShaderModel);
                break;
            }
        }
    }

    // --- ルートシグネチャ（1.2 から下げる）---
    {
        static constexpr D3D_ROOT_SIGNATURE_VERSION kRs[] = { D3D_ROOT_SIGNATURE_VERSION_1_2, D3D_ROOT_SIGNATURE_VERSION_1_1 };
        c.rootSignatureMaxVersion = 10;
        for (D3D_ROOT_SIGNATURE_VERSION v : kRs)
        {
            D3D12_FEATURE_DATA_ROOT_SIGNATURE d{};
            d.HighestVersion = v;
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &d, sizeof(d))))
            {
                c.rootSignatureMaxVersion = (d.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_2) ? 12u
                                          : (d.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1) ? 11u : 10u;
                break;
            }
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o0{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS, o0))
        c.resourceBindingTier = static_cast<u32>(o0.ResourceBindingTier);

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS5, o5))
        c.raytracingTier = static_cast<u32>(o5.RaytracingTier);
    c.omm = c.raytracingTier >= static_cast<u32>(D3D12_RAYTRACING_TIER_1_2);

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS6, o6))
        c.vrsTier = static_cast<u32>(o6.VariableShadingRateTier);

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS7, o7))
    {
        c.meshShaderTier  = static_cast<u32>(o7.MeshShaderTier);
        c.meshShader      = o7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        c.samplerFeedback = o7.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS9, o9))
        c.options9Bits = Bits({ o9.MeshShaderPipelineStatsSupported, o9.MeshShaderSupportsFullRangeRenderTargetArrayIndex,
                                o9.AtomicInt64OnTypedResourceSupported, o9.AtomicInt64OnGroupSharedSupported,
                                o9.DerivativesInMeshAndAmplificationShadersSupported });

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS12, o12))
    {
        c.enhancedBarriers     = o12.EnhancedBarriersSupported != FALSE;
        c.relaxedFormatCasting = o12.RelaxedFormatCastingSupported != FALSE;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS16, o16))
        c.gpuUploadHeap = o16.GPUUploadHeapSupported != FALSE;

    D3D12_FEATURE_DATA_D3D12_OPTIONS17 o17{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS17, o17))
        c.options17Bits = Bits({ o17.NonNormalizedCoordinateSamplersSupported, o17.ManualWriteTrackingResourceSupported });

    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
    if (Check(device, D3D12_FEATURE_D3D12_OPTIONS21, o21))
        c.wsTier = static_cast<u32>(o21.WorkGraphsTier);

    // SER は DXR 1.2 のときだけ意味がある（既存の判定と同じ条件）。
    if (c.raytracingTier >= static_cast<u32>(D3D12_RAYTRACING_TIER_1_2))
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS22 o22{};
        if (Check(device, D3D12_FEATURE_D3D12_OPTIONS22, o22))
            c.serActuallyReorders = o22.ShaderExecutionReorderingActuallyReorders != FALSE;
    }

    D3D12_FEATURE_DATA_TIGHT_ALIGNMENT ta{};
    if (Check(device, D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT, ta))
        c.tightAlignment = ta.SupportTier >= D3D12_TIGHT_ALIGNMENT_TIER_1;

    {
        D3D12_FEATURE_DATA_FENCE_BARRIERS fb{};
        fb.NodeIndex = 0;
        fb.CommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FENCE_BARRIERS, &fb, sizeof(fb))))
            c.fenceBarriersTier1 = fb.FenceBarriersTier >= D3D12_FENCE_BARRIERS_TIER_1;
    }

    {
        D3D12_FEATURE_DATA_ASYNC_COMMANDS ac{};
        if (Check(device, D3D12_FEATURE_ASYNC_COMMANDS, ac))
            c.asyncCommands = ac.Impl != D3D12_ASYNC_COMMANDS_IMPL_NOT_SUPPORTED;
    }

    {
        Microsoft::WRL::ComPtr<ID3D12Device10> d10;
        c.device10 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d10)));
        // ID3D12GraphicsCommandList7 は実リストで確かめる（使い捨てのリストを 1 つ作って閉じる）。
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
            && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
        {
            c.list7 = AsList7(list.Get()) != nullptr;
            list->Close();
        }
    }
    return c;
}

std::vector<DeviceCapsField> DeviceCapsFields(const DeviceCaps& c)
{
    using K = DeviceCapsField::Kind;
    std::vector<DeviceCapsField> f;
    f.push_back({ "highestShaderModel", K::Hex, c.highestShaderModel });
    f.push_back({ "enhancedBarriers", K::Bool, c.enhancedBarriers });
    f.push_back({ "relaxedFormatCasting", K::Bool, c.relaxedFormatCasting });
    f.push_back({ "gpuUploadHeap", K::Bool, c.gpuUploadHeap });
    f.push_back({ "tightAlignment", K::Bool, c.tightAlignment });
    f.push_back({ "rootSignatureMaxVersion", K::Int, c.rootSignatureMaxVersion });
    f.push_back({ "raytracingTier", K::Int, c.raytracingTier });
    f.push_back({ "serActuallyReorders", K::Bool, c.serActuallyReorders });
    f.push_back({ "omm", K::Bool, c.omm });
    f.push_back({ "fenceBarriersTier1", K::Bool, c.fenceBarriersTier1 });
    f.push_back({ "options9Bits", K::Hex, c.options9Bits });
    f.push_back({ "options17Bits", K::Hex, c.options17Bits });
    f.push_back({ "asyncCommands", K::Bool, c.asyncCommands });
    f.push_back({ "meshShader", K::Bool, c.meshShader });
    f.push_back({ "meshShaderTier", K::Int, c.meshShaderTier });
    f.push_back({ "samplerFeedback", K::Bool, c.samplerFeedback });
    f.push_back({ "vrsTier", K::Int, c.vrsTier });
    f.push_back({ "wsTier", K::Int, c.wsTier });
    f.push_back({ "resourceBindingTier", K::Int, c.resourceBindingTier });
    f.push_back({ "device10", K::Bool, c.device10 });
    f.push_back({ "list7", K::Bool, c.list7 });
    for (size_t i = 0; i < sizeof(kOptions9Names) / sizeof(kOptions9Names[0]); ++i)
        f.push_back({ kOptions9Names[i], K::Bool, (c.options9Bits >> i) & 1u });
    for (size_t i = 0; i < sizeof(kOptions17Names) / sizeof(kOptions17Names[0]); ++i)
        f.push_back({ kOptions17Names[i], K::Bool, (c.options17Bits >> i) & 1u });
    return f;
}

std::string DeviceCapsSummary(const DeviceCaps& c)
{
    char buf[512];
    const u32 sm = c.highestShaderModel;
    const u32 rt = c.raytracingTier;
    char dxr[16];
    if (rt >= 10) std::snprintf(dxr, sizeof(dxr), "DXR%u.%u", rt / 10, rt % 10);
    else          std::snprintf(dxr, sizeof(dxr), "DXR=none");
    std::snprintf(buf, sizeof(buf),
        "caps: SM%u.%u EB=%d UploadHeap=%d RS%u.%u %s SER=%d OMM=%d FenceBarrier=%d TightAlign=%d RelaxedCast=%d "
        "Mesh=%d SamplerFeedback=%d VRS=%u WorkGraphs=%u AsyncCmd=%d Device10=%d List7=%d",
        sm >> 4, sm & 0xF, c.enhancedBarriers ? 1 : 0, c.gpuUploadHeap ? 1 : 0,
        c.rootSignatureMaxVersion / 10, c.rootSignatureMaxVersion % 10, dxr,
        c.serActuallyReorders ? 1 : 0, c.omm ? 1 : 0, c.fenceBarriersTier1 ? 1 : 0, c.tightAlignment ? 1 : 0,
        c.relaxedFormatCasting ? 1 : 0, c.meshShader ? 1 : 0, c.samplerFeedback ? 1 : 0, c.vrsTier, c.wsTier,
        c.asyncCommands ? 1 : 0, c.device10 ? 1 : 0, c.list7 ? 1 : 0);
    return buf;
}

ID3D12GraphicsCommandList7* AsList7(ID3D12GraphicsCommandList* list)
{
    if (!list) return nullptr;
    ID3D12GraphicsCommandList7* l7 = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&l7))) || !l7) return nullptr;
    l7->Release();   // 同じ COM オブジェクト。呼び出し側は list の寿命の間だけ使う（参照を足さない契約）
    return l7;
}

} // namespace dx12e
