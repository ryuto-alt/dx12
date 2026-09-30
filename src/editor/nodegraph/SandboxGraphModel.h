#pragma once

// ===== テスト / サンドボックス用の単純な IGraphModel 実装 =====
// マテリアルとは無関係の「型付きグラフのデモ」（設計書 §7 G0）。ダミーの型（Float / Vec2 / Vec3 / Vec4 / Texture / Bool と多相 Any・
// ワイルドカード）と、ノード（定数・加算・乗算・混合・出力ほか約 30 種）を持つ。純ロジック（ImGui 非依存）。
//
// 接続規則（設計書 §3.2 を簡略化）: 数値型どうしは、F1 → Fn（スカラー拡張）と Fn → Fm（m < n の切り詰め）を許し、
// Fn → Fm（1 < n < m）は拒否（「Append を使ってください」）。Texture は Texture へだけ、Bool は Bool へだけ。
// 多相 Any の入出力は上流から推論（未接続なら Float）。リルート（ワイルドカード）は何でも通し、上流の型を引き継ぐ。

#include "editor/nodegraph/IGraphModel.h"

#include <unordered_map>

namespace dx12e::ng
{

class SandboxGraphModel final : public IGraphModel
{
public:
    enum Type : int { kFloat = 0, kVec2, kVec3, kVec4, kTexture, kBool, kAny, kWildcard, kTypeCount };

    SandboxGraphModel();

    // ---- IGraphModel ----
    const std::vector<NodeTypeDesc>& NodeTypes() const override { return m_types; }
    const NodeTypeDesc* FindNodeType(const std::string& id) const override;
    const PinTypeDesc&  PinTypeInfo(PinType t) const override;
    bool CanConnectTypes(PinType from, PinType to) const override;
    const char* RerouteTypeId() const override { return "reroute"; }

    const std::vector<NodeId>& Nodes() const override { return m_ids; }
    const NodeData* FindNode(NodeId id) const override;
    const std::vector<Edge>& Edges() const override { return m_edges; }
    const Edge* FindEdgeTo(PinRef input) const override;
    ConnectCheck CanConnect(PinRef output, PinRef input) const override;
    PinType ResolvedPinType(PinRef pin) const override;
    void Downstream(NodeId n, std::vector<NodeId>& out) const override;
    void Upstream(NodeId n, std::vector<NodeId>& out) const override;
    uint64_t Revision() const override { return m_rev; }

    NodeId AddNode(const NodeData& d) override;
    bool   RemoveNode(NodeId id) override;
    bool   Connect(PinRef output, PinRef input) override;
    bool   ConnectUnchecked(PinRef output, PinRef input) override;
    bool   Disconnect(PinRef input) override;
    void   SetNodePos(NodeId id, Vec2 pos) override;
    void   SetPinValue(PinRef input, const PinValue& v) override;
    void   Clear() override;

    // ---- サンドボックス専用の補助 ----
    // ノード型 typeId の入力 / 出力ピン名 → 添字（無ければ -1）。サンプル生成・テストが名前でつなぐために使う。
    int InputIndex(const std::string& typeId, const std::string& pinName) const;
    int OutputIndex(const std::string& typeId, const std::string& pinName) const;

private:
    struct ByTo { bool operator()(const Edge& a, const Edge& b) const { return a.to < b.to; } };
    void BuildTypes();
    void RebuildAdjacency() const;
    bool TypeOk(PinType from, PinType to) const;
    bool ConsumersAccept(NodeId reroute, PinType t, int depth) const;
    PinType ResolveImpl(PinRef pin, int depth) const;

    std::vector<NodeTypeDesc> m_types;
    std::unordered_map<std::string, int> m_typeIndex;
    PinTypeDesc m_pinTypes[kTypeCount];

    std::vector<NodeId> m_ids;   // 昇順
    std::unordered_map<NodeId, NodeData> m_nodes;
    std::vector<Edge>   m_edges; // to 昇順
    NodeId   m_nextId = 1;
    uint64_t m_rev = 1;

    mutable uint64_t m_adjRev = 0;
    mutable std::unordered_map<NodeId, std::vector<NodeId>> m_down, m_up;
    mutable uint64_t m_resolveRev = 0;
    mutable std::unordered_map<uint64_t, PinType> m_resolveCache;
};

} // namespace dx12e::ng
