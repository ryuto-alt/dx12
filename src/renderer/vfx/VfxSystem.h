#pragma once
// ===========================================================================
// VfxSystem（W1-BD4 / UnoFX）: GPU リソース、アセットの読み込み、インスタンスの管理、シミュレーションと描画の記録。
//   VfxFeature（IRenderFeature）が Stage ごとに Update / Draw を呼ぶ。
//   * VfxComponent を持つエンティティが 1 つも無く、インスタンスも無いフレームは、コマンドを 1 つも記録しない
//     （GPU リソースも最初の 1 個が現れるまで作らない）。
//   * 粒子は 1 本のプール（kPoolCapacity 個）の中の、インスタンスごとの連続区間（リング）に置く。
// ===========================================================================
#include <d3d12.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "core/Types.h"
#include "renderer/RenderStageHooks.h"
#include "renderer/vfx/VfxAsset.h"
#include "renderer/vfx/VfxSpawnMath.h"

namespace dx12e
{
struct VfxComponent;
}

namespace dx12e::vfx
{

class VfxSystem
{
public:
    VfxSystem();
    ~VfxSystem();

    void Update(StageCtx& ctx);   // AfterDepthPrepass（主ビューだけ）
    void Draw(StageCtx& ctx);     // AfterTranslucent（どのビューでも）
    u64  VramBytes() const { return m_vram; }

private:
    struct AssetEntry
    {
        bool        ok = false;
        std::string error;
        VfxAsset    asset;
        struct Em { u32 initStart = 0, initCount = 0, updStart = 0, updCount = 0; float maxLife = 1.0f; };
        std::vector<Em> em;
    };

    struct Instance
    {
        entt::entity entity = entt::null;
        u32          emitter = 0;
        std::string  assetPath;
        u32          base = 0, cap = 0, statSlot = 0;
        SpawnClock   clk;
        float        prevPos[3] = {0, 0, 0};
        bool         havePrev = false;
        bool         paused = false;
        bool         finished = false;
        bool         touched = false;
        u32          seed = 0;
    };

    // ---- 初期化 ----
    bool EnsureGpu(StageCtx& ctx);
    bool CreatePipelines(ID3D12Device* dev);
    bool CreateBuffers(ID3D12Device* dev, StageCtx& ctx);
    bool CreateCurlField(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, StageCtx& ctx);

    // ---- アセット / インスタンス ----
    AssetEntry* GetAsset(const std::string& path);
    Instance*   FindOrCreate(entt::entity e, u32 emitter, const std::string& assetPath, u32 cap, u32 seed, bool autoplay);
    void        Release(Instance& inst);
    bool        AllocRange(u32 size, u32& base);
    void        FreeRange(u32 base, u32 size);

    // ---- 記録 ----
    struct StepCtx;
    void BeginCompute(StageCtx& ctx);
    void RecordClear(ID3D12GraphicsCommandList* cmd, const Instance& inst);
    void RecordStep(ID3D12GraphicsCommandList* cmd, Instance& inst, const AssetEntry& a, const EmitterDesc& em,
                    const AssetEntry::Em& aem, const float rows[12], const float curPos[3],
                    double stepDt, bool isLast, float emissionMul);
    void UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r);
    void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void PublishStats(ID3D12GraphicsCommandList* cmd);

    // ---- GPU ----
    bool m_gpuTried = false, m_gpuReady = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_simRS, m_drawRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoEmit, m_psoSim, m_psoClear, m_psoStatClear, m_psoDraw;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pool, m_stats, m_opsUpload, m_curl, m_dummyDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readback[4];
    GOp*  m_opsMapped = nullptr;
    u32   m_opsUsed = 0;
    u32   m_curlSrv = 0xFFFFFFFFu, m_dummyDepthSrv = 0xFFFFFFFFu;
    u32*  m_readbackMapped[4] = {nullptr, nullptr, nullptr, nullptr};
    u32   m_readbackWritten = 0;
    D3D12_RESOURCE_STATES m_poolState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_statsState = D3D12_RESOURCE_STATE_COMMON;
    u64   m_vram = 0;
    bool  m_computeBound = false;

    // ---- CPU ----
    std::map<std::string, AssetEntry> m_assets;
    std::map<u64, Instance> m_instances;
    struct Range { u32 base, size; bool used; };
    std::vector<Range> m_ranges;
    bool m_statUsed[256] = {};
    u32  m_frame = 0;
    u32  m_alive[256] = {};   // 読み戻した生存数（数フレーム遅れ）
};

} // namespace dx12e::vfx
