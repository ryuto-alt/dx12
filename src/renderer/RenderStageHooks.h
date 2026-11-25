#pragma once
// ===========================================================================
// 描画ステージのフック（W05-25 / PROGRAM_PLAN W0.5-α3 R-00a）。
//   ApplicationRender.cpp の既存のパスとパスの間に 14 か所の「ステージ」を置き、
//   IRenderFeature を登録した機能がそこへ描画コマンドを足せるようにする。
//   ★既存のパスの中身は変えない。登録が 0 件のステージは何も記録しない（コマンドは 1 命令も増えない）。
//   ★このヘッダは Application.h から include されない（Application.h は前方宣言と削除関数だけを持つ）。
//
// 契約（Record の中の機能が守ること）:
//   - 入口でパイプライン状態（RootSig / PSO / RT / ビューポート / ヒープ）について何も仮定しない。使うものは自分で張る。
//   - 出口で状態は戻さなくてよい（呼び出し側が SRV ヒープだけ張り直す）。リソースは既定の置き場へ戻す。
//   - StageCtx のポインタ / ハンドルは、0 / null のとき「このステージのこの位置では得られない」の意味。必ず確認する。
// ===========================================================================
#include "core/Types.h"

#include <directx/d3d12.h>
#include <DirectXMath.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace dx12e
{

class DescriptorHeap;
class GpuTimer;
class ContactShadowWriter;   // 画面空間の影の書き込み口（W1 の C が定義する。それまでは null）
class ForwardPsoProvider;    // shadingRevision と debugView で PSO を差し替える入口（W1 の C が定義する。それまでは null）
struct ViewDesc;             // renderer/ViewDesc.h
struct DeviceCaps;           // graphics/DeviceCaps.h（W05-04）
struct FrameConstantsData;   // renderer/FrameConstants.h

enum class Stage : u8
{
    AfterDepthPrepass,
    BeforeShadows,
    AfterShadows,
    BeforeForwardOpaque,
    AfterForwardOpaque,
    BeforeTranslucent,
    AfterTranslucent,
    BeforeTemporal,
    AfterUpscale,
    PostPreBloom,
    PostPreUber,
    PostPostUber,
    EditorOverlay,
    EditorPreview,
    Count
};
inline constexpr u32 kStageCount = static_cast<u32>(Stage::Count);
static_assert(kStageCount == 14, "ステージは 14 個");

using StageMask = u32;
constexpr StageMask StageBit(Stage s) { return 1u << static_cast<u32>(s); }
inline constexpr StageMask kAllStages = (1u << kStageCount) - 1u;

inline const char* StageName(Stage s)
{
    switch (s)
    {
    case Stage::AfterDepthPrepass:   return "AfterDepthPrepass";
    case Stage::BeforeShadows:       return "BeforeShadows";
    case Stage::AfterShadows:        return "AfterShadows";
    case Stage::BeforeForwardOpaque: return "BeforeForwardOpaque";
    case Stage::AfterForwardOpaque:  return "AfterForwardOpaque";
    case Stage::BeforeTranslucent:   return "BeforeTranslucent";
    case Stage::AfterTranslucent:    return "AfterTranslucent";
    case Stage::BeforeTemporal:      return "BeforeTemporal";
    case Stage::AfterUpscale:        return "AfterUpscale";
    case Stage::PostPreBloom:        return "PostPreBloom";
    case Stage::PostPreUber:         return "PostPreUber";
    case Stage::PostPostUber:        return "PostPostUber";
    case Stage::EditorOverlay:       return "EditorOverlay";
    case Stage::EditorPreview:       return "EditorPreview";
    default:                         return "?";
    }
}

// クラスタードライティングへ足す灯（AppendClusterLights の引数）。
struct ClusterLight
{
    DirectX::XMFLOAT3 position{};
    float             range = 0.0f;
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};
    float             intensity = 1.0f;
};

// F3（レンダーグラフ）へ移すための宣言の口。今は何も受け取らない。
struct StageResourceDecl
{
};

struct StageCtx
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    const ViewDesc*            view = nullptr;           // 主ビュー / 副ビュー。オーバーレイなど ViewDesc を持たない位置では null
    u32                        internalW = 0, internalH = 0;   // レンダー解像度
    u32                        outW = 0, outH = 0;             // 出力（ポスト）解像度
    u32                        frameIndex = 0;
    bool                       primary = true;

    // 描画先と、既に出来ているもの（0 / null = この位置ではまだ無い）
    ID3D12Resource*             sceneColor = nullptr;      // HDR のシーン RT（レンダー解像度）
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv{};
    D3D12_GPU_DESCRIPTOR_HANDLE sceneColorSrv{};           // ポストのステージでは「今のチェーンの絵」の SRV
    ID3D12Resource*             depth = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};
    D3D12_GPU_DESCRIPTOR_HANDLE hiZSrv{};                  // 未提供なら ptr==0
    D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv{};             // 未提供なら ptr==0
    D3D12_GPU_VIRTUAL_ADDRESS   perFrameGpuVa = 0;         // b1（PerFrameConstants）。作る前のステージでは 0
    FrameConstantsData*         perFrameCpu = nullptr;     // PfSlot 経由で書く。作る前のステージでは null

    DescriptorHeap*             descriptors = nullptr;     // シェーダ可視の CBV/SRV/UAV ヒープ（ctx 作成時点で張られている）
    GpuTimer*                   timer = nullptr;           // 主ビューだけ非 null
    ContactShadowWriter*        contactShadow = nullptr;
    ForwardPsoProvider*         forwardPso = nullptr;
    const DeviceCaps*           caps = nullptr;            // W05-04 の DeviceCaps。機能がバリアの方式を選ぶために読む（F1 以降）
    ID3D12CommandQueue*         asyncCompute = nullptr;    // W4 の ASYNC-1 で入れる。それまでは null

    // 灯をクラスタへ足す口（自分の self を渡して呼ぶ。null なら未提供）
    void (*AppendClusterLights)(void* self, const ClusterLight* lights, u32 n) = nullptr;
    void* self = nullptr;
    // 主ビューだけ有効（副ビューでは無視される）。呼び出し側が結線する。
    void (*SetTexMipBiasFn)(void* self, float bias) = nullptr;
    // 速度プリパスを必ず走らせる要求（W5 の PI-08 が使う）。true を書くと次の判定で有効になる。
    bool* velocityPrepassRequest = nullptr;

    void SetTexMipBias(float b) { if (SetTexMipBiasFn && primary) SetTexMipBiasFn(self, b); }
    void RequestVelocityPrepass() { if (velocityPrepassRequest) *velocityPrepassRequest = true; }
};

struct IRenderFeature
{
    virtual ~IRenderFeature() = default;
    virtual const char* Name() const = 0;
    virtual StageMask   Stages() const = 0;               // どのステージで呼ぶか（ビットマスク）
    virtual i32         Priority(Stage) const = 0;        // StagePriority.h の帯から選ぶ
    virtual void        OnResize(u32 internalW, u32 internalH, u32 outW, u32 outH)
    {
        (void)internalW; (void)internalH; (void)outW; (void)outH;
    }
    virtual void        DeclareResources(StageResourceDecl&) {}   // F3 でレンダーグラフへ移すための口。今は何もしない
    virtual void        Record(Stage, StageCtx&) = 0;
    virtual u64         VramBytes() const { return 0; }
};

class FeatureRegistry
{
public:
    // 登録。Stages() と Priority() はここで 1 回だけ読んで表にする（以後の呼び出しで再評価しない）。
    void Add(std::unique_ptr<IRenderFeature> f)
    {
        if (!f) return;
        const u32 order = m_nextOrder++;
        const StageMask mask = f->Stages();
        IRenderFeature* raw = f.get();
        m_owned.push_back(std::move(f));
        for (u32 s = 0; s < kStageCount; ++s)
        {
            if (!(mask & (1u << s))) continue;
            auto& list = m_perStage[s];
            list.push_back(Entry{raw->Priority(static_cast<Stage>(s)), order, raw});
            std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b)
                             { return a.priority != b.priority ? a.priority < b.priority : a.order < b.order; });
        }
    }

    // 登録が 0 件のステージは何もしない（true なら Run を呼ぶ必要が無い）。
    bool Empty(Stage s) const { return m_perStage[static_cast<u32>(s)].empty(); }
    bool Empty() const { return m_owned.empty(); }
    size_t Count() const { return m_owned.size(); }

    void Run(Stage s, StageCtx& ctx)
    {
        for (const Entry& e : m_perStage[static_cast<u32>(s)])
            e.feature->Record(s, ctx);
    }

    // 解像度が変わったときだけ全機能へ通知する（変化が無ければ何もしない。初回は必ず通知する）。
    void SyncResize(u32 internalW, u32 internalH, u32 outW, u32 outH)
    {
        if (m_resizeKnown && m_iw == internalW && m_ih == internalH && m_ow == outW && m_oh == outH) return;
        m_resizeKnown = true;
        m_iw = internalW; m_ih = internalH; m_ow = outW; m_oh = outH;
        for (auto& f : m_owned) f->OnResize(internalW, internalH, outW, outH);
    }

    u64 TotalVramBytes() const
    {
        u64 t = 0;
        for (const auto& f : m_owned) t += f->VramBytes();
        return t;
    }

    // 試験用: ステージ s の実行順（機能名）。
    std::vector<const char*> OrderOf(Stage s) const
    {
        std::vector<const char*> v;
        for (const Entry& e : m_perStage[static_cast<u32>(s)]) v.push_back(e.feature->Name());
        return v;
    }

private:
    struct Entry
    {
        i32             priority;
        u32             order;
        IRenderFeature* feature;
    };
    std::vector<std::unique_ptr<IRenderFeature>> m_owned;
    std::vector<Entry>                           m_perStage[kStageCount];
    u32  m_nextOrder = 0;
    bool m_resizeKnown = false;
    u32  m_iw = 0, m_ih = 0, m_ow = 0, m_oh = 0;
};

// FeatureList.inc の UNO_FEATURE(Type) を順に Add する（FeatureList.cpp）。
// UNO_TESTING のビルドでは、環境変数 UNO_TEST_STAGE_RECT=1 のときだけ試験用の矩形の機能も足す。
void RegisterAllFeatures(FeatureRegistry& reg);

} // namespace dx12e
