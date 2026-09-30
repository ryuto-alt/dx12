#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;

// 環境キューブから IBL 派生（irradiance / prefiltered / BRDF LUT）を compute で生成し、
// shader-visible SRV ヒープ上に連続3枚(t5,t6,t7)を確保して保持する。
// 環境キューブが無い場合でも 1x1 ダミーを生成し、テーブルは常に bind 可能（hasIBL で分岐）。
class IBLBaker
{
public:
    // 初期化（compute RootSig / 3 PSO 生成）。1回だけ呼ぶ。
    void Initialize(GraphicsDevice& device, const std::wstring& shaderDirW);

    // シェーダーホットリロード用。3 compute PSO のみ作り直す(ルートシグネチャ/リソースは不変のため触らない)。
    void RecreatePipelines(GraphicsDevice& device);

    // 環境キューブ(envCube, SRV は不要・リソースから内部で SRV を作る)から派生を生成。
    // cmdList は記録のみ（Close/Execute/WaitIdle は呼び出し側）。
    // envCube が null の場合は黒ダミーを生成（hasIBL=false 運用）。
    // SRV 連続3枚は srvHeap.AllocateBlock(3) で確保し、先頭=irradiance。
    void Bake(GraphicsDevice& device, ID3D12GraphicsCommandList* cmdList,
              DescriptorHeap& srvHeap, ID3D12Resource* envCube);

    // ---- 増分再ベイク（物理大気 A1）-----------------------------------------------------------------------------
    // Bake で一度焼いた派生（SRV ブロックは作り直さない）を、環境キューブの中身が変わったときに「段ごと」に焼き直す。
    //   stage 0 = irradiance（拡散）/ stage 1..5 = prefilter の mip 0..4（スペキュラ）。BRDF LUT は環境に依らないので焼き直さない。
    //   1 段 = 1 回の Dispatch（数フレームに割って呼ぶ）。各段は「UAV へ遷移 → 書く → PIXEL_SHADER_RESOURCE へ戻す」を 1 回の
    //   呼び出しの中で完結する（フォワードが同フレームで読んでも状態が合う）。envCube は PIXEL_SHADER_RESOURCE 状態で渡すこと。
    //   frameSlot はディスクリプタの 3 面リング用（0..2。GPU が使用中のディスクリプタを書き換えないため）。
    //   Bake 済みで環境キューブの 1 面サイズが Bake 時と同じときだけ true。それ以外は false（呼び出し側は Bake し直す）。
    static constexpr u32 kRebakeStageCount = 6;
    bool  RebakeStage(GraphicsDevice& device, ID3D12GraphicsCommandList* cmdList, DescriptorHeap& srvHeap,
                      ID3D12Resource* envCube, u32 stage, u32 frameSlot);
    // RebakeStage が確保したディスクリプタを返す（Application の終了時 / SRV ブロックを解放するとき）。
    void  FreeRebakeDescriptors(DescriptorHeap& srvHeap);

    bool  IsValid()            const { return m_valid; }
    bool  HasEnvironment()     const { return m_hasEnv; }
    u32   GetIrradianceSrv()   const { return m_blockStart; }  // ブロック先頭(=t5)
    float GetMaxPrefilterMip() const { return static_cast<float>(kPrefilterMips - 1); }

    // SRV ブロックの先頭/枚数（Application が Free する用）
    u32   GetSrvBlockStart()   const { return m_blockStart; }
    u32   GetSrvBlockCount()   const { return 3; }

    void  Reset();   // Shutdown で device 解放前に呼ぶ

private:
    static constexpr u32 kIrradianceSize = 32;
    static constexpr u32 kPrefilterSize  = 128;
    static constexpr u32 kPrefilterMips  = 5;
    static constexpr u32 kBrdfSize       = 512;

    void CreateDerivedResources(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoIrradiance;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoIrradianceMip;   // 増分再ベイク用（mip 付きの大気環境キューブから軽く畳む。shaders/atmosphere/AtmosphereIrradiance.hlsl）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPrefilter;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBrdf;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_irradianceCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_prefilteredCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_brdfLut;
    // constants 用 upload バッファ（Dispatch ごとに別オフセット）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cbUpload;

    // RecreatePipelines 用に保持
    std::wstring m_shaderDir;

    // 派生3枚の現在ステート。Bake は 1 回きりではなく環境マップ差し替え/シーン切替でも
    // 呼ばれる。前回の末尾で PIXEL_SHADER_RESOURCE にしてあるので、2 回目以降は
    // UAV へ戻してから書く（戻し忘れるとステート不整合でドライバが落ちる）。
    D3D12_RESOURCE_STATES m_derivedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    u32  m_rebakeBlock = 0xFFFFFFFFu;  // RebakeStage 用: (env SRV 1 + UAV 6) × 3 面ぶん。遅延確保
    u32  m_bakedEnvSize = 0;           // 最後の Bake の環境キューブ 1 面サイズ（m_cbUpload の定数と対応）
    u32  m_blockStart = 0xFFFFFFFFu;  // srvHeap 上の連続3枚先頭(=irradiance)
    bool m_valid  = false;
    bool m_hasEnv = false;
    bool m_initialized = false;
};

} // namespace dx12e
