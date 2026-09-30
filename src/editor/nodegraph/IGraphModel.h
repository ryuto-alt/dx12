#pragma once

// ===== ノードグラフ UI が触るデータの抽象（IGraphModel）=====
// UI（GraphView / GraphDocument）はこのインターフェース越しにだけデータへ触る。マテリアル固有の型・ノード種は
// ここに持ち込まない（マテリアル用モデルは G1: src/renderer/matgraph/ が実装する）。
//
// 契約:
//   ・ID は安定（保存・Undo・コピペで保たれる）。ノードを消して同じ ID で AddNode し直せる（Undo が使う）。
//   ・ミューテーション（AddNode / RemoveNode / Connect / Disconnect / SetNodePos / SetPinValue / Clear）は
//     Undo を積まない「生の操作」。履歴は GraphDocument が積む。
//   ・Revision() は上のミューテーションのたびに増える（UI のキャッシュの無効化に使う）。
//   ・Nodes() は ID 昇順、Edges() は to の昇順（PinRef の operator<）。決定的な順序（保存の差分・テスト）。
//   ・入力ピンは接続を 1 本だけ持つ。Connect は同じ入力の既存接続を置き換える。
//   ・NodeTypes() が返すカタログの参照はモデルが生きている間ずっと有効（要素のアドレスを変えない）。

#include "editor/nodegraph/GraphTypes.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace dx12e::ng
{

class IGraphModel
{
public:
    virtual ~IGraphModel() = default;

    // ---- カタログ ----
    virtual const std::vector<NodeTypeDesc>& NodeTypes() const = 0;
    virtual const NodeTypeDesc* FindNodeType(const std::string& id) const = 0;
    virtual const PinTypeDesc&  PinTypeInfo(PinType t) const = 0;
    // 宣言された型どうしが（一般に）繋げるか。パレットの絞り込みとドラッグ中のピンの強調に使う。
    virtual bool CanConnectTypes(PinType from, PinType to) const = 0;
    // リルート点のノード型 ID（無ければ nullptr。UI は「ワイヤのダブルクリックで挿入」を出さない）。
    virtual const char* RerouteTypeId() const { return nullptr; }

    // ---- 読み取り ----
    virtual const std::vector<NodeId>& Nodes() const = 0;
    virtual const NodeData* FindNode(NodeId id) const = 0;
    virtual const std::vector<Edge>& Edges() const = 0;
    virtual const Edge* FindEdgeTo(PinRef input) const = 0;
    // 接続の可否（向き・型・循環・自己接続）。UI はドラッグ中の毎フレームこれで接続先の可否を出す。
    virtual ConnectCheck CanConnect(PinRef output, PinRef input) const = 0;
    // 多相ピンを上流から推論した型（多相でなければ宣言の型）。ワイヤ・ピンの色に使う。
    virtual PinType ResolvedPinType(PinRef pin) const = 0;
    // n の出力が（直接）繋がる先のノード / n の入力が（直接）繋がる元のノード（重複なし）。
    virtual void Downstream(NodeId n, std::vector<NodeId>& out) const = 0;
    virtual void Upstream(NodeId n, std::vector<NodeId>& out) const = 0;
    virtual uint64_t Revision() const = 0;

    // ---- 生の操作（Undo を積まない）----
    // d.id == 0 なら新しい ID を採番。d.id != 0 ならその ID で作る（既に使われていれば 0 を返す）。
    // d.values が空 / 数が合わなければ型の既定値で埋める。戻り値 = 作った ID（失敗 0）。
    virtual NodeId AddNode(const NodeData& d) = 0;
    virtual bool   RemoveNode(NodeId id) = 0;   // 繋がっている接続も消す
    virtual bool   Connect(PinRef output, PinRef input) = 0;   // CanConnect が通るときだけ。既存を置き換える
    // 履歴の復元（Undo/Redo）・読み込み用: 型の検査を通さず、構造（ノード・ピンの存在と向き）だけ確かめて繋ぐ。既存を置き換える。
    // ★モデルは「上流の型が変わって下流の接続が型不一致になる」状態を許す（UE と同じ。エラーとして見せる）。
    //   その接続を切って Undo するとき、検査つきの Connect は失敗してしまう（Undo が状態を厳密に戻せない）ので別口にした。
    virtual bool   ConnectUnchecked(PinRef output, PinRef input) = 0;
    virtual bool   Disconnect(PinRef input) = 0;
    virtual void   SetNodePos(NodeId id, Vec2 pos) = 0;
    virtual void   SetPinValue(PinRef input, const PinValue& v) = 0;
    virtual void   Clear() = 0;
};

// 出力 out → 入力 in を足すと循環になるか（in のノードから下流をたどって out のノードへ着くか）。
// モデルが CanConnect の中で使える共通実装。
inline bool WouldCreateCycle(const IGraphModel& m, PinRef out, PinRef in)
{
    if (out.node == in.node) return true;
    std::vector<NodeId> stack{in.node};
    std::vector<NodeId> tmp;
    std::unordered_set<NodeId> seen;
    while (!stack.empty())
    {
        const NodeId n = stack.back();
        stack.pop_back();
        if (n == out.node) return true;
        if (!seen.insert(n).second) continue;
        tmp.clear();
        m.Downstream(n, tmp);
        for (NodeId d : tmp) stack.push_back(d);
    }
    return false;
}

} // namespace dx12e::ng
