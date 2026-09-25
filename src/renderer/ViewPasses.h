#pragma once
// ===========================================================================
// ViewPasses — Application::RenderView の段のうち、IRenderPass（renderer/RenderPass.h）へ
// 切り出したもの。どれも「1 回の実行ぶんの型付き入力（Inputs）」を持つ軽い値で、
// RenderView がスタックに作って Execute する。GPU 資源は持たない（既存のサブシステムを借りる）。
//
// ★ここにあるのは「状態の張り直し」が呼び出し側に漏れていた段（compute の後の RootSig/PSO/
//   ヒープ再設定、スカイの後の再設定、フォグ構築の前後の影マップ遷移、パーティクル / フォグ合成の
//   深度遷移）。影 / 深度プリパス群 / ポストはまだ RenderView の中（段階的に移す。計画03 の追記参照）。
// ★Forward+ はメッシュを描く中身（描画リスト / PSO 群 / マテリアル）が Application にあるので、
//   パスは「状態を張る」ことだけに責任を持ち、描く中身は drawMeshes（呼び出し側のラムダ）に任せる。
// ===========================================================================
#include <functional>
#include <DirectXMath.h>

#include "renderer/RenderPass.h"
#include "renderer/DdgiVolume.h"
#include "renderer/VolumetricFogPass.h"
#include "renderer/OcclusionCullPass.h"
#include "renderer/RtScreenPass.h"
#include "renderer/ScreenSpaceGiPass.h"

namespace dx12e
{

class ClusteredLightCulling;
class DecalSystem;
class TaaPass;
class HiZPass;
class SSAOPass;
class ContactShadowPass;
struct SSAOSettings;
struct ContactShadowSettings;
struct RtSettings;
struct OcclusionBounds;
class SkyboxRenderer;
class ParticleSystem;
class GpuParticleSystem;
class RenderTarget;
class RootSignature;
class GraphicsDevice;
struct DdgiSettings;
struct VolumetricFogSettings;

// ---- 影マップ（スポット配列 / ポイントのキューブ配列 / CSM の各スライスへ深度だけを描く）------
// 1 枚の深度リソースの複数スライス（DSV）へ、それぞれの viewProj で深度を描く。
// ★影マップの既定の置き場は PIXEL_SHADER_RESOURCE（Forward / フォグが読む）。入口で DEPTH_WRITE へ、
//   出口で戻す（契約どおり）。描く中身（描画リスト / PSO / MASK）は drawDepth（呼び出し側）に任せる。
class ShadowMapPass final : public IRenderPass
{
public:
    struct Slice
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        DirectX::XMFLOAT4X4         viewProj{};      // 非転置
        float                       texelWorld = 0.0f;   // CSM だけ: 1 テクセルが何 m か（遠い LOD 落とし用）
    };
    struct Inputs
    {
        const char*     name    = "ShadowMap";
        ID3D12Resource* map     = nullptr;
        u32             size    = 0;          // スライスは正方形（ビューポート / シザー）
        RootSignature*  rootSig = nullptr;    // 深度パスのルートシグネチャ（メインと共用）
        const Slice*    slices  = nullptr;
        u32             sliceCount = 0;
        std::function<void(const Slice&)> drawDepth;
    };
    explicit ShadowMapPass(Inputs in) : m_in(std::move(in)) {}
    const char* Name() const override { return m_in.name; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ===========================================================================================
// 深度プリパス群（カメラ視点の深度を先に完成させ、それを読む画面空間のパス）
// ★深度の状態は ctx.depth に対して入口で Require するだけ（プリパス = DEPTH_WRITE /
//   Hi-Z = NON_PIXEL / それ以外 = PIXEL）。群の最後でビューが DEPTH_WRITE へ戻す。
//   旧コードは「プリパス後に PIXEL へ、Hi-Z の前後で NON_PIXEL と往復、最後に DEPTH_WRITE へ」を
//   手で書いていた（誰も深度を読まないフレームでも往復していた）。
// ===========================================================================================

// ---- 深度プリパス（深度のみ / 深度 + 速度 + G-Buffer の 2 モード。00-COORDINATION §5.5）------
class DepthPrepassPass final : public IRenderPass
{
public:
    struct Inputs
    {
        RootSignature*              rootSig = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
        u32  width = 0, height = 0;
        bool velocityGBuffer = false;          // true = RTV0 速度（TaaPass 所有）+ RTV1 G-Buffer
        RenderTarget*               gbuffer = nullptr;
        TaaPass*                    taa     = nullptr;
        std::function<void()>       drawDepth; // RenderDepthOnlyScene（呼び出し側）
    };
    explicit DepthPrepassPass(Inputs in) : m_in(std::move(in)) {}
    const char* Name() const override { return "DepthPrepass"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- Hi-Z 深度ピラミッド + オクルージョン判定（compute）--------------------------------
class HiZOcclusionPass final : public IRenderPass
{
public:
    struct Inputs
    {
        HiZPass*                            hiz       = nullptr;
        OcclusionCullPass*                  occlusion = nullptr;   // null なら判定しない
        GraphicsDevice*                     device    = nullptr;
        const std::vector<OcclusionBounds>* bounds    = nullptr;
        OcclusionCullPass::Params           params{};
        D3D12_GPU_DESCRIPTOR_HANDLE         depthSrv{};
    };
    explicit HiZOcclusionPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "HiZOcclusion"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- SSAO（深度 → AO → ブラー）---------------------------------------------------------
class SsaoGeneratePass final : public IRenderPass
{
public:
    struct Inputs
    {
        SSAOPass*                   ssao = nullptr;
        const SSAOSettings*         settings = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};
        DirectX::XMFLOAT4X4         proj{};              // ジッタなし
        float zNear = 0.1f, zFar = 1000.0f;
        u32   width = 0, height = 0;
    };
    explicit SsaoGeneratePass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "Ssao"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;
    u32  ResultSrv() const { return m_result; }   // 失敗時 kInvalidIndex

private:
    Inputs m_in;
    u32    m_result = 0xFFFFFFFFu;
};

// ---- コンタクトシャドウ（同じ深度を太陽方向へレイマーチ）-------------------------------
class ContactShadowGeneratePass final : public IRenderPass
{
public:
    struct Inputs
    {
        ContactShadowPass*           pass = nullptr;
        const ContactShadowSettings* settings = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE  depthSrv{};
        DirectX::XMFLOAT4X4          view{}, proj{};     // ジッタなし
        DirectX::XMFLOAT3            lightDir{};
        u32 width = 0, height = 0;
    };
    explicit ContactShadowGeneratePass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "ContactShadow"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;
    u32  ResultSrv() const { return m_result; }

private:
    Inputs m_in;
    u32    m_result = 0xFFFFFFFFu;
};

// ---- DXR の画面パス（RT サン影 / RT-AO / デバッグ。深度 + TLAS）-------------------------
class RtScreenGeneratePass final : public IRenderPass
{
public:
    struct Inputs
    {
        RtScreenPass*                      rt = nullptr;
        const RtScreenPass::GenerateDesc*  desc = nullptr;   // XMMATRIX を含むので借りる
        const RtSettings*                  settings = nullptr;
        bool shadow = false, ao = false, debug = false, debugAlbedo = false;
    };
    explicit RtScreenGeneratePass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "RtScreen"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;
    // 生成できなかったものは kInvalidIndex（呼び出し側は既定の枠を使い続ける）
    u32 ShadowSrv() const { return m_shadow; }
    u32 AoSrv()     const { return m_ao; }
    u32 DebugSrv()  const { return m_debug; }

private:
    Inputs m_in;
    u32 m_shadow = 0xFFFFFFFFu, m_ao = 0xFFFFFFFFu, m_debug = 0xFFFFFFFFu;
};

// ---- SSR / SSGI（深度 + G-Buffer + 速度 + 前フレームカラーをレイマーチ）-------------------
// run=false でも Execute は呼ぶ（GPU 計測の区間を旧コードと同じに保つ）。
class ScreenSpaceGiGeneratePass final : public IRenderPass
{
public:
    struct Inputs
    {
        ScreenSpaceGiPass*                     gi = nullptr;
        const ScreenSpaceGiPass::GenerateDesc* desc = nullptr;   // XMMATRIX を含むので借りる
        bool run = false;
    };
    explicit ScreenSpaceGiGeneratePass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "ScreenSpaceGi"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;
    u32 SsrSrv()  const { return m_ssr; }
    u32 SsgiSrv() const { return m_ssgi; }

private:
    Inputs m_in;
    u32 m_ssr = 0xFFFFFFFFu, m_ssgi = 0xFFFFFFFFu;
};

// ---- クラスタライトカリング（compute 2 パス）-------------------------------------------
// cull=false（正射 / 設定 OFF / 副ビュー）でもテーブルはバインドされるので読取状態にだけする。
class ClusterCullPass final : public IRenderPass
{
public:
    struct Inputs
    {
        ClusteredLightCulling* culling = nullptr;
        bool                   cull    = false;
        DirectX::XMFLOAT4X4    view{};           // ジッタなし・非転置
        float proj11 = 1.0f, proj22 = 1.0f;      // 投影行列の _11 / _22
        float zNear = 0.1f, zFarCluster = 500.0f, zFarCamera = 1000.0f;
        u32   lightCount = 0;                    // UploadLights が返した灯数
    };
    explicit ClusterCullPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "ClusterCull"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- DDGI のプローブ更新（compute。ワールド空間なのでフレームで 1 回＝主ビューだけ）------
class DdgiUpdatePass final : public IRenderPass
{
public:
    struct Inputs
    {
        DdgiVolume*             ddgi     = nullptr;
        GraphicsDevice*         device   = nullptr;
        const DdgiSettings*     settings = nullptr;
        DdgiVolume::UpdateDesc  desc{};
    };
    explicit DdgiUpdatePass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "DdgiUpdate"; }
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- デカールのクラスタへのビニング（compute）----------------------------------------
// アトラスの解決・SRV の書き込み・Upload は CPU の仕事なので呼び出し側に残してある。
class DecalCullPass final : public IRenderPass
{
public:
    struct Inputs
    {
        DecalSystem*        decals = nullptr;
        bool                cull   = false;     // false = 読取状態にするだけ
        DirectX::XMFLOAT4X4 view{};
        float proj11 = 1.0f, proj22 = 1.0f;
        float zNear = 0.1f, zFarCluster = 500.0f, zFarCamera = 1000.0f;
        u32   decalCount = 0;                   // Upload が返した個数
    };
    explicit DecalCullPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "DecalCull"; }
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- ボリュメトリックフォグ: froxel ボリュームの構築（compute 3 パス）--------------------
// 影マップ 3 種を compute から読むので NON_PIXEL へ遷移し、終わったら PIXEL へ戻す（出口の契約）。
class VolumetricFogBuildPass final : public IRenderPass
{
public:
    struct Inputs
    {
        VolumetricFogPass*            fog      = nullptr;
        GraphicsDevice*               device   = nullptr;
        const VolumetricFogSettings*  settings = nullptr;
        // ★値で持たない（XMMATRIX を含むので 16B 整列が要り、パスのオブジェクトに詰め物の
        //   警告 C4324 が出る）。呼び出し側のローカルを Execute の間だけ借りる。
        const VolumetricFogPass::ViewParams* params = nullptr;
        ID3D12Resource* csm          = nullptr;   // 既定の置き場は PIXEL_SHADER_RESOURCE
        ID3D12Resource* spotShadows  = nullptr;
        ID3D12Resource* pointShadows = nullptr;
    };
    explicit VolumetricFogBuildPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "VolumetricFogBuild"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- ボリュメトリックフォグ: 合成（フルスクリーン 1 枚をブレンドでシーン RT へ）----------
// 深度を SRV で読む（DSV は張らない）。
class FogCompositePass final : public IRenderPass
{
public:
    struct Inputs
    {
        VolumetricFogPass*          fog = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv{};
        ID3D12Resource*             sceneColor = nullptr;   // 宣言用
        u32 width = 0, height = 0;
    };
    explicit FogCompositePass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "FogComposite"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- スカイボックス（不透明の前に全画面を塗る。深度テスト OFF）---------------------------
class SkyboxPass final : public IRenderPass
{
public:
    struct Inputs
    {
        SkyboxRenderer*             sky = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE envCube{};
        DirectX::XMFLOAT4X4         invViewProjT{};   // ジッタ込み viewProj の逆行列（転置済み）
        float                       intensity = 1.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};       // PSO は深度を使わないが、Forward と同じ束ね方にする
        ID3D12Resource*             sceneColor = nullptr;   // 宣言用
        u32 width = 0, height = 0;
    };
    explicit SkyboxPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "Skybox"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- Forward+（不透明 / MASK / BLEND のメッシュ）---------------------------------------
// ★このパスの責任は「Forward の PS が読むものを全部張る」こと（ヒープ / ルートシグネチャ / RT /
//   ビューポート / b1 / 影 / スポット・ポイント影 / IBL / クラスタ）。AO / コンタクトシャドウ /
//   SSR / SSGI とマテリアル・ボーンはメッシュごとに drawMeshes の中（RenderSceneMeshes）が張る。
class ForwardScenePass final : public IRenderPass
{
public:
    struct Inputs
    {
        RootSignature*              rootSig = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS   perFrameCB = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE csmTable{};
        D3D12_GPU_DESCRIPTOR_HANDLE punctualShadowTable{};   // t9,t10（スポット配列 → ポイントキューブ配列）
        bool                        hasIblTable = false;
        D3D12_GPU_DESCRIPTOR_HANDLE iblTable{};
        bool                        hasClusterTable = false;
        D3D12_GPU_DESCRIPTOR_HANDLE clusterTable{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
        ID3D12Resource*             sceneColor = nullptr;   // 宣言用
        u32 width = 0, height = 0;
        std::function<void()>       drawMeshes;             // 描く中身（呼び出し側）
    };
    explicit ForwardScenePass(Inputs in) : m_in(std::move(in)) {}
    const char* Name() const override { return "ForwardScene"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

private:
    Inputs m_in;
};

// ---- パーティクル（CPU ビルボード + GPU パーティクル + 歪みバッファ）----------------------
// 深度は SRV で読む（soft particles。DSV は張らない）。
class ParticlesPass final : public IRenderPass
{
public:
    struct Inputs
    {
        ParticleSystem*             particles    = nullptr;
        GpuParticleSystem*          gpuParticles = nullptr;   // 無ければ飛ばす
        RenderTarget*               distortRT    = nullptr;   // 無ければ歪みを飛ばす
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv{};
        ID3D12Resource*             sceneColor = nullptr;     // 宣言用
        u32 width = 0, height = 0;                            // ビューポート
        bool                        hasDepthSrv = false;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};
        float projA = 0.0f, projB = 0.0f;                     // 投影の _33 / _43（深度の線形化）
        float rtWidth = 1.0f, rtHeight = 1.0f;                // シーン RT の実寸（soft particles の UV）
        DirectX::XMFLOAT4X4 viewProjJittered{};
        DirectX::XMFLOAT3   camRight{}, camUp{}, camPos{};
        float time = 0.0f;
        float gpuDt = 0.0f;                                   // GPU 粒子のシム dt（決定論キャプチャ中は 0）
    };
    explicit ParticlesPass(const Inputs& in) : m_in(in) {}
    const char* Name() const override { return "Particles"; }
    void DeclareResources(std::vector<PassResourceUse>& out) const override;
    void Execute(const RenderPassContext& ctx) override;

    // 歪みバッファへ描いたか（ポストの uber が読むかどうか）。Execute の後に読む。
    bool DistortionDrawn() const { return m_distortDrawn; }

private:
    Inputs m_in;
    bool   m_distortDrawn = false;
};

} // namespace dx12e
