// ============================================================================
// GraphModel.h — マテリアルグラフのデータモデル（純ロジック。ImGui / D3D 非依存）
//
//   ・接続は「入力側」に持つ（Node::inputs[pin] = 接続元 or リテラル）。
//     ワイヤ 1 本の変更 = 1 ノードの 1 エントリの変更なので、.dxmg の git 差分が 1 行で済む。
//   ・UI（G0 の IGraphModel アダプタ）が必要とする操作を素直な API で出す:
//       ノード / エッジの追加・削除・移動・プロパティ変更、接続可否判定（CanConnect）、
//       ピンの型の問い合わせ（PinType）、変更通知（AddListener）とバージョンカウンタ。
//   ・型推論と診断は GraphAnalysis（AnalyzeGraph）が計算し、Version() が変わらない限りキャッシュする。
//   ・スレッド安全ではない（呼び出し側で直列化する）。
// ============================================================================
#pragma once

#include "renderer/matgraph/NodeLibrary.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace dx12e::matgraph
{

using NodeId = std::string;   // "n_071829" / "out" など。'.' を含まない [A-Za-z0-9_]+

struct PinRef
{
    NodeId      node;
    std::string pin;
    bool operator==(const PinRef& o) const { return node == o.node && pin == o.pin; }
};

// 入力ピンの中身。link と literal は排他（後から入れた方が勝つ）。
struct InputBinding
{
    std::optional<PinRef> link;
    std::optional<Value>  literal;
    bool Empty() const { return !link && !literal; }
};

struct Node
{
    NodeId      id;
    std::string type;                              // NodeDef::type（未知の型もそのまま保持する）
    float       x = 0.0f, y = 0.0f;                // エディタ上の位置
    std::map<std::string, Json>         props;     // 設定値（PropDecl の型へ正規化済み）
    std::map<std::string, InputBinding> inputs;    // 入力ピン名 → 接続 / リテラル
};

struct Edge
{
    NodeId      fromNode;
    std::string fromPin;
    NodeId      toNode;
    std::string toPin;
    bool operator==(const Edge& o) const
    {
        return fromNode == o.fromNode && fromPin == o.fromPin && toNode == o.toNode && toPin == o.toPin;
    }
};

struct Comment
{
    std::string id;
    std::string text;
    std::string color = "#888888";
    float x = 0, y = 0, w = 200, h = 120;
};

// エディタのキャンバスの見え方（パン・ズーム）。UI 状態なので Version() を進めない（.dxmg の "view" キー。無ければ valid=false）。
struct ViewInfo
{
    bool  valid = false;
    float panX = 0.0f, panY = 0.0f;
    float zoom = 1.0f;
    bool operator==(const ViewInfo& o) const { return valid == o.valid && panX == o.panX && panY == o.panY && zoom == o.zoom; }
};

struct GraphSettings
{
    std::string blendMode    = "Opaque";      // Opaque / Masked / Translucent / Additive
    std::string shadingModel = "DefaultLit";  // DefaultLit / Unlit（予約: Subsurface / ClearCoat / Cloth / Hair / Eye）
    bool        twoSided     = false;
    float       maskClip     = 0.5f;          // Masked のしきい値
    bool operator==(const GraphSettings& o) const
    {
        return blendMode == o.blendMode && shadingModel == o.shadingModel && twoSided == o.twoSided && maskClip == o.maskClip;
    }
};

// ---- 変更通知 ---------------------------------------------------------------------
enum class ChangeKind : uint8_t
{
    NodeAdded, NodeRemoved, NodeMoved, PropChanged, LinkChanged, LiteralChanged,
    CommentChanged, SettingsChanged, MetaChanged, Reset,
};

struct GraphChange
{
    ChangeKind  kind = ChangeKind::Reset;
    NodeId      node;                 // 対象ノード（コメントなら空）
    std::string pin;                  // LinkChanged / LiteralChanged のとき入力ピン名
    bool        affectsStructure = true;  // true = 生成される HLSL が変わりうる（再コンパイルが要る）
};

// ---- 接続可否 ---------------------------------------------------------------------
struct ConnectCheck
{
    enum class Verdict : uint8_t { Ok, Warn, Reject };
    Verdict     verdict = Verdict::Ok;
    std::string code;      // Reject / Warn の診断コード
    std::string message;   // 日本語の理由（ツールチップに出せる）
    bool ok() const { return verdict != Verdict::Reject; }
};

// RemoveNode の戻り値。RestoreNode に渡せば元に戻る（Undo 用）。
struct RemovedNode
{
    Node              node;       // 入力側の接続はここに入っている
    std::vector<Edge> outgoing;   // このノードから他ノードへ出ていた接続（他ノード側の入力から外される）
};

// ピンの問い合わせ結果
struct PinInfo
{
    bool        exists    = false;
    bool        isOutput  = false;
    std::string name;
    ValueType   type      = ValueType::Invalid;   // 解決後の型（推論結果。未接続 / エラーなら Invalid）
    bool        connected = false;                // 入力: 接続あり / 出力: 1 本以上つながっている
    bool        hasLiteral = false;
    bool        reserved  = false;
    const PinDecl*    inDecl  = nullptr;
    const OutPinDecl* outDecl = nullptr;
};

struct GraphAnalysis;   // GraphAnalysis.h

class MaterialGraph
{
public:
    explicit MaterialGraph(const NodeLibrary* lib = &NodeLibrary::Builtin());
    ~MaterialGraph();
    MaterialGraph(MaterialGraph&&) noexcept;
    MaterialGraph& operator=(MaterialGraph&&) noexcept;
    MaterialGraph(const MaterialGraph&) = delete;
    MaterialGraph& operator=(const MaterialGraph&) = delete;

    // 変更通知と推論キャッシュを持たない複製（試し接続・Undo 用スナップショット）
    MaterialGraph Clone() const;

    const NodeLibrary& Library() const { return *m_lib; }

    // ---- メタ ------------------------------------------------------------------
    const std::string& Guid() const { return m_guid; }
    void SetGuid(const std::string& g);
    const std::string& Name() const { return m_name; }
    void SetName(const std::string& n);
    const GraphSettings& Settings() const { return m_settings; }
    void SetSettings(const GraphSettings& s);
    const ViewInfo& View() const { return m_view; }
    void SetView(const ViewInfo& v) { m_view = v; }   // 通知もバージョン更新もしない（UI 状態）

    // ---- バージョン -------------------------------------------------------------
    // Version()          : どんな変更でも +1
    // StructureVersion() : 生成される HLSL が変わりうる変更（ノード / 接続 / リテラル / Code プロパティ）で +1。
    //                      値スロットの編集・移動・名前変更・コメントでは変わらない → 再コンパイル不要の判定に使う
    uint64_t Version() const { return m_version; }
    uint64_t StructureVersion() const { return m_structVersion; }

    // ---- 変更通知 ---------------------------------------------------------------
    using Listener   = std::function<void(const GraphChange&)>;
    using ListenerId = uint32_t;
    ListenerId AddListener(Listener fn);
    void       RemoveListener(ListenerId id);

    // ---- ノード -----------------------------------------------------------------
    const std::map<NodeId, Node>& Nodes() const { return m_nodes; }
    const Node* FindNode(const NodeId& id) const;
    const NodeDef* DefOf(const NodeId& id) const;            // 未知の型なら nullptr

    // ノードを追加して id を返す。id を指定した場合は既存と衝突したら失敗（空文字を返す）。
    // 未知の型でも追加できる（診断で E_UNKNOWN_NODE になる。ファイル往復のため）。
    // props は登録表の既定値で初期化される。
    NodeId AddNode(const std::string& type, float x, float y, const NodeId& id = {});
    bool   RemoveNode(const NodeId& id, RemovedNode* undoInfo = nullptr);
    bool   RestoreNode(const RemovedNode& r);                // RemoveNode の取り消し
    bool   MoveNode(const NodeId& id, float x, float y);

    // プロパティ。値は PropDecl の型へ正規化される（不正なら false）。未知のプロパティ名は生のまま保存する。
    // ★名前は SetProp / GetProp にしない: windows.h の SetProp / GetProp マクロ（→ SetPropW）と衝突して、
    //   windows.h を先に include した翻訳単位でだけビルドが通らなくなる（実際に踏んだ）。
    bool         SetNodeProp(const NodeId& id, const std::string& key, const Json& value);
    Json         GetNodeProp(const NodeId& id, const std::string& key) const;   // 未設定なら既定値、無ければ null

    // ---- 接続 -------------------------------------------------------------------
    // 接続できるか（型 / 循環 / 予約ピン / 存在チェック）。UI のドラッグ中ツールチップ用。
    ConnectCheck CanConnect(const NodeId& fromNode, const std::string& fromPin,
                            const NodeId& toNode, const std::string& toPin) const;
    // 接続する。Reject なら何もせず false。toPin の既存の接続 / リテラルは置き換わる。
    bool Connect(const NodeId& fromNode, const std::string& fromPin,
                 const NodeId& toNode, const std::string& toPin, ConnectCheck* why = nullptr);
    // 履歴の復元（Undo / Redo）・読み込み用: 型と循環の検査を通さず、ノードとピンの存在だけ確かめて繋ぐ（既存を置き換える）。
    // 上流の型が変わって下流の接続が型不一致になった状態は正常（診断で E_TYPE_MISMATCH として見せる）で、
    // その接続を切って Undo するときに検査つきの Connect は失敗してしまうので別口にしてある。
    bool ConnectUnchecked(const NodeId& fromNode, const std::string& fromPin,
                          const NodeId& toNode, const std::string& toPin);
    // 入力ピンの接続とリテラルを外す（既定値に戻る）。
    bool Disconnect(const NodeId& toNode, const std::string& toPin);
    // 未接続ピンへ値を直書きする（既存の接続は外れる）。ピンの型で許されない値は false。
    bool SetLiteral(const NodeId& toNode, const std::string& toPin, const Value& v);
    bool ClearLiteral(const NodeId& toNode, const std::string& toPin);
    // 入力ピンの中身（無ければ nullptr）
    const InputBinding* Binding(const NodeId& toNode, const std::string& toPin) const;

    std::vector<Edge> Edges() const;                              // 全エッジ（toNode, toPin 順）
    std::vector<Edge> EdgesFrom(const NodeId& fromNode) const;    // ノードの出力から出ているエッジ
    std::vector<Edge> EdgesTo(const NodeId& toNode) const;

    // ---- 型の問い合わせ ---------------------------------------------------------
    PinInfo QueryPin(const NodeId& node, const std::string& pin, bool isOutput) const;
    ValueType PinType(const NodeId& node, const std::string& pin, bool isOutput) const { return QueryPin(node, pin, isOutput).type; }
    const GraphAnalysis& Analysis() const;   // 型推論 + 診断（Version() が同じ間キャッシュ）

    // ---- コメント ---------------------------------------------------------------
    const std::map<std::string, Comment>& Comments() const { return m_comments; }
    std::string AddComment(const Comment& c);                     // id が空なら "c_xxxx" を採番
    bool        SetComment(const Comment& c);
    bool        RemoveComment(const std::string& id);

    // ---- ID 採番 ----------------------------------------------------------------
    NodeId GenerateNodeId();          // "n_" + 6 桁 hex（重複しない）
    void   SetIdSeed(uint32_t seed);  // テスト用: 採番を決定的にする

    // ---- 読み込み用（GraphIO が使う。通知しない）--------------------------------
    void LoadNode(Node n);
    void LoadComment(Comment c);
    void LoadFinish();                // バージョンを進めて Reset 通知
    void Clear();

private:
    void Notify(const GraphChange& c);
    void Bump(bool structure);
    bool WouldCycle(const NodeId& fromNode, const NodeId& toNode) const;

    const NodeLibrary*              m_lib;
    std::map<NodeId, Node>          m_nodes;
    std::map<std::string, Comment>  m_comments;
    GraphSettings                   m_settings;
    ViewInfo                        m_view;
    std::string                     m_guid;
    std::string                     m_name;
    uint64_t                        m_version = 1;
    uint64_t                        m_structVersion = 1;
    std::mt19937                    m_rng;
    std::map<ListenerId, Listener>  m_listeners;
    ListenerId                      m_nextListener = 1;
    mutable std::shared_ptr<const GraphAnalysis> m_analysis;
    mutable uint64_t                m_analysisVersion = 0;
};

// 出力ノードの型 ID
inline constexpr const char* kOutputNodeType = "MaterialOutput";

} // namespace dx12e::matgraph
