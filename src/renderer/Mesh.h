#pragma once

#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "graphics/Buffer.h"

namespace dx12e
{

struct Material;
class DescriptorHeap;

struct Vertex
{
    DirectX::XMFLOAT3 position;                        // POSITION
    DirectX::XMFLOAT3 normal;                          // NORMAL
    DirectX::XMFLOAT4 color;                           // COLOR
    DirectX::XMFLOAT2 texCoord;                        // TEXCOORD
    DirectX::XMFLOAT4 tangent  = {0, 0, 0, 1};        // TANGENT (xyz=tangent, w=handedness)
    DirectX::XMUINT4  boneIndices = {0, 0, 0, 0};     // BLENDINDICES
    DirectX::XMFLOAT4 boneWeights = {0, 0, 0, 0};     // BLENDWEIGHT
};

// GPU インスタンス1個ぶん（slot1, PER_INSTANCE, stride=64）。
// r0..r2 = XMMatrixTranspose(world) の先頭3行（4行目は (0,0,0,1) 固定なので省略）。
struct MeshInstanceData
{
    DirectX::XMFLOAT4 r0;     // TEXCOORD8
    DirectX::XMFLOAT4 r1;     // TEXCOORD9
    DirectX::XMFLOAT4 r2;     // TEXCOORD10
    DirectX::XMFLOAT4 color;  // TEXCOORD11
};

// 速度パス用の per-instance「前フレームのワールド行列」（slot2, PER_INSTANCE, stride=48）。
// r0..r2 = XMMatrixTranspose(prevWorld) の先頭3行。
// ★既存の MeshInstanceData(64B) を太らせない: kMaxInstances(262144) × 3 フレームぶんの
//   UPLOAD ヒープなので、+48B すると TAA を使わない人にも +38MB 払わせることになる。
//   よって別バッファにして TAA 有効時のみ遅延確保する。
struct MeshInstancePrevData
{
    DirectX::XMFLOAT4 p0;     // TEXCOORD12
    DirectX::XMFLOAT4 p1;     // TEXCOORD13
    DirectX::XMFLOAT4 p2;     // TEXCOORD14
};

class Mesh
{
public:
    // cmd を渡すと VB/IB(LOD含む) を DEFAULT ヒープ(VRAM)に作りコピーを cmd に積む
    // （モデルロード経路は必ず渡すこと。UPLOAD 直読みは大型メッシュで GPU が桁違いに遅い）。
    void Initialize(GraphicsDevice& device,
                    const std::vector<Vertex>& vertices,
                    const std::vector<u32>& indices,
                    ID3D12GraphicsCommandList* cmd = nullptr);

    // 動的に頂点を書き換えるメッシュ（地形スカルプト等）用の初期化。
    // Initialize と違い meshoptimizer の頂点並べ替えも自動LOD生成もしない
    // ＝渡した配列の並びがそのまま GPU と m_verticesCache の並びになるので、
    // 以後 MutableVertices() で一部だけ書き換えて UploadVertexCache() で送り直せる。
    // （Initialize は remap+optimizeVertexFetch で頂点順を変えるため、部分更新には使えない）
    void InitializeDynamic(GraphicsDevice& device,
                           const std::vector<Vertex>& vertices,
                           const std::vector<u32>& indices,
                           ID3D12GraphicsCommandList* cmd = nullptr);

    // CPU 側の頂点キャッシュ（InitializeDynamic で渡した並びのまま）。
    // 書き換えたら UploadVertexCache() を呼ぶこと。
    std::vector<Vertex>& MutableVertices() { return m_verticesCache; }

    // 頂点キャッシュを GPU へ送り直し、AABB / 位置キャッシュ（ピッキング用）を取り直す。
    // インデックスと LOD は触らない。古い頂点バッファは DeferredRelease 経由で
    // フェンス完了後に解放されるので、描画コマンド記録中に呼んでも安全。
    void UploadVertexCache(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd = nullptr);

    void InitializeAsBox(GraphicsDevice& device);
    void InitializeAsPlane(GraphicsDevice& device, f32 size = 50.0f, u32 subdivisions = 1);
    void InitializeAsSphere(GraphicsDevice& device, f32 radius = 0.5f, u32 slices = 16, u32 stacks = 16);

    void FinishUpload();

    const VertexBuffer& GetVertexBuffer() const { return m_vertexBuffer; }
    const IndexBuffer&  GetIndexBuffer()  const { return m_indexBuffer; }
    u32 GetIndexCount() const { return m_indexBuffer.GetIndexCount(); }

    // 自動LOD: Initialize 時に meshoptimizer で簡略化インデックスを生成（LOD0=フル解像度）。
    // 小さいメッシュや簡略化が効かない形状では m_lodCount=1 のまま＝常に LOD0。
    // lod は範囲外なら最終LODへクランプ（影パスは +1 バイアスで呼んでも安全）。
    static constexpr u32 kMaxLods = 5;   // LOD0 + 簡略4段(30%/10%/3%/1%)
    u32 GetLodCount() const { return m_lodCount; }
    const IndexBuffer& GetIndexBufferLod(u32 lod) const
    {
        if (lod >= m_lodCount) lod = m_lodCount - 1;
        return (lod == 0) ? m_indexBuffer : m_lodIndexBuffers[lod - 1];
    }
    u32 GetIndexCountLod(u32 lod) const { return GetIndexBufferLod(lod).GetIndexCount(); }

    void SetMaterial(Material* mat) { m_material = mat; }
    const Material* GetMaterial() const { return m_material; }
    Material* GetMaterialMutable() { return m_material; }

    DirectX::XMFLOAT3 GetAABBMin() const { return m_aabbMin; }
    DirectX::XMFLOAT3 GetAABBMax() const { return m_aabbMax; }

    // ローカル空間バウンディング球の半径（AABB 対角の半分）。視錐台カリング用。
    float GetBoundingRadius() const
    {
        using namespace DirectX;
        const XMVECTOR mn = XMLoadFloat3(&m_aabbMin), mx = XMLoadFloat3(&m_aabbMax);
        return 0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(mx, mn)));
    }

    // 現在の頂点カラー（シーン保存用。キャッシュの先頭頂点を代表値とする）
    DirectX::XMFLOAT4 GetVertexColor() const {
        return m_verticesCache.empty() ? DirectX::XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f}
                                       : m_verticesCache[0].color;
    }
    const std::vector<DirectX::XMFLOAT3>& GetPositions() const { return m_positions; }
    // LOD0 のインデックス配列（CPU コピー）。GetPositions() と対で
    // レイ-三角形判定（エディタの精密ピッキング / ScenePick）に使う。
    // 【なぜ Initialize 時に丸ごとコピーするか】インデックスは DEFAULT ヒープ(VRAM)へ
    // アップロードしたら CPU からは二度と読めない＝「初回ピック時に生成」は原理的に不可能。
    // 252k 三角形でも 3MB で、同じ Mesh を全インスタンスで共有する（アセット単位）ため許容する。
    const std::vector<u32>& GetIndices() const { return m_indicesCache; }

    // UV スケール適用（頂点の texCoord を乗算して VertexBuffer を再作成）
    void ApplyUVScale(GraphicsDevice& device, float scaleU, float scaleV);

    // 頂点カラーを一括設定して VertexBuffer を再作成（Forward.hlsl が albedo に乗算）。
    // GPU アップロード同期を伴うので毎フレームではなく生成時に1度だけ呼ぶこと。
    void SetVertexColor(GraphicsDevice& device, float r, float g, float b, float a = 1.0f);

    // 頂点バッファを作り直すたびに +1 される版数。
    // ★DXR の BLAS キャッシュ用（計画09 §4.2 / R2）。VertexBuffer を作り直すと GPU の
    //   仮想アドレスが変わるので、それを指したままの BLAS は壊れたポインタを読む。
    //   地形/スカルプトのブラシは 1 ストロークごとに UploadVertexCache() を呼ぶので
    //   これを見ないと必ず踏む。BLAS キャッシュは (Mesh*, geometryVersion) をキーにすること。
    u32 GetGeometryVersion() const { return m_geometryVersion; }

    // ---- 計画09 Step 5（バインドレス）------------------------------------
    // レイのヒット点で頂点属性（UV / 法線 / 頂点カラー）を読むための VB/IB の SRV。
    // ★RT を実際に使うフレームで初めて払い出す。RT OFF のプロジェクトでは 1 個も消費しない。
    // ★VB を作り直すと m_geometryVersion が上がるので、そのとき SRV も張り直す
    //   （忘れると RT だけ古いジオメトリを読む。BLAS キャッシュとまったく同じ罠）。
    // 失敗（ヒープ枯渇）時は index が kInvalid のままになるので、呼び出し側は必ず確認すること。
    void EnsureRaytracingSrvs(GraphicsDevice& device, DescriptorHeap& srvHeap);
    u32  GetVbSrvIndex() const { return m_vbSrvIndex; }
    u32  GetIbSrvIndex() const { return m_ibSrvIndex; }

    static const D3D12_INPUT_ELEMENT_DESC* GetInputLayout();
    static u32 GetInputLayoutCount();
    // slot0(頂点) + slot1(MeshInstanceData, PER_INSTANCE) のインスタンシング用レイアウト。
    static const D3D12_INPUT_ELEMENT_DESC* GetInstancedInputLayout();
    static u32 GetInstancedInputLayoutCount();
    // 上記 + slot2(MeshInstancePrevData, PER_INSTANCE) を足した速度パス専用レイアウト。
    // 既存の kInstancedInputLayout は絶対に変更しない（既存 PSO が全部作り直しになる）。
    static const D3D12_INPUT_ELEMENT_DESC* GetVelocityInstancedInputLayout();
    static u32 GetVelocityInstancedInputLayoutCount();

private:
    // 自動LOD生成（Initialize 末尾から呼ぶ。失敗/効果なしなら m_lodCount=1 のまま）
    void GenerateLods(GraphicsDevice& device,
                      const std::vector<Vertex>& vertices,
                      const std::vector<u32>& indices,
                      ID3D12GraphicsCommandList* cmd);

    VertexBuffer m_vertexBuffer;
    IndexBuffer  m_indexBuffer;
    IndexBuffer  m_lodIndexBuffers[kMaxLods - 1];
    u32          m_lodCount = 1;
    Material*    m_material = nullptr;
    DirectX::XMFLOAT3 m_aabbMin = { 0, 0, 0 };
    DirectX::XMFLOAT3 m_aabbMax = { 0, 0, 0 };
    std::vector<DirectX::XMFLOAT3> m_positions; // Convex Hull 用の頂点座標キャッシュ
    std::vector<Vertex> m_verticesCache;         // UV スケール用の頂点データキャッシュ
    std::vector<u32>    m_indicesCache;          // LOD0 インデックスの CPU コピー（レイ-三角形判定用）
    u32                 m_geometryVersion = 0;   // VB を作り直すたびに +1（BLAS キャッシュ無効化用）
    // バインドレス用の SRV（計画09 Step 5）。0xFFFFFFFF = 未払い出し。
    u32                 m_vbSrvIndex = 0xFFFFFFFFu;
    u32                 m_ibSrvIndex = 0xFFFFFFFFu;
    u32                 m_srvGeometryVersion = 0xFFFFFFFFu;   // SRV を張った時点の版
};

} // namespace dx12e
