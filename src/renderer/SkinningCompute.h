#pragma once
//
// SkinningCompute — compute スキニング（計画09 Step 4）
//
// スキンドメッシュの「変形後の頂点位置」を DEFAULT ヒープのバッファへ書き出す。
// これが DXR の BLAS の入力になる（頂点シェーダ内スキニングだけだと変形後の頂点が
// GPU のどこにも残らないので、スキンドは TLAS に入れられなかった）。
//
// ★所有の考え方: 出力バッファは「エンティティ × サブメッシュ」単位。
//   Mesh* は ResourceManager がモデルパス単位でキャッシュしていて**複数エンティティで共有**
//   されるため（同じキャラを 10 体置くと Mesh* は 1 個・ポーズは 10 通り）、
//   Mesh* をキーにすると全員が同じポーズになる。呼び出し側が一意な u64 キーを作って渡すこと。
//
// ★ECS には依存しない（RaytracingScene と同じ方針）。キーは呼び出し側が畳んで渡す。
//
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class Mesh;

class SkinningCompute
{
public:
    // 変形後頂点の 1 頂点あたりのバイト数（float3 の位置のみ）。
    // BLAS の VertexBuffer.StrideInBytes にそのまま渡す。
    static constexpr u32 kOutStride = 12;

    void Initialize(GraphicsDevice& device, const std::wstring& shaderDir);
    void Shutdown();
    void RecreatePipelines(GraphicsDevice& device);   // シェーダーホットリロード用

    bool IsReady() const { return m_pso != nullptr; }

    // このフレームで使わなかった出力バッファを解放する準備。BeginFrame → Skin... → EndFrame。
    void BeginFrame();
    void EndFrame();

    // key のメッシュをスキニングして変形後バッファへ書く。
    // 戻り値 = 変形後バッファの GPU アドレス（0 なら失敗＝呼び出し側は TLAS へ入れないこと）。
    // bonesAddress は SkinningBuffer::GetGpuAddress(frameIndex)。
    //
    // poseHash: ボーン行列から作ったハッシュ。前フレームと同じなら**ディスパッチを丸ごと省く**。
    //   ★これが効果最大の最適化。Battlefield V は「ボーン行列が変わっていないキャラを飛ばす」
    //     だけで BLAS 更新を 400 → 50（8 倍削減）にしている。
    //     このエンジンでは編集中 Scene::Update に dt=0 が渡ってポーズが止まるので、
    //     エディタで触っている間はスキニングも BLAS 再構築もほぼ走らなくなる。
    D3D12_GPU_VIRTUAL_ADDRESS Skin(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                                   u64 key, const Mesh& mesh,
                                   D3D12_GPU_VIRTUAL_ADDRESS bonesAddress, u64 poseHash);

    // Skin() で書いた全バッファを UAV → NON_PIXEL_SHADER_RESOURCE へ遷移させる。
    // BLAS のビルド入力にする前に必ず 1 回呼ぶこと。
    // ★RaytracingScene が静的メッシュで「バリア不要」と書いているのは VB が GENERIC_READ で
    //   置かれているからで、compute の出力には当てはまらない。
    void TransitionForAccelerationStructureBuild(ID3D12GraphicsCommandList* cmd);

    struct Stats
    {
        u32 meshes   = 0;   // このフレームでスキニングしたメッシュ数
        u32 skipped  = 0;   // ポーズが前フレームと同じでディスパッチを省いた数
        u32 vertices = 0;   // 同 頂点数
        u64 bytes    = 0;   // 出力バッファの合計バイト数
    };
    const Stats& GetStats() const { return m_stats; }

private:
    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        u32  vertexCount = 0;
        u32  lastUsedFrame = 0;
        u64  poseHash = 0;   // 0 = 未スキニング（必ず 1 回は走らせる）
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        bool writtenThisFrame = false;
    };

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    std::wstring m_shaderDir;

    std::unordered_map<u64, Entry> m_entries;
    u32   m_frame = 0;
    Stats m_stats;
};

} // namespace dx12e
