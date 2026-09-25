#pragma once
// ===========================================================================
// IRenderPass — ビューの中の 1 段。「状態の責任をパスの中に閉じ込める」ための契約
// ---------------------------------------------------------------------------
// ★計画03 Phase1 の IRenderPass（ライフサイクルだけを共通化し、Execute は持たない）から設計を変えた。
//   1. 目的が変わった。Phase1 は「パス追加の定型（Initialize / Resize / Shutdown / シェーダー
//      リロード）を減らす」レジストリだった。今要るのは (a) 同じパスを複数のビュー（メイン /
//      カメラプレビュー / 反射プローブ / 知覚層の別視点）で回すこと、(b) compute の後に
//      Render() が手でメインの RootSig / PSO / ヒープ / RT / ビューポートを張り直していた
//      「作法」を無くすこと。どちらも「実行」の単位が要る。
//   2. Phase1 が Execute を外した理由（入力がパスごとに違う → 名前付きリソースレジストリが要る →
//      フレームグラフの入口）は、入力を「パスごとの型付きの Inputs 構造体」で持たせれば避けられる。
//      共通の Execute が受け取るのは、ビューとフレームで全パス共通のもの（コマンドリスト /
//      ヒープ / frameIndex / 計測 / 深度の状態）だけ。
//   3. ライフサイクル（Initialize / Resize / RecreatePipelines）は既存の各サブシステム
//      （ClusteredLightCulling / VolumetricFogPass …）が自己所有していて困っていないので入れない
//      （Phase1 のレジストリは引き続きトリガー待ち）。パスは 1 回の実行ぶんの軽い値で、
//      RenderView がスタックに作って Execute する。
//
// ★状態の契約（このファイルの本体）
//   入口: ルートシグネチャ / PSO / RT / ビューポート / シザー / ディスクリプタヒープ / トポロジに
//         ついて何も仮定しない。使うものは Execute の中で自分で張る。
//   出口: パイプライン状態は戻さない（次のパスが自分で張る。戻すのは無駄なうえ、戻し忘れが
//         「次のパスが偶然動く」を生んでいた）。
//         リソースは「既定の置き場」へ戻す（影マップ = PIXEL_SHADER_RESOURCE / シーン RT =
//         RENDER_TARGET）。★深度だけは例外で、ビューが持つ TrackedState に対して入口で
//         Require するだけ（戻さない）。深度を読むパスが続くときに往復のバリアを出さないため。
//         追跡の外のコード（Forward / ワールドスプライト / ポスト）へ渡す前に、ビューが
//         DEPTH_WRITE へ戻す。
//   → 呼び出し側に「このパスの後で RootSig / PSO / ヒープを張り直す」行が要らない。
//
// ★DeclareResources は次の段（レンダーグラフ: 自動バリア / 一時リソースのエイリアス）の口。
//   読む / 書く共有リソースと、パスの中で使う状態を列挙する。今は宣言として残すだけで、
//   バリアは各パスが自分で出している（順序決定をソルバに渡さない＝00-COORDINATION / 計画03 §2.3(d)）。
// ===========================================================================
#include <vector>
#include <directx/d3d12.h>

#include "core/Types.h"

namespace dx12e
{

class CommandList;
class DescriptorHeap;
class GpuTimer;

// 1 リソースの「今の状態」を追いかけ、要求された状態へだけ遷移を出す（同じ状態なら何もしない）。
// ★MiniEngine と同じ「同一状態なら no-op」の 20 行。RenderTarget / GpuResource が既に持っている
//   仕組みを、生の ID3D12Resource（メインの深度など）にも使えるようにしたもの。
struct TrackedState
{
    ID3D12Resource*       resource = nullptr;
    D3D12_RESOURCE_STATES state    = D3D12_RESOURCE_STATE_COMMON;

    void Require(CommandList& cmd, D3D12_RESOURCE_STATES next);
};

// パスが読む / 書く共有リソースの宣言（レンダーグラフの口）。
enum class PassAccess : u8 { Read, Write };
struct PassResourceUse
{
    const char*           name;       // "sceneColor" / "depth" / "csm" …（ログ・検証用）
    ID3D12Resource*       resource;   // nullptr = サブシステムの内部リソース（遷移も所有者がやる）
    D3D12_RESOURCE_STATES state;      // パスの中で使う状態
    PassAccess            access;
};

// 1 パスの実行文脈（ビューとフレームで全パス共通のもの）。
struct RenderPassContext
{
    CommandList*               cmd     = nullptr;
    ID3D12GraphicsCommandList* native  = nullptr;
    DescriptorHeap*            srvHeap = nullptr;   // シェーダ可視の CBV/SRV/UAV ヒープ（フレームで 1 枚）
    u32                        frameIndex = 0;
    GpuTimer*                  timer = nullptr;     // 主ビューだけ非 null（副ビューは計測しない）
    TrackedState*              depth = nullptr;     // このビューの深度（Require で要る状態へ）
};

class IRenderPass
{
public:
    virtual ~IRenderPass() = default;

    virtual const char* Name() const = 0;
    // 読む / 書く共有リソースを out へ足す（任意）。
    virtual void DeclareResources(std::vector<PassResourceUse>& out) const { (void)out; }
    // 実行。状態の契約はこのファイル先頭。
    virtual void Execute(const RenderPassContext& ctx) = 0;
};

} // namespace dx12e
