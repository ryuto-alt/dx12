#pragma once
// ===========================================================================
// PtSceneBuilder — パストレーサー専用のシーン(BLAS / TLAS / 各テーブル)を組む。
// ---------------------------------------------------------------------------
// なぜ RaytracingScene の TLAS を使わないのか
//   ・RaytracingScene は半透明 / アルファテスト(MASK)を TLAS から外し、全ジオメトリを OPAQUE で作る
//     (RT 影 / RT-AO / DDGI 向けの排他ハイブリッド)。パストレーサーは全部を光輸送に入れる必要がある。
//   ・ヒット点で引く材質(metallic / roughness / 法線 / MR / emissive / 不透明度)は GeometryInfo(16B)に無い。
//   ・ジョブは数分〜数十分続く。共有 TLAS / BLAS キャッシュはその間に別の用途で作り直される
//     (メッシュの解放 = RemoveBlas / Invalidate)ので、ジョブ開始時点の【スナップショット】を専用に持つ。
//   → BLAS も TLAS もここで持つ。GPU メモリ = BLAS(メッシュ単位で重複排除)+ TLAS + テーブル。
//
// D3D12 デバイスだけで動く(GraphicsDevice / DescriptorHeap に依存しない)。tests/pt_*.cpp が窓なしで使う。
// 頂点・インデックスの GPU バッファと raw SRV(バインドレスの添字)は【呼び出し側】が用意して渡す。
//
// 使い方
//   Init → AddGeometry / AddMaterial / AddInstance / AddLight を必要なだけ → BeginBuild
//   → (毎フレーム) BuildStep(cmd, 三角形予算) が true になるまで → FinishBuild(cmd)
//   → GetScene() を PathTracer::BeginJob へ。ジョブが終わるまでこのオブジェクトを生かしておくこと。
//   ★BuildStep / FinishBuild が積んだコマンドの実行完了後に ReleaseTransient() を呼ぶとステージング / スクラッチを返す。
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderer/pt/PtShared.h"

namespace dx12e::pt
{

struct GeometryDesc
{
    uint64_t key = 0;                        // 重複排除キー(0 = しない)。同じメッシュの複数インスタンスは同じ BLAS を使う
    D3D12_GPU_VIRTUAL_ADDRESS vbVA = 0;      // 頂点バッファ(位置がオフセット 0 の float3)
    uint32_t vbStride = 96;
    uint32_t vertexCount = 0;
    D3D12_GPU_VIRTUAL_ADDRESS ibVA = 0;      // u32 インデックス
    uint32_t indexCount = 0;
    uint32_t vbSrv = kNoIndex;               // 属性を引くための raw SRV(96B インターリーブ VB)
    uint32_t ibSrv = kNoIndex;               // raw SRV(u32 インデックス)
    // エミッシブ三角形の面積計算用(任意)。無ければこのジオメトリのエミッシブは NEE に入らない(BSDF ヒットでのみ寄与)。
    const DirectX::XMFLOAT3* cpuPositions = nullptr;   // vertexCount 個
    const uint32_t*          cpuIndices   = nullptr;   // indexCount 個
};

struct InstanceDesc
{
    uint32_t geometry = 0;                   // AddGeometry の戻り値
    DirectX::XMFLOAT4X4 world{};             // 行ベクトル規約(XMMATRIX を XMStoreFloat4x4 したもの)
    uint32_t material = 0;
    // スキンド: 変形後位置(float3 詰め = SkinningCompute の出力)のバッファのアドレス。AddInstance 時点では
    // 【元(フレームごとに書き換わる)】を渡す。RecordSkinnedCopies がジョブ専用バッファへ写して差し替える
    // (BLAS も、シェーダが頂点を引く raw SRV も、写しの方を指す＝ジョブ中にポーズが変わっても一貫する)。
    D3D12_GPU_VIRTUAL_ADDRESS deformedVbVA = 0;
    uint32_t deformedSrv = kNoIndex;         // 写しの raw SRV(RecordSkinnedCopies が作る。呼び出し側は kNoIndex のまま渡す)
    // エミッシブの NEE 用の「面積あたり重み」(= 放射輝度の輝度)。0 なら NEE に入れない。
    float emissiveLuma = 0.0f;
};

// GPU に渡すものの束。PathTracer::BeginJob が読む。
struct SceneGpu
{
    D3D12_GPU_VIRTUAL_ADDRESS tlas = 0;
    D3D12_GPU_VIRTUAL_ADDRESS instances = 0;
    D3D12_GPU_VIRTUAL_ADDRESS materials = 0;
    D3D12_GPU_VIRTUAL_ADDRESS lights = 0;
    D3D12_GPU_VIRTUAL_ADDRESS emissive = 0;
    uint32_t instanceCount = 0;
    uint32_t materialCount = 0;
    uint32_t lightCount = 0;
    uint32_t emissiveCount = 0;
    bool valid = false;
};

struct BuilderStats
{
    uint32_t instances = 0;
    uint32_t materials = 0;
    uint32_t lights = 0;
    uint32_t blasCount = 0;
    uint64_t blasBytes = 0;
    uint64_t tlasBytes = 0;
    uint64_t triangles = 0;          // TLAS に入る三角形の総数(インスタンス展開後)
    uint64_t blasTriangles = 0;      // BLAS 化した三角形(重複排除後)
    uint32_t emissiveTris = 0;       // NEE 用の三角形表の要素数
    uint32_t emissiveInstances = 0;  // NEE に入ったエミッシブインスタンス数
    uint32_t emissiveSkipped = 0;    // 上限超過などで NEE に入れなかったエミッシブインスタンス数
    uint32_t nonOpaqueInstances = 0; // MASK / BLEND(any-hit 相当を評価する)
    uint32_t skinnedInstances = 0;
    double   emissivePower = 0.0;    // Σ 面積 × 輝度(診断用)
};

class SceneBuilder
{
public:
    // エミッシブ三角形表の上限(1 要素 32B)。超えたインスタンスは NEE に入れない。
    static constexpr uint32_t kMaxEmissiveTris = 1u << 20;
    static constexpr uint32_t kMaxInstances    = 1u << 22;   // InstanceID は 24bit

    // shaderDir は PtCopy_CS.cso の置き場(スキンドを使うときだけ要る。空でもよい)。
    bool Init(ID3D12Device5* device, const std::wstring& shaderDir = std::wstring());
    void Reset();
    // 変形後頂点の写しに raw SRV(バインドレス)を 1 つ作って添字を返す関数。呼び出し側がヒープを持つ。
    // 失敗したら kNoIndex を返す(そのスキンドインスタンスは TLAS から外れる)。
    using RawSrvFactory = std::function<uint32_t(ID3D12Resource* buffer, uint64_t bytes)>;
    void SetRawSrvFactory(RawSrvFactory f) { m_srvFactory = std::move(f); }
    // スキンドインスタンスの変形後位置を専用バッファへ写す(BeginBuild の前・元バッファが有効なフレームの cmd へ)。
    // ★元の書き込みが同じコマンドリストで済んでいる(NON_PIXEL_SHADER_RESOURCE へ遷移済み)こと。
    bool RecordSkinnedCopies(ID3D12GraphicsCommandList4* cmd, std::string* err);

    uint32_t AddGeometry(const GeometryDesc& g);
    uint32_t AddMaterial(const MaterialGpu& m);
    uint32_t AddInstance(const InstanceDesc& d);
    void     AddLight(const LightGpu& l);

    // ビルド開始(BLAS の待ち行列を確定する)。以後 Add* は呼ばない。
    bool BeginBuild(std::string* err);
    // BLAS を三角形予算いっぱいまで cmd に積む。全部積んだら true。
    bool BuildStep(ID3D12GraphicsCommandList4* cmd, uint64_t triangleBudget);
    // TLAS とテーブルを積む。BuildStep が true を返した後に 1 回だけ。
    bool FinishBuild(ID3D12GraphicsCommandList4* cmd, std::string* err);
    // ステージング / スクラッチを解放する。★積んだコマンドの実行完了後に呼ぶこと。
    void ReleaseTransient();

    const SceneGpu& GetScene() const { return m_scene; }
    const BuilderStats& GetStats() const { return m_stats; }
    uint32_t GetMaterialCount() const { return static_cast<uint32_t>(m_materials.size()); }
    const std::vector<MaterialGpu>& Materials() const { return m_materials; }

private:
    struct Geometry
    {
        GeometryDesc desc;
        int blas = -1;                // m_blas の添字(静的の共有 BLAS)
    };
    struct Blas
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        D3D12_GPU_VIRTUAL_ADDRESS vbVA = 0;
        uint32_t vbStride = 0;
        uint32_t vertexCount = 0;
        D3D12_GPU_VIRTUAL_ADDRESS ibVA = 0;
        uint32_t indexCount = 0;
        uint64_t size = 0;
        uint64_t scratch = 0;
        bool built = false;
    };
    struct InstanceRec
    {
        InstanceDesc desc;
        int blas = -1;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name);
    // DEFAULT バッファを作って data を送る(ステージングは m_staging に保持)。終端状態 = NON_PIXEL_SHADER_RESOURCE。
    Microsoft::WRL::ComPtr<ID3D12Resource> UploadTable(ID3D12GraphicsCommandList4* cmd, const void* data,
        uint64_t bytes, const wchar_t* name);
    void BuildEmissiveTable(std::vector<EmissiveTriGpu>& out);

    ID3D12Device5* m_device = nullptr;
    std::wstring   m_shaderDir;
    RawSrvFactory  m_srvFactory;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_copyRs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_copyPso;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_skinCopies;   // 写し(ジョブの間ずっと生かす)
    std::vector<Geometry>    m_geometries;
    std::unordered_map<uint64_t, uint32_t> m_geometryByKey;
    std::vector<MaterialGpu> m_materials;
    std::vector<InstanceRec> m_instances;
    std::vector<LightGpu>    m_lights;
    std::vector<Blas>        m_blas;
    size_t                   m_blasCursor = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_scratch;
    uint64_t                 m_scratchSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_tlas, m_tlasScratch, m_instanceDescs;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceTable, m_materialTable, m_lightTable, m_emissiveTable;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_staging;
    SceneGpu     m_scene;
    BuilderStats m_stats;
    std::vector<InstanceGpu> m_instanceGpu;      // FinishBuild 中に作る(emissiveBase を入れるため)
    std::vector<uint32_t>    m_emissiveBase;     // インスタンスごとの emissiveBase
};

} // namespace dx12e::pt
