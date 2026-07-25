#pragma once

#include <directx/d3d12.h>

#include "core/Types.h"
#include "graphics/GpuResource.h"

namespace dx12e
{
class GraphicsDevice;

// DXR の加速構造（BLAS / TLAS）まわりで使う GPU バッファ。
//
// なぜ Buffer クラスを使わないのか:
//   Buffer::CreateBuffer は desc.Flags を D3D12_RESOURCE_FLAG_NONE 固定で作るので、
//   加速構造に必須の ALLOW_UNORDERED_ACCESS が立てられない。初期状態も
//   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE を渡す必要がある
//   （DXR 仕様上、AS のリソースは生涯この状態から動かせない。遷移バリアではなく
//     UAV バリアで同期する）。
//
// また Buffer / GpuResource::CreateResource は失敗時に throw するが、
// AS は VRAM 不足で普通に確保に失敗しうる（計画09 §5.3 の Gate 6）ので、
// ここは HRESULT を握って bool を返す。失敗しても例外を投げない。
class RtBuffer : public GpuResource
{
public:
    enum class Kind
    {
        // 加速構造の本体。DEFAULT + ALLOW_UNORDERED_ACCESS。
        // 初期状態 = RAYTRACING_ACCELERATION_STRUCTURE（以後ずっとこの状態）。
        AccelStruct,
        // ビルド用スクラッチ。DEFAULT + ALLOW_UNORDERED_ACCESS、状態 UNORDERED_ACCESS。
        Scratch,
        // CPU が毎フレーム書く（インスタンス記述など）。UPLOAD + 永続 Map。
        Upload,
    };

    // 失敗しても throw しない。false を返したら呼び出し側が機能を諦めること。
    // sizeInBytes は Kind に応じて必要なアラインへ内部で切り上げる。
    bool Initialize(GraphicsDevice& device, u64 sizeInBytes, Kind kind, const wchar_t* debugName);

    // 現在のバッファが sizeInBytes を満たさなければ作り直す（満たしていれば何もしない）。
    // 戻り値は「呼び出し後に有効なバッファがあるか」。
    bool EnsureCapacity(GraphicsDevice& device, u64 sizeInBytes, Kind kind, const wchar_t* debugName);

    void Release() { ReleaseResource(); m_size = 0; m_mapped = nullptr; }

    D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const
    {
        return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
    }
    u64   GetSizeInBytes() const { return m_size; }
    void* GetMappedPtr()   const { return m_mapped; }   // Kind::Upload のみ非 null
    bool  IsValid()        const { return m_resource != nullptr; }

private:
    u64   m_size   = 0;
    void* m_mapped = nullptr;
};

// AS まわりのアラインメント（d3d12.h の定数をそのまま使う。数値を直書きしないこと）
static constexpr u64 kAsAlignment       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;      // 256
static constexpr u64 kAsScratchAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;             // 256
static constexpr u64 kInstanceDescAlignment = D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT;          // 16

// nullptr（= 全リソース対象）の UAV バリアを 1 本積む。
// DXR の加速構造は状態遷移ができないので、
//   ・BLAS ビルド → TLAS ビルド（読み）
//   ・TLAS ビルド → RayQuery（読み）
//   ・前フレームの RayQuery（読み） → 今フレームのビルド（書き）
//   ・同じスクラッチバッファを使い回す連続ビルド
// の全てをこれで直列化する。デバッグレイヤは忘れても警告してくれない。
void RtUavBarrier(ID3D12GraphicsCommandList* cmd);

} // namespace dx12e
