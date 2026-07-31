#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
struct OcclusionBounds;

// Hi-Z ピラミッドを引いて、描画アイテムごとに「隠れているか」を GPU で判定する。
//
// 出力は 2 つ:
//  1. 可視性バッファ（uint64 x アイテム数。1=見える / 0=隠れている）
//     → D3D12_RESOURCE_STATE_PREDICATION へ遷移して SetPredication で描画を落とす。
//       ★D3D12 のプレディケーションはクエリを必要としない。「バッファ内の 64bit の値」で
//         判定できるので、compute が書いた結果をそのまま述語にできる。
//         読み戻しゼロ・遅延ゼロで、ルートシグネチャ/マテリアル/PSO/シェーダを一切触らない。
//  2. 統計（隠れていた数 / 判定した数）
//     → こちらは READBACK 経由で数フレーム遅れて CPU へ届く。表示専用で描画には使わない。
//
// ★判定は「今フレームの深度プリパス」から作った Hi-Z に対して行うので、時間的なズレが無い。
//   前フレームの結果を使う方式で起きる「速く振り向くと 1〜3 フレーム物が消える」は起きない。
class OcclusionCullPass
{
public:
    void Initialize(GraphicsDevice& device, const std::wstring& shaderDir);
    void Shutdown();
    void RecreatePipelines(GraphicsDevice& device);

    struct Params
    {
        // 深度プリパスと同じ**ジッタ付き** VP を「そのまま」入れること。
        // HLSL 側の列優先解釈に合わせる転置は Dispatch が中でやる
        // （呼び出し側でやると片方だけ忘れて絵が壊れるので、ここで閉じる）。
        DirectX::XMFLOAT4X4 viewProj;
        f32 vpX, vpY, vpW, vpH;          // ジオメトリが描かれている矩形（px）
        f32 hzbW, hzbH;                  // Hi-Z mip0 の解像度
        u32 mipCount;
    };

    // 判定を実行する。cmd には SRV ヒープが設定済みであること。
    // 戻り時、可視性バッファは PREDICATION 状態（そのまま SetPredication に渡せる）。
    // bounds[k] の判定結果は可視性バッファの k 番目（＝オフセット k*8）に入る。
    // ★呼び出し側は「個別に述語を張れるもの」だけを bounds に入れること。バッチに属する
    //   個々のアイテムを入れても述語を張る先が無く、判定コストと転送帯域を捨てるだけになる。
    void Dispatch(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                  const std::vector<OcclusionBounds>& bounds, const Params& p,
                  D3D12_GPU_DESCRIPTOR_HANDLE hzbSrvGpu, u32 frameIndex);

    // 前フレームまでの結果を CPU から回収する（数フレーム遅れ。表示専用）。
    void CollectStats(u32 frameIndex);

    bool IsReady()      const { return m_pso != nullptr; }
    u32  GetOccluded()  const { return m_statOccluded; }
    u32  GetTested()    const { return m_statTested; }
    u32  GetCapacity()  const { return m_capacity; }

    // 可視性バッファ（PREDICATION 状態）。アイテム i の述語は i*8 バイト目。
    // ★D3D12 の仕様上オフセットは 8 バイト境界でなければならない。
    ID3D12Resource* GetVisibilityBuffer() const { return m_visBuf.Get(); }

    // bounds[k] に対応する述語のオフセット（バイト）。D3D12 は 8 バイト境界を要求する。
    static u64 PredicateOffset(u32 slot) { return static_cast<u64>(slot) * kPredicateStride; }
    // 直近ディスパッチで判定した件数。slot がこれ未満であることを呼び出し側で確認すること
    // （リサイズ直後などにドローリストと判定結果の件数がズレうるため）。
    u32 GetSlotCount() const { return m_lastSlotCount; }

private:
    void EnsureCapacity(GraphicsDevice& device, u32 count);

    static constexpr u32 kFrameCount = 3;   // FrameResources::kFrameCount と一致させること
    // 述語 1 件のストライド（D3D12 は 64bit 値 + 8 バイト境界を要求する）。
    static constexpr u32 kPredicateStride = 8;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_boundsBuf[kFrameCount];   // UPLOAD（AABB）
    u8*                                    m_boundsMapped[kFrameCount]{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_visBuf;                   // DEFAULT（述語）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statsBuf;                 // DEFAULT（集計）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statsReadback[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statsZero;                // UPLOAD（0 クリア元）

    D3D12_RESOURCE_STATES m_visState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    u32  m_capacity     = 0;
    u32  m_statOccluded = 0;
    u32  m_statTested   = 0;
    u32  m_lastSlotCount  = 0;   // 直近ディスパッチで判定した件数
    bool m_statsPending[kFrameCount]{};

    std::wstring m_shaderDir;
};

} // namespace dx12e
