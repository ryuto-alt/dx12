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
//   - 出口で状態は戻さなくてよい（呼び出し側が SRV ヒープだけ張り直す）。
//   - StageCtx のポインタ / ハンドルは、0 / null のとき「このステージのこの位置では得られない」の意味。必ず確認する。
//   - リソース状態: sceneColor / depth の「入口の状態」は ctx.sceneColorState / ctx.depthResState に入る。状態を変えるときは
//     生のバリアを使わず ctx.RequireSceneColor() / ctx.RequireDepth() を呼ぶ（呼び出し側の追跡と同期する）。
//     出口で呼び出し側が、その 2 つを入口の状態へ戻す（機能は戻さなくてよい。戻した前提で書かない）。
//     sceneColor / depth 以外のリソース（自分のもの）は自分で管理する。
//
// ステージごとの sceneColor / sceneRtv / depth の状態（ApplicationRender.cpp の位置と一致させること）:
//   BeforeShadows, AfterShadows : sceneColor=null, sceneRtv=0（前フレームの読み取り状態のまま・まだ描かれていない）。
//                                 depth=null（まだ今フレームの深度は無い）。perFrame も無い。
//   AfterDepthPrepass           : 位置は「SSAO / コンタクトシャドウ / RT / SSR / SSGI の後（PrepassSSAO 計測区間の外）、
//                                 sceneRT の RENDER_TARGET 遷移とクリアの前」。sceneColor=null, sceneRtv=0。
//                                 depth / depthSrv は useDepthPrepass のときだけ有効（無いフレームは null / 0。この後 Clear される）。
//                                 depth の入口状態は DEPTH_WRITE。hiZSrv は Hi-Z が作られたフレームだけ、velocitySrv は速度プリパスのフレームだけ有効。
//   BeforeForwardOpaque 以降（AfterTranslucent まで）:
//                                 sceneColor / sceneRtv が有効で入口状態は RENDER_TARGET。depth は有効（入口状態は通常 DEPTH_WRITE）。
//   AfterForwardOpaque / BeforeTranslucent:
//                                 水が有るシーン: 不透明 →（AfterForwardOpaque）→ 水 →（BeforeTranslucent）→ 半透明。
//                                 水が無いシーン: 不透明と半透明が 1 つのパスのため、2 つは「パスの直後で続けて」呼ばれる。
//                                 つまり AfterForwardOpaque は、水が無いシーンでは半透明まで描かれた絵を見る
//                                 （水が有るシーンでは見ない）。屈折・不透明コピー系の機能はこの差を前提にすること。
//   BeforeTemporal, AfterUpscale, PostPreBloom, PostPreUber（ポスト）:
//                                 sceneRtv=0。sceneColor は sceneRT で入口状態は読み取り（PIXEL | NON_PIXEL）。
//                                 「今のチェーンの絵」は sceneColorSrv。depth は有効、depthSrv は読める位置のときだけ有効。
//   PostPostUber                : sceneColor / sceneRtv は「uber の最終出力先」（通常はバックバッファ。オフスクリーン撮影のフレームは
//                                 撮影用 RT）。入口状態は RENDER_TARGET で、機能もその状態で返す（RequireSceneColor は null）。
//                                 outW / outH はその出力先の大きさ。
//   EditorOverlay, EditorPreview: sceneColor / sceneRtv はバックバッファ（RENDER_TARGET）。outW / outH はバックバッファ（ウィンドウ）の大きさ。
//
// 提供の範囲（W05-25 時点）:
//   - perFrameGpuVa / perFrameCpu: b1 は BeforeShadows 〜 AfterDepthPrepass の時点では未作成（0 / null）。BeforeForwardOpaque 〜
//     AfterTranslucent は perFrameGpuVa と perFrameCpu（読み取り専用。書き込みは b1 のアップロード後なので GPU へ届かない）が有効。
//     ポスト系（BeforeTemporal 〜 PostPostUber）は perFrameGpuVa だけ有効で perFrameCpu は null。エディタ系は両方 0 / null。
//     b1 の中身を変えたい機能は、b1 を作る前のステージで AppendClusterLights / SetTexMipBias / RequestVelocityPrepass を使う。
//   - ForwardPsoProvider::depthPrepassActive は BeforeShadows / AfterShadows では常に false（深度プリパスの判定より前なので Select は LESS 版を返す）。
//   - hiZSrv / velocitySrv が指すリソースの状態は PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE の読み取り状態（遷移は不要。書き込む場合は自前で管理）。
//   - contactShadow.Provide は AfterDepthPrepass のステージの中だけ有効（その直後に取り込む。それ以降に呼んでも黙って無視される）。
//   - caps は null（DeviceCaps は W05-04 のマージ後に GraphicsDevice の getter 経由で結線する。追跡: 引き継ぎ事項）。
//   - asyncCompute は null（W4 の ASYNC-1）。
//   - forwardPso は「今のフォワード PSO の選び方を包んだだけ」（shadingRevision / debugView による差し替えは W1 の C）。
//   - contactShadow は差し込み口だけ（Provide した SRV を、AfterDepthPrepass の直後から内蔵のコンタクトシャドウの代わりに読む）。
//   - AppendClusterLights は「ライト収集の前」のステージ（BeforeShadows / AfterShadows / AfterDepthPrepass）だけ有効。
//     それより後のステージで呼んでも捨てられる。SetTexMipBias も b1 を作る前（同じ 3 ステージ）だけ有効。
//   - RequestVelocityPrepass は BeforeShadows / AfterShadows だけ有効（速度プリパスの判定より前）。
// ===========================================================================
#include "core/Types.h"
#include "graphics/GpuEvent.h"

#include <directx/d3d12.h>
#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace dx12e
{

class DescriptorHeap;
class GpuTimer;
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

// 画面空間の影（コンタクトシャドウ）の結果の差し込み口。AfterDepthPrepass で Provide すると、
// 以後のフォワード系がその SRV を内蔵のコンタクトシャドウの代わりに読む（番号は m_srvHeap 内）。
// 最小の形: 何も Provide しなければ従来どおり。W1 の C が拡張する。
class ContactShadowWriter
{
public:
    static constexpr u32 kNone = 0xFFFFFFFFu;
    void Provide(u32 srvIndex) { m_srvIndex = srvIndex; }
    u32  Provided() const { return m_srvIndex; }
    void Reset() { m_srvIndex = kNone; }
private:
    u32 m_srvIndex = kNone;
};

// 今のフォワード PSO の選び方を包んだだけの読み取り口（shadingRevision / debugView による差し替えは W1 の C）。
// ステージ呼び出しごとに、その時点の PSO（準備中は null）を詰めて渡す。
class ForwardPsoProvider
{
public:
    enum class Variant : u8 { Opaque, Mask, Blend };
    struct Set
    {
        ID3D12PipelineState* opaque = nullptr;        // 深度プリパス無し（LESS）
        ID3D12PipelineState* opaqueLEqual = nullptr;  // 深度プリパス有り（LESS_EQUAL）
        ID3D12PipelineState* mask = nullptr;
        ID3D12PipelineState* maskLEqual = nullptr;
        ID3D12PipelineState* blend = nullptr;
    };
    Set  staticMesh;
    Set  skinned;
    bool depthPrepassActive = false;

    ID3D12PipelineState* Select(bool skin, Variant v) const
    {
        const Set& s = skin ? skinned : staticMesh;
        switch (v)
        {
        case Variant::Mask:  return depthPrepassActive ? s.maskLEqual : s.mask;
        case Variant::Blend: return s.blend;
        default:             return depthPrepassActive ? s.opaqueLEqual : s.opaque;
        }
    }
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
    // 読み取り専用の DSV（READ_ONLY_DEPTH。CPU ハンドル）。AfterTranslucent の主ビュー / 副ビューで有効、それ以外のステージと
    // 作れない深度（D32_FLOAT 以外・MSAA）では ptr==0。深度のテストだけして書かない描画（VFX など）が OMSetRenderTargets に渡す。
    // ハンドルは Run の間だけ有効（次のステージ呼び出しで使い回される。保持しないこと）。
    // リソース状態: 入口は通常 DEPTH_WRITE（読み取り専用 DSV はそのまま使える）。深度を SRV としても読むなら
    // ctx.RequireDepth(DEPTH_READ | PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE) を呼んでから束ねる（DEPTH_READ を含めること）。
    D3D12_CPU_DESCRIPTOR_HANDLE depthDsvReadOnly{};
    D3D12_GPU_DESCRIPTOR_HANDLE hiZSrv{};                  // Hi-Z が作られたフレームの AfterDepthPrepass 以降（ポスト含む）。無ければ ptr==0
    D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv{};             // 速度プリパスのフレームの AfterDepthPrepass 以降（ポスト含む）。無ければ ptr==0
    // 入口の状態（sceneColor / depth が null のときは意味を持たない）。変えるときは Require* を使う。
    D3D12_RESOURCE_STATES       sceneColorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES       depthResState   = D3D12_RESOURCE_STATE_COMMON;
    // 呼び出し側の追跡と同期して遷移する口。null のとき（PostPostUber / エディタ系の sceneColor など）は遷移できない。
    void (*TransitionSceneColorFn)(void* user, D3D12_RESOURCE_STATES to) = nullptr;
    void (*TransitionDepthFn)(void* user, D3D12_RESOURCE_STATES to) = nullptr;
    void* stateUser = nullptr;
    bool RequireSceneColor(D3D12_RESOURCE_STATES to)
    {
        if (!TransitionSceneColorFn || !sceneColor) return false;
        TransitionSceneColorFn(stateUser, to); sceneColorState = to; return true;
    }
    bool RequireDepth(D3D12_RESOURCE_STATES to)
    {
        if (!TransitionDepthFn || !depth) return false;
        TransitionDepthFn(stateUser, to); depthResState = to; return true;
    }
    D3D12_GPU_VIRTUAL_ADDRESS   perFrameGpuVa = 0;         // b1（PerFrameConstants）。作る前のステージでは 0
    const FrameConstantsData*   perFrameCpu = nullptr;     // 読み取り専用（b1 は上げた後なので書き換えても GPU へ届かない）。作る前・ポスト系のステージでは null
    // 出力の矩形（バックバッファ上）。PostPostUber / EditorOverlay / EditorPreview で有効（vpW==0 なら未設定＝outW x outH 全体）。
    // uber はエディタのシーンビューの部分矩形へ描くので、出力を重ねる機能はこの矩形へ描くこと。副ビューでも outW / outH は主ビューの値のまま。
    u32                         vpLeft = 0, vpTop = 0, vpW = 0, vpH = 0;

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
    // このフレームのこのステージで記録するものがあるか。false の機能は Empty() / Run() で数えず、全部そうなら
    // そのステージは何も記録しない（GPU イベントも出さない）。毎フレーム何度か呼ばれるので軽くすること。
    virtual bool        Active(Stage) const { return true; }
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

    // GPU イベント名（"Stage:<名前>"）と長さ（NUL を含まない）。ステージごとに 1 回だけ作る（毎フレーム strlen しない）。
    struct EventName { std::string text; u32 len = 0; };
    static const EventName& StageNameForEvent(Stage s)
    {
        static const std::array<EventName, kStageCount> names = []
        {
            std::array<EventName, kStageCount> n;
            for (u32 i = 0; i < kStageCount; ++i)
            {
                n[i].text = std::string("Stage:") + StageName(static_cast<Stage>(i));
                n[i].len = static_cast<u32>(n[i].text.size());
            }
            return n;
        }();
        return names[static_cast<u32>(s)];
    }

    // 登録が 0 件、または登録された機能が全て Active(s)==false のステージは何もしない（true なら Run を呼ぶ必要が無い）。
    bool Empty(Stage s) const
    {
        for (const Entry& e : m_perStage[static_cast<u32>(s)])
            if (e.feature->Active(s)) return false;
        return true;
    }
    bool Empty() const { return m_owned.empty(); }
    size_t Count() const { return m_owned.size(); }

    void Run(Stage s, StageCtx& ctx)
    {
        if (Empty(s)) return;   // 全機能が非 Active なら何も記録しない（イベントも出さない）
        const EventName& en = StageNameForEvent(s);
        GpuEventScope stageScope(ctx.cmd, en.text.c_str(), en.len);   // W05-27: PIX / DRED にステージ名を出す（イベントだけ。計測は増やさない）
        for (const Entry& e : m_perStage[static_cast<u32>(s)])
            if (e.feature->Active(s)) e.feature->Record(s, ctx);
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
