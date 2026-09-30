#pragma once

// ===== グラフ文書: モデル + コメント + Undo/Redo + 空間分割 + ヒットテスト + 整列（純ロジック）=====
// UI（GraphView）はこの文書を通して編集する。生のモデル操作は Undo を積まないので、編集は必ずここのメソッドを使う。
//
// ★Undo の設計判断（docs/MATERIAL_GRAPH_DESIGN.md §6.5 との関係）:
//   設計書は「グラフ文書ごとに UndoSystem（editor/UndoCore.h）を 1 個」としていたが、
//   UndoSystem は履歴上限が 100 固定（kMaxHistory）で、G0 の合否「1 万操作 → 全 Undo で初期状態に厳密一致」を満たせない。
//   また AI（MCP）エントリの横取り機構は本 UI の外側の話なので、ここでは【独立スタック GraphHistory】を持つ
//   （上限は設定可・0=無制限）。コマンドは IGraphCommand（IUndoCommand と同形の Undo/Redo/GetName）なので、
//   G3 で McpUndoRouter へ束ねるときは「グラフ 1 操作 = IUndoCommand 1 個」のアダプタ 1 枚で繋げる。
//   連続ドラッグの併合は「ドラッグ中は生の操作 + 離した時に 1 コマンド」（Begin/Commit）で実現している。

#include "editor/nodegraph/GraphLayout.h"
#include "editor/nodegraph/GraphMath.h"
#include "editor/nodegraph/IGraphModel.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dx12e::ng
{

class GraphDocument;

// ---- コメントボックス（UI 側の持ち物。モデルには載せない）----
struct Comment
{
    CommentId   id = 0;
    Rect        rect;            // 全体（見出し帯を含む）
    std::string title;
    int         color = 0;       // 0..kCommentColors-1
    friend bool operator==(const Comment& a, const Comment& b)
    {
        return a.id == b.id && a.rect == b.rect && a.title == b.title && a.color == b.color;
    }
    friend bool operator!=(const Comment& a, const Comment& b) { return !(a == b); }
};

struct Selection
{
    std::set<NodeId>    nodes;
    std::set<CommentId> comments;
    bool Empty() const { return nodes.empty() && comments.empty(); }
    void Clear() { nodes.clear(); comments.clear(); }
    bool Has(NodeId n) const { return nodes.count(n) != 0; }
};

// ---- Undo / Redo ----
class IGraphCommand
{
public:
    virtual ~IGraphCommand() = default;
    virtual void Undo(GraphDocument& doc) = 0;
    virtual void Redo(GraphDocument& doc) = 0;
    virtual const char* GetName() const = 0;
};

class GraphHistory
{
public:
    // maxDepth: 0 = 無制限。
    void SetMaxDepth(size_t n) { m_maxDepth = n; }
    // 適用済みの操作を積む（Redo は呼ばない）。グループ中はグループへ入る。redo は捨てる。
    void Push(std::unique_ptr<IGraphCommand> cmd);
    void Undo(GraphDocument& doc);
    void Redo(GraphDocument& doc);
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    size_t UndoDepth() const { return m_undo.size(); }
    size_t RedoDepth() const { return m_redo.size(); }
    const char* PeekUndoName() const { return m_undo.empty() ? nullptr : m_undo.back()->GetName(); }
    const char* PeekRedoName() const { return m_redo.empty() ? nullptr : m_redo.back()->GetName(); }
    void Clear() { m_undo.clear(); m_redo.clear(); m_groups.clear(); }
    void DropRedo() { m_redo.clear(); }
    uint64_t EditSeq() const { return m_editSeq; }   // 単調増加（未保存判定用。Clear では戻さない）

    // 複数の操作を 1 回の Undo/Redo にまとめる。入れ子可。中身が空なら何も積まない。
    void BeginGroup(const char* name);
    void EndGroup();
    bool InGroup() const { return !m_groups.empty(); }

private:
    struct Group { const char* name; std::vector<std::unique_ptr<IGraphCommand>> cmds; };
    std::vector<std::unique_ptr<IGraphCommand>> m_undo, m_redo;
    std::vector<Group> m_groups;
    size_t   m_maxDepth = 0;
    uint64_t m_editSeq  = 0;
};

// ---- 整列（純関数）----
enum class AlignMode : uint8_t { Left, Right, Top, Bottom, CenterH, CenterV, DistributeH, DistributeV };
struct AlignItem { NodeId id = 0; Rect rect; };
// 各ノードの新しい左上位置（動かないものも含めて返す）。DistributeH/V は 3 個以上のときだけ動く（端は固定・間隔を等分）。
std::vector<std::pair<NodeId, Vec2>> ComputeAlign(AlignMode mode, const std::vector<AlignItem>& items);

// ---- ヒットテスト ----
struct Hit
{
    enum Kind : uint8_t { None, Node, Pin, Slot, Edge, CommentTitle, CommentResize, CommentBody } kind = None;
    NodeId    node = 0;
    PinRef    pin;                // Pin / Slot
    int       slotComp = 0;       // Slot: 成分
    ng::Edge  edge;               // Edge
    CommentId comment = 0;
};
struct HitParams
{
    float pinRadius  = 9.0f;   // ピンの当たり半径（グラフ単位。ズームが小さいときは呼び出し側が広げる）
    float edgeTol    = 6.0f;   // ワイヤの当たり距離（グラフ単位）
    float resizeSize = 14.0f;  // コメント右下のリサイズ当たり
    float commentTitleH = 26.0f;
    bool  slots = true;        // 値欄を当たり対象にするか（LOD で隠れているときは false）
    const std::unordered_map<NodeId, uint32_t>* z = nullptr;   // 前面順（大きいほど手前。無ければ ID 順）
};

class GraphDocument
{
public:
    explicit GraphDocument(IGraphModel* model);

    IGraphModel&       Model() { return *m_model; }
    const IGraphModel& Model() const { return *m_model; }
    GraphLayout&       Layout() { return m_layout; }
    GraphHistory&      History() { return m_history; }
    const GraphHistory& History() const { return m_history; }

    // ---- 幾何 ----
    Rect NodeRect(NodeId id) const;                 // 無ければ空矩形
    Vec2 PinPos(PinRef pin) const;
    Rect SlotRect(PinRef inputPin) const;
    Rect ContentBounds() const;                     // 全ノード + コメント（空なら EmptyRect）
    Rect BoundsOf(const Selection& s) const;
    Rect CommentContentRect(const Comment& c) const;

    // ---- コメント ----
    const std::vector<Comment>& Comments() const { return m_comments; }
    const Comment* FindComment(CommentId id) const;
    uint64_t CommentRevision() const { return m_commentRev; }
    // コメントの中に丸ごと入っているノード（コメントごと動かすときの同伴）
    std::vector<NodeId> NodesInsideComment(CommentId id) const;

    // ---- 編集（Undo を積む）----
    NodeId AddNode(const std::string& type, Vec2 pos);
    NodeId AddNodeData(NodeData d);                 // d.id == 0 で採番
    bool   Connect(PinRef out, PinRef in);          // 入力側が空でも置き換えでも可。false = 不可（理由は Model().CanConnect）
    bool   Disconnect(PinRef in);
    int    DisconnectPin(PinRef pin);               // ピンの全接続を切る（Alt+クリック）。切った本数
    void   RemoveItems(const Selection& s);         // ノード（+ 繋がる接続）・コメント
    void   SetPinValue(PinRef in, const PinValue& v);
    void   MoveItems(const Selection& s, Vec2 delta);   // コメントは同伴ノードを含めない（呼び出し側が s に入れる）
    void   SetNodePositions(const std::vector<std::pair<NodeId, Vec2>>& pos, const char* name = "ノードを移動");
    void   Align(const std::vector<NodeId>& nodes, AlignMode mode);
    CommentId AddComment(const Rect& rect, const std::string& title, int color);
    CommentId AddCommentAround(const std::vector<NodeId>& nodes, const std::string& title, int color);
    void   EditComment(CommentId id, const Comment& next);   // タイトル・色・矩形を 1 操作で
    NodeId InsertReroute(const Edge& e, Vec2 pos);  // 接続の途中にリルート点を挿入（1 操作）。RerouteTypeId が無ければ 0

    // ---- ドラッグ中の生の操作 + 離したときに 1 コマンドで確定 ----
    // 位置: 開始時に Begin で元の値を握り、Update で生の位置を書き換え、Commit で 1 コマンドにする（変化なしなら何も積まない）。
    struct MoveSession
    {
        std::vector<std::pair<NodeId, Vec2>> nodes;      // 元の位置
        std::vector<std::pair<CommentId, Rect>> comments;
        bool active = false;
    };
    MoveSession BeginMove(const Selection& s) const;
    void        UpdateMove(const MoveSession& ms, Vec2 delta);          // 元位置 + delta へ（生の操作）
    bool        CommitMove(MoveSession& ms, const char* name = "ノードを移動");
    void        CancelMove(MoveSession& ms);
    void SetPinValueLive(PinRef in, const PinValue& v) { m_model->SetPinValue(in, v); }
    bool CommitPinValue(PinRef in, const PinValue& oldValue);            // 生の変更を 1 コマンドへ
    void SetCommentLive(const Comment& c);
    bool CommitComment(const Comment& oldValue);

    // ---- 全消去（履歴も消す。新規グラフ / 読み込み前）----
    void Clear();

    // ---- 空間分割・ヒットテスト ----
    // モデル / コメントが変わっていれば索引を作り直す。以降の Query* は最新。
    void EnsureIndex();
    // r と重なるノード（外接矩形の交差）。fullyInside = true なら r に丸ごと入るものだけ。ID 昇順。
    void NodesInRect(const Rect& r, bool fullyInside, std::vector<NodeId>& out);
    void CommentsInRect(const Rect& r, bool fullyInside, std::vector<CommentId>& out) const;
    // r と外接矩形が重なる接続（Edges() の添字）。
    void EdgesInRect(const Rect& r, std::vector<int>& out);
    Hit  HitTest(Vec2 p, const HitParams& hp);
    // 力ずくの実装（テスト用: 空間分割の結果と一致するはず）。
    void NodesInRectBrute(const Rect& r, bool fullyInside, std::vector<NodeId>& out) const;
    void EdgesInRectBrute(const Rect& r, std::vector<int>& out) const;
    // 接続の曲線（グラフ座標）
    Bezier EdgeCurve(const Edge& e) const;

    // ---- 状態の同一性（Undo ファズ・往復テスト）----
    // 全ノード・値・接続・コメントを「浮動小数のビットまで」含む文字列で返す（ID の採番カウンタは含めない）。
    std::string Signature() const;

    uint64_t Revision() const { return m_model->Revision() * 1315423911u + m_commentRev; }

    // ---- 内部（コマンドと GraphIO が使う生の操作）----
    void RawAddComment(const Comment& c);
    void RawRemoveComment(CommentId id);
    void RawSetComment(const Comment& c);
    CommentId AllocCommentId() { return m_nextCommentId++; }
    void SetNextCommentIdAtLeast(CommentId id) { if (m_nextCommentId <= id) m_nextCommentId = id + 1; }
    void RawClearComments() { m_comments.clear(); ++m_commentRev; }

private:
    void RebuildIndex();

    IGraphModel*    m_model;
    mutable GraphLayout m_layout;
    GraphHistory    m_history;
    std::vector<Comment> m_comments;
    CommentId       m_nextCommentId = 1;
    uint64_t        m_commentRev = 1;

    // 索引
    SpatialGrid     m_nodeGrid, m_edgeGrid, m_commentGrid;
    uint64_t        m_indexModelRev = ~0ull, m_indexCommentRev = ~0ull;
    std::vector<uint32_t> m_tmpIds;
};

} // namespace dx12e::ng
