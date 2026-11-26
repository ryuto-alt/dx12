#pragma once
// ===========================================================================
// DeviceCaps（W05-04 / F0-2）: デバイスの機能の判定の「唯一の正」。
//   GraphicsDevice が作成時に QueryDeviceCaps を 1 回だけ呼び、既存の getter（GetHighestShaderModel など）は
//   ここの値へ委譲する（同じ問い合わせを二重にしない）。
//   このヘッダは GraphicsDevice.h からは include しない（120 TU に広げない。型が要る TU だけが include する）。
//   F0 では「読むだけ」。どの描画経路も caps で切り替えない（Enhanced Barriers を使うのは F1）。
// ===========================================================================
#include <Windows.h>

#include <directx/d3d12.h>

#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e
{

struct DeviceCaps
{
    u32  highestShaderModel = 0x60;        // D3D_SHADER_MODEL の値（0x69 = SM 6.9）。6.9 から下げて問い合わせる
    bool enhancedBarriers   = false;       // OPTIONS12.EnhancedBarriersSupported
    bool relaxedFormatCasting = false;     // OPTIONS12.RelaxedFormatCastingSupported
    bool gpuUploadHeap      = false;       // OPTIONS16.GPUUploadHeapSupported
    bool tightAlignment     = false;       // D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT の SupportTier >= 1（1.618+）
    u32  rootSignatureMaxVersion = 10;     // 10 = 1.0 / 11 = 1.1 / 12 = 1.2
    u32  raytracingTier     = 0;           // D3D12_RAYTRACING_TIER の値（0 / 10 / 11 / 12）
    bool serActuallyReorders = false;      // OPTIONS22（DXR 1.2 のときだけ問い合わせる）
    bool omm                = false;       // = raytracingTier >= D3D12_RAYTRACING_TIER_1_2（OMM は DXR 1.2 の一部。専用の tier は無い）
    bool fenceBarriersTier1 = false;       // D3D12_FEATURE_FENCE_BARRIERS の Tier >= 1（1.620）
    u32  options9Bits       = 0;           // OPTIONS9 の bool を bit0.. に詰めたもの（名前は DeviceCapsFields の options9 系）
    u32  options17Bits      = 0;           // OPTIONS17 同様
    bool asyncCommands      = false;       // D3D12_FEATURE_ASYNC_COMMANDS の Impl != NOT_SUPPORTED（1.62x）。無ければ false
    bool meshShader         = false;       // OPTIONS7.MeshShaderTier >= 1
    u32  meshShaderTier     = 0;           // D3D12_MESH_SHADER_TIER の値（GraphicsDevice::GetMeshShaderTier の元）
    bool samplerFeedback    = false;       // OPTIONS7.SamplerFeedbackTier >= 0.9
    u32  vrsTier            = 0;           // OPTIONS6.VariableShadingRateTier（0 / 1 / 2）
    u32  wsTier             = 0;           // OPTIONS21.WorkGraphsTier（0 / 10）。設計の "wsTier" を Work Graphs と解釈した
    u32  resourceBindingTier = 1;          // OPTIONS.ResourceBindingTier（1..3）。既定 1 は旧 GraphicsDevice の既定（TIER_1）との互換のため（他の項目の「失敗は 0 / false」とは異なる）
    bool device10           = false;       // ID3D12Device10 を QueryInterface できる
    bool list7              = false;       // ID3D12GraphicsCommandList7 を QueryInterface できる（実リストで確認）
};

// 1 項目の名前と値（MCP の JSON、DeepDiagnostics、ログが同じ表を使う）。
struct DeviceCapsField
{
    enum class Kind { Bool, Int, Hex };
    const char* name;
    Kind        kind;
    u64         value;
};

// 失敗した問い合わせは false / 0 のまま。例外を出さない。device が nullptr なら既定値のまま返す。
DeviceCaps QueryDeviceCaps(ID3D12Device* device);

// 全項目（上の構造体の順。続けて options9 / options17 の bit を 1 つずつ）。
std::vector<DeviceCapsField> DeviceCapsFields(const DeviceCaps& caps);

// 起動ログの 1 行（"caps: SM6.9 EB=1 UploadHeap=1 RS1.2 DXR1.2 SER=1 OMM=1 FenceBarrier=1 ..."）。
std::string DeviceCapsSummary(const DeviceCaps& caps);

// F1 の Barrier() 用の QI のヘルパ（W0.5 では使わない）。取れなければ nullptr。
// 返すポインタは list と同じ COM オブジェクトを指す（参照を足さない。list が生きている間だけ有効）。
ID3D12GraphicsCommandList7* AsList7(ID3D12GraphicsCommandList* list);

} // namespace dx12e
