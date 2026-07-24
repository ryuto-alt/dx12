#pragma once

#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "graphics/Buffer.h"

namespace dx12e
{

struct Material;

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

class Mesh
{
public:
    // cmd を渡すと VB/IB(LOD含む) を DEFAULT ヒープ(VRAM)に作りコピーを cmd に積む
    // （モデルロード経路は必ず渡すこと。UPLOAD 直読みは大型メッシュで GPU が桁違いに遅い）。
    void Initialize(GraphicsDevice& device,
                    const std::vector<Vertex>& vertices,
                    const std::vector<u32>& indices,
                    ID3D12GraphicsCommandList* cmd = nullptr);

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

    // UV スケール適用（頂点の texCoord を乗算して VertexBuffer を再作成）
    void ApplyUVScale(GraphicsDevice& device, float scaleU, float scaleV);

    // 頂点カラーを一括設定して VertexBuffer を再作成（Forward.hlsl が albedo に乗算）。
    // GPU アップロード同期を伴うので毎フレームではなく生成時に1度だけ呼ぶこと。
    void SetVertexColor(GraphicsDevice& device, float r, float g, float b, float a = 1.0f);

    static const D3D12_INPUT_ELEMENT_DESC* GetInputLayout();
    static u32 GetInputLayoutCount();
    // slot0(頂点) + slot1(MeshInstanceData, PER_INSTANCE) のインスタンシング用レイアウト。
    static const D3D12_INPUT_ELEMENT_DESC* GetInstancedInputLayout();
    static u32 GetInstancedInputLayoutCount();

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
};

} // namespace dx12e
